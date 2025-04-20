#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <fstream>
#include <map>
#include <unordered_map>

#include <d3d12.h>
#include <d3dx12.h>
#include <DirectXMath.h>
#include <DirectXTex.h>

#include "Model.h"

#include "GfxDevice.h"
#include "TextureUtility.h"

#include "DStorageLoader.h"

using model::ModelData;
using namespace DirectX;

static model::DefaultTextures gDefaultTextures;


bool CreateDDSTextureFromMemory(Microsoft::WRL::ComPtr<ID3D12Resource1>& outImage, const void* srcBuffer, size_t bufferSize, D3D12_RESOURCE_STATES afterState, D3D12_RESOURCE_FLAGS resFlags = D3D12_RESOURCE_FLAG_NONE)
{
  auto& gfxDevice = GetGfxDevice();
  using namespace std;

  DirectX::ScratchImage image;
  DirectX::TexMetadata  metadata;
  DirectX::DDS_FLAGS ddsFlags = DDS_FLAGS_NONE;

  HRESULT hr = DirectX::LoadFromDDSMemory(reinterpret_cast<const uint8_t*>(srcBuffer), bufferSize, ddsFlags, &metadata, image);
  if (FAILED(hr))
  {
    return false;
  }

  auto d3d12Device = gfxDevice->GetD3D12Device();
  Microsoft::WRL::ComPtr<ID3D12Resource> texture;
  DirectX::CREATETEX_FLAGS createFlags = DirectX::CREATETEX_DEFAULT;
  hr = DirectX::CreateTextureEx(d3d12Device.Get(), metadata, resFlags, createFlags, &texture);
  if (FAILED(hr))
  {
    return false;
  }

  std::vector<D3D12_SUBRESOURCE_DATA> subresources;
  DirectX::PrepareUpload(d3d12Device.Get(), image.GetImages(), image.GetImageCount(), metadata, subresources);
  auto uploadBufferSize = GetRequiredIntermediateSize(texture.Get(), 0, UINT(subresources.size()));

  // アップロードヒープの準備.
  CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
  auto uploadResDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
  ID3D12Resource1* uploadHeap = nullptr;
  hr = d3d12Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &uploadResDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadHeap));
  if (FAILED(hr))
  {
    return false;
  }

  auto commandList = gfxDevice->CreateCommandList();
  UpdateSubresources(commandList.Get(), texture.Get(), uploadHeap, 0, 0, UINT(subresources.size()), subresources.data());

  // シェーダーリソースへ変更.
  auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
    texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, afterState
  );
  commandList->ResourceBarrier(1, &barrier);
  commandList->Close();

  gfxDevice->Submit(commandList.Get());
  gfxDevice->WaitForGPU();
  uploadHeap->Release();
  texture.As(&outImage);
  return true;
}

Microsoft::WRL::ComPtr<ID3D12Resource1> CreateTextureFromScratchImage(DirectX::ScratchImage& image)
{
  auto& gfxDevice = GetGfxDevice();
  auto d3d12Device = gfxDevice->GetD3D12Device();
  Microsoft::WRL::ComPtr<ID3D12Resource> texture;

  DirectX::CREATETEX_FLAGS createFlags = DirectX::CREATETEX_DEFAULT;
  auto metadata = image.GetMetadata();
  HRESULT hr = DirectX::CreateTextureEx(d3d12Device.Get(), metadata, D3D12_RESOURCE_FLAG_NONE, createFlags, &texture);
  if (FAILED(hr))
  {
    return nullptr;
  }
  std::vector<D3D12_SUBRESOURCE_DATA> subresources;
  DirectX::PrepareUpload(d3d12Device.Get(), image.GetImages(), image.GetImageCount(), metadata, subresources);
  auto uploadBufferSize = GetRequiredIntermediateSize(texture.Get(), 0, UINT(subresources.size()));
  // アップロードヒープの準備.
  CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
  auto uploadResDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
  ID3D12Resource1* uploadHeap = nullptr;
  hr = d3d12Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &uploadResDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadHeap));
  if (FAILED(hr))
  {
    return nullptr;
  }

  auto commandList = gfxDevice->CreateCommandList();
  UpdateSubresources(commandList.Get(), texture.Get(), uploadHeap, 0, 0, UINT(subresources.size()), subresources.data());

  // シェーダーリソースへ変更.
  auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
    texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
  );
  commandList->ResourceBarrier(1, &barrier);
  commandList->Close();
  gfxDevice->Submit(commandList.Get());
  gfxDevice->WaitForGPU();
  uploadHeap->Release();
  
  Microsoft::WRL::ComPtr<ID3D12Resource1> ret;
  texture.As(&ret);
  return ret;

}

struct TextureSamplerInfo
{
  std::string fileName;
  uint32_t    mappingModeU, mappingModeV;
  D3D12_FILTER filter;
  uint32_t    addressMode;
};

