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
#include <EnkiTS/TaskScheduler_c.h>

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

static const float SpotLightIntensityFactor = 25.0f;

static enkiTaskScheduler* taskScheduler = nullptr;
static enkiTaskSet* taskSet = nullptr;
static const bool EnableMultithreadedCompilation = true;

struct PickingData
{
    Float3 Position;
    Float3 Normal;
};

struct LightConstants
{
    SpotLight Lights[AppSettings::MaxSpotLights];
    Float4x4 ShadowMatrices[AppSettings::MaxSpotLights];
};

struct ClusterConstants
{
    Float4x4 ViewProjection;
    Float4x4 InvProjection;
    float NearClip = 0.0f;
    float FarClip = 0.0f;
    float InvClipRange = 0.0f;
    uint32 NumXTiles = 0;
    uint32 NumYTiles = 0;
    uint32 NumXYTiles = 0;
    uint32 ElementsPerCluster = 0;
    uint32 InstanceOffset = 0;
    uint32 NumLights = 0;
    uint32 NumDecals = 0;

    uint32 BoundsBufferIdx = uint32(-1);
    uint32 VertexBufferIdx = uint32(-1);
    uint32 InstanceBufferIdx = uint32(-1);
};

struct MSAAMaskConstants
{
    uint32 NumXTiles = 0;
    uint32 MaterialIDMapIdx = uint32(-1);
    uint32 UVMapIdx = uint32(-1);
};

struct DeferredConstants
{
    Float4x4 InvViewProj;
    Float4x4 Projection;
    Float2 RTSize;
    uint32 NumComputeTilesX = 0;
};

struct PickingConstants
{
    Float4x4 InverseViewProjection;
    Uint2 PixelPos;
    Float2 RTSize;
    uint32 TangentMapIdx = uint32(-1);
    uint32 DepthMapIdx = uint32(-1);
};

struct ClusterVisConstants
{
    Float4x4 Projection;
    Float3 ViewMin;
    float NearClip = 0.0f;
    Float3 ViewMax;
    float InvClipRange = 0.0f;
    Float2 DisplaySize;
    uint32 NumXTiles = 0;
    uint32 NumXYTiles = 0;

    uint32 DecalClusterBufferIdx = uint32(-1);
    uint32 SpotLightClusterBufferIdx = uint32(-1);
};

enum ClusterRootParams : uint32
{
    ClusterParams_StandardDescriptors,
    ClusterParams_UAVDescriptors,
    ClusterParams_CBuffer,
    ClusterParams_AppSettings,

    NumClusterRootParams,
};

enum MSAAMaskRootParams : uint32
{
    MSAAMaskParams_StandardDescriptors,
    MSAAMaskParams_UAVDescriptors,
    MSAAMaskParams_CBuffer,
    MSAAMaskParams_AppSettings,

    NumMSAAMaskRootParams
};

enum DeferredRootParams : uint32
{
    DeferredParams_StandardDescriptors,
    DeferredParams_PSCBuffer,
    DeferredParams_ShadowCBuffer,
    DeferredParams_DeferredCBuffer,
    DeferredParams_LightCBuffer,
    DeferredParams_SRVIndices,
    DeferredParams_UAVDescriptors,
    DeferredParams_AppSettings,

    NumDeferredRootParams
};

enum PickingRootParams : uint32
{
    PickingParams_StandardDescriptors,
    PickingParams_UAVDescriptors,
    PickingParams_CBuffer,

    NumPickingRootParams
};

enum ClusterVisRootParams : uint32
{
    ClusterVisParams_StandardDescriptors,
    ClusterVisParams_CBuffer,
    ClusterVisParams_AppSettings,

    NumClusterVisRootParams,
};

enum ResolveRootParams : uint32
{
    ResolveParams_StandardDescriptors,
    ResolveParams_Constants,
    ResolveParams_AppSettings,

    NumResolveRootParams
};

enum SSAORootParams : uint32
{
    SSAOParams_StandardDescriptors,
    SSAOParams_UAVDescriptors,
    SSAOParams_CBuffer,
    SSAOParams_AppSettings,

    NumSSAORootParams
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

BindlessDeferred::BindlessDeferred(const wchar* cmdLine) : App(L"Bindless Deferred Texturing", cmdLine)
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

