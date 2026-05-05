#pragma once

#include "../loader/GltfLoader.h"

namespace demo {

class AsyncLoadingCoordinator
{
public:
  struct UploadBatch
  {
    std::vector<uint32_t> textureIndices;
    std::vector<uint32_t> materialIndices;
    std::vector<uint32_t> meshIndices;
    bool                  criticalBatch{false};
    bool                  finalBatch{false};
  };

  struct LoadProgress
  {
    uint32_t texturesLoaded{0};
    uint32_t texturesTotal{0};
    uint32_t materialsLoaded{0};
    uint32_t materialsTotal{0};
    uint32_t meshesLoaded{0};
    uint32_t meshesTotal{0};
    float    progressPercent{0.0f};
    bool     isComplete{false};
  };

  void begin(const GltfModel& model,
             const glm::vec3& cameraPos,
             uint32_t         firstBatchMeshBudget = 24,
             uint32_t         batchMeshBudget      = 64);

  // One-shot loading: upload all assets in single batch
  void beginOneShot(const GltfModel& model);

  [[nodiscard]] bool hasPendingBatches() const;
  [[nodiscard]] UploadBatch takeNextBatch();
  void markBatchUploaded(const UploadBatch& batch);
  [[nodiscard]] const LoadProgress& getProgress() const { return m_progress; }

private:
  const GltfModel*         m_model{nullptr};
  std::vector<uint32_t>    m_sortedMeshIndices;
  std::vector<bool>        m_queuedTextures;
  std::vector<bool>        m_queuedMaterials;
  std::vector<bool>        m_queuedMeshes;
  std::vector<bool>        m_uploadedTextures;
  std::vector<bool>        m_uploadedMaterials;
  std::vector<bool>        m_uploadedMeshes;
  size_t                   m_nextMeshCursor{0};
  uint32_t                 m_firstBatchMeshBudget{24};
  uint32_t                 m_batchMeshBudget{64};
  LoadProgress             m_progress{};

  void refreshProgress();
};

}  // namespace demo
