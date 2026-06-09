#include "GPUDrivenLightResources.h"

#include <algorithm>
#include <cstring>

namespace demo
{
	namespace
	{
		constexpr uint64_t kCoarseBoundsElementSize = sizeof(uint16_t) * 4u;
	} // namespace

	void GPUDrivenLightResources::init(rhi::Device& device, const CreateInfo& createInfo)
	{
		deinit();

		m_device = &device;
		m_maxPointLights = std::max(1u, createInfo.maxPointLights);
		m_maxSpotLights = std::max(1u, createInfo.maxSpotLights);
		m_frames.resize(std::max(1u, createInfo.frameCount));

		const uint64_t pointLightBytes = sizeof(shaderio::LightData) * static_cast<uint64_t>(m_maxPointLights);
		const uint64_t spotLightBytes = sizeof(shaderio::LightData) * static_cast<uint64_t>(m_maxSpotLights);
		const uint64_t pointCoarseBoundsBytes = kCoarseBoundsElementSize * static_cast<uint64_t>(m_maxPointLights);
		const uint64_t spotCoarseBoundsBytes = kCoarseBoundsElementSize * static_cast<uint64_t>(m_maxSpotLights);
		const uint64_t clusterCountBytes = sizeof(uint32_t) * static_cast<uint64_t>(shaderio::LClusterCount);
		const uint64_t clusterIndexBytes =
			sizeof(uint32_t) * static_cast<uint64_t>(shaderio::LClusterCount) * shaderio::LMaxLightsPerCluster;

		for (FrameResources& frame : m_frames)
		{
			frame.pointLightBuffer =
				createStorageBuffer(pointLightBytes, rhi::MemoryUsage::cpuToGpu);
			frame.spotLightBuffer =
				createStorageBuffer(spotLightBytes, rhi::MemoryUsage::cpuToGpu);
			frame.pointCoarseBoundsBuffer = createStorageBuffer(pointCoarseBoundsBytes, rhi::MemoryUsage::gpuOnly);
			frame.spotCoarseBoundsBuffer = createStorageBuffer(spotCoarseBoundsBytes, rhi::MemoryUsage::gpuOnly);
			frame.lightingUniformBuffer =
				createUniformBuffer(sizeof(shaderio::LightingUniforms), rhi::MemoryUsage::cpuToGpu);
			frame.coarseUniformBuffer =
				createUniformBuffer(sizeof(shaderio::LightCoarseCullingUniforms), rhi::MemoryUsage::cpuToGpu);
			frame.clusteredUniformBuffer =
				createUniformBuffer(sizeof(shaderio::ClusteredLightUniforms), rhi::MemoryUsage::cpuToGpu);
			frame.clusterCountsBuffer = createStorageBuffer(clusterCountBytes, rhi::MemoryUsage::gpuOnly);
			frame.clusterIndicesBuffer = createStorageBuffer(clusterIndexBytes, rhi::MemoryUsage::gpuOnly);
			frame.clusterStatsBuffer =
				createStorageBuffer(sizeof(ClusterStats), rhi::MemoryUsage::gpuToCpu);
		}
	}

	void GPUDrivenLightResources::deinit()
	{
		if (m_device != nullptr)
		{
			for (FrameResources& frame : m_frames)
			{
				destroyBuffer(frame.pointLightBuffer);
				destroyBuffer(frame.spotLightBuffer);
				destroyBuffer(frame.pointCoarseBoundsBuffer);
				destroyBuffer(frame.spotCoarseBoundsBuffer);
				destroyBuffer(frame.lightingUniformBuffer);
				destroyBuffer(frame.coarseUniformBuffer);
				destroyBuffer(frame.clusteredUniformBuffer);
				destroyBuffer(frame.clusterCountsBuffer);
				destroyBuffer(frame.clusterIndicesBuffer);
				destroyBuffer(frame.clusterStatsBuffer);
			}
		}

		m_frames.clear();
		m_device = nullptr;
		m_maxPointLights = 256;
		m_maxSpotLights = 128;
		m_activePointLights = 0;
		m_activeSpotLights = 0;
		m_lastClusterStats = {};
	}

