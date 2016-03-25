//=================================================================================================
//
//  MJP's DX12 Sample Framework
//  http://mynameismjp.wordpress.com/
//
//  All code licensed under the MIT license
//
//=================================================================================================

#include "PCH.h"

#include "DX12_Upload.h"
#include "DX12.h"
#include "GraphicsTypes.h"
#include "ShaderCompilation.h"

namespace SampleFramework12
{

namespace DX12
{

static ID3D12GraphicsCommandList* convertCmdList = nullptr;
static ID3D12CommandQueue* convertCmdQueue = nullptr;
static ID3D12CommandAllocator* convertCmdAllocator = nullptr;
static ID3D12RootSignature* convertRootSignature = nullptr;
static ID3D12PipelineState* convertPSO = nullptr;
static ID3D12PipelineState* convertArrayPSO = nullptr;
static CompiledShaderPtr convertCS;
static CompiledShaderPtr convertArrayCS;
static uint32 convertTGSize = 16;
static Fence convertFence;

static ID3D12GraphicsCommandList* readbackCmdList = nullptr;
static ID3D12CommandAllocator* readbackCmdAllocator = nullptr;
static Fence readbackFence;

static const uint64 UploadBufferSize = 16 * 1024 * 1024;
static const uint64 MaxUploadSubmissions = 16;
static ID3D12GraphicsCommandList* UploadCmdList = nullptr;
static ID3D12CommandQueue* UploadCmdQueue = nullptr;
static ID3D12Resource* UploadBuffer = nullptr;
static uint8* UploadBufferCPUAddr = nullptr;
static uint64 UploadBufferStart = 0;
static uint64 UploadBufferUsed = 0;
static Fence UploadFence;
static uint64 UploadFenceValue = 0;

struct UploadSubmission
{
    ID3D12CommandAllocator* CmdAllocator = nullptr;
    uint64 Offset = 0;
    uint64 Size = 0;
    uint64 FenceValue = uint64(-1);
    uint64 Padding = 0;

    void Reset()
    {
        Offset = 0;
        Size = 0;
        FenceValue = uint64(-1);
        Padding = 0;
    }
};

static UploadSubmission UploadSubmissions[MaxUploadSubmissions];
static uint64 UploadSubmissionStart = 0;
static uint64 UploadSubmissionUsed = 0;

static const uint64 TempBufferSize = 2 * 1024 * 1024;
static ID3D12Resource* TempFrameBuffers[RenderLatency] = { };
static uint8* TempFrameCPUMem[RenderLatency] = { };
static uint64 TempFrameGPUMem[RenderLatency] = { };
static uint64 TempFrameUsed = 0;

static void ClearFinishedUploads(uint64 flushCount)
{
    const uint64 start = UploadSubmissionStart;
    const uint64 used = UploadSubmissionUsed;
    for(uint64 i = 0; i < used; ++i)
    {
        const uint64 idx = (start + i) % MaxUploadSubmissions;
        UploadSubmission& submission = UploadSubmissions[idx];
        Assert_(submission.Size > 0);
        Assert_(submission.FenceValue != uint64(-1));
        Assert_(UploadBufferUsed >= submission.Size);

        if(i < flushCount)
            UploadFence.Wait(submission.FenceValue);

        if(UploadFence.Signaled(submission.FenceValue))
        {
            UploadSubmissionStart = (UploadSubmissionStart + 1) % MaxUploadSubmissions;
            UploadSubmissionUsed -= 1;
            UploadBufferStart = (UploadBufferStart + submission.Padding) % UploadBufferSize;
            Assert_(submission.Offset == UploadBufferStart);
            Assert_(UploadBufferStart + submission.Size <= UploadBufferSize);
            UploadBufferStart = (UploadBufferStart + submission.Size) % UploadBufferSize;
            UploadBufferUsed -= (submission.Size + submission.Padding);
            submission.Reset();

            if(UploadBufferUsed == 0)
                UploadBufferStart = 0;
        }
    }
}

static bool AllocUploadSubmission(uint64 size)
{
    Assert_(UploadSubmissionUsed <= MaxUploadSubmissions);
    if(UploadSubmissionUsed == MaxUploadSubmissions)
        return false;

    const uint64 submissionIdx = (UploadSubmissionStart + UploadSubmissionUsed) % MaxUploadSubmissions;
    Assert_(UploadSubmissions[submissionIdx].Size == 0);

    Assert_(UploadBufferUsed <= UploadBufferSize);
    if(size > (UploadBufferSize - UploadBufferUsed))
        return false;

    const uint64 start = UploadBufferStart;
    const uint64 end = UploadBufferStart + UploadBufferUsed;
    uint64 allocOffset = uint64(-1);
    uint64 padding = 0;
    if(end < UploadBufferSize)
    {
        const uint64 endAmt = UploadBufferSize - end;
        if(endAmt >= size)
        {
            allocOffset = end;
        }
        else if(start >= size)
        {
            // Wrap around to the beginning
            allocOffset = 0;
            UploadBufferUsed += endAmt;
            padding = endAmt;
        }
    }
    else
    {
        const uint64 wrappedEnd = end % UploadBufferSize;
        if((start - wrappedEnd) >= size)
            allocOffset = wrappedEnd;
    }

    if(allocOffset == uint64(-1))
        return false;

    UploadSubmissionUsed += 1;
    UploadBufferUsed += size;

    ++UploadFenceValue;

    UploadSubmissions[submissionIdx].Offset = allocOffset;
    UploadSubmissions[submissionIdx].Size = size;
    UploadSubmissions[submissionIdx].FenceValue = UploadFenceValue;
    UploadSubmissions[submissionIdx].Padding = padding;

    return true;
}

void Initialize_Upload()
{
    for(uint64 i = 0; i < MaxUploadSubmissions; ++i)
        DXCall(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&UploadSubmissions[i].CmdAllocator)));

