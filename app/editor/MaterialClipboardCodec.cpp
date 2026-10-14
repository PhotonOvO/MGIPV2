#include "MaterialClipboardCodec.h"

#include <mgif/scene_editor/GltfDocument.h>

#include <limits>

namespace demo::editor {

bool MaterialPasteAvailability::applicable() const noexcept {
  return menuEnabled && indexInActiveDocument &&
         activeDocumentMaterial.has_value();
}

bool MaterialPasteAvailability::wouldChange(
    std::optional<MaterialId> currentMaterial) const noexcept {
  return applicable() && currentMaterial != activeDocumentMaterial;
}

std::string MaterialClipboardCodec::encode(std::uint64_t documentIndex) {
  std::string result{prefix};
  result += std::to_string(documentIndex);
  return result;
}

ParsedMaterialClipboard MaterialClipboardCodec::parse(
    std::optional<std::string_view> text) noexcept {
  if (!text || text->empty()) {
    return {};
  }
  if (!text->starts_with(prefix)) {
    return {MaterialClipboardSyntax::malformed, std::nullopt};
  }

  std::string_view suffix = *text;
  suffix.remove_prefix(prefix.size());
  if (suffix.empty()) {
    return {MaterialClipboardSyntax::malformed, std::nullopt};
  }

  std::uint64_t result = 0u;
  for (const unsigned char character : suffix) {
    if (character < static_cast<unsigned char>('0') ||
        character > static_cast<unsigned char>('9')) {
      return {MaterialClipboardSyntax::malformed, std::nullopt};
    }
    const std::uint64_t digit =
        static_cast<std::uint64_t>(character -
                                   static_cast<unsigned char>('0'));
    if (result >
        (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
      return {MaterialClipboardSyntax::malformed, std::nullopt};
    }
    result = result * 10u + digit;
  }
  return {MaterialClipboardSyntax::parsed, result};
}

MaterialPasteAvailability MaterialClipboardCodec::resolve(
    const GltfDocument &activeDocument,
    const ParsedMaterialClipboard &parsed) noexcept {
  MaterialPasteAvailability result;
  result.parsed = parsed;
  result.menuEnabled = parsed.syntax == MaterialClipboardSyntax::parsed &&
                       parsed.documentIndex.has_value();
  if (!result.menuEnabled ||
      *parsed.documentIndex >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    return result;
  }

  const std::size_t index =
      static_cast<std::size_t>(*parsed.documentIndex);
  if (index >= activeDocument.objectCount(EditorObjectKind::material)) {
    return result;
  }
  const std::optional<MaterialId> material = MaterialId::from(
      activeDocument.idAt(EditorObjectKind::material, index));
  if (!material || !activeDocument.contains(material->untyped())) {
    return result;
  }
  result.indexInActiveDocument = true;
  result.activeDocumentMaterial = material;
  return result;
}

MaterialPasteAvailability MaterialClipboardCodec::resolveText(
    const GltfDocument &activeDocument,
    std::optional<std::string_view> text) noexcept {
  return resolve(activeDocument, parse(text));
}

MaterialPasteAvailability MaterialClipboardCodec::resolveOnPasteClick(
    const GltfDocument &activeDocument,
    const IOsTextClipboard &clipboard) noexcept {
  const std::optional<std::string> currentText = clipboard.readText();
  return resolveText(
      activeDocument,
      currentText
          ? std::optional<std::string_view>{std::string_view{*currentText}}
          : std::nullopt);
}

bool MaterialClipboardCodec::copy(const GltfDocument &activeDocument,
                                  MaterialId material,
                                  IOsTextClipboard &clipboard) {
  if (!material.valid() ||
      material.authority() != activeDocument.documentAuthority() ||
      !activeDocument.contains(material.untyped())) {
    return false;
  }
  const std::optional<std::size_t> index =
      activeDocument.indexOf(material.untyped());
  if (!index) {
    return false;
  }
  const std::string encoded = encode(static_cast<std::uint64_t>(*index));
  return !encoded.empty() && clipboard.writeText(encoded);
}

} // namespace demo::editor
