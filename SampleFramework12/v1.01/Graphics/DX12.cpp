//=================================================================================================
//
//  MJP's DX12 Sample Framework
//  http://mynameismjp.wordpress.com/
//
//  All code licensed under the MIT license
//
//=================================================================================================

#include "PCH.h"
#include "DX12.h"
#include "DX12_Upload.h"
#include "DX12_Helpers.h"
#include "GraphicsTypes.h"

#if Debug_
    #define UseDebugDevice_ 1
    #define BreakOnDXError_ (UseDebugDevice_ && 1)
    #define UseGPUValidation_ 0
#else
    #define UseDebugDevice_ 0
    #define BreakOnDXError_ 0
    #define UseGPUValidation_ 0
#endif

namespace SampleFramework12
{

namespace DX12
{

ID3D12Device5* Device = nullptr;
D3D_FEATURE_LEVEL FeatureLevel = D3D_FEATURE_LEVEL_11_0;
IDXGIFactory4* Factory = nullptr;
IDXGIAdapter1* Adapter = nullptr;

uint64 CurrentCPUFrame = 0;
uint64 CurrentGPUFrame = 0;
uint64 CurrFrameIdx = 0;

// Command list submission data
static const uint64 NumCmdAllocators = RenderLatency;

static Fence FrameFence;
static Array<Fence> ExtraFences;
static Array<ID3D12CommandQueue*> Queues;

struct CommandListData
{
    ID3D12GraphicsCommandList4* CmdList = nullptr;
    ID3D12CommandAllocator* CmdAllocators[NumCmdAllocators] = { };
    CmdListMode Mode = CmdListMode::Graphics;
};

struct CommandSubmission
{
    ID3D12CommandQueue* Queue = nullptr;
    Array<ID3D12CommandList*> CmdLists;
    Array<Fence*> WaitFences;
    Fence* SignalFence = nullptr;
};

static Array<CommandListData> CommandLists;
static Array<CommandSubmission> Submissions;

static ID3D12GraphicsCommandList4* FirstGfxCmdList = nullptr;
static ID3D12GraphicsCommandList4* LastGfxCmdList = nullptr;
static ID3D12CommandQueue* LastGfxQueue = nullptr;

// Deferred release
static GrowableList<IUnknown*> DeferredReleases[RenderLatency];
static bool ShuttingDown = false;

// Deferred SRV creation
struct DeferredSRVCreate
{
    ID3D12Resource* Resource = nullptr;
    D3D12_SHADER_RESOURCE_VIEW_DESC Desc = { };
    uint32 DescriptorIdx = uint32(-1);
};

static Array<DeferredSRVCreate> DeferredSRVCreates[RenderLatency];
static volatile uint64 DeferredSRVCreateCount[RenderLatency] = { };


static void ProcessDeferredReleases(uint64 frameIdx)
{
    for(uint64 i = 0; i < DeferredReleases[frameIdx].Count(); ++i)
        DeferredReleases[frameIdx][i]->Release();
    DeferredReleases[frameIdx].RemoveAll(nullptr);
}

static void ProcessDeferredSRVCreates(uint64 frameIdx)
{
    uint64 createCount = DeferredSRVCreateCount[frameIdx];
    for(uint64 i = 0; i < createCount; ++i)
    {
        DeferredSRVCreate& create = DeferredSRVCreates[frameIdx][i];
        Assert_(create.Resource != nullptr);

        D3D12_CPU_DESCRIPTOR_HANDLE handle = SRVDescriptorHeap.CPUHandleFromIndex(create.DescriptorIdx, frameIdx);
        Device->CreateShaderResourceView(create.Resource, &create.Desc, handle);

        create.Resource = nullptr;
        create.DescriptorIdx = uint32(-1);
    }

    DeferredSRVCreateCount[frameIdx] = 0;
}

static void CleanupSubmitResources()
{
    Submissions.Shutdown();

    FirstGfxCmdList = nullptr;
    LastGfxCmdList = nullptr;
    LastGfxQueue = nullptr;

    for(CommandListData& cmdList : CommandLists)
    {
        Release(cmdList.CmdList);
        for(uint32 i = 0; i < NumCmdAllocators; ++i)
            Release(cmdList.CmdAllocators[i]);
    }
    CommandLists.Shutdown();

    for(ID3D12CommandQueue* queue : Queues)
        Release(queue);
    Queues.Shutdown();

    for(Fence& fence : ExtraFences)
        fence.Shutdown();
    ExtraFences.Shutdown();
}

void Initialize(D3D_FEATURE_LEVEL minFeatureLevel, uint32 adapterIdx)
{
    ShuttingDown = false;

    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&Factory));
    if(FAILED(hr))
        throw Exception(L"Unable to create a DXGI 1.4 device.\n "
                        L"Make sure that your OS and driver support DirectX 12");

