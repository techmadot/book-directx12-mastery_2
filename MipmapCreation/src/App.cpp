#include "App.h"

#include "GfxDevice.h"
#include "FileLoader.h"
#include "Win32Application.h"

#include "imgui.h"
#include "imgui/backends/imgui_impl_dx12.h"
#include "imgui/backends/imgui_impl_win32.h"

#include "TextureUtility.h"
#include <DirectXTex.h>
#include <fstream>

#include <windows.h>
#include <d3dx12.h>
#include <d3dcompiler.h>

#include "shader/MipmapWriteVS.inc"
#include "shader/MipmapWritePS.inc"

#include "shader/GenerateMipsLinearCS.inc"
#include "shader/GenerateMipsLinearOddCS.inc"
#include "shader/GenerateMipsLinearOddXCS.inc"
#include "shader/GenerateMipsLinearOddYCS.inc"

#include <pix.h>

using namespace Microsoft::WRL;
using namespace DirectX;

extern "C"
{
  __declspec(dllexport) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION;
  __declspec(dllexport) extern const char8_t* D3D12SDKPath = u8".\\D3D12\\";
}

static std::unique_ptr<MyApplication> gMyApplication;
std::unique_ptr<MyApplication>& GetApplication()
{
  if (gMyApplication == nullptr)
  {
    gMyApplication = std::make_unique<MyApplication>();
  }
  return gMyApplication;
}

MyApplication::MyApplication()
{
  m_title = L"MipmapCreation";
}

void MyApplication::Initialize()
{
  auto& gfxDevice = GetGfxDevice();
  GfxDevice::DeviceInitParams initParams;
  initParams.formatDesired = DXGI_FORMAT_R8G8B8A8_UNORM;
  gfxDevice->Initialize(initParams);

#if _DEBUG
  //gfxDevice->GetD3D12Device()->SetStablePowerState(TRUE);
#endif

  PrepareDepthBuffer();
  PrepareRenderTarget();

  PrepareMipmapGenShaderRT();
  PrepareMipmapGenShaderCS();
  PrepareMipmapGenShaderSPD();

  PrepareImGui();

  PrepareSceneConstantBuffer();

  PrepareSimplePipeline();
  PreparePlaneData();

  PrepareModelDrawPipeline();

  PrepareModelData();
  PrepareReadbackObjects();
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
  m_depthBuffer.dsvHandle = gfxDevice->CreateDepthStencilView(m_depthBuffer.image, &dsvDesc);

}

void MyApplication::PrepareSceneConstantBuffer()
{
  auto& gfxDevice = GetGfxDevice();
  UINT constantBufferSize = sizeof(SceneParameters) * GfxDevice::BackBufferCount;
  assert((constantBufferSize % 256) == 0);
  D3D12_RESOURCE_DESC cbResDesc{
    .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
    .Alignment = 0,
    .Width = constantBufferSize,
    .Height = 1, .DepthOrArraySize = 1,  .MipLevels = 1,
    .Format = DXGI_FORMAT_UNKNOWN,
    .SampleDesc = {.Count = 1, .Quality = 0},
    .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    .Flags = D3D12_RESOURCE_FLAG_NONE
  };
  m_defaultSceneCB = gfxDevice->CreateBuffer(cbResDesc, D3D12_HEAP_TYPE_UPLOAD);
  m_renderSceneCB = gfxDevice->CreateBuffer(cbResDesc, D3D12_HEAP_TYPE_UPLOAD);
}

void MyApplication::PrepareSimplePipeline()
{
  auto& gfxDevice = GetGfxDevice();
  auto& loader = GetFileLoader();

  // 描画のためのパイプラインステートオブジェクトを作成.
  // ルートシグネチャの作成.
  D3D12_DESCRIPTOR_RANGE rangeSrvRanges[] = {
    {  // t0 テクスチャ用.
      .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
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
      .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
      .DescriptorTable = {
        .NumDescriptorRanges = _countof(rangeSrvRanges),
        .pDescriptorRanges = rangeSrvRanges,
      },
      .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
    },
  };
  D3D12_STATIC_SAMPLER_DESC samplerDesc{
    .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
    .AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
    .AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
    .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
    .MaxAnisotropy = 0,
    .MinLOD = 0.0f, .MaxLOD = FLT_MAX,
  };

  D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{
    .NumParameters = _countof(rootParams),
    .pParameters = rootParams,
    .NumStaticSamplers = 1,
    .pStaticSamplers = &samplerDesc,
    .Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
  };

  ComPtr<ID3DBlob> signature;
  ComPtr<ID3DBlob> error;
  D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
  m_simpleRootSignature = gfxDevice->CreateRootSignature(signature);

  // 頂点データのインプットレイアウト情報を作成.
  D3D12_INPUT_ELEMENT_DESC inputElementDesc[] = {
    {
      .SemanticName = "POSITION", .SemanticIndex = 0,
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
  };
  D3D12_INPUT_LAYOUT_DESC inputLayout{
    .pInputElementDescs = inputElementDesc,
    .NumElements = _countof(inputElementDesc),
  };
  // シェーダーコードの読み込み.
  std::vector<char> vsdata, psdata;
  loader->Load(L"res/shader/SimpleVS.cso", vsdata);
  loader->Load(L"res/shader/SimplePS.cso", psdata);
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
    .CullMode = D3D12_CULL_MODE_NONE,
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
    .pRootSignature = m_simpleRootSignature.Get(),
    .VS = vs, .PS = ps,
    .BlendState = blendState,
    .SampleMask = UINT_MAX,
    .RasterizerState = rasterizerState,
    .InputLayout = inputLayout,
    .PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
    .SampleDesc = {.Count = 1, .Quality = 0 }
  };
  psoDesc.DepthStencilState = depthStencilState;
  psoDesc.NumRenderTargets = 1;
  psoDesc.RTVFormats[0] = gfxDevice->GetSwapchainFormat();
  psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  m_drawSimplePipeline = gfxDevice->CreateGraphicsPipelineState(psoDesc);

}

void MyApplication::PreparePlaneData()
{
  auto& gfxDevice = GetGfxDevice();
  struct VertexPT {
    DirectX::XMFLOAT3 Position; DirectX::XMFLOAT2 Texcoord;
  } vertices[] = {
    { XMFLOAT3(-1.0f, 1.0f, 0.0f), XMFLOAT2(0.0f,0.0f) },
    { XMFLOAT3(-1.0f,-1.0f, 0.0f), XMFLOAT2(0.0f,1.0f) },
    { XMFLOAT3(1.0f, 1.0f, 0.0f), XMFLOAT2(1.0f,0.0f) },
    { XMFLOAT3(1.0f,-1.0f, 0.0f), XMFLOAT2(1.0f,1.0f) },
  };
  D3D12_RESOURCE_DESC resDesc{
    .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
    .Alignment = 0,
    .Width = sizeof(vertices),
    .Height = 1, .DepthOrArraySize = 1,  .MipLevels = 1,
    .Format = DXGI_FORMAT_UNKNOWN,
    .SampleDesc = {.Count = 1, .Quality = 0},
    .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    .Flags = D3D12_RESOURCE_FLAG_NONE
  };
  m_drawPlaneInfo.vertexBuffer = gfxDevice->CreateBuffer(resDesc, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, vertices);

  m_drawPlaneInfo.vbv = D3D12_VERTEX_BUFFER_VIEW{
    .BufferLocation = m_drawPlaneInfo.vertexBuffer->GetGPUVirtualAddress(),
    .SizeInBytes = UINT(resDesc.Width),
    .StrideInBytes = sizeof(VertexPT)
  };

  UINT constantBufferSize = sizeof(DrawParameters) * GfxDevice::BackBufferCount;
  assert((constantBufferSize % 256) == 0);
  D3D12_RESOURCE_DESC cbResDesc{
    .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
    .Alignment = 0,
    .Width = constantBufferSize,
    .Height = 1, .DepthOrArraySize = 1,  .MipLevels = 1,
    .Format = DXGI_FORMAT_UNKNOWN,
    .SampleDesc = {.Count = 1, .Quality = 0},
    .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    .Flags = D3D12_RESOURCE_FLAG_NONE
  };
  m_drawPlaneInfo.modelCB = gfxDevice->CreateBuffer(cbResDesc, D3D12_HEAP_TYPE_UPLOAD);

  D3D12_SAMPLER_DESC samplerDesc{
    .AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
    .AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
    .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
    .MaxAnisotropy = 0,
    .MinLOD = 0.0f, .MaxLOD = FLT_MAX,
  };
  m_defaultSampler = gfxDevice->CreateSampler(samplerDesc);
}

