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
#ifndef UseGatherPCF_
    #define UseGatherPCF_ 0
#endif

#ifndef ShadowMapMode_
    #define ShadowMapMode_ ShadowMapMode_DepthMap_
#endif

#ifndef UseReceiverPlaneBias_
    #define UseReceiverPlaneBias_ 1
#endif

//=================================================================================================
// Constants
//=================================================================================================
static const uint NumCascades = 4;

struct SunShadowConstants
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
};

struct MSMConstants
{
    float DepthBias;
    float MomentBias;
    float LightBleedingReduction;
};

struct SunShadowConstantsEVSM
{
    SunShadowConstants Base;
    EVSMConstants EVSM;
};

struct SunShadowConstantsMSM
{
    SunShadowConstants Base;
    MSMConstants MSM;
};

#if ShadowMapMode_ == ShadowMapMode_DepthMap_
    typedef uint ExtraShadowConstants;
    typedef SamplerComparisonState ShadowSampler;
#elif ShadowMapMode_ == ShadowMapMode_EVSM_
    typedef EVSMConstants ExtraShadowConstants;
    typedef SamplerState ShadowSampler;
#elif ShadowMapMode_ == ShadowMapMode_MSM_
    typedef MSMConstants ExtraShadowConstants;
    typedef SamplerState ShadowSampler;
#endif

//-------------------------------------------------------------------------------------------------
// Samples the EVSM shadow map
//-------------------------------------------------------------------------------------------------
float SampleShadowMapEVSM(in float3 shadowPos, in float3 shadowPosDX, in float3 shadowPosDY, in uint shadowMapIdx,
                          in Texture2DArray shadowMap, in SamplerState evsmSampler, in EVSMConstants evsmConstants, in float3 cascadeScale)
{
    float2 exponents = GetEVSMExponents(evsmConstants.PositiveExponent, evsmConstants.NegativeExponent, cascadeScale);
    float2 warpedDepth = WarpDepth(shadowPos.z, exponents);

    float4 occluder = shadowMap.SampleGrad(evsmSampler, float3(shadowPos.xy, shadowMapIdx), shadowPosDX.xy, shadowPosDY.xy);

    // Derivative of warping at depth
    float2 depthScale = 0.0001f * exponents * warpedDepth;
    float2 minVariance = depthScale * depthScale;

    float posContrib = ChebyshevUpperBound(occluder.xz, warpedDepth.x, minVariance.x, evsmConstants.LightBleedingReduction);
    float negContrib = ChebyshevUpperBound(occluder.yw, warpedDepth.y, minVariance.y, evsmConstants.LightBleedingReduction);
    float shadowContrib = posContrib;
    shadowContrib = min(shadowContrib, negContrib);

    return shadowContrib;
}

