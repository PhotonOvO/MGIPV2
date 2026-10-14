#include "ImGuiClipboardAdapter.h"

#include <imgui.h>

namespace demo::editor::ui {

std::optional<std::string> ImGuiClipboardAdapter::readText() const {
  if (ImGui::GetCurrentContext() == nullptr ||
      ImGui::GetPlatformIO().Platform_GetClipboardTextFn == nullptr) {
    return std::nullopt;
  }
  const char *text = ImGui::GetClipboardText();
  return text != nullptr ? std::optional<std::string>{std::string{text}}
                         : std::nullopt;
}

bool ImGuiClipboardAdapter::writeText(std::string_view text) {
  if (ImGui::GetCurrentContext() == nullptr ||
      ImGui::GetPlatformIO().Platform_SetClipboardTextFn == nullptr) {
    return false;
  }
  const std::string owned{text};
  ImGui::SetClipboardText(owned.c_str());
  return true;
}

} // namespace demo::editor::ui
