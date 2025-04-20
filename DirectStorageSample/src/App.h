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
  void PrepareModelDrawPipeline();
  void PrepareModelData();
  void PrepareImGui();
  void DestroyImGui();
  ComPtr<ID3D12GraphicsCommandList> MakeCommandList();

  struct Vertex
  {
    DirectX::XMFLOAT3 position;
  };

  ComPtr<ID3D12RootSignature> m_rootSignature;
  ComPtr<ID3D12PipelineState> m_drawOpaquePipeline;
  ComPtr<ID3D12PipelineState> m_drawBlendPipeline;

  struct DepthBufferInfo
  {
    ComPtr<ID3D12Resource1> image;
    GfxDevice::DescriptorHandle dsvHandle;
  } m_depthBuffer;
   
  D3D12_VIEWPORT m_viewport;
  D3D12_RECT m_scissorRect;

  struct ConstantBufferInfo
  {
    ComPtr<ID3D12Resource1> buffer;
    GfxDevice::DescriptorHandle descriptorCbv;
  } m_constantBuffer[GfxDevice::BackBufferCount];

  // コンスタントバッファに送るために1要素16バイトアライメントとった状態にしておく.
  struct SceneParameters
  {
    DirectX::XMFLOAT4X4 mtxView;
    DirectX::XMFLOAT4X4 mtxProj;
    DirectX::XMFLOAT4  lightDir = { 0.0f, 0.5f, 1.0f, 0 };  // 平行光源(World空間).各点において光が来る方向ベクトル(真上から光が来ているなら(0,1,0)
    DirectX::XMFLOAT3  eyePosition;
    float    time;
  } m_sceneParams;

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
    DirectX::XMFLOAT4 diffuse{};  // xyz:色, w:アルファ.
    DirectX::XMFLOAT4 specular{};
    DirectX::XMFLOAT4 ambient{};

    GfxDevice::DescriptorHandle srvDiffuse;
    GfxDevice::DescriptorHandle samplerDiffuse;
  };

  // 定数バッファに書き込む構造体.
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

  bool  m_requestReload = false;
  bool  m_isCoolingPeriod = false;
  using time_point = std::chrono::high_resolution_clock::time_point;
  time_point m_coolingTime;
  bool  m_isPreAllocationMode = false;
  float m_frameDeltaAccum = 0.0f;
  std::wstring m_title;

  uint32_t m_currentModelCount = 50;  // ロードするモデルの個数: 1～100で設定.
  std::string  m_loadStatusMessage;
  time_point m_startLoadingTime;
  time_point m_endLoadingTime;
  std::atomic<uint32_t> m_modelCountLoadCompleted;
  std::atomic<float> m_maxCpuUtilizationInLoading;

  //std::shared_ptr<model::ModelDeserialized> m_test;
  //using ModelDataDS = std::shared_ptr<model::ModelDeserialized>;
  //std::vector<ModelDataDS> m_modelDataEntries;

  struct GraphData
  {
    std::vector<double> cpuUsages;
    std::vector<double> gpuUsages;
    std::vector<double> gpuCopyUsages;
    std::vector<double> dedicatedMemory;
    std::vector<double> sharedMemory;

    enum {
      kMaxGraphSpan = 300,
    };
  } m_graphData;

  using SampleModel = std::shared_ptr<model::SimpleModel>;
  std::vector<SampleModel> m_modelList;
  std::vector<SampleModel> m_drawList;

  void LoadModelDataByDirectStorage();
  void UnloadModelData();
  void UpdateModelMatrices();
  void DrawModels(ComPtr<ID3D12GraphicsCommandList> commandList);

  void CheckLoadingComplete();

  std::string m_strBandwidth;
  std::string m_strCpuMemData;
  std::string m_strBufferData;
  std::string m_strTextureData;

  std::vector<std::wstring> m_fileList;
};

std::unique_ptr<MyApplication>& GetApplication();
