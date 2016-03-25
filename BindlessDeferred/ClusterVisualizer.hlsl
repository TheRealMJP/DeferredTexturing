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
struct ClusterVisConstants
{
    row_major float4x4 Projection;
    float3 ViewMin;
    float NearClip;
    float3 ViewMax;
    float InvClipRange;
    float2 DisplaySize;
    uint NumXTiles;
    uint NumXYTiles;
};

ConstantBuffer<ClusterVisConstants> CBuffer : register(b0);

ByteAddressBuffer DecalClusterBuffer : register(t0);
ByteAddressBuffer SpotLightClusterBuffer : register(t1);

// ================================================================================================
// Pixel shader for visualizing decal/light counts from an overhead view of the frustum
// ================================================================================================
float4 ClusterVisualizerPS(in float4 PositionSS : SV_Position, in float2 TexCoord : TEXCOORD) : SV_Target0
{
    float3 viewPos = lerp(CBuffer.ViewMin, CBuffer.ViewMax, float3(TexCoord.x, 0.5f, 1.0f - TexCoord.y));
    float4 projectedPos = mul(float4(viewPos, 1.0f), CBuffer.Projection);
    projectedPos.xyz /= projectedPos.w;
    projectedPos.y *= -1.0f;
    projectedPos.xy = projectedPos.xy * 0.5f + 0.5f;

    float2 screenPos = projectedPos.xy * CBuffer.DisplaySize;
    float normalizedZPos = saturate((viewPos.z - CBuffer.NearClip) * CBuffer.InvClipRange);
    uint3 tileCoords = uint3(uint2(screenPos) / ClusterTileSize, normalizedZPos * NumZTiles);
    uint clusterIdx = (tileCoords.z * CBuffer.NumXYTiles) + (tileCoords.y * CBuffer.NumXTiles) + tileCoords.x;

    if(projectedPos.x < 0.0f || projectedPos.x > 1.0f || projectedPos.y < 0.0f || projectedPos.y > 1.0f)
        return 0.0f;

    float3 output = 0.05f;

    {
        uint numLights = 0;
        uint clusterOffset = clusterIdx * SpotLightElementsPerCluster;

        [unroll]
        for(uint elemIdx = 0; elemIdx < SpotLightElementsPerCluster; ++elemIdx)
        {
            uint clusterElemMask = SpotLightClusterBuffer.Load((clusterOffset + elemIdx) * 4);
            numLights += countbits(clusterElemMask);
        }

        output.x += numLights / 10.0f;
    }

    {
        uint numDecals = 0;
        uint clusterOffset = clusterIdx * DecalElementsPerCluster;

        [unroll]
        for(uint elemIdx = 0; elemIdx < DecalElementsPerCluster; ++elemIdx)
        {
            uint clusterElemMask = DecalClusterBuffer.Load((clusterOffset + elemIdx) * 4);
            numDecals += countbits(clusterElemMask);
        }

        output.y += numDecals / 10.0f;
    }

    return float4(output, 0.9f);
}