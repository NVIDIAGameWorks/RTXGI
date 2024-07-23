/*
 * Copyright (c) 2019-2024, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#pragma once

#include <donut/app/ApplicationBase.h>
#include <donut/app/Camera.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/BindingCache.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/Scene.h>
#include <donut/engine/View.h>

#include "PathtracerUi.h"

// Unified Binding
struct DescriptorSetIDs 
{
    enum 
    {
        Globals,
        Denoiser,
        Nrc,
        Sharc,
        Bindless,
        COUNT
    };
};

#if ENABLE_DENOISER
#include "RenderTargets.h"
#include "NrdIntegration.h"
#endif // ENABLE_DENOISER

class ScopedMarker
{
public:
    ScopedMarker(nvrhi::ICommandList* commandList, const char* name)
        : m_commandList(commandList)
    {
        m_commandList->beginMarker(name);
    }
    ~ScopedMarker()
    {
        m_commandList->endMarker();
    }
private:
    nvrhi::ICommandList* m_commandList;
};

class Pathtracer : public donut::app::ApplicationBase
{
public:
    using ApplicationBase::ApplicationBase;

    struct PipelinePermutation
    {
        nvrhi::ShaderLibraryHandle shaderLibrary;
        nvrhi::rt::PipelineHandle pipeline;
        nvrhi::rt::ShaderTableHandle shaderTable;
    };

    Pathtracer(donut::app::DeviceManager* deviceManager, UIData& ui, nvrhi::GraphicsAPI api);
    virtual ~Pathtracer();
    
    bool Init(int argc, const char* const* argv);

    virtual bool LoadScene(std::shared_ptr<donut::vfs::IFileSystem> fs, const std::filesystem::path& sceneFileName) override;
    virtual void SceneUnloading() override;
    virtual void SceneLoaded() override;
    std::vector<std::string> const& GetAvailableScenes() const;
    std::shared_ptr<donut::engine::Scene>   GetScene() const;

    std::string GetCurrentSceneName() const;
    void SetPreferredSceneName(const std::string& sceneName);
    void SetCurrentSceneName(const std::string& sceneName);

    void CopyActiveCameraToFirstPerson();

    void EnableAnimations();
    void DisableAnimations();
    void Animate(float fElapsedTimeSeconds) override;

    bool KeyboardUpdate(int key, int scancode, int action, int mods) override;

    bool MousePosUpdate(double xpos, double ypos) override;
    bool MouseButtonUpdate(int button, int action, int mods) override;
    bool MouseScrollUpdate(double xoffset, double yoffset) override;

    bool CreateRayTracingPipeline(donut::engine::ShaderFactory& shaderFactory, PipelinePermutation& pipelinePermutation, std::vector<donut::engine::ShaderMacro>& pipelineMacros);
    bool CreateRayTracingPipelines();

#if ENABLE_NRC
    NrcIntegration* GetNrcInstance() const;
#endif

    void GetMeshBlasDesc(donut::engine::MeshInfo& mesh, nvrhi::rt::AccelStructDesc& blasDesc, bool skipTransmissiveMaterials) const;
    void CreateAccelStructs(nvrhi::ICommandList* commandList);
    void BuildTLAS(nvrhi::ICommandList* commandList, uint32_t frameIndex) const;

    void BackBufferResizing() override;

    void Render(nvrhi::IFramebuffer* framebuffer) override;

    std::shared_ptr<donut::engine::ShaderFactory> GetShaderFactory();
    std::shared_ptr<donut::vfs::IFileSystem> GetRootFS() const;

    std::shared_ptr<donut::engine::TextureCache> GetTextureCache();

    void RebuildAccelerationStructure();
    void ResetAccumulation();

    donut::app::FirstPersonCamera* GetCamera();

    std::string GetResolutionInfo();

private:
    std::shared_ptr<donut::vfs::RootFileSystem> m_rootFileSystem;
    std::shared_ptr<donut::vfs::NativeFileSystem> m_nativeFileSystem;

    enum PipelineType
    {
        DefaultPathTracing,
#if ENABLE_NRC
        NRC_Update,
        NRC_Query,
#endif // ENABLE_NRC
#if ENABLE_SHARC
        Sharc_Update,
        Sharc_Query,
#endif // ENABLE_SHARC
        Count
    };

    std::vector<donut::engine::ShaderMacro> m_pipelineMacros[PipelineType::Count];
    PipelinePermutation m_pipelinePermutations[PipelineType::Count];

    nvrhi::CommandListHandle m_commandList;
    nvrhi::BindingLayoutHandle m_globalBindingLayout;
    nvrhi::BindingSetHandle m_globalBindingSet;
    nvrhi::BindingLayoutHandle m_bindlessLayout;

    nvrhi::GraphicsPipelineHandle m_tonemappingPSO;
    nvrhi::BindingLayoutHandle m_tonemappingBindingLayout;
    nvrhi::BindingSetHandle m_tonemappingBindingSet;
    nvrhi::ShaderHandle m_tonemappingPS;

    nvrhi::rt::AccelStructHandle m_topLevelAS;
    bool m_rebuildAS = true;
    int m_cameraIndex = -1;

    nvrhi::BufferHandle m_constantBuffer;
    nvrhi::BufferHandle m_debugBuffer;

    std::shared_ptr<donut::engine::ShaderFactory> m_shaderFactory;
    std::shared_ptr<donut::engine::DescriptorTableManager> m_descriptorTable;

    std::vector<std::string> m_sceneFilesAvailable;
    std::string m_currentSceneName;
    std::shared_ptr<donut::engine::Scene> m_scene;

    nvrhi::TextureHandle m_accumulationBuffer;
    nvrhi::TextureHandle m_pathTracerOutputBuffer;

    donut::app::FirstPersonCamera m_camera;
    donut::engine::PlanarView m_view;
    donut::engine::PlanarView m_viewPrevious;

    std::shared_ptr<donut::engine::DirectionalLight> m_sunLight;
    std::shared_ptr<donut::engine::PointLight> m_headLight;

    std::unique_ptr<donut::engine::BindingCache> m_bindingCache;

    bool m_enableAnimations = false;
    float m_wallclockTime = 0.0f;
    int m_frameIndex = 0;

    UIData& m_ui;

    dm::affine3 m_prevViewMatrix;
    bool m_resetAccumulation;
    bool m_sceneReloaded;
    uint32_t m_accumulatedFrameCount;

    nvrhi::GraphicsAPI m_api;

#if ENABLE_NRC
    std::unique_ptr<NrcIntegration>     m_nrc;
    nrc::ContextSettings                m_nrcContextSettings;
    nrc::BuffersAllocationInfo          m_nrcBuffersAllocation;
    nvrhi::BindingLayoutHandle          m_nrcBindingLayout;
    nvrhi::BindingSetHandle             m_nrcBindingSet;
#endif // ENABLE_NRC

#if ENABLE_SHARC
    static const uint32_t               m_sharcInvalidEntry = 0;
    uint32_t                            m_sharcEntriesNum = 0;
    nvrhi::BufferHandle                 m_sharcHashEntriesBuffer;
    nvrhi::BufferHandle                 m_sharcCopyOffsetBuffer;
    nvrhi::BufferHandle                 m_sharcVoxelDataBuffer;
    nvrhi::BufferHandle                 m_sharcVoxelDataBufferPrev;

    nvrhi::BindingLayoutHandle          m_sharcBindingLayout;
    nvrhi::BindingSetHandle             m_sharcBindingSet;
    nvrhi::BindingSetHandle             m_sharcBindingSetSwapped;
    nvrhi::ShaderHandle                 m_sharcResolveCS;
    nvrhi::ComputePipelineHandle        m_sharcResolvePSO;
    nvrhi::ShaderHandle                 m_sharcHashCopyCS;
    nvrhi::ComputePipelineHandle        m_sharcHashCopyPSO;
#endif // ENABLE_SHARC

#if ENABLE_DENOISER
    nvrhi::BindingLayoutHandle          m_denoiserBindingLayout;
    nvrhi::BindingSetHandle             m_denoiserBindingSet;
    nvrhi::BindingSetHandle             m_denoiserOutBindingSet;
    nvrhi::ShaderHandle                 m_denoiserReblurPackCS;
    nvrhi::ComputePipelineHandle        m_denoiserReblurPackPSO;
    nvrhi::ShaderHandle                 m_denoiserReblurPack_NRC_CS;
    nvrhi::ComputePipelineHandle        m_denoiserReblurPack_NRC_PSO;
    nvrhi::ShaderHandle                 m_denoiserResolveCS;
    nvrhi::ComputePipelineHandle        m_denoiserResolvePSO;
    std::unique_ptr<RenderTargets>      m_renderTargets;
    std::unique_ptr<NrdIntegration>     m_nrd;
#endif // ENABLE_DENOISER

    // Unified Binding
    nvrhi::BindingLayoutHandle          m_dummyLayouts[DescriptorSetIDs::COUNT];
    nvrhi::BindingSetHandle             m_dummyBindingSets[DescriptorSetIDs::COUNT];
};