#pragma once

#include "../common/Common.h"
#include "../render/FrameSubmission.h"
#include "../render/Renderer.h"
#include "../rhi/RHISurface.h"
#include "../rhi/vulkan/VulkanSurface.h"

#include <memory>

#include "../rhi/vulkan/VulkanCommandList.h"

class MinimalLatestApp
{
public:
  MinimalLatestApp(VkExtent2D size = {800, 600})
      : m_windowSize(size)
  {
    VK_CHECK(volkInitialize());
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#ifdef USE_SLANG
    const char* windowTitle = "Minimal Demo (Slang)";
#else
    const char* windowTitle = "Minimal Demo (GLSL)";
#endif
    m_window  = glfwCreateWindow(m_windowSize.width, m_windowSize.height, windowTitle, nullptr, nullptr);
    m_surface = std::make_unique<demo::rhi::vulkan::VulkanSurface>();
    m_renderer.init(m_window, *m_surface, m_vSync);
    m_selectedMaterial = m_renderer.getMaterialHandle(0);
  }

  ~MinimalLatestApp()
  {
    m_renderer.shutdown(*m_surface);
    glfwDestroyWindow(m_window);
  }

  void run()
  {
    while(!glfwWindowShouldClose(m_window))
    {
      m_framePacer.paceFrame(m_vSync ? utils::getMonitorsMinRefreshRate() : 10000.0);
      glfwPollEvents();
      if(glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) == GLFW_TRUE)
      {
        ImGui_ImplGlfw_Sleep(10);
        continue;
      }

      ImGui_ImplVulkan_NewFrame();
      ImGui_ImplGlfw_NewFrame();
      ImGui::NewFrame();

      const ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingInCentralNode;
      ImGuiID dockID = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), dockFlags);
      if(!ImGui::DockBuilderGetNode(dockID)->IsSplitNode() && !ImGui::FindWindowByName("Viewport"))
      {
        ImGui::DockBuilderDockWindow("Viewport", dockID);
        ImGui::DockBuilderGetCentralNode(dockID)->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
        ImGuiID leftID = ImGui::DockBuilderSplitNode(dockID, ImGuiDir_Left, 0.2f, nullptr, &dockID);
        ImGui::DockBuilderDockWindow("Settings", leftID);
      }

      if(ImGui::BeginMainMenuBar())
      {
        if(ImGui::BeginMenu("File"))
        {
          if(ImGui::MenuItem("vSync", "", &m_vSync))
          {
            m_renderer.requestSwapchainRebuild();
          }
          ImGui::Separator();
          if(ImGui::MenuItem("Exit"))
            glfwSetWindowShouldClose(m_window, true);
          ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
      }

      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
      ImGui::Begin("Viewport");
      ImVec2              viewportContentSize = ImGui::GetContentRegionAvail();
      demo::rhi::Extent2D requestedViewportSize{uint32_t(viewportContentSize.x), uint32_t(viewportContentSize.y)};
      if(requestedViewportSize.width > 0 && requestedViewportSize.height > 0
         && (requestedViewportSize.width != m_viewportSize.width || requestedViewportSize.height != m_viewportSize.height))
      {
        m_viewportSize = requestedViewportSize;
        m_renderer.resize(m_viewportSize);
      }

      const demo::TextureHandle viewportTextureHandle = m_renderer.getViewportTextureHandle();
      ImGui::Image(m_renderer.getViewportTextureID(viewportTextureHandle), viewportContentSize);
      ImGui::SetCursorPos(ImVec2(0, 0));
      ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
      ImGui::End();
      ImGui::PopStyleVar();

      if(ImGui::Begin("Settings"))
      {
        const demo::MaterialHandle image1Material = m_renderer.getMaterialHandle(0);
        const demo::MaterialHandle image2Material = m_renderer.getMaterialHandle(1);

        bool useImage1 = m_selectedMaterial == image1Material;
        if(ImGui::RadioButton("Image 1", useImage1))
        {
          m_selectedMaterial = image1Material;
        }

        bool useImage2 = m_selectedMaterial == image2Material;
        if(ImGui::RadioButton("Image 2", useImage2))
        {
          m_selectedMaterial = image2Material;
        }

        ImGui::Separator();
        float color[3] = {m_clearColor.r, m_clearColor.g, m_clearColor.b};
        if(ImGui::ColorPicker3("Clear Color", color))
        {
          m_clearColor.r = color[0];
          m_clearColor.g = color[1];
          m_clearColor.b = color[2];
        }
      }
      ImGui::End();
      ImGui::Render();

      demo::RenderParams frameParams{};
      frameParams.viewportSize   = m_viewportSize;
      frameParams.deltaTime      = ImGui::GetIO().DeltaTime;
      frameParams.timeSeconds    = static_cast<float>(ImGui::GetTime());
      frameParams.materialHandle = m_selectedMaterial;
      frameParams.clearColor     = m_clearColor;
      frameParams.recordUi       = [](demo::rhi::CommandList& cmd) {
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), demo::rhi::vulkan::getNativeCommandBuffer(cmd));
      };

      m_renderer.render(frameParams);

      ImGui::EndFrame();
      if((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
      {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
      }
    }
  }

private:
  GLFWwindow*                                       m_window{};
  std::unique_ptr<demo::rhi::vulkan::VulkanSurface> m_surface;
  VkExtent2D                                        m_windowSize{800, 600};
  demo::rhi::Extent2D                               m_viewportSize{800, 600};
  demo::Renderer                                    m_renderer;
  utils::FramePacer                                 m_framePacer;

  bool                       m_vSync{true};
  demo::MaterialHandle       m_selectedMaterial{};
  demo::rhi::ClearColorValue m_clearColor{0.2f, 0.2f, 0.3f, 1.0f};
};