    DXCall(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, UploadSubmissions[0].CmdAllocator, nullptr, IID_PPV_ARGS(&UploadCmdList)));
    DXCall(UploadCmdList->Close());

    D3D12_COMMAND_QUEUE_DESC queueDesc = { };
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    DXCall(Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&UploadCmdQueue)));

    UploadFence.Init(0);

    D3D12_RESOURCE_DESC resourceDesc = { };
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = uint32(UploadBufferSize);
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Alignment = 0;

    DXCall(Device->CreateCommittedResource(DX12::GetUploadHeapProps(), D3D12_HEAP_FLAG_NONE, &resourceDesc,
                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&UploadBuffer)));

    D3D12_RANGE readRange = { };
    DXCall(UploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&UploadBufferCPUAddr)));

    // Temporary buffer memory that swaps every frame
    resourceDesc.Width = uint32(TempBufferSize);

    for(uint64 i = 0; i < RenderLatency; ++i)
    {
        DXCall(Device->CreateCommittedResource(DX12::GetUploadHeapProps(), D3D12_HEAP_FLAG_NONE, &resourceDesc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&TempFrameBuffers[i])));

        DXCall(TempFrameBuffers[i]->Map(0, &readRange, reinterpret_cast<void**>(&TempFrameCPUMem[i])));
        TempFrameGPUMem[i] = TempFrameBuffers[i]->GetGPUVirtualAddress();
    }

    // Texture conversion resources
    DXCall(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&convertCmdAllocator)));
    DXCall(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, convertCmdAllocator, nullptr, IID_PPV_ARGS(&convertCmdList)));
    DXCall(convertCmdList->Close());
    DXCall(convertCmdList->Reset(convertCmdAllocator, nullptr));

    D3D12_COMMAND_QUEUE_DESC convertQueueDesc = {};
    convertQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    convertQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    DXCall(Device->CreateCommandQueue(&convertQueueDesc, IID_PPV_ARGS(&convertCmdQueue)));

    CompileOptions opts;
    opts.Add("TGSize_", convertTGSize);
    const std::wstring shaderPath = SampleFrameworkDir() + L"Shaders\\DecodeTextureCS.hlsl";
    convertCS = CompileFromFile(shaderPath.c_str(), "DecodeTextureCS", ShaderType::Compute, ShaderProfile::SM51, opts);
    convertArrayCS = CompileFromFile(shaderPath.c_str(), "DecodeTextureArrayCS", ShaderType::Compute, ShaderProfile::SM51, opts);

    {
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].RegisterSpace = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].RegisterSpace = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rootParameters[1] = {};
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[0].DescriptorTable.pDescriptorRanges = ranges;
        rootParameters[0].DescriptorTable.NumDescriptorRanges = ArraySize_(ranges);

        D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        DX12::CreateRootSignature(&convertRootSignature, rootSignatureDesc);
    }

    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = { };
        psoDesc.CS = convertCS.ByteCode();
        psoDesc.pRootSignature = convertRootSignature;
        psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&convertPSO));

        psoDesc.CS = convertArrayCS.ByteCode();
        Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&convertArrayPSO));
    }

    convertFence.Init(0);

    // Readback resources
    DXCall(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&readbackCmdAllocator)));
    DXCall(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, readbackCmdAllocator, nullptr, IID_PPV_ARGS(&readbackCmdList)));
    DXCall(readbackCmdList->Close());
    DXCall(readbackCmdList->Reset(readbackCmdAllocator, nullptr));

    readbackFence.Init(0);
}

