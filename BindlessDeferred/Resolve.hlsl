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
#include <DescriptorTables.hlsl>
#include <Constants.hlsl>
#include "AppSettings.hlsl"

//=================================================================================================
// Resources
//=================================================================================================
struct ResolveConstants
{
    uint2 OutputSize;
    uint InputTextureIdx;
};

ConstantBuffer<ResolveConstants> CBuffer : register(b0);

float Luminance(in float3 clr)
{
    return dot(clr, float3(0.299f, 0.587f, 0.114f));
}

float4 ResolvePS(in float4 Position : SV_Position) : SV_Target0
{
    #if Deferred_
        Texture2D inputTexture = Tex2DTable[CBuffer.InputTextureIdx];
    #else
        Texture2DMS<float4> inputTexture = Tex2DMSTable[CBuffer.InputTextureIdx];
    #endif

    uint2 pixelPos = uint2(Position.xy);

    #if Deferred_
        if(inputTexture[pixelPos].w <= 1.0f)
            return float4(inputTexture[pixelPos].xyz, 1.0f);
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
            offset *= CBuffer.OutputSize;
            float3 texSample = inputTexture[pixelPos + offset].xyz;
        #else
            float3 texSample = inputTexture.Load(pixelPos, subSampleIdx).xyz;
        #endif

        texSample = max(texSample, 0.0f);

        float sampleLum = Luminance(texSample);
        sampleLum *= exposure;
        float weight = 1.0f / (1.0f + sampleLum);

        sum += texSample * weight;
        totalWeight += weight;
    }

    float3 output = sum / max(totalWeight, 0.00001f);
    output = max(output, 0.0f);

    return float4(output, 1.0f);
}