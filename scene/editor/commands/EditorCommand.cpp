#include <mgif/scene_editor/commands/EditorCommand.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace demo::editor {
namespace {

class Sha256 final {
public:
  void update(const void *source, std::size_t size) noexcept {
    const auto *bytes = static_cast<const std::uint8_t *>(source);
    m_totalBytes += static_cast<std::uint64_t>(size);
    while (size != 0) {
      const std::size_t available = m_buffer.size() - m_bufferSize;
      const std::size_t copied = size < available ? size : available;
      std::memcpy(m_buffer.data() + m_bufferSize, bytes, copied);
      m_bufferSize += copied;
      bytes += copied;
      size -= copied;
      if (m_bufferSize == m_buffer.size()) {
        transform(m_buffer.data());
        m_bufferSize = 0;
      }
    }
  }

  [[nodiscard]] CommandStateSha256 finish() noexcept {
    const std::uint64_t bitCount = m_totalBytes * 8u;
    m_buffer[m_bufferSize++] = 0x80u;
    if (m_bufferSize > 56u) {
      while (m_bufferSize < m_buffer.size())
        m_buffer[m_bufferSize++] = 0;
      transform(m_buffer.data());
      m_bufferSize = 0;
    }
    while (m_bufferSize < 56u)
      m_buffer[m_bufferSize++] = 0;
    for (std::size_t index = 0; index < 8u; ++index) {
      m_buffer[56u + index] = static_cast<std::uint8_t>(
          bitCount >> (56u - static_cast<unsigned>(index) * 8u));
    }
    transform(m_buffer.data());

    CommandStateSha256 digest{};
    for (std::size_t word = 0; word < m_state.size(); ++word) {
      for (std::size_t byte = 0; byte < 4u; ++byte) {
        digest[word * 4u + byte] = static_cast<std::uint8_t>(
            m_state[word] >> (24u - static_cast<unsigned>(byte) * 8u));
      }
    }
    return digest;
  }

private:
  [[nodiscard]] static constexpr std::uint32_t
  rotateRight(std::uint32_t value, unsigned bits) noexcept {
    return (value >> bits) | (value << (32u - bits));
  }

  void transform(const std::uint8_t *block) noexcept {
    static constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
        0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
        0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
        0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
        0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
        0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
        0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
        0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16u; ++index) {
      const std::size_t offset = index * 4u;
      words[index] = (static_cast<std::uint32_t>(block[offset]) << 24u) |
                     (static_cast<std::uint32_t>(block[offset + 1u]) << 16u) |
                     (static_cast<std::uint32_t>(block[offset + 2u]) << 8u) |
                     static_cast<std::uint32_t>(block[offset + 3u]);
    }
    for (std::size_t index = 16u; index < words.size(); ++index) {
      const std::uint32_t s0 = rotateRight(words[index - 15u], 7u) ^
                               rotateRight(words[index - 15u], 18u) ^
                               (words[index - 15u] >> 3u);
      const std::uint32_t s1 = rotateRight(words[index - 2u], 17u) ^
                               rotateRight(words[index - 2u], 19u) ^
                               (words[index - 2u] >> 10u);
      words[index] = words[index - 16u] + s0 + words[index - 7u] + s1;
    }

    std::uint32_t a = m_state[0];
    std::uint32_t b = m_state[1];
    std::uint32_t c = m_state[2];
    std::uint32_t d = m_state[3];
    std::uint32_t e = m_state[4];
    std::uint32_t f = m_state[5];
    std::uint32_t g = m_state[6];
    std::uint32_t h = m_state[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const std::uint32_t bigSigma1 =
          rotateRight(e, 6u) ^ rotateRight(e, 11u) ^ rotateRight(e, 25u);
      const std::uint32_t choose = (e & f) ^ ((~e) & g);
      const std::uint32_t first =
          h + bigSigma1 + choose + constants[index] + words[index];
      const std::uint32_t bigSigma0 =
          rotateRight(a, 2u) ^ rotateRight(a, 13u) ^ rotateRight(a, 22u);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t second = bigSigma0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + first;
      d = c;
      c = b;
      b = a;
      a = first + second;
    }
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
  }

  std::array<std::uint32_t, 8> m_state{
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
  };
  std::array<std::uint8_t, 64> m_buffer{};
  std::size_t m_bufferSize{0};
  std::uint64_t m_totalBytes{0};
};

void appendByte(Sha256 &hash, std::uint8_t value) noexcept {
  hash.update(&value, sizeof(value));
}

void appendU32(Sha256 &hash, std::uint32_t value) noexcept {
  std::array<std::uint8_t, 4> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index)
    bytes[index] = static_cast<std::uint8_t>(value >> (index * 8u));
  hash.update(bytes.data(), bytes.size());
}

