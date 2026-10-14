#include "EditorUiOsReplayCli.h"

#include <iostream>
#include <exception>
#include <string_view>
#include <utility>
#include <vector>

#if defined(MGIF_EDITOR_OS_REPLAY)
#include "../MinimalLatestApp.h"
#endif

int main(int argc, char **argv) {
  namespace os = demo::editor::testing::osreplay;
  std::vector<std::string_view> arguments;
  arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0u);
  for (int index = 1; index < argc; ++index)
    arguments.emplace_back(argv[index]);

  const os::CliParseResult parsed = os::parseEditorUiOsReplayCli(arguments);
  if (!parsed.accepted()) {
    std::cerr << "editor_ui_os_replay: " << parsed.error << '\n';
    return 1;
  }
#if defined(MGIF_EDITOR_OS_REPLAY)
  if (parsed.mode == os::CliMode::capability) {
    std::string_view capabilityCase = os::kU02Case;
    for (const std::string_view argument : arguments) {
      constexpr std::string_view prefix =
          "--editor-os-capability-case=";
      if (argument.starts_with(prefix))
        capabilityCase = argument.substr(prefix.size());
    }
    std::cout << os::editorUiOsReplayCapabilityJson(
                     os::editorUiOsReplayIntegrationToken(), capabilityCase)
              << '\n';
    return 0;
  }
  if (!parsed.runRequested() || !parsed.unconsumed.empty()) {
    std::cerr << "editor_ui_os_replay: complete OS run arguments required\n";
    return 1;
  }

  utils::Logger &logger = utils::Logger::getInstance();
  logger.setShowFlags(utils::Logger::eSHOW_TIME);
  logger.setLogLevel(utils::Logger::LogLevel::eINFO);
  try {
    MinimalLatestApp::AutomationOptions options;
    options.backend = demo::rhi::BackendType::vulkan;
    options.mode = MinimalLatestApp::AutomationMode::csmTranslateStop;
    options.fixedDeltaSeconds = 1.0f / 60.0f;
    options.warmupFrames = 8u;
    options.motionFrames = 24u;
    options.holdFrames = 8u;
    options.noUi = false;
    options.noPost = true;
    options.noDdgi = true;
    options.taa = false;
    options.autoExit = true;
    options.captureControlFrame = false;
    options.captureSyncTimeoutMilliseconds =
        parsed.options.timeoutMilliseconds;
    options.editorOsReplay = parsed.options;

    ASSERT(glfwInit() == GLFW_TRUE, "Could not initialize GLFW!");
    ASSERT(glfwVulkanSupported() == GLFW_TRUE,
           "GLFW: Vulkan not supported!");
    MinimalLatestApp app(
        {parsed.options.width, parsed.options.height}, std::move(options));
    app.run();
    glfwTerminate();
    return 0;
  } catch (const std::exception &error) {
    LOGE("[EDITOR_OS_REPLAY] marker=process-failed reason=%s", error.what());
    glfwTerminate();
    return 1;
  }
#else
  std::cerr << "editor_ui_os_replay: integration compile define is missing\n";
  return 1;
#endif
}
