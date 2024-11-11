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
    MainPass_StandardDescriptors,
    MainPass_VSCBuffer,
    MainPass_PSCBuffer,
    MainPass_ShadowCBuffer,
    MainPass_MatIndexCBuffer,
    MainPass_LightCBuffer,
    MainPass_SRVIndices,
    MainPass_AppSettings,

    NumMainPassRootParams,
};

struct MeshVSConstants
{
    Float4Align Float4x4 World;
    Float4Align Float4x4 View;
    Float4Align Float4x4 WorldViewProjection;
    float NearClip = 0.0f;
    float FarClip = 0.0f;
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
    meshDepthVS = CompileFromFile(L"DepthOnly.hlsl", "VS", ShaderType::Vertex);
    meshDepthAlphaTestPS = CompileFromFile(L"DepthOnly.hlsl", "PS", ShaderType::Pixel);

    CompileOptions opts;
    opts.Add("OutputUVGradients_", 1);
    opts.Add("AlphaTest_", 0);
    meshVS = CompileFromFile(L"Mesh.hlsl", "VS", ShaderType::Vertex, opts);
    meshPSForward = CompileFromFile(L"Mesh.hlsl", "PSForward", ShaderType::Pixel, opts);
    meshPSGBuffer[0] = CompileFromFile(L"Mesh.hlsl", "PSGBuffer", ShaderType::Pixel, opts);

    opts.Reset();
    opts.Add("OutputUVGradients_", 0);
    opts.Add("AlphaTest_", 0);
    meshPSGBuffer[1] = CompileFromFile(L"Mesh.hlsl", "PSGBuffer", ShaderType::Pixel, opts);

    opts.Reset();
    opts.Add("OutputUVGradients_", 1);
    opts.Add("AlphaTest_", 1);
    meshPSForwardAlphaTest = CompileFromFile(L"Mesh.hlsl", "PSForward", ShaderType::Pixel, opts);
    meshPSGBufferAlphaTest[0] = CompileFromFile(L"Mesh.hlsl", "PSGBuffer", ShaderType::Pixel, opts);

    opts.Reset();
    opts.Add("OutputUVGradients_", 0);
    opts.Add("AlphaTest_", 1);
    meshPSGBufferAlphaTest[1] = CompileFromFile(L"Mesh.hlsl", "PSGBuffer", ShaderType::Pixel, opts);
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

    LoadShaders();

    {
        DepthBufferInit dbInit;
        dbInit.Width = SunShadowMapSize;
        dbInit.Height = SunShadowMapSize;
        dbInit.Format = DXGI_FORMAT_D32_FLOAT;
        dbInit.MSAASamples = 1;
        dbInit.ArraySize = NumCascades;
        dbInit.InitialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        dbInit.Name = L"Sun Shadow Map";
        sunShadowMap.Initialize(dbInit);
    }

    {
        DepthBufferInit dbInit;
        dbInit.Width = SpotLightShadowMapSize;
        dbInit.Height = SpotLightShadowMapSize;
        dbInit.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dbInit.MSAASamples = 1;
        dbInit.ArraySize = model->SpotLights().Size();
        dbInit.InitialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        dbInit.Name = L"Spot Light Shadow Map";
        spotLightShadowMap.Initialize(dbInit);
    }

