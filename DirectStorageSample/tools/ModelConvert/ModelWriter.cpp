#include "ModelConvert.h"
#include <d3d12.h>
#include <d3dx12.h>
#include <DirectXTex.h>

using namespace DirectX;
namespace fs = std::filesystem;
using namespace model;

namespace {
  using namespace Microsoft::WRL;
  ComPtr<IDStorageFactory> gDStorageFactory;
  ComPtr<IDStorageQueue1>  gQueueSystemMemory;
  ComPtr<IDStorageCompressionCodec> gBufferCompression;
}

void Patch(std::ostream& s, std::streampos pos, void const* data, size_t size)
{
  auto oldPos = s.tellp();
  s.seekp(pos);
  s.write(reinterpret_cast<const char*>(data), size);
  s.seekp(oldPos);
}


template<typename T>
void Patch(std::ostream& s, std::streampos pos, const T& value)
{
  Patch(s, pos, &value, sizeof(value));
}

template<typename T>
void Patch(std::ostream& s, std::streampos pos, std::vector<T> const& values)
{
  Patch(s, pos, values.data(), values.size() * sizeof(T));
}

template<typename T>
class Fixup
{
  std::streampos m_pos;
public:
  Fixup() = default;
  Fixup(std::streampos fixupPos) : m_pos(fixupPos) {}
  void Set(std::ostream& stream, T const& value) const
  {
    Patch(stream, m_pos, value);
  }
  void Set(std::ostream& stream, std::streampos value) const
  {
    T t;
    t.OffsetFromRegionStart = static_cast<uint64_t>(value);
    Set(stream, t);
  }
};

template<typename T, typename FIXUP>
Fixup<FIXUP> MakeFixup(std::streampos startPos, T const* src, const FIXUP* fixup)
{
  auto byteSrc = reinterpret_cast<const uint8_t*>(src);
  auto byteField = reinterpret_cast<const uint8_t*>(fixup);
  std::ptrdiff_t offset = byteField - byteSrc;
  if ((offset + sizeof(*fixup)) > sizeof(*src))
  {
    throw std::runtime_error("Fixup outside of src structure");
  }
  std::streampos fixupPos = startPos + offset;
  return Fixup<FIXUP>(fixupPos);
}

template <typename T, typename ... FIXUPS>
std::tuple<Fixup<FIXUPS>...> WriteStruct(std::ostream& out, T const* src, FIXUPS const* ... fixups)
{
  auto startPos = out.tellp();
  out.write(reinterpret_cast<const char*>(src), sizeof(*src));
  return std::make_tuple(MakeFixup(startPos, src, fixups)...);
}


template<typename T>
void WriteArray(std::ostream& s, T const* data, size_t count)
{
  s.write(reinterpret_cast<const char*>(data), sizeof(*data) * count);
}
template<typename CONTAINER>
model::FixedArray<typename CONTAINER::value_type> WriteArray(std::ostream& s, CONTAINER const& data)
{
  auto pos = s.tellp();
  WriteArray(s, data.data(), data.size());
  model::FixedArray<typename CONTAINER::value_type> array;
  array.data.offset = static_cast<uint32_t>(pos);
  return array;
}

static std::streampos PadToAlignment(std::ostream& s, uint64_t alignment)
{
  auto pos = s.tellp();
  if (pos % alignment)
  {
    uint64_t desiredOffset = ((pos / alignment) + 1) * alignment;
    uint64_t padding = desiredOffset - pos;
    while (padding)
    {
      s.put(0);
      --padding;
    }
    return s.tellp();
  } else
  {
    return pos;
  }
}
template<typename CONTAINER>
model::Ptr<typename CONTAINER::value_type> WriteElementAlignedArray(
  std::ostream& s, CONTAINER const& data, uint64_t alignment)
{
  auto pos = PadToAlignment(s, alignment);
  for (auto& element : data)
  {
    s.write(reinterpret_cast<const char*>(&element), sizeof(element));
    PadToAlignment(s, alignment);
  }
  assert(pos % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT == 0);
  model::Ptr<typename CONTAINER::value_type> ptr;
  ptr.offset = static_cast<uint32_t>(pos);
  return ptr;
}

void InitializeDStorage()
{
  constexpr uint32_t NumCompressionThreads = 6;
  HRESULT hr = DStorageCreateCompressionCodec(
    DSTORAGE_COMPRESSION_FORMAT_GDEFLATE,
    NumCompressionThreads,
    IID_PPV_ARGS(&gBufferCompression)
  );
  if (FAILED(hr))
  {
    throw std::runtime_error("DStorageCreateCompressionCodec failed.");
  }
}

