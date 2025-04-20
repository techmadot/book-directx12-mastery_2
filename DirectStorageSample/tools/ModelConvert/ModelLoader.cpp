#include "ModelConvert.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/GltfMaterial.h>
#include <assimp/scene.h>

#include <DirectXTex.h>

using namespace DirectX;
namespace fs = std::filesystem;

static XMFLOAT4X4 ConvertMatrix(const aiMatrix4x4& from)
{
  XMFLOAT4X4 to;

  return XMFLOAT4X4(
    from.a1, from.b1, from.c1, from.d1,
    from.a2, from.b2, from.c2, from.d2,
    from.a3, from.b3, from.c3, from.d3,
    from.a4, from.b4, from.c4, from.d4
  );
  return to;
}
template<class T>
T toAlign(T value, T align)
{
  if ((value % align) == 0) { return value; }
  return ((value / align) + 1) * align;
}

static std::vector<std::string> GetTextureImages(const aiScene* scene)
{
  std::vector<std::string> textureList;
  for (uint32_t m = 0; m < scene->mNumMaterials; ++m)
  {
    const auto material = scene->mMaterials[m];
    // サポートするテクスチャタイプ.
    aiString texPath;
    aiReturn result;
    result = material->GetTexture(AI_MATKEY_BASE_COLOR_TEXTURE, &texPath);
    if (result == AI_SUCCESS)
    {
      textureList.push_back(texPath.C_Str());
    }
    result = material->GetTexture(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLICROUGHNESS_TEXTURE, &texPath);
    if (result == AI_SUCCESS)
    {
      textureList.push_back(texPath.C_Str());
    }
    result = material->GetTexture(aiTextureType_NORMALS, 0, &texPath);
    if (result == AI_SUCCESS)
    {
      textureList.push_back(texPath.C_Str());
    }
    result = material->GetTexture(aiTextureType_EMISSIVE, 0, &texPath);
    if (result == AI_SUCCESS)
    {
      textureList.push_back(texPath.C_Str());
    }
  }
  std::sort(textureList.begin(), textureList.end());
  auto itr = std::unique(textureList.begin(), textureList.end());
  textureList.erase(itr, textureList.end());
  return textureList;
}


struct TextureSamplerInfo
{
  std::string fileName;
  uint32_t    mappingModeU, mappingModeV;
  D3D12_FILTER filter;
  uint32_t    addressMode;
};
static void GetTextureList(std::vector<TextureSamplerInfo>& textures, const aiMaterial* material)
{
  textures.resize(model::kNumTextures);
  auto toAddressMode = [](auto type) {
    switch (type)
    {
      case aiTextureMapMode_Wrap: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
      case aiTextureMapMode_Clamp: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      case aiTextureMapMode_Mirror: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
      default: break; // 他は対応しない.
    }
    return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  };

  aiString texPath;
  uint32_t uvindex = 0u;
  aiTextureMapMode mapmode[3] = { aiTextureMapMode_Clamp, aiTextureMapMode_Clamp, aiTextureMapMode_Clamp };
  if (material->GetTexture(AI_MATKEY_BASE_COLOR_TEXTURE, &texPath, nullptr, &uvindex, nullptr, nullptr, mapmode) == AI_SUCCESS)
  {
    assert(uvindex == 0); //  マルチUVに対応しない.
    textures[model::kBaseColor] = { };
    auto& info = textures[model::kBaseColor];
    info.fileName = texPath.C_Str();
    info.addressMode |= toAddressMode(mapmode[0]) << 0;
    info.addressMode |= toAddressMode(mapmode[1]) << 2;
    info.filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  }
  if (material->GetTexture(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLICROUGHNESS_TEXTURE, &texPath, nullptr, &uvindex, nullptr, nullptr, mapmode) == AI_SUCCESS)
  {
    assert(uvindex == 0); //  マルチUVに対応しない.
    textures[model::kMetallicRoughness] = { };
    auto& info = textures[model::kMetallicRoughness];
    info.fileName = texPath.C_Str();
    info.addressMode |= toAddressMode(mapmode[0]) << 0;
    info.addressMode |= toAddressMode(mapmode[1]) << 2;
    info.filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  }
  if (material->GetTexture(aiTextureType_NORMALS, 0, &texPath, nullptr, &uvindex, nullptr, nullptr, mapmode) == AI_SUCCESS)
  {
    assert(uvindex == 0); //  マルチUVに対応しない.
    textures[model::kNormal] = { };
    auto& info = textures[model::kNormal];
    info.fileName = texPath.C_Str();
    info.addressMode |= toAddressMode(mapmode[0]) << 0;
    info.addressMode |= toAddressMode(mapmode[1]) << 2;
    info.filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  }
  if (material->GetTexture(aiTextureType_EMISSIVE, 0, &texPath) == AI_SUCCESS)
  {
    textures[model::kEmissive] = { };
    auto& info = textures[model::kEmissive];
    info.fileName = texPath.C_Str();
    info.addressMode |= toAddressMode(mapmode[0]) << 0;
    info.addressMode |= toAddressMode(mapmode[1]) << 2;
    info.filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  }
}

