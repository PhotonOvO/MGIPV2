#include "EditorOverlayPass.h"

#include "../editor/GizmoGeometry.h"
#include "../../common/logger.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

namespace demo {
namespace {

using editor::EditorOverlayDrawConstants;
using editor::EditorOverlayFrameUniforms;
using editor::EditorOverlayGizmoVertex;
using editor::EditorOverlayLineInstance;

constexpr std::uint32_t kGridPipelineVariant = 0x7601u;
constexpr std::uint32_t kAxesPipelineVariant = 0x7602u;
constexpr std::uint32_t kGizmoSolidForwardPipelineVariant = 0x7611u;
constexpr std::uint32_t kGizmoDitherForwardPipelineVariant = 0x7612u;
constexpr std::uint32_t kGizmoPlaneForwardPipelineVariant = 0x7613u;
constexpr std::uint32_t kGizmoSolidReversePipelineVariant = 0x7621u;
constexpr std::uint32_t kGizmoDitherReversePipelineVariant = 0x7622u;
constexpr std::uint32_t kGizmoPlaneReversePipelineVariant = 0x7623u;
constexpr std::uint64_t kFrameUniformBufferCapacity =
    (sizeof(EditorOverlayFrameUniforms) + 255u) & ~255ull;

static_assert(kFrameUniformBufferCapacity == 256u);

static_assert(editor::kEditorOverlayClipDepthZeroToOne ==
              static_cast<std::uint32_t>(
                  editor::EditorClipDepthRange::zeroToOne));
static_assert(editor::kEditorOverlayClipDepthNegativeOneToOne ==
              static_cast<std::uint32_t>(
                  editor::EditorClipDepthRange::negativeOneToOne));
static_assert(editor::kEditorOverlaySurfaceRotate90 ==
              static_cast<std::uint32_t>(
                  editor::EditorViewportSurfaceTransform::rotate90));
static_assert(editor::kEditorOverlaySurfaceRotate270 ==
              static_cast<std::uint32_t>(
                  editor::EditorViewportSurfaceTransform::rotate270));
static_assert(editor::kEditorOverlaySurfaceMirrorHorizontalRotate90 ==
              static_cast<std::uint32_t>(
                  editor::EditorViewportSurfaceTransform::
                      mirrorHorizontalRotate90));
static_assert(editor::kEditorOverlaySurfaceMirrorHorizontalRotate270 ==
              static_cast<std::uint32_t>(
                  editor::EditorViewportSurfaceTransform::
                      mirrorHorizontalRotate270));

[[nodiscard]] rhi::RHIResult fail(rhi::RHIErrorCode code,
                                  const char *message) noexcept {
  return rhi::RHIResult::fail(code, message);
}

[[nodiscard]] bool finite(double value) noexcept {
  return std::isfinite(value);
}

[[nodiscard]] std::optional<float> checkedFloat(double value) noexcept {
  constexpr double maximum =
      static_cast<double>((std::numeric_limits<float>::max)());
  if (!finite(value) || value < -maximum || value > maximum) {
    return std::nullopt;
  }
  const float narrowed = static_cast<float>(value);
  return std::isfinite(narrowed) ? std::optional<float>(narrowed)
                                 : std::nullopt;
}

[[nodiscard]] std::optional<glm::vec3>
checkedVec3(editor::OverlayVec3 value) noexcept {
  const auto x = checkedFloat(value.x);
  const auto y = checkedFloat(value.y);
  const auto z = checkedFloat(value.z);
  return x && y && z ? std::optional<glm::vec3>(glm::vec3(*x, *y, *z))
                     : std::nullopt;
}

[[nodiscard]] std::optional<std::array<glm::vec4, 4>>
checkedRows(const editor::OverlayMatrix4 &matrix) noexcept {
  std::array<glm::vec4, 4> rows{};
  for (std::size_t row = 0u; row < rows.size(); ++row) {
    std::array<float, 4> values{};
    for (std::size_t column = 0u; column < values.size(); ++column) {
      const auto narrowed = checkedFloat(matrix.elements[row * 4u + column]);
      if (!narrowed) {
        return std::nullopt;
      }
      values[column] = *narrowed;
    }
    rows[row] = glm::vec4(values[0], values[1], values[2], values[3]);
  }
  return rows;
}

[[nodiscard]] bool validSampleCount(rhi::SampleCount value) noexcept {
  switch (value) {
  case rhi::SampleCount::count1:
  case rhi::SampleCount::count2:
  case rhi::SampleCount::count4:
  case rhi::SampleCount::count8:
    return true;
  }
  return false;
}

[[nodiscard]] bool matchesGeneration(
    const editor::OverlayGenerationBinding &binding,
    const editor::EditorViewportMetrics &viewport) noexcept {
  return binding.viewportGeneration == viewport.viewportGeneration &&
         binding.projection == viewport.projection &&
         binding.view == viewport.view;
}

[[nodiscard]] bool validGridLine(const editor::GridLinePacket &line,
                                 bool axis) noexcept {
  if (!finite(line.alpha) || line.alpha < 0.0 || line.alpha > 1.0) {
    return false;
  }
  if (axis) {
    return (line.kind == editor::GridLineKind::xAxis ||
            line.kind == editor::GridLineKind::zAxis) &&
           line.blend == editor::GridBlendToken::alphaOver;
  }
  return (line.kind == editor::GridLineKind::parallelX ||
          line.kind == editor::GridLineKind::parallelZ) &&
         line.blend == editor::GridBlendToken::additive;
}

[[nodiscard]] glm::vec4 gridColor(const editor::GridLinePacket &line,
                                  bool axis) noexcept {
  if (axis) {
    return line.kind == editor::GridLineKind::xAxis
               ? glm::vec4(0.89f, 0.294f, 0.369f, 1.0f)
               : glm::vec4(0.25f, 0.451f, 0.851f, 1.0f);
  }
  const float level = static_cast<float>(line.lodIndex % 4u);
  const float intensity = std::clamp(0.56f - level * 0.07f, 0.28f, 0.56f);
  return glm::vec4(intensity, intensity, intensity, 1.0f);
}

[[nodiscard]] std::optional<EditorOverlayLineInstance>
encodeLine(const editor::GridLinePacket &line, bool axis) noexcept {
  const auto start = checkedVec3(line.start);
  const auto end = checkedVec3(line.end);
  if (!start || !end || !validGridLine(line, axis)) {
    return std::nullopt;
  }
  EditorOverlayLineInstance result;
  result.startWidth = glm::vec4(
      *start, editor::kEditorOverlayLineWidthLogicalPixels);
  result.endAlpha = glm::vec4(*end, static_cast<float>(line.alpha));
  result.color = gridColor(line, axis);
  return result;
}

[[nodiscard]] glm::vec4 gizmoBaseColor(editor::GizmoColorToken token) noexcept {
  switch (token) {
  case editor::GizmoColorToken::xAxis:
    return {0.89f, 0.294f, 0.369f, 1.0f};
  case editor::GizmoColorToken::yAxis:
    return {0.45f, 0.819f, 0.263f, 1.0f};
  case editor::GizmoColorToken::zAxis:
    return {0.25f, 0.451f, 0.851f, 1.0f};
  case editor::GizmoColorToken::xyPlane:
    return {0.78f, 0.78f, 0.25f, 0.45f};
  case editor::GizmoColorToken::xzPlane:
    return {0.78f, 0.25f, 0.78f, 0.45f};
  case editor::GizmoColorToken::yzPlane:
    return {0.25f, 0.78f, 0.78f, 0.45f};
  case editor::GizmoColorToken::uniform:
    return {0.65f, 0.65f, 0.65f, 1.0f};
  }
  return {};
}

[[nodiscard]] glm::vec4
gizmoColor(const editor::GizmoComponentDrawPacket &draw) noexcept {
  glm::vec4 color = gizmoBaseColor(draw.color);
  if (draw.state != editor::GizmoVisualStateToken::normal) {
    for (glm::length_t channel = 0; channel < 3; ++channel) {
      const float bright = std::clamp(color[channel] * 2.5f, 0.0f, 1.0f);
      color[channel] = color[channel] * 0.1f + bright * 0.9f;
    }
  }
  return color;
}

[[nodiscard]] bool planeComponent(editor::GizmoComponent component) noexcept {
  return component == editor::GizmoComponent::translateXY ||
         component == editor::GizmoComponent::translateXZ ||
         component == editor::GizmoComponent::translateYZ;
}

template <typename Callback>
[[nodiscard]] bool callNoThrow(Callback &&callback) noexcept {
  try {
    std::forward<Callback>(callback)();
    return true;
  } catch (...) {
    return false;
  }
}

[[nodiscard]] rhi::RHIResult writeBuffer(rhi::Device &device,
                                         rhi::BufferHandle buffer,
                                         std::span<const std::byte> bytes) noexcept {
  if (buffer.isNull() || bytes.empty()) {
    return fail(rhi::RHIErrorCode::invalidArgument,
                "Editor overlay buffer write is empty");
  }
  bool mappedOpen = false;
  try {
    const rhi::Result<rhi::MappedBufferRange> mapped = device.mapBufferRange(
        buffer, {.offset = 0u,
                 .size = static_cast<std::uint64_t>(bytes.size()),
                 .mode = rhi::BufferMapMode::write,
                 .invalidateBeforeRead = false});
    if (!mapped.succeeded() || mapped.value.data == nullptr ||
        mapped.value.size < bytes.size()) {
      return fail(mapped.error.isError() ? mapped.error.code
                                         : rhi::RHIErrorCode::backendFailure,
                  "Editor overlay buffer mapping failed");
    }
    mappedOpen = true;
    std::memcpy(mapped.value.data, bytes.data(), bytes.size());
    const rhi::RHIResult flushed = device.flushMappedBufferRange(
        buffer, 0u, static_cast<std::uint64_t>(bytes.size()));
    const bool unmapped =
        callNoThrow([&] { device.unmapBuffer(buffer); });
    mappedOpen = false;
    if (!unmapped) {
      return fail(rhi::RHIErrorCode::backendFailure,
                  "Editor overlay buffer unmap failed");
    }
    return flushed.succeeded()
               ? rhi::RHIResult::ok()
               : fail(flushed.error.code,
                      "Editor overlay buffer flush failed");
  } catch (...) {
    if (mappedOpen) {
      (void)callNoThrow([&] { device.unmapBuffer(buffer); });
    }
    return fail(rhi::RHIErrorCode::backendFailure,
                "Editor overlay buffer write threw");
  }
}

template <typename T>
[[nodiscard]] std::span<const std::byte> bytesOf(const T &value) noexcept {
  return std::as_bytes(std::span<const T>(&value, 1u));
}

} // namespace

bool EditorOverlayPass::SurfaceResources::valid() const noexcept {
  if (signature.outputFormat == rhi::TextureFormat::undefined ||
      signature.helperDepthFormat == rhi::TextureFormat::undefined ||
      extent.width == 0u || extent.height == 0u || helperDepth.isNull() ||
      helperDepthView.isNull()) {
    return false;
  }
  return std::all_of(pipelines.begin(), pipelines.end(),
                     [](rhi::PipelineHandle value) {
                       return !value.isNull();
                     });
}

EditorOverlayPass::~EditorOverlayPass() { (void)destroyAll(); }

void EditorOverlayPass::recordError(EditorOverlayPassError error) const noexcept {
  m_diagnostics.lastError = error;
}

rhi::RHIResult EditorOverlayPass::initialize(
    rhi::Device &device,
    const EditorOverlayPassCreateInfo &createInfo) noexcept {
  ++m_diagnostics.initializeCalls;
  if (m_device != nullptr) {
    recordError(EditorOverlayPassError::alreadyInitialized);
    return fail(rhi::RHIErrorCode::invalidState,
                "Editor overlay pass is already initialized");
  }
  if (createInfo.frameSlotCount == 0u || createInfo.shaderData.empty() ||
      createInfo.shaderFormat == rhi::ShaderIRFormat::unknown ||
      createInfo.helperDepthFormat == rhi::TextureFormat::undefined) {
    recordError(EditorOverlayPassError::invalidArgument);
    return fail(rhi::RHIErrorCode::invalidArgument,
                "Editor overlay initialization is invalid");
  }
  rhi::Queue *queue = device.getQueue(rhi::QueueClass::graphics);
  if (queue == nullptr || !queue->info().available ||
      queue->identity().isValid() == false) {
    recordError(EditorOverlayPassError::invalidArgument);
    return fail(rhi::RHIErrorCode::unsupported,
                "Editor overlay requires a graphics queue");
  }

  m_device = &device;
  m_graphicsQueue = queue;
  m_helperDepthFormat = createInfo.helperDepthFormat;
  const rhi::RHIResult statics = createStaticResources(createInfo);
  if (!statics.succeeded()) {
    recordError(EditorOverlayPassError::resourceFailure);
    (void)destroyAll();
    return statics;
  }
  const rhi::RHIResult frames =
      createFrameResources(createInfo.frameSlotCount);
  if (!frames.succeeded()) {
    recordError(EditorOverlayPassError::resourceFailure);
    (void)destroyAll();
    return frames;
  }
  m_diagnostics.lastError = EditorOverlayPassError::none;
  return rhi::RHIResult::ok();
}

rhi::RHIResult EditorOverlayPass::createStaticResources(
    const EditorOverlayPassCreateInfo &createInfo) noexcept {
  try {
    m_shaderLibrary = m_device->createShaderLibrary({
        .format = createInfo.shaderFormat,
        .data = createInfo.shaderData,
        .debugName = "editor-overlay",
    });
    if (m_shaderLibrary.isNull()) {
      return fail(rhi::RHIErrorCode::backendFailure,
                  "Editor overlay shader library creation failed");
    }

    m_sceneDepthSampler = m_device->createSampler({
        .magFilter = rhi::Filter::nearest,
        .minFilter = rhi::Filter::nearest,
        .mipmapMode = rhi::MipmapMode::nearest,
        .addressModeU = rhi::AddressMode::clampToEdge,
        .addressModeV = rhi::AddressMode::clampToEdge,
        .addressModeW = rhi::AddressMode::clampToEdge,
        .debugName = "EditorOverlaySceneDepthSampler",
    });
    if (m_sceneDepthSampler.isNull()) {
      return fail(rhi::RHIErrorCode::backendFailure,
                  "Editor overlay sampler creation failed");
    }

    const std::array bindings{
        rhi::ArgumentBinding{
            .binding = editor::kEditorOverlayFrameBinding,
            .type = rhi::ArgumentType::uniformBuffer,
            .visibility = rhi::ShaderStage::allGraphics,
        },
        rhi::ArgumentBinding{
            .binding = editor::kEditorOverlaySceneDepthBinding,
            .type = rhi::ArgumentType::combinedImageSampler,
            .visibility = rhi::ShaderStage::fragment,
        },
    };
    m_argumentLayout = m_device->createArgumentLayout({
        .bindings = bindings,
        .debugName = "EditorOverlayFrameLayout",
    });
    if (m_argumentLayout.isNull()) {
      return fail(rhi::RHIErrorCode::backendFailure,
                  "Editor overlay argument layout creation failed");
    }

    const editor::GizmoGeometryAtlas &atlas =
        editor::canonicalGizmoGeometry();
    if (atlas.identity().vertexCount == 0u ||
        atlas.identity().indexCount == 0u || atlas.vertices().empty() ||
        atlas.indices().empty()) {
      return fail(rhi::RHIErrorCode::invalidState,
                  "Canonical editor gizmo geometry is empty");
    }
    std::vector<EditorOverlayGizmoVertex> vertices;
    vertices.reserve(atlas.vertices().size());
    for (const editor::GizmoVertex &source : atlas.vertices()) {
      const auto position = checkedVec3(source.position);
      const auto normal = checkedVec3(source.normal);
      if (!position || !normal) {
        return fail(rhi::RHIErrorCode::invalidState,
                    "Canonical editor gizmo geometry is non-finite");
      }
      vertices.push_back({*position, *normal});
    }

    m_gizmoVertexBuffer = m_device->createBuffer({
        .size = static_cast<std::uint64_t>(vertices.size()) *
                sizeof(EditorOverlayGizmoVertex),
        .usage = rhi::BufferUsageFlags::vertex,
        .memoryUsage = rhi::MemoryUsage::cpuToGpu,
        .debugName = "EditorOverlayGizmoVertices",
    });
    m_gizmoIndexBuffer = m_device->createBuffer({
        .size = static_cast<std::uint64_t>(atlas.indices().size()) *
                sizeof(std::uint32_t),
        .usage = rhi::BufferUsageFlags::index,
        .memoryUsage = rhi::MemoryUsage::cpuToGpu,
        .debugName = "EditorOverlayGizmoIndices",
    });
    if (m_gizmoVertexBuffer.isNull() || m_gizmoIndexBuffer.isNull()) {
      return fail(rhi::RHIErrorCode::backendFailure,
                  "Editor overlay atlas buffer creation failed");
    }
    const rhi::RHIResult vertexWrite = writeBuffer(
        *m_device, m_gizmoVertexBuffer,
        std::as_bytes(std::span<const EditorOverlayGizmoVertex>(vertices)));
    if (!vertexWrite.succeeded()) {
      return vertexWrite;
    }
    return writeBuffer(
        *m_device, m_gizmoIndexBuffer,
        std::as_bytes(std::span<const std::uint32_t>(atlas.indices())));
  } catch (...) {
    return fail(rhi::RHIErrorCode::backendFailure,
                "Editor overlay static resource creation threw");
  }
}

rhi::RHIResult
EditorOverlayPass::createFrameResources(std::uint32_t frameSlotCount) noexcept {
  try {
    m_frames.resize(frameSlotCount);
    for (FrameSlot &slot : m_frames) {
      slot.encodedLines.reserve(
          editor::kEditorOverlayMaximumLineInstanceCount);
      slot.uniformBuffer = m_device->createBuffer({
          .size = kFrameUniformBufferCapacity,
          .usage = rhi::BufferUsageFlags::uniform,
          .memoryUsage = rhi::MemoryUsage::cpuToGpu,
          .allowArgumentTableBinding = true,
          .debugName = "EditorOverlayFrameUniform",
      });
      slot.lineBuffer = m_device->createBuffer({
          .size = static_cast<std::uint64_t>(
                      editor::kEditorOverlayMaximumLineInstanceCount) *
                  sizeof(EditorOverlayLineInstance),
          .usage = rhi::BufferUsageFlags::vertex,
          .memoryUsage = rhi::MemoryUsage::cpuToGpu,
          .debugName = "EditorOverlayLineInstances",
      });
      if (slot.uniformBuffer.isNull() || slot.lineBuffer.isNull()) {
        return fail(rhi::RHIErrorCode::backendFailure,
                    "Editor overlay frame buffer creation failed");
      }
      const rhi::RHIResult table = ensureFrameTable(slot);
      if (!table.succeeded()) {
        return table;
      }
    }
    return rhi::RHIResult::ok();
  } catch (...) {
    return fail(rhi::RHIErrorCode::backendFailure,
                "Editor overlay frame resource creation threw");
  }
}

rhi::RHIResult EditorOverlayPass::ensureFrameTable(FrameSlot &slot) noexcept {
  if (!slot.argumentTable.isNull() && !slot.tablePoisoned) {
    return rhi::RHIResult::ok();
  }
  try {
    if (!slot.argumentTable.isNull()) {
      m_device->destroyArgumentTable(slot.argumentTable);
      slot.argumentTable = {};
    }
    slot.argumentTable = m_device->createArgumentTable({
        .layout = m_argumentLayout,
        .lifetime = rhi::ArgumentTableLifetime::frameLocal,
        .debugName = "EditorOverlayFrameTable",
    });
    if (slot.argumentTable.isNull()) {
      return fail(rhi::RHIErrorCode::backendFailure,
                  "Editor overlay frame table creation failed");
    }
    slot.tablePoisoned = false;
    ++m_diagnostics.frameTableRebuilds;
    return rhi::RHIResult::ok();
  } catch (...) {
    return fail(rhi::RHIErrorCode::backendFailure,
                "Editor overlay frame table creation threw");
  }
}

rhi::RHIResult EditorOverlayPass::ensureSurfaceResources(
    const EditorOverlayPassFrame &frame) noexcept {
  const SurfaceSignature required{
      frame.targets.outputFormat, m_helperDepthFormat,
      frame.targets.outputSamples};
  const rhi::Extent2D requiredExtent{frame.viewport.colorAttachment.width,
                                     frame.viewport.colorAttachment.height};
  const bool signatureChanged = !m_surface.valid() ||
                                m_surface.signature != required;
  const bool extentChanged = !m_surface.valid() ||
                             m_surface.extent.width != requiredExtent.width ||
                             m_surface.extent.height != requiredExtent.height;
  if (!signatureChanged && !extentChanged) {
    return rhi::RHIResult::ok();
  }
  for (const FrameSlot &slot : m_frames) {
    if (slot.state == FrameState::prepared) {
      return fail(rhi::RHIErrorCode::invalidState,
                  "Editor overlay surface change has an unresolved frame");
    }
  }
  if (!m_device->isFormatSupported(
          m_helperDepthFormat,
          rhi::FormatFeatureFlag::depthStencilAttachment)) {
    return fail(rhi::RHIErrorCode::unsupported,
                "Editor overlay helper depth format is unsupported");
  }

  SurfaceResources candidate = m_surface;
  candidate.signature = required;
  candidate.extent = requiredExtent;
  candidate.helperDepth = {};
  candidate.helperDepthView = {};
  candidate.helperState = rhi::ResourceState::Undefined;
  if (signatureChanged) {
    candidate.pipelines = {};
  }

  try {
    candidate.helperDepth = m_device->createTexture({
        .dimension = rhi::TextureDimension::e2D,
        .format = m_helperDepthFormat,
        .usage = rhi::TextureUsageFlags::depthAttachment,
        .extent = {requiredExtent.width, requiredExtent.height, 1u},
        .mipLevels = 1u,
        .arrayLayers = 1u,
        .sampleCount = required.samples,
        .memoryUsage = rhi::MemoryUsage::transientAttachment,
        .debugName = "EditorOverlayHelperDepth",
    });
    if (candidate.helperDepth.isNull()) {
      return fail(rhi::RHIErrorCode::backendFailure,
                  "Editor overlay helper depth creation failed");
    }
    candidate.helperDepthView = m_device->createTextureView({
        .image = candidate.helperDepth,
        .format = m_helperDepthFormat,
        .viewType = rhi::ImageViewType::e2D,
        .aspect = rhi::TextureAspect::depth,
        .debugName = "EditorOverlayHelperDepthView",
    });
    if (candidate.helperDepthView.isNull()) {
      m_device->destroyTexture(candidate.helperDepth);
      return fail(rhi::RHIErrorCode::backendFailure,
                  "Editor overlay helper depth view creation failed");
    }
    ++m_diagnostics.helperDepthCreations;

    if (signatureChanged) {
      const std::array argumentSlots{
          rhi::PipelineArgumentSlotDesc{
              .slot = editor::kEditorOverlayArgumentSlot,
              .layout = m_argumentLayout,
              .visibility = rhi::ShaderStage::allGraphics,
              .debugName = "EditorOverlayFrame",
          },
      };
      const std::array rootBindings{
          rhi::RootBindingDesc{
              .slot = editor::kEditorOverlayRootSlot,
              .kind = rhi::RootBindingKind::constants,
              .visibility = rhi::ShaderStage::allGraphics,
              .size = sizeof(EditorOverlayDrawConstants),
              .alignment = alignof(EditorOverlayDrawConstants),
              .debugName = "EditorOverlayDraw",
          },
      };
      const rhi::PipelineBindingSchemaDesc schema{
          .argumentSlots = argumentSlots,
          .rootBindings = rootBindings,
      };
      const std::array lineStages{
          rhi::ShaderEntry{
              .stage = rhi::ShaderStage::vertex,
              .library = m_shaderLibrary,
              .entryPoint = "editorOverlayLineVertexMain",
          },
          rhi::ShaderEntry{
              .stage = rhi::ShaderStage::fragment,
              .library = m_shaderLibrary,
              .entryPoint = "editorOverlayFragmentMain",
          },
      };
      const std::array gizmoStages{
          rhi::ShaderEntry{
              .stage = rhi::ShaderStage::vertex,
              .library = m_shaderLibrary,
              .entryPoint = "editorOverlayGizmoVertexMain",
          },
          rhi::ShaderEntry{
              .stage = rhi::ShaderStage::fragment,
              .library = m_shaderLibrary,
              .entryPoint = "editorOverlayFragmentMain",
          },
      };
      const std::array lineBindings{
          rhi::VertexBindingDesc{
              .binding = 0u,
              .stride = sizeof(EditorOverlayLineInstance),
              .inputRate = rhi::VertexInputRate::perInstance,
          },
      };
      const std::array lineAttributes{
          rhi::VertexAttributeDesc{.location = 0u,
                                   .binding = 0u,
                                   .format = rhi::VertexFormat::r32g32b32a32Sfloat,
                                   .offset = offsetof(EditorOverlayLineInstance,
                                                      startWidth)},
          rhi::VertexAttributeDesc{.location = 1u,
                                   .binding = 0u,
                                   .format = rhi::VertexFormat::r32g32b32a32Sfloat,
                                   .offset = offsetof(EditorOverlayLineInstance,
                                                      endAlpha)},
          rhi::VertexAttributeDesc{.location = 2u,
                                   .binding = 0u,
                                   .format = rhi::VertexFormat::r32g32b32a32Sfloat,
                                   .offset = offsetof(EditorOverlayLineInstance,
                                                      color)},
      };
      const std::array gizmoBindings{
          rhi::VertexBindingDesc{
              .binding = 0u,
              .stride = sizeof(EditorOverlayGizmoVertex),
              .inputRate = rhi::VertexInputRate::perVertex,
          },
      };
      const std::array gizmoAttributes{
          rhi::VertexAttributeDesc{.location = 0u,
                                   .binding = 0u,
                                   .format = rhi::VertexFormat::r32g32b32Sfloat,
                                   .offset = offsetof(EditorOverlayGizmoVertex,
                                                      position)},
          rhi::VertexAttributeDesc{.location = 1u,
                                   .binding = 0u,
                                   .format = rhi::VertexFormat::r32g32b32Sfloat,
                                   .offset = offsetof(EditorOverlayGizmoVertex,
                                                      normal)},
      };
      const std::array dynamicStates{rhi::DynamicState::viewport,
                                     rhi::DynamicState::scissor};
      const std::array colorFormats{required.outputFormat};

      const rhi::BlendAttachmentState gridBlend{
          .blendEnable = true,
          .srcColorBlendFactor = rhi::BlendFactor::srcAlpha,
          .dstColorBlendFactor = rhi::BlendFactor::one,
          .colorBlendOp = rhi::BlendOp::add,
          .srcAlphaBlendFactor = rhi::BlendFactor::one,
          .dstAlphaBlendFactor = rhi::BlendFactor::one,
          .alphaBlendOp = rhi::BlendOp::add,
          .colorWriteMask = rhi::ColorComponentFlags::all,
      };
      const rhi::BlendAttachmentState axesBlend{
          .blendEnable = true,
          .srcColorBlendFactor = rhi::BlendFactor::srcAlpha,
          .dstColorBlendFactor = rhi::BlendFactor::oneMinusSrcAlpha,
          .colorBlendOp = rhi::BlendOp::add,
          .srcAlphaBlendFactor = rhi::BlendFactor::one,
          .dstAlphaBlendFactor = rhi::BlendFactor::oneMinusSrcAlpha,
          .alphaBlendOp = rhi::BlendOp::add,
          .colorWriteMask = rhi::ColorComponentFlags::all,
      };
      const rhi::BlendAttachmentState opaqueBlend{
          .blendEnable = false,
          .colorWriteMask = rhi::ColorComponentFlags::all,
      };

      const auto createPipeline = [&](PipelineFamily family,
                                      std::span<const rhi::ShaderEntry> stages,
                                      rhi::VertexInputLayoutDesc vertexInput,
                                      rhi::BlendAttachmentState blend,
                                      bool depthEnabled,
                                      rhi::CompareOp compare,
                                      std::uint32_t variant)
          -> rhi::PipelineHandle {
        const std::array blends{blend};
        const rhi::GraphicsPipelineDesc desc{
            .shaderStages = stages,
            .vertexInput = vertexInput,
            .rasterState = {.topology = rhi::PrimitiveTopology::triangleList,
                            .cullMode = rhi::CullMode::none,
                            .sampleCount = required.samples},
            .depthState = {.depthTestEnable = depthEnabled,
                           .depthWriteEnable = depthEnabled,
                           .depthCompareOp = compare},
            .blendStates = blends,
            .dynamicStates = dynamicStates,
            .renderingInfo = {.colorFormats = colorFormats,
                              .depthFormat = required.helperDepthFormat},
            .bindingSchema = schema,
            .specializationVariant = variant,
        };
        rhi::PipelineHandle pipeline = m_device->createGraphicsPipeline(desc);
        if (!pipeline.isNull()) {
          ++m_diagnostics.pipelineCreations;
        }
        candidate.pipelines[static_cast<std::size_t>(family)] = pipeline;
        return pipeline;
      };

      const rhi::VertexInputLayoutDesc lineInput{lineBindings, lineAttributes};
      const rhi::VertexInputLayoutDesc gizmoInput{gizmoBindings,
                                                   gizmoAttributes};
      createPipeline(PipelineFamily::grid, lineStages, lineInput, gridBlend,
                     false, rhi::CompareOp::always, kGridPipelineVariant);
      createPipeline(PipelineFamily::axes, lineStages, lineInput, axesBlend,
                     false, rhi::CompareOp::always, kAxesPipelineVariant);
      createPipeline(PipelineFamily::gizmoSolidForward, gizmoStages,
                     gizmoInput, opaqueBlend, true,
                     rhi::CompareOp::lessOrEqual,
                     kGizmoSolidForwardPipelineVariant);
      createPipeline(PipelineFamily::gizmoDitherForward, gizmoStages,
                     gizmoInput, opaqueBlend, true,
                     rhi::CompareOp::lessOrEqual,
                     kGizmoDitherForwardPipelineVariant);
      createPipeline(PipelineFamily::gizmoPlaneForward, gizmoStages,
                     gizmoInput, opaqueBlend, true,
                     rhi::CompareOp::lessOrEqual,
                     kGizmoPlaneForwardPipelineVariant);
      createPipeline(PipelineFamily::gizmoSolidReverse, gizmoStages,
                     gizmoInput, opaqueBlend, true,
                     rhi::CompareOp::greaterOrEqual,
                     kGizmoSolidReversePipelineVariant);
      createPipeline(PipelineFamily::gizmoDitherReverse, gizmoStages,
                     gizmoInput, opaqueBlend, true,
                     rhi::CompareOp::greaterOrEqual,
                     kGizmoDitherReversePipelineVariant);
      createPipeline(PipelineFamily::gizmoPlaneReverse, gizmoStages,
                     gizmoInput, opaqueBlend, true,
                     rhi::CompareOp::greaterOrEqual,
                     kGizmoPlaneReversePipelineVariant);
      if (!candidate.valid()) {
        (void)destroySurfaceResourceSet(candidate);
        return fail(rhi::RHIErrorCode::backendFailure,
                    "Editor overlay pipeline family creation failed");
      }
    }

    const bool replacing = m_surface.valid();
    SurfaceResources old = std::move(m_surface);
    m_surface = std::move(candidate);
    if (!signatureChanged) {
      old.pipelines = {};
    }
    const bool retiredOld = destroySurfaceResourceSet(old);
    if (replacing) {
      ++m_diagnostics.helperDepthReplacements;
    }
    return retiredOld
               ? rhi::RHIResult::ok()
               : fail(rhi::RHIErrorCode::backendFailure,
                      "Editor overlay old surface retirement failed");
  } catch (...) {
    if (!signatureChanged) {
      candidate.pipelines = {};
    }
    (void)destroySurfaceResourceSet(candidate);
    return fail(rhi::RHIErrorCode::backendFailure,
                "Editor overlay surface resource creation threw");
  }
}

rhi::RHIResult EditorOverlayPass::encodeFrame(
    FrameSlot &slot, const EditorOverlayPassFrame &frame) noexcept {
  if (frame.frameSerial == 0u || !frame.viewport.valid()) {
    return fail(rhi::RHIErrorCode::invalidArgument,
                "Editor overlay frame identity is invalid");
  }
  const bool hasGizmo = frame.gizmoVisible &&
                        std::any_of(frame.gizmo.components.begin(),
                                    frame.gizmo.components.end(),
                                    [](const auto &draw) {
                                      return draw.visible;
                                    });
  const bool hasGrid = frame.gridVisible && frame.grid.enabled;
  const bool hasDraws = hasGrid || hasGizmo;
  if (hasGrid) {
    if (!matchesGeneration(frame.grid.generation, frame.viewport) ||
        frame.gridLines.size() + frame.gridAxes.size() !=
            frame.grid.generatedLineCount ||
        frame.gridAxes.size() > 2u ||
        frame.gridLines.size() + frame.gridAxes.size() >
            editor::kEditorOverlayMaximumLineInstanceCount) {
      return fail(rhi::RHIErrorCode::invalidArgument,
                  "Editor overlay Grid packet is inconsistent");
    }
  } else if (!frame.gridLines.empty() || !frame.gridAxes.empty()) {
    return fail(rhi::RHIErrorCode::invalidArgument,
                "Disabled editor Grid carries line data");
  }
  if (hasGizmo) {
    if (!matchesGeneration(frame.gizmo.generation, frame.viewport) ||
        !finite(frame.gizmo.worldScale) || frame.gizmo.worldScale <= 0.0 ||
        !finite(frame.gizmo.canonicalVisualDiameter) ||
        frame.gizmo.canonicalVisualDiameter <= 0.0) {
      return fail(rhi::RHIErrorCode::invalidArgument,
                  "Editor overlay Gizmo packet is inconsistent");
    }
    const editor::GizmoGeometryAtlas &atlas =
        editor::canonicalGizmoGeometry();
    for (std::size_t index = 0u; index < frame.gizmo.components.size();
         ++index) {
      const auto &draw = frame.gizmo.components[index];
      if (draw.visible && draw.range != atlas.ranges()[index]) {
        return fail(rhi::RHIErrorCode::invalidArgument,
                    "Editor overlay Gizmo range differs from the atlas");
      }
      if (draw.visible && !checkedRows(draw.model)) {
        return fail(rhi::RHIErrorCode::invalidArgument,
                    "Editor overlay Gizmo transform is non-finite");
      }
    }
  }
  if (hasDraws) {
    const auto &targets = frame.targets;
    if (targets.outputTexture.isNull() || targets.outputView.isNull() ||
        targets.sceneDepthTexture.isNull() ||
        targets.sceneDepthView.isNull() ||
        targets.outputFormat == rhi::TextureFormat::undefined ||
        targets.sceneDepthFormat == rhi::TextureFormat::undefined ||
        !validSampleCount(targets.outputSamples) ||
        targets.sceneDepthSamples != rhi::SampleCount::count1) {
      return fail(rhi::RHIErrorCode::invalidArgument,
                  "Editor overlay borrowed targets are invalid");
    }
  }

  const auto viewProjection = checkedRows(frame.viewProjection);
  const auto logicalWidth = checkedFloat(frame.viewport.logical.extent.width);
  const auto logicalHeight = checkedFloat(frame.viewport.logical.extent.height);
  const auto scaleX = checkedFloat(frame.viewport.framebufferScale.x);
  const auto scaleY = checkedFloat(frame.viewport.framebufferScale.y);
  if (!viewProjection || !logicalWidth || !logicalHeight || !scaleX ||
      !scaleY) {
    return fail(rhi::RHIErrorCode::invalidArgument,
                "Editor overlay frame matrix or viewport cannot narrow");
  }

  slot.encodedLines.clear();
  if (hasGrid) {
    for (const editor::GridLinePacket &line : frame.gridLines) {
      const auto encoded = encodeLine(line, false);
      if (!encoded) {
        return fail(rhi::RHIErrorCode::invalidArgument,
                    "Editor overlay Grid line is invalid");
      }
      slot.encodedLines.push_back(*encoded);
    }
    slot.gridLineCount = static_cast<std::uint32_t>(slot.encodedLines.size());
    for (const editor::GridLinePacket &line : frame.gridAxes) {
      const auto encoded = encodeLine(line, true);
      if (!encoded) {
        return fail(rhi::RHIErrorCode::invalidArgument,
                    "Editor overlay Grid axis is invalid");
      }
      slot.encodedLines.push_back(*encoded);
    }
    slot.axisLineCount = static_cast<std::uint32_t>(frame.gridAxes.size());
  } else {
    slot.gridLineCount = 0u;
    slot.axisLineCount = 0u;
  }

  slot.uniforms = {};
  slot.uniforms.viewProjectionRows = *viewProjection;
  slot.uniforms.colorViewportPx = {
      static_cast<float>(frame.viewport.physical.origin.x),
      static_cast<float>(frame.viewport.physical.origin.y),
      static_cast<float>(frame.viewport.physical.extent.width),
      static_cast<float>(frame.viewport.physical.extent.height)};
  slot.uniforms.depthViewportPx = {
      static_cast<float>(frame.viewport.depthPhysical.origin.x),
      static_cast<float>(frame.viewport.depthPhysical.origin.y),
      static_cast<float>(frame.viewport.depthPhysical.extent.width),
      static_cast<float>(frame.viewport.depthPhysical.extent.height)};
  slot.uniforms.attachmentExtentsPx = {
      static_cast<float>(frame.viewport.colorAttachment.width),
      static_cast<float>(frame.viewport.colorAttachment.height),
      static_cast<float>(frame.viewport.depthAttachment.width),
      static_cast<float>(frame.viewport.depthAttachment.height)};
  slot.uniforms.logicalExtentAndFramebufferScale = {
      *logicalWidth, *logicalHeight, *scaleX, *scaleY};
  slot.uniforms.rasterParams = {
      editor::kEditorOverlayLineWidthLogicalPixels,
      editor::kEditorOverlayDepthEpsilon, 0.0f, 0.0f};
  slot.uniforms.flags = {
      frame.viewport.convention.reversedDepth ? 1u : 0u,
      static_cast<std::uint32_t>(frame.viewport.projectionKind),
      static_cast<std::uint32_t>(frame.viewport.surfaceTransform),
      static_cast<std::uint32_t>(frame.viewport.convention.depthRange)};
  slot.gizmo = frame.gizmo;
  slot.targets = frame.targets;
  slot.noDraw = !hasDraws;

  if (!hasDraws) {
    return rhi::RHIResult::ok();
  }
  const rhi::RHIResult table = ensureFrameTable(slot);
  if (!table.succeeded()) {
    return table;
  }
  const rhi::RHIResult uniforms =
      writeBuffer(*m_device, slot.uniformBuffer, bytesOf(slot.uniforms));
  if (!uniforms.succeeded()) {
    return uniforms;
  }
  if (!slot.encodedLines.empty()) {
    const rhi::RHIResult lines = writeBuffer(
        *m_device, slot.lineBuffer,
        std::as_bytes(std::span<const EditorOverlayLineInstance>(
            slot.encodedLines)));
    if (!lines.succeeded()) {
      return lines;
    }
  }
  const std::array writes{
      rhi::ArgumentWrite{
          .binding = editor::kEditorOverlayFrameBinding,
          .type = rhi::ArgumentType::uniformBuffer,
          .buffer = slot.uniformBuffer,
          .size = sizeof(EditorOverlayFrameUniforms),
      },
      rhi::ArgumentWrite{
          .binding = editor::kEditorOverlaySceneDepthBinding,
          .type = rhi::ArgumentType::combinedImageSampler,
          .textureView = frame.targets.sceneDepthView,
          .sampler = m_sceneDepthSampler,
          .accessIntent = rhi::ArgumentAccessIntent::sampledRead,
      },
  };
  try {
    m_device->updateArgumentTable(slot.argumentTable, writes);
    m_diagnostics.descriptorWrites += writes.size();
  } catch (...) {
    slot.tablePoisoned = true;
    if (!slot.argumentTable.isNull()) {
      (void)callNoThrow(
          [&] { m_device->destroyArgumentTable(slot.argumentTable); });
      slot.argumentTable = {};
    }
    return fail(rhi::RHIErrorCode::backendFailure,
                "Editor overlay descriptor update failed");
  }
  return rhi::RHIResult::ok();
}

EditorOverlayPrepareResult EditorOverlayPass::prepareFrame(
    std::uint32_t frameSlot, const EditorOverlayPassFrame &frame) noexcept {
  ++m_diagnostics.prepareCalls;
  if (m_device == nullptr || m_graphicsQueue == nullptr) {
    ++m_diagnostics.rejectedFrames;
    recordError(EditorOverlayPassError::notInitialized);
    return {.error = EditorOverlayPassError::notInitialized};
  }
  (void)collectCompletedFrames();
  if (frameSlot >= m_frames.size() || frame.frameSerial == 0u ||
      frame.frameSerial <= m_lastFrameSerial ||
      m_frames[frameSlot].state != FrameState::idle) {
    ++m_diagnostics.rejectedFrames;
    const EditorOverlayPassError error =
        frameSlot < m_frames.size() &&
                m_frames[frameSlot].state != FrameState::idle
            ? EditorOverlayPassError::frameBusy
            : EditorOverlayPassError::invalidArgument;
    recordError(error);
    return {.error = error};
  }

  const bool hasDraws =
      (frame.gridVisible && frame.grid.enabled) ||
      (frame.gizmoVisible &&
       std::any_of(frame.gizmo.components.begin(), frame.gizmo.components.end(),
                   [](const auto &draw) { return draw.visible; }));
  const rhi::SubmissionToken submissionBaseline =
      m_graphicsQueue->lastSubmittedToken();
  if (submissionBaseline.isValid() &&
      submissionBaseline.queue != m_graphicsQueue->identity()) {
    ++m_diagnostics.rejectedFrames;
    ++m_diagnostics.failuresBeforeSubmit;
    recordError(EditorOverlayPassError::invalidSubmission);
    return {.error = EditorOverlayPassError::invalidSubmission};
  }
  FrameSlot &slot = m_frames[frameSlot];
  const rhi::RHIResult encoded = encodeFrame(slot, frame);
  if (!encoded.succeeded()) {
    ++m_diagnostics.rejectedFrames;
    ++m_diagnostics.failuresBeforeSubmit;
    recordError(encoded.error.code == rhi::RHIErrorCode::invalidArgument
                    ? EditorOverlayPassError::invalidArgument
                    : EditorOverlayPassError::resourceFailure);
    return {.error = m_diagnostics.lastError};
  }
  if (hasDraws) {
    const rhi::RHIResult surface = ensureSurfaceResources(frame);
    if (!surface.succeeded()) {
      ++m_diagnostics.rejectedFrames;
      ++m_diagnostics.failuresBeforeSubmit;
      recordError(surface.error.code == rhi::RHIErrorCode::invalidArgument
                      ? EditorOverlayPassError::invalidArgument
                      : EditorOverlayPassError::resourceFailure);
      return {.error = m_diagnostics.lastError};
    }
  }

  std::uint32_t generation = m_nextTicketGeneration++;
  if (generation == 0u) {
    generation = m_nextTicketGeneration++;
  }
  slot.ticket = {frameSlot, generation, frame.frameSerial};
  slot.state = FrameState::prepared;
  slot.recorded = false;
  slot.submissionBaselineValue =
      submissionBaseline.isValid() ? submissionBaseline.value : 0u;
  slot.submission = {};
  m_lastFrameSerial = frame.frameSerial;
  m_diagnostics.lastError = EditorOverlayPassError::none;
  return {.ticket = slot.ticket,
          .helperDepth = hasDraws ? helperDepthBinding()
                                  : EditorOverlayHelperDepthBinding{}};
}

rhi::PipelineHandle EditorOverlayPass::pipelineFor(
    const editor::GizmoComponentDrawPacket &draw,
    const editor::GizmoDrawPacket &packet,
    const editor::EditorProjectionConvention &convention) const noexcept {
  const bool reverse = convention.reversedDepth;
  PipelineFamily family{};
  if (planeComponent(draw.range.component)) {
    family = reverse ? PipelineFamily::gizmoPlaneReverse
                     : PipelineFamily::gizmoPlaneForward;
  } else if (packet.depth == editor::GizmoDepthToken::occlusionDither) {
    family = reverse ? PipelineFamily::gizmoDitherReverse
                     : PipelineFamily::gizmoDitherForward;
  } else {
    family = reverse ? PipelineFamily::gizmoSolidReverse
                     : PipelineFamily::gizmoSolidForward;
  }
  return m_surface.pipelines[static_cast<std::size_t>(family)];
}

void EditorOverlayPass::execute(const PassContext &context) const {
  if (m_device == nullptr || context.commandBuffer == nullptr ||
      context.frameIndex >= m_frames.size()) {
    recordError(EditorOverlayPassError::recordingFailure);
    ++m_diagnostics.failuresBeforeSubmit;
    return;
  }
  FrameSlot &slot = m_frames[context.frameIndex];
  if (slot.state != FrameState::prepared || slot.recorded) {
    recordError(EditorOverlayPassError::frameBusy);
    ++m_diagnostics.failuresBeforeSubmit;
    return;
  }
  if (slot.noDraw) {
    slot.recorded = true;
    ++m_diagnostics.recordedFrames;
    return;
  }
  if (!m_surface.valid() || slot.argumentTable.isNull()) {
    recordError(EditorOverlayPassError::recordingFailure);
    ++m_diagnostics.failuresBeforeSubmit;
    return;
  }

  rhi::CommandBuffer &commandBuffer = *context.commandBuffer;
  bool eventOpen = false;
  bool encoderOpen = false;
  try {
    m_diagnostics.lastRecordingStage = EditorOverlayRecordingStage::beginEvent;
    commandBuffer.beginEvent("EditorOverlay");
    eventOpen = true;
    const rhi::RenderTargetDesc colorTarget{
        .texture = slot.targets.outputTexture,
        .view = slot.targets.outputView,
        .state = rhi::ResourceState::ColorAttachment,
        .loadOp = rhi::LoadOp::load,
        .storeOp = rhi::StoreOp::store,
    };
    const rhi::DepthTargetDesc depthTarget{
        .texture = m_surface.helperDepth,
        .view = m_surface.helperDepthView,
        .state = rhi::ResourceState::DepthStencilAttachment,
        .loadOp = rhi::LoadOp::clear,
        .storeOp = rhi::StoreOp::store,
        .clearValue = {
            .depth = slot.uniforms.flags.x != 0u ? 0.0f : 1.0f,
            .stencil = 0u,
        },
    };
    const auto &rect = slot.uniforms.colorViewportPx;
    const rhi::RenderPassDesc pass{
        .renderArea = {
            .offset = {static_cast<std::int32_t>(rect.x),
                       static_cast<std::int32_t>(rect.y)},
            .extent = {static_cast<std::uint32_t>(rect.z),
                       static_cast<std::uint32_t>(rect.w)},
        },
        .colorTargets = std::span<const rhi::RenderTargetDesc>(&colorTarget,
                                                               1u),
        .depthTarget = &depthTarget,
    };
    m_diagnostics.lastRecordingStage =
        EditorOverlayRecordingStage::beginRenderPass;
    rhi::RenderEncoder *encoder = commandBuffer.beginRenderPass(pass);
    if (encoder == nullptr) {
      throw 1;
    }
    encoderOpen = true;
    m_diagnostics.lastRecordingStage = EditorOverlayRecordingStage::dynamicState;
    encoder->setViewport({rect.x, rect.y, rect.z, rect.w, 0.0f, 1.0f});
    encoder->setScissor(pass.renderArea);
    EditorOverlayDrawConstants constants{};
    constants.modelRows = {glm::vec4(1.0f, 0.0f, 0.0f, 0.0f),
                           glm::vec4(0.0f, 1.0f, 0.0f, 0.0f),
                           glm::vec4(0.0f, 0.0f, 1.0f, 0.0f),
                           glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)};
    if (slot.gridLineCount != 0u) {
      m_diagnostics.lastRecordingStage = EditorOverlayRecordingStage::gridDraw;
      constants.color = glm::vec4(1.0f);
      constants.flags = {editor::kEditorOverlayDrawGrid,
                         editor::kEditorOverlayDepthSceneOccluded, 0u, 0u};
      encoder->setPipeline(m_surface.pipelines[static_cast<std::size_t>(
          PipelineFamily::grid)]);
      m_diagnostics.lastRecordingStage =
          EditorOverlayRecordingStage::argumentTable;
      encoder->setArgumentTable(rhi::ShaderStage::allGraphics,
                                editor::kEditorOverlayArgumentSlot,
                                slot.argumentTable);
      m_diagnostics.lastRecordingStage = EditorOverlayRecordingStage::gridDraw;
      encoder->setRootConstants(rhi::ShaderStage::allGraphics,
                                editor::kEditorOverlayRootSlot,
                                bytesOf(constants));
      encoder->bindVertexBuffer(0u, slot.lineBuffer, 0u);
      encoder->draw({.vertexCount = 6u,
                     .instanceCount = slot.gridLineCount});
      ++m_diagnostics.gridDraws;
    }
    if (slot.axisLineCount != 0u) {
      m_diagnostics.lastRecordingStage = EditorOverlayRecordingStage::axesDraw;
      constants.color = glm::vec4(1.0f);
      constants.flags = {editor::kEditorOverlayDrawAxes,
                         editor::kEditorOverlayDepthAlwaysVisible, 0u, 0u};
      encoder->setPipeline(m_surface.pipelines[static_cast<std::size_t>(
          PipelineFamily::axes)]);
      m_diagnostics.lastRecordingStage =
          EditorOverlayRecordingStage::argumentTable;
      encoder->setArgumentTable(rhi::ShaderStage::allGraphics,
                                editor::kEditorOverlayArgumentSlot,
                                slot.argumentTable);
      m_diagnostics.lastRecordingStage = EditorOverlayRecordingStage::axesDraw;
      encoder->setRootConstants(rhi::ShaderStage::allGraphics,
                                editor::kEditorOverlayRootSlot,
                                bytesOf(constants));
      encoder->bindVertexBuffer(
          0u, slot.lineBuffer,
          static_cast<std::uint64_t>(slot.gridLineCount) *
              sizeof(EditorOverlayLineInstance));
      encoder->draw({.vertexCount = 6u,
                     .instanceCount = slot.axisLineCount});
      ++m_diagnostics.axesDraws;
    }

    bool atlasBound = false;
    for (const editor::GizmoComponentDrawPacket &draw :
         slot.gizmo.components) {
      if (!draw.visible) {
        continue;
      }
      if (!atlasBound) {
        encoder->bindVertexBuffer(0u, m_gizmoVertexBuffer, 0u);
        atlasBound = true;
      }
      m_diagnostics.lastRecordingStage = EditorOverlayRecordingStage::gizmoDraw;
      constants = {};
      constants.modelRows = checkedRows(draw.model).value();
      constants.color = gizmoColor(draw);
      std::uint32_t coverage = 0u;
      if (planeComponent(draw.range.component)) {
        coverage |= editor::kEditorOverlayFlagPlaneChecker;
        constants.color.a = 1.0f;
      }
      if (slot.gizmo.depth == editor::GizmoDepthToken::occlusionDither) {
        coverage |= editor::kEditorOverlayFlagOcclusionDither;
      }
      constants.flags = {
          editor::kEditorOverlayDrawGizmo,
          slot.gizmo.depth == editor::GizmoDepthToken::occlusionDither
              ? editor::kEditorOverlayDepthXRayDithered
              : editor::kEditorOverlayDepthXRaySolid,
          static_cast<std::uint32_t>(draw.range.component), coverage};
      encoder->setPipeline(
          pipelineFor(draw, slot.gizmo,
                      editor::EditorProjectionConvention{
                          static_cast<editor::EditorClipDepthRange>(
                          slot.uniforms.flags.w),
                          slot.uniforms.flags.x != 0u, true}));
      m_diagnostics.lastRecordingStage =
          EditorOverlayRecordingStage::argumentTable;
      encoder->setArgumentTable(rhi::ShaderStage::allGraphics,
                                editor::kEditorOverlayArgumentSlot,
                                slot.argumentTable);
      m_diagnostics.lastRecordingStage = EditorOverlayRecordingStage::gizmoDraw;
      encoder->setRootConstants(rhi::ShaderStage::allGraphics,
                                editor::kEditorOverlayRootSlot,
                                bytesOf(constants));
      encoder->drawIndexed({
          .indexBuffer = m_gizmoIndexBuffer,
          .indexBufferOffset = 0u,
          .indexFormat = rhi::IndexFormat::uint32,
          .indexCount = draw.range.indexCount,
          .instanceCount = 1u,
          .firstIndex = draw.range.firstIndex,
          .vertexOffset = 0,
      });
      ++m_diagnostics.gizmoDraws;
    }
    m_diagnostics.lastRecordingStage = EditorOverlayRecordingStage::endEncoding;
    commandBuffer.endEncoding();
    encoderOpen = false;
    commandBuffer.endEvent();
    eventOpen = false;
    slot.recorded = true;
    ++m_diagnostics.recordedFrames;
    m_diagnostics.lastError = EditorOverlayPassError::none;
    m_diagnostics.lastRecordingStage = EditorOverlayRecordingStage::none;
  } catch (const std::exception &error) {
    if (m_diagnostics.failuresBeforeSubmit == 0u) {
      LOGE("EditorOverlay recording failed at stage %u: %s",
           static_cast<unsigned>(m_diagnostics.lastRecordingStage),
           error.what());
    }
    if (encoderOpen) {
      try {
        commandBuffer.endEncoding();
      } catch (...) {
      }
    }
    if (eventOpen) {
      try {
        commandBuffer.endEvent();
      } catch (...) {
      }
    }
    recordError(EditorOverlayPassError::recordingFailure);
    ++m_diagnostics.failuresBeforeSubmit;
  } catch (...) {
    if (m_diagnostics.failuresBeforeSubmit == 0u) {
      LOGE("EditorOverlay recording failed at stage %u with a non-standard exception",
           static_cast<unsigned>(m_diagnostics.lastRecordingStage));
    }
    if (encoderOpen) {
      try {
        commandBuffer.endEncoding();
      } catch (...) {
      }
    }
    if (eventOpen) {
      try {
        commandBuffer.endEvent();
      } catch (...) {
      }
    }
    recordError(EditorOverlayPassError::recordingFailure);
    ++m_diagnostics.failuresBeforeSubmit;
  }
}

