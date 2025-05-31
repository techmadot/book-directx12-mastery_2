﻿#include "GfxDevice.h"
#include "Win32Application.h"
#include <stdexcept>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

static std::unique_ptr<GfxDevice> gGfxDevice = nullptr;

std::unique_ptr<GfxDevice>& GetGfxDevice()
{
  if (gGfxDevice == nullptr)
  {
    gGfxDevice = std::make_unique<GfxDevice>();
  }
  return gGfxDevice;
}

void GfxDevice::Initialize(const DeviceInitParams& initParams)
{
  HRESULT hr;
  UINT dxgiFlags = 0;

#if _DEBUG // 開発時にはデバッグ機能を有効にする.
  ComPtr<ID3D12Debug> d3d12Debug;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d3d12Debug))))
  {
    d3d12Debug->EnableDebugLayer();
    dxgiFlags = DXGI_CREATE_FACTORY_DEBUG;

    ComPtr<ID3D12Debug3> debug3;
    d3d12Debug.As(&debug3);
    if (debug3)
    {
      debug3->SetEnableGPUBasedValidation(TRUE);
    }
  }
#endif

  hr = CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&m_dxgiFactory));
  ThrowIfFailed(hr, "CreateDXGIFactory2で失敗");

  // デバイスの選択.
  SelectDevice();
  hr = D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_d3d12Device));
  ThrowIfFailed(hr, "D3D12CreateDeviceで失敗");

  // コマンドキューの作成.
  D3D12_COMMAND_QUEUE_DESC queueDesc{
    .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
    .Priority = 0,
    .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
    .NodeMask = 0,
  };
  hr = m_d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue));
  ThrowIfFailed(hr, "CreateCommandQueueで失敗");

  // ディスクリプタヒープの作成.
  CreateDescriptorHeaps();

  // スワップチェインの作成.
  CreateSwapchain(initParams.formatDesired);

  // レンダーターゲットビューの準備.
  PrepareRenderTargetView();

  // コマンドアロケーターの作成.
  CreateCommandAllocators();

  m_frameIndex = m_swapchain->GetCurrentBackBufferIndex();
}

void GfxDevice::Shutdown()
{
  DestroyCommandAllocators();
  
  m_swapchain.Reset();
  m_commandQueue.Reset();
  m_d3d12Device.Reset();
}

GfxDevice::DescriptorHandle GfxDevice::GetSwapchainBufferDescriptor()
{
  return m_frameInfo[m_frameIndex].rtvDescriptor;
}

GfxDevice::ComPtr<ID3D12Resource1> GfxDevice::GetSwapchainBufferResource()
{
  return m_frameInfo[m_frameIndex].targetBuffer;
}

void GfxDevice::Submit(ID3D12CommandList* const commandList)
{
  m_commandQueue->ExecuteCommandLists(1, &commandList);
}

void GfxDevice::Present(UINT syncInterval, UINT flags)
{
  if (m_swapchain)
  {
    m_swapchain->Present(syncInterval, flags);
    const UINT64 currentFenceValue = m_frameInfo[m_frameIndex].fenceValue;
    m_commandQueue->Signal(m_frameFence.Get(), currentFenceValue);

    // インデックスを更新.
    m_frameIndex = m_swapchain->GetCurrentBackBufferIndex();

    const UINT64 expectValue = m_frameInfo[m_frameIndex].fenceValue;
    if (m_frameFence->GetCompletedValue() < expectValue)
    {
      m_frameFence->SetEventOnCompletion(expectValue, m_waitFence);
      WaitForSingleObjectEx(m_waitFence, INFINITE, FALSE);
    }
    m_frameInfo[m_frameIndex].fenceValue = currentFenceValue + 1;
  }
}

void GfxDevice::NewFrame()
{
  m_frameInfo[m_frameIndex].commandAllocator->Reset();
}

void GfxDevice::WaitForGPU()
{
  ComPtr<ID3D12Fence1> fence;
  HANDLE hWait = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  HRESULT hr;
  hr = m_d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  ThrowIfFailed(hr, "CreateFenceに失敗");

  const auto value = 1;
  m_commandQueue->Signal(fence.Get(), value);
  fence->SetEventOnCompletion(value, hWait);
  WaitForSingleObjectEx(hWait, INFINITE, FALSE);

  CloseHandle(hWait);
}


