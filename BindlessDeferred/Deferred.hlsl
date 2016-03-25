//=================================================================================================
//
//  Bindless Deferred Texturing Sample
//  by MJP
//  http://mynameismjp.wordpress.com/
//
//  All code and content licensed under the MIT license
//
//=================================================================================================

#define UseImplicitShadowDerivatives_ 0

//=================================================================================================
// Includes
//=================================================================================================
#include "Shading.hlsl"

//=================================================================================================
// Constant buffers
//=================================================================================================
struct DeferredConstants
{
    row_major float4x4 InvViewProj;
    row_major float4x4 Projection;
    float2 RTSize;
    uint NumComputeTilesX;
};

ConstantBuffer<ShadingConstants> PSCBuffer : register(b0);
ConstantBuffer<ShadowConstants> ShadowCBuffer : register(b1);
ConstantBuffer<DeferredConstants> DeferredCBuffer : register(b2);

static const uint ThreadGroupSize = DeferredTileSize * DeferredTileSize;

//=================================================================================================
// Resources
//=================================================================================================
RWTexture2D<float4> OutputTexture : register(u0);

struct TileMSAAMask
{
    uint Masks[DeferredTileMaskSize];
};

Texture2DArray<float> SunShadowMap : register(t0);
Texture2DArray<float> SpotLightShadowMap : register(t1);
StructuredBuffer<float4x4> SpotLightShadowMatrices : register(t2);
StructuredBuffer<MaterialTextureIndices> MaterialIndicesBuffer : register(t3);
StructuredBuffer<Decal> DecalBuffer : register(t4);
ByteAddressBuffer DecalClusterBuffer : register(t5);
StructuredBuffer<SpotLight> SpotLightBuffer : register(t6);
ByteAddressBuffer SpotLightClusterBuffer : register(t7);
StructuredBuffer<uint> NonMSAATiles : register(t8);
StructuredBuffer<uint> MSAATiles : register(t9);
#if MSAA_
    Texture2DMS<float4> TangentFrameMap : register(t10);
    Texture2DMS<float4> UVMap : register(t11);
    Texture2DMS<float4> UVGradientMap : register(t12);
    Texture2DMS<uint> MaterialIDMap : register(t13);
    Texture2DMS<float> DepthMap : register(t14);
    Texture2DMS<float4> SkyMap : register(t15);
    StructuredBuffer<TileMSAAMask> MSAAMaskBuffer : register(t16);
#else
    Texture2D<float4> TangentFrameMap : register(t10);
    Texture2D<float4> UVMap : register(t11);
    Texture2D<float4> UVGradientMap : register(t12);
    Texture2D<uint> MaterialIDMap : register(t13);
    Texture2D<float> DepthMap : register(t14);
#endif

Texture2D<float4> MaterialTextures[NumMaterialTextures_] : register(t17);

SamplerState AnisoSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);

// MSAA subsample locations
#if NumMSAASamples_ == 4
    static const float2 SubSampleOffsets[4] = {
        float2(-0.125f, -0.375f),
        float2( 0.375f, -0.125f),
        float2(-0.375f,  0.125f),
        float2( 0.125f,  0.375f),
    };
#elif NumMSAASamples_ == 2
    static const float2 SubSampleOffsets[2] = {
        float2( 0.25f,  0.25f),
        float2(-0.25f, -0.25f),
    };
#else
    static const float2 SubSampleOffsets[1] = {
        float2(0.0f, 0.0f),
    };
#endif

#if MSAA_ && ShadePerSample_
    // List of pixels needing per-sample shading
    groupshared uint TileMSAAPixels[ThreadGroupSize];
    groupshared uint NumMSAAPixels;
#endif

#if MSAA_
    #define MSAALoad_(tex, pos, idx) tex.Load(pos, idx)
#else
    #define MSAALoad_(tex, pos, idx) tex[pos]
#endif

