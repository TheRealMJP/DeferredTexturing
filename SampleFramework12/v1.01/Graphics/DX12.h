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
#include "..\\Containers.h"

namespace SampleFramework12
{

enum class CmdListMode : uint32
{
    Graphics = 0,
    Compute,
};


struct CmdQueueConfig
{
    CmdListMode Mode;
    const wchar* Name = nullptr;
};

struct CmdListConfig
{
    CmdListMode Mode;
    const wchar* Name = nullptr;
    const wchar* AllocatorName = nullptr;
};

struct CmdSubmissionConfig
{
    uint32 QueueIdx = uint32(-1);
    Array<uint32> CmdListIndices;
    Array<uint32> WaitFenceIndices;
    uint32 SignalFenceIdx = uint32(-1);
};

struct SubmitConfig
{
    Array<CmdQueueConfig> Queues;
    Array<CmdListConfig> CmdLists;
    Array<CmdSubmissionConfig> Submissions;
    uint32 NumFences = 0;
};

namespace DX12
{

// Constants
const uint64 RenderLatency = 2;

// Externals
extern ID3D12Device5* Device;
extern D3D_FEATURE_LEVEL FeatureLevel;
extern IDXGIFactory4* Factory;
extern IDXGIAdapter1* Adapter;

extern uint64 CurrentCPUFrame;  // Total number of CPU frames completed (completed means all command buffers submitted to the GPU)
extern uint64 CurrentGPUFrame;  // Total number of GPU frames completed (completed means that the GPU signals the fence)
extern uint64 CurrFrameIdx;     // CurrentCPUFrame % RenderLatency


// Lifetime
void Initialize(D3D_FEATURE_LEVEL minFeatureLevel, uint32 adapterIdx);
void Shutdown();

// Frame submission + synchronization
void BeginFrame();
void EndFrame(IDXGISwapChain4* swapChain, uint32 syncIntervals);
void FlushGPU();

// Submission configuration
void SetSubmitConfig(const SubmitConfig& config);

// Command list + queue access
ID3D12GraphicsCommandList4* CommandList(uint32 idx);
ID3D12GraphicsCommandList4* FirstGfxCommandList();
ID3D12GraphicsCommandList4* LastGfxCommandList();

ID3D12CommandQueue* CommandQueue(uint32 idx);
ID3D12CommandQueue* LastGfxCommandQueue();

// Resource lifetime
void DeferredRelease_(IUnknown* resource);

template<typename T> void DeferredRelease(T*& resource)
{
    IUnknown* base = resource;
    DeferredRelease_(base);
    resource = nullptr;
}

template<typename T> void Release(T*& resource)
{
    if(resource != nullptr) {
        resource->Release();
        resource = nullptr;
    }
}

void DeferredCreateSRV(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, uint32 descriptorIdx);

} // namespace DX12

} // namespace SampleFramework12


