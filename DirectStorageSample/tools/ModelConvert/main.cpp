#include "ModelConvert.h"
#include "Model.h"

#pragma comment(lib, "assimp.lib")
#pragma comment(lib, "d3d12.lib")

namespace fs = std::filesystem;

CompressType ParseCompressType(const std::string& typeStr) {
  static const std::unordered_map<std::string, CompressType> typeMap = {
    {"gdeflate", CompressType::GDeflate},
    {"uncompress", CompressType::Uncompress},
    {"texcompress", CompressType::TexCompress}
  };

  auto it = typeMap.find(typeStr);
  return (it != typeMap.end()) ? it->second : CompressType::GDeflate; // デフォルトは gdeflate
}

bool BuildModel(model::ModelData& modelData, fs::path modelFile)
{
  return true;
}



int main(int argc, char* argv[])
{
  fs::path inputFilePath, outputFilePath;
  CompressType compressType = CompressType::GDeflate; // デフォルト値
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "-i" && i + 1 < argc) {
      inputFilePath = argv[++i];
    } else if (arg == "-o" && i + 1 < argc) {
      outputFilePath = argv[++i];
    } else if (arg == "-t" && i + 1 < argc) {
      compressType = ParseCompressType(argv[++i]);
    }
  }
  if (inputFilePath.empty() || outputFilePath.empty()) {
    std::cerr << "Usage: " << argv[0] << " -i <input> -o <output> [-t <type>];";
    std::cerr << "  Type [ gdeflate, uncompress, texcompress]\n";
    return 1;
  }

  if (!outputFilePath.has_extension())
  {
    outputFilePath = outputFilePath.replace_extension("pak");
  }

  std::cout << "Compression Type: ";
  switch (compressType) {
    case CompressType::GDeflate: std::cout << "gdeflate"; break;
    case CompressType::Uncompress: std::cout << "uncompress"; break;
    case CompressType::TexCompress: std::cout << "texcompress"; break;
  }
  std::cout << std::endl;

  if (!fs::exists(inputFilePath))
  {
    std::cerr << "Not found input file.\n";
    return 1;
  }

  CoInitializeEx(NULL, COINIT_MULTITHREADED);

  model::ModelData modelData{};
  if (!LoadModelData(&modelData, inputFilePath))
  {
    std::cerr << "failure LoadModelData.\n";
    return 1;
  }

  if (!WriteModelData(&modelData, compressType, outputFilePath))
  {
    std::cerr << "failure WriteModelData.\n";
    return 1;
  }
  std::cout << "Convert done!!!\n";
  return 0;
}