static void BuildMaterials(model::ModelData& model, const aiScene* scene)
{
  model.textureNames = GetTextureImages(scene);

  const uint32_t numMaterials = scene->mNumMaterials;
  model.materialConstants.resize(numMaterials);
  model.materialTextures.resize(numMaterials);

  std::unordered_map<std::string, uint8_t> textureOptions;
  for (uint32_t i = 0; i < numMaterials; ++i)
  {
    const auto& srcMaterial = scene->mMaterials[i];
    auto& dstMaterial = model.materialConstants[i];

    aiColor3D baseColorFactor; float alphaChannel = 1.0f;
    srcMaterial->Get(AI_MATKEY_BASE_COLOR, baseColorFactor);
    srcMaterial->Get(AI_MATKEY_OPACITY, alphaChannel);
    float metallicFactor, roughnessFactor;
    srcMaterial->Get(AI_MATKEY_METALLIC_FACTOR, metallicFactor);
    srcMaterial->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughnessFactor);
    float alphaCutoff = 0.5f;
    srcMaterial->Get(AI_MATKEY_GLTF_ALPHACUTOFF, alphaCutoff);
    aiString alphaMode;
    srcMaterial->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode);
    aiColor3D emissiveColorFactor;
    srcMaterial->Get(AI_MATKEY_COLOR_EMISSIVE, emissiveColorFactor);

    dstMaterial.baseColorFactor[0] = float(baseColorFactor.r);
    dstMaterial.baseColorFactor[1] = float(baseColorFactor.g);
    dstMaterial.baseColorFactor[2] = float(baseColorFactor.b);
    dstMaterial.baseColorFactor[3] = alphaChannel;
    dstMaterial.metallicFactor = metallicFactor;
    dstMaterial.roughnessFactor = roughnessFactor;
    dstMaterial.alphaCutoff = alphaCutoff;
    dstMaterial.emissiveFactor[0] = emissiveColorFactor.r;
    dstMaterial.emissiveFactor[1] = emissiveColorFactor.g;
    dstMaterial.emissiveFactor[2] = emissiveColorFactor.b;
    dstMaterial.flags = 0;

    if (std::string(alphaMode.C_Str()) == "MASK")
    {
      dstMaterial.flags |= 1; // ALPHA_MASK
    }

    std::vector<TextureSamplerInfo> textures;
    GetTextureList(textures, srcMaterial);
    assert(textures.size() == model::kNumTextures);

    auto& dstTextureData = model.materialTextures[i];
    dstTextureData.addressModes = 0;
    for (uint32_t tindex = 0; tindex < model::kNumTextures; ++tindex)
    {
      dstTextureData.stringIdx[tindex] = 0xFFFFu;

      if (textures[tindex].fileName.empty() == false)
      {
        auto itr = std::find(model.textureNames.begin(), model.textureNames.end(), textures[tindex].fileName);
        assert(itr != model.textureNames.end());
        dstTextureData.stringIdx[tindex] = uint16_t(std::distance(model.textureNames.begin(), itr));
        dstTextureData.addressModes = textures[tindex].addressMode;
      } else
      {
        dstTextureData.addressModes = 0xFFFFFFFF;
      }
    }
  }
}

