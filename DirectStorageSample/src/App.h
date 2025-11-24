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
