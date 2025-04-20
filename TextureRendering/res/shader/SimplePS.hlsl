#include "ShaderCommon.hlsli"

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv0 : TEXCOORD0;
};

Texture2D gTex : register(t0);
SamplerState gSampler : register(s0);

float4 main(PSInput input) : SV_TARGET
{
  return gTex.Sample(gSampler, input.uv0);
  
}