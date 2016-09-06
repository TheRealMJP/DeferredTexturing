//=================================================================================================
//
//  Bindless Deferred Texturing Sample
//  by MJP
//  http://mynameismjp.wordpress.com/
//
//  All code and content licensed under the MIT license
//
//=================================================================================================

#include <PCH.h>

#include <InterfacePointers.h>
#include <Window.h>
#include <Input.h>
#include <Utility.h>
#include <Graphics/SwapChain.h>
#include <Graphics/ShaderCompilation.h>
#include <Graphics/Profiler.h>
#include <Graphics/Textures.h>
#include <Graphics/Sampling.h>
#include <Graphics/DX12.h>
#include <Graphics/DX12_Helpers.h>

#include "BindlessDeferred.h"
#include "SharedTypes.h"

using namespace SampleFramework12;
using std::wstring;

// Model filenames
static const wchar* ScenePaths[] =
{
    L"..\\Content\\Models\\Sponza\\Sponza.fbx",
};

static const float SceneScales[] = { 0.01f };
static const Float3 SceneCameraPositions[] = { Float3(-11.5f, 1.85f, -0.45f) };
static const Float2 SceneCameraRotations[] = { Float2(0.0f, 1.544f) };

StaticAssert_(ArraySize_(ScenePaths) == uint64(Scenes::NumValues));
StaticAssert_(ArraySize_(SceneScales) == uint64(Scenes::NumValues));
StaticAssert_(ArraySize_(SceneCameraPositions) == uint64(Scenes::NumValues));
StaticAssert_(ArraySize_(SceneCameraRotations) == uint64(Scenes::NumValues));

static const uint64 NumConeSides = 16;

struct PickingData
{
    Float3 Position;
    Float3 Normal;
};

enum DeferredRootParams
{
    Deferred_DeferredCBuffer,
    Deferred_PSCBuffer,
    Deferred_ShadowCBuffer,
    Deferred_LightCBuffer,
    Deferred_AppSettings,
    Deferred_Descriptors,
    Deferred_DecalDescriptors,

    NumDeferredRootParams
};

// Returns true if a sphere intersects a capped cone defined by a direction, height, and angle
static bool SphereConeIntersection(const Float3& coneTip, const Float3& coneDir, float coneHeight,
                                   float coneAngle, const Float3& sphereCenter, float sphereRadius)
{
    if(Float3::Dot(sphereCenter - coneTip, coneDir) > coneHeight + sphereRadius)
        return false;

    float cosHalfAngle = std::cos(coneAngle * 0.5f);
    float sinHalfAngle = std::sin(coneAngle * 0.5f);

    Float3 v = sphereCenter - coneTip;
    float a = Float3::Dot(v, coneDir);
    float b = a * sinHalfAngle / cosHalfAngle;
    float c = std::sqrt(Float3::Dot(v, v) - a * a);
    float d = c - b;
    float e = d * cosHalfAngle;

    return e < sphereRadius;
}

BindlessDeferred::BindlessDeferred() :  App(L"Bindless Deferred Texturing")
{
    minFeatureLevel = D3D_FEATURE_LEVEL_11_1;
    globalHelpText = "Bindless Deferred Texturing\n\n"
                     "Controls:\n\n"
                     "Use W/S/A/D/Q/E to move the camera, and hold right-click while dragging the mouse to rotate.";
}

void BindlessDeferred::BeforeReset()
{
}

void BindlessDeferred::AfterReset()
{
    float aspect = float(swapChain.Width()) / swapChain.Height();
    camera.SetAspectRatio(aspect);

    CreateRenderTargets();
}

