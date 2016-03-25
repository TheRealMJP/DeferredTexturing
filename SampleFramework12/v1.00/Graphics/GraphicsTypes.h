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

#include "..\\InterfacePointers.h"
#include "..\\Utility.h"
#include "..\\Containers.h"
#include "DX12.h"
#include "DX12_Upload.h"
#include "DX12_Helpers.h"

namespace SampleFramework12
{

struct DescriptorHandle
{
    D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle = { };
    D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle = { };

    #if UseAsserts_
        const void* ParentHeap = nullptr;
    #endif

    bool IsValid() const
    {
        return CPUHandle.ptr != 0;
    }

    bool ShaderVisible() const
    {
        return GPUHandle.ptr != 0;
    }
};

// Wrapper for D3D12 a descriptor heap
struct DescriptorHeap
{
    ID3D12DescriptorHeap* Heap = nullptr;
    uint64 NumDescriptors = 0;
    uint64 Allocated = 0;
    Array<uint32> DeadList;
    uint32 DescriptorSize = 0;
    bool32 ShaderVisible = false;
    D3D12_DESCRIPTOR_HEAP_TYPE HeapType = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    D3D12_CPU_DESCRIPTOR_HANDLE CPUStart = { };
    D3D12_GPU_DESCRIPTOR_HANDLE GPUStart = { };

    ~DescriptorHeap();

    void Init(ID3D12Device* device, uint64 numDescriptors, D3D12_DESCRIPTOR_HEAP_TYPE heapType, bool shaderVisible);
    void Shutdown();

    DescriptorHandle Allocate();
    void Free(DescriptorHandle& handle);
};

struct LinearDescriptorHeap
{
    ID3D12DescriptorHeap* Heap = nullptr;
    uint64 NumDescriptors = 0;
    uint64 Allocated = 0;
    uint32 DescriptorSize = 0;
    bool32 ShaderVisible = false;
    D3D12_DESCRIPTOR_HEAP_TYPE HeapType = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    D3D12_CPU_DESCRIPTOR_HANDLE CPUStart = {};
    D3D12_GPU_DESCRIPTOR_HANDLE GPUStart = {};

    ~LinearDescriptorHeap();

    void Init(ID3D12Device* device, uint64 numDescriptors, D3D12_DESCRIPTOR_HEAP_TYPE heapType, bool shaderVisible);
    void Shutdown();

    DescriptorHandle Allocate(uint64 count);
    void Reset();
};

enum class BufferLifetime
{
    Persistent = 0,
    Temporary
};

struct Buffer
{
    ID3D12Resource* Resource = nullptr;
    uint64 CurrBuffer = 0;
    uint8* CPUAddress = 0;
    uint64 GPUAddress = 0;
    uint64 Alignment = 0;
    uint64 Size = 0;
    bool32 Dynamic = false;
    BufferLifetime Lifetime = BufferLifetime::Persistent;

    #if UseAsserts_
        uint64 UploadFrame = uint64(-1);
    #endif

    Buffer();
    ~Buffer();

    void Initialize(uint64 size, uint64 alignment, bool32 dynamic, BufferLifetime lifetime, bool32 allowUAV, const void* initData, D3D12_RESOURCE_STATES initialState);
    void Shutdown();

    MapResult Map();
    MapResult MapAndSetData(const void* data, uint64 dataSize);
    template<typename T> MapResult MapAndSetData(const T& data) { return MapAndSetData(&data, sizeof(T)); }
    void UpdateData(const void* srcData, uint64 srcSize, uint64 dstOffset);

    void Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) const;
    void MakeReadable(ID3D12GraphicsCommandList* cmdList) const;
    void MakeWritable(ID3D12GraphicsCommandList* cmdList) const;
    void UAVBarrier(ID3D12GraphicsCommandList* cmdList) const;

    bool Initialized() const { return Size > 0; }

    #if UseAsserts_
        bool ReadyForBinding() const;
    #endif

private:

    Buffer(const Buffer& other) { }
};

// For aligning to float4 boundaries
#define Float4Align __declspec(align(16))
#define Float4Align_ __declspec(align(16))

