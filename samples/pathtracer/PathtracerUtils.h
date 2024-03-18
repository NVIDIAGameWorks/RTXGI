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

#define M_PI 3.141592653589f

#define FLT_MAX 3.402823466e+38f

// Jenkins's "one at a time" hash function
uint JenkinsHash(uint x)
{
    x += x << 10;
    x ^= x >> 6;
    x += x << 3;
    x ^= x >> 11;
    x += x << 15;
    return x;
}

// Maps integers to colors using the hash function (generates pseudo-random colors)
float3 HashAndColor(int i)
{
    uint hash = JenkinsHash(i);
    float r = ((hash >> 0) & 0xFF) / 255.0f;
    float g = ((hash >> 8) & 0xFF) / 255.0f;
    float b = ((hash >> 16) & 0xFF) / 255.0f;
    return float3(r, g, b);
}

uint InitRNG(uint2 pixel, uint2 resolution, uint frame)
{
    uint rngState = dot(pixel, uint2(1, resolution.x)) ^ JenkinsHash(frame);
    return JenkinsHash(rngState);
}

float UintToFloat(uint x)
{
    return asfloat(0x3f800000 | (x >> 9)) - 1.f;
}

uint XorShift(inout uint rngState)
{
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return rngState;
}

float Rand(inout uint rngState)
{
    return UintToFloat(XorShift(rngState));
}

float3 GetPerpendicularVector(float3 u)
{
    float3 a = abs(u);
    uint xm = ((a.x - a.y) < 0 && (a.x - a.z) < 0) ? 1 : 0;
    uint ym = (a.y - a.z) < 0 ? (1 ^ xm) : 0;
    uint zm = 1 ^ (xm | ym);
    return cross(u, float3(xm, ym, zm));
}

// Clever offset_ray function from Ray Tracing Gems chapter 6
// Offsets the ray origin from current position p, along normal n (which must be geometric normal)
// so that no self-intersection can occur.
float3 OffsetRay(const float3 p, const float3 n)
{
    static const float origin = 1.0f / 32.0f;
    static const float float_scale = 1.0f / 65536.0f;
    static const float int_scale = 256.0f;

    int3 of_i = int3(int_scale * n.x, int_scale * n.y, int_scale * n.z);

    float3 p_i = float3(
        asfloat(asint(p.x) + ((p.x < 0) ? -of_i.x : of_i.x)),
        asfloat(asint(p.y) + ((p.y < 0) ? -of_i.y : of_i.y)),
        asfloat(asint(p.z) + ((p.z < 0) ? -of_i.z : of_i.z)));

    return float3(abs(p.x) < origin ? p.x + float_scale * n.x : p_i.x,
        abs(p.y) < origin ? p.y + float_scale * n.y : p_i.y,
        abs(p.z) < origin ? p.z + float_scale * n.z : p_i.z);
}

// Bounce heatmap visualization: https://developer.nvidia.com/blog/profiling-dxr-shaders-with-timer-instrumentation/
inline float3 Temperature(float t)
{
    const float3 c[10] = {
        float3(0.0f / 255.0f,   2.0f / 255.0f,  91.0f / 255.0f),
        float3(0.0f / 255.0f, 108.0f / 255.0f, 251.0f / 255.0f),
        float3(0.0f / 255.0f, 221.0f / 255.0f, 221.0f / 255.0f),
        float3(51.0f / 255.0f, 221.0f / 255.0f,   0.0f / 255.0f),
        float3(255.0f / 255.0f, 252.0f / 255.0f,   0.0f / 255.0f),
        float3(255.0f / 255.0f, 180.0f / 255.0f,   0.0f / 255.0f),
        float3(255.0f / 255.0f, 104.0f / 255.0f,   0.0f / 255.0f),
        float3(226.0f / 255.0f,  22.0f / 255.0f,   0.0f / 255.0f),
        float3(191.0f / 255.0f,   0.0f / 255.0f,  83.0f / 255.0f),
        float3(145.0f / 255.0f,   0.0f / 255.0f,  65.0f / 255.0f)
    };

    const float s = t * 10.0f;

    const int cur = int(s) <= 9 ? int(s) : 9;
    const int prv = cur >= 1 ? cur - 1 : 0;
    const int nxt = cur < 9 ? cur + 1 : 9;

    const float blur = 0.8f;

    const float wc = smoothstep(float(cur) - blur, float(cur) + blur, s) * (1.0f - smoothstep(float(cur + 1) - blur, float(cur + 1) + blur, s));
    const float wp = 1.0f - smoothstep(float(cur) - blur, float(cur) + blur, s);
    const float wn = smoothstep(float(cur + 1) - blur, float(cur + 1) + blur, s);

    const float3 r = wc * c[cur] + wp * c[prv] + wn * c[nxt];
    return float3(clamp(r.x, 0.0f, 1.0f), clamp(r.y, 0.0f, 1.0f), clamp(r.z, 0.0f, 1.0f));
}

inline float3 BounceHeatmap(uint bounce)
{
    switch (bounce)
    {
    case 0:
        return float3(0.0f, 0.0f, 1.0f);
    case 1:
        return float3(0.0f, 1.0f, 0.0f);
    default:
        return float3(1.0f, 0.0f, 0.0f);
    }
}

