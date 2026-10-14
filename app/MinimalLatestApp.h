#pragma once

#include "../common/Common.h"
#include "../common/FrameDeferredValue.h"
#include "../common/ProfilerMarkers.h"

#include "GlfwSdl3Compat.h"
#include <backends/imgui_impl_sdl3.h>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#if defined(_MSC_VER)
#pragma comment(lib, "Comdlg32.lib")
#endif
#endif

#include "editor/EditorController.h"
#include "editor/EditorInputRouter.h"
#include "editor/EditorNativeGizmoInput.h"
#include "editor/EditorRendererAdapter.h"
#include "editor/EditorViewportRuntime.h"
#include "editor/ImGuiClipboardAdapter.h"
#include "editor/AutomationAssetIdentity.h"
#include "editor/EditorTransactionSmoke.h"
#if defined(MGIF_EDITOR_LIVE_REPLAY)
#include "editor/EditorTestDriver.h"
#endif
#if defined(MGIF_EDITOR_OS_REPLAY)
#include "editor/EditorUiOsReplayHost.h"
#include <mgif/scene_editor/SceneChangeCanonicalizer.h>
#endif
#include "../render/RendererFacade.h"
#include "../rhi/RHISurface.h"
#include "../render/Camera.h"
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/compatibility.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include "../render/SceneViewResolver.h"
#include "../scene/SceneAsset.h"
#include "../scene/SceneUploadPlanner.h"
#include "../third_party/LegitProfiler/ImGuiProfilerRenderer.h"
#include <mgif/scene_editor/GltfSemanticCompare.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_glfw.h>

#include <memory>
#include <optional>
#include <array>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
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
    demo::rhi::BackendType backend{demo::rhi::defaultBackend()};
    AutomationMode mode{AutomationMode::none};
    float fixedDeltaSeconds{1.0f / 60.0f};
    uint32_t warmupFrames{60u};
    uint32_t motionFrames{60u};
    uint32_t holdFrames{60u};
    bool noUi{false};
    bool noPost{false};
    bool noDdgi{false};
    bool ddgiSmoke{false};
    bool taa{false};
    bool autoExit{false};
    bool captureControlFrame{false};
    std::filesystem::path captureSyncDirectory{};
    std::filesystem::path metricsOutputPath{};
    // Normal production-editor startup controls. Automation paths retain
    // their fixture selection; these options make Demo a direct editor entry.
    bool editorEmptyStart{false};
    std::filesystem::path editorStartupScene{};
    std::filesystem::path editorTransactionDirectory{};
    // Test-only, Windows desktop visual evidence controls.  Empty values keep
    // the production automation path byte-for-byte compatible.  
    std::string editorVisualCase{};
    std::filesystem::path editorVisualStatePath{};
    std::filesystem::path editorVisualSyncDirectory{};
    std::string editorVisualVariant{};   
    std::optional<std::uint32_t> editorVisualComponent{};
    float editorVisualDpi{1.0f};
    // Pinned UI matrix DPI axis for the live acceptance target.  The host
    // monitor scale is fixed and SetProcessDpiAwarenessContext fails from a
    // child process, so the native window content scale cannot be varied.
    // Dear ImGui lays out in logical pixels and scales text plus auto-sized
    // windows by io.FontGlobalScale, which is the observable UI-scale lever
    // available here.  The applied value is read back and published in the
    // live state as a measured fact rather than assumed.
    float editorLiveUiDpiScale{1.0f};
    std::string editorVisualExecutableSha256{};
    std::string editorVisualSourceSha256{};
    // Test-only, Windows desktop visual evidence control.  A decorated window
    // cannot present a client area as tall as the host virtual screen because
    // the default WM_GETMINMAXINFO maximum track size is the virtual screen and
    // the non-client frame consumes part of it (measured 2560x1421 on a
    // 2560x1440 host).  The pinned resolution axis includes 2560x1440, so the
    // visual harness asks for an undecorated window to reach that exact client
    // extent.  Default false keeps every production window decorated.
    bool windowUndecorated{false};
#if defined(MGIF_EDITOR_LIVE_REPLAY)
    std::string editorLiveCase{};
    std::filesystem::path editorLiveEvidenceDirectory{};
    std::filesystem::path editorLiveSyncDirectory{};
    std::string editorLiveExecutableSha256{};
    std::string editorLiveSourceSha256{};
#endif
#if defined(MGIF_EDITOR_OS_REPLAY)
    demo::editor::testing::osreplay::CliOptions editorOsReplay{};
#endif
    uint32_t captureSyncTimeoutMilliseconds{30000u};
  };

  MinimalLatestApp(demo::rhi::Extent2D size = {1920, 1080})
      : MinimalLatestApp(size, AutomationOptions{})
  {
  }

  // Keep the explicit value-initialized form visible for the smoke contract:
  // AutomationOptions automationOptions = {}
  MinimalLatestApp(demo::rhi::Extent2D size, AutomationOptions automationOptions)
      : m_windowSize(size)
	  , m_viewportSize(size)
      , m_renderer(automationOptions.backend)
      , m_editorRendererAdapter(automationOptions.backend)
      , m_automationOptions(automationOptions)
      , m_editorTransactionSmoke(
            !automationOptions.editorTransactionDirectory.empty())
  {
    configureP6NativeInteraction();
    configureEditorVisualInteraction();
#if defined(MGIF_EDITOR_LIVE_REPLAY)
    configureEditorLiveReplay();
#endif
#if defined(MGIF_EDITOR_OS_REPLAY)
    configureEditorOsReplay();
#endif
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    if(m_automationOptions.windowUndecorated)
    {
      glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    }
#ifdef USE_SLANG
    const char* windowTitle = m_automationOptions.mode == AutomationMode::none
                                  ? "MGIF Scene Editor (Slang)"
                                  : "Minimal Demo (Slang)";
#else
    const char* windowTitle = m_automationOptions.mode == AutomationMode::none
                                  ? "MGIF Scene Editor (GLSL)"
                                  : "Minimal Demo (GLSL)";
#endif
    m_window  = glfwCreateWindow(m_windowSize.width, m_windowSize.height, windowTitle, nullptr, nullptr);
    // ImGui installs its callbacks later and chains these main-window observers.
    glfwSetWindowUserPointer(m_window, this);
    glfwSetKeyCallback(m_window, [](GLFWwindow* window, int key, int, int action, int) {
      auto* app = static_cast<MinimalLatestApp*>(glfwGetWindowUserPointer(window));
      if(app && key == GLFW_KEY_ESCAPE && action != GLFW_REPEAT)
        app->m_editorNativeGizmoInput.observeEscape(
            action == GLFW_PRESS, app->nativeGizmoGestureActive());
    });
    glfwSetMouseButtonCallback(m_window, [](GLFWwindow* window, int button, int action, int) {
      auto* app = static_cast<MinimalLatestApp*>(glfwGetWindowUserPointer(window));
      if(app && button == GLFW_MOUSE_BUTTON_LEFT)
      {
        if(action == GLFW_PRESS && app->m_editorFocusReleaseFence)
          app->m_editorPrimaryPressAfterFocus = true;
        app->m_editorNativeGizmoInput.observePrimaryButton(
            action == GLFW_PRESS, app->nativeGizmoGestureActive());
      }
    });
    glfwSetWindowFocusCallback(m_window, [](GLFWwindow* window, int focused) {
      auto* app = static_cast<MinimalLatestApp*>(glfwGetWindowUserPointer(window));
      if(app)
      {
        app->m_editorNativeGizmoInput.observeFocus(
            focused == GLFW_TRUE, app->nativeGizmoGestureActive());
        if(focused != GLFW_TRUE && app->m_editorController)
          app->m_editorController->discardViewportEscapeHandling();
      }
    });
    glfwSetDropCallback(m_window, [](GLFWwindow* window, int count,
                                     const char** paths) {
      auto* app = static_cast<MinimalLatestApp*>(
          glfwGetWindowUserPointer(window));
      if(app == nullptr || count <= 0 || paths == nullptr || paths[0] == nullptr)
      {
        return;
      }
      const bool merge = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                         glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
      app->handleDroppedScene(std::filesystem::path(paths[0]), merge);
    });
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(m_window, &framebufferWidth, &framebufferHeight);
    if(framebufferWidth > 0 && framebufferHeight > 0)
    {
      m_windowSize = {static_cast<uint32_t>(framebufferWidth),
                      static_cast<uint32_t>(framebufferHeight)};
      m_viewportSize = m_windowSize;
    }
    m_surface = m_renderer.createSurface();
    m_renderer.init(m_window, *m_surface, m_vSync);
    // The app layer pumps SDL events through glfwPollEvents() (see
    // app/GlfwSdl3Compat.h). Forward every event to the ImGui SDL3 backend
    // before the compat layer dispatches it to the editor's own callbacks.
    mgif_sdl::eventSink = [](const SDL_Event* event) {
      if (ImGui::GetCurrentContext() != nullptr)
      {
        ImGui_ImplSDL3_ProcessEvent(event);
      }
    };
    if(m_editorVisualEnabled
#if defined(MGIF_EDITOR_LIVE_REPLAY)
       || m_editorLiveEnabled
#endif
#if defined(MGIF_EDITOR_OS_REPLAY)
       || m_editorOsReplayEnabled
#endif
    )
    {
      ImGui::GetIO().IniFilename = nullptr;
    }
    m_editorRendererAdapter.bindRendererIdentity(
        m_renderer.editorRendererIdentity(), m_renderer.editorDeviceIdentity());
    m_selectedMaterial = m_renderer.getMaterialHandle(0);
    const demo::clipspace::BackendConvention projectionBackend =
        m_automationOptions.backend == demo::rhi::BackendType::d3d12
            ? demo::clipspace::BackendConvention::d3d12
            : m_automationOptions.backend == demo::rhi::BackendType::metal
                ? demo::clipspace::BackendConvention::metal
                : demo::clipspace::BackendConvention::vulkan;
    m_camera.setClipSpaceConvention(projectionBackend);
    m_sceneCameraNavigation.setClipSpaceConvention(projectionBackend);

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
#if defined(MGIF_EDITOR_LIVE_REPLAY)
    if(m_editorLiveEnabled)
    {
      ImGui::GetIO().FontGlobalScale =
          m_automationOptions.editorLiveUiDpiScale;
    }
#endif

    demo::editor::ui::EditorController::Callbacks editorCallbacks;
    editorCallbacks.onPublication =
        [this](const demo::editor::ui::EditorController::ProjectionPublication& publication) {
          if(m_editorTransactionSmoke.enabled()
             && m_automationSceneReadyObserved)
          {
            ++m_editorTransactionPublicationCount;
          }
          std::string error;
          if(!m_editorRendererAdapter.enqueue(publication, &error))
          {
            if(m_editorTransactionSmoke.enabled())
            {
              m_editorTransactionEnqueueError = error.empty()
                  ? "Editor publication enqueue failed"
                  : error;
            }
            reportEditorError(error);
          }
        };
    editorCallbacks.onError = [this](std::string_view message) {
      reportEditorError(message);
    };
    editorCallbacks.fileDialog =
        [this](const demo::editor::ui::NativeDialogRequest& request) {
          return showEditorFileDialog(request);
        };
    editorCallbacks.clipboard = &m_editorClipboard;
    editorCallbacks.cameraRequest =
        [this](const demo::editor::ui::CameraRequest& request) {
          return handleEditorCameraRequest(request);
        };
    editorCallbacks.requestExit = [this]() {
#if defined(MGIF_EDITOR_LIVE_REPLAY)
      if(m_editorLiveEnabled)
      {
        // A scripted exit command is an acceptance assertion, not a teardown
        // request. Closing the window here would end the run before the
        // terminal screenshot boundary and its capture handshake, so the
        // physical close is deferred until the marker stage completes.
        m_editorLiveExitRequested = true;
        LOGI("[EDITOR_LIVE_REPLAY] marker=exit-deferred case=%s frame=%llu",
             m_automationOptions.editorLiveCase.c_str(),
             static_cast<unsigned long long>(m_automationFrame));
        return;
      }
#endif
      glfwSetWindowShouldClose(m_window, true);
    };
    editorCallbacks.onDocumentReset = [this]() {
      // New/unload is a renderer lifecycle boundary, not an empty scene
      // publication. Clear the adapter fence and its immutable app snapshot.
      resetPublishedEditorScene();
      m_editorForceFreeCamera = false;
      m_editorDocumentCameraIndex.reset();
      m_editorViewportFocused = false;
      if(m_editorViewportRuntime != nullptr)
      {
        m_editorViewportRuntime->updateSceneSnapshot(
            nullptr, demo::editor::SceneSelectionValue{}, 0u);
      }
    };
    editorCallbacks.viewportControlsAvailable = [this]() noexcept {
      return m_editorViewportRuntime != nullptr &&
             m_editorViewportRuntime->ready();
    };
    editorCallbacks.rendererBusy = [this]() noexcept {
      const bool awaitingInitialPublication =
          m_editorController && m_editorController->hasDocument() &&
          !m_projectedSceneAsset;
      return m_editorRendererAdapter.hasPending() ||
             m_editorRendererAdapter.requiresAuthoritativeRebuild() ||
             awaitingInitialPublication;
    };
    editorCallbacks.rendererPublicationFailed = [this]() noexcept {
      return m_editorRendererAdapter.requiresAuthoritativeRebuild();
    };
    m_editorController =
        std::make_unique<demo::editor::ui::EditorController>(std::move(editorCallbacks));

    demo::editor::ui::EditorViewportRuntime::Callbacks viewportCallbacks;
    viewportCallbacks.applySelection = [this](
        const demo::editor::EditorSelectionEffect& effect) {
      const demo::editor::ui::RuntimeActionResult result =
          m_editorController
              ? m_editorController->applyPickingSelection(effect)
              : demo::editor::ui::RuntimeActionResult{
                    false, demo::editor::ui::ProjectionRefresh::none,
                    "Editor controller is unavailable"};
      if(m_p6NativeEnabled && result.accepted)
      {
        ++m_p6NativeSelectionEffects;
      }
      return result;
    };
    viewportCallbacks.applyCameraFocus = [this](
        const demo::editor::EditorCameraFocusEffect& effect) {
      if(!effect.valid() || effect.expectedViewRevision != m_editorViewRevision)
      {
        return false;
      }
      demo::Camera& camera = hasActiveSceneCamera()
                               ? m_sceneCameraNavigation : m_camera;
      const glm::vec3 eye = camera.getPosition();
      const glm::vec3 center(static_cast<float>(effect.center.x),
                             static_cast<float>(effect.center.y),
                             static_cast<float>(effect.center.z));
      const glm::vec3 offset = center - eye;
      const float length = glm::length(offset);
      if(!std::isfinite(length) || length <= 0.0f)
      {
        return false;
      }
      const glm::vec3 direction = offset / length;
      const float yaw = glm::degrees(std::atan2(direction.z, direction.x));
      const float pitch = glm::degrees(std::asin(
          glm::clamp(direction.y, -1.0f, 1.0f)));
      camera.setYawPitch(yaw, pitch);
      camera.update();
      m_moveSpeed = static_cast<float>(effect.navigationSpeed);
      m_editorViewRevision = effect.nextViewRevision;
      m_editorLastViewMatrix = camera.getViewMatrix();
      m_editorLastCameraProjectionMatrix = camera.getProjectionMatrix();
      m_editorLastViewValid = true;
      refreshActiveCameraUniforms();
      if(m_p6NativeEnabled)
      {
        ++m_p6NativeCameraEffects;
      }
      return true;
    };
	viewportCallbacks.pickPublication = [this]() {
	  return m_renderer.editorPickPublication();
    };
    viewportCallbacks.submitPick = [this](
        const demo::editor::EditorPickRequest& request) {
      const demo::editor::PickSubmitResult result =
          m_renderer.enqueueEditorPick(request);
      if(m_p6NativeEnabled && result.accepted())
      {
        ++m_p6NativeSubmitted;
        m_p6NativeLastRequest = request;
      }
      return result;
    };
    viewportCallbacks.pollPickResults = [this](
        std::vector<demo::editor::EditorPickResult>& results) {
      const std::size_t before = results.size();
      m_renderer.pollEditorPickResults(results);
      if(m_p6NativeEnabled)
      {
        for(std::size_t index = before; index < results.size(); ++index)
        {
          m_p6NativeTerminals.push_back(results[index]);
        }
      }
    };
    viewportCallbacks.cancelPicks = [this]() {
      m_renderer.cancelEditorPicks();
    };
    viewportCallbacks.rendererBusy = [this]() {
      const bool awaitingInitialPublication =
          m_editorController && m_editorController->hasDocument() &&
          !m_projectedSceneAsset;
      return m_editorRendererAdapter.hasPending() ||
             m_editorRendererAdapter.requiresAuthoritativeRebuild() ||
             awaitingInitialPublication ||
             m_renderer.editorPickingBusy();
    };
	viewportCallbacks.resolveOccurrence = [this](
		const demo::editor::EditorPickFence& fence,
		std::uint64_t occurrenceGeneration,
		demo::editor::RenderOccurrenceId occurrence) {
	  return m_renderer.editorResolveOccurrence(
		  fence, occurrenceGeneration, occurrence);
	};
	viewportCallbacks.resolveOccurrenceKey = [this](
		const demo::editor::EditorPickFence& fence,
		std::uint64_t occurrenceGeneration,
		demo::editor::PrimitiveOccurrenceKey occurrence) {
	  return m_renderer.editorResolveOccurrence(
		  fence, occurrenceGeneration, occurrence);
	};
	viewportCallbacks.nodeHighlightEligible = [this](
		const demo::editor::EditorPickFence& fence,
		std::uint64_t occurrenceGeneration, demo::editor::NodeId node) {
	  return m_renderer.editorNodeHighlightEligible(
		  fence, occurrenceGeneration, node);
    };
    viewportCallbacks.bindSelectionOccurrences = [this](
        const demo::editor::ISceneSelectionOccurrenceLookup* occurrences) {
      if(m_editorController)
      {
        m_editorController->setSelectionOccurrenceLookup(occurrences);
      }
    };
    viewportCallbacks.selectionValue = [this]() {
      return m_editorController
               ? m_editorController->sceneSelection().value()
               : demo::editor::SceneSelectionValue{};
    };
    viewportCallbacks.selectionRevision = [this]() {
      return m_editorController ? m_editorController->selectionRevision() : 0u;
    };
    viewportCallbacks.drainSelectionChanges = [this]() {
      return m_editorController
               ? m_editorController->drainSelectionChanges()
               : std::vector<demo::editor::SelectionChanged>{};
    };
    viewportCallbacks.browserFocus = [this](
        const demo::editor::SceneBrowserFocusRequest& request) {
      if(m_p6NativeEnabled)
      {
        ++m_p6NativeBrowserFocusEvents;
      }
      if(!m_editorController || request.selectTab) return;
      demo::editor::EditorObjectId selected{};
      std::optional<demo::editor::NodeId> selectedNode;
      bool primitiveTarget = false;
      std::visit([&](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr(std::is_same_v<Value, demo::editor::PrimitiveOccurrenceKey>)
        {
          selected = value.primitive.untyped();
          selectedNode = value.node;
          primitiveTarget = true;
        }
        else if constexpr(!std::is_same_v<Value, std::monostate>)
        {
          selected = value.untyped();
          if constexpr(std::is_same_v<Value, demo::editor::NodeId>)
          {
            selectedNode = value;
          }
        }
      }, request.target);
      std::vector<demo::editor::EditorObjectId> parentPath;
      std::optional<demo::editor::SceneId> targetScene;
      std::optional<demo::editor::MeshId> targetMesh;
      std::optional<demo::editor::ui::GraphRowKey> exactRow;
      const demo::editor::SceneProjectionResult* projection =
          m_editorController->projection();
      if(const auto* primitive =
             std::get_if<demo::editor::PrimitiveOccurrenceKey>(&request.target))
      {
        targetScene = primitive->scene;
        exactRow = demo::editor::ui::GraphRowKey{
            primitive->scene, primitive->primitive.untyped(), primitive->node,
            primitive->instanceOrdinal};
        if(projection)
        {
          const auto occurrence = std::find_if(
              projection->primitiveOccurrences.begin(),
              projection->primitiveOccurrences.end(),
              [&](const demo::editor::SceneProjectionPrimitiveOccurrence& value) {
                return value.key == *primitive;
              });
          if(occurrence != projection->primitiveOccurrences.end() &&
             occurrence->scenePrimitiveIndex < projection->asset.primitives.size())
          {
            const std::uint32_t documentMeshIndex =
                projection->asset.primitives[occurrence->scenePrimitiveIndex]
                    .documentMeshIndex;
            const auto mesh = projection->mapping.documentIdAt(
                demo::editor::EditorObjectKind::mesh, documentMeshIndex);
            targetMesh = mesh ? demo::editor::MeshId::from(*mesh)
                              : std::nullopt;
          }
        }
      }
      else if(projection && projection->activeScene)
      {
        targetScene = demo::editor::SceneId::from(
            demo::editor::editorObjectId(*projection->activeScene));
      }
      if(request.revealParents && selectedNode && projection)
      {
        // Resolve the parent chain against the clicked scene occurrence. The
        // active projection only contains one scene and cannot disambiguate a
        // shared node appearing in another scene row.
        const std::vector<demo::editor::NodeId> reverseParents =
            targetScene
                ? m_editorController->sceneNodeParentPath(*targetScene,
                                                           *selectedNode)
                : std::vector<demo::editor::NodeId>{};
        parentPath.reserve(reverseParents.size() +
                           (targetScene ? 1u : 0u) +
                           (targetMesh ? 1u : 0u) +
                           (primitiveTarget ? 1u : 0u));
        if(targetScene) parentPath.push_back(targetScene->untyped());
        for(const demo::editor::NodeId node : reverseParents)
        {
          parentPath.push_back(node.untyped());
        }
        if(primitiveTarget) parentPath.push_back(selectedNode->untyped());
        if(targetMesh) parentPath.push_back(targetMesh->untyped());
      }
      else if(request.revealParents && targetScene)
      {
        parentPath.push_back(targetScene->untyped());
      }
      m_editorController->ui().requestFocus(
          request, parentPath, selected, exactRow);
    };
    viewportCallbacks.setHighlight = [this](
        const demo::editor::SceneSelectionValue& selection,
        demo::editor::SelectionRevision revision) {
      if(m_p6NativeEnabled)
      {
        ++m_p6NativeHighlightUpdates;
      }
      (void)m_renderer.setEditorHighlight(selection, revision);
    };
    viewportCallbacks.getTransform = [this](demo::editor::NodeId node) {
      return m_editorController
                 ? m_editorController->viewportNodeTransform(node)
                 : std::optional<demo::editor::NodeTransform>{};
    };
    viewportCallbacks.transform = [this](demo::editor::NodeId node,
                                         demo::editor::ui::EditPhase phase,
                                         const demo::editor::NodeTransform& value) {
      if (!m_editorController) {
        return demo::editor::ui::RuntimeActionResult{
            false, demo::editor::ui::ProjectionRefresh::none,
            "Editor controller is unavailable"};
      }
      if (phase == demo::editor::ui::EditPhase::begin) {
        return m_editorController->beginGizmoTransform(node);
      }
      return m_editorController->previewGizmoTransform(node, value);
    };
    viewportCallbacks.finishTransform = [this](demo::editor::NodeId node,
                                                bool physicalRelease,
                                                bool escapePressed,
                                                bool focusLost) {
      return m_editorController
                 ? m_editorController->finishGizmoTransform(
                       node, physicalRelease, escapePressed, focusLost)
                 : demo::editor::ui::RuntimeActionResult{
                       false, demo::editor::ui::ProjectionRefresh::none,
                       "Editor controller is unavailable"};
    };
    viewportCallbacks.error = [this](std::string_view message) {
      reportEditorError(message);
    };
    m_editorViewportRuntime =
        std::make_unique<demo::editor::ui::EditorViewportRuntime>(
            std::move(viewportCallbacks));

    demo::editor::ui::DesktopGizmoCallbacks gizmoCallbacks;
    gizmoCallbacks.beginTransform = [this](demo::editor::NodeId node) {
      return m_editorController &&
             m_editorController->beginGizmoTransform(node).accepted;
    };
    gizmoCallbacks.previewTransform =
        [this](demo::editor::NodeId node,
               const demo::editor::GizmoLocalTransform& value) {
          if(!m_editorController) return false;
          demo::editor::NodeTransform transform;
          transform.translation = {value.translation.x, value.translation.y,
                                  value.translation.z};
          transform.rotation = {value.rotation.x, value.rotation.y,
                                value.rotation.z, value.rotation.w};
          transform.scale = {value.scale.x, value.scale.y, value.scale.z};
          return m_editorController->previewGizmoTransform(node, transform)
              .accepted;
        };
    gizmoCallbacks.finishTransform =
        [this](demo::editor::NodeId node,
               demo::editor::ui::DesktopGizmoTerminal terminal) {
          if(!m_editorController) return false;
          demo::editor::ui::RuntimeActionResult result;
          switch(terminal)
          {
          case demo::editor::ui::DesktopGizmoTerminal::commit:
            result = m_editorController->finishGizmoTransform(node, true, false,
                                                               false);
            break;
          case demo::editor::ui::DesktopGizmoTerminal::detachRetainingPreview:
            result = m_editorController->finishGizmoTransform(node, false, true,
                                                               false);
            break;
          case demo::editor::ui::DesktopGizmoTerminal::latchRetainingPreview:
            result = m_editorController->finishGizmoTransform(node, false, false,
                                                               true);
            break;
          }
          return result.accepted;
        };
    gizmoCallbacks.clearSelection = [this]() {
      if(m_editorController)
      {
        (void)m_editorController->applyViewportSelection(
            demo::editor::EditorSelectionSnapshot{}, false);
      }
    };
    m_editorGizmoAdapter =
        std::make_unique<demo::editor::ui::DesktopGizmoIntegrationAdapter>(
            std::move(gizmoCallbacks));

#if defined(MGIF_EDITOR_OS_REPLAY)
    initializeEditorOsReplay();
#endif

    // Load Sponza by default for FlaxGI smoke testing
    std::string path;
#if defined(MGIF_EDITOR_OS_REPLAY)
    if(m_editorOsReplayEnabled)
    {
      path = m_editorOsReplayFixturePath.string();
    }
    else
#endif
#if defined(MGIF_EDITOR_LIVE_REPLAY)
    if(m_editorLiveEnabled)
    {
      // U01 must reach its first frame without a document; the scripted
      // New/Open steps are the only real document transitions in that case.
      path = m_editorLiveEmptyStart ? std::string{}
                                    : m_editorLiveFixturePath.string();
    }
    else
#endif
    if(m_editorVisualEnabled)
    {
      path = m_editorVisualFixturePath.string();
    }
    else if(m_p6NativeEnabled)
    {
      path = m_p6NativeFixturePath.string();
    }
    else if(m_automationOptions.editorEmptyStart)
    {
      path.clear();
    }
    else if(!m_automationOptions.editorStartupScene.empty())
    {
      path = m_automationOptions.editorStartupScene.string();
    }
    else
    {
      path = "resources/GLTF_Sponza/sponza.gltf";
    }
    LOGI("[EDITOR_STARTUP] empty=%d scene=%s selected_path=%s",
         m_automationOptions.editorEmptyStart ? 1 : 0,
         m_automationOptions.editorStartupScene.string().c_str(),
         path.c_str());
    if(!path.empty())
    {
      setMeshSDFPathFromModelPath(path);
    }
    m_autoLoadSDFOnSceneReady = !m_automationOptions.noDdgi &&
                                !m_p6NativeEnabled &&
                                !m_editorVisualEnabled
#if defined(MGIF_EDITOR_LIVE_REPLAY)
                                && !m_editorLiveEnabled
#endif
#if defined(MGIF_EDITOR_OS_REPLAY)
                                && !m_editorOsReplayEnabled
#endif
                                ;
    if(m_automationOptions.noDdgi)
    {
      m_renderer.setDDGIEnabled(false);
    }
    if(!path.empty())
    {
      openModelWithEditor(path);
    }

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
    // SceneEditorUi is constructed after the ImGui context and must be
    // destroyed before the renderer tears that context down.
    m_editorViewportRuntime.reset();
    m_editorController.reset();
    resetPublishedEditorScene();
    m_renderer.shutdown(*m_surface);
    glfwDestroyWindow(m_window);
  }

  void run()
  {
    while(!glfwWindowShouldClose(m_window))
    {
      runFrame();
    }
  }

private:
#if defined(_MSC_VER)
  // Keep the per-frame call visible to native CPU profilers in optimized builds.
  __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
  __attribute__((noinline))
#endif
  void runFrame()
  {
      // Keep the frame marker at the function boundary for optional native
      // profiler integrations; the function call itself is the frame boundary.
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

      // Publish controller-prepared candidates at the frame boundary. The
      // previously accepted renderer scene remains active until this succeeds.
      framePhase = "PublishEditorProjection";
      {
        demo::profiling::ScopedCpuRange publicationRange(
            "AppPreRecord.PublishEditorProjection");
        publishPendingEditorProjection();
      }

      // Auto-load SDF after scene is fully uploaded (smoke test convenience)
      if (m_autoLoadSDFOnSceneReady && m_projectedSceneAsset &&
          m_editorRendererAdapter.initialized() &&
          !m_editorRendererAdapter.hasPending() && !m_meshSDFLoaded)
      {
        loadMeshSDFForDDGI();
        m_autoLoadSDFOnSceneReady = false;
      }

      }

      if(glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) == GLFW_TRUE)
      {
        SDL_Delay(10);
        return;
      }

      framePhase = "EditorTransactionAutomation";
      advanceEditorTransaction();
#if defined(MGIF_EDITOR_LIVE_REPLAY)
      framePhase = "EditorLiveReplay";
      if(m_editorLiveEmptyStart)
      {
        // An empty-start case never receives a scene-ready publication before
        // its scripted Open, so the frame loop owns script initialization.
        initializeEditorLiveReplay();
      }
      advanceEditorLiveReplay();
