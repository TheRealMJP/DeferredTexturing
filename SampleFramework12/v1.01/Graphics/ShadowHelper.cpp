//=================================================================================================
//
//  MJP's DX12 Sample Framework
//  http://mynameismjp.wordpress.com/
//
//  All code licensed under the MIT license
//
//=================================================================================================

#pragma once

#include "PCH.h"
#include "ShadowHelper.h"
#include "Camera.h"

#include <Utility.h>
#include <Graphics\\ShaderCompilation.h>
#include <Graphics\\GraphicsTypes.h>
#include <Graphics\\DX12_Helpers.h>

namespace SampleFramework12
{

namespace ShadowHelper
{

static const uint32 MaxFilterRadius = 4;

// Transforms from [-1,1] post-projection space to [0,1] UV space
Float4x4 ShadowScaleOffsetMatrix = Float4x4(Float4(0.5f,  0.0f, 0.0f, 0.0f),
                                            Float4(0.0f, -0.5f, 0.0f, 0.0f),
                                            Float4(0.0f,  0.0f, 1.0f, 0.0f),
                                            Float4(0.5f,  0.5f, 0.0f, 1.0f));

static CompiledShaderPtr fullScreenTriVS;
static CompiledShaderPtr smConvertPS;
static CompiledShaderPtr filterSMHorizontalPS[MaxFilterRadius + 1];
static CompiledShaderPtr filterSMVerticalPS[MaxFilterRadius + 1];

static ID3D12PipelineState* smConvertPSO = nullptr;
static ID3D12PipelineState* filterSMHorizontalPSO[MaxFilterRadius + 1] = { };
static ID3D12PipelineState* filterSMVerticalPSO[MaxFilterRadius + 1] = { };
static ID3D12RootSignature* rootSignature = nullptr;

static ShadowMapMode currSMMode = ShadowMapMode::NumValues;
static ShadowMSAAMode currMSAAMode = ShadowMSAAMode::NumValues;
static bool initialized = false;

enum RootParams : uint32
{
    RootParam_StandardDescriptors,
    RootParam_CBuffer,

    NumRootParams
};

struct ConvertConstants
{
    Float2 ShadowMapSize;
    float PositiveExponent = 0.0f;
    float NegativeExponent = 0.0f;
    float FilterSize = 0.0f;
    bool32 LinearizeDepth = 0;
    float NearClip = 0.0f;
    float InvClipRange = 0.0f;
    float Proj33 = 0.0f;
    float Proj43 = 0.0f;
    uint32 InputMapIdx = uint32(-1);
    uint32 ArraySliceIdx = 0;
};

void Initialize(ShadowMapMode smMode, ShadowMSAAMode msaaMode)
{
    Assert_(initialized == false);
    currSMMode = smMode;
    currMSAAMode = msaaMode;

    if(smMode == ShadowMapMode::EVSM || smMode == ShadowMapMode::MSM)
    {
        std::wstring fullScreenTriPath = SampleFrameworkDir() + L"Shaders\\FullScreenTriangle.hlsl";
        std::wstring smConvertPath = SampleFrameworkDir() + L"Shaders\\SMConvert.hlsl";
        fullScreenTriVS = CompileFromFile(fullScreenTriPath.c_str(), "FullScreenTriangleVS", ShaderType::Vertex);
        for(uint32 i = 0; i <= MaxFilterRadius; ++i)
        {
            CompileOptions opts;
            opts.Add("SampleRadius_", i);
            opts.Add("Vertical_", 0);
            filterSMHorizontalPS[i] = CompileFromFile(smConvertPath.c_str(), "FilterSM", ShaderType::Pixel);

            opts.Reset();
            opts.Add("SampleRadius_", i);
            opts.Add("Vertical_", 1);
            filterSMVerticalPS[i] = CompileFromFile(smConvertPath.c_str(), "FilterSM", ShaderType::Pixel, opts);
        }

        CompileOptions opts;
        opts.Add("EVSM_", smMode == ShadowMapMode::EVSM ? 1 : 0);
        opts.Add("MSM_", smMode == ShadowMapMode::MSM ? 1 : 0);
        opts.Add("MSAASamples_", NumMSAASamples());
        smConvertPS = CompileFromFile(smConvertPath.c_str(), "SMConvert", ShaderType::Pixel, opts);

        {
            D3D12_ROOT_PARAMETER1 rootParameters[NumRootParams] = { };
            rootParameters[RootParam_StandardDescriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rootParameters[RootParam_StandardDescriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            rootParameters[RootParam_StandardDescriptors].DescriptorTable.pDescriptorRanges = DX12::StandardDescriptorRanges();
            rootParameters[RootParam_StandardDescriptors].DescriptorTable.NumDescriptorRanges = DX12::NumStandardDescriptorRanges;

            rootParameters[RootParam_CBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            rootParameters[RootParam_CBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            rootParameters[RootParam_CBuffer].Descriptor.RegisterSpace = 0;
            rootParameters[RootParam_CBuffer].Descriptor.ShaderRegister = 0;

            D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
            rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
            rootSignatureDesc.pParameters = rootParameters;
            rootSignatureDesc.NumStaticSamplers = 0;
            rootSignatureDesc.pStaticSamplers = nullptr;
            rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

            DX12::CreateRootSignature(&rootSignature, rootSignatureDesc);
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = { };
        psoDesc.pRootSignature = rootSignature;
        psoDesc.VS = fullScreenTriVS.ByteCode();
        psoDesc.PS = smConvertPS.ByteCode();
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::NoCull);
        psoDesc.BlendState = DX12::GetBlendState(BlendState::Disabled);
        psoDesc.DepthStencilState = DX12::GetDepthState(DepthState::Disabled);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = SMFormat();
        psoDesc.SampleDesc.Count = 1;
        DXCall(DX12::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&smConvertPSO)));

        for(uint32 i = 0; i <= MaxFilterRadius; ++i)
        {
            psoDesc.PS = filterSMHorizontalPS[i].ByteCode();
            DXCall(DX12::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&filterSMHorizontalPSO[i])));

            psoDesc.PS = filterSMVerticalPS[i].ByteCode();
            DXCall(DX12::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&filterSMVerticalPSO[i])));
        }
    }

