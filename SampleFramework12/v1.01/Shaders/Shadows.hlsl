//=================================================================================================
//
//  MJP's DX12 Sample Framework
//  http://mynameismjp.wordpress.com/
//
//  All code licensed under the MIT license
//
//=================================================================================================

#include <EVSM.hlsl>
#include <MSM.hlsl>

#define ShadowMapMode_DepthMap_ 0
#define ShadowMapMode_EVSM_     1
#define ShadowMapMode_MSM_      2

// Options
#ifndef ShadowMapMode_
    #define ShadowMapMode_ ShadowMapMode_DepthMap_
#endif

#ifndef UseGatherPCF_
    #define UseGatherPCF_ 0
#endif

//=================================================================================================
// Constants
//=================================================================================================
static const uint NumCascades = 4;

struct SunShadowConstantsBase
{
    row_major float4x4 ShadowMatrix;
    float4 CascadeSplits;
    float4 CascadeOffsets[NumCascades];
    float4 CascadeScales[NumCascades];
};

struct EVSMConstants
{
    float PositiveExponent;
    float NegativeExponent;
    float LightBleedingReduction;
    uint Padding;
};

struct MSMConstants
{
    float DepthBias;
    float MomentBias;
    float LightBleedingReduction;
    uint Padding;
};

struct SunShadowConstantsDepthMap
{
    SunShadowConstantsBase Base;
    uint4 Extra;
};

struct SunShadowConstantsEVSM
{
    SunShadowConstantsBase Base;
    EVSMConstants Extra;
};

struct SunShadowConstantsMSM
{
    SunShadowConstantsBase Base;
    MSMConstants Extra;
};

#if ShadowMapMode_ == ShadowMapMode_DepthMap_
    #define SunShadowConstants SunShadowConstantsDepthMap
    typedef uint4 ExtraShadowConstants;
    typedef SamplerComparisonState ShadowSampler;
#elif ShadowMapMode_ == ShadowMapMode_EVSM_
    #define SunShadowConstants SunShadowConstantsEVSM
    typedef EVSMConstants ExtraShadowConstants;
    typedef SamplerState ShadowSampler;
#elif ShadowMapMode_ == ShadowMapMode_MSM_
    #define SunShadowConstants SunShadowConstantsMSM
    typedef MSMConstants ExtraShadowConstants;
    typedef SamplerState ShadowSampler;
#endif

//-------------------------------------------------------------------------------------------------
// Samples the EVSM shadow map with explicit gradients
//-------------------------------------------------------------------------------------------------
float SampleShadowMapEVSM(in float3 shadowPos, in float3 shadowPosDX, in float3 shadowPosDY, in uint arrayIdx,
                          in Texture2DArray shadowMap, in SamplerState evsmSampler, in EVSMConstants evsmConstants, in float3 cascadeScale)
{
    float2 exponents = GetEVSMExponents(evsmConstants.PositiveExponent, evsmConstants.NegativeExponent, cascadeScale);
    float2 warpedDepth = WarpDepth(shadowPos.z, exponents);

    float4 occluder = shadowMap.SampleGrad(evsmSampler, float3(shadowPos.xy, arrayIdx), shadowPosDX.xy, shadowPosDY.xy);

    // Derivative of warping at depth
    float2 depthScale = 0.0001f * exponents * warpedDepth;
    float2 minVariance = depthScale * depthScale;

    float posContrib = ChebyshevUpperBound(occluder.xz, warpedDepth.x, minVariance.x, evsmConstants.LightBleedingReduction);
    float negContrib = ChebyshevUpperBound(occluder.yw, warpedDepth.y, minVariance.y, evsmConstants.LightBleedingReduction);
    float shadowContrib = posContrib;
    shadowContrib = min(shadowContrib, negContrib);

    return shadowContrib;
}