#endif

      renderFrame([&] {
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
      demo::editor::ui::LayoutState editorLayout{};
      {
        demo::profiling::ScopedCpuRange dockspaceRange("AppPreRecord.ImGuiDockspace");
        const ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingInCentralNode;
        ImGuiID dockID = mainDockspaceID = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), dockFlags);
        if(m_editorVisualEnabled && !m_editorVisualDockReset)
        {
          ImGui::DockBuilderRemoveNodeChildNodes(dockID);
          m_editorVisualDockReset = true;
        }
        ImGuiDockNode* rootDockNode = ImGui::DockBuilderGetNode(dockID);
        if(rootDockNode != nullptr && !rootDockNode->IsSplitNode())
        {
          ImGuiID leftID = ImGui::DockBuilderSplitNode(dockID, ImGuiDir_Left, 0.2f, nullptr, &dockID);
          editorLayout.sceneBrowserDockId = leftID;
          ImGui::DockBuilderDockWindow("Settings", leftID);
          ImGui::DockBuilderDockWindow("Scene Browser", leftID);
          ImGuiID rightID = ImGui::DockBuilderSplitNode(dockID, ImGuiDir_Right, 0.28f, nullptr, &dockID);
          editorLayout.inspectorDockId = rightID;
          ImGui::DockBuilderDockWindow("Inspector", rightID);
          if(ImGuiDockNode* centralNode = ImGui::DockBuilderGetNode(dockID))
          {
            centralNode->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
          }
          ImGui::DockBuilderFinish(mainDockspaceID);
        }
      }

      ImGuiViewport* editorMainViewport = ImGui::GetMainViewport();
      ImVec2 editorViewportMin = editorMainViewport->WorkPos;
      ImVec2 editorViewportMax(
          editorMainViewport->WorkPos.x + editorMainViewport->WorkSize.x,
          editorMainViewport->WorkPos.y + editorMainViewport->WorkSize.y);
      if(const ImGuiDockNode* centralNode =
             ImGui::DockBuilderGetCentralNode(mainDockspaceID);
         centralNode != nullptr && centralNode->Size.x > 0.0f &&
         centralNode->Size.y > 0.0f)
      {
        editorViewportMin = centralNode->Pos;
        editorViewportMax = ImVec2(centralNode->Pos.x + centralNode->Size.x,
                                   centralNode->Pos.y + centralNode->Size.y);
      }
      if(m_automationOptions.noUi)
      {
        editorViewportMin = ImVec2(0.0f, 0.0f);
        editorViewportMax = ImGui::GetIO().DisplaySize;
      }

      if(m_editorController)
      {
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 mouse = io.MousePos;
        // Dear ImGui uses (-FLT_MAX, -FLT_MAX) as its "no mouse" sentinel.
        // Do not let that sentinel participate in hover/focus arbitration or
        // turn a stale click edge into viewport ownership.
        constexpr float imguiMousePosLimit =
            0.5f * (std::numeric_limits<float>::max)();
        const bool imguiMousePosValid =
            ImGui::IsMousePosValid(&mouse) &&
            std::isfinite(mouse.x) && std::isfinite(mouse.y) &&
            std::abs(mouse.x) < imguiMousePosLimit &&
            std::abs(mouse.y) < imguiMousePosLimit;
        const bool insideCentralViewport =
            imguiMousePosValid &&
            mouse.x >= editorViewportMin.x && mouse.y >= editorViewportMin.y &&
            mouse.x < editorViewportMax.x && mouse.y < editorViewportMax.y;
        const bool popupActive =
            ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        const bool viewportHovered = insideCentralViewport &&
                                     (m_editorVisualEnabled ||
                                      !io.WantCaptureMouse) && !popupActive;
        const bool pointerPressed =
            imguiMousePosValid &&
            (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
             ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
             ImGui::IsMouseClicked(ImGuiMouseButton_Middle));
        if(!m_editorVisualEnabled &&
           glfwGetWindowAttrib(m_window, GLFW_FOCUSED) != GLFW_TRUE)
        {
          m_editorViewportFocused = false;
        }
        else if(pointerPressed)
        {
          m_editorViewportFocused = viewportHovered;
        }
        m_editorController->setViewportInteraction(
            viewportHovered, m_editorViewportFocused);
      }

      if(m_editorController)
      {
        const auto animationTick = isEditorTransactionEnabled()
            ? demo::editor::ui::RuntimeActionResult{true,
                demo::editor::ui::ProjectionRefresh::none, {}}
            : m_editorController->updateAnimation(
                static_cast<double>(std::max(ImGui::GetIO().DeltaTime, 0.0f)));
        if(!animationTick.accepted && !animationTick.error.empty())
        {
          reportEditorError(animationTick.error);
        }
        // Animation publications are self-generated by the controller. Publish
        // them before building the UI model so Play/Pause never disables itself
        // behind the adapter's pending flag.
        publishPendingEditorProjection();
      }

      // Camera navigation must settle before the desktop adapter captures its
      // generation-bound view, depth, and inverse view-projection snapshot.
      updateCameraInput();
      if(m_editorController)
      {
        updateEditorViewportRuntime(editorViewportMin, editorViewportMax);
      }

      if(m_editorController)
      {
        demo::profiling::ScopedCpuRange editorUiRange("AppPreRecord.SceneEditorUI");
#if defined(MGIF_EDITOR_LIVE_REPLAY)
        if(m_editorLiveEnabled &&
           m_editorLiveStage == EditorLiveReplayStage::markerPending &&
           m_editorLiveMarkerRenderedFrames == 0u)
        {
          m_editorController->ui().requestSceneBrowserFocus();
          m_editorLiveSceneBrowserFocusApplied = true;
        }
#endif
#if defined(MGIF_EDITOR_OS_REPLAY)
        if(m_editorOsReplayEnabled && m_editorOsSceneBrowserFocusPending)
        {
          m_editorController->ui().requestSceneBrowserFocus();
        }
#endif
        const demo::editor::ui::RenderResult result =
            m_editorController->render(
                editorLayout
#if defined(MGIF_EDITOR_OS_REPLAY)
                , m_editorOsReplayEnabled && m_editorOsReplayWarmupComplete
                      ? m_editorOsReplayRuntime.get()
                      : nullptr
#endif
            );
        if(!result.lastError.empty())
        {
          reportEditorError(result.lastError);
        }
        finishEditorViewportRuntimeFrame();
        publishPendingEditorProjection();
      }

      glm::vec4 viewportImageRect{0.0f};
      {
        demo::profiling::ScopedCpuRange viewportPanelRange("AppPreRecord.FullWindowViewport");
        const demo::rhi::Extent2D requestedViewportSize{
          m_editorViewportMetrics.colorAttachment.width,
          m_editorViewportMetrics.colorAttachment.height
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

        const ImVec2 viewportImageMin = editorViewportMin;
        const ImVec2 viewportImageMax = editorViewportMax;
        viewportImageRect = glm::vec4(viewportImageMin.x,
                                      viewportImageMin.y,
                                      viewportImageMax.x - viewportImageMin.x,
                                      viewportImageMax.y - viewportImageMin.y);
        const demo::TextureHandle viewportTexture =
            m_renderer.getViewportTextureHandle();
        const ImTextureID viewportTextureId =
            m_renderer.getViewportTextureID(viewportTexture);
        if(viewportTextureId != ImTextureID{})
        {
          ImGui::GetBackgroundDrawList(editorMainViewport)->AddImage(
              viewportTextureId, viewportImageMin, viewportImageMax);
        }
        // The CPU-owned viewport runtime is the single presentation bridge for
        // the Grid/Gizmo packet. Draw it after the scene image and before any
        // unrelated debug overlays so the captured client pixels prove the
        // same snapshot that the RHI overlay pass consumed.
        auto *visualViewportRuntime = m_editorViewportRuntime.get();
        if(visualViewportRuntime != nullptr && !m_editorVisualEnabled &&
           !m_renderer.editorOverlayAvailable())
        {
          visualViewportRuntime->drawImGuiOverlay(
              ImGui::GetBackgroundDrawList(editorMainViewport));
        }
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
        if(m_editorController)
        {
          const demo::editor::ui::FrameModel editorFrame =
              m_editorController->buildFrameModel(editorLayout);
          m_editorController->ui().renderMultipleScenesSettings(
              editorFrame, *m_editorController);
        }

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
#if defined(MGIF_EDITOR_LIVE_REPLAY)
      if(m_editorLiveEnabled &&
         m_editorLiveStage == EditorLiveReplayStage::markerPending &&
         m_editorLiveMarkerRenderedFrames == 0u)
      {
        ImGui::SetWindowFocus("Scene Browser");
      }
#endif
#if defined(MGIF_EDITOR_OS_REPLAY)
      if(m_editorOsReplayEnabled && m_editorOsSceneBrowserFocusPending)
      {
        ImGui::SetWindowFocus("Scene Browser");
        m_editorOsSceneBrowserFocusPending = false;
      }
#endif

      {
        demo::profiling::ScopedCpuRange flaxDebugUiRange("AppPreRecord.FlaxDebugUI");
        // The visual evidence harness owns the editor viewport surface.  The
        // optional Flax diagnostic atlas uses a separate read/write texture
        // contract and is intentionally excluded from this focused capture.
        if(!m_editorController || !m_editorController->hasDocument())
        {
          drawFlaxDebugUI();
        }
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
        frameParams.editorMaterialColorPreview =
            m_editorController ? m_editorController->materialColorPreview()
                                : std::nullopt;
        frameParams.clearColor     = m_clearColor;
        frameParams.viewportImageRect = viewportImageRect;
        // Desktop scene resources are owned by the renderer transaction
        // publication. Legacy GltfUploadResult ownership is Android-only.
        frameParams.gltfModel      = nullptr;
        frameParams.cameraUniforms = &m_cameraUniforms;
        frameParams.editorViewportMetrics =
            m_editorViewportMetrics.valid() ? &m_editorViewportMetrics : nullptr;
        frameParams.editorOverlayFrame =
            m_editorGizmoAdapter != nullptr &&
                    m_editorGizmoAdapter->snapshot().has_value()
                ? &*m_editorGizmoAdapter->snapshot()
                : nullptr;
        frameParams.lightSettings  = resolveSceneLightSettings();
        if(m_projectedSceneAsset)
        {
          frameParams.sceneLights = m_projectedSceneAsset->lights;
          frameParams.sceneLightSceneNodes = m_projectedSceneAsset->nodes;
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

      framePhase = "AutomationCaptureSync";
      waitForAutomationCaptureHandshake();
      framePhase = "Render.3DSubmit";
      beginAutomationMetricsWindow();
      submitSceneFrame(frameParams);
      recordAutomationMetricsFrame();
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
#if defined(MGIF_EDITOR_OS_REPLAY)
      if(m_editorOsReplayEnabled && m_editorOsReplayRuntime &&
         !m_editorOsReplayWarmupComplete)
      {
        m_editorOsReplayWarmupComplete = true;
        LOGI("[EDITOR_OS_REPLAY] marker=warmup-complete");
      }
      framePhase = "EditorOsReplayPostPlatformRender";
      advanceEditorOsReplayAfterRender();
#endif
      });
      }
      catch(const std::exception& e)
      {
        if(isEditorTransactionEnabled()
           && m_editorTransactionSmoke.phase()
                  != demo::editor::automation::EditorTransactionSmoke::Phase::complete
           && m_editorTransactionSmoke.phase()
                  != demo::editor::automation::EditorTransactionSmoke::Phase::failed)
        {
          const auto failurePhase = m_editorTransactionSmoke.phase();
          (void)m_editorTransactionSmoke.reject("unexpected frame exception");
          m_editorTransactionFailureLogged = true;
          LOGE("[EDITOR_TRANSACTION] marker=automation-failed phase=%s frame=%llu reason=unexpected-frame-exception",
               demo::editor::automation::EditorTransactionSmoke::phaseName(
                   failurePhase).data(),
               static_cast<unsigned long long>(m_automationFrame));
        }
        LOGE("Frame failed during phase %s: %s", framePhase, e.what());
        throw;
      }
  }

  template <typename FrameBody>
#if defined(_MSC_VER)
  __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
  __attribute__((noinline))
#endif
  void renderFrame(FrameBody&& body)
  {
    // Editor UI construction, scene submission, and ImGui finalization are one
    // render phase of the frame from the profiler's point of view.
    demo::profiling::ScopedCpuRange renderRange("Render");
    body();
  }

#if defined(_MSC_VER)
  __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
  __attribute__((noinline))
#endif
  void submitSceneFrame(const demo::RenderParams& params)
  {
    // This is the single desktop render submission site for one runFrame call.
    demo::profiling::ScopedCpuRange rendererFacadeRange("Render.3DSubmit");
    m_renderer.render(params);
  }

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
  demo::editor::ui::EditorRendererAdapter           m_editorRendererAdapter;
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

  enum class P6NativeInteractionStage : std::uint8_t
  {
    idle = 0,
    waitingFirstSubmit,
    waitingFirstTerminal,
    waitingFirstHighlight,
    waitingSecondSubmit,
    waitingSecondTerminal,
    complete,
  };

  enum class EditorVisualStage : std::uint8_t
  {
    idle = 0,
    waitingProjection,
    ready,
    complete,
  };
#if defined(MGIF_EDITOR_LIVE_REPLAY)
  enum class EditorLiveReplayStage : std::uint8_t
  {
    disabled = 0,
    waitingScene,
    running,
    markerPending,
    complete,
    failed,
  };
#endif

  AutomationOptions m_automationOptions{};
  demo::editor::automation::EditorTransactionSmoke m_editorTransactionSmoke{};
  // Reset only at a smoke-approved durable publication/save boundary. Each
  // synchronous segment still observes the existing 60-second limit.
  std::chrono::steady_clock::time_point m_editorTransactionStartedAt{};
  bool m_editorTransactionDeadlineStarted{false};
  std::optional<demo::editor::NodeId> m_editorTransactionNode;
  std::string m_editorTransactionOriginalName;
  std::string m_editorTransactionMutatedName;
  std::filesystem::path m_editorTransactionOutputPath;
  std::size_t m_editorTransactionPublicationCount{0u};
  std::string m_editorTransactionEnqueueError;
  bool m_editorTransactionTargetAbsentAtStart{false};
  bool m_editorTransactionFailureLogged{false};
  bool m_automationSceneReadyObserved{false};
  bool m_automationStarted{false};
  bool m_automationComplete{false};
  uint64_t m_automationFrame{0u};
  AutomationPose m_automationCurrentPose{};
  AutomationPose m_automationPreviousPose{};
  bool m_automationMetricsWindowStarted{false};
  std::vector<double> m_automationCpuFrameMs;
  std::vector<double> m_automationGpuFrameMs;

  std::string m_p6NativeCase;
  std::filesystem::path m_p6NativeFixturePath;
  P6NativeInteractionStage m_p6NativeStage{
      P6NativeInteractionStage::idle};
  std::string m_p6NativeFailure;
  bool m_p6NativeEnabled{false};
  bool m_p6NativeSecondInjected{false};
  bool m_p6NativeDelayWaited{false};
  std::size_t m_p6NativeSubmitted{0u};
  std::size_t m_p6NativeSelectionEffects{0u};
  std::size_t m_p6NativeCameraEffects{0u};
  std::size_t m_p6NativeHighlightUpdates{0u};
  std::size_t m_p6NativeBrowserFocusEvents{0u};
  std::vector<demo::editor::EditorPickResult> m_p6NativeTerminals;
  std::optional<demo::editor::EditorPickRequest> m_p6NativeLastRequest;
  std::uint64_t m_p6NativeInitialHighlightPasses{0u};
  std::uint64_t m_p6NativeInitialOccurrencePasses{0u};
  std::uint64_t m_p6NativeInitialViewRevision{0u};
  std::uint64_t m_p6NativeInitialSelectionRevision{0u};
  float m_p6NativeInitialMoveSpeed{0.0f};
  demo::editor::LogicalViewportPoint m_p6NativeHitPoint{};
  demo::editor::LogicalViewportPoint m_p6NativeBlankPoint{};

  EditorVisualStage m_editorVisualStage{EditorVisualStage::idle};
  bool m_editorVisualEnabled{false};
  bool m_editorVisualStateWritten{false};
  bool m_editorVisualDockReset{false};
  std::optional<demo::editor::NodeId> m_editorVisualNode;
  std::filesystem::path m_editorVisualFixturePath;
  std::string m_editorVisualBoundary{"initial"};
  demo::editor::OverlayMatrix4 m_editorVisualParentWorld{};
  bool m_editorVisualParentWorldValid{false};
  bool m_editorVisualGpuIdleWaited{false};
#if defined(MGIF_EDITOR_LIVE_REPLAY)
  EditorLiveReplayStage m_editorLiveStage{EditorLiveReplayStage::disabled};
  bool m_editorLiveEnabled{false};
  bool m_editorLiveRendererIdleWaited{false};
  bool m_editorLiveSceneBrowserFocusApplied{false};
  bool m_editorLiveExitRequested{false};
  bool m_editorLiveEmptyStart{false};
  std::uint32_t m_editorLiveMarkerRenderedFrames{0u};
  std::filesystem::path m_editorLiveFixturePath;
  std::unique_ptr<demo::editor::testing::EditorTestDriver>
      m_editorLiveDriver;
  demo::editor::testing::TestScript m_editorLiveScript;
  demo::editor::testing::ReplayResult m_editorLiveResult;
  std::optional<demo::editor::testing::ActionResult>
      m_editorLivePendingResult;
  std::size_t m_editorLiveNextStep{0u};
  std::string m_editorLiveMarker;
  std::string m_editorLiveFailure;
#endif
#if defined(MGIF_EDITOR_OS_REPLAY)
  bool m_editorOsReplayEnabled{false};
  bool m_editorOsSceneBrowserFocusPending{false};
  bool m_editorOsGpuIdleWaited{false};
  bool m_editorOsReplayWarmupComplete{false};
  std::filesystem::path m_editorOsReplayFixturePath;
  std::unique_ptr<demo::editor::testing::osreplay::EditorUiOsReplayHost>
      m_editorOsReplayHost;
  std::unique_ptr<demo::editor::testing::osreplay::EditorUiOsReplayRuntime>
      m_editorOsReplayRuntime;
#endif

  // Immutable copy of the last projection accepted by the renderer adapter.
  // It supplies camera/light metadata for the matching renderer generation;
  // all editable document state remains owned by EditorController.
  std::shared_ptr<const demo::SceneAsset> m_projectedSceneAsset;

  // Camera
  demo::Camera m_camera;
  demo::Camera m_sceneCameraNavigation;
  bool m_editorForceFreeCamera{false};
  std::optional<uint32_t> m_editorDocumentCameraIndex;
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
  std::unique_ptr<demo::editor::ui::EditorController> m_editorController;
  std::unique_ptr<demo::editor::ui::EditorViewportRuntime>
      m_editorViewportRuntime;
  std::unique_ptr<demo::editor::ui::DesktopGizmoIntegrationAdapter>
      m_editorGizmoAdapter;
  bool m_editorViewportFrameOpen{false};
  std::string m_editorError;
  bool m_editorViewportFocused{false};
  demo::editor::EditorViewportMetrics m_editorViewportMetrics{};
  std::uint64_t m_editorViewportEpoch{1u};
  std::uint64_t m_editorViewRevision{1u};
  glm::mat4 m_editorLastViewMatrix{1.0f};
  glm::mat4 m_editorLastCameraProjectionMatrix{1.0f};
  bool m_editorLastViewValid{false};
  std::uint64_t m_editorInputFrame{1u};
  std::uint64_t m_editorInputSequence{1u};
  std::array<bool, 3> m_editorPointerDown{};
  std::array<bool, 3> m_editorPointerFocusSuspended{};
  bool m_editorEscapeDown{false};
  bool m_editorWindowFocused{false};
  // GLFW does not expose a button edge through polling after a focus loss.
  // Remember a press callback so a fresh gesture is not hidden behind the
  // stale pre-loss `m_editorPointerDown` value.
  bool m_editorPrimaryPressAfterFocus{false};
  // Native mouse-up callbacks that arrive after a focus-loss terminal are
  // retained until one independent focused frame.  This keeps callback
  // provenance while preventing a restore-frame synthetic up from being
  // mistaken for the user's late physical release.
  std::array<demo::editor::ui::NativeGizmoTerminalQueue::Event,
             demo::editor::ui::NativeGizmoTerminalQueue::kCapacity>
      m_editorDeferredFocusReleases{};
  std::size_t m_editorDeferredFocusReleaseCount{0u};
  bool m_editorFocusReleaseFence{false};
  bool m_editorFocusRestored{false};
  bool m_editorFocusRestorePending{false};
  demo::editor::ui::NativeGizmoTerminalQueue m_editorNativeGizmoInput;
  [[nodiscard]] bool nativeGizmoGestureActive() const noexcept {
    if(!m_editorGizmoAdapter) return false;
    const auto phase = m_editorGizmoAdapter->phase();
    return phase == demo::editor::GizmoInteractionPhase::dragging ||
           phase == demo::editor::GizmoInteractionPhase::draggingLatched;
  }
  ImVec2 m_editorLastMousePosition{0.0f, 0.0f};
  demo::editor::ui::ImGuiClipboardAdapter m_editorClipboard;
	char m_meshSDFPathBuffer[512] = "";
	std::string m_meshSDFStatus;
	bool m_meshSDFLoaded = false;
	bool m_autoLoadSDFOnSceneReady = false;

  ImGuiUtils::ProfilerGraph m_cpuProfilerGraph{240};
  ImGuiUtils::ProfilerGraph m_gpuProfilerGraph{240};
  std::vector<legit::ProfilerTask> m_cpuProfilerTasks;
  std::vector<legit::ProfilerTask> m_gpuProfilerTasks;
  bool m_runtimeProfilerInitialized{false};
  bool m_runtimeProfilerDisabled{false};

  void openModelWithEditor(const std::string& path);
  void handleDroppedScene(const std::filesystem::path& path, bool merge);
  void publishPendingEditorProjection();
  void reportEditorError(std::string_view message);
  [[nodiscard]] std::optional<std::filesystem::path> showEditorFileDialog(
      const demo::editor::ui::NativeDialogRequest& request);
  [[nodiscard]] demo::editor::ui::RuntimeActionResult handleEditorCameraRequest(
      const demo::editor::ui::CameraRequest& request);
  void resetPublishedEditorScene() noexcept;
  void drawFlaxDebugUI();
  void drawFlaxDebugViewportOverlay(const ImVec2& viewportMin, const ImVec2& viewportMax);
  void updateCameraInput();
  void updateEditorViewportRuntime(const ImVec2& viewportMin,
                                   const ImVec2& viewportMax);
  void sendEditorViewportEvent(
      demo::editor::EditorInputFrameId frame,
      demo::editor::EditorInputEventPayload payload,
      std::uint64_t callbackSequence = 0u,
      bool consumeIfAdapterUnclaimed = false) noexcept;
  void advanceP6NativeInteraction(demo::editor::EditorInputFrameId frame);
  void advanceEditorVisualInteraction(demo::editor::EditorInputFrameId frame);
  void configureP6NativeInteraction();
  void configureEditorVisualInteraction();
#if defined(MGIF_EDITOR_LIVE_REPLAY)
  void configureEditorLiveReplay();
  void initializeEditorLiveReplay();
  void advanceEditorLiveReplay();
  void onEditorLiveFrameRendered();
  [[nodiscard]] demo::editor::testing::AuthoritySummary
  captureEditorLiveAuthority() const;
  void writeEditorLiveEvidence(bool complete) const;
  void writeEditorLiveStep(
      const demo::editor::testing::ActionResult& result) const;
  [[noreturn]] void failEditorLiveReplay(std::string_view reason);
#endif
#if defined(MGIF_EDITOR_OS_REPLAY)
  void configureEditorOsReplay();
  void initializeEditorOsReplay();
  void advanceEditorOsReplayAfterRender();
  [[nodiscard]] std::optional<
      demo::editor::testing::osreplay::AuthorityProbe>
  captureEditorOsReplayAuthority() const;
  [[nodiscard]] bool editorOsReplayRendererSettled() const noexcept;
  [[noreturn]] void failEditorOsReplay(std::string_view reason);
#endif
  [[nodiscard]] bool editorVisualUsesOrthographicProjection() const noexcept;
  [[nodiscard]] double editorVisualGridUnit() const noexcept;
  [[nodiscard]] demo::editor::GizmoVisibility
  editorVisualGizmoVisibility() const noexcept;
  [[noreturn]] void failP6NativeInteraction(std::string_view reason);
  void finishEditorViewportRuntimeFrame();
  void setMeshSDFPathFromModelPath(const std::string& gltfPath);
  std::filesystem::path resolveMeshSDFPathForLoad() const;
  void loadMeshSDFForDDGI();
  [[nodiscard]] bool isAutomationEnabled() const;
  [[nodiscard]] const char* automationModeName() const;
  [[nodiscard]] uint64_t automationSettledFrame() const;
  [[nodiscard]] uint64_t automationTotalFrames() const;
  [[nodiscard]] AutomationPose automationPoseForFrame(uint64_t frame) const;
  [[nodiscard]] const char* automationTargetBoundaryMarkerForFrame(uint64_t frame) const;
  [[nodiscard]] const char* automationBoundaryMarkerForFrame(uint64_t frame) const;
  [[nodiscard]] const char* automationCaptureHandshakeMarkerForFrame(uint64_t frame) const;
  [[nodiscard]] bool isAutomationCaptureSyncEnabled() const;
  [[nodiscard]] bool isEditorTransactionEnabled() const noexcept;
  [[nodiscard]] std::optional<demo::editor::automation::EditorTransactionSmoke::Snapshot>
  captureEditorTransactionSnapshot() const;
  [[nodiscard]] static demo::editor::automation::EditorTransactionSmoke::ProjectionStamp
  editorTransactionProjectionStamp(
      const demo::editor::SceneProjectionResult& projection);
  [[nodiscard]] static std::string editorTransactionDigestHex(
      const demo::editor::automation::EditorTransactionSmoke::Digest& digest);
  void completeEditorTransactionWatchdogStage(
      demo::editor::automation::EditorTransactionSmoke::WatchdogStage stage);
  [[nodiscard]] bool editorTransactionNodeHasName(std::string_view name) const;
  void advanceEditorTransaction();
  void observeEditorTransactionPublication(
      const demo::editor::SceneProjectionResult& projection,
      demo::editor::ui::ProjectionRefresh refresh,
      const demo::editor::RendererTargetFence& resultingFence);
  [[noreturn]] void failEditorTransaction(std::string_view reason);
  void applyAutomationCameraPose();
  // The ordinary capture protocol waits before rendering.  Visual evidence
  // also needs one post-render fence so its normal screenshot cannot observe
  // pointer input that was queued for the following frame.
  void waitForAutomationCaptureHandshake(
      const char* forcedMarker = nullptr,
      const char* phase = "pre-render");
  void onAutomationFrameRendered();
  void beginAutomationMetricsWindow();
  void recordAutomationMetricsFrame();
  void writeAutomationMetrics() const;
  void writeEditorVisualState() const;
  [[nodiscard]] bool isEditorVisualEnabled() const noexcept;
  void logAutomationMarker(const char* marker) const;
  void resetSceneCameraNavigation();
  [[nodiscard]] bool populateActiveSceneCameraUniforms(shaderio::CameraUniforms& uniforms) const;
  [[nodiscard]] bool hasActiveSceneCamera() const;
  void updateActiveCamera();
  void refreshActiveCameraUniforms();
  void syncLightAnglesFromDirection();
  void syncLightDirectionFromAngles();
  demo::DirectionalLightSettings resolveSceneLightSettings() const;
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

inline void MinimalLatestApp::configureP6NativeInteraction()
{
  const char* requested = std::getenv("MGIF_P6_NATIVE_CASE");
  if(requested == nullptr || requested[0] == '\0')
  {
    return;
  }
  m_p6NativeCase = requested;
  if(m_p6NativeCase != "single-hit" &&
     m_p6NativeCase != "same-primitive-toggle" &&
     m_p6NativeCase != "blank-clear" &&
     m_p6NativeCase != "double-focus")
  {
    throw std::invalid_argument(
        "MGIF_P6_NATIVE_CASE must be single-hit, same-primitive-toggle, blank-clear, or double-focus");
  }
#ifndef MGIF_SOURCE_DIR
#define MGIF_SOURCE_DIR "G:/MGIF"
#endif
  m_p6NativeEnabled = true;
  m_p6NativeFixturePath = std::filesystem::path(MGIF_SOURCE_DIR) /
                          "tests" / "fixtures" / "gltf_editor" /
                          "r09_shared_mesh.gltf";
  if(!std::filesystem::is_regular_file(m_p6NativeFixturePath))
  {
    throw std::invalid_argument(
        "MGIF_P6_NATIVE_CASE fixture r09_shared_mesh.gltf is unavailable");
  }
}

inline void MinimalLatestApp::configureEditorVisualInteraction()
{
  if(m_automationOptions.editorVisualCase.empty())
  {
    return;
  }
#if defined(__ANDROID__)
  throw std::invalid_argument(
      "editor visual capture is desktop-only and cannot run on Android");
#else
  if(m_p6NativeEnabled)
  {
    throw std::invalid_argument(
        "editor visual capture cannot be combined with P6 native capture");
  }
  m_editorVisualEnabled = true;
  m_runtimeProfilerDisabled = true;
  m_debugOptions.showPassGpuProfile = false;
  m_debugOptions.showGPUCullingOverlay = false;
  m_debugOptions.showViewportAxis = false;
#ifndef MGIF_SOURCE_DIR
#define MGIF_SOURCE_DIR "G:/MGIF"
#endif
  const std::string_view fixture =
      m_automationOptions.editorVisualCase == "V01-grid"
          ? "tests/fixtures/gltf_editor/minimal_scene.gltf"
          : "tests/fixtures/gltf_editor/r09_shared_mesh.gltf";
  m_editorVisualFixturePath = std::filesystem::path(MGIF_SOURCE_DIR) / fixture;
  if(!std::filesystem::is_regular_file(m_editorVisualFixturePath))
  {
    throw std::invalid_argument(
        "editor visual capture fixture is unavailable");
  }
#endif
}

#if defined(MGIF_EDITOR_LIVE_REPLAY)
inline void MinimalLatestApp::configureEditorLiveReplay()
{
  if(m_automationOptions.editorLiveCase.empty())
  {
    return;
  }
#if defined(__ANDROID__)
  throw std::invalid_argument("editor live replay is desktop-only");
#else
  const bool knownLiveCase =
      m_automationOptions.editorLiveCase == "U01-empty-new-open" ||
      m_automationOptions.editorLiveCase == "U02-hierarchy" ||
      m_automationOptions.editorLiveCase == "U03-transform-gizmo" ||
      m_automationOptions.editorLiveCase == "U04-material" ||
      m_automationOptions.editorLiveCase == "U05-camera-light-vis" ||
      m_automationOptions.editorLiveCase == "U06-merge-compact-save" ||
      m_automationOptions.editorLiveCase == "U07-modified-failures" ||
      m_automationOptions.editorLiveCase == "U08-large-unicode-dpi";
  if(!knownLiveCase)
  {
    throw std::invalid_argument(
        "editor live replay case is outside the U01-U08 contract");
  }
  if(m_editorVisualEnabled || m_p6NativeEnabled ||
     isEditorTransactionEnabled())
  {
    throw std::invalid_argument(
        "editor live replay cannot share another editor automation mode");
  }
  if(m_automationOptions.mode == AutomationMode::none ||
     m_automationOptions.noUi || !m_automationOptions.autoExit)
  {
    throw std::invalid_argument(
        "editor live replay requires automation, visible UI, and auto-exit");
  }
  if(m_automationOptions.editorLiveEvidenceDirectory.empty() ||
     m_automationOptions.editorLiveSyncDirectory.empty() ||
     !m_automationOptions.editorLiveEvidenceDirectory.is_absolute() ||
     !m_automationOptions.editorLiveSyncDirectory.is_absolute() ||
     m_automationOptions.editorLiveEvidenceDirectory ==
         m_automationOptions.editorLiveSyncDirectory)
  {
    throw std::invalid_argument(
        "editor live evidence and sync directories must be distinct absolute paths");
  }
  const auto validSha256 = [](std::string_view value) {
    return value.size() == 64u &&
           std::all_of(value.begin(), value.end(), [](char character) {
             return std::isxdigit(
                        static_cast<unsigned char>(character)) != 0;
           });
  };
  if(!validSha256(m_automationOptions.editorLiveExecutableSha256) ||
     !validSha256(m_automationOptions.editorLiveSourceSha256))
  {
    throw std::invalid_argument(
        "editor live replay requires executable and source SHA-256 identity");
  }
  std::error_code filesystemError;
  if(std::filesystem::exists(
         m_automationOptions.editorLiveEvidenceDirectory, filesystemError) ||
     filesystemError)
  {
    throw std::invalid_argument(
        "editor live evidence directory must be fresh and absent");
  }
  filesystemError.clear();
  if(std::filesystem::exists(
         m_automationOptions.editorLiveSyncDirectory, filesystemError) ||
     filesystemError)
  {
    throw std::invalid_argument(
        "editor live sync directory must be fresh and absent");
  }
#ifndef MGIF_SOURCE_DIR
#define MGIF_SOURCE_DIR "G:/MGIF"
#endif
  const std::string_view fixtureName =
      m_automationOptions.editorLiveCase == "U01-empty-new-open" ||
              m_automationOptions.editorLiveCase == "U02-hierarchy"
          ? "multi_scene_oracle.gltf"
      : m_automationOptions.editorLiveCase == "U04-material"
          ? "r09_shared_mesh.gltf"
      : m_automationOptions.editorLiveCase == "U05-camera-light-vis"
          ? "live_camera_light.gltf"
      : m_automationOptions.editorLiveCase == "U06-merge-compact-save" ||
                m_automationOptions.editorLiveCase == "U07-modified-failures"
          ? "r09_shared_mesh.gltf"
      : m_automationOptions.editorLiveCase == "U08-large-unicode-dpi"
          ? "unicode_names.gltf"
          : "r09_shared_mesh.gltf";
  m_editorLiveFixturePath = std::filesystem::path(MGIF_SOURCE_DIR) /
                            "tests" / "fixtures" / "gltf_editor" /
                            fixtureName;
  // U01 is specified to start without a document and to open one mid-script,
  // so its fixture is bound for the scripted Open instead of being preloaded.
  m_editorLiveEmptyStart =
      m_automationOptions.editorLiveCase == "U01-empty-new-open";
  if(!std::filesystem::is_regular_file(m_editorLiveFixturePath))
  {
    throw std::invalid_argument(
        "editor live fixture is unavailable");
  }
  m_automationOptions.captureSyncDirectory =
      m_automationOptions.editorLiveSyncDirectory;
  m_automationOptions.captureControlFrame = false;
  m_editorLiveEnabled = true;
  m_editorLiveStage = EditorLiveReplayStage::waitingScene;
  m_runtimeProfilerDisabled = true;
  m_debugOptions.showPassGpuProfile = false;
  m_debugOptions.showGPUCullingOverlay = false;
  m_debugOptions.showViewportAxis = false;
#endif
}

inline demo::editor::testing::AuthoritySummary
MinimalLatestApp::captureEditorLiveAuthority() const
{
  demo::editor::testing::AuthoritySummary summary;
  if(m_editorController == nullptr)
  {
    return summary;
  }
  summary.lifecycleRevision = m_editorController->lifecycleRevision();
  const demo::editor::ui::FrameModel frame =
      m_editorController->buildFrameModel();
  summary.selectionRevision = frame.selectionRevision;
  switch(frame.selection.kind())
  {
  case demo::editor::EditorSelectionKind::none:
    summary.selectionKind = "None";
    break;
  case demo::editor::EditorSelectionKind::node:
    summary.selectionKind = "Node";
    break;
  case demo::editor::EditorSelectionKind::primitive:
    summary.selectionKind = "Primitive";
    break;
  case demo::editor::EditorSelectionKind::mesh:
    summary.selectionKind = "Mesh";
    break;
  case demo::editor::EditorSelectionKind::material:
    summary.selectionKind = "Material";
    break;
  case demo::editor::EditorSelectionKind::camera:
    summary.selectionKind = "Camera";
    break;
  case demo::editor::EditorSelectionKind::light:
    summary.selectionKind = "Light";
    break;
  }
  summary.hasDocument = m_editorController->hasDocument();
  summary.hasProjection = m_editorController->projection() != nullptr;
  const auto digestHex = [](const demo::editor::SceneDigest& digest) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string value;
    value.reserve(digest.size() * 2u);
    for(const std::uint8_t byte : digest)
    {
      value.push_back(hex[(byte >> 4u) & 0x0fu]);
      value.push_back(hex[byte & 0x0fu]);
    }
    return value;
  };
  if(const auto* session = m_editorController->session())
  {
    const auto& document = session->document();
    summary.documentRevision = document.revision();
    summary.cleanRevision = document.cleanRevision();
    summary.sceneActivationEpoch = document.sceneActivationEpoch();
    summary.sourcePath = document.sourcePath().generic_string();
    summary.documentDigest = digestHex(document.documentDigest());
    summary.undoCount = session->editor().history().undoCount();
    summary.redoCount = session->editor().history().redoCount();
  }
  if(const auto* projection = m_editorController->projection())
  {
    summary.mappingEpoch = projection->mappingEpoch;
    summary.semanticHash = projection->semanticHash;
    summary.projectionDigest = digestHex(projection->projectionDigest);
  }
  return summary;
}