static std::vector<const aiNode*> GetNodes(const aiNode* node)
{
  std::vector<const aiNode*> nodes;
  nodes.push_back(node);

  for (uint32_t i = 0; i < node->mNumChildren; ++i)
  {
    auto cnodes = GetNodes(node->mChildren[i]);
    nodes.insert(nodes.end(), cnodes.begin(), cnodes.end());
  }
  return nodes;
}

static void BuildMesh(std::vector<model::Mesh*>& meshList, std::vector<byte>& bufferMemory, const aiMesh* srcMesh, const aiMaterial* srcMaterial, uint32_t nodeIndex)
{
  struct Vertex
  {
    XMFLOAT3 Pos; XMFLOAT3 Normal; XMFLOAT2 UV;
    XMFLOAT3 Tangent; XMFLOAT3 Binormal;
  };
  size_t curVertexSize = sizeof(Vertex) * srcMesh->mNumVertices;
  auto indexCount = srcMesh->mNumFaces * 3;  // 3角形化しているため.
  size_t curIndexSize = sizeof(uint32_t) * indexCount;

  std::vector<byte> stagingBuffer;
  stagingBuffer.resize(curVertexSize + curIndexSize);

  std::vector<Vertex> vertices(srcMesh->mNumVertices);
  auto GetPosition = [&](auto i) { auto v = srcMesh->mVertices[i]; return XMFLOAT3(v.x, v.y, v.z); };
  auto GetNormal = [&](auto i) { auto v = srcMesh->mNormals[i]; return XMFLOAT3(v.x, v.y, v.z); };
  auto GetUV = [&](auto i) {
    auto uv = srcMesh->mTextureCoords[0];
    return uv != nullptr ? XMFLOAT2(uv[i].x, uv[i].y) : XMFLOAT2{ 0,0 };
    };
  auto GetTangent = [&](auto i) { auto v = srcMesh->mTangents[i]; return XMFLOAT3(v.x, v.y, v.z); };
  auto GetBinormal = [&](auto i) { auto v = srcMesh->mBitangents[i]; return XMFLOAT3(v.x, v.y, v.z); };

  for (uint32_t i = 0; i < vertices.size(); ++i)
  {
    auto& v = vertices[i];
    v.Pos = GetPosition(i);
    v.Normal = GetNormal(i);
    v.UV = GetUV(i);
    v.Tangent = XMFLOAT3(1, 0, 0);
    v.Binormal = XMFLOAT3(0, 1, 0);
    if (srcMesh->mTangents != nullptr)
    {
      v.Tangent = GetTangent(i); v.Binormal = GetBinormal(i);
    }
  }

  std::vector<uint32_t> indices; indices.reserve(indexCount);
  for (uint32_t i = 0; i < srcMesh->mNumFaces; ++i)
  {
    assert(srcMesh->mFaces[i].mNumIndices == 3);
    indices.push_back(srcMesh->mFaces[i].mIndices[0]);
    indices.push_back(srcMesh->mFaces[i].mIndices[1]);
    indices.push_back(srcMesh->mFaces[i].mIndices[2]);
  }
  memcpy(stagingBuffer.data(), vertices.data(), curVertexSize);
  memcpy(stagingBuffer.data() + curVertexSize, indices.data(), curIndexSize);

  auto mesh = new model::Mesh();
  mesh->materialCBV = srcMesh->mMaterialIndex;
  mesh->meshCBV = nodeIndex;
  mesh->vbOffset = static_cast<uint32_t>(bufferMemory.size());
  mesh->ibOffset = static_cast<uint32_t>(bufferMemory.size() + curVertexSize);
  mesh->vbSize = static_cast<uint32_t>(curVertexSize);
  mesh->ibSize = static_cast<uint32_t>(curIndexSize);
  mesh->vbStride = uint8_t(sizeof(Vertex));
  mesh->draw.primitiveCount = indexCount;

  auto convertTo = [](const aiVector3D& v) { return XMFLOAT3(v.x, v.y, v.z); };
  mesh->aabbMin = convertTo(srcMesh->mAABB.mMin);
  mesh->aabbMax = convertTo(srcMesh->mAABB.mMax);

  std::string alphaMode;
  if (aiString mode; srcMaterial->Get(AI_MATKEY_GLTF_ALPHAMODE, mode) == AI_SUCCESS)
  {
    alphaMode = std::string(mode.C_Str());
  }

  mesh->drawMode = model::DrawMode::DrawModeUnknown;
  if (alphaMode == "OPAQUE")
  {
    mesh->drawMode = model::DrawModeOpaque;
  }
  if (alphaMode == "MASK")
  {
    mesh->drawMode = model::DrawModeMask;
  }
  if (alphaMode == "BLEND")
  {
    mesh->drawMode = model::DrawModeBlend;
  }


  meshList.push_back(mesh);
  bufferMemory.insert(bufferMemory.end(), stagingBuffer.begin(), stagingBuffer.end());
}

