//=================================================================================================
// Constant buffers
//=================================================================================================
/*struct SSAOConstants
{
    uint DepthMapIdx;
    uint TangentMapIdx;
};

ConstantBuffer<SSAOConstants> CBuffer : register(b0);*/

RWTexture2D<unorm float> OutputTexture : register(u0);

//=================================================================================================
// Pixel shader for generating a per-sample shading mask
//=================================================================================================
[numthreads(8, 8, 1)]
void ComputeSSAO(in uint3 DispatchID : SV_DispatchThreadID)
{
    const float2 pixelPos = DispatchID.xy + 0.5f;
    OutputTexture[uint2(pixelPos)] = length(frac(pixelPos / 64.0f));
}