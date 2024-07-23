/*
 * Copyright (c) 2019-2024, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#include <donut/render/GBufferFillPass.h>
#include <donut/render/ForwardShadingPass.h>
#include <donut/render/DrawStrategy.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/BindingCache.h>
#include <donut/engine/SceneGraph.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <nvrhi/utils.h>
#include <donut/app/imgui_console.h>
#include <donut/app/imgui_renderer.h>
#include <donut/engine/TextureCache.h>

#include "Pathtracer.h"

using namespace donut;
using namespace donut::math;
using namespace donut::app;
using namespace donut::engine;

#if ENABLE_NRC
#include "NrcUtils.h"
#ifdef NRC_WITH_VULKAN
#include "NrcVk.h"
#endif // NRC_WITH_VULKAN
#endif // ENABLE_NRC

#include "../../donut/nvrhi/src/vulkan/vulkan-backend.h"

#include "lighting_cb.h"
#include "global_cb.h"

#if ENABLE_DENOISER
#include "NrdConfig.h"
#endif // ENABLE_DENOISER

// Required for Agility SDK on Windows 10. Setup 1.c. 2.a.
// https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 610; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

// TODO: Remove when this is addressed in Donut
extern "C" { __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;}
extern "C" { __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1; }

static const char* g_WindowTitle = "Pathtracer";

static uint32_t DivideRoundUp(uint32_t x, uint32_t divisor)
{
    return (x + divisor - 1) / divisor;
}

void InjectFeatures(VkDeviceCreateInfo& info)
{
    static vk::PhysicalDeviceFeatures2 deviceFeatures;
    vk::PhysicalDeviceVulkan12Features* features12 = (vk::PhysicalDeviceVulkan12Features*)info.pNext;

    assert((VkStructureType)features12->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    {
        features12->shaderBufferInt64Atomics = true;
        features12->shaderSharedInt64Atomics = true;
        features12->scalarBlockLayout = true;
    }

    deviceFeatures.features = *info.pEnabledFeatures;
    deviceFeatures.features.shaderInt64 = true;
    deviceFeatures.features.fragmentStoresAndAtomics = true;

    info.pEnabledFeatures = nullptr;
    deviceFeatures.pNext = (void*)info.pNext;
    info.pNext = &deviceFeatures;
}

std::filesystem::path GetLocalPath(std::string subfolder)
{
    static std::filesystem::path oneChoice;

    std::filesystem::path candidateA = app::GetDirectoryWithExecutable() / subfolder;
    std::filesystem::path candidateB = app::GetDirectoryWithExecutable().parent_path() / subfolder;
    if (std::filesystem::exists(candidateA))
        oneChoice = candidateA;
    else
        oneChoice = candidateB;

    return oneChoice;
}

Pathtracer::Pathtracer(DeviceManager* deviceManager, UIData& ui, nvrhi::GraphicsAPI api)
    : ApplicationBase(deviceManager)
    , m_ui(ui)
    , m_api(api)
{
}

Pathtracer::~Pathtracer()
{
#if ENABLE_NRC
    m_nrc->Shutdown();
#endif // ENABLE_NRC
}

bool Pathtracer::Init(int argc, const char* const* argv)
{
    char* sceneName = nullptr;
    for (int n = 1; n < argc; n++)
    {
        const char* arg = argv[n];

        if (!strcmp(arg, "-accumulate"))
            m_ui.enableAccumulation = true;
        else if (!strcmp(arg, "-scene"))
            sceneName = (char*)argv[n + 1];
        else if (!strcmp(arg, "-camera"))
            m_cameraIndex = atoi(argv[n + 1]);
#if ENABLE_NRC
        else if (!strcmp(arg, "-nrc"))
        {
            m_ui.currentMode = TechSelection::Nrc;
            m_ui.enableNrc = true;
        }
#endif // ENABLE_NRC
#if ENABLE_SHARC
        else if (!strcmp(arg, "-sharc"))
        {
            m_ui.currentMode = TechSelection::Sharc;
            m_ui.enableSharc = true;
        }
#endif // ENABLE_SHARC
    }

    m_resetAccumulation = true;
    m_accumulatedFrameCount = 0;
    m_ui.enableAnimations = m_enableAnimations;

    m_nativeFileSystem = std::make_shared<vfs::NativeFileSystem>();
    std::filesystem::path sceneFileName = app::GetDirectoryWithExecutable().parent_path() / "media/bistro.scene.json";
    std::filesystem::path mediaPath = app::GetDirectoryWithExecutable().parent_path() / "media";
    std::filesystem::path frameworkShaderPath = app::GetDirectoryWithExecutable() / "shaders/framework" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
    std::filesystem::path appShaderPath = app::GetDirectoryWithExecutable() / "shaders/pathtracer" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());

    m_rootFileSystem = std::make_shared<vfs::RootFileSystem>();
    m_rootFileSystem->mount("/media", mediaPath);
    m_rootFileSystem->mount("/shaders/donut", frameworkShaderPath);
    m_rootFileSystem->mount("/shaders/app", appShaderPath);
    m_rootFileSystem->mount("/native", m_nativeFileSystem);

    // Override default scene
    const std::string mediaExt = ".scene.json";
    if (sceneName)
    {
        sceneFileName = app::GetDirectoryWithExecutable().parent_path() / "media/";
        sceneFileName += sceneName;

        if (!strstr(sceneName, mediaExt.c_str()))
            sceneFileName += ".scene.json";
    }

#if ENABLE_DENOISER
    std::filesystem::path nrdShaderPath = app::GetDirectoryWithExecutable() / "shaders/nrd" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
    m_rootFileSystem->mount("/shaders/nrd", nrdShaderPath);
#endif // ENABLE_DENOISER

    // Get all scenes in "media" folder
    for (const auto& file : std::filesystem::directory_iterator(GetLocalPath("media")))
    {
        if (!file.is_regular_file())
            continue;
        std::string fileName = file.path().filename().string();
        std::string longExt = (fileName.size() <= mediaExt.length()) ? ("") : (fileName.substr(fileName.length() - mediaExt.length()));
        if (longExt == mediaExt)
            m_sceneFilesAvailable.push_back(file.path().string());
    }

    m_shaderFactory = std::make_shared<engine::ShaderFactory>(GetDevice(), m_rootFileSystem, "/shaders");
    m_CommonPasses = std::make_shared<engine::CommonRenderPasses>(GetDevice(), m_shaderFactory);
    m_bindingCache = std::make_unique<engine::BindingCache>(GetDevice());

#if ENABLE_NRC
    m_nrc = CreateNrcIntegration(m_api);
#endif // ENABLE_NRC

    nvrhi::BindlessLayoutDesc bindlessLayoutDesc;
    bindlessLayoutDesc.visibility = nvrhi::ShaderType::All;
    bindlessLayoutDesc.firstSlot = 0;
    bindlessLayoutDesc.maxCapacity = 1024;
    bindlessLayoutDesc.registerSpaces = {
        nvrhi::BindingLayoutItem::RawBuffer_SRV(1),
        nvrhi::BindingLayoutItem::Texture_SRV(2)
    };

    m_bindlessLayout = GetDevice()->createBindlessLayout(bindlessLayoutDesc);
    m_descriptorTable = std::make_shared<engine::DescriptorTableManager>(GetDevice(), m_bindlessLayout);
    m_TextureCache = std::make_shared<engine::TextureCache>(GetDevice(), m_nativeFileSystem, m_descriptorTable);

    SetAsynchronousLoadingEnabled(false);
    SetCurrentSceneName(sceneFileName.string());
    m_scene->FinishedLoading(GetFrameIndex());

    m_camera.SetMoveSpeed(3.f);

    m_constantBuffer = GetDevice()->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(LightingConstants), "LightingConstants", engine::c_MaxRenderPassConstantBufferVersions));

    m_debugBuffer = GetDevice()->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(GlobalConstants), "GlobalConstants", engine::c_MaxRenderPassConstantBufferVersions));

    // Unified binding
    nvrhi::BindingLayoutDesc bindingLayoutDesc;
    bindingLayoutDesc.visibility = nvrhi::ShaderType::All;
    bindingLayoutDesc.registerSpaceIsDescriptorSet = (m_api == nvrhi::GraphicsAPI::VULKAN);
    for (int i = 0; i < DescriptorSetIDs::COUNT; ++i)
    {
        bindingLayoutDesc.registerSpace = i;
        m_dummyLayouts[i] = GetDevice()->createBindingLayout(bindingLayoutDesc);

        nvrhi::BindingSetDesc dummyBindingDesc;
        m_dummyBindingSets[i] = GetDevice()->createBindingSet(dummyBindingDesc, m_dummyLayouts[i]);
    }

    // Global binding layout
    bindingLayoutDesc.registerSpace = DescriptorSetIDs::Globals;
    bindingLayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(1),
        nvrhi::BindingLayoutItem::RayTracingAccelStruct(0),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1), // instance
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2), // geometry
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3), // materials
        nvrhi::BindingLayoutItem::Sampler(0),
        nvrhi::BindingLayoutItem::Texture_UAV(0), // path tracer output
    };
    m_globalBindingLayout = GetDevice()->createBindingLayout(bindingLayoutDesc);

#if ENABLE_DENOISER
    bindingLayoutDesc.registerSpace = DescriptorSetIDs::Denoiser;
    bindingLayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::Texture_UAV(0),
        nvrhi::BindingLayoutItem::Texture_UAV(1),
        nvrhi::BindingLayoutItem::Texture_UAV(2),
        nvrhi::BindingLayoutItem::Texture_UAV(3),
        nvrhi::BindingLayoutItem::Texture_UAV(4),
        nvrhi::BindingLayoutItem::Texture_UAV(5),
        nvrhi::BindingLayoutItem::Texture_UAV(6),
        nvrhi::BindingLayoutItem::Texture_UAV(7),
    };
    m_denoiserBindingLayout = GetDevice()->createBindingLayout(bindingLayoutDesc);
#endif // ENABLE_DENOISER

#if ENABLE_NRC
    bindingLayoutDesc.registerSpace = DescriptorSetIDs::Nrc;
    bindingLayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0), // QueryPathInfo
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1), // TrainingPathInfo
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2), // PathVertices
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3), // QueryRadianceParams
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(4), // CountersData
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(5), // DebugTrainingPathInfo,
    };
    m_nrcBindingLayout = GetDevice()->createBindingLayout(bindingLayoutDesc);
#endif // ENABLE_NRC

#if ENABLE_SHARC
    bindingLayoutDesc.registerSpace = DescriptorSetIDs::Sharc;
    bindingLayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1),
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2),
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3),
    };
    m_sharcBindingLayout = GetDevice()->createBindingLayout(bindingLayoutDesc);
#endif // ENABLE_SHARC

    if (!CreateRayTracingPipelines())
        return false;

    // Prepare resources for the SHARC copy and resolve compute passes
#if ENABLE_SHARC
    {
        m_sharcEntriesNum = 4 * 1024 * 1024;

        // Buffers
        nvrhi::BufferDesc bufferDesc;
        bufferDesc.isConstantBuffer = false;
        bufferDesc.isVolatile = false;
        bufferDesc.canHaveUAVs = true;
        bufferDesc.cpuAccess = nvrhi::CpuAccessMode::None;
        bufferDesc.keepInitialState = true;
        bufferDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;

        bufferDesc.byteSize = m_sharcEntriesNum * sizeof(uint64_t);
        bufferDesc.structStride = sizeof(uint64_t);
        bufferDesc.debugName = "m_sharcHashEntriesBuffer";
        m_sharcHashEntriesBuffer = GetDevice()->createBuffer(bufferDesc);

        bufferDesc.byteSize = m_sharcEntriesNum * sizeof(uint32_t);
        bufferDesc.structStride = sizeof(uint32_t);
        bufferDesc.debugName = "m_sharcCopyOffsetBuffer";
        m_sharcCopyOffsetBuffer = GetDevice()->createBuffer(bufferDesc);

        bufferDesc.byteSize = m_sharcEntriesNum * sizeof(float4);
        bufferDesc.structStride = 4 * sizeof(uint32_t);
        bufferDesc.canHaveRawViews = true;

        bufferDesc.debugName = "m_sharcVoxelDataBuffer";
        m_sharcVoxelDataBuffer = GetDevice()->createBuffer(bufferDesc);

        bufferDesc.debugName = "m_sharcVoxelDataBufferPrev";
        m_sharcVoxelDataBufferPrev = GetDevice()->createBuffer(bufferDesc);

        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_sharcHashEntriesBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(1, m_sharcCopyOffsetBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(2, m_sharcVoxelDataBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(3, m_sharcVoxelDataBufferPrev),
        };
        m_sharcBindingSet = GetDevice()->createBindingSet(bindingSetDesc, m_sharcBindingLayout);

        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_sharcHashEntriesBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(1, m_sharcCopyOffsetBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(2, m_sharcVoxelDataBufferPrev),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(3, m_sharcVoxelDataBuffer),
        };
        m_sharcBindingSetSwapped = GetDevice()->createBindingSet(bindingSetDesc, m_sharcBindingLayout);

        {
            nvrhi::ComputePipelineDesc pipelineDesc;
            if (m_api == nvrhi::GraphicsAPI::D3D12) 
                pipelineDesc.bindingLayouts = { m_globalBindingLayout, m_sharcBindingLayout };
            else
                pipelineDesc.bindingLayouts = { m_globalBindingLayout, m_dummyLayouts[1], m_dummyLayouts[2], m_sharcBindingLayout };

            m_sharcResolveCS = m_shaderFactory->CreateShader("app/sharcResolve.hlsl", "sharcResolve", nullptr, nvrhi::ShaderType::Compute);
            pipelineDesc.CS = m_sharcResolveCS;
            m_sharcResolvePSO = GetDevice()->createComputePipeline(pipelineDesc);

            m_sharcHashCopyCS = m_shaderFactory->CreateShader("app/sharcResolve.hlsl", "sharcCompaction", nullptr, nvrhi::ShaderType::Compute);
            pipelineDesc.CS = m_sharcHashCopyCS;
            m_sharcHashCopyPSO = GetDevice()->createComputePipeline(pipelineDesc);
        }
    }
#endif // ENABLE_SHARC

#if ENABLE_DENOISER
    std::vector<ShaderMacro> denoiseMacros = { ShaderMacro("NRD_NORMAL_ENCODING", "2"), ShaderMacro("NRD_ROUGHNESS_ENCODING", "1") };

    {
        m_denoiserReblurPackCS = m_shaderFactory->CreateShader("app/denoiser.hlsl", "reblurPackData", &denoiseMacros, nvrhi::ShaderType::Compute);
        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.bindingLayouts = { m_globalBindingLayout, m_denoiserBindingLayout };
        pipelineDesc.CS = m_denoiserReblurPackCS;
        m_denoiserReblurPackPSO = GetDevice()->createComputePipeline(pipelineDesc);
    }

#if ENABLE_NRC
    {
        std::vector<ShaderMacro> denoiseMacrosNRC = denoiseMacros;
        denoiseMacrosNRC.push_back((ShaderMacro("ENABLE_NRC", "1")));

        m_denoiserReblurPack_NRC_CS = m_shaderFactory->CreateShader("app/denoiser.hlsl", "reblurPackData", &denoiseMacrosNRC, nvrhi::ShaderType::Compute);
        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.bindingLayouts = { m_globalBindingLayout, m_denoiserBindingLayout };
        pipelineDesc.CS = m_denoiserReblurPack_NRC_CS;
        m_denoiserReblurPack_NRC_PSO = GetDevice()->createComputePipeline(pipelineDesc);
    }
#endif // ENABLE_NRC

    {
        m_denoiserResolveCS = m_shaderFactory->CreateShader("app/denoiser.hlsl", "resolve", &denoiseMacros, nvrhi::ShaderType::Compute);
        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.bindingLayouts = { m_globalBindingLayout, m_denoiserBindingLayout };
        pipelineDesc.CS = m_denoiserResolveCS;
        m_denoiserResolvePSO = GetDevice()->createComputePipeline(pipelineDesc);
    }
#endif // ENABLE_DENOISER

    // Create the tonemapping pass
    {
        nvrhi::BindingLayoutDesc bindingLayoutDesc;
        bindingLayoutDesc.visibility = nvrhi::ShaderType::Pixel;
        bindingLayoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::Texture_UAV(0),
            nvrhi::BindingLayoutItem::Texture_UAV(1),
        };

        m_tonemappingBindingLayout = GetDevice()->createBindingLayout(bindingLayoutDesc);

        m_tonemappingPS = m_shaderFactory->CreateShader("app/tonemapping.hlsl", "main_ps", nullptr, nvrhi::ShaderType::Pixel);
    }

    m_commandList = GetDevice()->createCommandList();

    return true;
}

bool Pathtracer::CreateRayTracingPipelines()
{
    // Reset macros
    for (uint i = 0; i < PipelineType::Count; ++i)
        m_pipelineMacros[i].clear();

    // Select the current macros
#if ENABLE_DENOISER
    char const* enableDenoiserStr = m_ui.enableDenoiser ? "1" : "0";
#else // !ENABLE_DENOISER
    char const* enableDenoiserStr = "0";
#endif // !ENABLE_DENOISER

    m_pipelineMacros[PipelineType::DefaultPathTracing].push_back(ShaderMacro("REFERENCE", "1"));
    m_pipelineMacros[PipelineType::DefaultPathTracing].push_back(ShaderMacro("ENABLE_DENOISER", enableDenoiserStr));

#if ENABLE_NRC
    m_pipelineMacros[PipelineType::NRC_Update].push_back(ShaderMacro("NRC_UPDATE", "1"));
    m_pipelineMacros[PipelineType::NRC_Query].push_back(ShaderMacro("NRC_QUERY", "1"));
    m_pipelineMacros[PipelineType::NRC_Query].push_back(ShaderMacro("ENABLE_DENOISER", enableDenoiserStr));
#endif // ENABLE_NRC

#if ENABLE_SHARC
    m_pipelineMacros[PipelineType::Sharc_Update].push_back(ShaderMacro("SHARC_UPDATE", "1"));

    m_pipelineMacros[PipelineType::Sharc_Query].push_back(ShaderMacro("SHARC_QUERY", "1"));
    m_pipelineMacros[PipelineType::Sharc_Query].push_back(ShaderMacro("ENABLE_DENOISER", enableDenoiserStr));
#endif // ENABLE_SHARC

    // Create the RT pipelines
    for (uint32_t i = 0; i < PipelineType::Count; ++i)
    {
        if (!CreateRayTracingPipeline(*m_shaderFactory, m_pipelinePermutations[i], m_pipelineMacros[i]))
            return false;
    }

    return true;
}

#if ENABLE_NRC
NrcIntegration* Pathtracer::GetNrcInstance() const
{
    return m_nrc.get();
}
#endif

bool Pathtracer::LoadScene(std::shared_ptr<vfs::IFileSystem> fs, const std::filesystem::path& sceneFileName)
{
    engine::Scene* scene = new engine::Scene(GetDevice(), *m_shaderFactory, fs, m_TextureCache, m_descriptorTable, nullptr);

    if (scene->Load(sceneFileName))
    {
        m_sceneReloaded = true;
        m_scene = std::unique_ptr<engine::Scene>(scene);
        return true;
    }

    return false;
}

void Pathtracer::SceneLoaded()
{
    ApplicationBase::SceneLoaded();

    m_scene->FinishedLoading(GetFrameIndex());

    m_resetAccumulation = true;
    m_accumulatedFrameCount = 1;
    m_rebuildAS = true;

    // Look for an existing sunlight
    for (auto light : m_scene->GetSceneGraph()->GetLights())
    {
        if (light->GetLightType() == LightType_Directional)
        {
            m_sunLight = std::static_pointer_cast<DirectionalLight>(light);
            break;
        }
    }

    auto cameras = m_scene->GetSceneGraph()->GetCameras();
    if (!cameras.empty())
    {
        // Override camera
        if (m_cameraIndex != -1 && m_cameraIndex < cameras.size())
        {
            m_ui.activeSceneCamera = cameras[m_cameraIndex];
            m_cameraIndex = -1;
        }
        else
        {
            std::string cameraName = "DefaultCamera";
            auto it = std::find_if(cameras.begin(), cameras.end(), [cameraName](std::shared_ptr<donut::engine::SceneCamera> const& camera) {
                return camera->GetName() == cameraName;
                });
            it != cameras.end() ? m_ui.activeSceneCamera = *it : m_ui.activeSceneCamera = cameras[0];
        }

        CopyActiveCameraToFirstPerson();
    }
    else
    {
        m_ui.activeSceneCamera.reset();
        m_camera.LookAt(float3(0.f, 1.8f, 0.f), float3(1.f, 1.8f, 0.f));
    }

    // Create a sunlight if there isn't one in the scene
    if (!m_sunLight)
    {
        m_sunLight = std::make_shared<DirectionalLight>();
        auto node = std::make_shared<SceneGraphNode>();
        node->SetLeaf(m_sunLight);
        m_sunLight->SetName("Sun");
        m_scene->GetSceneGraph()->Attach(m_scene->GetSceneGraph()->GetRootNode(), node);
    }

    // Set the sunlight properties
    m_sunLight->angularSize = 0.8f;
    m_sunLight->irradiance = 20.f;
    m_sunLight->SetDirection(dm::double3(-0.049f, -0.87f, 0.48f));

}

void Pathtracer::SceneUnloading()
{
    GetDevice()->waitForIdle();

    m_shaderFactory->ClearCache();

    m_bindingCache->Clear();
    m_sunLight = nullptr;
    m_headLight = nullptr;
    m_ui.selectedMaterial = nullptr;
    m_ui.activeSceneCamera = nullptr;
    m_ui.targetLight = -1;

    m_topLevelAS = nullptr;

    // Force the buffers to be re-created, as well as the bindings
    BackBufferResizing();
}

std::vector<std::string> const& Pathtracer::GetAvailableScenes() const
{
    return m_sceneFilesAvailable;
}

std::string Pathtracer::GetCurrentSceneName() const
{
    return m_currentSceneName;
}

void Pathtracer::SetPreferredSceneName(const std::string& sceneName)
{
    SetCurrentSceneName(app::FindPreferredScene(m_sceneFilesAvailable, sceneName));
}

void Pathtracer::SetCurrentSceneName(const std::string& sceneName)
{
    if (m_currentSceneName == sceneName)
        return;

    m_currentSceneName = sceneName;

    BeginLoadingScene(m_nativeFileSystem, m_currentSceneName);

#if ENABLE_NRC
    if (!m_nrc->IsInitialized())
        m_nrc->Initialize(GetDevice());
#endif // ENABLE_NRC
}

void Pathtracer::CopyActiveCameraToFirstPerson()
{
    if (m_ui.activeSceneCamera)
    {
        dm::affine3 viewToWorld = m_ui.activeSceneCamera->GetViewToWorldMatrix();
        dm::float3 cameraPos = viewToWorld.m_translation;
        m_camera.LookAt(cameraPos, cameraPos + viewToWorld.m_linear.row2, viewToWorld.m_linear.row1);
    }
}

bool Pathtracer::KeyboardUpdate(int key, int scancode, int action, int mods)
{
    m_camera.KeyboardUpdate(key, scancode, action, mods);

    if (key == GLFW_KEY_F2 && action == GLFW_PRESS)
        m_ui.showUI = !m_ui.showUI;

    return true;
}

void Pathtracer::EnableAnimations()
{
    m_enableAnimations = true;
}

void Pathtracer::DisableAnimations()
{
    m_enableAnimations = false;
}

bool Pathtracer::MousePosUpdate(double xpos, double ypos)
{
    m_camera.MousePosUpdate(xpos, ypos);
    return true;
}

bool Pathtracer::MouseButtonUpdate(int button, int action, int mods)
{
    m_camera.MouseButtonUpdate(button, action, mods);
    return true;
}

bool Pathtracer::MouseScrollUpdate(double xoffset, double yoffset)
{
    m_camera.MouseScrollUpdate(xoffset, yoffset);
    return true;
}

void Pathtracer::Animate(float fElapsedTimeSeconds)
{
    m_camera.Animate(fElapsedTimeSeconds);

    if (IsSceneLoaded() && m_enableAnimations)
    {
        m_wallclockTime += fElapsedTimeSeconds;
        float offset = 0;

        for (const auto& anim : m_scene->GetSceneGraph()->GetAnimations())
        {
            float duration = anim->GetDuration();
            float integral;
            float animationTime = std::modf((m_wallclockTime + offset) / duration, &integral) * duration;
            (void)anim->Apply(animationTime);
            offset += 1.0f;
        }
    }

    GetDeviceManager()->SetInformativeWindowTitle(g_WindowTitle);
}

bool Pathtracer::CreateRayTracingPipeline(engine::ShaderFactory& shaderFactory, PipelinePermutation& pipelinePermutation, std::vector<engine::ShaderMacro>& pipelineMacros)
{
    nvrhi::ShaderLibraryHandle shaderLibrary = shaderFactory.CreateShaderLibrary("app/pathtracer.hlsl", &pipelineMacros);
    if (!shaderLibrary)
        return false;

    pipelinePermutation.shaderLibrary = shaderLibrary;

    auto macroDefined = [pipelineMacros](std::string inputToken)
    {
        auto it = std::find_if(pipelineMacros.begin(), pipelineMacros.end(), [&inputToken](const engine::ShaderMacro& macro) {return (macro.name.find(inputToken) != std::string::npos);/*return macro.name == inputToken;*/ });
        if (it != pipelineMacros.end())
            return it->definition == "1";
        return false;
    };

    nvrhi::rt::PipelineDesc pipelineDesc;
    for (int i = 0; i < DescriptorSetIDs::COUNT; ++i) 
        pipelineDesc.globalBindingLayouts.push_back(m_dummyLayouts[i]);
    pipelineDesc.globalBindingLayouts[DescriptorSetIDs::Globals] = m_globalBindingLayout;
    pipelineDesc.globalBindingLayouts[DescriptorSetIDs::Bindless] = m_bindlessLayout;

    pipelineDesc.shaders = {
        { "", shaderLibrary->getShader("RayGen", nvrhi::ShaderType::RayGeneration), nullptr },
        { "", shaderLibrary->getShader("Miss", nvrhi::ShaderType::Miss), nullptr },
        { "", shaderLibrary->getShader("ShadowMiss", nvrhi::ShaderType::Miss), nullptr }
    };

    pipelineDesc.hitGroups = { {
        "HitGroup",
        shaderLibrary->getShader("ClosestHit", nvrhi::ShaderType::ClosestHit),
        shaderLibrary->getShader("AnyHit", nvrhi::ShaderType::AnyHit),
        nullptr, // intersectionShader
        nullptr, // bindingLayout
        false  // isProceduralPrimitive
    },
    {
        "HitGroupShadow",
        shaderLibrary->getShader("ClosestHitShadow", nvrhi::ShaderType::ClosestHit),
        shaderLibrary->getShader("AnyHitShadow", nvrhi::ShaderType::AnyHit),
        nullptr, // intersectionShader
        nullptr, // bindingLayout
        false  // isProceduralPrimitive
    } };

    pipelineDesc.maxPayloadSize = sizeof(float) * 6;

