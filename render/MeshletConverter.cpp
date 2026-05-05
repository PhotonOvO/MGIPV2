#include "MeshletConverter.h"

#include <algorithm>
#include <unordered_set>

namespace demo {

namespace {

glm::vec3 readPosition(const GltfMeshData& mesh, uint32_t vertexIndex)
{
  const size_t base = static_cast<size_t>(vertexIndex) * 3u;
  return glm::vec3(mesh.positions[base + 0u], mesh.positions[base + 1u], mesh.positions[base + 2u]);
}

glm::vec4 computeBoundsSphere(const GltfMeshData& mesh, const std::vector<uint32_t>& indices, size_t begin, size_t end)
{
  glm::vec3 minBounds(std::numeric_limits<float>::max());
  glm::vec3 maxBounds(std::numeric_limits<float>::lowest());
  for(size_t i = begin; i < end; ++i)
  {
    const glm::vec3 position = readPosition(mesh, indices[i]);
    minBounds = glm::min(minBounds, position);
    maxBounds = glm::max(maxBounds, position);
  }

  const glm::vec3 center = 0.5f * (minBounds + maxBounds);
  float radius = 0.0f;
  for(size_t i = begin; i < end; ++i)
  {
    radius = std::max(radius, glm::length(readPosition(mesh, indices[i]) - center));
  }
  return glm::vec4(center, radius);
}

}  // namespace

MeshletConversionResult MeshletConverter::convert(const GltfMeshData& mesh, uint32_t maxVertices, uint32_t maxTriangles)
{
  MeshletConversionResult result;
  if(mesh.indices.empty() || mesh.positions.empty())
  {
    return result;
  }

  const uint32_t trianglesPerMeshlet = std::max(1u, maxTriangles);
  size_t cursor = 0;
  while(cursor + 2u < mesh.indices.size())
  {
    size_t end = cursor;
    std::unordered_set<uint32_t> uniqueVertices;
    uint32_t triangleCount = 0;
    while(end + 2u < mesh.indices.size() && triangleCount < trianglesPerMeshlet)
    {
      std::unordered_set<uint32_t> candidateVertices = uniqueVertices;
      candidateVertices.insert(mesh.indices[end + 0u]);
      candidateVertices.insert(mesh.indices[end + 1u]);
      candidateVertices.insert(mesh.indices[end + 2u]);
      if(candidateVertices.size() > maxVertices && triangleCount > 0)
      {
        break;
      }

      uniqueVertices = std::move(candidateVertices);
      end += 3u;
      ++triangleCount;
    }

    shaderio::Meshlet meshlet{};
    meshlet.boundsSphere = computeBoundsSphere(mesh, mesh.indices, cursor, end);
    meshlet.vertexOffset = 0u;
    meshlet.indexOffset = static_cast<uint32_t>(result.packedIndices.size());
    meshlet.triangleCount = triangleCount;
    meshlet.materialIndex = mesh.materialIndex >= 0 ? static_cast<uint32_t>(mesh.materialIndex) : UINT32_MAX;
    result.meshlets.push_back(meshlet);
    result.packedIndices.insert(result.packedIndices.end(), mesh.indices.begin() + static_cast<ptrdiff_t>(cursor),
                                mesh.indices.begin() + static_cast<ptrdiff_t>(end));
    result.triangleCount += triangleCount;
    cursor = end;
  }

  return result;
}

}  // namespace demo
