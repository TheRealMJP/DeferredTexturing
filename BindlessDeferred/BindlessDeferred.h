//=================================================================================================
//
//  Bindless Deferred Texturing Sample
//  by MJP
//  http://mynameismjp.wordpress.com/
//
//  All code and content licensed under the MIT license
//
//=================================================================================================

#pragma once

#include <PCH.h>

#include <App.h>
#include <InterfacePointers.h>
#include <Input.h>
#include <Graphics/Camera.h>
#include <Graphics/Model.h>
#include <Graphics/Skybox.h>
#include <Graphics/GraphicsTypes.h>

#include "PostProcessor.h"
#include "MeshRenderer.h"

using namespace SampleFramework12;

class BindlessDeferred : public App
{

protected:

    FirstPersonCamera camera;

    Skybox skybox;
    SkyCache skyCache;

    PostProcessor postProcessor;

    // Model
    Model sceneModels[uint64(Scenes::NumValues)];
    const Model* currentModel = nullptr;
    MeshRenderer meshRenderer;

    RenderTexture mainTarget;
    RenderTexture tangentFrameTarget;
    RenderTexture uvTarget;
    RenderTexture uvGradientsTarget;
    RenderTexture materialIDTarget;
    RenderTexture resolveTarget;
    RenderTexture deferredMSAATarget;
    DepthBuffer depthBuffer;

    Texture decalTextures[AppSettings::NumDecalTextures];
    StructuredBuffer decalBuffer;
    StructuredBuffer decalBoundsBuffer;
    StructuredBuffer decalInstanceBuffer;
    RawBuffer decalClusterBuffer;
    Array<Decal> decals;
    uint64 numDecals = 0;
    uint64 numIntersectingDecals = 0;

    Array<SpotLight> spotLights;
    StructuredBuffer spotLightBuffer;
    StructuredBuffer spotLightBoundsBuffer;
    StructuredBuffer spotLightInstanceBuffer;
    RawBuffer spotLightClusterBuffer;
    uint64 numIntersectingSpotLights = 0;

    ID3D12RootSignature* clusterRS = nullptr;
    CompiledShaderPtr clusterVS;
    CompiledShaderPtr clusterFrontFacePS;
    CompiledShaderPtr clusterBackFacePS;
    CompiledShaderPtr clusterIntersectingPS;
    ID3D12PipelineState* clusterFrontFacePSO = nullptr;
    ID3D12PipelineState* clusterBackFacePSO = nullptr;
    ID3D12PipelineState* clusterIntersectingPSO = nullptr;
    RenderTexture clusterMSAATarget;

    StructuredBuffer decalClusterVtxBuffer;
    FormattedBuffer decalClusterIdxBuffer;

    StructuredBuffer spotLightClusterVtxBuffer;
    FormattedBuffer spotLightClusterIdxBuffer;
    Array<Float3> coneVertices;

    StructuredBuffer pickingBuffer;
    ReadbackBuffer pickingReadbackBuffers[DX12::RenderLatency];
    ID3D12RootSignature* pickingRS = nullptr;
    ID3D12PipelineState* pickingPSOs[2] = { };
    CompiledShaderPtr pickingCS[2];
    MouseState currMouseState;
    Decal cursorDecal;
    float cursorDecalIntensity = 0.0f;
    uint64 currDecalType = 0;

    CompiledShaderPtr deferredCS[NumMSAAModes][2][2];
    ID3D12RootSignature* deferredRootSignature = nullptr;
    ID3D12PipelineState* deferredPSOs[2] = { };
    ID3D12CommandSignature* deferredCmdSignature = nullptr;

    CompiledShaderPtr msaaMaskCS[NumMSAAModes][2];
    ID3D12RootSignature* msaaMaskRootSignature = nullptr;
    ID3D12PipelineState* msaaMaskPSOs[2] = { };
    StructuredBuffer nonMsaaTileBuffer;
    StructuredBuffer msaaTileBuffer;
    StructuredBuffer nonMsaaArgsBuffer;
    StructuredBuffer msaaArgsBuffer;
    StructuredBuffer msaaMaskBuffer;

    CompiledShaderPtr fullScreenTriVS;
    CompiledShaderPtr resolvePS[NumMSAAModes][2];
    ID3D12RootSignature* resolveRootSignature = nullptr;
    ID3D12PipelineState* resolvePSOs[2] = { };

    CompiledShaderPtr clusterVisPS;
    ID3D12RootSignature* clusterVisRootSignature = nullptr;
    ID3D12PipelineState* clusterVisPSO = nullptr;

    struct ClusterConstants
    {
        Float4x4 ViewProjection;
        Float4x4 InvProjection;
        float NearClip;
        float FarClip;
        float InvClipRange;
        uint32 NumXTiles;
        uint32 NumYTiles;
        uint32 NumXYTiles;
        uint32 ElementsPerCluster;
        uint32 InstanceOffset;
        uint32 NumLights;
        uint32 NumDecals;
    };

    struct MSAAMaskConstants
    {
        uint32 NumXTiles;
    };

    struct DeferredConstants
    {
        Float4x4 InvViewProj;
        Float4x4 Projection;
        Float2 RTSize;
        uint32 NumComputeTilesX;
    };

    struct PickingConstants
    {
        Float4x4 InverseViewProjection;
        Uint2 PixelPos;
        Float2 RTSize;
    };

    struct ClusterVisConstants
    {
        Float4x4 Projection;
        Float3 ViewMin;
        float NearClip;
        Float3 ViewMax;
        float InvClipRange;
        Float2 DisplaySize;
        uint32 NumXTiles;
        uint32 NumXYTiles;
    };

    struct LightConstants
    {
        SpotLight Lights[AppSettings::MaxSpotLights];
        Float4x4 ShadowMatrices[AppSettings::MaxSpotLights];
    };

    ConstantBuffer<ClusterConstants> clusterConstants;
    ConstantBuffer<MSAAMaskConstants> msaaMaskConstants;
    ConstantBuffer<DeferredConstants> deferredConstants;
    ConstantBuffer<ShadingConstants> shadingConstants;
    ConstantBuffer<PickingConstants> pickingConstants;
    ConstantBuffer<ClusterVisConstants> clusterVisConstants;

    virtual void Initialize() override;
    virtual void Shutdown() override;

    virtual void Render(const Timer& timer) override;
    virtual void Update(const Timer& timer) override;

    virtual void BeforeReset() override;
    virtual void AfterReset() override;

    virtual void CreatePSOs() override;
    virtual void DestroyPSOs() override;

    void CreateRenderTargets();
    void InitializeScene();

    void UpdateDecals(const Timer& timer);
    void UpdateLights();

    void RenderClusters();
    void RenderForward();
    void RenderDeferred();
    void RenderResolve();
    void RenderPicking();
    void RenderClusterVisualizer();
    void RenderHUD(const Timer& timer);

public:

    BindlessDeferred();
};
