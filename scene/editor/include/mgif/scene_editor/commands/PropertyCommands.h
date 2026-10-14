#pragma once

#include <mgif/scene_editor/EditorFaultInjection.h>
#include <mgif/scene_editor/commands/EditorCommand.h>

namespace demo::editor {

[[nodiscard]] CommandPrepareResult prepareEditMaterialCommand(
    EditorCommandContext &context, MaterialSnapshot before,
    MaterialSnapshot after,
    EditorFaultInjector *faultInjector = nullptr) noexcept;

[[nodiscard]] CommandPrepareResult
prepareEditLightCommand(EditorCommandContext &context, LightSnapshot before,
                        LightSnapshot after,
                        EditorFaultInjector *faultInjector = nullptr) noexcept;

[[nodiscard]] ContinuousCommandPrepareResult prepareContinuousMaterialCommand(
    EditorCommandContext &context, MaterialId material,
    EditorFaultInjector *faultInjector = nullptr) noexcept;

[[nodiscard]] ContinuousCommandPrepareResult prepareContinuousLightCommand(
    EditorCommandContext &context, LightId light,
    EditorFaultInjector *faultInjector = nullptr) noexcept;

} // namespace demo::editor
