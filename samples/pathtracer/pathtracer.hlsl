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

#include <donut/shaders/bindless.h>
#include <donut/shaders/utils.hlsli>
#include <donut/shaders/vulkan.hlsli>
#include <donut/shaders/packing.hlsli>
#include <donut/shaders/surface.hlsli>
#include <donut/shaders/lighting.hlsli>
#include <donut/shaders/scene_material.hlsli>

#define ENABLE_NRC 1
#include "Nrc.hlsli"

#include "brdf.h"
#include "global_cb.h"
#include "lighting_cb.h"
#include "PathtracerUtils.h"

#if (SHARC_UPDATE || SHARC_QUERY)
#include "SharcCommon.h"
#endif // (SHARC_UPDATE || SHARC_QUERY)

#define BOUNCES_MIN                     3
#define RIS_CANDIDATES_LIGHTS           8 // Number of candidates used for resampling of analytical lights
#define SHADOW_RAY_IN_RIS               1 // Enable this to cast shadow rays for each candidate during resampling. This is expensive but increases quality
#define DISABLE_BACK_FACE_CULLING       1
#define SHADOW_RAY_INDEX                1
#define TRACING_DISTANCE                1000.0f
#define SHARC_ENABLE_DEBUG              1

struct RayPayload
{
    float hitDistance;
    uint instanceID;
    uint primitiveIndex;
    uint geometryIndex;
    float2 barycentrics;

    bool Hit() { return hitDistance > 0.0f; }
    bool IsFrontFacing() { return asuint(hitDistance) & 0x1; }
};

struct ShadowRayPayload
{
    float3 visibility;
};

ConstantBuffer<LightingConstants>               g_Lighting                              : register(b0, space0);
ConstantBuffer<GlobalConstants>                 g_Global                                : register(b1, space0);

RaytracingAccelerationStructure                 SceneBVH                                : register(t0, space0);
StructuredBuffer<InstanceData>                  t_InstanceData                          : register(t1, space0);
StructuredBuffer<GeometryData>                  t_GeometryData                          : register(t2, space0);
StructuredBuffer<MaterialConstants>             t_MaterialConstants                     : register(t3, space0);

RWTexture2D<float4>                             u_Output                                : register(u0, space0);
SamplerState                                    s_MaterialSampler                       : register(s0, space0);

//reg, dset
VK_BINDING(0, 4) ByteAddressBuffer               t_BindlessBuffers[]                     : register(t0, space1);
VK_BINDING(1, 4) Texture2D                       t_BindlessTextures[]                    : register(t0, space2);

#if ENABLE_DENOISER
RWTexture2D<float4>             u_OutputDiffuseHitDistance                              : register(u0, space1);
RWTexture2D<float4>             u_OutputSpecularHitDistance                             : register(u1, space1);
RWTexture2D<float>              u_OutputViewSpaceZ                                      : register(u2, space1);
RWTexture2D<float4>             u_OutputNormalRoughness                                 : register(u3, space1);
RWTexture2D<float4>             u_OutputMotionVectors                                   : register(u4, space1);
RWTexture2D<float4>             u_OutputEmissive                                        : register(u5, space1);
RWTexture2D<float4>             u_OutputDiffuseAlbedo                                   : register(u6, space1);
RWTexture2D<float4>             u_OutputSpecularAlbedo                                  : register(u7, space1);
#endif // ENABLE_DENOISER

#if ENABLE_NRC
RWStructuredBuffer<NrcPackedQueryPathInfo>      queryPathInfo                           : register(u0, space2); // Misc path info (vertexCount, queryIndex)
RWStructuredBuffer<NrcPackedTrainingPathInfo>   trainingPathInfo                        : register(u1, space2); // Misc path info (vertexCount, queryIndex)
RWStructuredBuffer<NrcPackedPathVertex>         trainingPathVertices                    : register(u2, space2); // Path vertex data used to train the neural radiance cache
RWStructuredBuffer<NrcRadianceParams>           queryRadianceParams                     : register(u3, space2);
RWStructuredBuffer<uint>                        countersData                            : register(u4, space2);

#define WRITE_TRAINING_DEBUG_PARAMS
#define WRITE_TRAINING_OUTPUT_DEBUG_PARAMS
#define WRITE_QUERY_OUTPUT_DEBUG_PARAMS
#endif // ENABLE_NRC

#if SHARC_UPDATE || SHARC_QUERY
RWStructuredBuffer<uint64_t>    u_SharcHashEntriesBuffer        : register(u0, space3);
RWStructuredBuffer<uint>        u_HashCopyOffsetBuffer          : register(u1, space3);
RWStructuredBuffer<uint4>       u_SharcVoxelDataBuffer          : register(u2, space3);
RWStructuredBuffer<uint4>       u_SharcVoxelDataBufferPrev      : register(u3, space3);
#endif // SHARC_UPDATE || SHARC_QUERY

RayDesc GeneratePinholeCameraRay(float2 normalisedDeviceCoordinate)
{
    // Set up the ray
    RayDesc ray;
    ray.Origin = g_Lighting.view.matViewToWorld[3].xyz;
    ray.TMin = 0.0f;
    ray.TMax = TRACING_DISTANCE;

    // Extract the aspect ratio and fov from the projection matrix
    float aspect = g_Lighting.view.matViewToClip[1][1] / g_Lighting.view.matViewToClip[0][0];
    float tanHalfFovY = 1.0f / g_Lighting.view.matViewToClip[1][1];

    // Compute the ray direction
    ray.Direction = normalize(
        ((normalisedDeviceCoordinate.x * 2.f - 1.f) * g_Lighting.view.matViewToWorld[0].xyz * tanHalfFovY * aspect) -
        ((normalisedDeviceCoordinate.y * 2.f - 1.f) * g_Lighting.view.matViewToWorld[1].xyz * tanHalfFovY) +
        g_Lighting.view.matViewToWorld[2].xyz);

    return ray;
}