template<class T>
T toAlign(T value, T align)
{
  if ((value % align) == 0) { return value; }
  return ((value / align) + 1) * align;
}

void model::DefaultTextures::Initialize()
{
  auto& gfxDevice = GetGfxDevice();
  auto d3d12Device = gfxDevice->GetD3D12Device();

  D3D12_DESCRIPTOR_HEAP_DESC desc{};
  desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  desc.NumDescriptors = 3;
  d3d12Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&gDefaultTextures.m_descriptorHeap));
  auto incrementSizeSRV = d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);


 auto cpuDescriptor = gDefaultTextures.m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Texture2D.MipLevels = 1;

  ScratchImage imageWork;
  imageWork.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1);
  uint32_t whitePixel = 0xFFFFFFFFu;
  memcpy(imageWork.GetPixels(), &whitePixel, sizeof(whitePixel));
  auto resTexture = CreateTextureFromScratchImage(imageWork);
  gDefaultTextures.defaultTextures[kDefaultWhite] = resTexture;
  resTexture->SetName(L"DefaultWhite");
   
  d3d12Device->CreateShaderResourceView(resTexture.Get(), &srvDesc, cpuDescriptor);
  gDefaultTextures.descriptors[kDefaultWhite] = cpuDescriptor;

  cpuDescriptor.ptr += incrementSizeSRV;

  uint32_t flatNormalPixel = 0x00FF8080;
  memcpy(imageWork.GetPixels(), &flatNormalPixel, sizeof(flatNormalPixel));
  resTexture = CreateTextureFromScratchImage(imageWork);
  gDefaultTextures.defaultTextures[kDefaultFlatNormal] = resTexture;
  d3d12Device->CreateShaderResourceView(resTexture.Get(), &srvDesc, cpuDescriptor);
  gDefaultTextures.descriptors[kDefaultFlatNormal] = cpuDescriptor;
  resTexture->SetName(L"DefaultFlatNormal");

  D3D12_SAMPLER_DESC samplerDesc{};
  samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  samplerDesc.MaxLOD = FLT_MAX;
  gDefaultTextures.defaultSampler = gfxDevice->CreateSampler(samplerDesc);
}

template<typename T1, typename T2>
void Fixup(model::MemoryRegion<T1>& region, model::Ptr<T2>& ptr)
{
  auto offset = ptr.offset;
  char* data = region.Data() + offset;
  ptr.ptr = reinterpret_cast<T2*>(data);
}

DSTORAGE_COMPRESSION_FORMAT ToCompressionFormat(model::DataCompressionType type)
{
  if (type == model::DataCompressionType::None)
  {
    return DSTORAGE_COMPRESSION_FORMAT_NONE;
  }
  if (type == model::DataCompressionType::GDeflate)
  {
    return DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
  }
  throw std::runtime_error("Unknown Compresstion Type");
}

model::SimpleModel::SimpleModel()
{
  m_ewHeaderLoaded.Init<SimpleModel, &SimpleModel::OnHeaderLoaded>(this);
  m_ewCpuMetadataLoaded.Init<SimpleModel, &SimpleModel::OnCpuMetadataLoaded>(this);
  m_ewCpuDataLoaded.Init<SimpleModel, &SimpleModel::OnCpuDataLoaded>(this);
  m_ewGpuDataLoaded.Init<SimpleModel, &SimpleModel::OnGpuDataLoaded>(this);
}

model::SimpleModel::~SimpleModel()
{
  auto& gfxDevice = GetGfxDevice();
  auto  d3d12Device = gfxDevice->GetD3D12Device();

  if (m_isRenderingPrepared)
  {
    for (auto handle : m_srvTables)
    {
      gfxDevice->DeallocateDescriptor(handle);
    }
    m_srvTables.clear();
  }
}

template<typename T>
void model::SimpleModel::EnqueueRead(uint64_t offset, T* dest)
{
  DSTORAGE_REQUEST r{};
  r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
  r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
  r.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
  r.Source.File.Source = m_file.Get();
  r.Source.File.Offset = offset;
  r.Source.File.Size = static_cast<uint32_t>(sizeof(T));
  r.Destination.Memory.Buffer = dest;
  r.Destination.Memory.Size = r.Source.File.Size;
  r.UncompressedSize = r.Destination.Memory.Size;
  r.CancellationTag = reinterpret_cast<uint64_t>(this);

  auto queue = GetDStorageLoader()->GetQueueSystemMemory();
  queue->EnqueueRequest(&r);
}
template<typename T>
model::MemoryRegion<T> model::SimpleModel::EnqueueReadMemoryRegion(model::Region<T>const& region)
{
  MemoryRegion<T> dest(std::make_unique<char[]>(region.uncompressedSize));
  DSTORAGE_REQUEST r{};

  r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
  r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
  r.Options.CompressionFormat = ToCompressionFormat(region.compressionType);
  r.Source.File.Source = m_file.Get();
  r.Source.File.Offset = region.data.offset;
  r.Source.File.Size = region.compressedSize;
  r.Destination.Memory.Buffer = dest.Data();
  r.Destination.Memory.Size = region.uncompressedSize;
  r.UncompressedSize = r.Destination.Memory.Size;
  r.CancellationTag = reinterpret_cast<uint64_t>(this);

  auto queue = GetDStorageLoader()->GetQueueSystemMemory();
  queue->EnqueueRequest(&r);
  return dest;
}