    LARGE_INTEGER umdVersion = { };
    Factory->EnumAdapters1(adapterIdx, &Adapter);

    if(Adapter == nullptr)
        throw Exception(L"Unable to locate a DXGI 1.4 adapter that supports a D3D12 device.\n"
                        L"Make sure that your OS and driver support DirectX 12");

    DXGI_ADAPTER_DESC1 desc = { };
    Adapter->GetDesc1(&desc);
    WriteLog("Creating DX12 device on adapter '%ls'", desc.Description);

    #if UseDebugDevice_
        ID3D12DebugPtr d3d12debug;
        DXCall(D3D12GetDebugInterface(IID_PPV_ARGS(&d3d12debug)));
        d3d12debug->EnableDebugLayer();

        #if UseGPUValidation_
            ID3D12Debug1Ptr debug1;
            d3d12debug->QueryInterface(IID_PPV_ARGS(&debug1));
            debug1->SetEnableGPUBasedValidation(true);
        #endif
    #endif

    #if EnableShaderModel6_ && 0
        // Enable experimental shader models
        DXCall(D3D12EnableExperimentalFeatures(1, &D3D12ExperimentalShaderModels, nullptr, nullptr));
    #endif

    DXCall(D3D12CreateDevice(Adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device)));

    // Check the maximum feature level, and make sure it's above our minimum
    D3D_FEATURE_LEVEL featureLevelsArray[4];
    featureLevelsArray[0] = D3D_FEATURE_LEVEL_11_0;
    featureLevelsArray[1] = D3D_FEATURE_LEVEL_11_1;
    featureLevelsArray[2] = D3D_FEATURE_LEVEL_12_0;
    featureLevelsArray[3] = D3D_FEATURE_LEVEL_12_1;
    D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels = { };
    featureLevels.NumFeatureLevels = ArraySize_(featureLevelsArray);
    featureLevels.pFeatureLevelsRequested = featureLevelsArray;
    DXCall(Device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featureLevels, sizeof(featureLevels)));
    FeatureLevel = featureLevels.MaxSupportedFeatureLevel;

    if(FeatureLevel < minFeatureLevel)
    {
        std::wstring majorLevel = ToString<int>(minFeatureLevel >> 12);
        std::wstring minorLevel = ToString<int>((minFeatureLevel >> 8) & 0xF);
        throw Exception(L"The device doesn't support the minimum feature level required to run this sample (DX" + majorLevel + L"." + minorLevel + L")");
    }

    #if UseDebugDevice_
        ID3D12InfoQueuePtr infoQueue;
        DXCall(Device->QueryInterface(IID_PPV_ARGS(&infoQueue)));

        D3D12_MESSAGE_ID disabledMessages[] =
        {
            D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,

            // These happen when capturing with VS diagnostics
            D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,
            D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE,
        };

        D3D12_INFO_QUEUE_FILTER filter = { };
        filter.DenyList.NumIDs = ArraySize_(disabledMessages);
        filter.DenyList.pIDList = disabledMessages;
        infoQueue->AddStorageFilterEntries(&filter);
    #endif

    #if BreakOnDXError_
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
    #endif

    CurrFrameIdx = CurrentCPUFrame % NumCmdAllocators;

    FrameFence.Init(0);

    for(uint64 i = 0; i < ArraySize_(DeferredSRVCreates); ++i)
        DeferredSRVCreates[i].Init(1024);

    Initialize_Helpers();
    Initialize_Upload();

    // Create a default submission setup with a single graphics queue submission
    SubmitConfig submitConfig;

    submitConfig.Queues.Init(1);
    CmdQueueConfig& queueConfig = submitConfig.Queues[0];
    queueConfig.Mode = CmdListMode::Graphics;
    queueConfig.Name = L"Primary Graphics Queue";

    submitConfig.CmdLists.Init(1);
    CmdListConfig& cmdListConfig = submitConfig.CmdLists[0];
    cmdListConfig.Mode = CmdListMode::Graphics;
    cmdListConfig.Name = L"Primary Graphics Command List";
    cmdListConfig.AllocatorName = L"Primary Graphics Command Allocator";

    submitConfig.Submissions.Init(1);
    CmdSubmissionConfig& submissionConfig = submitConfig.Submissions[0];
    submissionConfig.CmdListIndices.Init(1, 0);
    submissionConfig.QueueIdx = 0;

    SetSubmitConfig(submitConfig);
}

