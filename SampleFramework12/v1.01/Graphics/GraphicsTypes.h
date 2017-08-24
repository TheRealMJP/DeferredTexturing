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

struct PersistentDescriptorAlloc
{
    D3D12_CPU_DESCRIPTOR_HANDLE Handles[DX12::RenderLatency] = { };
    uint32 Index = uint32(-1);
};

struct TempDescriptorAlloc
{
    D3D12_CPU_DESCRIPTOR_HANDLE StartCPUHandle = { };
    D3D12_GPU_DESCRIPTOR_HANDLE StartGPUHandle = { };
    uint32 StartIndex = uint32(-1);
};

// Wrapper for D3D12 descriptor heaps that supports persistent and temporary allocations
struct DescriptorHeap
{
    ID3D12DescriptorHeap* Heaps[DX12::RenderLatency] = { };
    uint32 NumPersistent = 0;
    uint32 PersistentAllocated = 0;
    Array<uint32> DeadList;
    uint32 NumTemporary = 0;
    volatile int64 TemporaryAllocated = 0;
    uint32 HeapIndex = 0;
    uint32 NumHeaps = 0;
    uint32 DescriptorSize = 0;
    bool32 ShaderVisible = false;
    D3D12_DESCRIPTOR_HEAP_TYPE HeapType = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    D3D12_CPU_DESCRIPTOR_HANDLE CPUStart[DX12::RenderLatency] = { };
    D3D12_GPU_DESCRIPTOR_HANDLE GPUStart[DX12::RenderLatency] = { };
    SRWLOCK Lock = SRWLOCK_INIT;

    ~DescriptorHeap();

    void Init(uint32 numPersistent, uint32 numTemporary, D3D12_DESCRIPTOR_HEAP_TYPE heapType, bool shaderVisible);
    void Shutdown();

    PersistentDescriptorAlloc AllocatePersistent();
    void FreePersistent(uint32& idx);
    void FreePersistent(D3D12_CPU_DESCRIPTOR_HANDLE& handle);
    void FreePersistent(D3D12_GPU_DESCRIPTOR_HANDLE& handle);

    TempDescriptorAlloc AllocateTemporary(uint32 count);
    void EndFrame();

    D3D12_CPU_DESCRIPTOR_HANDLE CPUHandleFromIndex(uint32 descriptorIdx) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GPUHandleFromIndex(uint32 descriptorIdx) const;

    uint32 IndexFromHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle);
    uint32 IndexFromHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle);

    ID3D12DescriptorHeap* CurrentHeap() const;
    uint32 TotalNumDescriptors() const { return NumPersistent + NumTemporary; }
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
    bool32 GPUWritable = false;
    ID3D12Heap* Heap = nullptr;
    uint64 HeapOffset = 0;
    uint64 UploadFrame = uint64(-1);

    Buffer();
    ~Buffer();

    void Initialize(uint64 size, uint64 alignment, bool32 dynamic, bool32 gpuWritable,
                    bool32 allowUAV, const void* initData, D3D12_RESOURCE_STATES initialState,
                    ID3D12Heap* heap, uint64 heapOffset, const wchar* name);
    void Shutdown();

    MapResult Map();
    MapResult MapAndSetData(const void* data, uint64 dataSize);
    template<typename T> MapResult MapAndSetData(const T& data) { return MapAndSetData(&data, sizeof(T)); }
    uint64 UpdateData(const void* srcData, uint64 srcSize, uint64 dstOffset);
    uint64 MultiUpdateData(const void* srcData[], uint64 srcSize[], uint64 dstOffset[], uint64 numUpdates);

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

struct ConstantBufferInit
{
    uint64 Size = 0;
    bool32 Dynamic = true;
    bool32 GPUWritable = false;
    const void* InitData = nullptr;
    D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    ID3D12Heap* Heap = nullptr;
    uint64 HeapOffset = 0;
    const wchar* Name = nullptr;
};

struct ConstantBuffer
{
    Buffer InternalBuffer;
    uint64 CurrentGPUAddress = 0;

    ConstantBuffer();
    ~ConstantBuffer();

    void Initialize(const ConstantBufferInit& init);
    void Shutdown();

