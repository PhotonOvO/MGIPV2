#pragma once

#include <mgif/scene_editor/EditorFaultInjection.h>
#include <mgif/scene_editor/commands/EditorCommand.h>

#include <string>

namespace demo::editor {

[[nodiscard]] CommandPrepareResult
prepareRenameNodeCommand(EditorCommandContext &context, NodeId node,
                         std::string newName,
                         EditorFaultInjector *faultInjector = nullptr) noexcept;

[[nodiscard]] CommandPrepareResult prepareAddChildNodeCommand(
    EditorCommandContext &context, AddChildNodeRequest request,
    EditorFaultInjector *faultInjector = nullptr) noexcept;

[[nodiscard]] CommandPrepareResult
prepareAddLightCommand(EditorCommandContext &context, AddLightRequest request,
                       EditorFaultInjector *faultInjector = nullptr) noexcept;

[[nodiscard]] CommandPrepareResult prepareDuplicateNodeCommand(
    EditorCommandContext &context, DuplicateNodeRequest request,
    EditorFaultInjector *faultInjector = nullptr) noexcept;

[[nodiscard]] CommandPrepareResult
prepareDeleteNodeCommand(EditorCommandContext &context,
                         DeleteNodeRequest request,
                         EditorFaultInjector *faultInjector = nullptr) noexcept;

[[nodiscard]] CommandPrepareResult prepareReparentNodeCommand(
    EditorCommandContext &context, ReparentNodeRequest request,
    EditorFaultInjector *faultInjector = nullptr) noexcept;

} // namespace demo::editor
