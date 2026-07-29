#pragma once

#include "../common/Common.h"
#include "../common/FrameDeferredValue.h"
#include "../common/ProfilerMarkers.h"

#ifdef _WIN32
#include <windows.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include "../render/RendererFacade.h"
#include "../rhi/RHISurface.h"
#include "../loader/GltfLoader.h"
#include "../loader/SceneCacheSerializer.h"
#include "../render/AsyncLoadingCoordinator.h"
#include "../render/Camera.h"
#include "../render/SceneViewResolver.h"
#include "../scene/SceneAssetBuilder.h"
#include "../scene/SceneAssetSerializer.h"
#include "../scene/ParallelSceneLoader.h"
#include "../scene/SceneUploadPlanner.h"
#include "../third_party/LegitProfiler/ImGuiProfilerRenderer.h"

#include <memory>
#include <optional>
#include <future>
#include <atomic>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

class MinimalLatestApp
{
public:
  enum class AutomationMode
  {
    none,
    csmTranslateStop,
    csmRotateStop,
  };

  struct AutomationOptions
  {
    AutomationMode mode{AutomationMode::none};
    float fixedDeltaSeconds{1.0f / 60.0f};
    uint32_t warmupFrames{60u};
    uint32_t motionFrames{60u};
    uint32_t holdFrames{60u};
    bool noUi{false};
    bool noPost{false};
    bool noDdgi{false};
    bool taa{false};
    bool autoExit{false};
    bool captureControlFrame{false};
    std::filesystem::path captureSyncDirectory{};
    uint32_t captureSyncTimeoutMilliseconds{30000u};
  };

  MinimalLatestApp(demo::rhi::Extent2D size = {1920, 1080}, AutomationOptions automationOptions = {})
      : m_windowSize(size)
      , m_automationOptions(automationOptions)
  {
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#ifdef USE_SLANG
    const char* windowTitle = "Minimal Demo (Slang)";
#else
    const char* windowTitle = "Minimal Demo (GLSL)";
#endif
    m_window  = glfwCreateWindow(m_windowSize.width, m_windowSize.height, windowTitle, nullptr, nullptr);
    m_surface = m_renderer.createSurface();
    m_renderer.init(m_window, *m_surface, m_vSync);
    m_selectedMaterial = m_renderer.getMaterialHandle(0);
    m_gltfLoader       = std::make_unique<demo::GltfLoader>();

    // Initialize camera
    m_camera.setPerspective(45.0f, static_cast<float>(m_windowSize.width) / static_cast<float>(m_windowSize.height), 0.1f, 100.0f);
    m_camera.setPosition(glm::vec3(8.0f, 1.5f, 0.0f));
    m_camera.setYawPitch(180.0, 0.0);
    m_camera.update();
    resetSceneCameraNavigation();
    syncLightAnglesFromDirection();
    m_debugOptions.enableGPUFrustumCulling = false;
    m_debugOptions.enableGPUOcclusionCulling = false;
    m_debugOptions.enableGPUMeshletOcclusionCulling = false;
    m_debugOptions.enableIBL = false;


    ImGui::GetIO().ConfigFlags = ImGuiConfigFlags_DockingEnable;
    resetUIAppearanceStyle();

    // Load Sponza by default for FlaxGI smoke testing
    std::string path = "resources/GLTF_Sponza/sponza.gltf";
    std::strncpy(m_modelPathBuffer, path.c_str(), sizeof(m_modelPathBuffer) - 1);
    m_modelPathBuffer[sizeof(m_modelPathBuffer) - 1] = '\0';
    setMeshSDFPathFromModelPath(path);
    m_autoLoadSDFOnSceneReady = !m_automationOptions.noDdgi;
    if(m_automationOptions.noDdgi)
    {
      m_renderer.setDDGIEnabled(false);
    }
    loadModelAsync(path);

    if(isAutomationEnabled())
    {
      const std::string captureSyncDirectory = m_automationOptions.captureSyncDirectory.string();
      LOGI("[CSM_AUTOMATION] marker=config mode=%s fixed_dt=%.9f warmup=%u motion=%u hold=%u no_ui=%d no_post=%d no_ddgi=%d taa=%d auto_exit=%d capture_sync=%d capture_control=%d capture_sync_timeout_ms=%u capture_sync_dir=%s",
           automationModeName(),
           m_automationOptions.fixedDeltaSeconds,
           m_automationOptions.warmupFrames,
           m_automationOptions.motionFrames,
           m_automationOptions.holdFrames,
           m_automationOptions.noUi ? 1 : 0,
           m_automationOptions.noPost ? 1 : 0,
           m_automationOptions.noDdgi ? 1 : 0,
           m_automationOptions.taa ? 1 : 0,
           m_automationOptions.autoExit ? 1 : 0,
           isAutomationCaptureSyncEnabled() ? 1 : 0,
           m_automationOptions.captureControlFrame ? 1 : 0,
           m_automationOptions.captureSyncTimeoutMilliseconds,
           captureSyncDirectory.c_str());
    }
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
      demo::profiling::ScopedCpuRange frameCpuRange("Frame");
      const char* framePhase = "FrameStart";
      try
      {
      {
      demo::profiling::ScopedCpuRange updateCpuRange("Update");
      // Let the renderer/present path control pacing. Adding an app-side sleep
      // here only reduces CPU/GPU overlap and steady-state utilization.
      framePhase = "PollEvents";
      {
        demo::profiling::ScopedCpuRange pollEventsRange("AppPreRecord.PollEvents");
        glfwPollEvents();
      }
      // UI draw data may still reference the previous Flax debug descriptor until
      // the frame is submitted. Apply resource-rebuilding config changes only at
      // the next frame boundary, before any new ImGui draw commands are recorded.
      if(std::optional<demo::DDGIConfig> pendingConfig = m_pendingDDGIConfig.consume())
      {
        framePhase = "ApplyPendingDDGIConfig";
        m_renderer.setDDGIConfig(*pendingConfig);
      }

      // Check async loading progress
      framePhase = "UpdateAsyncLoading";
      {
        demo::profiling::ScopedCpuRange asyncLoadingRange("AppPreRecord.UpdateAsyncLoading");
        updateAsyncLoading();
      }

      // Auto-load SDF after scene is fully uploaded (smoke test convenience)
      if (m_autoLoadSDFOnSceneReady && !m_isLoading && !m_renderer.isSceneRenderingSuspended() && !m_meshSDFLoaded)
      {
        loadMeshSDFForDDGI();
        m_autoLoadSDFOnSceneReady = false;
      }

      // Camera input handling
      {
          demo::profiling::ScopedCpuRange inputCameraRange("AppPreRecord.InputCamera");
          if(isAutomationEnabled())
          {
              applyAutomationCameraPose();
          }
          else
          {
          // Keyboard movement
          glm::vec3 moveDir{0.0f};
          if(glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS) moveDir.z += 1.0f;
          if(glfwGetKey(m_window, GLFW_KEY_S) == GLFW_PRESS) moveDir.z -= 1.0f;
          if(glfwGetKey(m_window, GLFW_KEY_A) == GLFW_PRESS) moveDir.x -= 1.0f;
          if(glfwGetKey(m_window, GLFW_KEY_D) == GLFW_PRESS) moveDir.x += 1.0f;
          if(glfwGetKey(m_window, GLFW_KEY_Q) == GLFW_PRESS) moveDir.y += 1.0f;
          if(glfwGetKey(m_window, GLFW_KEY_E) == GLFW_PRESS) moveDir.y -= 1.0f;

          // F1: Toggle fullscreen
          static bool f1Pressed = false;
          if(glfwGetKey(m_window, GLFW_KEY_F1) == GLFW_PRESS)
          {
            if(!f1Pressed)
            {
              f1Pressed = true;
              toggleFullscreen();
            }
          }
          else
          {
            f1Pressed = false;
          }

          demo::Camera& navigationCamera = hasActiveSceneCamera() ? m_sceneCameraNavigation : m_camera;
          if(glm::length(moveDir) > 0.0f)
          {
              moveDir = glm::normalize(moveDir) * m_moveSpeed * ImGui::GetIO().DeltaTime;
              navigationCamera.move(moveDir);
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
                  navigationCamera.rotate(deltaX, -deltaY);  // Inverted Y for natural feel
              }
          }
          else if(m_cursorCaptured)
          {
              glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
              m_cursorCaptured = false;
          }
          }

          updateActiveCamera();
      }
      }

      if(glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) == GLFW_TRUE)
      {
        ImGui_ImplGlfw_Sleep(10);
        continue;
      }

