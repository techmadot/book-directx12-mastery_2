#pragma once
#include <filesystem>
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <iostream>
#include <functional>

#include <wrl/client.h>
#include <wrl/event.h>

#include <DirectXMath.h>
#include "GfxDevice.h"
#include "EventWait.h"
#include "DStorageLoader.h"

namespace model
{
  namespace fs = std::filesystem;
  using DirectX::XMFLOAT4X4;
  using DirectX::XMFLOAT3;
  using DirectX::XMFLOAT4;

  template<typename T>
  union Ptr
  {
    uint64_t offset;
    T* ptr;
  };
  template<typename T>
  struct FixedArray
  {
    Ptr<T> data;
    T& operator[](size_t index)
    {
      return data.ptr[index];
    }
    T const& operator[](size_t index) const
    {
      return data.ptr[index];
    }
  };
  enum class DataCompressionType : uint32_t
  {
    None = 0,
    GDeflate = 1,
  };

  template<typename T>
  struct Region
  {
    DataCompressionType compressionType;
    uint32_t reserved = 0;
    Ptr<T> data;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
  };
  using GpuRegion = Region<void>;

  template<typename T>
  class MemoryRegion
  {
    std::unique_ptr<char[]> m_buffer;
  public:
    MemoryRegion() = default;
    MemoryRegion(std::unique_ptr<char[]> buffer) : m_buffer(std::move(buffer)) {}
    char* Data() { return m_buffer.get(); }
    T* Get()
    {
      return reinterpret_cast<T*>(m_buffer.get());
    }
    const T* Get() const
    {
      return reinterpret_cast<const T*>(m_buffer.get());
    }
    T* operator->()
    {
      return reinterpret_cast<T*>(m_buffer.get());
    }
    T const* operator->() const
    {
      return reinterpret_cast<const T*>(m_buffer.get());
    }
  };


  struct DefaultTextures
  {
    enum {
      kDefaultWhite = 0, kDefaultFlatNormal, kNumTextures
    };
    Microsoft::WRL::ComPtr<ID3D12Resource1> defaultTextures[kNumTextures];
    D3D12_CPU_DESCRIPTOR_HANDLE descriptors[kNumTextures];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_descriptorHeap;

    GfxDevice::DescriptorHandle defaultSampler;
    static void Initialize();
  };

  enum { kBaseColor, kMetallicRoughness, kNormal, kEmissive, kNumTextures };
  enum DrawMode : uint8_t
  {
    DrawModeUnknown = 0,
    DrawModeOpaque,
    DrawModeMask,
    DrawModeBlend,
  };
  struct Mesh
  {
    uint32_t vbOffset;
    uint32_t vbSize;
    uint32_t ibOffset;
    uint32_t ibSize;
    uint8_t  vbStride;
    uint16_t meshCBV;
    uint16_t materialCBV;
    uint16_t srvTable;
    uint16_t samplerTable;
    DrawMode drawMode;
    struct Draw
    {
      uint32_t primitiveCount;
      uint32_t startIndex;
      uint32_t baseVertex;
    } draw;

    XMFLOAT3 aabbMin, aabbMax;
  };
  struct GraphNode
  {
    XMFLOAT4X4 xform;
    //XMFLOAT4 rotation;
    //XMFLOAT3 scale;
    XMFLOAT4X4 worldTransform;

    uint32_t matrixIdx;
    uint32_t nodeNameIndex;
    uint32_t parentIndex;
  };

  __declspec(align(256))
  struct MaterialConstantData
  {
    float baseColorFactor[4];
    float normalTextureScale;
    float metallicFactor;
    float roughnessFactor;
    float alphaCutoff;
    float emissiveFactor[3];
    uint32_t flags;
  };
  struct MaterialTextureData
  {
    uint16_t stringIdx[kNumTextures];
    uint32_t addressModes;
  };

  struct TextureMetadata
  {
    Ptr<char> name;
    GpuRegion mipmap;
  };
  struct CpuMetadataHeader
  {
    uint32_t numTextures;
    FixedArray<TextureMetadata> textures;
    FixedArray<D3D12_RESOURCE_DESC> textureDescs;

    uint32_t numMaterials;
  };

  struct CpuDataHeader
  {
    uint32_t numSceneGraphNodes;
    FixedArray<GraphNode> sceneGraph;
    uint32_t numMeshes;
    Ptr<uint8_t> meshes;
    
    uint32_t materialConstantsGpuOffset;
    FixedArray<MaterialTextureData> materials;
  };
  struct Header
  {
    char Id[4];
    uint16_t Version;
    GpuRegion unstructuredGpuData;
    Region<struct CpuMetadataHeader> cpuMetadata;
    Region<struct CpuDataHeader> cpuData;

    DirectX::XMFLOAT3 aabbMin, aabbMax{1.0f,1.0f,1.0f};
  };

  struct ModelData
  {
    std::vector<byte>   geometryData;
    std::vector<Mesh*>  meshes;
    std::vector<GraphNode> sceneGraph;
    std::vector<uint8_t> textureOptions;
    std::vector<MaterialTextureData> materialTextures;
    std::vector<MaterialConstantData> materialConstants;
    std::vector<std::string> nodeNames;
    std::vector<std::string> textureNames;
    
    std::vector<std::vector<byte>> textureImages;

    XMFLOAT3 aabbMin, aabbMax;
  };

 
  class SimpleModel
  {
    template<typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;
    using Buffer = ComPtr<ID3D12Resource1>;