#if ENABLE_DENOISER
    if (macroDefined("ENABLE_DENOISER"))
        pipelineDesc.globalBindingLayouts[DescriptorSetIDs::Denoiser] = m_denoiserBindingLayout;
#endif // ENABLE_DENOISER

#if ENABLE_NRC
    if (macroDefined("NRC_"))
        pipelineDesc.globalBindingLayouts[DescriptorSetIDs::Nrc] = m_nrcBindingLayout;
#endif // ENABLE_NRC

#if ENABLE_SHARC
    if (macroDefined("SHARC_"))
        pipelineDesc.globalBindingLayouts[DescriptorSetIDs::Sharc] = (m_sharcBindingLayout);
#endif // ENABLE_SHARC

    pipelinePermutation.pipeline = GetDevice()->createRayTracingPipeline(pipelineDesc);
    pipelinePermutation.shaderTable = pipelinePermutation.pipeline->createShaderTable();

    pipelinePermutation.shaderTable->setRayGenerationShader("RayGen");
    pipelinePermutation.shaderTable->addHitGroup("HitGroup");
    pipelinePermutation.shaderTable->addHitGroup("HitGroupShadow");
    pipelinePermutation.shaderTable->addMissShader("Miss");
    pipelinePermutation.shaderTable->addMissShader("ShadowMiss");

    return true;
}