enum GeometryAttributes
{
    GeomAttr_Position = 0x01,
    GeomAttr_TexCoord = 0x02,
    GeomAttr_Normal = 0x04,
    GeomAttr_Tangents = 0x08,
    GeomAttr_All = 0x0F
};

struct GeometrySample
{
    InstanceData instance;
    GeometryData geometry;
    MaterialConstants material;

    float3 vertexPositions[3];
    float2 vertexTexcoords[3];

    float3 objectSpacePosition;
    float2 texcoord;
    float3 flatNormal;
    float3 geometryNormal;
    float4 tangent;
};

GeometrySample GetGeometryFromHit(
    uint instanceIndex,
    uint triangleIndex,
    uint geometryIndex,
    bool isFrontFacing,
    float2 rayBarycentrics,
    GeometryAttributes attributes,
    StructuredBuffer<InstanceData> instanceBuffer,
    StructuredBuffer<GeometryData> geometryBuffer,
    StructuredBuffer<MaterialConstants> materialBuffer,
    ByteAddressBuffer bindlessBuffers[])
{
    GeometrySample gs = (GeometrySample)0;

    gs.instance = instanceBuffer[instanceIndex];
    gs.geometry = geometryBuffer[gs.instance.firstGeometryIndex + geometryIndex];
    gs.material = materialBuffer[gs.geometry.materialIndex];

    ByteAddressBuffer indexBuffer = bindlessBuffers[NonUniformResourceIndex(gs.geometry.indexBufferIndex)];
    ByteAddressBuffer vertexBuffer = bindlessBuffers[NonUniformResourceIndex(gs.geometry.vertexBufferIndex)];

    float3 barycentrics;
    barycentrics.yz = rayBarycentrics;
    barycentrics.x = 1.0f - (barycentrics.y + barycentrics.z);

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
        gs.geometryNormal = mul(gs.instance.transform, float4(gs.geometryNormal, 0.0f)).xyz;
        gs.geometryNormal = normalize(gs.geometryNormal);

        gs.geometryNormal *= isFrontFacing ? 1.0f : -1.0f;
    }

    if ((attributes & GeomAttr_Tangents) && gs.geometry.tangentOffset != ~0u)
    {
        float4 tangents[3];
        tangents[0] = Unpack_RGBA8_SNORM(vertexBuffer.Load(gs.geometry.tangentOffset + indices[0] * c_SizeOfNormal));
        tangents[1] = Unpack_RGBA8_SNORM(vertexBuffer.Load(gs.geometry.tangentOffset + indices[1] * c_SizeOfNormal));
        tangents[2] = Unpack_RGBA8_SNORM(vertexBuffer.Load(gs.geometry.tangentOffset + indices[2] * c_SizeOfNormal));
        gs.tangent.xyz = interpolate(tangents, barycentrics).xyz;
        gs.tangent.xyz = mul(gs.instance.transform, float4(gs.tangent.xyz, 0.0f)).xyz;
        gs.tangent.xyz = normalize(gs.tangent.xyz);
        gs.tangent.w = tangents[0].w;
    }

    float3 objectSpaceFlatNormal = normalize(cross(
        gs.vertexPositions[1] - gs.vertexPositions[0],
        gs.vertexPositions[2] - gs.vertexPositions[0]));

    gs.flatNormal = normalize(mul(gs.instance.transform, float4(objectSpaceFlatNormal, 0.0f)).xyz);
    gs.flatNormal *= isFrontFacing ? 1.0f : -1.0f;

    return gs;
}

enum MaterialAttributes
{
    MatAttr_BaseColor = 0x01,
    MatAttr_Emissive = 0x02,
    MatAttr_Normal = 0x04,
    MatAttr_MetalRough = 0x08,
    MatAttr_Transmission = 0x10,

    MatAttr_All = 0x1F
};