      demo::RenderParams frameParams{};
      {
      demo::profiling::ScopedCpuRange appPreRecordRange("AppPreRecord");

      framePhase = "ImGuiNewFrame";
      {
        demo::profiling::ScopedCpuRange imguiNewFrameRange("AppPreRecord.ImGuiNewFrame");
        m_renderer.beginUiFrame();
        if(isAutomationEnabled())
        {
          ImGui::GetIO().DeltaTime = m_automationOptions.fixedDeltaSeconds;
        }
        framePhase = "ImGuiFrameBegin";
      }
      framePhase = "RuntimeProfiler";
      {
        demo::profiling::ScopedCpuRange runtimeProfilerRange("AppPreRecord.RuntimeProfiler");
        if(!m_runtimeProfilerDisabled)
        {
          updateRuntimeProfiler();
        }
      }

      ImGuiID mainDockspaceID = 0;
      {
        demo::profiling::ScopedCpuRange dockspaceRange("AppPreRecord.ImGuiDockspace");
        const ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingInCentralNode;
        ImGuiID dockID = mainDockspaceID = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), dockFlags);
        ImGuiDockNode* rootDockNode = ImGui::DockBuilderGetNode(dockID);
        if(rootDockNode != nullptr && !rootDockNode->IsSplitNode())
        {
          ImGuiID leftID = ImGui::DockBuilderSplitNode(dockID, ImGuiDir_Left, 0.2f, nullptr, &dockID);
          ImGui::DockBuilderDockWindow("Settings", leftID);
          ImGui::DockBuilderDockWindow("Model Loader", leftID);
          ImGui::DockBuilderDockWindow("Scene Graph", leftID);
          ImGuiID rightID = ImGui::DockBuilderSplitNode(dockID, ImGuiDir_Right, 0.28f, nullptr, &dockID);
          ImGui::DockBuilderDockWindow("FlaxDebugUI", rightID);
          if(ImGuiDockNode* centralNode = ImGui::DockBuilderGetNode(dockID))
          {
            centralNode->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
          }
        }
      }

      {
        demo::profiling::ScopedCpuRange mainMenuRange("AppPreRecord.ImGuiMainMenu");
        if(ImGui::BeginMainMenuBar())
        {
          if(ImGui::BeginMenu("File"))
          {
            if(ImGui::MenuItem("vSync", "", &m_vSync))
            {
              m_renderer.setVSync(m_vSync);
            }
            ImGui::Separator();
            if(ImGui::MenuItem("Exit"))
              glfwSetWindowShouldClose(m_window, true);
            ImGui::EndMenu();
          }
          ImGui::Separator();
          ImGui::Text("FPS %.1f", ImGui::GetIO().Framerate);
          ImGui::EndMainMenuBar();
        }
      }

      glm::vec4 viewportImageRect{0.0f};
      {
        demo::profiling::ScopedCpuRange viewportPanelRange("AppPreRecord.FullWindowViewport");
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(m_window, &framebufferWidth, &framebufferHeight);
        const demo::rhi::Extent2D requestedViewportSize{
          static_cast<uint32_t>(std::max(framebufferWidth, 0)),
          static_cast<uint32_t>(std::max(framebufferHeight, 0))
        };
        if(requestedViewportSize.width > 0 && requestedViewportSize.height > 0
           && (requestedViewportSize.width != m_viewportSize.width || requestedViewportSize.height != m_viewportSize.height))
        {
          m_viewportSize = requestedViewportSize;
          m_renderer.resize(m_viewportSize);
          m_camera.setPerspective(45.0f, static_cast<float>(m_viewportSize.width) / static_cast<float>(m_viewportSize.height), 0.1f, 100.0f);
          // Camera uniforms were populated before viewport handling. Refresh them
          // without reapplying or advancing the automation pose/history so every
          // render pass consumes the resized projection in this same frame.
          refreshActiveCameraUniforms();
        }

        const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        ImVec2 viewportImageMin = mainViewport->WorkPos;
        ImVec2 viewportImageMax(mainViewport->WorkPos.x + mainViewport->WorkSize.x,
                                mainViewport->WorkPos.y + mainViewport->WorkSize.y);
        if(const ImGuiDockNode* centralNode = ImGui::DockBuilderGetCentralNode(mainDockspaceID);
           centralNode != nullptr && centralNode->Size.x > 0.0f && centralNode->Size.y > 0.0f)
        {
          viewportImageMin = centralNode->Pos;
          viewportImageMax = ImVec2(centralNode->Pos.x + centralNode->Size.x,
                                    centralNode->Pos.y + centralNode->Size.y);
        }
        viewportImageRect = glm::vec4(viewportImageMin.x,
                                      viewportImageMin.y,
                                      viewportImageMax.x - viewportImageMin.x,
                                      viewportImageMax.y - viewportImageMin.y);
        drawFlaxDebugViewportOverlay(viewportImageMin, viewportImageMax);
      }

      if(ImGui::Begin("Settings"))
      {
        demo::profiling::ScopedCpuRange settingsPanelRange("AppPreRecord.SettingsPanel");
        // Camera coordinates display
        ImGui::Separator();
        ImGui::Text("Camera Position:");
        const glm::vec3& camPos = m_cameraUniforms.cameraPosition;
        ImGui::Text("  X: %.2f", camPos.x);
        ImGui::Text("  Y: %.2f", camPos.y);
        ImGui::Text("  Z: %.2f", camPos.z);

        ImGui::Separator();
        ImGui::Text("Directional Light");
        ImGui::Checkbox("Test Directional Light", &m_enableTestDirectionalLight);
        if(m_enableTestDirectionalLight)
        {
          bool lightDirectionChanged = ImGui::DragFloat3("Direction", &m_lightSettings.direction.x, 0.01f, -1.0f, 1.0f, "%.3f");
          if(ImGui::Button("Reset Travel Direction"))
          {
            m_lightSettings.direction = glm::normalize(glm::vec3(0.27f, -0.9f, -0.3f));
          }
          if(lightDirectionChanged)
          {
            if(glm::length(m_lightSettings.direction) < 0.001f)
            {
              m_lightSettings.direction = glm::normalize(glm::vec3(0.6, -0.5, -0.6));
            }
            else
            {
              m_lightSettings.direction = glm::normalize(m_lightSettings.direction);
            }
          }
          ImGui::Text("Travel Dir: %.3f, %.3f, %.3f",
                      m_lightSettings.direction.x,
                      m_lightSettings.direction.y,
                      m_lightSettings.direction.z);
          ImGui::ColorEdit3("Test Color", &m_testDirectionalLightColor.x);
          ImGui::SliderFloat("Test Intensity", &m_testDirectionalLightIntensity, 0.0f, 20.0f, "%.2f");
        }
        ImGui::Checkbox("IBL", &m_debugOptions.enableIBL);
        if(m_debugOptions.enableIBL)
        {
          ImGui::SliderFloat("IBL Intensity", &m_debugOptions.iblIntensity, 0.0f, 2.0f, "%.2f");
          const char* iblDebugModes[] = {"Off", "Diffuse", "Specular", "Fallback", "Environment"};
          ImGui::Combo("IBL Debug", &m_debugOptions.iblDebugMode, iblDebugModes, IM_ARRAYSIZE(iblDebugModes));
        }
        ImGui::SliderFloat("Shadow Distance", &m_lightSettings.shadowDistance, 10.0f, 250.0f, "%.1f m", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Shadow Strength", &m_lightSettings.shadowStrength, 0.0f, 1.0f);
        ImGui::SliderFloat("Normal Bias", &m_lightSettings.normalBias, 0.0001f, 0.02f, "%.4f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Depth Bias", &m_lightSettings.depthBias, 0.0001f, 0.02f, "%.4f", ImGuiSliderFlags_Logarithmic);
        drawSceneLightsUI();

        ImGui::Separator();
        ImGui::Text("Debug Overlay");
        ImGui::Checkbox("Enable Debug Pass", &m_debugOptions.enabled);
        if(ImGui::TreeNode("Scene Overlays"))
        {
          ImGui::Checkbox("Scene Bounds", &m_debugOptions.showSceneBounds);
          ImGui::Checkbox("View Frustum", &m_debugOptions.showViewFrustum);
          ImGui::Checkbox("Viewport Axis", &m_debugOptions.showViewportAxis);
          ImGui::TreePop();
        }
        if(ImGui::TreeNode("Light Overlays"))
        {
          ImGui::Checkbox("Shadow Frustum", &m_debugOptions.showShadowFrustum);
          ImGui::Checkbox("Light Travel Direction", &m_debugOptions.showLightDirection);
          ImGui::Checkbox("glTF Local Lights", &m_debugOptions.enablePointLights);
          ImGui::Checkbox("Local Light Overlay", &m_debugOptions.showPointLights);
          ImGui::Checkbox("Coarse Cull Heatmap", &m_debugOptions.showLightCoarseCullingHeatmap);
          ImGui::Checkbox("Clustered Lighting", &m_debugOptions.enableClusteredLighting);
          ImGui::Checkbox("Cluster Heatmap", &m_debugOptions.showClusteredLightingHeatmap);
          ImGui::Checkbox("Cluster Overflow", &m_debugOptions.showClusteredLightingOverflow);
          ImGui::Checkbox("Ambient Occlusion", &m_debugOptions.enableAO);
          if(m_debugOptions.enableAO)
          {
            ImGui::SliderFloat("AO Radius", &m_debugOptions.aoRadius, 2.0f, 32.0f, "%.1f");
            ImGui::SliderFloat("AO Intensity", &m_debugOptions.aoIntensity, 0.0f, 2.0f, "%.2f");
          }
          ImGui::Checkbox("SSR", &m_debugOptions.enableSSR);
          if(m_debugOptions.enableSSR)
          {
            ImGui::SliderInt("SSR Max Steps", &m_debugOptions.ssrMaxSteps, 8, 64);
            ImGui::SliderFloat("SSR Thickness", &m_debugOptions.ssrThickness, 0.005f, 0.12f, "%.3f");
          }
          ImGui::Checkbox("Shadow Atlas", &m_debugOptions.enableShadowAtlas);
          ImGui::TreePop();
        }
        if(ImGui::TreeNode("Culling Overlays"))
        {
          ImGui::Checkbox("GPU Culling Overlay", &m_debugOptions.showGPUCullingOverlay);
          ImGui::Checkbox("Cull Distance", &m_debugOptions.showCullDistance);
          if(m_debugOptions.showCullDistance)
          {
            ImGui::SliderFloat("Cull Radius", &m_debugOptions.cullDistance, 1.0f, 80.0f);
          }
          ImGui::TreePop();
        }
        ImGui::Separator();
        ImGui::Text("Post Process");
        ImGui::Checkbox("Post Effects", &m_debugOptions.enablePostProcessing);
        ImGui::SliderFloat("AgX Highlight Compression",
                           &m_debugOptions.agxHighlightCompression, 0.0f, 0.4f, "%.2f");
        ImGui::SliderFloat("AgX Shadow Compression",
                           &m_debugOptions.agxShadowCompression, 0.0f, 0.8f, "%.2f");
        if(m_debugOptions.enablePostProcessing && ImGui::TreeNode("Exposure"))
        {
          ImGui::Checkbox("Adaptive", &m_debugOptions.enableAdaptiveExposure);
          ImGui::SliderFloat("Fixed", &m_debugOptions.postExposure, 0.1f, 4.0f, "%.2f");
          if(m_debugOptions.enableAdaptiveExposure)
          {
            ImGui::SliderFloat("Target Luma", &m_debugOptions.exposureTargetLuminance, 0.03f, 0.8f, "%.2f");
            ImGui::SliderFloat("Auto Min", &m_debugOptions.minAutoExposure, 0.05f, 2.0f, "%.2f");
            ImGui::SliderFloat("Auto Max", &m_debugOptions.maxAutoExposure, 1.0f, 8.0f, "%.2f");
          }
          ImGui::TreePop();
        }
        if(m_debugOptions.enablePostProcessing && ImGui::TreeNode("Temporal"))
        {
          ImGui::Checkbox("TAA", &m_debugOptions.enableTAA);
          ImGui::Checkbox("Show Velocity", &m_debugOptions.showVelocity);
          if(m_debugOptions.enableTAA)
          {
            ImGui::SliderFloat("Jitter Scale", &m_debugOptions.taaJitterScale, 0.0f, 2.0f, "%.2f");
            ImGui::SliderFloat("Blend Weight", &m_debugOptions.taaBlendWeight, 0.0f, 0.98f, "%.2f");
            ImGui::Checkbox("Filter Input (Lanczos)", &m_debugOptions.taaFilterInput);
            ImGui::Checkbox("Variance Clip Box", &m_debugOptions.taaVarianceClip);
            ImGui::Checkbox("Prevent Flicker (Lottes)", &m_debugOptions.taaPreventFlicker);
            ImGui::Checkbox("Catmull-Rom History", &m_debugOptions.taaCatmullRom);
          }
          ImGui::SliderFloat("Render Scale", &m_debugOptions.renderScale, 0.5f, 1.0f, "%.2f");
          const char* upscaleModes[] = {"Off", "TAA", "Spatial"};
          ImGui::Combo("Upscaling Mode", &m_debugOptions.upscalingMode, upscaleModes, IM_ARRAYSIZE(upscaleModes));
          ImGui::TreePop();
        }
        if(m_debugOptions.enablePostProcessing && ImGui::TreeNode("Bloom"))
        {
          ImGui::Checkbox("Enable", &m_debugOptions.enableBloom);
          if(m_debugOptions.enableBloom)
          {
            ImGui::SliderFloat("Intensity", &m_debugOptions.bloomIntensity, 0.0f, 2.0f, "%.2f");
            ImGui::SliderFloat("Threshold", &m_debugOptions.bloomThreshold, 0.0f, 8.0f, "%.2f");
          }
          ImGui::TreePop();
        }
        if(m_debugOptions.enablePostProcessing && ImGui::TreeNode("Color Grading"))
        {
          ImGui::Checkbox("Enable", &m_debugOptions.enableColorGrading);
          if(m_debugOptions.enableColorGrading)
          {
            ImGui::SliderFloat("Saturation", &m_debugOptions.colorSaturation, 0.0f, 2.0f, "%.2f");
            ImGui::SliderFloat("Contrast", &m_debugOptions.colorContrast, 0.5f, 2.0f, "%.2f");
            ImGui::SliderFloat("Gamma", &m_debugOptions.colorGamma, 0.5f, 2.5f, "%.2f");
            ImGui::SliderFloat("LUT Strength", &m_debugOptions.colorLutStrength, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Vignette", &m_debugOptions.vignetteIntensity, 0.0f, 1.0f, "%.2f");
          }
          ImGui::TreePop();
        }
        if(m_debugOptions.enablePostProcessing && ImGui::TreeNode("Lens"))
        {
          ImGui::Checkbox("Enable", &m_debugOptions.enableLensEffects);
          if(m_debugOptions.enableLensEffects)
          {
            ImGui::SliderFloat("Dirt", &m_debugOptions.lensDirtIntensity, 0.0f, 1.0f, "%.2f");
          }
          ImGui::TreePop();
        }
        if(m_debugOptions.maxAutoExposure < m_debugOptions.minAutoExposure)
        {
          m_debugOptions.maxAutoExposure = m_debugOptions.minAutoExposure;
        }

        // CSM Shadow debug panel
        {
          demo::profiling::ScopedCpuRange csmDebugPanelRange("AppPreRecord.CSMDebugPanel");
          drawCSMDebugPanel();
        }

        ImGui::Separator();
        ImGui::Text("GPU Culling");
        const shaderio::GPUCullStats& gpuCullStats = m_renderer.getLastGPUCullingStats();
        const uint32_t totalCullEvaluated = gpuCullStats.totalCount > 0 ? gpuCullStats.totalCount : 1u;
        if(ImGui::TreeNode("Controls"))
        {
          ImGui::Checkbox("Frustum Culling", &m_debugOptions.enableGPUFrustumCulling);
          ImGui::Checkbox("Hi-Z Occlusion Culling", &m_debugOptions.enableGPUOcclusionCulling);
          ImGui::Checkbox("Meshlet Hi-Z Occlusion", &m_debugOptions.enableGPUMeshletOcclusionCulling);
          ImGui::Checkbox("Meshlet Cone Culling", &m_debugOptions.enableGPUMeshletConeCulling);
          ImGui::TreePop();
        }
        if(ImGui::TreeNode("Stats"))
        {
          ImGui::Text("Visible: %u", gpuCullStats.visibleCount);
          ImGui::Text("Opaque Visible: %u / %u", gpuCullStats.opaqueVisibleCount, gpuCullStats.opaqueCount);
          ImGui::Text("Transparent Visible: %u / %u", gpuCullStats.transparentVisibleCount, gpuCullStats.transparentCount);
          ImGui::Text("Frustum Culled: %u", gpuCullStats.frustumCulledCount);
          ImGui::Text("Occlusion Culled: %u", gpuCullStats.occlusionCulledCount);
          ImGui::Text("Hi-Z Candidates: %u", gpuCullStats.hizCandidateCount);
          ImGui::Text("Hi-Z Tested: %u", gpuCullStats.hizTestedCount);
          ImGui::Text("Hi-Z Skipped Large: %u", gpuCullStats.hizRejectedLargeCount);
          ImGui::Text("Hi-Z Skipped Near: %u", gpuCullStats.hizRejectedNearCount);
          ImGui::Text("Hi-Z Skipped Offscreen: %u", gpuCullStats.hizRejectedOffscreenCount);
          ImGui::Text("Meshlet Cone Culled: %u", gpuCullStats.meshletConeCulledCount);
          ImGui::Text("Total: %u", gpuCullStats.totalCount);
          ImGui::Text("Visible Ratio: %.1f%%",
                      100.0f * static_cast<float>(gpuCullStats.visibleCount)
                          / static_cast<float>(totalCullEvaluated));
          ImGui::TreePop();
        }

        // Swapchain diagnostics
        ImGui::Separator();
        ImGui::Text("Renderer Backend: %s", m_renderer.getBackendName());
        {
          demo::profiling::ScopedCpuRange runtimeProfilerPanelRange("AppPreRecord.RuntimeProfilerPanel");
          drawRuntimeProfilerPanel();
        }
        if(m_renderer.getBackend() == demo::RendererBackend::gpuDriven)
        {
          const demo::GPUDrivenRuntimeStats gpuDrivenStats = m_renderer.getGPUDrivenRuntimeStats();
          const char* authorityName = "None";
          switch(gpuDrivenStats.authority)
          {
            case demo::GPUDrivenSceneAuthority::persistentCullObjects:
              authorityName = "Persistent Cull Objects";
              break;
            case demo::GPUDrivenSceneAuthority::futureSceneObjects:
              authorityName = "GPU Scene Objects";
              break;
            default:
              break;
          }
          const char* indirectSourceName = "None";
          switch(gpuDrivenStats.indirectSource)
          {
            case demo::GPUDrivenIndirectSourceKind::gpuCullingOpaqueIndirect:
              indirectSourceName = "GPUCullingPass Opaque Indirect";
              break;
            default:
              break;
          }
          ImGui::Text("Persistent Objects: %u", gpuDrivenStats.objectCount);
          ImGui::Text("GPU Path: %s",
                      m_renderer.isExperimentalMeshletPathEnabled() ? "Experimental Meshlet" : "Object-Level Shipping");
          ImGui::Text("Scene Authority: %s", authorityName);
          ImGui::Text("Indirect Source: %s", indirectSourceName);
          ImGui::Text("Indirect Draws: %u", gpuDrivenStats.indirectDrawCount);
          ImGui::Text("Indirect Stride: %u", gpuDrivenStats.indirectCommandStride);
          ImGui::Text("Persistent Cull Objects: %s", gpuDrivenStats.usesPersistentCullObjects ? "Yes" : "No");
          ImGui::Text("Render Chain Ownership: %s", gpuDrivenStats.ownsFullRenderChain ? "GPU-Driven Full Chain" : "Hybrid");
          ImGui::Text("Hi-Z Ownership: %s", gpuDrivenStats.ownsHiZVisibilityChain ? "GPU-Driven" : "Bridged");
          ImGui::Text("Hi-Z Generation: %llu", static_cast<unsigned long long>(gpuDrivenStats.hiZGeneration));
          const char* visibilityOwnershipLabel = "CPU Bootstrap";
          switch(gpuDrivenStats.visibilityOwnership)
          {
            case demo::GPUDrivenVisibilityOwnership::gpuSortCpuFeedback:
              visibilityOwnershipLabel = "GPU Sort + CPU Feedback";
              break;
            case demo::GPUDrivenVisibilityOwnership::gpuOwned:
              visibilityOwnershipLabel = "GPU-Owned";
              break;
            case demo::GPUDrivenVisibilityOwnership::cpuBootstrap:
            default:
              visibilityOwnershipLabel = "CPU Bootstrap";
              break;
          }
          ImGui::Text("Visibility Ownership: %s", visibilityOwnershipLabel);
          const auto ownershipLabel = [](demo::GPUDrivenOwnershipState ownership) -> const char* {
            switch(ownership)
            {
              case demo::GPUDrivenOwnershipState::gpuOwned:
                return "GPU-Owned";
              case demo::GPUDrivenOwnershipState::bridged:
                return "Bridged";
              case demo::GPUDrivenOwnershipState::legacy:
                return "Legacy";
              case demo::GPUDrivenOwnershipState::disabled:
              default:
                return "Disabled";
            }
          };
          ImGui::Text("Resource Ownership");
          ImGui::Text("  Attachments: %s", ownershipLabel(gpuDrivenStats.resourceOwnership.sceneAttachments));
          ImGui::Text("  Depth Pyramid: %s", ownershipLabel(gpuDrivenStats.resourceOwnership.depthPyramid));
          ImGui::Text("  Visibility: %s", ownershipLabel(gpuDrivenStats.resourceOwnership.visibility));
          ImGui::Text("  Lighting: %s", ownershipLabel(gpuDrivenStats.resourceOwnership.lightingResources));
          ImGui::Text("  Shadows: %s", ownershipLabel(gpuDrivenStats.resourceOwnership.shadowResources));
          ImGui::Text("  Materials: %s", ownershipLabel(gpuDrivenStats.resourceOwnership.materialDescriptors));
          if(ImGui::TreeNode("GPU Pass Ownership"))
          {
            for(const demo::GPUDrivenPassDiagnostic& passDiagnostic : gpuDrivenStats.passDiagnostics)
            {
              ImGui::Text("%s: %s", passDiagnostic.name.c_str(), ownershipLabel(passDiagnostic.ownership));
              if(!passDiagnostic.note.empty())
              {
                ImGui::TextWrapped("  %s", passDiagnostic.note.c_str());
              }
            }
            ImGui::TreePop();
          }
          if(ImGui::TreeNode("Visibility Diagnostics"))
          {
            const demo::GPUDrivenVisibilityDiagnostics& visibilityDiagnostics = gpuDrivenStats.visibilityDiagnostics;
            ImGui::Text("Safe Objects: %u", visibilityDiagnostics.safeObjectCount);
            ImGui::Text("Current GPU Objects: %u", visibilityDiagnostics.currentGPUCullingObjectCount);
            ImGui::Text("Previous GPU Objects: %u", visibilityDiagnostics.previousGPUCullingObjectCount);
            ImGui::Text("Sort Inputs: %u / padded %u",
                        visibilityDiagnostics.sortInputCount,
                        visibilityDiagnostics.sortPaddedCount);
            ImGui::Text("Capacities O/A/T: %u / %u / %u",
                        visibilityDiagnostics.opaqueCapacity,
                        visibilityDiagnostics.alphaCapacity,
                        visibilityDiagnostics.transparentCapacity);
            ImGui::Text("Same-Frame O/A/T: %u / %u / %u",
                        visibilityDiagnostics.sameFrameOpaqueCapacity,
                        visibilityDiagnostics.sameFrameAlphaCapacity,
                        visibilityDiagnostics.sameFrameTransparentCapacity);
            ImGui::Text("Depth Previous Indirect: %s",
                        visibilityDiagnostics.depthUsesPreviousFrameIndirect ? "Yes" : "No");
            ImGui::Text("Depth Sorted Bootstrap: %s",
                        visibilityDiagnostics.depthUsesSortedBootstrap ? "Yes" : "No");
            ImGui::Text("GBuffer Opaque/Alpha Patch: %s",
                        visibilityDiagnostics.gbufferOpaqueAlphaPatchDispatched ? "Yes" : "No");
            ImGui::Text("Transparent Patch: %s",
                        visibilityDiagnostics.transparentPatchDispatched ? "Yes" : "No");
            ImGui::Text("Transparent CPU Seed: %s",
                        visibilityDiagnostics.transparentOrderingCpuSeeded ? "Yes" : "No");
            ImGui::Text("Material Keys CPU Seed: %s",
                        visibilityDiagnostics.materialSortKeysCpuSeeded ? "Yes" : "No");
            ImGui::Text("Mobile Transparent Limit: %u%s",
                        visibilityDiagnostics.maxMobileTransparentDraws,
                        visibilityDiagnostics.transparentCapacityOverflow ? " (overflow)" : "");
            ImGui::TreePop();
          }
          if(ImGui::TreeNode("Hi-Z Diagnostics"))
          {
            const demo::GPUDrivenHiZDiagnostics& hiZDiagnostics = gpuDrivenStats.hiZDiagnostics;
            const double estimatedMiB =
                static_cast<double>(hiZDiagnostics.estimatedMemoryBytes) / (1024.0 * 1024.0);
            ImGui::Text("Valid: %s", hiZDiagnostics.valid ? "Yes" : "No");
            ImGui::Text("Bound For GPU Culling: %s", hiZDiagnostics.boundForGpuCulling ? "Yes" : "No");
            ImGui::Text("Source: %u x %u", hiZDiagnostics.sourceWidth, hiZDiagnostics.sourceHeight);
            ImGui::Text("Pyramid: %u x %u", hiZDiagnostics.pyramidWidth, hiZDiagnostics.pyramidHeight);
            ImGui::Text("Mips: %u / full %u", hiZDiagnostics.mipCount, hiZDiagnostics.fullMipCount);
            ImGui::Text("Policy: /%u, max mips %u, min mip %u",
                        hiZDiagnostics.policyDownsampleDivisor,
                        hiZDiagnostics.policyMaxMipCount,
                        hiZDiagnostics.policyMinMipSize);
            ImGui::Text("Estimated Memory: %.2f MiB", estimatedMiB);
            ImGui::Text("Generation: %llu", static_cast<unsigned long long>(hiZDiagnostics.generation));
            ImGui::Text("Controls F/O/MO/MC: %s / %s / %s / %s",
                        hiZDiagnostics.frustumCullingEnabled ? "On" : "Off",
                        hiZDiagnostics.occlusionCullingEnabled ? "On" : "Off",
                        hiZDiagnostics.meshletOcclusionEnabled ? "On" : "Off",
                        hiZDiagnostics.meshletConeCullingEnabled ? "On" : "Off");
            ImGui::Text("Depth Epsilon: %.4f", hiZDiagnostics.depthEpsilon);
            ImGui::Text("Radius Scale/Bias: %.2f / %.2f",
                        hiZDiagnostics.conservativeRadiusScale,
                        hiZDiagnostics.conservativeRadiusBias);
            ImGui::Text("Near Epsilon: %.5f", hiZDiagnostics.nearRejectEpsilon);
            ImGui::Text("Large Footprint Skip: %.1f px", hiZDiagnostics.largeObjectFootprintThreshold);
            ImGui::Text("Camera Delta: %.2f / %.2f",
                        hiZDiagnostics.cameraDeltaDistance,
                        hiZDiagnostics.fastCameraFallbackDistance);
            ImGui::Text("Fast Camera Fallback: %s",
                        hiZDiagnostics.fastCameraFallbackTriggered ? "Triggered" : "Idle");
            ImGui::TreePop();
          }
          if(ImGui::TreeNode("Post Process Diagnostics"))
          {
            const demo::GPUDrivenPostProcessDiagnostics& postDiagnostics =
                gpuDrivenStats.postProcessDiagnostics;
            const double outputMiB =
                static_cast<double>(postDiagnostics.outputMemoryBytes) / (1024.0 * 1024.0);
            const double hdrMiB =
                static_cast<double>(postDiagnostics.recommendedHdrMemoryBytes) / (1024.0 * 1024.0);
            const double bloomMiB =
                static_cast<double>(postDiagnostics.bloomHalfQuarterMemoryBytes) / (1024.0 * 1024.0);
            ImGui::Text("Output: %u x %u",
                        postDiagnostics.outputWidth,
                        postDiagnostics.outputHeight);
            ImGui::Text("Output Format: %s", postDiagnostics.outputFormatName.c_str());
            ImGui::Text("Scene Color: %s", postDiagnostics.sceneColorFormatName.c_str());
            ImGui::Text("Recommended HDR: %s", postDiagnostics.recommendedHdrFormatName.c_str());
            ImGui::Text("HDR Scene Color Active: %s",
                        postDiagnostics.hdrSceneColorActive ? "Yes" : "No");
            ImGui::Text("Tone Map Location: %s",
                        postDiagnostics.toneMapInLightPass ? "LightPass" : "FinalColor");
            if(ImGui::TreeNode("Temporal"))
            {
              ImGui::Text("Display: %u x %u",
                          postDiagnostics.displayWidth,
                          postDiagnostics.displayHeight);
              ImGui::Text("Internal: %u x %u scale %.2f",
                          postDiagnostics.internalWidth,
                          postDiagnostics.internalHeight,
                          postDiagnostics.renderScale);
              ImGui::Text("Velocity Buffer: %s",
                          postDiagnostics.velocityBufferActive ? "On" : "Off");
              ImGui::Text("TAA: %s history %s blend %.2f jitter %.2f",
                          postDiagnostics.taaPassActive ? "On" : "Off",
                          postDiagnostics.taaHistoryValid ? "Valid" : "Cold",
                          postDiagnostics.taaBlendWeight,
                          postDiagnostics.taaJitterScale);
              ImGui::Text("Upscale Mode: %u%s",
                          postDiagnostics.upscaleMode,
                          postDiagnostics.internalRenderScaleBlocked ? " (blocked)" : "");
              ImGui::TreePop();
            }
            ImGui::Text("Fixed Exposure: %.2f", postDiagnostics.fixedExposure);
            ImGui::Text("Adaptive Exposure: %s target %.2f range %.2f-%.2f",
                        postDiagnostics.adaptiveExposureActive ? "On" : "Off",
                        postDiagnostics.adaptiveExposureTarget,
                        postDiagnostics.minAutoExposure,
                        postDiagnostics.maxAutoExposure);
            ImGui::Text("Bloom: %.2f / threshold %.2f",
                        postDiagnostics.bloomIntensity,
                        postDiagnostics.bloomThreshold);
            ImGui::Text("Grade S/C/G/LUT/V: %.2f / %.2f / %.2f / %.2f / %.2f",
                        postDiagnostics.colorSaturation,
                        postDiagnostics.colorContrast,
                        postDiagnostics.colorGamma,
                        postDiagnostics.colorLutStrength,
                        postDiagnostics.vignetteIntensity);
            ImGui::Text("Lens Dirt: %.2f", postDiagnostics.lensDirtIntensity);
            ImGui::Text("Passes E/AE/B/F/LUT/Lens: %s / %s / %s / %s / %s / %s",
                        postDiagnostics.exposurePassActive ? "On" : "Off",
                        postDiagnostics.adaptiveExposureActive ? "On" : "Off",
                        postDiagnostics.bloomPassActive ? "On" : "Off",
                        postDiagnostics.finalColorPassActive ? "On" : "Off",
                        postDiagnostics.colorGradingLutActive ? "On" : "Off",
                        postDiagnostics.lensEffectsActive ? "On" : "Off");
            ImGui::Text("Memory LDR/HDR/Bloom: %.2f / %.2f / %.2f MiB",
                        outputMiB,
                        hdrMiB,
                        bloomMiB);
            ImGui::TreePop();
          }
          if(ImGui::TreeNode("IBL Diagnostics"))
          {
            const demo::GPUDrivenIBLDiagnostics& iblDiagnostics = gpuDrivenStats.iblDiagnostics;
            const double iblMiB =
                static_cast<double>(iblDiagnostics.estimatedMemoryBytes) / (1024.0 * 1024.0);
            ImGui::Text("Enabled: %s", iblDiagnostics.enabled ? "Yes" : "No");
            ImGui::Text("State: %s%s",
                        iblDiagnostics.loaded ? "Loaded" : "Fallback",
                        iblDiagnostics.fallback ? " (flat ambient)" : "");
            ImGui::Text("Source: %s", iblDiagnostics.path.empty() ? "<none>" : iblDiagnostics.path.c_str());
            ImGui::Text("Mode: %s", iblDiagnostics.sourceMode.empty() ? "<unset>" : iblDiagnostics.sourceMode.c_str());
            ImGui::Text("Status: %s", iblDiagnostics.status.c_str());
            ImGui::Text("Format: %s", iblDiagnostics.formatName.c_str());
            ImGui::Text("Size/Mips: %u x %u / %u",
                        iblDiagnostics.width,
                        iblDiagnostics.height,
                        iblDiagnostics.mipCount);
            ImGui::Text("Intensity: %.2f", iblDiagnostics.intensity);
            ImGui::Text("Debug Mode: %d", iblDiagnostics.debugMode);
            ImGui::Text("Split Sum IBL: irradiance %s, prefilter %s, BRDF LUT %s",
                        iblDiagnostics.irradianceReady ? "Ready" : "Deferred",
                        iblDiagnostics.prefilteredReady ? "Ready" : "Deferred",
                        iblDiagnostics.brdfLutReady ? "Ready" : "Deferred");
            ImGui::Text("Estimated Memory: %.2f MiB", iblMiB);
            ImGui::TreePop();
          }
          if(ImGui::TreeNode("Clustered Lighting Diagnostics"))
          {
            const demo::GPUDrivenClusteredLightingDiagnostics& clusterDiagnostics =
                gpuDrivenStats.clusteredLightingDiagnostics;
            ImGui::Text("Enabled: %s", clusterDiagnostics.enabled ? "Yes" : "No");
            ImGui::Text("Owned Resources: %s", clusterDiagnostics.resourcesOwned ? "Yes" : "No");
            ImGui::Text("Descriptors: %s", clusterDiagnostics.descriptorsReady ? "Ready" : "Missing");
            ImGui::Text("Fallback: %s", clusterDiagnostics.fallbackActive ? "Active" : "No");
            ImGui::Text("Grid: %u x %u x %u (%u)",
                        clusterDiagnostics.gridX,
                        clusterDiagnostics.gridY,
                        clusterDiagnostics.gridZ,
                        clusterDiagnostics.clusterCount);
            ImGui::Text("Lights: %u point, %u spot",
                        clusterDiagnostics.activePointLights,
                        clusterDiagnostics.activeSpotLights);
            ImGui::Text("Capacity: %u point, %u spot, %u per cluster",
                        clusterDiagnostics.maxPointLights,
                        clusterDiagnostics.maxSpotLights,
                        clusterDiagnostics.maxLightsPerCluster);
            ImGui::Text("Memory: %.2f MiB cluster, %.2f MiB light",
                        static_cast<double>(clusterDiagnostics.clusterMemoryBytes) / (1024.0 * 1024.0),
                        static_cast<double>(clusterDiagnostics.lightMemoryBytes) / (1024.0 * 1024.0));
            ImGui::Text("Occupancy: %u max, %u refs",
                        clusterDiagnostics.maxOccupancy,
                        clusterDiagnostics.appendedLightReferences);
            ImGui::Text("Overflow Clusters: %u", clusterDiagnostics.overflowClusterCount);
            ImGui::TreePop();
          }
          if(ImGui::TreeNode("AO / Reflections Diagnostics"))
          {
            const demo::GPUDrivenAOReflectionDiagnostics& aoRefl = gpuDrivenStats.aoReflectionDiagnostics;
            ImGui::Text("AO: %s / %s", aoRefl.aoEnabled ? "Enabled" : "Disabled", aoRefl.aoReady ? "Ready" : "Missing");
            ImGui::Text("AO Extent: %u x %u", aoRefl.aoWidth, aoRefl.aoHeight);
            ImGui::Text("SSR: %s / %s", aoRefl.ssrEnabled ? "Enabled" : "Disabled", aoRefl.ssrReady ? "Ready" : "Missing");
            ImGui::Text("SSR Extent: %u x %u", aoRefl.ssrWidth, aoRefl.ssrHeight);
            ImGui::Text("Memory: %.2f MiB", static_cast<double>(aoRefl.estimatedMemoryBytes) / (1024.0 * 1024.0));
            ImGui::TreePop();
          }
          if(ImGui::TreeNode("Shadow Atlas Diagnostics"))
          {
            const demo::GPUDrivenShadowAtlasDiagnostics& atlas = gpuDrivenStats.shadowAtlasDiagnostics;
            ImGui::Text("Enabled: %s", atlas.enabled ? "Yes" : "No");
            ImGui::Text("Ready: %s", atlas.ready ? "Yes" : "No");
            ImGui::Text("Fallback To CSM: %s", atlas.fallbackToCSM ? "Yes" : "No");
            ImGui::Text("Atlas: %u x %u, tile %u, capacity %u",
                        atlas.atlasWidth,
                        atlas.atlasHeight,
                        atlas.tileSize,
                        atlas.tileCapacity);
            ImGui::Text("Allocated Tiles: %u", atlas.allocatedTiles);
            ImGui::Text("Memory: %.2f MiB", static_cast<double>(atlas.estimatedMemoryBytes) / (1024.0 * 1024.0));
            ImGui::Text("Status: %s", atlas.status.c_str());
            ImGui::TreePop();
          }
          ImGui::Text("GPU Sort Feedback: %s", gpuDrivenStats.batchStats.sortPassCount > 0 ? "Active" : "Idle");
          ImGui::Text("Meshlets: %u", gpuDrivenStats.meshletCount);
          ImGui::Text("Meshlet Triangles: %u", gpuDrivenStats.meshletTriangleCount);
          ImGui::Text("Scene Uploads: %u", gpuDrivenStats.sceneUploadCount);
          ImGui::Text("Pending Scene Updates: %u", gpuDrivenStats.pendingSceneUpdates);
          ImGui::Text("Batch Builder: %u visible, %u sort passes",
                      gpuDrivenStats.batchStats.visibleCount,
                      gpuDrivenStats.batchStats.sortPassCount);
        }

        ImGui::Separator();
        ImGui::Text("Swapchain");
        ImGui::Text("VSync Requested: %s", m_renderer.getVSync() ? "On" : "Off");
        ImGui::Text("Present Mode: %s", m_renderer.getSwapchainPresentModeName());
        ImGui::Text("Swap Images: %u", m_renderer.getSwapchainImageCount());
        ImGui::Text("Fullscreen: %s", m_fullscreen ? "Yes" : "No");
      }
      ImGui::End();

      {
        demo::profiling::ScopedCpuRange flaxDebugUiRange("AppPreRecord.FlaxDebugUI");
        drawFlaxDebugUI();
      }
      {
        demo::profiling::ScopedCpuRange modelLoaderUiRange("AppPreRecord.ModelLoaderUI");
        drawModelLoaderUI();
      }
      {
        demo::profiling::ScopedCpuRange sceneGraphUiRange("AppPreRecord.SceneGraphUI");
        drawSceneGraphUI();
      }

      framePhase = "Render";
      {
        demo::profiling::ScopedCpuRange buildRenderParamsRange("AppPreRecord.BuildRenderParams");
        frameParams.viewportSize   = m_viewportSize;
        frameParams.deltaTime      = isAutomationEnabled()
                                   ? m_automationOptions.fixedDeltaSeconds
                                   : ImGui::GetIO().DeltaTime;
        frameParams.timeSeconds    = m_automationStarted
                                   ? static_cast<float>(m_automationFrame) * m_automationOptions.fixedDeltaSeconds
                                   : static_cast<float>(ImGui::GetTime());
        frameParams.materialHandle = m_selectedMaterial;
        frameParams.clearColor     = m_clearColor;
        frameParams.viewportImageRect = viewportImageRect;
        frameParams.gltfModel      = m_currentModel.has_value() ? &(*m_currentModel) : nullptr;
        frameParams.cameraUniforms = &m_cameraUniforms;
        frameParams.lightSettings  = resolveSceneLightSettings();
        if(const std::vector<demo::SceneLight>* lights = currentSceneLights())
        {
          frameParams.sceneLights = *lights;
        }
        if(m_activeSceneLoadPath == SceneLoadPath::experimentalSceneUploadPlan && m_sceneAsset.has_value())
        {
          frameParams.sceneLightSceneNodes = m_sceneAsset->nodes;
        }
        else if(m_sceneModel.has_value())
        {
          frameParams.sceneLightGltfNodes = m_sceneModel->nodes;
        }
        frameParams.debugOptions   = m_debugOptions;
        if(m_automationOptions.noPost)
        {
          frameParams.debugOptions.enablePostProcessing = false;
          frameParams.debugOptions.enableTAA = false;
          frameParams.debugOptions.upscalingMode = 0;
        }
        else if(m_automationOptions.taa)
        {
          frameParams.debugOptions.enablePostProcessing = true;
          frameParams.debugOptions.enableTAA = true;
          frameParams.debugOptions.upscalingMode = 1;
        }
        if(m_automationOptions.noDdgi)
        {
          frameParams.debugOptions.ddgiDebugVisualize = false;
          frameParams.debugOptions.flaxGIDebugOverlayEnabled = false;
          frameParams.debugOptions.flaxGIDebugViewMask = 0u;
          frameParams.debugOptions.flaxGIDebugMode = 0;
        }
        // Copy CSM debug settings to debugOptions
        frameParams.debugOptions.showShadowCascades    = m_showShadowCascades;
        frameParams.debugOptions.cascadeIndex          = m_cascadeIndex;
        frameParams.debugOptions.cascadeOverlayMode    = m_cascadeOverlayMode;
        frameParams.debugOptions.cascadeOverlayAlpha   = m_cascadeOverlayAlpha;
        if(m_automationStarted)
        {
          if(const char* boundary = automationTargetBoundaryMarkerForFrame(m_automationFrame);
             boundary != nullptr)
          {
            frameParams.automationDebugMarker =
                std::string("CSM_AUTOMATION_FRAME mode=") + automationModeName()
                + " boundary=" + boundary
                + " frame=" + std::to_string(m_automationFrame);
          }
        }
        frameParams.recordUi       = [hideUi = m_automationOptions.noUi]() {
          demo::profiling::ScopedCpuRange renderImguiDrawDataRange("RecordCommandBuffer.RenderImGuiDrawData");
          ImGui::Render();
          if(hideUi)
          {
            if(ImDrawData* drawData = ImGui::GetDrawData())
            {
              drawData->CmdListsCount = 0;
              drawData->TotalIdxCount = 0;
              drawData->TotalVtxCount = 0;
            }
          }
        };
      }
      }

      const bool freezeRenderingForStreamingUpload = m_isLoading && m_renderer.isSceneRenderingSuspended();
      if(!freezeRenderingForStreamingUpload)
      {
        framePhase = "AutomationCaptureSync";
        waitForAutomationCaptureHandshake();
        framePhase = "RendererFacadeRender";
        demo::profiling::ScopedCpuRange rendererFacadeRange("App.RendererFacadeRender");
        m_renderer.render(frameParams);
        ++m_gpuSmokeFrameCount;
        if(!m_loggedFirstFrame)
        {
          LOGI("GPU smoke: first frame rendered");
          m_loggedFirstFrame = true;
        }
        if(m_gpuSmokeFrameCount == 10u)
        {
          LOGI("GPU smoke: 10 frames rendered");
        }
        onAutomationFrameRendered();
      }

      {
        demo::profiling::ScopedCpuRange imguiEndFrameRange("AppPostRecord.ImGuiEndFrame");
        ImGui::EndFrame();
      }
      if((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
      {
        demo::profiling::ScopedCpuRange platformWindowsRange("AppPostRecord.ImGuiPlatformWindows");
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
      }
      }
      catch(const std::exception& e)
      {
        LOGE("Frame failed during phase %s: %s", framePhase, e.what());
        throw;
      }
    }
  }

