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
#include "AppSettings.hlsl"

struct TileMSAAMask
{
    uint Masks[DeferredTileMaskSize];
};

//=================================================================================================
// Constant buffers
//=================================================================================================
struct MSAAMaskConstants
{
    uint NumXTiles;
    uint MaterialIDMapIdx;
    uint UVMapIdx;
};

ConstantBuffer<MSAAMaskConstants> CBuffer : register(b0);

//=================================================================================================
// Resources
//=================================================================================================
Texture2DMS<uint> MaterialIDMaps[] : register(t0, space100);
Texture2DMS<float4> UVMap : register(t1);

AppendStructuredBuffer<uint> NonMSAATiles : register(u0);
AppendStructuredBuffer<uint> MSAATiles : register(u1);
RWStructuredBuffer<TileMSAAMask> MSAAMask : register(u2);

groupshared TileMSAAMask GroupMask;

//=================================================================================================
// Pixel shader for generating a per-sample shading mask
//=================================================================================================
[numthreads(DeferredTileSize, DeferredTileSize, 1)]
void MSAAMaskCS(in uint3 DispatchID : SV_DispatchThreadID, in uint GroupIndex : SV_GroupIndex, in uint3 GroupID : SV_GroupID)
{
    uint2 pixelPos = DispatchID.xy;

    Texture2DMS<float4> uvMap = Tex2DMSTable[CBuffer.UVMapIdx];
    Texture2DMS<uint> materialIDMap = MaterialIDMaps[CBuffer.UVMapIdx];

    uint materialID = materialIDMap.Load(pixelPos, 0) & 0x7F;
    float2 zGradients = uvMap.Load(pixelPos, 0).zw;
    uint edge = 0;

    [unroll]
    for(uint i = 1; i < MSAASamples_; ++i)
    {
        edge += (materialIDMap.Load(pixelPos, i) & 0x7F) != materialID;
        #if UseZGradients_
            edge += any(abs(UVMap.Load(pixelPos, i).zw - zGradients) > 0.0025f);
        #endif
    }

    const bool perSample = edge > 0;

    if(GroupIndex < DeferredTileMaskSize)
        GroupMask.Masks[GroupIndex] = 0;

    GroupMemoryBarrierWithGroupSync();

    if(perSample)
        InterlockedOr(GroupMask.Masks[GroupIndex / 32], 1u << (GroupIndex % 32));

    GroupMemoryBarrierWithGroupSync();

    if(GroupIndex == 0)
    {
        uint packedTilePos = GroupID.x & 0xFFFF;
        packedTilePos |= (GroupID.y & 0xFFFF) << 16;

        uint groupEdge = 0;
        for(uint j = 0; j < DeferredTileMaskSize; ++j)
            groupEdge |= GroupMask.Masks[j];

        if(groupEdge)
            MSAATiles.Append(packedTilePos);
        else
            NonMSAATiles.Append(packedTilePos);

        uint tileIdx = GroupID.y * CBuffer.NumXTiles + GroupID.x;
        MSAAMask[tileIdx] = GroupMask;
    }
}