std::vector<char> CompressData(model::DataCompressionType type, const void* source, size_t size)
{
  std::vector<char> destBuffer;
  if (type == model::DataCompressionType::None)
  {
    destBuffer.resize(size);
    memcpy(destBuffer.data(), source, size);
  }
  else
  {
    size_t maxSize = gBufferCompression->CompressBufferBound(size);
    destBuffer.resize(maxSize);
    size_t acturalCompressedSize = 0;
    HRESULT hr = gBufferCompression->CompressBuffer(
      source,
      size,
      DSTORAGE_COMPRESSION_BEST_RATIO,
      destBuffer.data(),
      destBuffer.size(),
      &acturalCompressedSize
    );
    if (FAILED(hr))
    {
      throw std::runtime_error("gBufferCompression->CompressBuffer failed.");
    }
    destBuffer.resize(acturalCompressedSize);
  }
  return destBuffer;
}

class ModelWriter
{
public:
  ModelWriter(std::ostream& out, const model::ModelData* modelData)
    : m_out(out), m_modelData(modelData)
  {
  }

  bool Write(model::DataCompressionType compressionType, bool useTextureCompression);

  void WriteTextures();
  GpuRegion WriteTextureRegion(uint32_t currentSubresource, uint32_t numSubresources,
    const std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>& layouts,
    const std::vector<UINT>& numRows,
    const std::vector<UINT64>& rowSizes,
    uint64_t totalBytes,
    std::vector<D3D12_SUBRESOURCE_DATA> const& subresources,
    const std::string& name);
  void WriteTexture(const std::string& name, uint8_t falgs);
  GpuRegion WriteUnstructuredGpuData();
  Region<CpuMetadataHeader> WriteCpuMetadata();
  Region<CpuDataHeader> WriteCpuData();

  std::vector<char> Compress(DataCompressionType type, std::vector<char>& source);
  std::string Compress(DataCompressionType type, std::string source);

  template<typename T, typename C>
  Region<T> WriteRegion(C uncompressedRegion, const char* name)
  {
    auto uncompressedSize = uncompressedRegion.size();
    C compressedRegion;
    auto compression = m_compression;
    if (compression == DataCompressionType::None)
    {
      compressedRegion = std::move(uncompressedRegion);
    } else
    {
      compressedRegion = Compress(m_compression, uncompressedRegion);
      if (compressedRegion.size() > uncompressedSize)
      {
        compression = DataCompressionType::None;
        compressedRegion = std::move(uncompressedRegion);
      }
    }
    Region<T> r;
    r.compressionType = compression;
    r.data.offset = static_cast<uint32_t>(m_out.tellp());
    r.compressedSize = static_cast<uint32_t>(compressedRegion.size());
    r.uncompressedSize = static_cast<uint32_t>(uncompressedSize);
    if (r.compressionType == DataCompressionType::None) { assert(r.compressedSize == r.uncompressedSize); }
    m_out.write(compressedRegion.data(), compressedRegion.size());

    auto logmsg = std::format("{:0>8x} : {} {} --> {}\n", r.data.offset, name, r.uncompressedSize, r.compressedSize);
    std::cout << logmsg;
    return r;
  }
private:
  const uint32_t StagingBufferSize = 64 * 1024 * 1024;
  const DXGI_FORMAT m_texCompressFormat = DXGI_FORMAT_BC3_UNORM;
  std::ostream& m_out;
  model::DataCompressionType m_compression;
  const model::ModelData* m_modelData;
  bool m_useTextureCompression;

  std::streampos m_materialConstantsGpuOffset;
  struct TextureMetadata
  {
    GpuRegion mipmaps;
  };
  std::vector<TextureMetadata> m_textureMetadata;
  std::vector<D3D12_RESOURCE_DESC> m_textureDescs;
  Microsoft::WRL::ComPtr<ID3D12Device> m_d3d12;

};

void ModelWriter::WriteTextures()
{
  for (size_t i = 0; i < m_modelData->textureNames.size(); ++i)
  {
    const auto& textureName = m_modelData->textureNames[i];
    uint8_t flags = 0;
    WriteTexture(textureName, flags);
  }
}

