//=================================================================================================
//
//  MJP's DX12 Sample Framework
//  http://mynameismjp.wordpress.com/
//
//  All code licensed under the MIT license
//
//=================================================================================================

#pragma once

#include "..\\PCH.h"
#include "DX12.h"

namespace SampleFramework12
{

// Forward declarations
struct DescriptorHeap;
struct DescriptorHandle;
struct LinearDescriptorHeap;

enum class BlendState : uint64
{
    Disabled = 0,
    Additive,
    AlphaBlend,
    PreMultiplied,
    NoColorWrites,
    PreMultipliedRGB,

    NumValues
};

enum class RasterizerState : uint64
{
    NoCull = 0,
    BackFaceCull,
    BackFaceCullNoZClip,
    FrontFaceCull,
    NoCullNoMS,
    Wireframe,

    NumValues
};

enum class DepthState : uint64
{
    Disabled = 0,
    Enabled,
    Reversed,
    WritesEnabled,
    ReversedWritesEnabled,

    NumValues
};

enum class SamplerState : uint64
{
    Linear = 0,
    LinearClamp,
    LinearBorder,
    Point,
    Anisotropic,
    ShadowMap,
    ShadowMapPCF,
    ReversedShadowMap,
    ReversedShadowMapPCF,

    NumValues
};

enum class CmdListMode : uint64
{
    Graphics = 0,
    Compute,
    Copy,
};

enum class ShaderResourceType : uint64
{
    SRV_UAV_CBV = 0,
    Sampler,
};

namespace DX12
{

// Constants
const uint64 ConstantBufferAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
const uint64 VertexBufferAlignment = 4;
const uint64 IndexBufferAlignment = 4;
const uint32 StandardMSAAPattern = 0xFFFFFFFF;

// Externals
extern uint32 RTVDescriptorSize;
extern uint32 SRVDescriptorSize;
extern uint32 UAVDescriptorSize;
extern uint32 CBVDescriptorSize;
extern uint32 DSVDescriptorSize;
extern uint32 SamplerDescriptorSize;

extern DescriptorHeap RTVDescriptorHeap;
extern DescriptorHeap SRVDescriptorHeap;
extern DescriptorHeap DSVDescriptorHeap;
extern DescriptorHeap SamplerDescriptorHeap;

extern LinearDescriptorHeap SRVDescriptorHeapGPU[RenderLatency];
extern LinearDescriptorHeap SamplerDescriptorHeapGPU[RenderLatency];

// Lifetime
void Initialize_Helpers();
void Shutdown_Helpers();

void EndFrame_Helpers();

// Resource Barriers
void TransitionResource(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                        D3D12_RESOURCE_STATES after, uint32 subResource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);

// Resource management
uint64 GetResourceSize(const D3D12_RESOURCE_DESC& desc, uint32 firstSubResource = 0, uint32 numSubResources = 1);
uint64 GetResourceSize(ID3D12Resource* resource, uint32 firstSubResource = 0, uint32 numSubResources = 1);

// Heap helpers
const D3D12_HEAP_PROPERTIES* GetDefaultHeapProps();
const D3D12_HEAP_PROPERTIES* GetUploadHeapProps();
const D3D12_HEAP_PROPERTIES* GetReadbackHeapProps();

// Render states
D3D12_BLEND_DESC GetBlendState(BlendState blendState);
D3D12_RASTERIZER_DESC GetRasterizerState(RasterizerState rasterizerState);
D3D12_DEPTH_STENCIL_DESC GetDepthState(DepthState depthState);
D3D12_SAMPLER_DESC GetSamplerState(SamplerState samplerState);
D3D12_STATIC_SAMPLER_DESC GetStaticSamplerState(SamplerState samplerState, uint32 shaderRegister = 0, uint32 registerSpace = 0,
                                                D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_PIXEL);
D3D12_STATIC_SAMPLER_DESC ConvertToStaticSampler(const D3D12_SAMPLER_DESC& samplerDesc, uint32 shaderRegister = 0, uint32 registerSpace = 0,
                                                 D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_PIXEL);

// Convenience functions
void SetViewport(ID3D12GraphicsCommandList* cmdList, uint64 width, uint64 height, float zMin = 0.0f, float zMax = 1.0f);
void CreateRootSignature(ID3D12RootSignature** rootSignature, const D3D12_ROOT_SIGNATURE_DESC& desc);
uint32 DispatchSize(uint64 numElements, uint64 groupSize);

// Resource binding
void SetDescriptorHeaps(ID3D12GraphicsCommandList* cmdList);
DescriptorHandle MakeDescriptorTable(uint64 count, const D3D12_CPU_DESCRIPTOR_HANDLE* handles,
                                     ShaderResourceType srType = ShaderResourceType::SRV_UAV_CBV);
void BindShaderResources(ID3D12GraphicsCommandList* cmdList, uint32 rootParameter, uint64 count,
                         const D3D12_CPU_DESCRIPTOR_HANDLE* handles, CmdListMode cmdListMode = CmdListMode::Graphics,
                         ShaderResourceType srType = ShaderResourceType::SRV_UAV_CBV);
void BindShaderResources(ID3D12GraphicsCommandList* cmdList, uint32 rootParameter, uint64 count,
                         const DescriptorHandle* handles, CmdListMode cmdListMode = CmdListMode::Graphics,
                         ShaderResourceType srType = ShaderResourceType::SRV_UAV_CBV);

} // namespace DX12

} // namespace SampleFramework12
