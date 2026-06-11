#include "LightResources.h"

#include <algorithm>
#include <cstring>
#include <span>

namespace demo
{
	namespace
	{
		constexpr uint64_t kCoarseBoundsElementSize = sizeof(uint16_t) * 4;

		rhi::BufferHandle createStorageBuffer(rhi::Device& device, uint64_t size, rhi::MemoryUsage memoryUsage)
		{
			return device.createBuffer(rhi::BufferDesc{
				.size = std::max<uint64_t>(size, 16),
				.usage = rhi::BufferUsageFlags::storage,
				.memoryUsage = memoryUsage,
				.debugName = "LightStorageBuffer",
			});
		}

		rhi::BufferHandle createUniformBuffer(rhi::Device& device, uint64_t size, rhi::MemoryUsage memoryUsage)
		{
			return device.createBuffer(rhi::BufferDesc{
				.size = std::max<uint64_t>(size, 16),
				.usage = rhi::BufferUsageFlags::uniform,
				.memoryUsage = memoryUsage,
				.debugName = "LightUniformBuffer",
			});
		}

		void updateMappedStorageBuffer(rhi::Device& device, rhi::BufferHandle buffer,
		                               std::span<const shaderio::LightData> lights)
		{
			if (buffer.isNull() || lights.empty())
			{
				return;
			}
			void* mappedData = device.mapBuffer(buffer);
			if (mappedData == nullptr)
			{
				return;
			}
			const uint64_t copySize = sizeof(shaderio::LightData) * lights.size();
			std::memcpy(mappedData, lights.data(), static_cast<size_t>(copySize));
		}

		void updateMappedUniformBuffer(rhi::Device& device, rhi::BufferHandle buffer,
		                               const shaderio::LightCoarseCullingUniforms& uniforms)
		{
			if (buffer.isNull())
			{
				return;
			}
			void* mappedData = device.mapBuffer(buffer);
			if (mappedData == nullptr)
			{
				return;
			}
			std::memcpy(mappedData, &uniforms, sizeof(uniforms));
		}
	} // namespace

	void LightResources::init(rhi::Device& device, const CreateInfo& createInfo)
	{
		deinit();

		m_rhiDevice = &device;
		m_maxPointLights = std::max(1u, createInfo.maxPointLights);
		m_maxSpotLights = std::max(1u, createInfo.maxSpotLights);
		m_frames.resize(std::max(1u, createInfo.frameCount));

		const uint64_t pointLightBufferSize = sizeof(shaderio::LightData) * m_maxPointLights;
		const uint64_t spotLightBufferSize = sizeof(shaderio::LightData) * m_maxSpotLights;
		const uint64_t pointBoundsBufferSize = kCoarseBoundsElementSize * m_maxPointLights;
		const uint64_t spotBoundsBufferSize = kCoarseBoundsElementSize * m_maxSpotLights;
		const uint64_t coarseUniformBufferSize = sizeof(shaderio::LightCoarseCullingUniforms);

		for (FrameResources& frame : m_frames)
		{
			frame.pointLightBuffer = createStorageBuffer(device, pointLightBufferSize, rhi::MemoryUsage::cpuToGpu);
			frame.spotLightBuffer = createStorageBuffer(device, spotLightBufferSize, rhi::MemoryUsage::cpuToGpu);
			frame.pointCoarseBoundsBuffer = createStorageBuffer(device, pointBoundsBufferSize,
			                                                    rhi::MemoryUsage::gpuOnly);
			frame.spotCoarseBoundsBuffer = createStorageBuffer(device, spotBoundsBufferSize, rhi::MemoryUsage::gpuOnly);
			frame.coarseCullingUniformBuffer = createUniformBuffer(device, coarseUniformBufferSize,
			                                                       rhi::MemoryUsage::cpuToGpu);
		}
	}

	void LightResources::deinit()
	{
		if (m_rhiDevice != nullptr)
		{
			for (FrameResources& frame : m_frames)
			{
				if (!frame.pointLightBuffer.isNull()) m_rhiDevice->destroyBuffer(frame.pointLightBuffer);
				if (!frame.spotLightBuffer.isNull()) m_rhiDevice->destroyBuffer(frame.spotLightBuffer);
				if (!frame.pointCoarseBoundsBuffer.isNull()) m_rhiDevice->destroyBuffer(frame.pointCoarseBoundsBuffer);
				if (!frame.spotCoarseBoundsBuffer.isNull()) m_rhiDevice->destroyBuffer(frame.spotCoarseBoundsBuffer);
				if (!frame.coarseCullingUniformBuffer.isNull()) m_rhiDevice->destroyBuffer(
					frame.coarseCullingUniformBuffer);
			}
		}

		m_frames.clear();
		m_rhiDevice = nullptr;
		m_maxPointLights = 256;
		m_maxSpotLights = 128;
	}

	void LightResources::updatePointLights(uint32_t frameIndex, const std::vector<shaderio::LightData>& lights)
	{
		if (frameIndex >= m_frames.size())
		{
			return;
		}
		const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(lights.size()), m_maxPointLights);
		updateMappedStorageBuffer(*m_rhiDevice, m_frames[frameIndex].pointLightBuffer, std::span(lights.data(), count));
	}

	void LightResources::updateSpotLights(uint32_t frameIndex, const std::vector<shaderio::LightData>& lights)
	{
		if (frameIndex >= m_frames.size())
		{
			return;
		}
		const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(lights.size()), m_maxSpotLights);
		updateMappedStorageBuffer(*m_rhiDevice, m_frames[frameIndex].spotLightBuffer, std::span(lights.data(), count));
	}

	void LightResources::updateCoarseCullingUniforms(uint32_t frameIndex,
	                                                 const shaderio::LightCoarseCullingUniforms& uniforms)
	{
		if (frameIndex >= m_frames.size())
		{
			return;
		}
		updateMappedUniformBuffer(*m_rhiDevice, m_frames[frameIndex].coarseCullingUniformBuffer, uniforms);
	}

	rhi::BufferHandle LightResources::getPointLightBuffer(uint32_t frameIndex) const
	{
		return frameIndex < m_frames.size() ? m_frames[frameIndex].pointLightBuffer : rhi::BufferHandle{};
	}

	rhi::BufferHandle LightResources::getSpotLightBuffer(uint32_t frameIndex) const
	{
		return frameIndex < m_frames.size() ? m_frames[frameIndex].spotLightBuffer : rhi::BufferHandle{};
	}

	rhi::BufferHandle LightResources::getPointCoarseBoundsBuffer(uint32_t frameIndex) const
	{
		return frameIndex < m_frames.size() ? m_frames[frameIndex].pointCoarseBoundsBuffer : rhi::BufferHandle{};
	}

	rhi::BufferHandle LightResources::getSpotCoarseBoundsBuffer(uint32_t frameIndex) const
	{
		return frameIndex < m_frames.size() ? m_frames[frameIndex].spotCoarseBoundsBuffer : rhi::BufferHandle{};
	}

	rhi::BufferHandle LightResources::getCoarseCullingUniformBuffer(uint32_t frameIndex) const
	{
		return frameIndex < m_frames.size() ? m_frames[frameIndex].coarseCullingUniformBuffer : rhi::BufferHandle{};
	}
} // namespace demo
