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

#include "MeshRenderer.h"

#include <Exceptions.h>
#include <Utility.h>
#include <Graphics/ShaderCompilation.h>
#include <Graphics/Skybox.h>
#include <Graphics/Profiler.h>

#include "AppSettings.h"

// Constants
static const uint64 SunShadowMapSize = 2048;
static const uint64 SpotLightShadowMapSize = 1024;

enum MainPassRootParams
{
    MainPass_MatIndexCBuffer,
    MainPass_PSCBuffer,
    MainPass_ShadowCBuffer,
    MainPass_LightCBuffer,
    MainPass_AppSettings,
    MainPass_SRV,
    MainPass_Decals,
    MainPass_VSCBuffer,

    NumMainPassRootParams,
};

// Used to sort by depth
struct ZSortComparer
{
    const Array<float>& MeshDepths;
    ZSortComparer(const Array<float>& meshDepths) : MeshDepths(meshDepths) { }

    bool operator()(uint32 a, uint32 b)
    {
        return MeshDepths[a] < MeshDepths[b];
    }
};

// Frustum culls meshes, and produces a buffer of visible mesh indices
static uint64 CullMeshes(const Camera& camera, const Array<DirectX::BoundingBox>& boundingBoxes, Array<uint32>& drawIndices)
{
    DirectX::BoundingFrustum frustum(camera.ProjectionMatrix().ToSIMD());
    frustum.Transform(frustum, 1.0f, camera.Orientation().ToSIMD(), camera.Position().ToSIMD());

    uint64 numVisible = 0;
    const uint64 numMeshes = boundingBoxes.Size();
    for(uint64 i = 0; i < numMeshes; ++i)
    {
        if(frustum.Intersects(boundingBoxes[i]))
            drawIndices[numVisible++] = uint32(i);
    }

    return numVisible;
}

// Frustum culls meshes, and produces a buffer of visible mesh indices. Also sorts the indices by depth.
static uint64 CullMeshesAndSort(const Camera& camera, const Array<DirectX::BoundingBox>& boundingBoxes,
                                Array<float>& meshDepths, Array<uint32>& drawIndices)
{
    DirectX::BoundingFrustum frustum(camera.ProjectionMatrix().ToSIMD());
    frustum.Transform(frustum, 1.0f, camera.Orientation().ToSIMD(), camera.Position().ToSIMD());

    Float4x4 viewMatrix = camera.ViewMatrix();

    uint64 numVisible = 0;
    const uint64 numMeshes = boundingBoxes.Size();
    for(uint64 i = 0; i < numMeshes; ++i)
    {
        if(frustum.Intersects(boundingBoxes[i]))
        {
            meshDepths[i] = Float3::Transform(Float3(boundingBoxes[i].Center), viewMatrix).z;
            drawIndices[numVisible++] = uint32(i);
        }
    }

    if(numVisible > 0)
    {
        ZSortComparer comparer(meshDepths);
        std::sort(drawIndices.Data(), drawIndices.Data() + numVisible, comparer);
    }

    return numVisible;
}

// Frustum culls meshes for an orthographic projection, and produces a buffer of visible mesh indices
static uint64 CullMeshesOrthographic(const OrthographicCamera& camera, bool ignoreNearZ, const Array<DirectX::BoundingBox>& boundingBoxes, Array<uint32>& drawIndices)
{
    Float3 mins = Float3(camera.MinX(), camera.MinY(), camera.NearClip());
    Float3 maxes = Float3(camera.MaxX(), camera.MaxY(), camera.FarClip());
    if(ignoreNearZ)
        mins.z = -10000.0f;

    Float3 extents = (maxes - mins) / 2.0f;
    Float3 center = mins + extents;
    center = Float3::Transform(center, camera.Orientation());
    center += camera.Position();

    DirectX::BoundingOrientedBox obb;
    obb.Extents = extents.ToXMFLOAT3();
    obb.Center = center.ToXMFLOAT3();
    obb.Orientation = camera.Orientation().ToXMFLOAT4();

    uint64 numVisible = 0;
    const uint64 numMeshes = boundingBoxes.Size();
    for(uint64 i = 0; i < numMeshes; ++i)
    {
        if(obb.Intersects(boundingBoxes[i]))
            drawIndices[numVisible++] = uint32(i);
    }

    return numVisible;
}