GfxDevice::ComPtr<ID3D12Resource1> GfxDevice::CreateBuffer(const D3D12_RESOURCE_DESC& resDesc, const D3D12_HEAP_PROPERTIES& heapProps)
{
  ComPtr<ID3D12Resource1> buffer;
  m_d3d12Device->CreateCommittedResource(
    &heapProps,
    D3D12_HEAP_FLAG_NONE,
    &resDesc,
    D3D12_RESOURCE_STATE_GENERIC_READ,
    nullptr,
    IID_PPV_ARGS(&buffer)
  );
  return buffer;
}

GfxDevice::ComPtr<ID3D12Resource1> GfxDevice::CreateImage2D(const D3D12_RESOURCE_DESC& resDesc, const D3D12_HEAP_PROPERTIES& heapProps, D3D12_RESOURCE_STATES resourceState, const D3D12_CLEAR_VALUE* clearValue)
{
  ComPtr<ID3D12Resource1> image;
  m_d3d12Device->CreateCommittedResource(
    &heapProps,
    D3D12_HEAP_FLAG_NONE,
    &resDesc,
    resourceState,
    clearValue,
    IID_PPV_ARGS(&image)
  );
  return image;
}

GfxDevice::ComPtr<ID3D12RootSignature> GfxDevice::CreateRootSignature(ComPtr<ID3DBlob> rootSignatureBlob)
{
  ComPtr<ID3D12RootSignature> rootSignature;
  HRESULT hr = m_d3d12Device->CreateRootSignature(
    0,
    rootSignatureBlob->GetBufferPointer(), rootSignatureBlob->GetBufferSize(),
    IID_PPV_ARGS(&rootSignature)
  );
  ThrowIfFailed(hr, "CreateRootSignatureに失敗");
  return rootSignature;
}

GfxDevice::ComPtr<ID3D12PipelineState> GfxDevice::CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& psoDesc)
{
  ComPtr<ID3D12PipelineState> pso;
  HRESULT hr = m_d3d12Device->CreateGraphicsPipelineState(
    &psoDesc, IID_PPV_ARGS(&pso));
  ThrowIfFailed(hr, "CreateGraphicsPipelineStateに失敗");
  return pso;
}

GfxDevice::ComPtr<ID3D12PipelineState> GfxDevice::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC& psoDesc)
{
  ComPtr<ID3D12PipelineState> pso;
  HRESULT hr = m_d3d12Device->CreateComputePipelineState(
    &psoDesc, IID_PPV_ARGS(&pso));
  ThrowIfFailed(hr, "CreateComputePipelineStateに失敗");
  return pso;
}

GfxDevice::ComPtr<ID3D12GraphicsCommandList> GfxDevice::CreateCommandList()
{
  ComPtr<ID3D12GraphicsCommandList> commandList;
  auto frameIndex = GetFrameIndex();
  
  HRESULT hr = m_d3d12Device->CreateCommandList(
    0,
    D3D12_COMMAND_LIST_TYPE_DIRECT,
    m_frameInfo[frameIndex].commandAllocator.Get(),
    nullptr,
    IID_PPV_ARGS(&commandList)
  );
  ThrowIfFailed(hr, "CreateCommandListに失敗");
  return commandList;
}

