#pragma once

#include "../common/Common.h"
#include "../rhi/RHIFrameContext.h"
#include "../rhi/RHISwapchain.h"

namespace utils {

class FramePacer
{
public:
  FramePacer()  = default;
  ~FramePacer() = default;

  // Frame rate limiting to monitor refresh rate
  void paceFrame(double targetFPS = 60.0)
  {
    const auto targetFrameTime = std::chrono::duration<double>(1.0 / targetFPS);

    // Pacing the CPU by enforcing at least `refreshInterval` seconds between
    // frames is all we need! If the GPU is fast things are OK; if the GPU is
    // slow then vkWaitSemaphores will take more time in the frame, which
    // will be counted in the CPU time.
    const auto currentTime   = std::chrono::high_resolution_clock::now();
    const auto frameDuration = currentTime - m_lastFrameTime;
    auto       sleepTime     = targetFrameTime - frameDuration;
#ifdef _WIN32
    // On Windows, we know that 1ms is just about the right time to subtract;
    // it's just under the average amount that Windows adds to the sleep call.
    // On Linux the timers are accurate enough that we don't need this.
    sleepTime -= std::chrono::duration<double>(std::chrono::milliseconds(1));
#endif
    if(sleepTime > std::chrono::duration<double>(0))
    {
#ifdef _WIN32
      // On Windows, the default timer might quantize to 15.625 ms; see
      // https://randomascii.wordpress.com/2020/10/04/windows-timer-resolution-the-great-rule-change/ .
      // We use timeBeginPeriod to temporarily increase the resolution to 1 ms.
      timeBeginPeriod(1);
#endif
      std::this_thread::sleep_for(sleepTime);
#ifdef _WIN32
      timeEndPeriod(1);
#endif
    }

    m_lastFrameTime = std::chrono::high_resolution_clock::now();
  }

private:
  std::chrono::high_resolution_clock::time_point m_lastFrameTime;
};

// Helper to return the minimum refresh rate of all monitors.
static double getMonitorsMinRefreshRate()
{
  // We need our target frame rate. We get this once per frame in case the
  // user changes their monitor's frame rate.
  // Ideally we'd get the exact composition rate for the current swapchain;
  // VK_EXT_present_timing will hopefully give us that when it's released.
  // Currently we use GLFW; this means we don't need anything
  // platform-specific, but means we only get an integer frame rate,
  // rounded down, across monitors. We take the minimum to avoid building up
  // frame latency.
  double refreshRate = std::numeric_limits<double>::infinity();
  {
    int           numMonitors = 0;
    GLFWmonitor** monitors    = glfwGetMonitors(&numMonitors);
    for(int i = 0; i < numMonitors; i++)
    {
      const GLFWvidmode* videoMode = glfwGetVideoMode(monitors[i]);
      if(videoMode)
      {
        refreshRate = std::min(refreshRate, static_cast<double>(videoMode->refreshRate));
      }
    }
  }
  // If we have no information about the frame rate or an impossible value,
  // use a default.
  if(std::isinf(refreshRate) || refreshRate <= 0.0)
  {
    refreshRate = 60.0;
  }

  return refreshRate;
}

}  // namespace utils

namespace demo {

bool                   acquireSwapchainImage(rhi::Swapchain& swapchain, uint32_t& imageIndexOut);
rhi::SubmissionReceipt submitFrame(rhi::FrameContext& frameContext, rhi::CommandList& commandList);
rhi::PresentResult     presentFrame(rhi::Swapchain& swapchain);

}  // namespace demo
