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

#include <donut/engine/SceneTypes.h>
#include <donut/engine/SceneGraph.h>
#include <donut/app/DeviceManager.h>
#include <donut/app/imgui_renderer.h>
#include <donut/app/imgui_console.h>

#define ENABLE_NRC          1
#define ENABLE_SHARC        1
#define ENABLE_DENOISER     1

#if ENABLE_NRC
#include "NrcIntegration.h"
#endif // ENABLE_NRC

enum class PTDebugOutputType : uint32_t
{
    None = 0,
    DiffuseReflectance = 1,
    WorldSpaceNormals = 2,
    WorldSpacePosition = 3,
    Barycentrics = 4,
    HitT = 5,
    InstanceID = 6,
    Emissives = 7,
    BounceHeatmap = 8,
};

enum TechSelection :  uint32_t
{
    None = 0, 
    Nrc = 1,
    Sharc = 2,
};

#if ENABLE_DENOISER
enum class DenoiserSelection : uint32_t
{
    None = 0,
    Nrd = 1,
};
#endif

enum class ToneMappingOperator : uint32_t
{
    Linear = 0,
    Reinhard = 1,
};

struct UIData
{
    bool                    showUI = true;
    bool                    enableAnimations = false;
    bool                    enableJitter = true;
    bool                    enableTransmission = false;
    bool                    enableBackFaceCull = true;
    int                     bouncesMax = 8;
    bool                    enableAccumulation = false;
    int                     accumulatedFrames = 1;
    int                     accumulatedFramesMax = 128;
    float                   exposureAdjustment = 0.0f;
    float                   roughnessMin = 0.0f;
    float                   roughnessMax = 1.0f;
    float                   metalnessMin = 0.0f;
    float                   metalnessMax = 1.0f;
    bool                    enableSky = true;
    bool                    enableEmissives = true;
    bool                    enableLighting = true;
    bool                    enableAbsorbtion = true;
    bool                    enableTransparentShadows = true;
    bool                    enableSoftShadows = true;
    float                   throughputThreshold = 0.01f;
    bool                    enableRussianRoulette = true;
    dm::float3              skyColor = dm::float3(0.5f, 0.75f, 1.0f);
    float                   skyIntensity = 2.f;
    int                     samplesPerPixel = 1;
    int                     targetLight = 0;
    bool                    enableTonemapping = true;

    TechSelection           currentMode = TechSelection::None;
    bool                    toneMappingClamp = true;
    ToneMappingOperator     toneMappingOperator = ToneMappingOperator::Reinhard;
    const char*             toneMappingOperatorStrings = "Linear\0Reinhard\0";

    bool                    enableNrc = false;
#if ENABLE_NRC
    bool                    nrcLearnIrradiance = true;
    bool                    nrcIncludeDirectIllumination = true;
    bool                    nrcTrainCache = true;
    bool                    nrcCalculateTrainingLoss = false;
    float                   nrcMaxAverageRadiance = 1.0f;
    NrcResolveMode          nrcResolveMode = NrcResolveMode::AddQueryResultToOutput;

    uint32_t                nrcTrainingWidth = 0;
    uint32_t                nrcTrainingHeight = 0;

    // Settings used by the path tracer
    bool                    nrcSkipDeltaVertices = false;
    float                   nrcTerminationHeuristicThreshold = 0.01f;
    bool                    nrcWriteDebugBuffers = false;
    bool                    nrcCopyDebugReadbackBuffers = false;
    bool                    nrcDebugBuffersHaveBeenCopied = false;
    int                     debugPixelPickerSampleIndex = 0;
    ImVec2                  debugPixel = {0, 0};
#endif // ENABLE_NRC

#if ENABLE_SHARC
    bool                    enableSharc = false;
    bool                    sharcEnableClear = false;
    bool                    sharcEnableUpdate = true;
    bool                    sharcEnableResolve = true;
    bool                    sharcEnableDebug = false;
    int                     sharcDownscaleFactor = 5;
    float                   sharcSceneScale = 50.0f;
    int                     sharcAccumulationFrameNum = 10;
    int                     sharcStaleFrameFrameNum = 64;
    float                   sharcRoughnessThreshold = 0.4f;
#endif // ENABLE_SHARC

#if ENABLE_DENOISER
    bool                    enableDenoiser = false;
    DenoiserSelection       denoiserSelection = DenoiserSelection::None;
    const char*             denoiserSelectionStrings = "None\0NRD\0";
#endif // ENABLE_DENOISER

    std::shared_ptr<donut::engine::Material> selectedMaterial = nullptr;
    std::shared_ptr<donut::engine::SceneCamera> activeSceneCamera = nullptr;

    PTDebugOutputType ptDebugOutput = PTDebugOutputType::None;
    const char* ptDebugOutputTypeStrings = "None\0Diffuse Reflectance\0Worldspace Normals\0Worldspace Position\0Barycentrics\0HitT\0InstanceID\0Emissives\0Bounce Heatmap\0";
};

class PathtracerUI : public donut::app::ImGui_Renderer
{
public:
    PathtracerUI(donut::app::DeviceManager* deviceManager, class Pathtracer& app, UIData& ui);
    virtual ~PathtracerUI();
protected:
    virtual void buildUI(void) override;
private:
    class Pathtracer& m_app;
    UIData& m_ui;

    ImFont* m_fontOpenSans = nullptr;
    ImFont* m_fontDroidMono = nullptr;

    std::shared_ptr<donut::engine::Light> m_selectedLight;
    int m_selectedLightIndex = 0;

    nvrhi::CommandListHandle m_commandList;
};