#ifndef MGIF_EDITOR_OVERLAY_SHADER_INTEROP_H
#define MGIF_EDITOR_OVERLAY_SHADER_INTEROP_H

#ifdef __SLANG__

static const uint kEditorOverlayArgumentSlot = 3u;
static const uint kEditorOverlayFrameBinding = 0u;
static const uint kEditorOverlaySceneDepthBinding = 1u;
static const uint kEditorOverlayRootSlot = 0u;

static const uint kEditorOverlayDrawGrid = 0u;
static const uint kEditorOverlayDrawAxes = 1u;
static const uint kEditorOverlayDrawGizmo = 2u;

static const uint kEditorOverlayDepthSceneOccluded = 0u;
static const uint kEditorOverlayDepthAlwaysVisible = 1u;
static const uint kEditorOverlayDepthXRaySolid = 2u;
static const uint kEditorOverlayDepthXRayDithered = 3u;

static const uint kEditorOverlayFlagPlaneChecker = 1u << 0u;
static const uint kEditorOverlayFlagOcclusionDither = 1u << 1u;

static const uint kEditorOverlayClipDepthZeroToOne = 0u;
static const uint kEditorOverlayClipDepthNegativeOneToOne = 1u;

static const uint kEditorOverlaySurfaceRotate90 = 1u;
static const uint kEditorOverlaySurfaceRotate270 = 3u;
static const uint kEditorOverlaySurfaceMirrorHorizontalRotate90 = 6u;
static const uint kEditorOverlaySurfaceMirrorHorizontalRotate270 = 7u;

struct EditorOverlayFrameUniforms
{
    float4 viewProjectionRows[4];
    float4 colorViewportPx;
    float4 depthViewportPx;
    float4 attachmentExtentsPx;
    float4 logicalExtentAndFramebufferScale;
    float4 rasterParams;
    uint4 flags;
};

struct EditorOverlayLineInstance
{
    float4 startWidth;
    float4 endAlpha;
    float4 color;
};

struct EditorOverlayGizmoVertex
{
    float3 position;
    float3 normal;
};

struct EditorOverlayDrawConstants
{
    float4 modelRows[4];
    float4 color;
    uint4 flags;
};

