#include "App.h"

#include "GfxDevice.h"
#include "FileLoader.h"
#include "Win32Application.h"

#include "imgui.h"
#include "imgui/backends/imgui_impl_dx12.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "implot.h"

#include "DStorageLoader.h"
#include "TextureUtility.h"
#include <DirectXTex.h>
#include <fstream>
#include <random>

#include <windows.h>

#include "tiny_performance_counter.h"

using namespace Microsoft::WRL;
using namespace DirectX;
namespace tpc = tiny_perf_counter;

static std::unique_ptr<MyApplication> gMyApplication;
std::unique_ptr<MyApplication>& GetApplication()
{
  if (gMyApplication == nullptr)
  {
    gMyApplication = std::make_unique<MyApplication>();
  }
  return gMyApplication;
}

DirectX::XMMATRIX CalculateWorldMatrix(int gx, int gy, int gz, std::shared_ptr<model::SimpleModel> model, float gridSize, float cellSize)
{
  DirectX::XMFLOAT3 aabbMin, aabbMax;
  model->GetModelAABB(aabbMin, aabbMax);

  float modelWidth = aabbMax.x - aabbMin.x;
  float modelHeight = aabbMax.y - aabbMin.y;
  float modelDepth = aabbMax.z - aabbMin.z;
  float scale = cellSize / (std::max)({ modelWidth, modelHeight, modelDepth });
  float gridCenterOffset = (gridSize * cellSize) / 2.0f - cellSize / 2.0f;

  float px = gx * cellSize - gridCenterOffset;
  float py = gy * cellSize - gridCenterOffset;
  float pz = gz * cellSize - gridCenterOffset;

  scale *= 0.75f;
  auto mtxScale = DirectX::XMMatrixScaling(scale, scale, scale);
  auto mtxTranslation = DirectX::XMMatrixTranslation(px, py, pz);
  auto mtxRotation = DirectX::XMMatrixRotationAxis(model->m_tumbleAxis, model->m_tumbleAngle);
  return mtxScale * mtxRotation * mtxTranslation;
}


MyApplication::MyApplication()
{
  m_title = L"DirectStorageSample";
}

void MyApplication::Initialize()
{
  {
    // パフォーマンスカウンタライブラリ初期化.
    tpc::InitParams initParams{
      .useGlobalCPUUtilization = false, // 本プロセスでの使用情報
    };
    tpc::Initialize(initParams);
  }

  auto& gfxDevice = GetGfxDevice();
  GfxDevice::DeviceInitParams initParams;
  initParams.formatDesired = DXGI_FORMAT_R8G8B8A8_UNORM;
  gfxDevice->Initialize(initParams);

  auto& loaderDStorage = GetDStorageLoader();
  loaderDStorage->Initialize(gfxDevice->GetD3D12Device().Get());

  PrepareDepthBuffer();

  PrepareImGui();

  PrepareSceneConstantBuffer();

  PrepareModelDrawPipeline();

  PrepareModelData();

  // ビューポートおよびシザー領域の設定.
  int width, height;
  Win32Application::GetWindowSize(width, height);
  m_viewport = D3D12_VIEWPORT{
    .TopLeftX = 0.0f, .TopLeftY = 0.0f,
    .Width = float(width),
    .Height = float(height),
    .MinDepth = 0.0f, .MaxDepth = 1.0f,
  };
  m_scissorRect = D3D12_RECT{
    .left = 0, .top = 0,
    .right = width, .bottom = height,
  };

}

void MyApplication::PrepareDepthBuffer()
{
  auto& gfxDevice = GetGfxDevice();
  int width, height;
  Win32Application::GetWindowSize(width, height);

  D3D12_RESOURCE_DESC resDesc{
    .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
    .Alignment = 0,
    .Width = UINT(width), .Height = UINT(height),
    .DepthOrArraySize = 1,
    .MipLevels = 1,
    .Format = DXGI_FORMAT_D32_FLOAT,
    .SampleDesc = { .Count = 1, .Quality = 0},
    .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
    .Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
  };
  D3D12_HEAP_PROPERTIES heapProps{
    .Type = D3D12_HEAP_TYPE_DEFAULT,
    .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
    .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
    .CreationNodeMask = 1, .VisibleNodeMask = 1,
  };
  D3D12_CLEAR_VALUE depthClear{
    .Format = resDesc.Format,
    .DepthStencil { .Depth = 1.0f, .Stencil = 0 },
  };
  m_depthBuffer.image = gfxDevice->CreateImage2D(resDesc, heapProps, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear);

  D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{
    .Format = resDesc.Format,
    .ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D,
    .Flags = D3D12_DSV_FLAG_NONE,
    .Texture2D = {
      .MipSlice = 0
    }
  };
  m_depthBuffer.dsvHandle = gfxDevice->CreateDepthStencilView(m_depthBuffer.image, dsvDesc);

}