static void BuildNode(uint32_t nodeIndex, model::ModelData& modelData, const aiNode* srcNode, uint32_t parentIndex, const aiScene* scene)
{
  auto& dstNode = modelData.sceneGraph[nodeIndex];
  dstNode.nodeNameIndex = nodeIndex;
  dstNode.xform = ConvertMatrix(srcNode->mTransformation);
  dstNode.parentIndex = parentIndex;

  if (parentIndex != UINT32_MAX)
  {
    const auto& parentNode = modelData.sceneGraph[parentIndex];
    XMMATRIX mtxLocal = XMLoadFloat4x4(&dstNode.xform);
    XMMATRIX mtxParent = XMLoadFloat4x4(&parentNode.worldTransform);
    XMMATRIX mtxWorld = XMMatrixMultiply(mtxLocal, mtxParent);
    XMStoreFloat4x4(&dstNode.worldTransform, mtxWorld);
  } else
  {
    dstNode.worldTransform = dstNode.xform;
  }

  if (srcNode->mNumMeshes > 0)
  {
    for (uint32_t i = 0; i < srcNode->mNumMeshes; ++i)
    {
      auto meshIndex = srcNode->mMeshes[i];
      const auto mesh = scene->mMeshes[meshIndex];
      const auto material = scene->mMaterials[mesh->mMaterialIndex];
      BuildMesh(modelData.meshes, modelData.geometryData, mesh, material, nodeIndex);
    }
  }
}

static std::vector<byte> LoadTextureCore(const uint8_t* data, size_t size)
{
  std::vector<byte> buffer;
  DirectX::ScratchImage image;
  DirectX::TexMetadata metadata;
  HRESULT hr;

  hr = DirectX::LoadFromTGAMemory(data, size, &metadata, image);
  if (FAILED(hr))
  {
    DDS_FLAGS flags = DDS_FLAGS_NONE;
    hr = DirectX::LoadFromDDSMemory(data, size, flags, &metadata, image);
  }
  if (FAILED(hr))
  {
    WIC_FLAGS flags = DirectX::WIC_FLAGS_IGNORE_SRGB; // DXGI_FORMAT_R8G8B8A8_UNORM のように+_SRGBなしで処理しておきたいので設定.
    hr = DirectX::LoadFromWICMemory(data, size, flags, &metadata, image);
  }
  if (FAILED(hr))
  {
    return buffer;
  }
  if (metadata.mipLevels == 1)
  {
    // ミップマップを作成する.
    DirectX::ScratchImage mipChain;
    TEX_FILTER_FLAGS flags = DirectX::TEX_FILTER_DEFAULT;
    flags |= DirectX::TEX_FILTER_BOX | TEX_FILTER_FORCE_NON_WIC;
    hr = DirectX::GenerateMipMaps(image.GetImages(), image.GetImageCount(), image.GetMetadata(), flags, 0, mipChain);
    image = std::move(mipChain);
    metadata = image.GetMetadata();
  }

  // DDSファイルイメージにして、返却.
  DDS_FLAGS writeDdsFlags = DDS_FLAGS_NONE;
  DirectX::Blob outBlob;
  hr = DirectX::SaveToDDSMemory(image.GetImages(), image.GetImageCount(), image.GetMetadata(), writeDdsFlags, outBlob);

  buffer.resize(outBlob.GetBufferSize());
  memcpy(buffer.data(), outBlob.GetBufferPointer(), buffer.size());

  return buffer;
}