RayDesc setupPrimaryRay(uint2 pixelPosition, PlanarViewConstants view)
{
    float2 uv = (float2(pixelPosition)+0.5) * view.viewportSizeInv;
    float4 clipPos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1);
    float4 worldPos = mul(clipPos, view.matClipToWorld);
    worldPos.xyz /= worldPos.w;

    RayDesc ray;
    ray.Origin = view.cameraDirectionOrPosition.xyz;
    ray.Direction = normalize(worldPos.xyz - ray.Origin);
    ray.TMin = 0;
    ray.TMax = 1000;
    return ray;
}

RayDesc setupShadowRay(float3 surfacePos, float3 viewIncident)
{
    RayDesc ray;
    ray.Origin = surfacePos - viewIncident * 0.001;
    ray.Direction = -g_Lighting.sunLight.direction;
    ray.TMin = 0;
    ray.TMax = 1000;
    return ray;
}

// Casts a shadow ray and returns true if light is not occluded ie. it hits nothing
// Note that we use dedicated hit group with simpler shaders for shadow rays
float3 CastShadowRay(float3 hitPosition, float3 surfaceNormal, float3 directionToLight, float tracingDistance)
{
    RayDesc ray;
    ray.Origin = OffsetRay(hitPosition, surfaceNormal);
    ray.Direction = directionToLight;
    ray.TMin = 0.0f;
    ray.TMax = tracingDistance;

    ShadowRayPayload payload;
    payload.visibility = float3(1.0f, 1.0f, 1.0f);

    uint rayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;
    TraceRay(SceneBVH, rayFlags, 0xFF, SHADOW_RAY_INDEX, 0, SHADOW_RAY_INDEX, ray, payload);

    return payload.visibility;
}

// Samples a random light from the pool of all lights using RIS (Resampled Importance Sampling)
bool SampleLightRIS(inout uint rngState, float3 hitPosition, float3 surfaceNormal, inout LightConstants selectedSample, out float lightSampleWeight)
{
    lightSampleWeight = 1.0f;
    if (g_Lighting.lightCount == 0)
        return false;

    selectedSample = g_Lighting.lights[0];
    if (g_Lighting.lightCount == 1)
        return true;

    float totalWeights = 0.0f;
    float samplePdfG = 0.0f;

    const uint candidateMax = min(g_Lighting.lightCount, RIS_CANDIDATES_LIGHTS);
    for (int i = 0; i < candidateMax; i++)
    {
        uint randomLightIndex = g_Global.targetLight >= 0 ? g_Global.targetLight : min(g_Lighting.lightCount - 1, uint(Rand(rngState) * g_Lighting.lightCount));
        LightConstants candidate = g_Lighting.lights[randomLightIndex];

        // PDF of uniform distribution is (1 / light count). Reciprocal of that PDF (simply a light count) is a weight of this sample
        float candidateWeight = float(g_Lighting.lightCount);

        {
            float3 lightVector;
            float lightDistance;
            float irradiance;
            float2 rand2 = float2(Rand(rngState), Rand(rngState));
            GetLightData(candidate, hitPosition, rand2, g_Global.enableSoftShadows, lightVector, lightDistance, irradiance);

#if SHADOW_RAY_IN_RIS
            // Casting a shadow ray for all candidates here is expensive, but can significantly decrease noise
            float3 vectorToLight = normalize(lightVector);
            if (any(CastShadowRay(hitPosition, surfaceNormal, vectorToLight, lightDistance) > 0.0f))
                continue;
#endif

            float candidatePdfG = irradiance;
            const float candidateRISWeight = candidatePdfG * candidateWeight;

            totalWeights += candidateRISWeight;
            if (Rand(rngState) < (candidateRISWeight / totalWeights))
            {
                selectedSample = candidate;
                samplePdfG = candidatePdfG;
            }
        }
    }

    if (totalWeights == 0.0f)
    {
        return false;
    }
    else
    {
        lightSampleWeight = (totalWeights / float(candidateMax)) / samplePdfG;

        return true;
    }
}

// Calculates probability of selecting BRDF (specular or diffuse) using the approximate Fresnel term
float GetSpecularBrdfProbability(MaterialSample material, float3 viewVector, float3 shadingNormal)
{
#if ENABLE_SPECULAR_LOBE
    // Evaluate Fresnel term using the shading normal
    // Note: we use the shading normal instead of the microfacet normal (half-vector) for Fresnel term here. That's suboptimal for rough surfaces at grazing angles, but half-vector is yet unknown at this point
    float specularF0 = luminance(material.specularF0);
    float diffuseReflectance = luminance(material.diffuseAlbedo);

    float fresnel = saturate(luminance(evalFresnel(specularF0, shadowedF90(specularF0), max(0.0f, dot(viewVector, shadingNormal)))));

    // Approximate relative contribution of BRDFs using the Fresnel term
    float specular = fresnel;
    float diffuse = diffuseReflectance * (1.0f - fresnel); //< If diffuse term is weighted by Fresnel, apply it here as well

    // Return probability of selecting specular BRDF over diffuse BRDF
    float probability = (specular / max(0.0001f, (specular + diffuse)));

    // Clamp probability to avoid undersampling of less prominent BRDF
    return clamp(probability, 0.1f, 0.9f);
#else // !ENABLE_SPECULAR_LOBE
    return 0.0f;
#endif // !ENABLE_SPECULAR_LOBE
}

