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
#include <DescriptorTables.hlsl>
#include <Sampling.hlsl>
#include "AppSettings.hlsl"

//=================================================================================================
// Constant buffers
//=================================================================================================
struct SSAOConstants
{
    row_major float4x4 ViewProjection;
    row_major float4x4 InvViewProjection;
    float2 TextureSize;

    uint DepthMapIdx;
    uint TangentMapIdx;
    uint MaterialIDMapIdx;
};

ConstantBuffer<SSAOConstants> CBuffer : register(b0);

RWTexture2D<unorm float> OutputTexture : register(u0);

float3 ProjectPosition(in float3 pos, in float4x4 projection) {
    float4 projectedPos = mul(float4(pos, 1.0f), projection);
    return projectedPos.xyz / projectedPos.w;
}

//=================================================================================================
// Pixel shader for generating a per-sample shading mask
//=================================================================================================
[numthreads(8, 8, 1)]
void ComputeSSAO(in uint3 DispatchID : SV_DispatchThreadID)
{
    Texture2D depthMap = Tex2DTable[CBuffer.DepthMapIdx];
    Texture2D tangentMap = Tex2DTable[CBuffer.TangentMapIdx];
    Texture2D materialIDMap = Tex2DTable[CBuffer.MaterialIDMapIdx];

    const float2 pixelCoord = DispatchID.xy + 0.5f;

    const float centerDepth = depthMap[uint2(pixelCoord)].x;
    const float2 centerPosUV = pixelCoord / CBuffer.TextureSize;
    const float3 centerPosNDC = float3((centerPosUV * 2.0f - 1.0f) * float2(1.0f, -1.0f), centerDepth);
    const float3 centerPosWS = ProjectPosition(centerPosNDC, CBuffer.InvViewProjection);

    const Quaternion tangentFrame = UnpackQuaternion(tangentMap[uint2(pixelCoord)]);
    float3x3 tangentFrameMatrix = QuatTo3x3(tangentFrame);
    const float3 centerNormalWS = tangentFrameMatrix._m20_m21_m22;

    float visibilitySum = 0.0f;

    const uint SqrtNumSamples = 4;
    const uint NumSamples = SqrtNumSamples * SqrtNumSamples;
    for(uint sampleIdx = 0; sampleIdx < NumSamples; ++sampleIdx)
    {
        const float2 samplePoints = SampleCMJ2D(sampleIdx, SqrtNumSamples, SqrtNumSamples, 0);
        const float3 sampleDirTS = SampleDirectionCosineHemisphere(samplePoints.x, samplePoints.y);
        const float3 sampleDirWS = mul(sampleDirTS, tangentFrameMatrix);

        const float sampleRadius = 0.1f;

        const float3 sampleRayEndWS = centerPosWS + sampleDirWS * sampleRadius;
        const float3 sampleRayEndNDC = ProjectPosition(sampleRayEndWS, CBuffer.ViewProjection);
        const float2 sampleRayEndCoord = (sampleRayEndNDC.xy * float2(0.5f, -0.5f) + 0.5f) * CBuffer.TextureSize;
        const float depthBufferDepth = depthMap[uint2(sampleRayEndCoord)].x;

        const float depthThreshold = 0.005f;
        const float depthDifference = sampleRayEndNDC.z - depthBufferDepth;

        visibilitySum += depthBufferDepth >= sampleRayEndNDC.z || depthDifference > depthThreshold  ? 1.0f : 0.0f;
    }

    float visibility = visibilitySum / NumSamples;

    OutputTexture[uint2(pixelCoord)] = visibility * visibility;
}