rhi::RHIResult EditorOverlayPass::resolveFrame(
    EditorOverlayFrameTicket ticket,
    EditorOverlayFrameDisposition disposition,
    rhi::SubmissionToken submission) noexcept {
  if (m_device == nullptr || !ticket.valid() || ticket.slot >= m_frames.size()) {
    recordError(EditorOverlayPassError::staleTicket);
    return fail(rhi::RHIErrorCode::invalidArgument,
                "Editor overlay frame ticket is invalid");
  }
  FrameSlot &slot = m_frames[ticket.slot];
  if (slot.state != FrameState::prepared || slot.ticket != ticket) {
    recordError(EditorOverlayPassError::staleTicket);
    return fail(rhi::RHIErrorCode::invalidState,
                "Editor overlay frame ticket is stale");
  }
  if (disposition == EditorOverlayFrameDisposition::notSubmitted) {
    const rhi::SubmissionToken published =
        m_graphicsQueue->lastSubmittedToken();
    const std::uint64_t publishedValue =
        published.isValid() && published.queue == m_graphicsQueue->identity()
            ? published.value
            : 0u;
    if (publishedValue != slot.submissionBaselineValue) {
      ++m_diagnostics.failuresAfterSubmit;
      recordError(EditorOverlayPassError::invalidSubmission);
      return fail(rhi::RHIErrorCode::invalidState,
                  "Editor overlay frame cannot be abandoned after queue progress");
    }
    slot.state = FrameState::idle;
    slot.recorded = false;
    slot.ticket = {};
    slot.submissionBaselineValue = 0u;
    slot.submission = {};
    ++m_diagnostics.abandonedFrames;
    m_diagnostics.lastError = EditorOverlayPassError::none;
    return rhi::RHIResult::ok();
  }
  const rhi::SubmissionToken published =
      m_graphicsQueue->lastSubmittedToken();
  if (!submission.isValid() ||
      submission.queue != m_graphicsQueue->identity() ||
      slot.submissionBaselineValue ==
          (std::numeric_limits<std::uint64_t>::max)() ||
      submission.value != slot.submissionBaselineValue + 1u ||
      !published.isValid() || published.queue != submission.queue ||
      submission.value != published.value) {
    ++m_diagnostics.failuresAfterSubmit;
    recordError(EditorOverlayPassError::invalidSubmission);
    return fail(rhi::RHIErrorCode::invalidArgument,
                "Editor overlay submission token is invalid");
  }
  const bool recordingFailed = !slot.recorded;
  slot.state = FrameState::submitted;
  slot.submission = submission;
  if (!slot.noDraw && m_surface.valid()) {
    m_surface.helperState = rhi::ResourceState::DepthStencilAttachment;
  }
  ++m_diagnostics.submittedFrames;
  if (recordingFailed) {
    ++m_diagnostics.failuresAfterSubmit;
    recordError(EditorOverlayPassError::recordingFailure);
  } else {
    m_diagnostics.lastError = EditorOverlayPassError::none;
  }
  return rhi::RHIResult::ok();
}