	void GPUDrivenLightResources::updateLights(uint32_t frameIndex,
	                                           const std::vector<shaderio::LightData>& pointLights,
	                                           const std::vector<shaderio::LightData>& spotLights)
	{
		if (frameIndex >= m_frames.size())
		{
			m_activePointLights = 0;
			m_activeSpotLights = 0;
			return;
		}

		FrameResources& frame = m_frames[frameIndex];
		m_activePointLights = std::min<uint32_t>(static_cast<uint32_t>(pointLights.size()), m_maxPointLights);
		m_activeSpotLights = std::min<uint32_t>(static_cast<uint32_t>(spotLights.size()), m_maxSpotLights);
		if (m_activePointLights > 0)
		{
			updateMappedBuffer(frame.pointLightBuffer, pointLights.data(),
			                   sizeof(shaderio::LightData) * m_activePointLights);
		}
		if (m_activeSpotLights > 0)
		{
			updateMappedBuffer(frame.spotLightBuffer, spotLights.data(),
			                   sizeof(shaderio::LightData) * m_activeSpotLights);
		}
	}

	void GPUDrivenLightResources::updateUniforms(uint32_t frameIndex,
	                                             const shaderio::LightingUniforms& lightingUniforms,
	                                             const shaderio::LightCoarseCullingUniforms& coarseUniforms,
	                                             const shaderio::ClusteredLightUniforms& clusteredUniforms)
	{
		if (frameIndex >= m_frames.size())
		{
			return;
		}

		updateMappedBuffer(m_frames[frameIndex].lightingUniformBuffer, &lightingUniforms, sizeof(lightingUniforms));
		updateMappedBuffer(m_frames[frameIndex].coarseUniformBuffer, &coarseUniforms, sizeof(coarseUniforms));
		updateMappedBuffer(m_frames[frameIndex].clusteredUniformBuffer, &clusteredUniforms, sizeof(clusteredUniforms));
	}

	rhi::BufferHandle GPUDrivenLightResources::getPointLightBuffer(uint32_t frameIndex) const
	{
		return frameIndex < m_frames.size() ? m_frames[frameIndex].pointLightBuffer : rhi::BufferHandle{};
	}

	rhi::BufferHandle GPUDrivenLightResources::getSpotLightBuffer(uint32_t frameIndex) const
	{
		return frameIndex < m_frames.size() ? m_frames[frameIndex].spotLightBuffer : rhi::BufferHandle{};
	}

	rhi::BufferHandle GPUDrivenLightResources::getPointCoarseBoundsBuffer(uint32_t frameIndex) const
	{
		return frameIndex < m_frames.size() ? m_frames[frameIndex].pointCoarseBoundsBuffer : rhi::BufferHandle{};
	}

	rhi::BufferHandle GPUDrivenLightResources::getSpotCoarseBoundsBuffer(uint32_t frameIndex) const
	{
		return frameIndex < m_frames.size() ? m_frames[frameIndex].spotCoarseBoundsBuffer : rhi::BufferHandle{};
	}

	rhi::BufferHandle GPUDrivenLightResources::getCoarseUniformBuffer(uint32_t frameIndex) const
	{
		return frameIndex < m_frames.size() ? m_frames[frameIndex].coarseUniformBuffer : rhi::BufferHandle{};
	}

	rhi::BufferHandle GPUDrivenLightResources::getLightingUniformBuffer(uint32_t frameIndex) const
	{
		return frameIndex < m_frames.size() ? m_frames[frameIndex].lightingUniformBuffer : rhi::BufferHandle{};
	}

	rhi::BufferHandle GPUDrivenLightResources::getClusteredUniformBuffer(uint32_t frameIndex) const
	{
		return frameIndex < m_frames.size() ? m_frames[frameIndex].clusteredUniformBuffer : rhi::BufferHandle{};
	}

	rhi::BufferHandle GPUDrivenLightResources::getClusterCountsBuffer(uint32_t frameIndex) const
	{
		return frameIndex < m_frames.size() ? m_frames[frameIndex].clusterCountsBuffer : rhi::BufferHandle{};
	}

	rhi::BufferHandle GPUDrivenLightResources::getClusterIndicesBuffer(uint32_t frameIndex) const
	{
		return frameIndex < m_frames.size() ? m_frames[frameIndex].clusterIndicesBuffer : rhi::BufferHandle{};
	}

	rhi::BufferHandle GPUDrivenLightResources::getClusterStatsBuffer(uint32_t frameIndex) const
	{
		return frameIndex < m_frames.size() ? m_frames[frameIndex].clusterStatsBuffer : rhi::BufferHandle{};
	}