GfxDevice::ComPtr<ID3D12Resource1> GfxDevice::CreateBuffer(const D3D12_RESOURCE_DESC& resDesc, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES resourceState, const void* srcData)
{
  bool useStaging = false;
  ComPtr<ID3D12Resource1> retBuffer;
  D3D12_RESOURCE_STATES initState = resourceState;
  if (heapType == D3D12_HEAP_TYPE_DEFAULT)
  {
    initState = D3D12_RESOURCE_STATE_COMMON;
  }
  if (srcData != nullptr && (heapType == D3D12_HEAP_TYPE_DEFAULT))
  {
    useStaging = true;
  }
  if (heapType == D3D12_HEAP_TYPE_READBACK || heapType == D3D12_HEAP_TYPE_CUSTOM)
  {
    throw std::runtime_error("Not Supported.");
  }

  D3D12_HEAP_PROPERTIES heapProps{
    .Type = heapType,
    .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
    .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
    .CreationNodeMask = 1, .VisibleNodeMask = 1,
  };

  HRESULT hr;
  hr = m_d3d12Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc, initState, nullptr, IID_PPV_ARGS(&retBuffer));
  ThrowIfFailed(hr, "CreateCommittedResourceに失敗");

  if (srcData != nullptr)
  {
    if (heapType == D3D12_HEAP_TYPE_UPLOAD)
    {
      // 直接メモリマップして書込み.
      void* p = nullptr;
      retBuffer->Map(0, nullptr, &p);
      if (p)
      {
        memcpy(p, srcData, resDesc.Width);
        retBuffer->Unmap(0, nullptr);
      }
    }
    else
    {
      // ステージングバッファを作成.
      D3D12_HEAP_PROPERTIES uploadHeapProps(heapProps);
      uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
      ComPtr<ID3D12Resource1> staging;
      hr = m_d3d12Device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &resDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging));
      ThrowIfFailed(hr, "CreateCommittedResourceに失敗(ステージングバッファ)");

      void* p = nullptr;
      staging->Map(0, nullptr, &p);
      if (p)
      {
        memcpy(p, srcData, resDesc.Width);
        staging->Unmap(0, nullptr);
      }

      // ステージングバッファから目的のバッファへ転送.
      auto commandList = CreateCommandList();
      D3D12_RESOURCE_BARRIER beginBarrier{
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {
          .pResource = retBuffer.Get(),
          .Subresource = 0,
          .StateBefore = D3D12_RESOURCE_STATE_COMMON,
          .StateAfter = D3D12_RESOURCE_STATE_COPY_DEST
        }
      };
      commandList->CopyResource(retBuffer.Get(), staging.Get());

      // リソース状態を変更.
      D3D12_RESOURCE_BARRIER lastBarrier{
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {
          .pResource = retBuffer.Get(),
          .Subresource = 0,
          .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
          .StateAfter = resourceState
        }
      };
      commandList->ResourceBarrier(1, &lastBarrier);
      commandList->Close();
      Submit(commandList.Get());
      WaitForGPU();
    }
  }
  return retBuffer;
}

GfxDevice::DescriptorHandle GfxDevice::CreateRenderTargetView(ComPtr<ID3D12Resource1> renderTargetResource, D3D12_RENDER_TARGET_VIEW_DESC* rtvDesc)
{
  auto rtvHandle = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  m_d3d12Device->CreateRenderTargetView(renderTargetResource.Get(), rtvDesc, rtvHandle.hCpu);
  return rtvHandle;
}

GfxDevice::DescriptorHandle GfxDevice::CreateDepthStencilView(ComPtr<ID3D12Resource1> depthImage, D3D12_DEPTH_STENCIL_VIEW_DESC* dsvDesc)
{
  auto dsvHandle = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  m_d3d12Device->CreateDepthStencilView(depthImage.Get(), dsvDesc, dsvHandle.hCpu);
  return dsvHandle;
}

GfxDevice::DescriptorHandle GfxDevice::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC& cbvDesc)
{
  DescriptorHandle descriptor = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  m_d3d12Device->CreateConstantBufferView(&cbvDesc, descriptor.hCpu);
  return descriptor;
}

GfxDevice::DescriptorHandle GfxDevice::CreateShaderResourceView(ComPtr<ID3D12Resource1> res, D3D12_SHADER_RESOURCE_VIEW_DESC* srvDesc)
{
  DescriptorHandle srvDescriptor = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  m_d3d12Device->CreateShaderResourceView(res.Get(), srvDesc, srvDescriptor.hCpu);
  return srvDescriptor;
}

GfxDevice::DescriptorHandle GfxDevice::CreateUnorderedAccessView(ComPtr<ID3D12Resource1> res, D3D12_UNORDERED_ACCESS_VIEW_DESC* uavDesc)
{
  DescriptorHandle uavDescriptor = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  m_d3d12Device->CreateUnorderedAccessView(res.Get(), nullptr, uavDesc, uavDescriptor.hCpu);
  return uavDescriptor;
}

GfxDevice::DescriptorHandle GfxDevice::CreateSampler(const D3D12_SAMPLER_DESC& samplerDesc)
{
  DescriptorHandle descriptor = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  m_d3d12Device->CreateSampler(&samplerDesc, descriptor.hCpu);
  return descriptor;
}

