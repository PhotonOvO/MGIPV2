#pragma once

#include <mgif/scene_editor/SceneProjection.h>

#include <optional>
#include <string>

namespace demo::editor::detail {

struct NodeNameProjectionDigestCache final {
  std::optional<SceneDigest> documentDigest;
  std::optional<std::string> semanticHash;
};

// Internal fast path for a controller-proven retained node-name command.
// Returning nullopt requires the caller to use SceneProjection::project.
[[nodiscard]] std::optional<SceneProjectionResult>
reprojectNodeNameOnly(const GltfDocument& document,
                      const SceneProjectionResult& previous,
                      NodeId node,
                      const SceneProjectionOptions& options,
                      const NodeNameProjectionDigestCache& cached = {}) noexcept;

} // namespace demo::editor::detail