// Computes world-space position from post-projection depth
float3 PositionFromDepth(in float zw, in float2 uv)
{
    float linearDepth = DeferredCBuffer.Projection._43 / (zw - DeferredCBuffer.Projection._33);
    float4 positionCS = float4(uv * 2.0f - 1.0f, zw, 1.0f);
    positionCS.y *= -1.0f;
    float4 positionWS = mul(positionCS, DeferredCBuffer.InvViewProj);
    return positionWS.xyz / positionWS.w;
}

// Shades a single sample point, given a pixel position and an MSAA subsample index
void ShadeSample(in uint2 pixelPos, in uint sampleIdx, in uint numMSAASamples)
{
    Quaternion tangentFrame = UnpackQuaternion(MSAALoad_(TangentFrameMap, pixelPos, sampleIdx));
    float2 uv = MSAALoad_(UVMap, pixelPos, sampleIdx).xy * DeferredUVScale;
    uint packedMaterialID = MSAALoad_(MaterialIDMap, pixelPos, sampleIdx);
    float zw = MSAALoad_(DepthMap, pixelPos, sampleIdx);

    // Recover the tangent frame handedness from the material ID, and then reconstruct the w component
    float handedness = packedMaterialID & 0x80 ? -1.0f : 1.0f;
    float3x3 tangentFrameMatrix = QuatTo3x3(tangentFrame);
    tangentFrameMatrix._m10_m11_m12 *= handedness;

    float2 zwGradients = MSAALoad_(UVMap, pixelPos, sampleIdx).zw;

    #if ComputeUVGradients_
        // Compute gradients, trying not to walk off the edge of the triangle that isn't coplanar
        float4 zwGradUp = MSAALoad_(UVMap, int2(pixelPos) + int2(0, -1), sampleIdx);
        float4 zwGradDown = MSAALoad_(UVMap, int2(pixelPos) + int2(0, 1), sampleIdx);
        float4 zwGradLeft = MSAALoad_(UVMap, int2(pixelPos) + int2(-1, 0), sampleIdx);
        float4 zwGradRight = MSAALoad_(UVMap, int2(pixelPos) + int2(1, 0), sampleIdx);

        uint matIDUp = MSAALoad_(MaterialIDMap, int2(pixelPos) + int2(0, -1), sampleIdx);
        uint matIDDown = MSAALoad_(MaterialIDMap, int2(pixelPos) + int2(0, 1), sampleIdx);
        uint matIDLeft = MSAALoad_(MaterialIDMap, int2(pixelPos) + int2(-1, 0), sampleIdx);
        uint matIDRight = MSAALoad_(MaterialIDMap, int2(pixelPos) + int2(1, 0), sampleIdx);

        const float zwGradThreshold = 0.0025f;
        bool up = all(abs(zwGradUp.zw - zwGradients) <= zwGradThreshold) && (matIDUp == packedMaterialID);
        bool down = all(abs(zwGradDown.zw - zwGradients) <= zwGradThreshold) && (matIDDown == packedMaterialID);
        bool left = all(abs(zwGradLeft.zw - zwGradients) <= zwGradThreshold) && (matIDLeft == packedMaterialID);
        bool right = all(abs(zwGradRight.zw - zwGradients) <= zwGradThreshold) && (matIDRight == packedMaterialID);

        float2 uvDX = 0.0f;
        float2 uvDY = 0.0f;

        if(up)
            uvDY = uv - zwGradUp.xy * DeferredUVScale;
        else if(down)
            uvDY = zwGradDown.xy * DeferredUVScale - uv;

        if(left)
            uvDX = uv - zwGradLeft.xy * DeferredUVScale;
        else if(right)
            uvDX = zwGradRight.xy * DeferredUVScale - uv;

        // Check for wrapping around due to frac(), and correct for it.
        if(uvDX.x > 1.0f)
            uvDX.x -= 2.0f;
        else if(uvDX.x < -1.0f)
            uvDX.x += 2.0f;
        if(uvDX.y > 1.0f)
            uvDX.y -= 2.0f;
        else if(uvDX.y < -1.0f)
            uvDX.y += 2.0f;

        if(uvDY.x > 1.0f)
            uvDY.x -= 2.0f;
        else if(uvDY.x < -1.0f)
            uvDY.x += 2.0f;
        if(uvDY.y > 1.0f)
            uvDY.y -= 2.0f;
        else if(uvDY.y < -1.0f)
            uvDY.y += 2.0f;
    #else
        // Read the UV gradients from the G-Buffer
        float4 uvGradients = MSAALoad_(UVGradientMap, pixelPos, sampleIdx);
        float2 uvDX = uvGradients.xy;
        float2 uvDY = uvGradients.zw;
    #endif

    float2 invRTSize = 1.0f / DeferredCBuffer.RTSize;

    // Reconstruct the surface position from the depth buffer
    float linearDepth = DeferredCBuffer.Projection._43 / (zw - DeferredCBuffer.Projection._33);
    float2 screenUV = (pixelPos + 0.5f + SubSampleOffsets[sampleIdx]) * invRTSize;
    float3 positionWS = PositionFromDepth(zw, screenUV);

    // Compute the position derivatives using the stored Z derivatives
    zwGradients = sign(zwGradients) * pow(abs(zwGradients), 2.0f);
    float2 zwNeighbors = saturate(zw.xx + zwGradients);
    float3 positionDX = PositionFromDepth(zwNeighbors.x, screenUV + (int2(1, 0) * invRTSize)) - positionWS;
    float3 positionDY = PositionFromDepth(zwNeighbors.y, screenUV + (int2(0, 1) * invRTSize)) - positionWS;

    uint materialID = packedMaterialID & 0x7F;

    MaterialTextureIndices matIndices = MaterialIndicesBuffer[NonUniformResourceIndex(materialID)];
    Texture2D<float4> AlbedoMap = MaterialTextures[NonUniformResourceIndex(matIndices.Albedo)];
    Texture2D<float4> NormalMap = MaterialTextures[NonUniformResourceIndex(matIndices.Normal)];
    Texture2D<float4> RoughnessMap = MaterialTextures[NonUniformResourceIndex(matIndices.Roughness)];
    Texture2D<float4> MetallicMap = MaterialTextures[NonUniformResourceIndex(matIndices.Metallic)];

    ShadingInput shadingInput;
    shadingInput.PositionSS = pixelPos;
    shadingInput.PositionWS = positionWS;
    shadingInput.PositionWS_DX = positionDX;
    shadingInput.PositionWS_DY = positionDY;
    shadingInput.DepthVS = linearDepth;
    shadingInput.TangentFrame = tangentFrameMatrix;

    shadingInput.AlbedoMap = AlbedoMap.SampleGrad(AnisoSampler, uv, uvDX, uvDY);
    shadingInput.NormalMap = NormalMap.SampleGrad(AnisoSampler, uv, uvDX, uvDY).xy;
    shadingInput.RoughnessMap = RoughnessMap.SampleGrad(AnisoSampler, uv, uvDX, uvDY).x;
    shadingInput.MetallicMap = MetallicMap.SampleGrad(AnisoSampler, uv, uvDX, uvDY).x;

    shadingInput.SpotLightShadowMatrices = SpotLightShadowMatrices;
    shadingInput.DecalBuffer = DecalBuffer;
    shadingInput.DecalClusterBuffer = DecalClusterBuffer;
    shadingInput.SpotLightBuffer = SpotLightBuffer;
    shadingInput.SpotLightClusterBuffer = SpotLightClusterBuffer;

    shadingInput.AnisoSampler = AnisoSampler;

    shadingInput.ShadingCBuffer = PSCBuffer;
    shadingInput.ShadowCBuffer = ShadowCBuffer;

    float3 shadingResult = ShadePixel(shadingInput, SunShadowMap, SpotLightShadowMap, ShadowSampler);

    #if MSAA_
        if(zw >= 1.0f)
            shadingResult = MSAALoad_(SkyMap, pixelPos, sampleIdx).xyz;
    #endif

    #if ShadePerSample_
        if(AppSettings.ShowMSAAMask)
            shadingResult = lerp(shadingResult, float3(0, 0.0f, 5.0f), 0.5f);;
    #endif

    if(AppSettings.ShowUVGradients)
        shadingResult = abs(float3(uvDX, uvDY.x)) * 64.0f;

    uint2 outputPos = pixelPos;
    #if ShadePerSample_
        // When MSAA is enabled, the output target is 2x the width/height since D3D doesn't
        // support writing to MSAA textures trough a UAV
        uint2 offset = uint2(sampleIdx % 2, sampleIdx / 2);
        offset *= uint2(DeferredCBuffer.RTSize);
        outputPos += offset;
    #endif

    OutputTexture[outputPos] = float4(shadingResult, float(numMSAASamples));
}

