//=================================================================================================
//
//  MJP's DX12 Sample Framework
//  http://mynameismjp.wordpress.com/
//
//  All code licensed under the MIT license
//
//=================================================================================================

struct VSInput
{
    float2 Position : POSITION;
    float4 Color : COLOR;
    float2 UV : UV;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float4 Color : COLOR;
    float2 UV : UV;
};

struct ImGuiConstants
{
    row_major float4x4 ProjectionMatrix;
};

Texture2D<float4> ImGuiTexture : register(t0);
SamplerState LinearSampler : register(s0);
ConstantBuffer<ImGuiConstants> ImGuiCB : register(b0);

VSOutput ImGuiVS(in VSInput input)
{
    VSOutput output;
    output.Position = mul(float4(input.Position, 0.0f, 1.0f), ImGuiCB.ProjectionMatrix);
    output.Color = input.Color;
    output.UV = input.UV;

    return output;
}

float4 ImGuiPS(in VSOutput input) : SV_Target0
{
    return ImGuiTexture.Sample(LinearSampler, input.UV) * input.Color;
}