private:
  void toggleFullscreen()
  {
    if(m_fullscreen)
    {
      m_renderer.setFullscreen(false, nullptr);
      glfwSetWindowAttrib(m_window, GLFW_FLOATING, GLFW_FALSE);
      glfwSetWindowAttrib(m_window, GLFW_DECORATED, GLFW_TRUE);
      glfwSetWindowMonitor(m_window, nullptr, m_windowedX, m_windowedY, m_windowedWidth, m_windowedHeight, 0);
      m_fullscreen = false;
    }
    else
    {
      // Save current window position and size
      glfwGetWindowPos(m_window, &m_windowedX, &m_windowedY);
      glfwGetWindowSize(m_window, &m_windowedWidth, &m_windowedHeight);

      // Switch to fullscreen on primary monitor
      GLFWmonitor* monitor = glfwGetPrimaryMonitor();
      const GLFWvidmode* mode = glfwGetVideoMode(monitor);
      glfwSetWindowAttrib(m_window, GLFW_DECORATED, GLFW_FALSE);
      glfwSetWindowAttrib(m_window, GLFW_FLOATING, GLFW_TRUE);
      glfwSetWindowMonitor(m_window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
      glfwFocusWindow(m_window);
      m_fullscreen = true;
#ifdef _WIN32
      HMONITOR hmonitor = MonitorFromWindow(glfwGetWin32Window(m_window), MONITOR_DEFAULTTONEAREST);
      m_renderer.setFullscreen(true, static_cast<void*>(hmonitor));
#else
      m_renderer.setFullscreen(true, nullptr);
#endif
    }
  }

  GLFWwindow*                                       m_window{};
  std::unique_ptr<demo::rhi::Surface>               m_surface;
  demo::rhi::Extent2D                               m_windowSize{1920, 1080};
  demo::rhi::Extent2D                               m_viewportSize{1920, 1080};
  demo::RendererFacade                              m_renderer;
  bool                       m_vSync{false};
  bool                       m_loggedFirstFrame{false};
  uint32_t                   m_gpuSmokeFrameCount{0};
  demo::MaterialHandle       m_selectedMaterial{};
  demo::rhi::ClearColorValue m_clearColor{0.2f, 0.2f, 0.3f, 1.0f};

  struct AutomationPose
  {
    glm::vec3 position{8.0f, 1.5f, 0.0f};
    float yawDegrees{180.0f};
    float pitchDegrees{0.0f};
  };

  AutomationOptions m_automationOptions{};
  bool m_automationSceneReadyObserved{false};
  bool m_automationStarted{false};
  bool m_automationComplete{false};
  uint64_t m_automationFrame{0u};
  AutomationPose m_automationCurrentPose{};
  AutomationPose m_automationPreviousPose{};

  // glTF model loading
  std::unique_ptr<demo::GltfLoader>               m_gltfLoader;
  std::optional<demo::GltfModel>                  m_sceneModel;
  std::optional<demo::SceneUploadResult>          m_currentModel;
  std::string                                     m_modelPath;
  bool                                            m_modelLoaded = false;

  // Camera
  demo::Camera m_camera;
  demo::Camera m_sceneCameraNavigation;
  float m_moveSpeed{5.0f};       // Units per second
  float m_rotateSpeed{0.1f};     // Mouse sensitivity
  bool m_cursorCaptured{false};  // Mouse capture state
  glm::vec2 m_lastMousePos{0.0f};
  shaderio::CameraUniforms m_cameraUniforms;  // Camera data for rendering

  // Fullscreen state
  bool m_fullscreen{false};
  int m_windowedX{0}, m_windowedY{0};
  int m_windowedWidth{800}, m_windowedHeight{600};
  demo::DirectionalLightSettings m_lightSettings{};
  bool m_enableTestDirectionalLight{false};
  glm::vec3 m_testDirectionalLightColor{1.0f, 0.95f, 0.85f};
  float m_testDirectionalLightIntensity{3.0f};
  float m_lightAzimuthDegrees{0.0f};
  float m_lightElevationDegrees{0.0f};
  demo::DebugPassOptions m_debugOptions{};
  demo::FrameDeferredValue<demo::DDGIConfig> m_pendingDDGIConfig;

  // CSM Shadow debug settings (copied to debugOptions in run())
  bool  m_showShadowCascades{true};
  int   m_cascadeIndex{-1};              // -1 = all cascades, 0-3 = specific cascade
  bool  m_cascadeOverlayMode{false};
  float m_cascadeOverlayAlpha{0.25f};

  // UI state
  char m_modelPathBuffer[512] = "resources/NV_Bistro/bistro_ktx.gltf";
	char m_meshSDFPathBuffer[512] = "";
	std::string m_meshSDFStatus;
	bool m_meshSDFLoaded = false;
	bool m_autoLoadSDFOnSceneReady = false;

  // Preset models
  struct PresetModel {
    const char* name;
    const char* path;
  };
  static constexpr PresetModel m_presetModels[] = {
    {"Sponza", "resources/GLTF_Sponza/sponza.gltf"},
    {"Bistro", "resources/GLTF_Bistro/bistro.gltf"},
    {"NVBistro", "resources/NV_Bistro/bistro_ktx.gltf"},
    {"SponzaNew", "resources/Sponza/sponza.gltf"},
    {"CornellBox", "resources/CornellBox/CornellBox.gltf"},
    {"test", "resources/test/test.gltf"}
  };
  int m_selectedPreset = 0;

  // Async loading state
  struct AsyncLoadResult
  {
    std::optional<demo::GltfModel> model;
    std::optional<demo::SceneAsset> sceneAsset;
    std::optional<demo::SceneUploadPlan> sceneUploadPlan;
    uint32_t sceneUploadJobCount = 0;
    bool experimentalAssetLoadedFromSceneAssetCache = false;
    std::string error;
  };

  enum class SceneLoadPath
  {
    legacyGltf,
    experimentalSceneUploadPlan,
  };

  std::future<AsyncLoadResult> m_loadFuture;
  std::optional<demo::AsyncLoadingCoordinator> m_asyncLoadingCoordinator;
  std::string m_pendingModelPath;
  std::string m_lastLoadError;
  bool m_isLoading = false;
  float m_loadProgress = 0.0f;
  std::string m_loadStatus;
  bool m_enableExperimentalSceneUploadPath{false};
  std::optional<demo::SceneAsset> m_sceneAsset;
  std::optional<demo::SceneAssetView> m_sceneAssetView;
  std::optional<demo::SceneUploadPlan> m_sceneUploadPlan;
  uint32_t m_sceneUploadJobCount{0};
  bool m_sceneAssetLoadedFromCache{false};
  SceneLoadPath m_activeSceneLoadPath{SceneLoadPath::legacyGltf};
  bool m_experimentalSceneCommitPending{false};
  int m_selectedSceneNode = -1;
  ImGuiUtils::ProfilerGraph m_cpuProfilerGraph{240};
  ImGuiUtils::ProfilerGraph m_gpuProfilerGraph{240};
  std::vector<legit::ProfilerTask> m_cpuProfilerTasks;
  std::vector<legit::ProfilerTask> m_gpuProfilerTasks;
  bool m_runtimeProfilerInitialized{false};
  bool m_runtimeProfilerDisabled{false};

  void loadModelAsync(const std::string& path);
  void unloadModel();
  void drawFlaxDebugUI();
  void drawFlaxDebugViewportOverlay(const ImVec2& viewportMin, const ImVec2& viewportMax);
  void drawModelLoaderUI();
  void setMeshSDFPathFromModelPath(const std::string& gltfPath);
  std::filesystem::path resolveMeshSDFPathForLoad() const;
  void loadMeshSDFForDDGI();
  void drawSceneGraphUI();
  void drawSceneNodeTree(int nodeIndex);
  void drawSelectedSceneNodeInspector();
  void applySceneGraphTransforms();
  void updateSceneNodeWorldTransform(int nodeIndex, const glm::mat4& parentTransform);
  void updateAsyncLoading();
  void beginLegacySceneUpload();
  void beginExperimentalSceneUpload();
  [[nodiscard]] bool isAutomationEnabled() const;
  [[nodiscard]] const char* automationModeName() const;
  [[nodiscard]] uint64_t automationSettledFrame() const;
  [[nodiscard]] uint64_t automationTotalFrames() const;
  [[nodiscard]] AutomationPose automationPoseForFrame(uint64_t frame) const;
  [[nodiscard]] const char* automationTargetBoundaryMarkerForFrame(uint64_t frame) const;
  [[nodiscard]] const char* automationBoundaryMarkerForFrame(uint64_t frame) const;
  [[nodiscard]] const char* automationCaptureHandshakeMarkerForFrame(uint64_t frame) const;
  [[nodiscard]] bool isAutomationCaptureSyncEnabled() const;
  void applyAutomationCameraPose();
  void waitForAutomationCaptureHandshake();
  void onAutomationFrameRendered();
  void logAutomationMarker(const char* marker) const;
  void resetSceneCameraNavigation();
  [[nodiscard]] bool populateActiveSceneCameraUniforms(shaderio::CameraUniforms& uniforms) const;
  [[nodiscard]] bool hasActiveSceneCamera() const;
  void updateActiveCamera();
  void refreshActiveCameraUniforms();
  void syncLightAnglesFromDirection();
  void syncLightDirectionFromAngles();
  std::vector<demo::SceneLight>* editableSceneLights();
  const std::vector<demo::SceneLight>* currentSceneLights() const;
  demo::DirectionalLightSettings resolveSceneLightSettings() const;
  void drawSceneLightsUI();
  void drawCSMDebugPanel();
  void resetUIAppearanceStyle();
  void updateRuntimeProfiler();
  void drawRuntimeProfilerPanel();
};