std::uint32_t EditorOverlayPass::collectCompletedFrames() noexcept {
  if (m_graphicsQueue == nullptr) {
    return 0u;
  }
  std::uint32_t completed = 0u;
  for (FrameSlot &slot : m_frames) {
    if (slot.state != FrameState::submitted) {
      continue;
    }
    ++m_diagnostics.queueCompletionQueries;
    bool isComplete = false;
    try {
      isComplete = m_graphicsQueue->isComplete(slot.submission);
    } catch (...) {
      ++m_diagnostics.failuresAfterSubmit;
      recordError(EditorOverlayPassError::resourceFailure);
      continue;
    }
    if (!isComplete) {
      continue;
    }
    slot.state = FrameState::idle;
    slot.recorded = false;
    slot.ticket = {};
    slot.submissionBaselineValue = 0u;
    slot.submission = {};
    ++completed;
    ++m_diagnostics.completedFrames;
  }
  return completed;
}

EditorOverlayHelperDepthBinding
EditorOverlayPass::helperDepthBinding() const noexcept {
  if (!m_surface.valid()) {
    return {};
  }
  return {
      .texture = m_surface.helperDepth,
      .view = m_surface.helperDepthView,
      .initialState = m_surface.helperState,
      .format = m_surface.signature.helperDepthFormat,
      .extent = m_surface.extent,
      .samples = m_surface.signature.samples,
  };
}

