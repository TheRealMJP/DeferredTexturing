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

struct PickingConstants
{
    row_major float4x4 InverseViewProjection;
    uint2 PixelPos;
    float2 RTSize;
};

ConstantBuffer<PickingConstants> CBuffer : register(b0);

struct PickingData
{
    float3 Position;
    float3 Normal;
};

#if MSAA_
    Texture2DMS<float4> TangentMap : register(t0);
    Texture2DMS<float> DepthMap : register(t1);
#else
    Texture2D<float4> TangentMap : register(t0);
    Texture2D<float> DepthMap : register(t1);
#endif

RWStructuredBuffer<PickingData> PickingBuffer : register(u0);

[numthreads(1, 1, 1)]
void PickingCS()
{
    #if MSAA_
        float zw = DepthMap.Load(CBuffer.PixelPos, 0);
        Quaternion tangentFrame = UnpackQuaternion(TangentMap.Load(CBuffer.PixelPos, 0));
    #else
        float zw = DepthMap[CBuffer.PixelPos];
        Quaternion tangentFrame = UnpackQuaternion(TangentMap[CBuffer.PixelPos]);
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