void MyApplication::PrepareSceneConstantBuffer()
{
  auto& gfxDevice = GetGfxDevice();
  UINT constantBufferSize = sizeof(SceneParameters);
  constantBufferSize = (constantBufferSize + 255) & ~255u;
  D3D12_RESOURCE_DESC cbResDesc{
    .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
    .Alignment = 0,
    .Width = constantBufferSize,
    .Height = 1,
    .DepthOrArraySize = 1,
    .MipLevels = 1,
    .Format = DXGI_FORMAT_UNKNOWN,
    .SampleDesc = {.Count = 1, .Quality = 0},
    .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    .Flags = D3D12_RESOURCE_FLAG_NONE
  };
  for (UINT i = 0; i < GfxDevice::BackBufferCount; ++i)
  {
    auto buffer = gfxDevice->CreateBuffer(cbResDesc, D3D12_HEAP_TYPE_UPLOAD);

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{
      .BufferLocation = buffer->GetGPUVirtualAddress(),
      .SizeInBytes = constantBufferSize,
    };
    m_constantBuffer[i].buffer = buffer;
    m_constantBuffer[i].descriptorCbv = gfxDevice->CreateConstantBufferView(cbvDesc);
  }
}

void MyApplication::PrepareModelDrawPipeline()
{
  auto& gfxDevice = GetGfxDevice();
  auto& loader = GetFileLoader();

  // 描画のためのパイプラインステートオブジェクトを作成.
  // ルートシグネチャの作成.
  D3D12_DESCRIPTOR_RANGE rangeSrvRanges[] = {
    {  // t0 モデルのベーステクスチャ用.
      .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
      .NumDescriptors = 4,
      .BaseShaderRegister = 0,
      .RegisterSpace = 0,
      .OffsetInDescriptorsFromTableStart = 0,
    }
  };
  D3D12_DESCRIPTOR_RANGE rangeSamplerRanges[] = {
    {  // s0 モデルのベーステクスチャ用のサンプラー.
      .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
      .NumDescriptors = 1,
      .BaseShaderRegister = 0,
      .RegisterSpace = 0,
      .OffsetInDescriptorsFromTableStart = 0,
    }
  };

  D3D12_ROOT_PARAMETER rootParams[] = {
    {
      .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
      .Constants = {
        .ShaderRegister = 0,
        .RegisterSpace = 0,
      },
      .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL
    },
    {
      .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
      .Constants = {
        .ShaderRegister = 1,
        .RegisterSpace = 0,
      },
      .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL
    },
    {
      .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
      .Constants = {
        .ShaderRegister = 2,
        .RegisterSpace = 0,
      },
      .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL
    },

    {
      .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
      .DescriptorTable = {
        .NumDescriptorRanges = _countof(rangeSrvRanges),
        .pDescriptorRanges = rangeSrvRanges,
      },
      .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
    },
    {
      .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
      .DescriptorTable = {
        .NumDescriptorRanges = _countof(rangeSamplerRanges),
        .pDescriptorRanges = rangeSamplerRanges,
      },
      .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
    },
  };

  D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{
    .NumParameters = _countof(rootParams),
    .pParameters = rootParams,
    .NumStaticSamplers = 0,
    .pStaticSamplers = nullptr,
    .Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
  };

  ComPtr<ID3DBlob> signature;
  ComPtr<ID3DBlob> error;
  D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
  m_rootSignature = gfxDevice->CreateRootSignature(signature);

  // 頂点データのインプットレイアウト情報を作成.
  D3D12_INPUT_ELEMENT_DESC inputElementDesc[] = {
    {
      .SemanticName = "POSITION", .SemanticIndex = 0,
      .Format = DXGI_FORMAT_R32G32B32_FLOAT,
      .InputSlot = 0, .AlignedByteOffset = 0,
      .InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
      .InstanceDataStepRate = 0,
    },
    {
      .SemanticName = "NORMAL", .SemanticIndex = 0,
      .Format = DXGI_FORMAT_R32G32B32_FLOAT,
      .InputSlot = 0, .AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT,
      .InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
      .InstanceDataStepRate = 0,
    },
    {
      .SemanticName = "TEXCOORD", .SemanticIndex = 0,
      .Format = DXGI_FORMAT_R32G32_FLOAT,
      .InputSlot = 0, .AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT,
      .InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
      .InstanceDataStepRate = 0,
    },
    {
      .SemanticName = "TANGENT", .SemanticIndex = 0,
      .Format = DXGI_FORMAT_R32G32B32_FLOAT,
      .InputSlot = 0, .AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT,
      .InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
      .InstanceDataStepRate = 0,
    },
    {
      .SemanticName = "BINORMAL", .SemanticIndex = 0,
      .Format = DXGI_FORMAT_R32G32B32_FLOAT,
      .InputSlot = 0, .AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT,
      .InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
      .InstanceDataStepRate = 0,
    },
  };
  D3D12_INPUT_LAYOUT_DESC inputLayout{
    .pInputElementDescs = inputElementDesc,
    .NumElements = _countof(inputElementDesc),
  };
  // シェーダーコードの読み込み.
  std::vector<char> vsdata, psdata;
  loader->Load(L"res/shader/VertexShader.cso", vsdata);
  loader->Load(L"res/shader/PixelShader.cso", psdata);
  D3D12_SHADER_BYTECODE vs{
    .pShaderBytecode = vsdata.data(),
    .BytecodeLength = vsdata.size(),
  };
  D3D12_SHADER_BYTECODE ps{
    .pShaderBytecode = psdata.data(),
    .BytecodeLength = psdata.size(),
  };

  // パイプラインステートオブジェクト作成時に使う各種ステート情報を準備.
  D3D12_BLEND_DESC blendState{
    .AlphaToCoverageEnable = FALSE,
    .IndependentBlendEnable = FALSE,
    .RenderTarget = {
      D3D12_RENDER_TARGET_BLEND_DESC{
        .BlendEnable = TRUE,
        .LogicOpEnable = FALSE,
        .SrcBlend = D3D12_BLEND_ONE,
        .DestBlend = D3D12_BLEND_ZERO,
        .BlendOp = D3D12_BLEND_OP_ADD,
        .SrcBlendAlpha = D3D12_BLEND_ONE,
        .DestBlendAlpha = D3D12_BLEND_ZERO,
        .BlendOpAlpha = D3D12_BLEND_OP_ADD,
        .LogicOp = D3D12_LOGIC_OP_NOOP,
        .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL
      },
    }
  };

  D3D12_RASTERIZER_DESC rasterizerState{
    .FillMode = D3D12_FILL_MODE_SOLID,
    .CullMode = D3D12_CULL_MODE_BACK,
    .FrontCounterClockwise = TRUE,
    .DepthBias = D3D12_DEFAULT_DEPTH_BIAS,
    .DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
    .SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS,
    .DepthClipEnable = TRUE,
    .MultisampleEnable = FALSE,
    .AntialiasedLineEnable = FALSE,
    .ForcedSampleCount = 0,
    .ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF
  };
  const D3D12_DEPTH_STENCILOP_DESC defaultStencilOp = {
    .StencilFailOp = D3D12_STENCIL_OP_KEEP,
    .StencilDepthFailOp = D3D12_STENCIL_OP_KEEP,
    .StencilPassOp = D3D12_STENCIL_OP_KEEP,
    .StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS
  };
  D3D12_DEPTH_STENCIL_DESC depthStencilState{
    .DepthEnable = TRUE,
    .DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL,
    .DepthFunc = D3D12_COMPARISON_FUNC_LESS,
    .StencilEnable = FALSE,
    .StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK,
    .StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK,
    .FrontFace = defaultStencilOp, .BackFace = defaultStencilOp
  };

  // 情報が揃ったのでパイプラインステートオブジェクトを作成する.
  D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {
    .pRootSignature = m_rootSignature.Get(),
    .VS = vs, .PS = ps,
    .BlendState = blendState,
    .SampleMask = UINT_MAX,
    .RasterizerState = rasterizerState,
    .InputLayout = inputLayout, 
    .PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
    .SampleDesc = { .Count = 1, .Quality = 0 }
  };
  psoDesc.DepthStencilState = depthStencilState;
  psoDesc.NumRenderTargets = 1;
  psoDesc.RTVFormats[0] = gfxDevice->GetSwapchainFormat();
  psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  m_drawOpaquePipeline = gfxDevice->CreateGraphicsPipelineState(psoDesc);

  // アルファブレンド用の設定.
  D3D12_DEPTH_STENCIL_DESC dssBlend = depthStencilState;
  dssBlend.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  auto& rt0 = blendState.RenderTarget[0];
  rt0.BlendEnable = TRUE;
  rt0.SrcBlend = D3D12_BLEND_SRC_ALPHA;
  rt0.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
  rt0.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  rt0.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;;

  psoDesc.DepthStencilState = dssBlend;
  psoDesc.BlendState = blendState;
  m_drawBlendPipeline = gfxDevice->CreateGraphicsPipelineState(psoDesc);
}