struct Attributes
{
    float2 uv;
};

GeometrySample getGeometryFromHit(
    uint instanceIndex,
    uint triangleIndex,
    uint geometryIndex,
    float2 rayBarycentrics,
    GeometryAttributes attributes,
    StructuredBuffer<InstanceData> instanceBuffer,
    StructuredBuffer<GeometryData> geometryBuffer,
    StructuredBuffer<MaterialConstants> materialBuffer)
{
    GeometrySample gs = (GeometrySample)0;

    gs.instance = instanceBuffer[instanceIndex];
    gs.geometry = geometryBuffer[gs.instance.firstGeometryIndex + geometryIndex];
    gs.material = materialBuffer[gs.geometry.materialIndex];

    ByteAddressBuffer indexBuffer = t_BindlessBuffers[NonUniformResourceIndex(gs.geometry.indexBufferIndex)];
    ByteAddressBuffer vertexBuffer = t_BindlessBuffers[NonUniformResourceIndex(gs.geometry.vertexBufferIndex)];

    float3 barycentrics;
    barycentrics.yz = rayBarycentrics;
    barycentrics.x = 1.0 - (barycentrics.y + barycentrics.z);

    uint3 indices = indexBuffer.Load3(gs.geometry.indexOffset + triangleIndex * c_SizeOfTriangleIndices);

    if (attributes & GeomAttr_Position)
    {
        gs.vertexPositions[0] = asfloat(vertexBuffer.Load3(gs.geometry.positionOffset + indices[0] * c_SizeOfPosition));
        gs.vertexPositions[1] = asfloat(vertexBuffer.Load3(gs.geometry.positionOffset + indices[1] * c_SizeOfPosition));
        gs.vertexPositions[2] = asfloat(vertexBuffer.Load3(gs.geometry.positionOffset + indices[2] * c_SizeOfPosition));
        gs.objectSpacePosition = interpolate(gs.vertexPositions, barycentrics);
    }

    if ((attributes & GeomAttr_TexCoord) && gs.geometry.texCoord1Offset != ~0u)
    {
        gs.vertexTexcoords[0] = asfloat(vertexBuffer.Load2(gs.geometry.texCoord1Offset + indices[0] * c_SizeOfTexcoord));
        gs.vertexTexcoords[1] = asfloat(vertexBuffer.Load2(gs.geometry.texCoord1Offset + indices[1] * c_SizeOfTexcoord));
        gs.vertexTexcoords[2] = asfloat(vertexBuffer.Load2(gs.geometry.texCoord1Offset + indices[2] * c_SizeOfTexcoord));
        gs.texcoord = interpolate(gs.vertexTexcoords, barycentrics);
    }

    if ((attributes & GeomAttr_Normal) && gs.geometry.normalOffset != ~0u)
    {
        float3 normals[3];
        normals[0] = Unpack_RGB8_SNORM(vertexBuffer.Load(gs.geometry.normalOffset + indices[0] * c_SizeOfNormal));
        normals[1] = Unpack_RGB8_SNORM(vertexBuffer.Load(gs.geometry.normalOffset + indices[1] * c_SizeOfNormal));
        normals[2] = Unpack_RGB8_SNORM(vertexBuffer.Load(gs.geometry.normalOffset + indices[2] * c_SizeOfNormal));
        gs.geometryNormal = interpolate(normals, barycentrics);
        gs.geometryNormal = mul(gs.instance.transform, float4(gs.geometryNormal, 0.0)).xyz;
        gs.geometryNormal = normalize(gs.geometryNormal);
    }

    if ((attributes & GeomAttr_Tangents) && gs.geometry.tangentOffset != ~0u)
    {
        float4 tangents[3];
        tangents[0] = Unpack_RGBA8_SNORM(vertexBuffer.Load(gs.geometry.tangentOffset + indices[0] * c_SizeOfNormal));
        tangents[1] = Unpack_RGBA8_SNORM(vertexBuffer.Load(gs.geometry.tangentOffset + indices[1] * c_SizeOfNormal));
        tangents[2] = Unpack_RGBA8_SNORM(vertexBuffer.Load(gs.geometry.tangentOffset + indices[2] * c_SizeOfNormal));
        gs.tangent.xyz = interpolate(tangents, barycentrics).xyz;
        gs.tangent.xyz = mul(gs.instance.transform, float4(gs.tangent.xyz, 0.0)).xyz;
        gs.tangent.xyz = normalize(gs.tangent.xyz);
        gs.tangent.w = tangents[0].w;
    }

    float3 objectSpaceFlatNormal = normalize(cross(
        gs.vertexPositions[1] - gs.vertexPositions[0],
        gs.vertexPositions[2] - gs.vertexPositions[0]));

    gs.flatNormal = normalize(mul(gs.instance.transform, float4(objectSpaceFlatNormal, 0.0)).xyz);

    return gs;
}