model::SimpleModel::Buffer model::SimpleModel::EnqueueReadBufferRegion(ID3D12Heap* heap, uint64_t offset, const model::GpuRegion& region)
{
  auto queue = GetDStorageLoader()->GetQueueGpuMemory();
  auto& gfxDevice = GetGfxDevice();
  ComPtr<ID3D12Resource1> resource;

  auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(region.uncompressedSize);
  resource = gfxDevice->CreateBuffer(bufferDesc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
  DSTORAGE_REQUEST r{};
  r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
  r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_BUFFER;
  r.Options.CompressionFormat = ToCompressionFormat(region.compressionType);
  r.Source.File.Source = m_file.Get();
  r.Source.File.Offset = region.data.offset;
  r.Source.File.Size = region.compressedSize;
  r.Destination.Buffer.Offset = 0;
  r.Destination.Buffer.Resource = resource.Get();
  r.Destination.Buffer.Size = region.uncompressedSize;
  r.UncompressedSize = r.Destination.Buffer.Size;
  r.CancellationTag = reinterpret_cast<uint64_t>(this);

  queue->EnqueueRequest(&r);
  return resource;
}

model::SimpleModel::Buffer model::SimpleModel::EnqueueReadBufferRegion(ID3D12Heap* heap, const D3D12_RESOURCE_ALLOCATION_INFO1& allocationInfo, const model::GpuRegion& region)
{
  auto queue = GetDStorageLoader()->GetQueueGpuMemory();
  auto& gfxDevice = GetGfxDevice();
  auto d3d12Device = gfxDevice->GetD3D12Device();
  ComPtr<ID3D12Resource1> resource;
  
  auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(region.uncompressedSize);
  d3d12Device->CreatePlacedResource(
    heap,
    allocationInfo.Offset,
    &bufferDesc,
    D3D12_RESOURCE_STATE_COMMON,
    nullptr,
    IID_PPV_ARGS(&resource)
  );
  DSTORAGE_REQUEST r{};
  r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
  r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_BUFFER;
  r.Options.CompressionFormat = ToCompressionFormat(region.compressionType);
  r.Source.File.Source = m_file.Get();
  r.Source.File.Offset = region.data.offset;
  r.Source.File.Size = region.compressedSize;
  r.Destination.Buffer.Offset = 0;
  r.Destination.Buffer.Resource = resource.Get();
  r.Destination.Buffer.Size = region.uncompressedSize;
  r.UncompressedSize = r.Destination.Buffer.Size;
  r.CancellationTag = reinterpret_cast<uint64_t>(this);

  queue->EnqueueRequest(&r);
  return resource;
}


Microsoft::WRL::ComPtr<ID3D12Resource1> model::SimpleModel::EnqueueReadTexture(ID3D12Heap* heap, uint64_t offset, const D3D12_RESOURCE_DESC& desc, const model::TextureMetadata& textureMetadata)
{
  auto queue = GetDStorageLoader()->GetQueueGpuMemory();
  auto& gfxDevice = GetGfxDevice();
  auto d3d12Device = gfxDevice->GetD3D12Device();
  ComPtr<ID3D12Resource1> resource;

  d3d12Device->CreatePlacedResource(
    heap,
    offset,
    &desc,
    D3D12_RESOURCE_STATE_COMMON,
    nullptr,
    IID_PPV_ARGS(&resource)
  );

  std::string name = textureMetadata.name.ptr;
  std::wstring wname(name.begin(), name.end());
  resource->SetName(wname.c_str());

  DSTORAGE_REQUEST r{};
  r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
  r.Options.CompressionFormat = ToCompressionFormat(textureMetadata.mipmap.compressionType);
  r.Source.File.Source = m_file.Get();
  r.Source.File.Offset = textureMetadata.mipmap.data.offset;
  r.Source.File.Size = textureMetadata.mipmap.compressedSize;
  r.UncompressedSize = textureMetadata.mipmap.uncompressedSize;
  r.CancellationTag = reinterpret_cast<uint64_t>(this);

  r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES;
  r.Destination.MultipleSubresources.Resource = resource.Get();
  r.Destination.MultipleSubresources.FirstSubresource = 0;

  queue->EnqueueRequest(&r);

  return resource;
}

bool model::SimpleModel::RequestLoad(std::filesystem::path filePath)
{
  auto factory = GetDStorageLoader()->GetFactory();
  factory->CreateStatusArray(DStorageStatusEntry::NumEntries, nullptr, IID_PPV_ARGS(&m_statusArray));
  auto hr = factory->OpenFile(filePath.wstring().c_str(), IID_PPV_ARGS(&m_file));
  if (FAILED(hr))
  {
    return false;
  }
  // DirectStorage経由でデータ読み取りを開始.
  // ファイルのヘッダ情報をシステムメモリへロードするリクエストを発行.
  EnqueueRead(0, &m_header);
  m_ewHeaderLoaded.SetThraedpoolWait();

  auto queue = GetDStorageLoader()->GetQueueSystemMemory();
  queue->EnqueueStatus(m_statusArray.Get(), DStorageStatusEntry::Metadata);
  queue->EnqueueSetEvent(m_ewHeaderLoaded);
  return true;
}

bool model::SimpleModel::RequestLoadHeaderOnly(std::filesystem::path filePath)
{
  std::ifstream infile(filePath, std::ios::binary);
  std::vector<char> workBuffer;
  if (infile)
  {
    workBuffer.resize(sizeof(model::Header));
    infile.read(workBuffer.data(), workBuffer.size());

    const auto* header = reinterpret_cast<const model::Header*>(workBuffer.data());
    const auto unstructuredGpuDataSize = header->unstructuredGpuData.uncompressedSize;

    std::vector<char> decodeBuffer;
    std::vector<D3D12_RESOURCE_DESC> resourceDescs;
    uint32_t textureCount = 0;
    if (header->cpuMetadata.compressionType == model::DataCompressionType::None)
    {
      size_t requestSize = header->cpuMetadata.uncompressedSize;
      decodeBuffer.resize(requestSize);

      auto offset = header->cpuMetadata.data.offset;
      infile.seekg(offset, std::ios::beg).read(decodeBuffer.data(), requestSize);

      auto* metadata = reinterpret_cast<model::CpuMetadataHeader*>(decodeBuffer.data());
      textureCount = metadata->numTextures;
      auto textureDescs = reinterpret_cast<D3D12_RESOURCE_DESC*>(decodeBuffer.data() + metadata->textureDescs.data.offset);
      resourceDescs.assign(textureDescs, textureDescs + textureCount);
    }
    if (header->cpuMetadata.compressionType == model::DataCompressionType::GDeflate)
    {
      decodeBuffer.resize(header->cpuMetadata.uncompressedSize);
      std::vector<char> sourceBuffer(header->cpuMetadata.compressedSize);
      auto offset = header->cpuMetadata.data.offset;
      infile.seekg(offset, std::ios::beg).read(sourceBuffer.data(), sourceBuffer.size());

      constexpr uint32_t NumCodecThread = 6;
      ComPtr<IDStorageCompressionCodec> compressionCodec;
      DStorageCreateCompressionCodec(
        DSTORAGE_COMPRESSION_FORMAT_GDEFLATE, NumCodecThread, IID_PPV_ARGS(&compressionCodec)
      );
      size_t decompressedSize = 0;
      compressionCodec->DecompressBuffer(
        sourceBuffer.data(), sourceBuffer.size(),
        decodeBuffer.data(), decodeBuffer.size(), &decompressedSize);

      auto* metadata = reinterpret_cast<model::CpuMetadataHeader*>(decodeBuffer.data());
      textureCount = metadata->numTextures;
      auto textureDescs = reinterpret_cast<D3D12_RESOURCE_DESC*>(decodeBuffer.data() + metadata->textureDescs.data.offset);
      resourceDescs.assign(textureDescs, textureDescs + metadata->numTextures);
    }

    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(unstructuredGpuDataSize);
    resourceDescs.push_back(bufferDesc);

    ComPtr<ID3D12Device4> device4;
    auto& gfxDevice = GetGfxDevice();
    gfxDevice->GetD3D12Device().As(&device4);

    std::vector<D3D12_RESOURCE_ALLOCATION_INFO1> resourceAllocationInfos(resourceDescs.size());
    D3D12_RESOURCE_ALLOCATION_INFO allocationInfo = device4->GetResourceAllocationInfo1(
      0,
      UINT(resourceDescs.size()),
      resourceDescs.data(),
      resourceAllocationInfos.data()
    );

    m_overallTextureAllocationInfo = allocationInfo;    // 今回用意したバッファも対応するものにセット.
    m_textureAllocationInfos = resourceAllocationInfos; // 今回用意したバッファも対応するものにセット.

    // ヒープを確保済みにしておく.
    D3D12_HEAP_DESC heapDesc{};
    heapDesc.Alignment = allocationInfo.Alignment;
    heapDesc.Flags = D3D12_HEAP_FLAG_NONE;
    heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapDesc.SizeInBytes = allocationInfo.SizeInBytes;
    HRESULT hr = device4->CreateHeap(&heapDesc, IID_PPV_ARGS(&m_localHeap));
    if (FAILED(hr))
    {
      return false;
    }

    {
      D3D12_HEAP_DESC heapDesc{};
      heapDesc.Alignment = 64*1024;// allocationInfo.Alignment;
      heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
      heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
      heapDesc.SizeInBytes = 8*1024*1024;
      ComPtr<ID3D12Heap> sampleHeap;
      HRESULT hr = device4->CreateHeap(&heapDesc, IID_PPV_ARGS(&sampleHeap));
      if (FAILED(hr))
      {
        return false;
      }

      ComPtr<ID3D12Resource> tex;
      auto resDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_BC7_UNORM, 512, 512, 1, 10);
      resDesc.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;

      ComPtr<ID3D12InfoQueue> infoQueue;
      device4.As(&infoQueue);
      if (infoQueue)
      {
        D3D12_MESSAGE_ID denyIds[] = {
            D3D12_MESSAGE_ID_CREATERESOURCE_INVALIDALIGNMENT_SMALLRESOURCE // WARNING #1380 を抑制
        };
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumIDs = _countof(denyIds);
        filter.DenyList.pIDList = denyIds;
        infoQueue->AddStorageFilterEntries(&filter);
      }

      auto info = device4->GetResourceAllocationInfo(0, 1, &resDesc);

      auto resDesc2 = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_BC7_UNORM, 256, 256, 1, 9);
      //resDesc2.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
      std::vector<D3D12_RESOURCE_DESC> resDescs = {
        resDesc, resDesc2,
      };

      D3D12_RESOURCE_ALLOCATION_INFO1 allocInfo1[2] = { 0 };

      info = device4->GetResourceAllocationInfo1(0, UINT(resDescs.size()), resDescs.data(), allocInfo1);


      hr = device4->CreatePlacedResource(
        sampleHeap.Get(),
        16384,
        &resDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&tex)
      );
      if (FAILED(hr))
      {
        return false;
      }

    }


    // ディスクリプタヒープの作成も先に済ませておく.
    D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc{
      .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
      .NumDescriptors = textureCount,
      .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
    };
    hr = device4->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&m_localDescriptorHeap));
    if (FAILED(hr))
    {
      return false;
    }
    m_isPrepareAllocationMode = true;
    return true;
  }
  return false;
}