MeshRenderer::MeshRenderer()
{
}

void MeshRenderer::LoadShaders()
{
    // Load the mesh shaders
    meshDepthVS = CompileFromFile(L"DepthOnly.hlsl", "VS", ShaderType::Vertex, ShaderProfile::SM51);

    CompileOptions opts;
    opts.Add("NumMaterialTextures_", uint32(model->MaterialTextures().Count()));
    opts.Add("OutputUVGradients_", 1);
    meshVS = CompileFromFile(L"Mesh.hlsl", "VS", ShaderType::Vertex, ShaderProfile::SM51, opts);
    meshPSForward = CompileFromFile(L"Mesh.hlsl", "PSForward", ShaderType::Pixel, ShaderProfile::SM51, opts);
    meshPSGBuffer[0] = CompileFromFile(L"Mesh.hlsl", "PSGBuffer", ShaderType::Pixel, ShaderProfile::SM51, opts);

    opts.Reset();
    opts.Add("NumMaterialTextures_", uint32(model->MaterialTextures().Count()));
    opts.Add("OutputUVGradients_", 0);
    meshPSGBuffer[1] = CompileFromFile(L"Mesh.hlsl", "PSGBuffer", ShaderType::Pixel, ShaderProfile::SM51, opts);
}

// Loads resources
void MeshRenderer::Initialize(const Model* model_)
{
    model = model_;

    const uint64 numMeshes = model->Meshes().Size();
    meshBoundingBoxes.Init(numMeshes);
    meshDrawIndices.Init(numMeshes, uint32(-1));
    meshZDepths.Init(numMeshes, FloatMax);
    for(uint64 i = 0; i < numMeshes; ++i)
    {
        const Mesh& mesh = model->Meshes()[i];
        DirectX::BoundingBox& boundingBox = meshBoundingBoxes[i];
        Float3 extents = (mesh.AABBMax() - mesh.AABBMin()) / 2.0f;
        Float3 center = mesh.AABBMin() + extents;
        boundingBox.Center = center.ToXMFLOAT3();
        boundingBox.Extents = extents.ToXMFLOAT3();
    }

    meshVSConstants.Initialize(BufferLifetime::Temporary);
    meshPSConstants.Initialize(BufferLifetime::Temporary);
    sunShadowConstants.Initialize(BufferLifetime::Temporary);

    LoadShaders();

    sunShadowMap.Initialize(SunShadowMapSize, SunShadowMapSize, DXGI_FORMAT_D32_FLOAT, 1, NumCascades);
    sunShadowMap.MakeReadable(DX12::CmdList);

    const uint64 numSpotLights = model->SpotLights().Size();
    spotLightShadowMap.Initialize(SpotLightShadowMapSize, SpotLightShadowMapSize, DXGI_FORMAT_D24_UNORM_S8_UINT, 1, numSpotLights);
    spotLightShadowMap.MakeReadable(DX12::CmdList);

    {
        // Create a structured buffer containing texture indices per-material
        const Array<MeshMaterial>& materials = model->Materials();
        const uint64 numMaterials = materials.Size();
        Array<MaterialTextureIndices> textureIndices(numMaterials);
        for(uint64 i = 0; i < numMaterials; ++i)
        {
            MaterialTextureIndices& matIndices = textureIndices[i];
            const MeshMaterial& material = materials[i];

            matIndices.Albedo = material.TextureIndices[uint64(MaterialTextures::Albedo)];
            matIndices.Normal = material.TextureIndices[uint64(MaterialTextures::Normal)];
            matIndices.Roughness = material.TextureIndices[uint64(MaterialTextures::Roughness)];
            matIndices.Metallic = material.TextureIndices[uint64(MaterialTextures::Metallic)];
        }

        StructuredBufferInit sbInit;
        sbInit.Stride = sizeof(MaterialTextureIndices);
        sbInit.NumElements = numMaterials;
        sbInit.Dynamic = false;
        sbInit.Lifetime = BufferLifetime::Persistent;
        sbInit.InitData = textureIndices.Data();
        materialTextureIndices.Initialize(sbInit);
    }

    {
        // Main pass root signature
        D3D12_DESCRIPTOR_RANGE1 srvRanges[1] = {};
        srvRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[0].NumDescriptors = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        srvRanges[0].BaseShaderRegister = 0;
        srvRanges[0].RegisterSpace = 0;
        srvRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE1 decalTextureRanges[1] = {};
        decalTextureRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        decalTextureRanges[0].NumDescriptors = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        decalTextureRanges[0].BaseShaderRegister = 0;
        decalTextureRanges[0].RegisterSpace = 1;
        decalTextureRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER1 rootParameters[NumMainPassRootParams] = {};

        // VSCBuffer
        rootParameters[MainPass_VSCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[MainPass_VSCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rootParameters[MainPass_VSCBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[MainPass_VSCBuffer].Descriptor.ShaderRegister = 0;

        // PSCBuffer
        rootParameters[MainPass_PSCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[MainPass_PSCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[MainPass_PSCBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[MainPass_PSCBuffer].Descriptor.ShaderRegister = 0;

        // ShadowCBuffer
        rootParameters[MainPass_ShadowCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[MainPass_ShadowCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[MainPass_ShadowCBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[MainPass_ShadowCBuffer].Descriptor.ShaderRegister = 1;

        // LightCBuffer
        rootParameters[MainPass_LightCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[MainPass_LightCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[MainPass_LightCBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[MainPass_LightCBuffer].Descriptor.ShaderRegister = 3;

        // AppSettings
        rootParameters[MainPass_AppSettings].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[MainPass_AppSettings].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[MainPass_AppSettings].Descriptor.RegisterSpace = 0;
        rootParameters[MainPass_AppSettings].Descriptor.ShaderRegister = AppSettings::CBufferRegister;

        // MatIndexCBuffer
        rootParameters[MainPass_MatIndexCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParameters[MainPass_MatIndexCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[MainPass_MatIndexCBuffer].Constants.Num32BitValues = 1;
        rootParameters[MainPass_MatIndexCBuffer].Constants.RegisterSpace = 0;
        rootParameters[MainPass_MatIndexCBuffer].Constants.ShaderRegister = 2;

        // SRV descriptors
        rootParameters[MainPass_SRV].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[MainPass_SRV].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[MainPass_SRV].DescriptorTable.pDescriptorRanges = srvRanges;
        rootParameters[MainPass_SRV].DescriptorTable.NumDescriptorRanges = 1;

        // Decal texture descriptors
        rootParameters[MainPass_Decals].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[MainPass_Decals].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[MainPass_Decals].DescriptorTable.pDescriptorRanges = decalTextureRanges;
        rootParameters[MainPass_Decals].DescriptorTable.NumDescriptorRanges = 1;

        D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = {};
        staticSamplers[0] = DX12::GetStaticSamplerState(SamplerState::Anisotropic, 0);
        staticSamplers[1] = DX12::GetStaticSamplerState(SamplerState::ShadowMapPCF, 1);

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = ArraySize_(staticSamplers);
        rootSignatureDesc.pStaticSamplers = staticSamplers;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        DX12::CreateRootSignature(&mainPassRootSignature, rootSignatureDesc);
    }

    {
        // G-Buffer root signature
        D3D12_ROOT_PARAMETER1 rootParameters[2] = {};

        // VSCBuffer
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[0].Descriptor.RegisterSpace = 0;
        rootParameters[0].Descriptor.ShaderRegister = 0;

        // MatIndexCBuffer
        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[1].Constants.Num32BitValues = 1;
        rootParameters[1].Constants.RegisterSpace = 0;
        rootParameters[1].Constants.ShaderRegister = 2;

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = 0;
        rootSignatureDesc.pStaticSamplers = nullptr;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        DX12::CreateRootSignature(&gBufferRootSignature, rootSignatureDesc);
    }

    {
        // Depth only root signature
        D3D12_ROOT_PARAMETER1 rootParameters[1] = {};

        // VSCBuffer
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rootParameters[0].Descriptor.RegisterSpace = 0;
        rootParameters[0].Descriptor.ShaderRegister = 0;

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = 0;
        rootSignatureDesc.pStaticSamplers = nullptr;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        DX12::CreateRootSignature(&depthRootSignature, rootSignatureDesc);
    }
}

void MeshRenderer::Shutdown()
{
    DestroyPSOs();
    sunShadowMap.Shutdown();
    spotLightShadowMap.Shutdown();
    materialTextureIndices.Shutdown();
    DX12::Release(mainPassRootSignature);
    DX12::Release(gBufferRootSignature);
    DX12::Release(depthRootSignature);
}

void MeshRenderer::CreatePSOs(DXGI_FORMAT mainRTFormat, DXGI_FORMAT depthFormat, const DXGI_FORMAT* gBufferFormats,
                              uint64 numGBuffers, uint32 numMSAASamples)
{
    if(model == nullptr)
        return;


    ID3D12Device* device = DX12::Device;

    {
        // Main pass PSO
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = mainPassRootSignature;
        psoDesc.VS = meshVS.ByteCode();
        psoDesc.PS = meshPSForward.ByteCode();
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::BackFaceCull);
        psoDesc.BlendState = DX12::GetBlendState(BlendState::Disabled);
        psoDesc.DepthStencilState = DX12::GetDepthState(DepthState::WritesEnabled);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 2;
        psoDesc.RTVFormats[0] = mainRTFormat;
        psoDesc.RTVFormats[1] = gBufferFormats[0];
        psoDesc.DSVFormat = depthFormat;
        psoDesc.SampleDesc.Count = numMSAASamples;
        psoDesc.SampleDesc.Quality = numMSAASamples > 1 ? DX12::StandardMSAAPattern : 0;
        psoDesc.InputLayout.NumElements = uint32(Model::NumInputElements());
        psoDesc.InputLayout.pInputElementDescs = Model::InputElements();
        DXCall(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mainPassPSO)));
    }

    {
        // G-Buffer PSO
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = gBufferRootSignature;
        psoDesc.VS = meshVS.ByteCode();
        psoDesc.PS = meshPSGBuffer[AppSettings::ComputeUVGradients ? 1 : 0].ByteCode();
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::BackFaceCull);
        psoDesc.BlendState = DX12::GetBlendState(BlendState::Disabled);
        psoDesc.DepthStencilState = DX12::GetDepthState(DepthState::WritesEnabled);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = uint32(numGBuffers);
        for(uint64 i = 0; i < numGBuffers; ++i)
            psoDesc.RTVFormats[i] = gBufferFormats[i];
        psoDesc.DSVFormat = depthFormat;
        psoDesc.SampleDesc.Count = numMSAASamples;
        psoDesc.SampleDesc.Quality = numMSAASamples > 1 ? DX12::StandardMSAAPattern : 0;
        psoDesc.InputLayout.NumElements = uint32(Model::NumInputElements());
        psoDesc.InputLayout.pInputElementDescs = Model::InputElements();
        DXCall(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&gBufferPSO)));
    }

    {
        // Depth-only PSO
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = depthRootSignature;
        psoDesc.VS = meshVS.ByteCode();
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::BackFaceCull);
        psoDesc.BlendState = DX12::GetBlendState(BlendState::Disabled);
        psoDesc.DepthStencilState = DX12::GetDepthState(DepthState::WritesEnabled);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 0;
        psoDesc.DSVFormat = depthFormat;
        psoDesc.SampleDesc.Count = numMSAASamples;
        psoDesc.SampleDesc.Quality = numMSAASamples > 1 ? DX12::StandardMSAAPattern : 0;
        psoDesc.InputLayout.NumElements = uint32(Model::NumInputElements());
        psoDesc.InputLayout.pInputElementDescs = Model::InputElements();
        DXCall(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&depthPSO)));

        // Spotlight shadow depth PSO
        psoDesc.DSVFormat = spotLightShadowMap.DSVFormat;
        psoDesc.SampleDesc.Count = spotLightShadowMap.MSAASamples;
        psoDesc.SampleDesc.Quality = spotLightShadowMap.MSAASamples > 1 ? DX12::StandardMSAAPattern : 0;
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::BackFaceCull);
        DXCall(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&spotLightShadowPSO)));

        // Sun shadow depth PSO
        psoDesc.DSVFormat = sunShadowMap.DSVFormat;
        psoDesc.SampleDesc.Count = sunShadowMap.MSAASamples;
        psoDesc.SampleDesc.Quality = sunShadowMap.MSAASamples > 1 ? DX12::StandardMSAAPattern : 0;
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::BackFaceCullNoZClip);
        DXCall(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&sunShadowPSO)));
    }
}

