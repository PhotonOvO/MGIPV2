#pragma once

#include <mgif/scene_editor/EditorObjectId.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace demo::editor {

class GltfDocument;

enum class MaterialClipboardSyntax : std::uint8_t {
  absent = 0,
  malformed,
  parsed,
};

struct ParsedMaterialClipboard final {
  MaterialClipboardSyntax syntax{MaterialClipboardSyntax::absent};
  std::optional<std::uint64_t> documentIndex;

  bool operator==(const ParsedMaterialClipboard &) const = default;
};

struct MaterialPasteAvailability final {
  ParsedMaterialClipboard parsed;
  bool menuEnabled{false};
  bool indexInActiveDocument{false};
  std::optional<MaterialId> activeDocumentMaterial;

  [[nodiscard]] bool applicable() const noexcept;
  [[nodiscard]] bool
  wouldChange(std::optional<MaterialId> currentMaterial) const noexcept;

  bool operator==(const MaterialPasteAvailability &) const = default;
};

class IOsTextClipboard {
public:
  virtual ~IOsTextClipboard() = default;

  [[nodiscard]] virtual std::optional<std::string> readText() const = 0;
  virtual bool writeText(std::string_view text) = 0;
};

class MaterialClipboardCodec final {
public:
  static constexpr std::string_view prefix{"gltf_material:"};

  [[nodiscard]] static std::string encode(std::uint64_t documentIndex);
  [[nodiscard]] static ParsedMaterialClipboard
  parse(std::optional<std::string_view> text) noexcept;
  [[nodiscard]] static ParsedMaterialClipboard
  parse(std::string_view text) noexcept {
    return parse(std::optional<std::string_view>{text});
  }
  [[nodiscard]] static MaterialPasteAvailability
  resolve(const GltfDocument &activeDocument,
          const ParsedMaterialClipboard &parsed) noexcept;
  [[nodiscard]] static MaterialPasteAvailability
  resolveText(const GltfDocument &activeDocument,
              std::optional<std::string_view> text) noexcept;
  [[nodiscard]] static MaterialPasteAvailability
  resolveText(const GltfDocument &activeDocument,
              std::string_view text) noexcept {
    return resolveText(activeDocument,
                       std::optional<std::string_view>{text});
  }
  [[nodiscard]] static MaterialPasteAvailability
  resolveOnPasteClick(const GltfDocument &activeDocument,
                      const IOsTextClipboard &clipboard) noexcept;
  [[nodiscard]] static bool copy(const GltfDocument &activeDocument,
                                 MaterialId material,
                                 IOsTextClipboard &clipboard);
};

} // namespace demo::editor