    initialized = true;
}

void Shutdown()
{
    Assert_(initialized);

    DX12::DeferredRelease(smConvertPSO);
    for(uint32 i = 0; i <= MaxFilterRadius; ++i)
    {
        DX12::DeferredRelease(filterSMHorizontalPSO[i]);
        DX12::DeferredRelease(filterSMVerticalPSO[i]);
    }

    DX12::DeferredRelease(rootSignature);

    currSMMode = ShadowMapMode::NumValues;
    currMSAAMode = ShadowMSAAMode::NumValues;
    initialized = false;
}

uint32 NumMSAASamples()
{
    Assert_(currMSAAMode != ShadowMSAAMode::NumValues);

    static const uint32 numMSAASamples[] = { 1, 2, 4 };
    StaticAssert_(ArraySize_(numMSAASamples) == uint32(ShadowMSAAMode::NumValues));
    return numMSAASamples[uint32(currMSAAMode)];
}

DXGI_FORMAT SMFormat()
{
    Assert_(currSMMode != ShadowMapMode::NumValues);

    if(currSMMode == ShadowMapMode::EVSM)
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    else if(currSMMode == ShadowMapMode::MSM)
        return DXGI_FORMAT_R16G16B16A16_UNORM;

    Assert_(false);
    return DXGI_FORMAT_UNKNOWN;
}