    {
        // Create a structured buffer containing texture indices per-material
        const Array<MeshMaterial>& materials = model->Materials();
        const uint64 numMaterials = materials.Size();
        Array<MaterialTextureIndices> textureIndices(numMaterials);
        materialHasAlphaTest.Init(numMaterials, false);
        for(uint64 i = 0; i < numMaterials; ++i)
        {
            MaterialTextureIndices& matIndices = textureIndices[i];
            const MeshMaterial& material = materials[i];

            matIndices.Albedo = material.Textures[uint64(MaterialTextures::Albedo)]->SRV;
            matIndices.Normal = material.Textures[uint64(MaterialTextures::Normal)]->SRV;
            matIndices.Roughness = material.Textures[uint64(MaterialTextures::Roughness)]->SRV;
            matIndices.Metallic = material.Textures[uint64(MaterialTextures::Metallic)]->SRV;

            const std::wstring& albedoTexName = material.TextureNames[uint32(MaterialTextures::Albedo)];
            if(albedoTexName == L"Sponza_Thorn_diffuse.png" || albedoTexName == L"VasePlant_diffuse.png")
                materialHasAlphaTest[i] = true;
            else
                materialHasAlphaTest[i] = false;
        }

        StructuredBufferInit sbInit;
        sbInit.Stride = sizeof(MaterialTextureIndices);
        sbInit.NumElements = numMaterials;
        sbInit.Dynamic = false;
        sbInit.InitData = textureIndices.Data();
        materialTextureIndices.Initialize(sbInit);
        materialTextureIndices.Resource()->SetName(L"Material Texture Indices");
    }

