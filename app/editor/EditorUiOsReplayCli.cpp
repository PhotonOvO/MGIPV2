#include "EditorUiOsReplayCli.h"

#include <mgif/scene_editor/SceneChangeCanonicalizer.h>

#include <algorithm>
#include <system_error>
#include <utility>

namespace demo::editor::testing::osreplay {
namespace {

void appendJsonString(std::string &output, std::string_view value) {
  static constexpr char hex[] = "0123456789abcdef";
  output.push_back('"');
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (character < 0x20u) {
        output += "\\u00";
        output.push_back(hex[(character >> 4u) & 0x0fu]);
        output.push_back(hex[character & 0x0fu]);
      } else {
        output.push_back(static_cast<char>(character));
      }
      break;
    }
  }
  output.push_back('"');
}

[[nodiscard]] bool startsWith(std::string_view value,
                              std::string_view prefix) noexcept {
  return value.size() >= prefix.size() &&
         value.substr(0u, prefix.size()) == prefix;
}

[[nodiscard]] bool lowercaseHex(std::string_view value,
                                std::size_t size) noexcept {
  return value.size() == size &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] bool parseUInt(std::string_view value,
                             std::uint32_t &result) noexcept {
  if (value.empty())
    return false;
  std::uint64_t parsed = 0u;
  for (const unsigned char character : value) {
    if (character < '0' || character > '9')
      return false;
    parsed = parsed * 10u + static_cast<std::uint64_t>(character - '0');
    if (parsed > 0xffffffffu)
      return false;
  }
  result = static_cast<std::uint32_t>(parsed);
  return true;
}

[[nodiscard]] std::string normalizedPathKey(
    const std::filesystem::path &path) {
  std::string value = path.lexically_normal().generic_string();
  for (char &character : value) {
    if (character >= 'A' && character <= 'Z')
      character = static_cast<char>(character - 'A' + 'a');
  }
  while (value.size() > 1u && value.back() == '/')
    value.pop_back();
  return value;
}

[[nodiscard]] bool sameOrNested(std::string_view left,
                                std::string_view right) noexcept {
  if (left == right)
    return true;
  const auto nested = [](std::string_view child,
                         std::string_view parent) noexcept {
    return child.size() > parent.size() &&
           child.substr(0u, parent.size()) == parent &&
           (parent.back() == '/' || child[parent.size()] == '/');
  };
  return nested(left, right) || nested(right, left);
}

[[nodiscard]] bool freshAbsoluteDirectoryPath(
    const std::filesystem::path &path) noexcept {
  if (path.empty() || !path.is_absolute())
    return false;
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  return !error && !exists;
}

[[nodiscard]] bool ordinaryDirectory(
    const std::filesystem::path &path) noexcept {
  std::error_code error;
  const std::filesystem::file_status linkStatus =
      std::filesystem::symlink_status(path, error);
  if (error || std::filesystem::is_symlink(linkStatus))
    return false;
  return std::filesystem::is_directory(path, error) && !error;
}

[[nodiscard]] bool nonceResidueFree(const std::filesystem::path &sync,
                                    std::string_view nonce) noexcept {
  std::error_code error;
  for (std::filesystem::directory_iterator current(sync, error), end;
       !error && current != end; current.increment(error)) {
    const std::string name = current->path().filename().generic_string();
    if (startsWith(name, std::string(nonce) + "."))
      return false;
  }
  return !error;
}

[[nodiscard]] std::string digestHex(const SceneDigest &digest) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string output;
  output.reserve(digest.size() * 2u);
  for (const std::uint8_t byte : digest) {
    output.push_back(hex[(byte >> 4u) & 0x0fu]);
    output.push_back(hex[byte & 0x0fu]);
  }
  return output;
}

[[nodiscard]] std::string planHash(const ReplayPlan &plan) {
  const std::string trace = plan.contractTrace();
  const auto *begin = reinterpret_cast<const std::uint8_t *>(trace.data());
  return digestHex(SceneChangeCanonicalizer::computeSha256(
      std::span<const std::uint8_t>(begin, trace.size())));
}

[[nodiscard]] ReplayPlan planForCase(std::string_view caseId) {
  if (isSameFrameGizmoCase(caseId))
    return makeU03SameFrameOsReplayPlan(caseId == kU03EscapeReleaseCase);
  if (isFocusGizmoCase(caseId))
    return makeU03FocusOsReplayPlan(caseId);
  if (caseId == kU03EscapeCase)
    return makeU03EscapeOsReplayPlan();
  if (caseId == kU03GizmoCase)
    return makeU03GizmoOsReplayPlan();
  if (caseId == kU03InspectorCase)
    return makeU03InspectorOsReplayPlan();
  return caseId == kU03Case ? makeU03OsReplayPlan() : makeU02OsReplayPlan();
}