void MyApplication::PrepareModelData()
{
  model::DefaultTextures::Initialize();

  // ファイルの読み込みパスを設定.
  // ※ gdeflate, uncompress のものについてはコンバーターで処理後のデータが必要.リポジトリには未コミット.
  std::vector<std::wstring> fileList;
  std::wstring searchPath = L"res/pakdata/texcompress";
  //searchPath = L"res/pakdata/gdeflate";
  //searchPath = L"res/pakdata/uncompress";

  for (const auto& entry : std::filesystem::directory_iterator(searchPath))
  {
    if (entry.is_regular_file())
    {
      auto fname = entry.path();
      fname.make_preferred();
      if (fname.extension() == ".pak")
      {
        fileList.push_back(fname.wstring());
      }
    }
  }
  constexpr size_t numInstancesMax = 200;
  for (size_t i = 0; i < std::max<size_t>(1, numInstancesMax / fileList.size()); ++i)
  {
    m_fileList.insert(m_fileList.end(), fileList.begin(), fileList.end());
  }
  if (auto remain = numInstancesMax - m_fileList.size(); remain > 0)
  {
    m_fileList.insert(m_fileList.end(), fileList.begin(), fileList.begin() + remain);
  }

  int count = 100;
  m_modelList.reserve(count);
  LoadModelDataByDirectStorage();
}

