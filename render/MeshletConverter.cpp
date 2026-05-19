#include "MeshletConverter.h"

#include <algorithm>
#include <limits>

#include <meshoptimizer.h>

namespace demo {

namespace {
constexpr float kMeshletConeWeight = 0.0f;

}  // namespace

MeshletConversionResult MeshletConverter::convert(const GltfMeshData& mesh, uint32_t maxVertices, uint32_t maxTriangles)
{
  MeshletConversionResult result;
  if(mesh.indices.empty() || mesh.positions.empty() || maxVertices == 0u || maxTriangles == 0u)
  {
    return result;
  }

  const size_t vertexCount = mesh.positions.size() / 3u;
  if(vertexCount == 0u)
  {
    return result;
  }

  maxVertices = std::clamp(maxVertices, 1u, 255u);
  maxTriangles = std::max(1u, maxTriangles);

  const size_t meshletBound = meshopt_buildMeshletsBound(mesh.indices.size(), maxVertices, maxTriangles);
  if(meshletBound == 0u)
  {
    return result;
  }

  std::vector<meshopt_Meshlet> meshlets(meshletBound);
  std::vector<unsigned int>    meshletVertices(meshletBound * maxVertices);
  std::vector<unsigned char>   meshletTriangles(meshletBound * maxTriangles * 3u);

  const size_t meshletCount = meshopt_buildMeshlets(meshlets.data(),
                                                    meshletVertices.data(),
                                                    meshletTriangles.data(),
                                                    mesh.indices.data(),
                                                    mesh.indices.size(),
                                                    mesh.positions.data(),
                                                    vertexCount,
                                                    sizeof(float) * 3u,
                                                    maxVertices,
                                                    maxTriangles,
                                                    kMeshletConeWeight);
  result.meshlets.reserve(meshletCount);
  result.packedIndices.reserve(mesh.indices.size());

  for(size_t meshletIndex = 0; meshletIndex < meshletCount; ++meshletIndex)
  {
    meshopt_Meshlet& sourceMeshlet = meshlets[meshletIndex];
    unsigned int*    vertices = meshletVertices.data() + sourceMeshlet.vertex_offset;
    unsigned char*   triangles = meshletTriangles.data() + sourceMeshlet.triangle_offset;

    meshopt_optimizeMeshlet(vertices, triangles, sourceMeshlet.triangle_count, sourceMeshlet.vertex_count);
    const meshopt_Bounds bounds = meshopt_computeMeshletBounds(vertices,
                                                               triangles,
                                                               sourceMeshlet.triangle_count,
                                                               mesh.positions.data(),
                                                               vertexCount,
                                                               sizeof(float) * 3u);

    shaderio::Meshlet meshlet{};
    meshlet.boundsSphere = glm::vec4(bounds.center[0], bounds.center[1], bounds.center[2], bounds.radius);
    meshlet.vertexOffset = 0u;
    meshlet.indexOffset = static_cast<uint32_t>(result.packedIndices.size());
    meshlet.triangleCount = sourceMeshlet.triangle_count;
    meshlet.materialIndex = mesh.materialIndex >= 0 ? static_cast<uint32_t>(mesh.materialIndex) : UINT32_MAX;
    result.meshlets.push_back(meshlet);

    for(uint32_t triangleIndex = 0; triangleIndex < sourceMeshlet.triangle_count; ++triangleIndex)
    {
      for(uint32_t corner = 0; corner < 3u; ++corner)
      {
        const uint32_t localVertexIndex = triangles[triangleIndex * 3u + corner];
        result.packedIndices.push_back(vertices[localVertexIndex]);
      }
    }
    result.triangleCount += sourceMeshlet.triangle_count;
  }

  return result;
}

}  // namespace demo