template<typename T> struct ConstantBuffer
{
    T Data;
    Buffer InternalBuffer;
    uint64 CurrentGPUAddress = 0;

    ConstantBuffer()
    {
    }

    ~ConstantBuffer()
    {
        Shutdown();
    }

    void Initialize(BufferLifetime lifetime)
    {
        InternalBuffer.Initialize(sizeof(T), DX12::ConstantBufferAlignment, true, lifetime, false, nullptr, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    }

    void Shutdown()
    {
        InternalBuffer.Shutdown();
    }

    void Upload()
    {
        MapResult result = InternalBuffer.MapAndSetData<T>(Data);
        CurrentGPUAddress = result.GPUAddress;
    }

    void SetAsGfxRootParameter(ID3D12GraphicsCommandList* cmdList, uint32 rootParameter) const
    {
        Assert_(InternalBuffer.ReadyForBinding());
        cmdList->SetGraphicsRootConstantBufferView(rootParameter, CurrentGPUAddress);
    }

    void SetAsComputeRootParameter(ID3D12GraphicsCommandList* cmdList, uint32 rootParameter) const
    {
        Assert_(InternalBuffer.ReadyForBinding());
        cmdList->SetComputeRootConstantBufferView(rootParameter, CurrentGPUAddress);
    }
};

struct StructuredBufferInit
{
    uint64 Stride = 0;
    uint64 NumElements = 0;
    bool32 CreateUAV = false;
    bool32 UseCounter = false;
    bool32 Dynamic = false;
    BufferLifetime Lifetime = BufferLifetime::Persistent;
    const void* InitData = nullptr;
    D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_GENERIC_READ;
};

struct StructuredBuffer
{
    Buffer InternalBuffer;
    uint64 Stride = 0;
    uint64 NumElements = 0;
    DescriptorHandle SRVHandles[DX12::RenderLatency];
    DescriptorHandle UAVHandle;
    ID3D12Resource* CounterResource = nullptr;
    DescriptorHandle CounterUAV;
    uint64 GPUAddress = 0;

    StructuredBuffer();
    ~StructuredBuffer();

    void Initialize(const StructuredBufferInit& init);
    void Shutdown();

    D3D12_CPU_DESCRIPTOR_HANDLE SRV() const;
    D3D12_CPU_DESCRIPTOR_HANDLE UAV() const;
    D3D12_VERTEX_BUFFER_VIEW VBView() const;
    ID3D12Resource* Resource() const { return InternalBuffer.Resource; }

    void* Map();
    template<typename T> T* Map() { return reinterpret_cast<T*>(Map()); };
    void MapAndSetData(const void* data, uint64 numElements);
    void UpdateData(const void* srcData, uint64 srcNumElements, uint64 dstElemOffset);

    void Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) const;
    void MakeReadable(ID3D12GraphicsCommandList* cmdList) const;
    void MakeWritable(ID3D12GraphicsCommandList* cmdList) const;
    void UAVBarrier(ID3D12GraphicsCommandList* cmdList) const;

private:

    StructuredBuffer(const StructuredBuffer& other) { }

};

struct FormattedBufferInit
{
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    uint64 NumElements = 0;
    bool32 CreateUAV = false;
    bool32 Dynamic = false;
    BufferLifetime Lifetime = BufferLifetime::Persistent;
    const void* InitData = nullptr;
    D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_GENERIC_READ;
};

struct FormattedBuffer
{
    Buffer InternalBuffer;
    uint64 Stride = 0;
    uint64 NumElements = 0;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    DescriptorHandle SRVHandles[DX12::RenderLatency];
    DescriptorHandle UAVHandle;
    uint64 GPUAddress = 0;

    FormattedBuffer();
    ~FormattedBuffer();

    void Initialize(const FormattedBufferInit& init);
    void Shutdown();

    D3D12_CPU_DESCRIPTOR_HANDLE SRV() const;
    D3D12_CPU_DESCRIPTOR_HANDLE UAV() const;
    D3D12_INDEX_BUFFER_VIEW IBView() const;
    ID3D12Resource* Resource() const { return InternalBuffer.Resource; }

    void* Map();
    template<typename T> T* Map() { return reinterpret_cast<T*>(Map()); };
    void MapAndSetData(const void* data, uint64 numElements);
    void UpdateData(const void* srcData, uint64 srcNumElements, uint64 dstElemOffset);

    void Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) const;
    void MakeReadable(ID3D12GraphicsCommandList* cmdList) const;
    void MakeWritable(ID3D12GraphicsCommandList* cmdList) const;
    void UAVBarrier(ID3D12GraphicsCommandList* cmdList) const;

private:

    FormattedBuffer(const FormattedBuffer& other) { }

};

struct RawBufferInit
{
    uint64 NumElements = 0;
    bool32 CreateUAV = false;
    bool32 Dynamic = false;
    BufferLifetime Lifetime = BufferLifetime::Persistent;
    const void* InitData = nullptr;
    D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_GENERIC_READ;
};

struct RawBuffer
{
    Buffer InternalBuffer;
    uint64 NumElements = 0;
    DescriptorHandle SRVHandles[DX12::RenderLatency];
    DescriptorHandle UAVHandle;
    uint64 GPUAddress = 0;

    const uint64 Stride = 4;

    RawBuffer();
    ~RawBuffer();

    void Initialize(const RawBufferInit& init);
    void Shutdown();

    D3D12_CPU_DESCRIPTOR_HANDLE SRV() const;
    D3D12_CPU_DESCRIPTOR_HANDLE UAV() const;
    ID3D12Resource* Resource() const { return InternalBuffer.Resource; }

    void* Map();
    template<typename T> T* Map() { return reinterpret_cast<T*>(Map()); };
    void MapAndSetData(const void* data, uint64 numElements);
    void UpdateData(const void* srcData, uint64 srcNumElements, uint64 dstElemOffset);