void MeshRenderer::DestroyPSOs()
{
    DX12::DeferredRelease(mainPassPSO);
    DX12::DeferredRelease(gBufferPSO);
    DX12::DeferredRelease(depthPSO);
    DX12::DeferredRelease(spotLightShadowPSO);
    DX12::DeferredRelease(sunShadowPSO);
}

// Renders all meshes in the model, with shadows
void MeshRenderer::RenderMainPass(ID3D12GraphicsCommandList* cmdList, const Camera& camera, const MainPassData& mainPassData)
{
    PIXMarker marker(cmdList, "Mesh Rendering");

    uint64 numVisible = 0;
    if(AppSettings::SortByDepth)
        numVisible = CullMeshesAndSort(camera, meshBoundingBoxes, meshZDepths, meshDrawIndices);
    else
        numVisible = CullMeshes(camera, meshBoundingBoxes, meshDrawIndices);

    cmdList->SetGraphicsRootSignature(mainPassRootSignature);
    cmdList->SetPipelineState(mainPassPSO);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    Float4x4 world;

    // Set constant buffers
    meshVSConstants.Data.World = world;
    meshVSConstants.Data.View = camera.ViewMatrix();
    meshVSConstants.Data.WorldViewProjection = world * camera.ViewProjectionMatrix();
    meshVSConstants.Upload();
    meshVSConstants.SetAsGfxRootParameter(cmdList, MainPass_VSCBuffer);

    meshPSConstants.Data.SunDirectionWS = AppSettings::SunDirection;
    meshPSConstants.Data.SunIrradiance = mainPassData.SkyCache->SunIrradiance;
    meshPSConstants.Data.CosSunAngularRadius = std::cos(DegToRad(AppSettings::SunSize));
    meshPSConstants.Data.SinSunAngularRadius = std::sin(DegToRad(AppSettings::SunSize));
    meshPSConstants.Data.CameraPosWS = camera.Position();

    meshPSConstants.Data.CursorDecalPos = mainPassData.CursorDecal.Position;
    meshPSConstants.Data.CursorDecalIntensity = mainPassData.CursorDecalIntensity;
    meshPSConstants.Data.CursorDecalOrientation = mainPassData.CursorDecal.Orientation;
    meshPSConstants.Data.CursorDecalSize = mainPassData.CursorDecal.Size;
    meshPSConstants.Data.CursorDecalType = mainPassData.CursorDecal.Type;
    meshPSConstants.Data.NumXTiles = uint32(AppSettings::NumXTiles);
    meshPSConstants.Data.NumXYTiles = uint32(AppSettings::NumXTiles * AppSettings::NumYTiles);
    meshPSConstants.Data.NearClip = camera.NearClip();
    meshPSConstants.Data.FarClip = camera.FarClip();

    meshPSConstants.Data.SkySH = mainPassData.SkyCache->SH;
    meshPSConstants.Upload();
    meshPSConstants.SetAsGfxRootParameter(cmdList, MainPass_PSCBuffer);

    sunShadowConstants.Upload();
    sunShadowConstants.SetAsGfxRootParameter(cmdList, MainPass_ShadowCBuffer);

    cmdList->SetGraphicsRootConstantBufferView(MainPass_LightCBuffer, mainPassData.SpotLightBuffer->InternalBuffer.GPUAddress);

    AppSettings::BindCBufferGfx(cmdList, MainPass_AppSettings);

    D3D12_CPU_DESCRIPTOR_HANDLE psSRVs[] =
    {
        sunShadowMap.SRV(),
        spotLightShadowMap.SRV(),
        materialTextureIndices.SRV(),
        mainPassData.DecalBuffer->SRV(),
        mainPassData.DecalClusterBuffer->SRV(),
        mainPassData.SpotLightClusterBuffer->SRV(),
    };

    // We need to get everything into a contiguous shader-visible descriptor table
    const uint64 numMaterialTextures = model->MaterialTextures().Count();
    LinearDescriptorHeap& descriptorHeap = DX12::SRVDescriptorHeapGPU[DX12::CurrFrameIdx];
    DescriptorHandle tableStart = descriptorHeap.Allocate(ArraySize_(psSRVs) + numMaterialTextures);

    // First the non-material SRVs
    for(uint64 i = 0; i < ArraySize_(psSRVs); ++i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dstDescriptor = tableStart.CPUHandle;
        dstDescriptor.ptr += i * DX12::SRVDescriptorSize;
        DX12::Device->CopyDescriptorsSimple(1, dstDescriptor, psSRVs[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // And now the material textures
    D3D12_CPU_DESCRIPTOR_HANDLE srcMaterialTextures = model->MaterialTextureDescriptors();
    D3D12_CPU_DESCRIPTOR_HANDLE dstMaterialTextures = tableStart.CPUHandle;
    dstMaterialTextures.ptr += ArraySize_(psSRVs) * DX12::SRVDescriptorSize;
    DX12::Device->CopyDescriptorsSimple(uint32(numMaterialTextures), dstMaterialTextures, srcMaterialTextures, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    cmdList->SetGraphicsRootDescriptorTable(MainPass_SRV, tableStart.GPUHandle);

    // Bind the decal textures
    D3D12_CPU_DESCRIPTOR_HANDLE decalDescriptors[AppSettings::NumDecalTextures] = { };
    for(uint64 i = 0; i < AppSettings::NumDecalTextures; ++i)
        decalDescriptors[i] = mainPassData.DecalTextures[i].SRV.CPUHandle;

    DX12::BindShaderResources(cmdList, MainPass_Decals, AppSettings::NumDecalTextures, decalDescriptors);

    // Bind vertices and indices
    D3D12_VERTEX_BUFFER_VIEW vbView = model->VertexBuffer().VBView();
    D3D12_INDEX_BUFFER_VIEW ibView = model->IndexBuffer().IBView();
    cmdList->IASetVertexBuffers(0, 1, &vbView);
    cmdList->IASetIndexBuffer(&ibView);

    // Draw all visible meshes
    uint32 currMaterial = uint32(-1);
    for(uint64 i = 0; i < numVisible; ++i)
    {
        uint64 meshIdx = meshDrawIndices[i];
        const Mesh& mesh = model->Meshes()[meshIdx];

        // Draw all parts
        for(uint64 partIdx = 0; partIdx < mesh.NumMeshParts(); ++partIdx)
        {
            const MeshPart& part = mesh.MeshParts()[partIdx];
            if(part.MaterialIdx != currMaterial)
            {
                cmdList->SetGraphicsRoot32BitConstant(MainPass_MatIndexCBuffer, part.MaterialIdx, 0);
                currMaterial = part.MaterialIdx;
            }
            cmdList->DrawIndexedInstanced(part.IndexCount, 1, mesh.IndexOffset() + part.IndexStart, mesh.VertexOffset(), 0);
        }
    }
}

// Renders all meshes to the G-Buffer
void MeshRenderer::RenderGBuffer(ID3D12GraphicsCommandList* cmdList, const Camera& camera)
{
    PIXMarker marker(cmdList, L"Render G-Buffer");
    CPUProfileBlock cpuProfileBlock("Render G-Buffer");

    uint64 numVisible = 0;
    if(AppSettings::SortByDepth)
        numVisible = CullMeshesAndSort(camera, meshBoundingBoxes, meshZDepths, meshDrawIndices);
    else
        numVisible = CullMeshes(camera, meshBoundingBoxes, meshDrawIndices);

    cmdList->SetGraphicsRootSignature(gBufferRootSignature);
    cmdList->SetPipelineState(gBufferPSO);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    Float4x4 world;

    // Set constant buffers
    meshVSConstants.Data.World = world;
    meshVSConstants.Data.View = camera.ViewMatrix();
    meshVSConstants.Data.WorldViewProjection = world * camera.ViewProjectionMatrix();
    meshVSConstants.Data.NearClip = camera.NearClip();
    meshVSConstants.Data.FarClip = camera.FarClip();
    meshVSConstants.Upload();
    meshVSConstants.SetAsGfxRootParameter(cmdList, 0);

    // Bind vertices and indices
    D3D12_VERTEX_BUFFER_VIEW vbView = model->VertexBuffer().VBView();
    D3D12_INDEX_BUFFER_VIEW ibView = model->IndexBuffer().IBView();
    cmdList->IASetVertexBuffers(0, 1, &vbView);
    cmdList->IASetIndexBuffer(&ibView);

    // Draw all visible meshes
    uint32 currMaterial = uint32(-1);
    for(uint64 i = 0; i < numVisible; ++i)
    {
        uint64 meshIdx = meshDrawIndices[i];
        const Mesh& mesh = model->Meshes()[meshIdx];

        // Draw all parts
        for(uint64 partIdx = 0; partIdx < mesh.NumMeshParts(); ++partIdx)
        {
            const MeshPart& part = mesh.MeshParts()[partIdx];
            if(part.MaterialIdx != currMaterial)
            {
                cmdList->SetGraphicsRoot32BitConstant(1, part.MaterialIdx, 0);
                currMaterial = part.MaterialIdx;
            }
            cmdList->DrawIndexedInstanced(part.IndexCount, 1, mesh.IndexOffset() + part.IndexStart, mesh.VertexOffset(), 0);
        }
    }
}

// Renders all meshes using depth-only rendering
void MeshRenderer::RenderDepth(ID3D12GraphicsCommandList* cmdList, const Camera& camera, ID3D12PipelineState* pso, uint64 numVisible)
{
    cmdList->SetGraphicsRootSignature(depthRootSignature);
    cmdList->SetPipelineState(pso);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    Float4x4 world;

    // Set constant buffers
    meshVSConstants.Data.World = world;
    meshVSConstants.Data.View = camera.ViewMatrix();
    meshVSConstants.Data.WorldViewProjection = world * camera.ViewProjectionMatrix();
    meshVSConstants.Upload();
    meshVSConstants.SetAsGfxRootParameter(cmdList, 0);

    // Bind vertices and indices
    D3D12_VERTEX_BUFFER_VIEW vbView = model->VertexBuffer().VBView();
    D3D12_INDEX_BUFFER_VIEW ibView = model->IndexBuffer().IBView();
    cmdList->IASetVertexBuffers(0, 1, &vbView);
    cmdList->IASetIndexBuffer(&ibView);

    // Draw all meshes
    for(uint64 i = 0; i < numVisible; ++i)
    {
        uint64 meshIdx = meshDrawIndices[i];
        const Mesh& mesh = model->Meshes()[meshIdx];

        // Draw the whole mesh
        cmdList->DrawIndexedInstanced(mesh.NumIndices(), 1, mesh.IndexOffset(), mesh.VertexOffset(), 0);
    }
}

// Renders all meshes using depth-only rendering for a sun shadow map
void MeshRenderer::RenderDepthPrepass(ID3D12GraphicsCommandList* cmdList, const Camera& camera)
{
    PIXMarker marker(cmdList, L"Depth Prepass");
    CPUProfileBlock cpuProfileBlock("Depth Prepass");
    ProfileBlock profileBlock(cmdList, "Depth Prepass");

    uint64 numVisible = 0;
    if(AppSettings::SortByDepth)
        numVisible = CullMeshesAndSort(camera, meshBoundingBoxes, meshZDepths, meshDrawIndices);
    else
        numVisible = CullMeshes(camera, meshBoundingBoxes, meshDrawIndices);

    RenderDepth(cmdList, camera, depthPSO, numVisible);
}

// Renders all meshes using depth-only rendering for a sun shadow map
void MeshRenderer::RenderSunShadowDepth(ID3D12GraphicsCommandList* cmdList, const OrthographicCamera& camera)
{
    const uint64 numVisible = CullMeshesOrthographic(camera, true, meshBoundingBoxes, meshDrawIndices);
    RenderDepth(cmdList, camera, sunShadowPSO, numVisible);
}

void MeshRenderer::RenderSpotLightShadowDepth(ID3D12GraphicsCommandList* cmdList, const Camera& camera)
{
    const uint64 numVisible = CullMeshes(camera, meshBoundingBoxes, meshDrawIndices);
    RenderDepth(cmdList, camera, spotLightShadowPSO, numVisible);
}

// Renders meshes using cascaded shadow mapping
void MeshRenderer::RenderSunShadowMap(ID3D12GraphicsCommandList* cmdList, const Camera& camera)
{
    PIXMarker marker(cmdList, L"Sun Shadow Map Rendering");
    CPUProfileBlock cpuProfileBlock("Sun Shadow Map Rendering");
    ProfileBlock profileBlock(cmdList, "Sun Shadow Map Rendering");

    OrthographicCamera cascadeCameras[NumCascades];
    PrepareShadowCascades(AppSettings::SunDirection, SunShadowMapSize, true, camera, sunShadowConstants.Data, cascadeCameras);

    sunShadowMap.MakeWritable(cmdList);

    // Render the meshes to each cascade
    for(uint64 cascadeIdx = 0; cascadeIdx < NumCascades; ++cascadeIdx)
    {
        PIXMarker cascadeMarker(cmdList, MakeString(L"Rendering Shadow Map Cascade %u", cascadeIdx).c_str());

        // Set the viewport
        DX12::SetViewport(cmdList, SunShadowMapSize, SunShadowMapSize);

        // Set the shadow map as the depth target
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = sunShadowMap.ArrayDSVs[cascadeIdx].CPUHandle;
        cmdList->OMSetRenderTargets(0, nullptr, false, &dsv);
        cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

        // Draw the mesh with depth only, using the new shadow camera
        RenderSunShadowDepth(cmdList, cascadeCameras[cascadeIdx]);
    }

    sunShadowMap.MakeReadable(cmdList);
}

// Render shadows for all spot lights
void MeshRenderer::RenderSpotLightShadowMap(ID3D12GraphicsCommandList* cmdList, const Camera& camera)
{
    PIXMarker marker(cmdList, L"Spot Light Shadow Map Rendering");
    CPUProfileBlock cpuProfileBlock("Spot Light Shadow Map Rendering");
    ProfileBlock profileBlock(cmdList, "Spot Light Shadow Map Rendering");

    spotLightShadowMap.MakeWritable(cmdList);

    const Array<ModelSpotLight>& spotLights = model->SpotLights();
    const uint64 numSpotLights = Min<uint64>(spotLights.Size(), AppSettings::MaxLightClamp);
    for(uint64 i = 0; i < numSpotLights; ++i)
    {
        PIXMarker lightMarker(cmdList, MakeString(L"Rendering Spot Light Shadow %u", i).c_str());

        // Set the viewport
        DX12::SetViewport(cmdList, SpotLightShadowMapSize, SpotLightShadowMapSize);

        // Set the shadow map as the depth target
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = spotLightShadowMap.ArrayDSVs[i].CPUHandle;
        cmdList->OMSetRenderTargets(0, nullptr, false, &dsv);
        cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

        const ModelSpotLight& light = spotLights[i];

        // Draw the mesh with depth only, using the new shadow camera
        PerspectiveCamera shadowCamera;
        shadowCamera.Initialize(1.0f, light.AngularAttenuation.y, 0.1f, AppSettings::SpotLightRange);
        shadowCamera.SetPosition(light.Position);
        shadowCamera.SetOrientation(light.Orientation);
        RenderSpotLightShadowDepth(cmdList, shadowCamera);

        Float4x4 shadowMatrix = shadowCamera.ViewProjectionMatrix() * ShadowScaleOffsetMatrix;
        spotLightShadowMatrices[i] = Float4x4::Transpose(shadowMatrix);
    }

    spotLightShadowMap.MakeReadable(cmdList);
}