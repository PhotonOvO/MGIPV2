#include "AsyncLoadingCoordinator.h"

#include <algorithm>
#include <limits>

namespace demo {

namespace {

enum AssetPriority : uint32_t
{
  Critical   = 100,
  High       = 50,
  Medium     = 25,
  Low        = 10,
  Background = 1,
};

struct PrioritizedMesh
{
  uint32_t index{0};
  uint32_t priority{Background};
  float    distance{0.0f};
};

glm::vec3 computeMeshCenter(const GltfMeshData& mesh)
{
  if(mesh.positions.empty())
  {
    return glm::vec3(mesh.transform[3]);
  }

  glm::vec3 minBounds(std::numeric_limits<float>::max());
  glm::vec3 maxBounds(std::numeric_limits<float>::lowest());
  for(size_t i = 0; i + 2 < mesh.positions.size(); i += 3)
  {
    const glm::vec3 local(mesh.positions[i + 0], mesh.positions[i + 1], mesh.positions[i + 2]);
    const glm::vec3 world = glm::vec3(mesh.transform * glm::vec4(local, 1.0f));
    minBounds = glm::min(minBounds, world);
    maxBounds = glm::max(maxBounds, world);
  }
  return 0.5f * (minBounds + maxBounds);
}

uint32_t calculatePriority(const glm::vec3& meshCenter, const glm::vec3& cameraPos)
{
  const float distance = glm::length(meshCenter - cameraPos);
  if(distance < 10.0f) return Critical;
  if(distance < 25.0f) return High;
  if(distance < 50.0f) return Medium;
  if(distance < 100.0f) return Low;
  return Background;
}

void appendTextureIndex(std::vector<uint32_t>& indices, uint32_t textureIndex, std::vector<bool>& queued, const std::vector<bool>& uploaded)
{
  if(textureIndex >= queued.size() || queued[textureIndex] || uploaded[textureIndex])
  {
    return;
  }

  queued[textureIndex] = true;
  indices.push_back(textureIndex);
}

}  // namespace

void AsyncLoadingCoordinator::begin(const GltfModel& model,
                                    const glm::vec3& cameraPos,
                                    uint32_t         firstBatchMeshBudget,
                                    uint32_t         batchMeshBudget)
{
  m_model                = &model;
  m_nextMeshCursor       = 0;
  m_firstBatchMeshBudget = std::max(firstBatchMeshBudget, 1u);
  m_batchMeshBudget      = std::max(batchMeshBudget, 1u);
  m_sortedMeshIndices.clear();

  m_queuedTextures.assign(model.images.size(), false);
  m_queuedMaterials.assign(model.materials.size(), false);
  m_queuedMeshes.assign(model.meshes.size(), false);
  m_uploadedTextures.assign(model.images.size(), false);
  m_uploadedMaterials.assign(model.materials.size(), false);
  m_uploadedMeshes.assign(model.meshes.size(), false);

  std::vector<PrioritizedMesh> prioritizedMeshes;
  prioritizedMeshes.reserve(model.meshes.size());
  for(uint32_t meshIndex = 0; meshIndex < static_cast<uint32_t>(model.meshes.size()); ++meshIndex)
  {
    const glm::vec3 center = computeMeshCenter(model.meshes[meshIndex]);
    prioritizedMeshes.push_back(PrioritizedMesh{
        .index    = meshIndex,
        .priority = calculatePriority(center, cameraPos),
        .distance = glm::length(center - cameraPos),
    });
  }

  std::sort(prioritizedMeshes.begin(), prioritizedMeshes.end(), [](const PrioritizedMesh& a, const PrioritizedMesh& b) {
    if(a.priority != b.priority)
    {
      return a.priority > b.priority;
    }
    return a.distance < b.distance;
  });

  m_sortedMeshIndices.reserve(prioritizedMeshes.size());
  for(const PrioritizedMesh& entry : prioritizedMeshes)
  {
    m_sortedMeshIndices.push_back(entry.index);
  }

  m_progress = {};
  m_progress.texturesTotal  = static_cast<uint32_t>(model.images.size());
  m_progress.materialsTotal = static_cast<uint32_t>(model.materials.size());
  m_progress.meshesTotal    = static_cast<uint32_t>(model.meshes.size());
  refreshProgress();
}

void AsyncLoadingCoordinator::beginOneShot(const GltfModel& model)
{
  // Use a very large budget to upload all assets in one batch
  begin(model, glm::vec3(0.0f), std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max());
}

bool AsyncLoadingCoordinator::hasPendingBatches() const
{
  return m_model != nullptr && m_nextMeshCursor < m_sortedMeshIndices.size();
}

AsyncLoadingCoordinator::UploadBatch AsyncLoadingCoordinator::takeNextBatch()
{
  UploadBatch batch{};
  if(m_model == nullptr || !hasPendingBatches())
  {
    batch.finalBatch = true;
    return batch;
  }

  const uint32_t meshBudget = m_progress.meshesLoaded == 0 ? m_firstBatchMeshBudget : m_batchMeshBudget;
  batch.criticalBatch = (m_progress.meshesLoaded == 0);

  while(m_nextMeshCursor < m_sortedMeshIndices.size() && batch.meshIndices.size() < meshBudget)
  {
    const uint32_t meshIndex = m_sortedMeshIndices[m_nextMeshCursor++];
    if(meshIndex >= m_queuedMeshes.size() || m_queuedMeshes[meshIndex])
    {
      continue;
    }

    m_queuedMeshes[meshIndex] = true;
    batch.meshIndices.push_back(meshIndex);

    const int materialIndex = m_model->meshes[meshIndex].materialIndex;
    if(materialIndex >= 0 && materialIndex < static_cast<int>(m_model->materials.size()))
    {
      const uint32_t materialSlot = static_cast<uint32_t>(materialIndex);
      if(!m_queuedMaterials[materialSlot] && !m_uploadedMaterials[materialSlot])
      {
        m_queuedMaterials[materialSlot] = true;
        batch.materialIndices.push_back(materialSlot);
      }

      const GltfMaterialData& material = m_model->materials[materialSlot];
      if(material.baseColorTexture >= 0)
      {
        appendTextureIndex(batch.textureIndices,
                           static_cast<uint32_t>(material.baseColorTexture),
                           m_queuedTextures,
                           m_uploadedTextures);
      }
      if(material.normalTexture >= 0)
      {
        appendTextureIndex(batch.textureIndices,
                           static_cast<uint32_t>(material.normalTexture),
                           m_queuedTextures,
                           m_uploadedTextures);
      }
      if(material.metallicRoughnessTexture >= 0)
      {
        appendTextureIndex(batch.textureIndices,
                           static_cast<uint32_t>(material.metallicRoughnessTexture),
                           m_queuedTextures,
                           m_uploadedTextures);
      }
      if(material.occlusionTexture >= 0)
      {
        appendTextureIndex(batch.textureIndices,
                           static_cast<uint32_t>(material.occlusionTexture),
                           m_queuedTextures,
                           m_uploadedTextures);
      }
      if(material.emissiveTexture >= 0)
      {
        appendTextureIndex(batch.textureIndices,
                           static_cast<uint32_t>(material.emissiveTexture),
                           m_queuedTextures,
                           m_uploadedTextures);
      }
    }
  }

  batch.finalBatch = !hasPendingBatches();
  return batch;
}

void AsyncLoadingCoordinator::markBatchUploaded(const UploadBatch& batch)
{
  for(uint32_t index : batch.textureIndices)
  {
    if(index < m_uploadedTextures.size())
    {
      m_uploadedTextures[index] = true;
    }
  }
  for(uint32_t index : batch.materialIndices)
  {
    if(index < m_uploadedMaterials.size())
    {
      m_uploadedMaterials[index] = true;
    }
  }
  for(uint32_t index : batch.meshIndices)
  {
    if(index < m_uploadedMeshes.size())
    {
      m_uploadedMeshes[index] = true;
    }
  }

  refreshProgress();
}

void AsyncLoadingCoordinator::refreshProgress()
{
  m_progress.texturesLoaded = static_cast<uint32_t>(std::count(m_uploadedTextures.begin(), m_uploadedTextures.end(), true));
  m_progress.materialsLoaded = static_cast<uint32_t>(std::count(m_uploadedMaterials.begin(), m_uploadedMaterials.end(), true));
  m_progress.meshesLoaded = static_cast<uint32_t>(std::count(m_uploadedMeshes.begin(), m_uploadedMeshes.end(), true));

  const float textureRatio = m_progress.texturesTotal > 0
                                 ? static_cast<float>(m_progress.texturesLoaded) / static_cast<float>(m_progress.texturesTotal)
                                 : 1.0f;
  const float materialRatio = m_progress.materialsTotal > 0
                                  ? static_cast<float>(m_progress.materialsLoaded) / static_cast<float>(m_progress.materialsTotal)
                                  : 1.0f;
  const float meshRatio = m_progress.meshesTotal > 0
                              ? static_cast<float>(m_progress.meshesLoaded) / static_cast<float>(m_progress.meshesTotal)
                              : 1.0f;

  m_progress.progressPercent = textureRatio * 0.4f + materialRatio * 0.1f + meshRatio * 0.5f;
  m_progress.isComplete = m_progress.texturesLoaded == m_progress.texturesTotal
                          && m_progress.materialsLoaded == m_progress.materialsTotal
                          && m_progress.meshesLoaded == m_progress.meshesTotal;
}

}  // namespace demo
