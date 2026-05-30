#define VMA_IMPLEMENTATION
#define VMA_LEAK_LOG_FORMAT(format, ...)                                                                               \
  {                                                                                                                    \
    printf((format), __VA_ARGS__);                                                                                     \
    printf("\n");                                                                                                      \
  }

#include "../common/Common.h"
#include "../loader/GltfLoader.h"
#include "../render/AsyncLoadingCoordinator.h"
#include "../render/Camera.h"
#include "../render/RendererFacade.h"
#include "../rhi/vulkan/VulkanCommandList.h"
#include "../rhi/vulkan/VulkanSurface.h"

#include <android/asset_manager.h>
#include <android_native_app_glue.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

bool extractAssetDir(AAssetManager* assetManager,
                     const std::string& assetDir,
                     const std::string& destDir)
{
  AAssetDir* dir = AAssetManager_openDir(assetManager, assetDir.c_str());
  if(dir == nullptr)
  {
    LOGE("Failed to open asset directory: %s", assetDir.c_str());
    return false;
  }

  std::string destPath = destDir + "/" + assetDir;
  std::filesystem::create_directories(destPath);

  const char* filename = nullptr;
  while((filename = AAssetDir_getNextFileName(dir)) != nullptr)
  {
    std::string assetPath = assetDir + "/" + filename;
    std::string outPath = destDir + "/" + assetPath;

    if(std::filesystem::exists(outPath))
    {
      continue;
    }

    AAsset* asset = AAssetManager_open(assetManager, assetPath.c_str(), AASSET_MODE_STREAMING);
    if(asset == nullptr)
    {
      continue;
    }

    const off_t assetSize = AAsset_getLength(asset);
    std::vector<uint8_t> buffer(static_cast<size_t>(assetSize));
    AAsset_read(asset, buffer.data(), buffer.size());
    AAsset_close(asset);

    FILE* outFile = fopen(outPath.c_str(), "wb");
    if(outFile != nullptr)
    {
      fwrite(buffer.data(), 1, buffer.size(), outFile);
      fclose(outFile);
    }
  }

  AAssetDir_close(dir);
  return true;
}

class AndroidDemoApp
{
public:
  explicit AndroidDemoApp(android_app* app)
      : m_app(app)
  {
  }

  ~AndroidDemoApp() { shutdown(); }

  void init(ANativeWindow* window)
  {
    if(window == nullptr || m_initialized)
    {
      return;
    }

    VK_CHECK(volkInitialize());
    m_surface = std::make_unique<demo::rhi::vulkan::VulkanSurface>();
    m_renderer.init(window, *m_surface, true);

    const int32_t width = std::max(1, ANativeWindow_getWidth(window));
    const int32_t height = std::max(1, ANativeWindow_getHeight(window));
    m_viewportSize = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    m_renderer.resize(m_viewportSize);

    m_camera.setPerspective(45.0f,
                            static_cast<float>(m_viewportSize.width) / static_cast<float>(m_viewportSize.height),
                            0.1f,
                            100.0f);
    m_camera.setPosition(glm::vec3(8.0f, 1.5f, 0.0f));
    m_camera.setYawPitch(180.0, 0.0);
    updateCamera();

    m_initialized = true;

    loadDefaultScene();
  }

  void shutdown()
  {
    if(!m_initialized)
    {
      return;
    }
    unloadModel();
    m_renderer.waitForIdle();
    m_renderer.shutdown(*m_surface);
    m_surface.reset();
    m_initialized = false;
  }