void Pathtracer::GetMeshBlasDesc(engine::MeshInfo& mesh, nvrhi::rt::AccelStructDesc& blasDesc, bool skipTransmissiveMaterials) const
{
    blasDesc.isTopLevel = false;
    blasDesc.debugName = mesh.name;

    for (const auto& geometry : mesh.geometries)
    {
        nvrhi::rt::GeometryDesc geometryDesc;
        auto& triangles = geometryDesc.geometryData.triangles;
        triangles.indexBuffer = mesh.buffers->indexBuffer;
        triangles.indexOffset = (mesh.indexOffset + geometry->indexOffsetInMesh) * sizeof(uint32_t);
        triangles.indexFormat = nvrhi::Format::R32_UINT;
        triangles.indexCount = geometry->numIndices;
        triangles.vertexBuffer = mesh.buffers->vertexBuffer;
        triangles.vertexOffset = (mesh.vertexOffset + geometry->vertexOffsetInMesh) * sizeof(float3) + mesh.buffers->getVertexBufferRange(engine::VertexAttribute::Position).byteOffset;
        triangles.vertexFormat = nvrhi::Format::RGB32_FLOAT;
        triangles.vertexStride = sizeof(float3);
        triangles.vertexCount = geometry->numVertices;
        geometryDesc.geometryType = nvrhi::rt::GeometryType::Triangles;
        geometryDesc.flags = (geometry->material->domain != engine::MaterialDomain::Opaque)
            ? nvrhi::rt::GeometryFlags::None
            : nvrhi::rt::GeometryFlags::Opaque;

        blasDesc.bottomLevelGeometries.push_back(geometryDesc);
    }

    if (mesh.skinPrototype)
        blasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
    else
        blasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace | nvrhi::rt::AccelStructBuildFlags::AllowCompaction;
}