static void BuildTextureImages(model::ModelData& modelData, const aiScene* scene, const std::filesystem::path& baseDirectory)
{
  std::vector<std::string> embedded;
  if (scene->mNumTextures)
  {
    for (uint32_t i = 0; i < scene->mNumTextures; ++i)
    {
      std::string name = "*";
      name += std::to_string(i);
      embedded.push_back(name);
    }
  }
  for (const auto& name : modelData.textureNames)
  {
    auto itr = std::find(embedded.begin(), embedded.end(), name);
    if (itr != embedded.end())
    {
      // 埋め込みテクスチャ.
      auto index = std::distance(embedded.begin(), itr);
      auto byteSize = scene->mTextures[index]->mWidth;
      auto& storeBuffer = modelData.textureImages.emplace_back();
      storeBuffer = LoadTextureCore(reinterpret_cast<const uint8_t*>(scene->mTextures[index]->pcData), byteSize);
    } else
    {
      // ファイルからのロード.
      auto filePath = baseDirectory / name.c_str();
      std::ifstream infile(filePath, std::ios::binary);
      if (infile)
      {
        std::vector<char> workBuf;
        workBuf.resize(infile.seekg(0, std::ifstream::end).tellg());
        infile.seekg(0, std::ifstream::beg).read(reinterpret_cast<char*>(workBuf.data()), workBuf.size());

        auto& storeBuffer = modelData.textureImages.emplace_back();
        storeBuffer = LoadTextureCore(reinterpret_cast<const uint8_t*>(workBuf.data()), workBuf.size());
      } else
      {
        // ファイルが見つからない.
      }
    }
  }
}

bool LoadModelData(model::ModelData* modelData, fs::path modelFile)
{
  if (!modelData)
  {
    return false;
  }

  auto baseDirectory = modelFile.parent_path();
  Assimp::Importer importer;
  uint32_t flags = 0;
  flags |= aiProcess_Triangulate;   // 3角形化する.
  flags |= aiProcess_RemoveRedundantMaterials;  // 冗長なマテリアルを削除.
  flags |= aiProcess_FlipUVs;   // テクスチャ座標系:左上を原点とする.
  flags |= aiProcess_GenBoundingBoxes; // バウンディングボックス生成.
  const auto scene = importer.ReadFile(modelFile.string(), flags);

  if (scene == nullptr) { return false; }
  if (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)
  {
    return false;
  }

  BuildMaterials(*modelData, scene);

  auto nodeList = GetNodes(scene->mRootNode);
  modelData->sceneGraph.resize(nodeList.size());
  for (const auto& node : nodeList)
  {
    modelData->nodeNames.push_back(node->mName.C_Str());
  }
  auto& bufferMemory = modelData->geometryData;

  std::unordered_map<const aiNode*, uint32_t> nodeIndexMap;
  for (uint32_t i = 0; i < nodeList.size(); ++i)
  {
    nodeIndexMap[nodeList[i]] = i;
  }

  for (uint32_t i = 0; i < modelData->sceneGraph.size(); ++i)
  {
    const auto srcNode = nodeList[i];
    auto parentIndex = (srcNode->mParent) ? nodeIndexMap[srcNode->mParent] : UINT32_MAX;
    BuildNode(i, *modelData, srcNode, parentIndex, scene);
  }

  BuildTextureImages(*modelData, scene, baseDirectory);

  XMVECTOR aabbMin = XMVectorSet(FLT_MAX, FLT_MAX, FLT_MAX, 0);
  XMVECTOR aabbMax = XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 0);
  for (const auto& mesh : modelData->meshes)
  {
    aabbMin = XMVectorMin(aabbMin, XMLoadFloat3(&mesh->aabbMin));
    aabbMax = XMVectorMax(aabbMax, XMLoadFloat3(&mesh->aabbMax));
  }
  XMStoreFloat3(&modelData->aabbMin, aabbMin);
  XMStoreFloat3(&modelData->aabbMax, aabbMax);

  return true;
}