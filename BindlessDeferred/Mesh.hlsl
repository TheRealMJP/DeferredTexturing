//=================================================================================================
//
//  Bindless Deferred Texturing Sample
//  by MJP
//  http://mynameismjp.wordpress.com/
//
//  All code and content licensed under the MIT license
//
//=================================================================================================

#define UseImplicitShadowDerivatives_ 1

//=================================================================================================
// Includes
//=================================================================================================
#include "Shading.hlsl"

//=================================================================================================
// Constant buffers
//=================================================================================================
struct VSConstants
{
    row_major float4x4 World;
	row_major float4x4 View;
    row_major float4x4 WorldViewProjection;
    float NearClip;
    float FarClip;
};

struct MatIndexConstants
{
    uint MaterialTextureIndicesIdx;
    uint MatIndex;
};

struct SRVIndexConstants
{
    uint SunShadowMapIdx;
    uint SpotLightShadowMapIdx;
    uint DecalBufferIdx;
    uint DecalClusterBufferIdx;
    uint SpotLightClusterBufferIdx;
};

ConstantBuffer<VSConstants> VSCBuffer : register(b0);
ConstantBuffer<ShadingConstants> PSCBuffer : register(b0);
ConstantBuffer<SunShadowConstants> ShadowCBuffer : register(b1);
ConstantBuffer<MatIndexConstants> MatIndexCBuffer : register(b2);
ConstantBuffer<LightConstants> LightCBuffer : register(b3);
ConstantBuffer<SRVIndexConstants> SRVIndices : register(b4);

//=================================================================================================
// Resources
//=================================================================================================
StructuredBuffer<MaterialTextureIndices> MaterialIndicesBuffers[] : register(t0, space100);
StructuredBuffer<Decal> DecalBuffers[] : register(t0, space101);
StructuredBuffer<MaterialTextureIndices> MatIndicesBufferForGBuffer : register(t0);

SamplerState AnisoSampler : register(s0);
SamplerComparisonState ShadowMapSampler : register(s1);

//=================================================================================================
// Input/Output structs
//=================================================================================================
struct VSInput
{
    float3 PositionOS 		    : POSITION;
    float3 NormalOS 		    : NORMAL;
    float2 UV 		            : UV;
	float3 TangentOS 		    : TANGENT;
	float3 BitangentOS		    : BITANGENT;
};

struct VSOutput
{
    float4 PositionCS 		    : SV_Position;

    float3 NormalWS 		    : NORMALWS;
    float3 PositionWS           : POSITIONWS;
    float DepthVS               : DEPTHVS;
	float3 TangentWS 		    : TANGENTWS;
	float3 BitangentWS 		    : BITANGENTWS;
	float2 UV 		            : UV;
};

struct PSInput
{
    float4 PositionSS 		    : SV_Position;

    float3 NormalWS 		    : NORMALWS;
    float3 PositionWS           : POSITIONWS;
    float DepthVS               : DEPTHVS;
    float3 TangentWS 		    : TANGENTWS;
	float3 BitangentWS 		    : BITANGENTWS;
    float2 UV 		            : UV;
};

struct PSOutputForward
{
    float4 Color : SV_Target0;
    float4 TangentFrame : SV_Target1;
};

struct PSOutputGBuffer
{
    float4 TangentFrame : SV_Target0;
    float4 UV : SV_Target1;
    uint MaterialID : SV_Target2;
    #if OutputUVGradients_
        float4 UVGradients : SV_Target3;
    #endif
};

//=================================================================================================
// Vertex Shader
//=================================================================================================
VSOutput VS(in VSInput input, in uint VertexID : SV_VertexID)
{
    VSOutput output;

    float3 positionOS = input.PositionOS;

    // Calc the world-space position
    output.PositionWS = mul(float4(positionOS, 1.0f), VSCBuffer.World).xyz;

    // Calc the clip-space position
    output.PositionCS = mul(float4(positionOS, 1.0f), VSCBuffer.WorldViewProjection);
    output.DepthVS = output.PositionCS.w;

	// Rotate the normal into world space
    output.NormalWS = normalize(mul(float4(input.NormalOS, 0.0f), VSCBuffer.World)).xyz;

	// Rotate the rest of the tangent frame into world space
	output.TangentWS = normalize(mul(float4(input.TangentOS, 0.0f), VSCBuffer.World)).xyz;
	output.BitangentWS = normalize(mul(float4(input.BitangentOS, 0.0f), VSCBuffer.World)).xyz;

    // Pass along the texture coordinates
    output.UV = input.UV;

    return output;
}

