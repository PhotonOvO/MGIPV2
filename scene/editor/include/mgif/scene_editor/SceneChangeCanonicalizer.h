#pragma once

#include <mgif/scene_editor/SceneChangeSet.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace demo::editor {

enum class SceneChangeValidationError : std::uint16_t {
  None = 0,
  InvalidSchema = 1,
  InvalidAuthority = 2,
  InvalidDomain = 3,
  InvalidEnvelope = 4,
  MonotonicOverflow = 5,
  InvalidActiveScene = 6,
  InvalidMutationToken = 7,
  InvalidSourceMutationChain = 8,
  InvalidMutationSerial = 9,
  InvalidStableId = 10,
  CrossAuthority = 11,
  InvalidActionValues = 12,
  FieldClassMismatch = 13,
  InvalidFieldValue = 14,
  InvalidPayload = 15,
  InvalidRange = 16,
  InvalidContentHash = 17,
  InvalidFullSnapshot = 18,
  IncompleteRemap = 19,
  DuplicateCreate = 20,
  ConflictingWrite = 21,
  DiscontinuousWriteChain = 22,
  GenerationAmbiguity = 23,
  NonClosedHierarchy = 24,
  DependencyCycle = 25,
  MutationTokenConflict = 26,
  CanonicalDigestMismatch = 27,
};

struct SceneChangeValidation final {
  static constexpr std::size_t kNoIndex =
      std::numeric_limits<std::size_t>::max();

  SceneChangeValidationError error{SceneChangeValidationError::None};
  std::size_t changeIndex{kNoIndex};
  std::size_t sourceMutationIndex{kNoIndex};
  std::string message;

  [[nodiscard]] bool valid() const noexcept {
    return error == SceneChangeValidationError::None;
  }
};

struct SceneCanonicalizationResult final {
  SceneChangeValidation validation;
  std::optional<SceneChangeSet> changeSet;
  std::vector<std::uint8_t> canonicalBytes;

  [[nodiscard]] bool succeeded() const noexcept {
    return validation.valid() && changeSet.has_value();
  }
};

// An immutable, unfinished SHA-256 state. It retains no borrowed input bytes.
class SceneSha256Prefix final {
public:
  [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(m_state); }
private:
  struct State;
  std::shared_ptr<const State> m_state;
  friend class SceneChangeCanonicalizer;
};

class SceneChangeCanonicalizer final {
public:
  // Canonical bytes begin with ASCII MGIFSCN1. Integers, enum tags, and
  // length prefixes are fixed-width little-endian; doubles use their exact
  // finite IEEE-754 binary64 bits. The digest field is excluded from its own
  // schema-tagged SHA-256 preimage.
  [[nodiscard]] static SceneCanonicalizationResult
  canonicalize(SceneChangeSet input);

  [[nodiscard]] static SceneCanonicalizationResult canonicalizeEvidence(
      SceneChangeHeader header, std::vector<SceneMutationEvidence> evidence,
      std::optional<FullSnapshotReference> fullSnapshot = std::nullopt);

  [[nodiscard]] static SceneChangeValidation
  validateCanonical(const SceneChangeSet &changeSet,
                    std::span<const std::uint8_t> canonicalBytes);

  [[nodiscard]] static SceneDigest
  computeSha256(std::span<const std::uint8_t> bytes) noexcept;
  [[nodiscard]] static SceneDigest computeSha256Parts(
      std::span<const std::span<const std::uint8_t>> parts) noexcept;
  [[nodiscard]] static SceneSha256Prefix captureSha256Prefix(
      std::span<const std::span<const std::uint8_t>> parts) noexcept;
  [[nodiscard]] static SceneDigest computeSha256WithPrefix(
      const SceneSha256Prefix& prefix, std::span<const std::uint8_t> suffix) noexcept;
  [[nodiscard]] static std::string
  encodeBase64(std::span<const std::uint8_t> bytes);
};

} // namespace demo::editor