void MyApplication::PrepareImGui()
{
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui_ImplWin32_Init(Win32Application::GetHwnd());
  ImPlot::CreateContext();

  auto& gfxDevice = GetGfxDevice();
  auto d3d12Device = gfxDevice->GetD3D12Device();
  // ImGui のフォントデータ用にディスクリプタを割り当てる.
  auto heapCbvSrv = gfxDevice->GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto fontDescriptor = gfxDevice->AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  ImGui_ImplDX12_Init(d3d12Device.Get(),
    gfxDevice->BackBufferCount,
    gfxDevice->GetSwapchainFormat(),
    heapCbvSrv.Get(),
    fontDescriptor.hCpu, fontDescriptor.hGpu
    );
}

void MyApplication::DestroyImGui()
{
  ImGui_ImplDX12_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImPlot::DestroyContext();
  ImGui::DestroyContext();
}

template<typename T>
void ShrinkGraphData(std::vector<T>& dataList, size_t span)
{
  if (dataList.size() > span)
  {
    dataList.erase(dataList.begin());
  }
}

void MyApplication::OnUpdate()
{
  m_graphData.cpuUsages.push_back(tpc::GetCPUUtilization());
  m_graphData.gpuUsages.push_back(tpc::GetGPUEngineUtilization());
  m_graphData.gpuCopyUsages.push_back(tpc::GetGPUEngineUtilization(L"Copy"));
  m_graphData.dedicatedMemory.push_back(tpc::GetUsedGPUDedicatedMemory() / (1024 * 1024.0) );
  m_graphData.sharedMemory.push_back(tpc::GetUsedGPUSharedMemory() / (1024 * 1024.0));
  ShrinkGraphData(m_graphData.cpuUsages, GraphData::kMaxGraphSpan);
  ShrinkGraphData(m_graphData.gpuUsages, GraphData::kMaxGraphSpan);
  ShrinkGraphData(m_graphData.gpuCopyUsages, GraphData::kMaxGraphSpan);
  ShrinkGraphData(m_graphData.dedicatedMemory, GraphData::kMaxGraphSpan);
  ShrinkGraphData(m_graphData.sharedMemory, GraphData::kMaxGraphSpan);

  auto currentCPUUtilization = tpc::GetCPUUtilization();

  if (m_requestReload)
  {
    UnloadModelData();
    m_isCoolingPeriod = true;
    m_coolingTime = std::chrono::high_resolution_clock::now() + std::chrono::seconds(3);
    m_requestReload = false;
    m_loadStatusMessage = "Unload & Cooling";
  }
  if (m_isCoolingPeriod && m_coolingTime < std::chrono::high_resolution_clock::now())
  {
    LoadModelDataByDirectStorage();
    m_isCoolingPeriod = false;
  }

  bool isLoadedAll = std::all_of(m_modelList.begin(), m_modelList.end(), [](auto v) { return v->IsFinishLoading(); });
  if (!isLoadedAll)
  {
    GetDStorageLoader()->GetQueueSystemMemory()->Submit();
    GetDStorageLoader()->GetQueueGpuMemory()->Submit();
  }
  if (isLoadedAll && !m_requestReload && !m_isCoolingPeriod)
  {
    m_loadStatusMessage = "Completed.";
  }

  // ロード自体に掛かった時間を計算. ローディング中は経過時間を求める.
  using namespace std::chrono;
  auto elapsedTime = (isLoadedAll ? m_endLoadingTime : high_resolution_clock::now()) - m_startLoadingTime;
  auto elapsedMilliseconds = duration_cast<milliseconds>(elapsedTime).count();

  // ImGui更新処理.
  ImGui_ImplDX12_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();

  // ImGuiを使用したUIの描画指示.
  ImGui::Begin("Information");
  ImGui::Text("FPS: %.2f", ImGui::GetIO().Framerate);
  float* lightDir = (float*)&m_sceneParams.lightDir;
  ImGui::InputFloat3("LightDir", lightDir);

  ImGui::Text("Status: %s", m_loadStatusMessage.c_str());
  ImGui::Text("Models: %d", m_modelCountLoadCompleted.load());
  ImGui::Text("LoadingTime: %.2f s", elapsedMilliseconds/1000.0f);
  ImGui::Text("Max CPUPeak: %.1f %%", m_maxCpuUtilizationInLoading.load());
  // 再ロード用ボタンや個数の設定が有効な範囲は、ロードが完了・クーリング状態ではないを満たすとき.
  ImGui::BeginDisabled(!(isLoadedAll && !m_isCoolingPeriod));
  if (ImGui::Button("Reload Models"))
  {
    m_requestReload = true;
  }
  ImGui::SliderInt("Model Count", (int*)&m_currentModelCount, 0, 200);
  ImGui::Checkbox("Pre-AllocationMode", &m_isPreAllocationMode);
  ImGui::EndDisabled();

  ImGui::Begin("Property");
  ImGui::Text("%s", m_strBandwidth.c_str());
  ImGui::Text("%s", m_strCpuMemData.c_str());
  ImGui::Text("%s", m_strBufferData.c_str());
  ImGui::Text("%s", m_strTextureData.c_str());
  ImGui::End();

  if(ImPlot::BeginPlot("GPU Usage (%)", ImVec2(-1, 100), ImPlotFlags_NoInputs) )
  {
    static const char* labels[] = { "0", "100" };
    static double ticks[] = { 0, 100 };
    ;
    auto col = ImPlot::GetColormapColor(1);
    ImPlot::SetNextLineStyle(col);
    ImPlot::SetNextFillStyle(col, 0.25);

    ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels);
    ImPlot::SetupAxisTicks(ImAxis_Y1, ticks, 2, labels);
    ImPlot::SetupAxesLimits(0, GraphData::kMaxGraphSpan, -1, 100.0);
    ImPlot::PlotLine("", m_graphData.gpuUsages.data(), int(m_graphData.gpuUsages.size()), 1, 0, ImPlotLineFlags_Shaded, 0);
    ImPlot::EndPlot();
  }
  if(ImPlot::BeginPlot("GPU (Copy) Usage", ImVec2(-1, 100), ImPlotFlags_NoInputs) )
  {
    static const char* labels[] = { "0", "100" };
    static double ticks[] = { 0, 100 };
    auto col = ImPlot::GetColormapColor(2);
    ImPlot::SetNextLineStyle(col);
    ImPlot::SetNextFillStyle(col, 0.25);

    ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels);
    ImPlot::SetupAxisTicks(ImAxis_Y1, ticks, 2, labels);
    ImPlot::SetupAxesLimits(0, GraphData::kMaxGraphSpan, -1, 100.0);
    ImPlot::PlotLine("", m_graphData.gpuCopyUsages.data(), int(m_graphData.gpuCopyUsages.size()), 1, 0, ImPlotLineFlags_Shaded, 0);
    ImPlot::EndPlot();
  }
  if(ImPlot::BeginPlot("GPU Memory (MB)", ImVec2(-1, 150), ImPlotFlags_NoInputs) )
  {
    ImPlot::SetupAxesLimits(0, GraphData::kMaxGraphSpan, 0, 10000.0);

    auto col = ImPlot::GetColormapColor(3);
    ImPlot::SetNextLineStyle(col);
    ImPlot::SetNextFillStyle(col, 0.25);
    ImPlot::PlotLine("Dedicated", m_graphData.dedicatedMemory.data(), int(m_graphData.dedicatedMemory.size()), 1, 0, ImPlotLineFlags_Shaded, 0);

    col = ImPlot::GetColormapColor(4);
    ImPlot::SetNextLineStyle(col);
    ImPlot::SetNextFillStyle(col, 0.25);
    ImPlot::PlotLine("Shared", m_graphData.sharedMemory.data(), int(m_graphData.sharedMemory.size()), 1, 0, ImPlotLineFlags_Shaded, 0);
    ImPlot::EndPlot();
  }
  if (ImPlot::BeginPlot("CPU Usage (%)", ImVec2(-1, 100), ImPlotFlags_NoInputs))
  {
    auto col = ImPlot::GetColormapColor(0);
    ImPlot::SetNextLineStyle(col);
    ImPlot::SetNextFillStyle(col, 0.25);

    //ImPlot::SetupAxes(nullptr, nullptr, 0, ImPlotAxisFlags_AutoFit);
    ImPlot::SetupAxes(nullptr, nullptr, 0, 0);
    ImPlot::SetupAxesLimits(0, GraphData::kMaxGraphSpan, -1, 100.0);
    ImPlot::PlotLine("", m_graphData.cpuUsages.data(), int(m_graphData.cpuUsages.size()), 1, 0, ImPlotLineFlags_Shaded, 0);
    ImPlot::EndPlot();
  }

  ImGui::End();

  auto& gfxDevice = GetGfxDevice();
  gfxDevice->NewFrame();

  // 描画のコマンドを作成.
  auto commandList = MakeCommandList();

  // 作成したコマンドを実行.
  gfxDevice->Submit(commandList.Get());
  // 描画した内容を画面へ反映.
  gfxDevice->Present(1);
}