void Shutdown_Upload()
{
    for(uint64 i = 0; i < ArraySize_(TempFrameBuffers); ++i)
        Release(TempFrameBuffers[i]);

    Release(UploadCmdList);
    Release(UploadBuffer);
    Release(UploadCmdQueue);
    UploadFence.Shutdown();
    for(uint64 i = 0; i < MaxUploadSubmissions; ++i)
        Release(UploadSubmissions[i].CmdAllocator);

    Release(convertCmdAllocator);
    Release(convertCmdList);
    Release(convertCmdQueue);
    Release(convertPSO);
    Release(convertArrayPSO);
    Release(convertRootSignature);
    convertFence.Shutdown();

    Release(readbackCmdAllocator);
    Release(readbackCmdList);
    readbackFence.Shutdown();
}

void EndFrame_Upload()
{
    // Make sure to sync on any pending uploads
    ClearFinishedUploads(0);
    GfxQueue->Wait(UploadFence.D3DFence, UploadFenceValue);

    TempFrameUsed = 0;
}

UploadContext ResourceUploadBegin(uint64 size)
{
    Assert_(Device != nullptr);

    size = AlignTo(size, 512);
    Assert_(size <= UploadBufferSize);
    Assert_(size > 0);

    ClearFinishedUploads(0);
    while(AllocUploadSubmission(size) == false)
        ClearFinishedUploads(1);

    Assert_(UploadSubmissionUsed > 0);
    const uint64 submissionIdx = (UploadSubmissionStart + (UploadSubmissionUsed - 1)) % MaxUploadSubmissions;
    UploadSubmission& submission = UploadSubmissions[submissionIdx];
    Assert_(submission.Size == size);

    DXCall(submission.CmdAllocator->Reset());
    DXCall(UploadCmdList->Reset(submission.CmdAllocator, nullptr));

    UploadContext context;
    context.CmdList = UploadCmdList;
    context.Resource = UploadBuffer;
    context.CPUAddress = UploadBufferCPUAddr + submission.Offset;
    context.ResourceOffset = submission.Offset;

    return context;
}

void ResourceUploadEnd(UploadContext& context)
{
    Assert_(context.CmdList != nullptr);

    // Finish off and execute the command list
    DXCall(UploadCmdList->Close());
    ID3D12CommandList* cmdLists[1] = { UploadCmdList };
    UploadCmdQueue->ExecuteCommandLists(1, cmdLists);

    Assert_(UploadSubmissionUsed > 0);
    const uint64 submissionIdx = (UploadSubmissionStart + (UploadSubmissionUsed - 1)) % MaxUploadSubmissions;
    UploadSubmission& submission = UploadSubmissions[submissionIdx];
    Assert_(submission.Size != 0);
    UploadFence.Signal(UploadCmdQueue, submission.FenceValue);

    context = UploadContext();
}

