#include "ShaderCommon.hlsli"

struct VSInput
{
    float4 position : POSITION;
    float2 texcoord0 : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv0 : TEXCOORD0;
};

PSInput main(VSInput input)
{
    PSInput result = (PSInput) 0;
    float4x4 mtxVP = mul(gScene.mtxView, gScene.mtxProj);
    float4 worldPos = mul(input.position, gMesh.mtxWorld);
    result.position = mul(worldPos, mtxVP);
    result.uv0 = input.texcoord0;
    return result;
}