inline void MinimalLatestApp::initializeEditorLiveReplay()
{
  if(!m_editorLiveEnabled ||
     m_editorLiveStage != EditorLiveReplayStage::waitingScene ||
     m_editorController == nullptr ||
     // An empty-start case has no session or projection yet: requiring them
     // here would deadlock U01 before its scripted New/Open ever runs.
     (!m_editorLiveEmptyStart &&
      (m_editorController->session() == nullptr ||
       m_editorController->projection() == nullptr)))
  {
    return;
  }
  demo::editor::testing::ScenarioBindings bindings;
  bindings.primaryDocument = m_editorLiveFixturePath;
  bindings.invalidDocument = m_editorLiveFixturePath.parent_path() /
                             "missing-document.gltf";
  bindings.mergeDocument = m_editorLiveFixturePath.parent_path() /
                           "minimal_scene.gltf";
  bindings.saveTarget = m_automationOptions.editorLiveEvidenceDirectory /
                        "saved.glb";
  bindings.unicodeName =
      "Live Unicode ä¸­æ ð combining-eÌ";
  if(m_editorLiveEmptyStart)
  {
    // U01 is specified to begin without a document: only the document paths
    // are bound here, and the scripted New/Open steps perform every real
    // document transition, so no scene-derived binding may be fabricated.
    LOGI("[EDITOR_LIVE_REPLAY] marker=empty-start case=%s",
         m_automationOptions.editorLiveCase.c_str());
  }
  else
  {
    auto& session = *m_editorController->session();
    auto& document = session.document();
    auto& commands = session.editor().commands();
    const auto objectAt = [&](demo::editor::EditorObjectKind kind,
                              std::size_t index) {
      if(index >= document.objectCount(kind))
      return demo::editor::EditorObjectId{};
      return document.idAt(kind, index);
    };
    const auto scene = demo::editor::SceneId::from(
        objectAt(demo::editor::EditorObjectKind::scene, 0u));
    const std::size_t sourceNodeIndex =
        m_automationOptions.editorLiveCase == "U02-hierarchy" ? 2u : 0u;
    const auto sourceNode = demo::editor::NodeId::from(
        objectAt(demo::editor::EditorObjectKind::node, sourceNodeIndex));
    const auto targetNode = demo::editor::NodeId::from(
        objectAt(demo::editor::EditorObjectKind::node, 1u));
    if(!scene || !sourceNode)
    {
      failEditorLiveReplay("live fixture lacks a primary scene/node");
    }
    const auto occurrence = commands.captureNodeOccurrence(*scene, *sourceNode);
    const auto transform = commands.nodeTransform(*sourceNode);
    if(!occurrence || !transform)
    {
      failEditorLiveReplay("live fixture lacks a revision-bound node occurrence");
    }
    const std::vector<demo::editor::NodeId> parents =
        document.sceneNodeParentPath(*scene, *sourceNode);
    bindings.primaryScene = *scene;
    bindings.primaryNode = *sourceNode;
    if(targetNode)
      bindings.secondaryNode = *targetNode;
    bindings.nodeTransform = *transform;
    if(occurrence)
      bindings.reparentSource = *occurrence;
    if(targetNode)
      bindings.reparentTarget = *targetNode;
    bindings.nodeSelection = demo::editor::EditorSelectionSnapshot{
        document.documentAuthority(),
        demo::editor::NodeSelection{
            *scene, *sourceNode,
            parents.empty()
                ? std::nullopt
                : std::optional<demo::editor::NodeId>{parents.back()}}};

    if(const auto primitive = demo::editor::PrimitiveId::from(
           objectAt(demo::editor::EditorObjectKind::primitive, 0u)))
    {
      const auto mesh = demo::editor::MeshId::from(
          objectAt(demo::editor::EditorObjectKind::mesh, 0u));
      if(mesh)
      {
        const auto material = demo::editor::MaterialId::from(
            objectAt(demo::editor::EditorObjectKind::material, 0u));
        bindings.primitiveSelection = demo::editor::EditorSelectionSnapshot{
            document.documentAuthority(),
            demo::editor::PrimitiveSelection{*scene, *sourceNode, *primitive,
                                             *mesh, 0u}};
        bindings.primitive = demo::editor::ui::PrimitiveContext{
            *scene, *sourceNode, *mesh, *primitive, material, true, true, true};
      }
    }
    bindings.primaryMaterial = demo::editor::MaterialId::from(
        objectAt(demo::editor::EditorObjectKind::material, 0u));
    bindings.alternateMaterial = demo::editor::MaterialId::from(
        objectAt(demo::editor::EditorObjectKind::material, 1u));
    bindings.camera = demo::editor::CameraId::from(
        objectAt(demo::editor::EditorObjectKind::camera, 0u));
    if(bindings.camera)
    {
      bindings.cameraNode = commands.cameraNode(*bindings.camera);
      bindings.cameraSelection = demo::editor::EditorSelectionSnapshot{
          document.documentAuthority(), *bindings.camera};
    }
    bindings.light = demo::editor::LightId::from(
        objectAt(demo::editor::EditorObjectKind::light, 0u));
    if(bindings.light)
    {
      bindings.lightSelection = demo::editor::EditorSelectionSnapshot{
          document.documentAuthority(), *bindings.light};
    }
    bindings.animation = demo::editor::AnimationId::from(
        objectAt(demo::editor::EditorObjectKind::animation, 0u));
  }

  if(m_automationOptions.editorLiveCase == "U07-modified-failures")
  {
    const auto blocker = m_automationOptions.editorLiveEvidenceDirectory /
                         "not-a-directory";
    std::ofstream output(blocker, std::ios::binary | std::ios::trunc);
    output << "block";
    output.close();
    bindings.saveTarget = blocker / "save.gltf";
  }

  std::string scriptError;
  auto script = demo::editor::testing::EditorTestDriver::defaultScript(
      m_automationOptions.editorLiveCase, bindings, &scriptError);
  if(!script)
  {
    failEditorLiveReplay(scriptError);
  }

  demo::editor::testing::EditorTestDriver::Callbacks callbacks;
  callbacks.dispatch = [this](const demo::editor::ui::UiCommand& command) {
    return m_editorController->dispatch(command);
  };
  callbacks.control =
      [this](const demo::editor::testing::ControllerAction& action) {
    using Kind = demo::editor::testing::ControllerActionKind;
    switch(action.kind)
    {
    case Kind::setGizmoHover:
      m_editorController->setGizmoHover(action.hovered);
      return demo::editor::ui::RuntimeActionResult{
          true, demo::editor::ui::ProjectionRefresh::none, {}};
    case Kind::beginGizmoTransform:
      return m_editorController->beginGizmoTransform(*action.node);
    case Kind::previewGizmoTransform:
      return m_editorController->previewGizmoTransform(
          *action.node, *action.transform);
    case Kind::finishGizmoTransform:
      return m_editorController->finishGizmoTransform(
          *action.node, action.physicalRelease, action.escapePressed,
          action.focusLost);
    case Kind::dismissError:
      m_editorController->dismissError();
      return demo::editor::ui::RuntimeActionResult{
          true, demo::editor::ui::ProjectionRefresh::none, {}};
    case Kind::activateSceneByIndex:
      if(!action.sceneIndex || m_editorController->session() == nullptr)
      {
        return demo::editor::ui::RuntimeActionResult{
            false, demo::editor::ui::ProjectionRefresh::none,
            "scene activation index is unavailable"};
      }
      {
        const auto& document = m_editorController->session()->document();
        if(*action.sceneIndex >=
           document.objectCount(demo::editor::EditorObjectKind::scene))
        {
          return demo::editor::ui::RuntimeActionResult{
              false, demo::editor::ui::ProjectionRefresh::none,
              "scene activation index is out of range"};
        }
        const auto scene = demo::editor::SceneId::from(
            document.idAt(demo::editor::EditorObjectKind::scene,
                          *action.sceneIndex));
        if(!scene)
        {
          return demo::editor::ui::RuntimeActionResult{
              false, demo::editor::ui::ProjectionRefresh::none,
              "scene activation id is invalid"};
        }
        return m_editorController->dispatch(
            demo::editor::ui::UiCommand{
                0u, demo::editor::ui::CommandSource::sceneBrowser,
                demo::editor::ui::ActivateScene{*scene}});
      }
    }
    return demo::editor::ui::RuntimeActionResult{
        false, demo::editor::ui::ProjectionRefresh::none,
        "invalid live controller action"};
  };
  callbacks.frame = [this]() {
    return m_editorController->buildFrameModel();
  };
  callbacks.persistent = [this]() {
    return m_editorController->persistentState();
  };
  callbacks.authority = [this]() {
    return captureEditorLiveAuthority();
  };
  callbacks.screenshot =
      [this](const demo::editor::testing::ScreenshotMarker& marker) {
    m_editorLiveMarker = marker.label;
  };
  m_editorLiveDriver =
      std::make_unique<demo::editor::testing::EditorTestDriver>(
          std::move(callbacks));
  const auto registration = m_editorLiveDriver->registerScript(*script);
  if(!registration.accepted)
  {
    failEditorLiveReplay(registration.error);
  }
  m_editorLiveScript = std::move(*script);
  m_editorLiveResult = {};
  m_editorLiveResult.scriptId = m_editorLiveScript.id;
  m_editorLiveNextStep = 0u;
  m_editorLiveStage = EditorLiveReplayStage::running;

  std::error_code filesystemError;
  std::filesystem::create_directories(
      m_automationOptions.editorLiveEvidenceDirectory / "steps",
      filesystemError);
  if(filesystemError)
  {
    failEditorLiveReplay(
        "could not create editor live evidence directory");
  }
  const auto writeFile = [](const std::filesystem::path& path,
                            std::string_view payload) {
    const std::filesystem::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(payload.data(),
                 static_cast<std::streamsize>(payload.size()));
    output.put('\n');
    output.close();
    if(!output)
    {
      throw std::runtime_error(
          "could not write editor live script manifest");
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if(error)
    {
      throw std::runtime_error(
          "could not publish editor live script manifest");
    }
  };
  writeFile(m_automationOptions.editorLiveEvidenceDirectory / "script.json",
            m_editorLiveScript.toJson());
  LOGI("[EDITOR_LIVE_REPLAY] marker=script-ready case=%s steps=%llu",
       m_editorLiveScript.id.c_str(),
       static_cast<unsigned long long>(m_editorLiveScript.steps.size()));
}

inline void MinimalLatestApp::writeEditorLiveStep(
    const demo::editor::testing::ActionResult& result) const
{
  const auto jsonString = [](std::string_view value) {
    std::string escaped{"\""};
    for(const char character : value)
    {
      if(character == '"' || character == '\\')
      {
        escaped.push_back('\\');
        escaped.push_back(character);
      }
      else if(character == '\n')
      {
        escaped += "\\n";
      }
      else
      {
        escaped.push_back(character);
      }
    }
    escaped.push_back('"');
    return escaped;
  };
  std::string payload =
      "{\"schemaVersion\":1,\"kind\":\"mgif.editor-ui-live-step\",";
  payload += "\"case\":" + jsonString(m_editorLiveScript.id);
  payload += ",\"frame\":" + std::to_string(m_automationFrame);
  payload +=
      ",\"dispatchMode\":\"direct-typed-command\",\"osClick\":false,"
      "\"nativeDialog\":false,\"identity\":{\"executable_sha256\":" +
      jsonString(m_automationOptions.editorLiveExecutableSha256) +
      ",\"source_sha256\":" +
      jsonString(m_automationOptions.editorLiveSourceSha256) +
      "},\"actionResult\":" + result.toJson() + "}";
  const std::filesystem::path path =
      m_automationOptions.editorLiveEvidenceDirectory / "steps" /
      (std::to_string(result.sequence) + "-" + result.actionId + ".json");
  const std::filesystem::path temporary = path.string() + ".tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  output.put('\n');
  output.close();
  if(!output)
  {
    throw std::runtime_error("could not write editor live step evidence");
  }
  std::error_code error;
  std::filesystem::remove(path, error);
  error.clear();
  std::filesystem::rename(temporary, path, error);
  if(error)
  {
    throw std::runtime_error("could not publish editor live step evidence");
  }
}

inline void MinimalLatestApp::writeEditorLiveEvidence(bool complete) const
{
  if(!m_editorLiveEnabled || m_editorLiveDriver == nullptr)
  {
    return;
  }
  const auto jsonString = [](std::string_view value) {
    std::string escaped{"\""};
    for(const unsigned char character : value)
    {
      switch(character)
      {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if(character < 0x20u)
        {
          static constexpr char hex[] = "0123456789abcdef";
          escaped += "\\u00";
          escaped.push_back(hex[(character >> 4u) & 0x0fu]);
          escaped.push_back(hex[character & 0x0fu]);
        }
        else
        {
          escaped.push_back(static_cast<char>(character));
        }
        break;
      }
    }
    escaped.push_back('"');
    return escaped;
  };
  const auto digestHex = [](const demo::editor::SceneDigest& digest) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string value;
    value.reserve(digest.size() * 2u);
    for(const std::uint8_t byte : digest)
    {
      value.push_back(hex[(byte >> 4u) & 0x0fu]);
      value.push_back(hex[byte & 0x0fu]);
    }
    return value;
  };
  const auto state = m_editorLiveDriver->snapshot(m_editorLiveMarker);
  const auto* projection = m_editorController != nullptr
                               ? m_editorController->projection()
                               : nullptr;
  const std::string runtimeDigest =
      projection != nullptr
          ? digestHex(projection->immutableSnapshot.contentDigest)
          : std::string(64u, '0');
  const bool rendererSettled =
      (m_editorController != nullptr && !m_editorController->hasDocument()) ||
      (m_editorRendererAdapter.initialized() &&
       !m_editorRendererAdapter.hasPending() &&
       !m_editorRendererAdapter.requiresAuthoritativeRebuild() &&
       m_projectedSceneAsset != nullptr);
  ImGuiWindow* sceneBrowser = ImGui::FindWindowByName("Scene Browser");
  const bool sceneBrowserVisible =
      sceneBrowser != nullptr && sceneBrowser->WasActive &&
      !sceneBrowser->Hidden &&
      (sceneBrowser->DockNode == nullptr ||
       sceneBrowser->DockNode->VisibleWindow == sceneBrowser);
  std::string common =
      "\"schemaVersion\":1,\"case\":" +
      jsonString(m_editorLiveScript.id) +
      ",\"frame\":" + std::to_string(m_automationFrame) +
      ",\"complete\":" + (complete ? "true" : "false") +
      ",\"dispatchMode\":\"direct-typed-command\",\"osClick\":false,"
      "\"nativeDialog\":false,\"screenshotClaim\":false,"
      "\"markerMeaning\":\"boundary-only\",\"marker\":" +
      jsonString(m_editorLiveMarker) +
      ",\"identity\":{\"executable_sha256\":" +
      jsonString(m_automationOptions.editorLiveExecutableSha256) +
      ",\"source_sha256\":" +
      jsonString(m_automationOptions.editorLiveSourceSha256) +
      "},\"renderer\":{\"settled\":" +
      (rendererSettled ? "true" : "false") +
      ",\"gpu_idle_waited\":" +
      (m_editorLiveRendererIdleWaited ? "true" : "false") +
      ",\"runtime_content_digest\":" + jsonString(runtimeDigest) + "}";
  common +=
      ",\"uiFocus\":{\"method\":\"acceptance-imgui-window-focus\","
      "\"scene_browser_tab_visible\":" +
      std::string(sceneBrowserVisible ? "true" : "false") + "}";
  common +=
      ",\"uiDpi\":{\"requested\":" +
      std::to_string(
          static_cast<double>(m_automationOptions.editorLiveUiDpiScale)) +
      ",\"applied\":" +
      std::to_string(static_cast<double>(ImGui::GetIO().FontGlobalScale)) +
      ",\"nativeContentScale\":" +
      std::to_string(
          static_cast<double>(ImGui::GetIO().DisplayFramebufferScale.x)) +
      ",\"source\":\"imgui-font-global-scale\"}";

  const std::string statePayload =
      "{\"kind\":\"mgif.editor-ui-live-state\"," + common +
      ",\"state\":" + state.toJson() +
      ",\"surfaceManifest\":" +
      demo::editor::testing::EditorTestDriver::surfaceManifestJson(state) +
      "}";
  const std::string resultPayload =
      "{\"kind\":\"mgif.editor-ui-live-result\"," + common +
      ",\"script\":" + m_editorLiveScript.toJson() +
      ",\"result\":" + m_editorLiveResult.toJson() +
      ",\"failure\":" + jsonString(m_editorLiveFailure) + "}";
  const auto writeFile = [](const std::filesystem::path& path,
                            std::string_view payload) {
    const std::filesystem::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(payload.data(),
                 static_cast<std::streamsize>(payload.size()));
    output.put('\n');
    output.close();
    if(!output)
    {
      throw std::runtime_error("could not write editor live evidence");
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if(error)
    {
      throw std::runtime_error("could not publish editor live evidence");
    }
  };
  writeFile(m_automationOptions.editorLiveEvidenceDirectory / "state.json",
            statePayload);
  writeFile(m_automationOptions.editorLiveEvidenceDirectory / "result.json",
            resultPayload);
}

[[noreturn]] inline void MinimalLatestApp::failEditorLiveReplay(
    std::string_view reason)
{
  m_editorLiveFailure =
      reason.empty() ? "unspecified editor live replay failure"
                     : std::string(reason);
  m_editorLiveResult.status =
      demo::editor::testing::ReplayStatus::failed;
  m_editorLiveResult.passed = false;
  m_editorLiveResult.error = m_editorLiveFailure;
  m_editorLiveStage = EditorLiveReplayStage::failed;
  try
  {
    writeEditorLiveEvidence(false);
  }
  catch(...)
  {
  }
  LOGE("[EDITOR_LIVE_REPLAY] marker=failed case=%s frame=%llu reason=%s",
       m_automationOptions.editorLiveCase.c_str(),
       static_cast<unsigned long long>(m_automationFrame),
       m_editorLiveFailure.c_str());
  throw std::runtime_error(
      "editor live replay failed: " + m_editorLiveFailure);
}

inline void MinimalLatestApp::advanceEditorLiveReplay()
{
  if(!m_editorLiveEnabled ||
     m_editorLiveStage != EditorLiveReplayStage::running ||
     (!m_automationSceneReadyObserved && !m_editorLiveEmptyStart) ||
     m_editorLiveDriver == nullptr)
  {
    return;
  }
  const bool nextActionIsExit = [&]() {
    if(m_editorLiveNextStep >= m_editorLiveScript.steps.size())
      return false;
    const auto *invoke = std::get_if<demo::editor::ui::InvokeCommand>(
        &m_editorLiveScript.steps[m_editorLiveNextStep].action.payload);
    return invoke != nullptr &&
           invoke->id == demo::editor::ui::CommandId::exit;
  }();
  // A document reset leaves the renderer adapter legitimately uninitialized:
  // without a document there is no publication left to settle, so the replay
  // must still reach its terminal exit and screenshot boundary steps instead
  // of waiting forever for a renderer projection that can never arrive.
  const bool rendererSettledWithoutDocument =
      m_editorController != nullptr && !m_editorController->hasDocument() &&
      !m_editorRendererAdapter.hasPending();
  if((!m_editorRendererAdapter.initialized() && !nextActionIsExit &&
      !rendererSettledWithoutDocument) ||
     m_editorRendererAdapter.requiresAuthoritativeRebuild())
  {
    if(m_editorRendererAdapter.requiresAuthoritativeRebuild())
    {
      if(m_editorLivePendingResult)
      {
        auto rejected = std::move(*m_editorLivePendingResult);
        m_editorLivePendingResult.reset();
        rejected.state = m_editorLiveDriver->snapshot("renderer-rejected");
        rejected.status = demo::editor::testing::ActionStatus::rejected;
        rejected.accepted = false;
        rejected.error =
            "renderer adapter requires an authoritative rebuild";
        m_editorLiveResult.steps.push_back(std::move(rejected));
        writeEditorLiveStep(m_editorLiveResult.steps.back());
      }
      failEditorLiveReplay(
          "renderer adapter requires an authoritative rebuild after the pending action");
    }
    return;
  }
  if(m_editorLiveNextStep >= m_editorLiveScript.steps.size())
  {
    failEditorLiveReplay("script exhausted without a screenshot marker");
  }
  const auto& step = m_editorLiveScript.steps[m_editorLiveNextStep];
  if(!m_editorLivePendingResult)
  {
    if(m_editorRendererAdapter.hasPending())
    {
      return;
    }
    auto dispatched = m_editorLiveDriver->dispatch(step.action);
    const bool acceptanceMatches =
        step.expectedAccepted &&
        dispatched.accepted == *step.expectedAccepted;
    const bool expectedRejection =
        step.expectedAccepted && !*step.expectedAccepted;
    const bool dispatchStatusValid =
        dispatched.status == demo::editor::testing::ActionStatus::accepted ||
        (dispatched.status ==
             demo::editor::testing::ActionStatus::rejected &&
         expectedRejection);
    if(!acceptanceMatches || !dispatchStatusValid)
    {
      m_editorLiveResult.steps.push_back(dispatched);
      writeEditorLiveStep(m_editorLiveResult.steps.back());
      failEditorLiveReplay(
          dispatched.error.empty() ? "typed live action failed closed"
                                   : dispatched.error);
    }
    m_editorLivePendingResult = std::move(dispatched);
  }
  if(m_editorRendererAdapter.hasPending())
  {
    return;
  }

  auto result = std::move(*m_editorLivePendingResult);
  m_editorLivePendingResult.reset();
  result.state = m_editorLiveDriver->snapshot(step.id);
  if(!step.expectedState || result.state.state != *step.expectedState)
  {
    result.status = demo::editor::testing::ActionStatus::invalid;
    result.accepted = false;
    result.error = "settled post-action state did not match expectation";
    m_editorLiveResult.steps.push_back(result);
    writeEditorLiveStep(m_editorLiveResult.steps.back());
    failEditorLiveReplay(result.error);
  }
  m_editorLiveResult.steps.push_back(std::move(result));
  writeEditorLiveStep(m_editorLiveResult.steps.back());
  LOGI("[EDITOR_LIVE_REPLAY] marker=action case=%s step=%llu id=%s "
       "source=direct-typed accepted=%d state=%s",
       m_editorLiveScript.id.c_str(),
       static_cast<unsigned long long>(m_editorLiveNextStep),
       step.id.c_str(), m_editorLiveResult.steps.back().accepted ? 1 : 0,
       demo::editor::testing::uiStateName(
           m_editorLiveResult.steps.back().state.state).data());
  ++m_editorLiveNextStep;
  if(std::holds_alternative<
         demo::editor::testing::ScreenshotMarkerAction>(
         step.action.payload))
  {
    if(m_editorLiveMarker.empty())
    {
      failEditorLiveReplay("screenshot boundary callback was not observed");
    }
    m_editorController->ui().requestSceneBrowserFocus();
    m_editorLiveSceneBrowserFocusApplied = true;
    m_editorLiveMarkerRenderedFrames = 0u;
    m_editorLiveStage = EditorLiveReplayStage::markerPending;
  }
}

inline void MinimalLatestApp::onEditorLiveFrameRendered()
{
  if(!m_editorLiveEnabled ||
     m_editorLiveStage != EditorLiveReplayStage::markerPending)
  {
    return;
  }
  if(m_editorLiveNextStep != m_editorLiveScript.steps.size())
  {
    failEditorLiveReplay(
        "U02 screenshot marker was not the terminal script step");
  }
  // A mid-script document open reaches its boundary marker only milliseconds
  // after the renderer publication, before the viewport has converged: with
  // the three-frame hold used by preloaded cases the U01 client held 11
  // unique colors and 2211 colorful samples against 131 and 11409 for U02.
  // Hold the boundary until the opened document has actually been presented
  // for a bounded number of frames.
  // 2026-09-09 convergence probe (window pinned onto the physical primary
  // monitor and verified topmost, so the capture really reads this window):
  //   hold 12/3  -> ready frame 8-18, near-black frame, uniqueColors 55/85,
  //                 luminance mean 3.9, top colours (0,0,0),(2,2,3),(2,3,4),
  //                 (5,8,12)=WindowBg*255; desktop-copy and window-dc-bitblt
  //                 agree byte-for-byte in profile.
  //   hold 120/90 -> ready frame 101, pure white ghost, uniqueColors 7,
  //                 luminance mean 255.0 (DWM substituted a ghost surface).
  // Neither hold yields a converged frame, because the capture handshake parks
  // without rendering or presenting: the photographed image is whatever was
  // last presented, and a longer pre-boundary render makes DWM ghost the parked
  // window.  The boundary must keep rendering/presenting while it waits for the
  // continue marker.  Keep the historical hold until that is implemented.
  const std::uint32_t markerFrameHold = m_editorLiveEmptyStart ? 12u : 3u;
  if(m_editorLiveMarkerRenderedFrames < markerFrameHold)
  {
    ++m_editorLiveMarkerRenderedFrames;
    if(m_editorLiveMarkerRenderedFrames < markerFrameHold)
    {
      return;
    }
  }
  ImGuiWindow* sceneBrowser = ImGui::FindWindowByName("Scene Browser");
  const bool sceneBrowserVisible =
      sceneBrowser != nullptr && sceneBrowser->WasActive &&
      !sceneBrowser->Hidden &&
      (sceneBrowser->DockNode == nullptr ||
       sceneBrowser->DockNode->VisibleWindow == sceneBrowser);
  LOGI("[EDITOR_LIVE_REPLAY] marker=focus-audit applied=%d found=%d "
       "active=%d hidden=%d dock_visible=%d rendered_frames=%u",
       m_editorLiveSceneBrowserFocusApplied ? 1 : 0,
       sceneBrowser != nullptr ? 1 : 0,
       sceneBrowser != nullptr && sceneBrowser->WasActive ? 1 : 0,
       sceneBrowser != nullptr && sceneBrowser->Hidden ? 1 : 0,
       sceneBrowser != nullptr &&
               (sceneBrowser->DockNode == nullptr ||
                sceneBrowser->DockNode->VisibleWindow == sceneBrowser)
           ? 1
           : 0,
       m_editorLiveMarkerRenderedFrames);
  if(!m_editorLiveSceneBrowserFocusApplied || !sceneBrowserVisible)
  {
    failEditorLiveReplay(
        "Scene Browser was not the visible dock tab at the screenshot boundary");
  }
  m_editorLiveResult.status =
      demo::editor::testing::ReplayStatus::passed;
  m_editorLiveResult.passed = true;
  m_editorLiveRendererIdleWaited = true;
  m_renderer.waitForIdle();
  writeEditorLiveEvidence(false);
  waitForAutomationCaptureHandshake(
      m_editorLiveMarker.c_str(), "post-render");
  m_editorLiveStage = EditorLiveReplayStage::complete;
  writeEditorLiveEvidence(true);
  LOGI("[EDITOR_LIVE_REPLAY] marker=complete case=%s steps=%llu "
       "dispatch=direct-typed screenshot_claim=0",
       m_editorLiveScript.id.c_str(),
       static_cast<unsigned long long>(m_editorLiveResult.steps.size()));
  if(m_automationOptions.autoExit || m_editorLiveExitRequested)
  {
    glfwSetWindowShouldClose(m_window, true);
  }
}
#endif

#if defined(MGIF_EDITOR_OS_REPLAY)
inline void MinimalLatestApp::configureEditorOsReplay()
{
  const auto& options = m_automationOptions.editorOsReplay;
  if(options.caseId.empty())
  {
    return;
  }
#if defined(__ANDROID__)
  throw std::invalid_argument("editor OS replay is desktop-only");
#else
  const bool knownCase =
      options.caseId == demo::editor::testing::osreplay::kU02Case ||
      options.caseId == demo::editor::testing::osreplay::kU03InspectorCase ||
      options.caseId == demo::editor::testing::osreplay::kU03GizmoCase ||
      demo::editor::testing::osreplay::isEscapeGizmoCase(options.caseId) ||
      demo::editor::testing::osreplay::isFocusGizmoCase(options.caseId);
  const bool validPlanSha = options.planSha256.size() == 64u &&
                            std::all_of(options.planSha256.begin(),
                                        options.planSha256.end(), [](char c) {
                                          return std::isxdigit(
                                              static_cast<unsigned char>(c)) != 0;
                                        });
  if(!knownCase || !validPlanSha)
  {
    throw std::invalid_argument("editor OS replay identity is invalid");
  }
  if(m_editorVisualEnabled || m_p6NativeEnabled ||
     isEditorTransactionEnabled()
#if defined(MGIF_EDITOR_LIVE_REPLAY)
     || m_editorLiveEnabled
#endif
  )
  {
    throw std::invalid_argument(
        "editor OS replay cannot share another automation mode");
  }
  if(m_automationOptions.backend != demo::rhi::BackendType::vulkan ||
     m_automationOptions.mode == AutomationMode::none ||
     m_automationOptions.noUi || !m_automationOptions.noPost ||
     !m_automationOptions.noDdgi || !m_automationOptions.autoExit)
  {
    throw std::invalid_argument(
        "editor OS replay requires Vulkan, visible UI, no post/DDGI, and auto-exit");
  }
#ifndef MGIF_SOURCE_DIR
#define MGIF_SOURCE_DIR "G:/MGIF"
#endif
  const char* fixture = options.subcase == "unique-root"
                            ? "minimal_scene.gltf"
                            : "r09_shared_mesh.gltf";
  m_editorOsReplayFixturePath = std::filesystem::path(MGIF_SOURCE_DIR) /
                                "tests" / "fixtures" / "gltf_editor" /
                                fixture;
  if(!std::filesystem::is_regular_file(m_editorOsReplayFixturePath))
  {
    throw std::invalid_argument("editor OS replay fixture is unavailable");
  }
  m_editorOsReplayEnabled = true;
  if(options.caseId == demo::editor::testing::osreplay::kU03GizmoCase ||
     demo::editor::testing::osreplay::isEscapeGizmoCase(options.caseId) ||
     demo::editor::testing::osreplay::isFocusGizmoCase(options.caseId))
  {
    m_runtimeProfilerDisabled = true;
    m_debugOptions.showPassGpuProfile = false;
    m_debugOptions.showGPUCullingOverlay = false;
    m_debugOptions.showViewportAxis = false;
  }
#endif
}

inline void MinimalLatestApp::initializeEditorOsReplay()
{
  if(!m_editorOsReplayEnabled || m_window == nullptr ||
     m_editorController == nullptr)
  {
    return;
  }
  namespace os = demo::editor::testing::osreplay;
  m_editorOsReplayHost = std::make_unique<os::EditorUiOsReplayHost>(
      m_window, m_automationOptions.editorOsReplay,
      [this]() { return m_editorNativeGizmoInput.pendingCount(); },
      [this]() { return m_editorNativeGizmoInput.focusInterrupted(); });
  os::RuntimeConfig config;
  config.plan = os::isFocusGizmoCase(m_automationOptions.editorOsReplay.caseId)
                    ? os::makeU03FocusOsReplayPlan(
                        m_automationOptions.editorOsReplay.caseId)
                    : os::isSameFrameGizmoCase(m_automationOptions.editorOsReplay.caseId)
                    ? os::makeU03SameFrameOsReplayPlan(
                        m_automationOptions.editorOsReplay.caseId == os::kU03EscapeReleaseCase)
                    : m_automationOptions.editorOsReplay.caseId == os::kU03EscapeCase
                    ? os::makeU03EscapeOsReplayPlan()
                    : m_automationOptions.editorOsReplay.caseId == os::kU03GizmoCase
                    ? os::makeU03GizmoOsReplayPlan()
                    : m_automationOptions.editorOsReplay.caseId == os::kU03InspectorCase
                    ? os::makeU03InspectorOsReplayPlan()
                    : os::makeU02OsReplayPlan();
  config.subcase = m_automationOptions.editorOsReplay.subcase;
  config.startStep = m_automationOptions.editorOsReplay.startStep;
  config.stopStep = m_automationOptions.editorOsReplay.stopStep;
  config.runNonce = m_automationOptions.editorOsReplay.runNonce;
  config.planSha256 = m_automationOptions.editorOsReplay.planSha256;
  config.executableSha256 =
      m_automationOptions.editorOsReplay.executableSha256;
  config.sourceSha256 = m_automationOptions.editorOsReplay.sourceSha256;
  config.requiredStableFrames = 2u;
  os::RuntimeCallbacks callbacks;
  callbacks.snapshot = [this]() -> std::optional<os::RuntimeHostSnapshot> {
    const auto authority = captureEditorOsReplayAuthority();
    if(!authority || !m_editorOsReplayHost)
    {
      return std::nullopt;
    }
    auto host = m_editorOsReplayHost->snapshot(
        *authority, editorOsReplayRendererSettled());
    if(host && os::isNativeGizmoReplayCase(
                    m_automationOptions.editorOsReplay.caseId) &&
       m_editorGizmoAdapter && m_editorGizmoAdapter->snapshot())
    {
      const auto observed = m_editorGizmoAdapter->interactionObservation();
      os::GizmoProbe g;
      g.renderedFrame = static_cast<std::uint64_t>(ImGui::GetFrameCount());
      g.inputFrame = observed.frameSerial;
      g.documentRevision = observed.documentRevision;
      g.selectionRevision = observed.selectionRevision;
      g.mappingEpoch = observed.mappingEpoch;
      if(observed.node) g.node = observed.node->untyped();
      if(observed.gestureNode) g.gestureNode = observed.gestureNode->untyped();
      const auto& rect = observed.logicalViewport;
      g.viewportRect = {static_cast<float>(rect.origin.x),
                        static_cast<float>(rect.origin.y),
                        static_cast<float>(rect.origin.x + rect.extent.width),
                        static_cast<float>(rect.origin.y + rect.extent.height)};
      g.pointer = {static_cast<float>(observed.pointer.x),
                   static_cast<float>(observed.pointer.y)};
      g.hasPointer = observed.hasPointer;
      g.ndcYDown = observed.ndcYDown;
      g.phase = static_cast<std::uint32_t>(observed.phase);
      g.hoveredComponent = static_cast<std::uint32_t>(observed.hoveredComponent);
      g.activeComponent = static_cast<std::uint32_t>(observed.activeComponent);
      g.captureHeld = observed.captureHeld;
      g.beginCount = observed.acceptedBeginCount;
      g.previewCount = observed.acceptedPreviewChangeCount;
      g.commitCount = observed.acceptedCommitCount;
      g.detachCount = observed.acceptedDetachCount;
      g.latchCount = observed.acceptedLatchCount;
      g.lastTerminalFrame = observed.lastTerminalFrame;
      g.lastTerminalSequence = observed.lastTerminalSequence;
      g.nativeCallbackSequence = m_editorNativeGizmoInput.callbackSequence();
      g.deliveredReleaseCount = observed.deliveredReleaseCount;
      g.escapePressCount = observed.escapePressCount;
      g.focusLossCount = observed.focusLossCount;
      g.suppressedReleaseCount = observed.suppressedReleaseCount;
      g.lastTerminalEventsOverflowed = observed.lastTerminalEventsOverflowed;
      for(std::size_t index = 0u; index < std::min<std::size_t>(
              observed.lastTerminalEventCount, observed.lastTerminalEvents.size()); ++index)
      {
        const auto& event = observed.lastTerminalEvents[index];
        using Kind = demo::editor::GizmoTerminalEventKind;
        const char* kind = "invalid";
        switch(event.kind)
        {
        case Kind::deliveredRelease: kind = "delivered-release"; break;
        case Kind::escapePressed: kind = "escape-pressed"; break;
        case Kind::focusLost: kind = "focus-lost"; break;
        case Kind::suppressedPhysicalMouseUp: kind = "suppressed-physical-mouse-up"; break;
        }
        g.lastTerminalEvents.push_back(
            {kind, event.sequence, observed.lastTerminalCallbackSequences[index]});
      }
      if(observed.lastTerminal)
      {
        using Terminal = demo::editor::ui::DesktopGizmoTerminal;
        g.terminal = *observed.lastTerminal == Terminal::commit ? "commit" :
                     *observed.lastTerminal == Terminal::detachRetainingPreview
                         ? "detach" : "latch";
      }
      const auto subject = observed.node ? observed.node : observed.gestureNode;
      if(subject)
      {
        const auto value = m_editorController->viewportNodeTransform(*subject);
        if(!value) return std::nullopt;
        g.translation = value->translation;
        g.rotation = value->rotation;
        g.scale = value->scale;
      }
      const auto* viewport = ImGui::GetMainViewport();
      const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
      if(!viewport || scale.x <= 0.0f || scale.y <= 0.0f) return std::nullopt;
      const auto pointRect = [&](double x, double y) {
        return demo::editor::ui::EditorUiProbeRect{
            static_cast<float>(x), static_cast<float>(y),
            static_cast<float>(x + 1.0 / scale.x),
            static_cast<float>(y + 1.0 / scale.y)};
      };
      if(const auto component = m_editorGizmoAdapter->componentProbe(
             demo::editor::GizmoComponent::translateY))
      {
        g.componentRect = pointRect(component->point.x, component->point.y);
        g.componentValid = component->frameSerial == observed.frameSerial &&
                           component->documentRevision == observed.documentRevision &&
                           component->selectionRevision == observed.selectionRevision &&
                           component->node == observed.node;
        g.componentEnabled = component->enabled;
      }
      // A real client location in the Browser band, outside the central
      // viewport. No release is redirected or synthesized by this observation.
      const double outsideX = viewport->Pos.x +
          std::floor((rect.origin.x - viewport->Pos.x) * scale.x - 24.0) / scale.x;
      const double outsideY = viewport->Pos.y +
          std::floor((rect.origin.y - viewport->Pos.y) * scale.y + 24.0) / scale.y;
      g.outsideRect = pointRect(outsideX, outsideY);
      host->gizmo = std::move(g);
    }
    return host;
  };
  callbacks.publishReady = [this](const os::ReadyProbe& ready) {
    return m_editorOsReplayHost
               ? m_editorOsReplayHost->publishReady(ready)
               : os::ReadyPublication{false, {}, "OS replay host is absent"};
  };
  m_editorOsReplayRuntime =
      std::make_unique<os::EditorUiOsReplayRuntime>(
          std::move(config), std::move(callbacks));
  if(m_editorOsReplayRuntime->failed())
  {
    failEditorOsReplay(m_editorOsReplayRuntime->error());
  }
  m_editorOsReplayWarmupComplete = false;
  m_editorOsSceneBrowserFocusPending = true;
  LOGI("[EDITOR_OS_REPLAY] marker=runtime-created subcase=%s steps=%u..%u",
       m_automationOptions.editorOsReplay.subcase.c_str(),
       m_automationOptions.editorOsReplay.startStep,
       m_automationOptions.editorOsReplay.stopStep);
}

inline std::optional<demo::editor::testing::osreplay::AuthorityProbe>
MinimalLatestApp::captureEditorOsReplayAuthority() const
{
  namespace os = demo::editor::testing::osreplay;
  if(!m_editorController || !m_editorController->session() ||
     !m_editorController->projection())
  {
    return std::nullopt;
  }
  const auto& document = m_editorController->session()->document();
  const auto& history = m_editorController->session()->editor().history();
  const auto& projection = *m_editorController->projection();
  if(projection.documentRevision != document.revision() ||
     projection.mapping.authority() != document.documentAuthority() ||
     projection.documentDigest != document.documentDigest() ||
     projection.projectionDigest !=
         projection.immutableSnapshot.projectionDigest ||
     projection.projectionDigest !=
         projection.immutableSnapshot.contentDigest)
  {
    return std::nullopt;
  }
  const auto digestHex = [](const demo::editor::SceneDigest& digest) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string value;
    value.reserve(digest.size() * 2u);
    for(const std::uint8_t byte : digest)
    {
      value.push_back(hex[(byte >> 4u) & 0x0fu]);
      value.push_back(hex[byte & 0x0fu]);
    }
    return value;
  };
  const auto idText = [](demo::editor::EditorObjectId id) {
    return std::string(demo::editor::editorObjectKindName(id.kind)) + ":" +
           std::to_string(id.slot) + ":" +
           std::to_string(id.generation) + ":" +
           std::to_string(id.authority.value);
  };
  std::string hierarchy;
  for(std::size_t index = 0u;
      index < document.objectCount(demo::editor::EditorObjectKind::scene);
      ++index)
  {
    const auto scene = demo::editor::SceneId::from(
        document.idAt(demo::editor::EditorObjectKind::scene, index));
    if(!scene)
    {
      return std::nullopt;
    }
    hierarchy += "scene\t" + idText(scene->untyped());
    for(const demo::editor::NodeId root : document.sceneRootNodeIds(*scene))
    {
      hierarchy += "\t" + idText(root.untyped());
    }
    hierarchy.push_back('\n');
  }
  for(std::size_t index = 0u;
      index < document.objectCount(demo::editor::EditorObjectKind::node);
      ++index)
  {
    const auto node = demo::editor::NodeId::from(
        document.idAt(demo::editor::EditorObjectKind::node, index));
    if(!node)
    {
      return std::nullopt;
    }
    hierarchy += "node\t" + idText(node->untyped());
    for(const demo::editor::NodeId child : document.nodeChildIds(*node))
    {
      hierarchy += "\t" + idText(child.untyped());
    }
    hierarchy.push_back('\n');
  }
  const auto* hierarchyBytes =
      reinterpret_cast<const std::uint8_t*>(hierarchy.data());
  const demo::editor::SceneDigest hierarchyDigest =
      demo::editor::SceneChangeCanonicalizer::computeSha256(
          std::span<const std::uint8_t>(hierarchyBytes, hierarchy.size()));

  const demo::editor::ui::FrameModel frame =
      m_editorController->buildFrameModel();
  os::AuthorityProbe result;
  result.documentAuthority = document.documentAuthority().value;
  result.documentRevision = document.revision();
  result.selectionRevision = frame.selectionRevision;
  result.undoCount = history.undoCount();
  result.redoCount = history.redoCount();
  result.documentDigest = digestHex(document.documentDigest());
  result.projectionDigest = digestHex(projection.projectionDigest);
  result.runtimeContentDigest =
      digestHex(projection.immutableSnapshot.contentDigest);
  result.hierarchyOrderDigest = digestHex(hierarchyDigest);
  result.undoDescription = std::string(history.undoDescription());
  result.redoDescription = std::string(history.redoDescription());
  switch(frame.selection.kind())
  {
  case demo::editor::EditorSelectionKind::none: result.selectionKind = "None"; break;
  case demo::editor::EditorSelectionKind::node: result.selectionKind = "Node"; break;
  case demo::editor::EditorSelectionKind::primitive: result.selectionKind = "Primitive"; break;
  case demo::editor::EditorSelectionKind::mesh: result.selectionKind = "Mesh"; break;
  case demo::editor::EditorSelectionKind::material: result.selectionKind = "Material"; break;
  case demo::editor::EditorSelectionKind::camera: result.selectionKind = "Camera"; break;
  case demo::editor::EditorSelectionKind::light: result.selectionKind = "Light"; break;
  }
  std::visit([&](const auto& value) {
    using Value = std::decay_t<decltype(value)>;
    if constexpr(std::is_same_v<Value, demo::editor::NodeSelection>)
    {
      result.selectedObject = value.node.untyped();
    }
    else if constexpr(std::is_same_v<Value, demo::editor::PrimitiveSelection>)
    {
      result.selectedObject = value.primitive.untyped();
    }
    else if constexpr(!std::is_same_v<Value, std::monostate>)
    {
      result.selectedObject = value.untyped();
    }
  }, frame.selection.value);
  std::size_t selectedRows = 0u;
  for(const auto& row : frame.browser.graphRows)
  {
    if(row.selected && row.selection == frame.selection &&
       result.selectedObject && row.key.object == *result.selectedObject)
    {
      ++selectedRows;
      result.selectedGraphRowKey = row.key;
    }
  }
  if((result.selectionKind == "None" && selectedRows != 0u) ||
     (result.selectionKind != "None" && selectedRows != 1u))
  {
    return std::nullopt;
  }
  return result;
}