    {
        // Main pass root signature
        D3D12_ROOT_PARAMETER1 rootParameters[NumMainPassRootParams] = {};

        // "Standard"  descriptor table
        rootParameters[MainPass_StandardDescriptors].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[MainPass_StandardDescriptors].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[MainPass_StandardDescriptors].DescriptorTable.pDescriptorRanges = DX12::StandardDescriptorRanges();
        rootParameters[MainPass_StandardDescriptors].DescriptorTable.NumDescriptorRanges = DX12::NumStandardDescriptorRanges;

        // VSCBuffer
        rootParameters[MainPass_VSCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[MainPass_VSCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rootParameters[MainPass_VSCBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[MainPass_VSCBuffer].Descriptor.ShaderRegister = 0;
        rootParameters[MainPass_VSCBuffer].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        // PSCBuffer
        rootParameters[MainPass_PSCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[MainPass_PSCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[MainPass_PSCBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[MainPass_PSCBuffer].Descriptor.ShaderRegister = 0;
        rootParameters[MainPass_PSCBuffer].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        // ShadowCBuffer
        rootParameters[MainPass_ShadowCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[MainPass_ShadowCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[MainPass_ShadowCBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[MainPass_ShadowCBuffer].Descriptor.ShaderRegister = 1;
        rootParameters[MainPass_ShadowCBuffer].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        // MatIndexCBuffer
        rootParameters[MainPass_MatIndexCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParameters[MainPass_MatIndexCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[MainPass_MatIndexCBuffer].Constants.Num32BitValues = 2;
        rootParameters[MainPass_MatIndexCBuffer].Constants.RegisterSpace = 0;
        rootParameters[MainPass_MatIndexCBuffer].Constants.ShaderRegister = 2;

        // LightCBuffer
        rootParameters[MainPass_LightCBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[MainPass_LightCBuffer].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[MainPass_LightCBuffer].Descriptor.RegisterSpace = 0;
        rootParameters[MainPass_LightCBuffer].Descriptor.ShaderRegister = 3;
        rootParameters[MainPass_LightCBuffer].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;

        // SRV descriptor indices
        rootParameters[MainPass_SRVIndices].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[MainPass_SRVIndices].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[MainPass_SRVIndices].Descriptor.RegisterSpace = 0;
        rootParameters[MainPass_SRVIndices].Descriptor.ShaderRegister = 4;
        rootParameters[MainPass_SRVIndices].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

        // AppSettings
        rootParameters[MainPass_AppSettings].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[MainPass_AppSettings].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[MainPass_AppSettings].Descriptor.RegisterSpace = 0;
        rootParameters[MainPass_AppSettings].Descriptor.ShaderRegister = AppSettings::CBufferRegister;
        rootParameters[MainPass_AppSettings].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;

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
        D3D12_ROOT_PARAMETER1 rootParameters[3] = {};

        // VSCBuffer
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[0].Descriptor.RegisterSpace = 0;
        rootParameters[0].Descriptor.ShaderRegister = 0;

        // MatIndexCBuffer
        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[1].Constants.Num32BitValues = 2;
        rootParameters[1].Constants.RegisterSpace = 0;
        rootParameters[1].Constants.ShaderRegister = 2;

        // "Standard"  descriptor table
        rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[2].DescriptorTable.pDescriptorRanges = DX12::StandardDescriptorRanges();
        rootParameters[2].DescriptorTable.NumDescriptorRanges = DX12::NumStandardDescriptorRanges;

        D3D12_STATIC_SAMPLER_DESC staticSamplers[1] = {};
        staticSamplers[0] = DX12::GetStaticSamplerState(SamplerState::Anisotropic, 0);

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = ArraySize_(staticSamplers);
        rootSignatureDesc.pStaticSamplers = staticSamplers;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        DX12::CreateRootSignature(&gBufferRootSignature, rootSignatureDesc);
    }

    {
        // Depth only root signature
        D3D12_ROOT_PARAMETER1 rootParameters[3] = {};

        // VSCBuffer
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rootParameters[0].Descriptor.RegisterSpace = 0;
        rootParameters[0].Descriptor.ShaderRegister = 0;

        // MatIndexCBuffer
        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[1].Constants.Num32BitValues = 2;
        rootParameters[1].Constants.RegisterSpace = 0;
        rootParameters[1].Constants.ShaderRegister = 1;

        // "Standard"  descriptor table
        rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParameters[2].DescriptorTable.pDescriptorRanges = DX12::StandardDescriptorRanges();
        rootParameters[2].DescriptorTable.NumDescriptorRanges = DX12::NumStandardDescriptorRanges;

        D3D12_STATIC_SAMPLER_DESC staticSamplers[1] = {};
        staticSamplers[0] = DX12::GetStaticSamplerState(SamplerState::Anisotropic, 0);

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = ArraySize_(staticSamplers);
        rootSignatureDesc.pStaticSamplers = staticSamplers;
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

        psoDesc.PS = meshPSForwardAlphaTest.ByteCode();
        DXCall(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mainPassAlphaTestPSO)));

        psoDesc.PS = meshPSForward.ByteCode();
        psoDesc.DepthStencilState = DX12::GetDepthState(DepthState::Enabled);
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
        DXCall(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mainPassDepthPrepassPSO)));
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

        psoDesc.PS = meshPSGBufferAlphaTest[AppSettings::ComputeUVGradients ? 1 : 0].ByteCode();
        DXCall(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&gBufferAlphaTestPSO)));
    }

    {
        // Depth-only PSO
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = depthRootSignature;
        psoDesc.VS = meshDepthVS.ByteCode();
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

        psoDesc.PS = meshDepthAlphaTestPS.ByteCode();
        DXCall(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&depthAlphaTestPSO)));
        psoDesc.PS = { };

        // Spotlight shadow depth PSO
        psoDesc.DSVFormat = spotLightShadowMap.DSVFormat;
        psoDesc.SampleDesc.Count = spotLightShadowMap.MSAASamples;
        psoDesc.SampleDesc.Quality = spotLightShadowMap.MSAASamples > 1 ? DX12::StandardMSAAPattern : 0;
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::BackFaceCull);
        DXCall(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&spotLightShadowPSO)));

        psoDesc.PS = meshDepthAlphaTestPS.ByteCode();
        DXCall(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&spotLightShadowAlphaTestPSO)));
        psoDesc.PS = { };

        // Sun shadow depth PSO
        psoDesc.DSVFormat = sunShadowMap.DSVFormat;
        psoDesc.SampleDesc.Count = sunShadowMap.MSAASamples;
        psoDesc.SampleDesc.Quality = sunShadowMap.MSAASamples > 1 ? DX12::StandardMSAAPattern : 0;
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::BackFaceCullNoZClip);
        DXCall(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&sunShadowPSO)));

        psoDesc.PS = meshDepthAlphaTestPS.ByteCode();
        DXCall(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&sunShadowAlphaTestPSO)));
        psoDesc.PS = { };
    }
}