void MyApplication::Shutdown()
{
  auto& gfxDevice = GetGfxDevice();
  gfxDevice->WaitForGPU();

  // リソースを解放.
  UnloadModelData();
  m_drawOpaquePipeline.Reset();
  m_rootSignature.Reset();

  // ImGui破棄処理.
  DestroyImGui();

  // グラフィックスデバイス関連解放.
  gfxDevice->Shutdown();

  auto& loaderDStorage = GetDStorageLoader();
  loaderDStorage->Shutdown();

  tpc::Shutdown();
}

ComPtr<ID3D12GraphicsCommandList>  MyApplication::MakeCommandList()
{
  auto& gfxDevice = GetGfxDevice();
  auto frameIndex = gfxDevice->GetFrameIndex();
  auto commandList = gfxDevice->CreateCommandList();

  // ルートシグネチャおよびパイプラインステートオブジェクト(PSO)をセット.
  commandList->SetGraphicsRootSignature(m_rootSignature.Get());
  commandList->SetPipelineState(m_drawOpaquePipeline.Get());

  commandList->RSSetViewports(1, &m_viewport);
  commandList->RSSetScissorRects(1, &m_scissorRect);

  auto renderTarget = gfxDevice->GetSwapchainBufferResource();
  auto barrierToRT = D3D12_RESOURCE_BARRIER{
    .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
    .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
    .Transition = {
      .pResource = renderTarget.Get(),
      .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
      .StateBefore = D3D12_RESOURCE_STATE_PRESENT,
      .StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET,
    }
  };

  commandList->ResourceBarrier(1, &barrierToRT);

  auto rtvHandle = gfxDevice->GetSwapchainBufferDescriptor();
  auto dsvHandle = m_depthBuffer.dsvHandle;
  commandList->OMSetRenderTargets(1, &rtvHandle.hCpu, FALSE, &(dsvHandle.hCpu));
  
  const float clearColor[] = { 0.75f, 0.9f, 1.0f, 1.0f };
  commandList->ClearRenderTargetView(rtvHandle.hCpu, clearColor, 0, nullptr);
  commandList->ClearDepthStencilView(dsvHandle.hCpu, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

  ID3D12DescriptorHeap* heaps[] = {
    gfxDevice->GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV).Get(),
    gfxDevice->GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER).Get(),
  };
  commandList->SetDescriptorHeaps(_countof(heaps), heaps);

  XMFLOAT3 eyePos(-1.0, 0.5, 10.0f), target(0, 0.0, 0), upDir(0, 1, 0);
  XMMATRIX mtxView = XMMatrixLookAtRH(
    XMLoadFloat3(&eyePos), XMLoadFloat3(&target), XMLoadFloat3(&upDir)
  );
  XMMATRIX mtxProj = XMMatrixPerspectiveFovRH(XM_PIDIV4, m_viewport.Width/m_viewport.Height, 0.1f, 200.0f);

  XMStoreFloat4x4(&m_sceneParams.mtxView, XMMatrixTranspose(mtxView));
  XMStoreFloat4x4(&m_sceneParams.mtxProj, XMMatrixTranspose(mtxProj));

  m_sceneParams.eyePosition = eyePos;
  m_sceneParams.time = m_frameDeltaAccum;
  m_frameDeltaAccum += ImGui::GetIO().DeltaTime;

  auto cb = m_constantBuffer[frameIndex].buffer;
  void* p;
  cb->Map(0, nullptr, &p);
  memcpy(p, &m_sceneParams, sizeof(m_sceneParams));
  cb->Unmap(0, nullptr);

  auto cbDescriptor = m_constantBuffer[frameIndex].descriptorCbv;
  commandList->SetGraphicsRootConstantBufferView(0, cb->GetGPUVirtualAddress());

  static int count = 0;
  XMMATRIX mtxWorldRoot = XMMatrixIdentity();
  mtxWorldRoot = XMMatrixRotationY(XMConvertToRadians(count*0.1f)) * XMMatrixRotationX(XMConvertToRadians(count * 0.12f));
  count++;

  if (!m_requestReload)
  {
    UpdateModelMatrices();
    DrawModels(commandList);
  }
  // ImGui による描画.
  ImGui::Render();
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList.Get());

  D3D12_RESOURCE_BARRIER barrierToPresent{
    .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
    .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
    .Transition = {
      .pResource = renderTarget.Get(),
      .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
      .StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET,
      .StateAfter = D3D12_RESOURCE_STATE_PRESENT,
    }
  };
  commandList->ResourceBarrier(1, &barrierToPresent);
  commandList->Close();
  return commandList;
}