void ConvertShadowMap(ID3D12GraphicsCommandList* cmdList, const DepthBuffer& depthMap, RenderTexture& smTarget,
                      uint32 arraySlice, RenderTexture& tempTarget, float filterSizeU, float filterSizeV,
                      bool linearizeDepth, float nearClip, float farClip, const Float4x4& projection,
                      float positiveExponent, float negativeExponent)
{
    Assert_(initialized);
    Assert_(currSMMode == ShadowMapMode::MSM || currSMMode == ShadowMapMode::EVSM);
    Assert_(NumMSAASamples() == depthMap.MSAASamples);
    Assert_(depthMap.Width() == smTarget.Width() && depthMap.Height() == smTarget.Height());

    PIXMarker event(cmdList, "Shadow Map Conversion");

    D3D12_CPU_DESCRIPTOR_HANDLE smTargetRTV = arraySlice == 0 ? smTarget.RTV : smTarget.ArrayRTVs[arraySlice];
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[1] = { smTargetRTV };
    cmdList->OMSetRenderTargets(1, rtvHandles, false, nullptr);

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    DX12::SetViewport(cmdList, smTarget.Width(), smTarget.Height());

    cmdList->SetGraphicsRootSignature(rootSignature);
    cmdList->SetPipelineState(smConvertPSO);

    DX12::BindStandardDescriptorTable(cmdList, RootParam_StandardDescriptors, CmdListMode::Graphics);

    ConvertConstants constants;
    constants.ShadowMapSize.x = float(depthMap.Width());
    constants.ShadowMapSize.y = float(depthMap.Height());
    constants.PositiveExponent = positiveExponent;
    constants.NegativeExponent = negativeExponent;
    constants.FilterSize = 0.0f;
    constants.LinearizeDepth = linearizeDepth ? 1 : 0;
    constants.NearClip = nearClip;
    constants.InvClipRange = 1.0f / (farClip - nearClip);
    constants.Proj33 = projection._33;
    constants.Proj43 = projection._43;
    constants.InputMapIdx = depthMap.SRV();
    constants.ArraySliceIdx = arraySlice;

    DX12::BindTempConstantBuffer(cmdList, constants, RootParam_CBuffer, CmdListMode::Graphics);

    cmdList->DrawInstanced(3, 1, 0, 0);

    filterSizeU = Clamp(filterSizeU, 1.0f, MaxShadowFilterSize);
    filterSizeV = Clamp(filterSizeV, 1.0f, MaxShadowFilterSize);
    if(filterSizeU > 1.0f || filterSizeV > 1.0f)
    {
        smTarget.MakeReadable(cmdList, 0, arraySlice);

        // Horizontal pass
        uint32 sampleRadiusU = uint32((filterSizeU / 2) + 0.499f);

        rtvHandles[0] = tempTarget.RTV;
        cmdList->OMSetRenderTargets(1, rtvHandles, false, nullptr);

        tempTarget.MakeWritable(cmdList, 0, 0);

        constants.FilterSize = filterSizeU;
        constants.InputMapIdx = smTarget.SRV();
        DX12::BindTempConstantBuffer(cmdList, constants, RootParam_CBuffer, CmdListMode::Graphics);

        cmdList->SetPipelineState(filterSMHorizontalPSO[sampleRadiusU]);

        cmdList->DrawInstanced(3, 1, 0, 0);

        tempTarget.MakeReadable(cmdList, 0, 0);

        // Vertical pass
        uint32 sampleRadiusV = uint32((filterSizeV / 2) + 0.499f);

        rtvHandles[0] = smTargetRTV;
        cmdList->OMSetRenderTargets(1, rtvHandles, false, nullptr);

        constants.FilterSize = filterSizeV;
        constants.InputMapIdx = tempTarget.SRV();
        DX12::BindTempConstantBuffer(cmdList, constants, RootParam_CBuffer, CmdListMode::Graphics);

        cmdList->SetPipelineState(filterSMVerticalPSO[sampleRadiusV]);

        cmdList->DrawInstanced(3, 1, 0, 0);

        // smTarget.MakeReadable(cmdList, 0, arraySlice);
    }

    // context->GenerateMips(varianceShadowMap.SRView);
}