void MeshRenderer::DestroyPSOs()
{
    DX12::DeferredRelease(mainPassPSO);
    DX12::DeferredRelease(mainPassAlphaTestPSO);
    DX12::DeferredRelease(mainPassDepthPrepassPSO);
    DX12::DeferredRelease(gBufferPSO);
    DX12::DeferredRelease(gBufferAlphaTestPSO);
    DX12::DeferredRelease(depthPSO);
    DX12::DeferredRelease(depthAlphaTestPSO);
    DX12::DeferredRelease(spotLightShadowPSO);
    DX12::DeferredRelease(spotLightShadowAlphaTestPSO);
    DX12::DeferredRelease(sunShadowPSO);
    DX12::DeferredRelease(sunShadowAlphaTestPSO);
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

    ID3D12PipelineState* basePSO = AppSettings::DepthPrepass ? mainPassDepthPrepassPSO : mainPassPSO;
    ID3D12PipelineState* alphaTestPSO = AppSettings::DepthPrepass ? mainPassDepthPrepassPSO : mainPassAlphaTestPSO; // Alpha test was already done during the depth prepass

    cmdList->SetGraphicsRootSignature(mainPassRootSignature);
    cmdList->SetPipelineState(basePSO);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D12PipelineState* currPSO = basePSO;

    DX12::BindStandardDescriptorTable(cmdList, MainPass_StandardDescriptors, CmdListMode::Graphics);

    Float4x4 world;

    // Set constant buffers
    MeshVSConstants vsConstants;
    vsConstants.World = world;
    vsConstants.View = camera.ViewMatrix();
    vsConstants.WorldViewProjection = world * camera.ViewProjectionMatrix();
    DX12::BindTempConstantBuffer(cmdList, vsConstants, MainPass_VSCBuffer, CmdListMode::Graphics);

    ShadingConstants psConstants;
    psConstants.SunDirectionWS = AppSettings::SunDirection;
    psConstants.SunIrradiance = mainPassData.SkyCache->SunIrradiance;
    psConstants.CosSunAngularRadius = std::cos(DegToRad(AppSettings::SunSize));
    psConstants.SinSunAngularRadius = std::sin(DegToRad(AppSettings::SunSize));
    psConstants.CameraPosWS = camera.Position();

    psConstants.CursorDecalPos = mainPassData.CursorDecal.Position;
    psConstants.CursorDecalIntensity = mainPassData.CursorDecalIntensity;
    psConstants.CursorDecalOrientation = mainPassData.CursorDecal.Orientation;
    psConstants.CursorDecalSize = mainPassData.CursorDecal.Size;
    psConstants.CursorDecalTexIdx = mainPassData.CursorDecal.AlbedoTexIdx;
    psConstants.NumXTiles = uint32(AppSettings::NumXTiles);
    psConstants.NumXYTiles = uint32(AppSettings::NumXTiles * AppSettings::NumYTiles);
    psConstants.NearClip = camera.NearClip();
    psConstants.FarClip = camera.FarClip();

    psConstants.SkySH = mainPassData.SkyCache->SH;
    DX12::BindTempConstantBuffer(cmdList, psConstants, MainPass_PSCBuffer, CmdListMode::Graphics);

    DX12::BindTempConstantBuffer(cmdList, sunShadowConstants, MainPass_ShadowCBuffer, CmdListMode::Graphics);

    mainPassData.SpotLightBuffer->SetAsGfxRootParameter(cmdList, MainPass_LightCBuffer);

    AppSettings::BindCBufferGfx(cmdList, MainPass_AppSettings);

    uint32 psSRVs[] =
    {
        sunShadowMap.SRV(),
        spotLightShadowMap.SRV(),
        mainPassData.DecalBuffer->SRV,
        mainPassData.DecalClusterBuffer->SRV,
        mainPassData.SpotLightClusterBuffer->SRV,
    };

    DX12::BindTempConstantBuffer(cmdList, psSRVs, MainPass_SRVIndices, CmdListMode::Graphics);

    cmdList->SetGraphicsRoot32BitConstant(MainPass_MatIndexCBuffer, materialTextureIndices.SRV, 0);

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
                cmdList->SetGraphicsRoot32BitConstant(MainPass_MatIndexCBuffer, part.MaterialIdx, 1);
                currMaterial = part.MaterialIdx;

                ID3D12PipelineState* psoToUse = materialHasAlphaTest[part.MaterialIdx] ? alphaTestPSO : basePSO;
                if(psoToUse != currPSO)
                {
                    cmdList->SetPipelineState(psoToUse);
                    currPSO = psoToUse;
                }
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

    DX12::BindStandardDescriptorTable(cmdList, 2, CmdListMode::Graphics);

    ID3D12PipelineState* currPSO = gBufferPSO;

    Float4x4 world;

    // Set constant buffers
    MeshVSConstants vsConstants;
    vsConstants.World = world;
    vsConstants.View = camera.ViewMatrix();
    vsConstants.WorldViewProjection = world * camera.ViewProjectionMatrix();
    vsConstants.NearClip = camera.NearClip();
    vsConstants.FarClip = camera.FarClip();
    DX12::BindTempConstantBuffer(cmdList, vsConstants, 0, CmdListMode::Graphics);

    cmdList->SetGraphicsRoot32BitConstant(1, materialTextureIndices.SRV, 0);

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
                cmdList->SetGraphicsRoot32BitConstant(1, part.MaterialIdx, 1);
                currMaterial = part.MaterialIdx;

                ID3D12PipelineState* psoToUse = materialHasAlphaTest[part.MaterialIdx] ? gBufferAlphaTestPSO : gBufferPSO;
                if(psoToUse != currPSO)
                {
                    cmdList->SetPipelineState(psoToUse);
                    currPSO = psoToUse;
                }
            }
            cmdList->DrawIndexedInstanced(part.IndexCount, 1, mesh.IndexOffset() + part.IndexStart, mesh.VertexOffset(), 0);
        }
    }
}