    void SetAsGfxRootParameter(ID3D12GraphicsCommandList* cmdList, uint32 rootParameter) const;
    void SetAsComputeRootParameter(ID3D12GraphicsCommandList* cmdList, uint32 rootParameter) const;

    void* Map();
    template<typename T> T* Map() { return reinterpret_cast<T*>(Map()); }
    void MapAndSetData(const void* data, uint64 dataSize);
    template<typename T> void MapAndSetData(const T& data) { MapAndSetData(&data, sizeof(T)); }
    void UpdateData(const void* srcData, uint64 srcSize, uint64 dstOffset);
    void MultiUpdateData(const void* srcData[], uint64 srcSize[], uint64 dstOffset[], uint64 numUpdates);
};

struct StructuredBufferInit
{
    uint64 Stride = 0;
    uint64 NumElements = 0;
    bool32 CreateUAV = false;
    bool32 UseCounter = false;
    bool32 Dynamic = false;
    bool32 GPUWritable = true;
    const void* InitData = nullptr;
    D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_GENERIC_READ;
    ID3D12Heap* Heap = nullptr;
    uint64 HeapOffset = 0;
    const wchar* Name = nullptr;
};

struct StructuredBuffer
{
    Buffer InternalBuffer;
    uint64 Stride = 0;
    uint64 NumElements = 0;
    uint32 SRV = uint32(-1);
    D3D12_CPU_DESCRIPTOR_HANDLE UAV = { };
    ID3D12Resource* CounterResource = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE CounterUAV = { };
    uint64 GPUAddress = 0;

    StructuredBuffer();
    ~StructuredBuffer();

    void Initialize(const StructuredBufferInit& init);
    void Shutdown();

    D3D12_VERTEX_BUFFER_VIEW VBView() const;
    ID3D12Resource* Resource() const { return InternalBuffer.Resource; }

    void* Map();
    template<typename T> T* Map() { return reinterpret_cast<T*>(Map()); }
    void MapAndSetData(const void* data, uint64 numElements);
    void UpdateData(const void* srcData, uint64 srcNumElements, uint64 dstElemOffset);
    void MultiUpdateData(const void* srcData[], uint64 srcNumElements[], uint64 dstElemOffset[], uint64 numUpdates);

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
    bool32 GPUWritable = true;
    const void* InitData = nullptr;
    D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_GENERIC_READ;
    ID3D12Heap* Heap = nullptr;
    uint64 HeapOffset = 0;
    const wchar* Name = nullptr;
};

struct FormattedBuffer
{
    Buffer InternalBuffer;
    uint64 Stride = 0;
    uint64 NumElements = 0;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    uint32 SRV = uint32(-1);
    D3D12_CPU_DESCRIPTOR_HANDLE UAV = { };
    uint64 GPUAddress = 0;

    FormattedBuffer();
    ~FormattedBuffer();

    void Initialize(const FormattedBufferInit& init);
    void Shutdown();

    D3D12_INDEX_BUFFER_VIEW IBView() const;
    ID3D12Resource* Resource() const { return InternalBuffer.Resource; }

    void* Map();
    template<typename T> T* Map() { return reinterpret_cast<T*>(Map()); };
    void MapAndSetData(const void* data, uint64 numElements);
    void UpdateData(const void* srcData, uint64 srcNumElements, uint64 dstElemOffset);
    void MultiUpdateData(const void* srcData[], uint64 srcNumElements[], uint64 dstElemOffset[], uint64 numUpdates);

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
    bool32 GPUWritable = true;
    const void* InitData = nullptr;
    D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_GENERIC_READ;
    ID3D12Heap* Heap = nullptr;
    uint64 HeapOffset = 0;
    const wchar* Name = nullptr;
};

struct RawBuffer
{
    Buffer InternalBuffer;
    uint64 NumElements = 0;
    uint32 SRV = uint32(-1);
    D3D12_CPU_DESCRIPTOR_HANDLE UAV = { };
    uint64 GPUAddress = 0;

    const uint64 Stride = 4;

    RawBuffer();
    ~RawBuffer();

    void Initialize(const RawBufferInit& init);
    void Shutdown();