MaterialSample SampleGeometryMaterial(
    GeometrySample gs,
    float2 texGrad_x,
    float2 texGrad_y,
    float mipLevel, // <-- Use a compile time constant for mipLevel, < 0 for aniso filtering
    MaterialAttributes attributes,
    SamplerState materialSampler,
    Texture2D bindlessTextures[])
{
    MaterialTextureSample textures = DefaultMaterialTextures();

    if ((attributes & MatAttr_BaseColor) && (gs.material.baseOrDiffuseTextureIndex >= 0) && (gs.material.flags & MaterialFlags_UseBaseOrDiffuseTexture) != 0)
    {
        Texture2D diffuseTexture = bindlessTextures[NonUniformResourceIndex(gs.material.baseOrDiffuseTextureIndex)];

        if (mipLevel >= 0)
            textures.baseOrDiffuse = diffuseTexture.SampleLevel(materialSampler, gs.texcoord, mipLevel);
        else
            textures.baseOrDiffuse = diffuseTexture.SampleGrad(materialSampler, gs.texcoord, texGrad_x, texGrad_y);
    }

    if ((attributes & MatAttr_Emissive) && (gs.material.emissiveTextureIndex >= 0) && (gs.material.flags & MaterialFlags_UseEmissiveTexture) != 0)
    {
        Texture2D emissiveTexture = bindlessTextures[NonUniformResourceIndex(gs.material.emissiveTextureIndex)];

        if (mipLevel >= 0)
            textures.emissive = emissiveTexture.SampleLevel(materialSampler, gs.texcoord, mipLevel);
        else
            textures.emissive = emissiveTexture.SampleGrad(materialSampler, gs.texcoord, texGrad_x, texGrad_y);
    }

    if ((attributes & MatAttr_Normal) && (gs.material.normalTextureIndex >= 0) && (gs.material.flags & MaterialFlags_UseNormalTexture) != 0)
    {
        Texture2D normalsTexture = bindlessTextures[NonUniformResourceIndex(gs.material.normalTextureIndex)];

        if (mipLevel >= 0)
            textures.normal = normalsTexture.SampleLevel(materialSampler, gs.texcoord, mipLevel);
        else
            textures.normal = normalsTexture.SampleGrad(materialSampler, gs.texcoord, texGrad_x, texGrad_y);
    }

    if ((attributes & MatAttr_MetalRough) && (gs.material.metalRoughOrSpecularTextureIndex >= 0) && (gs.material.flags & MaterialFlags_UseMetalRoughOrSpecularTexture) != 0)
    {
        Texture2D specularTexture = bindlessTextures[NonUniformResourceIndex(gs.material.metalRoughOrSpecularTextureIndex)];

        if (mipLevel >= 0)
            textures.metalRoughOrSpecular = specularTexture.SampleLevel(materialSampler, gs.texcoord, mipLevel);
        else
            textures.metalRoughOrSpecular = specularTexture.SampleGrad(materialSampler, gs.texcoord, texGrad_x, texGrad_y);
    }

    if ((attributes & MatAttr_Transmission) && (gs.material.transmissionTextureIndex >= 0) && (gs.material.flags & MaterialFlags_UseTransmissionTexture) != 0)
    {
        Texture2D transmissionTexture = bindlessTextures[NonUniformResourceIndex(gs.material.transmissionTextureIndex)];

        if (mipLevel >= 0)
            textures.transmission = transmissionTexture.SampleLevel(materialSampler, gs.texcoord, mipLevel);
        else
            textures.transmission = transmissionTexture.SampleGrad(materialSampler, gs.texcoord, texGrad_x, texGrad_y);
    }

    return EvaluateSceneMaterial(gs.geometryNormal, gs.tangent, gs.material, textures);
}

// Decodes light vector and distance from Light structure based on the light type
void GetLightData(LightConstants light, float3 surfacePos, float2 rand2, bool enableSoftShadows, out float3 incidentVector, out float lightDistance, out float irradiance)
{
    incidentVector = 0;
    float halfAngularSize = 0;
    irradiance = 0;
    lightDistance = 0;

    if (light.lightType == LightType_Directional)
    {
        if (enableSoftShadows)
        {
            float3 bitangent = normalize(GetPerpendicularVector(light.direction));
            float3 tangent = cross(bitangent, light.direction);

            float angle = rand2.x * 2.0f * M_PI;
            float distance = sqrt(rand2.y);

            incidentVector = light.direction + (bitangent * sin(angle) + tangent * cos(angle)) * tan(light.angularSizeOrInvRange * 0.5f) * distance;
            incidentVector = normalize(incidentVector);
        }
        else
        {
            incidentVector = light.direction;
        }

        lightDistance = FLT_MAX;
        halfAngularSize = light.angularSizeOrInvRange * 0.5f;
        irradiance = light.intensity;
    }
    else if (light.lightType == LightType_Spot || light.lightType == LightType_Point)
    {
        float3 lightToSurface = surfacePos - light.position;
        float distance = sqrt(dot(lightToSurface, lightToSurface));
        float rDistance = 1.0f / distance;
        incidentVector = lightToSurface * rDistance;
        lightDistance = length(lightToSurface);

        float attenuation = 1.0f;
        if (light.angularSizeOrInvRange > 0)
        {
            attenuation = square(saturate(1.0f - square(square(distance * light.angularSizeOrInvRange))));

            if (attenuation == 0)
                return;
        }

        float spotlight = 1.0f;
        if (light.lightType == LightType_Spot)
        {
            float LdotD = dot(incidentVector, light.direction);
            float directionAngle = acos(LdotD);
            spotlight = 1.0f - smoothstep(light.innerAngle, light.outerAngle, directionAngle);

            if (spotlight == 0)
                return;
        }

        if (light.radius > 0)
        {
            halfAngularSize = atan(min(light.radius * rDistance, 1.0f));

            // A good enough approximation for 2 * (1 - cos(halfAngularSize)), numerically more accurate for small angular sizes
            float solidAngleOverPi = square(halfAngularSize);
            float radianceTimesPi = light.intensity / square(light.radius);

            irradiance = radianceTimesPi * solidAngleOverPi;
        }
        else
        {
            irradiance = light.intensity * square(rDistance);
        }

        irradiance *= spotlight * attenuation;
    }
    else
    {
        return;
    }
}