rhi::RHIResult EditorOverlayPass::releaseSurfaceResources() noexcept {
  if (m_device == nullptr) {
    return rhi::RHIResult::ok();
  }
  for (const FrameSlot &slot : m_frames) {
    if (slot.state == FrameState::prepared) {
      recordError(EditorOverlayPassError::frameBusy);
      return fail(rhi::RHIErrorCode::invalidState,
                  "Editor overlay surface has an unresolved frame");
    }
  }
  if (!destroySurfaceResources()) {
    recordError(EditorOverlayPassError::resourceFailure);
    return fail(rhi::RHIErrorCode::backendFailure,
                "Editor overlay surface release failed");
  }
  ++m_diagnostics.surfaceReleases;
  m_diagnostics.lastError = EditorOverlayPassError::none;
  return rhi::RHIResult::ok();
}

rhi::RHIResult EditorOverlayPass::shutdown() noexcept {
  ++m_diagnostics.shutdownCalls;
  if (m_device == nullptr) {
    return rhi::RHIResult::ok();
  }
  for (const FrameSlot &slot : m_frames) {
    if (slot.state == FrameState::prepared) {
      recordError(EditorOverlayPassError::frameBusy);
      return fail(rhi::RHIErrorCode::invalidState,
                  "Editor overlay shutdown has an unresolved frame");
    }
  }
  if (!destroyAll()) {
    recordError(EditorOverlayPassError::resourceFailure);
    return fail(rhi::RHIErrorCode::backendFailure,
                "Editor overlay shutdown cleanup failed");
  }
  m_diagnostics.lastError = EditorOverlayPassError::none;
  return rhi::RHIResult::ok();
}

