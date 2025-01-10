/*
 * Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#pragma once

#include <NRD.h>
#include <nvrhi/nvrhi.h>
#include <donut/engine/BindingCache.h>

class RenderTargets;

namespace donut::engine
{
class PlanarView;
class ShaderFactory;
} // namespace donut::engine

class NrdIntegration
{
public:
    NrdIntegration(nvrhi::IDevice* device, nrd::Denoiser method);
    ~NrdIntegration();

    bool Initialize(uint32_t width, uint32_t height, donut::engine::ShaderFactory& shaderFactory);
    bool IsAvailable() const;

    void RunDenoiserPasses(nvrhi::ICommandList* commandList,
                           const RenderTargets& renderTargets,
                           int pass,
                           const donut::engine::PlanarView& view,
                           const donut::engine::PlanarView& viewPrev,
                           uint32_t frameIndex,
                           float disocclusionThreshold,
                           float disocclusionThresholdAlternate,
                           bool useDisocclusionThresholdAlternateMix,
                           bool enableValidation,
                           const void* methodSettings,
                           bool reset);

    const nrd::Denoiser GetDenoiser() const
    {
        return m_denoiser;
    }

private:
    nvrhi::DeviceHandle m_device;
    bool m_initialized;
    nrd::Instance* m_instance;
    nrd::Denoiser m_denoiser;
    nrd::Identifier m_identifier;

    struct NrdPipeline
    {
        nvrhi::ShaderHandle shader;
        nvrhi::BindingLayoutHandle bindingLayout;
        nvrhi::ComputePipelineHandle pipeline;
    };

    nvrhi::BufferHandle m_constantBuffer;
    std::vector<NrdPipeline> m_pipelines;
    std::vector<nvrhi::SamplerHandle> m_samplers;
    std::vector<nvrhi::TextureHandle> m_permanentTextures;
    std::vector<nvrhi::TextureHandle> m_transientTextures;
    donut::engine::BindingCache m_bindingCache;
};