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

#include "NRD/NRD.hlsli"

#define BLOCK_SIZE 16

ConstantBuffer<LightingConstants>   g_Lighting                  : register(b0, space0);
ConstantBuffer<GlobalConstants>     g_Global                    : register(b1, space0);

RWTexture2D<float4>             u_Output                        : register(u0, space0);

RWTexture2D<float4>             u_OutputDiffuseHitDistance      : register(u0, space1);
RWTexture2D<float4>             u_OutputSpecularHitDistance     : register(u1, space1);
RWTexture2D<float>              u_OutputViewSpaceZ              : register(u2, space1);
RWTexture2D<float4>             u_OutputNormalRoughness         : register(u3, space1);
RWTexture2D<float4>             u_OutputMotionVectors           : register(u4, space1);
RWTexture2D<float4>             u_OutputEmissive                : register(u5, space1);
RWTexture2D<float4>             u_OutputDiffuseAlbedo           : register(u6, space1);
RWTexture2D<float4>             u_OutputSpecularAlbedo          : register(u7, space1);

[numthreads(BLOCK_SIZE, BLOCK_SIZE, 1)]
void reblurPackData(in uint2 did : SV_DispatchThreadID)
{
    float viewSpaceZ = u_OutputViewSpaceZ[did];
    if (viewSpaceZ == 0)
        return;

    float4 normalRoughness = u_OutputNormalRoughness[did];
    float3 emissive = u_OutputEmissive[did].xyz;

#if ENABLE_NRC
    float3 nrcRadiance = u_Output[did].xyz;
#endif // ENABLE_NRC

    // Diffuse
    {
        float4 diffuseData = u_OutputDiffuseHitDistance[did];

#if ENABLE_NRC
        if (diffuseData.w)
        {
            diffuseData.xyz += nrcRadiance;
            nrcRadiance = 0.0f;
        }
#endif // ENABLE_NRC

        if (any(diffuseData.xyz))
        {
            float3 diffuseAlbedo = u_OutputDiffuseAlbedo[did].xyz;
            diffuseAlbedo += diffuseAlbedo == 0.0f;
            diffuseData.xyz -= emissive;
            diffuseData.xyz /= diffuseAlbedo;
        }

        float normalizedHitDistance = REBLUR_FrontEnd_GetNormHitDist(diffuseData.w, viewSpaceZ, g_Global.nrdHitDistanceParams);
        diffuseData = REBLUR_FrontEnd_PackRadianceAndNormHitDist(diffuseData.xyz, normalizedHitDistance);
        u_OutputDiffuseHitDistance[did] = diffuseData;
    }

#if ENABLE_SPECULAR_LOBE
    // Specular
    {
        float4 specularData = u_OutputSpecularHitDistance[did];

#if ENABLE_NRC
        specularData.xyz += nrcRadiance;
#endif // ENABLE_NRC

        if (any(specularData.xyz))
        {
            float3 specularAlbedo = u_OutputSpecularAlbedo[did].xyz;
            specularAlbedo += specularAlbedo == 0.0f;
            specularData.xyz -= emissive;
            specularData.xyz /= specularAlbedo;
        }

        float normalizedHitDistance = REBLUR_FrontEnd_GetNormHitDist(specularData.w, viewSpaceZ, g_Global.nrdHitDistanceParams, normalRoughness.w);
        specularData = REBLUR_FrontEnd_PackRadianceAndNormHitDist(specularData.xyz, normalizedHitDistance);
        u_OutputSpecularHitDistance[did] = specularData;
    }
#endif // ENABLE_SPECULAR_LOBE

    normalRoughness = NRD_FrontEnd_PackNormalAndRoughness(normalRoughness.xyz, normalRoughness.w);
    u_OutputNormalRoughness[did] = normalRoughness;
}

[numthreads(BLOCK_SIZE, BLOCK_SIZE, 1)]
void resolve(in uint2 did : SV_DispatchThreadID)
{
    float4 outputColor = u_OutputEmissive[did];
    float viewSpaceZ = u_OutputViewSpaceZ[did];

    if (viewSpaceZ != 0)
    {
        // Diffuse
        {
            float4 diffuseData = u_OutputDiffuseHitDistance[did];
            float3 diffuseAlbedo = u_OutputDiffuseAlbedo[did].xyz;
            diffuseData = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(diffuseData);

            outputColor.xyz += diffuseData.xyz * diffuseAlbedo;
        }

#if ENABLE_SPECULAR_LOBE
        // Specular
        {
            float4 specularData = u_OutputSpecularHitDistance[did];
            float3 specularAlbedo = u_OutputSpecularAlbedo[did].xyz;
            specularData = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(specularData);

            outputColor.xyz += specularData.xyz * specularAlbedo;
        }
#endif // ENABLE_SPECULAR_LOBE

        u_Output[did] = outputColor;
    }
}