inline bool MinimalLatestApp::editorOsReplayRendererSettled() const noexcept
{
  if(!m_editorOsReplayEnabled || !m_editorRendererAdapter.initialized() ||
     m_editorRendererAdapter.hasPending() ||
     m_editorRendererAdapter.requiresAuthoritativeRebuild() ||
     !m_editorRendererAdapter.activeFence() || !m_editorController ||
     !m_editorController->projection() || !m_projectedSceneAsset ||
     !m_editorTransactionEnqueueError.empty() || !m_editorError.empty())
  {
    return false;
  }
  const auto& projection = *m_editorController->projection();
  const auto& adapterFence = *m_editorRendererAdapter.activeFence();
  const demo::RendererSceneStateSnapshot renderer = m_renderer.activeSceneState();
  return renderer.initialized && renderer.fence &&
         *renderer.fence == adapterFence &&
         adapterFence.authority == projection.mapping.authority() &&
         adapterFence.appliedDocumentRevision == projection.documentRevision &&
         adapterFence.sceneActivationEpoch == projection.sceneActivationEpoch &&
         m_editorViewportMetrics.projection.valid() &&
         adapterFence.mappingEpoch ==
             m_editorViewportMetrics.projection.revision &&
         projection.mappingEpoch == m_editorViewportMetrics.projection.value &&
         adapterFence.rootExternalStateSerial ==
             projection.mapping.rootExternalStateSerial() &&
         adapterFence.activeScene == projection.activeScene &&
         adapterFence.projectionDigest == projection.projectionDigest &&
         m_projectedSceneAsset->documentRevision ==
             projection.asset.documentRevision &&
         m_projectedSceneAsset->documentRevision ==
             projection.documentRevision &&
         m_projectedSceneAsset->documentSemanticHash ==
             projection.asset.documentSemanticHash &&
         m_projectedSceneAsset->documentSemanticHash ==
             projection.semanticHash &&
         renderer.activeGenerationDigest ==
             m_editorRendererAdapter.activeGenerationDigest();
}

inline void MinimalLatestApp::advanceEditorOsReplayAfterRender()
{
  if(!m_editorOsReplayEnabled || !m_editorOsReplayRuntime ||
     !m_editorOsReplayHost)
  {
    return;
  }
  if(m_editorOsReplayRuntime->failed())
  {
    failEditorOsReplay(m_editorOsReplayRuntime->error());
  }
  m_renderer.waitForIdle();
  m_editorOsGpuIdleWaited = true;
  const bool published = m_editorOsReplayRuntime->onFrameRendered();
  if(m_editorOsReplayRuntime->failed())
  {
    failEditorOsReplay(m_editorOsReplayRuntime->error());
  }
  if(!published)
  {
    if((m_automationFrame % 120u) == 0u)
    {
      const auto authority = captureEditorOsReplayAuthority();
      const auto rendererState = m_renderer.activeSceneState();
      LOGI("[EDITOR_OS_REPLAY] marker=pending step=%u phase=%llu authority=%d "
           "settled=%d adapter_init=%d adapter_pending=%d adapter_rebuild=%d "
           "adapter_fence=%d renderer_init=%d renderer_fence=%d focused=%d "
           "stable_frames=%u stable_frame_serial=%llu ui_modal_pending=%d "
           "pending_reason=%s",
           m_editorOsReplayRuntime->currentStep(),
           static_cast<unsigned long long>(
               m_editorOsReplayRuntime->currentPhase()),
           authority ? 1 : 0,
           editorOsReplayRendererSettled() ? 1 : 0,
           m_editorRendererAdapter.initialized() ? 1 : 0,
           m_editorRendererAdapter.hasPending() ? 1 : 0,
           m_editorRendererAdapter.requiresAuthoritativeRebuild() ? 1 : 0,
           m_editorRendererAdapter.activeFence() ? 1 : 0,
           rendererState.initialized ? 1 : 0,
           rendererState.fence ? 1 : 0,
           glfwGetWindowAttrib(m_window, GLFW_FOCUSED) == GLFW_TRUE ? 1 : 0,
           m_editorOsReplayRuntime->pendingStableFrameCount(),
           static_cast<unsigned long long>(
               m_editorOsReplayRuntime->pendingStableFrameSerial()),
           m_editorController->ui().modalPending() ? 1 : 0,
           std::string(m_editorOsReplayRuntime->pendingReason()).c_str());
    }
    return;
  }
  if(!m_editorOsReplayRuntime->lastReady())
  {
    failEditorOsReplay("runtime published without a ready probe");
  }
  const auto receipt = m_editorOsReplayHost->waitForContinue(
      *m_editorOsReplayRuntime->lastReady());
  if(!m_editorOsReplayRuntime->acceptContinue(receipt))
  {
    failEditorOsReplay("runtime rejected the typed continue receipt");
  }
  m_editorOsReplayHost->confirmAcceptedContinue(receipt);
  if(m_editorOsReplayRuntime->complete())
  {
    m_editorOsReplayHost->writeResult(m_editorOsGpuIdleWaited);
    LOGI("[EDITOR_OS_REPLAY] marker=complete subcase=%s receipts=%llu",
         m_automationOptions.editorOsReplay.subcase.c_str(),
         static_cast<unsigned long long>(
             m_editorOsReplayHost->validatedReceiptCount()));
    glfwSetWindowShouldClose(m_window, true);
  }
}

[[noreturn]] inline void MinimalLatestApp::failEditorOsReplay(
    std::string_view reason)
{
  LOGE("[EDITOR_OS_REPLAY] marker=failed subcase=%s reason=%s",
       m_automationOptions.editorOsReplay.subcase.c_str(),
       std::string(reason).c_str());
  throw std::runtime_error("editor OS replay failed: " + std::string(reason));
}
#endif

inline bool MinimalLatestApp::editorVisualUsesOrthographicProjection() const noexcept
{
  return m_editorVisualEnabled &&
         m_automationOptions.editorVisualVariant.starts_with("orthographic-");
}

inline double MinimalLatestApp::editorVisualGridUnit() const noexcept
{
  const std::string_view variant = m_automationOptions.editorVisualVariant;
  if(variant.ends_with("-0.001")) return 0.001;
  if(variant.ends_with("-1000")) return 1000.0;
  return 1.0;
}

inline demo::editor::GizmoVisibility
MinimalLatestApp::editorVisualGizmoVisibility() const noexcept
{
  if(m_automationOptions.editorVisualCase != "V02-gizmo-components" ||
     !m_automationOptions.editorVisualComponent.has_value())
  {
    return demo::editor::GizmoVisibility::all;
  }
  const std::uint32_t component = *m_automationOptions.editorVisualComponent;
  if(component <= demo::editor::gizmoComponentIndex(
                      demo::editor::GizmoComponent::translateYZ))
  {
    return demo::editor::GizmoVisibility::translation;
  }
  if(component <= demo::editor::gizmoComponentIndex(
                      demo::editor::GizmoComponent::rotateZ))
  {
    return demo::editor::GizmoVisibility::rotation;
  }
  return demo::editor::GizmoVisibility::scale;
}

inline void MinimalLatestApp::sendEditorViewportEvent(
    demo::editor::EditorInputFrameId frame,
    demo::editor::EditorInputEventPayload payload,
    std::uint64_t callbackSequence,
    bool consumeIfAdapterUnclaimed) noexcept
{
  if(m_editorViewportRuntime == nullptr) return;
  m_editorInputSequence = std::max(
      m_editorInputSequence, m_editorViewportRuntime->nextInputSequence());
  if(m_editorInputSequence == std::numeric_limits<std::uint64_t>::max())
  {
    m_editorInputSequence = 1u;
  }
  demo::editor::EditorInputEvent event;
  event.sequence = {m_editorInputSequence++};
  event.frame = frame;
  event.payload = std::move(payload);
  if(m_editorGizmoAdapter != nullptr &&
     m_editorGizmoAdapter->route(event, callbackSequence))
  {
    const auto* key = std::get_if<demo::editor::EditorKeyEvent>(&event.payload);
    if(callbackSequence != 0u && key &&
       key->key == demo::editor::EditorInputKey::escape &&
       key->action == demo::editor::EditorInputAction::press && m_editorController)
      m_editorController->noteViewportEscapeHandled();
    return;
  }
  // Native callback terminals were already owned by the desktop Gizmo at the
  // time they were observed.  If a revision fence detached the adapter before
  // this frame drains them, consume the bounded batch fail-closed instead of
  // reclassifying the release as a camera/picking input.
  if(consumeIfAdapterUnclaimed)
    return;
  (void)m_editorViewportRuntime->route(event);
}

[[noreturn]] inline void MinimalLatestApp::failP6NativeInteraction(
    std::string_view reason)
{
  m_p6NativeFailure = reason.empty() ? "unspecified native interaction failure"
                                     : std::string(reason);
  LOGE("[P6_NATIVE] marker=failed case=%s frame=%llu reason=%s",
       m_p6NativeCase.c_str(),
       static_cast<unsigned long long>(m_automationFrame),
       m_p6NativeFailure.c_str());
  throw std::runtime_error("P6 native interaction failed: " +
                           m_p6NativeFailure);
}

inline void MinimalLatestApp::advanceP6NativeInteraction(
    demo::editor::EditorInputFrameId frame)
{
  if(!m_p6NativeEnabled || m_p6NativeStage == P6NativeInteractionStage::complete ||
     m_p6NativeFailure.size() != 0u || m_editorViewportRuntime == nullptr ||
     m_editorController == nullptr || !m_automationStarted ||
     m_automationFrame == 0u || !m_editorViewportRuntime->ready())
  {
    return;
  }
  const auto publication = m_renderer.editorPickPublication();
  if(!publication) return;
  if(m_p6NativeStage == P6NativeInteractionStage::idle &&
     (m_editorRendererAdapter.hasPending() || m_renderer.editorPickingBusy()))
  {
    return;
  }

  const auto extent = m_editorViewportMetrics.logical.extent;
  if(!(extent.width > 4.0 && extent.height > 4.0)) return;
  m_p6NativeHitPoint = {
      m_editorViewportMetrics.logical.origin.x + extent.width * 0.535,
      m_editorViewportMetrics.logical.origin.y + extent.height * 0.565};
  m_p6NativeBlankPoint = {
      m_editorViewportMetrics.logical.origin.x + extent.width - 2.0,
      m_editorViewportMetrics.logical.origin.y + extent.height - 2.0};

  const auto click = [&](demo::editor::LogicalViewportPoint point,
                         bool recognizedDouble) {
    sendEditorViewportEvent(frame, demo::editor::EditorPointerMoveEvent{point});
    sendEditorViewportEvent(
        frame, demo::editor::EditorPointerButtonEvent{
                   point, demo::editor::EditorPointerButton::primary,
                   demo::editor::EditorInputAction::press, recognizedDouble,
                   true});
    sendEditorViewportEvent(
        frame, demo::editor::EditorPointerButtonEvent{
                   point, demo::editor::EditorPointerButton::primary,
                   demo::editor::EditorInputAction::release, false, true});
  };
  const auto diagnostics = m_renderer.editorInteractionDiagnostics();
  const auto terminalCount = m_p6NativeTerminals.size();
  switch(m_p6NativeStage)
  {
  case P6NativeInteractionStage::idle:
    break;
  case P6NativeInteractionStage::waitingFirstSubmit:
    if(m_p6NativeCase != "double-focus" && !m_p6NativeDelayWaited)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(350));
      m_p6NativeDelayWaited = true;
    }
    if(m_p6NativeSubmitted >= 1u)
      m_p6NativeStage = P6NativeInteractionStage::waitingFirstTerminal;
    break;
  case P6NativeInteractionStage::waitingFirstTerminal:
    if(terminalCount >= 1u)
    {
      if(m_p6NativeTerminals.front().completion !=
             demo::editor::PickCompletion::hit ||
         m_p6NativeSelectionEffects == 0u &&
             m_p6NativeCase != "double-focus")
      {
        failP6NativeInteraction("first native GPU pick did not produce the expected hit/selection");
      }
      if(m_p6NativeCase == "double-focus")
      {
        if(m_p6NativeCameraEffects != 1u ||
           m_editorViewRevision != m_p6NativeInitialViewRevision + 1u)
        {
          failP6NativeInteraction("double-focus did not apply exactly one camera effect");
        }
        m_p6NativeStage = P6NativeInteractionStage::complete;
      }
      else
      {
        m_p6NativeStage = P6NativeInteractionStage::waitingFirstHighlight;
      }
    }
    break;
  case P6NativeInteractionStage::waitingFirstHighlight:
    if(diagnostics.highlightPassExecutions > m_p6NativeInitialHighlightPasses &&
       diagnostics.lastHighlightOccurrenceCount != 0u)
    {
      if(m_p6NativeCase == "single-hit")
      {
        m_p6NativeStage = P6NativeInteractionStage::complete;
      }
      else
      {
        click(m_p6NativeCase == "same-primitive-toggle"
                  ? m_p6NativeHitPoint : m_p6NativeBlankPoint,
              false);
        m_p6NativeSecondInjected = true;
        m_p6NativeDelayWaited = false;
        m_p6NativeStage = P6NativeInteractionStage::waitingSecondSubmit;
      }
    }
    break;
  case P6NativeInteractionStage::waitingSecondSubmit:
    if(m_p6NativeCase != "double-focus" && !m_p6NativeDelayWaited)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(350));
      m_p6NativeDelayWaited = true;
    }
    if(m_p6NativeSubmitted >= 2u)
      m_p6NativeStage = P6NativeInteractionStage::waitingSecondTerminal;
    break;
  case P6NativeInteractionStage::waitingSecondTerminal:
    if(terminalCount >= 2u)
    {
      const auto &second = m_p6NativeTerminals[1];
      const bool expected =
          (m_p6NativeCase == "same-primitive-toggle" &&
           second.completion == demo::editor::PickCompletion::hit) ||
          (m_p6NativeCase == "blank-clear" &&
           second.completion == demo::editor::PickCompletion::miss);
      if(!expected || m_p6NativeSelectionEffects < 2u ||
         !std::holds_alternative<std::monostate>(
             m_editorController->sceneSelection().value()))
      {
        failP6NativeInteraction("second native pick did not produce the expected toggle/clear result");
      }
      m_p6NativeStage = P6NativeInteractionStage::complete;
    }
    break;
  case P6NativeInteractionStage::complete:
    break;
  }

  if(m_p6NativeStage == P6NativeInteractionStage::idle)
  {
    m_p6NativeInitialHighlightPasses = diagnostics.highlightPassExecutions;
    m_p6NativeInitialOccurrencePasses = diagnostics.occurrencePassExecutions;
    m_p6NativeInitialViewRevision = m_editorViewRevision;
    m_p6NativeInitialSelectionRevision = m_editorController->selectionRevision();
    m_p6NativeInitialMoveSpeed = m_moveSpeed;
    if(m_p6NativeCase == "double-focus")
    {
      click(m_p6NativeHitPoint, false);
      sendEditorViewportEvent(
          frame, demo::editor::EditorPointerButtonEvent{
                     m_p6NativeHitPoint,
                     demo::editor::EditorPointerButton::primary,
                     demo::editor::EditorInputAction::press, true, true});
      sendEditorViewportEvent(
          frame, demo::editor::EditorPointerButtonEvent{
                     m_p6NativeHitPoint,
                     demo::editor::EditorPointerButton::primary,
                     demo::editor::EditorInputAction::release, false, true});
    }
    else
    {
      click(m_p6NativeHitPoint, false);
    }
    m_p6NativeStage = P6NativeInteractionStage::waitingFirstSubmit;
    m_p6NativeDelayWaited = false;
    LOGI("[P6_NATIVE] marker=started case=%s frame=%llu hit=(%.2f,%.2f) blank=(%.2f,%.2f)",
         m_p6NativeCase.c_str(),
         static_cast<unsigned long long>(m_automationFrame),
         m_p6NativeHitPoint.x, m_p6NativeHitPoint.y,
         m_p6NativeBlankPoint.x, m_p6NativeBlankPoint.y);
  }
}

inline bool MinimalLatestApp::isEditorVisualEnabled() const noexcept
{
  return m_editorVisualEnabled;
}

inline void MinimalLatestApp::advanceEditorVisualInteraction(
    demo::editor::EditorInputFrameId frame)
{
  if(!m_editorVisualEnabled || m_editorVisualStage == EditorVisualStage::complete ||
     m_editorVisualStage == EditorVisualStage::ready ||
     m_editorVisualStage == EditorVisualStage::idle ||
     !m_automationStarted || frame.value == 0u ||
     m_editorController == nullptr || m_editorViewportRuntime == nullptr ||
     !m_editorViewportRuntime->ready())
  {
    return;
  }

  if(m_editorVisualStage == EditorVisualStage::waitingProjection)
  {
    const demo::editor::SceneProjectionResult *projection =
        m_editorController->projection();
    if(projection == nullptr || !projection->succeeded())
    {
      return;
    }

    auto settings = m_editorController->viewportSettings();
    settings.showGrid = true;
    settings.showGizmo = m_automationOptions.editorVisualCase != "V01-grid";
    settings.snapEnabled = m_automationOptions.editorVisualCase ==
                           "V02-gizmo-components";
    settings.gridUnit = editorVisualGridUnit();
    m_editorController->setViewportSettings(settings);

    if(settings.showGizmo && !m_editorVisualNode.has_value())
    {
      const auto scene = m_editorController->session()->document().currentSceneId();
      const std::size_t visualNodeIndex = 0u;
      const auto nodeUntyped = projection->mapping.documentIdAt(
          demo::editor::EditorObjectKind::node, visualNodeIndex);
      const auto node = nodeUntyped.has_value()
                            ? demo::editor::NodeId::from(*nodeUntyped)
                            : std::optional<demo::editor::NodeId>{};
      if(scene.valid() && node.has_value())
      {
        const demo::editor::EditorSelectionSnapshot selection{
            projection->mapping.authority(),
            demo::editor::NodeSelection{scene, *node, std::nullopt}};
        const auto result = m_editorController->applyViewportSelection(
            selection, false);
        if(!result.accepted)
        {
          throw std::runtime_error(
              "editor visual capture could not select its fixture node");
        }
        m_editorVisualNode = *node;
        return;
      }
      LOGW("[EDITOR_VISUAL] node selection deferred: scene=%d node=%d",
           scene.valid() ? 1 : 0, node.has_value() ? 1 : 0);
    }

    if(m_editorRendererAdapter.hasPending())
    {
      // A selection/viewport-settings action can enqueue one immutable
      // publication. Let the host flush it before sealing the next packet.
      return;
    }

    const auto &snapshot = m_editorGizmoAdapter->snapshot();
    if(!snapshot.has_value() ||
       (settings.showGrid && !snapshot->gridVisible) ||
       (settings.showGizmo && !snapshot->gizmoVisible))
    {
      return;
    }
    writeEditorVisualState();
    m_editorVisualStateWritten = true;
    m_editorVisualStage = EditorVisualStage::ready;
    LOGI("[EDITOR_VISUAL] marker=ready case=%s frame=%llu dpi=%.3f",
         m_automationOptions.editorVisualCase.c_str(),
         static_cast<unsigned long long>(frame.value),
         static_cast<double>(m_automationOptions.editorVisualDpi));
  }
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
  const uint64_t base = automationSettledFrame() + 1u +
                        (m_automationOptions.captureControlFrame ? 1u : 0u);
  // Visual evidence captures the control frame after two following loop turns,
  // so release/scene publication has a stable presented frame to sample.
  const uint64_t visualControlTurn =
      m_editorVisualEnabled && m_automationOptions.captureControlFrame ? 2u : 0u;
  // Native picking cases need two delayed-click windows in addition to the
  // ordinary renderer settle window; keep the same fixed-clock transport.
  return m_p6NativeEnabled ? std::max<uint64_t>(base, 90u)
                           : base + visualControlTurn;
}

inline MinimalLatestApp::AutomationPose MinimalLatestApp::automationPoseForFrame(uint64_t frame) const
{
#if defined(MGIF_EDITOR_OS_REPLAY)
  if(m_automationOptions.editorOsReplay.caseId ==
         demo::editor::testing::osreplay::kU03GizmoCase ||
     demo::editor::testing::osreplay::isEscapeGizmoCase(
         m_automationOptions.editorOsReplay.caseId) ||
     demo::editor::testing::osreplay::isFocusGizmoCase(
         m_automationOptions.editorOsReplay.caseId))
    return {glm::vec3(0.0f, 0.0f, 4.0f), -90.0f, 0.0f};
#endif
  const AutomationPose start{};
  AutomationPose finish = start;
  if(m_p6NativeEnabled)
  {
    const AutomationPose native{glm::vec3(0.5f, 0.5f, -3.0f), 90.0f, 0.0f};
    return native;
  }
  if(m_editorVisualEnabled)
  {
    if(m_automationOptions.editorVisualCase == "V04-depth-xray")
    {
      return {glm::vec3(0.5f, 0.5f, 3.0f), -90.0f, 0.0f};
    }
    if(m_automationOptions.editorVisualVariant == "perspective-horizon")
    {
      return {glm::vec3(0.0f, 1.5f, 3.0f), -90.0f, -1.0f};
    }
    return {glm::vec3(3.0f, 2.0f, 3.0f), -135.0f, -25.2394f};
  }
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
  if(m_editorVisualEnabled && m_automationOptions.captureControlFrame &&
     frame == automationSettledFrame() + 2u)
  {
    return "control-rendered";
  }
  if(m_editorVisualEnabled && m_automationOptions.captureControlFrame &&
     frame == automationSettledFrame() + 3u)
  {
    return "control-presented";
  }
  return nullptr;
}