inline void MinimalLatestApp::resetUIAppearanceStyle()
{
  ImGuiStyle& uiStyle = ImGui::GetStyle();
  ImGui::StyleColorsDark(&uiStyle);

  uiStyle.Alpha = 1.0f;
  uiStyle.DisabledAlpha = 0.65f;
  uiStyle.WindowPadding = ImVec2(10.0f, 8.0f);
  uiStyle.FramePadding = ImVec2(5.0f, 3.0f);
  uiStyle.ItemSpacing = ImVec2(8.0f, 5.0f);
  uiStyle.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
  uiStyle.IndentSpacing = 20.0f;
  uiStyle.ScrollbarSize = 11.0f;
  uiStyle.GrabMinSize = 9.0f;
  uiStyle.WindowRounding = 2.0f;
  uiStyle.ChildRounding = 2.0f;
  uiStyle.PopupRounding = 2.0f;
  uiStyle.FrameRounding = 2.0f;
  uiStyle.ScrollbarRounding = 2.0f;
  uiStyle.GrabRounding = 1.0f;
  uiStyle.TabRounding = 2.0f;
  uiStyle.WindowBorderSize = 1.0f;
  uiStyle.ChildBorderSize = 0.0f;
  uiStyle.PopupBorderSize = 1.0f;
  uiStyle.FrameBorderSize = 0.0f;
  uiStyle.TabBorderSize = 0.0f;

  ImVec4* colors = uiStyle.Colors;
  colors[ImGuiCol_Text] = ImVec4(0.94f, 0.95f, 0.98f, 1.00f);
  colors[ImGuiCol_TextDisabled] = ImVec4(0.61f, 0.64f, 0.70f, 0.88f);

  // Low-opacity glass backing so the scene remains visible behind the panels.
  colors[ImGuiCol_WindowBg] = ImVec4(0.018f, 0.022f, 0.030f, 0.24f);
  colors[ImGuiCol_ChildBg] = ImVec4(0.018f, 0.022f, 0.030f, 0.08f);
  colors[ImGuiCol_PopupBg] = ImVec4(0.020f, 0.026f, 0.038f, 0.94f);
  colors[ImGuiCol_Border] = ImVec4(0.52f, 0.57f, 0.66f, 0.28f);
  colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

  // Widgets stay substantially more opaque than their parent window.
  colors[ImGuiCol_FrameBg] = ImVec4(0.040f, 0.052f, 0.072f, 0.56f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.060f, 0.130f, 0.220f, 0.78f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.070f, 0.180f, 0.310f, 0.92f);
  colors[ImGuiCol_TitleBg] = ImVec4(0.018f, 0.024f, 0.034f, 0.30f);
  colors[ImGuiCol_TitleBgActive] = ImVec4(0.026f, 0.050f, 0.080f, 0.48f);
  colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.018f, 0.024f, 0.034f, 0.20f);
  colors[ImGuiCol_MenuBarBg] = ImVec4(0.018f, 0.024f, 0.034f, 0.34f);
  colors[ImGuiCol_ScrollbarBg] = ImVec4(0.010f, 0.014f, 0.020f, 0.12f);
  colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.19f, 0.23f, 0.30f, 0.58f);
  colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.22f, 0.34f, 0.48f, 0.78f);
  colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.16f, 0.42f, 0.70f, 0.92f);

  const ImVec4 accent = ImVec4(0.11f, 0.48f, 0.88f, 1.00f);
  colors[ImGuiCol_CheckMark] = accent;
  colors[ImGuiCol_CheckboxSelectedBg] = ImVec4(0.05f, 0.27f, 0.52f, 0.88f);
  colors[ImGuiCol_SliderGrab] = ImVec4(0.10f, 0.43f, 0.80f, 0.94f);
  colors[ImGuiCol_SliderGrabActive] = ImVec4(0.16f, 0.62f, 1.00f, 1.00f);
  colors[ImGuiCol_Button] = ImVec4(0.05f, 0.19f, 0.34f, 0.62f);
  colors[ImGuiCol_ButtonHovered] = ImVec4(0.06f, 0.32f, 0.58f, 0.84f);
  colors[ImGuiCol_ButtonActive] = ImVec4(0.08f, 0.43f, 0.76f, 0.96f);
  colors[ImGuiCol_Header] = ImVec4(0.040f, 0.075f, 0.110f, 0.34f);
  colors[ImGuiCol_HeaderHovered] = ImVec4(0.060f, 0.240f, 0.420f, 0.68f);
  colors[ImGuiCol_HeaderActive] = ImVec4(0.070f, 0.340f, 0.610f, 0.88f);

  colors[ImGuiCol_Separator] = ImVec4(0.48f, 0.54f, 0.64f, 0.25f);
  colors[ImGuiCol_SeparatorHovered] = ImVec4(0.10f, 0.46f, 0.82f, 0.76f);
  colors[ImGuiCol_SeparatorActive] = accent;
  colors[ImGuiCol_ResizeGrip] = ImVec4(0.10f, 0.42f, 0.76f, 0.18f);
  colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.10f, 0.48f, 0.88f, 0.58f);
  colors[ImGuiCol_ResizeGripActive] = accent;
  colors[ImGuiCol_InputTextCursor] = ImVec4(0.90f, 0.94f, 1.00f, 1.00f);

  colors[ImGuiCol_Tab] = ImVec4(0.025f, 0.060f, 0.100f, 0.46f);
  colors[ImGuiCol_TabHovered] = ImVec4(0.060f, 0.280f, 0.500f, 0.82f);
  colors[ImGuiCol_TabSelected] = ImVec4(0.045f, 0.190f, 0.340f, 0.72f);
  colors[ImGuiCol_TabSelectedOverline] = accent;
  colors[ImGuiCol_TabDimmed] = ImVec4(0.018f, 0.030f, 0.046f, 0.32f);
  colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.030f, 0.100f, 0.170f, 0.50f);
  colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.10f, 0.40f, 0.72f, 0.68f);

  colors[ImGuiCol_DockingPreview] = ImVec4(0.10f, 0.48f, 0.88f, 0.64f);
  colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  colors[ImGuiCol_TableHeaderBg] = ImVec4(0.035f, 0.070f, 0.110f, 0.58f);
  colors[ImGuiCol_TableBorderStrong] = ImVec4(0.40f, 0.46f, 0.56f, 0.34f);
  colors[ImGuiCol_TableBorderLight] = ImVec4(0.32f, 0.38f, 0.46f, 0.22f);
  colors[ImGuiCol_TableRowBg] = ImVec4(0.02f, 0.03f, 0.04f, 0.03f);
  colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.05f, 0.08f, 0.11f, 0.08f);
  colors[ImGuiCol_TextSelectedBg] = ImVec4(0.10f, 0.42f, 0.76f, 0.48f);
  colors[ImGuiCol_TreeLines] = ImVec4(0.48f, 0.54f, 0.64f, 0.30f);
  colors[ImGuiCol_NavCursor] = accent;
  colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.36f);
}

inline void MinimalLatestApp::resetSceneCameraNavigation()
{
  m_sceneCameraNavigation.setPosition(glm::vec3(0.0f));
  m_sceneCameraNavigation.setYawPitch(-90.0f, 0.0f);
  m_sceneCameraNavigation.update();
}

inline bool MinimalLatestApp::isAutomationEnabled() const
{
  return m_automationOptions.mode != AutomationMode::none;
}

inline const char* MinimalLatestApp::automationModeName() const
{
  switch(m_automationOptions.mode)
  {
    case AutomationMode::csmTranslateStop: return "csm-translate-stop";
    case AutomationMode::csmRotateStop: return "csm-rotate-stop";
    default: return "none";
  }
}

inline uint64_t MinimalLatestApp::automationSettledFrame() const
{
  return static_cast<uint64_t>(m_automationOptions.warmupFrames)
       + static_cast<uint64_t>(m_automationOptions.motionFrames)
       + static_cast<uint64_t>(m_automationOptions.holdFrames) - 1u;
}

inline uint64_t MinimalLatestApp::automationTotalFrames() const
{
  return automationSettledFrame() + 1u + (m_automationOptions.captureControlFrame ? 1u : 0u);
}

inline MinimalLatestApp::AutomationPose MinimalLatestApp::automationPoseForFrame(uint64_t frame) const
{
  const AutomationPose start{};
  AutomationPose finish = start;
  if(m_automationOptions.mode == AutomationMode::csmTranslateStop)
  {
    finish.position.z += 1.0f;
  }
  else if(m_automationOptions.mode == AutomationMode::csmRotateStop)
  {
    finish.yawDegrees += 8.0f;
  }

  const uint64_t motionStart = m_automationOptions.warmupFrames;
  const uint64_t motionEnd = motionStart + m_automationOptions.motionFrames;
  if(frame < motionStart)
  {
    return start;
  }
  if(frame >= motionEnd)
  {
    return finish;
  }

  const float progress = static_cast<float>(frame - motionStart + 1u)
                       / static_cast<float>(m_automationOptions.motionFrames);
  AutomationPose pose = start;
  pose.position = glm::mix(start.position, finish.position, progress);
  pose.yawDegrees = glm::mix(start.yawDegrees, finish.yawDegrees, progress);
  pose.pitchDegrees = glm::mix(start.pitchDegrees, finish.pitchDegrees, progress);
  return pose;
}

