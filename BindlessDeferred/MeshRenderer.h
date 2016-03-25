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

#include <Graphics/Model.h>
#include <Graphics/GraphicsTypes.h>
#include <Graphics/Camera.h>
#include <Graphics/ShaderCompilation.h>
#include <Graphics/ShadowHelper.h>
#include <Graphics/SH.h>

#include "AppSettings.h"
#include "SharedTypes.h"

using namespace SampleFramework12;

namespace SampleFramework12
{
    struct SkyCache;
}

struct MainPassData
{
    const SkyCache* SkyCache = nullptr;
    const Texture* DecalTextures = nullptr;
    const StructuredBuffer* DecalBuffer = nullptr;
    Decal CursorDecal;
    float CursorDecalIntensity = 0.0f;
    const RawBuffer* DecalClusterBuffer = nullptr;
    const StructuredBuffer* SpotLightBuffer = nullptr;
    const RawBuffer* SpotLightClusterBuffer = nullptr;
};

struct ShadingConstants
{
    Float4Align Float3 SunDirectionWS;
    float CosSunAngularRadius;
    Float4Align Float3 SunIrradiance;
    float SinSunAngularRadius;
    Float4Align Float3 CameraPosWS;

    Float4Align Float3 CursorDecalPos;
    float CursorDecalIntensity;
    Float4Align Quaternion CursorDecalOrientation;
    Float4Align Float3 CursorDecalSize;
    uint32 CursorDecalType;
    uint32 NumXTiles;
    uint32 NumXYTiles;
    float NearClip;
    float FarClip;

    Float4Align ShaderSH9Color SkySH;
};

class MeshRenderer
{


public:

    MeshRenderer();

    void Initialize(const Model* sceneModel);
    void Shutdown();

    void CreatePSOs(DXGI_FORMAT mainRTFormat, DXGI_FORMAT depthFormat, const DXGI_FORMAT* gBufferFormats,
                    uint64 numGBuffers, uint32 numMSAASamples);
    void DestroyPSOs();

    void RenderMainPass(ID3D12GraphicsCommandList* cmdList, const Camera& camera, const MainPassData& mainPassData);
    void RenderGBuffer(ID3D12GraphicsCommandList* cmdList, const Camera& camera);

    void RenderDepthPrepass(ID3D12GraphicsCommandList* cmdList, const Camera& camera);
    void RenderSunShadowDepth(ID3D12GraphicsCommandList* cmdList, const OrthographicCamera& camera);
    void RenderSpotLightShadowDepth(ID3D12GraphicsCommandList* cmdList, const Camera& camera);

    void RenderSunShadowMap(ID3D12GraphicsCommandList* cmdList, const Camera& camera);
    void RenderSpotLightShadowMap(ID3D12GraphicsCommandList* cmdList, const Camera& camera);

    const DepthBuffer& SunShadowMap() const { return sunShadowMap; }
    const DepthBuffer& SpotLightShadowMap() const { return spotLightShadowMap; }
    const StructuredBuffer& SpotLightShadowMatrices() const { return spotLightShadowMatrices; }
    const StructuredBuffer& MaterialTextureIndicesBuffer() const { return materialTextureIndices; }
    ConstantBuffer<SunShadowConstants>& SunShadowConstantBuffer() { return sunShadowConstants; }

protected:

    void LoadShaders();
    void RenderDepth(ID3D12GraphicsCommandList* cmdList, const Camera& camera, ID3D12PipelineState* pso, uint64 numVisible);

    const Model* model = nullptr;

    DepthBuffer sunShadowMap;
    DepthBuffer spotLightShadowMap;
    StructuredBuffer spotLightShadowMatrices;

    StructuredBuffer materialTextureIndices;

    CompiledShaderPtr meshVS;
    CompiledShaderPtr meshPSForward;
    CompiledShaderPtr meshPSGBuffer[2];
    ID3D12PipelineState* mainPassPSO = nullptr;
    ID3D12RootSignature* mainPassRootSignature = nullptr;
    ID3D12PipelineState* gBufferPSO = nullptr;
    ID3D12RootSignature* gBufferRootSignature = nullptr;

    CompiledShaderPtr meshDepthVS;
    ID3D12PipelineState* depthPSO = nullptr;
    ID3D12PipelineState* sunShadowPSO = nullptr;
    ID3D12PipelineState* spotLightShadowPSO = nullptr;
    ID3D12RootSignature* depthRootSignature = nullptr;

    Array<DirectX::BoundingBox> meshBoundingBoxes;
    Array<uint32> meshDrawIndices;
    Array<float> meshZDepths;

    // Constant buffers
    struct MeshVSConstants
    {
        Float4Align Float4x4 World;
        Float4Align Float4x4 View;
        Float4Align Float4x4 WorldViewProjection;
        float NearClip;
        float FarClip;
    };

    ConstantBuffer<MeshVSConstants> meshVSConstants;
    ConstantBuffer<ShadingConstants> meshPSConstants;
    ConstantBuffer<SunShadowConstants> sunShadowConstants;

};