GfxDevice::DescriptorHandle GfxDevice::AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE type)
{
  DescriptorHandle handle = { };
  auto info = GetDescriptorHeapInfo(type);
  if (!info->freeHandles.empty())
  {
    handle = info->freeHandles.back();
    if (handle.count == 1)
    {
      info->freeHandles.pop_back();
      return handle;
    }
  }

  auto desc = info->heap->GetDesc();
  if (desc.NumDescriptors <= info->usedIndex)
  {
    DebugBreak();
  }

  handle.count = 1;
  handle.type = type;
  handle.hCpu = info->heap->GetCPUDescriptorHandleForHeapStart();
  handle.hCpu.ptr += info->handleSize * info->usedIndex;
  handle.hGpu.ptr = 0;
  if (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
  {
    handle.hGpu = info->heap->GetGPUDescriptorHandleForHeapStart();
    handle.hGpu.ptr += info->handleSize * info->usedIndex;
  }
  info->usedIndex++;
  return handle;
}

GfxDevice::DescriptorHandle GfxDevice::AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT num)
{
  DescriptorHandle baseHandle = { };
  auto info = GetDescriptorHeapInfo(type);
  std::lock_guard lock(info->mutexHeap);

  if (!info->freeHandles.empty())
  {
    auto handle = info->freeHandles.back();
    if (handle.count == num)
    {
      info->freeHandles.pop_back();
      baseHandle = handle;
      return baseHandle;
    }
  }

  auto desc = info->heap->GetDesc();
  if (desc.NumDescriptors <= info->usedIndex + num)
  {
    DebugBreak();
  }

  baseHandle.count = num;
  baseHandle.type = type;
  baseHandle.hCpu = info->heap->GetCPUDescriptorHandleForHeapStart();
  baseHandle.hCpu.ptr += info->handleSize * info->usedIndex;
  baseHandle.hGpu.ptr = 0;
  if (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
  {
    baseHandle.hGpu = info->heap->GetGPUDescriptorHandleForHeapStart();
    baseHandle.hGpu.ptr += info->handleSize * info->usedIndex;
  }
  info->usedIndex += num;
  return baseHandle;
}

void GfxDevice::DeallocateDescriptor(DescriptorHandle descriptor)
{
  auto info = GetDescriptorHeapInfo(descriptor.type);
  info->freeHandles.push_back(descriptor);
}

GfxDevice::ComPtr<ID3D12DescriptorHeap> GfxDevice::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE type)
{
  auto info = GetDescriptorHeapInfo(type);
  return info->heap;
}

GfxDevice::ComPtr<ID3D12CommandAllocator> GfxDevice::GetD3D12CommandAllocator(int index)
{
  return m_frameInfo[index].commandAllocator;
}

void GfxDevice::ThrowIfFailed(HRESULT hr, const std::string& errorMsg)
{
  if (FAILED(hr))
  {
    OutputDebugStringA(errorMsg.c_str());
    OutputDebugStringA("\n");
    throw std::runtime_error(errorMsg.c_str());
  }
}

