//=================================================================================================
//
//	MJP's DX12 Sample Framework
//  http://mynameismjp.wordpress.com/
//
//  All code licensed under the MIT license
//
//=================================================================================================

#include <DescriptorTables.hlsl>

//=================================================================================================
// Resources
//=================================================================================================

// Inputs
struct SRVIndicesLayout
{
    uint InputTexture;
};

ConstantBuffer<SRVIndicesLayout> SRVIndices : register(b0);


// Outputs
RWBuffer<float4> OutputBuffer : register(u0);

//=================================================================================================
// Entry points
//=================================================================================================
[numthreads(TGSize_, TGSize_, 1)]
void DecodeTextureCS(in uint3 GroupID : SV_GroupID, in uint3 GroupThreadID : SV_GroupThreadID)
{
    Texture2D inputTexture = Tex2DTable[SRVIndices.InputTexture];

    uint2 textureSize = 0;
    inputTexture.GetDimensions(textureSize.x, textureSize.y);
	const uint2 texelIdx = GroupID.xy * uint2(TGSize_, TGSize_) + GroupThreadID.xy;
    const uint bufferIdx = texelIdx.y * textureSize.x + texelIdx.x;
	OutputBuffer[bufferIdx] = inputTexture[texelIdx];
}

[numthreads(TGSize_, TGSize_, 1)]
void DecodeTextureArrayCS(in uint3 GroupID : SV_GroupID, in uint3 GroupThreadID : SV_GroupThreadID)
{
    Texture2DArray inputTexture = Tex2DArrayTable[SRVIndices.InputTexture];

    uint2 textureSize = 0;
    uint numSlices = 0;
    inputTexture.GetDimensions(textureSize.x, textureSize.y, numSlices);
	const uint3 texelIdx = uint3(GroupID.xy * uint2(TGSize_, TGSize_) + GroupThreadID.xy, GroupID.z);
    const uint bufferIdx = texelIdx.z * (textureSize.x * textureSize.y) + (texelIdx.y * textureSize.x) + texelIdx.x;
	OutputBuffer[bufferIdx] = inputTexture[texelIdx];
}