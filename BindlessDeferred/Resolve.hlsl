//=================================================================================================
//
//  Bindless Deferred Texturing Sample
//  by MJP
//  http://mynameismjp.wordpress.com/
//
//  All code and content licensed under the MIT license
//
//=================================================================================================

//=================================================================================================
// Includes
//=================================================================================================
#include <Constants.hlsl>
#include "AppSettings.hlsl"

//=================================================================================================
// Resources
//=================================================================================================
#if Deferred_
    Texture2D<float4> InputTexture : register(t0);
#else
    Texture2DMS<float4> InputTexture : register(t0);
#endif

struct ResolveConstants
{
    uint2 OutputSize;
};

ConstantBuffer<ResolveConstants> ResolveCBuffer : register(b0);

float Luminance(in float3 clr)
{
    return dot(clr, float3(0.299f, 0.587f, 0.114f));
}

float4 ResolvePS(in float4 Position : SV_Position) : SV_Target0
{
    uint2 pixelPos = uint2(Position.xy);

    #if Deferred_
        if(InputTexture[pixelPos].w <= 1.0f)
            return float4(InputTexture[pixelPos].xyz, 1.0f);
    #endif

    const float ExposureFilterOffset = 2.0f;
    const float exposure = exp2(AppSettings.Exposure + ExposureFilterOffset) / FP16Scale;

    float3 sum = 0.0f;
    float totalWeight = 0.0f;

    [unroll]
    for(uint subSampleIdx = 0; subSampleIdx < MSAASamples_; ++subSampleIdx)
    {
        #if Deferred_
            // For the deferred path, the output target is 2x the width/height since
            // D3D doesn't support writing to MSAA textures trough a UAV
            uint2 offset = uint2(subSampleIdx % 2, subSampleIdx / 2);
            offset *= ResolveCBuffer.OutputSize;
            float3 sample = InputTexture[pixelPos + offset].xyz;
        #else
            float3 sample = InputTexture.Load(pixelPos, subSampleIdx).xyz;
        #endif

        sample = max(sample, 0.0f);

        float sampleLum = Luminance(sample);
        sampleLum *= exposure;
        float weight = 1.0f / (1.0f + sampleLum);

        sum += sample * weight;
        totalWeight += weight;
    }

    float3 output = sum / max(totalWeight, 0.00001f);
    output = max(output, 0.0f);

    return float4(output, 1.0f);
}