inline const char* MinimalLatestApp::automationCaptureHandshakeMarkerForFrame(uint64_t frame) const
{
  const uint64_t lastMovingFrame =
      static_cast<uint64_t>(m_automationOptions.warmupFrames)
      + static_cast<uint64_t>(m_automationOptions.motionFrames) - 1u;
  const uint64_t firstStillFrame = lastMovingFrame + 1u;
  if(frame + 1u == lastMovingFrame)
  {
    return "arm-last-moving";
  }
  if(const char* boundary = automationBoundaryMarkerForFrame(frame); boundary != nullptr)
  {
    return boundary;
  }
  // The visual harness acknowledges the completed first-still frame at the
  // following pre-render boundary.  This gives GLFW/DWM one full loop turn to
  // present the normal image while the cursor is still held at the blank point.
  if(m_editorVisualEnabled && frame == firstStillFrame + 1u)
  {
    return "normal-rendered";
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

inline bool MinimalLatestApp::isEditorTransactionEnabled() const noexcept
{
  return m_editorTransactionSmoke.enabled();
}

inline demo::editor::automation::EditorTransactionSmoke::ProjectionStamp
MinimalLatestApp::editorTransactionProjectionStamp(
    const demo::editor::SceneProjectionResult& projection)
{
  return {
      projection.documentRevision,
      projection.documentDigest,
      projection.projectionDigest,
      projection.immutableSnapshot.contentDigest,
      projection.mappingEpoch,
  };
}

inline std::optional<demo::editor::automation::EditorTransactionSmoke::Snapshot>
MinimalLatestApp::captureEditorTransactionSnapshot() const
{
  if(!m_editorController || !m_editorController->session()
     || !m_editorController->projection())
  {
    return std::nullopt;
  }
  const demo::editor::GltfDocument& document =
      m_editorController->session()->document();
  const demo::editor::EditorHistory& history =
      m_editorController->session()->editor().history();
  const demo::editor::SceneProjectionResult& projection =
      *m_editorController->projection();
  if(projection.documentRevision != document.revision()
     || projection.mapping.appliedDocumentRevision() != document.revision()
     || projection.mapping.authority() != document.documentAuthority()
     || projection.mapping.projectionDigest() != projection.projectionDigest
     || projection.immutableSnapshot.documentDigest !=
            projection.documentDigest
     || projection.immutableSnapshot.projectionDigest !=
            projection.projectionDigest)
  {
    return std::nullopt;
  }
  return demo::editor::automation::EditorTransactionSmoke::Snapshot{
      editorTransactionProjectionStamp(projection),
      document.cleanRevision(),
      history.undoCount(),
      history.redoCount(),
  };
}

inline std::string MinimalLatestApp::editorTransactionDigestHex(
    const demo::editor::automation::EditorTransactionSmoke::Digest& digest)
{
  static constexpr char digits[] = "0123456789abcdef";
  std::string result(digest.size() * 2u, '0');
  for(std::size_t index = 0u; index < digest.size(); ++index)
  {
    result[index * 2u] = digits[(digest[index] >> 4u) & 0x0fu];
    result[index * 2u + 1u] = digits[digest[index] & 0x0fu];
  }
  return result;
}

inline void MinimalLatestApp::completeEditorTransactionWatchdogStage(
    demo::editor::automation::EditorTransactionSmoke::WatchdogStage stage)
{
  using Smoke = demo::editor::automation::EditorTransactionSmoke;
  const auto boundary = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      boundary - m_editorTransactionStartedAt);
  if(!m_editorTransactionDeadlineStarted
     || !m_editorTransactionSmoke.completeWatchdogStage(
         stage, elapsed, m_automationFrame))
  {
    failEditorTransaction(m_editorTransactionSmoke.failure().empty()
        ? "editor transaction watchdog boundary was unavailable"
        : m_editorTransactionSmoke.failure());
  }
  LOGI("[EDITOR_TRANSACTION_TIMING] stage=%s elapsed_ms=%lld deadline_ms=60000 frame=%llu",
       Smoke::watchdogStageName(stage).data(),
       static_cast<long long>(elapsed.count()),
       static_cast<unsigned long long>(m_automationFrame));
  m_editorTransactionStartedAt = boundary;
}

inline bool MinimalLatestApp::editorTransactionNodeHasName(
    std::string_view name) const
{
  return m_editorTransactionNode.has_value() && m_editorController
      && m_editorController->session()
      && m_editorController->session()->document().contains(
          m_editorTransactionNode->untyped())
      && m_editorController->session()->document().objectName(
          m_editorTransactionNode->untyped()) == name;
}

inline void MinimalLatestApp::advanceEditorTransaction()
{
  using Smoke = demo::editor::automation::EditorTransactionSmoke;
  namespace editor_ui = demo::editor::ui;

  if(!isEditorTransactionEnabled()
     || m_editorTransactionSmoke.phase() == Smoke::Phase::complete)
  {
    return;
  }
  if(m_editorTransactionSmoke.phase() == Smoke::Phase::failed)
  {
    failEditorTransaction(m_editorTransactionSmoke.failure());
  }
  if(!m_editorTransactionEnqueueError.empty())
  {
    failEditorTransaction(m_editorTransactionEnqueueError);
  }
  if(!m_automationSceneReadyObserved)
  {
    return;
  }
  if(!m_automationStarted)
  {
    failEditorTransaction("camera automation did not start after scene-ready");
  }
  if(m_editorRendererAdapter.hasPending() ||
     !m_editorRendererAdapter.initialized() || !m_projectedSceneAsset)
  {
    failEditorTransaction("renderer publication became unstable after scene-ready");
  }
  if(m_editorTransactionDeadlineStarted)
  {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_editorTransactionStartedAt);
    if(!m_editorTransactionSmoke.checkDeadline(elapsed, m_automationFrame))
    {
      failEditorTransaction(m_editorTransactionSmoke.failure());
    }
  }

  const auto dispatchEvidence = [this](
      const editor_ui::RuntimeActionResult& result,
      editor_ui::ProjectionRefresh expectedRefresh,
      std::size_t beforePublicationCount) {
    return Smoke::DispatchEvidence{
        result.accepted,
        result.refresh == expectedRefresh,
        m_editorRendererAdapter.hasPending(),
        m_editorTransactionPublicationCount - beforePublicationCount,
    };
  };

  switch(m_editorTransactionSmoke.phase())
  {
    case Smoke::Phase::ready:
    {
      if(m_automationFrame != 0u || m_editorRendererAdapter.hasPending())
      {
        failEditorTransaction(
            "rename could not start on frame 0 with an empty publication queue");
      }
      const auto baseline = captureEditorTransactionSnapshot();
      const demo::editor::SceneProjectionResult* projection =
          m_editorController ? m_editorController->projection() : nullptr;
      if(!baseline || projection == nullptr || !m_editorController->session())
      {
        failEditorTransaction("baseline editor snapshot was unavailable");
      }
      const std::optional<demo::editor::EditorObjectId> mappedNode =
          projection->mapping.documentIdAt(
              demo::editor::EditorObjectKind::node, 0u);
      if(!mappedNode)
      {
        failEditorTransaction("strict scene projection contains no mapped node 0");
      }
      m_editorTransactionNode = demo::editor::NodeId::from(*mappedNode);
      if(!m_editorTransactionNode
         || !m_editorController->session()->document().contains(*mappedNode))
      {
        failEditorTransaction("mapped node identity is stale or cross-document");
      }
      m_editorTransactionOriginalName = std::string(
          m_editorController->session()->document().objectName(*mappedNode));
      m_editorTransactionMutatedName =
          m_editorTransactionOriginalName == "MGIF_P5_EDITOR_TRANSACTION"
              ? "MGIF_P5_EDITOR_TRANSACTION_1"
              : "MGIF_P5_EDITOR_TRANSACTION";
      m_editorTransactionOutputPath =
          m_automationOptions.editorTransactionDirectory /
          "sponza-transaction.glb";

      std::error_code filesystemError;
      m_editorTransactionTargetAbsentAtStart =
          !std::filesystem::exists(m_editorTransactionOutputPath,
                                   filesystemError);
      if(filesystemError || !m_editorTransactionTargetAbsentAtStart
         || !std::filesystem::is_empty(
             m_automationOptions.editorTransactionDirectory,
             filesystemError)
         || filesystemError)
      {
        failEditorTransaction(
            "isolated Save As directory was no longer empty at mutation start");
      }

      editor_ui::UiCommand command;
      command.sequence = 0u;
      command.source = editor_ui::CommandSource::sceneBrowser;
      command.payload = editor_ui::RenameObject{
          m_editorTransactionNode->untyped(),
          m_editorTransactionMutatedName};
      const std::size_t publicationsBefore =
          m_editorTransactionPublicationCount;
      const auto synchronousStartedAt = std::chrono::steady_clock::now();
      const editor_ui::RuntimeActionResult result =
          m_editorController->dispatch(command);
      if(!m_editorTransactionEnqueueError.empty())
      {
        failEditorTransaction(m_editorTransactionEnqueueError);
      }
      const auto mutated = captureEditorTransactionSnapshot();
      if(!mutated
         || !editorTransactionNodeHasName(m_editorTransactionMutatedName))
      {
        failEditorTransaction("rename did not update the selected stable node");
      }
      if(!m_editorTransactionSmoke.submitMutation(
             *baseline, *mutated,
             dispatchEvidence(result, editor_ui::ProjectionRefresh::hierarchy,
                              publicationsBefore),
             m_automationFrame))
      {
        failEditorTransaction(m_editorTransactionSmoke.failure());
      }
      const auto synchronousElapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - synchronousStartedAt);
      LOGI("[EDITOR_TRANSACTION_TIMING] stage=rename-dispatch elapsed_ms=%lld revision=%llu mapping_epoch=%llu document_digest=%s",
           static_cast<long long>(synchronousElapsed.count()),
           static_cast<unsigned long long>(
               mutated->projection.documentRevision),
           static_cast<unsigned long long>(mutated->projection.mappingEpoch),
           editorTransactionDigestHex(
               mutated->projection.documentDigest).c_str());
      LOGI("[CSM_AUTOMATION] marker=automation-start mode=%s transaction=rename-undo-redo-save frame_budget=40",
           automationModeName());
      return;
    }
    case Smoke::Phase::mutationAcceptedByRenderer:
    {
      if(!editorTransactionNodeHasName(m_editorTransactionMutatedName)
         || m_editorRendererAdapter.hasPending())
      {
        failEditorTransaction(
            "mutation acknowledgement did not leave one stable CPU state");
      }
      editor_ui::UiCommand command;
      command.sequence = 0u;
      command.source = editor_ui::CommandSource::shortcut;
      command.payload = editor_ui::InvokeCommand{editor_ui::CommandId::undo,
                                                 std::nullopt};
      const std::size_t publicationsBefore =
          m_editorTransactionPublicationCount;
      const auto synchronousStartedAt = std::chrono::steady_clock::now();
      const editor_ui::RuntimeActionResult result =
          m_editorController->dispatch(command);
      if(!m_editorTransactionEnqueueError.empty())
      {
        failEditorTransaction(m_editorTransactionEnqueueError);
      }
      const auto restored = captureEditorTransactionSnapshot();
      if(!restored
         || !editorTransactionNodeHasName(m_editorTransactionOriginalName))
      {
        failEditorTransaction("undo did not restore the original node name");
      }
      if(!m_editorTransactionSmoke.submitUndo(
             *restored,
             dispatchEvidence(result, editor_ui::ProjectionRefresh::properties,
                              publicationsBefore),
             m_automationFrame))
      {
        failEditorTransaction(m_editorTransactionSmoke.failure());
      }
      const auto synchronousElapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - synchronousStartedAt);
      LOGI("[EDITOR_TRANSACTION_TIMING] stage=undo-dispatch elapsed_ms=%lld revision=%llu mapping_epoch=%llu document_digest=%s",
           static_cast<long long>(synchronousElapsed.count()),
           static_cast<unsigned long long>(
               restored->projection.documentRevision),
           static_cast<unsigned long long>(restored->projection.mappingEpoch),
           editorTransactionDigestHex(
               restored->projection.documentDigest).c_str());
      return;
    }
    case Smoke::Phase::undoObserved:
    {
      if(!editorTransactionNodeHasName(m_editorTransactionOriginalName)
         || m_editorRendererAdapter.hasPending())
      {
        failEditorTransaction(
            "undo acknowledgement did not leave one stable CPU state");
      }
      editor_ui::UiCommand command;
      command.sequence = 0u;
      command.source = editor_ui::CommandSource::shortcut;
      command.payload = editor_ui::InvokeCommand{editor_ui::CommandId::redo,
                                                 std::nullopt};
      const std::size_t publicationsBefore =
          m_editorTransactionPublicationCount;
      const auto synchronousStartedAt = std::chrono::steady_clock::now();
      const editor_ui::RuntimeActionResult result =
          m_editorController->dispatch(command);
      if(!m_editorTransactionEnqueueError.empty())
      {
        failEditorTransaction(m_editorTransactionEnqueueError);
      }
      const auto reapplied = captureEditorTransactionSnapshot();
      if(!reapplied
         || !editorTransactionNodeHasName(m_editorTransactionMutatedName))
      {
        failEditorTransaction("redo did not reapply the mutated node name");
      }
      if(!m_editorTransactionSmoke.submitRedo(
             *reapplied,
             dispatchEvidence(result, editor_ui::ProjectionRefresh::properties,
                              publicationsBefore),
             m_automationFrame))
      {
        failEditorTransaction(m_editorTransactionSmoke.failure());
      }
      const auto synchronousElapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - synchronousStartedAt);
      LOGI("[EDITOR_TRANSACTION_TIMING] stage=redo-dispatch elapsed_ms=%lld revision=%llu mapping_epoch=%llu document_digest=%s",
           static_cast<long long>(synchronousElapsed.count()),
           static_cast<unsigned long long>(
               reapplied->projection.documentRevision),
           static_cast<unsigned long long>(reapplied->projection.mappingEpoch),
           editorTransactionDigestHex(
               reapplied->projection.documentDigest).c_str());
      return;
    }
    case Smoke::Phase::redoObserved:
    {
      if(!editorTransactionNodeHasName(m_editorTransactionMutatedName)
         || m_editorRendererAdapter.hasPending())
      {
        failEditorTransaction(
            "redo acknowledgement did not leave one stable CPU state");
      }
      std::error_code filesystemError;
      const bool targetStillAbsent =
          !std::filesystem::exists(m_editorTransactionOutputPath,
                                   filesystemError);
      if(filesystemError || !targetStillAbsent
         || !m_editorTransactionTargetAbsentAtStart)
      {
        failEditorTransaction("Save As target appeared before publication");
      }

      editor_ui::UiCommand command;
      command.sequence = 0u;
      command.source = editor_ui::CommandSource::dialog;
      command.payload = editor_ui::InvokeCommand{
          editor_ui::CommandId::saveAs, m_editorTransactionOutputPath};
      const std::size_t publicationsBefore =
          m_editorTransactionPublicationCount;
      const auto synchronousStartedAt = std::chrono::steady_clock::now();
      const editor_ui::RuntimeActionResult result =
          m_editorController->dispatch(command);
      if(m_editorTransactionPublicationCount != publicationsBefore
         || m_editorRendererAdapter.hasPending())
      {
        failEditorTransaction("Save As unexpectedly published a renderer update");
      }

      filesystemError.clear();
      const std::filesystem::file_status targetStatus =
          std::filesystem::symlink_status(m_editorTransactionOutputPath,
                                          filesystemError);
      const bool targetRegular = !filesystemError
          && std::filesystem::is_regular_file(targetStatus)
          && !std::filesystem::is_symlink(targetStatus);
      filesystemError.clear();
      const std::uint64_t targetBytes = targetRegular
          ? std::filesystem::file_size(m_editorTransactionOutputPath,
                                       filesystemError)
          : 0u;
      if(filesystemError)
      {
        failEditorTransaction("Could not stat the Save As output");
      }
#ifdef _WIN32
      const DWORD targetAttributes =
          GetFileAttributesW(m_editorTransactionOutputPath.c_str());
      if(targetAttributes == INVALID_FILE_ATTRIBUTES
         || (targetAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
      {
        failEditorTransaction("Save As output is a reparse alias");
      }
#endif

      demo::editor::GltfLoadResult reloaded =
          demo::editor::GltfDocumentIO::loadCandidate(
              m_editorTransactionOutputPath);
      const bool reloadSucceeded =
          reloaded.succeeded() && reloaded.candidate() != nullptr;
      const demo::editor::GltfDocument* liveDocument =
          m_editorController && m_editorController->session()
              ? &m_editorController->session()->document()
              : nullptr;
      const std::optional<std::size_t> liveNodeIndex =
          liveDocument && m_editorTransactionNode
              ? liveDocument->indexOf(m_editorTransactionNode->untyped())
              : std::nullopt;
      const bool semanticEquivalent = reloadSucceeded && liveDocument
          && demo::editor::GltfSemanticCompare::compare(
                 *liveDocument, *reloaded.candidate()).equivalent();
      bool reloadedMutationPresent = false;
      if(reloadSucceeded && liveNodeIndex
         && *liveNodeIndex < reloaded.candidate()->objectCount(
                                 demo::editor::EditorObjectKind::node))
      {
        const demo::editor::EditorObjectId reloadedNode =
            reloaded.candidate()->idAt(
                demo::editor::EditorObjectKind::node,
                *liveNodeIndex);
        reloadedMutationPresent =
            reloaded.candidate()->objectName(reloadedNode)
            == m_editorTransactionMutatedName;
      }

      bool liveSourceIsTarget = false;
      if(liveDocument)
      {
        filesystemError.clear();
        liveSourceIsTarget = std::filesystem::equivalent(
            liveDocument->sourcePath(), m_editorTransactionOutputPath,
            filesystemError);
        if(filesystemError)
        {
          liveSourceIsTarget = false;
        }
      }
      const auto live = captureEditorTransactionSnapshot();
      Smoke::SaveEvidence save;
      save.dispatchAccepted = result.accepted;
      save.targetAbsentAtStart = m_editorTransactionTargetAbsentAtStart;
      save.targetRegularFile = targetRegular;
      save.targetBytes = targetBytes;
      save.reloadSucceeded = reloadSucceeded;
      save.semanticEquivalent = semanticEquivalent;
      save.reloadedMutationPresent = reloadedMutationPresent;
      save.liveSourceIsTarget = liveSourceIsTarget;
      save.liveDocumentClean = liveDocument != nullptr
          && !liveDocument->dirty();
      if(live)
      {
        save.live = *live;
      }
      if(!live
         || !m_editorTransactionSmoke.verifySave(save, m_automationFrame))
      {
        failEditorTransaction(live
            ? m_editorTransactionSmoke.failure()
            : "live document snapshot disappeared after Save As");
      }
      LOGI("[CSM_AUTOMATION] marker=save-complete mode=%s revision=%llu clean_revision=%llu bytes=%llu reload_valid=1 semantic_equal=1",
           automationModeName(),
           static_cast<unsigned long long>(
               live->projection.documentRevision),
           static_cast<unsigned long long>(live->cleanRevision),
           static_cast<unsigned long long>(targetBytes));
      const auto synchronousElapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - synchronousStartedAt);
      LOGI("[EDITOR_TRANSACTION_TIMING] stage=save-as elapsed_ms=%lld bytes=%llu reload_valid=%d semantic_equivalent=%d mutation_present=%d source_target=%d live_clean=%d document_digest=%s",
           static_cast<long long>(synchronousElapsed.count()),
           static_cast<unsigned long long>(targetBytes),
           reloadSucceeded ? 1 : 0,
           semanticEquivalent ? 1 : 0,
           reloadedMutationPresent ? 1 : 0,
           liveSourceIsTarget ? 1 : 0,
           save.liveDocumentClean ? 1 : 0,
           editorTransactionDigestHex(live->projection.documentDigest).c_str());
      completeEditorTransactionWatchdogStage(
          Smoke::WatchdogStage::saveVerification);
      return;
    }
    case Smoke::Phase::disabled:
    case Smoke::Phase::waitingSceneReady:
    case Smoke::Phase::mutationSubmitted:
    case Smoke::Phase::undoSubmitted:
    case Smoke::Phase::redoSubmitted:
    case Smoke::Phase::saveVerified:
    case Smoke::Phase::budgetComplete:
    case Smoke::Phase::complete:
      return;
    case Smoke::Phase::failed:
      break;
  }
  failEditorTransaction(m_editorTransactionSmoke.failure());
}

inline void MinimalLatestApp::observeEditorTransactionPublication(
    const demo::editor::SceneProjectionResult& projection,
    demo::editor::ui::ProjectionRefresh refresh,
    const demo::editor::RendererTargetFence& resultingFence)
{
  using Smoke = demo::editor::automation::EditorTransactionSmoke;
  if(!isEditorTransactionEnabled() || !m_automationSceneReadyObserved)
  {
    return;
  }
  if(refresh == demo::editor::ui::ProjectionRefresh::none
     || refresh == demo::editor::ui::ProjectionRefresh::selectionOnly)
  {
    failEditorTransaction(
        "renderer consumed a non-mutating transaction publication");
  }
  if(!resultingFence.valid()
     || resultingFence.authority != projection.mapping.authority()
     || resultingFence.appliedDocumentRevision != projection.documentRevision
     || resultingFence.sceneActivationEpoch != projection.sceneActivationEpoch
     || resultingFence.rootExternalStateSerial !=
            projection.mapping.rootExternalStateSerial()
     || resultingFence.activeScene != projection.activeScene
     || resultingFence.projectionDigest != projection.projectionDigest)
  {
    failEditorTransaction(
        "renderer acceptance fence did not match the published projection");
  }
  const auto current = captureEditorTransactionSnapshot();
  if(!current)
  {
    failEditorTransaction(
        "controller snapshot was unavailable at renderer acceptance");
  }
  const Smoke::ProjectionStamp accepted =
      editorTransactionProjectionStamp(projection);
  switch(m_editorTransactionSmoke.phase())
  {
    case Smoke::Phase::mutationSubmitted:
      if(refresh != demo::editor::ui::ProjectionRefresh::hierarchy
         || !editorTransactionNodeHasName(m_editorTransactionMutatedName)
         || !m_editorTransactionSmoke.observeMutationAccepted(
             accepted, m_automationFrame))
      {
        failEditorTransaction(
            m_editorTransactionSmoke.failure().empty()
                ? "renderer mutation publication had the wrong refresh or node value"
                : m_editorTransactionSmoke.failure());
      }
      completeEditorTransactionWatchdogStage(
          Smoke::WatchdogStage::renamePublication);
      return;
    case Smoke::Phase::undoSubmitted:
      if(refresh != demo::editor::ui::ProjectionRefresh::properties
         || !m_editorTransactionSmoke.observeUndoAccepted(
             *current, accepted,
             editorTransactionNodeHasName(m_editorTransactionOriginalName),
             m_automationFrame))
      {
        failEditorTransaction(
            m_editorTransactionSmoke.failure().empty()
                ? "renderer undo publication had the wrong refresh"
                : m_editorTransactionSmoke.failure());
      }
      completeEditorTransactionWatchdogStage(
          Smoke::WatchdogStage::undoPublication);
      LOGI("[CSM_AUTOMATION] marker=undo-observed mode=%s revision=%llu undo=0 redo=1 digest=%s renderer_revision=%llu",
           automationModeName(),
           static_cast<unsigned long long>(
               current->projection.documentRevision),
           editorTransactionDigestHex(
               current->projection.documentDigest).c_str(),
           static_cast<unsigned long long>(
               resultingFence.appliedDocumentRevision));
      return;
    case Smoke::Phase::redoSubmitted:
      if(refresh != demo::editor::ui::ProjectionRefresh::properties
         || !m_editorTransactionSmoke.observeRedoAccepted(
             *current, accepted,
             editorTransactionNodeHasName(m_editorTransactionMutatedName),
             m_automationFrame))
      {
        failEditorTransaction(
            m_editorTransactionSmoke.failure().empty()
                ? "renderer redo publication had the wrong refresh"
                : m_editorTransactionSmoke.failure());
      }
      completeEditorTransactionWatchdogStage(
          Smoke::WatchdogStage::redoPublication);
      LOGI("[CSM_AUTOMATION] marker=redo-observed mode=%s revision=%llu undo=1 redo=0 digest=%s renderer_revision=%llu",
           automationModeName(),
           static_cast<unsigned long long>(
               current->projection.documentRevision),
           editorTransactionDigestHex(
               current->projection.documentDigest).c_str(),
           static_cast<unsigned long long>(
               resultingFence.appliedDocumentRevision));
      return;
    default:
      failEditorTransaction(
          "unexpected renderer publication interleaved with the transaction");
  }
}

