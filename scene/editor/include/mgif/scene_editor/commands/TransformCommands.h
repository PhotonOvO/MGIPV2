#pragma once

#include <mgif/scene_editor/EditorFaultInjection.h>
#include <mgif/scene_editor/commands/EditorCommand.h>

namespace demo::editor {

[[nodiscard]] CommandPrepareResult
prepareTransformCommand(EditorCommandContext &context, NodeId node,
                        NodeTransform before, NodeTransform after,
                        EditorFaultInjector *faultInjector = nullptr) noexcept;

[[nodiscard]] ContinuousCommandPrepareResult prepareContinuousTransformCommand(
    EditorCommandContext &context, NodeId node,
    EditorFaultInjector *faultInjector = nullptr) noexcept;

} // namespace demo::editor