    ShadowHelper::Initialize(ShadowMapMode::DepthMap, ShadowMSAAMode::MSAA1x);

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
        sbInit.Dynamic = true;
        sbInit.CPUAccessible = false;
        sbInit.InitialState = D3D12_RESOURCE_STATE_COMMON;
        decalBuffer.Initialize(sbInit);
        decalBuffer.Resource()->SetName(L"Decal Buffer");
    }

    {
        // Decal bounds and instance buffers
        StructuredBufferInit sbInit;
        sbInit.Stride = sizeof(ClusterBounds);
        sbInit.NumElements = AppSettings::MaxDecals;
        sbInit.Dynamic = true;
        sbInit.CPUAccessible = true;
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
        sbInit.CPUAccessible = true;
        spotLightBoundsBuffer.Initialize(sbInit);

        sbInit.Stride = sizeof(uint32);
        spotLightInstanceBuffer.Initialize(sbInit);
    }

    {
        // Spot light and shadow bounds buffer
        ConstantBufferInit cbInit;
        cbInit.Size = sizeof(LightConstants);
        cbInit.Dynamic = true;
        cbInit.CPUAccessible = false;
        cbInit.InitialState = D3D12_RESOURCE_STATE_COMMON;
        cbInit.Name = L"Spot Light Buffer";

        spotLightBuffer.Initialize(cbInit);
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

        nonMsaaArgsBuffer.Resource()->SetName(L"Non-MSAA Args Buffer");
        msaaArgsBuffer.Resource()->SetName(L"MSAA Args Buffer");
    }

    {
        CompileOptions opts;
        opts.Add("FrontFace_", 1);
        opts.Add("BackFace_", 0);
        opts.Add("Intersecting_", 0);

        // Clustering shaders
        clusterVS = CompileFromFile(L"Clusters.hlsl", "ClusterVS", ShaderType::Vertex, opts);
        clusterFrontFacePS = CompileFromFile(L"Clusters.hlsl", "ClusterPS", ShaderType::Pixel, opts);

        opts.Reset();
        opts.Add("FrontFace_", 0);
        opts.Add("BackFace_", 1);
        opts.Add("Intersecting_", 0);
        clusterBackFacePS = CompileFromFile(L"Clusters.hlsl", "ClusterPS", ShaderType::Pixel, opts);

        opts.Reset();
        opts.Add("FrontFace_", 0);
        opts.Add("BackFace_", 0);
        opts.Add("Intersecting_", 1);
        clusterIntersectingPS = CompileFromFile(L"Clusters.hlsl", "ClusterPS", ShaderType::Pixel, opts);
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
        pickingCS[0] = CompileFromFile(L"Picking.hlsl", "PickingCS", ShaderType::Compute, opts);

        opts.Reset();
        opts.Add("MSAA_", 1);
        pickingCS[1] = CompileFromFile(L"Picking.hlsl", "PickingCS", ShaderType::Compute, opts);
    }

    // Compile MSAA mask generation shaders
    for(uint64 msaaMode = 1; msaaMode < NumMSAAModes; ++msaaMode)
    {
        CompileOptions opts;
        opts.Add("MSAASamples_", AppSettings::NumMSAASamples(MSAAModes(msaaMode)));
        opts.Add("UseZGradients_", 0);
        msaaMaskCS[msaaMode][0] = CompileFromFile(L"MSAAMask.hlsl", "MSAAMaskCS", ShaderType::Compute, opts);

        opts.Reset();
        opts.Add("MSAASamples_", AppSettings::NumMSAASamples(MSAAModes(msaaMode)));
        opts.Add("UseZGradients_", 1);
        msaaMaskCS[msaaMode][1] = CompileFromFile(L"MSAAMask.hlsl", "MSAAMaskCS", ShaderType::Compute, opts);
    }

    // Compile resolve shaders
    for(uint64 msaaMode = 1; msaaMode < NumMSAAModes; ++msaaMode)
    {
        for(uint64 deferred = 0; deferred < 2; ++deferred)
        {
            CompileOptions opts;
            opts.Add("MSAASamples_", AppSettings::NumMSAASamples(MSAAModes(msaaMode)));
            opts.Add("Deferred_", uint32(deferred));
            resolvePS[msaaMode][deferred] = CompileFromFile(L"Resolve.hlsl", "ResolvePS", ShaderType::Pixel, opts);
        }
    }

    // Compile cluster visualization shaders
    clusterVisPS = CompileFromFile(L"ClusterVisualizer.hlsl", "ClusterVisualizerPS", ShaderType::Pixel);

    std::wstring fullScreenTriPath = SampleFrameworkDir() + L"Shaders\\FullScreenTriangle.hlsl";
    fullScreenTriVS = CompileFromFile(fullScreenTriPath.c_str(), "FullScreenTriangleVS", ShaderType::Vertex);

    ssaoCS = CompileFromFile(L"SSAO.hlsl", "ComputeSSAO", ShaderType::Compute);

    {
        // Clustering root signature
        D3D12_DESCRIPTOR_RANGE1 uavRanges[1] = {};
        uavRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRanges[0].NumDescriptors = 1;
        uavRanges[0].BaseShaderRegister = 0;
        uavRanges[0].RegisterSpace = 0;
        uavRanges[0].OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER1 rootParameters[NumClusterRootParams] = {};

        // Standard SRV descriptors
        rootParameters[ClusterParams_StandardDescriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[ClusterParams_StandardDescriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rootParameters[ClusterParams_StandardDescriptors].DescriptorTable.pDescriptorRanges = DX12::StandardDescriptorRanges();
        rootParameters[ClusterParams_StandardDescriptors].DescriptorTable.NumDescriptorRanges = DX12::NumStandardDescriptorRanges;

        // PS UAV descriptors
        rootParameters[ClusterParams_UAVDescriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[ClusterParams_UAVDescriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[ClusterParams_UAVDescriptors].DescriptorTable.pDescriptorRanges = uavRanges;
        rootParameters[ClusterParams_UAVDescriptors].DescriptorTable.NumDescriptorRanges = ArraySize_(uavRanges);

        // CBuffer
        rootParameters[ClusterParams_CBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[ClusterParams_CBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[ClusterParams_CBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[ClusterParams_CBuffer].Descriptor.ShaderRegister = 0;
        rootParameters[ClusterParams_CBuffer].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        // AppSettings
        rootParameters[ClusterParams_AppSettings].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[ClusterParams_AppSettings].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[ClusterParams_AppSettings].Descriptor.RegisterSpace = 0;
        rootParameters[ClusterParams_AppSettings].Descriptor.ShaderRegister = AppSettings::CBufferRegister;
        rootParameters[ClusterParams_AppSettings].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = 0;
        rootSignatureDesc.pStaticSamplers = nullptr;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        DX12::CreateRootSignature(&clusterRS, rootSignatureDesc);
    }

    {
        // Picking root signature
        D3D12_DESCRIPTOR_RANGE1 descriptorRanges[1] = {};
        descriptorRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        descriptorRanges[0].NumDescriptors = 1;
        descriptorRanges[0].BaseShaderRegister = 0;
        descriptorRanges[0].RegisterSpace = 0;
        descriptorRanges[0].OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER1 rootParameters[NumPickingRootParams] = {};

        rootParameters[PickingParams_StandardDescriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[PickingParams_StandardDescriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[PickingParams_StandardDescriptors].DescriptorTable.pDescriptorRanges = DX12::StandardDescriptorRanges();
        rootParameters[PickingParams_StandardDescriptors].DescriptorTable.NumDescriptorRanges = DX12::NumStandardDescriptorRanges;

        rootParameters[PickingParams_UAVDescriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[PickingParams_UAVDescriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[PickingParams_UAVDescriptors].DescriptorTable.pDescriptorRanges = descriptorRanges;
        rootParameters[PickingParams_UAVDescriptors].DescriptorTable.NumDescriptorRanges = ArraySize_(descriptorRanges);

        rootParameters[PickingParams_CBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[PickingParams_CBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[PickingParams_CBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[PickingParams_CBuffer].Descriptor.ShaderRegister = 0;
        rootParameters[PickingParams_CBuffer].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = 0;
        rootSignatureDesc.pStaticSamplers = nullptr;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        DX12::CreateRootSignature(&pickingRS, rootSignatureDesc);
    }

    {
        // MSAA mask root signature
        D3D12_DESCRIPTOR_RANGE1 descriptorRanges[1] = {};
        descriptorRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        descriptorRanges[0].NumDescriptors = 3;
        descriptorRanges[0].BaseShaderRegister = 0;
        descriptorRanges[0].RegisterSpace = 0;
        descriptorRanges[0].OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER1 rootParameters[NumMSAAMaskRootParams] = {};
        rootParameters[MSAAMaskParams_StandardDescriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[MSAAMaskParams_StandardDescriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[MSAAMaskParams_StandardDescriptors].DescriptorTable.pDescriptorRanges = DX12::StandardDescriptorRanges();
        rootParameters[MSAAMaskParams_StandardDescriptors].DescriptorTable.NumDescriptorRanges = DX12::NumStandardDescriptorRanges;

        rootParameters[MSAAMaskParams_UAVDescriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[MSAAMaskParams_UAVDescriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[MSAAMaskParams_UAVDescriptors].DescriptorTable.pDescriptorRanges = descriptorRanges;
        rootParameters[MSAAMaskParams_UAVDescriptors].DescriptorTable.NumDescriptorRanges = ArraySize_(descriptorRanges);

        rootParameters[MSAAMaskParams_CBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[MSAAMaskParams_CBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[MSAAMaskParams_CBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[MSAAMaskParams_CBuffer].Descriptor.ShaderRegister = 0;
        rootParameters[MSAAMaskParams_CBuffer].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        rootParameters[MSAAMaskParams_AppSettings].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[MSAAMaskParams_AppSettings].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[MSAAMaskParams_AppSettings].Descriptor.RegisterSpace = 0;
        rootParameters[MSAAMaskParams_AppSettings].Descriptor.ShaderRegister = AppSettings::CBufferRegister;
        rootParameters[MSAAMaskParams_AppSettings].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = 0;
        rootSignatureDesc.pStaticSamplers = nullptr;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        DX12::CreateRootSignature(&msaaMaskRootSignature, rootSignatureDesc);
    }

    {
        // Resolve root signature
        D3D12_ROOT_PARAMETER1 rootParameters[NumResolveRootParams] = {};

        // Standard SRV descriptors
        rootParameters[ResolveParams_StandardDescriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[ResolveParams_StandardDescriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[ResolveParams_StandardDescriptors].DescriptorTable.pDescriptorRanges = DX12::StandardDescriptorRanges();
        rootParameters[ResolveParams_StandardDescriptors].DescriptorTable.NumDescriptorRanges = DX12::NumStandardDescriptorRanges;

        // CBuffer
        rootParameters[ResolveParams_Constants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParameters[ResolveParams_Constants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[ResolveParams_Constants].Constants.Num32BitValues = 3;
        rootParameters[ResolveParams_Constants].Constants.RegisterSpace = 0;
        rootParameters[ResolveParams_Constants].Constants.ShaderRegister = 0;

        // AppSettings
        rootParameters[ResolveParams_AppSettings].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[ResolveParams_AppSettings].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[ResolveParams_AppSettings].Descriptor.RegisterSpace = 0;
        rootParameters[ResolveParams_AppSettings].Descriptor.ShaderRegister = AppSettings::CBufferRegister;
        rootParameters[ResolveParams_AppSettings].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = 0;
        rootSignatureDesc.pStaticSamplers = nullptr;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        DX12::CreateRootSignature(&resolveRootSignature, rootSignatureDesc);
    }

    {
        // Cluster visualization root signature
        D3D12_ROOT_PARAMETER1 rootParameters[NumClusterVisRootParams] = {};

        // Standard SRV descriptors
        rootParameters[ClusterVisParams_StandardDescriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[ClusterVisParams_StandardDescriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[ClusterVisParams_StandardDescriptors].DescriptorTable.pDescriptorRanges = DX12::StandardDescriptorRanges();
        rootParameters[ClusterVisParams_StandardDescriptors].DescriptorTable.NumDescriptorRanges = DX12::NumStandardDescriptorRanges;

        // CBuffer
        rootParameters[ClusterVisParams_CBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[ClusterVisParams_CBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[ClusterVisParams_CBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[ClusterVisParams_CBuffer].Descriptor.ShaderRegister = 0;
        rootParameters[ClusterVisParams_CBuffer].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        // AppSettings
        rootParameters[ClusterVisParams_AppSettings].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[ClusterVisParams_AppSettings].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[ClusterVisParams_AppSettings].Descriptor.RegisterSpace = 0;
        rootParameters[ClusterVisParams_AppSettings].Descriptor.ShaderRegister = AppSettings::CBufferRegister;
        rootParameters[ClusterVisParams_AppSettings].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = 0;
        rootSignatureDesc.pStaticSamplers = nullptr;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        DX12::CreateRootSignature(&clusterVisRootSignature, rootSignatureDesc);
    }

    {
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
        // SSAO root signature
        D3D12_DESCRIPTOR_RANGE1 descriptorRanges[1] = {};
        descriptorRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        descriptorRanges[0].NumDescriptors = 1;
        descriptorRanges[0].BaseShaderRegister = 0;
        descriptorRanges[0].RegisterSpace = 0;
        descriptorRanges[0].OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER1 rootParameters[NumMSAAMaskRootParams] = {};
        rootParameters[SSAOParams_StandardDescriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[SSAOParams_StandardDescriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[SSAOParams_StandardDescriptors].DescriptorTable.pDescriptorRanges = DX12::StandardDescriptorRanges();
        rootParameters[SSAOParams_StandardDescriptors].DescriptorTable.NumDescriptorRanges = DX12::NumStandardDescriptorRanges;

        rootParameters[SSAOParams_UAVDescriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[SSAOParams_UAVDescriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[SSAOParams_UAVDescriptors].DescriptorTable.pDescriptorRanges = descriptorRanges;
        rootParameters[SSAOParams_UAVDescriptors].DescriptorTable.NumDescriptorRanges = ArraySize_(descriptorRanges);

        rootParameters[SSAOParams_CBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[SSAOParams_CBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[SSAOParams_CBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[SSAOParams_CBuffer].Descriptor.ShaderRegister = 0;
        rootParameters[SSAOParams_CBuffer].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        rootParameters[SSAOParams_AppSettings].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[SSAOParams_AppSettings].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[SSAOParams_AppSettings].Descriptor.RegisterSpace = 0;
        rootParameters[SSAOParams_AppSettings].Descriptor.ShaderRegister = AppSettings::CBufferRegister;
        rootParameters[SSAOParams_AppSettings].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = 0;
        rootSignatureDesc.pStaticSamplers = nullptr;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        DX12::CreateRootSignature(&ssaoRootSignature, rootSignatureDesc);
    }
}

void BindlessDeferred::Shutdown()
{
    ShadowHelper::Shutdown();

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

    DX12::Release(msaaMaskRootSignature);
    nonMsaaTileBuffer.Shutdown();
    msaaTileBuffer.Shutdown();
    nonMsaaArgsBuffer.Shutdown();
    msaaArgsBuffer.Shutdown();
    msaaMaskBuffer.Shutdown();

    pickingBuffer.Shutdown();
    DX12::Release(pickingRS);
    for(uint64 i = 0; i < ArraySize_(pickingReadbackBuffers); ++i)
        pickingReadbackBuffers[i].Shutdown();

    DX12::Release(clusterVisRootSignature);

    mainTarget.Shutdown();
    tangentFrameTarget.Shutdown();
    resolveTarget.Shutdown();
    depthBuffer.Shutdown();
    uvTarget.Shutdown();
    uvGradientsTarget.Shutdown();
    materialIDTarget.Shutdown();
    deferredMSAATarget.Shutdown();
    ssaoTarget.Shutdown();

    DX12::Release(resolveRootSignature);

    DX12::Release(ssaoRootSignature);
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

        clusterFrontFacePSO->SetName(L"Cluster Front-Face PSO");
        clusterBackFacePSO->SetName(L"Cluster Back-Face PSO");
        clusterIntersectingPSO->SetName(L"Cluster Intersecting PSO");
    }

    {
        // SSAO PSO's
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = { };
        psoDesc.CS = ssaoCS.ByteCode();
        psoDesc.pRootSignature = ssaoRootSignature;
        DXCall(DX12::Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&ssaoPSO)));
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

    if(taskSet == nullptr || EnableMultithreadedCompilation == false)
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
    DX12::DeferredRelease(ssaoPSO);
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

    {
        RenderTextureInit rtInit;
        rtInit.Width = width;
        rtInit.Height = height;
        rtInit.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rtInit.MSAASamples = NumSamples;
        rtInit.ArraySize = 1;
        rtInit.CreateUAV = NumSamples == 1;
        rtInit.InitialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        rtInit.Name = L"Main Target";
        mainTarget.Initialize(rtInit);
    }

    {
        RenderTextureInit rtInit;
        rtInit.Width = width;
        rtInit.Height = height;
        rtInit.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        rtInit.MSAASamples = NumSamples;
        rtInit.ArraySize = 1;
        rtInit.CreateUAV = false;
        rtInit.InitialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        rtInit.Name = L"Tangent Frame Target";
        tangentFrameTarget.Initialize(rtInit);
    }

    {
        RenderTextureInit rtInit;
        rtInit.Width = width;
        rtInit.Height = height;
        rtInit.Format = DXGI_FORMAT_R16G16B16A16_SNORM;
        rtInit.MSAASamples = NumSamples;
        rtInit.ArraySize = 1;
        rtInit.CreateUAV = false;
        rtInit.InitialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        rtInit.Name = L"UV Target";
        uvTarget.Initialize(rtInit);
    }

    {
        RenderTextureInit rtInit;
        rtInit.Width = width;
        rtInit.Height = height;
        rtInit.Format = DXGI_FORMAT_R16G16B16A16_SNORM;
        rtInit.MSAASamples = NumSamples;
        rtInit.ArraySize = 1;
        rtInit.CreateUAV = false;
        rtInit.InitialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        rtInit.Name = L"UV Gradient Target";
        uvGradientsTarget.Initialize(rtInit);
    }

    {
        RenderTextureInit rtInit;
        rtInit.Width = width;
        rtInit.Height = height;
        rtInit.Format = DXGI_FORMAT_R8_UINT;
        rtInit.MSAASamples = NumSamples;
        rtInit.ArraySize = 1;
        rtInit.CreateUAV = false;
        rtInit.InitialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        rtInit.Name = L"Material ID Target";
        materialIDTarget.Initialize(rtInit);
    }

    {
        RenderTextureInit rtInit;
        rtInit.Width = width;
        rtInit.Height = height;
        rtInit.Format = DXGI_FORMAT_R16_UNORM;
        rtInit.MSAASamples = 1;
        rtInit.ArraySize = 1;
        rtInit.CreateUAV = true;
        rtInit.InitialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        rtInit.Name = L"SSAO Target";
        ssaoTarget.Initialize(rtInit);
    }

    if(NumSamples > 1)
    {
        {
            RenderTextureInit rtInit;
            rtInit.Width = width;
            rtInit.Height = height;
            rtInit.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            rtInit.MSAASamples = 1;
            rtInit.ArraySize = 1;
            rtInit.CreateUAV = false;
            rtInit.InitialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            rtInit.Name = L"Resolve Target";
            resolveTarget.Initialize(rtInit);
        }

        {
            RenderTextureInit rtInit;
            rtInit.Width = width * 2;
            rtInit.Height = NumSamples == 4 ? height * 2 : height;
            rtInit.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            rtInit.MSAASamples = 1;
            rtInit.ArraySize = 1;
            rtInit.CreateUAV = true;
            rtInit.InitialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            rtInit.Name = L"Deferred MSAA Target";
            deferredMSAATarget.Initialize(rtInit);
        }
    }

    {
        DepthBufferInit dbInit;
        dbInit.Width = width;
        dbInit.Height = height;
        dbInit.Format = DXGI_FORMAT_D32_FLOAT;
        dbInit.MSAASamples = NumSamples;
        dbInit.InitialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_DEPTH_READ;
        dbInit.Name = L"Main Depth Buffer";
        depthBuffer.Initialize(dbInit);
    }

    AppSettings::NumXTiles = (width + (AppSettings::ClusterTileSize - 1)) / AppSettings::ClusterTileSize;
    AppSettings::NumYTiles = (height + (AppSettings::ClusterTileSize - 1)) / AppSettings::ClusterTileSize;
    const uint64 numXYZTiles = AppSettings::NumXTiles * AppSettings::NumYTiles * AppSettings::NumZTiles;

    {
        // Render target for forcing MSAA during cluster rasterization. Ideally we would use ForcedSampleCount for this,
        // but it's currently causing the Nvidia driver to crash. :(
        RenderTextureInit rtInit;
        rtInit.Width = AppSettings::NumXTiles;
        rtInit.Height = AppSettings::NumYTiles;
        rtInit.Format = DXGI_FORMAT_R8_UNORM;
        rtInit.MSAASamples = 1;
        rtInit.ArraySize = 1;
        rtInit.CreateUAV = false;
        rtInit.Name = L"Deferred MSAA Target";

        ClusterRasterizationModes rastMode = AppSettings::ClusterRasterizationMode;
        if(rastMode == ClusterRasterizationModes::MSAA4x)
        {
            rtInit.MSAASamples = 4;
            clusterMSAATarget.Initialize(rtInit);
        }
        else if(rastMode == ClusterRasterizationModes::MSAA8x)
        {
            rtInit.MSAASamples = 8;
            clusterMSAATarget.Initialize(rtInit);
        }
        else
            clusterMSAATarget.Shutdown();
    }

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

void BindlessDeferred::CompileShadersTask(uint32 start, uint32 end, uint32 threadNum, void* args)
{
    BindlessDeferred* app = (BindlessDeferred*)args;

    for(uint32 i = start; i < end; ++i)
    {
        uint32 msaaMode = i / 4;
        uint32 computeUVGradients = (i / 2) % 2;
        uint32 perSample = i % 2;

        uint32 numMSAASamples = AppSettings::NumMSAASamples(MSAAModes(msaaMode));
        uint32 msaa = msaaMode > 0;
        if(msaa == 0 && perSample == 1)
            continue;

        CompileOptions opts;
        opts.Add("MSAA_", msaa);
        opts.Add("NumMSAASamples_", numMSAASamples);
        opts.Add("ShadePerSample_", perSample);
        opts.Add("ComputeUVGradients_", computeUVGradients);
        app->deferredCS[msaaMode][computeUVGradients][perSample] = CompileFromFile(L"Deferred.hlsl", "DeferredCS", ShaderType::Compute, opts);
    }
}

void BindlessDeferred::InitializeScene()
{
    currentModel = &sceneModels[uint64(AppSettings::CurrentScene)];
    meshRenderer.Shutdown();
    DX12::FlushGPU();
    meshRenderer.Initialize(currentModel);

    const uint64 numMaterialTextures = currentModel->MaterialTextures().Count();

    camera.SetPosition(SceneCameraPositions[uint64(AppSettings::CurrentScene)]);
    camera.SetXRotation(SceneCameraRotations[uint64(AppSettings::CurrentScene)].x);
    camera.SetYRotation(SceneCameraRotations[uint64(AppSettings::CurrentScene)].y);

    if(EnableMultithreadedCompilation)
    {
        if(taskSet != nullptr)
        {
            enkiWaitForTaskSet(taskScheduler, taskSet);
        }
        else
        {
            taskScheduler = enkiCreateTaskScheduler();
            taskSet = enkiCreateTaskSet(taskScheduler, CompileShadersTask);
        }

        // Kick off tasks to compile the deferred compute shaders
        enkiAddTaskSetToPipe(taskScheduler, taskSet, this, uint32(MSAAModes::NumValues) * 2 * 2);
    }
    else
    {
        CompileShadersTask(0, uint32(MSAAModes::NumValues) * 2 * 2, 0, this);
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
            spotLight.Intensity = srcLight.Intensity * SpotLightIntensityFactor;
            spotLight.AngularAttenuationX = std::cos(srcLight.AngularAttenuation.x * 0.5f);
            spotLight.AngularAttenuationY = std::cos(srcLight.AngularAttenuation.y * 0.5f);
            spotLight.Range = AppSettings::SpotLightRange;
        }

        AppSettings::MaxLightClamp.SetValue(int32(numSpotLights));
    }

    {
        DX12::DeferredRelease(deferredRootSignature);

        // Deferred root signature
        D3D12_DESCRIPTOR_RANGE1 descriptorRanges[1] = {};
        descriptorRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        descriptorRanges[0].NumDescriptors = 1;
        descriptorRanges[0].BaseShaderRegister = 0;
        descriptorRanges[0].RegisterSpace = 0;
        descriptorRanges[0].OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER1 rootParameters[NumDeferredRootParams] = {};

        rootParameters[DeferredParams_StandardDescriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[DeferredParams_StandardDescriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[DeferredParams_StandardDescriptors].DescriptorTable.pDescriptorRanges = DX12::StandardDescriptorRanges();
        rootParameters[DeferredParams_StandardDescriptors].DescriptorTable.NumDescriptorRanges = DX12::NumStandardDescriptorRanges;

        // PSCBuffer
        rootParameters[DeferredParams_PSCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[DeferredParams_PSCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[DeferredParams_PSCBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[DeferredParams_PSCBuffer].Descriptor.ShaderRegister = 0;
        rootParameters[DeferredParams_PSCBuffer].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        // ShadowCBuffer
        rootParameters[DeferredParams_ShadowCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[DeferredParams_ShadowCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[DeferredParams_ShadowCBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[DeferredParams_ShadowCBuffer].Descriptor.ShaderRegister = 1;
        rootParameters[DeferredParams_ShadowCBuffer].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        // DeferredCBuffer
        rootParameters[DeferredParams_DeferredCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[DeferredParams_DeferredCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[DeferredParams_DeferredCBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[DeferredParams_DeferredCBuffer].Descriptor.ShaderRegister = 2;
        rootParameters[DeferredParams_DeferredCBuffer].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        // LightCBuffer
        rootParameters[DeferredParams_LightCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[DeferredParams_LightCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[DeferredParams_LightCBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[DeferredParams_LightCBuffer].Descriptor.ShaderRegister = 3;
        rootParameters[DeferredParams_LightCBuffer].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;

        // SRV Indices
        rootParameters[DeferredParams_SRVIndices].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[DeferredParams_SRVIndices].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[DeferredParams_SRVIndices].Descriptor.RegisterSpace = 0;
        rootParameters[DeferredParams_SRVIndices].Descriptor.ShaderRegister = 4;
        rootParameters[DeferredParams_SRVIndices].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        // UAV's
        rootParameters[DeferredParams_UAVDescriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[DeferredParams_UAVDescriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[DeferredParams_UAVDescriptors].DescriptorTable.pDescriptorRanges = descriptorRanges;
        rootParameters[DeferredParams_UAVDescriptors].DescriptorTable.NumDescriptorRanges = ArraySize_(descriptorRanges);

        // AppSettings
        rootParameters[DeferredParams_AppSettings].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[DeferredParams_AppSettings].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[DeferredParams_AppSettings].Descriptor.RegisterSpace = 0;
        rootParameters[DeferredParams_AppSettings].Descriptor.ShaderRegister = AppSettings::CBufferRegister;

        D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = {};
        staticSamplers[0] = DX12::GetStaticSamplerState(SamplerState::Anisotropic, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
        staticSamplers[1] = DX12::GetStaticSamplerState(SamplerState::ShadowMapPCF, 1, 0, D3D12_SHADER_VISIBILITY_ALL);

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = ArraySize_(staticSamplers);
        rootSignatureDesc.pStaticSamplers = staticSamplers;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        DX12::CreateRootSignature(&deferredRootSignature, rootSignatureDesc);
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

    if(EnableMultithreadedCompilation && taskSet != nullptr && enkiIsTaskSetComplete(taskScheduler, taskSet))
    {
        enkiDeleteTaskSet(taskSet);
        enkiDeleteTaskScheduler(taskScheduler);
        taskSet = nullptr;
        taskScheduler = nullptr;

        DestroyPSOs();
        CreatePSOs();
    }
}

void BindlessDeferred::Render(const Timer& timer)
{
    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;

    CPUProfileBlock cpuProfileBlock("Render");
    ProfileBlock gpuProfileBlock(cmdList, "Render Total");

    if(taskSet != nullptr)
    {
        // We're still waiting for shaders to compile, so print a message to the screen and skip the render loop
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[1] = { swapChain.BackBuffer().RTV };
        cmdList->OMSetRenderTargets(1, rtvHandles, false, nullptr);

        const float clearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        cmdList->ClearRenderTargetView(rtvHandles[0], clearColor, 0, nullptr);

        DX12::SetViewport(cmdList, swapChain.Width(), swapChain.Height());

        Float2 viewportSize;
        viewportSize.x = float(swapChain.Width());
        viewportSize.y = float(swapChain.Height());
        spriteRenderer.Begin(cmdList, viewportSize, SpriteFilterMode::Point, SpriteBlendMode::AlphaBlend);

        wchar text[32] = L"Compiling Shaders...";
        uint32 numDots = uint32(Frac(timer.ElapsedSecondsF()) * 4.0f);
        text[17 + numDots] = 0;
        Float2 textSize = font.MeasureText(text);

        Float2 textPos = (viewportSize * 0.5f) - (textSize * 0.5f);
        spriteRenderer.RenderText(cmdList, font, text, textPos, Float4(1.0f, 1.0f, 1.0f, 1.0f));

        spriteRenderer.End();

        return;
    }

    RenderClusters();

    if(AppSettings::EnableSun)
        meshRenderer.RenderSunShadowMap(cmdList, camera);

    if(AppSettings::RenderLights)
        meshRenderer.RenderSpotLightShadowMap(cmdList, camera);

    {
        // Update the light constant buffer
        const void* srcData[2] = { spotLights.Data(), meshRenderer.SpotLightShadowMatrices() };
        uint64 sizes[2] = { spotLights.MemorySize(), spotLights.Size() * sizeof(Float4x4) };
        uint64 offsets[2] = { 0, sizeof(SpotLight) * AppSettings::MaxSpotLights };
        spotLightBuffer.MultiUpdateData(srcData, sizes, offsets, ArraySize_(srcData));
    }

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

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[1] = { swapChain.BackBuffer().RTV };
    cmdList->OMSetRenderTargets(1, rtvHandles, false, nullptr);

    RenderClusterVisualizer();

    DX12::SetViewport(cmdList, swapChain.Width(), swapChain.Height());

    RenderHUD(timer);
}

void BindlessDeferred::UpdateDecals(const Timer& timer)
{
    if(AppSettings::ClearDecals.Pressed())
        numDecals = 0;

    // Update picking and placing new decals
    cursorDecal = Decal();
    cursorDecal.AlbedoTexIdx = uint32(-1);
    cursorDecal.NormalTexIdx = uint32(-1);
    cursorDecalIntensity = 0.0f;
    if(currMouseState.IsOverWindow && AppSettings::EnableDecalPicker)
    {
        // Update the decal cursor
        const uint64 albedoTexIdx = currDecalType * AppSettings::NumTexturesPerDecal;
        const uint64 normalTexIdx = albedoTexIdx + 1;
        Assert_(albedoTexIdx < ArraySize_(decalTextures));

        Float2 textureSize;
        textureSize.x = float(decalTextures[albedoTexIdx].Width);
        textureSize.y = float(decalTextures[albedoTexIdx].Height);
        const float sizeScale = 1 / 1024.0f;

        const PickingData* pickingData = pickingReadbackBuffers[DX12::CurrFrameIdx].Map<PickingData>();
        if(pickingData->Normal != Float3(0.0f, 0.0f, 0.0f))
        {
            const float decalThickness = 0.125f;

            cursorDecal.Position = pickingData->Position;
            cursorDecal.Size = Float3(textureSize.x * sizeScale, textureSize.y * sizeScale, decalThickness);
            cursorDecal.AlbedoTexIdx = decalTextures[albedoTexIdx].SRV;
            cursorDecal.NormalTexIdx = decalTextures[normalTexIdx].SRV;
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

        if(AppSettings::AnimateLightIntensity)
        {
            float intensityFactor = std::cos(appTimer.ElapsedSecondsF() * Pi + spotLightIdx * 0.1f);
            intensityFactor = intensityFactor * 0.5f + 1.0f;
            spotLights[spotLightIdx].Intensity = srcSpotLight.Intensity * intensityFactor * SpotLightIntensityFactor;
        }
        else
            spotLights[spotLightIdx].Intensity = srcSpotLight.Intensity * SpotLightIntensityFactor;
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

    {
        // Clear decal clusters
        D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptors[1] = { decalClusterBuffer.UAV };
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = DX12::TempDescriptorTable(cpuDescriptors, ArraySize_(cpuDescriptors));

        uint32 values[4] = { };
        cmdList->ClearUnorderedAccessViewUint(gpuHandle, cpuDescriptors[0], decalClusterBuffer.InternalBuffer.Resource, values, 0, nullptr);
    }

    {
        // Clear spot light clusters
        D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptors[1] = { spotLightClusterBuffer.UAV };
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = DX12::TempDescriptorTable(cpuDescriptors, ArraySize_(cpuDescriptors));

        uint32 values[4] = { };
        cmdList->ClearUnorderedAccessViewUint(gpuHandle, cpuDescriptors[0], spotLightClusterBuffer.InternalBuffer.Resource, values, 0, nullptr);
    }

    ClusterConstants clusterConstants;
    clusterConstants.ViewProjection = camera.ViewProjectionMatrix();
    clusterConstants.InvProjection = Float4x4::Invert(camera.ProjectionMatrix());
    clusterConstants.NearClip = camera.NearClip();
    clusterConstants.FarClip = camera.FarClip();
    clusterConstants.InvClipRange = 1.0f / (camera.FarClip() - camera.NearClip());
    clusterConstants.NumXTiles = uint32(AppSettings::NumXTiles);
    clusterConstants.NumYTiles = uint32(AppSettings::NumYTiles);
    clusterConstants.NumXYTiles = uint32(AppSettings::NumXTiles * AppSettings::NumYTiles);
    clusterConstants.InstanceOffset = 0;
    clusterConstants.NumLights = Min<uint32>(uint32(spotLights.Size()), AppSettings::MaxLightClamp);
    clusterConstants.NumDecals = uint32(Min(numDecals, AppSettings::MaxDecals));

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[1] = { clusterMSAATarget.RTV };
    ClusterRasterizationModes rastMode = AppSettings::ClusterRasterizationMode;
    if(rastMode == ClusterRasterizationModes::MSAA4x || rastMode == ClusterRasterizationModes::MSAA8x)
        cmdList->OMSetRenderTargets(1, rtvHandles, false, nullptr);
    else
        cmdList->OMSetRenderTargets(0, nullptr, false, nullptr);

    DX12::SetViewport(cmdList, AppSettings::NumXTiles, AppSettings::NumYTiles);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    cmdList->SetGraphicsRootSignature(clusterRS);

    DX12::BindStandardDescriptorTable(cmdList, ClusterParams_StandardDescriptors, CmdListMode::Graphics);

    if(AppSettings::RenderDecals)
    {
        // Update decal clusters
        decalClusterBuffer.UAVBarrier(cmdList);

        D3D12_INDEX_BUFFER_VIEW ibView = decalClusterIdxBuffer.IBView();
        cmdList->IASetIndexBuffer(&ibView);

        clusterConstants.ElementsPerCluster = uint32(AppSettings::DecalElementsPerCluster);
        clusterConstants.InstanceOffset = 0;
        clusterConstants.BoundsBufferIdx = decalBoundsBuffer.SRV;
        clusterConstants.VertexBufferIdx = decalClusterVtxBuffer.SRV;
        clusterConstants.InstanceBufferIdx = decalInstanceBuffer.SRV;
        DX12::BindTempConstantBuffer(cmdList, clusterConstants, ClusterParams_CBuffer, CmdListMode::Graphics);

        AppSettings::BindCBufferGfx(cmdList, ClusterParams_AppSettings);

        D3D12_CPU_DESCRIPTOR_HANDLE uavs[] = { decalClusterBuffer.UAV };
        DX12::BindTempDescriptorTable(cmdList, uavs, ArraySize_(uavs), ClusterParams_UAVDescriptors, CmdListMode::Graphics);

        const uint64 numDecalsToRender = Min(numDecals, AppSettings::MaxDecals);
        Assert_(numIntersectingDecals <= numDecalsToRender);
        const uint64 numNonIntersecting = numDecalsToRender - numIntersectingDecals;

        // Render back faces for decals that intersect with the camera
        cmdList->SetPipelineState(clusterIntersectingPSO);

        cmdList->DrawIndexedInstanced(uint32(decalClusterIdxBuffer.NumElements), uint32(numIntersectingDecals), 0, 0, 0);

        // Now for all other decals, render the back faces followed by the front faces
        cmdList->SetPipelineState(clusterBackFacePSO);

        clusterConstants.InstanceOffset = uint32(numIntersectingDecals);
        DX12::BindTempConstantBuffer(cmdList, clusterConstants, ClusterParams_CBuffer, CmdListMode::Graphics);

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

        clusterConstants.ElementsPerCluster = uint32(AppSettings::SpotLightElementsPerCluster);
        clusterConstants.InstanceOffset = 0;
        clusterConstants.BoundsBufferIdx = spotLightBoundsBuffer.SRV;
        clusterConstants.VertexBufferIdx = spotLightClusterVtxBuffer.SRV;
        clusterConstants.InstanceBufferIdx = spotLightInstanceBuffer.SRV;
        DX12::BindTempConstantBuffer(cmdList, clusterConstants, ClusterParams_CBuffer, CmdListMode::Graphics);

        AppSettings::BindCBufferGfx(cmdList, ClusterParams_AppSettings);

        D3D12_CPU_DESCRIPTOR_HANDLE uavs[] = { spotLightClusterBuffer.UAV };
        DX12::BindTempDescriptorTable(cmdList, uavs, ArraySize_(uavs), ClusterParams_UAVDescriptors, CmdListMode::Graphics);

        const uint64 numLightsToRender = Min<uint64>(spotLights.Size(), AppSettings::MaxLightClamp);
        Assert_(numIntersectingSpotLights <= numLightsToRender);
        const uint64 numNonIntersecting = numLightsToRender - numIntersectingSpotLights;

        // Render back faces for decals that intersect with the camera
        cmdList->SetPipelineState(clusterIntersectingPSO);

        cmdList->DrawIndexedInstanced(uint32(spotLightClusterIdxBuffer.NumElements), uint32(numIntersectingSpotLights), 0, 0, 0);

        // Now for all other lights, render the back faces followed by the front faces
        cmdList->SetPipelineState(clusterBackFacePSO);

        clusterConstants.InstanceOffset = uint32(numIntersectingSpotLights);
        DX12::BindTempConstantBuffer(cmdList, clusterConstants, ClusterParams_CBuffer, CmdListMode::Graphics);

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
        // Transition render targets and depth buffers back to a writable state, and sync on the shadow maps
        D3D12_RESOURCE_BARRIER barriers[5] = {};
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

        barriers[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[3].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[3].Transition.pResource = meshRenderer.SunShadowMap().Resource();
        barriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[3].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        barriers[4].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[4].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[4].Transition.pResource = meshRenderer.SpotLightShadowMap().Resource();
        barriers[4].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barriers[4].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[4].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        cmdList->ResourceBarrier(ArraySize_(barriers), barriers);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[2] = { mainTarget.RTV, tangentFrameTarget.RTV };
    cmdList->OMSetRenderTargets(2, rtvHandles, false, &depthBuffer.DSV);

    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    cmdList->ClearRenderTargetView(rtvHandles[0], clearColor, 0, nullptr);
    cmdList->ClearRenderTargetView(rtvHandles[1], clearColor, 0, nullptr);
    cmdList->ClearDepthStencilView(depthBuffer.DSV, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

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

        cmdList->OMSetRenderTargets(1, rtvHandles, false, &depthBuffer.DSV);

        // Render the sky
        skybox.RenderSky(cmdList, camera.ViewMatrix(), camera.ProjectionMatrix(), skyCache, true);

        {
            // Make our targets readable again, which will force a sync point. Also transition
            // the shadow maps back to their writable state
            D3D12_RESOURCE_BARRIER barriers[5] = {};
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

            barriers[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[3].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[3].Transition.pResource = meshRenderer.SunShadowMap().Resource();
            barriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            barriers[3].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            barriers[4].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[4].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[4].Transition.pResource = meshRenderer.SpotLightShadowMap().Resource();
            barriers[4].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barriers[4].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            barriers[4].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            cmdList->ResourceBarrier(ArraySize_(barriers), barriers);
        }
    }
}

void BindlessDeferred::RenderDeferred()
{
    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;

    PIXMarker marker(cmdList, "Render Deferred");

    {
        // Transition our G-Buffer targets to a writable state, and sync on shadow map rendering
        D3D12_RESOURCE_BARRIER barriers[7] = {};
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
        barriers[4].Transition.pResource = meshRenderer.SunShadowMap().Resource();
        barriers[4].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barriers[4].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[4].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        barriers[5].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[5].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[5].Transition.pResource = meshRenderer.SpotLightShadowMap().Resource();
        barriers[5].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barriers[5].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[5].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        barriers[6].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[6].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[6].Transition.pResource = uvGradientsTarget.Resource();
        barriers[6].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[6].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[6].Transition.Subresource = 0;

        const uint32 numBarriers = AppSettings::ComputeUVGradients ? ArraySize_(barriers) - 1 : ArraySize_(barriers);
        cmdList->ResourceBarrier(numBarriers, barriers);
    }

    {
        // Set the G-Buffer render targets and clear them
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[] = { tangentFrameTarget.RTV, uvTarget.RTV,
                                                     materialIDTarget.RTV, uvGradientsTarget.RTV };
        const uint32 numTargets = AppSettings::ComputeUVGradients ? ArraySize_(rtvHandles) - 1 : ArraySize_(rtvHandles);
        cmdList->OMSetRenderTargets(numTargets, rtvHandles, false, &depthBuffer.DSV);

        const float clearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for(uint64 i = 0; i < numTargets; ++i)
            cmdList->ClearRenderTargetView(rtvHandles[i], clearColor, 0, nullptr);
        cmdList->ClearDepthStencilView(depthBuffer.DSV, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
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

    RenderSSAO();

    const uint32 numComputeTilesX = uint32(AlignTo(mainTarget.Width(), AppSettings::DeferredTileSize) / AppSettings::DeferredTileSize);
    const uint32 numComputeTilesY = uint32(AlignTo(mainTarget.Height(), AppSettings::DeferredTileSize) / AppSettings::DeferredTileSize);

    if(msaaEnabled)
    {
        // Generate dispatch lists for for per-sample shading
        PIXMarker maskMarker(cmdList, "MSAA Mask");
        ProfileBlock profileBlock(cmdList, "MSAA Mask");

        // Clear the structure counts
        {
            D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptors[1] = { nonMsaaTileBuffer.CounterUAV };
            D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = DX12::TempDescriptorTable(cpuDescriptors, ArraySize_(cpuDescriptors));

            uint32 values[4] = {};
            cmdList->ClearUnorderedAccessViewUint(gpuHandle, cpuDescriptors[0], nonMsaaTileBuffer.CounterResource, values, 0, nullptr);
        }

        {
            D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptors[1] = { msaaTileBuffer.CounterUAV };
            D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = DX12::TempDescriptorTable(cpuDescriptors, ArraySize_(cpuDescriptors));

            uint32 values[4] = {};
            cmdList->ClearUnorderedAccessViewUint(gpuHandle, cpuDescriptors[0], msaaTileBuffer.CounterResource, values, 0, nullptr);
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

        DX12::BindStandardDescriptorTable(cmdList, MSAAMaskParams_StandardDescriptors, CmdListMode::Compute);

        MSAAMaskConstants msaaMaskConstants;
        msaaMaskConstants.NumXTiles = numComputeTilesX;
        msaaMaskConstants.MaterialIDMapIdx = materialIDTarget.SRV();
        msaaMaskConstants.UVMapIdx = uvTarget.SRV();
        DX12::BindTempConstantBuffer(cmdList, msaaMaskConstants, MSAAMaskParams_CBuffer, CmdListMode::Compute);

        D3D12_CPU_DESCRIPTOR_HANDLE uavs[] = { nonMsaaTileBuffer.UAV, msaaTileBuffer.UAV, msaaMaskBuffer.UAV };
        DX12::BindTempDescriptorTable(cmdList, uavs, ArraySize_(uavs), MSAAMaskParams_UAVDescriptors, CmdListMode::Compute);

        AppSettings::BindCBufferCompute(cmdList, MSAAMaskParams_AppSettings);

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

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[1] = { mainTarget.RTV };
        cmdList->OMSetRenderTargets(1, rtvHandles, false, &depthBuffer.DSV);

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

        DX12::BindStandardDescriptorTable(cmdList, DeferredParams_StandardDescriptors, CmdListMode::Compute);

        // Set constant buffers
        DeferredConstants deferredConstants;
        deferredConstants.InvViewProj = Float4x4::Invert(camera.ViewProjectionMatrix());
        deferredConstants.Projection = camera.ProjectionMatrix();
        deferredConstants.RTSize = Float2(float(mainTarget.Width()), float(mainTarget.Height()));
        deferredConstants.NumComputeTilesX = numComputeTilesX;
        DX12::BindTempConstantBuffer(cmdList, deferredConstants, DeferredParams_DeferredCBuffer, CmdListMode::Compute);

        ShadingConstants shadingConstants;
        shadingConstants.SunDirectionWS = AppSettings::SunDirection;
        shadingConstants.SunIrradiance = skyCache.SunIrradiance;
        shadingConstants.CosSunAngularRadius = std::cos(DegToRad(AppSettings::SunSize));
        shadingConstants.SinSunAngularRadius = std::sin(DegToRad(AppSettings::SunSize));
        shadingConstants.CameraPosWS = camera.Position();

        shadingConstants.CursorDecalPos = cursorDecal.Position;
        shadingConstants.CursorDecalIntensity = cursorDecalIntensity;
        shadingConstants.CursorDecalOrientation = cursorDecal.Orientation;
        shadingConstants.CursorDecalSize = cursorDecal.Size;
        shadingConstants.CursorDecalTexIdx = cursorDecal.AlbedoTexIdx;
        shadingConstants.NumXTiles = uint32(AppSettings::NumXTiles);
        shadingConstants.NumXYTiles = uint32(AppSettings::NumXTiles * AppSettings::NumYTiles);
        shadingConstants.NearClip = camera.NearClip();
        shadingConstants.FarClip = camera.FarClip();
        shadingConstants.SkySH = skyCache.SH;

        DX12::BindTempConstantBuffer(cmdList, shadingConstants, DeferredParams_PSCBuffer, CmdListMode::Compute);

        const SunShadowConstantsDepthMap& sunShadowConstants = meshRenderer.SunShadowConstantData();
        DX12::BindTempConstantBuffer(cmdList, sunShadowConstants, DeferredParams_ShadowCBuffer, CmdListMode::Compute);

        spotLightBuffer.SetAsComputeRootParameter(cmdList, DeferredParams_LightCBuffer);

        AppSettings::BindCBufferCompute(cmdList, DeferredParams_AppSettings);

        uint32 skyTargetSRV = DX12::NullTexture2DSRV;
        if(msaaEnabled)
            skyTargetSRV = mainTarget.SRV();

        uint32 srvIndices[] =
        {
            meshRenderer.SunShadowMap().SRV(),
            meshRenderer.SpotLightShadowMap().SRV(),
            meshRenderer.MaterialTextureIndicesBuffer().SRV,
            decalBuffer.SRV,
            decalClusterBuffer.SRV,
            spotLightClusterBuffer.SRV,
            nonMsaaTileBuffer.SRV,
            msaaTileBuffer.SRV,
            tangentFrameTarget.SRV(),
            uvTarget.SRV(),
            uvGradientsTarget.SRV(),
            materialIDTarget.SRV(),
            depthBuffer.SRV(),
            skyTargetSRV,
            msaaMaskBuffer.SRV,
            ssaoTarget.SRV(),
        };

        DX12::BindTempConstantBuffer(cmdList, srvIndices, DeferredParams_SRVIndices, CmdListMode::Compute);

        D3D12_CPU_DESCRIPTOR_HANDLE uavs[] = { deferredTarget.UAV };
        DX12::BindTempDescriptorTable(cmdList, uavs, ArraySize_(uavs), DeferredParams_UAVDescriptors, CmdListMode::Compute);

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
            barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[1].Transition.pResource = mainTarget.Resource();
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            cmdList->ResourceBarrier(ArraySize_(barriers), barriers);
        }
        else
        {
            // Render the sky in the empty areas
            mainTarget.Transition(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RENDER_TARGET);

            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[1] = { mainTarget.RTV };
            cmdList->OMSetRenderTargets(1, rtvHandles, false, &depthBuffer.DSV);

            skybox.RenderSky(cmdList, camera.ViewMatrix(), camera.ProjectionMatrix(), skyCache, true);

            mainTarget.Transition(cmdList, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }

    {
        // Transition the shadow maps back into their writable state
        D3D12_RESOURCE_BARRIER barriers[2] = {};

        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[0].Transition.pResource = meshRenderer.SunShadowMap().Resource();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.StateAfter =  D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[1].Transition.pResource = meshRenderer.SpotLightShadowMap().Resource();
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.StateAfter =  D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        cmdList->ResourceBarrier(ArraySize_(barriers), barriers);
    }
}

void BindlessDeferred::RenderSSAO()
{
    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;

    PIXMarker marker(cmdList, "Render SSAO");

    D3D12_RESOURCE_BARRIER barriers[7] = {};
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[1].Transition.pResource = ssaoTarget.Resource();
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.Subresource = 0;

    ssaoTarget.Transition(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cmdList->SetComputeRootSignature(ssaoRootSignature);
    cmdList->SetPipelineState(ssaoPSO);

    DX12::BindStandardDescriptorTable(cmdList, SSAOParams_StandardDescriptors, CmdListMode::Compute);

    /*MSAAMaskConstants msaaMaskConstants;
    msaaMaskConstants.NumXTiles = numComputeTilesX;
    msaaMaskConstants.MaterialIDMapIdx = materialIDTarget.SRV();
    msaaMaskConstants.UVMapIdx = uvTarget.SRV();
    DX12::BindTempConstantBuffer(cmdList, msaaMaskConstants, SSAOParams_CBuffer, CmdListMode::Compute);*/

    D3D12_CPU_DESCRIPTOR_HANDLE uavs[] = { ssaoTarget.UAV };
    DX12::BindTempDescriptorTable(cmdList, uavs, ArraySize_(uavs), SSAOParams_UAVDescriptors, CmdListMode::Compute);

    AppSettings::BindCBufferCompute(cmdList, SSAOParams_AppSettings);

    cmdList->Dispatch(DX12::DispatchSize(ssaoTarget.Width(), 8), DX12::DispatchSize(ssaoTarget.Height(), 8), 1);

    ssaoTarget.Transition(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[1] = { resolveTarget.RTV };
    cmdList->OMSetRenderTargets(ArraySize_(rtvs), rtvs, false, nullptr);
    DX12::SetViewport(cmdList, resolveTarget.Width(), resolveTarget.Height());

    const uint64 deferred = AppSettings::RenderMode == RenderModes::DeferredTexturing ? 1 : 0;
    ID3D12PipelineState* pso = resolvePSOs[deferred];

    cmdList->SetGraphicsRootSignature(resolveRootSignature);
    cmdList->SetPipelineState(pso);

    DX12::BindStandardDescriptorTable(cmdList, ResolveParams_StandardDescriptors, CmdListMode::Graphics);

    cmdList->SetGraphicsRoot32BitConstant(ResolveParams_Constants, uint32(mainTarget.Width()), 0);
    cmdList->SetGraphicsRoot32BitConstant(ResolveParams_Constants, uint32(mainTarget.Height()), 1);
    cmdList->SetGraphicsRoot32BitConstant(ResolveParams_Constants, deferred ? deferredMSAATarget.SRV() : mainTarget.SRV(), 2);

    AppSettings::BindCBufferGfx(cmdList, ResolveParams_AppSettings);

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

    DX12::BindStandardDescriptorTable(cmdList, PickingParams_StandardDescriptors, CmdListMode::Compute);

    PickingConstants pickingConstants;
    pickingConstants.InverseViewProjection = Float4x4::Invert(camera.ViewProjectionMatrix());
    pickingConstants.PixelPos = Uint2(currMouseState.X, currMouseState.Y);
    pickingConstants.RTSize.x = float(mainTarget.Width());
    pickingConstants.RTSize.y = float(mainTarget.Height());
    pickingConstants.TangentMapIdx = tangentFrameTarget.SRV();
    pickingConstants.DepthMapIdx = depthBuffer.SRV();
    DX12::BindTempConstantBuffer(cmdList, pickingConstants, PickingParams_CBuffer, CmdListMode::Compute);

    pickingBuffer.MakeWritable(cmdList);

    D3D12_CPU_DESCRIPTOR_HANDLE uavs[] = { pickingBuffer.UAV };
    DX12::BindTempDescriptorTable(cmdList, uavs, ArraySize_(uavs), PickingParams_UAVDescriptors, CmdListMode::Compute);

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

    DX12::BindStandardDescriptorTable(cmdList, ClusterVisParams_StandardDescriptors, CmdListMode::Graphics);

    Float4x4 invProjection = Float4x4::Invert(camera.ProjectionMatrix());
    Float3 farTopRight = Float3::Transform(Float3(1.0f, 1.0f, 1.0f), invProjection);
    Float3 farBottomLeft = Float3::Transform(Float3(-1.0f, -1.0f, 1.0f), invProjection);

    ClusterVisConstants clusterVisConstants;
    clusterVisConstants.Projection = camera.ProjectionMatrix();
    clusterVisConstants.ViewMin = Float3(farBottomLeft.x, farBottomLeft.y, camera.NearClip());
    clusterVisConstants.NearClip = camera.NearClip();
    clusterVisConstants.ViewMax = Float3(farTopRight.x, farTopRight.y, camera.FarClip());
    clusterVisConstants.InvClipRange = 1.0f / (camera.FarClip() - camera.NearClip());
    clusterVisConstants.DisplaySize = displaySize;
    clusterVisConstants.NumXTiles = uint32(AppSettings::NumXTiles);
    clusterVisConstants.NumXYTiles = uint32(AppSettings::NumXTiles * AppSettings::NumYTiles);
    clusterVisConstants.DecalClusterBufferIdx = decalClusterBuffer.SRV;
    clusterVisConstants.SpotLightClusterBufferIdx = spotLightClusterBuffer.SRV;
    DX12::BindTempConstantBuffer(cmdList, clusterVisConstants, ClusterVisParams_CBuffer, CmdListMode::Graphics);

    AppSettings::BindCBufferGfx(cmdList, ClusterVisParams_AppSettings);

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
    BindlessDeferred app(lpCmdLine);
    app.Run();
}
