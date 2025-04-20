#define MipmapWriteRootSig \
    "RootFlags(0), " \
    "DescriptorTable(SRV(t0, numDescriptors = 1))," \
    "StaticSampler(s0," \
        "addressU = TEXTURE_ADDRESS_CLAMP," \
        "addressV = TEXTURE_ADDRESS_CLAMP," \
        "addressW = TEXTURE_ADDRESS_CLAMP," \
        "filter = FILTER_MIN_MAG_MIP_LINEAR)"

struct VSOutput
{
  float4 Position : SV_POSITION;
  float2 UV0 : TEXCOORD0;
};

[RootSignature(MipmapWriteRootSig)]
VSOutput main( uint vertexID: SV_VertexID )
{
  VSOutput result = (VSOutput)0;
  float2 positions[6] =
  {
    float2(0, 0), float2(1, 0), float2(0, 1), // 三角形1
    float2(0, 1), float2(1, 0), float2(1, 1)  // 三角形2
  };
  float2 uvs[6] =
  {
    float2(0, 1), float2(1, 1), float2(0, 0),
    float2(0, 0), float2(1, 1), float2(1, 0)
  };
    
  // 頂点座標を -1 ～ 1 に変換
  result.Position = float4(positions[vertexID] * 2.0 - 1.0, 0.0, 1.0);
  result.UV0 = uvs[vertexID];

  return result;
}
