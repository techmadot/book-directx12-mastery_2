#pragma once
#include <d3d12.h>
#include <dstorage.h>
#include <wrl/client.h>
#include <memory>
#include <filesystem>

class DirectStorageHandle;
class DirectStorageLoader
{
public:
  void Initialize(ID3D12Device* d3d12Device);
  void Shutdown();

  std::unique_ptr<DirectStorageHandle> CreateHandle(std::filesystem::path filePath, uint32_t statusCount);
  void CloseHandle(std::unique_ptr<DirectStorageHandle>& handle);

  Microsoft::WRL::ComPtr<IDStorageFactory> GetFactory() {
    return m_dsFactory;
  }
  Microsoft::WRL::ComPtr<IDStorageQueue1> GetQueueSystemMemory() { return m_dsQueueSystemMemory; }
  Microsoft::WRL::ComPtr<IDStorageQueue1> GetQueueGpuMemory() { return m_dsQueueGpuMemory; }

private:
  Microsoft::WRL::ComPtr<IDStorageFactory> m_dsFactory;
  Microsoft::WRL::ComPtr<IDStorageQueue1> m_dsQueueSystemMemory;
  Microsoft::WRL::ComPtr<IDStorageQueue1> m_dsQueueGpuMemory;

};

class DirectStorageHandle
{
public:
  DirectStorageHandle() = default;

  template<typename T>
  void EnqueueRead(uint64_t offset, T* dest)
  {
    auto size = uint32_t(sizeof(T));
    EnqueueReadCore(dest, offset, size);
  }
private:
  void EnqueueReadCore(void* dest, uint64_t offset, uint32_t size);
  Microsoft::WRL::ComPtr<IDStorageFile> file;
  Microsoft::WRL::ComPtr<IDStorageStatusArray> statusArray;

  friend class DirectStorageLoader;
};

std::unique_ptr<DirectStorageLoader>& GetDStorageLoader();