void Pathtracer::CreateAccelStructs(nvrhi::ICommandList* commandList)
{
    for (const auto& mesh : m_scene->GetSceneGraph()->GetMeshes())
    {
        if (mesh->buffers->hasAttribute(engine::VertexAttribute::JointWeights))
            continue;

        nvrhi::rt::AccelStructDesc blasDesc;
        GetMeshBlasDesc(*mesh, blasDesc, !m_ui.enableTransmission);

        nvrhi::rt::AccelStructHandle accelStruct = GetDevice()->createAccelStruct(blasDesc);

        if (!mesh->skinPrototype)
            nvrhi::utils::BuildBottomLevelAccelStruct(commandList, accelStruct, blasDesc);

        mesh->accelStruct = accelStruct;
    }

    nvrhi::rt::AccelStructDesc tlasDesc;
    tlasDesc.isTopLevel = true;
    tlasDesc.topLevelMaxInstances = m_scene->GetSceneGraph()->GetMeshInstances().size();
    m_topLevelAS = GetDevice()->createAccelStruct(tlasDesc);
}

void Pathtracer::BuildTLAS(nvrhi::ICommandList* commandList, uint32_t frameIndex) const
{
    {
        ScopedMarker scopedMarker(commandList, "Skinned BLAS Updates");

        // Transition all the buffers to their necessary states before building the BLAS'es to allow BLAS batching
        for (const auto& skinnedInstance : m_scene->GetSceneGraph()->GetSkinnedMeshInstances())
        {
            commandList->setAccelStructState(skinnedInstance->GetMesh()->accelStruct, nvrhi::ResourceStates::AccelStructWrite);
            commandList->setBufferState(skinnedInstance->GetMesh()->buffers->vertexBuffer, nvrhi::ResourceStates::AccelStructBuildInput);
        }
        commandList->commitBarriers();

        // Build BLAS instances
        for (const auto& skinnedInstance : m_scene->GetSceneGraph()->GetSkinnedMeshInstances())
        {
            nvrhi::rt::AccelStructDesc blasDesc;
            GetMeshBlasDesc(*skinnedInstance->GetMesh(), blasDesc, !m_ui.enableTransmission);

            nvrhi::utils::BuildBottomLevelAccelStruct(commandList, skinnedInstance->GetMesh()->accelStruct, blasDesc);
        }
    }

    std::vector<nvrhi::rt::InstanceDesc> instances;
    for (const auto& instance : m_scene->GetSceneGraph()->GetMeshInstances())
    {
        nvrhi::rt::InstanceDesc instanceDesc;
        instanceDesc.bottomLevelAS = instance->GetMesh()->accelStruct;
        instanceDesc.instanceMask = 1;
        instanceDesc.instanceID = instance->GetInstanceIndex();

        auto node = instance->GetNode();
        dm::affineToColumnMajor(node->GetLocalToWorldTransformFloat(), instanceDesc.transform);

        instances.push_back(instanceDesc);
    }

    // Compact acceleration structures that are tagged for compaction and have finished executing the original build
    commandList->compactBottomLevelAccelStructs();

    ScopedMarker scopedMarker(commandList, "TLAS Update");
    commandList->buildTopLevelAccelStruct(m_topLevelAS, instances.data(), instances.size());
}