[shader("miss")]
void Miss(inout RayPayload payload : SV_RayPayload)
{
    payload.hitDistance = -1.0f;
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload : SV_RayPayload, in Attributes attrib : SV_IntersectionAttributes)
{
    payload.hitDistance = RayTCurrent();
    payload.instanceID = InstanceID();
    payload.primitiveIndex = PrimitiveIndex();
    payload.geometryIndex = GeometryIndex();
    payload.barycentrics = attrib.uv;

    uint packedDistance = asuint(payload.hitDistance) & (~0x1u);
    packedDistance |= HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE ? 0x1 : 0x0;

    payload.hitDistance = asfloat(packedDistance);
}

[shader("anyhit")]
void AnyHit(inout RayPayload payload : SV_RayPayload, in Attributes attrib : SV_IntersectionAttributes)
{
    GeometrySample geometry = getGeometryFromHit(InstanceID(), PrimitiveIndex(), GeometryIndex(), attrib.uv, GeomAttr_TexCoord, t_InstanceData, t_GeometryData, t_MaterialConstants);

    MaterialSample material = SampleGeometryMaterial(geometry, 0, 0, 0, MatAttr_All, s_MaterialSampler, t_BindlessTextures);

    switch (geometry.material.domain)
    {
        case MaterialDomain_AlphaTested:
        case MaterialDomain_TransmissiveAlphaTested:
        {
            if (material.opacity < geometry.material.alphaCutoff)
                IgnoreHit();

            break;
        }

        default:
            break;
    }
    // AcceptHit but continue looking for the closest hit
}

[shader("miss")]
void ShadowMiss(inout ShadowRayPayload payload : SV_RayPayload)
{
}

[shader("closesthit")]
void ClosestHitShadow(inout ShadowRayPayload payload : SV_RayPayload, in Attributes attrib : SV_IntersectionAttributes)
{
    payload.visibility = float3(0.0f, 0.0f, 0.0f);
}

[shader("anyhit")]
void AnyHitShadow(inout ShadowRayPayload payload : SV_RayPayload, in Attributes attrib : SV_IntersectionAttributes)
{
    GeometrySample geometry = getGeometryFromHit(InstanceID(), PrimitiveIndex(), GeometryIndex(), attrib.uv, GeomAttr_TexCoord, t_InstanceData, t_GeometryData, t_MaterialConstants);

    MaterialSample material = SampleGeometryMaterial(geometry, 0, 0, 0, MatAttr_All, s_MaterialSampler, t_BindlessTextures);

    switch (geometry.material.domain)
    {
        case MaterialDomain_AlphaTested:
        case MaterialDomain_TransmissiveAlphaTested:
        {
            if (material.opacity < geometry.material.alphaCutoff)
                IgnoreHit();

            break;
        }

        default:
            // Modulate the visiblity by the material's transmission
            payload.visibility *= (1.0f - material.opacity) * material.baseColor;
            if (dot(payload.visibility, 0.333f) > 0.001f)
                IgnoreHit();

            break;
    }
    AcceptHitAndEndSearch();
}

struct AccumulatedSampleData
{
    float3 radiance;
#if ENABLE_DENOISER
    float hitDistance;
#if ENABLE_SPECULAR_LOBE
    uint diffuseSampleNum;
    float3 specularRadiance;
    float specularHitDistance;
#endif // ENABLE_SPECULAR_LOBE
#endif // ENABLE_DENOISER
};

void UpdateSampleData(inout AccumulatedSampleData accumulatedSampleData, float3 sampleRadiance, bool isDiffusePath, float hitDistance)
{
#if ENABLE_DENOISER
#if ENABLE_SPECULAR_LOBE
    if (!isDiffusePath)
    {
        accumulatedSampleData.specularRadiance += sampleRadiance;
        accumulatedSampleData.specularHitDistance += hitDistance;
        return;
    }
    accumulatedSampleData.diffuseSampleNum++;
#endif // ENABLE_SPECULAR_LOBE
    accumulatedSampleData.hitDistance += hitDistance;
#endif // ENABLE_DENOISER

    accumulatedSampleData.radiance += sampleRadiance;
}

void ResolveSampleData(inout AccumulatedSampleData accumulatedSampleData, uint sampleNum, float intensityScale)
{
#if ENABLE_DENOISER
#if ENABLE_SPECULAR_LOBE
    uint specularSampleNum = sampleNum - accumulatedSampleData.diffuseSampleNum;
    if (specularSampleNum)
    {
        accumulatedSampleData.specularRadiance *= (intensityScale / specularSampleNum);
        accumulatedSampleData.specularHitDistance *= (1.0f / specularSampleNum);
    }
    u_OutputSpecularHitDistance[DispatchRaysIndex().xy] = float4(accumulatedSampleData.specularRadiance, accumulatedSampleData.specularHitDistance);
    uint diffuseSampleNum = accumulatedSampleData.diffuseSampleNum;
#else // !ENABLE_SPECULAR_LOBE
    uint diffuseSampleNum = sampleNum;
#endif // !ENABLE_SPECULAR_LOBE
    if (diffuseSampleNum)
    {
        accumulatedSampleData.radiance *= (intensityScale / diffuseSampleNum);
        accumulatedSampleData.hitDistance *= (1.0f / diffuseSampleNum);
    }
    u_OutputDiffuseHitDistance[DispatchRaysIndex().xy] = float4(accumulatedSampleData.radiance, accumulatedSampleData.hitDistance);
#else // !ENABLE_DENOISER
    accumulatedSampleData.radiance *= (intensityScale / sampleNum);
    u_Output[DispatchRaysIndex().xy] = float4(accumulatedSampleData.radiance, 1.0f);
#endif // !ENABLE_DENOISER
}