  public:
    SimpleModel();
    ~SimpleModel();

    // --------------------------------
    // ロード系.
    // --------------------------------
    // DirectStorage経由でデータをロード.
    bool RequestLoad(std::filesystem::path filePath);
    bool RequestLoadHeaderOnly(std::filesystem::path filePath);


    bool IsFinishLoading();
    bool IsRenderingPrepared();

    // --------------------------------
    // 描画系.
    // --------------------------------
    // 行列データの更新.
    void UpdateMatrices(DirectX::XMMATRIX transform);
    // 行列データを GPU (VRAM) へコピー転送.
    void SubmitMatrices(ComPtr<ID3D12GraphicsCommandList> commandList);

    // 描画コマンドの発行.
    void Draw(ComPtr<ID3D12GraphicsCommandList> commandList, DrawMode drawMode);

    void GetModelAABB(DirectX::XMFLOAT3& aabbMin, DirectX::XMFLOAT3& aabbMax);

    // サンプル用 固有データなど.
    DirectX::XMVECTOR m_tumbleAxis;
    float m_tumbleAngle = 0.0f;

    void SetLoadingCompleteCallback(std::function<void(SimpleModel*)> callback);

    struct DataSizeProperty
    {
      size_t texturesByteCount;
      size_t buffersByteCount;
      size_t cpuByteCount;
      size_t GDeflateByteCount;
      size_t uncompressedByteCount;
    } m_dataSizeProperty;
  private:
    void CreateRenderingData();
    
    __declspec(align(256)) struct MeshConstants
    {
      XMFLOAT4X4 mtxWorld;
    };
    // 描画用メッシュの情報.
    struct MeshInstance
    {
      uint32_t vbOffset;
      uint32_t vbSize;
      uint32_t ibOffset;
      uint32_t ibSize;
      uint8_t  vbStride;

      struct Draw
      {
        uint32_t primitiveCount;
        uint32_t startIndex;
        uint32_t baseVertex;
      } draw;

      D3D12_GPU_VIRTUAL_ADDRESS meshCBV;
      D3D12_GPU_VIRTUAL_ADDRESS materialCBV;

      GfxDevice::DescriptorHandle textureHandles;
      GfxDevice::DescriptorHandle samplerHandles;

      DrawMode drawMode;
    };
    struct Texture
    {
      ComPtr<ID3D12Resource1> resource;
      D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptor;
    };
    std::vector<MeshInstance> m_meshes;
    std::vector<GraphNode>    m_sceneGraph;
    std::vector<std::string>  m_nodeNames;
    std::vector<std::string>  m_textureNames;
    std::vector<Texture>      m_textureImages;
    std::vector<GfxDevice::DescriptorHandle> m_srvTables;

    ComPtr<ID3D12Heap> m_localHeap;
    ComPtr<ID3D12DescriptorHeap> m_localDescriptorHeap;
    std::vector<D3D12_RESOURCE_ALLOCATION_INFO1> m_textureAllocationInfos;
    D3D12_RESOURCE_ALLOCATION_INFO m_overallTextureAllocationInfo;
    std::atomic<bool> m_isRenderingPrepared = false;
    bool m_isPrepareAllocationMode = false;

    // モデルデータ関連.
    Header m_header = { };
    MemoryRegion<CpuMetadataHeader> m_cpuMetadata;  // CPUデータに関するメタデータ情報.
    MemoryRegion<CpuDataHeader>     m_cpuData;
    MemoryRegion<void> m_gpuUnstructuredData;

    Buffer m_meshConstantsCPU;
    Buffer m_meshConstantsGPU;
    Buffer m_materialConstants;
    Buffer m_gpuBufferBlock;

    // ローディング情報.
    template<typename T>
    void EnqueueRead(uint64_t offset, T* dest);
    template<typename T>
    MemoryRegion<T> EnqueueReadMemoryRegion(model::Region<T>const& region);
    Buffer EnqueueReadBufferRegion(ID3D12Heap* heap, uint64_t offset, const model::GpuRegion& region);
    ComPtr<ID3D12Resource1> EnqueueReadTexture(ID3D12Heap* heap, uint64_t offset, const D3D12_RESOURCE_DESC& desc, const model::TextureMetadata& textureMetadata);

    Buffer EnqueueReadBufferRegion(ID3D12Heap* heap, const D3D12_RESOURCE_ALLOCATION_INFO1& allocationInfo, const model::GpuRegion& region);

    ComPtr<IDStorageFile> m_file;
    ComPtr<IDStorageStatusArray> m_statusArray;
    EventWait m_ewHeaderLoaded;
    EventWait m_ewCpuMetadataLoaded;
    EventWait m_ewCpuDataLoaded;
    EventWait m_ewGpuDataLoaded;

    enum DStorageStatusEntry : uint32_t
    {
      Metadata,
      CpuData,
      GpuData,
      NumEntries,
    };
    void OnHeaderLoaded();
    void OnCpuMetadataLoaded();
    void OnCpuDataLoaded();
    void OnGpuDataLoaded();
    void OnAllDataLoaded();

    std::mutex m_mutex;
    std::atomic<bool> m_isMeatadataLoaded = false;
    std::atomic<bool> m_isCpuDataLoaded = false;
    std::atomic<bool> m_isGpuDataLoaded = false;

    std::function<void(SimpleModel*)> m_callbackLoadingComplete;
  };
}
