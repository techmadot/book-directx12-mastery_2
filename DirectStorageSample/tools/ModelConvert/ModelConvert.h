#pragma once

#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "Model.h"

enum class CompressType {
  GDeflate,
  Uncompress,
  TexCompress // BC3+GDeflate
};

bool LoadModelData(model::ModelData* modelData, std::filesystem::path modelFile);
bool WriteModelData(const model::ModelData* modelData, CompressType compressType, std::filesystem::path outputFilePath);