void BindlessDeferred::Initialize()
{
    // Check if the device supports conservative rasterization
    D3D12_FEATURE_DATA_D3D12_OPTIONS features = { };
    DX12::Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &features, sizeof(features));
    if(features.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2)
        throw Exception("This demo requires a GPU that supports FEATURE_LEVEL_11_1 with D3D12_RESOURCE_BINDING_TIER_2");

    if(features.ConservativeRasterizationTier == D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED)
    {
        AppSettings::ClusterRasterizationMode.SetValue(ClusterRasterizationModes::MSAA8x);
        AppSettings::ClusterRasterizationMode.ClampNumValues(uint32(ClusterRasterizationModes::NumValues) - 1);
    }

    // Load the scenes
    for(uint64 i = 0; i < uint64(Scenes::NumValues); ++i)
    {
        ModelLoadSettings settings;
        settings.FilePath = ScenePaths[i];
        settings.ForceSRGB = true;
        settings.SceneScale = SceneScales[i];
        settings.MergeMeshes = false;
        sceneModels[i].CreateWithAssimp(settings);
    }

    float aspect = float(swapChain.Width()) / swapChain.Height();
    camera.Initialize(aspect, Pi_4, 0.1f, 35.0f);

    InitializeScene();

    skybox.Initialize();

    postProcessor.Initialize();

    // Load the decal textures
    for(uint64 i = 0; i < AppSettings::NumDecalTypes; ++i)
    {
        LoadTexture(decalTextures[i * 2 + 0], MakeString(L"..\\Content\\Textures\\Decals\\Decal_%02u_Albedo.tga", uint32(i)).c_str(), true);
        LoadTexture(decalTextures[i * 2 + 1], MakeString(L"..\\Content\\Textures\\Decals\\Decal_%02u_Normal.png", uint32(i)).c_str(), false);
    }

    decals.Init(AppSettings::MaxDecals);

    {
        // Decal buffer
        StructuredBufferInit sbInit;
        sbInit.Stride = sizeof(Decal);
        sbInit.NumElements = AppSettings::MaxDecals;
        sbInit.Dynamic = false;
        sbInit.Lifetime = BufferLifetime::Persistent;
        sbInit.InitialState = D3D12_RESOURCE_STATE_COPY_DEST;
        decalBuffer.Initialize(sbInit);
    }

    {
        // Decal bounds and instance buffers
        StructuredBufferInit sbInit;
        sbInit.Stride = sizeof(ClusterBounds);
        sbInit.NumElements = AppSettings::MaxDecals;
        sbInit.Dynamic = true;
        sbInit.Lifetime = BufferLifetime::Temporary;
        decalBoundsBuffer.Initialize(sbInit);

        sbInit.Stride = sizeof(uint32);
        decalInstanceBuffer.Initialize(sbInit);
    }

    {
        // Spot light bounds and instance buffers
        StructuredBufferInit sbInit;
        sbInit.Stride = sizeof(ClusterBounds);
        sbInit.NumElements = AppSettings::MaxSpotLights;
        sbInit.Dynamic = true;
        sbInit.Lifetime = BufferLifetime::Temporary;
        spotLightBoundsBuffer.Initialize(sbInit);

        sbInit.Stride = sizeof(uint32);
        spotLightInstanceBuffer.Initialize(sbInit);
    }

    {
        // Spot light and shadow bounds buffer (actually used as a constant buffer)
        StructuredBufferInit sbInit;
        sbInit.Stride = sizeof(LightConstants);
        sbInit.NumElements = 1;
        sbInit.Dynamic = false;
        sbInit.Lifetime = BufferLifetime::Persistent;
        sbInit.InitialState = D3D12_RESOURCE_STATE_COPY_DEST;

        spotLightBuffer.Initialize(sbInit);
    }

    {
        // Indirect args buffers for deferred rendering
        StructuredBufferInit sbInit;
        sbInit.NumElements = 1;
        sbInit.Stride = sizeof(D3D12_DISPATCH_ARGUMENTS);

        uint32 initData[3] = { 0, 1, 1 };
        sbInit.InitData = initData;
        sbInit.InitialState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;

        nonMsaaArgsBuffer.Initialize(sbInit);
        msaaArgsBuffer.Initialize(sbInit);
    }

    {
        CompileOptions opts;
        opts.Add("FrontFace_", 1);
        opts.Add("BackFace_", 0);
        opts.Add("Intersecting_", 0);

        // Clustering shaders
        clusterVS = CompileFromFile(L"Clusters.hlsl", "ClusterVS", ShaderType::Vertex, ShaderProfile::SM51, opts);
        clusterFrontFacePS = CompileFromFile(L"Clusters.hlsl", "ClusterPS", ShaderType::Pixel, ShaderProfile::SM51, opts);

        opts.Reset();
        opts.Add("FrontFace_", 0);
        opts.Add("BackFace_", 1);
        opts.Add("Intersecting_", 0);
        clusterBackFacePS = CompileFromFile(L"Clusters.hlsl", "ClusterPS", ShaderType::Pixel, ShaderProfile::SM51, opts);

        opts.Reset();
        opts.Add("FrontFace_", 0);
        opts.Add("BackFace_", 0);
        opts.Add("Intersecting_", 1);
        clusterIntersectingPS = CompileFromFile(L"Clusters.hlsl", "ClusterPS", ShaderType::Pixel, ShaderProfile::SM51, opts);
    }

    MakeBoxGeometry(decalClusterVtxBuffer, decalClusterIdxBuffer, 2.0f);    // resulting box is [-1, 1]
    MakeConeGeometry(NumConeSides, spotLightClusterVtxBuffer, spotLightClusterIdxBuffer, coneVertices);

    {
        // Picking buffer
        StructuredBufferInit sbInit;
        sbInit.Stride = sizeof(PickingData);
        sbInit.NumElements = 1;
        sbInit.CreateUAV = 1;
        pickingBuffer.Initialize(sbInit);
    }

    for(uint64 i = 0; i < DX12::RenderLatency; ++i)
    {
        pickingReadbackBuffers[i].Initialize(sizeof(PickingData));
        pickingReadbackBuffers[i].Resource->SetName(L"Picking Readback Buffer");
    }

    {
        // Compile picking shaders
        CompileOptions opts;
        opts.Add("MSAA_", 0);
        pickingCS[0] = CompileFromFile(L"Picking.hlsl", "PickingCS", ShaderType::Compute, ShaderProfile::SM51, opts);

        opts.Reset();
        opts.Add("MSAA_", 1);
        pickingCS[1] = CompileFromFile(L"Picking.hlsl", "PickingCS", ShaderType::Compute, ShaderProfile::SM51, opts);
    }

    // Compile MSAA mask generation shaders
    for(uint64 msaaMode = 1; msaaMode < NumMSAAModes; ++msaaMode)
    {
        CompileOptions opts;
        opts.Add("MSAASamples_", AppSettings::NumMSAASamples(MSAAModes(msaaMode)));
        opts.Add("UseZGradients_", 0);
        msaaMaskCS[msaaMode][0] = CompileFromFile(L"MSAAMask.hlsl", "MSAAMaskCS", ShaderType::Compute, ShaderProfile::SM51, opts);

        opts.Reset();
        opts.Add("MSAASamples_", AppSettings::NumMSAASamples(MSAAModes(msaaMode)));
        opts.Add("UseZGradients_", 1);
        msaaMaskCS[msaaMode][1] = CompileFromFile(L"MSAAMask.hlsl", "MSAAMaskCS", ShaderType::Compute, ShaderProfile::SM51, opts);
    }

    // Compile resolve shaders
    for(uint64 msaaMode = 1; msaaMode < NumMSAAModes; ++msaaMode)
    {
        for(uint64 deferred = 0; deferred < 2; ++deferred)
        {
            CompileOptions opts;
            opts.Add("MSAASamples_", AppSettings::NumMSAASamples(MSAAModes(msaaMode)));
            opts.Add("Deferred_", uint32(deferred));
            resolvePS[msaaMode][deferred] = CompileFromFile(L"Resolve.hlsl", "ResolvePS", ShaderType::Pixel, ShaderProfile::SM51, opts);
        }
    }

    // Compile cluster visualization shaders
    clusterVisPS = CompileFromFile(L"ClusterVisualizer.hlsl", "ClusterVisualizerPS", ShaderType::Pixel, ShaderProfile::SM51);

    std::wstring fullScreenTriPath = SampleFrameworkDir() + L"Shaders\\FullScreenTriangle.hlsl";
    fullScreenTriVS = CompileFromFile(fullScreenTriPath.c_str(), "FullScreenTriangleVS", ShaderType::Vertex, ShaderProfile::SM51);

    // Create constant buffers
    clusterConstants.Initialize(BufferLifetime::Temporary);
    msaaMaskConstants.Initialize(BufferLifetime::Temporary);
    deferredConstants.Initialize(BufferLifetime::Temporary);
    shadingConstants.Initialize(BufferLifetime::Temporary);
    pickingConstants.Initialize(BufferLifetime::Temporary);
    clusterVisConstants.Initialize(BufferLifetime::Temporary);

    {
        // Clustering root signature
        D3D12_DESCRIPTOR_RANGE srvRanges[1] = {};
        srvRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[0].NumDescriptors = 3;
        srvRanges[0].BaseShaderRegister = 0;
        srvRanges[0].RegisterSpace = 0;
        srvRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE uavRanges[1] = {};
        uavRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRanges[0].NumDescriptors = 1;
        uavRanges[0].BaseShaderRegister = 0;
        uavRanges[0].RegisterSpace = 0;
        uavRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rootParameters[4] = {};

        // CBuffer
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[0].Descriptor.RegisterSpace = 0;
        rootParameters[0].Descriptor.ShaderRegister = 0;

        // AppSettings
        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[1].Descriptor.RegisterSpace = 0;
        rootParameters[1].Descriptor.ShaderRegister = AppSettings::CBufferRegister;

        // VS SRV descriptors
        rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rootParameters[2].DescriptorTable.pDescriptorRanges = srvRanges;
        rootParameters[2].DescriptorTable.NumDescriptorRanges = ArraySize_(srvRanges);

        // PS SRV descriptors
        rootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[3].DescriptorTable.pDescriptorRanges = uavRanges;
        rootParameters[3].DescriptorTable.NumDescriptorRanges = ArraySize_(uavRanges);

        D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = 0;
        rootSignatureDesc.pStaticSamplers = nullptr;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        DX12::CreateRootSignature(&clusterRS, rootSignatureDesc);
    }

    {
        // Picking root signature
        D3D12_DESCRIPTOR_RANGE descriptorRanges[2] = {};
        descriptorRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        descriptorRanges[0].NumDescriptors = 2;
        descriptorRanges[0].BaseShaderRegister = 0;
        descriptorRanges[0].RegisterSpace = 0;
        descriptorRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        descriptorRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        descriptorRanges[1].NumDescriptors = 1;
        descriptorRanges[1].BaseShaderRegister = 0;
        descriptorRanges[1].RegisterSpace = 0;
        descriptorRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rootParameters[2] = {};
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[0].Descriptor.RegisterSpace = 0;
        rootParameters[0].Descriptor.ShaderRegister = 0;

        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[1].DescriptorTable.pDescriptorRanges = descriptorRanges;
        rootParameters[1].DescriptorTable.NumDescriptorRanges = ArraySize_(descriptorRanges);

        D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = 0;
        rootSignatureDesc.pStaticSamplers = nullptr;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        DX12::CreateRootSignature(&pickingRS, rootSignatureDesc);
    }

     {
        // Deferred root signature
        D3D12_DESCRIPTOR_RANGE descriptorRanges[2] = {};
        descriptorRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        descriptorRanges[0].NumDescriptors = 1;
        descriptorRanges[0].BaseShaderRegister = 0;
        descriptorRanges[0].RegisterSpace = 0;
        descriptorRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        descriptorRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        descriptorRanges[1].NumDescriptors = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        descriptorRanges[1].BaseShaderRegister = 0;
        descriptorRanges[1].RegisterSpace = 0;
        descriptorRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE decalTextureRanges[1] = {};
        decalTextureRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        decalTextureRanges[0].NumDescriptors = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        decalTextureRanges[0].BaseShaderRegister = 0;
        decalTextureRanges[0].RegisterSpace = 1;
        decalTextureRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rootParameters[NumDeferredRootParams] = {};

        // DeferredCBuffer
        rootParameters[Deferred_DeferredCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[Deferred_DeferredCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[Deferred_DeferredCBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[Deferred_DeferredCBuffer].Descriptor.ShaderRegister = 2;

        // PSCBuffer
        rootParameters[Deferred_PSCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[Deferred_PSCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[Deferred_PSCBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[Deferred_PSCBuffer].Descriptor.ShaderRegister = 0;

        // ShadowCBuffer
        rootParameters[Deferred_ShadowCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[Deferred_ShadowCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[Deferred_ShadowCBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[Deferred_ShadowCBuffer].Descriptor.ShaderRegister = 1;

        // LightCBuffer
        rootParameters[Deferred_LightCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[Deferred_LightCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[Deferred_LightCBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[Deferred_LightCBuffer].Descriptor.ShaderRegister = 3;

        // AppSettings
        rootParameters[Deferred_AppSettings].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[Deferred_AppSettings].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[Deferred_AppSettings].Descriptor.RegisterSpace = 0;
        rootParameters[Deferred_AppSettings].Descriptor.ShaderRegister = AppSettings::CBufferRegister;

        // Descriptors
        rootParameters[Deferred_Descriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[Deferred_Descriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[Deferred_Descriptors].DescriptorTable.pDescriptorRanges = descriptorRanges;
        rootParameters[Deferred_Descriptors].DescriptorTable.NumDescriptorRanges = ArraySize_(descriptorRanges);

        // Decal texture descriptors
        rootParameters[Deferred_DecalDescriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[Deferred_DecalDescriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[Deferred_DecalDescriptors].DescriptorTable.pDescriptorRanges = decalTextureRanges;
        rootParameters[Deferred_DecalDescriptors].DescriptorTable.NumDescriptorRanges = 1;

        D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = {};
        staticSamplers[0] = DX12::GetStaticSamplerState(SamplerState::Anisotropic, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
        staticSamplers[1] = DX12::GetStaticSamplerState(SamplerState::ShadowMapPCF, 1, 0, D3D12_SHADER_VISIBILITY_ALL);

        D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = ArraySize_(staticSamplers);
        rootSignatureDesc.pStaticSamplers = staticSamplers;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        DX12::CreateRootSignature(&deferredRootSignature, rootSignatureDesc);

        // Command signature for MSAA deferred indirect dispatch
        D3D12_INDIRECT_ARGUMENT_DESC argsDescs[1] = { };
        argsDescs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

        D3D12_COMMAND_SIGNATURE_DESC cmdSignatureDesc = { };
        cmdSignatureDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        cmdSignatureDesc.NodeMask = 0;
        cmdSignatureDesc.NumArgumentDescs = ArraySize_(argsDescs);
        cmdSignatureDesc.pArgumentDescs = argsDescs;
        DXCall(DX12::Device->CreateCommandSignature(&cmdSignatureDesc, nullptr, IID_PPV_ARGS(&deferredCmdSignature)));
    }

    {
        // MSAA mask root signature
        D3D12_DESCRIPTOR_RANGE descriptorRanges[2] = {};
        descriptorRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        descriptorRanges[0].NumDescriptors = 2;
        descriptorRanges[0].BaseShaderRegister = 0;
        descriptorRanges[0].RegisterSpace = 0;
        descriptorRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        descriptorRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        descriptorRanges[1].NumDescriptors = 3;
        descriptorRanges[1].BaseShaderRegister = 0;
        descriptorRanges[1].RegisterSpace = 0;
        descriptorRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rootParameters[3] = {};

        // MSAAMaskCBuffer
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[0].Descriptor.RegisterSpace = 0;
        rootParameters[0].Descriptor.ShaderRegister = 0;

        // AppSettings
        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[1].Descriptor.RegisterSpace = 0;
        rootParameters[1].Descriptor.ShaderRegister = AppSettings::CBufferRegister;

        // SRV descriptors
        rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[2].DescriptorTable.pDescriptorRanges = descriptorRanges;
        rootParameters[2].DescriptorTable.NumDescriptorRanges = ArraySize_(descriptorRanges);

        D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = 0;
        rootSignatureDesc.pStaticSamplers = nullptr;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        DX12::CreateRootSignature(&msaaMaskRootSignature, rootSignatureDesc);
    }

    {
        // Resolve root signature
        D3D12_DESCRIPTOR_RANGE srvRanges[1] = {};
        srvRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[0].NumDescriptors = 1;
        srvRanges[0].BaseShaderRegister = 0;
        srvRanges[0].RegisterSpace = 0;
        srvRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rootParameters[3] = {};

        // ResolveCBuffer
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[0].Constants.Num32BitValues = 2;
        rootParameters[0].Constants.RegisterSpace = 0;
        rootParameters[0].Constants.ShaderRegister = 0;

        // AppSettings
        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[1].Descriptor.RegisterSpace = 0;
        rootParameters[1].Descriptor.ShaderRegister = AppSettings::CBufferRegister;

        // SRV descriptors
        rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[2].DescriptorTable.pDescriptorRanges = srvRanges;
        rootParameters[2].DescriptorTable.NumDescriptorRanges = ArraySize_(srvRanges);

        D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = 0;
        rootSignatureDesc.pStaticSamplers = nullptr;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        DX12::CreateRootSignature(&resolveRootSignature, rootSignatureDesc);
    }

    {
        // Cluster visualization root signature
        D3D12_DESCRIPTOR_RANGE srvRanges[1] = {};
        srvRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[0].NumDescriptors = 2;
        srvRanges[0].BaseShaderRegister = 0;
        srvRanges[0].RegisterSpace = 0;
        srvRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rootParameters[3] = {};

        // CBuffer
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[0].Descriptor.RegisterSpace = 0;
        rootParameters[0].Descriptor.ShaderRegister = 0;

        // AppSettings
        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[1].Descriptor.RegisterSpace = 0;
        rootParameters[1].Descriptor.ShaderRegister = AppSettings::CBufferRegister;

        // SRV descriptors
        rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[2].DescriptorTable.pDescriptorRanges = srvRanges;
        rootParameters[2].DescriptorTable.NumDescriptorRanges = ArraySize_(srvRanges);

        D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = 0;
        rootSignatureDesc.pStaticSamplers = nullptr;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        DX12::CreateRootSignature(&clusterVisRootSignature, rootSignatureDesc);
    }
}

void BindlessDeferred::Shutdown()
{
    for(uint64 i = 0; i < ArraySize_(sceneModels); ++i)
        sceneModels[i].Shutdown();
    meshRenderer.Shutdown();
    skybox.Shutdown();
    skyCache.Shutdown();
    postProcessor.Shutdown();

    decalBuffer.Shutdown();
    decalBoundsBuffer.Shutdown();
    decalClusterBuffer.Shutdown();
    decalInstanceBuffer.Shutdown();
    for(uint64 i = 0; i < ArraySize_(decalTextures); ++i)
        decalTextures[i].Shutdown();

    spotLightBuffer.Shutdown();
    spotLightBoundsBuffer.Shutdown();
    spotLightClusterBuffer.Shutdown();
    spotLightInstanceBuffer.Shutdown();

    DX12::Release(clusterRS);
    clusterMSAATarget.Shutdown();

    decalClusterVtxBuffer.Shutdown();
    decalClusterIdxBuffer.Shutdown();

    spotLightClusterVtxBuffer.Shutdown();
    spotLightClusterIdxBuffer.Shutdown();

    DX12::Release(deferredRootSignature);
    DX12::Release(deferredCmdSignature);

    deferredConstants.Shutdown();
    msaaMaskConstants.Shutdown();
    shadingConstants.Shutdown();
    DX12::Release(msaaMaskRootSignature);
    nonMsaaTileBuffer.Shutdown();
    msaaTileBuffer.Shutdown();
    nonMsaaArgsBuffer.Shutdown();
    msaaArgsBuffer.Shutdown();
    msaaMaskBuffer.Shutdown();

    pickingBuffer.Shutdown();
    DX12::Release(pickingRS);
    pickingConstants.Shutdown();
    for(uint64 i = 0; i < ArraySize_(pickingReadbackBuffers); ++i)
        pickingReadbackBuffers[i].Shutdown();

    clusterVisConstants.Shutdown();
    DX12::Release(clusterVisRootSignature);

    mainTarget.Shutdown();
    tangentFrameTarget.Shutdown();
    resolveTarget.Shutdown();
    depthBuffer.Shutdown();
    uvTarget.Shutdown();
    uvGradientsTarget.Shutdown();
    materialIDTarget.Shutdown();
    deferredMSAATarget.Shutdown();

    DX12::Release(resolveRootSignature);
}

void BindlessDeferred::CreatePSOs()
{
    DXGI_FORMAT gBufferFormats[] = { tangentFrameTarget.Format(), uvTarget.Format(), materialIDTarget.Format(), uvGradientsTarget.Format() };
    uint64 numGBuffers = AppSettings::ComputeUVGradients ? ArraySize_(gBufferFormats) - 1 : ArraySize_(gBufferFormats);
    meshRenderer.CreatePSOs(mainTarget.Texture.Format, depthBuffer.DSVFormat, gBufferFormats, numGBuffers, mainTarget.MSAASamples);
    skybox.CreatePSOs(mainTarget.Texture.Format, depthBuffer.DSVFormat, mainTarget.MSAASamples);
    postProcessor.CreatePSOs();

    {
        // Clustering PSO
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = clusterRS;
        psoDesc.BlendState = DX12::GetBlendState(BlendState::Disabled);
        psoDesc.DepthStencilState = DX12::GetDepthState(DepthState::Disabled);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 0;
        psoDesc.VS = clusterVS.ByteCode();

        ClusterRasterizationModes rastMode = AppSettings::ClusterRasterizationMode;
        if(rastMode == ClusterRasterizationModes::MSAA4x || rastMode == ClusterRasterizationModes::MSAA8x)
        {
            psoDesc.SampleDesc.Count = clusterMSAATarget.MSAASamples;
            psoDesc.SampleDesc.Quality = DX12::StandardMSAAPattern;
            psoDesc.NumRenderTargets = 1;
            psoDesc.RTVFormats[0] = clusterMSAATarget.Format();
        }
        else
            psoDesc.SampleDesc.Count = 1;

        D3D12_CONSERVATIVE_RASTERIZATION_MODE crMode = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        if(rastMode == ClusterRasterizationModes::Conservative)
            crMode = D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;

        psoDesc.PS = clusterFrontFacePS.ByteCode();
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::BackFaceCull);
        psoDesc.RasterizerState.ConservativeRaster = crMode;
        DXCall(DX12::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&clusterFrontFacePSO)));

        psoDesc.PS = clusterBackFacePS.ByteCode();
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::FrontFaceCull);
        psoDesc.RasterizerState.ConservativeRaster = crMode;
        DXCall(DX12::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&clusterBackFacePSO)));

        psoDesc.PS = clusterIntersectingPS.ByteCode();
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::FrontFaceCull);
        psoDesc.RasterizerState.ConservativeRaster = crMode;
        DXCall(DX12::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&clusterIntersectingPSO)));
    }

    const bool msaaEnabled = AppSettings::MSAAMode != MSAAModes::MSAANone;
    const uint64 msaaModeIdx = uint64(AppSettings::MSAAMode);

    if(msaaEnabled)
    {
        // MSAA mask PSO's
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = { };
        psoDesc.CS = msaaMaskCS[uint64(AppSettings::MSAAMode)][0].ByteCode();
        psoDesc.pRootSignature = msaaMaskRootSignature;
        DXCall(DX12::Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&msaaMaskPSOs[0])));

        psoDesc.CS = msaaMaskCS[uint64(AppSettings::MSAAMode)][1].ByteCode();
        DXCall(DX12::Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&msaaMaskPSOs[1])));
    }

    {
        // Deferred rendering PSO
        const uint64 uvGradIdx = AppSettings::ComputeUVGradients ? 1 : 0;
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = { };
        psoDesc.CS = deferredCS[msaaModeIdx][uvGradIdx][0].ByteCode();
        psoDesc.pRootSignature = deferredRootSignature;
        DXCall(DX12::Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&deferredPSOs[0])));

        if(AppSettings::MSAAMode != MSAAModes::MSAANone)
        {
            psoDesc.CS = deferredCS[msaaModeIdx][uvGradIdx][1].ByteCode();
            DXCall(DX12::Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&deferredPSOs[1])));
        }
    }

    if(msaaEnabled)
    {
        // Resolve PSO
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = resolveRootSignature;
        psoDesc.VS = fullScreenTriVS.ByteCode();
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::NoCull);
        psoDesc.BlendState = DX12::GetBlendState(BlendState::Disabled);
        psoDesc.DepthStencilState = DX12::GetDepthState(DepthState::Disabled);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = mainTarget.Format();
        psoDesc.SampleDesc.Count = 1;

        psoDesc.PS = resolvePS[msaaModeIdx][0].ByteCode();
        DXCall(DX12::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&resolvePSOs[0])));

        psoDesc.PS = resolvePS[msaaModeIdx][1].ByteCode();
        DXCall(DX12::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&resolvePSOs[1])));
    }

    {
        // Cluster visualizer PSO
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = clusterVisRootSignature;
        psoDesc.VS = fullScreenTriVS.ByteCode();
        psoDesc.PS = clusterVisPS.ByteCode();
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::NoCull);
        psoDesc.BlendState = DX12::GetBlendState(BlendState::AlphaBlend);
        psoDesc.DepthStencilState = DX12::GetDepthState(DepthState::Disabled);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = swapChain.Format();
        psoDesc.SampleDesc.Count = 1;
        DXCall(DX12::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&clusterVisPSO)));
    }

    {
        // Picking PSO
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = { };
        psoDesc.CS = pickingCS[0].ByteCode();
        psoDesc.pRootSignature = pickingRS;
        DXCall(DX12::Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pickingPSOs[0])));

        psoDesc.CS = pickingCS[1].ByteCode();
        DXCall(DX12::Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pickingPSOs[1])));
    }
}