void Pathtracer::BackBufferResizing()
{
    m_accumulationBuffer = nullptr;
    m_bindingCache->Clear();
    m_resetAccumulation = true;

    m_pathTracerOutputBuffer = nullptr;

#if ENABLE_DENOISER
    m_renderTargets = nullptr;
    m_nrd = nullptr;
    m_denoiserBindingSet = nullptr;
    m_denoiserOutBindingSet = nullptr;
#endif // ENABLE_DENOISER
}

void Pathtracer::Render(nvrhi::IFramebuffer* framebuffer)
{
    nvrhi::IDevice* device = GetDevice();
    const auto& fbInfo = framebuffer->getFramebufferInfo();

    m_scene->RefreshSceneGraph(GetFrameIndex());

    m_commandList->open();

    if (!m_pathTracerOutputBuffer || m_rebuildAS)
    {
        device->waitForIdle();

        if (m_rebuildAS)
            CreateAccelStructs(m_commandList);

        nvrhi::TextureDesc desc;
        desc.width = fbInfo.width;
        desc.height = fbInfo.height;
        desc.isUAV = true;
        desc.keepInitialState = true;
        desc.format = nvrhi::Format::RGBA32_FLOAT;
        desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        desc.debugName = "PathTracerOutput";
        m_pathTracerOutputBuffer = device->createTexture(desc);

        // TODO: create this binding set if something changes, like the scene
        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),
            nvrhi::BindingSetItem::ConstantBuffer(1, m_debugBuffer),
            nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_topLevelAS),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_scene->GetInstanceBuffer()),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_scene->GetGeometryBuffer()),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(3, m_scene->GetMaterialBuffer()),
            nvrhi::BindingSetItem::Sampler(0, m_CommonPasses->m_AnisotropicWrapSampler),
            nvrhi::BindingSetItem::Texture_UAV(0, m_pathTracerOutputBuffer),
        };

        m_globalBindingSet = device->createBindingSet(bindingSetDesc, m_globalBindingLayout);
    }
    m_rebuildAS = false;

    // Transition pathTracerOutput
    m_commandList->setTextureState(m_pathTracerOutputBuffer.Get(), nvrhi::TextureSubresourceSet(0, 1, 0, 1), nvrhi::ResourceStates::UnorderedAccess);
    m_commandList->commitBarriers();

    nvrhi::Viewport windowViewport(float(fbInfo.width), float(fbInfo.height));
    m_viewPrevious = m_view;
    m_view.SetViewport(windowViewport);
    m_view.SetMatrices(m_camera.GetWorldToViewMatrix(), perspProjD3DStyleReverse(dm::PI_f * 0.25f, windowViewport.width() / windowViewport.height(), 0.1f));
    m_view.UpdateCache();
    if (GetFrameIndex() == 0)
        m_viewPrevious = m_view;

    m_accumulatedFrameCount++;
    if (m_prevViewMatrix != m_view.GetViewMatrix())
    {
        m_resetAccumulation = true;
        m_prevViewMatrix = m_view.GetViewMatrix();
    }

    if (m_enableAnimations)
        m_resetAccumulation = true;

    if (m_resetAccumulation)
        m_accumulatedFrameCount = 1;

    m_scene->Refresh(m_commandList, GetFrameIndex());
    BuildTLAS(m_commandList, GetFrameIndex());

    LightingConstants constants = {};
    constants.skyColor = m_ui.enableSky ? float4(m_ui.skyColor * m_ui.skyIntensity, 1.0f) : float4(0.0f, 0.0f, 0.0f, 1.0f);
    m_view.FillPlanarViewConstants(constants.view);
    m_viewPrevious.FillPlanarViewConstants(constants.viewPrev);

    m_sunLight->FillLightConstants(constants.sunLight);

    // Add all lights
    constants.lightCount = 0;
    for (auto light : m_scene->GetSceneGraph()->GetLights())
    {
        if (constants.lightCount < MAX_LIGHTS)
            light->FillLightConstants(constants.lights[constants.lightCount++]);
    }

#if ENABLE_NRC
    // Update NRC
    if (m_ui.enableNrc)
    {
        // Check settings that would require NRC to be re-configured
        nrc::ContextSettings nrcContextSettings;
        nrcContextSettings.learnIrradiance = m_ui.nrcLearnIrradiance;
        nrcContextSettings.includeDirectLighting = m_ui.nrcIncludeDirectIllumination;

        int32_t frameWidth, frameHeight;
        GetDeviceManager()->GetWindowDimensions(frameWidth, frameHeight);
        nrcContextSettings.frameDimensions = { (uint32_t) frameWidth, (uint32_t) frameHeight };

        nrcContextSettings.trainingDimensions = nrc::computeIdealTrainingDimensions(nrcContextSettings.frameDimensions);
        nrcContextSettings.maxPathVertices = m_ui.bouncesMax;
        nrcContextSettings.samplesPerPixel = m_ui.samplesPerPixel;
        
        dm::box3 aabb = m_scene->GetSceneGraph()->GetRootNode()->GetGlobalBoundingBox();
        nrcContextSettings.sceneBoundsMin = { aabb.m_mins[0], aabb.m_mins[1], aabb.m_mins[2] };
        nrcContextSettings.sceneBoundsMax = { aabb.m_maxs[0], aabb.m_maxs[1], aabb.m_maxs[2] };
        //nrcContextSettings.collectAccurateLoss = false;

        if (nrcContextSettings != m_nrcContextSettings)
        {
            // The context settings have changed, so we need to re-configure NRC
            m_nrc->Configure(nrcContextSettings);
            m_nrcContextSettings = nrcContextSettings;

            // Create NVRHI binding set
            nvrhi::BindingSetDesc bindingSetDesc;
            bindingSetDesc.bindings = {
                nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_nrc->m_bufferHandles[nrc::BufferIdx::QueryPathInfo]),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(1, m_nrc->m_bufferHandles[nrc::BufferIdx::TrainingPathInfo]),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(2, m_nrc->m_bufferHandles[nrc::BufferIdx::TrainingPathVertices]),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(3, m_nrc->m_bufferHandles[nrc::BufferIdx::QueryRadianceParams]),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(4, m_nrc->m_bufferHandles[nrc::BufferIdx::Counter]),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(5, m_nrc->m_bufferHandles[nrc::BufferIdx::DebugTrainingPathInfo]),
            };
            m_nrcBindingSet = device->createBindingSet(bindingSetDesc, m_nrcBindingLayout);
        }

        // Settings expected to change frequently that do not require instance reset
        nrc::FrameSettings nrcPerFrameSettings;
        nrcPerFrameSettings.maxExpectedAverageRadianceValue = m_ui.nrcMaxAverageRadiance;
        nrcPerFrameSettings.terminationHeuristicThreshold = m_ui.nrcTerminationHeuristicThreshold;
        nrcPerFrameSettings.trainingTerminationHeuristicThreshold = m_ui.nrcTerminationHeuristicThreshold;
        nrcPerFrameSettings.resolveMode = m_ui.nrcResolveMode;

        m_nrc->BeginFrame(m_commandList, nrcPerFrameSettings);
        m_nrc->PopulateShaderConstants(constants.nrcConstants);
    }
