#include "../MinimalLatestApp.h"

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::string_view optionValue(std::string_view argument,
                             std::string_view name) {
  const std::string prefix = std::string(name) + "=";
  return argument.starts_with(prefix) ? argument.substr(prefix.size())
                                      : std::string_view{};
}

std::uint32_t parseUnsigned(std::string_view value,
                            std::string_view option) {
  if (value.empty())
    throw std::invalid_argument(std::string(option) + " requires a value");
  errno = 0;
  char *end = nullptr;
  const std::string text(value);
  const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
  if (errno != 0 || end == nullptr || *end != '\0' ||
      parsed > std::numeric_limits<std::uint32_t>::max())
    throw std::invalid_argument("invalid unsigned value for " +
                                std::string(option));
  return static_cast<std::uint32_t>(parsed);
}

void validateSha256(std::string_view value, std::string_view option) {
  if (value.size() != 64u ||
      !std::all_of(value.begin(), value.end(), [](char character) {
        return std::isxdigit(static_cast<unsigned char>(character)) != 0;
      }))
    throw std::invalid_argument(std::string(option) +
                                " requires 64 hexadecimal characters");
}

bool registeredCase(std::string_view value) {
  static constexpr std::array<std::string_view, 8> cases = {
      "U01-empty-new-open", "U02-hierarchy", "U03-transform-gizmo",
      "U04-material", "U05-camera-light-vis", "U06-merge-compact-save",
      "U07-modified-failures", "U08-large-unicode-dpi"};
  return std::find(cases.begin(), cases.end(), value) != cases.end();
}

struct Arguments final {
  MinimalLatestApp::AutomationOptions options;
  demo::rhi::Extent2D extent{1280u, 720u};
  bool help{false};
};

Arguments parseArguments(int argc, char **argv) {
  Arguments parsed;
  parsed.options.backend = demo::rhi::BackendType::vulkan;
  parsed.options.mode = MinimalLatestApp::AutomationMode::csmTranslateStop;
  parsed.options.fixedDeltaSeconds = 1.0f / 60.0f;
  parsed.options.warmupFrames = 8u;
  parsed.options.motionFrames = 24u;
  parsed.options.holdFrames = 8u;
  parsed.options.noUi = false;
  parsed.options.noPost = true;
  parsed.options.noDdgi = true;
  parsed.options.autoExit = true;
  parsed.options.captureControlFrame = false;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      parsed.help = true;
    } else if (const auto value =
                   optionValue(argument, "--editor-live-case");
               !value.empty()) {
      if (!registeredCase(value))
        throw std::invalid_argument("unknown editor live replay case");
      parsed.options.editorLiveCase = std::string(value);
    } else if (const auto value =
                   optionValue(argument, "--editor-live-evidence-dir");
               !value.empty()) {
      parsed.options.editorLiveEvidenceDirectory =
          std::filesystem::path(std::string(value));
    } else if (const auto value =
                   optionValue(argument, "--editor-live-sync-dir");
               !value.empty()) {
      parsed.options.editorLiveSyncDirectory =
          std::filesystem::path(std::string(value));
    } else if (const auto value = optionValue(
                   argument, "--editor-live-executable-sha256");
               !value.empty()) {
      validateSha256(value, "--editor-live-executable-sha256");
      parsed.options.editorLiveExecutableSha256 = std::string(value);
    } else if (const auto value =
                   optionValue(argument, "--editor-live-source-sha256");
               !value.empty()) {
      validateSha256(value, "--editor-live-source-sha256");
      parsed.options.editorLiveSourceSha256 = std::string(value);
    } else if (const auto value =
                   optionValue(argument, "--capture-sync-timeout-ms");
               !value.empty()) {
      parsed.options.captureSyncTimeoutMilliseconds =
          parseUnsigned(value, "--capture-sync-timeout-ms");
    } else if (const auto value =
                   optionValue(argument, "--editor-ui-dpi-scale");
               !value.empty()) {
      float scale = 0.0f;
      try {
        scale = std::stof(std::string(value));
      } catch (const std::exception&) {
        throw std::invalid_argument(
            "--editor-ui-dpi-scale must be a decimal number");
      }
      if (!(scale >= 1.0f && scale <= 4.0f))
        throw std::invalid_argument(
            "--editor-ui-dpi-scale must be in [1, 4]");
      parsed.options.editorLiveUiDpiScale = scale;
    } else if (argument == "--editor-window-undecorated") {
      // The pinned resolution axis includes 2560x1440, which a decorated
      // window cannot present as a client area on a 2560x1440 display
      // because the default WM_GETMINMAXINFO max track size is the virtual
      // screen and the non-client frame consumes part of it.
      parsed.options.windowUndecorated = true;
    } else if (const auto value = optionValue(argument, "--width");
               !value.empty()) {
      parsed.extent.width = parseUnsigned(value, "--width");
    } else if (const auto value = optionValue(argument, "--height");
               !value.empty()) {
      parsed.extent.height = parseUnsigned(value, "--height");
    } else if (const auto value = optionValue(argument, "--backend");
               !value.empty()) {
      if (value != "vulkan")
        throw std::invalid_argument(
            "editor live replay currently requires Vulkan");
    } else {
      throw std::invalid_argument("unknown editor live replay option: " +
                                  std::string(argument));
    }
  }

  if (!parsed.help) {
    if (parsed.options.editorLiveCase.empty() ||
        parsed.options.editorLiveEvidenceDirectory.empty() ||
        parsed.options.editorLiveSyncDirectory.empty() ||
        parsed.options.editorLiveExecutableSha256.empty() ||
        parsed.options.editorLiveSourceSha256.empty())
      throw std::invalid_argument(
          "case, fresh evidence/sync directories, and identities are required");
    if (parsed.extent.width < 640u || parsed.extent.height < 480u)
      throw std::invalid_argument(
          "editor live replay extent must be at least 640x480");
    if (parsed.options.captureSyncTimeoutMilliseconds == 0u)
      throw std::invalid_argument(
          "capture sync timeout must be greater than zero");
  }
  return parsed;
}

