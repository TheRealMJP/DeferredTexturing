//=================================================================================================
//
//  MJP's DX12 Sample Framework
//  http://mynameismjp.wordpress.com/
//
//  All code licensed under the MIT license
//
//=================================================================================================

#include "PCH.h"

#include "PostProcessHelper.h"

#include "..\\Utility.h"
#include "ShaderCompilation.h"
#include "DX12.h"

namespace AppSettings
{
    const extern uint32 CBufferRegister;
    void BindCBufferGfx(ID3D12GraphicsCommandList* cmdList, uint32 rootParameter);
}

namespace SampleFramework12
{

static const uint32 MaxInputs = 8;

PostProcessHelper::PostProcessHelper()
{
}

PostProcessHelper::~PostProcessHelper()
{
    Assert_(tempRenderTargets.Count() == 0);
    Assert_(pipelineStates.Count() == 0);
}

void PostProcessHelper::Initialize()
{
    // Load the shaders
    std::wstring fullScreenTriPath = SampleFrameworkDir() + L"Shaders\\FullScreenTriangle.hlsl";
    fullScreenTriVS = CompileFromFile(fullScreenTriPath.c_str(), "FullScreenTriangleVS", ShaderType::Vertex, ShaderProfile::SM51);

    {
        D3D12_DESCRIPTOR_RANGE1 srvRanges[1] = {};
        srvRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[0].NumDescriptors = MaxInputs;
        srvRanges[0].BaseShaderRegister = 0;
        srvRanges[0].RegisterSpace = 0;
        srvRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER1 rootParameters[2] = {};

        // AppSettings
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[0].Descriptor.RegisterSpace = 0;
        rootParameters[0].Descriptor.ShaderRegister = AppSettings::CBufferRegister;

        // SRV descriptors
        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[1].DescriptorTable.pDescriptorRanges = srvRanges;
        rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;

        D3D12_STATIC_SAMPLER_DESC staticSamplers[4] = {};
        staticSamplers[0] = DX12::GetStaticSamplerState(SamplerState::Point, 0);
        staticSamplers[1] = DX12::GetStaticSamplerState(SamplerState::LinearClamp, 1);
        staticSamplers[2] = DX12::GetStaticSamplerState(SamplerState::Linear, 2);
        staticSamplers[3] = DX12::GetStaticSamplerState(SamplerState::LinearBorder, 3);

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = ArraySize_(staticSamplers);
        rootSignatureDesc.pStaticSamplers = staticSamplers;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        DX12::CreateRootSignature(&rootSignature, rootSignatureDesc);
        rootSignature->SetName(L"PostProcessHelper");
    }
}

void PostProcessHelper::Shutdown()
{
    ClearCache();

    DX12::Release(rootSignature);
}

void PostProcessHelper::ClearCache()
{
    for(uint64 i = 0; i < tempRenderTargets.Count(); ++i)
    {
        TempRenderTarget* tempRT = tempRenderTargets[i];
        tempRT->RT.Shutdown();
        delete tempRT;
    }

    tempRenderTargets.RemoveAll(nullptr);

    for(uint64 i = 0; i < pipelineStates.Count(); ++i)
        DX12::DeferredRelease(pipelineStates[i].PSO);

    pipelineStates.RemoveAll(CachedPSO());
}

TempRenderTarget* PostProcessHelper::GetTempRenderTarget(uint64 width, uint64 height, DXGI_FORMAT format, bool useAsUAV)
{
    for(uint64 i = 0; i < tempRenderTargets.Count(); ++i)
    {
        TempRenderTarget* tempRT = tempRenderTargets[i];
        if(tempRT->InUse)
            continue;

        const RenderTexture& rt = tempRT->RT;
        if(rt.Texture.Width == width && rt.Texture.Height == height && rt.Texture.Format == format && useAsUAV == rt.UAV.IsValid()) {
            tempRT->InUse = true;
            return tempRT;
        }
    }

    TempRenderTarget* tempRT = new TempRenderTarget();
    tempRT->RT.Initialize(width, height, format, 1, 1, useAsUAV);
    tempRT->RT.Texture.Resource->SetName(L"PP Temp Render Target");
    tempRT->InUse = true;
    tempRenderTargets.Add(tempRT);

    return tempRT;
}

void PostProcessHelper::Begin(ID3D12GraphicsCommandList* cmdList_)
{
    Assert_(cmdList == nullptr);
    cmdList = cmdList_;
}

void PostProcessHelper::End()
{
    Assert_(cmdList != nullptr);
    cmdList = nullptr;

    for(uint64 i = 0; i < tempRenderTargets.Count(); ++i)
        Assert_(tempRenderTargets[i]->InUse == false);
}

void PostProcessHelper::PostProcess(CompiledShaderPtr pixelShader, const char* name, const RenderTexture& input, const RenderTexture& output)
{
    D3D12_CPU_DESCRIPTOR_HANDLE inputs[1] = { input.SRV() };
    const RenderTexture* outputs[1] = { &output };
    PostProcess(pixelShader, name, inputs, 1, outputs, 1);
}

void PostProcessHelper::PostProcess(CompiledShaderPtr pixelShader, const char* name, const RenderTexture& input, const TempRenderTarget* output)
{
    D3D12_CPU_DESCRIPTOR_HANDLE inputs[1] = { input.SRV() };
    const RenderTexture* outputs[1] = { &output->RT };
    PostProcess(pixelShader, name, inputs, 1, outputs, 1);
}

void PostProcessHelper::PostProcess(CompiledShaderPtr pixelShader, const char* name, const TempRenderTarget* input, const RenderTexture& output)
{
    D3D12_CPU_DESCRIPTOR_HANDLE inputs[1] = { input->RT.SRV() };
    const RenderTexture* outputs[1] = { &output };
    PostProcess(pixelShader, name, inputs, 1, outputs, 1);
}

void PostProcessHelper::PostProcess(CompiledShaderPtr pixelShader, const char* name, const TempRenderTarget* input, const TempRenderTarget* output)
{
    D3D12_CPU_DESCRIPTOR_HANDLE inputs[1] = { input->RT.SRV() };
    const RenderTexture* outputs[1] = { &output->RT };
    PostProcess(pixelShader, name, inputs, 1, outputs, 1);
}

struct HashSource
{
    DXGI_FORMAT OutputFormats[8] = { };
    uint64 MSAASamples = 0;
};

void PostProcessHelper::PostProcess(CompiledShaderPtr pixelShader, const char* name, const D3D12_CPU_DESCRIPTOR_HANDLE* inputs, uint64 numInputs,
                                    const RenderTexture*const* outputs, uint64 numOutputs)
{
    Assert_(cmdList != nullptr);
    Assert_(numOutputs > 0);
    Assert_(outputs != nullptr);
    Assert_(numInputs == 0 || inputs != nullptr);
    Assert_(numInputs <= MaxInputs);

    PIXMarker marker(cmdList, name);

    HashSource hashSource;
    for(uint64 i = 0; i < numOutputs; ++i)
    {
        hashSource.OutputFormats[i] = outputs[i]->Texture.Format;
        hashSource.MSAASamples = outputs[i]->MSAASamples;
    }

    Hash psoHash = GenerateHash(&hashSource, sizeof(HashSource));
    psoHash = CombineHashes(psoHash, pixelShader->ByteCodeHash);

    ID3D12PipelineState* pso = nullptr;

    // The most linear of searches!
    const uint64 numPSOs = pipelineStates.Count();
    for(uint64 i = 0; i < numPSOs; ++i)
    {
        if(pipelineStates[i].Hash == psoHash)
        {
            pso = pipelineStates[i].PSO;
            break;
        }
    }

    if(pso == nullptr)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = rootSignature;
        psoDesc.VS = fullScreenTriVS.ByteCode();
        psoDesc.PS = pixelShader.ByteCode();
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::NoCull);
        psoDesc.BlendState = DX12::GetBlendState(BlendState::Disabled);
        psoDesc.DepthStencilState = DX12::GetDepthState(DepthState::Disabled);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = uint32(numOutputs);
        for(uint64 i = 0; i < numOutputs; ++i)
            psoDesc.RTVFormats[i] = hashSource.OutputFormats[i];
        psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        psoDesc.SampleDesc.Count = uint32(hashSource.MSAASamples);
        DXCall(DX12::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)));

        CachedPSO cachedPSO;
        cachedPSO.Hash = psoHash;
        cachedPSO.PSO = pso;
        pipelineStates.Add(cachedPSO);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[8] = { };
    for(uint64 i = 0; i < numOutputs; ++i)
        rtvHandles[i] = outputs[i]->RTV.CPUHandle;
    cmdList->OMSetRenderTargets(uint32(numOutputs), rtvHandles, false, nullptr);

    cmdList->SetGraphicsRootSignature(rootSignature);
    cmdList->SetPipelineState(pso);

    AppSettings::BindCBufferGfx(cmdList, 0);

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandles[MaxInputs] = { };
    for(uint64 i = 0; i < numInputs; ++i)
        srvHandles[i] = inputs[i];
    for(uint64 i = numInputs; i < MaxInputs; ++i)
        srvHandles[i] = DX12::NullTexture2DSRV.CPUHandle;

    DX12::BindShaderResources(cmdList, 1, MaxInputs, srvHandles);

    DX12::SetViewport(cmdList, outputs[0]->Texture.Width, outputs[0]->Texture.Height);

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);

}

}