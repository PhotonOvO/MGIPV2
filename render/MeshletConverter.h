#pragma once

#include "../common/Common.h"
#include "../loader/GltfLoader.h"

#include <vector>

namespace demo {

struct MeshletConversionResult
{
  std::vector<shaderio::Meshlet> meshlets;
  std::vector<uint32_t>          packedIndices;
  uint32_t                       triangleCount{0};
};

class MeshletConverter
{
public:
  static MeshletConversionResult convert(const GltfMeshData& mesh, uint32_t maxVertices = 64, uint32_t maxTriangles = 124);
};

}  // namespace demo