//-------------------------------------------------------------------------------------------------
// Samples the EVSM shadow map with implicit gradients
//-------------------------------------------------------------------------------------------------
float SampleShadowMapEVSM(in float3 shadowPos, in uint arrayIdx, in Texture2DArray shadowMap, in SamplerState evsmSampler,
                          in EVSMConstants evsmConstants, in float3 cascadeScale)
{
    float2 exponents = GetEVSMExponents(evsmConstants.PositiveExponent, evsmConstants.NegativeExponent, cascadeScale);
    float2 warpedDepth = WarpDepth(shadowPos.z, exponents);

    float4 occluder = shadowMap.Sample(evsmSampler, float3(shadowPos.xy, arrayIdx));

    // Derivative of warping at depth
    float2 depthScale = 0.0001f * exponents * warpedDepth;
    float2 minVariance = depthScale * depthScale;

    float posContrib = ChebyshevUpperBound(occluder.xz, warpedDepth.x, minVariance.x, evsmConstants.LightBleedingReduction);
    float negContrib = ChebyshevUpperBound(occluder.yw, warpedDepth.y, minVariance.y, evsmConstants.LightBleedingReduction);
    float shadowContrib = posContrib;
    shadowContrib = min(shadowContrib, negContrib);

    return shadowContrib;
}

//-------------------------------------------------------------------------------------------------
// Samples the MSM shadow map with explicit gradients
//-------------------------------------------------------------------------------------------------
float SampleShadowMapMSM(in float3 shadowPos, in float3 shadowPosDX, in float3 shadowPosDY, in uint arrayIdx,
                         in Texture2DArray shadowMap, in SamplerState evsmSampler, in MSMConstants msmConstants)
{
    float4 moments = shadowMap.SampleGrad(evsmSampler, float3(shadowPos.xy, arrayIdx), shadowPosDX.xy, shadowPosDY.xy);
    moments = ConvertOptimizedMoments(moments);
    float result = ComputeMSMHamburger(moments, shadowPos.z, msmConstants.DepthBias, msmConstants.MomentBias);

    return ReduceLightBleeding(result, msmConstants.LightBleedingReduction);
}

//-------------------------------------------------------------------------------------------------
// Samples the MSM shadow map with implicit gradients
//-------------------------------------------------------------------------------------------------
float SampleShadowMapMSM(in float3 shadowPos, in uint arrayIdx, in Texture2DArray shadowMap,
                         in SamplerState evsmSampler, in MSMConstants msmConstants)
{
    float4 moments = shadowMap.Sample(evsmSampler, float3(shadowPos.xy, arrayIdx));
    moments = ConvertOptimizedMoments(moments);
    float result = ComputeMSMHamburger(moments, shadowPos.z, msmConstants.DepthBias, msmConstants.MomentBias);

    return ReduceLightBleeding(result, msmConstants.LightBleedingReduction);
}

#if UseGatherPCF_