std::vector<MyApplication::TextureInfo>::const_iterator MyApplication::FindModelTexture(const std::string& filePath, const ModelData& model)
{
  return std::find_if(model.textureList.begin(), model.textureList.end(), [&](const auto& v) { return v.filePath == filePath; });
}

void MyApplication::LoadModelDataByDirectStorage()
{
  std::default_random_engine rng;
  std::uniform_real_distribution<float> d(-1.0f, 1.0f);
  
  m_modelList.reserve(m_currentModelCount);
  m_loadStatusMessage = "Loading";
  m_maxCpuUtilizationInLoading = 0.0f;

  // ファイルリストを入れ替え.
  std::shuffle(m_fileList.begin(), m_fileList.end(), rng);

  for (uint32_t i = 0; i < m_currentModelCount; ++i)
  {
    auto model = std::make_shared<model::SimpleModel>();
    m_modelList.push_back(model);
    model->m_tumbleAxis = XMVectorSet(d(rng), d(rng), d(rng), 0.0f);
    model->m_tumbleAngle = d(rng);
    model->SetLoadingCompleteCallback([&](auto m) { CheckLoadingComplete(); });
  }

  if (m_isPreAllocationMode)
  {
    // 先にヘッダをロードしてヒープの確保を行っておく.
    for (uint32_t i = 0; i < m_modelList.size(); ++i)
    {
      m_modelList[i]->RequestLoadHeaderOnly(m_fileList[i]);
    }
  }

  tpc::ResetPeakCPU();

  // ロードの開始.
  m_startLoadingTime = std::chrono::high_resolution_clock::now();
  uint32_t fileIndex = 0;
  for (uint32_t i = 0; i < m_modelList.size(); ++i)
  {
    m_modelList[i]->RequestLoad(m_fileList[i]);
  }
}

