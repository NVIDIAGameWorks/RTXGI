#pragma once
/*
 * Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
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

#define EXIT_MAX_BOUNCE 0
#define EXIT_HIT_SKY 1
#define EXIT_RUSSIAN_ROULETTE 2
#define EXIT_WITHIN_SURFACE 3
#define EXIT_SMALL_THROUGHPUT 4

// Set to 1 to over-ride the UI
#define USE_DEFAULT_SETTINGS 0

#endif