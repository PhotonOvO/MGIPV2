#include "MinimalLatestApp.h"
#include "editor/EditorTransactionPath.h"

#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace
{
using AutomationMode = MinimalLatestApp::AutomationMode;
using AutomationOptions = MinimalLatestApp::AutomationOptions;

[[nodiscard]] std::string_view optionValue(std::string_view argument, std::string_view name)
{
	const std::string prefix = std::string(name) + "=";
	if(!argument.starts_with(prefix))
	{
		return {};
	}
	return argument.substr(prefix.size());
}

[[nodiscard]] uint32_t parseFrameCount(std::string_view value, const char* optionName)
{
	if(value.empty())
	{
		throw std::invalid_argument(std::string(optionName) + " requires a value");
	}
	char* end = nullptr;
	errno = 0;
	const std::string text(value);
	const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
	if(errno != 0 || end == nullptr || *end != '\0' || parsed > std::numeric_limits<uint32_t>::max())
	{
		throw std::invalid_argument(std::string("Invalid frame count for ") + optionName);
	}
	return static_cast<uint32_t>(parsed);
}

[[nodiscard]] float parseFixedDelta(std::string_view value)
{
	if(value.empty())
	{
		throw std::invalid_argument("--fixed-dt requires a value");
	}
	char* end = nullptr;
	errno = 0;
	const std::string text(value);
	const float parsed = std::strtof(text.c_str(), &end);
	if(errno != 0 || end == nullptr || *end != '\0' || !std::isfinite(parsed) || !(parsed > 0.0f))
	{
		throw std::invalid_argument("--fixed-dt must be a positive finite number");
	}
	return parsed;
}

void validateVisualSha256(std::string_view value, const char* optionName)
{
	if(value.size() != 64u || !std::all_of(value.begin(), value.end(), [](char character) {
		return std::isxdigit(static_cast<unsigned char>(character)) != 0;
	}))
	{
		throw std::invalid_argument(std::string(optionName) + " must be a 64-character hexadecimal SHA-256");
	}
}

void validateEditorTransactionOptions(AutomationOptions& options)
{
	if(options.editorTransactionDirectory.empty())
	{
		return;
	}
	if(options.mode == AutomationMode::none || !options.noUi || !options.autoExit)
	{
		throw std::invalid_argument(
			"--editor-transaction-dir requires --automation, --no-ui, and --auto-exit");
	}
	if(options.warmupFrames != 8u || options.motionFrames != 24u
	   || options.holdFrames != 8u || options.captureControlFrame)
	{
		throw std::invalid_argument(
			"--editor-transaction-dir requires exactly 8/24/8 frames and no control frame");
	}
	const demo::editor::automation::EditorTransactionDirectoryValidation validated =
		demo::editor::automation::validateEditorTransactionDirectory(
			options.editorTransactionDirectory,
			std::filesystem::path{"resources/GLTF_Sponza/sponza.gltf"});
	options.editorTransactionDirectory = validated.directory;
}

[[nodiscard]] AutomationOptions parseAutomationOptions(int argc, char** argv, bool& showHelp)
{
	AutomationOptions options{};
	for(int index = 1; index < argc; ++index)
	{
		const std::string_view argument(argv[index]);
		if(argument == "--help" || argument == "-h")
		{
			showHelp = true;
		}
		else if(const std::string_view value = optionValue(argument, "--backend"); !value.empty())
		{
			if(value == "vulkan") options.backend = demo::rhi::BackendType::vulkan;
			else if(value == "d3d12") options.backend = demo::rhi::BackendType::d3d12;
			else throw std::invalid_argument("Unknown --backend value: " + std::string(value));
		}
		else if(const std::string_view value = optionValue(argument, "--automation"); !value.empty())
		{
			if(value == "csm-translate-stop") options.mode = AutomationMode::csmTranslateStop;
			else if(value == "csm-rotate-stop") options.mode = AutomationMode::csmRotateStop;
			else throw std::invalid_argument("Unknown --automation mode: " + std::string(value));
		}
		else if(const std::string_view value = optionValue(argument, "--fixed-dt"); !value.empty())
		{
			options.fixedDeltaSeconds = parseFixedDelta(value);
		}
		else if(const std::string_view value = optionValue(argument, "--warmup-frames"); !value.empty())
		{
			options.warmupFrames = parseFrameCount(value, "--warmup-frames");
		}
		else if(const std::string_view value = optionValue(argument, "--motion-frames"); !value.empty())
		{
			options.motionFrames = parseFrameCount(value, "--motion-frames");
		}
		else if(const std::string_view value = optionValue(argument, "--hold-frames"); !value.empty())
		{
			options.holdFrames = parseFrameCount(value, "--hold-frames");
		}
		else if(argument == "--no-ui")
		{
			options.noUi = true;
		}
		else if(argument == "--no-post")
		{
			options.noPost = true;
		}
		else if(argument == "--no-ddgi")
		{
			options.noDdgi = true;
		}
		else if(argument == "--taa")
		{
			options.taa = true;
		}
		else if(argument == "--auto-exit")
		{
			options.autoExit = true;
		}
		else if(argument == "--capture-control-frame")
		{
			options.captureControlFrame = true;
		}
		else if(argument == "--editor-empty")
		{
			options.editorEmptyStart = true;
		}
		else if(const std::string_view value = optionValue(argument, "--editor-scene"); !value.empty())
		{
			options.editorStartupScene = std::filesystem::path(std::string(value));
		}
		else if(argument == "--editor-window-undecorated")
		{
			// Test-only: drop the non-client frame so a client extent equal to
			// the host virtual screen (2560x1440) stays reachable.  A decorated
			// window clamps to 2560x1421 on this host.
			options.windowUndecorated = true;
		}
		else if(const std::string_view value = optionValue(argument, "--capture-sync-dir"); !value.empty())
		{
			options.captureSyncDirectory = std::filesystem::path(std::string(value));
		}
		else if(const std::string_view value = optionValue(argument, "--capture-sync-timeout-ms"); !value.empty())
		{
			options.captureSyncTimeoutMilliseconds = parseFrameCount(value, "--capture-sync-timeout-ms");
		}
		else if(const std::string_view value = optionValue(argument, "--metrics-output"); !value.empty())
		{
			options.metricsOutputPath = std::filesystem::path(std::string(value));
		}
		else if(const std::string_view value = optionValue(argument, "--editor-visual-case"); !value.empty())
		{
			options.editorVisualCase = std::string(value);
		}
		else if(const std::string_view value = optionValue(argument, "--editor-visual-state"); !value.empty())
		{
			options.editorVisualStatePath = std::filesystem::path(std::string(value));
		}
			else if(const std::string_view value = optionValue(argument, "--editor-visual-sync-dir"); !value.empty())
			{
				options.editorVisualSyncDirectory = std::filesystem::path(std::string(value));
			}
			else if(const std::string_view value = optionValue(argument, "--editor-visual-variant"); !value.empty())
			{
				options.editorVisualVariant = std::string(value);
			}
			else if(const std::string_view value = optionValue(argument, "--editor-visual-component"); !value.empty())
			{
				const std::uint32_t component = parseFrameCount(value, "--editor-visual-component");
				if(component >= demo::editor::kGizmoComponentCount)
				{
					throw std::invalid_argument("--editor-visual-component must be in [0, 12]");
				}
				options.editorVisualComponent = component;
			}
		else if(const std::string_view value = optionValue(argument, "--editor-visual-dpi"); !value.empty())
		{
			const float dpi = parseFixedDelta(value);
			if(!(dpi >= 1.0f && dpi <= 4.0f))
			{
				throw std::invalid_argument("--editor-visual-dpi must be in [1, 4]");
			}
			options.editorVisualDpi = dpi;
		}
		else if(const std::string_view value = optionValue(argument, "--editor-visual-executable-sha256"); !value.empty())
		{
			validateVisualSha256(value, "--editor-visual-executable-sha256");
			options.editorVisualExecutableSha256 = std::string(value);
		}
		else if(const std::string_view value = optionValue(argument, "--editor-visual-source-sha256"); !value.empty())
		{
			validateVisualSha256(value, "--editor-visual-source-sha256");
			options.editorVisualSourceSha256 = std::string(value);
		}
		else if(argument.starts_with("--editor-transaction-dir="))
		{
			const std::string_view value =
				argument.substr(std::string_view{"--editor-transaction-dir="}.size());
			if(value.empty())
			{
				throw std::invalid_argument(
					"--editor-transaction-dir requires a value");
			}
			options.editorTransactionDirectory =
				std::filesystem::path(std::string(value));
		}
		else
		{
			throw std::invalid_argument("Unknown command-line option: " + std::string(argument));
		}
	}

	if(options.mode != AutomationMode::none
	   && (options.warmupFrames == 0u || options.motionFrames == 0u || options.holdFrames == 0u))
	{
		throw std::invalid_argument("Automation requires warmup, motion, and hold frame counts greater than zero");
	}
	if(options.noPost && options.taa)
	{
		throw std::invalid_argument("--no-post and --taa cannot be used together");
	}
	if(options.editorEmptyStart && !options.editorStartupScene.empty())
	{
		throw std::invalid_argument("--editor-empty and --editor-scene cannot be used together");
	}
	if(!options.captureSyncDirectory.empty() && options.mode == AutomationMode::none)
	{
		throw std::invalid_argument("--capture-sync-dir requires --automation");
	}
	if(options.noDdgi && options.mode == AutomationMode::none)
	{
		throw std::invalid_argument("--no-ddgi requires --automation");
	}
	// The capture-sync protocol publishes one ready/continue marker per application frame.
	// hold=2 would place arm-settled on first-still, so require a distinct pre-settled
	// frame. The production RenderDoc run uses warmup=8, motion=24, hold=8.
	if(!options.captureSyncDirectory.empty() && options.holdFrames < 3u)
	{
		throw std::invalid_argument(
			"--capture-sync-dir requires --hold-frames=3 or greater so first-still, arm-settled, and settled use distinct application frames");
	}
	if(options.captureControlFrame && options.captureSyncDirectory.empty())
	{
		throw std::invalid_argument("--capture-control-frame requires --capture-sync-dir");
	}
	if(options.captureSyncTimeoutMilliseconds == 0u)
	{
		throw std::invalid_argument("--capture-sync-timeout-ms must be greater than zero");
	}
	validateEditorTransactionOptions(options);
	const bool visualPathRequested = !options.editorVisualCase.empty() ||
	                                 !options.editorVisualStatePath.empty() ||
	                                 !options.editorVisualSyncDirectory.empty() ||
	                                 !options.editorVisualVariant.empty() ||
	                                 options.editorVisualComponent.has_value();
	if(visualPathRequested)
	{
		if(options.mode == AutomationMode::none || options.noUi || !options.autoExit)
		{
			throw std::invalid_argument(
				"editor visual capture requires automation, UI, and auto-exit");
		}
		if(options.editorVisualCase != "V01-grid" &&
		   options.editorVisualCase != "V02-gizmo-components" &&
		   options.editorVisualCase != "V03-parent-space" &&
		   options.editorVisualCase != "V04-depth-xray" &&
		   options.editorVisualCase != "V07-dpi-resize")
		{
			throw std::invalid_argument("unknown --editor-visual-case");
		}
		if(options.editorVisualStatePath.empty() ||
		   options.editorVisualSyncDirectory.empty() ||
		   !options.editorVisualStatePath.is_absolute() ||
		   !options.editorVisualSyncDirectory.is_absolute())
		{
			throw std::invalid_argument(
				"editor visual state and sync paths must be absolute and non-empty");
		}
			if(options.editorVisualExecutableSha256.empty() || options.editorVisualSourceSha256.empty())
		{
			throw std::invalid_argument(
				"editor visual executable/source SHA-256 values are required");
			}
			const auto isVariant = [&](std::initializer_list<std::string_view> allowed) {
				return std::find(allowed.begin(), allowed.end(), options.editorVisualVariant) != allowed.end();
			};
			const bool componentAbsent = !options.editorVisualComponent.has_value();
			const bool variantValid =
				(options.editorVisualCase == "V01-grid" &&
				 componentAbsent &&
				 isVariant({"perspective-unit-0.001", "perspective-unit-1", "perspective-unit-1000",
				            "orthographic-unit-0.001", "orthographic-unit-1", "orthographic-unit-1000",
				            "perspective-horizon"})) ||
				(options.editorVisualCase == "V02-gizmo-components" &&
				 options.editorVisualVariant == "component" && options.editorVisualComponent.has_value()) ||
				(options.editorVisualCase == "V03-parent-space" &&
				 componentAbsent &&
				 options.editorVisualVariant == "rotated-negative-nonuniform") ||
				(options.editorVisualCase == "V04-depth-xray" &&
				 componentAbsent &&
				 isVariant({"xray", "occlusion-dither"})) ||
				(options.editorVisualCase == "V07-dpi-resize" &&
				 componentAbsent &&
				 options.editorVisualVariant == "framebuffer-dpi-resize");
			if(!variantValid)
			{
				throw std::invalid_argument("editor visual case/variant/component combination is invalid");
			}
		if(!options.captureSyncDirectory.empty() &&
		   options.captureSyncDirectory != options.editorVisualSyncDirectory)
		{
			throw std::invalid_argument(
				"editor visual sync directory conflicts with capture sync directory");
		}
		// Reuse the existing frame handshake so the visual harness observes the
		// same pre-render boundaries as the renderer smoke protocol.
		options.captureSyncDirectory = options.editorVisualSyncDirectory;
		options.captureControlFrame = true;
	}
	return options;
}

void printUsage()
{
	std::cout
	    << "Usage: Demo [--backend=vulkan|d3d12]\n"
	    << "            [--automation=csm-translate-stop|csm-rotate-stop]\n"
	    << "            [--fixed-dt=SECONDS] [--warmup-frames=N]\n"
	    << "            [--motion-frames=N] [--hold-frames=N]\n"
	    << "            [--no-ui] [--no-post|--taa] [--no-ddgi] [--auto-exit]\n"
	    << "            [--capture-sync-dir=PATH] [--capture-sync-timeout-ms=N]\n"
	    << "            [--capture-control-frame] [--metrics-output=PATH]\n"
	    << "            [--editor-empty|--editor-scene=PATH]\n"
	    << "            [--editor-window-undecorated]\n"
		    << "            [--editor-visual-case=V01-grid|V02-gizmo-components|V03-parent-space|V04-depth-xray|V07-dpi-resize]\n"
		    << "            [--editor-visual-state=ABSOLUTE_JSON] [--editor-visual-sync-dir=ABSOLUTE_DIR]\n"
		    << "            [--editor-visual-variant=NAME] [--editor-visual-component=0..12]\n"
	    << "            [--editor-visual-dpi=1..4]\n"
	    << "            [--editor-visual-executable-sha256=HEX] [--editor-visual-source-sha256=HEX]\n"
	    << "            [--editor-transaction-dir=ABSOLUTE_EMPTY_TEMP_CHILD]"
	    << "  (strict child of the process-configured absolute temp root)"
	    << std::endl;
}
}  // namespace

int main(int argc, char** argv)
{
	// getchar();
	// Get the logger instance
	utils::Logger& logger = utils::Logger::getInstance();
	// logger.enableFileOutput(false);  // Don't write log to file
	logger.setShowFlags(utils::Logger::eSHOW_TIME);
	logger.setLogLevel(utils::Logger::LogLevel::eINFO); // Default is Warning, we show more information
	LOGI("Starting ... ");

	try
	{
		bool showHelp = false;
		const AutomationOptions automationOptions = parseAutomationOptions(argc, argv, showHelp);
		if(showHelp)
		{
			printUsage();
			return 0;
		}

		ASSERT(glfwInit() == GLFW_TRUE, "Could not initialize GLFW!");
		ASSERT(glfwVulkanSupported() == GLFW_TRUE, "GLFW: Vulkan not supported!");

		MinimalLatestApp app({1920, 1080}, automationOptions);
		app.run();

		glfwTerminate();
	}
	catch (const std::exception& e)
	{
		LOGE("%s", e.what());
		return 1;
	}
	return 0;
}
