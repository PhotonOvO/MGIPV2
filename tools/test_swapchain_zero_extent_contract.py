"""Contracts for deferring desktop swapchain rebuilds at zero extent."""

import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


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


class SwapchainZeroExtentContractTests(unittest.TestCase):
    def test_vulkan_rebuild_preserves_live_swapchain_while_minimized(self) -> None:
        rebuild = function_source(
            read("rhi/vulkan/VulkanSwapchain.cpp"),
            "void VulkanSwapchain::rebuild()",
        )

        query = rebuild.index("vkGetPhysicalDeviceSurfaceCapabilitiesKHR(")
        zero_extent = rebuild.index("nextExtent.width == 0 || nextExtent.height == 0")
        defer = rebuild.index("m_needsRebuild = true;", zero_extent)
        early_return = rebuild.index("return;", defer)
        wait_idle = rebuild.index("vkQueueWaitIdle(m_queue);")
        destroy = rebuild.index("destroyResources();")

        self.assertLess(query, zero_extent)
        self.assertLess(zero_extent, defer)
        self.assertLess(defer, early_return)
        self.assertLess(early_return, wait_idle)
        self.assertLess(wait_idle, destroy)

    def test_d3d12_rebuild_also_defers_zero_extent(self) -> None:
        rebuild = function_source(
            read("rhi/d3d12/D3D12Swapchain.cpp"),
            "void D3D12Swapchain::rebuild()",
        )

        query = rebuild.index("const Extent2D nextExtent = queryClientExtent();")
        zero_extent = rebuild.index("nextExtent.width == 0 || nextExtent.height == 0")
        defer = rebuild.index("m_needsRebuild = true;", zero_extent)
        early_return = rebuild.index("return;", defer)
        wait_idle = rebuild.index("waitQueueIdle();")

        self.assertLess(query, zero_extent)
        self.assertLess(zero_extent, defer)
        self.assertLess(defer, early_return)
        self.assertLess(early_return, wait_idle)


if __name__ == "__main__":
    unittest.main()
