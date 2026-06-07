#pragma once

#include <cstdint>

namespace demo::rhi {

// Capability tiers are backend-neutral contracts consumed by the renderer.
// Core is the v1 mandatory floor; extension tiers are optional opt-ins.
enum class CapabilityTier : uint8_t
{
  Core = 0,
  ExtensionAsyncCompute,
  ExtensionMeshShader,
  ExtensionRayTracing,
  DescriptorHeap,
  Residency,
  PipelineCompiler,
  MultiQueue,
};

// Device capability report expressed in portable vocabulary.
// Fields are intentionally independent from backend-native feature structs.
struct CapabilityReport
{
  // v1 mandatory floor
  bool coreGraphics{false};
  bool coreCompute{false};
  bool coreBindless{false};

  // Optional extension tiers
  bool extensionAsyncCompute{false};
  bool extensionMeshShader{false};
  bool extensionRayTracing{false};
  bool descriptorHeap{false};
  bool residency{false};
  bool pipelineCompiler{false};
  bool multiQueue{false};
};

// Requirements requested at device creation.
// Core defaults to required=true to enforce deterministic v1 behavior.
struct CapabilityRequirements
{
  bool requireCoreGraphics{true};
  bool requireCoreCompute{true};
  bool requireCoreBindless{true};

  bool requireExtensionAsyncCompute{false};
  bool requireExtensionMeshShader{false};
  bool requireExtensionRayTracing{false};
  bool requireDescriptorHeap{false};
  bool requireResidency{false};
  bool requirePipelineCompiler{false};
  bool requireMultiQueue{false};
};

// Stable failure codes for capability negotiation.
// Ordering is deliberate and used by evaluateCapabilityRequirements() for determinism.
enum class RHICapabilityError : uint8_t
{
  None = 0,
  MissingCoreGraphics,
  MissingCoreCompute,
  MissingCoreBindless,
  MissingExtensionAsyncCompute,
  MissingExtensionMeshShader,
  MissingExtensionRayTracing,
  MissingDescriptorHeap,
  MissingResidency,
  MissingPipelineCompiler,
  MissingMultiQueue,
};

constexpr bool supportsTier(const CapabilityReport& report, CapabilityTier tier)
{
  switch(tier)
  {
    case CapabilityTier::Core:
      return report.coreGraphics && report.coreCompute && report.coreBindless;
    case CapabilityTier::ExtensionAsyncCompute:
      return report.extensionAsyncCompute;
    case CapabilityTier::ExtensionMeshShader:
      return report.extensionMeshShader;
    case CapabilityTier::ExtensionRayTracing:
      return report.extensionRayTracing;
    case CapabilityTier::DescriptorHeap:
      return report.descriptorHeap;
    case CapabilityTier::Residency:
      return report.residency;
    case CapabilityTier::PipelineCompiler:
      return report.pipelineCompiler;
    case CapabilityTier::MultiQueue:
      return report.multiQueue;
    default:
      return false;
  }
}

constexpr RHICapabilityError evaluateCapabilityRequirements(const CapabilityReport& report, const CapabilityRequirements& requirements)
{
  if(requirements.requireCoreGraphics && !report.coreGraphics)
  {
    return RHICapabilityError::MissingCoreGraphics;
  }
  if(requirements.requireCoreCompute && !report.coreCompute)
  {
    return RHICapabilityError::MissingCoreCompute;
  }
  if(requirements.requireCoreBindless && !report.coreBindless)
  {
    return RHICapabilityError::MissingCoreBindless;
  }
  if(requirements.requireExtensionAsyncCompute && !report.extensionAsyncCompute)
  {
    return RHICapabilityError::MissingExtensionAsyncCompute;
  }
  if(requirements.requireExtensionMeshShader && !report.extensionMeshShader)
  {
    return RHICapabilityError::MissingExtensionMeshShader;
  }
  if(requirements.requireExtensionRayTracing && !report.extensionRayTracing)
  {
    return RHICapabilityError::MissingExtensionRayTracing;
  }
  if(requirements.requireDescriptorHeap && !report.descriptorHeap)
  {
    return RHICapabilityError::MissingDescriptorHeap;
  }
  if(requirements.requireResidency && !report.residency)
  {
    return RHICapabilityError::MissingResidency;
  }
  if(requirements.requirePipelineCompiler && !report.pipelineCompiler)
  {
    return RHICapabilityError::MissingPipelineCompiler;
  }
  if(requirements.requireMultiQueue && !report.multiQueue)
  {
    return RHICapabilityError::MissingMultiQueue;
  }
  return RHICapabilityError::None;
}

constexpr const char* toString(RHICapabilityError error)
{
  switch(error)
  {
    case RHICapabilityError::None:
      return "none";
    case RHICapabilityError::MissingCoreGraphics:
      return "missing_core_graphics";
    case RHICapabilityError::MissingCoreCompute:
      return "missing_core_compute";
    case RHICapabilityError::MissingCoreBindless:
      return "missing_core_bindless";
    case RHICapabilityError::MissingExtensionAsyncCompute:
      return "missing_extension_async_compute";
    case RHICapabilityError::MissingExtensionMeshShader:
      return "missing_extension_mesh_shader";
    case RHICapabilityError::MissingExtensionRayTracing:
      return "missing_extension_ray_tracing";
    case RHICapabilityError::MissingDescriptorHeap:
      return "missing_descriptor_heap";
    case RHICapabilityError::MissingResidency:
      return "missing_residency";
    case RHICapabilityError::MissingPipelineCompiler:
      return "missing_pipeline_compiler";
    case RHICapabilityError::MissingMultiQueue:
      return "missing_multi_queue";
    default:
      return "unknown_capability_error";
  }
}

}  // namespace demo::rhi
