//=================================================================================================
//
//	MJP's DX11 Sample Framework
//  http://mynameismjp.wordpress.com/
//
//  All code licensed under the MIT license
//
//=================================================================================================

//=================================================================================================
// Resources
//=================================================================================================

// Inputs
Texture2D<float4> InputTexture : register(t0);
Texture2DArray<float4> InputTextureArray : register(t0);

// Outputs
RWBuffer<float4> OutputBuffer : register(u0);

//=================================================================================================
// Entry points
//=================================================================================================
[numthreads(TGSize_, TGSize_, 1)]
void DecodeTextureCS(in uint3 GroupID : SV_GroupID, in uint3 GroupThreadID : SV_GroupThreadID)
{
    uint2 textureSize = 0;
    InputTexture.GetDimensions(textureSize.x, textureSize.y);
	const uint2 texelIdx = GroupID.xy * uint2(TGSize_, TGSize_) + GroupThreadID.xy;
    const uint bufferIdx = texelIdx.y * textureSize.x + texelIdx.x;
	OutputBuffer[bufferIdx] = InputTexture[texelIdx];
}

[numthreads(TGSize_, TGSize_, 1)]
void DecodeTextureArrayCS(in uint3 GroupID : SV_GroupID, in uint3 GroupThreadID : SV_GroupThreadID)
{
    uint2 textureSize = 0;
    uint numSlices = 0;
    InputTextureArray.GetDimensions(textureSize.x, textureSize.y, numSlices);
	const uint3 texelIdx = uint3(GroupID.xy * uint2(TGSize_, TGSize_) + GroupThreadID.xy, GroupID.z);
    const uint bufferIdx = texelIdx.z * (textureSize.x * textureSize.y) + (texelIdx.y * textureSize.x) + texelIdx.x;
	OutputBuffer[bufferIdx] = InputTextureArray[texelIdx];
}