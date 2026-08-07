#include "D3D12Surface.h"

#define NOMINMAX
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <Windows.h>

#include <limits>
#include <stdexcept>

namespace demo::rhi::d3d12 {

void D3D12Surface::initD3D12(void* dxgiFactory, void* dxgiAdapter, const WindowHandle& window)
{
  if(dxgiFactory == nullptr || dxgiAdapter == nullptr)
    throw std::runtime_error("D3D12Surface requires an initialized DXGI factory and adapter");
  if(m_nativeWindow != nullptr)
    throw std::runtime_error("D3D12Surface::init called twice");
  if(window.nativeWindow == nullptr)
    throw std::runtime_error("D3D12Surface requires a valid GLFW window");

  GLFWwindow* glfwWindow = static_cast<GLFWwindow*>(window.nativeWindow);
  HWND hwnd = glfwGetWin32Window(glfwWindow);
  if(hwnd == nullptr || !IsWindow(hwnd))
    throw std::runtime_error("D3D12Surface could not resolve a valid Win32 window");

  m_nativeWindow = hwnd;
}

void D3D12Surface::deinit()
{
  m_nativeWindow = nullptr;
}

SurfaceCapabilities D3D12Surface::queryCapabilities() const
{
  HWND hwnd = static_cast<HWND>(m_nativeWindow);
  if(hwnd == nullptr || !IsWindow(hwnd))
    throw std::runtime_error("D3D12Surface::queryCapabilities called before init");

  RECT clientRect{};
  if(!GetClientRect(hwnd, &clientRect))
    throw std::runtime_error("GetClientRect failed for D3D12 surface");

  const uint32_t width = static_cast<uint32_t>(clientRect.right - clientRect.left);
  const uint32_t height = static_cast<uint32_t>(clientRect.bottom - clientRect.top);
  constexpr uint32_t maxExtent = static_cast<uint32_t>(std::numeric_limits<int32_t>::max());

  SurfaceCapabilities capabilities{};
  capabilities.minImageCount = 2;
  capabilities.maxImageCount = 16;
  capabilities.currentExtent = {width, height};
  capabilities.minImageExtent = {1, 1};
  capabilities.maxImageExtent = {maxExtent, maxExtent};
  return capabilities;
}

}  // namespace demo::rhi::d3d12
