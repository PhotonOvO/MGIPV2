#pragma once

#include "../common/Common.h"
#include "../render/FrameSubmission.h"
#include "../render/Renderer.h"
#include "../rhi/RHISurface.h"
#include "../rhi/vulkan/VulkanSurface.h"
#include "../loader/GltfLoader.h"
#include "../render/Camera.h"

#include <memory>
#include <optional>

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
    m_gltfLoader       = std::make_unique<demo::GltfLoader>();

    // Initialize camera
    m_camera.setPerspective(45.0f, static_cast<float>(m_windowSize.width) / static_cast<float>(m_windowSize.height), 0.1f, 100.0f);
    m_camera.setPosition(glm::vec3(0.0f, 0.0f, 3.0f));
    m_camera.update();
  }

  ~MinimalLatestApp()
  {
    unloadModel();
    m_renderer.shutdown(*m_surface);
    glfwDestroyWindow(m_window);
  }

  void run()
  {
    while(!glfwWindowShouldClose(m_window))
    {
      m_framePacer.paceFrame(m_vSync ? utils::getMonitorsMinRefreshRate() : 10000.0);
      glfwPollEvents();

      // Camera input handling
      {
          // Keyboard movement
          glm::vec3 moveDir{0.0f};
          if(glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS) moveDir.z += 1.0f;
          if(glfwGetKey(m_window, GLFW_KEY_S) == GLFW_PRESS) moveDir.z -= 1.0f;
          if(glfwGetKey(m_window, GLFW_KEY_A) == GLFW_PRESS) moveDir.x -= 1.0f;
          if(glfwGetKey(m_window, GLFW_KEY_D) == GLFW_PRESS) moveDir.x += 1.0f;
          if(glfwGetKey(m_window, GLFW_KEY_Q) == GLFW_PRESS) moveDir.y += 1.0f;
          if(glfwGetKey(m_window, GLFW_KEY_E) == GLFW_PRESS) moveDir.y -= 1.0f;

          if(glm::length(moveDir) > 0.0f)
          {
              moveDir = glm::normalize(moveDir) * m_moveSpeed * ImGui::GetIO().DeltaTime;
              m_camera.move(moveDir);
          }

          // Mouse rotation (right-click to capture)
          if(glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
          {
              if(!m_cursorCaptured)
              {
                  glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                  double xpos, ypos;
                  glfwGetCursorPos(m_window, &xpos, &ypos);
                  m_lastMousePos = glm::vec2(static_cast<float>(xpos), static_cast<float>(ypos));
                  m_cursorCaptured = true;
              }
              else
              {
                  double xpos, ypos;
                  glfwGetCursorPos(m_window, &xpos, &ypos);
                  float deltaX = static_cast<float>(xpos - m_lastMousePos.x) * m_rotateSpeed;
                  float deltaY = static_cast<float>(ypos - m_lastMousePos.y) * m_rotateSpeed;
                  m_lastMousePos = glm::vec2(xpos, ypos);
                  m_camera.rotate(deltaX, -deltaY);  // Inverted Y for natural feel
              }
          }
          else if(m_cursorCaptured)
          {
              glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
              m_cursorCaptured = false;
          }

          // Update camera matrices
          m_camera.update();

          // Update camera uniforms for rendering
          m_cameraUniforms.view = m_camera.getViewMatrix();
          m_cameraUniforms.projection = m_camera.getProjectionMatrix();
          m_cameraUniforms.viewProjection = m_camera.getViewProjectionMatrix();
          m_cameraUniforms.cameraPosition = m_camera.getPosition();
      }

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
        m_camera.setPerspective(45.0f, static_cast<float>(m_viewportSize.width) / static_cast<float>(m_viewportSize.height), 0.1f, 100.0f);
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

      drawModelLoaderUI();

      ImGui::Render();

      demo::RenderParams frameParams{};
      frameParams.viewportSize   = m_viewportSize;
      frameParams.deltaTime      = ImGui::GetIO().DeltaTime;
      frameParams.timeSeconds    = static_cast<float>(ImGui::GetTime());
      frameParams.materialHandle = m_selectedMaterial;
      frameParams.clearColor     = m_clearColor;
      frameParams.gltfModel      = m_currentModel.has_value() ? &(*m_currentModel) : nullptr;
      frameParams.cameraUniforms = &m_cameraUniforms;
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

  // glTF model loading
  std::unique_ptr<demo::GltfLoader>               m_gltfLoader;
  std::optional<demo::GltfUploadResult>           m_currentModel;
  std::string                                     m_modelPath;
  bool                                            m_modelLoaded = false;

  // Camera
  demo::Camera m_camera;
  float m_moveSpeed{5.0f};       // Units per second
  float m_rotateSpeed{0.1f};     // Mouse sensitivity
  bool m_cursorCaptured{false};  // Mouse capture state
  glm::vec2 m_lastMousePos{0.0f};
  shaderio::CameraUniforms m_cameraUniforms;  // Camera data for rendering

  // UI state
  char m_modelPathBuffer[512] = "resources/shader_ball.gltf";

  void loadModel(const std::string& path);
  void unloadModel();
  void drawModelLoaderUI();
};

inline void MinimalLatestApp::loadModel(const std::string& path)
{
  demo::GltfModel model;
  if(!m_gltfLoader->load(path, model))
  {
    LOGE("Failed to load model: %s, error: %s", path.c_str(), m_gltfLoader->getLastError().c_str());
    return;
  }

  m_renderer.waitForIdle();
  unloadModel();

  // Upload model to GPU
  m_renderer.executeUploadCommand([&model, this](VkCommandBuffer cmd) {
    m_currentModel = m_renderer.uploadGltfModel(model, cmd);
  });

  m_modelPath  = path;
  m_modelLoaded = true;

  LOGI("Loaded glTF model: %s (%zu meshes, %zu materials, %zu textures)", path.c_str(), model.meshes.size(), model.materials.size(),
       model.images.size());
}

inline void MinimalLatestApp::unloadModel()
{
  if(m_modelLoaded && m_currentModel.has_value())
  {
    m_renderer.waitForIdle();
    m_renderer.destroyGltfResources(*m_currentModel);
    m_currentModel.reset();
    m_modelLoaded = false;
  }
}

inline void MinimalLatestApp::drawModelLoaderUI()
{
  if(ImGui::Begin("Model Loader"))
  {
    // Model path input
    ImGui::InputText("Model Path", m_modelPathBuffer, sizeof(m_modelPathBuffer));

    // Load button
    if(ImGui::Button("Load Model"))
    {
      loadModel(std::string(m_modelPathBuffer));
    }

    ImGui::SameLine();

    // Unload button
    if(ImGui::Button("Unload"))
    {
      unloadModel();
    }

    // Preset models
    ImGui::Separator();
    ImGui::Text("Presets:");

    if(ImGui::Button("Triangle (default)"))
    {
      unloadModel();
      m_modelLoaded = false;
    }

    // Current model info
    if(m_modelLoaded)
    {
      ImGui::Separator();
      ImGui::Text("Current Model: %s", m_modelPath.c_str());
      if(m_currentModel.has_value())
      {
        ImGui::Text("  Meshes: %zu", m_currentModel->meshes.size());
        ImGui::Text("  Materials: %zu", m_currentModel->materials.size());
        ImGui::Text("  Textures: %zu", m_currentModel->textures.size());
      }
    }
  }
  ImGui::End();
}
