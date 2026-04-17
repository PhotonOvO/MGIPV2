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
#include <future>
#include <atomic>

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
    m_camera.setPosition(glm::vec3(8.0f, 1.5f, 0.0f));
    m_camera.setYawPitch(180.0, 0.0);
    m_camera.update();
    syncLightAnglesFromDirection();


    ImGui::GetIO().ConfigFlags = ImGuiConfigFlags_DockingEnable;

    // Load default scene automatically
    std::string path = "resources/Test/test.gltf";
    loadModelAsync(path);
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

      // Check async loading progress
      updateAsyncLoading();

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
          m_cameraUniforms.inverseViewProjection = glm::inverse(m_cameraUniforms.viewProjection);
          m_cameraUniforms.cameraPosition = m_camera.getPosition();
          m_cameraUniforms.shadowConstantBias = 0.0f;
          m_cameraUniforms.shadowDirectionAndSlopeBias = glm::vec4(0.0f);
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
      glm::vec4 viewportImageRect{0.0f};
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
      const ImVec2 viewportImageMin = ImGui::GetItemRectMin();
      const ImVec2 viewportImageMax = ImGui::GetItemRectMax();
      viewportImageRect = glm::vec4(viewportImageMin.x,
                                    viewportImageMin.y,
                                    viewportImageMax.x - viewportImageMin.x,
                                    viewportImageMax.y - viewportImageMin.y);
      ImGui::SetCursorPos(ImVec2(0, 0));
      ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
      ImGui::End();
      ImGui::PopStyleVar();

      if(ImGui::Begin("Settings"))
      {
        // Camera coordinates display
        ImGui::Separator();
        ImGui::Text("Camera Position:");
        const glm::vec3& camPos = m_camera.getPosition();
        ImGui::Text("  X: %.2f", camPos.x);
        ImGui::Text("  Y: %.2f", camPos.y);
        ImGui::Text("  Z: %.2f", camPos.z);

        ImGui::Separator();
        ImGui::Text("Directional Light");
        bool lightDirectionChanged = false;
        lightDirectionChanged |= ImGui::SliderFloat("Travel Azimuth", &m_lightAzimuthDegrees, -180.0f, 180.0f, "%.1f deg");
        lightDirectionChanged |= ImGui::SliderFloat("Travel Elevation", &m_lightElevationDegrees, -89.0f, 89.0f, "%.1f deg");
        if(ImGui::Button("Reset Travel Direction"))
        {
          m_lightSettings.direction = glm::normalize(glm::vec3(-0.45f, -0.8f, -0.25f));
          syncLightAnglesFromDirection();
        }
        if(lightDirectionChanged)
        {
          syncLightDirectionFromAngles();
        }
        ImGui::Text("Travel Dir: %.3f, %.3f, %.3f",
                    m_lightSettings.direction.x,
                    m_lightSettings.direction.y,
                    m_lightSettings.direction.z);
        ImGui::ColorEdit3("Light Color", &m_lightSettings.color.x);
        ImGui::ColorEdit3("Ambient", &m_lightSettings.ambient.x);
        ImGui::SliderFloat("Shadow Distance", &m_lightSettings.shadowDistance, 5.0f, 80.0f);
        ImGui::SliderFloat("Shadow Strength", &m_lightSettings.shadowStrength, 0.0f, 1.0f);
        ImGui::SliderFloat("Normal Bias", &m_lightSettings.normalBias, 0.0001f, 0.02f, "%.4f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Depth Bias", &m_lightSettings.depthBias, 0.0001f, 0.02f, "%.4f", ImGuiSliderFlags_Logarithmic);

        ImGui::Separator();
        ImGui::Text("Debug Overlay");
        ImGui::Checkbox("Enable Debug Pass", &m_debugOptions.enabled);
        ImGui::Checkbox("Scene Bounds", &m_debugOptions.showSceneBounds);
        ImGui::Checkbox("Shadow Frustum", &m_debugOptions.showShadowFrustum);
        ImGui::Checkbox("View Frustum", &m_debugOptions.showViewFrustum);
        ImGui::Checkbox("Light Travel Direction", &m_debugOptions.showLightDirection);
        ImGui::Checkbox("Point Lights", &m_debugOptions.showPointLights);
        ImGui::Checkbox("Viewport Axis", &m_debugOptions.showViewportAxis);
        ImGui::Checkbox("Coarse Cull Heatmap", &m_debugOptions.showLightCoarseCullingHeatmap);
        ImGui::Checkbox("GPU Culling Overlay", &m_debugOptions.showGPUCullingOverlay);
        ImGui::SliderFloat("Point Max Radius", &m_debugOptions.pointLightMaxRadius, 0.5f, 12.0f, "%.2f");
        ImGui::SliderFloat("Point Intensity", &m_debugOptions.pointLightIntensityScale, 0.25f, 16.0f, "%.2f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::Checkbox("Cull Distance", &m_debugOptions.showCullDistance);
        ImGui::SliderFloat("Cull Radius", &m_debugOptions.cullDistance, 1.0f, 80.0f);

        // CSM Shadow debug panel
        drawCSMDebugPanel();

        ImGui::Separator();
        ImGui::Text("GPU Culling");
        const shaderio::GPUCullStats& gpuCullStats = m_renderer.getLastGPUCullingStats();
        const uint32_t totalCullEvaluated = gpuCullStats.totalCount > 0 ? gpuCullStats.totalCount : 1u;
        ImGui::Text("Visible: %u", gpuCullStats.visibleCount);
        ImGui::Text("Opaque Visible: %u / %u", gpuCullStats.opaqueVisibleCount, gpuCullStats.opaqueCount);
        ImGui::Text("Transparent Visible: %u / %u", gpuCullStats.transparentVisibleCount, gpuCullStats.transparentCount);
        ImGui::Text("Frustum Culled: %u", gpuCullStats.frustumCulledCount);
        ImGui::Text("Occlusion Culled: %u", gpuCullStats.occlusionCulledCount);
        ImGui::Text("Total: %u", gpuCullStats.totalCount);
        ImGui::Text("Visible Ratio: %.1f%%", 100.0f * static_cast<float>(gpuCullStats.visibleCount) / static_cast<float>(totalCullEvaluated));

        // Swapchain diagnostics
        ImGui::Separator();
        ImGui::Text("Swapchain");
        ImGui::Text("Present Mode: %s", m_vSync ? "FIFO" : "Mailbox");
        ImGui::Text("Swap Images: %u", m_renderer.getSwapchainImageCount());
      }
      ImGui::End();

      drawModelLoaderUI();
      drawSceneGraphUI();

      demo::RenderParams frameParams{};
      frameParams.viewportSize   = m_viewportSize;
      frameParams.deltaTime      = ImGui::GetIO().DeltaTime;
      frameParams.timeSeconds    = static_cast<float>(ImGui::GetTime());
      frameParams.materialHandle = m_selectedMaterial;
      frameParams.clearColor     = m_clearColor;
      frameParams.viewportImageRect = viewportImageRect;
      frameParams.gltfModel      = m_currentModel.has_value() ? &(*m_currentModel) : nullptr;
      frameParams.cameraUniforms = &m_cameraUniforms;
      frameParams.lightSettings  = m_lightSettings;
      frameParams.debugOptions   = m_debugOptions;
      // Copy CSM debug settings to debugOptions
      frameParams.debugOptions.showShadowCascades    = m_showShadowCascades;
      frameParams.debugOptions.cascadeIndex          = m_cascadeIndex;
      frameParams.debugOptions.cascadeOverlayMode    = m_cascadeOverlayMode;
      frameParams.debugOptions.cascadeOverlayAlpha   = m_cascadeOverlayAlpha;
      frameParams.recordUi       = [](demo::rhi::CommandList& cmd) {
        ImGui::Render();
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
  std::optional<demo::GltfModel>                  m_sceneModel;
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
  demo::DirectionalLightSettings m_lightSettings{};
  float m_lightAzimuthDegrees{0.0f};
  float m_lightElevationDegrees{0.0f};
  demo::DebugPassOptions m_debugOptions{};

  // CSM Shadow debug settings (copied to debugOptions in run())
  bool  m_showShadowCascades{true};
  int   m_cascadeIndex{-1};              // -1 = all cascades, 0-3 = specific cascade
  bool  m_cascadeOverlayMode{false};
  float m_cascadeOverlayAlpha{0.25f};

  // UI state
  char m_modelPathBuffer[512] = "resources/GLTF_Sponza/sponza.gltf";

  // Preset models
  struct PresetModel {
    const char* name;
    const char* path;
  };
  static constexpr PresetModel m_presetModels[] = {
    {"Sponza", "resources/GLTF_Sponza/sponza.gltf"},
    {"Bistro", "resources/GLTF_Bistro/bistro.gltf"},
	{"SponzaNew", "resources/Sponza/sponza.gltf"}
  };
  int m_selectedPreset = 0;

  // Async loading state
  std::future<std::optional<demo::GltfModel>> m_loadFuture;
  std::string m_pendingModelPath;
  bool m_isLoading = false;
  float m_loadProgress = 0.0f;
  std::string m_loadStatus;
  int m_selectedSceneNode = -1;

  void loadModelAsync(const std::string& path);
  void unloadModel();
  void drawModelLoaderUI();
  void drawSceneGraphUI();
  void drawSceneNodeTree(int nodeIndex);
  void drawSelectedSceneNodeInspector();
  void applySceneGraphTransforms();
  void updateSceneNodeWorldTransform(int nodeIndex, const glm::mat4& parentTransform);
  void updateAsyncLoading();
  void syncLightAnglesFromDirection();
  void syncLightDirectionFromAngles();
  void drawCSMDebugPanel();
};

inline void MinimalLatestApp::syncLightAnglesFromDirection()
{
  glm::vec3 direction = m_lightSettings.direction;
  if(glm::length(direction) < 0.001f)
  {
    direction = glm::normalize(glm::vec3(-0.45f, -0.8f, -0.25f));
  }
  else
  {
    direction = glm::normalize(direction);
  }

  m_lightSettings.direction = direction;
  m_lightElevationDegrees = glm::degrees(std::asin(glm::clamp(direction.y, -1.0f, 1.0f)));
  m_lightAzimuthDegrees = glm::degrees(std::atan2(direction.x, direction.z));
}

inline void MinimalLatestApp::syncLightDirectionFromAngles()
{
  const float azimuthRadians = glm::radians(m_lightAzimuthDegrees);
  const float elevationRadians = glm::radians(m_lightElevationDegrees);
  const float planarLength = std::cos(elevationRadians);

  m_lightSettings.direction = glm::normalize(glm::vec3(
      planarLength * std::sin(azimuthRadians),
      std::sin(elevationRadians),
      planarLength * std::cos(azimuthRadians)));
}

inline void MinimalLatestApp::loadModelAsync(const std::string& path)
{
  // Don't start a new load if already loading
  if(m_isLoading)
  {
    return;
  }

  m_isLoading = true;
  m_loadProgress = 0.0f;
  m_loadStatus = "Starting load...";
  m_pendingModelPath = path;

  // Start async loading (only file parsing, no member access)
  m_loadFuture = std::async(std::launch::async, [path]() -> std::optional<demo::GltfModel> {
    demo::GltfLoader loader;
    demo::GltfModel model;
    if(!loader.load(path, model))
    {
      return std::nullopt;
    }
    return model;
  });
}

inline void MinimalLatestApp::updateAsyncLoading()
{
  if(!m_isLoading || !m_loadFuture.valid())
  {
    return;
  }

  // Simulate progress while waiting
  if(m_loadProgress < 0.4f)
  {
    m_loadProgress += 0.005f;
    m_loadStatus = "Loading glTF file...";
  }

  // Check if loading is complete (non-blocking)
  auto status = m_loadFuture.wait_for(std::chrono::milliseconds(0));
  if(status == std::future_status::ready)
  {
    m_loadProgress = 0.5f;
    m_loadStatus = "Preparing GPU upload...";

    auto result = m_loadFuture.get();
    if(result.has_value())
    {
      m_loadStatus = "Uploading to GPU...";
      m_loadProgress = 0.7f;

      m_renderer.waitForIdle();
      unloadModel();

      m_loadProgress = 0.9f;

      m_sceneModel = std::move(*result);
      m_selectedSceneNode = m_sceneModel->rootNodes.empty() ? -1 : m_sceneModel->rootNodes.front();

      // Upload model to GPU
      m_renderer.executeUploadCommand([this](VkCommandBuffer cmd) {
        m_currentModel = m_renderer.uploadGltfModel(*m_sceneModel, cmd);
      });

      m_modelPath = m_pendingModelPath;
      m_modelLoaded = true;
      m_loadProgress = 1.0f;
      m_loadStatus = "Done!";

      LOGI("Loaded glTF model: %s (%zu meshes, %zu materials, %zu textures)",
           m_pendingModelPath.c_str(), m_sceneModel->meshes.size(), m_sceneModel->materials.size(), m_sceneModel->images.size());
    }
    else
    {
      m_loadStatus = "Failed to load model";
      m_loadProgress = 0.0f;
      LOGE("Failed to load model: %s", m_pendingModelPath.c_str());
    }

    m_isLoading = false;
  }
}

inline void MinimalLatestApp::unloadModel()
{
  if(m_modelLoaded && m_currentModel.has_value())
  {
    m_renderer.waitForIdle();
    m_renderer.destroyGltfResources(*m_currentModel);
    m_currentModel.reset();
    m_sceneModel.reset();
    m_selectedSceneNode = -1;
    m_modelLoaded = false;
  }
}

inline void MinimalLatestApp::drawModelLoaderUI()
{
  if(ImGui::Begin("Model Loader"))
  {
    // Preset model dropdown
    ImGui::Text("Select Model:");
    const char* currentName = m_presetModels[m_selectedPreset].name;
    if(ImGui::BeginCombo("##PresetCombo", currentName))
    {
      for(int i = 0; i < static_cast<int>(sizeof(m_presetModels) / sizeof(m_presetModels[0])); ++i)
      {
        const bool isSelected = (i == m_selectedPreset);
        if(ImGui::Selectable(m_presetModels[i].name, isSelected))
        {
          m_selectedPreset = i;
          std::strncpy(m_modelPathBuffer, m_presetModels[i].path, sizeof(m_modelPathBuffer) - 1);
          m_modelPathBuffer[sizeof(m_modelPathBuffer) - 1] = '\0';
        }
        if(isSelected)
        {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }

    ImGui::Separator();

    // Custom path input (optional)
    if(ImGui::CollapsingHeader("Custom Path"))
    {
      ImGui::InputText("Path", m_modelPathBuffer, sizeof(m_modelPathBuffer));
    }

    // Load button
    if(ImGui::Button(m_isLoading ? "Loading..." : "Load Model", ImVec2(120, 0)))
    {
      if(!m_isLoading)
      {
        loadModelAsync(std::string(m_modelPathBuffer));
      }
    }

    ImGui::SameLine();

    // Unload button
    if(ImGui::Button("Unload", ImVec2(80, 0)))
    {
      unloadModel();
    }

    // Progress bar during loading
    if(m_isLoading)
    {
      ImGui::Separator();
      ImGui::Text("%s", m_loadStatus.c_str());
      ImGui::ProgressBar(m_loadProgress, ImVec2(-1, 0));
    }

    // Clear scene
    ImGui::Separator();
    if(ImGui::Button("Clear Scene"))
    {
      unloadModel();
      m_modelLoaded = false;
    }

    // Current model info
    if(m_modelLoaded)
    {
      ImGui::Separator();
      ImGui::Text("Current: %s", m_modelPath.c_str());
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

inline void MinimalLatestApp::drawSceneGraphUI()
{
  if(ImGui::Begin("Scene Graph"))
  {
    if(!m_sceneModel.has_value() || !m_currentModel.has_value())
    {
      ImGui::TextDisabled("No scene loaded.");
    }
    else
    {
      ImGui::Text("Model: %s", m_sceneModel->name.c_str());
      ImGui::Separator();

      const float panelWidth = ImGui::GetContentRegionAvail().x;
      const float treeWidth = panelWidth * 0.5f;

      ImGui::BeginChild("##SceneTree", ImVec2(treeWidth, 0.0f), true);
      for(const int rootNodeIndex : m_sceneModel->rootNodes)
      {
        drawSceneNodeTree(rootNodeIndex);
      }
      ImGui::EndChild();

      ImGui::SameLine();

      ImGui::BeginChild("##SceneInspector", ImVec2(0.0f, 0.0f), true);
      drawSelectedSceneNodeInspector();
      ImGui::EndChild();
    }
  }
  ImGui::End();
}

inline void MinimalLatestApp::drawSceneNodeTree(int nodeIndex)
{
  if(!m_sceneModel.has_value() || nodeIndex < 0 || nodeIndex >= static_cast<int>(m_sceneModel->nodes.size()))
  {
    return;
  }

  const demo::GltfNodeData& node = m_sceneModel->nodes[nodeIndex];
  const bool hasChildren = !node.children.empty();
  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick
                           | ImGuiTreeNodeFlags_SpanAvailWidth;
  if(!hasChildren)
  {
    flags |= ImGuiTreeNodeFlags_Leaf;
  }
  if(m_selectedSceneNode == nodeIndex)
  {
    flags |= ImGuiTreeNodeFlags_Selected;
  }

  std::string label = node.name;
  if(node.meshCount > 0)
  {
    label += " (" + std::to_string(node.meshCount) + ")";
  }

  const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<intptr_t>(nodeIndex)), flags, "%s", label.c_str());
  if(ImGui::IsItemClicked())
  {
    m_selectedSceneNode = nodeIndex;
  }

  if(open)
  {
    for(const int childIndex : node.children)
    {
      drawSceneNodeTree(childIndex);
    }
    ImGui::TreePop();
  }
}

inline void MinimalLatestApp::drawSelectedSceneNodeInspector()
{
  if(!m_sceneModel.has_value() || m_selectedSceneNode < 0 || m_selectedSceneNode >= static_cast<int>(m_sceneModel->nodes.size()))
  {
    ImGui::TextDisabled("Select a node to edit its transform.");
    return;
  }

  demo::GltfNodeData& node = m_sceneModel->nodes[m_selectedSceneNode];
  ImGui::Text("Node");
  ImGui::Separator();
  ImGui::TextWrapped("%s", node.name.c_str());
  ImGui::Text("Children: %d", static_cast<int>(node.children.size()));
  ImGui::Text("Meshes: %u", node.meshCount);
  ImGui::Text("Parent: %s",
              node.parent >= 0 && node.parent < static_cast<int>(m_sceneModel->nodes.size())
                  ? m_sceneModel->nodes[node.parent].name.c_str()
                  : "<root>");

  ImGui::Separator();
  ImGui::Text("Local Transform");

  bool transformChanged = false;
  transformChanged |= ImGui::DragFloat3("Translation", &node.translation.x, 0.05f);
  transformChanged |= ImGui::DragFloat3("Rotation", &node.rotationEulerDegrees.x, 0.5f);
  transformChanged |= ImGui::DragFloat3("Scale", &node.scale.x, 0.01f, 0.001f, 1000.0f, "%.3f");

  if(transformChanged)
  {
    node.scale = glm::max(node.scale, glm::vec3(0.001f));
    node.rotation = glm::normalize(glm::quat(glm::radians(node.rotationEulerDegrees)));
    applySceneGraphTransforms();
  }

  ImGui::Separator();
  const glm::vec3 worldPosition = glm::vec3(node.worldTransform[3]);
  ImGui::Text("World Position");
  ImGui::Text("  X: %.3f", worldPosition.x);
  ImGui::Text("  Y: %.3f", worldPosition.y);
  ImGui::Text("  Z: %.3f", worldPosition.z);
}

inline void MinimalLatestApp::applySceneGraphTransforms()
{
  if(!m_sceneModel.has_value() || !m_currentModel.has_value())
  {
    return;
  }

  for(const int rootNodeIndex : m_sceneModel->rootNodes)
  {
    updateSceneNodeWorldTransform(rootNodeIndex, glm::mat4(1.0f));
  }
}

inline void MinimalLatestApp::updateSceneNodeWorldTransform(int nodeIndex, const glm::mat4& parentTransform)
{
  if(!m_sceneModel.has_value() || !m_currentModel.has_value())
  {
    return;
  }
  if(nodeIndex < 0 || nodeIndex >= static_cast<int>(m_sceneModel->nodes.size()))
  {
    return;
  }

  demo::GltfNodeData& node = m_sceneModel->nodes[nodeIndex];
  node.localTransform = glm::translate(glm::mat4(1.0f), node.translation)
                      * glm::mat4_cast(node.rotation)
                      * glm::scale(glm::mat4(1.0f), node.scale);
  node.worldTransform = parentTransform * node.localTransform;

  const uint32_t meshEnd = node.firstMeshIndex + node.meshCount;
  for(uint32_t meshIndex = node.firstMeshIndex; meshIndex < meshEnd; ++meshIndex)
  {
    if(meshIndex < m_sceneModel->meshes.size())
    {
      m_sceneModel->meshes[meshIndex].transform = node.worldTransform;
    }
    if(meshIndex < m_currentModel->meshes.size())
    {
      m_renderer.updateMeshTransform(m_currentModel->meshes[meshIndex], node.worldTransform);
    }
  }

  for(const int childIndex : node.children)
  {
    updateSceneNodeWorldTransform(childIndex, node.worldTransform);
  }
}

inline void MinimalLatestApp::drawCSMDebugPanel()
{
  if(ImGui::CollapsingHeader("CSM Shadows"))
  {
    ImGui::Indent();

    ImGui::Checkbox("Show Cascade Frustums", &m_showShadowCascades);

    if(m_showShadowCascades)
    {
      static const char* cascadeNames[] = {
        "All Cascades", "Cascade 0 (Near)", "Cascade 1", "Cascade 2", "Cascade 3 (Far)"
      };
      ImGui::Combo("Cascade Filter", &m_cascadeIndex, cascadeNames, 5);

      ImGui::Checkbox("Cascade Overlay (Screen)", &m_cascadeOverlayMode);
      if(m_cascadeOverlayMode)
      {
        ImGui::SliderFloat("Overlay Alpha", &m_cascadeOverlayAlpha, 0.1f, 0.5f);
      }
    }

    // Display split distances from shadow uniforms
    shaderio::ShadowUniforms* shadowData = m_renderer.getShadowUniformsData();
    if(shadowData != nullptr)
    {
      ImGui::Separator();
      ImGui::Text("Cascade Split Distances:");
      const glm::vec4& splits = shadowData->cascadeSplitDistances;
      ImGui::Text("  C0: %.2f", splits.x);
      ImGui::Text("  C1: %.2f", splits.y);
      ImGui::Text("  C2: %.2f", splits.z);
      ImGui::Text("  C3: %.2f", splits.w);
      ImGui::Text("  Resolution: %d", m_renderer.getCSMShadowResources().getCascadeResolution());
    }

    ImGui::Unindent();
  }
}
