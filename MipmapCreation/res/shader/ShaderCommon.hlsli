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
    float4 diffuse;
    float4 specular;
    float4 ambient;
    uint mode;
};

ConstantBuffer<SceneParameters> gScene : register(b0);
ConstantBuffer<MeshParameters> gMesh : register(b1);