// 7x7 disc kernel
static const uint ShadowFilterSize = 7;
static const float W[ShadowFilterSize][ShadowFilterSize] =
{
    { 0.0f, 0.0f, 0.5f, 1.0f, 0.5f, 0.0f, 0.0f },
    { 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f },
    { 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f },
    { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
    { 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f },
    { 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f },
    { 0.0f, 0.0f, 0.5f, 1.0f, 0.5f, 0.0f, 0.0f }
};

//-------------------------------------------------------------------------------------------------
// Samples the shadow map with a fixed-size PCF kernel optimized with GatherCmp. Uses code
// from "Fast Conventional Shadow Filtering" by Holger Gruen, in GPU Pro.
//-------------------------------------------------------------------------------------------------
float SampleShadowMapPCF(in float3 shadowPos, in uint arrayIdx, in Texture2DArray shadowMap,
                               in SamplerComparisonState shadowSampler) {
    float2 shadowMapSize;
    float numSlices;
    shadowMap.GetDimensions(shadowMapSize.x, shadowMapSize.y, numSlices);
    float2 texelSize = 1.0f / shadowMapSize;

    float lightDepth = shadowPos.z - 0.001f;

    const int FS_2 = int(ShadowFilterSize) / 2;

    float2 tc = shadowPos.xy;

    float4 s = 0.0f;
    float2 stc = (shadowMapSize * tc.xy) + float2(0.5f, 0.5f);
    float2 tcs = floor(stc);
    float2 fc = 0.0f;
    float w = 0.0f;
    float4 v1[FS_2 + 1];
    float2 v0[FS_2 + 1];

    fc.xy = stc - tcs;
    tc.xy = tcs / shadowMapSize;

    for(uint y = 0; y < ShadowFilterSize; ++y)
        for(uint x = 0; x < ShadowFilterSize; ++x)
            w += W[y][x];

    // -- loop over the rows
    [unroll]
    for(int row = -FS_2; row <= FS_2; row += 2)
    {
        [unroll]
        for(int col = -FS_2; col <= FS_2; col += 2)
        {
            float value = W[row + FS_2][col + FS_2];

            if(col > -FS_2)
                value += W[row + FS_2][col + FS_2 - 1];

            if(col < FS_2)
                value += W[row + FS_2][col + FS_2 + 1];

            if(row > -FS_2) {
                value += W[row + FS_2 - 1][col + FS_2];

                if(col < FS_2)
                    value += W[row + FS_2 - 1][col + FS_2 + 1];

                if(col > -FS_2)
                    value += W[row + FS_2 - 1][col + FS_2 - 1];
            }

            if(value != 0.0f)
                v1[(col + FS_2) / 2] = shadowMap.GatherCmp(shadowSampler, float3(tc.xy, arrayIdx), lightDepth, int2(col, row));
            else
                v1[(col + FS_2) / 2] = 0.0f;

            if(col == -FS_2)
            {
                s.x += (1.0f - fc.y) * (v1[0].w * (W[row + FS_2][col + FS_2] - W[row + FS_2][col + FS_2] * fc.x) + v1[0].z * (fc.x * (W[row + FS_2][col + FS_2]
                                     - W[row + FS_2][col + FS_2 + 1.0f]) + W[row + FS_2][col + FS_2 + 1]));
                s.y += fc.y * (v1[0].x * (W[row + FS_2][col + FS_2] - W[row + FS_2][col + FS_2] * fc.x)
                                     + v1[0].y * (fc.x * (W[row + FS_2][col + FS_2] - W[row + FS_2][col + FS_2 + 1])
                                     +  W[row + FS_2][col + FS_2 + 1]));
                if(row > -FS_2)
                {
                    s.z += (1.0f - fc.y) * (v0[0].x * (W[row + FS_2 - 1][col + FS_2] - W[row + FS_2 - 1][col + FS_2] * fc.x)
                                           + v0[0].y * (fc.x * (W[row + FS_2 - 1][col + FS_2] - W[row + FS_2 - 1][col + FS_2 + 1])
                                           + W[row + FS_2 - 1][col + FS_2 + 1]));
                    s.w += fc.y * (v1[0].w * (W[row + FS_2 - 1][col + FS_2] - W[row + FS_2 - 1][col + FS_2] * fc.x)
                                        + v1[0].z * (fc.x * (W[row + FS_2 - 1][col + FS_2] - W[row + FS_2 - 1][col + FS_2 + 1])
                                        + W[row + FS_2 - 1][col + FS_2 + 1]));
                }
            }
            else if(col == FS_2)
            {
                s.x += (1 - fc.y) * (v1[FS_2].w * (fc.x * (W[row + FS_2][col + FS_2 - 1] - W[row + FS_2][col + FS_2]) + W[row + FS_2][col + FS_2])
                                     + v1[FS_2].z * fc.x * W[row + FS_2][col + FS_2]);
                s.y += fc.y * (v1[FS_2].x * (fc.x * (W[row + FS_2][col + FS_2 - 1] - W[row + FS_2][col + FS_2] ) + W[row + FS_2][col + FS_2])
                                     + v1[FS_2].y * fc.x * W[row + FS_2][col + FS_2]);
                if(row > -FS_2) {
                    s.z += (1 - fc.y) * (v0[FS_2].x * (fc.x * (W[row + FS_2 - 1][col + FS_2 - 1] - W[row + FS_2 - 1][col + FS_2])
                                        + W[row + FS_2 - 1][col + FS_2]) + v0[FS_2].y * fc.x * W[row + FS_2 - 1][col + FS_2]);
                    s.w += fc.y * (v1[FS_2].w * (fc.x * (W[row + FS_2 - 1][col + FS_2 - 1] - W[row + FS_2 - 1][col + FS_2])
                                        + W[row + FS_2 - 1][col + FS_2]) + v1[FS_2].z * fc.x * W[row + FS_2 - 1][col + FS_2]);
                }
            }
            else
            {
                s.x += (1 - fc.y) * (v1[(col + FS_2) / 2].w * (fc.x * (W[row + FS_2][col + FS_2 - 1] - W[row + FS_2][col + FS_2 + 0] ) + W[row + FS_2][col + FS_2 + 0])
                                    + v1[(col + FS_2) / 2].z * (fc.x * (W[row + FS_2][col + FS_2 - 0] - W[row + FS_2][col + FS_2 + 1]) + W[row + FS_2][col + FS_2 + 1]));
                s.y += fc.y * (v1[(col + FS_2) / 2].x * (fc.x * (W[row + FS_2][col + FS_2-1] - W[row + FS_2][col + FS_2 + 0]) + W[row + FS_2][col + FS_2 + 0])
                                    + v1[(col + FS_2) / 2].y * (fc.x * (W[row + FS_2][col + FS_2 - 0] - W[row + FS_2][col + FS_2 + 1]) + W[row + FS_2][col + FS_2 + 1]));
                if(row > -FS_2) {
                    s.z += (1 - fc.y) * (v0[(col + FS_2) / 2].x * (fc.x * (W[row + FS_2 - 1][col + FS_2 - 1] - W[row + FS_2 - 1][col + FS_2 + 0]) + W[row + FS_2 - 1][col + FS_2 + 0])
                                            + v0[(col + FS_2) / 2].y * (fc.x * (W[row + FS_2 - 1][col + FS_2 - 0] - W[row + FS_2 - 1][col + FS_2 + 1]) + W[row + FS_2 - 1][col + FS_2 + 1]));
                    s.w += fc.y * (v1[(col + FS_2) / 2].w * (fc.x * (W[row + FS_2 - 1][col + FS_2 - 1] - W[row + FS_2 - 1][col + FS_2 + 0]) + W[row + FS_2 - 1][col + FS_2 + 0])
                                            + v1[(col + FS_2) / 2].z * (fc.x * (W[row + FS_2 - 1][col + FS_2 - 0] - W[row + FS_2 - 1][col + FS_2 + 1]) + W[row + FS_2 - 1][col + FS_2 + 1]));
                }
            }

            if(row != FS_2)
                v0[(col + FS_2) / 2] = v1[(col + FS_2) / 2].xy;
        }
    }

    return dot(s, 1.0f) / w;
}

#else

//-------------------------------------------------------------------------------------------------
// Helper function for SampleShadowMapPCF
//-------------------------------------------------------------------------------------------------
float SampleShadowMap(in float2 baseUV, in float u, in float v, in float2 shadowMapSizeInv,
                      in uint arrayIdx, in float depth, in Texture2DArray shadowMap,
                      in SamplerComparisonState pcfSampler)
{
    float2 uv = baseUV + float2(u, v) * shadowMapSizeInv;
    return shadowMap.SampleCmpLevelZero(pcfSampler, float3(uv, arrayIdx), depth);
}

//-------------------------------------------------------------------------------------------------
// Samples a shadow depth map with optimized PCF filtering
//-------------------------------------------------------------------------------------------------
float SampleShadowMapPCF(in float3 shadowPos, in uint arrayIdx, in Texture2DArray shadowMap,
                               in SamplerComparisonState pcfSampler)
{
    float2 shadowMapSize;
    float numSlices;
    shadowMap.GetDimensions(shadowMapSize.x, shadowMapSize.y, numSlices);

    float lightDepth = shadowPos.z - 0.001f;

    float2 uv = shadowPos.xy * shadowMapSize; // 1 unit - 1 texel

    float2 shadowMapSizeInv = 1.0 / shadowMapSize;

    float2 baseuv = floor(uv + 0.5f);

    float s = (uv.x + 0.5 - baseuv.x);
    float t = (uv.y + 0.5 - baseuv.y);

    baseuv -= float2(0.5, 0.5);
    baseuv *= shadowMapSizeInv;

    float sum = 0;

    #define FilterSize_ 7

    #if FilterSize_ == 2
        return shadowMap.SampleCmpLevelZero(pcfSampler, float3(shadowPos.xy, arrayIdx), lightDepth);
    #elif FilterSize_ == 3

        float uw0 = (3 - 2 * s);
        float uw1 = (1 + 2 * s);

        float u0 = (2 - s) / uw0 - 1;
        float u1 = s / uw1 + 1;

        float vw0 = (3 - 2 * t);
        float vw1 = (1 + 2 * t);

        float v0 = (2 - t) / vw0 - 1;
        float v1 = t / vw1 + 1;

        sum += uw0 * vw0 * SampleShadowMap(baseuv, u0, v0, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw1 * vw0 * SampleShadowMap(baseuv, u1, v0, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw0 * vw1 * SampleShadowMap(baseuv, u0, v1, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw1 * vw1 * SampleShadowMap(baseuv, u1, v1, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);

        return sum * 1.0f / 16;

    #elif FilterSize_ == 5

        float uw0 = (4 - 3 * s);
        float uw1 = 7;
        float uw2 = (1 + 3 * s);

        float u0 = (3 - 2 * s) / uw0 - 2;
        float u1 = (3 + s) / uw1;
        float u2 = s / uw2 + 2;

        float vw0 = (4 - 3 * t);
        float vw1 = 7;
        float vw2 = (1 + 3 * t);

        float v0 = (3 - 2 * t) / vw0 - 2;
        float v1 = (3 + t) / vw1;
        float v2 = t / vw2 + 2;

        sum += uw0 * vw0 * SampleShadowMap(baseuv, u0, v0, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw1 * vw0 * SampleShadowMap(baseuv, u1, v0, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw2 * vw0 * SampleShadowMap(baseuv, u2, v0, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);

        sum += uw0 * vw1 * SampleShadowMap(baseuv, u0, v1, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw1 * vw1 * SampleShadowMap(baseuv, u1, v1, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw2 * vw1 * SampleShadowMap(baseuv, u2, v1, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);

        sum += uw0 * vw2 * SampleShadowMap(baseuv, u0, v2, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw1 * vw2 * SampleShadowMap(baseuv, u1, v2, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw2 * vw2 * SampleShadowMap(baseuv, u2, v2, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);

        return sum * 1.0f / 144;

    #else // FilterSize_ == 7

        float uw0 = (5 * s - 6);
        float uw1 = (11 * s - 28);
        float uw2 = -(11 * s + 17);
        float uw3 = -(5 * s + 1);

        float u0 = (4 * s - 5) / uw0 - 3;
        float u1 = (4 * s - 16) / uw1 - 1;
        float u2 = -(7 * s + 5) / uw2 + 1;
        float u3 = -s / uw3 + 3;

        float vw0 = (5 * t - 6);
        float vw1 = (11 * t - 28);
        float vw2 = -(11 * t + 17);
        float vw3 = -(5 * t + 1);

        float v0 = (4 * t - 5) / vw0 - 3;
        float v1 = (4 * t - 16) / vw1 - 1;
        float v2 = -(7 * t + 5) / vw2 + 1;
        float v3 = -t / vw3 + 3;

        sum += uw0 * vw0 * SampleShadowMap(baseuv, u0, v0, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw1 * vw0 * SampleShadowMap(baseuv, u1, v0, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw2 * vw0 * SampleShadowMap(baseuv, u2, v0, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw3 * vw0 * SampleShadowMap(baseuv, u3, v0, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);

        sum += uw0 * vw1 * SampleShadowMap(baseuv, u0, v1, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw1 * vw1 * SampleShadowMap(baseuv, u1, v1, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw2 * vw1 * SampleShadowMap(baseuv, u2, v1, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw3 * vw1 * SampleShadowMap(baseuv, u3, v1, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);

        sum += uw0 * vw2 * SampleShadowMap(baseuv, u0, v2, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw1 * vw2 * SampleShadowMap(baseuv, u1, v2, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw2 * vw2 * SampleShadowMap(baseuv, u2, v2, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw3 * vw2 * SampleShadowMap(baseuv, u3, v2, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);

        sum += uw0 * vw3 * SampleShadowMap(baseuv, u0, v3, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw1 * vw3 * SampleShadowMap(baseuv, u1, v3, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw2 * vw3 * SampleShadowMap(baseuv, u2, v3, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);
        sum += uw3 * vw3 * SampleShadowMap(baseuv, u3, v3, shadowMapSizeInv, arrayIdx, lightDepth, shadowMap, pcfSampler);

        return sum * 1.0f / 2704;

    #endif
}


#endif

//-------------------------------------------------------------------------------------------------
// Calculates the offset to use for sampling the shadow map, based on the surface normal
//-------------------------------------------------------------------------------------------------
float3 GetShadowPosOffset(in float nDotL, in float3 normal, in float shadowMapSize)
{
    const float offsetScale = 4.0f;
    float texelSize = 2.0f / shadowMapSize;
    float nmlOffsetScale = saturate(1.0f - nDotL);
    return texelSize * offsetScale * nmlOffsetScale * normal;
}

//--------------------------------------------------------------------------------------
// Computes the visibility for a directional light using explicit position derivatives
//--------------------------------------------------------------------------------------
float SunShadowVisibility(in float3 positionWS, in float3 positionNeighborX, in float3 positionNeighborY,
                          in float depthVS, in float3 shadowPosOffset, in float2 uvOffset,
                          in Texture2DArray sunShadowMap, in ShadowSampler shadowSampler,
                          in SunShadowConstants sunConstants)
{

    // Figure out which cascade to sample from
    uint cascadeIdx = 0;

    [unroll]
    for(uint i = 0; i < NumCascades - 1; ++i)
    {
        [flatten]
        if(depthVS > sunConstants.Base.CascadeSplits[i])
            cascadeIdx = i + 1;
    }

    // Project into shadow space
    float3 finalOffset = shadowPosOffset / abs(sunConstants.Base.CascadeScales[cascadeIdx].z);
    float3 shadowPos = mul(float4(positionWS + finalOffset, 1.0f), sunConstants.Base.ShadowMatrix).xyz;
    float3 shadowPosDX = mul(float4(positionNeighborX + finalOffset, 1.0f), sunConstants.Base.ShadowMatrix).xyz - shadowPos;
    float3 shadowPosDY = mul(float4(positionNeighborY + finalOffset, 1.0f), sunConstants.Base.ShadowMatrix).xyz - shadowPos;

    shadowPos += sunConstants.Base.CascadeOffsets[cascadeIdx].xyz;
    shadowPos *= sunConstants.Base.CascadeScales[cascadeIdx].xyz;

    shadowPosDX *= sunConstants.Base.CascadeScales[cascadeIdx].xyz;
    shadowPosDY *= sunConstants.Base.CascadeScales[cascadeIdx].xyz;

    shadowPos.xy += uvOffset;

    #if ShadowMapMode_ == ShadowMapMode_DepthMap_
        return SampleShadowMapPCF(shadowPos, cascadeIdx, sunShadowMap, shadowSampler);
    #elif ShadowMapMode_ == ShadowMapMode_EVSM_
        return SampleShadowMapEVSM(shadowPos, shadowPosDX, shadowPosDY, cascadeIdx, sunShadowMap,
                                   shadowSampler, sunConstants.Extra, sunConstants.Base.CascadeScales[cascadeIdx].xyz);
    #elif ShadowMapMode_ == ShadowMapMode_MSM_
        return SampleShadowMapMSM(shadowPos, shadowPosDX, shadowPosDY, cascadeIdx, sunShadowMap,
                                  shadowSampler, sunConstants.Extra);
    #endif
}

//--------------------------------------------------------------------------------------
// Computes the visibility for a directional light using implicit derivatives
//--------------------------------------------------------------------------------------
float SunShadowVisibility(in float3 positionWS, in float depthVS, in float3 shadowPosOffset, in float2 uvOffset,
                          in Texture2DArray sunShadowMap, in ShadowSampler shadowSampler,
                          in SunShadowConstants sunConstants)
{
    // Figure out which cascade to sample from
    uint cascadeIdx = 0;

    [unroll]
    for(uint i = 0; i < NumCascades - 1; ++i)
    {
        [flatten]
        if(depthVS > sunConstants.Base.CascadeSplits[i])
            cascadeIdx = i + 1;
    }

    // Project into shadow space
    float3 finalOffset = shadowPosOffset / abs(sunConstants.Base.CascadeScales[cascadeIdx].z);
    float3 shadowPos = mul(float4(positionWS + finalOffset, 1.0f), sunConstants.Base.ShadowMatrix).xyz;

    shadowPos += sunConstants.Base.CascadeOffsets[cascadeIdx].xyz;
    shadowPos *= sunConstants.Base.CascadeScales[cascadeIdx].xyz;

    shadowPos.xy += uvOffset;

    #if ShadowMapMode_ == ShadowMapMode_DepthMap_
        return SampleShadowMapPCF(shadowPos, cascadeIdx, sunShadowMap, shadowSampler);
    #elif ShadowMapMode_ == ShadowMapMode_EVSM_
        return SampleShadowMapEVSM(shadowPos, cascadeIdx, sunShadowMap, shadowSampler, sunConstants.Extra,
                                   sunConstants.Base.CascadeScales[cascadeIdx].xyz);
    #elif ShadowMapMode_ == ShadowMapMode_MSM_
        return SampleShadowMapMSM(shadowPos, cascadeIdx, sunShadowMap, shadowSampler, sunConstants.Extra);
    #endif
}

//--------------------------------------------------------------------------------------
// Computes the visibility for a spot light using implicit derivatives
//--------------------------------------------------------------------------------------
float SpotLightShadowVisibility(in float3 positionWS, in float4x4 shadowMatrix, in uint shadowMapIdx,
                                in float3 shadowPosOffset, in Texture2DArray shadowMap,
                                in ShadowSampler shadowSampler, in float2 clipPlanes,
                                in ExtraShadowConstants extraConstants)
{
    #if ShadowMapMode_ == ShadowMapMode_DepthMap_
        const float3 posOffset = shadowPosOffset;
    #else
        const float3 posOffset = 0.0f;
    #endif

    // Project into shadow space
    float4 shadowPos = mul(float4(positionWS + posOffset, 1.0f), shadowMatrix);
    shadowPos.xyz /= shadowPos.w;

    #if ShadowMapMode_ == ShadowMapMode_EVSM_ || ShadowMapMode_ == ShadowMapMode_MSM_
        shadowPos.z = (shadowPos.w - clipPlanes.x) / (clipPlanes.y - clipPlanes.x);
    #endif

    #if ShadowMapMode_ == ShadowMapMode_DepthMap_
        return SampleShadowMapPCF(shadowPos.xyz, shadowMapIdx, shadowMap, shadowSampler);
    #elif ShadowMapMode_ == ShadowMapMode_EVSM_
        return SampleShadowMapEVSM(shadowPos.xyz, shadowMapIdx, shadowMap, shadowSampler, extraConstants, 1.0f);
    #elif ShadowMapMode_ == ShadowMapMode_MSM_
        return SampleShadowMapMSM(shadowPos.xyz, shadowMapIdx, shadowMap, shadowSampler, extraConstants);
    #endif
}

//--------------------------------------------------------------------------------------
// Computes the visibility for a spot light using explicit position derivatives
//--------------------------------------------------------------------------------------
float SpotLightShadowVisibility(in float3 positionWS, in float3 positionNeighborX, in float3 positionNeighborY,
                                in float4x4 shadowMatrix, in uint shadowMapIdx, in float3 shadowPosOffset,
                                in Texture2DArray shadowMap, in ShadowSampler shadowSampler, in float2 clipPlanes,
                                in ExtraShadowConstants extraConstants)
{
    #if ShadowMapMode_ == ShadowMapMode_DepthMap_
        const float3 posOffset = shadowPosOffset;
    #else
        const float3 posOffset = 0.0f;
    #endif

    // Project into shadow space
    float4 shadowPos = mul(float4(positionWS + posOffset, 1.0f), shadowMatrix);
    shadowPos.xyz /= shadowPos.w;

    float4 shadowPosDX = mul(float4(positionNeighborX, 1.0f), shadowMatrix);
    shadowPosDX.xyz /= shadowPosDX.w;
    shadowPosDX.xyz -= shadowPos.xyz;

    float4 shadowPosDY = mul(float4(positionNeighborY, 1.0f), shadowMatrix);
    shadowPosDY.xyz /= shadowPosDY.w;
    shadowPosDY.xyz -= shadowPos.xyz;

    #if ShadowMapMode_ == ShadowMapMode_EVSM_ || ShadowMapMode_ == ShadowMapMode_MSM_
        shadowPos.z = (shadowPos.w - clipPlanes.x) / (clipPlanes.y - clipPlanes.x);
    #endif

    #if ShadowMapMode_ == ShadowMapMode_DepthMap_
        return SampleShadowMapPCF(shadowPos.xyz, shadowMapIdx, shadowMap, shadowSampler);
    #elif ShadowMapMode_ == ShadowMapMode_EVSM_
        return SampleShadowMapEVSM(shadowPos.xyz, shadowPosDX.xyz, shadowPosDY.xyz, shadowMapIdx, shadowMap, shadowSampler, extraConstants, 1.0f);
    #elif ShadowMapMode_ == ShadowMapMode_MSM_
        return SampleShadowMapMSM(shadowPos.xyz, shadowPosDX.xyz, shadowPosDY.xyz, shadowMapIdx, shadowMap, shadowSampler, extraConstants);
    #endif
}