bool model::SimpleModel::IsFinishLoading()
{
  return m_isCpuDataLoaded && m_isGpuDataLoaded;
}

bool model::SimpleModel::IsRenderingPrepared()
{
  return m_isRenderingPrepared;
}

// ヘッダ部がロード完了後に呼ばれる.
void model::SimpleModel::OnHeaderLoaded()
{
  auto status = m_statusArray->GetHResult(DStorageStatusEntry::Metadata);
  if (FAILED(status))
  {
    // ロードに失敗している.
    return;
  }
  // ヘッダのチェック.
  if (m_header.Version != 0xFFFE)
  {
    return;
  }

  // システムメモリ側にロードしたいメタデータをリクエスト.
  m_cpuMetadata = EnqueueReadMemoryRegion<model::CpuMetadataHeader>(m_header.cpuMetadata);
  m_ewCpuMetadataLoaded.SetThraedpoolWait();
  auto queue = GetDStorageLoader()->GetQueueSystemMemory();
  queue->EnqueueSetEvent(m_ewCpuMetadataLoaded);
}

// CPUデータ部のメタデータロード完了後に呼ばれる.
void model::SimpleModel::OnCpuMetadataLoaded()
{
  // メタデータ領域のセットアップ.
  Fixup(m_cpuMetadata, m_cpuMetadata->textures.data);
  Fixup(m_cpuMetadata, m_cpuMetadata->textureDescs.data);
  for (uint32_t i = 0; i < m_cpuMetadata->numTextures; ++i)
  {
    Fixup(m_cpuMetadata, m_cpuMetadata->textures[i].name);
  }

  // GPU用のリソースを確保するための準備を行う.
  if (!m_isPrepareAllocationMode)
  {
    auto& gfxDevice = GetGfxDevice();
    ComPtr<ID3D12Device4> device4;
    gfxDevice->GetD3D12Device().As(&device4);
    m_textureAllocationInfos.resize(m_cpuMetadata->numTextures);
    m_overallTextureAllocationInfo = device4->GetResourceAllocationInfo1(
      0,
      m_cpuMetadata->numTextures,
      m_cpuMetadata->textureDescs.data.ptr,
      m_textureAllocationInfos.data());

    D3D12_HEAP_DESC heapDesc{
      .SizeInBytes = m_overallTextureAllocationInfo.SizeInBytes,
      .Properties = {.Type = D3D12_HEAP_TYPE_DEFAULT },
      .Alignment = 64 * 1024,
      .Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES,
    };
    device4->CreateHeap(&heapDesc, IID_PPV_ARGS(&m_localHeap));

    D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc{
      .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
      .NumDescriptors = m_cpuMetadata->numTextures,
    };
    device4->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&m_localDescriptorHeap));
  }
  assert(m_localDescriptorHeap.Get() != nullptr);
  assert(m_localHeap.Get() != nullptr);
  m_isMeatadataLoaded = true;

  // 引き続きデータ本体(CPU/GPU)のロードリクエストを発行する.
  {
    // CPU用
    m_cpuData = EnqueueReadMemoryRegion<model::CpuDataHeader>(m_header.cpuData);
    auto queue = GetDStorageLoader()->GetQueueSystemMemory();
    queue->EnqueueStatus(m_statusArray.Get(), DStorageStatusEntry::CpuData);
    m_ewCpuDataLoaded.SetThraedpoolWait();
    queue->EnqueueSetEvent(m_ewCpuDataLoaded);
  }
  {
    // GPU用
    auto queue = GetDStorageLoader()->GetQueueGpuMemory();
    m_textureImages.resize(m_cpuMetadata->numTextures);
    for (uint32_t i = 0; i < m_cpuMetadata->numTextures; ++i)
    {
      m_textureImages[i].resource = EnqueueReadTexture(
        m_localHeap.Get(),
        m_textureAllocationInfos[i].Offset,
        m_cpuMetadata->textureDescs[i],
        m_cpuMetadata->textures[i]
      );
    }
    if (!m_isPrepareAllocationMode)
    {
      m_gpuBufferBlock = EnqueueReadBufferRegion(
        m_localHeap.Get(),
        0,
        m_header.unstructuredGpuData
      );
    }
    else
    {
      m_gpuBufferBlock = EnqueueReadBufferRegion(
        m_localHeap.Get(), m_textureAllocationInfos[m_cpuMetadata->numTextures], m_header.unstructuredGpuData
      );
    }
    queue->EnqueueStatus(m_statusArray.Get(), DStorageStatusEntry::GpuData);
    m_ewGpuDataLoaded.SetThraedpoolWait();
    queue->EnqueueSetEvent(m_ewGpuDataLoaded);
  }

  // サイズ情報を計算.
  m_dataSizeProperty.texturesByteCount = 0;
  m_dataSizeProperty.buffersByteCount = m_header.unstructuredGpuData.uncompressedSize;
  m_dataSizeProperty.cpuByteCount = m_header.cpuData.uncompressedSize;
  m_dataSizeProperty.GDeflateByteCount = 0;
  auto accumulateSize = [&](auto const& region)
    {
      switch (region.compressionType)
      {
      case model::DataCompressionType::None:
        m_dataSizeProperty.uncompressedByteCount = region.uncompressedSize;
        return;
      case model::DataCompressionType::GDeflate:
        m_dataSizeProperty.GDeflateByteCount += region.uncompressedSize;
        break;
      }
    };
  accumulateSize(m_header.unstructuredGpuData);
  accumulateSize(m_header.cpuData);
  m_dataSizeProperty.texturesByteCount = 0;
  for (uint32_t textureIndex = 0; textureIndex < m_cpuMetadata->numTextures; ++textureIndex)
  {
    auto& texture = m_cpuMetadata->textures[textureIndex];
    accumulateSize(texture.mipmap);
    m_dataSizeProperty.texturesByteCount += texture.mipmap.uncompressedSize;
  }
}