bool EditorOverlayPass::destroySurfaceResources() noexcept {
  return destroySurfaceResourceSet(m_surface);
}

bool EditorOverlayPass::destroySurfaceResourceSet(
    SurfaceResources &surface) noexcept {
  if (m_device == nullptr) {
    surface = {};
    return true;
  }
  bool succeeded = true;
  for (rhi::PipelineHandle &pipeline : surface.pipelines) {
    if (!pipeline.isNull()) {
      succeeded = callNoThrow([&] { m_device->destroyPipeline(pipeline); }) &&
                  succeeded;
      pipeline = {};
    }
  }
  if (!surface.helperDepthView.isNull()) {
    succeeded = callNoThrow(
                    [&] { m_device->destroyTextureView(surface.helperDepthView); }) &&
                succeeded;
  }
  if (!surface.helperDepth.isNull()) {
    succeeded =
        callNoThrow([&] { m_device->destroyTexture(surface.helperDepth); }) &&
        succeeded;
  }
  surface = {};
  return succeeded;
}

bool EditorOverlayPass::destroyFrameResources() noexcept {
  bool succeeded = true;
  if (m_device != nullptr) {
    for (FrameSlot &slot : m_frames) {
      if (!slot.argumentTable.isNull()) {
        succeeded = callNoThrow(
                        [&] { m_device->destroyArgumentTable(slot.argumentTable); }) &&
                    succeeded;
      }
      if (!slot.lineBuffer.isNull()) {
        succeeded =
            callNoThrow([&] { m_device->destroyBuffer(slot.lineBuffer); }) &&
            succeeded;
      }
      if (!slot.uniformBuffer.isNull()) {
        succeeded = callNoThrow(
                        [&] { m_device->destroyBuffer(slot.uniformBuffer); }) &&
                    succeeded;
      }
      slot = {};
    }
  }
  m_frames.clear();
  return succeeded;
}