void appendU64(Sha256 &hash, std::uint64_t value) noexcept {
  std::array<std::uint8_t, 8> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index)
    bytes[index] = static_cast<std::uint8_t>(value >> (index * 8u));
  hash.update(bytes.data(), bytes.size());
}

void appendBytes(Sha256 &hash, const std::uint8_t *bytes,
                 std::size_t size) noexcept {
  appendU64(hash, static_cast<std::uint64_t>(size));
  if (size != 0)
    hash.update(bytes, size);
}

void appendString(Sha256 &hash, std::string_view value) noexcept {
  appendU64(hash, static_cast<std::uint64_t>(value.size()));
  if (!value.empty())
    hash.update(value.data(), value.size());
}

void appendId(Sha256 &hash, EditorObjectId value) noexcept {
  appendByte(hash, static_cast<std::uint8_t>(value.kind));
  appendU32(hash, value.slot);
  appendU32(hash, value.generation);
  appendU64(hash, value.authority.value);
}

template <EditorObjectKind Kind>
void appendId(Sha256 &hash, TypedEditorObjectId<Kind> value) noexcept {
  appendId(hash, value.untyped());
}

template <typename Id>
void appendOptionalId(Sha256 &hash, const std::optional<Id> &value) noexcept {
  appendByte(hash, value ? 1u : 0u);
  if (value)
    appendId(hash, *value);
}

void appendSelection(Sha256 &hash,
                     const EditorSelectionSnapshot &value) noexcept {
  appendU64(hash, value.authority.value);
  appendByte(hash, static_cast<std::uint8_t>(value.value.index()));
  std::visit(
      [&](const auto &selected) noexcept {
        using Selected = std::remove_cvref_t<decltype(selected)>;
        if constexpr (std::is_same_v<Selected, NodeSelection>) {
          appendId(hash, selected.scene);
          appendId(hash, selected.node);
          appendOptionalId(hash, selected.parent);
        } else if constexpr (std::is_same_v<Selected, PrimitiveSelection>) {
          appendId(hash, selected.scene);
          appendId(hash, selected.node);
          appendId(hash, selected.primitive);
          appendId(hash, selected.mesh);
        } else if constexpr (!std::is_same_v<Selected, std::monostate>) {
          appendId(hash, selected);
        }
      },
      value.value);
}

void appendPayload(Sha256 &hash,
                   const CanonicalPayloadInspection &value) noexcept {
  appendString(hash, value.typeTag);
  appendBytes(hash, value.canonicalBytes.data(), value.canonicalBytes.size());
}

void appendStructuralSide(Sha256 &hash,
                          const StructuralSideInspection &value) noexcept {
  appendByte(hash, value.present ? 1u : 0u);
  appendBytes(hash, value.canonicalBytes.data(), value.canonicalBytes.size());
}

} // namespace

std::optional<CommandMergeKey> EditorCommand::mergeKey() const noexcept {
  return std::nullopt;
}

ExecutedValidation
EditorCommand::validateExecuted(const EditorCommandContext &) const noexcept {
  return {};
}

std::optional<std::uint64_t>
EditorCommand::mergedSnapshotBytes(const EditorCommand &) const noexcept {
  return std::nullopt;
}

bool EditorCommand::tryAbsorbNewer(EditorCommand &) noexcept { return false; }

namespace detail {

CommandStateSha256 retainedCommandInspectionSha256(
    const RetainedCommandInspection &inspection) noexcept {
  Sha256 hash;
  appendU32(hash, inspection.schemaVersion);
  appendByte(hash, static_cast<std::uint8_t>(inspection.kind));
  appendByte(hash, static_cast<std::uint8_t>(inspection.lifecycle));
  appendU64(hash, inspection.stateRevision);
  appendU64(hash, inspection.retainedSnapshotBytes);
  appendOptionalId(hash, inspection.stableTarget);
  appendU64(hash, inspection.authority.value);
  appendU64(hash, inspection.preparedToken.authority.value);
  appendU64(hash, inspection.preparedToken.revision);
  appendU64(hash, inspection.preparedToken.serial);
  appendPayload(hash, inspection.beforePayload);
  appendPayload(hash, inspection.finalPayload);
  appendStructuralSide(hash, inspection.undoStructuralSide);
  appendStructuralSide(hash, inspection.redoStructuralSide);
  appendSelection(hash, inspection.executeSelection);
  appendSelection(hash, inspection.undoSelection);
  appendSelection(hash, inspection.redoSelection);
  return hash.finish();
}

void finalizeRetainedCommandInspection(
    RetainedCommandInspection &inspection) noexcept {
  inspection.canonicalSha256 = retainedCommandInspectionSha256(inspection);
}

bool retainedCommandInspectionHashMatches(
    const RetainedCommandInspection &inspection) noexcept {
  return inspection.canonicalSha256 ==
         retainedCommandInspectionSha256(inspection);
}

} // namespace detail
} // namespace demo::editor