// CPU側データのロード完了後に呼ばれる.
void model::SimpleModel::OnCpuDataLoaded()
{
  Fixup(m_cpuData, m_cpuData->sceneGraph.data);
  Fixup(m_cpuData, m_cpuData->meshes);
  Fixup(m_cpuData, m_cpuData->materials.data);
  
  {
    std::unique_lock lock(m_mutex);
    // CPU側データは処理が完了.
    m_isCpuDataLoaded = true;
    if (m_isGpuDataLoaded)
    {
      // GPU側データは既に完了状態のため、最終工程を実行.
      OnAllDataLoaded();
    }
  }
}

// GPU側データのロード完了後に呼ばれる.
void model::SimpleModel::OnGpuDataLoaded()
{
  std::unique_lock lock(m_mutex);
  // GPU側データがロードは完了.
  m_isGpuDataLoaded = true;
  if (m_isCpuDataLoaded)
  {
    // CPU側データは既に完了状態のため、最終工程を実行.
    OnAllDataLoaded();
  }
}

// ロード完了後の最後の工程.
// ロード済みデータからディスクリプタを構築や描画用リソースの準備
void model::SimpleModel::OnAllDataLoaded()
{
  auto resultLoadCPU = m_statusArray->GetHResult(uint32_t(DStorageStatusEntry::CpuData));
  auto resultLoadGPU = m_statusArray->GetHResult(uint32_t(DStorageStatusEntry::GpuData));
  if (FAILED(resultLoadCPU) || FAILED(resultLoadGPU))
  {
    return;
  }

  if (m_callbackLoadingComplete)
  {
    m_callbackLoadingComplete(this);
  }

  // 描画用のデータ構築を実行.
  CreateRenderingData();
}


