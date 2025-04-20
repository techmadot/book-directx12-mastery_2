#include "DStorageLoader.h"
#include <exception>
#include <stdexcept>


static std::unique_ptr<DirectStorageLoader> gDStorage;
std::unique_ptr<DirectStorageLoader>& GetDStorageLoader()
{
  if (gDStorage == nullptr)
  {
    gDStorage = std::make_unique<DirectStorageLoader>();
  }
  return gDStorage;
}


void DirectStorageLoader::Initialize(ID3D12Device* d3d12Device)
{
  HRESULT hr;
  DSTORAGE_CONFIGURATION config{};
  config.DisableGpuDecompression = false;
  config.DisableBypassIO = false;
  hr = DStorageSetConfiguration(&config);
  if (FAILED(hr))
  {
    throw std::runtime_error("DStorageSetConfiguration failed.");
  }
  hr = DStorageGetFactory(IID_PPV_ARGS(&m_dsFactory));
  if (FAILED(hr))
  {
    throw std::runtime_error("DStorageGetFactory failed.");
  }
#if _DEBUG
  m_dsFactory->SetDebugFlags(DSTORAGE_DEBUG_BREAK_ON_ERROR | DSTORAGE_DEBUG_SHOW_ERRORS);
#endif
  m_dsFactory->SetStagingBufferSize(128 * 1024 * 1024);

  {
    // システムメモリ転送キューの準備
    DSTORAGE_QUEUE_DESC queueDesc{};
    queueDesc.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
    queueDesc.Priority = DSTORAGE_PRIORITY_NORMAL;
    queueDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    queueDesc.Name = "DStorage_QueueSystemMemory";
    hr = m_dsFactory->CreateQueue(&queueDesc, IID_PPV_ARGS(&m_dsQueueSystemMemory));
    if (FAILED(hr)) {
      throw std::runtime_error("CreateQueue failed.");
    }
  }
  {
    // GPUメモリ転送キューの準備
    DSTORAGE_QUEUE_DESC queueDesc{};
    queueDesc.Device = d3d12Device;
    queueDesc.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
    queueDesc.Priority = DSTORAGE_PRIORITY_NORMAL;
    queueDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    queueDesc.Name = "DStorage_QueueSystemMemory";
    hr = m_dsFactory->CreateQueue(&queueDesc, IID_PPV_ARGS(&m_dsQueueGpuMemory));
    if (FAILED(hr)) {
      throw std::runtime_error("CreateQueue failed. (GPU)");
    }
  }
}

void DirectStorageLoader::Shutdown()
{
  m_dsQueueGpuMemory.Reset();
  m_dsQueueSystemMemory.Reset();
  m_dsFactory.Reset();
}

std::unique_ptr<DirectStorageHandle> DirectStorageLoader::CreateHandle(std::filesystem::path filePath, uint32_t statusCount)
{
  auto handle = std::make_unique<DirectStorageHandle>();
  HRESULT hr;
  hr = m_dsFactory->OpenFile(filePath.wstring().c_str(), IID_PPV_ARGS(&handle->file));
  if (FAILED(hr))
  {
    return handle;
  }
  hr = m_dsFactory->CreateStatusArray(statusCount, nullptr, IID_PPV_ARGS(&handle->statusArray));
  if (FAILED(hr))
  {
    return handle;
  }
  return handle;
}

void DirectStorageLoader::CloseHandle(std::unique_ptr<DirectStorageHandle>& handle)
{
  handle->file.Reset();
  handle->statusArray.Reset();

  auto mask = 0xFFFFFFFFFFFFll;
  auto tag = reinterpret_cast<uint64_t>(handle.get());
  m_dsQueueSystemMemory->CancelRequestsWithTag(mask, tag);
  m_dsQueueGpuMemory->CancelRequestsWithTag(mask, tag);
}

void DirectStorageHandle::EnqueueReadCore(void* dest, uint64_t offset, uint32_t size)
{
  DSTORAGE_REQUEST r{};
  r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
  r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
  r.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
  r.Source.File.Source = file.Get();
  r.Source.File.Offset = offset;
  r.Source.File.Size = size;
  r.Destination.Memory.Buffer = dest;
  r.Destination.Memory.Size = r.Source.File.Size;
  r.UncompressedSize = r.Destination.Memory.Size;
  r.CancellationTag = reinterpret_cast<uint64_t>(this);
  
  auto queue = GetDStorageLoader()->GetQueueSystemMemory();
  queue->EnqueueRequest(&r);
  queue->Submit();
}
