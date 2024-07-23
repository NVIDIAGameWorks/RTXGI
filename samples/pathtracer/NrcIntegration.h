/*
* Copyright (c) 2024, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#pragma once

#include <nvrhi/nvrhi.h>
#include <NrcCommon.h>
#include <memory>

// NVRHI handles to NRC Buffers
struct NrcBufferHandles
{
    nvrhi::BufferHandle nrcBufferHandles[(int)nrc::BufferIdx::Count];

    // Allow direct access to the array with nrc::BufferIdx
    const nvrhi::BufferHandle& operator[](nrc::BufferIdx idx) const { return nrcBufferHandles[(int)idx]; }
    nvrhi::BufferHandle& operator[](nrc::BufferIdx idx) { return nrcBufferHandles[(int)idx]; }
};

class NrcIntegration
{
public:
    virtual bool Initialize(nvrhi::IDevice* device) = 0;

    virtual void Shutdown() = 0;

    virtual void Configure(const nrc::ContextSettings& contextSettings) = 0;

    virtual void BeginFrame(nvrhi::ICommandList* cmdList, const nrc::FrameSettings& frameSettings) = 0;

    virtual float QueryAndTrain(nvrhi::ICommandList* cmdList, bool calculateTrainingLoss) = 0;

    virtual void Resolve(nvrhi::ICommandList* cmdList, nvrhi::TextureHandle outputBuffer) = 0;

    virtual void EndFrame(nvrhi::CommandQueue* cmdQueue) = 0;

    virtual size_t GetCurrentMemoryConsumption() const = 0;

    virtual void PopulateShaderConstants(struct NrcConstants& outConstants) const = 0;

    bool IsInitialized() const { return m_initialized; };
    
    NrcBufferHandles m_bufferHandles;

protected:
    nvrhi::IDevice* m_device;
    bool m_initialized = false;
    bool m_enableDebugBuffers;
    nrc::BuffersAllocationInfo m_buffersAllocation;
    nrc::ContextSettings m_contextSettings;
    nrc::FrameSettings m_frameSettings;  
};

std::unique_ptr<NrcIntegration> CreateNrcIntegration(nvrhi::GraphicsAPI);
