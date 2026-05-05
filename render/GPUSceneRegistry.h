#pragma once

#include "../common/Common.h"
#include "../common/Handles.h"

#include <cstdint>
#include <vector>

namespace demo {

struct GPUSceneRegistrationDesc
{
  MeshHandle meshHandle{};
  uint32_t   meshIndex{0};
  uint32_t   materialIndex{UINT32_MAX};
  glm::mat4  transform{1.0f};
  glm::vec4  boundsSphere{0.0f};
  uint32_t   flags{0};
  uint32_t   indexCount{0};
  uint32_t   firstIndex{0};
  int32_t    vertexOffset{0};
};

class GPUSceneRegistry
{
public:
  void init(VkDevice device, VmaAllocator allocator);
  void deinit();
  void clear();

  [[nodiscard]] uint32_t registerObject(const GPUSceneRegistrationDesc& desc);
  void                   removeObject(uint32_t objectID);
  void                   updateTransform(uint32_t objectID, const glm::mat4& newTransform, const glm::vec4& newBoundsSphere);

  void syncToGpu(VkCommandBuffer cmd);

  [[nodiscard]] uint64_t getBufferAddress() const { return static_cast<uint64_t>(m_objectBuffer.address); }
  [[nodiscard]] uint64_t getCullBufferAddress() const { return static_cast<uint64_t>(m_cullObjectBuffer.address); }
  [[nodiscard]] VkBuffer getCullBufferHandle() const { return m_cullObjectBuffer.buffer; }
  [[nodiscard]] uint32_t getObjectCount() const { return static_cast<uint32_t>(m_gpuObjects.size()); }
  [[nodiscard]] bool     isDirty() const { return m_dirty; }
  [[nodiscard]] const std::vector<shaderio::GPUCullObject>& getOverlayObjects() const { return m_cullObjects; }

private:
  struct ObjectSlot
  {
    bool                           occupied{false};
    uint32_t                       denseIndex{0};
    GPUSceneRegistrationDesc       desc{};
    shaderio::GPUSceneObject       gpuObject{};
    shaderio::GPUCullObject        cullObject{};
  };

  void ensureCapacity(uint32_t requiredCount);
  void rebuildPackedObject(uint32_t objectID);
  void destroyBuffer(utils::Buffer& buffer);
  static shaderio::GPUSceneObject packSceneObject(const GPUSceneRegistrationDesc& desc);
  static shaderio::GPUCullObject  packCullObject(const GPUSceneRegistrationDesc& desc);

  VkDevice                     m_device{VK_NULL_HANDLE};
  VmaAllocator                 m_allocator{nullptr};
  utils::Buffer                m_objectBuffer{};
  utils::Buffer                m_cullObjectBuffer{};
  utils::Buffer                m_updateBuffer{};
  uint32_t                     m_capacity{0};
  bool                         m_dirty{false};
  std::vector<ObjectSlot>      m_slots{1};
  std::vector<uint32_t>        m_freeList;
  std::vector<uint32_t>        m_denseSlotIds;
  std::vector<shaderio::GPUSceneObject> m_gpuObjects;
  std::vector<shaderio::GPUCullObject>  m_cullObjects;
};

}  // namespace demo
