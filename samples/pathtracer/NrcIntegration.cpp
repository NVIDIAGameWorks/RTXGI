/*
* Copyright (c) 2024, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "NrcIntegration.h"
#include "NrcUtils.h"
#include <NrcD3d12.h>
#ifdef NRC_WITH_VULKAN
#include <NrcVk.h>
#endif

#ifdef NRC_ENABLE_DLL_CHECK
#include "NrcSecurity.h"
#include <Psapi.h>
#include <codecvt>
#endif

#include <nvrhi/utils.h>
#include <donut/core/math/math.h>
#include <donut/engine/View.h>
#include <donut/engine/ShaderFactory.h>
#include <sstream>
#include <donut/core/log.h>

#include <string>
#include <mutex>
#include "../../donut/nvrhi/src/vulkan/vulkan-backend.h"

using namespace donut::math;

#define SAFE_RELEASE(x)                                                                                                                                                            \
   {                                                                                                                                                                              \
       if (x)                                                                                                                                                                     \
       {                                                                                                                                                                          \
           x->Release();                                                                                                                                                          \
           x = NULL;                                                                                                                                                              \
       }                                                                                                                                                                          \
   }

static const D3D12_HEAP_PROPERTIES g_uploadHeapProperties = { D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
static const D3D12_HEAP_PROPERTIES g_defaultHeapProperties = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
static const D3D12_HEAP_PROPERTIES g_readbackHeapProperties = { D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };


static const bool g_enableSDKMemoryAllocation = true;
static const bool g_useCustomCPUMemoryAllocator = false;

// Utility
static void NrcLoggerCallback(const char* message, nrc::LogLevel logLevel)
{

    static std::mutex loggerMutex;

    // Make the logging thread-safe
    loggerMutex.lock();
    {
        const int kMinLogLevel = (int)nrc::LogLevel::Info;
        if (((int)logLevel >= kMinLogLevel) || (logLevel == nrc::LogLevel::Error))
        {
            std::wstring wstr = NrcUtils::StringToWstring(message);
            OutputDebugString(wstr.c_str());
#if 0
            if (logLevel == nrc::LogLevel::Error)
            {
                // Halt on NRC error
                NrcUtils::Validate(E_FAIL, LPWSTR(wstr.c_str()));
            }
#endif
        }
    }
    loggerMutex.unlock();
}

static void NrcMemoryEventsCallback(nrc::MemoryEventType eventType, size_t size, const char* bufferName)
{

    static std::mutex loggerMutex;

    loggerMutex.lock();
    {
        std::string message = "NRC SDK Memory Stats: ";

        switch (eventType)
        {
        case nrc::MemoryEventType::Allocation:
            message += std::to_string(size) + " bytes allocated (" + bufferName + ")\n";
            break;
        case nrc::MemoryEventType::Deallocation:
            message += std::to_string(size) + " bytes deallocated (" + bufferName + ")\n";
            break;
        case nrc::MemoryEventType::MemoryStats:
            message += std::to_string(size) + " bytes currently allocated in total\n";
            break;
        }

#if _DEBUG
        OutputDebugStringA(message.c_str());
#endif
    }
    loggerMutex.unlock();
}

static void* NrcCustomAllocatorCallback(const size_t bytes)
{
    return (void*)new char[bytes];
}

static void NrcCustomDeallocatorCallback(void* pointer, const size_t bytes)
{
    delete[] pointer;
}

static void FillBufferDescs(nvrhi::BufferDesc* bufferDescs, nrc::BuffersAllocationInfo const& buffersAllocationInfo)
{
    // Create an array of nvrhi::BufferDesc for the nrc buffers
    for (uint i = 0; i < (uint)nrc::BufferIdx::Count; ++i)
    {
        nvrhi::BufferDesc& bufferDesc = bufferDescs[i];
        const nrc::BufferIdx bufferIdx = (nrc::BufferIdx)i;
        const nrc::AllocationInfo& allocationInfo = buffersAllocationInfo[nrc::BufferIdx(i)];

        bufferDesc = nvrhi::BufferDesc();
        bufferDesc.isConstantBuffer = false;
        bufferDesc.isVolatile = false;
        bufferDesc.cpuAccess = nvrhi::CpuAccessMode::None;
        bufferDesc.keepInitialState = true;
        if (allocationInfo.elementCount > 0)
        {
            bufferDesc.byteSize = static_cast<UINT>(allocationInfo.elementCount) * allocationInfo.elementSize;
            bufferDesc.structStride = allocationInfo.elementSize;
            bufferDesc.canHaveUAVs = allocationInfo.allowUAV;
            bufferDesc.canHaveRawViews = (bufferIdx == nrc::BufferIdx::Counter);
            bufferDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            bufferDesc.setDebugName(allocationInfo.debugName);
        }
    }
}

static void CreateResources(nvrhi::BufferDesc const* bufferDescs, NrcBufferHandles& m_nrcBufferHandles, nvrhi::IDevice* device)
{
    // Create NVRHI buffers
    for (uint i = 0; i < (uint)nrc::BufferIdx::Count; ++i)
    {
        nvrhi::BufferDesc const& bufferDesc = bufferDescs[i];
        const nrc::BufferIdx bufferIdx = (nrc::BufferIdx)i;
        if (bufferDesc.byteSize > 0)
        {
            m_nrcBufferHandles[bufferIdx] = device->createBuffer(bufferDesc);
        }
        else
        {
            m_nrcBufferHandles[bufferIdx] = nullptr;
        }
    }
}

#ifdef NRC_ENABLE_DLL_CHECK
const std::wstring GetDllPath(const std::wstring& dllName)
{
    HMODULE hMod = GetModuleHandle(dllName.c_str());

    wchar_t path[MAX_PATH];
    DWORD size = GetModuleFileNameW(hMod, path, MAX_PATH);
    assert (size != 0);
    
    return std::wstring(path, size);
}
#endif

class NrcD3d12Integration : public NrcIntegration 
{
public:

    bool Initialize(nvrhi::IDevice* device)
    {
        nrc::GlobalSettings globalSettings;

        // First, set logger callbacks for catching messages from NRC
        globalSettings.loggerFn = &NrcLoggerCallback;
        globalSettings.memoryLoggerFn = &NrcMemoryEventsCallback;

        // Optionally, use custom CPU memory provided by the user application
        if (g_useCustomCPUMemoryAllocator)
        {
            globalSettings.allocatorFn = &NrcCustomAllocatorCallback;
            globalSettings.deallocatorFn = &NrcCustomDeallocatorCallback;
        }

        globalSettings.enableGPUMemoryAllocation = g_enableSDKMemoryAllocation;
        // Only enable debug buffers in development and not production
        globalSettings.enableDebugBuffers = true;
        m_enableDebugBuffers = globalSettings.enableDebugBuffers;

        // Initialize the NRC Library
        nrc::Status status = nrc::Status::OK;

        // Verify the signature of the loaded NRC DLL
#ifdef NRC_ENABLE_DLL_CHECK
        nrc::security::VerifySignature(GetDllPath(L"NRC_D3D12.dll").c_str()) ? status = nrc::Status::OK : status = nrc::Status::InternalError;
        if (status != nrc::Status::OK)
            return m_initialized;
#endif

        status = nrc::d3d12::Initialize(globalSettings);
        if (status != nrc::Status::OK)
            return m_initialized;

        // Create an NRC Context
        ID3D12Device* nativeDevice = device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);
        ID3D12Device5* nativeDevice5 = nullptr;
        if (SUCCEEDED(nativeDevice->QueryInterface(IID_PPV_ARGS(&nativeDevice5))))
        {
            status = nrc::d3d12::Context::Create(nativeDevice5, m_nrcContext);
            assert(status == nrc::Status::OK);

            m_device = device;

            if (!m_initialized && (status == nrc::Status::OK))
                m_initialized = true;
        }
        return m_initialized;
    }

    void Shutdown()
    {
        if (m_nrcContext)
        {
            nrc::d3d12::Context::Destroy(*m_nrcContext);
            m_nrcContext = nullptr;
        }

        nrc::d3d12::Shutdown();

        m_initialized = false;
    }

    void Configure(const nrc::ContextSettings& contextSettings)
    {
        nrc::Status status;

        // Configuration has changed
        m_contextSettings = contextSettings;

        nrc::d3d12::Context::GetBuffersAllocationInfo(contextSettings, m_buffersAllocation);
        nvrhi::BufferDesc bufferDescs[(int)nrc::BufferIdx::Count];
        FillBufferDescs(bufferDescs, m_buffersAllocation);

        if (g_enableSDKMemoryAllocation)
        {
            // NRC library manages memory in this case.
            // Pass it the new configuration
            status = m_nrcContext->Configure(contextSettings);

            // The NRC SDK is managing buffer allocations, so we need to pull those native buffers into NVRHI
            nrc::d3d12::Buffers const& buffers = *(m_nrcContext->GetBuffers());
            for (uint i = 0; i < (uint)nrc::BufferIdx::Count; ++i)
            {
                const nrc::BufferIdx bufferIdx = (nrc::BufferIdx)i;
                nrc::d3d12::BufferInfo const& bufferInfo = buffers[bufferIdx];
                if (bufferInfo.resource != nullptr)
                {
                    m_bufferHandles[bufferIdx] = m_device->createHandleForNativeBuffer(nvrhi::ObjectTypes::D3D12_Resource, bufferInfo.resource, bufferDescs[i]);
                }
                else
                {
                    m_bufferHandles[bufferIdx] = nullptr;
                }
            }
        }
        else
        {
            // Create NVRHI buffers
            CreateResources(bufferDescs, m_bufferHandles, m_device);

            // Pass the buffers to NRC
            for (uint i = 0; i < (uint)nrc::BufferIdx::Count; ++i)
            {
                const nrc::BufferIdx bufferIdx = (nrc::BufferIdx)i;
                m_buffers[bufferIdx].resource = reinterpret_cast<ID3D12Resource*>(m_bufferHandles[bufferIdx]->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource).pointer);
                m_buffers[bufferIdx].allocatedSize = bufferDescs[i].byteSize;
            }
            status = m_nrcContext->Configure(contextSettings, &m_buffers);
        }

        if (status != nrc::Status::OK)
            NrcUtils::Validate(E_FAIL, LPWSTR(L"NRC Configure step failed."));
    }

    void BeginFrame(nvrhi::ICommandList* cmdList, const nrc::FrameSettings& frameSettings)
    {
        nrc::Status status;

        ID3D12GraphicsCommandList4* nativeCmdList = reinterpret_cast<ID3D12GraphicsCommandList4*>(cmdList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList).pointer);
        if (nativeCmdList)
        {
            status = m_nrcContext->BeginFrame(nativeCmdList, frameSettings);
            if (status != nrc::Status::OK)
                NrcUtils::Validate(E_FAIL, LPWSTR(L"NRC BeginFrame call failed."));
        }
    }

    float QueryAndTrain(nvrhi::ICommandList* cmdList, bool calculateTrainingLoss)
    {
        ID3D12GraphicsCommandList4* nativeCmdList = reinterpret_cast<ID3D12GraphicsCommandList4*>(cmdList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList).pointer);
        float trainingLoss = 0.0f;

        if (nativeCmdList)
        {
            nrc::Status status = m_nrcContext->QueryAndTrain(nativeCmdList, (calculateTrainingLoss ? &trainingLoss : nullptr));
            if (status != nrc::Status::OK)
                NrcUtils::Validate(E_FAIL, LPWSTR(L"NRC QueryAndTrain call failed."));
        }

        return trainingLoss;
    }

    void Resolve(nvrhi::ICommandList* cmdList, nvrhi::TextureHandle outputBuffer)
    {
        ID3D12Resource* outputResource = reinterpret_cast<ID3D12Resource*>(outputBuffer->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource).pointer);
        ID3D12GraphicsCommandList4* nativeCmdList = reinterpret_cast<ID3D12GraphicsCommandList4*>(cmdList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList).pointer);
        if (nativeCmdList)
        {
            nrc::Status status = m_nrcContext->Resolve(nativeCmdList, outputResource);
            if (status != nrc::Status::OK)
                NrcUtils::Validate(E_FAIL, LPWSTR(L"NRC Resolve call failed."));
        }
    }

    void EndFrame(nvrhi::CommandQueue* cmdQueue)
    {
        ID3D12CommandQueue* nativeCmdQueue = reinterpret_cast<ID3D12CommandQueue*>(m_device->getNativeQueue(nvrhi::ObjectTypes::D3D12_CommandQueue, nvrhi::CommandQueue::Graphics).pointer);
        if (nativeCmdQueue)
        {
            nrc::Status status = m_nrcContext->EndFrame(nativeCmdQueue);
            if (status != nrc::Status::OK)
                NrcUtils::Validate(E_FAIL, LPWSTR(L"NRC EndFrame call failed."));
        }
    }

    size_t GetCurrentMemoryConsumption() const
    {
        const nrc::d3d12::Buffers& buffers = *(m_nrcContext->GetBuffers());

        size_t totalAllocatedMemory = 0;
        for (nrc::d3d12::BufferInfo const& buffer : buffers.buffers)
        {
            totalAllocatedMemory += buffer.allocatedSize;
        }
        return totalAllocatedMemory;
    }

    void PopulateShaderConstants(struct NrcConstants& outConstants) const
    {
        m_nrcContext->PopulateShaderConstants(outConstants);
    }

    nrc::d3d12::Buffers m_buffers;

private:
    nrc::d3d12::Context* m_nrcContext;
};

#ifdef NRC_WITH_VULKAN
class NrcVulkanIntegration : public NrcIntegration
{
public:
    bool Initialize(nvrhi::IDevice* device)
    {
        nrc::GlobalSettings globalSettings;

        // First, set logger callbacks for catching messages from NRC
        globalSettings.loggerFn = &NrcLoggerCallback;
        globalSettings.memoryLoggerFn = &NrcMemoryEventsCallback;

        // Optionally, use custom CPU memory provided by the user application
        if (g_useCustomCPUMemoryAllocator)
        {
            globalSettings.allocatorFn = &NrcCustomAllocatorCallback;
            globalSettings.deallocatorFn = &NrcCustomDeallocatorCallback;
        }

        globalSettings.enableGPUMemoryAllocation = g_enableSDKMemoryAllocation;
        // Only enable debug buffers in development and not production
        globalSettings.enableDebugBuffers = true;
        m_enableDebugBuffers = globalSettings.enableDebugBuffers;

        // Initialize the NRC Library
        nrc::Status status = nrc::Status::OK;

        // Verify the signature of the loaded NRC DLL
#ifdef NRC_ENABLE_DLL_CHECK
        nrc::security::VerifySignature(GetDllPath(L"NRC_Vulkan.dll").c_str()) ? status = nrc::Status::OK : status = nrc::Status::InternalError;
        if (status != nrc::Status::OK)
            return m_initialized;
#endif

        status = nrc::vulkan::Initialize(globalSettings);
        if (status != nrc::Status::OK)
        {
            return m_initialized;
        }

        // Create an NRC Context
        VkDevice nativeDevice = device->getNativeObject(nvrhi::ObjectTypes::VK_Device);
        VkPhysicalDevice nativeGPU = device->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice);
        VkInstance apiInstance = device->getNativeObject(nvrhi::ObjectTypes::VK_Instance);

        if (nativeDevice != nullptr && nativeGPU != nullptr)
        {
            status = nrc::vulkan::Context::Create(nativeDevice, nativeGPU, apiInstance, m_nrcContext);
            assert(status == nrc::Status::OK);
            m_device = device;

            if (!m_initialized && (status == nrc::Status::OK))
            {
                m_initialized = true;
            }
        }
        return m_initialized;
    }

    void Shutdown()
    {
        if (m_nrcContext)
        {
            nrc::vulkan::Context::Destroy(*m_nrcContext);
            m_nrcContext = nullptr;
        }

        nrc::vulkan::Shutdown();

        m_initialized = false;
    }

    void Configure(const nrc::ContextSettings& contextSettings)
    {
        nrc::Status status;

        // Configuration has changed
        m_contextSettings = contextSettings;

        nrc::vulkan::Context::GetBuffersAllocationInfo(contextSettings, m_buffersAllocation);
        nvrhi::BufferDesc bufferDescs[(int)nrc::BufferIdx::Count];
        FillBufferDescs(bufferDescs, m_buffersAllocation);

        if (g_enableSDKMemoryAllocation)
        {
            // NRC library manages memory in this case.
            // Pass it the new configuration
            status = m_nrcContext->Configure(contextSettings);

            // The NRC SDK is managing buffer allocations, so we need to pull those native buffers into NVRHI
            nrc::vulkan::Buffers const& buffers = *(m_nrcContext->GetBuffers());
            for (uint i = 0; i < (uint)nrc::BufferIdx::Count; ++i)
            {
                const nrc::BufferIdx bufferIdx = (nrc::BufferIdx)i;
                nrc::vulkan::BufferInfo const& bufferInfo = buffers[bufferIdx];
                if (bufferInfo.resource != nullptr)
                {
                    m_bufferHandles[bufferIdx] = m_device->createHandleForNativeBuffer(nvrhi::ObjectTypes::VK_Buffer, bufferInfo.resource, bufferDescs[i]);
                }
                else
                {
                    m_bufferHandles[bufferIdx] = nullptr;
                }
            }
        }
        else
        {
            // Create NVRHI buffers
            CreateResources(bufferDescs, m_bufferHandles, m_device);

            // Pass the buffers to NRC
            for (uint i = 0; i < (uint)nrc::BufferIdx::Count; ++i)
            {
                const nrc::BufferIdx bufferIdx = (nrc::BufferIdx)i;
                m_buffers[bufferIdx].resource = reinterpret_cast<VkBuffer>(m_bufferHandles[bufferIdx]->getNativeObject(nvrhi::ObjectTypes::VK_Buffer).pointer);
                m_buffers[bufferIdx].allocatedSize = bufferDescs[i].byteSize;
                m_buffers[bufferIdx].allocatedOffset = 0;

                auto addressInfo = vk::BufferDeviceAddressInfo().setBuffer(m_buffers[bufferIdx].resource);

                VkDevice nativeDevice = m_device->getNativeObject(nvrhi::ObjectTypes::VK_Device);
                m_buffers[bufferIdx].deviceAddress = vk::Device(nativeDevice).getBufferAddress(addressInfo);  
            }
            status = m_nrcContext->Configure(contextSettings, &m_buffers);
        }

        if (status != nrc::Status::OK)
        {
            NrcUtils::Validate(E_FAIL, LPWSTR(L"NRC Configure step failed."));
        }
    }

    void BeginFrame(nvrhi::ICommandList* cmdList, const nrc::FrameSettings& frameSettings)
    {
        VkCommandBuffer cmdBuffer = cmdList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
        if (cmdBuffer)
        {
            nrc::Status status = m_nrcContext->BeginFrame(cmdBuffer, frameSettings);
            if (status != nrc::Status::OK)
                NrcUtils::Validate(E_FAIL, LPWSTR(L"NRC BeginFrame call failed."));
        }
    }

    void EndFrame(nvrhi::CommandQueue* cmdQueue)
    {
        VkQueue nativeCmdQueue = reinterpret_cast<VkQueue>(cmdQueue);
        if (nativeCmdQueue)
        {
            nrc::Status status = m_nrcContext->EndFrame(nativeCmdQueue);
            if (status != nrc::Status::OK)
                NrcUtils::Validate(E_FAIL, LPWSTR(L"NRC EndFrame call failed."));
        }
    }

    float QueryAndTrain(nvrhi::ICommandList* cmdList, bool calculateTrainingLoss)
    {
        VkCommandBuffer buffer = cmdList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
        float trainingLoss = 0.0f;

        if (buffer)
        {
            nrc::Status status = m_nrcContext->QueryAndTrain(buffer, (calculateTrainingLoss ? &trainingLoss : nullptr));
            if (status != nrc::Status::OK)
            {
                NrcUtils::Validate(E_FAIL, LPWSTR(L"NRC QueryAndTrain call failed."));
            }
        }

        return trainingLoss;
    }

    void Resolve(nvrhi::ICommandList* cmdList, nvrhi::TextureHandle outputBuffer)
    {
        VkImage outputImage = outputBuffer->getNativeObject(nvrhi::ObjectTypes::VK_Image);
        VkImageView outputView = outputBuffer->getNativeView(nvrhi::ObjectTypes::VK_ImageView);
        VkCommandBuffer cmdBuffer = cmdList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
        if (cmdBuffer)
        {
            nrc::Status status = m_nrcContext->Resolve(cmdBuffer, outputView);
            if (status != nrc::Status::OK)
                NrcUtils::Validate(E_FAIL, LPWSTR(L"NRC Resolve call failed."));
        }
    }

    void PopulateShaderConstants(struct NrcConstants& outConstants) const
    {
        m_nrcContext->PopulateShaderConstants(outConstants);
    }

    void AllocateOrCheckAllResources()
    {
        nrc::BuffersAllocationInfo bufferAllocations;
        nrc::vulkan::Context::GetBuffersAllocationInfo(m_contextSettings, bufferAllocations);
    }

    size_t GetCurrentMemoryConsumption() const
    {
        const nrc::vulkan::Buffers& buffers = *(m_nrcContext->GetBuffers());

        size_t totalAllocatedMemory = 0;
        for (nrc::vulkan::BufferInfo const& buffer : buffers.buffers)
        {
            totalAllocatedMemory += buffer.allocatedSize;
        }
        return totalAllocatedMemory;
    }

    nrc::vulkan::Buffers m_buffers;

private:
    nrc::vulkan::Context* m_nrcContext;
    
};

std::unique_ptr<NrcIntegration> CreateNrcIntegration(nvrhi::GraphicsAPI api)
{
    if (api == nvrhi::GraphicsAPI::VULKAN)
        return std::make_unique<NrcVulkanIntegration>();
    else
        return std::make_unique<NrcD3d12Integration>();
}

#endif