void PathTraceRays()
{
    const uint2 launchIndex = DispatchRaysIndex().xy;
    const uint2 launchDimensions = DispatchRaysDimensions().xy;
    uint rngState = InitRNG(launchIndex, launchDimensions, g_Global.frameIndex);

    AccumulatedSampleData accumulatedSampleData = (AccumulatedSampleData)0;
    float3 debugColor = float3(0.0f, 0.0f, 0.0f);

    // Initialize common resources required by the NRC
    NrcBuffers buffers;
    buffers.queryPathInfo = queryPathInfo;
    buffers.trainingPathInfo = trainingPathInfo;
    buffers.trainingPathVertices = trainingPathVertices;
    buffers.queryRadianceParams = queryRadianceParams;
    buffers.countersData = countersData;

    // Create NrcContext
    NrcContext nrcContext = NrcCreateContext(g_Lighting.nrcConstants, buffers, launchIndex);

#if SHARC_UPDATE || SHARC_QUERY
    SharcState sharcState;

    sharcState.gridParameters.cameraPosition = g_Lighting.sharcCameraPosition.xyz;
    sharcState.gridParameters.logarithmBase = SHARC_GRID_LOGARITHM_BASE;
    sharcState.gridParameters.sceneScale = g_Lighting.sharcSceneScale;

    sharcState.hashMapData.capacity = g_Lighting.sharcEntriesNum;
    sharcState.hashMapData.hashEntriesBuffer = u_SharcHashEntriesBuffer;

#if !SHARC_ENABLE_64_BIT_ATOMICS
    sharcState.hashMapData.lockBuffer = u_HashCopyOffsetBuffer;
#endif // !SHARC_ENABLE_64_BIT_ATOMICS

    sharcState.voxelDataBuffer = u_SharcVoxelDataBuffer;
#if SHARC_ENABLE_CACHE_RESAMPLING
    sharcState.voxelDataBufferPrev = u_SharcVoxelDataBufferPrev;
#endif // SHARC_ENABLE_CACHE_RESAMPLING
#endif // SHARC_UPDATE || SHARC_QUERY

#if NRC_UPDATE || SHARC_UPDATE
    const int samplesPerPixel = 1;
#else
    const int samplesPerPixel = g_Global.samplesPerPixel;
#endif

    for (int sampleIndex = 0; sampleIndex < samplesPerPixel; sampleIndex++)
    {
        // Initialize NRC data for path and sample index traced in this thread
        NrcPathState nrcPathState = NrcCreatePathState(Rand(rngState));
        NrcSetSampleIndex(nrcContext, sampleIndex);

#if SHARC_UPDATE
        SharcInit(sharcState);
#endif // SHARC_UPDATE

        float2 pixel = float2(launchIndex);

#if NRC_UPDATE || SHARC_UPDATE
        const bool doJitter = true; // Always jitter when we're updating a radiance cache
#else
        const bool doJitter = g_Global.enableJitter;
#endif
        pixel += doJitter ? float2(Rand(rngState), Rand(rngState)) : 0.5f.xx;

        RayDesc ray = GeneratePinholeCameraRay(pixel / launchDimensions);
#if !SHARC_UPDATE
        float3 sampleRadiance = float3(0.0f, 0.0f, 0.0f);
        float3 throughput = float3(1.0f, 1.0f, 1.0f);
#endif // !SHARC_UPDATE
        float materialRoughnessPrev = 0.0f;
        bool isDiffusePath = true; // Used by denoiser
        float hitDistance = 0.0f; // Used by denoiser

        bool internalRay = false;

        RayPayload payload;
        payload.hitDistance = -1.0f;
        payload.instanceID = ~0U;
        payload.primitiveIndex = ~0U;
        payload.geometryIndex = ~0U;
        payload.barycentrics = 0;

        int bounce;
        for (bounce = 0; true/* break from the middle */; bounce++)
        {
            uint rayFlags = (!g_Global.enableBackFaceCull || internalRay) ? RAY_FLAG_NONE : RAY_FLAG_CULL_BACK_FACING_TRIANGLES;

#if DISABLE_BACK_FACE_CULLING
            rayFlags &= (~RAY_FLAG_CULL_BACK_FACING_TRIANGLES);
#endif // DISABLE_BACK_FACE_CULLING

            TraceRay(SceneBVH, rayFlags, 0xFF, 0, 0, 0, ray, payload);

#if SHARC_UPDATE
            // When updating SHaRC, we're only interested in one path segment at a time
            // (SHaRC handles the propagation of radiance along the path)
            // So we don't need to track throughput along the path.
            // A simple way to achieve this is to reset it to 1 at the start of the bounce.
            float3 sampleRadiance = 0.0f.xxx;
            float3 throughput = 1.0f.xxx;
#endif // SHARC_UPDATE

#if ENABLE_DENOISER
            if (bounce == 1)
                hitDistance = payload.Hit() ? payload.hitDistance : TRACING_DISTANCE;
#endif // ENABLE_DENOISER

            // On a miss, load the sky value and break out of the ray tracing loop
            if (!payload.Hit())
            {
                float3 skyValue = g_Lighting.skyColor.rgb;
#if SHARC_UPDATE
                SharcUpdateMiss(sharcState, skyValue);
#endif // SHARC_UPDATE

                NrcUpdateOnMiss(nrcPathState);

                sampleRadiance += skyValue * throughput;

#if ENABLE_DENOISER
                if (bounce == 0)
                    u_Output[launchIndex] = float4(sampleRadiance, 1.0f);
#endif // ENABLE_DENOISER

                break;
            }
#if ENABLE_DENOISER && NRC_QUERY
            else if (bounce == 0)
            {
                u_Output[launchIndex] = float4(0.0f, 0.0f, 0.0f, 0.0f);
            }
#endif // ENABLE_DENOISER && NRC_QUERY

            GeometrySample geometry = getGeometryFromHit(payload.instanceID, payload.primitiveIndex, payload.geometryIndex, payload.barycentrics, GeomAttr_All, t_InstanceData, t_GeometryData, t_MaterialConstants);
            MaterialSample material = SampleGeometryMaterial(geometry, 0, 0, 0, MatAttr_All, s_MaterialSampler, t_BindlessTextures);
            material.emissiveColor = g_Global.enableEmissives ? material.emissiveColor : 0;

            if (material.hasMetalRoughParams)
            {
                // Remap roughness and metalness according to the UI sliders
                material.roughness = lerp(g_Global.roughnessMin, g_Global.roughnessMax, material.roughness);
                material.metalness = lerp(g_Global.metalnessMin, g_Global.metalnessMax, material.metalness);
                material.diffuseAlbedo = lerp(material.baseColor * (1.0 - c_DielectricSpecular), 0.0, material.metalness);
                material.specularF0 = lerp(c_DielectricSpecular, material.baseColor.rgb, material.metalness);
            }

            // Flip normals towards the incident ray direction (needed for backfacing triangles)
            float3 viewVector = -ray.Direction;

            // Flip the triangle normal, based on positional data, NOT the provided vertex normal
            float3 geometryNormal = geometry.flatNormal;
            if (dot(geometryNormal, viewVector) < 0.0f)
                geometryNormal = -geometryNormal;

            // Flip the shading normal, based on texture
            float3 shadingNormal = material.shadingNormal;
            if (dot(geometryNormal, shadingNormal) < 0.0f)
                shadingNormal = -shadingNormal;

            float3 hitPos = ray.Origin + ray.Direction * payload.hitDistance;

            // Construct NRCSurfaceData structure needed for creating a query point at this hit location
            NrcSurfaceAttributes surfaceAttributes = (NrcSurfaceAttributes)0;
            surfaceAttributes.encodedPosition = NrcEncodePosition(hitPos, g_Lighting.nrcConstants);
            surfaceAttributes.roughness = material.roughness;
            surfaceAttributes.specularF0 = material.specularF0;
            surfaceAttributes.diffuseReflectance = material.diffuseAlbedo;
            surfaceAttributes.shadingNormal = shadingNormal;
            surfaceAttributes.viewVector = viewVector;
            surfaceAttributes.isDeltaLobe = (material.metalness == 1.0f && material.roughness == 0.0f); // Set to true for perfectly smooth surfaces
                
            float hitDistance = payload.hitDistance;

            NrcProgressState nrcProgressState = NrcUpdateOnHit(nrcContext, nrcPathState, surfaceAttributes, hitDistance, bounce, throughput, sampleRadiance);
            if (nrcProgressState == NrcProgressState::TerminateImmediately)
                break;

#if SHARC_UPDATE || SHARC_QUERY
            // Construct SharcHitData structure needed for creating a query point at this hit location
            SharcHitData sharcHitData;
            sharcHitData.positionWorld = hitPos;
            sharcHitData.normalWorld = geometryNormal;

#if SHARC_SEPARATE_EMISSIVE
            sharcHitData.emissive = material.emissiveColor;
#endif // SHARC_SEPARATE_EMISSIVE
#endif // SHARC_UPDATE || SHARC_QUERY

#if SHARC_UPDATE
            material.roughness = max(g_Lighting.sharcRoughnessThreshold, material.roughness);
#endif // SHARC_UPDATE

#if SHARC_QUERY
            {
                uint gridLevel = GetGridLevel(hitPos, sharcState.gridParameters);
                float voxelSize = GetVoxelSize(gridLevel, sharcState.gridParameters);
                bool isValidHit = payload.hitDistance > voxelSize * sqrt(3.0f);

                materialRoughnessPrev = min(materialRoughnessPrev, 0.99f);
                float alpha = materialRoughnessPrev * materialRoughnessPrev;
                float footrprint = payload.hitDistance * sqrt(0.5f * alpha * alpha / (1.0f - alpha * alpha));
                isValidHit &= footrprint > voxelSize;

                float3 sharcRadiance;
                if (isValidHit && SharcGetCachedRadiance(sharcState, sharcHitData, sharcRadiance, false))
                {
                    sampleRadiance += sharcRadiance * throughput;

                    break; // Terminate the path once we've looked up into the cache
                }

#if SHARC_ENABLE_DEBUG
                if (g_Global.sharcDebug)
                {
                    float3 debugColor;
                    SharcGetCachedRadiance(sharcState, sharcHitData, debugColor, true);
                    u_Output[DispatchRaysIndex().xy] = float4(debugColor, 1.0f);

                    return;
                }
#endif // SHARC_ENABLE_DEBUG
            }
#endif // SHARC_QUERY

            if (g_Global.enableLighting)
            {
                // Evaluate direct light (next event estimation), start by sampling one light 
                LightConstants light = g_Lighting.lights[0];
                float lightWeight = 1.0f;

                if (SampleLightRIS(rngState, hitPos, geometryNormal, light, lightWeight))
                {
                    float3 shadowHitPos = hitPos;
                    float3 shadowNormal = geometryNormal;
                    float3 shadowV = viewVector;

                    // Prepare data needed to evaluate the light
                    float3 incidentVector;
                    float lightDistance;
                    float irradiance;
                    float2 rand2 = float2(Rand(rngState), Rand(rngState));
                    GetLightData(light, shadowHitPos, rand2, g_Global.enableSoftShadows, incidentVector, lightDistance, irradiance);
                    float3 vectorToLight = normalize(-incidentVector);

                    // Cast shadow ray towards the selected light
                    float3 lightVisibility = CastShadowRay(shadowHitPos, shadowNormal, vectorToLight, lightDistance);
                    if (any(lightVisibility > 0.0f))
                    {
                        // If light is not in shadow, evaluate BRDF and accumulate its contribution into radiance
                        // This is an entry point for evaluation of all other BRDFs based on selected configuration (for direct light)
                        float3 lightContribution = evalCombinedBRDF(shadingNormal, vectorToLight, shadowV, material) * light.color * irradiance * lightWeight * lightVisibility;
                        sampleRadiance += lightContribution * throughput;
                    }
                }
            }

            // Terminate the loop early on the last bounce (we don't need to sample the BRDF)
            if (bounce == g_Global.bouncesMax - 1)
            {
                NrcSetDebugPathTerminationReason(nrcPathState, NrcDebugPathTerminationReason::MaxPathVertices);
                break;
            }

#if !(SHARC_UPDATE && SHARC_SEPARATE_EMISSIVE)
            sampleRadiance += material.emissiveColor * throughput;
#endif

            // Terminate the loop after the emissives and direct light contribution has been added if NRC CreateQuery call 
            // requested delayed termination. In case direct lighting is not being cached (radianceCacheDirect paramter is 
            //  false), we need to add direct lighting on hit where we query NRC before terminating the loop.
            if (nrcProgressState == NrcProgressState::TerminateAfterDirectLighting)
                break;

#if SHARC_UPDATE
            if (!SharcUpdateHit(sharcState, sharcHitData, sampleRadiance, Rand(rngState)))
                break;
#endif // SHARC_UPDATE

            // Russian roulette
            if (g_Global.enableRussianRoulette && NrcCanUseRussianRoulette(nrcPathState) && (bounce > BOUNCES_MIN))
            {
                float rrProbability = min(0.95f, luminance(throughput));
                const bool terminate = (rrProbability < Rand(rngState));
                if (terminate)
                    break;
                else
                    throughput /= rrProbability;
            }

            // Sample BRDF to generate the next ray
            // First, figure out whether to sample diffuse or specular BRDF
            int brdfType = DIFFUSE_TYPE;

            // Fast path for mirrors
            if (material.metalness == 1.0f && material.roughness == 0.0f)
            {
                brdfType = SPECULAR_TYPE;
            }
            else
            {
                float specularBrdfProbability = GetSpecularBrdfProbability(material, viewVector, shadingNormal);

                if (Rand(rngState) < specularBrdfProbability)
                {
                    brdfType = SPECULAR_TYPE;
                    throughput /= specularBrdfProbability;
                }
                else if (g_Global.enableTransmission)
                {
                    float transmissiveProbability = (1.0f - specularBrdfProbability) * material.transmission;

                    if (Rand(rngState) < material.transmission)
                    {
                        brdfType = TRANSMISSIVE_TYPE;
                        throughput /= transmissiveProbability;
                    }
                    else
                    {
                        brdfType = DIFFUSE_TYPE;
                        throughput /= (1.0f - specularBrdfProbability - transmissiveProbability);
                    }
                }
                else
                {
                    brdfType = DIFFUSE_TYPE;
                    throughput /= (1.0f - specularBrdfProbability);
                }
            }

#if SHARC_QUERY
            materialRoughnessPrev += brdfType == DIFFUSE_TYPE ? 1.0f : material.roughness;
#endif // SHARC_QUERY

#if ENABLE_DENOISER
            if (bounce == 0)
            {
                isDiffusePath = brdfType == DIFFUSE_TYPE;
                // Fill GBuffer data
                if (sampleIndex == 0)
                {
                    u_OutputViewSpaceZ[launchIndex] = dot(hitPos - g_Lighting.view.matViewToWorld[3].xyz, g_Lighting.view.matViewToWorld[2].xyz);
                    u_OutputNormalRoughness[launchIndex] = float4(shadingNormal, material.roughness);
                    // Motion vectors
                    {
                        float4 positionClip = mul(float4(hitPos, 1.0f), g_Lighting.view.matWorldToClip);
                        positionClip.xyz /= positionClip.w;
                        float4 positionClipPrev = mul(float4(hitPos, 1.0f), g_Lighting.viewPrev.matWorldToClip);
                        positionClipPrev.xyz /= positionClipPrev.w;

                        float3 motionVector;
                        motionVector.xy = (positionClipPrev.xy - positionClip.xy) * g_Lighting.view.clipToWindowScale;
                        motionVector.z = positionClipPrev.w - positionClip.w;

                        u_OutputMotionVectors[launchIndex] = float4(motionVector, 0.0f);
                        u_OutputEmissive[launchIndex] = float4(sampleRadiance, 1.0f);
                        u_OutputDiffuseAlbedo[launchIndex] = float4(material.diffuseAlbedo, isDiffusePath ? 1.0f : 0.0f);
                        u_OutputSpecularAlbedo[launchIndex] = float4(EnvBRDFApprox2(material.specularF0, material.roughness * material.roughness, 0.0f), 1.0f);
                    }
                }
            }
#endif // ENABLE_DENOISER

            // Run importance sampling of selected BRDF to generate reflecting ray direction
            float3 brdfWeight = float3(0.0f, 0.0f, 0.0f);
            float brdfPdf = 0.0f;
            float refractiveIndex = 1.0f;

            // Generates a new ray direction
            float2 rand2 = float2(Rand(rngState), Rand(rngState));
            if (!evalIndirectCombinedBRDF(rand2, shadingNormal, geometryNormal, viewVector, material, brdfType, refractiveIndex, ray.Direction, brdfWeight, brdfPdf))
            {
                NrcSetDebugPathTerminationReason(nrcPathState, NrcDebugPathTerminationReason::BRDFAbsorption);
                break; // Ray was eaten by the surface :(
            }

            NrcSetBrdfPdf(nrcPathState, brdfPdf);

            // Refraction requires the ray offset to go in the opposite direction
            bool transition = dot(geometryNormal, ray.Direction) <= 0.0f;
            ray.Origin = OffsetRay(hitPos, transition ? -geometryNormal : geometryNormal);

            // If we are internal, assume we will be leaving the object on a transition and air has an ior of ~1.0
            if (internalRay)
            {
                refractiveIndex = 1.0f / refractiveIndex;

                //if (g_Global.enableAbsorbtion)
                //    throughput *= exp(-0.5f * payload.hitDistance); // Beers law of attenuation
            }

            if (transition)
                internalRay = !internalRay;

            if (g_Global.enableOcclusion)
                throughput *= material.occlusion;

            // Account for surface properties using the BRDF "weight"
            throughput *= brdfWeight;

#if SHARC_UPDATE
            SharcSetThroughput(sharcState, throughput);
#else // !SHARC_UPDATE
            if (luminance(throughput) < g_Global.throughputThreshold)
                break;
#endif // !SHARC_UPDATE
        }

#if !SHARC_UPDATE
        NrcWriteFinalPathInfo(nrcContext, nrcPathState, throughput, sampleRadiance);
        UpdateSampleData(accumulatedSampleData, sampleRadiance, isDiffusePath, hitDistance);

        if (g_Global.debugOutputMode == 8 /* Bounce Heatmap */)
            debugColor = BounceHeatmap(bounce);
#endif // !SHARC_UPDATE
    }

#if !SHARC_UPDATE && !NRC_UPDATE // Don't write any output when we're just updating a radiance cache
    {
        // Write radiance to output buffer
        ResolveSampleData(accumulatedSampleData, g_Global.samplesPerPixel, 1.0f);
    }
    // Debug output calculation
    if (g_Global.debugOutputMode != 0)
    {
        float2 pixel = float2(DispatchRaysIndex().xy) + 0.5.xx;
        RayDesc ray = GeneratePinholeCameraRay(pixel / float2(launchDimensions));
        RayPayload payload = (RayPayload)0;
        TraceRay(SceneBVH, 0, 0xFF, 0, 0, 0, ray, payload);

        if (payload.Hit())
        {
            GeometrySample geometry = getGeometryFromHit(payload.instanceID, payload.primitiveIndex, payload.geometryIndex, payload.barycentrics, GeomAttr_All, t_InstanceData, t_GeometryData, t_MaterialConstants);

            if (g_Global.debugOutputMode == 1 /* DiffuseReflectance */)
            {
                MaterialSample material = SampleGeometryMaterial(geometry, 0, 0, 0, MatAttr_All, s_MaterialSampler, t_BindlessTextures);
                debugColor = material.diffuseAlbedo;
            }
            else if (g_Global.debugOutputMode == 2 /* WorldSpaceNormals */)
            {
                debugColor = geometry.geometryNormal * 0.5f + 0.5f;
            }
            else if (g_Global.debugOutputMode == 3 /* WorldSpacePosition */)
            {
                debugColor = mul(geometry.instance.transform, float4(geometry.objectSpacePosition, 1.0f)).xyz;
            }
            else if (g_Global.debugOutputMode == 4 /* Barycentrics */)
            {
                debugColor = float3(1 - payload.barycentrics.x - payload.barycentrics.y, payload.barycentrics.x, payload.barycentrics.y);
            }
            else if (g_Global.debugOutputMode == 5 /* HitT */)
            {
                debugColor = float3(payload.hitDistance, payload.hitDistance, payload.hitDistance) / 100.0f;
            }
            else if (g_Global.debugOutputMode == 6 /* InstanceID */)
            {
                debugColor = HashAndColor(payload.instanceID);
            }
            else if (g_Global.debugOutputMode == 7 /* Emissives */)
            {
                MaterialSample material = SampleGeometryMaterial(geometry, 0, 0, 0, MatAttr_All, s_MaterialSampler, t_BindlessTextures);
                debugColor = material.emissiveColor;
            }
            else if (g_Global.debugOutputMode == 8 /* Heat map */)
            {
                // Already set
            }
        }

        u_Output[launchIndex] = float4(debugColor, 1.0f);
    }
#endif // !SHARC_UPDATE && !NRC_UPDATE
}

[shader("raygeneration")]
void RayGen()
{
    PathTraceRays();
}