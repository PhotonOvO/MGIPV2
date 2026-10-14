#pragma once

#include "MaterialClipboardCodec.h"

namespace demo::editor::ui {

// The sole desktop OS-text transport used by the frozen material codec.
// Clipboard grammar remains entirely in MaterialClipboardCodec.
class ImGuiClipboardAdapter final : public IOsTextClipboard {
public:
  [[nodiscard]] std::optional<std::string> readText() const override;
  bool writeText(std::string_view text) override;
};

} // namespace demo::editor::ui
