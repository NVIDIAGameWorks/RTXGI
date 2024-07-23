#pragma once
/*
* Copyright (c) 2014-2024, NVIDIA CORPORATION. All rights reserved.
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

#ifndef GLOBAL_CB_H
#define GLOBAL_CB_H

struct GlobalConstants
{
    int enableJitter;
    int sharcDebug;
    int enableBackFaceCull;
    int bouncesMax;

    int frameIndex;
    uint enableAccumulation;
    float recipAccumulatedFrames;
    int accumulatedFramesMax;

    int enableEmissives;
    int enableLighting;
    int enableTransmission;
    int enableOcclusion;

    int enableAbsorbtion;
    int enableTransparentShadows;
    int enableSoftShadows;
    int enableRussianRoulette;

    int samplesPerPixel;
    int targetLight;
    uint debugOutputMode;
    float intensityScale;

    float throughputThreshold;
    float exposureScale;
    uint toneMappingOperator;
    uint clamp;

    uint nrcEnableTerminationHeuristic;
    uint nrcSkipDeltaVertices;
    uint pad0;
    float nrcTerminationHeuristicThreshold;

    float4 nrdHitDistanceParams;

    float roughnessMin;
    float roughnessMax;
    float metalnessMin;
    float metalnessMax;
};

#define EXIT_MAX_BOUNCE         0
#define EXIT_HIT_SKY            1
#define EXIT_RUSSIAN_ROULETTE   2
#define EXIT_WITHIN_SURFACE     3
#define EXIT_SMALL_THROUGHPUT   4

// Set to 1 to over-ride the UI
#define USE_DEFAULT_SETTINGS    0

#endif