inline const char* MinimalLatestApp::automationTargetBoundaryMarkerForFrame(uint64_t frame) const
{
  const uint64_t lastMovingFrame =
      static_cast<uint64_t>(m_automationOptions.warmupFrames)
      + static_cast<uint64_t>(m_automationOptions.motionFrames) - 1u;
  if(frame == lastMovingFrame)
  {
    return "last-moving";
  }
  if(frame == lastMovingFrame + 1u)
  {
    return "first-still";
  }
  if(frame == automationSettledFrame())
  {
    return "settled";
  }
  return nullptr;
}

inline const char* MinimalLatestApp::automationBoundaryMarkerForFrame(uint64_t frame) const
{
  if(const char* boundary = automationTargetBoundaryMarkerForFrame(frame); boundary != nullptr)
  {
    return boundary;
  }
  if(m_automationOptions.captureControlFrame && frame == automationSettledFrame() + 1u)
  {
    return "control-still";
  }
  return nullptr;
}

inline const char* MinimalLatestApp::automationCaptureHandshakeMarkerForFrame(uint64_t frame) const
{
  const uint64_t lastMovingFrame =
      static_cast<uint64_t>(m_automationOptions.warmupFrames)
      + static_cast<uint64_t>(m_automationOptions.motionFrames) - 1u;
  if(frame + 1u == lastMovingFrame)
  {
    return "arm-last-moving";
  }
  if(const char* boundary = automationBoundaryMarkerForFrame(frame); boundary != nullptr)
  {
    return boundary;
  }
  if(frame + 1u == automationSettledFrame())
  {
    return "arm-settled";
  }
  return nullptr;
}

inline bool MinimalLatestApp::isAutomationCaptureSyncEnabled() const
{
  return isAutomationEnabled() && !m_automationOptions.captureSyncDirectory.empty();
}

inline void MinimalLatestApp::applyAutomationCameraPose()
{
  if(!m_automationSceneReadyObserved)
  {
    return;
  }
  if(!m_automationStarted)
  {
    m_automationStarted = true;
    m_automationFrame = 0u;
    m_automationCurrentPose = automationPoseForFrame(0u);
    m_automationPreviousPose = m_automationCurrentPose;
    LOGI("[CSM_AUTOMATION] marker=start mode=%s frame=0", automationModeName());
  }

  const uint64_t poseFrame = m_automationComplete ? automationTotalFrames() - 1u : m_automationFrame;
  m_automationCurrentPose = automationPoseForFrame(poseFrame);
  m_camera.setPosition(m_automationCurrentPose.position);
  m_camera.setYawPitch(m_automationCurrentPose.yawDegrees, m_automationCurrentPose.pitchDegrees);
}