#endif

#if ENABLE_SHARC
    if (m_ui.enableSharc)
    {
        static float3 sharcCameraPosition = m_view.GetViewOrigin();
        static float3 sharcCameraPositionPrev = m_view.GetViewOrigin();

        constants.sharcEntriesNum = m_sharcEntriesNum;
        constants.sharcDownscaleFactor = m_ui.sharcDownscaleFactor;
        constants.sharcSceneScale = m_ui.sharcSceneScale;
        constants.sharcRoughnessThreshold = m_ui.sharcRoughnessThreshold;
        constants.sharcCameraPositionPrev.xyz() = sharcCameraPositionPrev;
        constants.sharcCameraPosition.xyz() = sharcCameraPosition;
        constants.sharcAccumulationFrameNum = m_ui.sharcAccumulationFrameNum;
        constants.sharcStaleFrameNum = m_ui.sharcStaleFrameFrameNum;

        if (m_ui.sharcEnableUpdate)
        {
            sharcCameraPositionPrev = sharcCameraPosition;
            sharcCameraPosition = m_view.GetViewOrigin();
        }
    }
#endif // ENABLE_SHARC

    static bool enableDenoiser = false;
    bool skipDenoiser = m_ui.ptDebugOutput != PTDebugOutputType::None;
#if ENABLE_DENOISER
    bool resetDenoiser = enableDenoiser != m_ui.enableDenoiser;
    if (resetDenoiser && !skipDenoiser)
        CreateRayTracingPipelines();

    if (m_ui.enableDenoiser && !m_nrd)
    {
        // Create all the resources for the denoiser
        assert(!m_renderTargets && !m_denoiserBindingSet && !m_denoiserOutBindingSet);

        if (!m_renderTargets)
            m_renderTargets = std::make_unique<RenderTargets>(device, fbInfo.width, fbInfo.height);

        if (!m_nrd)
        {
            nrd::Denoiser denoiserMethod = nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
            m_nrd = std::make_unique<NrdIntegration>(device, denoiserMethod);
            m_nrd->Initialize(fbInfo.width, fbInfo.height, *m_shaderFactory);
        }

        nvrhi::BindingSetDesc bindingSetDesc;

        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::Texture_UAV(0, m_renderTargets->denoiserInDiffRadianceHitDist),
            nvrhi::BindingSetItem::Texture_UAV(1, m_renderTargets->denoiserInSpecRadianceHitDist),
            nvrhi::BindingSetItem::Texture_UAV(2, m_renderTargets->denoiserViewSpaceZ),
            nvrhi::BindingSetItem::Texture_UAV(3, m_renderTargets->denoiserNormalRoughness),
            nvrhi::BindingSetItem::Texture_UAV(4, m_renderTargets->denoiserMotionVectors),
            nvrhi::BindingSetItem::Texture_UAV(5, m_renderTargets->denoiserEmissive),
            nvrhi::BindingSetItem::Texture_UAV(6, m_renderTargets->denoiserDiffuseAlbedo),
            nvrhi::BindingSetItem::Texture_UAV(7, m_renderTargets->denoiserSpecularAlbedo),
        };

        m_denoiserBindingSet = device->createBindingSet(bindingSetDesc, m_denoiserBindingLayout);

        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::Texture_UAV(0, m_renderTargets->denoiserOutDiffRadianceHitDist),
            nvrhi::BindingSetItem::Texture_UAV(1, m_renderTargets->denoiserOutSpecRadianceHitDist),
            nvrhi::BindingSetItem::Texture_UAV(2, m_renderTargets->denoiserViewSpaceZ),
            nvrhi::BindingSetItem::Texture_UAV(3, m_renderTargets->denoiserNormalRoughness),
            nvrhi::BindingSetItem::Texture_UAV(4, m_renderTargets->denoiserMotionVectors),
            nvrhi::BindingSetItem::Texture_UAV(5, m_renderTargets->denoiserEmissive),
            nvrhi::BindingSetItem::Texture_UAV(6, m_renderTargets->denoiserDiffuseAlbedo),
            nvrhi::BindingSetItem::Texture_UAV(7, m_renderTargets->denoiserSpecularAlbedo),
        };

        m_denoiserOutBindingSet = device->createBindingSet(bindingSetDesc, m_denoiserBindingLayout);
    }
    enableDenoiser = m_ui.enableDenoiser && !skipDenoiser;
#endif // ENABLE_DENOISER

    m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

    GlobalConstants globalConstants = {};
    globalConstants.enableJitter = (m_ui.enableJitter && !enableDenoiser) || (m_ui.enableJitter && enableDenoiser && m_ui.enableAccumulation);
    globalConstants.enableBackFaceCull = m_ui.enableBackFaceCull;
    globalConstants.bouncesMax = m_ui.bouncesMax;
    globalConstants.frameIndex = m_frameIndex++;
    globalConstants.enableAccumulation = m_ui.enableAccumulation;
    globalConstants.accumulatedFramesMax = m_resetAccumulation ? 1 : m_ui.accumulatedFramesMax;
    globalConstants.recipAccumulatedFrames = m_ui.enableAccumulation ? (1.0f / (float) m_accumulatedFrameCount) : 1.0f;
    globalConstants.intensityScale = 1.0f;
    globalConstants.enableEmissives = m_ui.enableEmissives;
    globalConstants.enableLighting = m_ui.enableLighting;
    globalConstants.enableTransmission = m_ui.enableTransmission;
    globalConstants.enableAbsorbtion = m_ui.enableAbsorbtion;
    globalConstants.enableTransparentShadows = m_ui.enableTransparentShadows;
    globalConstants.enableSoftShadows = m_ui.enableSoftShadows;
    globalConstants.throughputThreshold = m_ui.throughputThreshold;
    globalConstants.enableRussianRoulette = m_ui.enableRussianRoulette;
    globalConstants.samplesPerPixel = m_ui.samplesPerPixel;
    globalConstants.exposureScale = donut::math::exp2f(m_ui.exposureAdjustment);
    globalConstants.roughnessMin = m_ui.roughnessMin;
    globalConstants.roughnessMax = std::max(m_ui.roughnessMin, m_ui.roughnessMax);
    globalConstants.metalnessMin = m_ui.metalnessMin;
    globalConstants.metalnessMax = std::max(m_ui.metalnessMin, m_ui.metalnessMax);

    globalConstants.clamp = (uint)m_ui.toneMappingClamp;
    globalConstants.toneMappingOperator = (uint)m_ui.toneMappingOperator;

    globalConstants.targetLight = m_ui.targetLight;
    globalConstants.debugOutputMode = (uint)m_ui.ptDebugOutput;

#if ENABLE_NRC
    globalConstants.nrcSkipDeltaVertices = m_ui.nrcSkipDeltaVertices;
    globalConstants.nrcTerminationHeuristicThreshold = m_ui.nrcTerminationHeuristicThreshold;
#endif // ENABLE_NRC

#if ENABLE_SHARC
    globalConstants.sharcDebug = m_ui.sharcEnableDebug;
#endif // ENABLE_SHARC

#if ENABLE_DENOISER
    if (enableDenoiser)
    {
        globalConstants.samplesPerPixel = m_ui.samplesPerPixel;

        nrd::HitDistanceParameters hitDistanceParameters;
        globalConstants.nrdHitDistanceParams = (float4&)hitDistanceParameters;
    }
#endif // ENABLE_DENOISER

    m_commandList->writeBuffer(m_debugBuffer, &globalConstants, sizeof(globalConstants));
    m_commandList->clearState();

    nvrhi::rt::State state;
    for (int i = 0; i < DescriptorSetIDs::COUNT; ++i) 
        state.bindings.push_back(m_dummyBindingSets[i]); // Unified Binding
    state.bindings[DescriptorSetIDs::Globals]  = m_globalBindingSet;
    state.bindings[DescriptorSetIDs::Bindless] = (m_descriptorTable->GetDescriptorTable());

