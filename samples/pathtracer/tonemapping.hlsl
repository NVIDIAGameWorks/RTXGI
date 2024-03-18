/*
 * Copyright (c) 2019-2024, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#pragma pack_matrix(row_major)

#include "global_cb.h"

ConstantBuffer<GlobalConstants>     g_Global                    : register(b0);
RWTexture2D<float4>                 pathTracingOutputBuffer     : register(u0);
RWTexture2D<float4>                 accumulationBuffer          : register(u1);

float CalculateLuminance(float3 color)
{
    return dot(color, float3(0.299f, 0.587f, 0.114f));
}

float3 ToneMapLinear(float3 color)
{
    return color;
}

float3 ToneMapReinhard(float3 color)
{
    float luminance = CalculateLuminance(color);
    float reinhard = luminance / (luminance + 1.0f);

    return color * (reinhard / luminance);
}

float3 ToneMap(float3 color)
{
	float3 outColor = color * g_Global.exposureScale;

    if (g_Global.toneMappingOperator == 0 /* Linear */)
        outColor = ToneMapLinear(outColor);
    else if (g_Global.toneMappingOperator == 1 /* Reinhard */)
        outColor = ToneMapReinhard(outColor);
		
    return outColor;
}

void main_ps(in float4 pos: SV_Position, in float2 uv: UV, out float4 output: SV_Target)
{
    float4 color = pathTracingOutputBuffer[pos.xy];
    if (g_Global.enableAccumulation)
    {
        if (g_Global.recipAccumulatedFrames < 1.0f)
        {
            float4 accumulatedColor = accumulationBuffer[pos.xy];
            color = lerp(accumulatedColor, color, g_Global.recipAccumulatedFrames);
        }
        accumulationBuffer[pos.xy] = color;
    }

    color.xyz = ToneMap(color.xyz);
	
    if (g_Global.clamp)
        color = saturate(color);

    output = float4(color);
}