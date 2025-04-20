#pragma once
#define NOMINMAX

#include <memory>
#include <string>
#include <vector>
#include <wrl.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <DirectXTex.h>

#include "GfxDevice.h"
#include "Model.h"

#include "FidelityFX/host/ffx_spd.h"
#include "FidelityFX/host/backends/dx12/ffx_dx12.h"

#ifdef _DEBUG
 #pragma comment(lib, "ffx_spd_x64d.lib")
 #pragma comment(lib, "ffx_backend_dx12_x64d.lib")
#else
 #pragma comment(lib, "ffx_spd_x64.lib")
 #pragma comment(lib, "ffx_backend_dx12_x64.lib")
#endif

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

  // ミップマップ生成用各種シェーダーの準備.
  void PrepareMipmapGenShaderRT();
  void PrepareMipmapGenShaderCS();
  void PrepareMipmapGenShaderSPD();

  // リードバック用オブジェクトの準備.
  void PrepareReadbackObjects();

  ComPtr<ID3D12GraphicsCommandList> MakeCommandList();

  void UpdateModelMatrix();
  void DrawModel(ComPtr<ID3D12GraphicsCommandList> commandList);

  void DrawMipmapBase(ComPtr<ID3D12GraphicsCommandList> commandList);
  void DrawRenderTarget(ComPtr<ID3D12GraphicsCommandList> commandList);
  void DrawDefaultTarget(ComPtr<ID3D12GraphicsCommandList> commandList);

  // テクスチャレンダリングを繰り返してミップマップ生成.
  void GenerateMipmapRT(ComPtr<ID3D12GraphicsCommandList> commandList);
  
  // コンピュートシェーダーによるミップマップ生成 (Microsoftサンプルのシェーダーを活用).
  void GenerateMipmapCS(ComPtr<ID3D12GraphicsCommandList> commandList);

  // SPDによるミップマップ生成.
  void GenerateMipmapSPD(ComPtr<ID3D12GraphicsCommandList> commandList);

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


  ComPtr<ID3D12PipelineState> m_writeMipmapCSPipeline;
  ComPtr<ID3D12PipelineState> m_writeMipmapSPDPipeline;


  // レンダーターゲット情報.
  struct {
    ComPtr<ID3D12Resource1> resColorBuffer;
    GfxDevice::DescriptorHandle srvColor;
    // 各ミップマップに対するディスクリプタ.
    std::vector<GfxDevice::DescriptorHandle> rtvColorMip;
    std::vector<GfxDevice::DescriptorHandle> srvColorMip;

    // 生成に使うパイプラインおよびルートシグネチャ.
    ComPtr<ID3D12PipelineState> writeMipmapPso;
    ComPtr<ID3D12RootSignature> rootSignature;
  } m_mipmapRT;

  __declspec(align(256)) struct MipmapGenConstants
  {
    uint32_t SrcMipLevel;   // ソースとなるミップレベル
    uint32_t NumMipLevels;  // 書込みMip数
    DirectX::XMFLOAT2 TexelSize;  // テクセルサイズ. (1.0/OutMip1)
  };
  // コンピュートシェーダーDownsample
  struct {
    ComPtr<ID3D12Resource1> resColorBuffer;
    GfxDevice::DescriptorHandle rtvColor;
    GfxDevice::DescriptorHandle srvColor;
    

    // 各ミップマップに対するディスクリプタ.
    std::vector<GfxDevice::DescriptorHandle> uavColorMip;
    std::vector<GfxDevice::DescriptorHandle> srvColorMip;

    // 生成に使うパイプラインおよびルートシグネチャ.
    ComPtr<ID3D12PipelineState> writeMipmapPso[4];
    ComPtr<ID3D12RootSignature> rootSignature;

    ComPtr<ID3D12Resource1> cbMinGenBuffer;
  } m_mipmapCS;
  const UINT MaxMipmapExecLevel = 14; // 2^14:16384なので14段用意.

  // FidelityFX-SPD
  struct {
    ComPtr<ID3D12Resource1> resColorBuffer;
    GfxDevice::DescriptorHandle rtvColor; // ベースレベル.
    GfxDevice::DescriptorHandle srvColor;

    FfxSpdContext contextSpd;
    void* scratchBuffer;
  } m_mipmapSPD;

  struct {
    ComPtr<ID3D12Resource1> resDepthBuffer;
    GfxDevice::DescriptorHandle dsvDepth;
  } m_renderTargetCommonDepth;

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
  GfxDevice::DescriptorHandle m_defaultSampler;

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

  DirectX::XMFLOAT4 m_lightDir = { 0.0f, 0.5f, 1.0f, 0 };  // 平行光源(World空間).各点において光が来る方向ベクトル(真上から光が来ているなら(0,1,0)
  DirectX::XMFLOAT4 m_globalSpecular = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 30.0f);
  DirectX::XMFLOAT4 m_globalAmbient = DirectX::XMFLOAT4(0.15f, 0.15f, 0.15f, 0.0f);
  float m_frameDeltaAccum = 0.0f;
  std::wstring m_title;
  bool m_overwrite = false;
  bool m_skipGenMipmapRT = false;
  bool m_skipGenMipmapCS = false;
  bool m_skipGenMipmapSPD = false;

  const UINT RenderTexWidth = 2048;
  const UINT RenderTexHeight = 2048;

  struct {
    D3D12_VERTEX_BUFFER_VIEW vbv;
    ComPtr<ID3D12Resource1>  vertexBuffer;
    ConstantBuffer           modelCB;
  } m_drawPlaneInfo;

  // 描画時にどのテクスチャを使うか.
  enum MipTextureSource
  {
    FromRenderTarget = 0,
    FromCompute = 1,
    FromSPD = 2,
  };
  MipTextureSource m_drawMipSource = FromRenderTarget;

  // 
  struct SaveTextureRequest
  {
    ComPtr<ID3D12Resource1> resTexture; // 保存対象のテクスチャ.
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts;
    std::wstring name;
  };
  std::vector<SaveTextureRequest> m_saveTextureRequests;
  enum SaveTextureState {
    StateWaitForReadback = 0,
    StateWaitForWriteComplete = 1,
  };
  struct {
    ComPtr<ID3D12Resource1> resReadbackBuffer;
    ComPtr<ID3D12Fence1> fence;
    UINT64 nextFenceValue = 0;
    std::optional<SaveTextureState>  saveState;
  } m_readbackInfo;

  bool m_bSaveRequestButton = false;  // GUIからSaveボタンが押されたときtrue
  bool m_bSaveRequestButton2 = false;  // GUIからSaveボタンが押されたときtrue

  // 保存対象をリードバックバッファに書込み.
  void WriteToReadbackBuffer(ComPtr<ID3D12GraphicsCommandList> commandList, SaveTextureRequest* request, UINT64* pBufferOffset);

  // テクスチャ保存に関する処理.
  void ProcessSaveTexture();

  // リードバックバッファからScratchImageの作成.
  bool CreateScratchImageFromRequest(DirectX::ScratchImage& scratchImage, const SaveTextureRequest* request, const void* readbackBuffer);
};

std::unique_ptr<MyApplication>& GetApplication();