void model::SimpleModel::SetLoadingCompleteCallback(std::function<void(SimpleModel*)> callback)
{
  m_callbackLoadingComplete = callback;
}

void model::SimpleModel::CreateRenderingData()
{
  // TextureDescriptorを作る.
  auto& gfxDevice = GetGfxDevice();
  auto  d3d12Device = gfxDevice->GetD3D12Device();

  auto increment = gfxDevice->GetD3D12Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE descriptors = m_localDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
  for (uint32_t i = 0; i < m_cpuMetadata->numTextures; ++i)
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = m_cpuMetadata->textureDescs[i].MipLevels;

    d3d12Device->CreateShaderResourceView(
      m_textureImages[i].resource.Get(),
      &srvDesc,
      CD3DX12_CPU_DESCRIPTOR_HANDLE(descriptors, i, increment)
    );
  }
  auto numMaterials = m_cpuMetadata->numMaterials;
  m_srvTables.resize(numMaterials);
  for (uint32_t matIdx = 0; matIdx < numMaterials; ++matIdx)
  {
    const auto& srcMat = m_cpuData->materials[matIdx];
    D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptors[model::kNumTextures] =
    {
      gDefaultTextures.descriptors[DefaultTextures::kDefaultWhite],
      gDefaultTextures.descriptors[DefaultTextures::kDefaultWhite],
      gDefaultTextures.descriptors[DefaultTextures::kDefaultFlatNormal],
      gDefaultTextures.descriptors[DefaultTextures::kDefaultWhite],
    };
    for (uint32_t i = 0; i < model::kNumTextures; ++i)
    {
      if (srcMat.stringIdx[i] != 0xffff)
      {
        cpuDescriptors[i].ptr = descriptors.ptr + increment * srcMat.stringIdx[i];
      }
    }

    // 描画で使用する側のディスクリプタヒープへコピー.
    UINT destCount = model::kNumTextures;
    UINT sourceSizes[] = { 1, 1, 1, 1 };
    auto baseHandle = gfxDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, destCount);
    d3d12Device->CopyDescriptors(
      1, &baseHandle.hCpu, &destCount,
      destCount, cpuDescriptors, sourceSizes, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
    );
    m_srvTables[matIdx] = baseHandle;
  }

  // 定数バッファの支度.
  const auto cpuDataHeader = m_cpuData.Get();
  const auto cpuMetadataHeader = m_cpuMetadata.Get();
  auto meshConstantBufferSize = cpuDataHeader->numSceneGraphNodes * sizeof(MeshConstants);
  auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(meshConstantBufferSize);
  m_meshConstantsCPU = gfxDevice->CreateBuffer(resDesc, D3D12_HEAP_TYPE_UPLOAD);
  m_meshConstantsGPU = gfxDevice->CreateBuffer(resDesc, D3D12_HEAP_TYPE_DEFAULT);

  auto gpuBufferBaseAddress = m_gpuBufferBlock->GetGPUVirtualAddress();
  auto materialConstantsGpuOffset = cpuDataHeader->materialConstantsGpuOffset;
  auto textureCount = cpuMetadataHeader->numTextures;
  auto materialCount = cpuMetadataHeader->numMaterials;
  auto meshCount = cpuDataHeader->numMeshes;

  auto GetMeshPtr = [&](auto index){
    return reinterpret_cast<const model::Mesh*>(m_cpuData.Get()->meshes.ptr)[index];
  };
  // 描画用メッシュ情報の構築.
  for (uint32_t i = 0; i < meshCount; ++i)
  {
    const auto& srcMesh = GetMeshPtr(i);
    auto& dstMesh = m_meshes.emplace_back();
    dstMesh.vbOffset = srcMesh.vbOffset;
    dstMesh.vbSize = srcMesh.vbSize;
    dstMesh.ibOffset = srcMesh.ibOffset;
    dstMesh.ibSize = srcMesh.ibSize;
    dstMesh.vbStride = srcMesh.vbStride;
    dstMesh.draw.primitiveCount = dstMesh.ibSize / sizeof(uint32_t);
    dstMesh.drawMode = srcMesh.drawMode;

    auto materialIndex = srcMesh.materialCBV;
    auto meshIndex = srcMesh.meshCBV;
    dstMesh.meshCBV = m_meshConstantsGPU->GetGPUVirtualAddress() + sizeof(MeshConstants) * meshIndex;
    dstMesh.materialCBV = gpuBufferBaseAddress + materialConstantsGpuOffset + sizeof(MaterialConstantData) * materialIndex;

    dstMesh.textureHandles = m_srvTables[materialIndex];
    dstMesh.samplerHandles = gDefaultTextures.defaultSampler;
  }
  for (uint32_t i = 0; i < cpuDataHeader->numSceneGraphNodes; ++i)
  {
    m_sceneGraph.push_back(cpuDataHeader->sceneGraph[i]);
  }
  UpdateMatrices(XMMatrixIdentity());
  m_isRenderingPrepared = true;
}