inline void MinimalLatestApp::waitForAutomationCaptureHandshake()
{
  if(!isAutomationCaptureSyncEnabled() || !m_automationStarted || m_automationComplete)
  {
    return;
  }

  const char* boundary = automationCaptureHandshakeMarkerForFrame(m_automationFrame);
  if(boundary == nullptr)
  {
    return;
  }

  std::error_code filesystemError;
  std::filesystem::create_directories(m_automationOptions.captureSyncDirectory, filesystemError);
  if(filesystemError)
  {
    throw std::runtime_error("Could not create capture sync directory: " + filesystemError.message());
  }

  const std::filesystem::path readyPath =
      m_automationOptions.captureSyncDirectory / (std::string(boundary) + ".ready.json");
  const std::filesystem::path readyTempPath = readyPath.string() + ".tmp";
  const std::filesystem::path continuePath =
      m_automationOptions.captureSyncDirectory / (std::string(boundary) + ".continue");

  const auto removeStaleMarker = [](const std::filesystem::path& path) {
    std::error_code removeError;
    std::filesystem::remove(path, removeError);
    if(removeError)
    {
      throw std::runtime_error("Could not remove stale capture sync marker " + path.string()
                               + ": " + removeError.message());
    }
  };
  removeStaleMarker(readyTempPath);
  removeStaleMarker(readyPath);
  removeStaleMarker(continuePath);

  {
    std::ofstream readyFile(readyTempPath, std::ios::out | std::ios::trunc);
    if(!readyFile)
    {
      throw std::runtime_error("Could not open capture ready marker: " + readyTempPath.string());
    }
    readyFile << std::fixed << std::setprecision(9)
              << "{\n"
              << "  \"protocol\": \"mgif-csm-capture-sync-v1\",\n"
              << "  \"phase\": \"pre-render\",\n"
              << "  \"no_post\": " << (m_automationOptions.noPost ? "true" : "false") << ",\n"
              << "  \"no_ddgi\": " << (m_automationOptions.noDdgi ? "true" : "false") << ",\n"
              << "  \"taa\": " << (m_automationOptions.taa ? "true" : "false") << ",\n"
              << "  \"capture_control\": " << (m_automationOptions.captureControlFrame ? "true" : "false") << ",\n"
              << "  \"marker\": \"" << boundary << "\",\n"
              << "  \"mode\": \"" << automationModeName() << "\",\n"
              << "  \"frame\": " << m_automationFrame << ",\n"
              << "  \"current\": {\n"
              << "    \"position\": [" << m_automationCurrentPose.position.x << ", "
              << m_automationCurrentPose.position.y << ", " << m_automationCurrentPose.position.z << "],\n"
              << "    \"yaw_degrees\": " << m_automationCurrentPose.yawDegrees << ",\n"
              << "    \"pitch_degrees\": " << m_automationCurrentPose.pitchDegrees << "\n"
              << "  },\n"
              << "  \"previous\": {\n"
              << "    \"position\": [" << m_automationPreviousPose.position.x << ", "
              << m_automationPreviousPose.position.y << ", " << m_automationPreviousPose.position.z << "],\n"
              << "    \"yaw_degrees\": " << m_automationPreviousPose.yawDegrees << ",\n"
              << "    \"pitch_degrees\": " << m_automationPreviousPose.pitchDegrees << "\n"
              << "  }\n"
              << "}\n";
    readyFile.flush();
    if(!readyFile)
    {
      throw std::runtime_error("Could not write capture ready marker: " + readyTempPath.string());
    }
  }

  std::filesystem::rename(readyTempPath, readyPath, filesystemError);
  if(filesystemError)
  {
    throw std::runtime_error("Could not publish capture ready marker: " + filesystemError.message());
  }

  const std::string readyPathText = readyPath.string();
  LOGI("[CSM_AUTOMATION] marker=capture-ready boundary=%s mode=%s frame=%llu phase=pre-render no_post=%d no_ddgi=%d taa=%d capture_control=%d ready=%s",
       boundary,
       automationModeName(),
       static_cast<unsigned long long>(m_automationFrame),
       m_automationOptions.noPost ? 1 : 0,
       m_automationOptions.noDdgi ? 1 : 0,
       m_automationOptions.taa ? 1 : 0,
       m_automationOptions.captureControlFrame ? 1 : 0,
       readyPathText.c_str());

  const auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(m_automationOptions.captureSyncTimeoutMilliseconds);
  while(true)
  {
    const bool continueRequested = std::filesystem::exists(continuePath, filesystemError);
    if(filesystemError)
    {
      throw std::runtime_error("Could not query capture continue marker: " + filesystemError.message());
    }
    if(continueRequested)
    {
      break;
    }
    if(std::chrono::steady_clock::now() >= deadline)
    {
      LOGE("[CSM_AUTOMATION] marker=capture-timeout boundary=%s mode=%s frame=%llu timeout_ms=%u",
           boundary,
           automationModeName(),
           static_cast<unsigned long long>(m_automationFrame),
           m_automationOptions.captureSyncTimeoutMilliseconds);
      throw std::runtime_error("Timed out waiting for capture continue marker: " + continuePath.string());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  std::filesystem::remove(continuePath, filesystemError);
  if(filesystemError)
  {
    throw std::runtime_error("Could not consume capture continue marker: " + filesystemError.message());
  }
  LOGI("[CSM_AUTOMATION] marker=capture-continue boundary=%s mode=%s frame=%llu phase=pre-render",
       boundary,
       automationModeName(),
       static_cast<unsigned long long>(m_automationFrame));
}

inline void MinimalLatestApp::logAutomationMarker(const char* marker) const
{
  LOGI("[CSM_AUTOMATION] marker=%s mode=%s frame=%llu current_pos=(%.6f,%.6f,%.6f) current_yaw_pitch=(%.6f,%.6f) previous_pos=(%.6f,%.6f,%.6f) previous_yaw_pitch=(%.6f,%.6f)",
       marker,
       automationModeName(),
       static_cast<unsigned long long>(m_automationFrame),
       m_automationCurrentPose.position.x,
       m_automationCurrentPose.position.y,
       m_automationCurrentPose.position.z,
       m_automationCurrentPose.yawDegrees,
       m_automationCurrentPose.pitchDegrees,
       m_automationPreviousPose.position.x,
       m_automationPreviousPose.position.y,
       m_automationPreviousPose.position.z,
       m_automationPreviousPose.yawDegrees,
       m_automationPreviousPose.pitchDegrees);
}

inline void MinimalLatestApp::onAutomationFrameRendered()
{
  if(!isAutomationEnabled())
  {
    return;
  }

  if(!m_automationSceneReadyObserved)
  {
    constexpr const char* sponzaPath = "resources/GLTF_Sponza/sponza.gltf";
    if(m_modelLoaded && !m_isLoading && !m_renderer.isSceneRenderingSuspended()
       && m_currentModel.has_value() && m_modelPath == sponzaPath)
    {
      m_automationSceneReadyObserved = true;
      LOGI("[CSM_AUTOMATION] marker=scene-ready mode=%s model=%s",
           automationModeName(), m_modelPath.c_str());
    }
    return;
  }

  if(!m_automationStarted || m_automationComplete)
  {
    return;
  }

  if(const char* boundary = automationBoundaryMarkerForFrame(m_automationFrame); boundary != nullptr)
  {
    logAutomationMarker(boundary);
  }

  m_automationPreviousPose = m_automationCurrentPose;
  if(m_automationFrame == automationTotalFrames() - 1u)
  {
    m_automationComplete = true;
    LOGI("[CSM_AUTOMATION] marker=complete mode=%s frames=%llu",
         automationModeName(),
         static_cast<unsigned long long>(automationTotalFrames()));
    if(m_automationOptions.autoExit)
    {
      glfwSetWindowShouldClose(m_window, true);
    }
    return;
  }
  ++m_automationFrame;
}

inline bool MinimalLatestApp::populateActiveSceneCameraUniforms(shaderio::CameraUniforms& uniforms) const
{
  if(m_activeSceneLoadPath == SceneLoadPath::experimentalSceneUploadPlan && m_sceneAsset.has_value())
  {
    return demo::populatePrimarySceneCameraUniforms(
        m_sceneAsset->cameras,
        m_sceneAsset->nodes,
        std::span<const demo::GltfNodeData>{},
        m_camera.getProjectionMatrix(),
        uniforms);
  }
  if(m_sceneModel.has_value())
  {
    return demo::populatePrimarySceneCameraUniforms(
        m_sceneModel->cameras,
        std::span<const demo::SceneNode>{},
        m_sceneModel->nodes,
        m_camera.getProjectionMatrix(),
        uniforms);
  }

  return false;
}

inline bool MinimalLatestApp::hasActiveSceneCamera() const
{
  shaderio::CameraUniforms sceneCameraUniforms{};
  return populateActiveSceneCameraUniforms(sceneCameraUniforms);
}

inline void MinimalLatestApp::updateActiveCamera()
{
  m_camera.update();
  m_sceneCameraNavigation.update();
  refreshActiveCameraUniforms();
}

inline void MinimalLatestApp::refreshActiveCameraUniforms()
{
  if(m_automationStarted)
  {
    demo::populateCameraUniforms(
        m_camera.getViewMatrix(), m_camera.getProjectionMatrix(), m_camera.getPosition(), m_cameraUniforms);
    return;
  }

  shaderio::CameraUniforms sceneCameraUniforms{};
  if(populateActiveSceneCameraUniforms(sceneCameraUniforms))
  {
    demo::populateNavigatedSceneCameraUniforms(
        sceneCameraUniforms, m_sceneCameraNavigation.getViewMatrix(), m_cameraUniforms);
  }
  else
  {
    demo::populateCameraUniforms(
        m_camera.getViewMatrix(), m_camera.getProjectionMatrix(), m_camera.getPosition(), m_cameraUniforms);
  }
}

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

inline std::vector<demo::SceneLight>* MinimalLatestApp::editableSceneLights()
{
  if(m_activeSceneLoadPath == SceneLoadPath::experimentalSceneUploadPlan && m_sceneAsset.has_value())
  {
    return &m_sceneAsset->lights;
  }
  return m_sceneModel.has_value() ? &m_sceneModel->lights : nullptr;
}

inline const std::vector<demo::SceneLight>* MinimalLatestApp::currentSceneLights() const
{
  if(m_activeSceneLoadPath == SceneLoadPath::experimentalSceneUploadPlan && m_sceneAsset.has_value())
  {
    return &m_sceneAsset->lights;
  }
  return m_sceneModel.has_value() ? &m_sceneModel->lights : nullptr;
}


inline demo::DirectionalLightSettings MinimalLatestApp::resolveSceneLightSettings() const
{
  demo::DirectionalLightSettings settings = m_lightSettings;

  if(m_activeSceneLoadPath == SceneLoadPath::experimentalSceneUploadPlan && m_sceneAsset.has_value())
  {
    if(demo::applyPrimarySceneDirectionalLight(
           m_sceneAsset->lights,
           m_sceneAsset->nodes,
           std::span<const demo::GltfNodeData>{},
           settings))
    {
      return settings;
    }
  }
  else if(m_sceneModel.has_value())
  {
    if(demo::applyPrimarySceneDirectionalLight(
           m_sceneModel->lights,
           std::span<const demo::SceneNode>{},
           m_sceneModel->nodes,
           settings))
    {
      return settings;
    }
  }

  settings.color = glm::vec3(0.0f);
  settings.ambient = glm::vec3(0.0f);

  if(m_enableTestDirectionalLight)
  {
    settings.direction = m_lightSettings.direction;
    settings.color = m_testDirectionalLightColor * m_testDirectionalLightIntensity;
  }

  return settings;
}

inline void MinimalLatestApp::drawSceneLightsUI()
{
  std::vector<demo::SceneLight>* lights = editableSceneLights();
  ImGui::Separator();
  ImGui::Text("glTF Lights");
  if(lights == nullptr)
  {
    ImGui::TextDisabled("No scene loaded.");
    return;
  }
  if(lights->empty())
  {
    ImGui::TextDisabled("Current glTF has no KHR_lights_punctual lights.");
    return;
  }

  static const char* typeNames[] = {"Directional", "Point", "Spot"};
  for(size_t lightIndex = 0; lightIndex < lights->size(); ++lightIndex)
  {
    demo::SceneLight& light = (*lights)[lightIndex];
    ImGui::PushID(static_cast<int>(lightIndex));
    const uint32_t typeIndex = static_cast<uint32_t>(light.type);
    const char* typeName = typeIndex < 3u ? typeNames[typeIndex] : "Unknown";
    const std::string label = light.name.empty()
                                  ? ("Light " + std::to_string(lightIndex) + "##Light")
                                  : (light.name + "##Light");
    if(ImGui::TreeNode(label.c_str(), "%s (%s)", light.name.empty() ? "<unnamed>" : light.name.c_str(), typeName))
    {
      ImGui::Checkbox("Enabled", &light.enabled);
      ImGui::ColorEdit3("Color", &light.color.x);
      ImGui::DragFloat("Intensity", &light.intensity, 0.1f, 0.0f, 100000.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
      if(light.type == demo::SceneLightType::point || light.type == demo::SceneLightType::spot)
      {
        ImGui::DragFloat("Range", &light.range, 0.05f, 0.0f, 10000.0f, "%.3f");
        ImGui::TextDisabled("Range 0 uses scene-bounds fallback.");
      }
      if(light.type == demo::SceneLightType::spot)
      {
        float innerDegrees = glm::degrees(light.innerConeAngle);
        float outerDegrees = glm::degrees(light.outerConeAngle);
        bool coneChanged = false;
        coneChanged |= ImGui::SliderFloat("Inner Cone", &innerDegrees, 0.0f, 89.0f, "%.1f deg");
        coneChanged |= ImGui::SliderFloat("Outer Cone", &outerDegrees, 0.1f, 90.0f, "%.1f deg");
        if(coneChanged)
        {
          outerDegrees = std::max(outerDegrees, innerDegrees + 0.1f);
          light.innerConeAngle = glm::radians(innerDegrees);
          light.outerConeAngle = glm::radians(outerDegrees);
        }
      }
      if(light.nodeIndex >= 0)
      {
        ImGui::Text("Node: %d", light.nodeIndex);
      }
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
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
  m_lastLoadError.clear();
  m_asyncLoadingCoordinator.reset();
  m_sceneAssetView.reset();
  m_sceneAsset.reset();
  m_sceneUploadPlan.reset();
  m_sceneUploadJobCount = 0;
  m_sceneAssetLoadedFromCache = false;
  m_activeSceneLoadPath = SceneLoadPath::legacyGltf;

  // Start async loading (only file parsing, no member access)
  const bool experimentalSceneUploadPath =
      m_enableExperimentalSceneUploadPath && m_renderer.getBackend() == demo::RendererBackend::gpuDriven;
  m_loadFuture = std::async(std::launch::async, [path, experimentalSceneUploadPath]() -> AsyncLoadResult {
    AsyncLoadResult result;
    demo::SceneCacheSerializer cacheSerializer;
    demo::GltfLoader loader;
    demo::SceneAssetSerializer assetSerializer;
    demo::GltfModel model;

    const std::filesystem::path sourcePath(path);
    const std::filesystem::path cachePath = demo::SceneCacheSerializer::buildCachePath(sourcePath);
    const std::filesystem::path assetPath = demo::SceneAssetSerializer::buildAssetPath(sourcePath);

    try
    {
      if(experimentalSceneUploadPath && assetSerializer.isValid(assetPath, sourcePath))
      {
        demo::SceneAsset asset;
        bool sceneAssetCacheLoaded = false;
        if(assetSerializer.load(assetPath, asset))
        {
          sceneAssetCacheLoaded = true;
          demo::ParallelSceneLoader parallelLoader;
          demo::ParallelSceneLoader::BuildResult planBuildResult =
              parallelLoader.build(demo::makeSceneAssetView(asset));
          if(planBuildResult.cancelled)
          {
            result.error = "Parallel scene upload planning was cancelled";
            return result;
          }
          const demo::SceneUploadPlanValidationResult validation =
              demo::SceneUploadPlanner::validate(demo::makeSceneAssetView(asset), planBuildResult.plan);
          if(!validation.valid)
          {
            LOGW("Ignoring scene asset cache %s: %s", assetPath.string().c_str(), validation.error.c_str());
          }
          else
          {

            result.experimentalAssetLoadedFromSceneAssetCache = true;
            result.sceneUploadJobCount = static_cast<uint32_t>(planBuildResult.orderedJobs.size());
            result.sceneUploadPlan = std::move(planBuildResult.plan);
            result.sceneAsset = std::move(asset);
            LOGI("Loaded scene asset cache: %s", assetPath.string().c_str());
            return result;
          }
        }

        if(!sceneAssetCacheLoaded)
        {
          LOGW("Ignoring invalid scene asset cache %s: %s",
               assetPath.string().c_str(),
               assetSerializer.getLastError().c_str());
        }
      }

      if(cacheSerializer.isCacheValid(cachePath, sourcePath))
      {
        if(cacheSerializer.loadCache(cachePath, model))
        {
          LOGI("Loaded scene cache: %s", cachePath.string().c_str());
          result.model = std::move(model);
          if(!experimentalSceneUploadPath)
          {
            return result;
          }
        }
        else
        {
          LOGW("Ignoring invalid scene cache %s: %s",
               cachePath.string().c_str(),
               cacheSerializer.getLastError().c_str());
          std::error_code removeError;
          std::filesystem::remove(cachePath, removeError);
        }
      }

      if(!result.model.has_value() && !loader.load(path, model))
      {
        result.error = loader.getLastError();
        return result;
      }

      if(!result.model.has_value() && !cacheSerializer.saveCache(cachePath, model, sourcePath))
      {
        LOGW("Failed to save scene cache %s: %s", cachePath.string().c_str(), cacheSerializer.getLastError().c_str());
      }

      if(!result.model.has_value())
      {
        result.model = std::move(model);
      }

      if(experimentalSceneUploadPath && result.model.has_value())
      {
        demo::SceneAsset asset;
        bool loadedExperimentalAsset = false;

        if(assetSerializer.isValid(assetPath, sourcePath))
        {
          if(assetSerializer.load(assetPath, asset))
          {
            result.experimentalAssetLoadedFromSceneAssetCache = true;
            loadedExperimentalAsset = true;
            LOGI("Loaded scene asset cache: %s", assetPath.string().c_str());
          }
          else
          {
            LOGW("Ignoring invalid scene asset cache %s: %s",
                 assetPath.string().c_str(),
                 assetSerializer.getLastError().c_str());
          }
        }

        if(!loadedExperimentalAsset)
        {
          asset = demo::SceneAssetBuilder::build(*result.model);
          if(!assetSerializer.save(assetPath, asset, sourcePath))
          {
            LOGW("Failed to save scene asset cache %s: %s",
                 assetPath.string().c_str(),
                 assetSerializer.getLastError().c_str());
          }
        }

        if(experimentalSceneUploadPath)
        {
          demo::ParallelSceneLoader parallelLoader;
          demo::ParallelSceneLoader::BuildResult planBuildResult =
              parallelLoader.build(demo::makeSceneAssetView(asset));
          if(planBuildResult.cancelled)
          {
            result.error = "Parallel scene upload planning was cancelled";
            return result;
          }
          const demo::SceneUploadPlanValidationResult validation =
              demo::SceneUploadPlanner::validate(demo::makeSceneAssetView(asset), planBuildResult.plan);
          if(!validation.valid)
          {
            result.error = "SceneUploadPlan validation failed: " + validation.error;
            return result;
          }

          result.sceneUploadJobCount = static_cast<uint32_t>(planBuildResult.orderedJobs.size());
          result.sceneUploadPlan = std::move(planBuildResult.plan);
        }
        else
        {
          demo::SceneUploadPlanner planner;
          demo::SceneUploadPlanBuildResult planBuildResult = planner.build(demo::makeSceneAssetView(asset));
          const demo::SceneUploadPlanValidationResult validation =
              demo::SceneUploadPlanner::validate(demo::makeSceneAssetView(asset), planBuildResult.plan);
          if(!validation.valid)
          {
            result.error = "SceneUploadPlan validation failed: " + validation.error;
            return result;
          }
          result.sceneUploadJobCount = static_cast<uint32_t>(planBuildResult.orderedJobs.size());
          result.sceneUploadPlan = std::move(planBuildResult.plan);
        }
        result.sceneAsset = std::move(asset);
      }

      return result;
    }
    catch(const std::bad_alloc&)
    {
      LOGE("Out of memory while loading scene: %s", path.c_str());
      result.error = "Out of memory while loading scene";
      return result;
    }
    catch(const std::exception& e)
    {
      LOGE("Scene load failed with exception for %s: %s", path.c_str(), e.what());
      result.error = e.what();
      return result;
    }
  });
}

inline void MinimalLatestApp::updateAsyncLoading()
{
  if(!m_isLoading)
  {
    return;
  }

  if(m_loadFuture.valid())
  {
    if(m_loadProgress < 0.35f)
    {
      m_loadProgress += 0.005f;
      m_loadStatus = "Parsing glTF and checking cache...";
    }

    auto status = m_loadFuture.wait_for(std::chrono::milliseconds(0));
    if(status == std::future_status::ready)
    {
      AsyncLoadResult loadResult;
      try
      {
        loadResult = m_loadFuture.get();
      }
      catch(const std::bad_alloc&)
      {
        m_loadStatus = "Model future allocation failed";
        m_loadProgress = 0.0f;
        m_isLoading = false;
        LOGE("Out of memory while retrieving parsed scene: %s", m_pendingModelPath.c_str());
        return;
      }
      catch(const std::exception& e)
      {
        m_loadStatus = "Model load failed";
        m_loadProgress = 0.0f;
        m_isLoading = false;
        LOGE("Failed to retrieve loaded scene %s: %s", m_pendingModelPath.c_str(), e.what());
        return;
      }

      m_lastLoadError = std::move(loadResult.error);
      const bool hasExperimentalLoad =
          m_enableExperimentalSceneUploadPath
          && loadResult.sceneAsset.has_value()
          && loadResult.sceneUploadPlan.has_value();
      if(loadResult.model.has_value() || hasExperimentalLoad)
      {
        try
        {
          m_loadStatus = "Preparing upload...";
          m_loadProgress = 0.4f;

          m_renderer.waitForIdle();
          unloadModel();

          if(loadResult.model.has_value())
          {
            m_sceneModel = std::move(*loadResult.model);
          }
          else
          {
            m_sceneModel.reset();
          }
          m_sceneAsset = std::move(loadResult.sceneAsset);
          if(m_sceneAsset.has_value())
          {
            m_sceneAssetView = demo::makeSceneAssetView(*m_sceneAsset);
          }
          else
          {
            m_sceneAssetView.reset();
          }
          m_sceneUploadPlan = std::move(loadResult.sceneUploadPlan);
          m_sceneUploadJobCount = loadResult.sceneUploadJobCount;
          m_sceneAssetLoadedFromCache = loadResult.experimentalAssetLoadedFromSceneAssetCache;
          if(m_enableExperimentalSceneUploadPath && m_renderer.getBackend() == demo::RendererBackend::gpuDriven
             && m_sceneAsset.has_value() && !m_sceneAsset->rootNodes.empty())
          {
            m_selectedSceneNode = static_cast<int>(m_sceneAsset->rootNodes.front());
          }
          else if(m_sceneModel.has_value())
          {
            m_selectedSceneNode = m_sceneModel->rootNodes.empty() ? -1 : m_sceneModel->rootNodes.front();
          }
          else
          {
            m_selectedSceneNode = -1;
          }

          m_modelPath = m_pendingModelPath;
          m_modelLoaded = false;

          if(m_sceneModel.has_value())
          {
            LOGI("Loaded glTF model: %s (%zu meshes, %zu materials, %zu textures, %zu lights)",
                 m_pendingModelPath.c_str(),
                 m_sceneModel->meshes.size(),
                 m_sceneModel->materials.size(),
                 m_sceneModel->images.size(),
                 m_sceneModel->lights.size());
          }
          if(m_sceneAsset.has_value())
          {
            LOGI("Loaded SceneAsset: %s (%zu meshes, %zu materials, %zu textures, %zu lights)",
                 m_pendingModelPath.c_str(),
                 m_sceneAsset->meshes.size(),
                 m_sceneAsset->materials.size(),
                 m_sceneAsset->textures.size(),
                 m_sceneAsset->lights.size());
          }
          if(m_sceneUploadPlan.has_value())
          {
            LOGI("Prepared experimental SceneUploadPlan: meshes=%zu textures=%zu materials=%zu instances=%zu draws=%zu jobs=%u source=%s",
                 m_sceneUploadPlan->meshes.size(),
                 m_sceneUploadPlan->textures.size(),
                 m_sceneUploadPlan->materials.size(),
                 m_sceneUploadPlan->instances.instances.size(),
                 m_sceneUploadPlan->drawCommands.size(),
                 m_sceneUploadJobCount,
                 m_sceneAssetLoadedFromCache ? "sceneasset" : "gltf-build");
          }

          m_renderer.setSceneRenderingSuspended(true);
          if(m_enableExperimentalSceneUploadPath && m_renderer.getBackend() == demo::RendererBackend::gpuDriven
             && m_sceneAsset.has_value() && m_sceneAssetView.has_value()
             && m_sceneUploadPlan.has_value())
          {
            beginExperimentalSceneUpload();
          }
          else
          {
            ASSERT(m_sceneModel.has_value(), "Legacy scene upload requires a loaded glTF model");
            beginLegacySceneUpload();
          }
        }
        catch(const std::bad_alloc&)
        {
          m_sceneModel.reset();
          m_sceneAssetView.reset();
          m_sceneAsset.reset();
          m_sceneUploadPlan.reset();
          m_sceneUploadJobCount = 0;
          m_sceneAssetLoadedFromCache = false;
          m_currentModel.reset();
          m_asyncLoadingCoordinator.reset();
          m_experimentalSceneCommitPending = false;
          m_modelLoaded = false;
          m_loadStatus = "Model allocation failed";
          m_loadProgress = 0.0f;
          m_isLoading = false;
          LOGE("Out of memory while preparing scene upload: %s", m_pendingModelPath.c_str());
          return;
        }
      }
      else
      {
        m_loadStatus = "Failed to load model";
        m_loadProgress = 0.0f;
        m_isLoading = false;
        LOGE("Failed to load model: %s (%s)",
             m_pendingModelPath.c_str(),
             m_lastLoadError.empty() ? "unknown error" : m_lastLoadError.c_str());
        return;
      }
    }
  }

  if(m_activeSceneLoadPath == SceneLoadPath::legacyGltf
     && m_asyncLoadingCoordinator.has_value()
     && m_sceneModel.has_value()
     && m_currentModel.has_value())
  {
    demo::AsyncLoadingCoordinator::LoadProgress progress = m_asyncLoadingCoordinator->getProgress();
    if(m_asyncLoadingCoordinator->hasPendingBatches())
    {
      demo::AsyncLoadingCoordinator::UploadBatch batch = m_asyncLoadingCoordinator->takeNextBatch();
      if(!batch.meshIndices.empty() || !batch.materialIndices.empty() || !batch.textureIndices.empty())
      {
        LOGI("Scene upload batch: critical=%d final=%d textures=%zu materials=%zu meshes=%zu",
             batch.criticalBatch ? 1 : 0,
             batch.finalBatch ? 1 : 0,
             batch.textureIndices.size(),
             batch.materialIndices.size(),
             batch.meshIndices.size());
        m_loadStatus = batch.criticalBatch ? "Uploading critical scene assets..." : "Streaming remaining scene assets...";
        m_renderer.executeUploadCommand([this, batch](demo::rhi::CommandBuffer& cmd) {
          m_renderer.uploadGltfModelBatch(*m_sceneModel,
                                          batch.textureIndices,
                                          batch.materialIndices,
                                          batch.meshIndices,
                                          *m_currentModel,
                                          cmd);
        });
        m_asyncLoadingCoordinator->markBatchUploaded(batch);
        progress = m_asyncLoadingCoordinator->getProgress();
      }
    }

    m_loadProgress = 0.35f + progress.progressPercent * 0.65f;
    if(progress.isComplete)
    {
      m_loadStatus = "Finalizing uploads...";
      m_renderer.waitForIdle();
      m_loadProgress = 1.0f;
      m_loadStatus = "Done!";
      m_modelLoaded = true;
      m_renderer.setSceneRenderingSuspended(false);
      m_isLoading = false;
    }
  }

  if(m_activeSceneLoadPath == SceneLoadPath::experimentalSceneUploadPlan
     && m_asyncLoadingCoordinator.has_value()
     && m_sceneAsset.has_value()
     && m_sceneUploadPlan.has_value()
     && m_currentModel.has_value())
  {
    demo::AsyncLoadingCoordinator::LoadProgress progress = m_asyncLoadingCoordinator->getProgress();
    if(m_asyncLoadingCoordinator->hasPendingBatches())
    {
      demo::AsyncLoadingCoordinator::UploadBatch batch = m_asyncLoadingCoordinator->takeNextBatch();
      if(!batch.meshIndices.empty()
         || !batch.materialIndices.empty()
         || !batch.textureIndices.empty()
         || !batch.instanceIndices.empty()
         || !batch.drawCommandIndices.empty())
      {
        LOGI("Scene plan batch: critical=%d final=%d textures=%zu materials=%zu meshes=%zu instances=%zu draws=%zu",
             batch.criticalBatch ? 1 : 0,
             batch.finalBatch ? 1 : 0,
             batch.textureIndices.size(),
             batch.materialIndices.size(),
             batch.meshIndices.size(),
             batch.instanceIndices.size(),
             batch.drawCommandIndices.size());
        m_loadStatus = batch.criticalBatch ? "Preparing critical SceneUploadPlan batches..."
                                           : "Preparing SceneUploadPlan batches...";
        m_asyncLoadingCoordinator->markBatchUploaded(batch);
        progress = m_asyncLoadingCoordinator->getProgress();
      }
    }

    m_loadProgress = 0.35f + progress.progressPercent * 0.35f;
    if(progress.isComplete && m_experimentalSceneCommitPending)
    {
      m_loadStatus = "Committing SceneUploadPlan...";
      m_loadProgress = 0.75f;
      m_renderer.executeUploadCommand([this](demo::rhi::CommandBuffer& cmd) {
        *m_currentModel = m_renderer.commitSceneUploadPlan(*m_sceneAsset, *m_sceneUploadPlan, cmd);
      });
      m_renderer.waitForIdle();
      m_experimentalSceneCommitPending = false;
      m_loadProgress = 1.0f;
      m_loadStatus = "Done!";
      m_modelLoaded = true;
      m_renderer.setSceneRenderingSuspended(false);
      m_isLoading = false;
    }
  }
}

inline void MinimalLatestApp::beginLegacySceneUpload()
{
  ASSERT(m_sceneModel.has_value(), "Legacy scene upload requires a loaded glTF model");

  m_activeSceneLoadPath = SceneLoadPath::legacyGltf;
  resetSceneCameraNavigation();
  m_currentModel.emplace();
  updateActiveCamera();
  m_renderer.initializeGltfUploadResult(*m_sceneModel, *m_currentModel);
  m_asyncLoadingCoordinator.emplace();
  m_asyncLoadingCoordinator->begin(*m_sceneModel, m_cameraUniforms.cameraPosition, 24, 96);
}

inline void MinimalLatestApp::beginExperimentalSceneUpload()
{
  ASSERT(m_renderer.getBackend() == demo::RendererBackend::gpuDriven,
         "Experimental scene upload is currently only supported by GPUDrivenRenderer");
  ASSERT(m_sceneAsset.has_value(), "Experimental scene upload requires a SceneAsset");
  ASSERT(m_sceneAssetView.has_value(), "Experimental scene upload requires a SceneAssetView");
  ASSERT(m_sceneUploadPlan.has_value(), "Experimental scene upload requires a SceneUploadPlan");

  m_activeSceneLoadPath = SceneLoadPath::experimentalSceneUploadPlan;
  resetSceneCameraNavigation();
  m_loadStatus = "Scheduling SceneUploadPlan...";
  m_loadProgress = 0.4f;
  m_currentModel.emplace();
  updateActiveCamera();
  m_asyncLoadingCoordinator.emplace();
  m_asyncLoadingCoordinator->begin(*m_sceneAssetView, *m_sceneUploadPlan, m_cameraUniforms.cameraPosition, 24, 96);
  m_experimentalSceneCommitPending = true;
}

inline void MinimalLatestApp::unloadModel()
{
  m_asyncLoadingCoordinator.reset();
  m_sceneAssetView.reset();
  m_sceneAsset.reset();
  m_sceneUploadPlan.reset();
  m_sceneUploadJobCount = 0;
  m_sceneAssetLoadedFromCache = false;
  m_experimentalSceneCommitPending = false;
  m_activeSceneLoadPath = SceneLoadPath::legacyGltf;
  m_renderer.setSceneRenderingSuspended(false);
  if(m_currentModel.has_value())
  {
    m_renderer.waitForIdle();
    m_renderer.destroyGltfResources(*m_currentModel);
    m_currentModel.reset();
    m_sceneModel.reset();
    m_selectedSceneNode = -1;
    m_modelLoaded = false;
  }
  else
  {
    m_currentModel.reset();
    m_sceneModel.reset();
    m_selectedSceneNode = -1;
    m_modelLoaded = false;
  }
}

inline void MinimalLatestApp::setMeshSDFPathFromModelPath(const std::string& gltfPath)
{
  const std::filesystem::path source(gltfPath);
  std::filesystem::path sdfPath = source;
  sdfPath.replace_filename(source.stem().string() + "_sdf.bin");
  std::strncpy(m_meshSDFPathBuffer, sdfPath.string().c_str(), sizeof(m_meshSDFPathBuffer) - 1);
  m_meshSDFPathBuffer[sizeof(m_meshSDFPathBuffer) - 1] = '\0';
}

inline std::filesystem::path MinimalLatestApp::resolveMeshSDFPathForLoad() const
{
  const std::filesystem::path explicitPath(m_meshSDFPathBuffer);
  std::error_code ec;
  if(!explicitPath.empty() && std::filesystem::exists(explicitPath, ec) && !ec)
  {
    return explicitPath;
  }

  const std::filesystem::path source(m_modelPathBuffer);
  const std::filesystem::path upperBin = source.parent_path() / (source.stem().string() + "_SDF.bin");
  if(std::filesystem::exists(upperBin, ec) && !ec)
  {
    return upperBin;
  }

  const std::filesystem::path lowerBin = source.parent_path() / (source.stem().string() + "_sdf.bin");
  if(std::filesystem::exists(lowerBin, ec) && !ec)
  {
    return lowerBin;
  }

  const std::filesystem::path legacySameExtension =
      source.parent_path() / (source.stem().string() + "_sdf" + source.extension().string());
  if(std::filesystem::exists(legacySameExtension, ec) && !ec)
  {
    return legacySameExtension;
  }

  const std::filesystem::path preferred = lowerBin;
  return explicitPath.empty() ? preferred : explicitPath;
}

inline void MinimalLatestApp::loadMeshSDFForDDGI()
{
  if(m_isLoading)
  {
    m_meshSDFStatus = "Wait for scene loading to finish first.";
    return;
  }

  const std::filesystem::path sdfPath = resolveMeshSDFPathForLoad();
  std::string error;
  if(m_renderer.loadDDGIMeshSDF(sdfPath, error))
  {
    std::strncpy(m_meshSDFPathBuffer, sdfPath.string().c_str(), sizeof(m_meshSDFPathBuffer) - 1);
    m_meshSDFPathBuffer[sizeof(m_meshSDFPathBuffer) - 1] = '\0';
    m_meshSDFLoaded = true;
    m_meshSDFStatus = "Loaded: " + sdfPath.string();
  }
  else
  {
    m_meshSDFLoaded = false;
    m_meshSDFStatus = error.empty() ? ("Failed to load: " + sdfPath.string()) : error;
  }
}

inline void MinimalLatestApp::drawFlaxDebugViewportOverlay(const ImVec2& viewportMin,
                                                            const ImVec2& viewportMax)
{
  if(!m_debugOptions.flaxGIDebugOverlayEnabled)
  {
    return;
  }
  const demo::FlaxGIDebugViewSet views = m_renderer.getFlaxGIDebugViewSet();
  if(!views.isValid())
  {
    return;
  }

  ImDrawList* drawList = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
  drawList->PushClipRect(viewportMin, viewportMax, true);
  const float viewportWidth = viewportMax.x - viewportMin.x;
  const float tileSize = glm::clamp(viewportWidth * 0.14f, 72.0f, 144.0f);
  const float gap = 5.0f;
  const float atlasWidth = 4.0f * tileSize + 3.0f * gap;
  const ImVec2 origin(viewportMax.x - atlasWidth - 8.0f, viewportMin.y + 8.0f);
  const ImTextureID textureId = static_cast<ImTextureID>(views.textureId);

  for(uint32_t index = 0; index < static_cast<uint32_t>(demo::FlaxGIDebugView::count); ++index)
  {
    if((m_debugOptions.flaxGIDebugViewMask & (1u << index)) == 0u)
    {
      continue;
    }
    const uint32_t column = index % views.columns;
    const uint32_t row = index / views.columns;
    const ImVec2 minimum(origin.x + column * (tileSize + gap),
                         origin.y + row * (tileSize + gap));
    const ImVec2 maximum(minimum.x + tileSize, minimum.y + tileSize);
    const ImVec2 uv0(static_cast<float>(column) / static_cast<float>(views.columns),
                     static_cast<float>(row) / static_cast<float>(views.rows));
    const ImVec2 uv1(static_cast<float>(column + 1u) / static_cast<float>(views.columns),
                     static_cast<float>(row + 1u) / static_cast<float>(views.rows));
    drawList->AddRectFilled(ImVec2(minimum.x - 1.0f, minimum.y - 1.0f),
                            ImVec2(maximum.x + 1.0f, maximum.y + 1.0f), IM_COL32(0, 0, 0, 220));
    drawList->AddImage(textureId, minimum, maximum, uv0, uv1);
    const std::string_view label = demo::flaxGIDebugViewName(static_cast<demo::FlaxGIDebugView>(index));
    drawList->AddRectFilled(minimum, ImVec2(maximum.x, minimum.y + ImGui::GetFontSize() + 4.0f),
                            IM_COL32(0, 0, 0, 175));
    drawList->AddText(ImVec2(minimum.x + 3.0f, minimum.y + 2.0f), IM_COL32_WHITE,
                      std::string(label).c_str());
  }
  drawList->PopClipRect();
}

inline void MinimalLatestApp::drawFlaxDebugUI()
{
  if(!ImGui::Begin("FlaxDebugUI"))
  {
    ImGui::End();
    return;
  }

  demo::DDGIConfig config = m_renderer.getDDGIConfig();
  bool configChanged = false;
  bool enabled = config.enabled;
  if(ImGui::Checkbox("Enable GI", &enabled))
  {
    config.enabled = enabled;
    configChanged = true;
  }
  int mode = config.runtimeMode == demo::DDGIRuntimeMode::flaxStyle ? 1 : 0;
  const char* modes[] = {"Current DDGI", "FlaxGI"};
  if(ImGui::Combo("GI Runtime", &mode, modes, IM_ARRAYSIZE(modes)))
  {
    config.runtimeMode = mode == 1 ? demo::DDGIRuntimeMode::flaxStyle
                                   : demo::DDGIRuntimeMode::current;
    configChanged = true;
  }

  ImGui::SeparatorText("Execution Control");
  configChanged |= ImGui::Checkbox("Freeze GI", &config.flaxGIFreeze);
  ImGui::SameLine();
  if(ImGui::Button(config.flaxGISingleStep ? "Full Frame Armed" : "Run Full Frame"))
  {
    config.flaxGISingleStep = true;
    config.flaxGIFreeze = false;
    configChanged = true;
  }
  if(ImGui::Button("Reset Probe History"))
  {
    ++m_debugOptions.flaxGIResetRequestId;
  }

  constexpr demo::FlaxGIDebugStage runnableStages[] = {
    demo::FlaxGIDebugStage::classify,
    demo::FlaxGIDebugStage::initArgs,
    demo::FlaxGIDebugStage::updateInactive,
    demo::FlaxGIDebugStage::traceRays,
    demo::FlaxGIDebugStage::updateDistance,
    demo::FlaxGIDebugStage::updateIrradiance,
  };
  int selectedStage = static_cast<int>(m_debugOptions.flaxGIRunToStage)
                    - static_cast<int>(demo::FlaxGIDebugStage::classify);
  selectedStage = glm::clamp(selectedStage, 0, static_cast<int>(std::size(runnableStages)) - 1);
  const std::string selectedName(demo::flaxGIDebugStageName(runnableStages[selectedStage]));
  if(ImGui::BeginCombo("Stop After Stage", selectedName.c_str()))
  {
    for(int index = 0; index < static_cast<int>(std::size(runnableStages)); ++index)
    {
      const bool selected = index == selectedStage;
      const std::string name(demo::flaxGIDebugStageName(runnableStages[index]));
      if(ImGui::Selectable(name.c_str(), selected))
      {
        m_debugOptions.flaxGIRunToStage = runnableStages[index];
      }
      if(selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  if(ImGui::Button("Run To Selected Stage"))
  {
    ++m_debugOptions.flaxGIRunToStageRequestId;
  }

  ImGui::SeparatorText("FlaxGI Parameters");
  configChanged |= ImGui::SliderFloat("Coverage Radius", &config.giDistance, 5.0f, 200.0f, "%.0f");
  configChanged |= ImGui::SliderFloat("Ray Max Distance", &config.maxDistance, 5.0f, 200.0f, "%.0f");
  configChanged |= ImGui::SliderFloat("History Weight", &config.probeHistoryWeight, 0.5f, 0.999f, "%.3f");
  configChanged |= ImGui::SliderFloat("Normal Bias", &config.normalBias, 0.0f, 2.0f, "%.3f");
  configChanged |= ImGui::SliderFloat("View Bias", &config.viewBias, 0.0f, 2.0f, "%.3f");
  configChanged |= ImGui::SliderFloat("Indirect Intensity", &config.indirectLightingIntensity, 0.0f, 5.0f, "%.2f");
  int maxProbes = static_cast<int>(config.maxUpdatedProbesPerFrame);
  if(ImGui::InputInt("Max Probes / Frame", &maxProbes, 64, 256))
  {
    config.maxUpdatedProbesPerFrame = static_cast<uint32_t>(std::max(maxProbes, 0));
    configChanged = true;
  }
  ImGui::TextDisabled("All allocated cascades execute; DDGI Cascade selects their debug draw.");

  configChanged |= ImGui::Checkbox("Request Surface Atlas", &config.enableGlobalSurfaceAtlas);
  ImGui::SameLine();
  ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f), "Not Implemented");
  bool disableIBL = config.flaxGIDisableIBL;
  if(ImGui::Checkbox("Disable IBL While Inspecting FlaxGI", &disableIBL))
  {
    config.flaxGIDisableIBL = disableIBL;
    m_debugOptions.flaxGIDisableIBL = disableIBL;
    configChanged = true;
  }

  ImGui::SeparatorText("Probe Visualization");
  ImGui::Checkbox("Visualize FlaxGI Probes", &m_debugOptions.ddgiDebugVisualize);
  ImGui::SliderFloat("Bounce Debug Chroma Boost", &m_debugOptions.flaxGIDebugChromaBoost,
                     1.0f, 2.0f, "%.2f");
  if(m_debugOptions.ddgiDebugVisualize)
  {
    ImGui::SliderFloat("Probe Visualize Scale", &m_debugOptions.flaxGIProbeVisualizationScale,
                       0.1f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
  }
  const int maximumDebugCascade =
    std::max(static_cast<int>(config.maxCascades) - 1, 0);
  ImGui::SliderInt("DDGI Cascade (-1 = All)",
                   &m_debugOptions.ddgiDebugCascadeIndex, -1, maximumDebugCascade);
  ImGui::Checkbox("Only Active Probes", &m_debugOptions.ddgiDebugActiveProbes);
  ImGui::Checkbox("DDGI Probe State", &m_debugOptions.ddgiDebugProbeState);
  ImGui::Checkbox("Surface Atlas Coverage", &m_debugOptions.ddgiDebugSurfaceAtlasCoverage);

  ImGui::SeparatorText("Viewport Debug Tiles");
  ImGui::Checkbox("Overlay On Main Viewport", &m_debugOptions.flaxGIDebugOverlayEnabled);
  ImGui::SliderFloat("SDF Z Slice", &m_debugOptions.flaxGIDebugSDFSlice, 0.0f, 1.0f, "%.3f");
  ImGui::SliderFloat("Debug Exposure", &m_debugOptions.flaxGIDebugExposure, 0.05f, 8.0f, "%.2f",
                     ImGuiSliderFlags_Logarithmic);
  ImGui::InputInt("Probe Irradiance Page", &m_debugOptions.flaxGIProbeOverviewPage, 1, 4);
  m_debugOptions.flaxGIProbeOverviewPage =
    std::max(m_debugOptions.flaxGIProbeOverviewPage, 0);
  ImGui::TextDisabled(
    "Green Distance/State tiles are semantic state colors; bounce RGB is shown in "
    "Trace Radiance / Probe Irradiance.");
  for(uint32_t index = 0; index < static_cast<uint32_t>(demo::FlaxGIDebugView::count); ++index)
  {
    bool visible = (m_debugOptions.flaxGIDebugViewMask & (1u << index)) != 0u;
    const std::string label(demo::flaxGIDebugViewName(static_cast<demo::FlaxGIDebugView>(index)));
    if(ImGui::Checkbox(label.c_str(), &visible))
    {
      if(visible) m_debugOptions.flaxGIDebugViewMask |= (1u << index);
      else m_debugOptions.flaxGIDebugViewMask &= ~(1u << index);
    }
  }

  ImGui::SeparatorText("Stage Validation");
  const demo::FlaxGIDebugSnapshot snapshot = m_renderer.getFlaxGIDebugSnapshot();
  ImGui::Text("Distance Moments: v%u, normalized E[d] / E[d^2]",
              snapshot.distanceMomentSchemaVersion);
  ImGui::Text("Spacing / Horizon: %.3f / %.3f", snapshot.probeSpacing,
              snapshot.distanceMomentHorizon);
  ImGui::Text("SDF Voxel / Ray Start / Self Hit: %.4f / %.4f / %.4f",
              snapshot.sdfVoxelSize, snapshot.traceRayStartOffset,
              snapshot.selfHitThreshold);
  ImGui::Text("Visibility Floor: %.3f", snapshot.minimumVisibility);
  ImGui::Separator();
  for(uint32_t index = 0; index < static_cast<uint32_t>(demo::FlaxGIDebugStage::count); ++index)
  {
    const demo::FlaxGIDebugStage stage = static_cast<demo::FlaxGIDebugStage>(index);
    const demo::FlaxGIDebugStageStatus& status = snapshot.stages[index];
    const ImVec4 color = status.state == demo::FlaxGIDebugStageState::valid
      ? ImVec4(0.35f, 0.95f, 0.45f, 1.0f)
      : status.state == demo::FlaxGIDebugStageState::invalid
        || status.state == demo::FlaxGIDebugStageState::blocked
        ? ImVec4(1.0f, 0.35f, 0.3f, 1.0f)
        : status.state == demo::FlaxGIDebugStageState::notImplemented
          ? ImVec4(1.0f, 0.55f, 0.2f, 1.0f)
          : ImVec4(0.75f, 0.8f, 0.9f, 1.0f);
    const std::string stageName(demo::flaxGIDebugStageName(stage));
    const std::string stateName(demo::flaxGIDebugStageStateName(status.state));
    ImGui::TextColored(color, "%02u %-24s %s", index + 1u, stageName.c_str(), stateName.c_str());
    if(!status.reason.empty())
    {
      ImGui::Indent();
      ImGui::TextWrapped("%.*s", static_cast<int>(status.reason.size()), status.reason.data());
      ImGui::Unindent();
    }
  }

  const demo::FlaxGIDebugTelemetry& telemetry = snapshot.telemetry;
  ImGui::SeparatorText("GPU Telemetry");
  if(telemetry.gpuReadbackValid)
  {
    ImGui::Text("Source Frame: %llu", static_cast<unsigned long long>(telemetry.sourceFrame));
    ImGui::TextColored(telemetry.activeProbeCountValid ? ImVec4(0.35f, 0.95f, 0.45f, 1.0f)
                                                       : ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
                       "Active Probes: %u (%s)", telemetry.classifiedActiveProbeCount,
                       telemetry.activeProbeCountValid ? "Valid" : "Classify Failed");
    ImGui::Text("Trace Dispatch: %u,%u,%u", telemetry.actualDispatch.trace.x,
                telemetry.actualDispatch.trace.y, telemetry.actualDispatch.trace.z);
    ImGui::Text("Distance Dispatch: %u,%u,%u", telemetry.actualDispatch.distance.x,
                telemetry.actualDispatch.distance.y, telemetry.actualDispatch.distance.z);
    ImGui::Text("Irradiance Dispatch: %u,%u,%u", telemetry.actualDispatch.irradiance.x,
                telemetry.actualDispatch.irradiance.y, telemetry.actualDispatch.irradiance.z);
    ImGui::TextColored(telemetry.indirectArgsValid ? ImVec4(0.35f, 0.95f, 0.45f, 1.0f)
                                                    : ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
                       "Indirect Args: %s", telemetry.indirectArgsValid ? "Valid" : "Mismatch");
  }
  else
  {
    ImGui::TextDisabled("GPU telemetry is pending the first completed readback frame.");
  }

  const char* lightingModes[] = {"Off", "Irradiance", "No Visibility", "Diffuse Term", "Gate State", "Atlas Direct"};
  ImGui::Combo("Lighting Debug", &m_debugOptions.flaxGIDebugMode,
               lightingModes, IM_ARRAYSIZE(lightingModes));
  ImGui::SliderFloat("Lighting Debug Scale", &m_debugOptions.flaxGIDebugScale, 0.01f, 10.0f, "%.2f");

  if(configChanged)
  {
    m_pendingDDGIConfig.defer(config);
  }
  ImGui::End();
}
inline void MinimalLatestApp::drawModelLoaderUI()
{
  if(ImGui::Begin("Model Loader"))
  {
    ImGui::Checkbox("Experimental SceneUploadPlan", &m_enableExperimentalSceneUploadPath);
    ImGui::TextDisabled("Legacy rendering/upload path stays available. This switch enables the SceneAsset + plan upload path.");
    ImGui::Separator();

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
          setMeshSDFPathFromModelPath(m_modelPathBuffer);
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
      if(ImGui::Button("Use Path For Mesh SDF"))
      {
        setMeshSDFPathFromModelPath(m_modelPathBuffer);
      }
    }

    if(ImGui::CollapsingHeader("DDGI Mesh SDF", ImGuiTreeNodeFlags_DefaultOpen))
    {
      demo::DDGIConfig ddgiConfig = m_renderer.getDDGIConfig();
      bool ddgiEnabled = ddgiConfig.enabled;
      if(ImGui::Checkbox("Enable DDGI", &ddgiEnabled))
      {
        ddgiConfig.enabled = ddgiEnabled;
        m_pendingDDGIConfig.defer(ddgiConfig);
      }
      bool ddgiConfigChanged = false;
      if(ddgiConfig.runtimeMode == demo::DDGIRuntimeMode::current)
      {
        ImGui::SeparatorText("Current DDGI Parameters");
        ddgiConfigChanged |= ImGui::SliderFloat("Hysteresis", &ddgiConfig.hysteresis, 0.0f, 0.995f, "%.3f");
        ddgiConfigChanged |= ImGui::SliderFloat("DDGI Weight", &ddgiConfig.ddgiWeight, 0.0f, 1.0f, "%.2f");
        int updateStride = static_cast<int>(ddgiConfig.updateStride);
        if(ImGui::InputInt("Update Stride", &updateStride))
        {
          ddgiConfig.updateStride = static_cast<uint32_t>(std::max(updateStride, 1));
          ddgiConfigChanged = true;
        }
      }
      if(ddgiConfigChanged)
      {
        ddgiConfig.hysteresis = glm::clamp(ddgiConfig.hysteresis, 0.0f, 0.995f);
        ddgiConfig.ddgiWeight = glm::clamp(ddgiConfig.ddgiWeight, 0.0f, 1.0f);
        ddgiConfig.updateStride = std::max(ddgiConfig.updateStride, 1u);
        m_pendingDDGIConfig.defer(ddgiConfig);
      }
      ImGui::InputText("Mesh SDF Path", m_meshSDFPathBuffer, sizeof(m_meshSDFPathBuffer));
      if(ImGui::Button("From glTF Path"))
      {
        setMeshSDFPathFromModelPath(m_modelPathBuffer);
      }
      ImGui::SameLine();
      if(ImGui::Button("Load Mesh SDF"))
      {
        loadMeshSDFForDDGI();
      }
      ImGui::SameLine();
      if(ImGui::Button("Reset History"))
      {
        m_renderer.resetDDGIHistory();
        m_meshSDFStatus = "DDGI history reset.";
      }
      ImGui::Text("DDGI: %s", m_renderer.isDDGIEnabled() ? "enabled" : "disabled");
      ImGui::Text("DDGI Mode: %s", ddgiConfig.runtimeMode == demo::DDGIRuntimeMode::flaxStyle ? "Flax-style" : "Current");
      ImGui::Text("Mesh SDF: %s", (m_meshSDFLoaded || m_renderer.hasDDGIMeshSDF()) ? "loaded" : "not loaded");
      if(!m_meshSDFStatus.empty())
      {
        ImGui::TextWrapped("%s", m_meshSDFStatus.c_str());
      }
    }

    // Load button
    if(ImGui::Button(m_isLoading ? "Loading..." : "Load Model", ImVec2(120, 0)))
    {
      if(!m_isLoading)
      {
        setMeshSDFPathFromModelPath(m_modelPathBuffer);
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
      if(m_asyncLoadingCoordinator.has_value())
      {
        const demo::AsyncLoadingCoordinator::LoadProgress& progress = m_asyncLoadingCoordinator->getProgress();
        ImGui::Text("Meshes %u/%u  Materials %u/%u  Textures %u/%u",
                    progress.meshesLoaded,
                    progress.meshesTotal,
                    progress.materialsLoaded,
                    progress.materialsTotal,
                    progress.texturesLoaded,
                    progress.texturesTotal);
        if(m_activeSceneLoadPath == SceneLoadPath::experimentalSceneUploadPlan)
        {
          ImGui::Text("Instances %u/%u  Draws %u/%u",
                      progress.instancesLoaded,
                      progress.instancesTotal,
                      progress.drawCommandsLoaded,
                      progress.drawCommandsTotal);
        }
      }
      if(m_enableExperimentalSceneUploadPath && m_renderer.getBackend() == demo::RendererBackend::gpuDriven)
      {
        ImGui::TextDisabled("Experimental path keeps the legacy glTF upload route available as fallback.");
      }
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
      ImGui::Text("Path: %s",
                  m_activeSceneLoadPath == SceneLoadPath::experimentalSceneUploadPlan
                      ? "Experimental SceneUploadPlan"
                      : "Legacy glTF Upload");
      if(m_currentModel.has_value())
      {
        ImGui::Text("  Meshes: %zu", m_currentModel->meshes.size());
        ImGui::Text("  Materials: %zu", m_currentModel->materials.size());
        ImGui::Text("  Textures: %zu", m_currentModel->textures.size());
      }
      if(m_enableExperimentalSceneUploadPath && m_renderer.getBackend() == demo::RendererBackend::gpuDriven
         && m_sceneUploadPlan.has_value())
      {
        ImGui::Separator();
        ImGui::Text("Experimental SceneAsset: %s",
                    m_sceneAssetLoadedFromCache ? "Loaded from .sceneasset" : "Built from glTF");
        ImGui::Text("  Jobs: %u", m_sceneUploadJobCount);
        ImGui::Text("  Mesh plans: %zu", m_sceneUploadPlan->meshes.size());
        ImGui::Text("  Texture plans: %zu", m_sceneUploadPlan->textures.size());
        ImGui::Text("  Material plans: %zu", m_sceneUploadPlan->materials.size());
        ImGui::Text("  Instances: %zu", m_sceneUploadPlan->instances.instances.size());
        ImGui::Text("  Draw plans: %zu", m_sceneUploadPlan->drawCommands.size());
      }
    }
  }
  ImGui::End();
}

inline void MinimalLatestApp::drawSceneGraphUI()
{
  if(ImGui::Begin("Scene Graph"))
  {
    const bool useSceneAssetGraph = m_activeSceneLoadPath == SceneLoadPath::experimentalSceneUploadPlan
                                 && m_sceneAsset.has_value();
    const bool hasLegacyGraph = m_sceneModel.has_value();
    if((!useSceneAssetGraph && !hasLegacyGraph) || !m_currentModel.has_value())
    {
      ImGui::TextDisabled("No scene loaded.");
    }
    else
    {
      const char* sceneName = useSceneAssetGraph ? m_sceneAsset->name.c_str() : m_sceneModel->name.c_str();
      ImGui::Text("Model: %s", sceneName);
      ImGui::Separator();

      const float panelWidth = ImGui::GetContentRegionAvail().x;
      const float treeWidth = panelWidth * 0.5f;

      ImGui::BeginChild("##SceneTree", ImVec2(treeWidth, 0.0f), true);
      if(useSceneAssetGraph)
      {
        for(const uint32_t rootNodeIndex : m_sceneAsset->rootNodes)
        {
          drawSceneNodeTree(static_cast<int>(rootNodeIndex));
        }
      }
      else
      {
        for(const int rootNodeIndex : m_sceneModel->rootNodes)
        {
          drawSceneNodeTree(rootNodeIndex);
        }
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
  const bool useSceneAssetGraph = m_activeSceneLoadPath == SceneLoadPath::experimentalSceneUploadPlan
                               && m_sceneAsset.has_value();
  if(useSceneAssetGraph)
  {
    if(nodeIndex < 0 || nodeIndex >= static_cast<int>(m_sceneAsset->nodes.size()))
    {
      return;
    }

    const demo::SceneNode& node = m_sceneAsset->nodes[nodeIndex];
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
    if(!node.meshRefs.empty())
    {
      label += " (" + std::to_string(node.meshRefs.size()) + ")";
    }

    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<intptr_t>(nodeIndex)), flags, "%s", label.c_str());
    if(ImGui::IsItemClicked())
    {
      m_selectedSceneNode = nodeIndex;
    }

    if(open)
    {
      for(const uint32_t childIndex : node.children)
      {
        drawSceneNodeTree(static_cast<int>(childIndex));
      }
      ImGui::TreePop();
    }
    return;
  }

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
  const bool useSceneAssetGraph = m_activeSceneLoadPath == SceneLoadPath::experimentalSceneUploadPlan
                               && m_sceneAsset.has_value();
  if(useSceneAssetGraph)
  {
    if(m_selectedSceneNode < 0 || m_selectedSceneNode >= static_cast<int>(m_sceneAsset->nodes.size()))
    {
      ImGui::TextDisabled("Select a node to edit its transform.");
      return;
    }

    demo::SceneNode& node = m_sceneAsset->nodes[m_selectedSceneNode];
    ImGui::Text("Node");
    ImGui::Separator();
    ImGui::TextWrapped("%s", node.name.c_str());
    ImGui::Text("Children: %d", static_cast<int>(node.children.size()));
    ImGui::Text("Meshes: %d", static_cast<int>(node.meshRefs.size()));
    ImGui::Text("Parent: %s",
                node.parent >= 0 && node.parent < static_cast<int>(m_sceneAsset->nodes.size())
                    ? m_sceneAsset->nodes[node.parent].name.c_str()
                    : "<root>");

    ImGui::Separator();
    ImGui::Text("Local Transform");

    glm::vec3 rotationEulerDegrees = glm::degrees(glm::eulerAngles(node.rotation));
    bool transformChanged = false;
    transformChanged |= ImGui::DragFloat3("Translation", &node.translation.x, 0.05f);
    transformChanged |= ImGui::DragFloat3("Rotation", &rotationEulerDegrees.x, 0.5f);
    transformChanged |= ImGui::DragFloat3("Scale", &node.scale.x, 0.01f, 0.001f, 1000.0f, "%.3f");

    if(transformChanged)
    {
      node.scale = glm::max(node.scale, glm::vec3(0.001f));
      node.rotation = glm::normalize(glm::quat(glm::radians(rotationEulerDegrees)));
      applySceneGraphTransforms();
    }

    ImGui::Separator();
    const glm::vec3 worldPosition = glm::vec3(node.worldTransform[3]);
    ImGui::Text("World Position");
    ImGui::Text("  X: %.3f", worldPosition.x);
    ImGui::Text("  Y: %.3f", worldPosition.y);
    ImGui::Text("  Z: %.3f", worldPosition.z);
    return;
  }

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
  if(!m_currentModel.has_value())
  {
    return;
  }

  if(m_activeSceneLoadPath == SceneLoadPath::experimentalSceneUploadPlan && m_sceneAsset.has_value())
  {
    for(const uint32_t rootNodeIndex : m_sceneAsset->rootNodes)
    {
      updateSceneNodeWorldTransform(static_cast<int>(rootNodeIndex), glm::mat4(1.0f));
    }
    m_sceneAssetView = demo::makeSceneAssetView(*m_sceneAsset);
    m_renderer.syncActiveSceneRuntimeState(*m_currentModel);
    // Scene Graph edits may have changed the active camera node or any ancestor.
    // Rebuild only the current uniforms snapshot; input and temporal history are
    // advanced elsewhere exactly once per rendered frame.
    refreshActiveCameraUniforms();
    return;
  }

  if(!m_sceneModel.has_value())
  {
    return;
  }

  for(const int rootNodeIndex : m_sceneModel->rootNodes)
  {
    updateSceneNodeWorldTransform(rootNodeIndex, glm::mat4(1.0f));
  }
  refreshActiveCameraUniforms();
  m_renderer.syncActiveSceneRuntimeState(*m_currentModel);
}

inline void MinimalLatestApp::updateSceneNodeWorldTransform(int nodeIndex, const glm::mat4& parentTransform)
{
  if(!m_currentModel.has_value())
  {
    return;
  }

  if(m_activeSceneLoadPath == SceneLoadPath::experimentalSceneUploadPlan && m_sceneAsset.has_value())
  {
    if(nodeIndex < 0 || nodeIndex >= static_cast<int>(m_sceneAsset->nodes.size()))
    {
      return;
    }

    demo::SceneNode& node = m_sceneAsset->nodes[nodeIndex];
    node.localTransform = glm::translate(glm::mat4(1.0f), node.translation)
                        * glm::mat4_cast(node.rotation)
                        * glm::scale(glm::mat4(1.0f), node.scale);
    node.worldTransform = parentTransform * node.localTransform;

    if(m_sceneUploadPlan.has_value())
    {
      for(const demo::SceneDrawInstance& instance : m_sceneUploadPlan->instances.instances)
      {
        if(instance.nodeIndex == static_cast<uint32_t>(nodeIndex))
        {
          m_renderer.updateSceneInstanceTransform(instance.instanceIndex, node.worldTransform);
        }
      }
    }

    for(const uint32_t childIndex : node.children)
    {
      updateSceneNodeWorldTransform(static_cast<int>(childIndex), node.worldTransform);
    }
    return;
  }

  if(!m_sceneModel.has_value())
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

inline void MinimalLatestApp::updateRuntimeProfiler()
{
  try
  {
    const demo::RuntimeProfileSnapshot snapshot = m_renderer.getRuntimeProfileSnapshot();
    if(snapshot.passNames.empty())
    {
      return;
    }

    static constexpr size_t kMaxReasonableProfilePassCount = 256;
    const size_t safePassCount = std::min({
        snapshot.passNames.size(),
        snapshot.cpuPassDurationsMs.size(),
        snapshot.gpuPassDurationsMs.size(),
        kMaxReasonableProfilePassCount,
    });
    if(safePassCount == 0)
    {
      return;
    }

    static constexpr std::array<uint32_t, 8> kTaskColors = {
        legit::Colors::peterRiver,
        legit::Colors::emerald,
        legit::Colors::sunFlower,
        legit::Colors::carrot,
        legit::Colors::amethyst,
        legit::Colors::alizarin,
        legit::Colors::clouds,
        legit::Colors::turqoise,
    };

    auto buildTasks = [](const std::vector<std::string>& passNames,
                         const std::vector<double>& durationsMs,
                         size_t count,
                         const std::array<uint32_t, 8>& colors,
                         std::vector<legit::ProfilerTask>& outTasks) {
      outTasks.clear();
      outTasks.reserve(count);

      double cursorSeconds = 0.0;
      for(size_t i = 0; i < count; ++i)
      {
        const double durationSeconds = std::max(0.0, durationsMs[i]) * 1e-3;
        if(durationSeconds <= 0.0)
        {
          continue;
        }

        legit::ProfilerTask task{};
        task.startTime = cursorSeconds;
        task.endTime = cursorSeconds + durationSeconds;
        task.name = passNames[i];
        task.color = colors[i % colors.size()];
        outTasks.push_back(task);
        cursorSeconds = task.endTime;
      }
    };

    buildTasks(snapshot.passNames, snapshot.cpuPassDurationsMs, safePassCount, kTaskColors, m_cpuProfilerTasks);
    buildTasks(snapshot.passNames, snapshot.gpuPassDurationsMs, safePassCount, kTaskColors, m_gpuProfilerTasks);

    m_cpuProfilerGraph.LoadFrameData(m_cpuProfilerTasks.data(), m_cpuProfilerTasks.size());
    if(snapshot.gpuValid)
    {
      m_gpuProfilerGraph.LoadFrameData(m_gpuProfilerTasks.data(), m_gpuProfilerTasks.size());
    }
    else
    {
      m_gpuProfilerGraph.LoadFrameData(nullptr, 0);
    }

    m_runtimeProfilerInitialized = true;
  }
  catch(const std::bad_alloc&)
  {
    m_cpuProfilerTasks.clear();
    m_gpuProfilerTasks.clear();
    m_cpuProfilerGraph.LoadFrameData(nullptr, 0);
    m_gpuProfilerGraph.LoadFrameData(nullptr, 0);
    m_runtimeProfilerInitialized = false;
    m_runtimeProfilerDisabled = true;
    LOGE("Runtime profiler disabled after allocation failure");
  }
}

inline void MinimalLatestApp::drawRuntimeProfilerPanel()
{
  if(m_runtimeProfilerDisabled)
  {
    ImGui::TextUnformatted("Runtime Profiler: disabled after allocation failure.");
    return;
  }

  if(!m_runtimeProfilerInitialized)
  {
    ImGui::TextUnformatted("Runtime Profiler: waiting for frame timings...");
    return;
  }

  ImGui::SeparatorText("Runtime Profiler");
  const float maxFrameTime = std::max(1.0f / 30.0f, ImGui::GetIO().DeltaTime * 1.5f);
  const float availableWidth = ImGui::GetContentRegionAvail().x;
  const int legendWidth = 220;
  const int graphWidth = std::max(120, static_cast<int>(availableWidth) - legendWidth);

  ImGui::TextUnformatted("CPU Pass Timeline");
  m_cpuProfilerGraph.useColoredLegendText = true;
  m_cpuProfilerGraph.frameWidth = 3;
  m_cpuProfilerGraph.frameSpacing = 1;
  m_cpuProfilerGraph.RenderTimings(graphWidth, legendWidth, 140, 0, maxFrameTime);

  ImGui::Spacing();
  ImGui::TextUnformatted("GPU Pass Timeline");
  m_gpuProfilerGraph.useColoredLegendText = true;
  m_gpuProfilerGraph.frameWidth = 3;
  m_gpuProfilerGraph.frameSpacing = 1;
  m_gpuProfilerGraph.RenderTimings(graphWidth, legendWidth, 140, 0, maxFrameTime);
}