// Renders all meshes using depth-only rendering
void MeshRenderer::RenderDepth(ID3D12GraphicsCommandList* cmdList, const Camera& camera, ID3D12PipelineState* pso, ID3D12PipelineState* alphaTestPSO, uint64 numVisible)
{
    cmdList->SetGraphicsRootSignature(depthRootSignature);
    cmdList->SetPipelineState(pso);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D12PipelineState* currPSO = gBufferPSO;

    DX12::BindStandardDescriptorTable(cmdList, 2, CmdListMode::Graphics);

    Float4x4 world;

    // Set constant buffers
    MeshVSConstants vsConstants;
    vsConstants.World = world;
    vsConstants.View = camera.ViewMatrix();
    vsConstants.WorldViewProjection = world * camera.ViewProjectionMatrix();
    DX12::BindTempConstantBuffer(cmdList, vsConstants, 0, CmdListMode::Graphics);

    cmdList->SetGraphicsRoot32BitConstant(1, materialTextureIndices.SRV, 0);

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
                cmdList->SetGraphicsRoot32BitConstant(1, part.MaterialIdx, 1);
                currMaterial = part.MaterialIdx;

                ID3D12PipelineState* psoToUse = materialHasAlphaTest[part.MaterialIdx] ? alphaTestPSO : pso;
                if(psoToUse != currPSO)
                {
                    cmdList->SetPipelineState(psoToUse);
                    currPSO = psoToUse;
                }
            }
            cmdList->DrawIndexedInstanced(part.IndexCount, 1, mesh.IndexOffset() + part.IndexStart, mesh.VertexOffset(), 0);
        }
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

    RenderDepth(cmdList, camera, depthPSO, depthAlphaTestPSO, numVisible);
}