    void Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) const;
    void MakeReadable(ID3D12GraphicsCommandList* cmdList) const;
    void MakeWritable(ID3D12GraphicsCommandList* cmdList) const;
    void UAVBarrier(ID3D12GraphicsCommandList* cmdList) const;

private:

    RawBuffer(const RawBuffer& other) { }

};

struct ReadbackBuffer
{
    ID3D12Resource* Resource = nullptr;
    uint64 Size = 0;

    ReadbackBuffer();
    ~ReadbackBuffer();

    void Initialize(uint64 size);
    void Shutdown();

    void* Map();
    template<typename T> T* Map() { return reinterpret_cast<T*>(Map()); };
    void Unmap();

private:

    ReadbackBuffer(const ReadbackBuffer& other) { }
};

struct Fence
{
    ID3D12Fence* D3DFence = nullptr;
    HANDLE FenceEvent = INVALID_HANDLE_VALUE;

    ~Fence();

    void Init(uint64 initialValue = 0);
    void Shutdown();

    void Signal(ID3D12CommandQueue* queue, uint64 fenceValue);
    void Wait(uint64 fenceValue);
    bool Signaled(uint64 fenceValue);
    void Clear(uint64 fenceValue);
};

struct Texture
{
    DescriptorHandle SRV;
    ID3D12Resource* Resource = nullptr;
    uint32 Width = 0;
    uint32 Height = 0;
    uint32 Depth = 0;
    uint32 NumMips = 0;
    uint32 ArraySize = 0;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    bool32 Cubemap = false;

    Texture();
    ~Texture();

    bool Valid() const
    {
        return Resource != nullptr;
    }

    void Shutdown();

private:

    Texture(const Texture& other) { }
};

struct RenderTexture
{
    Texture Texture;
    DescriptorHandle RTV;
    DescriptorHandle UAV;
    Array<DescriptorHandle> ArrayRTVs;
    uint32 MSAASamples = 0;

    RenderTexture();
    ~RenderTexture();

    void Initialize(uint64 width, uint64 height, DXGI_FORMAT format, uint64 msaaSamples = 1, uint64 arraySize = 1,
                    bool32 createUAV = false, D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    void Shutdown();

    void Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, uint64 mipLevel = uint64(-1), uint64 arraySlice = uint64(-1)) const;
    void MakeReadable(ID3D12GraphicsCommandList* cmdList, uint64 mipLevel = uint64(-1), uint64 arraySlice = uint64(-1)) const;
    void MakeWritable(ID3D12GraphicsCommandList* cmdList, uint64 mipLevel = uint64(-1), uint64 arraySlice = uint64(-1)) const;

    D3D12_CPU_DESCRIPTOR_HANDLE SRV() const { return Texture.SRV.CPUHandle; }
    uint64 Width() const { return Texture.Width; }
    uint64 Height() const { return Texture.Height; }
    DXGI_FORMAT Format() const { return Texture.Format; }
    ID3D12Resource* Resource() const { return Texture.Resource; }
    uint64 SubResourceIndex(uint64 mipLevel, uint64 arraySlice) const { return arraySlice * Texture.NumMips + mipLevel; }

private:

    RenderTexture(const RenderTexture& other) { }
};

struct DepthBuffer
{
    Texture Texture;
    DescriptorHandle DSV;
    DescriptorHandle ReadOnlyDSV;
    Array<DescriptorHandle> ArrayDSVs;
    uint32 MSAASamples = 0;
    DXGI_FORMAT DSVFormat = DXGI_FORMAT_UNKNOWN;

    DepthBuffer();
    ~DepthBuffer();

    void Initialize(uint64 width, uint64 height, DXGI_FORMAT format, uint64 msaaSamples = 1, uint64 arraySize = 1,
                    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE);
    void Shutdown();

    void Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, uint64 arraySlice = uint64(-1)) const;
    void MakeReadable(ID3D12GraphicsCommandList* cmdList, uint64 arraySlice = uint64(-1)) const;
    void MakeWritable(ID3D12GraphicsCommandList* cmdList, uint64 arraySlice = uint64(-1)) const;

    D3D12_CPU_DESCRIPTOR_HANDLE SRV() const { return Texture.SRV.CPUHandle; }
    uint64 Width() const { return Texture.Width; }
    uint64 Height() const { return Texture.Height; }
    ID3D12Resource* Resource() const { return Texture.Resource; }

private:

    DepthBuffer(const DepthBuffer& other) { }
};

struct PIXMarker
{
    ID3D12GraphicsCommandList* CmdList = nullptr;

    PIXMarker(ID3D12GraphicsCommandList* cmdList, const wchar* msg) : CmdList(cmdList)
    {
        PIXBeginEvent(cmdList, 0, msg);
    }

    PIXMarker(ID3D12GraphicsCommandList* cmdList, const char* msg) : CmdList(cmdList)
    {
        PIXBeginEvent(cmdList, 0, msg);
    }

    ~PIXMarker()
    {
        PIXEndEvent(CmdList);
    }
};

}