model::GpuRegion ModelWriter::WriteTextureRegion(uint32_t currentSubresource, uint32_t numSubresources, const std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>& layouts, const std::vector<UINT>& numRows, const std::vector<UINT64>& rowSizes, uint64_t totalBytes, std::vector<D3D12_SUBRESOURCE_DATA> const& subresources, const std::string& name)
{
  std::vector<char> data(totalBytes);
  for (auto i = 0u; i < numSubresources; ++i)
  {
    auto const& layout = layouts[i];
    auto const& subresource = subresources[currentSubresource + i];

    D3D12_MEMCPY_DEST memcpyDest{};
    memcpyDest.pData = data.data() + layout.Offset;
    memcpyDest.RowPitch = layout.Footprint.RowPitch;
    memcpyDest.SlicePitch = layout.Footprint.RowPitch * numRows[i];

    MemcpySubresource(
      &memcpyDest,
      &subresource,
      static_cast<SIZE_T>(rowSizes[i]),
      numRows[i],
      layout.Footprint.Depth);
  }

  return WriteRegion<void>(data, name.c_str());
}

void ModelWriter::WriteTexture(const std::string& name, uint8_t flags)
{
  auto itr = std::find(m_modelData->textureNames.begin(), m_modelData->textureNames.end(), name);
  auto index = uint32_t(std::distance(m_modelData->textureNames.begin(), itr));
  auto imageData = m_modelData->textureImages[index];
  std::unique_ptr<DirectX::ScratchImage> image(new DirectX::ScratchImage());
  DDS_FLAGS ddsFlags = DDS_FLAGS_NONE;
  HRESULT hr = DirectX::LoadFromDDSMemory(imageData.data(), imageData.size(), ddsFlags, nullptr, *image);

  if (m_useTextureCompression)
  {
    std::unique_ptr<DirectX::ScratchImage> compressImage(new DirectX::ScratchImage);
    hr = DirectX::Compress(
      image->GetImages(), image->GetImageCount(), image->GetMetadata(),
      m_texCompressFormat, TEX_COMPRESS_DEFAULT, 0.5, *compressImage);
    image.swap(compressImage);
  }

  const auto metadata = image->GetMetadata();
  std::vector<D3D12_SUBRESOURCE_DATA> subresouces;
  hr = PrepareUpload(
    m_d3d12.Get(), image->GetImages(), image->GetImageCount(), image->GetMetadata(), subresouces);
  if (FAILED(hr))
  {
    throw std::runtime_error("Texture preparation failed");
  }

  D3D12_RESOURCE_DESC desc{};
  desc.Width = static_cast<UINT>(metadata.width);
  desc.Height = static_cast<UINT>(metadata.height);
  desc.MipLevels = static_cast<UINT16>(metadata.mipLevels);
  desc.DepthOrArraySize = (metadata.dimension == TEX_DIMENSION_TEXTURE3D)
    ? static_cast<UINT16>(metadata.depth)
    : static_cast<UINT16>(metadata.arraySize);
  desc.Format = metadata.format;
  desc.SampleDesc.Count = 1;
  desc.Dimension = static_cast<D3D12_RESOURCE_DIMENSION>(metadata.dimension);

  auto const totalSubresourceCount = CD3DX12_RESOURCE_DESC(desc).Subresources(m_d3d12.Get());

  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(totalSubresourceCount);
  std::vector<UINT> numRows(totalSubresourceCount);
  std::vector<UINT64> rowSizes(totalSubresourceCount);

  uint64_t totalBytes = 0;
  uint32_t currentSubresource = 0;

  m_d3d12->GetCopyableFootprints(
    &desc,
    currentSubresource,
    totalSubresourceCount - currentSubresource,
    0,
    layouts.data(),
    numRows.data(),
    rowSizes.data(),
    &totalBytes);

  assert(totalBytes < StagingBufferSize);

  std::stringstream regionName;
  regionName << name << " mips " << totalSubresourceCount;
  GpuRegion remainingMipsRegion = WriteTextureRegion(
    0,
    totalSubresourceCount,
    layouts,
    numRows, rowSizes, totalBytes, subresouces, regionName.str()
  );

  TextureMetadata textureMetadata{};
  textureMetadata.mipmaps = remainingMipsRegion;
  m_textureMetadata.push_back(std::move(textureMetadata));
  m_textureDescs.push_back(desc);
}

model::GpuRegion ModelWriter::WriteUnstructuredGpuData()
{
  std::stringstream s;
  WriteArray(s, m_modelData->geometryData);
  m_materialConstantsGpuOffset = WriteElementAlignedArray(
    s,
    m_modelData->materialConstants,
    D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT
  ).offset;
  return WriteRegion<void>(s.str(), "GPU Data");
}

