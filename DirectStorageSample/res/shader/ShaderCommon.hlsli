struct SceneParameters
{
    float4x4 mtxView;
    float4x4 mtxProj;
    float4 lightDir;
    float3 eyePosition;
    float time;
};
struct MeshParameters
{
    float4x4 mtxWorld;
};
struct MaterialParameters
{
    float4 baseColorFactor;
    float normalTextureScale;
    float metallicFactor;
    float roughnessFactor;
    float alphaCutoff;
    
    float3 emissiveFactor;
    uint  flags;
};
    
ConstantBuffer<SceneParameters> gScene : register(b0);
ConstantBuffer<MeshParameters> gMesh : register(b1);
ConstantBuffer<MaterialParameters> gMaterial : register(b2);

struct VSInput
{
    float4 position : POSITION;
    float3 normal : NORMAL;
    float2 texcoord0 : TEXCOORD0;
    float3 tangent : TANGENT0;
    float3 binormal : BINORMAL0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 worldPosition : POSITION;
    float3 worldNormal : NORMAL;
    float2 uv0 : TEXCOORD0;
    float3 tangent : TANGENT0;
    float3 binormal : BINORMAL0;
};