void Shutdown()
{
    Assert_(CurrentCPUFrame == CurrentGPUFrame);
    ShuttingDown = true;

    for(uint64 i = 0; i < ArraySize_(DeferredReleases); ++i)
        ProcessDeferredReleases(i);

    CleanupSubmitResources();

    FrameFence.Shutdown();

    Release(Factory);
    Release(Adapter);

    Shutdown_Helpers();
    Shutdown_Upload();

    #if BreakOnDXError_
        if(Device != nullptr)
        {
            ID3D12InfoQueuePtr infoQueue;
            DXCall(Device->QueryInterface(IID_PPV_ARGS(&infoQueue)));
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        }
    #endif

    #if UseDebugDevice_ && 0
        if(Device != nullptr)
        {
            ID3D12DebugDevicePtr debugDevice;
            DXCall(Device->QueryInterface(IID_PPV_ARGS(&debugDevice)));
            debugDevice->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL);
        }
    #endif

    Release(Device);
}

void BeginFrame()
{
    Assert_(Device);

    for(CommandListData& cmdListData : CommandLists)
        SetDescriptorHeaps(cmdListData.CmdList);
}

void EndFrame(IDXGISwapChain4* swapChain, uint32 syncIntervals)
{
    Assert_(Device);

    for(CommandListData& cmdListData : CommandLists)
        DXCall(cmdListData.CmdList->Close());

    for(ID3D12CommandQueue* queue : Queues)
        WaitOnResourceUploads(queue);

    ++CurrentCPUFrame;

    for(CommandSubmission& submission : Submissions)
    {
        for(Fence* waitFence : submission.WaitFences)
            waitFence->GPUWait(submission.Queue, CurrentCPUFrame);

        submission.Queue->ExecuteCommandLists(uint32(submission.CmdLists.Size()), submission.CmdLists.Data());

        if(submission.SignalFence != nullptr)
            submission.SignalFence->Signal(submission.Queue, CurrentCPUFrame);
    }

    // Present the frame.
    if(swapChain)
        DXCall(swapChain->Present(syncIntervals, syncIntervals == 0 ? DXGI_PRESENT_ALLOW_TEARING : 0));

    // Signal the fence with the current frame number, so that we can check back on it
    FrameFence.Signal(LastGfxQueue, CurrentCPUFrame);

    // Wait for the GPU to catch up before we stomp an executing command buffer
    const uint64 gpuLag = DX12::CurrentCPUFrame - DX12::CurrentGPUFrame;
    Assert_(gpuLag <= DX12::RenderLatency);
    if(gpuLag >= DX12::RenderLatency)
    {
        // Make sure that the previous frame is finished
        FrameFence.Wait(DX12::CurrentGPUFrame + 1);
        ++DX12::CurrentGPUFrame;
    }

    CurrFrameIdx = DX12::CurrentCPUFrame % NumCmdAllocators;

    // Prepare the command buffers to be used for the next frame
    for(CommandListData& cmdListData : CommandLists)
    {
        DXCall(cmdListData.CmdAllocators[CurrFrameIdx]->Reset());
        DXCall(cmdListData.CmdList->Reset(cmdListData.CmdAllocators[CurrFrameIdx], nullptr));
    }

    EndFrame_Helpers();
    EndFrame_Upload();

    // See if we have any deferred releases to process
    ProcessDeferredReleases(CurrFrameIdx);

    ProcessDeferredSRVCreates(CurrFrameIdx);
}

