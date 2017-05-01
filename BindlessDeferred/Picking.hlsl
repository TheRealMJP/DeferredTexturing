//=================================================================================================
//
//  Bindless Deferred Texturing Sample
//  by MJP
//  http://mynameismjp.wordpress.com/
//
//  All code and content licensed under the MIT license
//
//=================================================================================================

#include <DescriptorTables.hlsl>
#include <Quaternion.hlsl>

struct PickingConstants
{
    row_major float4x4 InverseViewProjection;
    uint2 PixelPos;
    float2 RTSize;
    uint TangentMapIdx;
    uint DepthMapIdx;
};

ConstantBuffer<PickingConstants> CBuffer : register(b0);

struct PickingData
{
    float3 Position;
    float3 Normal;
};

RWStructuredBuffer<PickingData> PickingBuffer : register(u0);

[numthreads(1, 1, 1)]
void PickingCS()
{
    #if MSAA_
        Texture2DMS<float4> tangentMap = Tex2DMSTable[CBuffer.TangentMapIdx];
        Texture2DMS<float4> depthMap = Tex2DMSTable[CBuffer.DepthMapIdx];

        float zw = depthMap.Load(CBuffer.PixelPos, 0).x;
        Quaternion tangentFrame = UnpackQuaternion(tangentMap.Load(CBuffer.PixelPos, 0));
    #else
        Texture2D tangentMap = Tex2DTable[CBuffer.TangentMapIdx];
        Texture2D depthMap = Tex2DTable[CBuffer.DepthMapIdx];

        float zw = depthMap[CBuffer.PixelPos].x;
        Quaternion tangentFrame = UnpackQuaternion(tangentMap[CBuffer.PixelPos]);
    #endif

    float2 uv = (CBuffer.PixelPos + 0.5f) / CBuffer.RTSize;
    uv = uv * 2.0f - 1.0f;
    uv.y *= -1.0f;
    float4 positionWS = mul(float4(uv, zw, 1.0f), CBuffer.InverseViewProjection);

    float3 normal = normalize(QuatRotate(float3(0.0f, 0.0f, 1.0f), tangentFrame));

    PickingData pickingData;
    pickingData.Position =  positionWS.xyz / positionWS.w;
    pickingData.Normal = normal;
    PickingBuffer[0] = pickingData;
}