  void render()
  {
    if(!m_initialized || m_app == nullptr || m_app->window == nullptr)
    {
      return;
    }

    updateAsyncLoading();

    const int32_t width = std::max(1, ANativeWindow_getWidth(m_app->window));
    const int32_t height = std::max(1, ANativeWindow_getHeight(m_app->window));
    demo::rhi::Extent2D requestedSize{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    if(requestedSize.width != m_viewportSize.width || requestedSize.height != m_viewportSize.height)
    {
      m_viewportSize = requestedSize;
      m_renderer.resize(m_viewportSize);
      m_camera.setPerspective(45.0f,
                              static_cast<float>(m_viewportSize.width) / static_cast<float>(m_viewportSize.height),
                              0.1f,
                              100.0f);
      updateCamera();
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();
    ImGui::Begin("VKDemo Android");
    ImGui::TextUnformatted("GPU Driven renderer");
    ImGui::Text("Viewport: %u x %u", m_viewportSize.width, m_viewportSize.height);
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    if(m_modelLoaded && m_sceneModel.has_value())
    {
      ImGui::Separator();
      ImGui::Text("Scene: %s", m_sceneModel->name.c_str());
      ImGui::Text("Meshes: %zu  Materials: %zu  Textures: %zu",
                  m_sceneModel->meshes.size(),
                  m_sceneModel->materials.size(),
                  m_sceneModel->images.size());
    }
    else if(m_isLoading)
    {
      ImGui::Separator();
      ImGui::Text("Loading: %s", m_loadStatus.c_str());
    }
    ImGui::End();

    demo::RenderParams params{};
    params.viewportSize = m_viewportSize;
    params.deltaTime = ImGui::GetIO().DeltaTime;
    params.timeSeconds = static_cast<float>(ImGui::GetTime());
    params.cameraUniforms = &m_cameraUniforms;
    params.lightSettings = m_lightSettings;
    params.debugOptions = m_debugOptions;
    params.gltfModel = m_uploadResult.has_value() ? &(*m_uploadResult) : nullptr;
    params.recordUi = [](demo::rhi::CommandList& cmd) {
      ImGui::Render();
      ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), demo::rhi::vulkan::getNativeCommandBuffer(cmd));
    };

    m_renderer.render(params);
  }