[[nodiscard]] CliParseResult invalid(std::string message,
                                     CliParseResult result = {}) {
  result.mode = CliMode::invalid;
  result.error = std::move(message);
  return result;
}

struct Seen final {
  bool caseId{false};
  bool subcase{false};
  bool startStep{false};
  bool stopStep{false};
  bool evidence{false};
  bool sync{false};
  bool nonce{false};
  bool planHash{false};
  bool executableHash{false};
  bool sourceHash{false};
  bool width{false};
  bool height{false};
  bool timeout{false};

  [[nodiscard]] bool complete() const noexcept {
    return caseId && subcase && startStep && stopStep && evidence && sync &&
           nonce && planHash && executableHash && sourceHash && width &&
           height && timeout;
  }
};

template <typename Assign>
bool assignOnce(bool &seen, std::string_view value, Assign &&assign) {
  if (seen || value.empty())
    return false;
  seen = true;
  return std::forward<Assign>(assign)(value);
}

} // namespace

CliParseResult
parseEditorUiOsReplayCli(std::span<const std::string_view> arguments) {
  CliParseResult result;
  Seen seen;
  bool capability = false;
  std::string capabilityCase;
  bool anyRunArgument = false;

  for (const std::string_view argument : arguments) {
    if (argument == "--editor-os-capability") {
      if (capability)
        return invalid("duplicate --editor-os-capability", std::move(result));
      capability = true;
      continue;
    }
    if (startsWith(argument, "--editor-os-capability-case=")) {
      if (!capabilityCase.empty())
        return invalid("duplicate --editor-os-capability-case",
                       std::move(result));
      capabilityCase = std::string(argument.substr(28u));
      if (capabilityCase != kU02Case && capabilityCase != kU03Case &&
          capabilityCase != kU03InspectorCase && capabilityCase != kU03GizmoCase &&
          !isEscapeGizmoCase(capabilityCase) &&
          !isFocusGizmoCase(capabilityCase))
        return invalid("unknown capability case", std::move(result));
      continue;
    }
    if (!startsWith(argument, "--editor-os-")) {
      result.unconsumed.emplace_back(argument);
      continue;
    }

    anyRunArgument = true;
    const std::size_t separator = argument.find('=');
    if (separator == std::string_view::npos)
      return invalid("OS replay argument requires =value", std::move(result));
    const std::string_view key = argument.substr(0u, separator);
    const std::string_view value = argument.substr(separator + 1u);
    bool accepted = false;
    if (key == "--editor-os-case") {
      accepted = assignOnce(seen.caseId, value, [&](std::string_view item) {
        result.options.caseId = item;
        return true;
      });
    } else if (key == "--editor-os-subcase") {
      accepted = assignOnce(seen.subcase, value, [&](std::string_view item) {
        result.options.subcase = item;
        return true;
      });
    } else if (key == "--editor-os-start-step") {
      accepted = assignOnce(seen.startStep, value, [&](std::string_view item) {
        return parseUInt(item, result.options.startStep);
      });
    } else if (key == "--editor-os-stop-step") {
      accepted = assignOnce(seen.stopStep, value, [&](std::string_view item) {
        return parseUInt(item, result.options.stopStep);
      });
    } else if (key == "--editor-os-evidence-dir") {
      accepted = assignOnce(seen.evidence, value, [&](std::string_view item) {
        result.options.evidenceDirectory = std::filesystem::path(item);
        return true;
      });
    } else if (key == "--editor-os-sync-dir") {
      accepted = assignOnce(seen.sync, value, [&](std::string_view item) {
        result.options.syncDirectory = std::filesystem::path(item);
        return true;
      });
    } else if (key == "--editor-os-run-nonce") {
      accepted = assignOnce(seen.nonce, value, [&](std::string_view item) {
        result.options.runNonce = item;
        return true;
      });
    } else if (key == "--editor-os-plan-sha256") {
      accepted = assignOnce(seen.planHash, value, [&](std::string_view item) {
        result.options.planSha256 = item;
        return true;
      });
    } else if (key == "--editor-os-executable-sha256") {
      accepted = assignOnce(seen.executableHash, value,
                            [&](std::string_view item) {
                              result.options.executableSha256 = item;
                              return true;
                            });
    } else if (key == "--editor-os-source-sha256") {
      accepted = assignOnce(seen.sourceHash, value, [&](std::string_view item) {
        result.options.sourceSha256 = item;
        return true;
      });
    } else if (key == "--editor-os-width") {
      accepted = assignOnce(seen.width, value, [&](std::string_view item) {
        return parseUInt(item, result.options.width);
      });
    } else if (key == "--editor-os-height") {
      accepted = assignOnce(seen.height, value, [&](std::string_view item) {
        return parseUInt(item, result.options.height);
      });
    } else if (key == "--editor-os-timeout-ms") {
      accepted = assignOnce(seen.timeout, value, [&](std::string_view item) {
        return parseUInt(item, result.options.timeoutMilliseconds);
      });
    } else {
      return invalid("unknown --editor-os-* argument", std::move(result));
    }
    if (!accepted)
      return invalid("duplicate, empty, or malformed OS replay argument",
                     std::move(result));
  }

  if (capability) {
    if (anyRunArgument || !result.unconsumed.empty())
      return invalid("capability mode must be the only argument",
                     std::move(result));
    result.mode = CliMode::capability;
    return result;
  }
  if (!anyRunArgument)
    return capabilityCase.empty() ? result
        : invalid("capability case requires capability mode", std::move(result));
  if (!seen.complete())
    return invalid("OS replay run arguments are incomplete", std::move(result));

  if (!capabilityCase.empty())
    return invalid("capability case cannot be combined with a run", std::move(result));
  if (result.options.caseId != kU02Case && result.options.caseId != kU03Case &&
      result.options.caseId != kU03InspectorCase && result.options.caseId != kU03GizmoCase &&
      !isEscapeGizmoCase(result.options.caseId) &&
      !isFocusGizmoCase(result.options.caseId))
    return invalid("case is not registered", std::move(result));
  if (result.options.caseId == kU03Case)
    return invalid("full U03 viewport/gizmo observations are not integrated",
                   std::move(result));
  const ReplayPlan selectedPlan = planForCase(result.options.caseId);
  if (!selectedPlan.validate().valid || selectedPlan.steps.empty() ||
      result.options.startStep > result.options.stopStep ||
      result.options.stopStep >= selectedPlan.steps.size())
    return invalid("case/subcase/step range is not registered",
                   std::move(result));
  if (result.options.caseId == kU02Case) {
    const bool mainRange = result.options.subcase == "main" &&
                           result.options.startStep == 0u &&
                           result.options.stopStep == 20u;
    const bool uniqueRange = result.options.subcase == "unique-root" &&
                             result.options.startStep == 21u &&
                             result.options.stopStep == 28u;
    if (!mainRange && !uniqueRange)
      return invalid("case/subcase/step range is not registered",
                     std::move(result));
  }
  if ((result.options.caseId == kU03InspectorCase || result.options.caseId == kU03GizmoCase ||
       (isEscapeGizmoCase(result.options.caseId) ||
        isFocusGizmoCase(result.options.caseId))) &&
      (result.options.subcase != "main" || result.options.startStep != 0u ||
       result.options.stopStep + 1u != selectedPlan.steps.size()))
    return invalid("partial run must cover the complete scoped plan",
                   std::move(result));
  for (std::uint32_t index = result.options.startStep;
       index <= result.options.stopStep; ++index) {
    if (selectedPlan.steps[index].subcase != result.options.subcase)
      return invalid("case/subcase/step range is not registered",
                     std::move(result));
  }
  const std::string selectedPlanHash = planHash(selectedPlan);
  if (!lowercaseHex(result.options.runNonce, 32u) ||
      !lowercaseHex(result.options.planSha256, 64u) ||
      result.options.planSha256 != selectedPlanHash ||
      !lowercaseHex(result.options.executableSha256, 64u) ||
      !lowercaseHex(result.options.sourceSha256, 64u))
    return invalid("OS replay nonce/hash identity is invalid",
                   std::move(result));
  if (result.options.width < 640u || result.options.width > 3840u ||
      result.options.height < 480u || result.options.height > 2160u ||
      result.options.timeoutMilliseconds < 10000u ||
      result.options.timeoutMilliseconds > 900000u)
    return invalid("OS replay extent/timeout is outside the contract",
                   std::move(result));
  if (!freshAbsoluteDirectoryPath(result.options.evidenceDirectory) ||
      !result.options.syncDirectory.is_absolute())
    return invalid("OS replay evidence must be absolute/fresh and sync absolute",
                   std::move(result));
  const std::string evidence = normalizedPathKey(result.options.evidenceDirectory);
  const std::string sync = normalizedPathKey(result.options.syncDirectory);
  if (sameOrNested(evidence, sync) || sameOrNested(sync, evidence))
    return invalid("OS replay evidence/sync directories must be distinct",
                   std::move(result));
  const std::filesystem::path appDirectory =
      result.options.evidenceDirectory.lexically_normal().parent_path();
  const std::filesystem::path authorityRoot = appDirectory.parent_path();
  const std::filesystem::path expectedSync = authorityRoot / "sync";
  if (normalizedPathKey(appDirectory.filename()) != "app" ||
      normalizedPathKey(expectedSync) != sync ||
      !ordinaryDirectory(authorityRoot) || !ordinaryDirectory(appDirectory) ||
      !ordinaryDirectory(result.options.syncDirectory) ||
      !nonceResidueFree(result.options.syncDirectory,
                        result.options.runNonce))
    return invalid("OS replay paths violate authority-root/reparse/nonce rules",
                   std::move(result));

  result.mode = CliMode::run;
  return result;
}