model::Region<model::CpuMetadataHeader> ModelWriter::WriteCpuMetadata()
{
  std::stringstream s;
  CpuMetadataHeader header{};
  auto [fixupHeader] = WriteStruct(s, &header, &header);
  header.numTextures = static_cast<uint32_t>(m_modelData->textureNames.size());

  std::vector<model::TextureMetadata> textureMetadata;
  textureMetadata.reserve(m_textureMetadata.size());
  assert(m_textureMetadata.size() == m_modelData->textureNames.size());
  for (size_t i = 0; i < m_textureMetadata.size(); ++i)
  {
    model::TextureMetadata metadata{};
    metadata.name = WriteArray(s, m_modelData->textureNames[i]).data;
    s.put(0); // for nullterminate.

    metadata.mipmap = m_textureMetadata[i].mipmaps;
    textureMetadata.push_back(metadata);
  }
  header.textures = WriteArray(s, textureMetadata);
  header.textureDescs = WriteArray(s, m_textureDescs);
  header.numMaterials = static_cast<uint32_t>(m_modelData->materialConstants.size());

  fixupHeader.Set(s, header);
  return WriteRegion<CpuMetadataHeader>(s.str(), "CPU Metadata");
}

model::Region<model::CpuDataHeader> ModelWriter::WriteCpuData()
{
  std::stringstream s;
  model::CpuDataHeader header{};
  auto [fixupHeader] = WriteStruct(s, &header, &header);
  header.numSceneGraphNodes = static_cast<uint32_t>(m_modelData->sceneGraph.size());
  header.sceneGraph = WriteArray(s, m_modelData->sceneGraph);

  header.numMeshes = static_cast<uint32_t>(m_modelData->meshes.size());
  header.meshes.offset = static_cast<uint32_t>(s.tellp());
  for (size_t i = 0; i < m_modelData->meshes.size(); ++i)
  {
    const Mesh* mesh = m_modelData->meshes[i];
    s.write(reinterpret_cast<const char*>(mesh), sizeof(Mesh));
  }

  header.materialConstantsGpuOffset = static_cast<uint32_t>(m_materialConstantsGpuOffset);
  assert(m_modelData->materialConstants.size() == m_modelData->materialTextures.size());
  header.materials.data.offset = static_cast<uint32_t>(s.tellp());

  for (auto& materialTextureData : m_modelData->materialTextures)
  {
    model::MaterialTextureData m{};
    for (size_t i = 0; i < _countof(m.stringIdx); ++i)
    {
      m.stringIdx[i] = materialTextureData.stringIdx[i];
    }
    m.addressModes = materialTextureData.addressModes;
    WriteStruct(s, &m);
  }
  fixupHeader.Set(s, header);
  return WriteRegion<CpuDataHeader>(s.str(), "CPU Data");
}

std::vector<char> ModelWriter::Compress(DataCompressionType type, std::vector<char>& source)
{
  if (type == DataCompressionType::None) { return source; }
  std::vector<char> dest = ::CompressData(type, source.data(), source.size());
  return dest;
}

std::string ModelWriter::Compress(DataCompressionType type, std::string source)
{
  if (type == DataCompressionType::None) { return source; }
  auto temp = ::CompressData(type, source.data(), source.size());
  std::string ret;
  ret.resize(temp.size());
  memcpy(ret.data(), temp.data(), temp.size());
  return ret;
}


bool ModelWriter::Write(model::DataCompressionType compressionType, bool useTextureCompression)
{
  D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_d3d12));
  m_compression = compressionType;
  m_useTextureCompression = useTextureCompression;

  Header header{};
  header.Id[0] = 'T';
  header.Id[1] = 'P';
  header.Id[2] = 'A';
  header.Id[3] = 'K';
  header.Version = 0xFFFE;
  auto [fixupHeader] = WriteStruct(m_out, &header, &header);
  WriteTextures();
  header.unstructuredGpuData = WriteUnstructuredGpuData();
  header.cpuMetadata = WriteCpuMetadata();
  header.cpuData = WriteCpuData();
  header.aabbMin = m_modelData->aabbMin;
  header.aabbMax = m_modelData->aabbMax;

  fixupHeader.Set(m_out, header);

  return true;
}

bool WriteModelData(const model::ModelData* modelData, CompressType compressType, fs::path outputFilePath)
{
  if (!modelData)
  {
    return false;
  }

  InitializeDStorage();

  std::ofstream outfile(outputFilePath, std::ios::out | std::ios::trunc | std::ios::binary);
  model::DataCompressionType compressionType = model::DataCompressionType::GDeflate;

  if (compressType == CompressType::Uncompress)
  {
    compressionType = model::DataCompressionType::None;
  }
  bool useTexCompress = false;
  if (compressType == CompressType::TexCompress)
  {
    useTexCompress = true;
  }

  ModelWriter modelWriter(outfile, modelData);
  return modelWriter.Write(compressionType, useTexCompress);
}