  int32_t handleInput(AInputEvent* event)
  {
    if(event == nullptr)
    {
      return 0;
    }
    return ImGui_ImplAndroid_HandleInputEvent(event) ? 1 : 0;
  }

private:
  void updateCamera()
  {
    m_camera.update();
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

  void loadDefaultScene()
  {
    if(m_app == nullptr || m_app->activity == nullptr)
    {
      return;
    }

    AAssetManager* assetManager = m_app->activity->assetManager;
    const char* internalPath = m_app->activity->internalDataPath;
    if(assetManager == nullptr || internalPath == nullptr)
    {
      LOGE("Missing asset manager or internal data path");
      return;
    }

    m_isLoading = true;
    m_loadStatus = "Extracting assets...";

    const std::string assetDir = "Sponza";
    if(!extractAssetDir(assetManager, assetDir, internalPath))
    {
      m_isLoading = false;
      m_loadStatus = "Asset extraction failed";
      LOGE("Failed to extract asset directory: %s", assetDir.c_str());
      return;
    }

    std::string gltfPath = std::string(internalPath) + "/" + assetDir + "/Sponza.gltf";
    LOGI("Loading glTF from: %s", gltfPath.c_str());

    m_loadStatus = "Parsing glTF...";
    demo::GltfLoader loader;
    demo::GltfModel model;
    if(!loader.load(gltfPath, model))
    {
      m_isLoading = false;
      m_loadStatus = "glTF parse failed";
      LOGE("Failed to load glTF: %s", loader.getLastError().c_str());
      return;
    }

    LOGI("Parsed glTF: %s (%zu meshes, %zu materials, %zu textures)",
         model.name.c_str(), model.meshes.size(), model.materials.size(), model.images.size());

    m_loadStatus = "Preparing GPU upload...";
    m_sceneModel = std::move(model);
    m_uploadResult.emplace();
    m_renderer.initializeGltfUploadResult(*m_sceneModel, *m_uploadResult);

    m_asyncCoordinator.emplace();
    m_asyncCoordinator->beginOneShot(*m_sceneModel);
    m_renderer.setSceneRenderingSuspended(true);
  }

  void updateAsyncLoading()
  {
    if(!m_asyncCoordinator.has_value() || !m_sceneModel.has_value() || !m_uploadResult.has_value())
    {
      return;
    }

    if(m_asyncCoordinator->hasPendingBatches())
    {
      demo::AsyncLoadingCoordinator::UploadBatch batch = m_asyncCoordinator->takeNextBatch();
      if(!batch.meshIndices.empty() || !batch.materialIndices.empty() || !batch.textureIndices.empty())
      {
        m_loadStatus = "Uploading scene assets...";
        m_renderer.executeUploadCommand([this, &batch](VkCommandBuffer cmd) {
          m_renderer.uploadGltfModelBatch(*m_sceneModel,
                                          batch.textureIndices,
                                          batch.materialIndices,
                                          batch.meshIndices,
                                          *m_uploadResult,
                                          cmd);
        });
        m_asyncCoordinator->markBatchUploaded(batch);
      }
    }

    const demo::AsyncLoadingCoordinator::LoadProgress& progress = m_asyncCoordinator->getProgress();
    if(progress.isComplete)
    {
      m_renderer.waitForIdle();
      m_renderer.setSceneRenderingSuspended(false);
      m_modelLoaded = true;
      m_isLoading = false;
      m_loadStatus = "Done";
      m_asyncCoordinator.reset();
      LOGI("Scene upload complete");
    }
  }

  void unloadModel()
  {
    m_asyncCoordinator.reset();
    m_renderer.setSceneRenderingSuspended(false);
    if(m_uploadResult.has_value())
    {
      m_renderer.waitForIdle();
      m_renderer.destroyGltfResources(*m_uploadResult);
      m_uploadResult.reset();
    }
    m_sceneModel.reset();
    m_modelLoaded = false;
    m_isLoading = false;
  }

  android_app* m_app{nullptr};
  bool m_initialized{false};
  std::unique_ptr<demo::rhi::vulkan::VulkanSurface> m_surface;
  demo::RendererFacade m_renderer;
  demo::Camera m_camera;
  demo::rhi::Extent2D m_viewportSize{1, 1};
  shaderio::CameraUniforms m_cameraUniforms{};
  demo::DirectionalLightSettings m_lightSettings{};
  demo::DebugPassOptions m_debugOptions{};

  std::optional<demo::GltfModel> m_sceneModel;
  std::optional<demo::GltfUploadResult> m_uploadResult;
  std::optional<demo::AsyncLoadingCoordinator> m_asyncCoordinator;
  bool m_modelLoaded{false};
  bool m_isLoading{false};
  std::string m_loadStatus;
};

void handleCommand(android_app* app, int32_t command)
{
  auto* demoApp = static_cast<AndroidDemoApp*>(app->userData);
  if(demoApp == nullptr)
  {
    return;
  }

  switch(command)
  {
    case APP_CMD_INIT_WINDOW:
      demoApp->init(app->window);
      break;
    case APP_CMD_TERM_WINDOW:
      demoApp->shutdown();
      break;
    default:
      break;
  }
}

int32_t handleInput(android_app* app, AInputEvent* event)
{
  auto* demoApp = static_cast<AndroidDemoApp*>(app->userData);
  return demoApp != nullptr ? demoApp->handleInput(event) : 0;
}

}  // namespace

void android_main(android_app* app)
{
  utils::Logger& logger = utils::Logger::getInstance();
  logger.enableFileOutput(false);
  logger.setShowFlags(utils::Logger::eSHOW_TIME);
  logger.setLogLevel(utils::Logger::LogLevel::eINFO);

  AndroidDemoApp demoApp(app);
  app->userData = &demoApp;
  app->onAppCmd = handleCommand;
  app->onInputEvent = handleInput;

  while(true)
  {
    int events = 0;
    android_poll_source* source = nullptr;
    while(ALooper_pollOnce(0, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0)
    {
      if(source != nullptr)
      {
        source->process(app, source);
      }
      if(app->destroyRequested != 0)
      {
        demoApp.shutdown();
        return;
      }
    }

    demoApp.render();
  }
}