//=================================================================================================
// Compute shader for deferred texturing
//=================================================================================================
[numthreads(DeferredTileSize, DeferredTileSize, 1)]
void DeferredCS(in uint3 DispatchID : SV_DispatchThreadID, in uint GroupIndex : SV_GroupIndex,
                in uint3 GroupID : SV_GroupID, in uint3 GroupThreadID : SV_GroupThreadID)
{
    #if MSAA_
        // When MSAA is enabled, we have list of Edge and non-Edge tiles in a buffer and we use
        // ExecuteIndirect to dispatch the appropriate number of thread groups for each case
        #if ShadePerSample_
            const uint packedTilePos = MSAATiles[GroupID.x];
        #else
            const uint packedTilePos = NonMSAATiles[GroupID.x];
        #endif
        const uint2 tilePos = uint2(packedTilePos & 0xFFFF, packedTilePos >> 16);
        const uint2 pixelPos = tilePos * DeferredTileSize + GroupThreadID.xy;

        #if ShadePerSample_
            // See if the pixel we're working on is an edge pixel
            const uint tileIdx = tilePos.y * DeferredCBuffer.NumComputeTilesX + tilePos.x;
            TileMSAAMask tileMask = MSAAMaskBuffer[tileIdx];
            const uint msaaEdge = tileMask.Masks[GroupIndex / 32] & (1 << (GroupIndex % 32));
            const uint numMSAASamples = msaaEdge ? NumMSAASamples_ : 1;
        #else
            const uint numMSAASamples = 1;
        #endif
    #else
        const uint2 pixelPos = DispatchID.xy;
        const uint numMSAASamples = 1;
    #endif

    ShadeSample(pixelPos, 0, numMSAASamples);

    #if ShadePerSample_
        if(GroupIndex == 0)
            NumMSAAPixels = 0;

        GroupMemoryBarrierWithGroupSync();

        if(msaaEdge)
        {
            // If this is an edge pixel, we need to shade the rest of the subsamples. To do this,
            // we build a list in thread group shared memory containing all of the edge pixels
            // and then loop over the list in batches of contiguous thread.
            uint listIndex;
            InterlockedAdd(NumMSAAPixels, 1, listIndex);
            TileMSAAPixels[listIndex] = (pixelPos.y << 16) | (pixelPos.x & 0xFFFF);
        }

        GroupMemoryBarrierWithGroupSync();

        const uint extraSamples = (NumMSAASamples_ - 1);
        const uint numSamples = NumMSAAPixels * extraSamples;

        // Shade the rest of the samples for edge pixels
        // NOTE:  this loop is unrolled to work around an issue with the shader compiler where
        //        optimization fails to converge
        [unroll(extraSamples)]
        for(uint msaaPixelIdx = GroupIndex; msaaPixelIdx < numSamples; msaaPixelIdx += ThreadGroupSize)
        {
            uint listIdx = msaaPixelIdx / extraSamples;
            uint sampleIdx = (msaaPixelIdx % extraSamples) + 1;

            uint2 samplePixelPos;
            samplePixelPos.x = TileMSAAPixels[listIdx] & 0xFFFF;
            samplePixelPos.y = TileMSAAPixels[listIdx] >> 16;

            ShadeSample(samplePixelPos, sampleIdx, NumMSAASamples_);
        }

        if(AppSettings.ShowMSAAMask && msaaEdge)
            OutputTexture[pixelPos] = float4(0.0f, 0.0f, 100.0f, 1.0f);
    #endif
}