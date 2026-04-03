#pragma once

#include "../common/Common.h"

namespace utils {

class SamplerPool
{
public:
  SamplerPool() = default;
  ~SamplerPool() { assert(m_device == VK_NULL_HANDLE && "Missing deinit()"); }
  // Initialize the sampler pool with the device reference, then we can later acquire samplers
  void init(VkDevice device) { m_device = device; }
  // Destroy internal resources and reset its initial state
  void deinit()
  {
    for(const auto& entry : m_samplerMap)
    {
      vkDestroySampler(m_device, entry.second, nullptr);
    }
    m_samplerMap.clear();
    *this = {};
  }
  // Get or create VkSampler based on VkSamplerCreateInfo
  VkSampler acquireSampler(const VkSamplerCreateInfo& createInfo)
  {
    if(auto it = m_samplerMap.find(createInfo); it != m_samplerMap.end())
    {
      // If found, return existing sampler
      return it->second;
    }

    // Otherwise, create a new sampler
    VkSampler newSampler     = createSampler(createInfo);
    m_samplerMap[createInfo] = newSampler;
    return newSampler;
  }

  void releaseSampler(VkSampler sampler)
  {
    for(auto it = m_samplerMap.begin(); it != m_samplerMap.end();)
    {
      if(it->second == sampler)
      {
        vkDestroySampler(m_device, it->second, nullptr);
        it = m_samplerMap.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

private:
  VkDevice m_device{};

  struct SamplerCreateInfoHash
  {
    std::size_t operator()(const VkSamplerCreateInfo& info) const
    {
      std::size_t seed{0};
      seed = hashCombine(seed, info.magFilter);
      seed = hashCombine(seed, info.minFilter);
      seed = hashCombine(seed, info.mipmapMode);
      seed = hashCombine(seed, info.addressModeU);
      seed = hashCombine(seed, info.addressModeV);
      seed = hashCombine(seed, info.addressModeW);
      seed = hashCombine(seed, info.mipLodBias);
      seed = hashCombine(seed, info.anisotropyEnable);
      seed = hashCombine(seed, info.maxAnisotropy);
      seed = hashCombine(seed, info.compareEnable);
      seed = hashCombine(seed, info.compareOp);
      seed = hashCombine(seed, info.minLod);
      seed = hashCombine(seed, info.maxLod);
      seed = hashCombine(seed, info.borderColor);
      seed = hashCombine(seed, info.unnormalizedCoordinates);

      return seed;
    }
  };

  struct SamplerCreateInfoEqual
  {
    bool operator()(const VkSamplerCreateInfo& lhs, const VkSamplerCreateInfo& rhs) const
    {
      return std::memcmp(&lhs, &rhs, sizeof(VkSamplerCreateInfo)) == 0;
    }
  };

  // Stores unique samplers with their corresponding VkSamplerCreateInfo
  std::unordered_map<VkSamplerCreateInfo, VkSampler, SamplerCreateInfoHash, SamplerCreateInfoEqual> m_samplerMap;

  // Internal function to create a new VkSampler
  const VkSampler createSampler(const VkSamplerCreateInfo& createInfo) const
  {
    ASSERT(m_device, "Initialization was missing");
    VkSampler sampler{};
    VK_CHECK(vkCreateSampler(m_device, &createInfo, nullptr, &sampler));
    return sampler;
  }
};

}
