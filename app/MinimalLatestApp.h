#pragma once

#include "../common/Common.h"
#include "../common/ProfilerMarkers.h"
#include "../common/TracyProfiling.h"

#ifdef _WIN32
#include <windows.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include "../render/RendererFacade.h"
#include "../rhi/RHISurface.h"
#include "../rhi/vulkan/VulkanSurface.h"
#include "../loader/GltfLoader.h"
#include "../loader/SceneCacheSerializer.h"
#include "../render/AsyncLoadingCoordinator.h"
#include "../render/Camera.h"
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
    std::string path = "resources/Sponza/Sponza.gltf";
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
        TRACY_ZONE_SCOPED("App::PollEvents");
        glfwPollEvents();
      }

      // Check async loading progress
      framePhase = "UpdateAsyncLoading";
      {
        demo::profiling::ScopedCpuRange asyncLoadingRange("AppPreRecord.UpdateAsyncLoading");
        TRACY_ZONE_SCOPED("App::UpdateAsyncLoading");
        updateAsyncLoading();
      }

      // Camera input handling
      {
          demo::profiling::ScopedCpuRange inputCameraRange("AppPreRecord.InputCamera");
          TRACY_ZONE_SCOPED("App::InputCamera");
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
          m_cameraUniforms.unjitteredViewProjection = m_cameraUniforms.viewProjection;
          m_cameraUniforms.unjitteredInverseViewProjection = m_cameraUniforms.inverseViewProjection;
          m_cameraUniforms.prevUnjitteredViewProjection = m_cameraUniforms.viewProjection;
          m_cameraUniforms.prevJitteredViewProjection = m_cameraUniforms.viewProjection;
          m_cameraUniforms.cameraPosition = m_camera.getPosition();
          m_cameraUniforms.shadowConstantBias = 0.0f;
          m_cameraUniforms.shadowDirectionAndSlopeBias = glm::vec4(0.0f);
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

      framePhase = "ImGuiVulkanNewFrame";
      {
        demo::profiling::ScopedCpuRange imguiNewFrameRange("AppPreRecord.ImGuiNewFrame");
        TRACY_ZONE_SCOPED("App::ImGuiNewFrame");
        ImGui_ImplVulkan_NewFrame();
        framePhase = "ImGuiGlfwNewFrame";
        ImGui_ImplGlfw_NewFrame();
        framePhase = "ImGuiFrameBegin";
        ImGui::NewFrame();
      }
      framePhase = "RuntimeProfiler";
      {
        demo::profiling::ScopedCpuRange runtimeProfilerRange("AppPreRecord.RuntimeProfiler");
        TRACY_ZONE_SCOPED("App::UpdateRuntimeProfiler");
        if(!m_runtimeProfilerDisabled)
        {
          updateRuntimeProfiler();
        }
      }

      {
        demo::profiling::ScopedCpuRange dockspaceRange("AppPreRecord.ImGuiDockspace");
        TRACY_ZONE_SCOPED("App::Dockspace");
        const ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingInCentralNode;
        ImGuiID dockID = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), dockFlags);
        if(!ImGui::DockBuilderGetNode(dockID)->IsSplitNode() && !ImGui::FindWindowByName("Viewport"))
        {
          ImGui::DockBuilderDockWindow("Viewport", dockID);
          ImGui::DockBuilderGetCentralNode(dockID)->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
          ImGuiID leftID = ImGui::DockBuilderSplitNode(dockID, ImGuiDir_Left, 0.2f, nullptr, &dockID);
          ImGui::DockBuilderDockWindow("Settings", leftID);
        }
      }

      {
        demo::profiling::ScopedCpuRange mainMenuRange("AppPreRecord.ImGuiMainMenu");
        TRACY_ZONE_SCOPED("App::MainMenu");
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
          ImGui::EndMainMenuBar();
        }
      }

      glm::vec4 viewportImageRect{0.0f};
      {
        demo::profiling::ScopedCpuRange viewportPanelRange("AppPreRecord.ViewportPanel");
        TRACY_ZONE_SCOPED("App::ViewportPanel");
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
      }

      if(ImGui::Begin("Settings"))
      {
        demo::profiling::ScopedCpuRange settingsPanelRange("AppPreRecord.SettingsPanel");
        TRACY_ZONE_SCOPED("App::SettingsPanel");
        // Camera coordinates display
        ImGui::Separator();
        ImGui::Text("Camera Position:");
        const glm::vec3& camPos = m_camera.getPosition();
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
            ImGui::SliderFloat("Threshold", &m_debugOptions.bloomThreshold, 0.1f, 8.0f, "%.2f");
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
          TRACY_ZONE_SCOPED("App::CSMDebugPanel");
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
          TRACY_ZONE_SCOPED("App::RuntimeProfilerPanel");
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
          const auto vkFormatLabel = [](VkFormat format) -> const char* {
            switch(format)
            {
              case VK_FORMAT_B8G8R8A8_UNORM:
                return "B8G8R8A8_UNORM";
              case VK_FORMAT_R8G8B8A8_UNORM:
                return "R8G8B8A8_UNORM";
              case VK_FORMAT_R8G8B8A8_SRGB:
                return "R8G8B8A8_SRGB";
              case VK_FORMAT_R16G16B16A16_SFLOAT:
                return "R16G16B16A16_SFLOAT";
              case VK_FORMAT_UNDEFINED:
                return "Undefined";
              default:
                return "Other";
            }
          };
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
            ImGui::Text("Output Format: %s", vkFormatLabel(postDiagnostics.outputFormat));
            ImGui::Text("Scene Color: %s", vkFormatLabel(postDiagnostics.sceneColorFormat));
            ImGui::Text("Recommended HDR: %s", vkFormatLabel(postDiagnostics.recommendedHdrFormat));
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
            ImGui::Text("Grade S/C/G/V: %.2f / %.2f / %.2f / %.2f",
                        postDiagnostics.colorSaturation,
                        postDiagnostics.colorContrast,
                        postDiagnostics.colorGamma,
                        postDiagnostics.vignetteIntensity);
            ImGui::Text("Lens Dirt: %.2f", postDiagnostics.lensDirtIntensity);
            ImGui::Text("Passes E/AE/B/F/Grade/Lens: %s / %s / %s / %s / %s / %s",
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
            ImGui::Text("Format: %s", vkFormatLabel(iblDiagnostics.format));
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
        demo::profiling::ScopedCpuRange modelLoaderUiRange("AppPreRecord.ModelLoaderUI");
        TRACY_ZONE_SCOPED("App::ModelLoaderUI");
        drawModelLoaderUI();
      }
      {
        demo::profiling::ScopedCpuRange sceneGraphUiRange("AppPreRecord.SceneGraphUI");
        TRACY_ZONE_SCOPED("App::SceneGraphUI");
        drawSceneGraphUI();
      }

      framePhase = "Render";
      {
        demo::profiling::ScopedCpuRange buildRenderParamsRange("AppPreRecord.BuildRenderParams");
        TRACY_ZONE_SCOPED("App::BuildRenderParams");
        frameParams.viewportSize   = m_viewportSize;
        frameParams.deltaTime      = ImGui::GetIO().DeltaTime;
        frameParams.timeSeconds    = static_cast<float>(ImGui::GetTime());
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
        // Copy CSM debug settings to debugOptions
        frameParams.debugOptions.showShadowCascades    = m_showShadowCascades;
        frameParams.debugOptions.cascadeIndex          = m_cascadeIndex;
        frameParams.debugOptions.cascadeOverlayMode    = m_cascadeOverlayMode;
        frameParams.debugOptions.cascadeOverlayAlpha   = m_cascadeOverlayAlpha;
        frameParams.recordUi       = [](demo::rhi::CommandList& cmd) {
          demo::profiling::ScopedCpuRange renderImguiDrawDataRange("RecordCommandBuffer.RenderImGuiDrawData");
          TRACY_ZONE_SCOPED("App::RenderImGuiDrawData");
          ImGui::Render();
          ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), demo::rhi::vulkan::getNativeCommandBuffer(cmd));
        };
      }
      }

      const bool freezeRenderingForStreamingUpload = m_isLoading && m_renderer.isSceneRenderingSuspended();
      if(!freezeRenderingForStreamingUpload)
      {
        demo::profiling::ScopedCpuRange rendererFacadeRange("App.RendererFacadeRender");
        TRACY_ZONE_SCOPED("App::RendererFacade::render");
        m_renderer.render(frameParams);
      }

      {
        demo::profiling::ScopedCpuRange imguiEndFrameRange("AppPostRecord.ImGuiEndFrame");
        TRACY_ZONE_SCOPED("App::ImGuiEndFrame");
        ImGui::EndFrame();
      }
      if((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
      {
        demo::profiling::ScopedCpuRange platformWindowsRange("AppPostRecord.ImGuiPlatformWindows");
        TRACY_ZONE_SCOPED("App::ImGuiPlatformWindows");
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
  std::unique_ptr<demo::rhi::vulkan::VulkanSurface> m_surface;
  VkExtent2D                                        m_windowSize{800, 600};
  demo::rhi::Extent2D                               m_viewportSize{800, 600};
  demo::RendererFacade                              m_renderer;
  bool                       m_vSync{false};
  demo::MaterialHandle       m_selectedMaterial{};
  demo::rhi::ClearColorValue m_clearColor{0.2f, 0.2f, 0.3f, 1.0f};

  // glTF model loading
  std::unique_ptr<demo::GltfLoader>               m_gltfLoader;
  std::optional<demo::GltfModel>                  m_sceneModel;
  std::optional<demo::SceneUploadResult>          m_currentModel;
  std::string                                     m_modelPath;
  bool                                            m_modelLoaded = false;

  // Camera
  demo::Camera m_camera;
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

  // CSM Shadow debug settings (copied to debugOptions in run())
  bool  m_showShadowCascades{true};
  int   m_cascadeIndex{-1};              // -1 = all cascades, 0-3 = specific cascade
  bool  m_cascadeOverlayMode{false};
  float m_cascadeOverlayAlpha{0.25f};

  // UI state
  char m_modelPathBuffer[512] = "resources/NV_Bistro/bistro_ktx.gltf";

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
  void drawModelLoaderUI();
  void drawSceneGraphUI();
  void drawSceneNodeTree(int nodeIndex);
  void drawSelectedSceneNodeInspector();
  void applySceneGraphTransforms();
  void updateSceneNodeWorldTransform(int nodeIndex, const glm::mat4& parentTransform);
  void updateAsyncLoading();
  void beginLegacySceneUpload();
  void beginExperimentalSceneUpload();
  void syncLightAnglesFromDirection();
  void syncLightDirectionFromAngles();
  std::vector<demo::SceneLight>* editableSceneLights();
  const std::vector<demo::SceneLight>* currentSceneLights() const;
  glm::mat4 resolveSceneLightNodeTransform(const demo::SceneLight& light) const;
  demo::DirectionalLightSettings resolveSceneLightSettings() const;
  void drawSceneLightsUI();
  void drawCSMDebugPanel();
  void updateRuntimeProfiler();
  void drawRuntimeProfilerPanel();
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

inline glm::mat4 MinimalLatestApp::resolveSceneLightNodeTransform(const demo::SceneLight& light) const
{
  if(light.nodeIndex >= 0)
  {
    const size_t nodeIndex = static_cast<size_t>(light.nodeIndex);
    if(m_activeSceneLoadPath == SceneLoadPath::experimentalSceneUploadPlan && m_sceneAsset.has_value()
       && nodeIndex < m_sceneAsset->nodes.size())
    {
      return m_sceneAsset->nodes[nodeIndex].worldTransform;
    }
    if(m_sceneModel.has_value() && nodeIndex < m_sceneModel->nodes.size())
    {
      return m_sceneModel->nodes[nodeIndex].worldTransform;
    }
  }
  return glm::mat4(1.0f);
}

inline demo::DirectionalLightSettings MinimalLatestApp::resolveSceneLightSettings() const
{
  demo::DirectionalLightSettings settings = m_lightSettings;
  settings.color = glm::vec3(0.0f);
  settings.ambient = glm::vec3(0.0f);

  if(m_enableTestDirectionalLight)
  {
    settings.direction = m_lightSettings.direction;
    settings.color = m_testDirectionalLightColor * m_testDirectionalLightIntensity;
    return settings;
  }

  const std::vector<demo::SceneLight>* lights = currentSceneLights();
  if(lights == nullptr)
  {
    return settings;
  }

  for(const demo::SceneLight& light : *lights)
  {
    if(!light.enabled || light.type != demo::SceneLightType::directional)
    {
      continue;
    }

    const glm::mat4 worldTransform = resolveSceneLightNodeTransform(light);
    glm::vec3 direction = glm::vec3(worldTransform * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
    if(glm::length(direction) < 0.001f)
    {
      direction = m_lightSettings.direction;
    }
    settings.direction = glm::normalize(direction);
    settings.color = light.color * light.intensity;
    return settings;
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
        m_renderer.executeUploadCommand([this, batch](VkCommandBuffer cmd) {
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
      m_renderer.executeUploadCommand([this](VkCommandBuffer cmd) {
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
  m_currentModel.emplace();
  m_renderer.initializeGltfUploadResult(*m_sceneModel, *m_currentModel);
  m_asyncLoadingCoordinator.emplace();
  m_asyncLoadingCoordinator->begin(*m_sceneModel, m_camera.getPosition(), 24, 96);
}

inline void MinimalLatestApp::beginExperimentalSceneUpload()
{
  ASSERT(m_renderer.getBackend() == demo::RendererBackend::gpuDriven,
         "Experimental scene upload is currently only supported by GPUDrivenRenderer");
  ASSERT(m_sceneAsset.has_value(), "Experimental scene upload requires a SceneAsset");
  ASSERT(m_sceneAssetView.has_value(), "Experimental scene upload requires a SceneAssetView");
  ASSERT(m_sceneUploadPlan.has_value(), "Experimental scene upload requires a SceneUploadPlan");

  m_activeSceneLoadPath = SceneLoadPath::experimentalSceneUploadPlan;
  m_loadStatus = "Scheduling SceneUploadPlan...";
  m_loadProgress = 0.4f;
  m_currentModel.emplace();
  m_asyncLoadingCoordinator.emplace();
  m_asyncLoadingCoordinator->begin(*m_sceneAssetView, *m_sceneUploadPlan, m_camera.getPosition(), 24, 96);
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
