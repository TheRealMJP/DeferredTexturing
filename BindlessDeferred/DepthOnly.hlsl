//=================================================================================================
//
//  Bindless Deferred Texturing Sample
//  by MJP
//  http://mynameismjp.wordpress.com/
//
//  All code and content licensed under the MIT license
//
//=================================================================================================

#include <Quaternion.hlsl>
#include <DescriptorTables.hlsl>
#include "SharedTypes.h"

// ================================================================================================
// Constant buffers
// ================================================================================================
struct VSConstants
{
    row_major float4x4 World;
	row_major float4x4 View;
    row_major float4x4 WorldViewProjection;
};

struct MatIndexConstants
{
    uint MaterialTextureIndicesIdx;
    uint MatIndex;
};

ConstantBuffer<VSConstants> VSCBuffer : register(b0);
ConstantBuffer<MatIndexConstants> MatIndexCBuffer : register(b1);

//=================================================================================================
// Resources
//=================================================================================================
StructuredBuffer<MaterialTextureIndices> MaterialIndicesBuffers[] : register(t0, space100);

SamplerState AnisoSampler : register(s0);

// ================================================================================================
// Input/Output structs
// ================================================================================================
struct VSInput
{
    float4 PositionOS 		: POSITION;
    float2 UV               : UV;
};

struct VSOutput
{
    float4 PositionCS 		: SV_Position;
    float2 UV               : UV;
};

// ================================================================================================
// Vertex Shader
// ================================================================================================
VSOutput VS(in VSInput input)
{
    VSOutput output;

    // Calc the clip-space position
    output.PositionCS = mul(input.PositionOS, VSCBuffer.WorldViewProjection);
    output.UV = input.UV;

    return output;
}

// ================================================================================================
// Pixel Shader
// ================================================================================================
void PS(in VSOutput input)
{
    StructuredBuffer<MaterialTextureIndices> matIndicesBuffer = MaterialIndicesBuffers[MatIndexCBuffer.MaterialTextureIndicesIdx];
    MaterialTextureIndices matIndices = matIndicesBuffer[MatIndexCBuffer.MatIndex];
    Texture2D AlbedoMap = Tex2DTable[matIndices.Albedo];
    const float alpha = AlbedoMap.Sample(AnisoSampler, input.UV).w;
    if(alpha < 0.5f)
        discard;
}