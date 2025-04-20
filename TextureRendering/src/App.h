#pragma once
#include <memory>
#include <string>
#include <vector>
#include <wrl.h>
#include <d3d12.h>
#include <DirectXMath.h>

#include "GfxDevice.h"
#include "Model.h"

class MyApplication 
{
  template<class T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;
public:
  MyApplication();
  std::wstring GetTitle() const { return m_title; }

  void Initialize();
  void OnUpdate();
  void Shutdown();

private:
  void PrepareDepthBuffer();
  void PrepareSceneConstantBuffer();
  void PrepareSimplePipeline();
  void PreparePlaneData();
  void PrepareModelDrawPipeline();
  void PrepareModelData();
  void PrepareImGui();
  void DestroyImGui();

  // テクスチャレンダリング用のレンダーターゲット(Color+Depth)の作成.
  void PrepareRenderTarget();

  ComPtr<ID3D12GraphicsCommandList> MakeCommandList();

  void DrawModel(ComPtr<ID3D12GraphicsCommandList> commandList);

  void DrawRenderTarget(ComPtr<ID3D12GraphicsCommandList> commandList);
  void DrawDefaultTarget(ComPtr<ID3D12GraphicsCommandList> commandList);

  struct Vertex
  {
    DirectX::XMFLOAT3 position;
  };

  ComPtr<ID3D12RootSignature> m_modelRootSignature;
  ComPtr<ID3D12RootSignature> m_simpleRootSignature;
  ComPtr<ID3D12PipelineState> m_drawOpaquePipeline;
  ComPtr<ID3D12PipelineState> m_drawBlendPipeline;
  ComPtr<ID3D12PipelineState> m_drawSimplePipeline;

  struct DepthBufferInfo
  {
    ComPtr<ID3D12Resource1> image;
    GfxDevice::DescriptorHandle dsvHandle;
  } m_depthBuffer;

  // レンダーターゲット情報.
  struct {
    ComPtr<ID3D12Resource1> resColorBuffer;
    ComPtr<ID3D12Resource1> resDepthBuffer;
    GfxDevice::DescriptorHandle rtvColor;
    GfxDevice::DescriptorHandle dsvDepth;
    GfxDevice::DescriptorHandle srvColor;
  } m_renderTarget;
   
  // コンスタントバッファに送るために1要素16バイトアライメントとった状態にしておく.
  __declspec(align(256))
  struct SceneParameters
  {
    DirectX::XMFLOAT4X4 mtxView;
    DirectX::XMFLOAT4X4 mtxProj;
    DirectX::XMFLOAT4  lightDir = { 0.0f, 0.5f, 1.0f, 0 };  // 平行光源(World空間).各点において光が来る方向ベクトル(真上から光が来ているなら(0,1,0)
    DirectX::XMFLOAT3  eyePosition;
    float    time;
  };
  using ConstantBuffer = GfxDevice::ComPtr<ID3D12Resource1>;
  ConstantBuffer m_renderSceneCB;
  ConstantBuffer m_defaultSceneCB;

  struct PolygonMesh
  {
    D3D12_VERTEX_BUFFER_VIEW vbViews[3];
    D3D12_INDEX_BUFFER_VIEW  ibv;

    ComPtr<ID3D12Resource1>  position;
    ComPtr<ID3D12Resource1>  normal;
    ComPtr<ID3D12Resource1>  texcoord0;
    ComPtr<ID3D12Resource1>  indices;

    uint32_t indexCount;
    uint32_t vertexCount;
    uint32_t materialIndex;
  };
  struct MeshMaterial
  {
    ModelMaterial::AlphaMode alphaMode;

    DirectX::XMFLOAT4 diffuse{};  // xyz:色, w:アルファ.
    DirectX::XMFLOAT4 specular{};
    DirectX::XMFLOAT4 ambient{};

    GfxDevice::DescriptorHandle srvDiffuse;
    GfxDevice::DescriptorHandle samplerDiffuse;
  };

  // 定数バッファに書き込む構造体.
  __declspec(align(256))
  struct DrawParameters
  {
    DirectX::XMFLOAT4X4 mtxWorld;
    DirectX::XMFLOAT4   baseColor; // diffuse + alpha
    DirectX::XMFLOAT4   specular;  // specular
    DirectX::XMFLOAT4   ambient;   // ambient

    uint32_t  mode;
    uint32_t  padd0;
    uint32_t  padd1;
    uint32_t  padd2;
  };

  struct TextureInfo
  {
    std::string filePath;
    ComPtr<ID3D12Resource1> texResource;
    GfxDevice::DescriptorHandle srvDescriptor;
  };
  struct DrawInfo
  {
    ComPtr<ID3D12Resource1> modelMeshConstantBuffer[GfxDevice::BackBufferCount];

    int meshIndex = -1;
    int materialIndex = -1;
  };

  struct ModelData
  {
    std::vector<PolygonMesh> meshes;
    std::vector<MeshMaterial> materials;
    std::vector<DrawInfo> drawInfos;
    std::vector<TextureInfo> textureList;
    std::vector<TextureInfo> embeddedTextures;
    DirectX::XMMATRIX mtxWorld;
  } m_model;
  std::vector<TextureInfo>::const_iterator FindModelTexture(const std::string& filePath, const ModelData& model);

  DirectX::XMFLOAT4 m_lightDir = { 0.0f, 0.5f, 1.0f, 0 };  // 平行光源(World空間).各点において光が来る方向ベクトル(真上から光が来ているなら(0,1,0)
  DirectX::XMFLOAT4 m_globalSpecular = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 30.0f);
  DirectX::XMFLOAT4 m_globalAmbient = DirectX::XMFLOAT4(0.15f, 0.15f, 0.15f, 0.0f);
  float m_frameDeltaAccum = 0.0f;
  std::wstring m_title;
  bool m_overwrite = false;

  const UINT RenderTexWidth = 2048;
  const UINT RenderTexHeight = 2048;

  struct {
    D3D12_VERTEX_BUFFER_VIEW vbv;
    ComPtr<ID3D12Resource1>  vertexBuffer;
    ConstantBuffer           modelCB;
  } m_drawPlaneInfo;
};

std::unique_ptr<MyApplication>& GetApplication();