#if ENABLE_DENOISER
    if (enableDenoiser)
    {
        assert(m_denoiserBindingSet);
        state.bindings[DescriptorSetIDs::Denoiser] = m_denoiserBindingSet;

        m_commandList->clearTextureFloat(m_renderTargets->denoiserViewSpaceZ, nvrhi::AllSubresources, nvrhi::Color(0.0f));
    }
#endif // ENABLE_DENOISER

    bool runReferencePathTracer = true;
#if ENABLE_NRC

    if (m_ui.enableNrc)
    {
        ScopedMarker scopedMarker(m_commandList, "Nrc");

        runReferencePathTracer = false;

        assert(m_nrcBindingSet);
        state.bindings[DescriptorSetIDs::Nrc] = m_nrcBindingSet;
        {
            nvrhi::rt::DispatchRaysArguments args;
            ScopedMarker scopedMarker(m_commandList, "NrcUpdateAndQueryRT");

            // NRC query
            if (m_denoiserBindingSet && enableDenoiser)
                state.bindings[DescriptorSetIDs::Denoiser] = m_denoiserBindingSet;
            
            state.shaderTable = m_pipelinePermutations[PipelineType::NRC_Query].shaderTable;
            m_commandList->setRayTracingState(state);
            args.width = fbInfo.width;
            args.height = fbInfo.height;
            m_commandList->dispatchRays(args);
            // There is no dependency between the two
            // raygens, so a barrier is not required here.
            // NVRHI sees that we're using the same UAVs and inserts UAV barriers.
            // Fortunately, we can tell it that we know better.
            m_commandList->setEnableAutomaticBarriers(false);

            // NRC update
            if (m_ui.nrcTrainCache)
            {
                state.bindings[DescriptorSetIDs::Denoiser] = m_dummyBindingSets[DescriptorSetIDs::Denoiser];
                state.shaderTable = m_pipelinePermutations[PipelineType::NRC_Update].shaderTable;
                m_commandList->setRayTracingState(state);
                args.width = m_nrcContextSettings.trainingDimensions.x;
                args.height = m_nrcContextSettings.trainingDimensions.y;
                m_commandList->dispatchRays(args);
                m_commandList->setEnableAutomaticBarriers(true);
            }
        }

        {
            ScopedMarker scopedMarker(m_commandList, "NrcQueryAndTrain");
            m_nrc->QueryAndTrain(m_commandList, m_ui.nrcCalculateTrainingLoss);
        }

        if (m_ui.ptDebugOutput == PTDebugOutputType::None)
        {
            ScopedMarker scopedMarker(m_commandList, "NrcResolve");
            m_nrc->Resolve(m_commandList, m_pathTracerOutputBuffer);
        }
    }

    // Reset heap
    m_commandList->clearState();
#endif // ENABLE_NRC

#if ENABLE_SHARC
    if (m_ui.enableSharc)
    {
        ScopedMarker scopedMarker(m_commandList, "Sharc");

        runReferencePathTracer = false;

        state.bindings[DescriptorSetIDs::Sharc] = m_sharcBindingSet;

        if (m_ui.sharcEnableUpdate)
        {
            if (m_ui.sharcEnableClear || m_sceneReloaded)
            {
                m_commandList->clearBufferUInt(m_sharcHashEntriesBuffer, m_sharcInvalidEntry);
                m_commandList->clearBufferUInt(m_sharcCopyOffsetBuffer, 0);
                m_commandList->clearBufferUInt(m_sharcVoxelDataBuffer, 0);
                m_commandList->clearBufferUInt(m_sharcVoxelDataBufferPrev, 0);
            }

            if (m_ui.sharcEnableResolve)
            {
                std::swap(m_sharcVoxelDataBuffer, m_sharcVoxelDataBufferPrev);
                std::swap(m_sharcBindingSet, m_sharcBindingSetSwapped);

                m_commandList->clearBufferUInt(m_sharcVoxelDataBuffer, 0);
            }

            // SHARC update
            {
                // Update never uses denoiser; set to dummy.
                state.bindings[DescriptorSetIDs::Denoiser] = m_dummyBindingSets[DescriptorSetIDs::Denoiser];
                state.bindings[DescriptorSetIDs::Sharc] = m_sharcBindingSet;

                state.shaderTable = m_pipelinePermutations[PipelineType::Sharc_Update].shaderTable;
                m_commandList->setRayTracingState(state);

                nvrhi::rt::DispatchRaysArguments args;
                args.width = fbInfo.width / m_ui.sharcDownscaleFactor;
                args.height = fbInfo.height / m_ui.sharcDownscaleFactor;

                ScopedMarker scopedMarker(m_commandList, "SharcUpdate");
                m_commandList->dispatchRays(args);
            }

            if (m_ui.sharcEnableResolve)
            {
                nvrhi::ComputeState computeState;
                // Unified Binding
                if (m_api == nvrhi::GraphicsAPI::D3D12)
                    computeState.bindings = { m_globalBindingSet, m_sharcBindingSet };
                else
                    computeState.bindings = { m_globalBindingSet, m_dummyBindingSets[1], m_dummyBindingSets[2], m_sharcBindingSet };

                // SHARC resolve
                {
                    computeState.pipeline = m_sharcResolvePSO;
                    m_commandList->setComputeState(computeState);

                    const uint groupSize = 256;
                    const dm::uint2 dispatchSize = { DivideRoundUp(m_sharcEntriesNum, groupSize), 1 };

                    ScopedMarker scopedMarker(m_commandList, "SharcResolve");
                    m_commandList->dispatch(dispatchSize.x, dispatchSize.y);
                }

                // SHARC compaction
                {
                    computeState.pipeline = m_sharcHashCopyPSO;
                    m_commandList->setComputeState(computeState);

                    const uint groupSize = 256;
                    const dm::uint2 dispatchSize = { DivideRoundUp(m_sharcEntriesNum, groupSize), 1 };
                    ScopedMarker scopedMarker(m_commandList, "SharcCompaction");
                    m_commandList->dispatch(dispatchSize.x, dispatchSize.y);
                }
            }
        }

        // Unified Binding
        if (m_denoiserBindingSet && enableDenoiser)
            state.bindings[DescriptorSetIDs::Denoiser]= m_denoiserBindingSet;

        // SHARC query
        {
            state.shaderTable = m_pipelinePermutations[PipelineType::Sharc_Query].shaderTable;
            m_commandList->setRayTracingState(state);

            nvrhi::rt::DispatchRaysArguments args;
            args.width = fbInfo.width;
            args.height = fbInfo.height;
            ScopedMarker scopedMarker(m_commandList, "SharcQuery");
            m_commandList->dispatchRays(args);
        }
    }
#endif // ENABLE_SHARC

    if (runReferencePathTracer)
    {
        state.shaderTable = m_pipelinePermutations[PipelineType::DefaultPathTracing].shaderTable;
        m_commandList->setRayTracingState(state);

        nvrhi::rt::DispatchRaysArguments args;
        args.width = fbInfo.width;
        args.height = fbInfo.height;
        ScopedMarker scopedMarker(m_commandList, "ReferencePathTracer");
        m_commandList->dispatchRays(args);
    }

#if ENABLE_DENOISER
    if (enableDenoiser)
    {
        // Denoiser data packing
        {
            nvrhi::ComputeState computeState;
            computeState.bindings = { m_globalBindingSet, m_denoiserBindingSet };
            computeState.pipeline = m_ui.enableNrc ? m_denoiserReblurPack_NRC_PSO : m_denoiserReblurPackPSO;
            m_commandList->setComputeState(computeState);

            const uint groupSize = 16;
            const dm::uint2 dispatchSize = { DivideRoundUp(fbInfo.width, groupSize), DivideRoundUp(fbInfo.height, groupSize) };
            m_commandList->dispatch(dispatchSize.x, dispatchSize.y);
        }

        nrd::ReblurSettings reblurSettings = NrdConfig::GetDefaultREBLURSettings();
        m_nrd->RunDenoiserPasses(m_commandList, *m_renderTargets, 0, m_view, m_viewPrevious, GetFrameIndex(), 0.01f, 0.05f, false, false, &reblurSettings, resetDenoiser);

        // Denoiser resolve
        {
            nvrhi::ComputeState computeState;
            computeState.bindings = { m_globalBindingSet, m_denoiserOutBindingSet };
            computeState.pipeline = m_denoiserResolvePSO;
            m_commandList->setComputeState(computeState);

            const uint groupSize = 16;
            const dm::uint2 dispatchSize = { DivideRoundUp(fbInfo.width, groupSize), DivideRoundUp(fbInfo.height, groupSize) };
            m_commandList->dispatch(dispatchSize.x, dispatchSize.y);
        }
    }