#else

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace demo::editor {

inline constexpr std::uint32_t kEditorOverlayArgumentSlot = 3u;
inline constexpr std::uint32_t kEditorOverlayFrameBinding = 0u;
inline constexpr std::uint32_t kEditorOverlaySceneDepthBinding = 1u;
inline constexpr std::uint32_t kEditorOverlayRootSlot = 0u;

inline constexpr std::uint32_t kEditorOverlayDrawGrid = 0u;
inline constexpr std::uint32_t kEditorOverlayDrawAxes = 1u;
inline constexpr std::uint32_t kEditorOverlayDrawGizmo = 2u;

inline constexpr std::uint32_t kEditorOverlayDepthSceneOccluded = 0u;
inline constexpr std::uint32_t kEditorOverlayDepthAlwaysVisible = 1u;
inline constexpr std::uint32_t kEditorOverlayDepthXRaySolid = 2u;
inline constexpr std::uint32_t kEditorOverlayDepthXRayDithered = 3u;

inline constexpr std::uint32_t kEditorOverlayFlagPlaneChecker = 1u << 0u;
inline constexpr std::uint32_t kEditorOverlayFlagOcclusionDither = 1u << 1u;

inline constexpr std::uint32_t kEditorOverlayClipDepthZeroToOne = 0u;
inline constexpr std::uint32_t kEditorOverlayClipDepthNegativeOneToOne = 1u;

inline constexpr std::uint32_t kEditorOverlaySurfaceRotate90 = 1u;
inline constexpr std::uint32_t kEditorOverlaySurfaceRotate270 = 3u;
inline constexpr std::uint32_t
    kEditorOverlaySurfaceMirrorHorizontalRotate90 = 6u;
inline constexpr std::uint32_t
    kEditorOverlaySurfaceMirrorHorizontalRotate270 = 7u;

inline constexpr std::uint32_t kEditorOverlayMaximumLineInstanceCount =
    501u * 2u * 3u + 2u;
inline constexpr float kEditorOverlayLineWidthLogicalPixels = 1.5f;
inline constexpr float kEditorOverlayDepthEpsilon = 1.0e-5f;

struct alignas(16) EditorOverlayFrameUniforms final {
  std::array<glm::vec4, 4> viewProjectionRows{};
  glm::vec4 colorViewportPx{};
  glm::vec4 depthViewportPx{};
  glm::vec4 attachmentExtentsPx{};
  glm::vec4 logicalExtentAndFramebufferScale{};
  glm::vec4 rasterParams{};
  glm::uvec4 flags{};
};

struct alignas(16) EditorOverlayLineInstance final {
  glm::vec4 startWidth{};
  glm::vec4 endAlpha{};
  glm::vec4 color{};
};

struct EditorOverlayGizmoVertex final {
  glm::vec3 position{};
  glm::vec3 normal{};
};

struct alignas(16) EditorOverlayDrawConstants final {
  std::array<glm::vec4, 4> modelRows{};
  glm::vec4 color{};
  glm::uvec4 flags{};
};

static_assert(std::is_standard_layout_v<EditorOverlayFrameUniforms>);
static_assert(std::is_trivially_copyable_v<EditorOverlayFrameUniforms>);
static_assert(alignof(EditorOverlayFrameUniforms) == 16u);
static_assert(sizeof(EditorOverlayFrameUniforms) == 160u);
static_assert(offsetof(EditorOverlayFrameUniforms, viewProjectionRows) == 0u);
static_assert(offsetof(EditorOverlayFrameUniforms, colorViewportPx) == 64u);
static_assert(offsetof(EditorOverlayFrameUniforms, depthViewportPx) == 80u);
static_assert(offsetof(EditorOverlayFrameUniforms, attachmentExtentsPx) == 96u);
static_assert(offsetof(EditorOverlayFrameUniforms,
                       logicalExtentAndFramebufferScale) == 112u);
static_assert(offsetof(EditorOverlayFrameUniforms, rasterParams) == 128u);
static_assert(offsetof(EditorOverlayFrameUniforms, flags) == 144u);

static_assert(std::is_standard_layout_v<EditorOverlayLineInstance>);
static_assert(std::is_trivially_copyable_v<EditorOverlayLineInstance>);
static_assert(alignof(EditorOverlayLineInstance) == 16u);
static_assert(sizeof(EditorOverlayLineInstance) == 48u);
static_assert(offsetof(EditorOverlayLineInstance, startWidth) == 0u);
static_assert(offsetof(EditorOverlayLineInstance, endAlpha) == 16u);
static_assert(offsetof(EditorOverlayLineInstance, color) == 32u);

static_assert(std::is_standard_layout_v<EditorOverlayGizmoVertex>);
static_assert(std::is_trivially_copyable_v<EditorOverlayGizmoVertex>);
static_assert(sizeof(EditorOverlayGizmoVertex) == 24u);
static_assert(offsetof(EditorOverlayGizmoVertex, position) == 0u);
static_assert(offsetof(EditorOverlayGizmoVertex, normal) == 12u);

static_assert(std::is_standard_layout_v<EditorOverlayDrawConstants>);
static_assert(std::is_trivially_copyable_v<EditorOverlayDrawConstants>);
static_assert(alignof(EditorOverlayDrawConstants) == 16u);
static_assert(sizeof(EditorOverlayDrawConstants) == 96u);
static_assert(sizeof(EditorOverlayDrawConstants) <= 256u);
static_assert(offsetof(EditorOverlayDrawConstants, modelRows) == 0u);
static_assert(offsetof(EditorOverlayDrawConstants, color) == 64u);
static_assert(offsetof(EditorOverlayDrawConstants, flags) == 80u);

} // namespace demo::editor

#endif

#endif