void printUsage() {
  std::cout
      << "Usage: editor_ui_live_replay"
      << " --editor-live-case=U01-empty-new-open|U02-hierarchy|"
         "U03-transform-gizmo|U04-material|U05-camera-light-vis|"
         "U06-merge-compact-save|U07-modified-failures|"
         "U08-large-unicode-dpi"
      << " --editor-live-evidence-dir=ABSENT_ABSOLUTE_DIR"
      << " --editor-live-sync-dir=ABSENT_ABSOLUTE_DIR"
      << " --editor-live-executable-sha256=HEX"
      << " --editor-live-source-sha256=HEX"
      << " [--width=N] [--height=N]"
      << " [--editor-ui-dpi-scale=N] [--editor-window-undecorated]"
      << " [--capture-sync-timeout-ms=N] [--backend=vulkan]\n"
      << "All eight registered cases are implemented; an unregistered case "
         "fails closed.\n";
}

} // namespace

int main(int argc, char **argv) {
  utils::Logger &logger = utils::Logger::getInstance();
  logger.setShowFlags(utils::Logger::eSHOW_TIME);
  logger.setLogLevel(utils::Logger::LogLevel::eINFO);
  try {
    const Arguments arguments = parseArguments(argc, argv);
    if (arguments.help) {
      printUsage();
      return 0;
    }
    ASSERT(glfwInit() == GLFW_TRUE, "Could not initialize GLFW!");
    ASSERT(glfwVulkanSupported() == GLFW_TRUE,
           "GLFW: Vulkan not supported!");
    MinimalLatestApp app(arguments.extent, arguments.options);
    app.run();
    glfwTerminate();
    return 0;
  } catch (const std::exception &error) {
    LOGE("[EDITOR_LIVE_REPLAY] marker=process-failed reason=%s",
         error.what());
    glfwTerminate();
    return 1;
  }
}