void model::SimpleModel::UpdateMatrices(DirectX::XMMATRIX transform)
{
  if (m_sceneGraph.empty()) { return; }
  assert(m_sceneGraph[0].parentIndex == UINT32_MAX);

  for (uint32_t i = 0; i < m_sceneGraph.size(); ++i)
  {
    auto& node = m_sceneGraph[i];
    XMMATRIX mtxLocal = XMLoadFloat4x4(&node.xform);
    XMMATRIX mtxParent = transform;
    if (node.parentIndex != UINT32_MAX)
    {
      const auto& parent = m_sceneGraph[node.parentIndex];
      mtxParent = XMLoadFloat4x4(&parent.worldTransform);
    }
    XMMATRIX mtxWorld = XMMatrixMultiply(mtxLocal, mtxParent);
    XMStoreFloat4x4(&node.worldTransform, mtxWorld);
  }
}

void model::SimpleModel::SubmitMatrices(ComPtr<ID3D12GraphicsCommandList> commandList)
{
  auto& gfxDevice = GetGfxDevice();
  void* p = nullptr;
  m_meshConstantsCPU->Map(0, nullptr, &p);
  if (p)
  {
    MeshConstants* meshConstants = reinterpret_cast<MeshConstants*>(p);
    for (uint32_t i = 0; i < m_sceneGraph.size(); ++i)
    {
      auto mtx = XMLoadFloat4x4(&m_sceneGraph[i].worldTransform);
      XMStoreFloat4x4(&meshConstants[i].mtxWorld, XMMatrixTranspose(mtx));  // 転置して転送.
    }
    m_meshConstantsCPU->Unmap(0, nullptr);
  }
  UINT64 bufferSize = m_meshConstantsCPU->GetDesc().Width;
  D3D12_RESOURCE_BARRIER before = CD3DX12_RESOURCE_BARRIER::Transition(
    m_meshConstantsGPU.Get(), D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_RESOURCE_BARRIER after = CD3DX12_RESOURCE_BARRIER::Transition(
    m_meshConstantsGPU.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
  commandList->ResourceBarrier(1, &before);
  commandList->CopyBufferRegion(m_meshConstantsGPU.Get(), 0, m_meshConstantsCPU.Get(), 0, bufferSize);
  commandList->ResourceBarrier(1, &after);
}

void model::SimpleModel::Draw(ComPtr<ID3D12GraphicsCommandList> commandList, DrawMode drawMode)
{
  for (const auto& mesh : m_meshes)
  {
    if (drawMode != mesh.drawMode)
    {
      continue;
    }
    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = m_gpuBufferBlock->GetGPUVirtualAddress() + mesh.ibOffset;
    ibv.Format = DXGI_FORMAT_R32_UINT;
    ibv.SizeInBytes = mesh.ibSize;
    commandList->IASetIndexBuffer(&ibv);

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = m_gpuBufferBlock->GetGPUVirtualAddress() + mesh.vbOffset;
    vbv.SizeInBytes = mesh.vbSize;
    vbv.StrideInBytes = mesh.vbStride;
    commandList->IASetVertexBuffers(0, 1, &vbv);

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->SetGraphicsRootConstantBufferView(1, mesh.meshCBV);
    commandList->SetGraphicsRootConstantBufferView(2, mesh.materialCBV);
    commandList->SetGraphicsRootDescriptorTable(3, mesh.textureHandles.hGpu);
    commandList->SetGraphicsRootDescriptorTable(4, mesh.samplerHandles.hGpu);

    commandList->DrawIndexedInstanced(mesh.draw.primitiveCount, 1, 0, 0, 0);
  }
}

void model::SimpleModel::GetModelAABB(DirectX::XMFLOAT3& aabbMin, DirectX::XMFLOAT3& aabbMax)
{
  aabbMin = m_header.aabbMin;
  aabbMax = m_header.aabbMax;
}