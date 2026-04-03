#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace demo::rhi {

//--------------------------------------------------------------------------------------------------
// Shader Stage Flags
//--------------------------------------------------------------------------------------------------
// Portable shader stage flags that map cleanly to backend-native stage masks
enum class ShaderStageFlagBits : uint32_t
{
  none     = 0,
  vertex   = 1u << 0u,
  fragment = 1u << 1u,
  compute  = 1u << 2u,
};

// Bitwise OR operator for ShaderStageFlagBits
constexpr ShaderStageFlagBits operator|(ShaderStageFlagBits lhs, ShaderStageFlagBits rhs)
{
  return static_cast<ShaderStageFlagBits>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

// Combined shader stage flags
constexpr ShaderStageFlagBits operator|=(ShaderStageFlagBits& lhs, ShaderStageFlagBits rhs)
{
  lhs = lhs | rhs;
  return lhs;
}

// Check if flag is set
constexpr bool operator&(ShaderStageFlagBits lhs, ShaderStageFlagBits rhs)
{
  return (static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs)) != 0;
}

//--------------------------------------------------------------------------------------------------
// Resource Types
//--------------------------------------------------------------------------------------------------
// Portable resource type enumeration for shader bindings
enum class ShaderResourceType : uint8_t
{
  unknown               = 0,
  sampler               = 1,  // Combined image sampler or standalone sampler
  texture               = 2,  // Sampled image (SRV)
  storageTexture        = 3,  // Storage image (UAV)
  uniformBuffer         = 4,  // UBO/Constant buffer (CBV)
  storageBuffer         = 5,  // SSBO/UAV
  inputAttachment       = 6,  // Input attachment (Vulkan-specific)
  accelerationStructure = 7,  // Ray tracing acceleration structure
};

//--------------------------------------------------------------------------------------------------
// Descriptor Type (Backend Mapping)
//--------------------------------------------------------------------------------------------------
// Maps shader resources to backend descriptor types
enum class DescriptorType : uint8_t
{
  sampler               = 0,
  combinedImageSampler  = 1,
  sampledImage          = 2,
  storageImage          = 3,
  uniformBuffer         = 4,
  storageBuffer         = 5,
  uniformBufferDynamic  = 6,
  storageBufferDynamic  = 7,
  inputAttachment       = 8,
  accelerationStructure = 9,
};

//--------------------------------------------------------------------------------------------------
// Resource Binding
//--------------------------------------------------------------------------------------------------
// Represents a single shader resource binding point
struct ShaderResourceBinding
{
  std::string         name;            // Variable name in shader (e.g., "sceneInfo")
  ShaderResourceType  resourceType;    // Type of resource
  DescriptorType      descriptorType;  // Backend descriptor type
  ShaderStageFlagBits stageFlags;      // Which stages access this resource
  uint32_t            set;             // Descriptor set index (logical)
  uint32_t            binding;         // Binding index within set (logical)
  uint32_t            arraySize;       // Array size (1 for non-arrayed resources)
  uint32_t            offset;          // Byte offset in uniform buffer (0 for most types)

  // Default constructor
  ShaderResourceBinding() = default;

  // Constructor with all fields
  ShaderResourceBinding(const std::string&  name_,
                        ShaderResourceType  resourceType_,
                        DescriptorType      descriptorType_,
                        ShaderStageFlagBits stageFlags_,
                        uint32_t            set_,
                        uint32_t            binding_,
                        uint32_t            arraySize_ = 1,
                        uint32_t            offset_    = 0)
      : name(name_)
      , resourceType(resourceType_)
      , descriptorType(descriptorType_)
      , stageFlags(stageFlags_)
      , set(set_)
      , binding(binding_)
      , arraySize(arraySize_)
      , offset(offset_)
  {
  }
};

//--------------------------------------------------------------------------------------------------
// Push Constant Range
//--------------------------------------------------------------------------------------------------
// Represents a push constant range for fast, small data uploads
struct PushConstantRange
{
  ShaderStageFlagBits stageFlags;  // Which stages use this range
  uint32_t            offset;      // Byte offset in push constant block
  uint32_t            size;        // Size in bytes

