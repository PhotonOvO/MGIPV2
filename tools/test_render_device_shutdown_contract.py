"""Source contracts for RenderDevice retirement-safe shutdown and re-init."""

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


RENDER_DEVICE = read("render/RenderDevice.cpp")
VULKAN_DEVICE = read("rhi/vulkan/VulkanDevice.cpp")
VULKAN_DEVICE_HEADER = read("rhi/vulkan/VulkanDevice.h")
ANDROID_APP = read("app/AndroidNativeApp.cpp")


def function_source(source: str, signature: str) -> str:
    start = source.index(signature)
    opening_brace = source.index("{", start)
    depth = 0
    for index in range(opening_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"Unterminated function: {signature}")


class RenderDeviceShutdownContractTests(unittest.TestCase):
    def test_frame_context_outlives_resource_retirement_and_is_detached(self) -> None:
        shutdown = function_source(
            RENDER_DEVICE,
            "void RenderDevice::shutdown(",
        )

        final_wait_idle = shutdown.rindex("m_device.device->waitIdle();")
        detach_frame_context = shutdown.index("setFrameContext(nullptr);")
        deinit_frame_context = shutdown.index("m_perFrame.frameContext->deinit();")
        reset_frame_context = shutdown.index("m_perFrame.frameContext.reset();")
        deinit_surface = shutdown.index("surface.deinit();")
        deinit_device = shutdown.index("m_device.device->deinit();")

        for retirement_producer in (
            "destroySampler(",
            "destroyTextureView(",
            "destroyBuffer(",
            "destroyTexture(",
            "destroyPipelines();",
            "m_csmShadowResources.deinit();",
            "m_swapchainDependent.sceneResources.deinit();",
            "m_imguiRenderer.shutdown();",
            "m_meshPool.deinit();",
            "freeRhiStagingBuffers(",
        ):
            self.assertLess(
                shutdown.rindex(retirement_producer),
                final_wait_idle,
                retirement_producer,
            )

        self.assertLess(final_wait_idle, detach_frame_context)
        self.assertLess(detach_frame_context, deinit_frame_context)
        self.assertLess(deinit_frame_context, reset_frame_context)
        self.assertLess(reset_frame_context, deinit_surface)
        self.assertLess(deinit_surface, deinit_device)
        self.assertEqual(shutdown.count("m_perFrame.frameContext->deinit();"), 1)
        self.assertIn("drained all pending native retirements", shutdown)

    def test_vulkan_device_raw_frame_context_pointer_is_rewired_on_reinit(self) -> None:
        create_frame_context = function_source(
            VULKAN_DEVICE,
            "std::unique_ptr<FrameContext> VulkanDevice::createFrameContext(",
        )
        retirement_timeline = function_source(
            VULKAN_DEVICE,
            "uint64_t VulkanDevice::retirementTimelineValue() const",
        )
        device_deinit = function_source(
            VULKAN_DEVICE,
            "void VulkanDevice::deinit()",
        )
        renderer_init = function_source(
            RENDER_DEVICE,
            "void RenderDevice::init(",
        )
        create_frame_submission = function_source(
            RENDER_DEVICE,
            "void RenderDevice::createFrameSubmission(",
        )

        self.assertIn("m_frameContext = frameContext.get();", create_frame_context)
        self.assertIn("if (m_frameContext == nullptr)", retirement_timeline)
        self.assertIn("m_frameContext->getCurrentFrameValue()", retirement_timeline)
        self.assertIn("m_frameContext = nullptr;", device_deinit)
        self.assertIn(
            "void setFrameContext(VulkanFrameContext* frameContext)",
            VULKAN_DEVICE_HEADER,
        )
        self.assertIn(
            "m_device.device->createFrameContext(",
            create_frame_submission,
        )
        self.assertLess(
            renderer_init.index("m_device.device->init(deviceCreateInfo);"),
            renderer_init.index("createFrameSubmission("),
        )

    def test_android_term_init_window_runs_full_shutdown_reinit_contract(self) -> None:
        handle_command = function_source(
            ANDROID_APP,
            "void handleCommand(",
        )
        android_init = function_source(
            ANDROID_APP,
            "void init(ANativeWindow* window)",
        )
        android_shutdown = function_source(
            ANDROID_APP,
            "void shutdown()",
        )

        self.assertRegex(
            handle_command,
            re.compile(r"APP_CMD_INIT_WINDOW.*?demoApp->init", re.DOTALL),
        )
        self.assertRegex(
            handle_command,
            re.compile(r"APP_CMD_TERM_WINDOW.*?demoApp->shutdown", re.DOTALL),
        )
        self.assertLess(
            android_init.index("m_surface = m_renderer.createSurface();"),
            android_init.index("m_renderer.init("),
        )
        self.assertLess(
            android_shutdown.index("m_renderer.waitForIdle();"),
            android_shutdown.index("m_renderer.shutdown("),
        )
        self.assertLess(
            android_shutdown.index("m_renderer.shutdown("),
            android_shutdown.index("m_surface.reset();"),
        )
        self.assertLess(
            android_shutdown.index("m_surface.reset();"),
            android_shutdown.index("m_initialized = false;"),
        )


if __name__ == "__main__":
    unittest.main()