void MyApplication::PrepareModelDrawPipeline()
{
  auto& gfxDevice = GetGfxDevice();
  auto& loader = GetFileLoader();

  // 描画のためのパイプラインステートオブジェクトを作成.
  // ルートシグネチャの作成.
  D3D12_DESCRIPTOR_RANGE rangeSrvRanges[] = {
    {  // t0 モデルのディフューズテクスチャ用.
      .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
      .NumDescriptors = 1,
      .BaseShaderRegister = 0,
      .RegisterSpace = 0,
      .OffsetInDescriptorsFromTableStart = 0,
    }
  };
  D3D12_DESCRIPTOR_RANGE rangeSamplerRanges[] = {
    {  // s0 モデルのディフューズテクスチャ用のサンプラー.
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
  m_modelRootSignature = gfxDevice->CreateRootSignature(signature);

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
      .InputSlot = 1, .AlignedByteOffset = 0,
      .InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
      .InstanceDataStepRate = 0,
    },
    {
      .SemanticName = "TEXCOORD", .SemanticIndex = 0,
      .Format = DXGI_FORMAT_R32G32_FLOAT,
      .InputSlot = 2, .AlignedByteOffset = 0,
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
  loader->Load(L"res/shader/RenderSceneVS.cso", vsdata);
  loader->Load(L"res/shader/RenderScenePS.cso", psdata);
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
    .pRootSignature = m_modelRootSignature.Get(),
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
  ModelLoader loader;
  std::vector<ModelMesh> modelMeshes;
  std::vector<ModelMaterial> modelMaterials;
  std::vector<ModelEmbeddedTextureData> modelEmbeddedTextures;
  const char* modelFile = "res/model/sponza/Sponza.gltf";

  if (!loader.Load(modelFile, modelMeshes, modelMaterials, modelEmbeddedTextures))
  {
    MessageBoxW(NULL, L"モデルのロードに失敗", L"Error", MB_OK);
    return;
  }

  auto& gfxDevice = GetGfxDevice();
  for (const auto& embeddedInfo : modelEmbeddedTextures)
  {
    auto& texture = m_model.embeddedTextures.emplace_back();
    auto success = CreateTextureFromMemory(texture.texResource, embeddedInfo.data.data(), embeddedInfo.data.size(), true);
    assert(success);
  }
  for (const auto& material : modelMaterials)
  {
    auto& dstMaterial = m_model.materials.emplace_back();

    dstMaterial.alphaMode = material.alphaMode;
    dstMaterial.diffuse.x = material.diffuse.x;
    dstMaterial.diffuse.y = material.diffuse.y;
    dstMaterial.diffuse.z = material.diffuse.z;
    dstMaterial.diffuse.w = material.alpha;
    dstMaterial.specular.x = material.specular.x;
    dstMaterial.specular.y = material.specular.y;
    dstMaterial.specular.z = material.specular.z;
    dstMaterial.specular.w = 50.0f; // サンプルでは固定値で設定.

    D3D12_SAMPLER_DESC samplerDesc{
      .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
      .AddressU = material.texDiffuse.addressModeU,
      .AddressV = material.texDiffuse.addressModeV,
      .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
      .ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
      .MinLOD = 0, .MaxLOD = D3D12_FLOAT32_MAX,
    };

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{
      .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
      .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
      .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
      .Texture2D = {
        .MostDetailedMip = 0,
        .MipLevels = 1,
        .PlaneSlice = 0, .ResourceMinLODClamp = 0.
      }
    };
    GfxDevice::DescriptorHandle diffuseSrvDescriptor;

    bool success;
    if (material.texDiffuse.embeddedIndex == -1)
    {
      // ファイルから読み込み.
      auto& info = m_model.textureList.emplace_back();
      info.filePath = material.texDiffuse.filePath;

      success = CreateTextureFromFile(info.texResource, info.filePath, true);
      const auto texDesc = info.texResource->GetDesc();
      srvDesc.Format = texDesc.Format;
      srvDesc.Texture2D.MipLevels = texDesc.MipLevels;
      assert(success);
      diffuseSrvDescriptor = gfxDevice->CreateShaderResourceView(info.texResource, &srvDesc);
    }
    else
    {
      // 埋め込みテクスチャから読み込み.
      auto& embTexture = m_model.embeddedTextures[material.texDiffuse.embeddedIndex];
      const auto texDesc = embTexture.texResource->GetDesc();
      srvDesc.Format = texDesc.Format;
      srvDesc.Texture2D.MipLevels = texDesc.MipLevels;
      diffuseSrvDescriptor = gfxDevice->CreateShaderResourceView(embTexture.texResource, &srvDesc);
    }

    dstMaterial.srvDiffuse = diffuseSrvDescriptor;
    dstMaterial.samplerDiffuse = gfxDevice->CreateSampler(samplerDesc);
  }

  for (const auto& mesh : modelMeshes)
  {
    auto& dstMesh = m_model.meshes.emplace_back();
    D3D12_RESOURCE_DESC resDesc{
      .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
      .Alignment = 0,
      .Width = 0,
      .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1,
      .Format = DXGI_FORMAT_UNKNOWN,
      .SampleDesc = {.Count = 1, .Quality = 0 },
      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
      .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    UINT stride = 0;
    auto resourceState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    UINT vertexCount = UINT(mesh.positions.size());
    UINT indexCount = UINT(mesh.indices.size());

    stride = sizeof(XMFLOAT3);
    resDesc.Width = stride * vertexCount;
    dstMesh.position = gfxDevice->CreateBuffer(resDesc,
      D3D12_HEAP_TYPE_DEFAULT, resourceState,
      mesh.positions.data());
    dstMesh.vbViews[0] = {
      .BufferLocation = dstMesh.position->GetGPUVirtualAddress(),
      .SizeInBytes = UINT(resDesc.Width),
      .StrideInBytes = stride,
    };

    stride = sizeof(XMFLOAT3);
    resDesc.Width = stride * vertexCount;
    dstMesh.normal = gfxDevice->CreateBuffer(resDesc,
      D3D12_HEAP_TYPE_DEFAULT, resourceState,
      mesh.normals.data());
    dstMesh.vbViews[1] = {
      .BufferLocation = dstMesh.normal->GetGPUVirtualAddress(),
      .SizeInBytes = UINT(resDesc.Width),
      .StrideInBytes = stride,
    };

    stride = sizeof(XMFLOAT2);
    resDesc.Width = stride * vertexCount;
    dstMesh.texcoord0 = gfxDevice->CreateBuffer(resDesc,
      D3D12_HEAP_TYPE_DEFAULT, resourceState,
      mesh.texcoords.data());
    dstMesh.vbViews[2] = {
      .BufferLocation = dstMesh.texcoord0->GetGPUVirtualAddress(),
      .SizeInBytes = UINT(resDesc.Width),
      .StrideInBytes = stride,
    };

    resDesc.Width = indexCount * sizeof(uint32_t);
    dstMesh.indices = gfxDevice->CreateBuffer(resDesc,
      D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_INDEX_BUFFER,
      mesh.indices.data());
    dstMesh.ibv = {
      .BufferLocation = dstMesh.indices->GetGPUVirtualAddress(),
      .SizeInBytes = UINT(resDesc.Width),
      .Format = DXGI_FORMAT_R32_UINT,
    };

    dstMesh.indexCount = indexCount;
    dstMesh.vertexCount = vertexCount;
    dstMesh.materialIndex = mesh.materialIndex;
  }

  // メッシュ単位の描画情報を組み立てる.
  for (uint32_t i = 0; i < m_model.meshes.size(); ++i)
  {
    auto& mesh = m_model.meshes[i];
    auto& material = m_model.materials[mesh.materialIndex];

    auto& info = m_model.drawInfos.emplace_back();
    info.materialIndex = mesh.materialIndex;
    info.meshIndex = i;

    for (int j = 0; j < GfxDevice::BackBufferCount; ++j)
    {
      auto bufferSize = sizeof(DrawParameters);
      bufferSize = (bufferSize + 255) & ~(255);
      D3D12_RESOURCE_DESC resDesc{
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment = 0,
        .Width = bufferSize,
        .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = {.Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
      };
      info.modelMeshConstantBuffer[j] = gfxDevice->CreateBuffer(
        resDesc, D3D12_HEAP_TYPE_UPLOAD);
    }
  }
}

void MyApplication::PrepareImGui()
{
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui_ImplWin32_Init(Win32Application::GetHwnd());

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
  ImGui::DestroyContext();
}

void MyApplication::OnUpdate()
{
  // ImGui更新処理.
  ImGui_ImplDX12_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();

  // ImGuiを使用したUIの描画指示.
  ImGui::Begin("Information");
  ImGui::Text("FPS: %.2f", ImGui::GetIO().Framerate);

  ImGui::SetNextItemOpen(true, ImGuiCond_Once);
  if (ImGui::CollapsingHeader("Skip GenMipmaps"))
  {
    ImGui::Checkbox("Skip GenMipmaps [RT ]", &m_skipGenMipmapRT);
    ImGui::Checkbox("Skip GenMipmaps [CS ]", &m_skipGenMipmapCS);
    ImGui::Checkbox("Skip GenMipmaps [SPD]", &m_skipGenMipmapSPD);
  }

  ImGui::SetNextItemOpen(true, ImGuiCond_Once);
  if (ImGui::CollapsingHeader("Render"))
  {
    float* lightDir = (float*)&m_lightDir;
    ImGui::InputFloat3("LightDir", lightDir);
    ImGui::Checkbox("Overwrite", &m_overwrite);

    float* specularColor = (float*)&m_globalSpecular;
    ImGui::InputFloat3("Specular", specularColor);
    ImGui::InputFloat("Power", (float*)&m_globalSpecular.w);
    float* ambientColor = (float*)&m_globalAmbient;
    ImGui::InputFloat3("Ambient", ambientColor);

    ImGui::Combo("MipSource", (int*)&m_drawMipSource, "RenderTarget\0Compute\0SPD\0\0");
  }

  ImGui::SetNextItemOpen(true, ImGuiCond_Always);
  if (ImGui::CollapsingHeader("Control"))
  {
    ImGui::BeginDisabled(m_bSaveRequestButton);
    if (ImGui::Button("Save MipmapTexture"))
    {
      m_bSaveRequestButton = true;
    }
    ImGui::EndDisabled();

    if (ImGui::Button("Save MipmapTexture (Capture)"))
    {
      m_bSaveRequestButton2 = true;
    }
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

  if (m_bSaveRequestButton)
  {
    ProcessSaveTexture();
  }


  if (m_bSaveRequestButton2)
  {
    auto image = std::make_shared<DirectX::ScratchImage>();
    HRESULT hr = DirectX::CaptureTexture(
      gfxDevice->GetD3D12CommandQueue().Get(),
      m_mipmapRT.resColorBuffer.Get(),
      false,
      *image,
      D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
      D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE
      );
    if (SUCCEEDED(hr))
    {
      std::thread thr([this, image]() {
        HRESULT hr = DirectX::SaveToDDSFile(
          image->GetImages(), image->GetImageCount(),
          image->GetMetadata(),
          DirectX::DDS_FLAGS_NONE, L"saved.dds");
          if (FAILED(hr))
          {
            OutputDebugStringA("Failed CaptureTexture\n");
          }
          m_bSaveRequestButton2 = false;
        });
      thr.detach();
    }
  }

  m_frameDeltaAccum += ImGui::GetIO().DeltaTime;
}

void MyApplication::Shutdown()
{
  auto& gfxDevice = GetGfxDevice();
  gfxDevice->WaitForGPU();

  // リソースを解放.
  m_drawOpaquePipeline.Reset();
  m_modelRootSignature.Reset();

  // SPDのリソースを解放.
  ffxSpdContextDestroy(&m_mipmapSPD.contextSpd);
  if (m_mipmapSPD.scratchBuffer)
  {
    free(m_mipmapSPD.scratchBuffer); m_mipmapSPD.scratchBuffer = nullptr;
  }

  // ImGui破棄処理.
  DestroyImGui();

  // グラフィックスデバイス関連解放.
  gfxDevice->Shutdown();
}

// テクスチャレンダリング用のレンダーターゲット(Color+Depth)の作成.
void MyApplication::PrepareRenderTarget()
{
  auto& gfxDevice = GetGfxDevice();
  D3D12_HEAP_PROPERTIES defaultHeap{
    .Type = D3D12_HEAP_TYPE_DEFAULT,
    .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
    .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
    .CreationNodeMask = 1, .VisibleNodeMask = 1,
  };
  // レンダーターゲット描画中で使用するデプスバッファの作成.
  // 各手法でこのデプスバッファは使い回す.
  D3D12_RESOURCE_DESC resDepthDesc{
    .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
    .Alignment = 0,
    .Width = RenderTexWidth, .Height = RenderTexHeight,
    .DepthOrArraySize = 1,
    .MipLevels = 1,
    .Format = DXGI_FORMAT_D32_FLOAT,
    .SampleDesc = {.Count = 1, .Quality = 0},
    .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
    .Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
  };
  D3D12_CLEAR_VALUE depthClear{
    .Format = resDepthDesc.Format,
    .DepthStencil {.Depth = 1.0f, .Stencil = 0 },
  };
  m_renderTargetCommonDepth.resDepthBuffer = gfxDevice->CreateImage2D(resDepthDesc, defaultHeap, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear);
  m_renderTargetCommonDepth.dsvDepth = gfxDevice->CreateDepthStencilView(m_renderTargetCommonDepth.resDepthBuffer, nullptr);


  // 描画先となるカラーテクスチャの作成.
  UINT mipmapCount = (std::max)(RenderTexHeight, RenderTexHeight);
  mipmapCount = UINT(std::log2(mipmapCount)) + 1;
  auto resRenderTex = D3D12_RESOURCE_DESC{
    .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
    .Width = RenderTexWidth, .Height = RenderTexHeight,
    .DepthOrArraySize = 1,
    .MipLevels = UINT16(mipmapCount),
    .Format= DXGI_FORMAT_R8G8B8A8_UNORM,
    .SampleDesc = {.Count = 1, .Quality = 0},
    .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
    .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
  };
  D3D12_CLEAR_VALUE colorClear{
    .Format = resRenderTex.Format,
    .Color = { 0.0f, 0.0f, 0.0f, 0.0f }
  };
  m_mipmapRT.resColorBuffer = gfxDevice->CreateImage2D(
    resRenderTex, defaultHeap, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, &colorClear);

  auto resRenderTex2 = D3D12_RESOURCE_DESC{
    .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
    .Width = RenderTexWidth, .Height = RenderTexHeight,
    .DepthOrArraySize = 1,
    .MipLevels = UINT16(mipmapCount),
    .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
    .SampleDesc = {.Count = 1, .Quality = 0},
    .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
    .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  };
  m_mipmapCS.resColorBuffer = gfxDevice->CreateImage2D(resRenderTex2, defaultHeap, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, &colorClear);
  m_mipmapSPD.resColorBuffer = gfxDevice->CreateImage2D(resRenderTex2, defaultHeap, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, &colorClear);
  m_mipmapCS.resColorBuffer->SetName(L"MipmapTexture(CS)");
  m_mipmapSPD.resColorBuffer->SetName(L"MipmapTexture(SPD)");

  // 各ディスクリプタの作成.
  m_mipmapRT.rtvColorMip.resize(resRenderTex.MipLevels);
  m_mipmapRT.srvColorMip.resize(resRenderTex.MipLevels);
  for (UINT16 mipLevel = 0; mipLevel < resRenderTex.MipLevels; ++mipLevel)
  {
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{
      .Format = resRenderTex.Format,
      .ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D,
      .Texture2D = {
        .MipSlice = mipLevel
      },
    };
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{
      .Format = resRenderTex.Format,
      .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
      .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
      .Texture2D = {
        .MostDetailedMip = mipLevel,
        .MipLevels = 1,
      },
    };
    m_mipmapRT.rtvColorMip[mipLevel] = gfxDevice->CreateRenderTargetView(m_mipmapRT.resColorBuffer, &rtvDesc);
    m_mipmapRT.srvColorMip[mipLevel] = gfxDevice->CreateShaderResourceView(m_mipmapRT.resColorBuffer, &srvDesc);
  }
  m_mipmapRT.srvColor = gfxDevice->CreateShaderResourceView(m_mipmapRT.resColorBuffer, nullptr);

  // CS: 各ディスクリプタの作成.
  m_mipmapCS.srvColorMip.resize(resRenderTex2.MipLevels);
  m_mipmapCS.uavColorMip.resize(resRenderTex2.MipLevels);
  for (UINT16 mipLevel = 0; mipLevel < resRenderTex2.MipLevels; ++mipLevel)
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{
      .Format = resRenderTex2.Format,
      .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D,
      .Texture2D = {
        .MipSlice = mipLevel,
      }
    };
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{
      .Format = resRenderTex.Format,
      .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
      .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
      .Texture2D = {
        .MostDetailedMip = mipLevel, .MipLevels = 1,
      },
    };
    m_mipmapCS.uavColorMip[mipLevel] = gfxDevice->CreateUnorderedAccessView(m_mipmapCS.resColorBuffer, &uavDesc);
    m_mipmapCS.srvColorMip[mipLevel] = gfxDevice->CreateShaderResourceView(m_mipmapCS.resColorBuffer, &srvDesc);
  }
  m_mipmapCS.rtvColor = gfxDevice->CreateRenderTargetView(m_mipmapCS.resColorBuffer, nullptr);
  m_mipmapCS.srvColor = gfxDevice->CreateShaderResourceView(m_mipmapCS.resColorBuffer, nullptr);

  // CS-SPD: ディスクリプタの作成.
  m_mipmapSPD.rtvColor = gfxDevice->CreateRenderTargetView(m_mipmapSPD.resColorBuffer, nullptr);
  m_mipmapSPD.srvColor = gfxDevice->CreateShaderResourceView(m_mipmapSPD.resColorBuffer, nullptr);
}

void MyApplication::PrepareMipmapGenShaderRT()
{
  auto& gfxDevice = GetGfxDevice();

  D3D12_RASTERIZER_DESC rasterizerState{
    .FillMode = D3D12_FILL_MODE_SOLID,
    .CullMode = D3D12_CULL_MODE_NONE, .FrontCounterClockwise = TRUE,
    .DepthBias = D3D12_DEFAULT_DEPTH_BIAS,
    .DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
    .SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS,
    .DepthClipEnable = TRUE, .MultisampleEnable = FALSE, .AntialiasedLineEnable = FALSE,
    .ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF
  };
  D3D12_BLEND_DESC blendState{
    .AlphaToCoverageEnable = FALSE,
    .IndependentBlendEnable = FALSE,
    .RenderTarget = {
      D3D12_RENDER_TARGET_BLEND_DESC{
        .BlendEnable = FALSE,
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

  D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {
    .VS = {
      .pShaderBytecode = g_MipmapWriteVS,
      .BytecodeLength = sizeof(g_MipmapWriteVS)
    },
    .PS = {
      .pShaderBytecode = g_MipmapWritePS,
      .BytecodeLength = sizeof(g_MipmapWritePS)
    },
    .BlendState = blendState,
    .SampleMask = UINT_MAX,
    .RasterizerState = rasterizerState,
    .PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
    .NumRenderTargets = 1,
    .RTVFormats = { DXGI_FORMAT_R8G8B8A8_UNORM },
    .SampleDesc = { .Count = 1, .Quality = 0 },
  };
  m_mipmapRT.writeMipmapPso = gfxDevice->CreateGraphicsPipelineState(psoDesc);

  // シェーダーコードでルートシグネチャを定義済みのため取り出して生成.
  ComPtr<ID3DBlob> blob = nullptr;
  D3DGetBlobPart(g_MipmapWriteVS, sizeof(g_MipmapWriteVS), D3D_BLOB_ROOT_SIGNATURE, 0, &blob);
  m_mipmapRT.rootSignature = gfxDevice->CreateRootSignature(blob);
}

void MyApplication::PrepareMipmapGenShaderCS()
{
  auto& gfxDevice = GetGfxDevice();
  D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
  psoDesc.CS.pShaderBytecode = g_GenerateMipsLinearCS;
  psoDesc.CS.BytecodeLength = sizeof(g_GenerateMipsLinearCS);
  m_mipmapCS.writeMipmapPso[0] = gfxDevice->CreateComputePipelineState(psoDesc);

  psoDesc.CS.pShaderBytecode = g_GenerateMipsLinearOddXCS;
  psoDesc.CS.BytecodeLength = sizeof(g_GenerateMipsLinearOddXCS);
  m_mipmapCS.writeMipmapPso[1] = gfxDevice->CreateComputePipelineState(psoDesc);

  psoDesc.CS.pShaderBytecode = g_GenerateMipsLinearOddYCS;
  psoDesc.CS.BytecodeLength = sizeof(g_GenerateMipsLinearOddYCS);
  m_mipmapCS.writeMipmapPso[2] = gfxDevice->CreateComputePipelineState(psoDesc);

  psoDesc.CS.pShaderBytecode = g_GenerateMipsLinearOddCS;
  psoDesc.CS.BytecodeLength = sizeof(g_GenerateMipsLinearOddCS);
  m_mipmapCS.writeMipmapPso[3] = gfxDevice->CreateComputePipelineState(psoDesc);

  // シェーダーコードでルートシグネチャを定義済みのため取り出して生成.
  ComPtr<ID3DBlob> blob = nullptr;
  D3DGetBlobPart(g_GenerateMipsLinearCS, sizeof(g_GenerateMipsLinearCS), D3D_BLOB_ROOT_SIGNATURE, 0, &blob);
  m_mipmapCS.rootSignature = gfxDevice->CreateRootSignature(blob);

  // 定数バッファも使用するため作成.
  // MaxMipmapExecLevel x バックバッファ数 のサイズで確保し、オフセット付きでアクセスとする.
  auto cbBufferSize = sizeof(MipmapGenConstants) * MaxMipmapExecLevel;
  cbBufferSize *= GfxDevice::BackBufferCount;
  auto cbResDesc = CD3DX12_RESOURCE_DESC::Buffer(cbBufferSize);
  m_mipmapCS.cbMinGenBuffer = gfxDevice->CreateBuffer(cbResDesc, D3D12_HEAP_TYPE_UPLOAD);
}

void MyApplication::PrepareMipmapGenShaderSPD()
{
  auto d3d12Device = GetGfxDevice()->GetD3D12Device();
  FfxSpdContextDescription ctxDesc{};
  ctxDesc.downsampleFilter = FFX_SPD_DOWNSAMPLE_FILTER_MEAN;
  ctxDesc.flags = FFX_SPD_SAMPLER_LOAD | FFX_SPD_WAVE_INTEROP_WAVE_OPS;

  const auto maxContext = FFX_SPD_CONTEXT_COUNT;
  auto scratchBufferSize = ffxGetScratchMemorySizeDX12(maxContext);
  void* scratchBuffer = malloc(scratchBufferSize);
  if (scratchBuffer)
  {
    memset(scratchBuffer, 0, scratchBufferSize);
  }
  auto errcode = ffxGetInterfaceDX12(
    &ctxDesc.backendInterface, ffxGetDeviceDX12(d3d12Device.Get()),
    scratchBuffer, scratchBufferSize, maxContext);
  assert(errcode == FFX_OK);

  errcode = ffxSpdContextCreate(&m_mipmapSPD.contextSpd, &ctxDesc);
  assert(errcode == FFX_OK);
  m_mipmapSPD.scratchBuffer = scratchBuffer;

#if _DEBUG 
  // FidelityFX 1.1.3 時点のものを使用すると、
  // EXECUTION ERROR #1387: GPU_BASED_VALIDATION_STRUCTURED_BUFFER_STRIDE_MISMATCH が発生する.
  // 何も手を入れず使う場合には、以下のエラー抑制としておく.
  ComPtr<ID3D12InfoQueue> infoQueue;
  d3d12Device.As(&infoQueue);
  if (infoQueue)
  {
    D3D12_MESSAGE_ID denyIds[] = {
      // WARNING #1380 を抑制
      D3D12_MESSAGE_ID_GPU_BASED_VALIDATION_STRUCTURED_BUFFER_STRIDE_MISMATCH
    };
    D3D12_INFO_QUEUE_FILTER filter{};
    filter.DenyList.NumIDs = _countof(denyIds);
    filter.DenyList.pIDList = denyIds;
    infoQueue->AddStorageFilterEntries(&filter);
  }
#endif
}

void MyApplication::PrepareReadbackObjects()
{
  auto& gfxDevice = GetGfxDevice();

  // リードバックヒープにバッファを作成.
  auto bufferSize = 128 * 1024 * 1024;
  auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);
  m_readbackInfo.resReadbackBuffer = gfxDevice->CreateBuffer(resDesc, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  m_readbackInfo.fence = gfxDevice->CreateFence();
}

ComPtr<ID3D12GraphicsCommandList>  MyApplication::MakeCommandList()
{
  auto& gfxDevice = GetGfxDevice();
  auto frameIndex = gfxDevice->GetFrameIndex();
  auto commandList = gfxDevice->CreateCommandList();

  ID3D12DescriptorHeap* heaps[] = {
    gfxDevice->GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV).Get(),
    gfxDevice->GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER).Get(),
  };
  commandList->SetDescriptorHeaps(_countof(heaps), heaps);

  // モデルの定数バッファ更新.
  UpdateModelMatrix();

  // ミップマップへ描画(ベースレベルのみ)
  DrawMipmapBase(commandList);


  // レンダーテクスチャをテクスチャとして使うためのバリアを設定.
  auto backbuffer = gfxDevice->GetSwapchainBufferResource();
  D3D12_RESOURCE_BARRIER beforeBarriers[] = {
    {
      .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
      .Transition = {
        .pResource = backbuffer.Get(),
        .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        .StateBefore = D3D12_RESOURCE_STATE_PRESENT,
        .StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET,
      }
    },
    // RTによるミップマップ生成用.
    CD3DX12_RESOURCE_BARRIER::Transition(
      m_mipmapRT.resColorBuffer.Get(),
      D3D12_RESOURCE_STATE_RENDER_TARGET,
      D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
      0), // base:0
    // CSによるミップマップ生成用.
    CD3DX12_RESOURCE_BARRIER::Transition(
      m_mipmapCS.resColorBuffer.Get(),
      D3D12_RESOURCE_STATE_RENDER_TARGET,
      D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
      0), // base:0
    // CS(SPD) によるミップマップ生成用.
    CD3DX12_RESOURCE_BARRIER::Transition(
      m_mipmapSPD.resColorBuffer.Get(),
      D3D12_RESOURCE_STATE_RENDER_TARGET,
      D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
      0), // base:0
  };
  commandList->ResourceBarrier(_countof(beforeBarriers), beforeBarriers);

  if (!m_skipGenMipmapRT)
  {
    GenerateMipmapRT(commandList);
  }
  if (!m_skipGenMipmapCS)
  {
    GenerateMipmapCS(commandList);
  }
  if (!m_skipGenMipmapSPD)
  {
    GenerateMipmapSPD(commandList);
  }

  if(m_skipGenMipmapRT)
  {
    // スキップ時,後続整合性のためのバリア設定.
    auto texture = m_mipmapRT.resColorBuffer;
    auto miplevels = texture->GetDesc().MipLevels;
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    for (UINT i = 1; i < miplevels; ++i)
    {
      auto& b = barriers.emplace_back();
      b = CD3DX12_RESOURCE_BARRIER::Transition(
        texture.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, i );
    }
    commandList->ResourceBarrier(UINT(barriers.size()), barriers.data());
  }
  if (m_skipGenMipmapCS)
  {
    // 必要ならバリア.
  }
  if (m_skipGenMipmapSPD)
  {
    // 必要ならバリア.
  }

  // SinglePassDownsamplerによってディスクリプタヒープが切り替わってしまうため、
  // ディスクリプタヒープを戻す.
  commandList->SetDescriptorHeaps(_countof(heaps), heaps);


  // 描画先をデフォルトに戻す.
  auto rtvHandle = gfxDevice->GetSwapchainBufferDescriptor();
  auto dsvHandle = m_depthBuffer.dsvHandle;
  commandList->OMSetRenderTargets(
    1, &rtvHandle.hCpu, FALSE, &dsvHandle.hCpu);

  const float clearColor[] = { 0.75f, 0.9f, 1.0f, 1.0f };
  commandList->ClearRenderTargetView(rtvHandle.hCpu, clearColor, 0, nullptr);
  commandList->ClearDepthStencilView(dsvHandle.hCpu, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

  // デフォルトのメインバッファ(バックバッファ)への描画.
  DrawDefaultTarget(commandList);

  // ImGui による描画.
  ImGui::Render();
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList.Get());

  D3D12_RESOURCE_BARRIER barrierToPresent{
    .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
    .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
    .Transition = {
      .pResource = backbuffer.Get(),
      .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
      .StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET,
      .StateAfter = D3D12_RESOURCE_STATE_PRESENT,
    }
  };
  commandList->ResourceBarrier(1, &barrierToPresent);

  // テクスチャの保存要求があればリードバックバッファに書込み.
  if (m_bSaveRequestButton && m_saveTextureRequests.empty())
  {
    // RenderTarget実装版のテクスチャを保存.
    UINT64 bufferOffset = 0;
    SaveTextureRequest request{};
    request.resTexture = m_mipmapRT.resColorBuffer;
    request.name = L"Color_MipmapsRT.dds";
    WriteToReadbackBuffer(commandList, &request, &bufferOffset);
    m_saveTextureRequests.push_back(request);

    // ComputeShader実装版のテクスチャを保存.
    request.resTexture = m_mipmapCS.resColorBuffer;
    request.name = L"Color_MipmapsCS.dds";
    WriteToReadbackBuffer(commandList, &request, &bufferOffset);
    m_saveTextureRequests.push_back(request);

    // SinglePassDownsampler実装版のテクスチャを保存.
    request.resTexture = m_mipmapSPD.resColorBuffer;
    request.name = L"Color_MipmapsSPD.dds";
    WriteToReadbackBuffer(commandList, &request, &bufferOffset);
    m_saveTextureRequests.push_back(request);
  }

  commandList->Close();
  return commandList;
}

void MyApplication::UpdateModelMatrix()
{
  auto& gfxDevice = GetGfxDevice();
  int frameIndex = gfxDevice->GetFrameIndex();

  // モデルのワールド行列を更新.
  m_model.mtxWorld = XMMatrixRotationY(m_frameDeltaAccum * 0.15f);

  for (uint32_t i = 0; i < m_model.drawInfos.size(); ++i)
  {
    const auto& info = m_model.drawInfos[i];
    const auto& mesh = m_model.meshes[i];
    const auto& material = m_model.materials[mesh.materialIndex];

    DrawParameters drawParams{};
    XMStoreFloat4x4(&drawParams.mtxWorld, XMMatrixTranspose(m_model.mtxWorld));
    drawParams.baseColor = material.diffuse;
    drawParams.specular = material.specular;
    drawParams.ambient = material.ambient;
    if (material.alphaMode == ModelMaterial::ALPHA_MODE_MASK)
    {
      drawParams.mode = 1;
    }
    if (m_overwrite)
    {
      drawParams.specular = m_globalSpecular;
      drawParams.ambient = m_globalAmbient;
    }

    // 定数バッファの更新.
    auto& cb = info.modelMeshConstantBuffer[frameIndex];
    void* p;
    cb->Map(0, nullptr, &p);
    memcpy(p, &drawParams, sizeof(drawParams));
    cb->Unmap(0, nullptr);
  }
}

void MyApplication::DrawModel(ComPtr<ID3D12GraphicsCommandList> commandList)
{
  auto& gfxDevice = GetGfxDevice();
  int frameIndex = gfxDevice->GetFrameIndex();

  auto modeList = {
    ModelMaterial::ALPHA_MODE_OPAQUE, ModelMaterial::ALPHA_MODE_MASK, ModelMaterial::ALPHA_MODE_BLEND
  };
  
  for (auto mode : modeList)
  {
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    switch (mode)
    {
    default:
    case ModelMaterial::ALPHA_MODE_OPAQUE:
      commandList->SetPipelineState(m_drawOpaquePipeline.Get());
      break;
    case ModelMaterial::ALPHA_MODE_MASK:
      commandList->SetPipelineState(m_drawOpaquePipeline.Get());
      break;
    case ModelMaterial::ALPHA_MODE_BLEND:
      commandList->SetPipelineState(m_drawBlendPipeline.Get());
      break;
    }
    for (uint32_t i = 0; i < m_model.drawInfos.size(); ++i)
    {
      const auto& info = m_model.drawInfos[i];
      const auto& mesh = m_model.meshes[i];
      const auto& material = m_model.materials[mesh.materialIndex];

      if (material.alphaMode != mode)
      {
        continue;
      }
      auto& cb = info.modelMeshConstantBuffer[frameIndex];


      // 描画.
      commandList->IASetVertexBuffers(0, _countof(mesh.vbViews), mesh.vbViews);
      commandList->IASetIndexBuffer(&mesh.ibv);
      commandList->SetGraphicsRootConstantBufferView(1, cb->GetGPUVirtualAddress());
      commandList->SetGraphicsRootDescriptorTable(2, material.srvDiffuse.hGpu);
      commandList->SetGraphicsRootDescriptorTable(3, material.samplerDiffuse.hGpu);

      commandList->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
    }
  }

}

// ミップマップのベースレベルへの描画処理.
void MyApplication::DrawMipmapBase(ComPtr<ID3D12GraphicsCommandList> commandList)
{
  // 描画先をテクスチャにする切り替えの処理.
  std::vector<D3D12_RESOURCE_BARRIER> toRenderTargets =
  {
    CD3DX12_RESOURCE_BARRIER::Transition(
      m_mipmapRT.resColorBuffer.Get(),D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
    CD3DX12_RESOURCE_BARRIER::Transition(
      m_mipmapCS.resColorBuffer.Get(),D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET, 0),
    CD3DX12_RESOURCE_BARRIER::Transition(
      m_mipmapSPD.resColorBuffer.Get(),D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET, 0),
  };
  commandList->ResourceBarrier(UINT(toRenderTargets.size()), toRenderTargets.data());

  const float colorBlack[] = { 0.0f, 0.0f, 0.0f, 0.0f };

  PIXBeginEvent(commandList.Get(),PIX_COLOR(32, 128, 255), L"DrawMipmapBase");
  // レンダーテクスチャ編. Base0書込み.
  {
    auto rtv = m_mipmapRT.rtvColorMip[0].hCpu;
    auto dsv = m_renderTargetCommonDepth.dsvDepth.hCpu;
    commandList->OMSetRenderTargets( 1, &rtv, FALSE, &dsv);

    commandList->ClearRenderTargetView(rtv, colorBlack, 0, nullptr);
    commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    DrawRenderTarget(commandList);  // 描画.
  }
  // コンピュートシェーダー編. Base0書込み.
  {
    auto rtv = m_mipmapCS.rtvColor.hCpu;
    auto dsv = m_renderTargetCommonDepth.dsvDepth.hCpu;
    commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    commandList->ClearRenderTargetView(rtv, colorBlack, 0, nullptr);
    commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    DrawRenderTarget(commandList);  // 描画.
  }


  // FidelityFX SPD編. Base0書込み.
  {
    auto rtv = m_mipmapSPD.rtvColor.hCpu;
    auto dsv = m_renderTargetCommonDepth.dsvDepth.hCpu;
    commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    commandList->ClearRenderTargetView(rtv, colorBlack, 0, nullptr);
    commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    DrawRenderTarget(commandList);  // 描画.
  }
  PIXEndEvent(commandList.Get());
}

void MyApplication::DrawRenderTarget(ComPtr<ID3D12GraphicsCommandList> commandList)
{
  auto& gfxDevice = GetGfxDevice();
  int frameIndex = gfxDevice->GetFrameIndex();

  // テクスチャへの描画に合わせて、描画先解像度を設定.
  D3D12_VIEWPORT viewport{
    .Width = float(RenderTexWidth), .Height = float(RenderTexHeight),
    .MinDepth = 0.0f, .MaxDepth = 1.0f
  };
  D3D12_RECT scissorRect{
    .left = 0, .top = 0,
    .right = LONG(RenderTexWidth), .bottom = LONG(RenderTexHeight),
  };
  XMMATRIX mtxProj = XMMatrixPerspectiveFovRH(XM_PIDIV4, float(RenderTexWidth) / float(RenderTexHeight), 0.1f, 200.0f);

  commandList->RSSetViewports(1, &viewport);
  commandList->RSSetScissorRects(1, &scissorRect);

  // テクスチャ描画用シーンでのカメラ位置の設定.
  XMFLOAT3 eyePos(5.0f, 1.0f, 0.0f), target(0, 1.1f, 0), upDir(0, 1, 0);
  XMMATRIX mtxView = XMMatrixLookAtRH(
    XMLoadFloat3(&eyePos), XMLoadFloat3(&target), XMLoadFloat3(&upDir)
  );

  SceneParameters sceneParams{};
  XMStoreFloat4x4(&sceneParams.mtxView, XMMatrixTranspose(mtxView));
  XMStoreFloat4x4(&sceneParams.mtxProj, XMMatrixTranspose(mtxProj));

  sceneParams.eyePosition = eyePos;
  sceneParams.lightDir = m_lightDir;
  sceneParams.time = m_frameDeltaAccum;

  // 現在のフレームに対応するブロックへデータを書き込む.
  auto cb = m_renderSceneCB;
  auto addressSceneCB = cb->GetGPUVirtualAddress();
  void* p;
  cb->Map(0, nullptr, &p);
  if (p)
  {
    UINT64 offset = sizeof(SceneParameters) * frameIndex;
    UINT writtenSize = sizeof(SceneParameters);
    auto dst = reinterpret_cast<char*>(p) + offset;
    memcpy(dst, &sceneParams, writtenSize);

    D3D12_RANGE range{};
    range.Begin = offset;
    range.End = offset + writtenSize;
    cb->Unmap(0, &range);
    addressSceneCB += offset;
  }

  commandList->SetGraphicsRootSignature(m_modelRootSignature.Get());
  commandList->SetGraphicsRootConstantBufferView(0, addressSceneCB);

  // モデルを描画.
  DrawModel(commandList);
}

void MyApplication::DrawDefaultTarget(ComPtr<ID3D12GraphicsCommandList> commandList)
{
  auto& gfxDevice = GetGfxDevice();
  int frameIndex = gfxDevice->GetFrameIndex();

  // ウィンドウの描画に合わせて、解像度を設定.
  // ビューポートおよびシザー領域の設定.
  int width, height;
  Win32Application::GetWindowSize(width, height);
  auto viewport = D3D12_VIEWPORT{
    .TopLeftX = 0.0f, .TopLeftY = 0.0f,
    .Width = float(width),
    .Height = float(height),
    .MinDepth = 0.0f, .MaxDepth = 1.0f,
  };
  auto scissorRect = D3D12_RECT{
    .left = 0, .top = 0,
    .right = width, .bottom = height,
  };
  XMMATRIX mtxProj = XMMatrixPerspectiveFovRH(XM_PIDIV4, viewport.Width / viewport.Height, 0.1f, 200.0f);

  commandList->RSSetViewports(1, &viewport);
  commandList->RSSetScissorRects(1, &scissorRect);

  // テクスチャ描画用シーンでのカメラ位置の設定.
  XMFLOAT3 eyePos(0.0, 0.5, 1.0f), target(0, 0.0, 0), upDir(0, 1, 0);
  XMMATRIX mtxView = XMMatrixLookAtRH(
    XMLoadFloat3(&eyePos), XMLoadFloat3(&target), XMLoadFloat3(&upDir)
  );

  SceneParameters sceneParams{};
  XMStoreFloat4x4(&sceneParams.mtxView, XMMatrixTranspose(mtxView));
  XMStoreFloat4x4(&sceneParams.mtxProj, XMMatrixTranspose(mtxProj));

  sceneParams.eyePosition = eyePos;
  sceneParams.lightDir = m_lightDir;
  sceneParams.time = m_frameDeltaAccum;

  DrawParameters drawData{};
  auto rotation = XMMatrixRotationY(m_frameDeltaAccum * 0.1f)* XMMatrixRotationX(m_frameDeltaAccum * 0.15f);
  XMStoreFloat4x4(&drawData.mtxWorld, rotation);

  // 現在のフレームに対応するブロックへデータを書き込む.
  auto sceneCB = m_defaultSceneCB;
  auto addressSceneCB = sceneCB->GetGPUVirtualAddress();
  void* p;
  sceneCB->Map(0, nullptr, &p);
  if (p)
  {
    UINT64 offset = sizeof(SceneParameters) * frameIndex;
    UINT writtenSize = sizeof(SceneParameters);
    auto dst = reinterpret_cast<char*>(p) + offset;
    memcpy(dst, &sceneParams, writtenSize);

    D3D12_RANGE range{};
    range.Begin = offset;
    range.End = offset + writtenSize;
    sceneCB->Unmap(0, &range);
    addressSceneCB += offset;
  }

  auto modelCB = m_drawPlaneInfo.modelCB;
  auto addressModelCB = modelCB->GetGPUVirtualAddress();
  modelCB->Map(0, nullptr, &p);
  if (p)
  {
    UINT64 offset = sizeof(DrawParameters) * frameIndex;
    UINT writtenSize = sizeof(DrawParameters);
    auto dst = reinterpret_cast<char*>(p) + offset;
    memcpy(dst, &drawData, writtenSize);

    D3D12_RANGE range{};
    range.Begin = offset;
    range.End = offset + writtenSize;
    modelCB->Unmap(0, &range);
    addressModelCB += offset;
  }

  // テクスチャ1枚を貼るだけのシンプルなものなため、Mipmap(RT)で作ったルートシグネチャを再利用.
  commandList->SetGraphicsRootSignature(m_simpleRootSignature.Get());
  commandList->IASetVertexBuffers(0, 1, &m_drawPlaneInfo.vbv);
  commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  commandList->SetPipelineState(m_drawSimplePipeline.Get());
  commandList->SetGraphicsRootConstantBufferView(0, addressSceneCB);
  commandList->SetGraphicsRootConstantBufferView(1, addressModelCB);

  // 生成したミップマップありテクスチャを設定.
  D3D12_GPU_DESCRIPTOR_HANDLE srvTexColor;
  switch (m_drawMipSource)
  {
  default:
  case FromRenderTarget:
    srvTexColor = m_mipmapRT.srvColor.hGpu;
    break;
  case FromCompute:
    srvTexColor = m_mipmapCS.srvColor.hGpu;
    break;
  case FromSPD:
    srvTexColor = m_mipmapSPD.srvColor.hGpu;
    break;
  }
  commandList->SetGraphicsRootDescriptorTable(2, srvTexColor);

  commandList->DrawInstanced(4, 1, 0, 0);
}

void MyApplication::GenerateMipmapRT(ComPtr<ID3D12GraphicsCommandList> commandList)
{
  PIXBeginEvent(commandList.Get(), PIX_COLOR(255, 128, 0), L"WriteMipRT");

  // ベースレベルのテクスチャはシェーダーリソース状態.
  // 他のレベルを描画して、次の段の入力として使う.
  const auto texDesc = m_mipmapRT.resColorBuffer->GetDesc();
  const auto MipLevels = texDesc.MipLevels;

  commandList->SetGraphicsRootSignature(m_mipmapRT.rootSignature.Get());
  commandList->SetPipelineState(m_mipmapRT.writeMipmapPso.Get());
  const float colorBlack[] = { 0.0f, 0.0f, 0.0f, 0.0f };
  for (UINT mip = 1; mip < MipLevels; ++mip)
  {
    auto width = (std::max)(UINT(texDesc.Width) >> mip, 1u);
    auto height = (std::max)(UINT(texDesc.Height) >> mip, 1u);
    D3D12_VIEWPORT viewport{
      .Width = float(width), .Height = float(height),
      .MinDepth = 0.0f, .MaxDepth = 1.0f,
    };
    D3D12_RECT scissor{
      .left = 0, .top = 0,
      .right = LONG(width), .bottom = LONG(height),
    };

    auto rtv = m_mipmapRT.rtvColorMip[mip].hCpu;
    auto srv = m_mipmapRT.srvColorMip[mip - 1].hGpu;
    commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    commandList->ClearRenderTargetView(rtv, colorBlack, 0, nullptr);
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->SetGraphicsRootDescriptorTable(0, srv);
    commandList->DrawInstanced(6, 1, 0, 0);

    // 描画先のデータは次の段の入力で使うためバリアを設定する.
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
      m_mipmapRT.resColorBuffer.Get(),
      D3D12_RESOURCE_STATE_RENDER_TARGET,
      D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
      mip
    );
    commandList->ResourceBarrier(1, &barrier);
  }
  // ここに到達時にはミップマップ全てのレベルで SHADER_RESOURCE になっている.
  PIXEndEvent(commandList.Get());
}

void MyApplication::GenerateMipmapCS(ComPtr<ID3D12GraphicsCommandList> commandList)
{
  auto& gfxDevice = GetGfxDevice();
  auto frameIndex = gfxDevice->GetFrameIndex();

  PIXBeginEvent(commandList.Get(), PIX_COLOR(32, 255, 0), L"WriteMipCS");
  // ベースレベルのテクスチャはシェーダーリソース状態.
  // サブのレベルを全てアンオーダードアクセス状態に変更する.
  const auto texDesc = m_mipmapCS.resColorBuffer->GetDesc();
  const auto MipLevels = texDesc.MipLevels;
  std::vector<D3D12_RESOURCE_BARRIER> beforeBarriers;
  for (UINT i = 1; i < MipLevels; ++i)
  {
    beforeBarriers.push_back(
      CD3DX12_RESOURCE_BARRIER::Transition(
        m_mipmapCS.resColorBuffer.Get(),
        D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        i
      )
    );
  }
  commandList->ResourceBarrier(UINT(beforeBarriers.size()), beforeBarriers.data());

  // ミップマップ生成.
  commandList->SetComputeRootSignature(m_mipmapCS.rootSignature.Get());
  commandList->SetPipelineState(m_mipmapCS.writeMipmapPso->Get());
  void* p = nullptr;
  m_mipmapCS.cbMinGenBuffer->Map(0, nullptr, &p);
  auto frameOffset = frameIndex * (sizeof(MipmapGenConstants) * MaxMipmapExecLevel);
  auto writePtr = reinterpret_cast<char*>(p) + frameOffset;

  for (UINT topMip = 0; topMip < UINT(MipLevels - 1);)
  {
    UINT srcWidth = UINT(texDesc.Width >> topMip);
    UINT srcHeight = UINT(texDesc.Height >> topMip);
    UINT dstWidth = srcWidth >> 1, dstHeight = srcHeight >> 1;

    uint32_t AdditionalMips;
    UINT value = (dstWidth == 1 ? dstHeight : dstWidth) | (dstHeight == 1 ? dstWidth : dstHeight);
    _BitScanForward((unsigned long*)&AdditionalMips, value);
    UINT numMips = 1 + std::min(3u, AdditionalMips); // 合計4枚がMAX.
    if (topMip + numMips > MipLevels)
    {
      numMips = MipLevels - topMip;
    }
    dstWidth = std::max(1u, dstWidth);
    dstHeight = std::max(1u, dstHeight);

    // ソース解像度のタイプに合わせてパイプラインを選択.
    uint32_t nonPowerOfTwo = (srcWidth & 1) | (srcHeight & 1) << 1;
    auto csPso = m_mipmapCS.writeMipmapPso[nonPowerOfTwo];
    commandList->SetPipelineState(csPso.Get());

    MipmapGenConstants mipmapGenParams{};
    mipmapGenParams.SrcMipLevel = topMip;
    mipmapGenParams.NumMipLevels = numMips;
    mipmapGenParams.TexelSize.x = 1.0f / float(dstWidth);
    mipmapGenParams.TexelSize.y = 1.0f / float(dstHeight);
    auto bufferOffset = sizeof(MipmapGenConstants) * topMip;
    memcpy(writePtr + bufferOffset, &mipmapGenParams, sizeof(mipmapGenParams));

    auto cbGpuAddress = m_mipmapCS.cbMinGenBuffer->GetGPUVirtualAddress();
    cbGpuAddress += frameOffset;
    cbGpuAddress += bufferOffset;
    commandList->SetComputeRootConstantBufferView(0, cbGpuAddress);

    // 読み込み元になるミップマップ画像をセット.
    commandList->SetComputeRootDescriptorTable(1, m_mipmapCS.srvColorMip[topMip].hGpu);

    // 書込み先をセット.
    for (UINT i = 0; i < numMips; ++i)
    {
      constexpr auto slotIndex = 2; 
      commandList->SetComputeRootDescriptorTable(
        slotIndex + i,
        m_mipmapCS.uavColorMip[topMip + i + 1].hGpu
      );
    }

    // 書込みの実行.
    auto x = UINT(std::ceil(dstWidth / 8.0)), y = UINT(std::ceil(dstHeight / 8.0));
    commandList->Dispatch(x, y, 1);

    // UAVバリアを発行後、書き込んだミップレベルをシェーダーリソース状態へ更新.
    auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_mipmapCS.resColorBuffer.Get());
    std::vector<D3D12_RESOURCE_BARRIER> toSrvBarriers;
    toSrvBarriers.reserve(numMips);
    for (UINT i = 0; i < numMips; ++i)
    {
      toSrvBarriers.push_back(
        CD3DX12_RESOURCE_BARRIER::Transition(
          m_mipmapCS.resColorBuffer.Get(),
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
          topMip + i + 1
        )
      );
    }
    commandList->ResourceBarrier(UINT(toSrvBarriers.size()), toSrvBarriers.data());

    topMip += numMips;
  }
  D3D12_RANGE writeRange{};
  writeRange.Begin = frameOffset;
  writeRange.End = frameOffset + sizeof(MipmapGenConstants) * MaxMipmapExecLevel;
  m_mipmapCS.cbMinGenBuffer->Unmap(0, &writeRange);

  // ここに到達時にはミップマップ全てのレベルで SHADER_RESOURCE になっている.
  PIXEndEvent(commandList.Get());
}

void MyApplication::GenerateMipmapSPD(ComPtr<ID3D12GraphicsCommandList> commandList)
{
  PIXBeginEvent(commandList.Get(), PIX_COLOR(255, 32, 16), L"GenSPD");
  auto texResource = m_mipmapSPD.resColorBuffer;
  FfxResource resTexture{
    .resource = texResource.Get(),
    .description = ffxGetResourceDescriptionDX12(texResource.Get()),
    .state = FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ,
  };

  FfxSpdDispatchDescription dispatchParams{};
  dispatchParams.commandList = commandList.Get();
  dispatchParams.resource = resTexture;

  auto errcode = ffxSpdContextDispatch(&m_mipmapSPD.contextSpd, &dispatchParams);
  assert(errcode == FFX_OK);
  PIXEndEvent(commandList.Get());
}

void MyApplication::WriteToReadbackBuffer(ComPtr<ID3D12GraphicsCommandList> commandList, SaveTextureRequest* request, UINT64* pBufferOffset)
{
  auto& gfxDevice = GetGfxDevice();
  auto d3d12Device = gfxDevice->GetD3D12Device();
  if (request == nullptr || pBufferOffset == nullptr)
  {
    return;
  }
  auto resTexture = request->resTexture;

  // 対象リソースをコピー元のステートへ.
  auto toCopySource = CD3DX12_RESOURCE_BARRIER::Transition(
    resTexture.Get(),
    D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
    D3D12_RESOURCE_STATE_COPY_SOURCE
  );
  commandList->ResourceBarrier(1, &toCopySource);

  // メモリ配置情報を取得.
  auto& layouts = request->layouts;
  auto resDesc = resTexture->GetDesc();
  UINT numSubresources = resDesc.MipLevels * resDesc.DepthOrArraySize;
  layouts.resize(numSubresources);
  UINT64 requestdSize = 0;
  d3d12Device->GetCopyableFootprints(&resDesc, 0, numSubresources, 0, layouts.data(), nullptr, nullptr, &requestdSize);
  auto bufferBase = *pBufferOffset;

  // コピー
  for (UINT m = 0; m < numSubresources; ++m)
  {
    D3D12_TEXTURE_COPY_LOCATION srcLoc{}, dstLoc{};
    srcLoc.pResource = request->resTexture.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = m;

    dstLoc.pResource = m_readbackInfo.resReadbackBuffer.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    layouts[m].Offset += bufferBase;
    dstLoc.PlacedFootprint = layouts[m];

    commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
  }
  bufferBase += requestdSize;
  *pBufferOffset = bufferBase;

  // 対象リソースをシェーダーリソースに戻す.
  auto toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
    resTexture.Get(),
    D3D12_RESOURCE_STATE_COPY_SOURCE,
    D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE
  );
  commandList->ResourceBarrier(1, &toShaderResource);

}

void MyApplication::ProcessSaveTexture()
{
  auto& gfxDevice = GetGfxDevice();
  if (!m_readbackInfo.saveState.has_value())
  {
    // 保存の開始. フェンスを設定する.
    m_readbackInfo.nextFenceValue++;
    gfxDevice->GetD3D12CommandQueue()->Signal(
      m_readbackInfo.fence.Get(),
      m_readbackInfo.nextFenceValue
    );
    m_readbackInfo.saveState = SaveTextureState::StateWaitForReadback;
  }

  if (m_readbackInfo.saveState == SaveTextureState::StateWaitForReadback)
  {
    if (m_readbackInfo.fence->GetCompletedValue() == m_readbackInfo.nextFenceValue)
    {
      // リードバックバッファへの書込みが完了したのでテクスチャの保存
      void* p;
      m_readbackInfo.resReadbackBuffer->Map(0, nullptr, &p);
      using ScratchImageList = std::vector<DirectX::ScratchImage>;
      auto imageList = std::make_shared<ScratchImageList>();

      for (auto request : m_saveTextureRequests)
      {
        auto& image = imageList->emplace_back();
        CreateScratchImageFromRequest(image, &request, p);
      }
      m_readbackInfo.resReadbackBuffer->Unmap(0, nullptr);

      // ファイルIOは時間が掛かることを想定し、別スレッドで処理.
      std::thread saveThread([=, this]() {
        // imageList の各要素を取り出してファイルに保存.
        for (uint32_t i = 0; i < m_saveTextureRequests.size(); ++i)
        {
          auto& image = (*imageList)[i];
          auto& fname = m_saveTextureRequests[i].name;

          auto saveFilePath = std::filesystem::path(L"./saveTextures");
          if (!std::filesystem::exists(saveFilePath))
          {
            std::filesystem::create_directories(saveFilePath);
          }
          saveFilePath = saveFilePath / fname;

          DirectX::SaveToDDSFile(
            image.GetImages(), image.GetImageCount(),
            image.GetMetadata(),
            DirectX::DDS_FLAGS_NONE, saveFilePath.c_str());
        }

        // 時間が掛かる処理を模してスリープ.
        std::this_thread::sleep_for(std::chrono::seconds(5));

        // ステータス情報をクリア.
        m_bSaveRequestButton = false;
        m_saveTextureRequests.clear();
        m_readbackInfo.saveState.reset();
      });
      saveThread.detach();
      m_readbackInfo.saveState = SaveTextureState::StateWaitForWriteComplete;
    }
  }
}

bool MyApplication::CreateScratchImageFromRequest(DirectX::ScratchImage& scratchImage, const SaveTextureRequest* request, const void* readbackBuffer)
{
  if (request == nullptr || readbackBuffer == nullptr)
  {
    return false;
  }
  const auto resDesc = request->resTexture->GetDesc();
  const auto& layouts = request->layouts;

  if (resDesc.DepthOrArraySize == 6)
  {
    auto cubemaps = resDesc.DepthOrArraySize / 6;
    scratchImage.InitializeCube(
      resDesc.Format, size_t(resDesc.Width), size_t(resDesc.Height), cubemaps, resDesc.MipLevels);
  } else
  {
    scratchImage.Initialize2D(
      resDesc.Format, size_t(resDesc.Width), size_t(resDesc.Height), resDesc.DepthOrArraySize, resDesc.MipLevels);
  }
  for (UINT item = 0; item < resDesc.DepthOrArraySize; ++item)
  {
    for (UINT i = 0; i < resDesc.MipLevels; ++i)
    {
      const auto& layout = layouts[i + resDesc.MipLevels * item];
      auto* image = scratchImage.GetImage(i, item, 0);

      const auto* pSrc = static_cast<const uint8_t*>(readbackBuffer) + layout.Offset;
      auto* pDst = image->pixels;
      for (UINT row = 0; row < layout.Footprint.Height; ++row)
      {
        memcpy(pDst, pSrc, image->rowPitch);
        pSrc += layout.Footprint.RowPitch;
        pDst += image->rowPitch;
      }
    }
  }
  return true;
}
