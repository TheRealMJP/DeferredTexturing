//=================================================================================================
//
//  MJP's DX12 Sample Framework
//  http://mynameismjp.wordpress.com/
//
//  All code licensed under the MIT license
//
//=================================================================================================

#include "PCH.h"

#include "SpriteRenderer.h"
#include "ShaderCompilation.h"
#include "SpriteFont.h"
#include "Textures.h"

namespace SampleFramework12
{

enum RootParams
{
    SRVParam_VS,
    SRVParam_PS,
    CBVParam,

    NumRootParams
};

SpriteRenderer::SpriteRenderer()
{

}

SpriteRenderer::~SpriteRenderer()
{
}

void SpriteRenderer::Initialize()
{
    const std::wstring shaderPath = SampleFrameworkDir() + L"Shaders\\Sprite.hlsl";

    // Load the shaders
    vertexShader = CompileFromFile(shaderPath.c_str(), "SpriteVS", ShaderType::Vertex);
    pixelShader = CompileFromFile(shaderPath.c_str(), "SpritePS", ShaderType::Pixel);

    // Create the instance data buffer
    StructuredBufferInit sbInit;
    sbInit.Dynamic = true;
    sbInit.Lifetime = BufferLifetime::Temporary;
    sbInit.NumElements = MaxBatchSize;
    sbInit.Stride = sizeof(SpriteDrawData);
    instanceDataBuffer.Initialize(sbInit);

    // Create the index buffer
    uint16 indices[] = { 0, 1, 2, 3, 0, 2 };
    indexBuffer.Initialize(sizeof(indices), 4, false, BufferLifetime::Persistent, false, indices, D3D12_RESOURCE_STATE_INDEX_BUFFER);

    // Create our constant buffers
    perBatchCB.Initialize(BufferLifetime::Temporary);

    LoadTexture(defaultTexture, L"..\\Content\\Textures\\Default.dds");

    {
        // Make the root signature
        D3D12_DESCRIPTOR_RANGE1 ranges[2] = { };
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].RegisterSpace = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER1 rootParameters[NumRootParams] = { };
        rootParameters[SRVParam_VS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[SRVParam_VS].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rootParameters[SRVParam_VS].DescriptorTable.pDescriptorRanges = ranges;
        rootParameters[SRVParam_VS].DescriptorTable.NumDescriptorRanges = 1;

        rootParameters[SRVParam_PS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[SRVParam_PS].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[SRVParam_PS].DescriptorTable.pDescriptorRanges = ranges;
        rootParameters[SRVParam_PS].DescriptorTable.NumDescriptorRanges = 1;

        rootParameters[CBVParam].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[CBVParam].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[CBVParam].Descriptor.RegisterSpace = 0;
        rootParameters[CBVParam].Descriptor.ShaderRegister = 0;

        D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = { };
        staticSamplers[0] = DX12::GetStaticSamplerState(SamplerState::Point, 0);
        staticSamplers[1] = DX12::GetStaticSamplerState(SamplerState::LinearClamp, 1);

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = { };
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = ArraySize_(staticSamplers);
        rootSignatureDesc.pStaticSamplers = staticSamplers;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        DX12::CreateRootSignature(&rootSignature, rootSignatureDesc);
    }
}

void SpriteRenderer::Shutdown()
{
    DestroyPSOs();

    indexBuffer.Shutdown();
    instanceDataBuffer.Shutdown();
    defaultTexture.Shutdown();
    DX12::Release(rootSignature);
}

void SpriteRenderer::CreatePSOs(DXGI_FORMAT rtFormat)
{
    // Make PSO's for all blend modes
    const BlendState blendStates[] = { BlendState::AlphaBlend, BlendState::Disabled };
    StaticAssert_(ArraySize_(blendStates) == uint64(SpriteBlendMode::NumValues));

    for(uint64 i = 0; i < uint64(SpriteBlendMode::NumValues); ++i)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = rootSignature;
        psoDesc.VS = vertexShader.ByteCode();
        psoDesc.PS = pixelShader.ByteCode();
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::NoCull);
        psoDesc.BlendState = DX12::GetBlendState(blendStates[i]);
        psoDesc.DepthStencilState = DX12::GetDepthState(DepthState::Disabled);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = rtFormat;
        psoDesc.SampleDesc.Count = 1;
        DXCall(DX12::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineStates[i])));
    }
}

void SpriteRenderer::DestroyPSOs()
{
    for(uint64 i = 0; i < ArraySize_(pipelineStates); ++i)
        DX12::DeferredRelease(pipelineStates[i]);
}