MapResult AcquireTempBufferMem(uint64 size, uint64 alignment)
{
    TempFrameUsed = AlignTo(TempFrameUsed, alignment);
    Assert_(TempFrameUsed + size <= TempBufferSize);

    MapResult result;
    result.CPUAddress = TempFrameCPUMem[CurrFrameIdx] + TempFrameUsed;
    result.GPUAddress = TempFrameGPUMem[CurrFrameIdx] + TempFrameUsed;
    result.ResourceOffset = TempFrameUsed;
    result.Resource = TempFrameBuffers[CurrFrameIdx];

    TempFrameUsed += size;

    return result;
}

void ConvertAndReadbackTexture(const Texture& texture, DXGI_FORMAT outputFormat, ReadbackBuffer& readbackBuffer)
{
    Assert_(convertCmdList != nullptr);
    Assert_(texture.Valid());
    Assert_(texture.Depth == 1);

    // Create a buffer for the CS to write flattened, converted texture data into
    FormattedBufferInit init;
    init.Format = outputFormat;
    init.NumElements = texture.Width * texture.Height * texture.ArraySize;
    init.CreateUAV = true;

    FormattedBuffer convertBuffer;
    convertBuffer.Initialize(init);

    convertBuffer.Transition(convertCmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Run the conversion compute shader
    DX12::SetDescriptorHeaps(convertCmdList);
    convertCmdList->SetComputeRootSignature(convertRootSignature);
    convertCmdList->SetPipelineState(texture.ArraySize > 1 ? convertArrayPSO : convertPSO);

    D3D12_CPU_DESCRIPTOR_HANDLE descriptors[2] = { texture.SRV.CPUHandle, convertBuffer.UAV() };
    BindShaderResources(convertCmdList, 0, ArraySize_(descriptors), descriptors, CmdListMode::Compute, ShaderResourceType::SRV_UAV_CBV);

    uint32 dispatchX = DispatchSize(convertTGSize, texture.Width);
    uint32 dispatchY = DispatchSize(convertTGSize, texture.Height);
    uint32 dispatchZ = texture.ArraySize;
    convertCmdList->Dispatch(dispatchX, dispatchY, dispatchZ);

    convertBuffer.Transition(convertCmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Execute the conversion command list and signal a fence
    DXCall(convertCmdList->Close());
    ID3D12CommandList* cmdLists[1] = { convertCmdList };
    convertCmdQueue->ExecuteCommandLists(1, cmdLists);

    convertFence.Signal(convertCmdQueue, 1);

    // Have the readback wait for conversion finish, and then have it copy the data to a readback buffer
    ID3D12CommandQueue* readbackQueue = UploadCmdQueue;
    readbackQueue->Wait(convertFence.D3DFence, 1);

    readbackBuffer.Shutdown();
    readbackBuffer.Initialize(convertBuffer.InternalBuffer.Size);

    readbackCmdList->CopyResource(readbackBuffer.Resource, convertBuffer.InternalBuffer.Resource);

    // Execute the readback command list and signal a fence
    DXCall(readbackCmdList->Close());
    cmdLists[0] = readbackCmdList;
    readbackQueue->ExecuteCommandLists(1, cmdLists);

    readbackFence.Signal(readbackQueue, 1);

    readbackFence.Wait(1);

    // Clean up
    convertFence.Clear(0);
    readbackFence.Clear(0);

    DXCall(convertCmdAllocator->Reset());
    DXCall(convertCmdList->Reset(convertCmdAllocator, nullptr));

    DXCall(readbackCmdAllocator->Reset());
    DXCall(readbackCmdList->Reset(readbackCmdAllocator, nullptr));

    convertBuffer.Shutdown();
}

} // namespace DX12

} // namespace SampleFramework12