//=================================================================================================
//
//  MJP's DX12 Sample Framework
//  http://mynameismjp.wordpress.com/
//
//  All code licensed under the MIT license
//
//=================================================================================================

//=================================================================================================
// Includes
//=================================================================================================
#include <EVSM.hlsl>
#include <MSM.hlsl>
#include <DescriptorTables.hlsl>

//=================================================================================================
// Constants
//=================================================================================================
#ifndef MSAASamples_
    #define MSAASamples_ 1
#endif

#ifndef SampleRadius_
    #define SampleRadius_ 0
#endif

//=================================================================================================
// Resources
//=================================================================================================
struct ConvertConstants
{
    float2 ShadowMapSize;
    float PositiveExponent;
    float NegativeExponent;
    float FilterSize;
    bool LinearizeDepth;
    float NearClip;
    float InvClipRange;
    float Proj33;
    float Proj43;
    uint InputMapIdx;
    uint ArraySliceIdx;
};

ConstantBuffer<ConvertConstants> CBuffer : register(b0);

float4 SMConvert(in float4 Position : SV_Position) : SV_Target0
{
    float sampleWeight = 1.0f / float(MSAASamples_);
    uint2 coords = uint2(Position.xy);

    float2 exponents = GetEVSMExponents(CBuffer.PositiveExponent, CBuffer.NegativeExponent, 1.0f);

    float4 average = float4(0.0f, 0.0f, 0.0f, 0.0f);

    // Sample indices to Load() must be literal, so force unroll
    [unroll]
    for(uint i = 0; i < MSAASamples_; ++i)
    {
        // Convert to EVSM representation
        #if MSAASamples_ > 1
            Texture2DMS<float4> shadowMap = Tex2DMSTable[CBuffer.InputMapIdx];
            float depth = shadowMap.Load(coords, i).x;
        #else
            Texture2D<float4> shadowMap = Tex2DTable[CBuffer.InputMapIdx];
            float depth = shadowMap[coords].x;
        #endif

        if(CBuffer.LinearizeDepth)
        {
            depth = CBuffer.Proj43 / (depth - CBuffer.Proj33);
            depth = (depth - CBuffer.NearClip) * CBuffer.InvClipRange;
        }

        #if MSM_
            float4 msmDepth = GetOptimizedMoments(depth);
            average += sampleWeight * msmDepth;
        #elif EVSM_
            float2 vsmDepth = WarpDepth(depth, exponents);
            average += sampleWeight * float4(vsmDepth.xy, vsmDepth.xy * vsmDepth.xy);
        #endif
    }

    return average;
}

float4 FilterSample(in float2 screenPos, in float offset, in float2 mapSize)
{
    float2 samplePos = screenPos;

    #if Vertical_
        samplePos.y = clamp(screenPos.y + offset, 0, mapSize.y);
        Texture2D<float4> shadowMap = Tex2DTable[CBuffer.InputMapIdx];
        return shadowMap[uint2(samplePos)];
    #else
        samplePos.x = clamp(screenPos.x + offset, 0, mapSize.x);
        Texture2DArray<float4> shadowMap = Tex2DArrayTable[CBuffer.InputMapIdx];
        return shadowMap[uint3(samplePos, CBuffer.ArraySliceIdx)];
    #endif
}

float4 FilterSM(in float4 Position : SV_Position) : SV_Target0
{
    const float Radius = CBuffer.FilterSize / 2.0f;

    float4 sum = 0.0f;

    [unroll]
    for(int i = -SampleRadius_; i <= SampleRadius_; ++i)
    {
        float4 sample = FilterSample(Position.xy, i, CBuffer.ShadowMapSize);

        sample *= saturate((Radius + 0.5f) - abs(i));

        sum += sample;
    }

    return sum / CBuffer.FilterSize;
}