namespace {

std::string capabilityJson(bool runtimeIntegrated) {
  const ReplayPlan plan = makeU02OsReplayPlan();
  const PlanValidation validation = plan.validate();
  const bool integrated = runtimeIntegrated && validation.valid &&
                          plan.steps.size() == 29u &&
                          plan.phaseCount() == 91u;
  std::string output =
      "{\"documentKind\":\"mgif.editor-ui-os-replay-capability\","
      "\"schemaVersion\":1,\"protocol\":";
  appendJsonString(output, kProtocol);
  output += ",\"case\":";
  appendJsonString(output, kU02Case);
  output += ",\"integration\":";
  output += integrated ? "true" : "false";
  output += ",\"sendInputOnly\":true,\"planSha256\":";
  appendJsonString(output, kPlanContractSha256);
  output += ",\"sourceRole\":";
  appendJsonString(output, kCapabilitySourceRole);
  output +=
      ",\"requiresHarnessExecutableHashBinding\":true,"
      "\"stepCount\":29,\"phaseCount\":91,\"processes\":["
      "{\"subcase\":\"main\",\"startStep\":0,\"stopStep\":20},"
      "{\"subcase\":\"unique-root\",\"startStep\":21,"
      "\"stopStep\":28}]}";
  return output;
}

std::string capabilityJsonU03(bool runtimeIntegrated, std::string_view caseId) {
  const ReplayPlan plan = planForCase(caseId);
  const PlanValidation validation = plan.validate();
  const bool integrated = runtimeIntegrated && validation.valid &&
                          (caseId == kU03InspectorCase || caseId == kU03GizmoCase ||
                           isEscapeGizmoCase(caseId) || isFocusGizmoCase(caseId));
  std::string output =
      "{\"documentKind\":\"mgif.editor-ui-os-replay-capability\","
      "\"schemaVersion\":1,\"protocol\":";
  appendJsonString(output, kProtocol);
  output += ",\"case\":";
  appendJsonString(output, caseId);
  output += ",\"parentCase\":\"U03-transform-gizmo\",\"coverageComplete\":false";
  output += ",\"integration\":";
  output += integrated ? "true" : "false";
  output += ",\"sendInputOnly\":true,\"planSha256\":";
  appendJsonString(output, planHash(plan));
  output += ",\"sourceRole\":";
  appendJsonString(output, kCapabilitySourceRole);
  output += ",\"requiresHarnessExecutableHashBinding\":true,\"stepCount\":" +
            std::to_string(plan.steps.size()) + ",\"phaseCount\":" +
            std::to_string(plan.phaseCount()) + ",\"processes\":[";
  bool first = true;
  for (std::size_t index = 0; index < plan.steps.size();) {
    const std::string &subcase = plan.steps[index].subcase;
    std::size_t end = index;
    while (end + 1u < plan.steps.size() &&
           plan.steps[end + 1u].subcase == subcase)
      ++end;
    if (!first)
      output += ",";
    first = false;
    output += "{\"subcase\":";
    appendJsonString(output, subcase);
    output += ",\"startStep\":" + std::to_string(index) +
              ",\"stopStep\":" + std::to_string(end) + "}";
    index = end + 1u;
  }
  output += "]}";
  return output;
}

} // namespace

std::string editorUiOsReplayCapabilityJson() {
  return capabilityJson(false);
}

std::string editorUiOsReplayCapabilityJson(CapabilityIntegrationToken) {
  return capabilityJson(true);
}

std::string editorUiOsReplayCapabilityJson(CapabilityIntegrationToken,
                                           std::string_view caseId) {
  if (caseId == kU03Case || caseId == kU03InspectorCase || caseId == kU03GizmoCase ||
      isEscapeGizmoCase(caseId) || isFocusGizmoCase(caseId))
    return capabilityJsonU03(true, caseId);
  return capabilityJson(true);
}

} // namespace demo::editor::testing::osreplay