//=================================================================================================
// Pixel Shader for clustered forward rendering
//=================================================================================================
PSOutputForward PSForward(in PSInput input)
{
	float3 vtxNormalWS = normalize(input.NormalWS);
    float3 positionWS = input.PositionWS;

	float3 tangentWS = normalize(input.TangentWS);
	float3 bitangentWS = normalize(input.BitangentWS);
	float3x3 tangentFrame = float3x3(tangentWS, bitangentWS, vtxNormalWS);

    StructuredBuffer<MaterialTextureIndices> matIndicesBuffer = MaterialIndicesBuffers[MatIndexCBuffer.MaterialTextureIndicesIdx];
    MaterialTextureIndices matIndices = matIndicesBuffer[MatIndexCBuffer.MatIndex];
    Texture2D AlbedoMap = Tex2DTable[matIndices.Albedo];
    Texture2D NormalMap = Tex2DTable[matIndices.Normal];
    Texture2D RoughnessMap = Tex2DTable[matIndices.Roughness];
    Texture2D MetallicMap = Tex2DTable[matIndices.Metallic];

    #if AlphaTest_
        const float alpha = AlbedoMap.Sample(AnisoSampler, input.UV).w;
        if(alpha < 0.5f)
            discard;
    #endif

    ShadingInput shadingInput;
    shadingInput.PositionSS = uint2(input.PositionSS.xy);
    shadingInput.PositionWS = input.PositionWS;
    shadingInput.PositionWS_DX = ddx_fine(input.PositionWS);
    shadingInput.PositionWS_DY = ddy_fine(input.PositionWS);
    shadingInput.DepthVS = input.DepthVS;
    shadingInput.TangentFrame = tangentFrame;

    shadingInput.AlbedoMap = AlbedoMap.Sample(AnisoSampler, input.UV);
    shadingInput.NormalMap = NormalMap.Sample(AnisoSampler, input.UV).xy;
    shadingInput.RoughnessMap = RoughnessMap.Sample(AnisoSampler, input.UV).x;
    shadingInput.MetallicMap = MetallicMap.Sample(AnisoSampler, input.UV).x;

    shadingInput.DecalBuffer = DecalBuffers[SRVIndices.DecalBufferIdx];
    shadingInput.DecalClusterBuffer = RawBufferTable[SRVIndices.DecalClusterBufferIdx];
    shadingInput.SpotLightClusterBuffer = RawBufferTable[SRVIndices.SpotLightClusterBufferIdx];

    shadingInput.AnisoSampler = AnisoSampler;

    shadingInput.ShadingCBuffer = PSCBuffer;
    shadingInput.ShadowCBuffer = ShadowCBuffer;
    shadingInput.LightCBuffer = LightCBuffer;

    // The DXIL validator is complaining if we do this after the wave operations inside of ShadePixel
    float3 gradients = abs(float3(ddx(input.UV), ddy(input.UV).x)) * 64.0f;

    Texture2DArray sunShadowMap = Tex2DArrayTable[SRVIndices.SunShadowMapIdx];
    Texture2DArray spotLightShadowMap = Tex2DArrayTable[SRVIndices.SpotLightShadowMapIdx];

    float3 shadingResult = ShadePixel(shadingInput, sunShadowMap, spotLightShadowMap, ShadowMapSampler);

    if(AppSettings.ShowUVGradients)
        shadingResult = gradients;

    // The tangent frame can have arbitrary handedness, so we force it to be left-handed.
    // We don't pack the handedness bit for forward rendering, since the decal picking only
    // cares about the normal direction.
    float handedness = dot(bitangentWS, cross(vtxNormalWS, tangentWS)) > 0.0f ? 1.0f : -1.0f;
    bitangentWS *= handedness;
    Quaternion tangentFrameQ = QuatFrom3x3(float3x3(tangentWS, bitangentWS, vtxNormalWS));

    PSOutputForward output;
    output.Color = float4(shadingResult, 1.0f);
    output.TangentFrame = PackQuaternion(tangentFrameQ);

    return output;
}

//=================================================================================================
// Pixel Shader for G-Buffer rendering using deferred texturing
//=================================================================================================
PSOutputGBuffer PSGBuffer(in PSInput input)
{
    float3 normalWS = normalize(input.NormalWS);
    float3 tangentWS = normalize(input.TangentWS);
    float3 bitangentWS = normalize(input.BitangentWS);

    // The tangent frame can have arbitrary handedness, so we force it to be left-handed and then
    // pack the handedness into the material ID
    float handedness = dot(bitangentWS, cross(normalWS, tangentWS)) > 0.0f ? 1.0f : -1.0f;
    bitangentWS *= handedness;

    Quaternion tangentFrame = QuatFrom3x3(float3x3(tangentWS, bitangentWS, normalWS));

    PSOutputGBuffer output;
    output.TangentFrame = PackQuaternion(tangentFrame);
    output.UV.xy = frac(input.UV / DeferredUVScale);
    output.UV.zw = float2(ddx_fine(input.PositionSS.z), ddy_fine(input.PositionSS.z));
    output.UV.zw = sign(output.UV.zw) * pow(abs(output.UV.zw), 1 / 2.0f);
    output.MaterialID = MatIndexCBuffer.MatIndex & 0x7F;
    if(handedness == -1.0f)
        output.MaterialID |= 0x80;

    #if OutputUVGradients_
        output.UVGradients = float4(ddx_fine(input.UV), ddy_fine(input.UV));
    #endif

    #if AlphaTest_
        StructuredBuffer<MaterialTextureIndices> matIndicesBuffer = MaterialIndicesBuffers[MatIndexCBuffer.MaterialTextureIndicesIdx];
        MaterialTextureIndices matIndices = matIndicesBuffer[MatIndexCBuffer.MatIndex];
        Texture2D AlbedoMap = Tex2DTable[matIndices.Albedo];
        const float alpha = AlbedoMap.Sample(AnisoSampler, input.UV).w;
        if(alpha < 0.5f)
            discard;
    #endif

    return output;
}
