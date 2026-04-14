#define VMA_IMPLEMENTATION
#define VMA_LEAK_LOG_FORMAT(format, ...)                                                                               \
  {                                                                                                                    \
    printf((format), __VA_ARGS__);                                                                                     \
    printf("\n");                                                                                                      \
  }

#include "MinimalLatestApp.h"

#ifdef _WIN32
#include <windows.h>
#endif

int main()
{
    getchar();
  // Get the logger instance
  utils::Logger& logger = utils::Logger::getInstance();
  // logger.enableFileOutput(false);  // Don't write log to file
  logger.setShowFlags(utils::Logger::eSHOW_TIME);
  logger.setLogLevel(utils::Logger::LogLevel::eINFO);  // Default is Warning, we show more information
  LOGI("Starting ... ");

  try
  {
    ASSERT(glfwInit() == GLFW_TRUE, "Could not initialize GLFW!");
    ASSERT(glfwVulkanSupported() == GLFW_TRUE, "GLFW: Vulkan not supported!");

    MinimalLatestApp app({800, 600});
    app.run();

    glfwTerminate();
  }
  catch(const std::exception& e)
  {
    LOGE("%s", e.what());
    return 1;
  }
  return 0;
}