bool EditorOverlayPass::destroyStaticResources() noexcept {
  bool succeeded = true;
  if (m_device != nullptr) {
    if (!m_gizmoIndexBuffer.isNull()) {
      succeeded =
          callNoThrow([&] { m_device->destroyBuffer(m_gizmoIndexBuffer); }) &&
          succeeded;
    }
    if (!m_gizmoVertexBuffer.isNull()) {
      succeeded = callNoThrow(
                      [&] { m_device->destroyBuffer(m_gizmoVertexBuffer); }) &&
                  succeeded;
    }
    if (!m_argumentLayout.isNull()) {
      succeeded = callNoThrow(
                      [&] { m_device->destroyArgumentLayout(m_argumentLayout); }) &&
                  succeeded;
    }
    if (!m_sceneDepthSampler.isNull()) {
      succeeded = callNoThrow(
                      [&] { m_device->destroySampler(m_sceneDepthSampler); }) &&
                  succeeded;
    }
    if (!m_shaderLibrary.isNull()) {
      succeeded = callNoThrow(
                      [&] { m_device->destroyShaderLibrary(m_shaderLibrary); }) &&
                  succeeded;
    }
  }
  m_gizmoIndexBuffer = {};
  m_gizmoVertexBuffer = {};
  m_argumentLayout = {};
  m_sceneDepthSampler = {};
  m_shaderLibrary = {};
  return succeeded;
}

