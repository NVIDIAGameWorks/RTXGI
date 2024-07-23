/*
 * Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#include "global_cb.h"
#include "lighting_cb.h"

#include "SharcCommon.h"

#define LINEAR_BLOCK_SIZE           256

ConstantBuffer<LightingConstants>   g_Lighting                  : register(b0, space0);
ConstantBuffer<GlobalConstants>     g_Global                    : register(b1, space0);

RWStructuredBuffer<uint64_t>        u_SharcHashEntriesBuffer    : register(u0, space3);
RWStructuredBuffer<uint>            u_HashCopyOffsetBuffer      : register(u1, space3);
RWStructuredBuffer<uint4>           u_SharcVoxelDataBuffer      : register(u2, space3);
RWStructuredBuffer<uint4>           u_SharcVoxelDataBufferPrev  : register(u3, space3);

[numthreads(LINEAR_BLOCK_SIZE, 1, 1)]
void sharcCompaction(in uint2 did : SV_DispatchThreadID)
{
    HashMapData hashMapData;
    hashMapData.capacity = g_Lighting.sharcEntriesNum;
    hashMapData.hashEntriesBuffer = u_SharcHashEntriesBuffer;

    SharcCopyHashEntry(did.x, hashMapData, u_HashCopyOffsetBuffer);
}

[numthreads(LINEAR_BLOCK_SIZE, 1, 1)]
void sharcResolve(in uint2 did : SV_DispatchThreadID)
{
    GridParameters gridParameters;
    gridParameters.cameraPosition = g_Lighting.sharcCameraPosition.xyz;
    gridParameters.cameraPositionPrev = g_Lighting.sharcCameraPositionPrev.xyz;
    gridParameters.sceneScale = g_Lighting.sharcSceneScale;
    gridParameters.logarithmBase = SHARC_GRID_LOGARITHM_BASE;

    HashMapData hashMapData;
    hashMapData.capacity = g_Lighting.sharcEntriesNum;
    hashMapData.hashEntriesBuffer = u_SharcHashEntriesBuffer;

    SharcResolveEntry(did.x, gridParameters, hashMapData, u_HashCopyOffsetBuffer, u_SharcVoxelDataBuffer, u_SharcVoxelDataBufferPrev, g_Lighting.sharcAccumulationFrameNum, g_Lighting.sharcStaleFrameNum);
}