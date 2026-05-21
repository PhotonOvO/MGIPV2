#define VMA_IMPLEMENTATION
#define VMA_LEAK_LOG_FORMAT(format, ...)                                                                               \
  {                                                                                                                    \
    printf((format), __VA_ARGS__);                                                                                     \
    printf("\n");                                                                                                      \
  }

#include "../common/Common.h"
#include "../render/Camera.h"
#include "../render/RendererFacade.h"
#include "../rhi/vulkan/VulkanCommandList.h"
#include "../rhi/vulkan/VulkanSurface.h"

#include <android_native_app_glue.h>

#include <algorithm>
#include <memory>

namespace {

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
  }

  void shutdown()
  {
    if(!m_initialized)
    {
      return;
    }
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
    ImGui::End();

    demo::RenderParams params{};
    params.viewportSize = m_viewportSize;
    params.deltaTime = ImGui::GetIO().DeltaTime;
    params.timeSeconds = static_cast<float>(ImGui::GetTime());
    params.cameraUniforms = &m_cameraUniforms;
    params.lightSettings = m_lightSettings;
    params.debugOptions = m_debugOptions;
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
    m_cameraUniforms.cameraPosition = m_camera.getPosition();
    m_cameraUniforms.shadowConstantBias = 0.0f;
    m_cameraUniforms.shadowDirectionAndSlopeBias = glm::vec4(0.0f);
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
