struct VSOutput
{
  float4 Position : SV_POSITION;
  float2 UV0 : TEXCOORD0;
};

Texture2D gTex : register(t0);
SamplerState gSampler : register(s0);

float4 main(VSOutput In) : SV_TARGET
{
	float4 color = float4(In.UV0, 0, 1);
	color = gTex.Sample(gSampler, In.UV0);
    return color;
}
