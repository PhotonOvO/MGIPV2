#include "MinimalLatestApp.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

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
		else if(const std::string_view value = optionValue(argument, "--capture-sync-dir"); !value.empty())
		{
			options.captureSyncDirectory = std::filesystem::path(std::string(value));
		}
		else if(const std::string_view value = optionValue(argument, "--capture-sync-timeout-ms"); !value.empty())
		{
			options.captureSyncTimeoutMilliseconds = parseFrameCount(value, "--capture-sync-timeout-ms");
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
	return options;
}

void printUsage()
{
	std::cout
	    << "Usage: Demo [--automation=csm-translate-stop|csm-rotate-stop]\n"
	    << "            [--fixed-dt=SECONDS] [--warmup-frames=N]\n"
	    << "            [--motion-frames=N] [--hold-frames=N]\n"
	    << "            [--no-ui] [--no-post|--taa] [--no-ddgi] [--auto-exit]\n"
	    << "            [--capture-sync-dir=PATH] [--capture-sync-timeout-ms=N]\n"
	    << "            [--capture-control-frame]\n";
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