void FlushGPU()
{
    Assert_(Device);

    // Wait for the GPU to fully catch up with the CPU
    Assert_(CurrentCPUFrame >= CurrentGPUFrame);
    if(CurrentCPUFrame > CurrentGPUFrame)
    {
        FrameFence.Wait(CurrentCPUFrame);
        CurrentGPUFrame = CurrentCPUFrame;
    }

    // Clean up what we can now
    for(uint64 i = 1; i < RenderLatency; ++i)
    {
        uint64 frameIdx = (i + CurrFrameIdx) % RenderLatency;
        ProcessDeferredReleases(frameIdx);
        ProcessDeferredSRVCreates(frameIdx);
    }
}

void SetSubmitConfig(const SubmitConfig& config)
{
    FlushGPU();

    CleanupSubmitResources();

    ExtraFences.Init(config.NumFences);
    for(Fence& fence : ExtraFences)
        fence.Init(0);

    {
        // Create queues
        const uint64 numQueues = config.Queues.Size();
        Assert_(numQueues > 0);
        Queues.Init(numQueues, nullptr);

        uint64 numGfxQueues = 0;
        for(uint64 queueIdx = 0; queueIdx < numQueues; ++queueIdx)
        {
            const CmdQueueConfig& queueConfig = config.Queues[queueIdx];

            D3D12_COMMAND_QUEUE_DESC queueDesc = {};
            queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
            queueDesc.Type = queueConfig.Mode == CmdListMode::Graphics ? D3D12_COMMAND_LIST_TYPE_DIRECT : D3D12_COMMAND_LIST_TYPE_COMPUTE;
            DXCall(Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&Queues[queueIdx])));

            if(queueConfig.Name != nullptr)
                Queues[queueIdx]->SetName(queueConfig.Name);

            if(queueConfig.Mode == CmdListMode::Graphics)
                ++numGfxQueues;
        }

        Assert_(numGfxQueues > 0);
    }

    {
        // Create command lists
        const uint64 numCmdLists = config.CmdLists.Size();
        CommandLists.Init(numCmdLists);

        uint64 numGfxCmdLists = 0;
        for(uint64 cmdListIdx = 0; cmdListIdx < numCmdLists; ++cmdListIdx)
        {
            const CmdListConfig& cmdListConfig = config.CmdLists[cmdListIdx];
            CommandListData& cmdListData = CommandLists[cmdListIdx];

            cmdListData.Mode = cmdListConfig.Mode;
            D3D12_COMMAND_LIST_TYPE cmdListType = cmdListConfig.Mode == CmdListMode::Graphics ? D3D12_COMMAND_LIST_TYPE_DIRECT : D3D12_COMMAND_LIST_TYPE_COMPUTE;

            for(uint32 allocIdx = 0; allocIdx < NumCmdAllocators; ++allocIdx)
            {
                DXCall(Device->CreateCommandAllocator(cmdListType, IID_PPV_ARGS(&cmdListData.CmdAllocators[allocIdx])));
                if(cmdListConfig.AllocatorName != nullptr)
                {
                    std::wstring nameStr = MakeString(L"%s (%u)", cmdListConfig.AllocatorName, allocIdx);
                    cmdListData.CmdAllocators[allocIdx]->SetName(nameStr.c_str());
                }
            }

            DXCall(Device->CreateCommandList(0, cmdListType, cmdListData.CmdAllocators[0], nullptr, IID_PPV_ARGS(&cmdListData.CmdList)));
            DXCall(cmdListData.CmdList->Close());
            if(cmdListConfig.Name != nullptr)
                cmdListData.CmdList->SetName(cmdListConfig.Name);

            DXCall(cmdListData.CmdAllocators[CurrFrameIdx]->Reset());
            DXCall(cmdListData.CmdList->Reset(cmdListData.CmdAllocators[CurrFrameIdx], nullptr));

            if(cmdListConfig.Mode == CmdListMode::Graphics)
                ++numGfxCmdLists;
        }

        Assert_(numGfxCmdLists > 0);
    }

    {
        // Prepare submissions
        const uint64 numSubmissions = config.Submissions.Size();
        Submissions.Init(numSubmissions);

        for(uint64 submissionIdx = 0; submissionIdx < numSubmissions; ++submissionIdx)
        {
            const CmdSubmissionConfig& submissionConfig = config.Submissions[submissionIdx];
            CommandSubmission& submission = Submissions[submissionIdx];

            submission.Queue = Queues[submissionConfig.QueueIdx];
            if(config.Queues[submissionConfig.QueueIdx].Mode == CmdListMode::Graphics)
                LastGfxQueue = submission.Queue;

            const uint64 numCmdLists = submissionConfig.CmdListIndices.Size();
            Assert_(numCmdLists > 0);
            submission.CmdLists.Init(numCmdLists, nullptr);

            for(uint64 cmdListIdx = 0; cmdListIdx < numCmdLists; ++cmdListIdx)
            {
                const uint32 idx = submissionConfig.CmdListIndices[cmdListIdx];
                submission.CmdLists[cmdListIdx] = CommandLists[idx].CmdList;

                if(CommandLists[idx].Mode == CmdListMode::Graphics)
                {
                    if(FirstGfxCmdList == nullptr)
                        FirstGfxCmdList = CommandLists[idx].CmdList;
                    LastGfxCmdList = CommandLists[idx].CmdList;
                }
            }

            const uint64 numWaitFences = submissionConfig.WaitFenceIndices.Size();
            submission.WaitFences.Init(numWaitFences, nullptr);
            for(uint64 waitFenceIdx = 0; waitFenceIdx < numWaitFences; ++waitFenceIdx)
            {
                const uint32 fenceIdx = submissionConfig.WaitFenceIndices[waitFenceIdx];
                submission.WaitFences[waitFenceIdx] = &ExtraFences[fenceIdx];
            }

            if(submissionConfig.SignalFenceIdx != uint32(-1))
                submission.SignalFence = &ExtraFences[submissionConfig.SignalFenceIdx];
        }
    }

    Assert_(FirstGfxCmdList != nullptr);
    Assert_(LastGfxCmdList != nullptr);
    Assert_(LastGfxQueue!= nullptr);
}