float SampleShadowMapEVSM(in float3 shadowPos, in float3 shadowPosDX, in float3 shadowPosDY, in Texture2D shadowMap,
                          in SamplerState evsmSampler, in EVSMConstants evsmConstants)
{
    float2 exponents = GetEVSMExponents(evsmConstants.PositiveExponent, evsmConstants.NegativeExponent, 1.0f);
    float2 warpedDepth = WarpDepth(shadowPos.z, exponents);

    float4 occluder = shadowMap.SampleGrad(evsmSampler, shadowPos.xy, shadowPosDX.xy, shadowPosDY.xy);

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
// Samples the MSM shadow map
//-------------------------------------------------------------------------------------------------
float SampleShadowMapMSM(in float3 shadowPos, in float3 shadowPosDX, in float3 shadowPosDY, in uint shadowMapIdx,
                         in Texture2DArray shadowMap, in SamplerState evsmSampler, in MSMConstants msmConstants)
{
    float4 moments = shadowMap.SampleGrad(evsmSampler, float3(shadowPos.xy, shadowMapIdx), shadowPosDX.xy, shadowPosDY.xy);
    moments = ConvertOptimizedMoments(moments);
    float result = ComputeMSMHamburger(moments, shadowPos.z, msmConstants.DepthBias, msmConstants.MomentBias);

    return ReduceLightBleeding(result, msmConstants.LightBleedingReduction);
}

//-------------------------------------------------------------------------------------------------
// Samples the MSM shadow map
//-------------------------------------------------------------------------------------------------
float SampleShadowMapMSM(in float3 shadowPos, in float3 shadowPosDX, in float3 shadowPosDY,
                         in Texture2D shadowMap, in SamplerState evsmSampler, in MSMConstants msmConstants)
{
    float4 moments = shadowMap.SampleGrad(evsmSampler, shadowPos.xy, shadowPosDX.xy, shadowPosDY.xy);
    moments = ConvertOptimizedMoments(moments);
    float result = ComputeMSMHamburger(moments, shadowPos.z, msmConstants.DepthBias, msmConstants.MomentBias);

    return ReduceLightBleeding(result, msmConstants.LightBleedingReduction);
}

//-------------------------------------------------------------------------------------------------
// Computes the factor used to bias the depth comparison value based on the derivatives of the
// shadow-space position with respect to screen-space X and Y.
//-------------------------------------------------------------------------------------------------
float2 ComputeReceiverPlaneDepthBias(in float3 uvDX, in float3 uvDY)
{
    float2 biasUV;
    biasUV.x = uvDY.y * uvDX.z - uvDX.y * uvDY.z;
    biasUV.y = uvDX.x * uvDY.z - uvDY.x * uvDX.z;
    biasUV *= 1.0f / ((uvDX.x * uvDY.y) - (uvDX.y * uvDY.x));
    return biasUV;
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
float SampleShadowMapGatherPCF(in float3 shadowPos, in float2 planeBias, in uint shadowMapIdx,
                               in Texture2DArray shadowMap, in SamplerComparisonState shadowSampler) {
    float2 shadowMapSize;
    float numSlices;
    shadowMap.GetDimensions(shadowMapSize.x, shadowMapSize.y, numSlices);
    float2 texelSize = 1.0f / shadowMapSize;

    float lightDepth = shadowPos.z;

    // Static depth biasing to make up for incorrect fractional sampling on the shadow map grid
    float fractionalError = dot(float2(1.0f, 1.0f) * texelSize, abs(planeBias));
    lightDepth -= min(fractionalError, 0.01f);

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
            {
                float sampleDepth = lightDepth;

                // Compute offset and apply planar depth bias
                float2 offset = float2(col, row) * texelSize;
                sampleDepth -= saturate(-dot(offset, planeBias));

                v1[(col + FS_2) / 2] = shadowMap.GatherCmp(shadowSampler, float3(tc.xy, shadowMapIdx), sampleDepth, int2(col, row));
            }
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

//-------------------------------------------------------------------------------------------------
// Samples the shadow map with a fixed-size PCF kernel optimized with GatherCmp. Uses code
// from "Fast Conventional Shadow Filtering" by Holger Gruen, in GPU Pro.
//-------------------------------------------------------------------------------------------------
float SampleShadowMapGatherPCF(in float3 shadowPos, in float2 planeBias, in Texture2D shadowMap,
                               in SamplerComparisonState shadowSampler) {
    float2 shadowMapSize;
    shadowMap.GetDimensions(shadowMapSize.x, shadowMapSize.y);
    float2 texelSize = 1.0f / shadowMapSize;

    float lightDepth = shadowPos.z;

    // Static depth biasing to make up for incorrect fractional sampling on the shadow map grid
    float fractionalError = dot(float2(1.0f, 1.0f) * texelSize, abs(planeBias));
    lightDepth -= min(fractionalError, 0.01f);

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
            {
                float sampleDepth = lightDepth;

                // Compute offset and apply planar depth bias
                float2 offset = float2(col, row) * texelSize;
                sampleDepth -= saturate(-dot(offset, planeBias));

                v1[(col + FS_2) / 2] = shadowMap.GatherCmp(shadowSampler, tc.xy, sampleDepth, int2(col, row));
            }
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

#endif

//-------------------------------------------------------------------------------------------------
// Samples the shadow map with a 2x2 hardware PCF kernel
//-------------------------------------------------------------------------------------------------
float SampleShadowMapSimplePCF(in float3 shadowPos, in float2 planeBias, in uint shadowMapIdx,
                               in Texture2DArray shadowMap, in SamplerComparisonState shadowSampler) {
    float2 shadowMapSize;
    float numSlices;
    shadowMap.GetDimensions(shadowMapSize.x, shadowMapSize.y, numSlices);
    float2 texelSize = 1.0f / shadowMapSize;

    float lightDepth = shadowPos.z;

    // Static depth biasing to make up for incorrect fractional sampling on the shadow map grid
    float fractionalError = dot(float2(1.0f, 1.0f) * texelSize, abs(planeBias));
    lightDepth -= min(fractionalError, 0.01f);

    return shadowMap.SampleCmpLevelZero(shadowSampler, float3(shadowPos.xy, shadowMapIdx), lightDepth);
}

//-------------------------------------------------------------------------------------------------
// Samples the shadow map with a 2x2 hardware PCF kernel
//-------------------------------------------------------------------------------------------------
float SampleShadowMapSimplePCF(in float3 shadowPos, in float2 planeBias,
                               in Texture2D shadowMap, in SamplerComparisonState shadowSampler) {
    float2 shadowMapSize;
    shadowMap.GetDimensions(shadowMapSize.x, shadowMapSize.y);
    float2 texelSize = 1.0f / shadowMapSize;

    float lightDepth = shadowPos.z;

    // Static depth biasing to make up for incorrect fractional sampling on the shadow map grid
    float fractionalError = dot(float2(1.0f, 1.0f) * texelSize, abs(planeBias));
    lightDepth -= min(fractionalError, 0.01f);

    return shadowMap.SampleCmpLevelZero(shadowSampler, shadowPos.xy, lightDepth);
}


//--------------------------------------------------------------------------------------
// Computes the visibility for a directional light using implicit derivatives
//--------------------------------------------------------------------------------------
float SunShadows(in float3 shadowPos, in float3 shadowPosDX, in float3 shadowPosDY, in float depthVS,
                 in Texture2DArray sunShadowMap, in ShadowSampler shadowSampler,
                 in SunShadowConstants sunConstants, in ExtraShadowConstants extraConstants)
{
    // Figure out which cascade to sample from
    uint cascadeIdx = 0;

    [unroll]
    for(uint i = 0; i < NumCascades - 1; ++i)
    {
        [flatten]
        if(depthVS > sunConstants.CascadeSplits[i])
            cascadeIdx = i + 1;
    }

    shadowPos += sunConstants.CascadeOffsets[cascadeIdx].xyz;
    shadowPos *= sunConstants.CascadeScales[cascadeIdx].xyz;

    shadowPosDX *= sunConstants.CascadeScales[cascadeIdx].xyz;
    shadowPosDY *= sunConstants.CascadeScales[cascadeIdx].xyz;

    #if ShadowMapMode_ == ShadowMapMode_DepthMap_
        #if UseReceiverPlaneBias_
            float2 planeBias = ComputeReceiverPlaneDepthBias(shadowPosDX, shadowPosDY);
        #else
            shadowPos.z -= 0.01f;
            float2 planeBias = 0.0f;
        #endif

        #if UseGatherPCF_
            return SampleShadowMapGatherPCF(shadowPos, planeBias, cascadeIdx, sunShadowMap, shadowSampler);
        #else
            return SampleShadowMapSimplePCF(shadowPos, planeBias, cascadeIdx, sunShadowMap, shadowSampler);
        #endif
    #elif ShadowMapMode_ == ShadowMapMode_EVSM_
        return SampleShadowMapEVSM(shadowPos, shadowPosDX, shadowPosDY, cascadeIdx, sunShadowMap,
                                   shadowSampler, extraConstants, sunConstants.CascadeScales[cascadeIdx].xyz);
    #elif ShadowMapMode_ == ShadowMapMode_MSM_
        return SampleShadowMapMSM(shadowPos, shadowPosDX, shadowPosDY, cascadeIdx, sunShadowMap,
                                  shadowSampler, extraConstants);
    #endif
}


//--------------------------------------------------------------------------------------
// Computes the visibility for a directional light using implicit derivatives
//--------------------------------------------------------------------------------------
float SunShadowVisibility(in float3 positionWS, in float depthVS,
                          in Texture2DArray sunShadowMap, in ShadowSampler shadowSampler,
                          in SunShadowConstants sunConstants, in ExtraShadowConstants extraConstants)
{
    // Project into shadow space
    float3 shadowPos = mul(float4(positionWS, 1.0f), sunConstants.ShadowMatrix).xyz;
    float3 shadowPosDX = ddx(shadowPos);
    float3 shadowPosDY = ddy(shadowPos);

    return SunShadows(shadowPos, shadowPosDX, shadowPosDY, depthVS, sunShadowMap, shadowSampler, sunConstants, extraConstants);
}

//--------------------------------------------------------------------------------------
// Computes the visibility for a directional light using explicit position derivatives
//--------------------------------------------------------------------------------------
float SunShadowVisibility(in float3 positionWS, in float3 positionNeighborX, in float3 positionNeighborY, in float depthVS,
                           in Texture2DArray sunShadowMap, in ShadowSampler shadowSampler,
                           in SunShadowConstants sunConstants, in ExtraShadowConstants extraConstants)
{
    // Project into shadow space
    float3 shadowPos = mul(float4(positionWS, 1.0f), sunConstants.ShadowMatrix).xyz;
    float3 shadowPosDX = mul(float4(positionNeighborX, 1.0f), sunConstants.ShadowMatrix).xyz - shadowPos;
    float3 shadowPosDY = mul(float4(positionNeighborY, 1.0f), sunConstants.ShadowMatrix).xyz - shadowPos;

    return SunShadows(shadowPos, shadowPosDX, shadowPosDY, depthVS, sunShadowMap, shadowSampler, sunConstants, extraConstants);
}

//--------------------------------------------------------------------------------------
// Computes the visibility for a spot light using explicit position derivatives
//--------------------------------------------------------------------------------------
float SpotLightShadows(in float4 shadowPos, in float3 shadowPosDX, in float3 shadowPosDY,
                       in float4x4 shadowMatrix, in uint shadowMapIdx,
                       in Texture2DArray shadowMap, in ShadowSampler shadowSampler,
                       in float2 clipPlanes, in ExtraShadowConstants extraConstants)
{
    #if ShadowMapMode_ == ShadowMapMode_EVSM_ || ShadowMapMode_ == ShadowMapMode_MSM_
        shadowPos.z = (shadowPos.w - clipPlanes.x) / (clipPlanes.y - clipPlanes.x);
    #endif

    #if ShadowMapMode_ == ShadowMapMode_DepthMap_
        #if UseReceiverPlaneBias_
            shadowPos.z -= 0.00001f;
            float2 planeBias = ComputeReceiverPlaneDepthBias(shadowPosDX, shadowPosDY);
        #else
            shadowPos.z -= 0.001f;
            float2 planeBias = 0.0f;
        #endif

        #if UseGatherPCF_
            return SampleShadowMapGatherPCF(shadowPos.xyz, planeBias, shadowMapIdx, shadowMap, shadowSampler);
        #else
            return SampleShadowMapSimplePCF(shadowPos.xyz, planeBias, shadowMapIdx, shadowMap, shadowSampler);
        #endif
    #elif ShadowMapMode_ == ShadowMapMode_EVSM_
        return SampleShadowMapEVSM(shadowPos.xyz, shadowPosDX, shadowPosDY, shadowMapIdx, shadowMap,
                                   shadowSampler, extraConstants, 1.0f);
    #elif ShadowMapMode_ == ShadowMapMode_MSM_
        return SampleShadowMapMSM(shadowPos.xyz, shadowPosDX, shadowPosDY, shadowMapIdx, shadowMap,
                                  shadowSampler, extraConstants);
    #endif
}

//--------------------------------------------------------------------------------------
// Computes the visibility for a spot light using explicit position derivatives
//--------------------------------------------------------------------------------------
float SpotLightShadows(in float4 shadowPos, in float3 shadowPosDX, in float3 shadowPosDY,
                       in float4x4 shadowMatrix, in Texture2D shadowMap,
                       in ShadowSampler shadowSampler, in float2 clipPlanes,
                       in ExtraShadowConstants extraConstants)
{
    #if ShadowMapMode_ == ShadowMapMode_EVSM_ || ShadowMapMode_ == ShadowMapMode_MSM_
        shadowPos.z = (shadowPos.w - clipPlanes.x) / (clipPlanes.y - clipPlanes.x);
    #endif

    #if ShadowMapMode_ == ShadowMapMode_DepthMap_
        #if UseReceiverPlaneBias_
            shadowPos.z -= 0.00001f;
            float2 planeBias = ComputeReceiverPlaneDepthBias(shadowPosDX, shadowPosDY);
        #else
            shadowPos.z -= 0.001f;
            float2 planeBias = 0.0f;
        #endif

        #if UseGatherPCF_
            return SampleShadowMapGatherPCF(shadowPos.xyz, planeBias, shadowMap, shadowSampler);
        #else
            return SampleShadowMapSimplePCF(shadowPos.xyz, planeBias, shadowMap, shadowSampler);
        #endif
    #elif ShadowMapMode_ == ShadowMapMode_EVSM_
        return SampleShadowMapEVSM(shadowPos.xyz, shadowPosDX, shadowPosDY, shadowMap,
                                   shadowSampler, extraConstants);
    #elif ShadowMapMode_ == ShadowMapMode_MSM_
        return SampleShadowMapMSM(shadowPos.xyz, shadowPosDX, shadowPosDY, shadowMap,
                                  shadowSampler, extraConstants);
    #endif
}

//--------------------------------------------------------------------------------------
// Computes the visibility for a spot light using implicit derivatives
//--------------------------------------------------------------------------------------
float SpotLightShadowVisibility(in float3 positionWS, in float4x4 shadowMatrix, in uint shadowMapIdx,
                                in Texture2DArray shadowMap, in ShadowSampler shadowSampler,
                                in float2 clipPlanes, in ExtraShadowConstants extraConstants)
{
    // Project into shadow space
    float4 shadowPos = mul(float4(positionWS, 1.0f), shadowMatrix);
    shadowPos.xyz /= shadowPos.w;

    return SpotLightShadows(shadowPos, ddx(shadowPos.xyz), ddy(shadowPos.xyz), shadowMatrix,
                            shadowMapIdx, shadowMap, shadowSampler, clipPlanes, extraConstants);
}

//--------------------------------------------------------------------------------------
// Computes the visibility for a spot light using implicit derivatives
//--------------------------------------------------------------------------------------
float SpotLightShadowVisibility(in float3 positionWS, in float4x4 shadowMatrix,
                                in Texture2D shadowMap, in ShadowSampler shadowSampler, in float2 clipPlanes,
                                in ExtraShadowConstants extraConstants)
{
    // Project into shadow space
    float4 shadowPos = mul(float4(positionWS, 1.0f), shadowMatrix);
    shadowPos.xyz /= shadowPos.w;

    return SpotLightShadows(shadowPos, ddx(shadowPos.xyz), ddy(shadowPos.xyz),
                            shadowMatrix, shadowMap, shadowSampler, clipPlanes, extraConstants);
}

//--------------------------------------------------------------------------------------
// Computes the visibility for a spot light using explicit position derivatives
//--------------------------------------------------------------------------------------
float SpotLightShadowVisibility(in float3 positionWS, in float3 positionNeighborX, in float3 positionNeighborY,
                                in float4x4 shadowMatrix, in uint shadowMapIdx, in Texture2DArray shadowMap,
                                in ShadowSampler shadowSampler, in float2 clipPlanes, in ExtraShadowConstants extraConstants)
{
    // Project into shadow space
    float4 shadowPos = mul(float4(positionWS, 1.0f), shadowMatrix);
    shadowPos.xyz /= shadowPos.w;

    float4 shadowPosDX = mul(float4(positionNeighborX, 1.0f), shadowMatrix);
    shadowPosDX.xyz /= shadowPosDX.w;
    shadowPosDX.xyz -= shadowPos.xyz;

    float4 shadowPosDY = mul(float4(positionNeighborY, 1.0f), shadowMatrix);
    shadowPosDY.xyz /= shadowPosDY.w;
    shadowPosDY.xyz -= shadowPos.xyz;

    return SpotLightShadows(shadowPos, shadowPosDX.xyz, shadowPosDY.xyz, shadowMatrix,
                            shadowMapIdx, shadowMap, shadowSampler, clipPlanes, extraConstants);
}

//--------------------------------------------------------------------------------------
// Computes the visibility for a spot light using explicit position derivatives
//--------------------------------------------------------------------------------------
float SpotLightShadowVisibility(in float3 positionWS, in float3 positionNeighborX, in float3 positionNeighborY,
                                in float4x4 shadowMatrix, in Texture2D shadowMap, in ShadowSampler shadowSampler, in float2 clipPlanes,
                                in ExtraShadowConstants extraConstants)
{
    // Project into shadow space
    float4 shadowPos = mul(float4(positionWS, 1.0f), shadowMatrix);
    shadowPos.xyz /= shadowPos.w;

    float4 shadowPosDX = mul(float4(positionNeighborX, 1.0f), shadowMatrix);
    shadowPosDX.xyz /= shadowPosDX.w;
    shadowPosDX.xyz -= shadowPos.xyz;

    float4 shadowPosDY = mul(float4(positionNeighborY, 1.0f), shadowMatrix);
    shadowPosDY.xyz /= shadowPosDY.w;
    shadowPosDY.xyz -= shadowPos.xyz;

    return SpotLightShadows(shadowPos, shadowPosDX.xyz, shadowPosDY.xyz,
                            shadowMatrix, shadowMap, shadowSampler, clipPlanes, extraConstants);
}