    ID3D12Resource* Resource() const { return InternalBuffer.Resource; }

    void* Map();
    template<typename T> T* Map() { return reinterpret_cast<T*>(Map()); };
    void MapAndSetData(const void* data, uint64 numElements);
    void UpdateData(const void* srcData, uint64 srcNumElements, uint64 dstElemOffset);
    void MultiUpdateData(const void* srcData[], uint64 srcNumElements[], uint64 dstElemOffset[], uint64 numUpdates);

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
    uint32 SRV = uint32(-1);
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
    D3D12_CPU_DESCRIPTOR_HANDLE RTV = { };
    D3D12_CPU_DESCRIPTOR_HANDLE UAV = { };
    Array<D3D12_CPU_DESCRIPTOR_HANDLE> ArrayRTVs;
    uint32 MSAASamples = 0;
    uint32 MSAAQuality = 0;

    RenderTexture();
    ~RenderTexture();

    void Initialize(uint64 width, uint64 height, DXGI_FORMAT format, uint64 msaaSamples = 1, uint64 arraySize = 1,
                    bool32 createUAV = false, D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    void Shutdown();

    void Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, uint64 mipLevel = uint64(-1), uint64 arraySlice = uint64(-1)) const;
    void MakeReadable(ID3D12GraphicsCommandList* cmdList, uint64 mipLevel = uint64(-1), uint64 arraySlice = uint64(-1)) const;
    void MakeWritable(ID3D12GraphicsCommandList* cmdList, uint64 mipLevel = uint64(-1), uint64 arraySlice = uint64(-1)) const;

    uint32 SRV() const { return Texture.SRV; }
    uint64 Width() const { return Texture.Width; }
    uint64 Height() const { return Texture.Height; }
    DXGI_FORMAT Format() const { return Texture.Format; }
    ID3D12Resource* Resource() const { return Texture.Resource; }
    uint64 SubResourceIndex(uint64 mipLevel, uint64 arraySlice) const { return arraySlice * Texture.NumMips + mipLevel; }

private:

    RenderTexture(const RenderTexture& other) { }
};

struct VolumeTexture
{
    Texture Texture;
    D3D12_CPU_DESCRIPTOR_HANDLE UAV = { };

    VolumeTexture();
    ~VolumeTexture();

    void Initialize(uint64 width, uint64 height, uint64 depth, DXGI_FORMAT format, D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_GENERIC_READ);
    void Shutdown();

    void Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) const;
    void MakeReadable(ID3D12GraphicsCommandList* cmdList) const;
    void MakeWritable(ID3D12GraphicsCommandList* cmdList) const;

    uint32 SRV() const { return Texture.SRV; }
    uint64 Width() const { return Texture.Width; }
    uint64 Height() const { return Texture.Height; }
    uint64 Depth() const { return Texture.Depth; }
    DXGI_FORMAT Format() const { return Texture.Format; }
    ID3D12Resource* Resource() const { return Texture.Resource; }

private:

    VolumeTexture(const RenderTexture& other) { }
};

struct DepthBuffer
{
    Texture Texture;
    D3D12_CPU_DESCRIPTOR_HANDLE DSV = { };
    D3D12_CPU_DESCRIPTOR_HANDLE ReadOnlyDSV = { };
    Array<D3D12_CPU_DESCRIPTOR_HANDLE> ArrayDSVs;
    uint32 MSAASamples = 0;
    uint32 MSAAQuality = 0;
    DXGI_FORMAT DSVFormat = DXGI_FORMAT_UNKNOWN;

    DepthBuffer();
    ~DepthBuffer();

    void Initialize(uint64 width, uint64 height, DXGI_FORMAT format, uint64 msaaSamples = 1, uint64 arraySize = 1,
                    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE);
    void Shutdown();

    void Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, uint64 arraySlice = uint64(-1)) const;
    void MakeReadable(ID3D12GraphicsCommandList* cmdList, uint64 arraySlice = uint64(-1)) const;
    void MakeWritable(ID3D12GraphicsCommandList* cmdList, uint64 arraySlice = uint64(-1)) const;

    uint32 SRV() const { return Texture.SRV; }
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