  PushConstantRange()
      : stageFlags(ShaderStageFlagBits::none)
      , offset(0)
      , size(0)
  {
  }

  PushConstantRange(ShaderStageFlagBits stageFlags_, uint32_t offset_, uint32_t size_)
      : stageFlags(stageFlags_)
      , offset(offset_)
      , size(size_)
  {
  }
};

//--------------------------------------------------------------------------------------------------
// Specialization Constant
//--------------------------------------------------------------------------------------------------
// Represents a specialization constant that can be set at pipeline creation time
struct SpecializationConstant
{
  uint32_t constantId;  // Constant ID (must match shader [[vk::constant_id(ID)]])
  uint32_t offset;      // Byte offset in specialization data
  uint32_t size;        // Size in bytes

  SpecializationConstant()
      : constantId(0)
      , offset(0)
      , size(0)
  {
  }

  SpecializationConstant(uint32_t constantId_, uint32_t offset_, uint32_t size_)
      : constantId(constantId_)
      , offset(offset_)
      , size(size_)
  {
  }
};

//--------------------------------------------------------------------------------------------------
// Shader Entry Point
//--------------------------------------------------------------------------------------------------
// Represents a single entry point within a shader module
struct ShaderEntryPoint
{
  std::string         name;   // Entry point function name (e.g., "vertexMain")
  ShaderStageFlagBits stage;  // Shader stage for this entry point

  ShaderEntryPoint() = default;

  ShaderEntryPoint(const std::string& name_, ShaderStageFlagBits stage_)
      : name(name_)
      , stage(stage_)
  {
  }
};

//--------------------------------------------------------------------------------------------------
// Shader Reflection Data
//--------------------------------------------------------------------------------------------------
// Complete reflection metadata for a compiled shader module
struct ShaderReflectionData
{
  std::string                         name;                     // Shader module name (optional)
  std::vector<ShaderEntryPoint>       entryPoints;              // All entry points in this module
  std::vector<ShaderResourceBinding>  resourceBindings;         // All resource bindings
  std::vector<PushConstantRange>      pushConstantRanges;       // Push constant ranges
  std::vector<SpecializationConstant> specializationConstants;  // Specialization constants

  // Total size of push constant block across all stages
  uint32_t pushConstantSize = 0;

  // Get total number of descriptor sets
  uint32_t getDescriptorSetCount() const
  {
    uint32_t maxSet = 0;
    for(const auto& binding : resourceBindings)
    {
      maxSet = (maxSet > binding.set) ? maxSet : binding.set;
    }
    return maxSet + 1;  // Sets are 0-indexed
  }

  // Get all bindings for a specific descriptor set
  std::vector<ShaderResourceBinding> getBindingsForSet(uint32_t set) const
  {
    std::vector<ShaderResourceBinding> result;
    for(const auto& binding : resourceBindings)
    {
      if(binding.set == set)
      {
        result.push_back(binding);
      }
    }
    return result;
  }
};

//--------------------------------------------------------------------------------------------------
// Shader Compile Result
//--------------------------------------------------------------------------------------------------
// Result of shader compilation including SPIR-V binary and reflection metadata
struct ShaderCompileResult
{
  std::vector<uint32_t> spirvCode;   // Compiled SPIR-V bytecode
  ShaderReflectionData  reflection;  // Reflection metadata from shader

  bool isSuccess() const { return !spirvCode.empty(); }

  size_t getByteSize() const { return spirvCode.size() * sizeof(uint32_t); }
};

//--------------------------------------------------------------------------------------------------
// Reflection Metadata Format
//--------------------------------------------------------------------------------------------------
// Version of reflection metadata format for compatibility checking
struct ReflectionMetadataVersion
{
  uint32_t major = 1;
  uint32_t minor = 0;
  uint32_t patch = 0;
};

}  // namespace demo::rhi