[[noreturn]] inline void MinimalLatestApp::failEditorTransaction(
    std::string_view reason)
{
  using Smoke = demo::editor::automation::EditorTransactionSmoke;
  const Smoke::Phase activePhase =
      m_editorTransactionSmoke.phase() == Smoke::Phase::failed
          ? m_editorTransactionSmoke.failurePhase()
          : m_editorTransactionSmoke.phase();
  std::string failureReason = reason.empty()
      ? std::string{"unspecified transaction failure"}
      : std::string{reason};
  for(char& character : failureReason)
  {
    const unsigned char value = static_cast<unsigned char>(character);
    if(value < 0x20u || value == 0x7fu)
    {
      character = ' ';
    }
  }
  (void)m_editorTransactionSmoke.reject(failureReason);
  if(!m_editorTransactionFailureLogged)
  {
    m_editorTransactionFailureLogged = true;
    LOGE("[EDITOR_TRANSACTION] marker=automation-failed phase=%s frame=%llu reason=%s",
         Smoke::phaseName(activePhase).data(),
         static_cast<unsigned long long>(m_automationFrame),
         failureReason.c_str());
  }
  throw std::runtime_error(
      "Editor transaction automation failed: " + failureReason);
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

inline void MinimalLatestApp::waitForAutomationCaptureHandshake(
    const char* forcedMarker, const char* phase)
{
#if defined(MGIF_EDITOR_LIVE_REPLAY)
  if(m_editorLiveEnabled && forcedMarker == nullptr)
  {
    return;
  }
#endif
  if(!isAutomationCaptureSyncEnabled() || !m_automationStarted || m_automationComplete)
  {
    return;
  }

  const char* boundary = forcedMarker != nullptr
      ? forcedMarker
      : automationCaptureHandshakeMarkerForFrame(m_automationFrame);
  if(boundary == nullptr)
  {
    return;
  }

  const bool visualNormalRendered =
      m_editorVisualEnabled && std::string_view(boundary) == "normal-rendered";
  const bool visualPostRenderBoundary =
      m_editorVisualEnabled &&
      (visualNormalRendered ||
       std::string_view(boundary) == "control-rendered" ||
       std::string_view(boundary) == "control-presented");
  const bool visualScreenshotBoundary =
      m_editorVisualEnabled &&
      (visualPostRenderBoundary ||
       std::string_view(boundary) == "arm-settled" ||
       std::string_view(boundary) == "settled" ||
       std::string_view(boundary) == "control-still" ||
       std::string_view(boundary) == "control-rendered" ||
       std::string_view(boundary) == "control-presented");
#if defined(MGIF_EDITOR_LIVE_REPLAY)
  const bool liveScreenshotBoundary =
      m_editorLiveEnabled && forcedMarker != nullptr;
#else
  constexpr bool liveScreenshotBoundary = false;
#endif
  const bool visualFrameBinding =
      visualScreenshotBoundary || liveScreenshotBoundary ||
      (m_editorVisualEnabled &&
       (std::string_view(boundary) == "arm-settled" ||
        std::string_view(boundary) == "settled"));
  const char* handshakePhase = visualPostRenderBoundary
      ? "post-render"
      : (phase != nullptr && phase[0] != '\0'
      ? phase
      : "pre-render");

  if(visualScreenshotBoundary || liveScreenshotBoundary)
  {
    // Drain the backend before every visual screenshot fence so the state and
    // pixels observe a completed preceding frame, not a CPU-only submit.
    m_editorVisualGpuIdleWaited = true;
    m_renderer.waitForIdle();
  }

  if(m_editorVisualEnabled && m_editorVisualStage != EditorVisualStage::idle)
  {
    // The adapter snapshot was closed at the end of the preceding frame; seal
    // that coherent value before publishing each boundary consumed by the
    // external capture harness.
    m_editorVisualBoundary = boundary;
    writeEditorVisualState();
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
              << "  \"phase\": \"" << handshakePhase << "\",\n"
              << (visualFrameBinding
                      ? "  \"rendered_frame\": "
                        + std::to_string(m_automationFrame == 0u
                                             ? 0u
                                             : m_automationFrame - 1u)
                        + ",\n"
                      : "")
              << "  \"no_post\": " << (m_automationOptions.noPost ? "true" : "false") << ",\n"
              << "  \"no_ddgi\": " << (m_automationOptions.noDdgi ? "true" : "false") << ",\n"
              << "  \"taa\": " << (m_automationOptions.taa ? "true" : "false") << ",\n"
              << "  \"capture_control\": " << (m_automationOptions.captureControlFrame ? "true" : "false") << ",\n"
              << "  \"marker\": \"" << boundary << "\",\n"
              << "  \"mode\": \"" << automationModeName() << "\",\n"
              << "  \"frame\": " << m_automationFrame << ",\n";
#if defined(MGIF_EDITOR_LIVE_REPLAY)
    if(m_editorLiveEnabled)
    {
      const auto authority = captureEditorLiveAuthority();
      readyFile << "  \"live\": {\n"
                << "    \"case\": \""
                << m_automationOptions.editorLiveCase << "\",\n"
                << "    \"dispatch_mode\": \"direct-typed-command\",\n"
                << "    \"os_click\": false,\n"
                << "    \"native_dialog\": false,\n"
                << "    \"state_path\": \""
                << (m_automationOptions.editorLiveEvidenceDirectory /
                    "state.json").generic_string() << "\",\n"
                << "    \"result_path\": \""
                << (m_automationOptions.editorLiveEvidenceDirectory /
                    "result.json").generic_string() << "\",\n"
                << "    \"executable_sha256\": \""
                << m_automationOptions.editorLiveExecutableSha256
                << "\",\n"
                << "    \"source_sha256\": \""
                << m_automationOptions.editorLiveSourceSha256 << "\",\n"
                << "    \"document_digest\": \""
                << authority.documentDigest << "\",\n"
                << "    \"projection_digest\": \""
                << authority.projectionDigest << "\"\n"
                << "  },\n";
    }
#endif
    readyFile << "  \"current\": {\n"
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
  LOGI("[CSM_AUTOMATION] marker=capture-ready boundary=%s mode=%s frame=%llu phase=%s no_post=%d no_ddgi=%d taa=%d capture_control=%d ready=%s",
       boundary,
       automationModeName(),
       static_cast<unsigned long long>(m_automationFrame),
       handshakePhase,
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
    // The Windows visual harness intentionally pauses at each capture
    // boundary. Keep the native window responsive so DWM never substitutes a
    // white ghost window and posted pointer/button input reaches GLFW before
    // the following frame. Other capture protocols preserve their old loop.
    if(m_editorVisualEnabled
#if defined(MGIF_EDITOR_LIVE_REPLAY)
       || m_editorLiveEnabled
#endif
    )
    {
      glfwPollEvents();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  for(std::uint32_t attempt = 0u; ; ++attempt)
  {
    filesystemError.clear();
    std::filesystem::remove(continuePath, filesystemError);
    if(!filesystemError || attempt >= 20u)
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if(filesystemError)
  {
    throw std::runtime_error("Could not consume capture continue marker: " + filesystemError.message());
  }
  LOGI("[CSM_AUTOMATION] marker=capture-continue boundary=%s mode=%s frame=%llu phase=%s",
       boundary,
       automationModeName(),
       static_cast<unsigned long long>(m_automationFrame),
       handshakePhase);
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

inline void MinimalLatestApp::beginAutomationMetricsWindow()
{
  if(m_automationOptions.metricsOutputPath.empty() ||
     m_automationMetricsWindowStarted ||
     !m_automationStarted ||
     m_automationFrame < m_automationOptions.warmupFrames)
  {
    return;
  }

  m_renderer.resetRhiHotPathCounters();
  m_automationCpuFrameMs.clear();
  m_automationGpuFrameMs.clear();
  m_automationMetricsWindowStarted = true;
  LOGI("[RHI_METRICS] marker=window-start backend=%s frame=%llu",
       m_automationOptions.backend == demo::rhi::BackendType::d3d12
         ? "d3d12" : "vulkan",
       static_cast<unsigned long long>(m_automationFrame));
}

inline void MinimalLatestApp::recordAutomationMetricsFrame()
{
  if(!m_automationMetricsWindowStarted)
  {
    return;
  }

  const demo::RuntimeProfileSnapshot snapshot =
    m_renderer.getRuntimeProfileSnapshot();
  if(!snapshot.cpuPassDurationsMs.empty())
  {
    const double cpuMs = std::accumulate(
      snapshot.cpuPassDurationsMs.begin(),
      snapshot.cpuPassDurationsMs.end(), 0.0,
      [](double total, double value) {
        return total + std::max(0.0, value);
      });
    m_automationCpuFrameMs.push_back(cpuMs);
  }
  if(snapshot.gpuValid && !snapshot.gpuPassDurationsMs.empty())
  {
    const double gpuMs = std::accumulate(
      snapshot.gpuPassDurationsMs.begin(),
      snapshot.gpuPassDurationsMs.end(), 0.0,
      [](double total, double value) {
        return total + std::max(0.0, value);
      });
    m_automationGpuFrameMs.push_back(gpuMs);
  }
}

inline void MinimalLatestApp::writeAutomationMetrics() const
{
  if(!m_automationMetricsWindowStarted ||
     m_automationOptions.metricsOutputPath.empty())
  {
    return;
  }

  struct MetricSummary
  {
    size_t count{0};
    double average{0.0};
    double minimum{0.0};
    double maximum{0.0};
    double p95{0.0};
  };
  const auto summarize = [](const std::vector<double>& values) {
    MetricSummary summary{};
    summary.count = values.size();
    if(values.empty())
    {
      return summary;
    }
    summary.average =
      std::accumulate(values.begin(), values.end(), 0.0) /
      static_cast<double>(values.size());
    const auto [minimum, maximum] =
      std::minmax_element(values.begin(), values.end());
    summary.minimum = *minimum;
    summary.maximum = *maximum;
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    const size_t p95Index = static_cast<size_t>(
      std::ceil(static_cast<double>(sorted.size()) * 0.95)) - 1u;
    summary.p95 = sorted[std::min(p95Index, sorted.size() - 1u)];
    return summary;
  };

  const MetricSummary cpu = summarize(m_automationCpuFrameMs);
  const MetricSummary gpu = summarize(m_automationGpuFrameMs);
  const demo::rhi::RHIHotPathCounters counters =
    m_renderer.getRhiHotPathCounters();
  const demo::EditorGpuInteractionDiagnostics interactionDiagnostics =
    m_renderer.editorInteractionDiagnostics();
  const char* backend =
    m_automationOptions.backend == demo::rhi::BackendType::d3d12
      ? "d3d12" : "vulkan";

  std::error_code filesystemError;
  const std::filesystem::path parent =
    m_automationOptions.metricsOutputPath.parent_path();
  if(!parent.empty())
  {
    std::filesystem::create_directories(parent, filesystemError);
    if(filesystemError)
    {
      throw std::runtime_error(
        "Could not create metrics output directory: " +
        filesystemError.message());
    }
  }

  std::ofstream output(
    m_automationOptions.metricsOutputPath,
    std::ios::out | std::ios::trunc);
  if(!output)
  {
    throw std::runtime_error(
      "Could not open metrics output: " +
      m_automationOptions.metricsOutputPath.string());
  }
  const auto writeSummary = [&output](
    const char* name, const MetricSummary& summary, bool comma) {
    output << "    " << std::quoted(name) << ": {"
           << std::quoted("samples") << ": " << summary.count
           << ", " << std::quoted("average_ms") << ": " << summary.average
           << ", " << std::quoted("min_ms") << ": " << summary.minimum
           << ", " << std::quoted("max_ms") << ": " << summary.maximum
           << ", " << std::quoted("p95_ms") << ": " << summary.p95 << "}"
           << (comma ? "," : "") << std::endl;
  };

  output << std::fixed << std::setprecision(6)
         << '{' << std::endl
         << "  " << std::quoted("schema") << ": "
         << std::quoted("mgif-rhi-stable-window-v1") << ',' << std::endl
         << "  " << std::quoted("backend") << ": "
         << std::quoted(backend) << ',' << std::endl
         << "  " << std::quoted("automation_mode") << ": "
         << std::quoted(automationModeName()) << ',' << std::endl
         << "  " << std::quoted("warmup_frames") << ": "
         << m_automationOptions.warmupFrames << ',' << std::endl
         << "  " << std::quoted("timings") << ": {" << std::endl;
  writeSummary("cpu_pass_sum", cpu, true);
  writeSummary("gpu_pass_sum", gpu, false);
  output << "  }," << std::endl
         << "  " << std::quoted("hot_path_counters") << ": {" << std::endl
         << "    " << std::quoted("command_buffer_begins") << ": "
         << counters.commandBufferBegins << ',' << std::endl
         << "    " << std::quoted("encoder_begins") << ": "
         << counters.encoderBegins << ',' << std::endl
         << "    " << std::quoted("queue_submits") << ": "
         << counters.queueSubmits << ',' << std::endl
         << "    " << std::quoted("submitted_command_buffers") << ": "
         << counters.submittedCommandBuffers << ',' << std::endl
         << "    " << std::quoted("descriptor_updates") << ": "
         << counters.descriptorUpdates << ',' << std::endl
         << "    " << std::quoted("descriptor_allocations") << ": "
         << counters.descriptorAllocations << ',' << std::endl
         << "    " << std::quoted("table_version_allocations") << ": "
         << counters.tableVersionAllocations << ',' << std::endl
         << "    " << std::quoted("pipeline_creations") << ": "
         << counters.pipelineCreations << ',' << std::endl
         << "    " << std::quoted("argument_layout_creations") << ": "
         << counters.argumentLayoutCreations << ',' << std::endl
         << "    " << std::quoted("texture_view_creations") << ": "
         << counters.textureViewCreations << ',' << std::endl
         << "    " << std::quoted("command_recording_heap_allocations") << ": "
         << counters.commandRecordingHeapAllocations << ',' << std::endl
         << "    " << std::quoted("command_recording_native_object_creations") << ": "
         << counters.commandRecordingNativeObjectCreations << std::endl
         << "  }," << std::endl
         << "  " << std::quoted("stable_recording_budget_met") << ": "
         << (counters.stableRecordingBudgetMet() ? "true" : "false")
         << (m_p6NativeEnabled ? "," : "") << std::endl;
  if(m_p6NativeEnabled)
  {
    const auto completionName = [](demo::editor::PickCompletion completion) {
      switch(completion)
      {
      case demo::editor::PickCompletion::hit: return "hit";
      case demo::editor::PickCompletion::miss: return "miss";
      case demo::editor::PickCompletion::error: return "error";
      case demo::editor::PickCompletion::cancelled: return "cancelled";
      }
      return "unknown";
    };
    const auto intentName = [](demo::editor::PickIntent intent) {
      return intent == demo::editor::PickIntent::doubleFocus
                 ? "doubleFocus" : "singleSelect";
    };
    output << "  " << std::quoted("p6_native") << ": {" << std::endl
           << "    " << std::quoted("case") << ": "
           << std::quoted(m_p6NativeCase) << "," << std::endl
           << "    " << std::quoted("fixture") << ": "
           << std::quoted(m_p6NativeFixturePath.string()) << "," << std::endl
           << "    " << std::quoted("completed") << ": "
           << (m_p6NativeStage == P6NativeInteractionStage::complete
                   ? "true" : "false") << "," << std::endl
           << "    " << std::quoted("submitted") << ": "
           << m_p6NativeSubmitted << "," << std::endl
           << "    " << std::quoted("terminal_count") << ": "
           << m_p6NativeTerminals.size() << "," << std::endl
           << "    " << std::quoted("selection_effects") << ": "
           << m_p6NativeSelectionEffects << "," << std::endl
           << "    " << std::quoted("camera_effects") << ": "
           << m_p6NativeCameraEffects << "," << std::endl
           << "    " << std::quoted("highlight_updates") << ": "
           << m_p6NativeHighlightUpdates << "," << std::endl
           << "    " << std::quoted("browser_focus_events") << ": "
           << m_p6NativeBrowserFocusEvents << "," << std::endl
           << "    " << std::quoted("initial_selection_revision") << ": "
           << m_p6NativeInitialSelectionRevision << "," << std::endl
           << "    " << std::quoted("final_selection_revision") << ": "
           << (m_editorController ? m_editorController->selectionRevision() : 0u)
           << "," << std::endl
           << "    " << std::quoted("initial_view_revision") << ": "
           << m_p6NativeInitialViewRevision << "," << std::endl
           << "    " << std::quoted("final_view_revision") << ": "
           << m_editorViewRevision << "," << std::endl
           << "    " << std::quoted("initial_move_speed") << ": "
           << m_p6NativeInitialMoveSpeed << "," << std::endl
           << "    " << std::quoted("final_move_speed") << ": "
           << m_moveSpeed << "," << std::endl
           << "    " << std::quoted("gpu_occurrence_passes") << ": "
           << interactionDiagnostics.occurrencePassExecutions << "," << std::endl
           << "    " << std::quoted("gpu_highlight_passes") << ": "
           << interactionDiagnostics.highlightPassExecutions << "," << std::endl
           << "    " << std::quoted("gpu_highlight_occurrence_count") << ": "
           << interactionDiagnostics.lastHighlightOccurrenceCount << "," << std::endl
           << "    " << std::quoted("gpu_highlight_selection_revision") << ": "
           << interactionDiagnostics.lastHighlightSelectionRevision << "," << std::endl
           << "    " << std::quoted("terminals") << ": [" << std::endl;
    for(std::size_t index = 0u; index < m_p6NativeTerminals.size(); ++index)
    {
      const demo::editor::EditorPickResult &terminal = m_p6NativeTerminals[index];
      output << "      {" << std::quoted("serial") << ": " << terminal.serial
             << ", " << std::quoted("intent") << ": "
             << std::quoted(intentName(terminal.intent))
             << ", " << std::quoted("completion") << ": "
             << std::quoted(completionName(terminal.completion))
             << ", " << std::quoted("reason") << ": "
             << static_cast<unsigned>(terminal.reason)
             << ", " << std::quoted("occurrence_slot") << ": "
             << (terminal.occurrence ? terminal.occurrence->slot : 0u)
             << "}" << (index + 1u == m_p6NativeTerminals.size() ? "" : ",")
             << std::endl;
    }
    output << "    ]" << std::endl << "  }" << std::endl;
  }
  output << '}' << std::endl;
  output.flush();
  if(!output)
  {
    throw std::runtime_error(
      "Could not write metrics output: " +
      m_automationOptions.metricsOutputPath.string());
  }
	LOGI("[RHI_METRICS] marker=written backend=%s output=%s",
	     backend, m_automationOptions.metricsOutputPath.string().c_str());
}

inline void MinimalLatestApp::writeEditorVisualState() const
{
  if(!m_editorVisualEnabled || m_automationOptions.editorVisualStatePath.empty())
  {
    return;
  }
  std::error_code filesystemError;
  const auto parent = m_automationOptions.editorVisualStatePath.parent_path();
  if(!parent.empty())
  {
    std::filesystem::create_directories(parent, filesystemError);
    if(filesystemError)
    {
      throw std::runtime_error(
          "Could not create editor visual state directory: " +
          filesystemError.message());
    }
  }
  std::ofstream output(m_automationOptions.editorVisualStatePath,
                       std::ios::out | std::ios::trunc);
  if(!output)
  {
    throw std::runtime_error(
        "Could not open editor visual state: " +
        m_automationOptions.editorVisualStatePath.string());
  }

  const auto *projection =
      m_editorController != nullptr ? m_editorController->projection() : nullptr;
  const auto selection = m_editorController != nullptr
                             ? m_editorController->sceneSelection().value()
                             : demo::editor::SceneSelectionValue{};
  const std::optional<demo::editor::ui::FrameModel> visualUiModel =
      m_editorController != nullptr
          ? std::optional<demo::editor::ui::FrameModel>{
                m_editorController->buildFrameModel()}
          : std::nullopt;
  const auto *overlay =
      m_editorGizmoAdapter != nullptr &&
              m_editorGizmoAdapter->snapshot().has_value()
          ? &*m_editorGizmoAdapter->snapshot()
          : nullptr;
#ifndef __ANDROID__
  const auto rhiOverlayDiagnostics = m_renderer.editorOverlayDiagnostics();
#endif
  int windowWidth = 0;
  int windowHeight = 0;
  int framebufferWidth = 0;
  int framebufferHeight = 0;
  float contentScaleX = 1.0f;
  float contentScaleY = 1.0f;
  glfwGetWindowSize(m_window, &windowWidth, &windowHeight);
  glfwGetFramebufferSize(m_window, &framebufferWidth, &framebufferHeight);
  glfwGetWindowContentScale(m_window, &contentScaleX, &contentScaleY);
  const double observedFramebufferScaleX = windowWidth > 0
      ? static_cast<double>(framebufferWidth) / static_cast<double>(windowWidth)
      : 0.0;
  const double observedFramebufferScaleY = windowHeight > 0
      ? static_cast<double>(framebufferHeight) / static_cast<double>(windowHeight)
      : 0.0;
  const bool syntheticOffscreenScale =
      std::abs(m_editorViewportMetrics.framebufferScale.x -
               observedFramebufferScaleX) > 0.01 ||
      std::abs(m_editorViewportMetrics.framebufferScale.y -
               observedFramebufferScaleY) > 0.01;
  const demo::editor::LogicalViewportPoint mappingLogical{
      m_editorViewportMetrics.logical.origin.x +
          m_editorViewportMetrics.logical.extent.width * 0.5,
      m_editorViewportMetrics.logical.origin.y +
          m_editorViewportMetrics.logical.extent.height * 0.5};
  const auto mappingPhysical =
      m_editorViewportMetrics.logicalToPhysical(mappingLogical);
  const demo::SceneNode *visualNode = nullptr;
  if(projection != nullptr && m_editorVisualNode.has_value())
  {
    const auto index = projection->mapping.sceneAssetIndex(
        m_editorVisualNode->untyped());
    if(index.has_value() && *index < projection->asset.nodes.size())
    {
      visualNode = &projection->asset.nodes[*index];
    }
  }
  const auto writeJsonString = [&output](std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    output << '"';
    for (const unsigned char character : value) {
      switch (character) {
      case '"': output << "\\\""; break;
      case '\\': output << "\\\\"; break;
      case '\b': output << "\\b"; break;
      case '\f': output << "\\f"; break;
      case '\n': output << "\\n"; break;
      case '\r': output << "\\r"; break;
      case '\t': output << "\\t"; break;
      default:
        if (character < 0x20u)
          output << "\\u00" << hex[(character >> 4u) & 0xfu]
                 << hex[character & 0xfu];
        else
          output << static_cast<char>(character);
        break;
      }
    }
    output << '"';
  };
  const auto writeNumber = [&output](double value) {
    if (!std::isfinite(value)) {
      output << "null";
      return;
    }
    output << std::setprecision(17) << value;
  };
  const auto writeVec2 = [&output, &writeNumber](double x, double y) {
    output << '[';
    writeNumber(x);
    output << ',';
    writeNumber(y);
    output << ']';
  };
  const auto writeVec3 = [&output, &writeNumber](double x, double y, double z) {
    output << '[';
    writeNumber(x);
    output << ',';
    writeNumber(y);
    output << ',';
    writeNumber(z);
    output << ']';
  };
  const auto writeMatrix = [&output, &writeNumber](const demo::editor::OverlayMatrix4 &matrix) {
    output << '[';
    for(std::size_t index = 0u; index < matrix.elements.size(); ++index)
    {
      if(index != 0u) output << ',';
      writeNumber(matrix.elements[index]);
    }
    output << ']';
  };
  const auto writeObjectId = [&output](
                                demo::editor::EditorObjectId id) {
    if (!id.valid()) {
      output << "null";
      return;
    }
    output << "{\"kind\":"
           << static_cast<unsigned>(id.kind)
           << ",\"slot\":" << id.slot
           << ",\"generation\":" << id.generation
           << ",\"authority\":" << id.authority.value << '}';
  };
  const auto digestHex = [](const demo::editor::SceneDigest &digest) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2u);
    for (const std::uint8_t byte : digest) {
      result.push_back(hex[(byte >> 4u) & 0xfu]);
      result.push_back(hex[byte & 0xfu]);
    }
    return result;
  };
  const demo::editor::ui::WindowState visualWindows =
      m_editorController != nullptr
          ? m_editorController->ui().state().windows
          : demo::editor::ui::WindowState{};
  const demo::editor::ui::ViewportSettings visualViewport =
      m_editorController != nullptr
          ? m_editorController->ui().state().viewport
          : demo::editor::ui::ViewportSettings{};
  const auto projectComponentBounds = [&](const auto &component) {
    struct Bounds final {
      bool valid{false};
      double minX{0.0};
      double minY{0.0};
      double maxX{0.0};
      double maxY{0.0};
    } result;
    if(overlay == nullptr || !component.visible)
    {
      return result;
    }
    const auto multiply = [](const demo::editor::OverlayMatrix4 &matrix,
                             const std::array<double, 4> &value) {
      std::array<double, 4> result{};
      for(std::size_t row = 0u; row < 4u; ++row)
      {
        for(std::size_t column = 0u; column < 4u; ++column)
        {
          result[row] += matrix.elements[row * 4u + column] * value[column];
        }
      }
      return result;
    };
    const auto &bounds = component.range.localBounds;
    for(std::uint32_t corner = 0u; corner < 8u; ++corner)
    {
      const std::array<double, 4> local{
          (corner & 1u) != 0u ? bounds.maximum.x : bounds.minimum.x,
          (corner & 2u) != 0u ? bounds.maximum.y : bounds.minimum.y,
          (corner & 4u) != 0u ? bounds.maximum.z : bounds.minimum.z, 1.0};
      const auto world = multiply(component.model, local);
      const auto clip = multiply(overlay->viewProjection, world);
      if(!std::isfinite(clip[3]) || std::abs(clip[3]) <= 1.0e-12)
      {
        continue;
      }
      const double ndcX = clip[0] / clip[3];
      const double ndcY = clip[1] / clip[3];
      const double x = m_editorViewportMetrics.logical.origin.x +
                       (ndcX * 0.5 + 0.5) *
                           m_editorViewportMetrics.logical.extent.width;
      const double y = m_editorViewportMetrics.logical.origin.y +
                       (ndcY * 0.5 + 0.5) *
                           m_editorViewportMetrics.logical.extent.height;
      if(!std::isfinite(x) || !std::isfinite(y))
      {
        continue;
      }
      if(!result.valid)
      {
        result.valid = true;
        result.minX = result.maxX = x;
        result.minY = result.maxY = y;
      }
      else
      {
        result.minX = std::min(result.minX, x);
        result.minY = std::min(result.minY, y);
        result.maxX = std::max(result.maxX, x);
        result.maxY = std::max(result.maxY, y);
      }
    }
    return result;
  };

  struct VisualPickProbe final
  {
    bool valid{false};
    double logicalX{0.0};
    double logicalY{0.0};
    std::uint32_t classifiedComponent{
        static_cast<std::uint32_t>(demo::editor::GizmoComponent::none)};
  } pickProbe;
  const std::optional<std::uint32_t> probeTarget =
      m_automationOptions.editorVisualComponent.has_value()
          ? m_automationOptions.editorVisualComponent
          : (m_automationOptions.editorVisualCase != "V01-grid"
                 ? std::optional<std::uint32_t>{2u}
                 : std::nullopt);
  if(overlay != nullptr && overlay->gizmoVisible && probeTarget.has_value() &&
     *probeTarget < overlay->gizmo.components.size())
  {
    const std::uint32_t target = *probeTarget;
    const auto &component = overlay->gizmo.components[target];
    const auto bounds = projectComponentBounds(component);
    const auto inverseModel = demo::editor::invertOverlayMatrix(component.model);
    demo::editor::GizmoCameraSnapshot camera;
    camera.ndcYDown = m_automationOptions.backend == demo::rhi::BackendType::vulkan;
    camera.generation = {
        m_editorViewportMetrics.viewportGeneration,
        m_editorViewportMetrics.projection,
        m_editorViewportMetrics.view};
    const glm::mat4 inverse = glm::inverse(m_cameraUniforms.viewProjection);
    for(std::size_t row = 0u; row < 4u; ++row)
    {
      for(std::size_t column = 0u; column < 4u; ++column)
      {
        camera.inverseViewProjection.elements[row * 4u + column] =
            static_cast<double>(inverse[column][row]);
      }
    }
    if(bounds.valid && inverseModel.has_value())
    {
      const double minimumX = bounds.minX - 4.0;
      const double maximumX = bounds.maxX + 4.0;
      const double minimumDrawY = bounds.minY - 4.0;
      const double maximumDrawY = bounds.maxY + 4.0;
      const double centerX = (bounds.minX + bounds.maxX) * 0.5;
      const double centerDrawY = (bounds.minY + bounds.maxY) * 0.5;
      const double mirrorY = 2.0 * m_editorViewportMetrics.logical.origin.y +
                             m_editorViewportMetrics.logical.extent.height;
      const double centerLogicalY = camera.ndcYDown ? centerDrawY : mirrorY - centerDrawY;
      const int minimumLogicalX = static_cast<int>(std::floor(minimumX));
      const int maximumLogicalX = static_cast<int>(std::ceil(maximumX));
      const int minimumLogicalY =
          static_cast<int>(std::floor(camera.ndcYDown ? minimumDrawY : mirrorY - maximumDrawY));
      const int maximumLogicalY =
          static_cast<int>(std::ceil(camera.ndcYDown ? maximumDrawY : mirrorY - minimumDrawY));
      double bestScore = std::numeric_limits<double>::infinity();
      for(int logicalPixelY = minimumLogicalY;
          logicalPixelY <= maximumLogicalY; ++logicalPixelY)
      {
        const double logicalY = static_cast<double>(logicalPixelY);
        for(int logicalPixelX = minimumLogicalX;
            logicalPixelX <= maximumLogicalX; ++logicalPixelX)
        {
          const double logicalX = static_cast<double>(logicalPixelX);
          const auto ray = demo::editor::makeGizmoRay(
              {logicalX, logicalY}, m_editorViewportMetrics, camera);
          if(!ray.has_value()) continue;
          const auto localOrigin = demo::editor::transformOverlayPoint(
              *inverseModel, ray->origin);
          const auto localDirection = demo::editor::transformOverlayDirection(
              *inverseModel, ray->direction);
          if(!localOrigin.has_value() || !localDirection.has_value()) continue;
          const auto hit = demo::editor::classifyGizmoHit(
              {*localOrigin, *localDirection},
              demo::editor::canonicalGizmoGeometry(), {},
              editorVisualGizmoVisibility());
          if(!hit.hit() || demo::editor::gizmoComponentIndex(hit.component) != target)
          {
            continue;
          }
          const double dx = logicalX - centerX;
          const double dy = logicalY - centerLogicalY;
          const double score = std::abs(hit.missDistance) * 1000000.0 +
                               dx * dx + dy * dy;
          if(score < bestScore)
          {
            bestScore = score;
            pickProbe = {true, logicalX, logicalY, target};
          }
        }
      }
    }
  }
  double parentDeterminant = 0.0;
  bool parentInvertible = false;
  if(m_editorVisualParentWorldValid)
  {
    const auto &m = m_editorVisualParentWorld.elements;
    parentDeterminant =
        m[0] * (m[5] * m[10] - m[6] * m[9]) -
        m[1] * (m[4] * m[10] - m[6] * m[8]) +
        m[2] * (m[4] * m[9] - m[5] * m[8]);
    parentInvertible =
        demo::editor::invertOverlayMatrix(m_editorVisualParentWorld).has_value();
  }

  output << std::boolalpha << "{\n"
         << "  \"schema\": \"mgif.editor-visual-state.v2\",\n"
         << "  \"case\": ";
  writeJsonString(m_automationOptions.editorVisualCase);
  output << ",\n  \"variant\": ";
  writeJsonString(m_automationOptions.editorVisualVariant);
  output << ",\n  \"target_component\": ";
  if(m_automationOptions.editorVisualComponent.has_value())
  {
    output << *m_automationOptions.editorVisualComponent;
  }
  else
  {
    output << "null";
  }
  output << ",\n  \"requested_dpi\": ";
  writeNumber(static_cast<double>(m_automationOptions.editorVisualDpi));
  output << ",\n  \"frame\": " << m_automationFrame << ",\n"
         << "  \"boundary\": ";
  writeJsonString(m_editorVisualBoundary);
  output << ",\n"
         << "  \"viewport\": {\n"
         << "    \"logical_origin\": ";
  writeVec2(m_editorViewportMetrics.logical.origin.x,
            m_editorViewportMetrics.logical.origin.y);
  output << ",\n    \"logical_extent\": ";
  writeVec2(m_editorViewportMetrics.logical.extent.width,
            m_editorViewportMetrics.logical.extent.height);
  output << ",\n    \"window_logical_extent\": [" << windowWidth << ','
         << windowHeight << "],\n"
         << "    \"window_framebuffer_extent\": [" << framebufferWidth << ','
         << framebufferHeight << "],\n"
         << "    \"observed_window_framebuffer_scale\": ";
  writeVec2(observedFramebufferScaleX, observedFramebufferScaleY);
  output << ",\n    \"observed_window_content_scale\": ";
  writeVec2(contentScaleX, contentScaleY);
  output << ",\n    \"physical_extent\": ["
         << m_editorViewportMetrics.physical.extent.width << ','
         << m_editorViewportMetrics.physical.extent.height << "],\n"
         << "    \"framebuffer_scale\": ";
  writeVec2(m_editorViewportMetrics.framebufferScale.x,
            m_editorViewportMetrics.framebufferScale.y);
  output << ",\n    \"framebuffer_scale_source\": ";
  writeJsonString(syntheticOffscreenScale ? "synthetic-offscreen"
                                          : "native-window");
  output << ",\n    \"mapping_probe\": {\"logical\": ";
  writeVec2(mappingLogical.x, mappingLogical.y);
  output << ",\"physical_valid\":" << mappingPhysical.has_value();
  if(mappingPhysical.has_value())
  {
    output << ",\"physical\":[" << mappingPhysical->x << ','
           << mappingPhysical->y << "],\"local\":["
           << mappingPhysical->localX << ',' << mappingPhysical->localY << ']';
  }
  output << '}';
  output << ",\n    \"depth_physical_origin\": ["
         << m_editorViewportMetrics.depthPhysical.origin.x << ','
         << m_editorViewportMetrics.depthPhysical.origin.y << "],\n"
         << "    \"depth_physical_extent\": ["
         << m_editorViewportMetrics.depthPhysical.extent.width << ','
         << m_editorViewportMetrics.depthPhysical.extent.height << "],\n"
         << "    \"color_attachment_extent\": ["
         << m_editorViewportMetrics.colorAttachment.width << ','
         << m_editorViewportMetrics.colorAttachment.height << "],\n"
         << "    \"depth_attachment_extent\": ["
         << m_editorViewportMetrics.depthAttachment.width << ','
         << m_editorViewportMetrics.depthAttachment.height << "]";
  output << ",\n    \"viewport_generation\": "
         << m_editorViewportMetrics.viewportGeneration << ",\n"
         << "    \"projection_revision\": "
         << m_editorViewportMetrics.projection.revision << ",\n"
         << "    \"view_revision\": "
         << m_editorViewportMetrics.view.revision << ",\n"
         << "    \"surface_transform_token\": "
         << static_cast<unsigned>(m_editorViewportMetrics.surfaceTransform)
         << ",\n"
         << "    \"projection_kind\": ";
  writeJsonString(m_editorViewportMetrics.projectionKind ==
                          demo::editor::EditorProjectionKind::orthographic
                      ? "orthographic" : "perspective");
  output << "\n  },\n"
         << "  \"document\": {\n"
         << "    \"revision\": "
         << (projection != nullptr ? projection->documentRevision : 0u) << ",\n"
         << "    \"mapping_epoch\": "
         << (projection != nullptr ? projection->mappingEpoch : 0u) << ",\n"
         << "    \"selection_revision\": "
         << (m_editorController != nullptr
                 ? m_editorController->selectionRevision()
                 : 0u)
         << ",\n    \"document_digest\": ";
  writeJsonString(projection != nullptr ? digestHex(projection->documentDigest)
                                        : std::string{});
  output << ",\n    \"projection_digest\": ";
  writeJsonString(projection != nullptr ? digestHex(projection->projectionDigest)
                                        : std::string{});
  output << ",\n    \"runtime_content_digest\": ";
  writeJsonString(projection != nullptr
                      ? digestHex(projection->immutableSnapshot.contentDigest)
                      : std::string{});
  output << "\n  },\n"
         << "  \"selection\": {\n"
         << "    \"kind\": "
         << static_cast<unsigned>(demo::editor::sceneSelectionKind(selection))
         << ",\n"
         << "    \"revision\": "
         << (m_editorController != nullptr
                 ? m_editorController->selectionRevision()
                 : 0u)
         << "\n  },\n"
         << "  \"selected_node_transform\": {\n"
         << "    \"available\": " << (visualNode != nullptr);
  if(visualNode != nullptr)
  {
    output << ",\n    \"translation\": ";
    writeVec3(visualNode->translation.x, visualNode->translation.y,
              visualNode->translation.z);
    output << ",\n    \"rotation\": [";
    writeNumber(visualNode->rotation.x);
    output << ',';
    writeNumber(visualNode->rotation.y);
    output << ',';
    writeNumber(visualNode->rotation.z);
    output << ',';
    writeNumber(visualNode->rotation.w);
    output << "],\n    \"scale\": ";
    writeVec3(visualNode->scale.x, visualNode->scale.y, visualNode->scale.z);
  }
  output << "\n  },\n"
         << "  \"history\": {\n"
         << "    \"undo\": "
         << (m_editorController != nullptr && m_editorController->session() != nullptr
                 ? m_editorController->session()->editor().history().undoCount()
                 : 0u)
         << ",\n    \"redo\": "
         << (m_editorController != nullptr && m_editorController->session() != nullptr
                 ? m_editorController->session()->editor().history().redoCount()
                 : 0u)
         << ",\n    \"can_undo\": "
         << (m_editorController != nullptr && m_editorController->session() != nullptr
                 ? m_editorController->session()->editor().history().canUndo()
                 : false)
         << ",\n    \"can_redo\": "
         << (m_editorController != nullptr && m_editorController->session() != nullptr
                 ? m_editorController->session()->editor().history().canRedo()
                 : false)
         << "\n  },\n"
         << "  \"identity\": {\n"
         << "    \"executable_sha256\": ";
  writeJsonString(m_automationOptions.editorVisualExecutableSha256);
  output << ",\n    \"source_sha256\": ";
  writeJsonString(m_automationOptions.editorVisualSourceSha256);
  output << "\n  },\n"
         << "  \"ui\": {\n"
         << "    \"lifecycle\": "
         << (visualUiModel ? static_cast<unsigned>(visualUiModel->lifecycle) : 0u)
         << ",\n    \"has_document\": "
         << (visualUiModel ? visualUiModel->hasDocument : false)
         << ",\n    \"mutation_enabled\": "
         << (visualUiModel ? visualUiModel->mutationEnabled : false)
         << ",\n    \"modal_pending\": "
         << (visualUiModel ? visualUiModel->modalPending : false)
         << ",\n    \"renderer_busy\": "
         << (visualUiModel ? visualUiModel->rendererBusy : false)
         << ",\n    \"browser_tab\": "
         << (m_editorController
                 ? static_cast<unsigned>(m_editorController->ui().browserTab())
                 : 0u)
         << ",\n    \"scene_browser_visible\": "
         << (m_editorController
                 ? m_editorController->ui().state().windows.sceneBrowser
                 : false)
         << ",\n    \"inspector_visible\": "
         << (m_editorController
                 ? m_editorController->ui().state().windows.inspector
                 : false)
         << ",\n    \"asset_version\": ";
  writeJsonString(visualUiModel ? visualUiModel->browser.asset.version
                                : std::string{});
  output << ",\n    \"asset_generator\": ";
  writeJsonString(visualUiModel ? visualUiModel->browser.asset.generator
                                : std::string{});
  output << ",\n    \"asset_extension_count\": "
         << (visualUiModel ? visualUiModel->browser.asset.extensions.size() : 0u)
         << ",\n    \"animation_count\": "
         << (visualUiModel ? visualUiModel->browser.animation.items.size() : 0u)
         << ",\n    \"variant_count\": "
         << (visualUiModel ? visualUiModel->browser.variants.size() : 0u)
         << ",\n    \"graph_row_count\": "
         << (visualUiModel ? visualUiModel->browser.graphRows.size() : 0u)
         << ",\n    \"resource_group_count\": "
         << (visualUiModel ? visualUiModel->browser.resourceGroups.size() : 0u)
         << ",\n    \"inspector_section_count\": "
         << (visualUiModel ? visualUiModel->inspector.sections.size() : 0u)
         << ",\n    \"command_count\": "
         << demo::editor::ui::commandDescriptors().size()
         << ",\n    \"commands\": [";
  const auto commandDescriptors = demo::editor::ui::commandDescriptors();
  for (std::size_t index = 0u; index < commandDescriptors.size(); ++index) {
    if (index != 0u)
      output << ',';
    const auto &descriptor = commandDescriptors[index];
    const demo::editor::ui::CommandEnablement enablement =
        visualUiModel
            ? demo::editor::ui::evaluateCommand(
                  descriptor.id, *visualUiModel, visualWindows, visualViewport)
            : demo::editor::ui::CommandEnablement{};
    output << "{\"id\":" << static_cast<unsigned>(descriptor.id)
           << ",\"menu\":";
    writeJsonString(descriptor.menu);
    output << ",\"label\":";
    writeJsonString(descriptor.label);
    output << ",\"shortcut\":";
    writeJsonString(descriptor.shortcut);
    output << ",\"enabled\":" << enablement.enabled
           << ",\"checked\":" << enablement.checked << '}';
  }
  output << "],\n    \"scenes\": [";
  if (visualUiModel) {
    for (std::size_t index = 0u; index < visualUiModel->scenes.size(); ++index) {
      if (index != 0u)
        output << ',';
      const auto &scene = visualUiModel->scenes[index];
      output << "{\"id\":";
      writeObjectId(scene.id.untyped());
      output << ",\"document_index\":" << scene.documentIndex
             << ",\"name\":";
      writeJsonString(scene.name);
      output << ",\"current\":" << scene.current << '}';
    }
  }
  output << "],\n    \"graph_rows\": [";
  if (visualUiModel) {
    for (std::size_t index = 0u;
         index < visualUiModel->browser.graphRows.size(); ++index) {
      if (index != 0u)
        output << ',';
      const auto &row = visualUiModel->browser.graphRows[index];
      output << "{\"scene\":";
      writeObjectId(row.key.scene.untyped());
      output << ",\"object\":";
      writeObjectId(row.key.object);
      output << ",\"kind\":" << static_cast<unsigned>(row.kind)
             << ",\"document_index\":" << row.documentIndex
             << ",\"depth\":" << row.depth << ",\"label\":";
      writeJsonString(row.label);
      output << ",\"secondary\":";
      writeJsonString(row.secondary);
      output << ",\"selected\":" << row.selected
             << ",\"enabled\":" << row.enabled
             << ",\"effective_visible\":" << row.effectiveVisible
             << ",\"default_expanded\":" << row.defaultExpanded
             << ",\"accepts_node_drop\":" << row.acceptsNodeDrop << '}';
    }
  }
  output << "],\n    \"inspector\": {\"selection_kind\":"
         << (visualUiModel
                 ? static_cast<unsigned>(visualUiModel->inspector.selection.kind())
                 : 0u)
         << ",\"selection_revision\":"
         << (visualUiModel ? visualUiModel->inspector.selectionRevision : 0u)
         << ",\"sections\":[";
  if (visualUiModel) {
    for (std::size_t index = 0u;
         index < visualUiModel->inspector.sections.size(); ++index) {
      if (index != 0u)
        output << ',';
      const auto &section = visualUiModel->inspector.sections[index];
      output << "{\"kind\":" << static_cast<unsigned>(section.kind)
             << ",\"label\":";
      writeJsonString(section.label);
      output << ",\"default_open\":" << section.defaultOpen
             << ",\"fields\":[";
      for (std::size_t fieldIndex = 0u; fieldIndex < section.fields.size();
           ++fieldIndex) {
        if (fieldIndex != 0u)
          output << ',';
        const auto &field = section.fields[fieldIndex];
        output << "{\"id\":" << static_cast<unsigned>(field.id)
               << ",\"value_kind\":" << field.value.index()
               << ",\"visible\":" << field.visible
               << ",\"enabled\":" << field.enabled
               << ",\"read_only\":" << field.readOnly
               << ",\"target\":";
        if (field.target)
          writeObjectId(*field.target);
        else
          output << "null";
        output << '}';
      }
      output << "]}";
    }
  }
  output << "]},\n    \"action_log\":[{\"marker\":";
  writeJsonString(m_editorVisualBoundary);
  output << ",\"frame\":" << m_automationFrame
         << ",\"source\":\"visual-boundary\"}]\n  },\n"
         << "  \"fence\": {\n"
         << "    \"frame_serial\": "
         << (overlay != nullptr ? overlay->frameSerial : 0u) << ",\n"
         << "    \"document_revision\": "
         << (overlay != nullptr ? overlay->documentRevision : 0u) << ",\n"
         << "    \"selection_revision\": "
         << (overlay != nullptr ? overlay->selectionRevision : 0u) << ",\n"
         << "    \"mapping_epoch\": "
         << (overlay != nullptr ? overlay->mappingEpoch : 0u) << ",\n"
         << "    \"viewport_generation\": "
         << (overlay != nullptr ? overlay->viewport.viewportGeneration : 0u) << ",\n"
         << "    \"projection_revision\": "
         << (overlay != nullptr ? overlay->viewport.projection.revision : 0u) << ",\n"
         << "    \"view_revision\": "
         << (overlay != nullptr ? overlay->viewport.view.revision : 0u)
         << ",\n    \"gpu_idle_waited\": "
         << m_editorVisualGpuIdleWaited << "\n  },\n"
#ifndef __ANDROID__
         << "  \"rhi_overlay\": {\n"
         << "    \"available\": " << m_renderer.editorOverlayAvailable() << ",\n"
         << "    \"prepare_calls\": " << rhiOverlayDiagnostics.prepareCalls << ",\n"
         << "    \"recorded_frames\": " << rhiOverlayDiagnostics.recordedFrames << ",\n"
         << "    \"submitted_frames\": " << rhiOverlayDiagnostics.submittedFrames << ",\n"
         << "    \"completed_frames\": " << rhiOverlayDiagnostics.completedFrames << ",\n"
         << "    \"grid_draws\": " << rhiOverlayDiagnostics.gridDraws << ",\n"
         << "    \"axes_draws\": " << rhiOverlayDiagnostics.axesDraws << ",\n"
         << "    \"gizmo_draws\": " << rhiOverlayDiagnostics.gizmoDraws << ",\n"
         << "    \"rejected_frames\": " << rhiOverlayDiagnostics.rejectedFrames << ",\n"
         << "    \"failures_before_submit\": " << rhiOverlayDiagnostics.failuresBeforeSubmit << ",\n"
         << "    \"failures_after_submit\": " << rhiOverlayDiagnostics.failuresAfterSubmit << ",\n"
         << "    \"native_escape_calls\": " << rhiOverlayDiagnostics.nativeEscapeCalls << ",\n"
         << "    \"last_recording_stage\": "
         << static_cast<unsigned>(rhiOverlayDiagnostics.lastRecordingStage) << ",\n"
         << "    \"last_error\": " << static_cast<unsigned>(rhiOverlayDiagnostics.lastError) << "\n  },\n"
#endif
         << "  \"overlay\": {\n"
         << "    \"present\": " << (overlay != nullptr);
  if(overlay == nullptr)
  {
    output << "\n  },\n  \"marker\": \"visual-ready\"\n}\n";
  }
  else
  {
    output << ",\n    \"frame_serial\": " << overlay->frameSerial
           << ",\n    \"grid_visible\": " << overlay->gridVisible
           << ",\n    \"grid_line_count\": " << overlay->gridLines.size()
           << ",\n    \"grid_axis_count\": " << overlay->gridAxes.size()
           << ",\n    \"grid\": {\n"
           << "      \"base_unit\": ";
    writeNumber(overlay->grid.baseUnit);
    output << ",\n      \"distance_to_floor\": ";
    writeNumber(overlay->grid.distanceToFloor);
    output << ",\n      \"center\": ";
    writeVec2(overlay->grid.center.x, overlay->grid.center.y);
    output << ",\n      \"decade_steps\": [";
    for(std::size_t index = 0u; index < overlay->grid.decadeSteps.size(); ++index)
    {
      if(index != 0u) output << ',';
      writeNumber(overlay->grid.decadeSteps[index]);
    }
    output << "],\n      \"active_lods\": [";
    for(std::size_t index = 0u; index < overlay->grid.activeLods.size(); ++index)
    {
      if(index != 0u) output << ',';
      const auto &lod = overlay->grid.activeLods[index];
      output << "{\"level\":" << static_cast<unsigned>(lod.levelIndex)
             << ",\"step\":";
      writeNumber(lod.step);
      output << ",\"alpha\":";
      writeNumber(lod.alpha);
      output << '}';
    }
    output << "]\n    },\n"
           << "    \"gizmo_visible\": " << overlay->gizmoVisible
           << ",\n    \"gizmo_depth_token\": "
           << static_cast<unsigned>(overlay->gizmo.depth)
           << ",\n    \"gizmo_plane_token\": "
           << static_cast<unsigned>(overlay->gizmo.plane)
           << ",\n    \"gizmo_snap_token\": "
           << static_cast<unsigned>(overlay->gizmo.snap)
           << ",\n    \"gizmo_basis_token\": "
           << static_cast<unsigned>(overlay->gizmo.basis)
           << ",\n    \"gizmo_phase\": "
           << (m_editorGizmoAdapter != nullptr
                   ? static_cast<unsigned>(m_editorGizmoAdapter->phase()) : 0u)
           << ",\n    \"gizmo_pointer_captured\": "
           << (m_editorGizmoAdapter != nullptr &&
               m_editorGizmoAdapter->pointerCaptured())
           << ",\n    \"gizmo_hovered_component\": "
           << (m_editorGizmoAdapter != nullptr
                   ? static_cast<unsigned>(
                         m_editorGizmoAdapter->hoveredComponent())
                   : static_cast<unsigned>(demo::editor::GizmoComponent::none))
           << ",\n    \"gizmo_world_scale\": ";
    writeNumber(overlay->gizmo.worldScale);
    output << ",\n    \"gizmo_canonical_visual_diameter\": ";
    writeNumber(overlay->gizmo.canonicalVisualDiameter);
    output << ",\n    \"parent_world_valid\": "
           << m_editorVisualParentWorldValid
           << ",\n    \"parent_determinant\": ";
    writeNumber(parentDeterminant);
    output << ",\n    \"parent_invertible\": " << parentInvertible
           << ",\n    \"parent_world\": ";
    writeMatrix(m_editorVisualParentWorld);
    output << ",\n    \"pick_probe\": {\"valid\":" << pickProbe.valid
           << ",\"logical_x\":";
    writeNumber(pickProbe.logicalX);
    output << ",\"logical_y\":";
    writeNumber(pickProbe.logicalY);
    output << ",\"classified_component\":"
           << pickProbe.classifiedComponent << "},\n"
           << "    \"gizmo_components\": [";
    for(std::size_t index = 0u; index < overlay->gizmo.components.size();
        ++index)
    {
      if(index != 0u) output << ',';
      const auto &component = overlay->gizmo.components[index];
      const auto bounds = projectComponentBounds(component);
      output << "{\"index\": " << index
             << ", \"visible\": " << component.visible
             << ", \"vertices\": " << component.range.vertexCount
             << ", \"indices\": " << component.range.indexCount
             << ", \"state\": "
             << static_cast<unsigned>(component.state)
             << ", \"screen_bounds\": {\"valid\": " << bounds.valid
             << ", \"x\": ";
      writeNumber(bounds.minX);
      output << ", \"y\": ";
      writeNumber(bounds.minY);
      output << ", \"width\": ";
      writeNumber(bounds.maxX - bounds.minX);
      output << ", \"height\": ";
      writeNumber(bounds.maxY - bounds.minY);
      output << "}}";
    }
    output << "]\n  },\n  \"marker\": \"visual-ready\"\n}\n";
  }
  output.flush();
  if(!output)
  {
    throw std::runtime_error("Could not write editor visual state");
  }
  LOGI("[EDITOR_VISUAL] marker=state-written case=%s frame=%llu path=%s",
       m_automationOptions.editorVisualCase.c_str(),
       static_cast<unsigned long long>(m_automationFrame),
       m_automationOptions.editorVisualStatePath.string().c_str());
}

inline void MinimalLatestApp::onAutomationFrameRendered()
{
  if(!isAutomationEnabled())
  {
    return;
  }

  if(!m_automationSceneReadyObserved)
  {
    const bool hasPublishedScene = m_editorRendererAdapter.initialized() &&
                                   m_projectedSceneAsset != nullptr &&
                                   !m_editorRendererAdapter.hasPending();
    const bool productionStartupSceneReady =
        !m_automationOptions.editorStartupScene.empty() &&
        m_editorController != nullptr && m_editorController->hasDocument() &&
        m_projectedSceneAsset != nullptr &&
        !m_editorRendererAdapter.hasPending();
    const std::string_view publishedSource =
        m_projectedSceneAsset ? std::string_view(m_projectedSceneAsset->sourcePath)
                              : std::string_view{};
    if((hasPublishedScene || productionStartupSceneReady) &&
       (productionStartupSceneReady ||
        (
#if defined(MGIF_EDITOR_OS_REPLAY)
           m_editorOsReplayEnabled
               ? demo::editor::automation::sameAssetIdentity(
                     m_editorOsReplayFixturePath.string(), publishedSource)
               :
#endif
#if defined(MGIF_EDITOR_LIVE_REPLAY)
           m_editorLiveEnabled
               ? demo::editor::automation::sameAssetIdentity(
                     m_editorLiveFixturePath.string(), publishedSource)
               :
#endif
           m_editorVisualEnabled
           ? demo::editor::automation::sameAssetIdentity(
                 m_editorVisualFixturePath.string(), publishedSource)
           : m_p6NativeEnabled
               ? demo::editor::automation::sameAssetIdentity(
                     m_p6NativeFixturePath.string(), publishedSource)
               : !m_automationOptions.editorStartupScene.empty()
                   ? demo::editor::automation::sameAssetIdentity(
                         m_automationOptions.editorStartupScene.string(),
                         publishedSource)
                   : demo::editor::automation::isSceneReadyModel(publishedSource))))
    {
      if(isEditorTransactionEnabled()
         && !m_editorTransactionSmoke.observeSceneReady(
             m_editorController != nullptr
                 && m_editorController->session() != nullptr
                 && m_editorController->projection() != nullptr,
             hasPublishedScene && m_editorRendererAdapter.initialized(),
             !m_editorRendererAdapter.hasPending()))
      {
        failEditorTransaction(m_editorTransactionSmoke.failure());
      }
      m_automationSceneReadyObserved = true;
#if defined(MGIF_EDITOR_LIVE_REPLAY)
      if(m_editorLiveEnabled)
      {
        initializeEditorLiveReplay();
        LOGI("[EDITOR_LIVE_REPLAY] marker=scene-ready case=%s",
             m_automationOptions.editorLiveCase.c_str());
      }
#endif
      if(m_editorVisualEnabled)
      {
        m_editorVisualStage = EditorVisualStage::waitingProjection;
        LOGI("[EDITOR_VISUAL] marker=scene-ready case=%s",
             m_automationOptions.editorVisualCase.c_str());
      }
      if(isEditorTransactionEnabled())
      {
        m_editorTransactionDeadlineStarted = true;
        m_editorTransactionStartedAt = std::chrono::steady_clock::now();
      }
      std::string automationSceneModel =
          m_editorVisualEnabled
              ? m_editorVisualFixturePath.string()
              : m_p6NativeEnabled
                    ? m_p6NativeFixturePath.string()
                    : std::string(
                          demo::editor::automation::sceneReadyMarkerModel());
#if defined(MGIF_EDITOR_OS_REPLAY)
      if(m_editorOsReplayEnabled)
      {
        automationSceneModel = m_editorOsReplayFixturePath.string();
        LOGI("[EDITOR_OS_REPLAY] marker=scene-ready subcase=%s",
             m_automationOptions.editorOsReplay.subcase.c_str());
      }
#endif
#if defined(MGIF_EDITOR_LIVE_REPLAY)
      if(m_editorLiveEnabled)
      {
        automationSceneModel = m_editorLiveFixturePath.string();
      }
#endif
      LOGI("[CSM_AUTOMATION] marker=scene-ready mode=%s model=%s",
           automationModeName(), automationSceneModel.c_str());
    }
    return;
  }

  if(!m_automationStarted || m_automationComplete)
  {
    return;
  }

#if defined(MGIF_EDITOR_LIVE_REPLAY)
  if(m_editorLiveEnabled &&
     m_editorLiveStage == EditorLiveReplayStage::markerPending)
  {
    onEditorLiveFrameRendered();
    if(m_editorLiveStage == EditorLiveReplayStage::complete)
    {
      return;
    }
  }
#endif

  if(isEditorTransactionEnabled()
     && !m_editorTransactionSmoke.observeRenderedFrame(m_automationFrame))
  {
    failEditorTransaction(m_editorTransactionSmoke.failure());
  }

  if(const char* boundary = automationBoundaryMarkerForFrame(m_automationFrame); boundary != nullptr)
  {
    logAutomationMarker(boundary);
  }

#if defined(MGIF_EDITOR_LIVE_REPLAY)
  if(m_editorLiveEnabled)
  {
    m_automationPreviousPose = m_automationCurrentPose;
    if(m_automationFrame == std::numeric_limits<std::uint64_t>::max())
    {
      failEditorLiveReplay("live replay frame counter is exhausted");
    }
    ++m_automationFrame;
    return;
  }
#endif

#if defined(MGIF_EDITOR_OS_REPLAY)
  if(m_editorOsReplayEnabled)
  {
    m_automationPreviousPose = m_automationCurrentPose;
    if(m_automationFrame == std::numeric_limits<std::uint64_t>::max())
    {
      failEditorOsReplay("OS replay frame counter is exhausted");
    }
    ++m_automationFrame;
    return;
  }
#endif

  m_automationPreviousPose = m_automationCurrentPose;
  if(m_automationFrame == automationTotalFrames() - 1u)
  {
    if(m_p6NativeEnabled &&
       m_p6NativeStage != P6NativeInteractionStage::complete)
    {
      failP6NativeInteraction(
          m_p6NativeFailure.empty()
              ? "native interaction did not complete before the fixed frame budget"
              : m_p6NativeFailure);
    }
    m_automationComplete = true;
    writeAutomationMetrics();
    LOGI("[CSM_AUTOMATION] marker=complete mode=%s frames=%llu",
         automationModeName(),
         static_cast<unsigned long long>(automationTotalFrames()));
    if(isEditorTransactionEnabled()
       && m_editorTransactionSmoke.phase()
              == demo::editor::automation::EditorTransactionSmoke::Phase::complete)
    {
      LOGI("[CSM_AUTOMATION] marker=frame-budget-complete mode=%s frames=40",
           automationModeName());
      LOGI("[CSM_AUTOMATION] marker=automation-complete mode=%s frames=40 auto_exit=1",
           automationModeName());
    }
    if(isEditorTransactionEnabled()
       && m_editorTransactionSmoke.phase()
              != demo::editor::automation::EditorTransactionSmoke::Phase::complete)
    {
      failEditorTransaction("editor transaction did not complete its 40-frame budget");
    }
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
  if(m_editorForceFreeCamera)
  {
    return false;
  }

  if(!m_projectedSceneAsset) return false;
  std::span<const demo::SceneCamera> cameras(m_projectedSceneAsset->cameras);
  if(m_editorDocumentCameraIndex.has_value()
     && *m_editorDocumentCameraIndex < cameras.size())
  {
    cameras = cameras.subspan(*m_editorDocumentCameraIndex, 1);
  }
  return demo::populatePrimarySceneCameraUniforms(
      cameras, m_projectedSceneAsset->nodes,
      std::span<const demo::GltfNodeData>{}, m_camera.getProjectionMatrix(),
      uniforms, m_camera.getProjectionConvention());
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
    glm::mat4 projection = m_camera.getProjectionMatrix();
    if(editorVisualUsesOrthographicProjection())
    {
      const float aspect = m_viewportSize.height > 0u
          ? static_cast<float>(m_viewportSize.width) /
                static_cast<float>(m_viewportSize.height)
          : 1.0f;
      constexpr float halfVerticalSpan = 3.0f;
      projection = demo::clipspace::makeOrthographicProjection(
          -halfVerticalSpan * aspect, halfVerticalSpan * aspect,
          -halfVerticalSpan, halfVerticalSpan, 0.1f, 100.0f,
          m_camera.getProjectionConvention());
    }
    demo::populateCameraUniforms(m_camera.getViewMatrix(), projection,
                                 m_camera.getPosition(), m_cameraUniforms);
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

inline demo::DirectionalLightSettings MinimalLatestApp::resolveSceneLightSettings() const
{
  demo::DirectionalLightSettings settings = m_lightSettings;

  if(m_projectedSceneAsset)
  {
    if(demo::applyPrimarySceneDirectionalLight(
           m_projectedSceneAsset->lights,
           m_projectedSceneAsset->nodes,
           std::span<const demo::GltfNodeData>{},
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

inline void MinimalLatestApp::openModelWithEditor(const std::string& path)
{
  // Let the controller validate and prepare the candidate before touching the
  // active renderer publication.  A failed parse/projection must leave the
  // current document and its renderer generation visible; a successful full
  // publication performs the renderer reset atomically at its flush boundary.
  if(!m_editorController)
  {
    reportEditorError("Editor controller is unavailable");
    return;
  }

  std::string error;
  if(!m_editorController->open(path, &error))
  {
    reportEditorError(error);
    return;
  }
  m_editorForceFreeCamera = false;
  m_editorDocumentCameraIndex.reset();
}

inline void MinimalLatestApp::handleDroppedScene(
    const std::filesystem::path& path, bool merge)
{
  if(!m_editorController || path.empty())
  {
    return;
  }
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  if(extension != ".gltf" && extension != ".glb")
  {
    reportEditorError("Dropped file is not a glTF or GLB scene");
    return;
  }
  const demo::editor::ui::CommandId commandId =
      merge ? demo::editor::ui::CommandId::importMerge
            : demo::editor::ui::CommandId::open;
  const demo::editor::ui::RuntimeActionResult result =
      m_editorController->dispatch(demo::editor::ui::UiCommand{
          0u, demo::editor::ui::CommandSource::menu,
          demo::editor::ui::InvokeCommand{commandId, path}});
  if(!result.accepted && !result.error.empty())
  {
    reportEditorError(result.error);
  }
}

inline void MinimalLatestApp::publishPendingEditorProjection()
{
  if(isEditorTransactionEnabled() && !m_editorTransactionEnqueueError.empty())
  {
    failEditorTransaction(m_editorTransactionEnqueueError);
  }
  while(m_editorRendererAdapter.hasPending())
  {
    demo::editor::ui::EditorRendererAdapter::FlushResult published =
        m_editorRendererAdapter.flush(m_renderer);
    if(!published.consumed)
    {
      break;
    }
    if(!published.accepted || !published.projection
       || (isEditorTransactionEnabled() && !published.resultingFence))
    {
      reportEditorError(published.message.empty()
                            ? "Renderer rejected the editor scene publication"
                            : published.message);
      if(isEditorTransactionEnabled())
      {
        failEditorTransaction(published.message.empty()
            ? "renderer rejected the editor scene publication"
            : published.message);
      }
      return;
    }

    if(isEditorTransactionEnabled())
    {
      observeEditorTransactionPublication(*published.projection,
                                          published.refresh,
                                          *published.resultingFence);
    }
    if(published.resultingFence)
    {
      const std::uint64_t mappingEpoch =
          published.resultingFence->mappingEpoch;
      m_editorViewportMetrics.projection = {
          mappingEpoch, published.projection->mappingEpoch};
    }

    demo::editor::SceneProjectionResult projection =
        std::move(*published.projection);
    const std::string sourcePath = projection.asset.sourcePath;
    m_projectedSceneAsset =
        std::make_shared<const demo::SceneAsset>(std::move(projection.asset));
    if(published.refresh == demo::editor::ui::ProjectionRefresh::fullScene)
    {
      m_editorForceFreeCamera = false;
      m_editorDocumentCameraIndex.reset();
    }
    if(!sourcePath.empty())
    {
      setMeshSDFPathFromModelPath(sourcePath);
    }
    m_editorError.clear();
    resetSceneCameraNavigation();
    refreshActiveCameraUniforms();
  }
}

inline void MinimalLatestApp::reportEditorError(std::string_view message)
{
  if(message.empty() || message == m_editorError)
  {
    return;
  }

  m_editorError.assign(message);
  LOGE("Editor: %s", m_editorError.c_str());
}

inline std::optional<std::filesystem::path> MinimalLatestApp::showEditorFileDialog(
    const demo::editor::ui::NativeDialogRequest& request)
{
#ifdef _WIN32
  std::array<wchar_t, 32768> fileName{};
  const std::wstring suggested = request.suggestedFile.wstring();
  const std::size_t suggestedLength =
      std::min(suggested.size(), fileName.size() - 1);
  std::copy_n(suggested.data(), suggestedLength, fileName.data());

  const std::wstring title(request.title.begin(), request.title.end());
  const std::wstring initialDirectory = request.initialDirectory.wstring();
  static constexpr wchar_t filter[] =
      L"glTF Scene (*.gltf;*.glb)\0*.gltf;*.glb\0All Files (*.*)\0*.*\0";

  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = glfwGetWin32Window(m_window);
  dialog.lpstrFilter = filter;
  dialog.lpstrFile = fileName.data();
  dialog.nMaxFile = static_cast<DWORD>(fileName.size());
  dialog.lpstrInitialDir = initialDirectory.empty() ? nullptr : initialDirectory.c_str();
  dialog.lpstrTitle = title.empty() ? nullptr : title.c_str();
  dialog.lpstrDefExt = L"gltf";
  dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;

  BOOL accepted = FALSE;
  if(request.kind == demo::editor::ui::NativeDialogKind::saveScene)
  {
    dialog.Flags |= OFN_OVERWRITEPROMPT;
    accepted = GetSaveFileNameW(&dialog);
  }
  else
  {
    dialog.Flags |= OFN_FILEMUSTEXIST;
    accepted = GetOpenFileNameW(&dialog);
  }

  if(accepted != FALSE)
  {
    return std::filesystem::path(fileName.data());
  }

  const DWORD dialogError = CommDlgExtendedError();
  if(dialogError != 0)
  {
    reportEditorError("Native file dialog failed with error "
                      + std::to_string(dialogError));
  }
#else
  (void)request;
#endif
  return std::nullopt;
}

inline demo::editor::ui::RuntimeActionResult MinimalLatestApp::handleEditorCameraRequest(
    const demo::editor::ui::CameraRequest& request)
{
  using demo::editor::ui::CameraRequestKind;
  using demo::editor::ui::ProjectionRefresh;
  using demo::editor::ui::RuntimeActionResult;

  if(request.kind == CameraRequestKind::setDocumentCameraFromView)
  {
    const demo::editor::SceneProjectionResult* projection =
        m_editorController ? m_editorController->projection() : nullptr;
    if(projection == nullptr || !request.camera.has_value())
    {
      return RuntimeActionResult{false, ProjectionRefresh::none,
                                 "No document camera is available"};
    }
    const std::optional<uint32_t> cameraIndex =
        projection->mapping.sceneAssetIndex(request.camera->untyped());
    if(!cameraIndex || *cameraIndex >= projection->asset.cameras.size()
       || !m_editorController->session())
    {
      return RuntimeActionResult{false, ProjectionRefresh::none,
                                 "The document camera is not in the active projection"};
    }
    std::optional<demo::editor::NodeId> node = request.node;
    if(!node)
    {
      const int32_t nodeIndex = projection->asset.cameras[*cameraIndex].nodeIndex;
      if(nodeIndex < 0 || static_cast<std::size_t>(nodeIndex) >=
                              projection->asset.nodes.size())
      {
        return RuntimeActionResult{false, ProjectionRefresh::none,
                                   "The document camera has no owning node"};
      }
      const auto stable = projection->mapping.documentIdAt(
          demo::editor::EditorObjectKind::node,
          projection->asset.nodes[static_cast<std::size_t>(nodeIndex)].sourceNodeIndex);
      if(!stable) return RuntimeActionResult{false, ProjectionRefresh::none,
                                             "The camera node identity is unavailable"};
      node = demo::editor::NodeId::from(*stable);
    }
    if(!node || !node->valid())
      return RuntimeActionResult{false, ProjectionRefresh::none,
                                 "The document camera node is stale"};

    // Camera::getViewMatrix is a typed MGIF value. Decompose its inverse into
    // a local glTF node transform, accounting for an attached parent.
    const glm::mat4 world = glm::inverse(m_cameraUniforms.view);
    glm::vec3 translation{};
    glm::vec3 scale{};
    glm::quat orientation{};
    glm::vec3 skew{};
    glm::vec4 perspective{};
    if(!glm::decompose(world, scale, orientation, translation, skew, perspective)
       || !glm::all(glm::isfinite(translation))
       || !glm::all(glm::isfinite(scale))
       || !glm::all(glm::isfinite(orientation)))
    {
      return RuntimeActionResult{false, ProjectionRefresh::none,
                                 "Current camera view cannot be decomposed"};
    }
    const auto nodeIndex = projection->mapping.sceneAssetIndex(node->untyped());
    if(!nodeIndex || *nodeIndex >= projection->asset.nodes.size())
      return RuntimeActionResult{false, ProjectionRefresh::none,
                                 "The camera node is not in the active projection"};
    const demo::SceneNode& projectedNode = projection->asset.nodes[*nodeIndex];
    glm::mat4 local = world;
    if(projectedNode.parent >= 0
       && static_cast<std::size_t>(projectedNode.parent) < projection->asset.nodes.size())
    {
      local = glm::inverse(projection->asset.nodes[
          static_cast<std::size_t>(projectedNode.parent)].worldTransform) * world;
    }
    if(!glm::decompose(local, scale, orientation, translation, skew, perspective)
       || !glm::all(glm::isfinite(translation))
       || !glm::all(glm::isfinite(scale))
       || !glm::all(glm::isfinite(orientation))
       || std::abs(glm::length(orientation) - 1.0f) > 1e-3f)
    {
      return RuntimeActionResult{false, ProjectionRefresh::none,
                                 "Camera node transform is invalid"};
    }
    const auto properties = m_editorController->session()->editor().commands()
                                .cameraProperties(*request.camera);
    if(!properties) return RuntimeActionResult{false, ProjectionRefresh::none,
                                                "Camera properties are unavailable"};
    demo::editor::NodeTransform transform;
    transform.translation = {translation.x, translation.y, translation.z};
    orientation = glm::normalize(orientation);
    transform.rotation = {orientation.x, orientation.y, orientation.z, orientation.w};
    transform.scale = {scale.x, scale.y, scale.z};
    return m_editorController->setCameraFromView(*request.camera, *node,
                                                 *properties, transform);
  }

  if(request.kind == CameraRequestKind::applyDocumentCamera)
  {
    const demo::editor::SceneProjectionResult* projection =
        m_editorController ? m_editorController->projection() : nullptr;
    if(projection == nullptr || !request.camera.has_value())
    {
      return RuntimeActionResult{
          false, ProjectionRefresh::none, "No document camera is available"};
    }
    const std::optional<uint32_t> cameraIndex =
        projection->mapping.sceneAssetIndex(request.camera->untyped());
    if(!cameraIndex.has_value() || *cameraIndex >= projection->asset.cameras.size())
    {
      return RuntimeActionResult{
          false, ProjectionRefresh::none, "The document camera is not in the active projection"};
    }

    m_editorDocumentCameraIndex = cameraIndex;
    m_editorForceFreeCamera = false;
    resetSceneCameraNavigation();
    refreshActiveCameraUniforms();
    return RuntimeActionResult{true, ProjectionRefresh::none, {}};
  }

  const demo::editor::SceneProjectionResult* projection =
      m_editorController ? m_editorController->projection() : nullptr;
  if(projection == nullptr)
  {
    return RuntimeActionResult{
        false, ProjectionRefresh::none, "No editor projection is available"};
  }

  demo::SceneUploadPlanner planner;
  demo::SceneUploadPlanBuildResult buildResult =
      planner.build(demo::makeSceneAssetView(projection->asset));
  const demo::SceneUploadPlanValidationResult boundPlan =
      demo::bindSceneUploadPlanProjection(
          demo::makeSceneAssetView(projection->asset), projection->mapping,
          buildResult.plan);
  if(!boundPlan.valid)
  {
    return RuntimeActionResult{false, ProjectionRefresh::none,
                               "The render occurrence plan is stale"};
  }

  std::vector<demo::editor::NodeId> selectedSubtree;
  std::optional<demo::editor::PrimitiveOccurrenceKey> selectedPrimitive;
  if(request.kind == CameraRequestKind::fitSelection && m_editorController)
  {
    const demo::editor::SceneSelectionValue& selection =
        m_editorController->sceneSelection().value();
    if(const auto* node = std::get_if<demo::editor::NodeId>(&selection))
    {
      const auto found = std::find_if(
          projection->occurrenceNodes.begin(), projection->occurrenceNodes.end(),
          [&](const demo::editor::SceneProjectionNodeOccurrence& candidate) {
            return candidate.node == *node;
          });
      if(found != projection->occurrenceNodes.end())
      {
        const std::uint32_t depth = found->depth;
        for(auto current = found; current != projection->occurrenceNodes.end();
            ++current)
        {
          if(current != found && current->depth <= depth) break;
          selectedSubtree.push_back(current->node);
        }
      }
    }
    else if(const auto* primitive =
                std::get_if<demo::editor::PrimitiveOccurrenceKey>(&selection))
    {
      selectedPrimitive = *primitive;
    }
  }

  glm::vec3 boundsMin(std::numeric_limits<float>::max());
  glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
  bool foundBounds = false;
  for(const demo::InstanceCullRecord& record : buildResult.plan.cullRecords)
  {
    if(record.instanceIndex >= buildResult.plan.instances.instances.size())
    {
      continue;
    }
    const demo::SceneDrawInstance& instance =
        buildResult.plan.instances.instances[record.instanceIndex];
    if(instance.nodeIndex >= projection->asset.nodes.size() ||
       instance.primitiveIndex >= projection->asset.primitives.size() ||
       !projection->asset.nodes[instance.nodeIndex].effectiveVisible ||
       !projection->asset.primitives[instance.primitiveIndex].effectiveVisible)
    {
      continue;
    }
    if(request.kind == CameraRequestKind::fitSelection)
    {
      bool selected = false;
      if(selectedPrimitive)
      {
        const demo::SceneDrawableSourceIdentity identity =
            instance.sourceIdentity();
        selected = identity.sceneId == demo::editor::stableSceneId(
                       selectedPrimitive->scene.untyped()) &&
                   identity.nodeId == demo::editor::stableSceneId(
                       selectedPrimitive->node.untyped()) &&
                   identity.primitiveId == demo::editor::stableSceneId(
                       selectedPrimitive->primitive.untyped()) &&
                   identity.instanceOrdinal ==
                       selectedPrimitive->instanceOrdinal;
      }
      else
      {
        const auto node = demo::editor::NodeId::from(
            demo::editor::editorObjectId(instance.nodeSourceId));
        selected = node && std::find(selectedSubtree.begin(),
                                     selectedSubtree.end(), *node) !=
                               selectedSubtree.end();
      }
      if(!selected) continue;
    }
    boundsMin = glm::min(boundsMin, record.worldBoundsMin);
    boundsMax = glm::max(boundsMax, record.worldBoundsMax);
    foundBounds = true;
  }

  if(!foundBounds)
  {
    return RuntimeActionResult{
        false, ProjectionRefresh::none,
        request.kind == CameraRequestKind::fitSelection
            ? "The selection has no renderable bounds"
            : "The scene has no renderable bounds"};
  }

  const glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
  const float radius = std::max(glm::length(boundsMax - center), 0.5f);
  glm::vec3 backward = glm::vec3(glm::inverse(m_camera.getViewMatrix())[2]);
  if(glm::length(backward) < 0.001f)
  {
    backward = glm::vec3(0.0f, 0.0f, 1.0f);
  }
  else
  {
    backward = glm::normalize(backward);
  }
  const glm::vec3 position = center + backward * (radius * 2.75f);
  const glm::vec3 forward = glm::normalize(center - position);
  const float yaw = glm::degrees(std::atan2(forward.z, forward.x));
  const float pitch = glm::degrees(std::asin(std::clamp(forward.y, -1.0f, 1.0f)));
  const float aspect = static_cast<float>(
      std::max(m_editorViewportMetrics.colorAttachment.width, 1u)) /
      static_cast<float>(
          std::max(m_editorViewportMetrics.colorAttachment.height, 1u));

  m_camera.setPerspective(45.0f, aspect, 0.1f, std::max(100.0f, radius * 8.0f));
  m_camera.setPosition(position);
  m_camera.setYawPitch(yaw, pitch);
  m_camera.update();
  if(m_editorViewRevision != std::numeric_limits<std::uint64_t>::max())
  {
    ++m_editorViewRevision;
  }
  m_editorLastViewMatrix = m_camera.getViewMatrix();
  m_editorLastCameraProjectionMatrix = m_camera.getProjectionMatrix();
  m_editorLastViewValid = true;
  m_editorForceFreeCamera = true;
  refreshActiveCameraUniforms();
  return RuntimeActionResult{true, ProjectionRefresh::none, {}};
}

inline void MinimalLatestApp::resetPublishedEditorScene() noexcept
{
  m_editorRendererAdapter.reset(m_renderer);
  m_projectedSceneAsset.reset();
  m_editorNativeGizmoInput.discardPending();
  m_editorDeferredFocusReleaseCount = 0u;
  m_editorFocusReleaseFence = false;
  m_editorFocusRestored = false;
  m_editorFocusRestorePending = false;
  m_editorPrimaryPressAfterFocus = false;
  m_editorPointerDown = {};
  m_editorPointerFocusSuspended = {};
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

  const std::filesystem::path source =
      m_projectedSceneAsset
          ? std::filesystem::path(m_projectedSceneAsset->sourcePath)
          : std::filesystem::path{};
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
  if(m_editorRendererAdapter.hasPending() ||
     !m_editorRendererAdapter.initialized() || !m_projectedSceneAsset)
  {
    m_meshSDFStatus = "Wait for editor scene publication to finish first.";
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

inline void MinimalLatestApp::updateCameraInput()
{
  demo::profiling::ScopedCpuRange inputCameraRange("AppPreRecord.InputCamera");
  if(isAutomationEnabled())
  {
    // A native double-focus is a real editor camera action. Once it has been
    // accepted, keep the focused camera instead of overwriting it with the
    // fixed automation pose later in the same frame.
    if(!(m_p6NativeEnabled && m_p6NativeCameraEffects != 0u))
    {
      applyAutomationCameraPose();
    }
  }
  else
  {
    const bool viewportCapture =
        m_editorViewportRuntime != nullptr &&
        m_editorViewportRuntime->cameraNavigationSuppressed();
    const bool gizmoCapture =
        m_editorGizmoAdapter != nullptr &&
        m_editorGizmoAdapter->suppressesCameraAndPicking();
    if((viewportCapture || gizmoCapture) && m_cursorCaptured)
    {
      glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
      m_cursorCaptured = false;
    }
    if(!viewportCapture && !gizmoCapture)
    {
      glm::vec3 moveDir{0.0f};
      if(glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS) moveDir.z += 1.0f;
      if(glfwGetKey(m_window, GLFW_KEY_S) == GLFW_PRESS) moveDir.z -= 1.0f;
      if(glfwGetKey(m_window, GLFW_KEY_A) == GLFW_PRESS) moveDir.x -= 1.0f;
      if(glfwGetKey(m_window, GLFW_KEY_D) == GLFW_PRESS) moveDir.x += 1.0f;
      if(glfwGetKey(m_window, GLFW_KEY_Q) == GLFW_PRESS) moveDir.y += 1.0f;
      if(glfwGetKey(m_window, GLFW_KEY_E) == GLFW_PRESS) moveDir.y -= 1.0f;

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

      demo::Camera& navigationCamera =
          hasActiveSceneCamera() ? m_sceneCameraNavigation : m_camera;
      if(glm::length(moveDir) > 0.0f)
      {
        moveDir = glm::normalize(moveDir) * m_moveSpeed *
                  ImGui::GetIO().DeltaTime;
        navigationCamera.move(moveDir);
      }

      if(glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
      {
        if(!m_cursorCaptured)
        {
          glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
          double xpos = 0.0;
          double ypos = 0.0;
          glfwGetCursorPos(m_window, &xpos, &ypos);
          m_lastMousePos = glm::vec2(static_cast<float>(xpos),
                                     static_cast<float>(ypos));
          m_cursorCaptured = true;
        }
        else
        {
          double xpos = 0.0;
          double ypos = 0.0;
          glfwGetCursorPos(m_window, &xpos, &ypos);
          const float deltaX =
              static_cast<float>(xpos - m_lastMousePos.x) * m_rotateSpeed;
          const float deltaY =
              static_cast<float>(ypos - m_lastMousePos.y) * m_rotateSpeed;
          m_lastMousePos = glm::vec2(static_cast<float>(xpos),
                                     static_cast<float>(ypos));
          navigationCamera.rotate(deltaX, -deltaY);
        }
      }
      else if(m_cursorCaptured)
      {
        glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        m_cursorCaptured = false;
      }
    }
  }
  updateActiveCamera();
}

inline void MinimalLatestApp::updateEditorViewportRuntime(
    const ImVec2& viewportMin, const ImVec2& viewportMax)
{
  m_editorViewportFrameOpen = false;
  if(m_editorViewportRuntime == nullptr || m_editorController == nullptr)
  {
    return;
  }

  const ImGuiIO& io = ImGui::GetIO();
  const float nativeScaleX = std::isfinite(io.DisplayFramebufferScale.x) &&
                                     io.DisplayFramebufferScale.x > 0.0f
                                 ? io.DisplayFramebufferScale.x
                                 : 1.0f;
  const float nativeScaleY = std::isfinite(io.DisplayFramebufferScale.y) &&
                                     io.DisplayFramebufferScale.y > 0.0f
                                 ? io.DisplayFramebufferScale.y
                                 : 1.0f;
  const bool syntheticVisualFramebuffer =
      m_editorVisualEnabled &&
      m_automationOptions.editorVisualCase == "V07-dpi-resize" &&
      (std::abs(nativeScaleX - m_automationOptions.editorVisualDpi) > 0.01f ||
       std::abs(nativeScaleY - m_automationOptions.editorVisualDpi) > 0.01f);
  const float scaleX = syntheticVisualFramebuffer
                           ? m_automationOptions.editorVisualDpi : nativeScaleX;
  const float scaleY = syntheticVisualFramebuffer
                           ? m_automationOptions.editorVisualDpi : nativeScaleY;
  const float logicalWidth = std::max(0.0f, viewportMax.x - viewportMin.x);
  const float logicalHeight = std::max(0.0f, viewportMax.y - viewportMin.y);
  // Consume callbacks before the zero-area early return as well.  A minimized
  // window must not leave an unbounded native FIFO that is replayed against a
  // later viewport generation.
  const bool nativeGestureAtFrameStart = nativeGizmoGestureActive();
  const auto nativeTerminals = m_editorNativeGizmoInput.take();
  if(nativeTerminals.overflowed)
    throw std::runtime_error("native Gizmo callback queue overflowed");
  if(logicalWidth <= 0.0f || logicalHeight <= 0.0f)
  {
    return;
  }

  const demo::editor::PhysicalViewportExtent physicalExtent =
      demo::editor::ui::EditorViewportRuntime::physicalExtentForLogical(
          {logicalWidth, logicalHeight}, {scaleX, scaleY});
  if(physicalExtent.width != m_viewportSize.width ||
     physicalExtent.height != m_viewportSize.height)
  {
    m_viewportSize = {physicalExtent.width, physicalExtent.height};
    m_renderer.resize(m_viewportSize);
    m_camera.setPerspective(
        45.0f,
        static_cast<float>(m_viewportSize.width) /
            static_cast<float>(m_viewportSize.height),
        0.1f, 100.0f);
    refreshActiveCameraUniforms();
  }
  const std::uint64_t projectionRevision =
      m_editorRendererAdapter.activeFence()
          ? m_editorRendererAdapter.activeFence()->mappingEpoch : 0u;
  const bool viewportChanged =
      m_editorViewportMetrics.logical.origin.x != viewportMin.x ||
      m_editorViewportMetrics.logical.origin.y != viewportMin.y ||
      m_editorViewportMetrics.logical.extent.width != logicalWidth ||
      m_editorViewportMetrics.logical.extent.height != logicalHeight ||
      m_editorViewportMetrics.physical.extent != physicalExtent ||
      m_editorViewportMetrics.framebufferScale.x != scaleX ||
      m_editorViewportMetrics.framebufferScale.y != scaleY;
  if(viewportChanged)
  {
    if(m_editorViewportEpoch != std::numeric_limits<std::uint64_t>::max())
    {
      ++m_editorViewportEpoch;
    }
  }
  if(m_editorViewportEpoch == 0u) m_editorViewportEpoch = 1u;
  const glm::mat4 currentView = m_cameraUniforms.view;
  const glm::mat4 currentProjection = m_cameraUniforms.projection;
  if(!m_editorLastViewValid || currentView != m_editorLastViewMatrix ||
     currentProjection != m_editorLastCameraProjectionMatrix)
  {
    if(m_editorLastViewValid &&
       m_editorViewRevision != std::numeric_limits<std::uint64_t>::max())
    {
      ++m_editorViewRevision;
    }
    m_editorLastViewMatrix = currentView;
    m_editorLastCameraProjectionMatrix = currentProjection;
    m_editorLastViewValid = true;
  }
  if(m_editorViewRevision == 0u) m_editorViewRevision = 1u;
  const bool orthographic =
      demo::clipspace::isOrthographicProjection(m_cameraUniforms.projection);
  const double projectionScaleY =
      std::abs(static_cast<double>(m_cameraUniforms.projection[1][1]));
  demo::editor::EditorViewportMetrics next;
  next.logical = {{viewportMin.x, viewportMin.y},
                  {logicalWidth, logicalHeight}};
  next.framebufferLogicalOrigin = next.logical.origin;
  next.physical = {{0u, 0u}, physicalExtent};
  next.depthPhysical = next.physical;
  next.colorAttachment = physicalExtent;
  next.depthAttachment = physicalExtent;
  next.framebufferScale = {scaleX, scaleY};
  next.surfaceTransform =
      demo::editor::EditorViewportSurfaceTransform::identity;
  next.viewportGeneration = m_editorViewportEpoch;
  next.projection = {
      projectionRevision, m_editorViewportMetrics.projection.value};
  next.view = {m_editorViewRevision, m_editorViewRevision};
  next.convention = {
      demo::editor::EditorClipDepthRange::zeroToOne, true, true};
  next.projectionKind = orthographic
      ? demo::editor::EditorProjectionKind::orthographic
      : demo::editor::EditorProjectionKind::perspective;
  next.verticalFieldOfViewRadians = projectionScaleY > 0.0
      ? 2.0 * std::atan(1.0 / projectionScaleY) : 0.0;
  next.orthographicVerticalSpan = projectionScaleY > 0.0
      ? 2.0 / projectionScaleY : 0.0;
  m_editorViewportMetrics = next;

  const bool windowFocused =
      glfwGetWindowAttrib(m_window, GLFW_FOCUSED) == GLFW_TRUE;
  const demo::editor::ui::ViewportSettings settings =
      m_editorController->viewportSettings();
  demo::editor::ui::EditorViewportRuntime::FrameInput frameInput;
  frameInput.viewport = m_editorViewportMetrics;
  frameInput.view = m_cameraUniforms.view;
  frameInput.cameraProjection = m_cameraUniforms.projection;
  frameInput.viewProjection = m_cameraUniforms.viewProjection;
  frameInput.cameraPosition = m_cameraUniforms.cameraPosition;
  frameInput.windowFocused =
      (m_p6NativeEnabled || m_editorVisualEnabled) ? true : windowFocused;
  frameInput.modalActive = m_editorVisualEnabled
      ? false
      : ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
  frameInput.imguiPointerCaptured =
      (m_p6NativeEnabled || m_editorVisualEnabled) ? false : io.WantCaptureMouse;
  frameInput.imguiKeyboardCaptured =
      (m_p6NativeEnabled || m_editorVisualEnabled) ? false : io.WantCaptureKeyboard;
  frameInput.imguiTextCaptured =
      (m_p6NativeEnabled || m_editorVisualEnabled) ? false : io.WantTextInput;
  frameInput.cameraNavigationEnabled =
      m_editorViewportFocused && !m_automationOptions.noUi &&
      !m_p6NativeEnabled;
  // P7's adapter is the sole Grid/Gizmo packet producer; the legacy runtime
  // remains active only for P6 input, picking, and camera arbitration.
  frameInput.showGrid = false;
  // P7's desktop adapter is the sole Gizmo owner. The P6 runtime remains the
  // picking/camera fallback for events the adapter does not consume.
  frameInput.showGizmo = false;
  const bool editorVisualSnap = m_editorVisualEnabled &&
                                m_automationOptions.editorVisualCase ==
                                    "V02-gizmo-components";
  frameInput.snapEnabled = editorVisualSnap || settings.snapEnabled;
  const double editorVisualGrid =
      m_editorVisualEnabled ? editorVisualGridUnit() : settings.gridUnit;
  frameInput.gridUnit = editorVisualGrid;
  frameInput.snapRotationDegrees = settings.snapRotationDegrees;
  frameInput.snapScale = settings.snapScale;
  frameInput.projection = m_editorController->projection();
  frameInput.selection = m_editorController->sceneSelection().value();
  frameInput.selectionRevision = m_editorController->selectionRevision();

  if(m_editorInputFrame == std::numeric_limits<std::uint64_t>::max())
  {
    m_editorInputFrame = 1u;
  }
  const demo::editor::EditorInputFrameId frame{m_editorInputFrame++};
  if(m_editorGizmoAdapter != nullptr)
  {
    auto toOverlayMatrix = [](const glm::mat4& matrix) {
      demo::editor::OverlayMatrix4 result;
      for(std::size_t row = 0u; row < 4u; ++row)
      {
        for(std::size_t column = 0u; column < 4u; ++column)
        {
          result.elements[row * 4u + column] =
              static_cast<double>(matrix[column][row]);
        }
      }
      return result;
    };
    demo::editor::ui::DesktopGizmoFrameInput gizmoFrame;
    gizmoFrame.frameSerial = frame.value;
    gizmoFrame.viewport = m_editorViewportMetrics;
    gizmoFrame.viewProjection = toOverlayMatrix(m_cameraUniforms.viewProjection);
    gizmoFrame.camera.ndcYDown =
        m_automationOptions.backend == demo::rhi::BackendType::vulkan;
    gizmoFrame.camera.generation = {
        m_editorViewportMetrics.viewportGeneration,
        m_editorViewportMetrics.projection,
        m_editorViewportMetrics.view};
    gizmoFrame.camera.inverseViewProjection =
        toOverlayMatrix(glm::inverse(m_cameraUniforms.viewProjection));
    gizmoFrame.viewDepth.generation = gizmoFrame.camera.generation;
    gizmoFrame.gridSettings.enabled = !m_p6NativeEnabled && settings.showGrid;
    gizmoFrame.gridSettings.baseUnit = editorVisualGrid;
    gizmoFrame.gridInput.cameraPosition = {
        static_cast<double>(m_cameraUniforms.cameraPosition.x),
        static_cast<double>(m_cameraUniforms.cameraPosition.y),
        static_cast<double>(m_cameraUniforms.cameraPosition.z)};
    const glm::mat4 inverseView = glm::inverse(m_cameraUniforms.view);
    const glm::vec3 cameraForward =
        glm::normalize(-glm::vec3(inverseView[2]));
    gizmoFrame.gridInput.cameraForward = {
        static_cast<double>(cameraForward.x), static_cast<double>(cameraForward.y),
        static_cast<double>(cameraForward.z)};
    gizmoFrame.gizmoSettings.visible = !m_p6NativeEnabled && settings.showGizmo;
    gizmoFrame.gizmoSettings.snapEnabled = editorVisualSnap || settings.snapEnabled;
    gizmoFrame.gizmoSettings.basis =
        m_automationOptions.editorVisualCase == "V03-parent-space"
            ? demo::editor::GizmoBasisToken::parentLocal
            : demo::editor::GizmoBasisToken::global;
    gizmoFrame.gizmoSettings.visibility = editorVisualGizmoVisibility();
    gizmoFrame.gizmoSettings.occlusionDither =
        m_automationOptions.editorVisualVariant == "occlusion-dither";
    gizmoFrame.snapRotationDegrees = settings.snapRotationDegrees;
    gizmoFrame.snapScale = settings.snapScale;
    gizmoFrame.modalActive = m_editorVisualEnabled
        ? false
        : ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    gizmoFrame.imguiPointerCaptured =
        (m_p6NativeEnabled || m_editorVisualEnabled) ? false : io.WantCaptureMouse;
    gizmoFrame.imguiKeyboardCaptured =
        (m_p6NativeEnabled || m_editorVisualEnabled) ? false : io.WantCaptureKeyboard;
    gizmoFrame.imguiTextCaptured =
        (m_p6NativeEnabled || m_editorVisualEnabled) ? false : io.WantTextInput;
    gizmoFrame.viewDirectionWorld = {
        static_cast<double>(cameraForward.x), static_cast<double>(cameraForward.y),
        static_cast<double>(cameraForward.z)};
    const auto selectedValue = m_editorController->sceneSelection().value();
    std::optional<demo::editor::NodeId> selectedNode;
    if(const auto* nodeSelection =
           std::get_if<demo::editor::NodeId>(&selectedValue))
    {
      selectedNode = *nodeSelection;
    }
    else if(const auto* primitiveSelection =
                std::get_if<demo::editor::PrimitiveOccurrenceKey>(&selectedValue))
    {
      // Primitive highlighting remains occurrence-based, but the node Gizmo
      // attaches to the owning stable node rather than to a primitive index.
      selectedNode = primitiveSelection->node;
    }
    if(selectedNode)
    {
      if(const demo::editor::SceneProjectionResult* projection =
             m_editorController->projection())
      {
        const auto index = projection->mapping.sceneAssetIndex(
            selectedNode->untyped());
        if(index && *index < projection->asset.nodes.size())
        {
          const demo::SceneNode& node = projection->asset.nodes[*index];
          demo::editor::GizmoAttachment attachment;
          attachment.nodeId =
              demo::editor::ui::stableGizmoNodeId(*selectedNode);
          attachment.local.translation = {
              static_cast<double>(node.translation.x),
              static_cast<double>(node.translation.y),
              static_cast<double>(node.translation.z)};
          attachment.local.rotation = {
              static_cast<double>(node.rotation.x),
              static_cast<double>(node.rotation.y),
              static_cast<double>(node.rotation.z),
              static_cast<double>(node.rotation.w)};
          attachment.local.scale = {
              static_cast<double>(node.scale.x), static_cast<double>(node.scale.y),
              static_cast<double>(node.scale.z)};
          attachment.parentWorld = node.parent >= 0 &&
                                           static_cast<std::size_t>(node.parent) <
                                               projection->asset.nodes.size()
                                       ? toOverlayMatrix(projection->asset
                                                             .nodes[node.parent]
                                                             .worldTransform)
                                       : toOverlayMatrix(projection->asset
                                                             .sceneRootTransform
                                                             .matrix);
          if(m_automationOptions.editorVisualCase == "V03-parent-space")
          {
            // Keep the fixture immutable while exercising the bounded
            // parent-space path with a rotated, negative, non-uniform basis.
            const double angle = 0.5235987755982988; // 30 degrees
            const double cosine = std::cos(angle);
            const double sine = std::sin(angle);
            demo::editor::OverlayMatrix4 parent{};
            parent.elements = {
                -cosine * 1.25, 0.0, sine * 1.60, 0.0,
                0.0, 0.75, 0.0, 0.0,
                sine * 1.25, 0.0, cosine * 1.60, 0.0,
                0.0, 0.0, 0.0, 1.0};
            attachment.parentWorld = parent;
          }
          if(m_editorVisualEnabled)
          {
            m_editorVisualParentWorld = attachment.parentWorld;
            m_editorVisualParentWorldValid = true;
          }
          attachment.documentRevision = projection->documentRevision;
          attachment.selectionRevision = m_editorController->selectionRevision();
          gizmoFrame.documentRevision = projection->documentRevision;
          gizmoFrame.selectionRevision = m_editorController->selectionRevision();
          gizmoFrame.mappingEpoch = projection->mappingEpoch;
          gizmoFrame.selectedNode = *selectedNode;
          gizmoFrame.attachment = attachment;
          const glm::vec3 worldPivot =
              glm::vec3(node.worldTransform * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
          const float viewZ =
              (m_cameraUniforms.view * glm::vec4(worldPivot, 1.0f)).z;
          gizmoFrame.viewDepth.positiveViewDepth =
              std::max(0.001, std::abs(static_cast<double>(viewZ)));
        }
      }
    }
    m_editorGizmoAdapter->beginFrame(std::move(gizmoFrame));
  }
  m_editorViewportRuntime->beginFrame(std::move(frameInput), frame);
  m_editorViewportFrameOpen = true;

  auto send = [&](demo::editor::EditorInputEventPayload payload,
                  std::uint64_t callbackSequence = 0u,
                  bool consumeIfAdapterUnclaimed = false) {
    sendEditorViewportEvent(frame, std::move(payload), callbackSequence,
                            consumeIfAdapterUnclaimed);
  };

  // Deferred callbacks are emitted before any newer native callbacks.  This
  // preserves the process-wide FIFO when a release from the previous
  // focus-loss batch and a new callback happen to share this frame.  The
  // pending bit is sampled before processing the current batch, so callbacks
  // discovered below remain deferred until the next independent frame.
  const bool replayDeferredFocusReleases =
      windowFocused && m_editorFocusRestorePending;
  if(replayDeferredFocusReleases)
  {
    for(std::size_t index = 0u;
        index < m_editorDeferredFocusReleaseCount; ++index)
    {
      const auto &release = m_editorDeferredFocusReleases[index];
      send(demo::editor::EditorPointerButtonEvent{
               {static_cast<double>(m_editorLastMousePosition.x),
                static_cast<double>(m_editorLastMousePosition.y)},
               demo::editor::EditorPointerButton::primary,
               demo::editor::EditorInputAction::release, false, false},
           release.callbackSequence, true);
    }
    if(glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) != GLFW_PRESS)
      m_editorPointerDown[0] = false;
    m_editorDeferredFocusReleaseCount = 0u;
    m_editorFocusRestorePending = false;
    // The primary suppression fence remains until a fresh press, but the
    // focus-transition gate has crossed its one-frame restore boundary.
    m_editorFocusReleaseFence = false;
    m_editorFocusRestored = false;
  }

  if(m_editorPrimaryPressAfterFocus && !nativeGizmoGestureActive())
  {
    // A fresh press is allowed to establish a new polling edge only after the
    // old adapter gesture has detached.  While a latched gesture still owns
    // the node, retain its suppression fence until Escape/cleanup resolves it.
    m_editorPointerDown[0] = false;
    m_editorPointerFocusSuspended[0] = false;
    m_editorPrimaryPressAfterFocus = false;
  }

  // Drain native callback terminals before polling the final GLFW focus and
  // button state.  A focus loss/regain can happen entirely between frames;
  // preserving callback order keeps a queued mouse-up suppressed even when
  // the window is focused again by the time this frame is assembled.
  const bool nativeBatchInterrupted = nativeTerminals.focusInterrupted;
  bool nativeFocusEventDrained = false;
  bool focusRestoredThisFrame = false;
  bool focusLostInNativeBatch = false;
  const bool nativeGestureOwned =
      nativeGestureAtFrameStart || nativeGizmoGestureActive();
  const bool nativeBatchOwned =
      !m_p6NativeEnabled &&
      (nativeGestureOwned || nativeTerminals.count != 0u);
  if(nativeBatchOwned)
  {
    using NativeKind = demo::editor::ui::NativeGizmoTerminalQueue::Kind;
    if(nativeBatchInterrupted && nativeGestureOwned)
      m_editorPointerFocusSuspended[0] = true;
    for(std::size_t index = 0u; index < nativeTerminals.count; ++index)
    {
      const auto& terminal = nativeTerminals.events[index];
      // A callback can outlive the adapter's attachment fence (for example
      // after a document replacement). Drain it, but never reinterpret it as
      // a P6 camera/picking event or create a new focus fence.
      if(!nativeGestureOwned)
      {
        switch(terminal.kind)
        {
        case NativeKind::escapePressed:
          send(demo::editor::EditorKeyEvent{
              demo::editor::EditorInputKey::escape,
              demo::editor::EditorInputAction::press},
               terminal.callbackSequence, true);
          m_editorEscapeDown = true;
          break;
        case NativeKind::primaryReleased:
          // The callback is already stale with respect to the adapter. Keep
          // the polled edge consumed so the next frame cannot manufacture a
          // second delivered release through the fallback router, regardless
          // of whether the OS button query has caught up yet.
          m_editorPointerDown[0] = false;
          m_editorPointerFocusSuspended[0] = true;
          send(demo::editor::EditorPointerButtonEvent{
                   {static_cast<double>(m_editorLastMousePosition.x),
                    static_cast<double>(m_editorLastMousePosition.y)},
                   demo::editor::EditorPointerButton::primary,
                   demo::editor::EditorInputAction::release, false, false},
               terminal.callbackSequence, true);
          break;
        case NativeKind::focusLost:
          send(demo::editor::EditorFocusEvent{
              demo::editor::EditorFocusAction::lost},
               terminal.callbackSequence, true);
          m_editorWindowFocused = false;
          break;
        }
        continue;
      }
      switch(terminal.kind)
      {
      case NativeKind::escapePressed:
        send(demo::editor::EditorKeyEvent{
            demo::editor::EditorInputKey::escape,
            demo::editor::EditorInputAction::press}, terminal.callbackSequence,
             true);
        m_editorEscapeDown = true;
        break;
      case NativeKind::focusLost:
        for(std::size_t button = 0u;
            button < m_editorPointerDown.size(); ++button)
        {
          // Once a focus boundary has suspended a button, only a fresh press
          // may clear that fence.  A second loss must never weaken it.
          m_editorPointerFocusSuspended[button] =
              m_editorPointerFocusSuspended[button] ||
              m_editorPointerDown[button];
        }
        // The active native Gizmo owns the primary button even if a queued
        // release was observed first and already cleared the polled edge.
        // Keep that primary fence until a fresh press proves a new gesture.
        m_editorPointerFocusSuspended[0] = true;
        m_editorFocusReleaseFence = true;
        m_editorFocusRestored = false;
        focusLostInNativeBatch = true;
        send(demo::editor::EditorFocusEvent{
            demo::editor::EditorFocusAction::lost}, terminal.callbackSequence,
             true);
        // The callback is authoritative for this transition.  Updating the
        // cached state prevents the later polling pass from emitting a second
        // focus-lost event; a final focused state will emit only the matching
        // gained edge below.
        m_editorWindowFocused = false;
        nativeFocusEventDrained = true;
        break;
      case NativeKind::primaryReleased:
      {
        const bool deferRelease =
            focusLostInNativeBatch ||
            (m_editorFocusReleaseFence && !m_editorFocusRestored);
        if(deferRelease)
        {
          if(m_editorDeferredFocusReleaseCount >=
             m_editorDeferredFocusReleases.size())
          {
            throw std::runtime_error(
                "deferred native Gizmo release queue overflowed");
          }
          m_editorDeferredFocusReleases[m_editorDeferredFocusReleaseCount++] =
              terminal;
          // If focus has already been restored, the next independent frame
          // owns delivery.  A loss-before-restore batch sets this when the
          // gain edge is observed below.
          if(m_editorFocusRestored)
            m_editorFocusRestorePending = true;
          break;
        }
        const bool delivered = !nativeBatchInterrupted &&
                               !m_editorPointerFocusSuspended[0];
        send(demo::editor::EditorPointerButtonEvent{
            {static_cast<double>(m_editorLastMousePosition.x),
             static_cast<double>(m_editorLastMousePosition.y)},
             demo::editor::EditorPointerButton::primary,
            demo::editor::EditorInputAction::release, false, delivered},
             terminal.callbackSequence, true);
        if(glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) != GLFW_PRESS)
          m_editorPointerDown[0] = false;
        // Keep a suppressed release owned by the focus fence.  Clearing here
        // would allow a repeated late callback to become a real commit.
        if(delivered)
        {
          m_editorPointerFocusSuspended[0] = false;
          m_editorFocusReleaseFence = false;
          m_editorFocusRestored = false;
        }
        break;
      }
      }
    }
  }

  // A focus callback received while no native Gizmo terminal was active still
  // has to reach the shared router.  There is no FIFO focus event in that
  // case, so synthesize the state edge once and let the final GLFW state add a
  // matching gain if the window was restored before this frame.
  if(nativeBatchInterrupted && !nativeFocusEventDrained &&
     nativeGizmoGestureActive())
  {
    m_editorFocusReleaseFence = true;
    m_editorFocusRestored = false;
  }
  if(!m_p6NativeEnabled && nativeBatchInterrupted &&
     !nativeFocusEventDrained && m_editorWindowFocused)
  {
    if(nativeGizmoGestureActive())
    {
      m_editorFocusReleaseFence = true;
      m_editorFocusRestored = false;
    }
    send(demo::editor::EditorFocusEvent{demo::editor::EditorFocusAction::lost});
    m_editorWindowFocused = false;
  }

  if(windowFocused != m_editorWindowFocused)
  {
    const bool priorSuppressionFence = m_editorFocusReleaseFence;
    if(!windowFocused)
    {
      for(std::size_t index = 0u; index < m_editorPointerDown.size(); ++index)
      {
        m_editorPointerFocusSuspended[index] =
            m_editorPointerFocusSuspended[index] ||
            m_editorPointerDown[index];
      }
      if(nativeGizmoGestureActive())
        m_editorPointerFocusSuspended[0] = true;
      m_editorFocusRestored = false;
      if(nativeGizmoGestureActive())
        m_editorFocusReleaseFence = true;
    }
    else if(m_editorFocusReleaseFence)
    {
      // A restore edge only arms delivery. Deferred callback releases are
      // emitted on the following independent focused frame.
      m_editorFocusRestored = true;
      if(m_editorDeferredFocusReleaseCount != 0u)
        m_editorFocusRestorePending = true;
      else
      {
        // There is no callback payload waiting for the next frame.  Keep the
        // primary-button suppression fence, but retire the focus-transition
        // gate so a later ordinary focus regain is not treated as a replay.
        m_editorFocusReleaseFence = false;
        m_editorFocusRestored = false;
      }
    }
    send(demo::editor::EditorFocusEvent{
        windowFocused ? demo::editor::EditorFocusAction::gained
                      : demo::editor::EditorFocusAction::lost});
    focusRestoredThisFrame = windowFocused && priorSuppressionFence;
    m_editorWindowFocused = windowFocused;
  }

  demo::editor::LogicalViewportPoint pointer{
      static_cast<double>(io.MousePos.x), static_cast<double>(io.MousePos.y)};
  const double finiteMouseLimit =
      0.5 * static_cast<double>((std::numeric_limits<float>::max)());
  bool pointerUsable =
      ImGui::IsMousePosValid(&io.MousePos) &&
      std::isfinite(pointer.x) && std::isfinite(pointer.y) &&
      std::abs(pointer.x) < finiteMouseLimit &&
      std::abs(pointer.y) < finiteMouseLimit;
  if(m_editorVisualEnabled || (!pointerUsable && windowFocused))
  {
    double xpos = 0.0;
    double ypos = 0.0;
    glfwGetCursorPos(m_window, &xpos, &ypos);
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    pointer = {xpos + mainViewport->Pos.x, ypos + mainViewport->Pos.y};
    pointerUsable = std::isfinite(pointer.x) && std::isfinite(pointer.y) &&
                    std::abs(pointer.x) < finiteMouseLimit &&
                    std::abs(pointer.y) < finiteMouseLimit;
  }
  // Visual replay may tolerate a missing cursor, but it must never turn the
  // ImGui sentinel into a typed viewport event or an invalid gizmo ray.
  // A finite ImGui position can survive one frame after WM_KILLFOCUS on some
  // backend versions.  Never feed that stale coordinate to the viewport while
  // a native Gizmo owns an unfocused drag; the focus terminal must be the last
  // input observed for that frame.
  if(pointerUsable && (windowFocused || m_editorVisualEnabled))
  {
    m_editorLastMousePosition = ImVec2(static_cast<float>(pointer.x),
                                       static_cast<float>(pointer.y));
    if(!m_p6NativeEnabled)
    {
      send(demo::editor::EditorPointerMoveEvent{pointer});
    }
    if(m_editorVisualEnabled && m_editorGizmoAdapter != nullptr)
    {
      int windowX = 0;
      int windowY = 0;
      glfwGetWindowPos(m_window, &windowX, &windowY);
      const ImGuiViewport* inputViewport = ImGui::GetMainViewport();
      LOGI("[EDITOR_VISUAL_INPUT] frame=%llu pointer=(%.3f,%.3f) io=(%.3f,%.3f) hovered=%u phase=%u focus=%d left=%d",
           static_cast<unsigned long long>(m_automationFrame), pointer.x,
           pointer.y, static_cast<double>(io.MousePos.x),
           static_cast<double>(io.MousePos.y),
           static_cast<unsigned>(m_editorGizmoAdapter->hoveredComponent()),
           static_cast<unsigned>(m_editorGizmoAdapter->phase()),
           windowFocused ? 1 : 0,
           glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS
               ? 1 : 0);
      LOGI("[EDITOR_VISUAL_WINDOW] frame=%llu pos=(%d,%d) main_viewport=(%.3f,%.3f) display=(%.3f,%.3f)",
           static_cast<unsigned long long>(m_automationFrame), windowX, windowY,
           inputViewport != nullptr ? inputViewport->Pos.x : 0.0f,
           inputViewport != nullptr ? inputViewport->Pos.y : 0.0f,
           io.DisplaySize.x, io.DisplaySize.y);
    }
  }

  if(windowFocused && !m_p6NativeEnabled && !focusRestoredThisFrame)
  {
    const std::array<int, 3> glfwButtons{
        GLFW_MOUSE_BUTTON_LEFT, GLFW_MOUSE_BUTTON_RIGHT,
        GLFW_MOUSE_BUTTON_MIDDLE};
    for(std::size_t index = 0u; index < glfwButtons.size(); ++index)
    {
      const bool down = glfwGetMouseButton(m_window, glfwButtons[index]) == GLFW_PRESS;
      if(down == m_editorPointerDown[index]) continue;
      const bool suspended = m_editorPointerFocusSuspended[index];
      send(demo::editor::EditorPointerButtonEvent{
          {static_cast<double>(m_editorLastMousePosition.x),
           static_cast<double>(m_editorLastMousePosition.y)},
          static_cast<demo::editor::EditorPointerButton>(index),
          down ? demo::editor::EditorInputAction::press
               : demo::editor::EditorInputAction::release,
          index == 0u && down &&
                  ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left),
          down || !suspended});
      m_editorPointerDown[index] = down;
      // A release observed after focus loss is intentionally consumed as a
      // suppressed edge. Keep the suspension fence until a fresh press.
      if(down || !suspended)
        m_editorPointerFocusSuspended[index] = false;
      if(index == 0u && !down && suspended)
      {
        // A polling up consumed after focus restoration retires only the
        // focus-transition gate.  The primary suppression fence remains until
        // a fresh press, preventing repeated late releases from committing.
        m_editorFocusReleaseFence = false;
        m_editorFocusRestored = false;
        m_editorFocusRestorePending = false;
      }
    }

    const bool escapeDown = glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
    if(escapeDown != m_editorEscapeDown)
    {
      send(demo::editor::EditorKeyEvent{
          demo::editor::EditorInputKey::escape,
          escapeDown ? demo::editor::EditorInputAction::press
                      : demo::editor::EditorInputAction::release});
      m_editorEscapeDown = escapeDown;
    }
  }

  if(std::isfinite(io.MouseWheel) && std::isfinite(io.MouseWheelH) &&
     (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f))
  {
    send(demo::editor::EditorPointerWheelEvent{
        {static_cast<double>(m_editorLastMousePosition.x),
         static_cast<double>(m_editorLastMousePosition.y)},
        static_cast<double>(io.MouseWheelH), static_cast<double>(io.MouseWheel)});
  }
  if(m_p6NativeEnabled)
  {
    advanceP6NativeInteraction(frame);
  }
  (void)m_editorViewportRuntime->tick();
}

inline void MinimalLatestApp::finishEditorViewportRuntimeFrame()
{
  if(!m_editorViewportFrameOpen || m_editorViewportRuntime == nullptr ||
     m_editorController == nullptr)
  {
    return;
  }
  m_editorViewportFrameOpen = false;
  const glm::mat4 currentView = m_cameraUniforms.view;
  const glm::mat4 currentProjection = m_cameraUniforms.projection;
  if(!m_editorLastViewValid || currentView != m_editorLastViewMatrix ||
     currentProjection != m_editorLastCameraProjectionMatrix)
  {
    if(m_editorLastViewValid &&
       m_editorViewRevision != std::numeric_limits<std::uint64_t>::max())
    {
      ++m_editorViewRevision;
    }
    m_editorLastViewMatrix = currentView;
    m_editorLastCameraProjectionMatrix = currentProjection;
    m_editorLastViewValid = true;
  }
  if(m_editorViewRevision == 0u) m_editorViewRevision = 1u;
  m_editorViewportMetrics.view = {m_editorViewRevision, m_editorViewRevision};
  m_editorViewportRuntime->updateViewSnapshot(
      m_editorViewportMetrics, m_cameraUniforms.view,
      m_cameraUniforms.projection, m_cameraUniforms.viewProjection,
      m_cameraUniforms.cameraPosition);
  m_editorViewportRuntime->updateSceneSnapshot(
      m_editorController->projection(),
      m_editorController->sceneSelection().value(),
      m_editorController->selectionRevision());
  if(m_editorGizmoAdapter != nullptr)
  {
    if(const auto* projection = m_editorController->projection())
    {
      m_editorGizmoAdapter->refreshRevisions(
          projection->documentRevision, m_editorController->selectionRevision(),
          projection->mappingEpoch);
    }
    m_editorGizmoAdapter->closeFrame();
    if(m_editorVisualEnabled && m_editorGizmoAdapter->snapshot().has_value())
    {
      const auto &visualSnapshot = *m_editorGizmoAdapter->snapshot();
      if(visualSnapshot.gizmoVisible &&
         !visualSnapshot.gizmo.components.empty())
      {
        demo::editor::OverlayMatrix4 inverseViewProjection{};
        const glm::mat4 inverse = glm::inverse(m_cameraUniforms.viewProjection);
        for(std::size_t row = 0u; row < 4u; ++row)
        {
          for(std::size_t column = 0u; column < 4u; ++column)
          {
            inverseViewProjection.elements[row * 4u + column] =
                static_cast<double>(inverse[column][row]);
          }
        }
        demo::editor::GizmoCameraSnapshot cameraSnapshot;
        cameraSnapshot.ndcYDown =
            m_automationOptions.backend == demo::rhi::BackendType::vulkan;
        cameraSnapshot.generation = {
            visualSnapshot.viewport.viewportGeneration,
            visualSnapshot.viewport.projection,
            visualSnapshot.viewport.view};
        cameraSnapshot.inverseViewProjection = inverseViewProjection;
        const demo::editor::LogicalViewportPoint diagnosticPointer{
            static_cast<double>(m_editorLastMousePosition.x),
            static_cast<double>(m_editorLastMousePosition.y)};
        const auto ray = demo::editor::makeGizmoRay(
            diagnosticPointer, visualSnapshot.viewport, cameraSnapshot);
        demo::editor::GizmoHit diagnosticHit;
        if(ray)
        {
          LOGI("[EDITOR_VISUAL_RAY] origin=(%.6f,%.6f,%.6f) dir=(%.6f,%.6f,%.6f)",
               ray->origin.x, ray->origin.y, ray->origin.z,
               ray->direction.x, ray->direction.y, ray->direction.z);
          const auto inverseModel = demo::editor::invertOverlayMatrix(
              visualSnapshot.gizmo.components[0].model);
          if(inverseModel)
          {
            const auto localOrigin = demo::editor::transformOverlayPoint(
                *inverseModel, ray->origin);
            const auto localDirection = demo::editor::transformOverlayDirection(
                *inverseModel, ray->direction);
            if(localOrigin && localDirection)
            {
              LOGI("[EDITOR_VISUAL_LOCAL_RAY] origin=(%.6f,%.6f,%.6f) dir=(%.6f,%.6f,%.6f)",
                   localOrigin->x, localOrigin->y, localOrigin->z,
                   localDirection->x, localDirection->y, localDirection->z);
              diagnosticHit = demo::editor::classifyGizmoHit(
                  {*localOrigin, *localDirection},
                  demo::editor::canonicalGizmoGeometry(), {},
                  demo::editor::GizmoVisibility::all);
            }
          }
        }
        LOGI("[EDITOR_VISUAL_HIT] frame=%llu pointer=(%.3f,%.3f) ray=%d hit=%d component=%u miss=%.6f",
             static_cast<unsigned long long>(m_automationFrame),
             diagnosticPointer.x, diagnosticPointer.y, ray ? 1 : 0,
             diagnosticHit.hit() ? 1 : 0,
             static_cast<unsigned>(diagnosticHit.component),
             diagnosticHit.missDistance);
      }
    }
    // Terminal callbacks may publish a new projection; refresh the P6 runtime
    // snapshot before its own frame-close bookkeeping.
    m_editorViewportRuntime->updateSceneSnapshot(
        m_editorController->projection(),
        m_editorController->sceneSelection().value(),
        m_editorController->selectionRevision());
    if(m_editorVisualEnabled)
    {
      advanceEditorVisualInteraction(
          demo::editor::EditorInputFrameId{m_editorInputFrame == 0u
                                               ? 0u
                                               : m_editorInputFrame - 1u});
    }
  }
  m_editorViewportRuntime->endFrame();
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