void MyApplication::UnloadModelData()
{
  for (auto& model : m_modelList)
  {
    model.reset();
  }
  m_modelList.clear();
  m_modelCountLoadCompleted = 0;
}

void MyApplication::UpdateModelMatrices()
{
  static int count = 0;
  XMMATRIX mtxWorldRoot = XMMatrixIdentity();
  mtxWorldRoot = XMMatrixRotationY(XMConvertToRadians(count * 0.1f)) * XMMatrixRotationX(XMConvertToRadians(count * 0.12f));
  count++;

  int index = 0;
  for (auto& model : m_modelList)
  {
    // 行列を更新し反映.
    int pz = index / 25;
    int px = (index - 25 * pz) % 5;
    int py = (index - 25 * pz) / 5;
    auto mtxWorld = CalculateWorldMatrix(px, py, pz, model, 5.0, 1.5f);
    index++;

    if (!model->IsRenderingPrepared())
    {
      continue;
    }
    model->UpdateMatrices(mtxWorld * mtxWorldRoot);
    model->m_tumbleAngle += 0.01f;

    // 描画用リストに追加.
    m_drawList.push_back(model);
  }
}

void MyApplication::DrawModels(ComPtr<ID3D12GraphicsCommandList> commandList)
{
  for (auto& model : m_drawList)
  {
    model->SubmitMatrices(commandList);
  }

  model::DrawMode ModeList[] = {
    model::DrawMode::DrawModeOpaque, model::DrawMode::DrawModeMask,
    model::DrawMode::DrawModeBlend,
  };
  for (auto mode : ModeList)
  {
    switch (mode)
    {
    default:
    case model::DrawModeOpaque:
    case model::DrawModeMask:
      commandList->SetPipelineState(m_drawOpaquePipeline.Get());
      break;
    case model::DrawModeBlend:
      commandList->SetPipelineState(m_drawBlendPipeline.Get());
      break;
    }

    for (auto model : m_drawList)
    {
      model->Draw(commandList, mode);
    }
  }
  m_drawList.clear();
}

