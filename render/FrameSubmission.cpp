#include "FrameSubmission.h"

namespace demo {

bool acquireSwapchainImage(rhi::Swapchain& swapchain, uint32_t& imageIndexOut)
{
  const rhi::AcquireResult acquireResult = swapchain.acquireNextImage();
  if(acquireResult.status == rhi::AcquireResult::Status::outOfDate)
  {
    return false;
  }

  if(acquireResult.status == rhi::AcquireResult::Status::notReady)
  {
    return false;
  }

  imageIndexOut = acquireResult.imageIndex;
  return true;
}

rhi::SubmissionReceipt submitFrame(rhi::FrameContext& frameContext, rhi::CommandList& commandList)
{
  return frameContext.endFrame(&commandList);
}

rhi::PresentResult presentFrame(rhi::Swapchain& swapchain)
{
  return swapchain.present();
}

}  // namespace demo