void GfxDevice::SelectDevice()
{
  UINT adapterIndex = 0;
  ComPtr<IDXGIAdapter1> adapter;
  while (DXGI_ERROR_NOT_FOUND != m_dxgiFactory->EnumAdapters1(adapterIndex, &adapter))
  {
    DXGI_ADAPTER_DESC1 desc1{};
    adapter->GetDesc1(&desc1);
    ++adapterIndex;
    if (desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
      continue;

    // D3D12は使用可能か
    HRESULT hr = D3D12CreateDevice(
      adapter.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr);
    if (SUCCEEDED(hr))
    {
      adapter.As(&m_adapter);
      break;
    }
  }
}

void GfxDevice::CreateSwapchain(DXGI_FORMAT dxgiFormat)
{
  Win32Application::GetWindowSize(m_width, m_height);
  m_dxgiFormat = dxgiFormat;
  ComPtr<IDXGISwapChain1> swapchain;
  DXGI_SWAP_CHAIN_DESC1 swapchainDesc{
    .Width = UINT(m_width),
    .Height = UINT(m_height),
    .Format = m_dxgiFormat,
    .SampleDesc = {
      .Count = 1,
      .Quality = 0,
    },
    .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
    .BufferCount = BackBufferCount,
    .Scaling = DXGI_SCALING_STRETCH,
    .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
  };
  HWND hwnd = Win32Application::GetHwnd();
  HRESULT hr = m_dxgiFactory->CreateSwapChainForHwnd(
    m_commandQueue.Get(),
    hwnd,
    &swapchainDesc,
    nullptr,
    nullptr,
    &swapchain);
  swapchain.As(&m_swapchain); // IDXGISwapChain4 取得.
  m_dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
}

void GfxDevice::CreateDescriptorHeaps()
{
  D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{
  .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
  .NumDescriptors = 64,
  .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
  .NodeMask = 0,
  };
  D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{
    .Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
    .NumDescriptors = 64,
    .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
    .NodeMask = 0,
  };
  D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{
    .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
    .NumDescriptors = 65536,
    .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    .NodeMask = 0,
  };
  D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc{
    .Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
    .NumDescriptors = 2048,
    .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    .NodeMask = 0,
  };

  HRESULT hr;
  hr = m_d3d12Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvDescriptorHeap.heap));
  m_rtvDescriptorHeap.handleSize = m_d3d12Device->GetDescriptorHandleIncrementSize(rtvHeapDesc.Type);
  ThrowIfFailed(hr, "ID3D12DescriptorHeap(RTV)作成失敗");

  hr = m_d3d12Device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvDescriptorHeap.heap));
  m_dsvDescriptorHeap.handleSize = m_d3d12Device->GetDescriptorHandleIncrementSize(dsvHeapDesc.Type);
  ThrowIfFailed(hr, "ID3D12DescriptorHeap(DSV)作成失敗");

  hr = m_d3d12Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvDescriptorHeap.heap));
  m_srvDescriptorHeap.handleSize = m_d3d12Device->GetDescriptorHandleIncrementSize(srvHeapDesc.Type);
  ThrowIfFailed(hr, "ID3D12DescriptorHeap(SRV)作成失敗");

  hr = m_d3d12Device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&m_samplerDescriptorHeap.heap));
  m_samplerDescriptorHeap.handleSize = m_d3d12Device->GetDescriptorHandleIncrementSize(samplerHeapDesc.Type);
  ThrowIfFailed(hr, "ID3D12DescriptorHeap(Sampler)作成失敗");
}

void GfxDevice::PrepareRenderTargetView()
{
  // スワップチェインイメージへのレンダーターゲットビュー生成
  for (UINT i = 0; i < BackBufferCount; ++i)
  {
    ComPtr<ID3D12Resource1> renderTarget;
    m_swapchain->GetBuffer(i, IID_PPV_ARGS(&renderTarget));

    auto descriptor = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_d3d12Device->CreateRenderTargetView(renderTarget.Get(), nullptr, descriptor.hCpu);

    m_frameInfo[i].rtvDescriptor = descriptor;
    m_frameInfo[i].targetBuffer = renderTarget;
  }
}

void GfxDevice::CreateCommandAllocators()
{
  m_waitFence = CreateEvent(NULL, FALSE, FALSE, NULL);
  HRESULT hr;
  hr = m_d3d12Device->CreateFence(
    0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_frameFence)
  );
  ThrowIfFailed(hr, "CreateFenceに失敗.");

  for (UINT i = 0; i < BackBufferCount; ++i)
  {
    auto& frame = m_frameInfo[i];
    frame.fenceValue = 1;

    hr = m_d3d12Device->CreateCommandAllocator(
      D3D12_COMMAND_LIST_TYPE_DIRECT,
      IID_PPV_ARGS(&frame.commandAllocator)
    );
    ThrowIfFailed(hr, "CreateCommandAllocatorに失敗");
  }
}

void GfxDevice::DestroyCommandAllocators()
{
  m_frameFence.Reset();
  for (UINT i = 0; i < BackBufferCount; ++i)
  {
    auto& frame = m_frameInfo[i];
    frame.commandAllocator.Reset();
  }
}

GfxDevice::DescriptorHeapInfo* GfxDevice::GetDescriptorHeapInfo(D3D12_DESCRIPTOR_HEAP_TYPE type)
{
  switch (type)
  {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: return &m_srvDescriptorHeap;
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: return  &m_samplerDescriptorHeap;
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV: return &m_rtvDescriptorHeap;
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: return &m_dsvDescriptorHeap;
  }
  return nullptr;
}