void BindlessDeferred::DestroyPSOs()
{
    meshRenderer.DestroyPSOs();
    skybox.DestroyPSOs();
    postProcessor.DestroyPSOs();
    DX12::DeferredRelease(clusterFrontFacePSO);
    DX12::DeferredRelease(clusterBackFacePSO);
    DX12::DeferredRelease(clusterIntersectingPSO);
    DX12::DeferredRelease(pickingPSOs[0]);
    DX12::DeferredRelease(pickingPSOs[1]);
    DX12::DeferredRelease(clusterVisPSO);
    for(uint64 i = 0; i < ArraySize_(msaaMaskPSOs); ++i)
        DX12::DeferredRelease(msaaMaskPSOs[i]);
    for(uint64 i = 0; i < ArraySize_(deferredPSOs); ++i)
        DX12::DeferredRelease(deferredPSOs[i]);
    for(uint64 i = 0; i < ArraySize_(resolvePSOs); ++i)
        DX12::DeferredRelease(resolvePSOs[i]);
}

// Creates all required render targets
void BindlessDeferred::CreateRenderTargets()
{
    uint32 width = swapChain.Width();
    uint32 height = swapChain.Height();
    const uint32 NumSamples = AppSettings::NumMSAASamples();

    mainTarget.Initialize(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, NumSamples, 1, NumSamples == 1);

    tangentFrameTarget.Initialize(width, height, DXGI_FORMAT_R10G10B10A2_UNORM, NumSamples, 1, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    uvTarget.Initialize(width, height, DXGI_FORMAT_R16G16B16A16_SNORM, NumSamples, 1, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    uvGradientsTarget.Initialize(width, height, DXGI_FORMAT_R16G16B16A16_SNORM, NumSamples, 1, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    materialIDTarget.Initialize(width, height, DXGI_FORMAT_R8_UINT, NumSamples, 1, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if(NumSamples > 1)
    {
        resolveTarget.Initialize(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, false);

        uint32 deferredWidth = width * 2;
        uint32 deferredHeight = NumSamples == 4 ? height * 2 : height;
        deferredMSAATarget.Initialize(deferredWidth, deferredHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, true);
    }

    depthBuffer.Initialize(width, height, DXGI_FORMAT_D32_FLOAT, NumSamples, 1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_DEPTH_READ);
    depthBuffer.Resource()->SetName(L"Main Depth Buffer");

    AppSettings::NumXTiles = (width + (AppSettings::ClusterTileSize - 1)) / AppSettings::ClusterTileSize;
    AppSettings::NumYTiles = (height + (AppSettings::ClusterTileSize - 1)) / AppSettings::ClusterTileSize;
    const uint64 numXYZTiles = AppSettings::NumXTiles * AppSettings::NumYTiles * AppSettings::NumZTiles;

    // Render target for forcing MSAA during cluster rasterization. Ideally we would use ForcedSampleCount for this,
    // but it's currently causing the Nvidia driver to crash. :(
    ClusterRasterizationModes rastMode = AppSettings::ClusterRasterizationMode;
    if(rastMode == ClusterRasterizationModes::MSAA4x)
        clusterMSAATarget.Initialize(AppSettings::NumXTiles, AppSettings::NumYTiles, DXGI_FORMAT_R8_UNORM, 4, 1, false);
    else if(rastMode == ClusterRasterizationModes::MSAA8x)
        clusterMSAATarget.Initialize(AppSettings::NumXTiles, AppSettings::NumYTiles, DXGI_FORMAT_R8_UNORM, 8, 1, false);
    else
        clusterMSAATarget.Shutdown();

    {
        // Decal cluster bitmask buffer
        RawBufferInit rbInit;
        rbInit.NumElements = numXYZTiles * AppSettings::DecalElementsPerCluster;
        rbInit.CreateUAV = true;
        decalClusterBuffer.Initialize(rbInit);
        decalClusterBuffer.InternalBuffer.Resource->SetName(L"Decal Cluster Buffer");
    }

    {
        // Spotlight cluster bitmask buffer
        RawBufferInit rbInit;
        rbInit.NumElements = numXYZTiles * AppSettings::SpotLightElementsPerCluster;
        rbInit.CreateUAV = true;
        spotLightClusterBuffer.Initialize(rbInit);
        spotLightClusterBuffer.InternalBuffer.Resource->SetName(L"Spot Light Cluster Buffer");
    }

    {
        const uint64 numComputeTilesX = AlignTo(mainTarget.Width(), AppSettings::DeferredTileSize) / AppSettings::DeferredTileSize;
        const uint64 numComputeTilesY = AlignTo(mainTarget.Height(), AppSettings::DeferredTileSize) / AppSettings::DeferredTileSize;

        // AppendBuffer for storing coordinates of tiles with "edge" pixels for MSAA sampling
        StructuredBufferInit sbInit;
        sbInit.NumElements = numComputeTilesX * numComputeTilesY;
        sbInit.Stride = sizeof(uint32);
        sbInit.CreateUAV = true;
        sbInit.UseCounter = true;
        sbInit.InitialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        msaaTileBuffer.Initialize(sbInit);
        msaaTileBuffer.InternalBuffer.Resource->SetName(L"MSAA Tile Buffer");

        // AppendBuffer for storing coordinates of tiles with non-edge pixels for MSAA sampling
        nonMsaaTileBuffer.Initialize(sbInit);
        nonMsaaTileBuffer.InternalBuffer.Resource->SetName(L"Non-MSAA Tile Buffer");

        // Buffer storing 1 bit per pixel indicating MSAA edge pixels
        sbInit.Stride = AppSettings::DeferredTileMaskSize * sizeof(uint32);
        sbInit.UseCounter = false;
        msaaMaskBuffer.Initialize(sbInit);
        msaaMaskBuffer.InternalBuffer.Resource->SetName(L"MSAA Mask Buffer");
    }
}

void BindlessDeferred::InitializeScene()
{
    currentModel = &sceneModels[uint64(AppSettings::CurrentScene)];
    meshRenderer.Shutdown();
    DX12::FlushGPU();
    meshRenderer.Initialize(currentModel);

    camera.SetPosition(SceneCameraPositions[uint64(AppSettings::CurrentScene)]);
    camera.SetXRotation(SceneCameraRotations[uint64(AppSettings::CurrentScene)].x);
    camera.SetYRotation(SceneCameraRotations[uint64(AppSettings::CurrentScene)].y);

    for(uint64 msaaMode = 0; msaaMode < uint64(MSAAModes::NumValues); ++msaaMode)
    {
        const uint32 msaa = msaaMode > 0;
        const uint32 numMSAASamples = AppSettings::NumMSAASamples(MSAAModes(msaaMode));

        for(uint32 computeUVGradients = 0; computeUVGradients < 2; ++computeUVGradients)
        {
            // Compile deferred shaders
            CompileOptions opts;
            opts.Add("MSAA_", msaa);
            opts.Add("NumMSAASamples_", numMSAASamples);
            opts.Add("ShadePerSample_", 0);
            opts.Add("NumMaterialTextures_", uint32(currentModel->MaterialTextures().Count()));
            opts.Add("ComputeUVGradients_", computeUVGradients);
            deferredCS[msaaMode][computeUVGradients][0] = CompileFromFile(L"Deferred.hlsl", "DeferredCS", ShaderType::Compute, ShaderProfile::SM51, opts);

            if(msaa)
            {
                opts.Reset();
                opts.Add("MSAA_", msaa);
                opts.Add("NumMSAASamples_", numMSAASamples);
                opts.Add("ShadePerSample_", 1);
                opts.Add("NumMaterialTextures_", uint32(currentModel->MaterialTextures().Count()));
                opts.Add("ComputeUVGradients_", computeUVGradients);
                deferredCS[msaaMode][computeUVGradients][1] = CompileFromFile(L"Deferred.hlsl", "DeferredCS", ShaderType::Compute, ShaderProfile::SM51, opts);
            }
        }
    }

    {
        // Initialize the spotlight data used for rendering
        const uint64 numSpotLights = Min(currentModel->SpotLights().Size(), AppSettings::MaxSpotLights);
        spotLights.Init(numSpotLights);

        for(uint64 i = 0; i < numSpotLights; ++i)
        {
            const ModelSpotLight& srcLight = currentModel->SpotLights()[i];

            SpotLight& spotLight = spotLights[i];
            spotLight.Position = srcLight.Position;
            spotLight.Direction = -srcLight.Direction;
            spotLight.Intensity = srcLight.Intensity * 25.0f;
            spotLight.AngularAttenuationX = std::cos(srcLight.AngularAttenuation.x * 0.5f);
            spotLight.AngularAttenuationY = std::cos(srcLight.AngularAttenuation.y * 0.5f);
            spotLight.Range = AppSettings::SpotLightRange;
        }

        AppSettings::MaxLightClamp.SetValue(int32(numSpotLights));
    }

    numDecals = 0;
}

void BindlessDeferred::Update(const Timer& timer)
{
    CPUProfileBlock profileBlock("Update");

    AppSettings::UpdateUI();

    MouseState mouseState = MouseState::GetMouseState(window);
    KeyboardState kbState = KeyboardState::GetKeyboardState(window);

    currMouseState = mouseState;

    if(kbState.IsKeyDown(KeyboardState::Escape))
        window.Destroy();

    float CamMoveSpeed = 5.0f * timer.DeltaSecondsF();
    const float CamRotSpeed = 0.180f * timer.DeltaSecondsF();

    // Move the camera with keyboard input
    if(kbState.IsKeyDown(KeyboardState::LeftShift))
        CamMoveSpeed *= 0.25f;

    Float3 camPos = camera.Position();
    if(kbState.IsKeyDown(KeyboardState::W))
        camPos += camera.Forward() * CamMoveSpeed;
    else if (kbState.IsKeyDown(KeyboardState::S))
        camPos += camera.Back() * CamMoveSpeed;
    if(kbState.IsKeyDown(KeyboardState::A))
        camPos += camera.Left() * CamMoveSpeed;
    else if (kbState.IsKeyDown(KeyboardState::D))
        camPos += camera.Right() * CamMoveSpeed;
    if(kbState.IsKeyDown(KeyboardState::Q))
        camPos += camera.Up() * CamMoveSpeed;
    else if (kbState.IsKeyDown(KeyboardState::E))
        camPos += camera.Down() * CamMoveSpeed;
    camera.SetPosition(camPos);

    // Rotate the camera with the mouse
    if(mouseState.RButton.Pressed && mouseState.IsOverWindow)
    {
        float xRot = camera.XRotation();
        float yRot = camera.YRotation();
        xRot += mouseState.DY * CamRotSpeed;
        yRot += mouseState.DX * CamRotSpeed;
        camera.SetXRotation(xRot);
        camera.SetYRotation(yRot);
    }

    UpdateDecals(timer);
    UpdateLights();

    appViewMatrix = camera.ViewMatrix();

    // Toggle VSYNC
    swapChain.SetVSYNCEnabled(AppSettings::EnableVSync ? true : false);

    skyCache.Init(AppSettings::SunDirection, AppSettings::SunSize, AppSettings::GroundAlbedo, AppSettings::Turbidity, true);

    if(AppSettings::MSAAMode.Changed() || AppSettings::ClusterRasterizationMode.Changed())
    {
        DestroyPSOs();
        CreateRenderTargets();
        CreatePSOs();
    }

    if(AppSettings::CurrentScene.Changed())
    {
        currentModel = &sceneModels[uint64(AppSettings::CurrentScene)];
        DestroyPSOs();
        InitializeScene();
        CreatePSOs();
    }

    if(AppSettings::ComputeUVGradients.Changed())
    {
        DestroyPSOs();
        CreatePSOs();
    }
}

void BindlessDeferred::Render(const Timer& timer)
{
    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;

    CPUProfileBlock cpuProfileBlock("Render");
    ProfileBlock gpuProfileBlock(cmdList, "Render Total");

    RenderClusters();

    if(AppSettings::EnableSun)
        meshRenderer.RenderSunShadowMap(cmdList, camera);

    if(AppSettings::RenderLights)
        meshRenderer.RenderSpotLightShadowMap(cmdList, camera);

    // Update the light constant buffer
    spotLightBuffer.InternalBuffer.UpdateData(spotLights.Data(), spotLights.MemorySize(), 0);
    spotLightBuffer.InternalBuffer.UpdateData(meshRenderer.SpotLightShadowMatrices(), spotLights.Size() * sizeof(Float4x4),
                                              sizeof(SpotLight) * AppSettings::MaxSpotLights);
    spotLightBuffer.Transition(DX12::CmdList, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    if(AppSettings::RenderMode == RenderModes::ClusteredForward)
        RenderForward();
    else
        RenderDeferred();

    RenderPicking();
    RenderResolve();

    RenderTexture& finalRT = mainTarget.MSAASamples > 1 ? resolveTarget : mainTarget;

    {
        ProfileBlock ppProfileBlock(cmdList, "Post Processing");
        postProcessor.Render(cmdList, finalRT, swapChain.BackBuffer());
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[1] = { swapChain.BackBuffer().RTV.CPUHandle };
    cmdList->OMSetRenderTargets(1, rtvHandles, false, nullptr);

    RenderClusterVisualizer();

    DX12::SetViewport(cmdList, swapChain.Width(), swapChain.Height());

    RenderHUD(timer);

    // Transition updatable resources back to copy dest state
    decalBuffer.Transition(DX12::CmdList, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST);
    spotLightBuffer.Transition(DX12::CmdList, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COPY_DEST);
}

void BindlessDeferred::UpdateDecals(const Timer& timer)
{
    if(AppSettings::ClearDecals.Pressed())
        numDecals = 0;

    // Update picking and placing new decals
    cursorDecal = Decal();
    cursorDecal.Type = uint32(-1);
    cursorDecalIntensity = 0.0f;
    if(currMouseState.IsOverWindow && AppSettings::EnableDecalPicker)
    {
        // Update the decal cursor
        const uint64 texIdx = currDecalType * AppSettings::NumTexturesPerDecal;
        Assert_(texIdx < ArraySize_(decalTextures));

        Float2 textureSize;
        textureSize.x = float(decalTextures[texIdx].Width);
        textureSize.y = float(decalTextures[texIdx].Height);
        const float sizeScale = 1 / 1024.0f;

        const PickingData* pickingData = pickingReadbackBuffers[DX12::CurrFrameIdx].Map<PickingData>();
        if(pickingData->Normal != Float3(0.0f, 0.0f, 0.0f))
        {
            const float decalThickness = 0.125f;

            cursorDecal.Position = pickingData->Position;
            cursorDecal.Size = Float3(textureSize.x * sizeScale, textureSize.y * sizeScale, decalThickness);
            cursorDecal.Type = uint32(currDecalType);
            cursorDecalIntensity = float(std::cos(timer.ElapsedSecondsD() * Pi2)) * 0.25f + 0.5f;

            Float3 forward = -pickingData->Normal;
            Float3 up = std::abs(Float3::Dot(forward, Float3(0.0f, 1.0f, 0.0f))) < 0.99f ? Float3(0.0f, 1.0f, 0.0f) : Float3(0.0f, 0.0f, 1.0f);
            Float3 right = Float3::Normalize(Float3::Cross(up, forward));
            up = Float3::Cross(forward, right);
            Float3x3 orientation = Float3x3(right, up, forward);

            cursorDecal.Orientation = Quaternion(orientation);

            if(currMouseState.MButton.RisingEdge)
            {
                // Place a new decal, and fill the buffer
                const uint64 decalIdx = (numDecals++) % AppSettings::MaxDecals;
                decals[decalIdx] = cursorDecal;

                currDecalType = (currDecalType + 1) % uint64(AppSettings::NumDecalTypes);
            }
        }

        pickingReadbackBuffers[DX12::CurrFrameIdx].Unmap();
    }

    // Update the Z bounds, and fill the buffers
    const Float4x4 viewMatrix = camera.ViewMatrix();
    const float nearClip = camera.NearClip();
    const float farClip = camera.FarClip();
    const float zRange = farClip - nearClip;
    const Float3 cameraPos = camera.Position();
    const float3 boxVerts[8] = { float3(-1,  1, -1), float3(1,  1, -1), float3(-1,  1, 1), float3(1,  1, 1),
                                      float3(-1, -1, -1), float3(1, -1, -1), float3(-1, -1, 1), float3(1, -1, 1) };

    // Come up with an oriented bounding box that surrounds the near clipping plane. We'll test this box
    // for intersection with the decal's bounding box, and use that to estimate if the bounding
    // geometry will end up getting clipped by the camera's near clipping plane
    Float3 nearClipCenter = cameraPos + nearClip * camera.Forward();
    Float4x4 invProjection = Float4x4::Invert(camera.ProjectionMatrix());
    Float3 nearTopRight = Float3::Transform(Float3(1.0f, 1.0f, 0.0f), invProjection);
    Float3 nearClipExtents = Float3(nearTopRight.x, nearTopRight.y, 0.01f);
    DirectX::BoundingOrientedBox nearClipBox(nearClipCenter.ToXMFLOAT3(), nearClipExtents.ToXMFLOAT3(), camera.Orientation().ToXMFLOAT4());

    ClusterBounds* boundsData = decalBoundsBuffer.Map<ClusterBounds>();
    bool intersectsCamera[AppSettings::MaxDecals] = { };

    const uint64 numDecalsToUpdate = Min(numDecals, AppSettings::MaxDecals);
    for(uint64 decalIdx = 0; decalIdx < numDecalsToUpdate; ++decalIdx)
    {
        const Decal& decal = decals[decalIdx];

        // Compute conservative Z bounds for the decal based on vertices of the bounding geometry
        float minZ = FloatMax;
        float maxZ = -FloatMax;
        for(uint64 i = 0; i < 8; ++i)
        {
            Float3 boxVert = boxVerts[i] * decal.Size;
            boxVert = Float3::Transform(boxVert, decal.Orientation);
            boxVert += decal.Position;

            float vertZ = Float3::Transform(boxVert, viewMatrix).z;
            minZ = Min(minZ, vertZ);
            maxZ = Max(maxZ, vertZ);
        }

        minZ = Saturate((minZ - nearClip) / zRange);
        maxZ = Saturate((maxZ - nearClip) / zRange);

        uint64 minZTile = uint64(minZ * AppSettings::NumZTiles);
        uint64 maxZTile = Min(uint64(maxZ * AppSettings::NumZTiles), AppSettings::NumZTiles - 1);

        ClusterBounds bounds;
        bounds.Position = decal.Position;
        bounds.Orientation = decal.Orientation;
        bounds.Scale = decal.Size;
        bounds.ZBounds = Uint2(uint32(minZTile), uint32(maxZTile));
        boundsData[decalIdx] = bounds;

        // Estimate if this decal's bounding geometry intersects with the camera's near clip plane
        DirectX::BoundingOrientedBox box = DirectX::BoundingOrientedBox(decal.Position.ToXMFLOAT3(), decal.Size.ToXMFLOAT3(), decal.Orientation.ToXMFLOAT4());
        intersectsCamera[decalIdx] = box.Intersects(nearClipBox);
    }

    decalBuffer.UpdateData(decals.Data(), decals.Size(), 0);
    decalBuffer.Transition(DX12::CmdList, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);

    numIntersectingDecals = 0;
    uint32* instanceData = decalInstanceBuffer.Map<uint32>();

    for(uint64 decalIdx = 0; decalIdx < numDecalsToUpdate; ++decalIdx)
        if(intersectsCamera[decalIdx])
            instanceData[numIntersectingDecals++] = uint32(decalIdx);

    uint64 offset = numIntersectingDecals;
    for(uint64 decalIdx = 0; decalIdx < numDecalsToUpdate; ++decalIdx)
        if(intersectsCamera[decalIdx] == false)
            instanceData[offset++] = uint32(decalIdx);
}

void BindlessDeferred::UpdateLights()
{
    const uint64 numSpotLights = Min<uint64>(spotLights.Size(), AppSettings::MaxLightClamp);

    // This is an additional scale factor that's needed to make sure that our polygonal bounding cone
    // fully encloses the actual cone representing the light's area of influence
    const float inRadius = std::cos(Pi / NumConeSides);
    const float scaleCorrection = 1.0f / inRadius;

    const Float4x4 viewMatrix = camera.ViewMatrix();
    const float nearClip = camera.NearClip();
    const float farClip = camera.FarClip();
    const float zRange = farClip - nearClip;
    const Float3 cameraPos = camera.Position();
    const uint64 numConeVerts = coneVertices.Size();

    // Come up with a bounding sphere that surrounds the near clipping plane. We'll test this sphere
    // for intersection with the spot light's bounding cone, and use that to over-estimate if the bounding
    // geometry will end up getting clipped by the camera's near clipping plane
    Float3 nearClipCenter = cameraPos + nearClip * camera.Forward();
    Float4x4 invViewProjection = Float4x4::Invert(camera.ViewProjectionMatrix());
    Float3 nearTopRight = Float3::Transform(Float3(1.0f, 1.0f, 0.0f), invViewProjection);
    float nearClipRadius = Float3::Length(nearTopRight - nearClipCenter);

    ClusterBounds* boundsData = spotLightBoundsBuffer.Map<ClusterBounds>();
    bool intersectsCamera[AppSettings::MaxDecals] = { };

    // Update the light bounds buffer
    for(uint64 spotLightIdx = 0; spotLightIdx < numSpotLights; ++spotLightIdx)
    {
        const SpotLight& spotLight = spotLights[spotLightIdx];
        const ModelSpotLight& srcSpotLight = currentModel->SpotLights()[spotLightIdx];
        ClusterBounds bounds;
        bounds.Position = spotLight.Position;
        bounds.Orientation = srcSpotLight.Orientation;
        bounds.Scale.x = bounds.Scale.y = std::tan(srcSpotLight.AngularAttenuation.y / 2.0f) * spotLight.Range * scaleCorrection;
        bounds.Scale.z = spotLight.Range;

        // Compute conservative Z bounds for the light based on vertices of the bounding geometry
        float minZ = FloatMax;
        float maxZ = -FloatMax;
        for(uint64 i = 0; i < numConeVerts; ++i)
        {
            Float3 coneVert = coneVertices[i] * bounds.Scale;
            coneVert = Float3::Transform(coneVert, bounds.Orientation);
            coneVert += bounds.Position;

            float vertZ = Float3::Transform(coneVert, viewMatrix).z;
            minZ = Min(minZ, vertZ);
            maxZ = Max(maxZ, vertZ);
        }

        minZ = Saturate((minZ - nearClip) / zRange);
        maxZ = Saturate((maxZ - nearClip) / zRange);

        bounds.ZBounds.x = uint32(minZ * AppSettings::NumZTiles);
        bounds.ZBounds.y = Min(uint32(maxZ * AppSettings::NumZTiles), uint32(AppSettings::NumZTiles - 1));

        // Estimate if the light's bounding geometry intersects with the camera's near clip plane
        boundsData[spotLightIdx] = bounds;
        intersectsCamera[spotLightIdx] = SphereConeIntersection(spotLight.Position, srcSpotLight.Direction, spotLight.Range,
                                                                srcSpotLight.AngularAttenuation.y, nearClipCenter, nearClipRadius);
    }

    numIntersectingSpotLights = 0;
    uint32* instanceData = spotLightInstanceBuffer.Map<uint32>();

    for(uint64 spotLightIdx = 0; spotLightIdx < numSpotLights; ++spotLightIdx)
        if(intersectsCamera[spotLightIdx])
            instanceData[numIntersectingSpotLights++] = uint32(spotLightIdx);

    uint64 offset = numIntersectingSpotLights;
    for(uint64 spotLightIdx = 0; spotLightIdx < numSpotLights; ++spotLightIdx)
        if(intersectsCamera[spotLightIdx] == false)
            instanceData[offset++] = uint32(spotLightIdx);
}

void BindlessDeferred::RenderClusters()
{
    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;

    PIXMarker marker(cmdList, "Cluster Update");
    ProfileBlock profileBlock(cmdList, "Cluster Update");

    decalClusterBuffer.MakeWritable(cmdList);
    spotLightClusterBuffer.MakeWritable(cmdList);

    if(AppSettings::RenderDecals)
    {
        // Clear decal clusters
        D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptors[1] = { decalClusterBuffer.UAV() };
        DescriptorHandle gpuHandle = DX12::MakeDescriptorTable(ArraySize_(cpuDescriptors), cpuDescriptors);

        uint32 values[4] = { };
        cmdList->ClearUnorderedAccessViewUint(gpuHandle.GPUHandle, cpuDescriptors[0], decalClusterBuffer.InternalBuffer.Resource, values, 0, nullptr);
    }

    if(AppSettings::RenderLights)
    {
        // Clear spot light clusters
        D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptors[1] = { spotLightClusterBuffer.UAV() };
        DescriptorHandle gpuHandle = DX12::MakeDescriptorTable(ArraySize_(cpuDescriptors), cpuDescriptors);

        uint32 values[4] = { };
        cmdList->ClearUnorderedAccessViewUint(gpuHandle.GPUHandle, cpuDescriptors[0], spotLightClusterBuffer.InternalBuffer.Resource, values, 0, nullptr);
    }

    clusterConstants.Data.ViewProjection = camera.ViewProjectionMatrix();
    clusterConstants.Data.InvProjection = Float4x4::Invert(camera.ProjectionMatrix());
    clusterConstants.Data.NearClip = camera.NearClip();
    clusterConstants.Data.FarClip = camera.FarClip();
    clusterConstants.Data.InvClipRange = 1.0f / (camera.FarClip() - camera.NearClip());
    clusterConstants.Data.NumXTiles = uint32(AppSettings::NumXTiles);
    clusterConstants.Data.NumYTiles = uint32(AppSettings::NumYTiles);
    clusterConstants.Data.NumXYTiles = uint32(AppSettings::NumXTiles * AppSettings::NumYTiles);
    clusterConstants.Data.InstanceOffset = 0;
    clusterConstants.Data.NumLights = Min<uint32>(uint32(spotLights.Size()), AppSettings::MaxLightClamp);
    clusterConstants.Data.NumDecals = uint32(Min(numDecals, AppSettings::MaxDecals));

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[1] = { clusterMSAATarget.RTV.CPUHandle };
    ClusterRasterizationModes rastMode = AppSettings::ClusterRasterizationMode;
    if(rastMode == ClusterRasterizationModes::MSAA4x || rastMode == ClusterRasterizationModes::MSAA8x)
        cmdList->OMSetRenderTargets(1, rtvHandles, false, nullptr);
    else
        cmdList->OMSetRenderTargets(0, nullptr, false, nullptr);

    DX12::SetViewport(cmdList, AppSettings::NumXTiles, AppSettings::NumYTiles);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    cmdList->SetGraphicsRootSignature(clusterRS);

    if(AppSettings::RenderDecals)
    {
        // Update decal clusters
        decalClusterBuffer.UAVBarrier(cmdList);

        D3D12_INDEX_BUFFER_VIEW ibView = decalClusterIdxBuffer.IBView();
        cmdList->IASetIndexBuffer(&ibView);

        clusterConstants.Data.ElementsPerCluster = uint32(AppSettings::DecalElementsPerCluster);
        clusterConstants.Data.InstanceOffset = 0;
        clusterConstants.Upload();
        clusterConstants.SetAsGfxRootParameter(cmdList, 0);

        AppSettings::BindCBufferGfx(cmdList, 1);

        D3D12_CPU_DESCRIPTOR_HANDLE srvDescriptors[] = { decalBoundsBuffer.SRV(), decalClusterVtxBuffer.SRV(), decalInstanceBuffer.SRV() };
        DX12::BindShaderResources(cmdList, 2, ArraySize_(srvDescriptors), srvDescriptors);

        D3D12_CPU_DESCRIPTOR_HANDLE uavDescriptors[] = { decalClusterBuffer.UAV() };
        DX12::BindShaderResources(cmdList, 3, ArraySize_(uavDescriptors), uavDescriptors);

        const uint64 numDecalsToRender = Min(numDecals, AppSettings::MaxDecals);
        Assert_(numIntersectingDecals <= numDecalsToRender);
        const uint64 numNonIntersecting = numDecalsToRender - numIntersectingDecals;

        // Render back faces for decals that intersect with the camera
        cmdList->SetPipelineState(clusterIntersectingPSO);

        cmdList->DrawIndexedInstanced(uint32(decalClusterIdxBuffer.NumElements), uint32(numIntersectingDecals), 0, 0, 0);

        // Now for all other decals, render the back faces followed by the front faces
        cmdList->SetPipelineState(clusterBackFacePSO);

        clusterConstants.Data.InstanceOffset = uint32(numIntersectingDecals);
        clusterConstants.Upload();
        clusterConstants.SetAsGfxRootParameter(cmdList, 0);

        cmdList->DrawIndexedInstanced(uint32(decalClusterIdxBuffer.NumElements), uint32(numNonIntersecting), 0, 0, 0);

        decalClusterBuffer.UAVBarrier(cmdList);

        cmdList->SetPipelineState(clusterFrontFacePSO);

        cmdList->DrawIndexedInstanced(uint32(decalClusterIdxBuffer.NumElements), uint32(numNonIntersecting), 0, 0, 0);
    }

    if(AppSettings::RenderLights)
    {
        // Update decal clusters
        spotLightClusterBuffer.UAVBarrier(cmdList);

        D3D12_INDEX_BUFFER_VIEW ibView = spotLightClusterIdxBuffer.IBView();
        cmdList->IASetIndexBuffer(&ibView);

        clusterConstants.Data.ElementsPerCluster = uint32(AppSettings::SpotLightElementsPerCluster);
        clusterConstants.Data.InstanceOffset = 0;
        clusterConstants.Upload();

        clusterConstants.SetAsGfxRootParameter(cmdList, 0);

        AppSettings::BindCBufferGfx(cmdList, 1);

        D3D12_CPU_DESCRIPTOR_HANDLE srvDescriptors[] = { spotLightBoundsBuffer.SRV(), spotLightClusterVtxBuffer.SRV(), spotLightInstanceBuffer.SRV() };
        DX12::BindShaderResources(cmdList, 2, ArraySize_(srvDescriptors), srvDescriptors);

        D3D12_CPU_DESCRIPTOR_HANDLE uavDescriptors[] = { spotLightClusterBuffer.UAV() };
        DX12::BindShaderResources(cmdList, 3, ArraySize_(uavDescriptors), uavDescriptors);

        const uint64 numLightsToRender = Min<uint64>(spotLights.Size(), AppSettings::MaxLightClamp);
        Assert_(numIntersectingSpotLights <= numLightsToRender);
        const uint64 numNonIntersecting = numLightsToRender - numIntersectingSpotLights;

        // Render back faces for decals that intersect with the camera
        cmdList->SetPipelineState(clusterIntersectingPSO);

        cmdList->DrawIndexedInstanced(uint32(spotLightClusterIdxBuffer.NumElements), uint32(numIntersectingSpotLights), 0, 0, 0);

        // Now for all other lights, render the back faces followed by the front faces
        cmdList->SetPipelineState(clusterBackFacePSO);

        clusterConstants.Data.InstanceOffset = uint32(numIntersectingSpotLights);
        clusterConstants.Upload();
        clusterConstants.SetAsGfxRootParameter(cmdList, 0);

        cmdList->DrawIndexedInstanced(uint32(spotLightClusterIdxBuffer.NumElements), uint32(numNonIntersecting), 0, 0, 0);

        spotLightClusterBuffer.UAVBarrier(cmdList);

        cmdList->SetPipelineState(clusterFrontFacePSO);

        cmdList->DrawIndexedInstanced(uint32(spotLightClusterIdxBuffer.NumElements), uint32(numNonIntersecting), 0, 0, 0);
    }

    // Sync
    decalClusterBuffer.MakeReadable(cmdList);
    spotLightClusterBuffer.MakeReadable(cmdList);
}

void BindlessDeferred::RenderForward()
{
    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;

    PIXMarker marker(cmdList, "Forward rendering");

    {
        // Transition render targets and depth buffers back to a writable state
        D3D12_RESOURCE_BARRIER barriers[3] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[0].Transition.pResource = mainTarget.Resource();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.Subresource = 0;

        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[1].Transition.pResource = tangentFrameTarget.Resource();
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[1].Transition.Subresource = 0;

        barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[2].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[2].Transition.pResource = depthBuffer.Resource();
        barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_DEPTH_READ;
        barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barriers[2].Transition.Subresource = 0;

        cmdList->ResourceBarrier(ArraySize_(barriers), barriers);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[2] = { mainTarget.RTV.CPUHandle, tangentFrameTarget.RTV.CPUHandle };
    cmdList->OMSetRenderTargets(2, rtvHandles, false, &depthBuffer.DSV.CPUHandle);

    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    cmdList->ClearRenderTargetView(rtvHandles[0], clearColor, 0, nullptr);
    cmdList->ClearRenderTargetView(rtvHandles[1], clearColor, 0, nullptr);
    cmdList->ClearDepthStencilView(depthBuffer.DSV.CPUHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    DX12::SetViewport(cmdList, mainTarget.Width(), mainTarget.Height());

    if(AppSettings::DepthPrepass)
        meshRenderer.RenderDepthPrepass(cmdList, camera);

    {
        ProfileBlock profileBlock(cmdList, "Forward Rendering Pass");

        // Render the main forward pass
        MainPassData mainPassData;
        mainPassData.SkyCache = &skyCache;
        mainPassData.DecalTextures = decalTextures;
        mainPassData.DecalBuffer = &decalBuffer;
        mainPassData.CursorDecal = cursorDecal;
        mainPassData.CursorDecalIntensity = cursorDecalIntensity;
        mainPassData.DecalClusterBuffer = &decalClusterBuffer;
        mainPassData.SpotLightBuffer = &spotLightBuffer;
        mainPassData.SpotLightClusterBuffer = &spotLightClusterBuffer;
        meshRenderer.RenderMainPass(cmdList, camera, mainPassData);

        cmdList->OMSetRenderTargets(1, rtvHandles, false, &depthBuffer.DSV.CPUHandle);

        // Render the sky
        skybox.RenderSky(cmdList, camera.ViewMatrix(), camera.ProjectionMatrix(), skyCache, true);

        {
            // Make our targets readable again, which will force a sync point
            D3D12_RESOURCE_BARRIER barriers[3] = {};
            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[0].Transition.pResource = mainTarget.Resource();
            barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barriers[0].Transition.Subresource = 0;

            barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[1].Transition.pResource = tangentFrameTarget.Resource();
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[1].Transition.Subresource = 0;

            barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[2].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[2].Transition.pResource = depthBuffer.Resource();
            barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_DEPTH_READ;
            barriers[2].Transition.Subresource = 0;

            cmdList->ResourceBarrier(ArraySize_(barriers), barriers);
        }
    }
}

void BindlessDeferred::RenderDeferred()
{
    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;

    PIXMarker marker(cmdList, "Render Deferred");

    {
        // Transition our G-Buffer targets to a writable state
        D3D12_RESOURCE_BARRIER barriers[5] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[0].Transition.pResource = depthBuffer.Resource();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_DEPTH_READ;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barriers[0].Transition.Subresource = 0;

        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[1].Transition.pResource = tangentFrameTarget.Resource();
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[1].Transition.Subresource = 0;

        barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[2].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[2].Transition.pResource = uvTarget.Resource();
        barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[2].Transition.Subresource = 0;

        barriers[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[3].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[3].Transition.pResource = materialIDTarget.Resource();
        barriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[3].Transition.Subresource = 0;

        barriers[4].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[4].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[4].Transition.pResource = uvGradientsTarget.Resource();
        barriers[4].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[4].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[4].Transition.Subresource = 0;

        const uint32 numBarriers = AppSettings::ComputeUVGradients ? ArraySize_(barriers) - 1 : ArraySize_(barriers);
        cmdList->ResourceBarrier(numBarriers, barriers);
    }

    {
        // Set the G-Buffer render targets and clear them
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[] = { tangentFrameTarget.RTV.CPUHandle, uvTarget.RTV.CPUHandle,
                                                     materialIDTarget.RTV.CPUHandle, uvGradientsTarget.RTV.CPUHandle };
        const uint32 numTargets = AppSettings::ComputeUVGradients ? ArraySize_(rtvHandles) - 1 : ArraySize_(rtvHandles);
        cmdList->OMSetRenderTargets(numTargets, rtvHandles, false, &depthBuffer.DSV.CPUHandle);

        const float clearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for(uint64 i = 0; i < numTargets; ++i)
            cmdList->ClearRenderTargetView(rtvHandles[i], clearColor, 0, nullptr);
        cmdList->ClearDepthStencilView(depthBuffer.DSV.CPUHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    }

    DX12::SetViewport(cmdList, mainTarget.Width(), mainTarget.Height());

    /*if(AppSettings::DepthPrepass)
        meshRenderer.RenderDepthPrepass(cmdList, camera);*/

    const uint64 msaaMode = uint64(AppSettings::MSAAMode);
    const bool msaaEnabled = AppSettings::MSAAMode != MSAAModes::MSAANone;

    {
        // Render the G-Buffer, and sync
        ProfileBlock profileBlock(cmdList, "G-Buffer Rendering");

        meshRenderer.RenderGBuffer(cmdList, camera);

        D3D12_RESOURCE_BARRIER barriers[5] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[0].Transition.pResource = depthBuffer.Resource();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_DEPTH_READ;
        barriers[0].Transition.Subresource = 0;

        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[1].Transition.pResource = tangentFrameTarget.Resource();
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.Subresource = 0;

        barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[2].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[2].Transition.pResource = uvTarget.Resource();
        barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[2].Transition.Subresource = 0;

        barriers[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[3].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[3].Transition.pResource = materialIDTarget.Resource();
        barriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[3].Transition.Subresource = 0;

        barriers[4].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[4].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[4].Transition.pResource = uvGradientsTarget.Resource();
        barriers[4].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[4].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[4].Transition.Subresource = 0;

        const uint32 numBarriers = AppSettings::ComputeUVGradients ? ArraySize_(barriers) - 1 : ArraySize_(barriers);
        cmdList->ResourceBarrier(numBarriers, barriers);
    }

    const uint32 numComputeTilesX = uint32(AlignTo(mainTarget.Width(), AppSettings::DeferredTileSize) / AppSettings::DeferredTileSize);
    const uint32 numComputeTilesY = uint32(AlignTo(mainTarget.Height(), AppSettings::DeferredTileSize) / AppSettings::DeferredTileSize);

    if(msaaEnabled)
    {
        // Generate dispatch lists for for per-sample shading
        PIXMarker maskMarker(cmdList, "MSAA Mask");
        ProfileBlock profileBlock(cmdList, "MSAA Mask");

        // Clear the structure counts
        {
            D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptors[1] = { nonMsaaTileBuffer.CounterUAV.CPUHandle };
            DescriptorHandle gpuHandle = DX12::MakeDescriptorTable(ArraySize_(cpuDescriptors), cpuDescriptors);

            uint32 values[4] = {};
            cmdList->ClearUnorderedAccessViewUint(gpuHandle.GPUHandle, cpuDescriptors[0], nonMsaaTileBuffer.CounterResource, values, 0, nullptr);
        }

        {
            D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptors[1] = { msaaTileBuffer.CounterUAV.CPUHandle };
            DescriptorHandle gpuHandle = DX12::MakeDescriptorTable(ArraySize_(cpuDescriptors), cpuDescriptors);

            uint32 values[4] = {};
            cmdList->ClearUnorderedAccessViewUint(gpuHandle.GPUHandle, cpuDescriptors[0], msaaTileBuffer.CounterResource, values, 0, nullptr);
        }

        {
            // Issue barriers
            D3D12_RESOURCE_BARRIER barriers[5] = {};
            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[0].UAV.pResource = nonMsaaTileBuffer.CounterResource;

            barriers[1] = barriers[0];
            barriers[1].UAV.pResource = msaaTileBuffer.CounterResource;

            barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[2].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[2].Transition.pResource = nonMsaaTileBuffer.Resource();
            barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[2].Transition.Subresource = 0;

            barriers[3] = barriers[2];
            barriers[3].Transition.pResource = msaaTileBuffer.Resource();

            barriers[4] = barriers[2];
            barriers[4].Transition.pResource = msaaMaskBuffer.Resource();

            cmdList->ResourceBarrier(ArraySize_(barriers), barriers);
        }

        // Generate the edge mask, and fill the buffers containing edge/non-edge tiles
        cmdList->SetComputeRootSignature(msaaMaskRootSignature);
        cmdList->SetPipelineState(AppSettings::UseZGradientsForMSAAMask ? msaaMaskPSOs[1] : msaaMaskPSOs[0]);

        msaaMaskConstants.Data.NumXTiles = numComputeTilesX;
        msaaMaskConstants.Upload();
        msaaMaskConstants.SetAsComputeRootParameter(cmdList, 0);

        AppSettings::BindCBufferCompute(cmdList, 1);

        D3D12_CPU_DESCRIPTOR_HANDLE descriptors[] =
        {
            materialIDTarget.SRV(),
            uvTarget.SRV(),
            nonMsaaTileBuffer.UAV(),
            msaaTileBuffer.UAV(),
            msaaMaskBuffer.UAV(),
        };

        DX12::BindShaderResources(cmdList, 2, ArraySize_(descriptors), descriptors, CmdListMode::Compute);

        cmdList->Dispatch(numComputeTilesX, numComputeTilesY, 1);

        {
            // Sync on our buffers
            D3D12_RESOURCE_BARRIER barriers[4] = {};
            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[0].Transition.pResource = nonMsaaTileBuffer.CounterResource;
            barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[0].Transition.Subresource = 0;

            barriers[1] = barriers[0];
            barriers[1].Transition.pResource = msaaTileBuffer.CounterResource;

            barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[2].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[2].Transition.pResource = nonMsaaArgsBuffer.Resource();
            barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
            barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            barriers[2].Transition.Subresource = 0;

            barriers[3] = barriers[2];
            barriers[3].Transition.pResource = msaaArgsBuffer.Resource();

            cmdList->ResourceBarrier(ArraySize_(barriers), barriers);
        }

        // Copy off the structure counts to our indirect args buffers
        cmdList->CopyBufferRegion(nonMsaaArgsBuffer.Resource(), 0, nonMsaaTileBuffer.CounterResource, 0, sizeof(uint32));
        cmdList->CopyBufferRegion(msaaArgsBuffer.Resource(), 0, msaaTileBuffer.CounterResource, 0, sizeof(uint32));

        {
            // Issue remaining barriers
            D3D12_RESOURCE_BARRIER barriers[7] = {};
            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[0].Transition.pResource = nonMsaaTileBuffer.CounterResource;
            barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[0].Transition.Subresource = 0;

            barriers[1] = barriers[0];
            barriers[1].Transition.pResource = msaaTileBuffer.CounterResource;

            barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[2].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[2].Transition.pResource = nonMsaaArgsBuffer.Resource();
            barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
            barriers[2].Transition.Subresource = 0;

            barriers[3] = barriers[2];
            barriers[3].Transition.pResource = msaaArgsBuffer.Resource();

            barriers[4].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[4].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[4].Transition.pResource = nonMsaaTileBuffer.Resource();
            barriers[4].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[4].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[4].Transition.Subresource = 0;

            barriers[5] = barriers[4];
            barriers[5].Transition.pResource = msaaTileBuffer.Resource();

            barriers[6] = barriers[4];
            barriers[6].Transition.pResource = msaaMaskBuffer.Resource();

            cmdList->ResourceBarrier(ArraySize_(barriers), barriers);
        }
    }

    if(msaaEnabled)
    {
        // Render the sky in the empty areas
        mainTarget.Transition(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[1] = { mainTarget.RTV.CPUHandle };
        cmdList->OMSetRenderTargets(1, rtvHandles, false, &depthBuffer.DSV.CPUHandle);

        skybox.RenderSky(cmdList, camera.ViewMatrix(), camera.ProjectionMatrix(), skyCache, true);

        mainTarget.Transition(cmdList, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    {
        // Render the full-screen deferred pass
        PIXMarker deferredMarker(cmdList, "Deferred Rendering");
        ProfileBlock profileBlock(cmdList, "Deferred Rendering");

        RenderTexture& deferredTarget = msaaEnabled ? deferredMSAATarget : mainTarget;
        deferredTarget.Transition(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        cmdList->SetComputeRootSignature(deferredRootSignature);
        cmdList->SetPipelineState(deferredPSOs[0]);

        // Set constant buffers
        deferredConstants.Data.InvViewProj = Float4x4::Invert(camera.ViewProjectionMatrix());
        deferredConstants.Data.Projection = camera.ProjectionMatrix();
        deferredConstants.Data.RTSize = Float2(float(mainTarget.Width()), float(mainTarget.Height()));
        deferredConstants.Data.NumComputeTilesX = numComputeTilesX;
        deferredConstants.Upload();
        deferredConstants.SetAsComputeRootParameter(cmdList, Deferred_DeferredCBuffer);

        shadingConstants.Data.SunDirectionWS = AppSettings::SunDirection;
        shadingConstants.Data.SunIrradiance = skyCache.SunIrradiance;
        shadingConstants.Data.CosSunAngularRadius = std::cos(DegToRad(AppSettings::SunSize));
        shadingConstants.Data.SinSunAngularRadius = std::sin(DegToRad(AppSettings::SunSize));
        shadingConstants.Data.CameraPosWS = camera.Position();

        shadingConstants.Data.CursorDecalPos = cursorDecal.Position;
        shadingConstants.Data.CursorDecalIntensity = cursorDecalIntensity;
        shadingConstants.Data.CursorDecalOrientation = cursorDecal.Orientation;
        shadingConstants.Data.CursorDecalSize = cursorDecal.Size;
        shadingConstants.Data.CursorDecalType = cursorDecal.Type;
        shadingConstants.Data.NumXTiles = uint32(AppSettings::NumXTiles);
        shadingConstants.Data.NumXYTiles = uint32(AppSettings::NumXTiles * AppSettings::NumYTiles);
        shadingConstants.Data.NearClip = camera.NearClip();
        shadingConstants.Data.FarClip = camera.FarClip();

        shadingConstants.Data.SkySH = skyCache.SH;
        shadingConstants.Upload();
        shadingConstants.SetAsComputeRootParameter(cmdList, Deferred_PSCBuffer);

        ConstantBuffer<SunShadowConstants>& sunShadowConstants = meshRenderer.SunShadowConstantBuffer();
        sunShadowConstants.Upload();
        sunShadowConstants.SetAsComputeRootParameter(cmdList, Deferred_ShadowCBuffer);

        cmdList->SetComputeRootConstantBufferView(Deferred_LightCBuffer, spotLightBuffer.InternalBuffer.GPUAddress);

        AppSettings::BindCBufferCompute(cmdList, Deferred_AppSettings);

        D3D12_CPU_DESCRIPTOR_HANDLE descriptors[] =
        {
            deferredTarget.UAV.CPUHandle,
            meshRenderer.SunShadowMap().SRV(),
            meshRenderer.SpotLightShadowMap().SRV(),
            meshRenderer.MaterialTextureIndicesBuffer().SRV(),
            decalBuffer.SRV(),
            decalClusterBuffer.SRV(),
            spotLightClusterBuffer.SRV(),
            nonMsaaTileBuffer.SRV(),
            msaaTileBuffer.SRV(),
            tangentFrameTarget.SRV(),
            uvTarget.SRV(),
            uvGradientsTarget.SRV(),
            materialIDTarget.SRV(),
            depthBuffer.SRV(),
            mainTarget.SRV(),
            msaaMaskBuffer.SRV(),
        };

        // We need to get everything into a contiguous shader-visible descriptor table
        const uint64 numMaterialTextures = currentModel->MaterialTextures().Count();
        LinearDescriptorHeap& descriptorHeap = DX12::SRVDescriptorHeapGPU[DX12::CurrFrameIdx];
        DescriptorHandle tableStart = descriptorHeap.Allocate(ArraySize_(descriptors) + numMaterialTextures);

        // First the non-material descriptors
        for(uint64 i = 0; i < ArraySize_(descriptors); ++i)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE dstDescriptor = tableStart.CPUHandle;
            dstDescriptor.ptr += i * DX12::SRVDescriptorSize;
            DX12::Device->CopyDescriptorsSimple(1, dstDescriptor, descriptors[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        // And now the material textures
        D3D12_CPU_DESCRIPTOR_HANDLE srcMaterialTextures = currentModel->MaterialTextureDescriptors();
        D3D12_CPU_DESCRIPTOR_HANDLE dstMaterialTextures = tableStart.CPUHandle;
        dstMaterialTextures.ptr += ArraySize_(descriptors) * DX12::SRVDescriptorSize;
        DX12::Device->CopyDescriptorsSimple(uint32(numMaterialTextures), dstMaterialTextures, srcMaterialTextures, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        cmdList->SetComputeRootDescriptorTable(Deferred_Descriptors, tableStart.GPUHandle);

        // Bind the decal textures
        D3D12_CPU_DESCRIPTOR_HANDLE decalDescriptors[AppSettings::NumDecalTextures] = { };
        for(uint64 i = 0; i < AppSettings::NumDecalTextures; ++i)
            decalDescriptors[i] = decalTextures[i].SRV.CPUHandle;

        DX12::BindShaderResources(cmdList, Deferred_DecalDescriptors, AppSettings::NumDecalTextures, decalDescriptors, CmdListMode::Compute);

        if(msaaEnabled)
            cmdList->ExecuteIndirect(deferredCmdSignature, 1, nonMsaaArgsBuffer.Resource(), 0, nullptr, 0);
        else
            cmdList->Dispatch(numComputeTilesX, numComputeTilesY, 1);

        if(msaaEnabled)
        {
            // No need to sync here, both passes write to different tiles
            cmdList->SetPipelineState(deferredPSOs[1]);

            cmdList->ExecuteIndirect(deferredCmdSignature, 1, msaaArgsBuffer.Resource(), 0, nullptr, 0);

            // Sync on the results
            D3D12_RESOURCE_BARRIER barriers[2] = {};
            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[0].Transition.pResource = deferredMSAATarget.Resource();
            barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barriers[0].Transition.Subresource = 0;

            barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[1].Transition.pResource = mainTarget.Resource();
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barriers[1].Transition.Subresource = 0;

            cmdList->ResourceBarrier(ArraySize_(barriers), barriers);
        }
        else
        {
            // Render the sky in the empty areas
            mainTarget.Transition(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RENDER_TARGET);

            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[1] = { mainTarget.RTV.CPUHandle };
            cmdList->OMSetRenderTargets(1, rtvHandles, false, &depthBuffer.DSV.CPUHandle);

            skybox.RenderSky(cmdList, camera.ViewMatrix(), camera.ProjectionMatrix(), skyCache, true);

            mainTarget.Transition(cmdList, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }
}

// Performs MSAA resolve with a full-screen pixel shader
void BindlessDeferred::RenderResolve()
{
    if(AppSettings::MSAAMode == MSAAModes::MSAANone)
        return;

    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;

    PIXMarker pixMarker(cmdList, "MSAA Resolve");
    ProfileBlock profileBlock(cmdList, "MSAA Resolve");

    resolveTarget.MakeWritable(cmdList);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[1] = { resolveTarget.RTV.CPUHandle };
    cmdList->OMSetRenderTargets(ArraySize_(rtvs), rtvs, false, nullptr);
    DX12::SetViewport(cmdList, resolveTarget.Width(), resolveTarget.Height());

    const uint64 deferred = AppSettings::RenderMode == RenderModes::DeferredTexturing ? 1 : 0;
    ID3D12PipelineState* pso = resolvePSOs[deferred];

    cmdList->SetGraphicsRootSignature(resolveRootSignature);
    cmdList->SetPipelineState(pso);

    cmdList->SetGraphicsRoot32BitConstant(0, uint32(mainTarget.Width()), 0);
    cmdList->SetGraphicsRoot32BitConstant(0, uint32(mainTarget.Height()), 1);

    AppSettings::BindCBufferGfx(cmdList, 1);

    D3D12_CPU_DESCRIPTOR_HANDLE srvs[1] = { deferred ? deferredMSAATarget.SRV() : mainTarget.SRV() };
    DX12::BindShaderResources(cmdList, 2, ArraySize_(srvs), srvs);

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetIndexBuffer(nullptr);
    cmdList->IASetVertexBuffers(0, 0, nullptr);

    cmdList->DrawInstanced(3, 1, 0, 0);

    resolveTarget.MakeReadable(cmdList);
}

// Runs a simple compute shader that reads depth + tangent from for a particular pixel, and copies
// the results to a readback buffer that's used to generate the "cursor" decal
void BindlessDeferred::RenderPicking()
{
    if(currMouseState.IsOverWindow == false || AppSettings::EnableDecalPicker == false)
        return;

    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;

    PIXMarker pixMarker(cmdList, "Picking");

    cmdList->SetPipelineState(AppSettings::MSAAMode != MSAAModes::MSAANone ? pickingPSOs[1] : pickingPSOs[0]);
    cmdList->SetComputeRootSignature(pickingRS);

    pickingConstants.Data.InverseViewProjection = Float4x4::Invert(camera.ViewProjectionMatrix());
    pickingConstants.Data.PixelPos = Uint2(currMouseState.X, currMouseState.Y);
    pickingConstants.Data.RTSize.x = float(mainTarget.Width());
    pickingConstants.Data.RTSize.y = float(mainTarget.Height());
    pickingConstants.Upload();
    pickingConstants.SetAsComputeRootParameter(cmdList, 0);

    pickingBuffer.MakeWritable(cmdList);

    D3D12_CPU_DESCRIPTOR_HANDLE descriptors[3] = { tangentFrameTarget.SRV(), depthBuffer.SRV(), pickingBuffer.UAV() };
    DX12::BindShaderResources(cmdList, 1, ArraySize_(descriptors), descriptors, CmdListMode::Compute);

    cmdList->Dispatch(1, 1, 1);

    pickingBuffer.MakeReadable(cmdList);

    cmdList->CopyResource(pickingReadbackBuffers[DX12::CurrFrameIdx].Resource, pickingBuffer.InternalBuffer.Resource);
}

// Renders the 2D "overhead" visualizer that shows per-cluster light/decal counts
void BindlessDeferred::RenderClusterVisualizer()
{
    if(AppSettings::ShowClusterVisualizer == false)
        return;

    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;

    PIXMarker pixMarker(cmdList, "Cluster Visualizer");

    Float2 displaySize = Float2(float(swapChain.Width()), float(swapChain.Height()));
    Float2 drawSize = displaySize * 0.375f;
    Float2 drawPos = displaySize * (0.5f + (0.5f - 0.375f) / 2.0f);

    D3D12_VIEWPORT viewport = { };
    viewport.Width = drawSize.x;
    viewport.Height = drawSize.y;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    viewport.TopLeftX = drawPos.x;
    viewport.TopLeftY = drawPos.y;

    D3D12_RECT scissorRect = { };
    scissorRect.left = 0;
    scissorRect.top = 0;
    scissorRect.right = uint32(swapChain.Width());
    scissorRect.bottom = uint32(swapChain.Height());

    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissorRect);

    cmdList->SetGraphicsRootSignature(clusterVisRootSignature);
    cmdList->SetPipelineState(clusterVisPSO);

    Float4x4 invProjection = Float4x4::Invert(camera.ProjectionMatrix());
    Float3 farTopRight = Float3::Transform(Float3(1.0f, 1.0f, 1.0f), invProjection);
    Float3 farBottomLeft = Float3::Transform(Float3(-1.0f, -1.0f, 1.0f), invProjection);

    clusterVisConstants.Data.Projection = camera.ProjectionMatrix();
    clusterVisConstants.Data.ViewMin = Float3(farBottomLeft.x, farBottomLeft.y, camera.NearClip());
    clusterVisConstants.Data.NearClip = camera.NearClip();
    clusterVisConstants.Data.ViewMax = Float3(farTopRight.x, farTopRight.y, camera.FarClip());
    clusterVisConstants.Data.InvClipRange = 1.0f / (camera.FarClip() - camera.NearClip());
    clusterVisConstants.Data.DisplaySize = displaySize;
    clusterVisConstants.Data.NumXTiles = uint32(AppSettings::NumXTiles);
    clusterVisConstants.Data.NumXYTiles = uint32(AppSettings::NumXTiles * AppSettings::NumYTiles);
    clusterVisConstants.Upload();
    clusterVisConstants.SetAsGfxRootParameter(cmdList, 0);

    AppSettings::BindCBufferGfx(cmdList, 1);

    D3D12_CPU_DESCRIPTOR_HANDLE srvs[] = { decalClusterBuffer.SRV(), spotLightClusterBuffer.SRV() };
    DX12::BindShaderResources(cmdList, 2, ArraySize_(srvs), srvs);

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetIndexBuffer(nullptr);
    cmdList->IASetVertexBuffers(0, 0, nullptr);

    cmdList->DrawInstanced(3, 1, 0, 0);
}

void BindlessDeferred::RenderHUD(const Timer& timer)
{
    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;
    PIXMarker pixMarker(cmdList, "HUD Pass");

    Float2 viewportSize;
    viewportSize.x = float(swapChain.Width());
    viewportSize.y = float(swapChain.Height());
    spriteRenderer.Begin(cmdList, viewportSize, SpriteFilterMode::Point, SpriteBlendMode::AlphaBlend);

    Float2 textPos = Float2(25.0f, 25.0f);
    wstring fpsText = MakeString(L"Frame Time: %.2fms (%u FPS)", 1000.0f / fps, fps);
    spriteRenderer.RenderText(cmdList, font, fpsText.c_str(), textPos, Float4(1.0f, 1.0f, 0.0f, 1.0f));

    spriteRenderer.End();
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    BindlessDeferred app;
    app.Run();
}