void MyApplication::CheckLoadingComplete()
{
  using namespace std::chrono;

  m_modelCountLoadCompleted++;
  if (m_modelCountLoadCompleted == m_currentModelCount)
  {
    m_endLoadingTime = std::chrono::high_resolution_clock::now();
    m_maxCpuUtilizationInLoading = (float)tpc::GetPeakCPUUtilization();

    size_t total = 0;
    size_t cpuDataTotal = 0, bufferDataTotal = 0, textureDataTotal = 0;
    for (const auto& model : m_modelList)
    {
      const auto& prop = model->m_dataSizeProperty;
      total += (prop.cpuByteCount + prop.buffersByteCount + prop.texturesByteCount);
      cpuDataTotal += prop.cpuByteCount;
      bufferDataTotal += prop.buffersByteCount;
      textureDataTotal += prop.texturesByteCount;
    }

    auto secondFloat = duration<float, seconds::period>(m_endLoadingTime - m_startLoadingTime);
    auto bandwidth = (total / secondFloat.count()) / 1000.0f / 1000.0f / 1000.0f;
    m_strBandwidth = std::format("Bandwidth: {:7.2f} GB/s", bandwidth);
    m_strCpuMemData = std::format("CPU Mem Data: {:7.2f} MiB", cpuDataTotal / 1024.0f / 1024.0f);
    m_strBufferData = std::format(" Buffer Data: {:7.2f} MiB", bufferDataTotal / 1024.0f / 1024.0f);
    m_strTextureData = std::format("Texture Data: {:7.2f} MiB", textureDataTotal / 1024.0f / 1024.0f);
  }
}