void PrepareCascades(const Float3& lightDir, uint64 shadowMapSize, bool stabilize, const Camera& camera,
                     SunShadowConstants& constants, OrthographicCamera* cascadeCameras)
{
    const float MinDistance = 0.0f;
    const float MaxDistance = 1.0f;

    // Compute the split distances based on the partitioning mode
    float cascadeSplits[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    {
        float lambda = 0.5f;

        float nearClip = camera.NearClip();
        float farClip = camera.FarClip();
        float clipRange = farClip - nearClip;

        float minZ = nearClip + MinDistance * clipRange;
        float maxZ = nearClip + MaxDistance * clipRange;

        float range = maxZ - minZ;
        float ratio = maxZ / minZ;

        for(uint32 i = 0; i < NumCascades; ++i)
        {
            float p = (i + 1) / static_cast<float>(NumCascades);
            float log = minZ * std::pow(ratio, p);
            float uniform = minZ + range * p;
            float d = lambda * (log - uniform) + uniform;
            cascadeSplits[i] = (d - nearClip) / clipRange;
        }
    }

    Float3 c0Extents;
    Float4x4 c0Matrix;

    // Prepare the projections ofr each cascade
    for(uint64 cascadeIdx = 0; cascadeIdx < NumCascades; ++cascadeIdx)
    {
        // Get the 8 points of the view frustum in world space
        Float3 frustumCornersWS[8] =
        {
            Float3(-1.0f,  1.0f, 0.0f),
            Float3( 1.0f,  1.0f, 0.0f),
            Float3( 1.0f, -1.0f, 0.0f),
            Float3(-1.0f, -1.0f, 0.0f),
            Float3(-1.0f,  1.0f, 1.0f),
            Float3( 1.0f,  1.0f, 1.0f),
            Float3( 1.0f, -1.0f, 1.0f),
            Float3(-1.0f, -1.0f, 1.0f),
        };

        float prevSplitDist = cascadeIdx == 0 ? MinDistance : cascadeSplits[cascadeIdx - 1];
        float splitDist = cascadeSplits[cascadeIdx];

        Float4x4 invViewProj = Float4x4::Invert(camera.ViewProjectionMatrix());
        for(uint64 i = 0; i < 8; ++i)
            frustumCornersWS[i] = Float3::Transform(frustumCornersWS[i], invViewProj);

        // Get the corners of the current cascade slice of the view frustum
        for(uint64 i = 0; i < 4; ++i)
        {
            Float3 cornerRay = frustumCornersWS[i + 4] - frustumCornersWS[i];
            Float3 nearCornerRay = cornerRay * prevSplitDist;
            Float3 farCornerRay = cornerRay * splitDist;
            frustumCornersWS[i + 4] = frustumCornersWS[i] + farCornerRay;
            frustumCornersWS[i] = frustumCornersWS[i] + nearCornerRay;
        }

        // Calculate the centroid of the view frustum slice
        Float3 frustumCenter = Float3(0.0f);
        for(uint64 i = 0; i < 8; ++i)
            frustumCenter += frustumCornersWS[i];
        frustumCenter *= (1.0f / 8.0f);

        // Pick the up vector to use for the light camera
        Float3 upDir = camera.Right();

        Float3 minExtents;
        Float3 maxExtents;

        if(stabilize)
        {
            // This needs to be constant for it to be stable
            upDir = Float3(0.0f, 1.0f, 0.0f);

            // Calculate the radius of a bounding sphere surrounding the frustum corners
            float sphereRadius = 0.0f;
            for(uint64 i = 0; i < 8; ++i)
            {
                float dist = Float3::Length(Float3(frustumCornersWS[i]) - frustumCenter);
                sphereRadius = Max(sphereRadius, dist);
            }

            sphereRadius = std::ceil(sphereRadius * 16.0f) / 16.0f;

            maxExtents = Float3(sphereRadius, sphereRadius, sphereRadius);
            minExtents = -maxExtents;
        }
        else
        {
            // Create a temporary view matrix for the light
            Float3 lightCameraPos = frustumCenter;
            Float3 lookAt = frustumCenter - lightDir;
            DirectX::XMMATRIX lightView = DirectX::XMMatrixLookAtLH(lightCameraPos.ToSIMD(), lookAt.ToSIMD(), upDir.ToSIMD());

            // Calculate an AABB around the frustum corners
            DirectX::XMVECTOR mins = DirectX::XMVectorSet(FloatMax, FloatMax, FloatMax, FloatMax);
            DirectX::XMVECTOR maxes = DirectX::XMVectorSet(-FloatMax, -FloatMax, -FloatMax, -FloatMax);
            for(uint32 i = 0; i < 8; ++i)
            {
                DirectX::XMVECTOR corner = DirectX::XMVector3TransformCoord(frustumCornersWS[i].ToSIMD(), lightView);
                mins = DirectX::XMVectorMin(mins, corner);
                maxes = DirectX::XMVectorMax(maxes, corner);
            }

            minExtents = Float3(mins);
            maxExtents = Float3(maxes);
        }

        // Adjust the min/max to accommodate the filtering size
        float scale = (shadowMapSize + 7.0f) / shadowMapSize;
        minExtents.x *= scale;
        minExtents.y *= scale;
        maxExtents.x *= scale;
        maxExtents.y *= scale;

        Float3 cascadeExtents = maxExtents - minExtents;

        // Get position of the shadow camera
        Float3 shadowCameraPos = frustumCenter + lightDir * -minExtents.z;

        // Come up with a new orthographic camera for the shadow caster
        OrthographicCamera& shadowCamera = cascadeCameras[cascadeIdx];
        shadowCamera.Initialize(minExtents.x, minExtents.y, maxExtents.x, maxExtents.y, 0.0f, cascadeExtents.z);
        shadowCamera.SetLookAt(shadowCameraPos, frustumCenter, upDir);

        if(stabilize)
        {
            // Create the rounding matrix, by projecting the world-space origin and determining
            // the fractional offset in texel space
            DirectX::XMMATRIX shadowMatrix = shadowCamera.ViewProjectionMatrix().ToSIMD();
            DirectX::XMVECTOR shadowOrigin = DirectX::XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
            shadowOrigin = DirectX::XMVector4Transform(shadowOrigin, shadowMatrix);
            shadowOrigin = DirectX::XMVectorScale(shadowOrigin, shadowMapSize / 2.0f);

            DirectX::XMVECTOR roundedOrigin = DirectX::XMVectorRound(shadowOrigin);
            DirectX::XMVECTOR roundOffset = DirectX::XMVectorSubtract(roundedOrigin, shadowOrigin);
            roundOffset = DirectX::XMVectorScale(roundOffset, 2.0f / shadowMapSize);
            roundOffset = DirectX::XMVectorSetZ(roundOffset, 0.0f);
            roundOffset = DirectX::XMVectorSetW(roundOffset, 0.0f);

            DirectX::XMMATRIX shadowProj = shadowCamera.ProjectionMatrix().ToSIMD();
            shadowProj.r[3] = DirectX::XMVectorAdd(shadowProj.r[3], roundOffset);
            shadowCamera.SetProjection(Float4x4(shadowProj));
        }

        Float4x4 shadowMatrix = shadowCamera.ViewProjectionMatrix();
        shadowMatrix = shadowMatrix * ShadowScaleOffsetMatrix;

        // Store the split distance in terms of view space depth
        const float clipDist = camera.FarClip() - camera.NearClip();
        constants.CascadeSplits[cascadeIdx] = camera.NearClip() + splitDist * clipDist;

        if(cascadeIdx == 0)
        {
            c0Extents = cascadeExtents;
            c0Matrix = shadowMatrix;
            constants.ShadowMatrix = shadowMatrix;
            constants.CascadeOffsets[0] = Float4(0.0f, 0.0f, 0.0f, 0.0f);
            constants.CascadeScales[0] = Float4(1.0f, 1.0f, 1.0f, 1.0f);
        }
        else
        {
            // Calculate the position of the lower corner of the cascade partition, in the UV space
            // of the first cascade partition
            Float4x4 invCascadeMat = Float4x4::Invert(shadowMatrix);
            Float3 cascadeCorner = Float3::Transform(Float3(0.0f, 0.0f, 0.0f), invCascadeMat);
            cascadeCorner = Float3::Transform(cascadeCorner, c0Matrix);

            // Do the same for the upper corner
            Float3 otherCorner = Float3::Transform(Float3(1.0f, 1.0f, 1.0f), invCascadeMat);
            otherCorner = Float3::Transform(otherCorner, c0Matrix);

            // Calculate the scale and offset
            Float3 cascadeScale = Float3(1.0f, 1.0f, 1.f) / (otherCorner - cascadeCorner);
            constants.CascadeOffsets[cascadeIdx] = Float4(-cascadeCorner, 0.0f);
            constants.CascadeScales[cascadeIdx] = Float4(cascadeScale, 1.0f);
        }
    }
}

}

}
