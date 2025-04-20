#include "ShaderCommon.hlsli"
#define M_PI     (3.1415926535897932384626433832795)
#define M_INV_PI (1.0 / M_PI)

Texture2D gTexBase : register(t0);
Texture2D gTexMetallicRoughness : register(t1);
Texture2D gTexNormalMap : register(t2);
Texture2D gTexEmissiveMap : register(t3);

SamplerState gSampler : register(s0);

float microfacetDistribution(float alphaRoughness, float dotNH)
{
  float roughnessSq = alphaRoughness * alphaRoughness;
  float f = (dotNH * roughnessSq - dotNH) * dotNH + 1.0;
  return roughnessSq / (M_PI * f * f);
}
float3 specularReflection(float3 reflectance0, float dotVH)
{
  float x = clamp(1.0 - dotVH, 0.0, 1.0);
  return reflectance0 + (1.0 - reflectance0) * pow(x, 5.0);
}
float geometricOcclusion(float alphaRoughness, float dotNL, float dotNV)
{
  float r = alphaRoughness;
  float attenuationL = 2.0 * dotNL / max(0.001, (dotNL + sqrt(r * r + (1.0 - r * r) * (dotNL * dotNL))) );
  float attenuationV = 2.0 * dotNV / max(0.001, (dotNV + sqrt(r * r + (1.0 - r * r) * (dotNV * dotNV))) );
  return attenuationL * attenuationV;
}

float3 GetNormal(PSInput input)
{
  float3 normalMap = gTexNormalMap.Sample(gSampler, input.uv0).xyz;
  normalMap = normalize(normalMap * 2.0 - 1.0);
  float3 N = normalize(input.worldNormal), T = normalize(input.tangent), B = normalize(input.binormal);
  float3x3 mtxTBN = float3x3(T, B, N);
  return normalize(mul(normalMap, mtxTBN));
}

float4 main(PSInput input) : SV_TARGET
{
  // 光源に向かうベクトル.
  float3 worldNormal = GetNormal(input); //normalize(input.worldNormal.xyz);
    float3 toLightDir = normalize(gScene.lightDir.xyz);
    float3 toViewDir = normalize(gScene.eyePosition.xyz - input.worldPosition.xyz);
    float dotNL = saturate(dot(worldNormal, toLightDir));

    float metallic = gMaterial.metallicFactor;
    float roughness = gMaterial.roughnessFactor;

    float4 texMetallicRoughness = gTexMetallicRoughness.Sample(gSampler, input.uv0);
    metallic *= texMetallicRoughness.b;
    roughness *= texMetallicRoughness.g;

    float dotNV = clamp(dot(worldNormal, toViewDir), 0, 1);
    float3 H = normalize(toViewDir + toLightDir);
    float dotNH = clamp(dot(worldNormal, H), 0, 1);
    float dotVH = clamp(dot(toViewDir, H), 0, 1);
    
    const float3 F0 = (float3)0.04;
    float alphaRoughness = roughness * roughness;

    float4 baseColor = gMaterial.baseColorFactor;
    baseColor *= gTexBase.Sample(gSampler, input.uv0);
    if ( (gMaterial.flags & 1) != 0)
    {
      clip(baseColor.a - gMaterial.alphaCutoff);
    }

    float3 diffuse = baseColor.xyz * (1.0 - F0);
    diffuse *= 1.0 - metallic;
    float3 specular = lerp(F0, baseColor.xyz, metallic);

    float  D = microfacetDistribution(alphaRoughness, dotNH);
    float3 F = specularReflection(specular, dotVH);
    float  G = geometricOcclusion(alphaRoughness, dotNL, dotNV);

    float3 diffuseContrib = (1.0 - F) * diffuse * M_INV_PI;
    float3 specularContrib = D * F * G / max(4.0 * dotNL * dotNV, 0.000001);

    float3 color;
    color = dotNL * (diffuseContrib + specularContrib);

    float3 emissive = gMaterial.emissiveFactor;
    emissive *= gTexEmissiveMap.Sample(gSampler, input.uv0).rgb;
    color += emissive;


    float exposure = 2.5;
    color.rgb *= exposure;

    float4 outColor = float4(color, baseColor.w);

    outColor = lerp(outColor, float4(worldNormal * 0.5 + 0.5, 1), 0);
    outColor = lerp(outColor, float4(roughness, roughness, roughness, 1), 0);
    return outColor;
}