void SpriteRenderer::Begin(ID3D12GraphicsCommandList* cmdList, Float2 viewportSize, SpriteFilterMode filterMode, SpriteBlendMode blendMode)
{
    cmdList->SetPipelineState(pipelineStates[uint64(blendMode)]);
    cmdList->SetGraphicsRootSignature(rootSignature);

    perBatchCB.Data.LinearSampling = filterMode == SpriteFilterMode::Linear ? 1 : 0;
    perBatchCB.Data.ViewportSize = viewportSize;

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_INDEX_BUFFER_VIEW ibView = { };
    ibView.BufferLocation = indexBuffer.GPUAddress;
    ibView.SizeInBytes = uint32(indexBuffer.Size);
    ibView.Format = DXGI_FORMAT_R16_UINT;
    cmdList->IASetIndexBuffer(&ibView);
}

void SpriteRenderer::Render(ID3D12GraphicsCommandList* cmdList, const Texture* texture, const SpriteTransform& transform,
                            const Float4& color, const Float4* drawRect)
{
    if(texture == nullptr)
        texture = &defaultTexture;

    SpriteDrawData drawData;
    drawData.Transform = transform;
    drawData.Color = color;
    if(drawRect != nullptr)
        drawData.DrawRect = *drawRect;
    else
        drawData.DrawRect = Float4(0.0f, 0.0f, float(texture->Width), float(texture->Height));

    RenderBatch(cmdList, texture, &drawData, 1);
}

void SpriteRenderer::RenderBatch(ID3D12GraphicsCommandList* cmdList, const Texture* texture,
                                 const SpriteDrawData* drawData, uint64 numSprites)
{
    if(numSprites == 0)
        return;

    if(texture == nullptr)
        texture = &defaultTexture;

    perBatchCB.Data.TextureSize = Float2(float(texture->Width), float(texture->Height));
    perBatchCB.Upload();
    perBatchCB.SetAsGfxRootParameter(cmdList, CBVParam);

    #if Debug_
        // Make sure the draw rects are all valid
        for(uint64 i = 0; i < numSprites; ++i)
        {
            Float4 drawRect = drawData[i].DrawRect;
            Assert_(drawRect.x >= 0 && drawRect.x < texture->Width);
            Assert_(drawRect.y >= 0 && drawRect.y < texture->Height);
            Assert_(drawRect.z > 0 && drawRect.x + drawRect.z <= texture->Width);
            Assert_(drawRect.w > 0 && drawRect.y + drawRect.w <= texture->Height);
        }
    #endif

    uint64 numSpritesLeft = numSprites;
    for(uint64 offset = 0; offset < numSprites; offset += MaxBatchSize)
    {
        const uint64 spritesToDraw = Min<uint64>(MaxBatchSize, numSpritesLeft);

        // Fill up the instance buffer
        instanceDataBuffer.MapAndSetData(drawData + offset, spritesToDraw);

        // Bind the resources
        D3D12_CPU_DESCRIPTOR_HANDLE vsDescriptors[] = { instanceDataBuffer.SRV() };
        DX12::BindShaderResources(cmdList, SRVParam_VS, ArraySize_(vsDescriptors), vsDescriptors);

        D3D12_CPU_DESCRIPTOR_HANDLE psDescriptors[] = { texture->SRV.CPUHandle };
        DX12::BindShaderResources(cmdList, SRVParam_PS, ArraySize_(psDescriptors), psDescriptors);

        // Draw
        cmdList->DrawIndexedInstanced(6, uint32(spritesToDraw), 0, 0, 0);

        numSpritesLeft -= spritesToDraw;
    }
}

void SpriteRenderer::RenderText(ID3D12GraphicsCommandList* cmdList, const SpriteFont& font,
                                const wchar* text, Float2 position, const Float4& color)
{
    Assert_(text != nullptr);

    const uint64 numChars = wcslen(text);

    SpriteTransform textTransform = SpriteTransform(position);

    uint64 numCharsLeft = numChars;
    for(uint64 offset = 0; offset < numChars; offset += MaxBatchSize)
    {
        uint64 numCharsToDraw = Min<uint64>(numChars, MaxBatchSize);
        uint64 currentDraw = 0;

        for(uint64 i = 0; i < numCharsToDraw; ++i)
        {
            wchar character = text[i + offset];
            if(character == ' ')
                textTransform.Position.x += font.SpaceWidth();
            else if(character == '\n')
            {
                textTransform.Position.y += font.CharHeight();
                textTransform.Position.x = 0;
            }
            else
            {
                SpriteFont::CharDesc desc = font.GetCharDescriptor(character);

                textDrawData[currentDraw].Transform = textTransform;
                textDrawData[currentDraw].Color = color;
                textDrawData[currentDraw].DrawRect.x = desc.X;
                textDrawData[currentDraw].DrawRect.y = desc.Y;
                textDrawData[currentDraw].DrawRect.z = desc.Width;
                textDrawData[currentDraw].DrawRect.w = desc.Height;

                textTransform.Position.x += desc.Width + 1;

                ++currentDraw;
            }
        }

        RenderBatch(cmdList, font.FontTexture(), textDrawData, currentDraw);

        numCharsLeft -= numCharsToDraw;
    }
}

void SpriteRenderer::End()
{
}

}