// Renders all meshes using depth-only rendering for a sun shadow map
void MeshRenderer::RenderSunShadowDepth(ID3D12GraphicsCommandList* cmdList, const OrthographicCamera& camera)
{
    const uint64 numVisible = CullMeshesOrthographic(camera, true, meshBoundingBoxes, meshDrawIndices);
    RenderDepth(cmdList, camera, sunShadowPSO, sunShadowAlphaTestPSO, numVisible);
}

void MeshRenderer::RenderSpotLightShadowDepth(ID3D12GraphicsCommandList* cmdList, const Camera& camera)
{
    const uint64 numVisible = CullMeshes(camera, meshBoundingBoxes, meshDrawIndices);
    RenderDepth(cmdList, camera, spotLightShadowPSO, spotLightShadowAlphaTestPSO, numVisible);
}

// Renders meshes using cascaded shadow mapping
void MeshRenderer::RenderSunShadowMap(ID3D12GraphicsCommandList* cmdList, const Camera& camera)
{
    PIXMarker marker(cmdList, L"Sun Shadow Map Rendering");
    CPUProfileBlock cpuProfileBlock("Sun Shadow Map Rendering");
    ProfileBlock profileBlock(cmdList, "Sun Shadow Map Rendering");

    OrthographicCamera cascadeCameras[NumCascades];
    ShadowHelper::PrepareCascades(AppSettings::SunDirection, SunShadowMapSize, true, camera, sunShadowConstants.Base, cascadeCameras);

    // Render the meshes to each cascade
    for(uint64 cascadeIdx = 0; cascadeIdx < NumCascades; ++cascadeIdx)
    {
        PIXMarker cascadeMarker(cmdList, MakeString(L"Rendering Shadow Map Cascade %u", cascadeIdx).c_str());

        // Set the viewport
        DX12::SetViewport(cmdList, SunShadowMapSize, SunShadowMapSize);

        // Set the shadow map as the depth target
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = sunShadowMap.ArrayDSVs[cascadeIdx];
        cmdList->OMSetRenderTargets(0, nullptr, false, &dsv);
        cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

        // Draw the mesh with depth only, using the new shadow camera
        RenderSunShadowDepth(cmdList, cascadeCameras[cascadeIdx]);
    }
}

// Render shadows for all spot lights
void MeshRenderer::RenderSpotLightShadowMap(ID3D12GraphicsCommandList* cmdList, const Camera& camera)
{
    PIXMarker marker(cmdList, L"Spot Light Shadow Map Rendering");
    CPUProfileBlock cpuProfileBlock("Spot Light Shadow Map Rendering");
    ProfileBlock profileBlock(cmdList, "Spot Light Shadow Map Rendering");

    const Array<ModelSpotLight>& spotLights = model->SpotLights();
    const uint64 numSpotLights = Min<uint64>(spotLights.Size(), AppSettings::MaxLightClamp);
    for(uint64 i = 0; i < numSpotLights; ++i)
    {
        PIXMarker lightMarker(cmdList, MakeString(L"Rendering Spot Light Shadow %u", i).c_str());

        // Set the viewport
        DX12::SetViewport(cmdList, SpotLightShadowMapSize, SpotLightShadowMapSize);

        // Set the shadow map as the depth target
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = spotLightShadowMap.ArrayDSVs[i];
        cmdList->OMSetRenderTargets(0, nullptr, false, &dsv);
        cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

        const ModelSpotLight& light = spotLights[i];

        // Draw the mesh with depth only, using the new shadow camera
        PerspectiveCamera shadowCamera;
        shadowCamera.Initialize(1.0f, light.AngularAttenuation.y, AppSettings::SpotShadowNearClip, AppSettings::SpotLightRange);
        shadowCamera.SetPosition(light.Position);
        shadowCamera.SetOrientation(light.Orientation);
        RenderSpotLightShadowDepth(cmdList, shadowCamera);

        Float4x4 shadowMatrix = shadowCamera.ViewProjectionMatrix() * ShadowHelper::ShadowScaleOffsetMatrix;
        spotLightShadowMatrices[i] = Float4x4::Transpose(shadowMatrix);
    }
}