	void GPUDrivenLightResources::cacheClusterStats(uint32_t frameIndex)
	{
		if (frameIndex >= m_frames.size())
		{
			m_lastClusterStats = {};
			return;
		}

		const rhi::BufferHandle buffer = m_frames[frameIndex].clusterStatsBuffer;
		void* mapped = m_device != nullptr ? m_device->mapBuffer(buffer) : nullptr;
		if (buffer.isNull() || mapped == nullptr)
		{
			m_lastClusterStats = {};
			return;
		}

		std::memcpy(&m_lastClusterStats, mapped, sizeof(m_lastClusterStats));
	}

	GPUDrivenLightResources::Diagnostics GPUDrivenLightResources::getDiagnostics() const
	{
		const uint64_t clusterCountsBytes = sizeof(uint32_t) * static_cast<uint64_t>(shaderio::LClusterCount);
		const uint64_t clusterIndicesBytes =
			sizeof(uint32_t) * static_cast<uint64_t>(shaderio::LClusterCount) * shaderio::LMaxLightsPerCluster;
		const uint64_t clusterStatsBytes = sizeof(ClusterStats);
		const uint64_t lightBytes =
			sizeof(shaderio::LightData) * static_cast<uint64_t>(m_maxPointLights + m_maxSpotLights)
			+ kCoarseBoundsElementSize * static_cast<uint64_t>(m_maxPointLights + m_maxSpotLights)
			+ sizeof(shaderio::LightingUniforms)
			+ sizeof(shaderio::LightCoarseCullingUniforms)
			+ sizeof(shaderio::ClusteredLightUniforms);

		const bool hasFrame = !m_frames.empty();
		return Diagnostics{
			.clusterGridX = shaderio::LClusterGridSizeX,
			.clusterGridY = shaderio::LClusterGridSizeY,
			.clusterGridZ = shaderio::LClusterGridSizeZ,
			.clusterCount = shaderio::LClusterCount,
			.maxLightsPerCluster = shaderio::LMaxLightsPerCluster,
			.maxPointLights = m_maxPointLights,
			.maxSpotLights = m_maxSpotLights,
			.activePointLights = m_activePointLights,
			.activeSpotLights = m_activeSpotLights,
			.maxOccupancy = m_lastClusterStats.maxOccupancy,
			.overflowClusterCount = m_lastClusterStats.overflowClusterCount,
			.appendedLightReferences = m_lastClusterStats.appendedLightReferences,
			.clusterMemoryBytes = clusterCountsBytes + clusterIndicesBytes + clusterStatsBytes,
			.lightMemoryBytes = lightBytes,
			.initialized = m_device != nullptr && hasFrame,
			.clusteredDescriptorsReady = hasFrame && !m_frames.front().clusterCountsBuffer.isNull()
			&& !m_frames.front().clusterIndicesBuffer.isNull(),
			.lightingDescriptorsReady = hasFrame && !m_frames.front().pointLightBuffer.isNull()
			&& !m_frames.front().coarseUniformBuffer.isNull(),
		};
	}

	rhi::BufferHandle GPUDrivenLightResources::createStorageBuffer(uint64_t size, rhi::MemoryUsage usage) const
	{
		return m_device->createBuffer(rhi::BufferDesc{
			.size = std::max<uint64_t>(size, 16),
			.usage = rhi::BufferUsageFlags::storage | rhi::BufferUsageFlags::transferDst |
			rhi::BufferUsageFlags::transferSrc,
			.memoryUsage = usage,
			.allowArgumentTableBinding = true,
			.debugName = "GPUDrivenLightStorageBuffer",
		});
	}

	rhi::BufferHandle GPUDrivenLightResources::createUniformBuffer(uint64_t size, rhi::MemoryUsage usage) const
	{
		return m_device->createBuffer(rhi::BufferDesc{
			.size = std::max<uint64_t>(size, 16),
			.usage = rhi::BufferUsageFlags::uniform,
			.memoryUsage = usage,
			.allowArgumentTableBinding = true,
			.debugName = "GPUDrivenLightUniformBuffer",
		});
	}

	void GPUDrivenLightResources::destroyBuffer(rhi::BufferHandle& buffer) const
	{
		if (m_device != nullptr && !buffer.isNull())
		{
			m_device->destroyBuffer(buffer);
			buffer = {};
		}
	}

	void GPUDrivenLightResources::updateMappedBuffer(rhi::BufferHandle buffer, const void* data, uint64_t size) const
	{
		if (buffer.isNull() || data == nullptr || size == 0 || m_device == nullptr)
		{
			return;
		}

		void* mappedData = m_device->mapBuffer(buffer);
		ASSERT(mappedData != nullptr, "GPUDrivenLightResources update requires mapped buffer");
		std::memcpy(mappedData, data, static_cast<size_t>(size));
	}
} // namespace demo
