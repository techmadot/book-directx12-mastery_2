﻿#include "App.h"

#include "GfxDevice.h"
#include "FileLoader.h"
#include "Win32Application.h"

#include "imgui.h"
#include "imgui/backends/imgui_impl_dx12.h"
#include "imgui/backends/imgui_impl_win32.h"

#include "TextureUtility.h"
#include <DirectXTex.h>
#include <fstream>
#include <random>

#include <windows.h>

using namespace Microsoft::WRL;
using namespace DirectX;

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
  m_title = L"TextureRendering";
}

void MyApplication::Initialize()
{
  auto& gfxDevice = GetGfxDevice();
  GfxDevice::DeviceInitParams initParams;
  initParams.formatDesired = DXGI_FORMAT_R8G8B8A8_UNORM;
  gfxDevice->Initialize(initParams);

  PrepareDepthBuffer();
  PrepareRenderTarget();

  PrepareImGui();

  PrepareSceneConstantBuffer();

  PrepareSimplePipeline();
  PreparePlaneData();

  PrepareModelDrawPipeline();

  PrepareModelData();
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
  D3D12_DESCRIPTOR_RANGE rangeSamplerRanges[] = {
    {  // s0 サンプラー.
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
  float* lightDir = (float*)&m_lightDir;
  ImGui::InputFloat3("LightDir", lightDir);
  ImGui::Checkbox("Overwrite", &m_overwrite);

  float* specularColor = (float*)&m_globalSpecular;
  ImGui::InputFloat3("Specular", specularColor);
  ImGui::InputFloat("Power", (float*)&m_globalSpecular.w);
  float* ambientColor = (float*)&m_globalAmbient;
  ImGui::InputFloat3("Ambient", ambientColor);
  ImGui::End();

  auto& gfxDevice = GetGfxDevice();
  gfxDevice->NewFrame();

  // 描画のコマンドを作成.
  auto commandList = MakeCommandList();

  // 作成したコマンドを実行.
  gfxDevice->Submit(commandList.Get());
  // 描画した内容を画面へ反映.
  gfxDevice->Present(1);

  m_frameDeltaAccum += ImGui::GetIO().DeltaTime;
}

void MyApplication::Shutdown()
{
  auto& gfxDevice = GetGfxDevice();
  gfxDevice->WaitForGPU();

  // リソースを解放.
  m_drawOpaquePipeline.Reset();
  m_modelRootSignature.Reset();

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
  // 描画先となるカラーテクスチャの作成.
  auto resRenderTex = D3D12_RESOURCE_DESC{
    .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
    .Width = RenderTexWidth, .Height = RenderTexHeight,
    .DepthOrArraySize = 1,
    .MipLevels = 1,
    .Format= DXGI_FORMAT_R8G8B8A8_UNORM,
    .SampleDesc = {.Count = 1, .Quality = 0},
    .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
    .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
  };
  D3D12_CLEAR_VALUE colorClear{
    .Format = resRenderTex.Format,
    .Color = { 0.0f, 0.0f, 0.0f, 0.0f }
  };
  m_renderTarget.resColorBuffer = gfxDevice->CreateImage2D(resRenderTex, defaultHeap, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &colorClear);


  // レンダーターゲット描画中で使用するデプスバッファの作成.
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
  m_renderTarget.resDepthBuffer = gfxDevice->CreateImage2D(resDepthDesc, defaultHeap, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear);

  // 各ディスクリプタの作成.
  m_renderTarget.rtvColor = gfxDevice->CreateRenderTargetView(m_renderTarget.resColorBuffer, nullptr);
  m_renderTarget.srvColor = gfxDevice->CreateShaderResourceView(m_renderTarget.resColorBuffer, nullptr);
  m_renderTarget.dsvDepth = gfxDevice->CreateDepthStencilView(m_renderTarget.resDepthBuffer, nullptr);
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

  // 描画先をテクスチャにする切り替えの処理.
  auto toRenderTarget = D3D12_RESOURCE_BARRIER{
    .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
    .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
    .Transition = {
      .pResource = m_renderTarget.resColorBuffer.Get(),
      .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
      .StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
      .StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET,
    }
  };
  commandList->ResourceBarrier(1, &toRenderTarget);
  commandList->OMSetRenderTargets(
    1, &m_renderTarget.rtvColor.hCpu, FALSE,
    &m_renderTarget.dsvDepth.hCpu);

  const float colorBlack[] = { 0.0f, 0.0f, 0.0f, 0.0f };
  commandList->ClearRenderTargetView(m_renderTarget.rtvColor.hCpu, colorBlack, 0, nullptr);
  commandList->ClearDepthStencilView(m_renderTarget.dsvDepth.hCpu, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

  // レンダーターゲット(テクスチャ)への描画.
  DrawRenderTarget(commandList);

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
    {
      .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
      .Transition = {
        .pResource = m_renderTarget.resColorBuffer.Get(),
        .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        .StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET,
        .StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
      }
    }
  };
  commandList->ResourceBarrier(_countof(beforeBarriers), beforeBarriers);

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
  commandList->Close();
  return commandList;
}

std::vector<MyApplication::TextureInfo>::const_iterator MyApplication::FindModelTexture(const std::string& filePath, const ModelData& model)
{
  return std::find_if(model.textureList.begin(), model.textureList.end(), [&](const auto& v) { return v.filePath == filePath; });
}

void MyApplication::DrawModel(ComPtr<ID3D12GraphicsCommandList> commandList)
{
  auto& gfxDevice = GetGfxDevice();
  int frameIndex = gfxDevice->GetFrameIndex();

  // モデルのワールド行列を更新.
  m_model.mtxWorld = XMMatrixRotationY(m_frameDeltaAccum * 0.25f);
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
  XMFLOAT3 eyePos(5.0, 1.0, 0.0f), target(0, 1.0, 0), upDir(0, 1, 0);
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
  XMFLOAT3 eyePos(0.0, 0.5, 3.0f), target(0, 0.0, 0), upDir(0, 1, 0);
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
  auto rotation = XMMatrixRotationY(m_frameDeltaAccum * 0.1f) * XMMatrixRotationX(m_frameDeltaAccum * 0.15f);
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

  commandList->IASetVertexBuffers(0, 1, &m_drawPlaneInfo.vbv);
  commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  commandList->SetPipelineState(m_drawSimplePipeline.Get());
  commandList->SetGraphicsRootConstantBufferView(0, addressSceneCB);
  commandList->SetGraphicsRootConstantBufferView(1, addressModelCB);
  commandList->SetGraphicsRootDescriptorTable(2, m_renderTarget.srvColor.hGpu); // 直前で描画したテクスチャ
  commandList->DrawInstanced(4, 1, 0, 0);

}
