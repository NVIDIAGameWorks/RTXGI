/*
 * Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#ifndef LIGHTING_CB_H
#define LIGHTING_CB_H

#include <donut/shaders/light_cb.h>
#include <donut/shaders/view_cb.h>

// Does not affect local lights shading
#define ENABLE_SPECULAR_LOBE 1

#if ENABLE_NRC
#define NRC_RW_STRUCTURED_BUFFER(T) RWStructuredBuffer<T>
#include "NRCStructures.h"
#endif // ENABLE_NRC

#define MAX_LIGHTS 8

struct LightingConstants
{
    float4 skyColor;

    int lightCount;
    int sharcAccumulationFrameNum;
    int sharcStaleFrameNum;
    int sharcEnableAntifirefly;

    int sharcEntriesNum;
    int sharcDownscaleFactor;
    float sharcSceneScale;
    float sharcRoughnessThreshold;

    float4 sharcCameraPosition;
    float4 sharcCameraPositionPrev;

    PlanarViewConstants view;
    PlanarViewConstants viewPrev;
    PlanarViewConstants updatePassView;

    LightConstants sunLight;
    LightConstants headLight;
    LightConstants lights[MAX_LIGHTS];

#if ENABLE_NRC
    NrcConstants nrcConstants;
#endif // ENABLE_NRC
};

#endif // LIGHTING_CB_H