bool EditorOverlayPass::destroyAll() noexcept {
  const bool surfaces = destroySurfaceResources();
  const bool frames = destroyFrameResources();
  const bool statics = destroyStaticResources();
  m_device = nullptr;
  m_graphicsQueue = nullptr;
  m_lastFrameSerial = 0u;
  m_nextTicketGeneration = 1u;
  return surfaces && frames && statics;
}

PassNode::HandleSlice<PassResourceDependency>
EditorOverlayPass::getDependencies() const {
  static const std::array dependencies{
      PassResourceDependency::texture(
          kPassOutputHandle, ResourceAccess::readWrite,
          rhi::StageFlags::rasterColorOut, rhi::HazardFlags::textureWrites,
          rhi::ResourceState::ColorAttachment),
      PassResourceDependency::texture(
          kPassSceneDepthHandle, ResourceAccess::read,
          rhi::ShaderStage::fragment, rhi::ResourceState::ShaderRead),
      PassResourceDependency::texture(
          kPassEditorOverlayHelperDepthHandle, ResourceAccess::write,
          rhi::StageFlags::rasterDepthOut, rhi::HazardFlags::depthStencil,
          rhi::ResourceState::DepthStencilAttachment),
  };
  return {dependencies.data(), static_cast<std::uint32_t>(dependencies.size())};
}

} // namespace demo