#endif // ENABLE_DENOISER

    // Accumulation and tonemapping
    {
        if (!m_accumulationBuffer)
        {
            nvrhi::TextureDesc desc;
            desc.width = fbInfo.width;
            desc.height = fbInfo.height;
            desc.isUAV = true;
            desc.keepInitialState = true;
            desc.format = nvrhi::Format::RGBA32_FLOAT;
            desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            desc.debugName = "AccumulationBuffer";
            m_accumulationBuffer = device->createTexture(desc);
        }

        if (!m_tonemappingPSO)
        {
            nvrhi::GraphicsPipelineDesc pipelineDesc;
            pipelineDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
            pipelineDesc.VS = m_CommonPasses->m_FullscreenVS;
            pipelineDesc.PS = m_tonemappingPS;
            pipelineDesc.bindingLayouts = { m_tonemappingBindingLayout };

            pipelineDesc.renderState.rasterState.setCullNone();
            pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
            pipelineDesc.renderState.depthStencilState.stencilEnable = false;

            m_tonemappingPSO = device->createGraphicsPipeline(pipelineDesc, framebuffer);
        }

        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_debugBuffer),
            nvrhi::BindingSetItem::Texture_UAV(0, m_pathTracerOutputBuffer),
            nvrhi::BindingSetItem::Texture_UAV(1, m_accumulationBuffer)
        };

        m_tonemappingBindingSet = device->createBindingSet(bindingSetDesc, m_tonemappingBindingLayout);

        nvrhi::GraphicsState state;
        state.pipeline = m_tonemappingPSO;
        state.framebuffer = framebuffer;
        state.bindings = { m_tonemappingBindingSet };
        state.viewport = m_view.GetViewportState();

        m_commandList->setGraphicsState(state);

        nvrhi::DrawArguments args;
        args.instanceCount = 1;
        args.vertexCount = 4;
        m_commandList->draw(args);
    }

    m_commandList->close();
    device->executeCommandList(m_commandList);

    m_resetAccumulation = false;
    m_sceneReloaded = false;

#if ENABLE_NRC
    if (m_ui.enableNrc)
    {
        if (m_api == nvrhi::GraphicsAPI::D3D12)
            m_nrc->EndFrame(device->getNativeQueue(nvrhi::ObjectTypes::D3D12_CommandQueue, nvrhi::CommandQueue::Graphics));
        else if (m_api == nvrhi::GraphicsAPI::VULKAN)
            m_nrc->EndFrame(device->getNativeQueue(nvrhi::ObjectTypes::VK_Queue, nvrhi::CommandQueue::Graphics));
    }
#endif // ENABLE_NRC
}

std::shared_ptr<donut::engine::ShaderFactory> Pathtracer::GetShaderFactory()
{
    return m_shaderFactory;
}

std::shared_ptr<vfs::IFileSystem> Pathtracer::GetRootFS() const
{
    return m_rootFileSystem;
}

std::shared_ptr<TextureCache> Pathtracer::GetTextureCache()
{
    return m_TextureCache;
}

std::shared_ptr<donut::engine::Scene> Pathtracer::GetScene() const
{
    return m_scene;
}

void Pathtracer::ResetAccumulation()
{
    m_resetAccumulation = true;
}

void Pathtracer::RebuildAccelerationStructure()
{
    m_rebuildAS = true;
}

FirstPersonCamera* Pathtracer::GetCamera()
{
    return &m_camera;
}

std::string Pathtracer::GetResolutionInfo()
{
    if (m_pathTracerOutputBuffer)
    {
        uint2 resolution = uint2(m_pathTracerOutputBuffer.Get()->getDesc().width, m_pathTracerOutputBuffer.Get()->getDesc().height);
        return std::to_string(resolution.x) + " x " + std::to_string(resolution.y);
    }

    return "uninitialized";
}

#ifdef WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int __argc, const char** __argv)
#endif
{
    nvrhi::GraphicsAPI api = app::GetGraphicsAPIFromCommandLine(__argc, __argv);
    app::DeviceManager* deviceManager = app::DeviceManager::Create(api);

    bool disableNrc = false;
    app::DeviceCreationParameters deviceParams;
    deviceParams.enableRayTracingExtensions = true;
    deviceParams.startFullscreen = false;
    deviceParams.backBufferWidth = 1920;
    deviceParams.backBufferHeight = 1080;

    for (int n = 1; n < __argc; n++)
    {
        const char* arg = __argv[n];

        if (!strcmp(arg, "-fullscreen"))
            deviceParams.startFullscreen = true;
        else if (!strcmp(arg, "-disablenrc"))
            disableNrc = true;
        else if (!strcmp(arg, "-width"))
            deviceParams.backBufferWidth = atoi(__argv[n + 1]);
        else if (!strcmp(arg, "-height"))
            deviceParams.backBufferHeight = atoi(__argv[n + 1]);
    }

#ifdef _DEBUG
    deviceParams.enableDebugRuntime = true;
    deviceParams.enableNvrhiValidationLayer = true;
#endif

    if (api == nvrhi::GraphicsAPI::VULKAN)
    {
#if ENABLE_NRC
#ifdef NRC_WITH_VULKAN
        if (!disableNrc)
        {
            char const* const* nrcDeviceExtensions;
            uint32_t numNrcDeviceExtensions = nrc::vulkan::GetVulkanDeviceExtensions(nrcDeviceExtensions);
            for (uint32_t i = 0; i < numNrcDeviceExtensions; ++i)
                deviceParams.requiredVulkanDeviceExtensions.push_back(nrcDeviceExtensions[i]);

            char const* const* nrcInstanceExtensions;
            uint32_t numNrcInstanceExtensions = nrc::vulkan::GetVulkanInstanceExtensions(nrcInstanceExtensions);
            for (uint32_t i = 0; i < numNrcInstanceExtensions; ++i)
                deviceParams.requiredVulkanInstanceExtensions.push_back(nrcInstanceExtensions[i]);

            deviceParams.requiredVulkanDeviceExtensions.push_back(VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME);
            deviceParams.requiredVulkanDeviceExtensions.push_back(VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME);
        }
#endif // NRC_WITH_VULKAN
#endif // ENABLE_NRC

        // Option used by SHARC
        {
            deviceParams.requiredVulkanDeviceExtensions.push_back(VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME);
            deviceParams.requiredVulkanDeviceExtensions.push_back(VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME);
        }
    }

    deviceParams.deviceCreateInfoCallback = &InjectFeatures;

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, g_WindowTitle))
    {
        log::fatal("Cannot initialize a graphics device with the requested parameters");
        return 1;
    }

    if (!deviceManager->GetDevice()->queryFeatureSupport(nvrhi::Feature::RayTracingPipeline))
    {
        log::fatal("The graphics device does not support Ray Tracing Pipelines");
        return 1;
    }

    {
        UIData uiData;
        Pathtracer demo(deviceManager, uiData, api);
        if (demo.Init(__argc, __argv))
        {
            PathtracerUI gui(deviceManager, demo, uiData);
            gui.Init(demo.GetShaderFactory());

            deviceManager->AddRenderPassToBack(&demo);
            deviceManager->AddRenderPassToBack(&gui);

            deviceManager->RunMessageLoop();

            deviceManager->RemoveRenderPass(&gui);
            deviceManager->RemoveRenderPass(&demo);
        }
    }

    deviceManager->Shutdown();

    delete deviceManager;

    return 0;
}
