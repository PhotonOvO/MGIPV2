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

uint64_t estimateTextureUploadBytes(const GltfModel& model, uint32_t textureIndex)
{
  if(textureIndex >= model.images.size())
  {
    return 0;
  }

  return static_cast<uint64_t>(model.images[textureIndex].pixels.size());
}

uint64_t estimateMeshUploadBytes(const GltfMeshData& mesh)
{
  const uint64_t vertexCount = static_cast<uint64_t>(mesh.positions.size() / 3u);
  return vertexCount * 48ull + static_cast<uint64_t>(mesh.indices.size()) * sizeof(uint32_t);
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
  m_firstBatchTextureBudgetBytes = 32ull << 20;
  m_batchTextureBudgetBytes = 64ull << 20;
  m_firstBatchMeshUploadBudgetBytes = 16ull << 20;
  m_batchMeshUploadBudgetBytes = 32ull << 20;
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
  const uint64_t textureBudgetBytes =
      m_progress.meshesLoaded == 0 ? m_firstBatchTextureBudgetBytes : m_batchTextureBudgetBytes;
  const uint64_t meshUploadBudgetBytes =
      m_progress.meshesLoaded == 0 ? m_firstBatchMeshUploadBudgetBytes : m_batchMeshUploadBudgetBytes;
  batch.criticalBatch = (m_progress.meshesLoaded == 0);
  uint64_t batchTextureBytes = 0;
  uint64_t batchMeshBytes = 0;

  while(m_nextMeshCursor < m_sortedMeshIndices.size() && batch.meshIndices.size() < meshBudget)
  {
    const uint32_t meshIndex = m_sortedMeshIndices[m_nextMeshCursor++];
    if(meshIndex >= m_queuedMeshes.size() || m_queuedMeshes[meshIndex])
    {
      continue;
    }

    const GltfMeshData& mesh = m_model->meshes[meshIndex];
    std::vector<uint32_t> candidateTextures;
    uint64_t candidateTextureBytes = 0;
    uint64_t candidateMeshBytes = estimateMeshUploadBytes(mesh);
    const int materialIndex = mesh.materialIndex;
    if(materialIndex >= 0 && materialIndex < static_cast<int>(m_model->materials.size()))
    {
      const uint32_t materialSlot = static_cast<uint32_t>(materialIndex);
      const GltfMaterialData& material = m_model->materials[materialSlot];
      auto stageTexture = [&](int textureIndex) {
        if(textureIndex < 0)
        {
          return;
        }

        const uint32_t textureSlot = static_cast<uint32_t>(textureIndex);
        if(textureSlot >= m_queuedTextures.size() || m_queuedTextures[textureSlot] || m_uploadedTextures[textureSlot])
        {
          return;
        }

        if(std::find(candidateTextures.begin(), candidateTextures.end(), textureSlot) != candidateTextures.end())
        {
          return;
        }

        candidateTextures.push_back(textureSlot);
        candidateTextureBytes += estimateTextureUploadBytes(*m_model, textureSlot);
      };

      stageTexture(material.baseColorTexture);
      stageTexture(material.normalTexture);
      stageTexture(material.metallicRoughnessTexture);
      stageTexture(material.occlusionTexture);
      stageTexture(material.emissiveTexture);

      const bool exceedsTextureBudget =
          batchTextureBytes + candidateTextureBytes > textureBudgetBytes;
      const bool exceedsMeshBudget =
          batchMeshBytes + candidateMeshBytes > meshUploadBudgetBytes;
      if((exceedsTextureBudget || exceedsMeshBudget) && !batch.meshIndices.empty())
      {
        --m_nextMeshCursor;
        break;
      }

      if(!m_queuedMaterials[materialSlot] && !m_uploadedMaterials[materialSlot])
      {
        m_queuedMaterials[materialSlot] = true;
        batch.materialIndices.push_back(materialSlot);
      }

      for(uint32_t textureSlot : candidateTextures)
      {
        appendTextureIndex(batch.textureIndices, textureSlot, m_queuedTextures, m_uploadedTextures);
      }
      batchTextureBytes += candidateTextureBytes;
    }
    else if(batchMeshBytes + candidateMeshBytes > meshUploadBudgetBytes && !batch.meshIndices.empty())
    {
      --m_nextMeshCursor;
      break;
    }

    m_queuedMeshes[meshIndex] = true;
    batch.meshIndices.push_back(meshIndex);
    batchMeshBytes += candidateMeshBytes;
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
