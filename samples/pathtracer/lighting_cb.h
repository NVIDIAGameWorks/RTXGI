/*
* Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
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
    int padding_0;

    int sharcEntriesNum;
    int sharcDownscaleFactor;
    float sharcSceneScale;
    float sharcRoughnessThreshold;

    float4 sharcCameraPosition;
    float4 sharcCameraPositionPrev;

    PlanarViewConstants view;
    PlanarViewConstants viewPrev;

    LightConstants sunLight;
    LightConstants headLight;
    LightConstants lights[MAX_LIGHTS];

#if ENABLE_NRC
    NrcConstants nrcConstants;
#endif // ENABLE_NRC
};

#endif // LIGHTING_CB_H