ID3D12GraphicsCommandList4* CommandList(uint32 idx)
{
    return CommandLists[idx].CmdList;
}

ID3D12GraphicsCommandList4* FirstGfxCommandList()
{
    return FirstGfxCmdList;
}

ID3D12GraphicsCommandList4* LastGfxCommandList()
{
    return LastGfxCmdList;
}

ID3D12CommandQueue* CommandQueue(uint32 idx)
{
    return Queues[idx];
}

ID3D12CommandQueue* LastGfxCommandQueue()
{
    return LastGfxQueue;
}

void DeferredRelease_(IUnknown* resource)
{
    if(resource == nullptr)
        return;

    if(ShuttingDown || Device == nullptr)
    {
        // Free-for-all!
        resource->Release();
        return;
    }

    DeferredReleases[CurrFrameIdx].Add(resource);
}

void DeferredCreateSRV(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, uint32 descriptorIdx)
{
    for(uint64 i = 1; i < RenderLatency; ++i)
    {
        uint64 frameIdx = (CurrentCPUFrame + i) % RenderLatency;
        uint64 writeIdx = InterlockedIncrement(&DeferredSRVCreateCount[frameIdx]) - 1;
        DeferredSRVCreate& create = DeferredSRVCreates[frameIdx][writeIdx];
        create.Resource = resource;
        create.Desc = desc;
        create.DescriptorIdx = descriptorIdx;
    }
}

} // namespace SampleFramework12

} // namespace SampleFramework12

