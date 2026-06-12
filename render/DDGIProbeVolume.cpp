#include "DDGIProbeVolume.h"

#include "../common/Common.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace demo
{
	glm::uvec3 DDGIProbeVolume::computeGridDims(const glm::vec3& boundsMin,
	                                            const glm::vec3& boundsMax,
	                                            float probeSpacing)
	{
		const float spacing = std::max(probeSpacing, 1e-3f);
		const glm::vec3 sceneLength = glm::max(boundsMax - boundsMin, glm::vec3(0.0f));
		glm::uvec3 dims{};
		for (int axis = 0; axis < 3; ++axis)
		{
			// ivec3(sceneLength / probeSpacing) + 2 boundary probes, then a hard
			// per-axis ceiling so a huge scene AABB cannot blow up GPU memory.
			const uint32_t interior = static_cast<uint32_t>(sceneLength[axis] / spacing);
			dims[axis] = std::clamp(interior + 2u, 2u, kMaxGridDims[axis]);
		}
		return dims;
	}

	DDGIProbeVolumeDesc DDGIProbeVolume::makeDesc(const DDGIConfig& config,
	                                              const glm::vec3& sceneBoundsMin,
	                                              const glm::vec3& sceneBoundsMax)
	{
		return DDGIProbeVolumeDesc{
			.gridDims = config.gridDims, // 0 => derived from bounds in init()
			.probeSpacing = config.probeSpacing,
			.irradianceTexelSize = config.irradianceTexelSize,
			.depthTexelSize = config.depthTexelSize,
			.raysPerProbe = config.raysPerProbe,
			.sceneBoundsMin = sceneBoundsMin,
			.sceneBoundsMax = sceneBoundsMax,
		};
	}

	rhi::Extent2D DDGIProbeVolume::atlasExtent(const glm::uvec3& gridDims, uint32_t texelSize)
	{
		const uint32_t tile = texelSize + kProbeBorderTexels;
		return rhi::Extent2D{
			.width = tile * gridDims.x * gridDims.y + kAtlasEdgeTexels,
			.height = tile * gridDims.z + kAtlasEdgeTexels,
		};
	}

	void DDGIProbeVolume::init(rhi::Device& device, const DDGIProbeVolumeDesc& desc)
	{
		deinit();

		m_device = &device;
		m_desc = desc;
		if (m_desc.gridDims.x == 0u || m_desc.gridDims.y == 0u || m_desc.gridDims.z == 0u)
		{
			m_desc.gridDims = computeGridDims(m_desc.sceneBoundsMin, m_desc.sceneBoundsMax, m_desc.probeSpacing);
		}
		m_desc.gridDims = glm::min(m_desc.gridDims, kMaxGridDims);
		m_desc.irradianceTexelSize = std::max(m_desc.irradianceTexelSize, 1u);
		m_desc.depthTexelSize = std::max(m_desc.depthTexelSize, 1u);
		m_desc.raysPerProbe = std::max(m_desc.raysPerProbe, 1u);
		m_totalProbes = m_desc.gridDims.x * m_desc.gridDims.y * m_desc.gridDims.z;

		m_irradianceAtlasExtent = atlasExtent(m_desc.gridDims, m_desc.irradianceTexelSize);
		m_depthAtlasExtent = atlasExtent(m_desc.gridDims, m_desc.depthTexelSize);
		m_radianceExtent = rhi::Extent2D{.width = m_desc.raysPerProbe, .height = m_totalProbes};

		const auto createAtlas = [&](rhi::TextureFormat format, const rhi::Extent2D& extent,
		                             const char* debugName)
		{
			// storage: written by the probe-update compute passes (D2-3);
			// sampled: read back by SampleProbe in the lighting pass (D3-1) and
			// by the history blend (D4-1).
			return device.createTexture(rhi::TextureDesc{
				.dimension = rhi::TextureDimension::e2D,
				.format = format,
				.usage = rhi::TextureUsageFlags::sampled | rhi::TextureUsageFlags::storage,
				.extent = {extent.width, extent.height, 1u},
				.mipLevels = 1,
				.arrayLayers = 1,
				.sampleCount = rhi::SampleCount::count1,
				.memoryUsage = rhi::MemoryUsage::gpuOnly,
				.debugName = debugName,
			});
		};

		m_irradianceAtlas = createAtlas(rhi::TextureFormat::rgba16Sfloat, m_irradianceAtlasExtent,
		                                "ddgi-irradiance-atlas");
		m_irradianceAtlasHistory = createAtlas(rhi::TextureFormat::rgba16Sfloat, m_irradianceAtlasExtent,
		                                       "ddgi-irradiance-atlas-history");
		m_depthAtlas = createAtlas(rhi::TextureFormat::rg16Sfloat, m_depthAtlasExtent,
		                           "ddgi-depth-atlas");
		m_depthAtlasHistory = createAtlas(rhi::TextureFormat::rg16Sfloat, m_depthAtlasExtent,
		                                  "ddgi-depth-atlas-history");
		// Per-ray hit radiance: row = probe index, column = ray index. Written
		// by GISDFRays (D2-2), consumed by the probe update passes (D2-3).
		m_radianceBuffer = createAtlas(rhi::TextureFormat::rgba16Sfloat, m_radianceExtent,
		                               "ddgi-radiance");

		// Probe world positions: host-visible storage buffer consumed through
		// its GPU address (same pattern as the gpuCulling cpuToGpu buffers).
		m_probePositionBuffer = device.createBuffer(rhi::BufferDesc{
			.size = sizeof(glm::vec4) * static_cast<uint64_t>(m_totalProbes),
			.usage = rhi::BufferUsageFlags::storage | rhi::BufferUsageFlags::shaderDeviceAddress,
			.memoryUsage = rhi::MemoryUsage::cpuToGpu,
			.allowGpuAddress = true,
			.debugName = "ddgi-probe-positions",
		});
		m_probePositionAddress = device.getBufferGpuAddress(m_probePositionBuffer);
		uploadProbePositions();
	}

	void DDGIProbeVolume::deinit()
	{
		if (m_device == nullptr)
		{
			return;
		}

		if (!m_probePositionBuffer.isNull()) m_device->destroyBuffer(m_probePositionBuffer);
		m_probePositionBuffer = {};
		m_probePositionAddress = {};

		if (!m_radianceBuffer.isNull()) m_device->destroyTexture(m_radianceBuffer);
		if (!m_depthAtlasHistory.isNull()) m_device->destroyTexture(m_depthAtlasHistory);
		if (!m_depthAtlas.isNull()) m_device->destroyTexture(m_depthAtlas);
		if (!m_irradianceAtlasHistory.isNull()) m_device->destroyTexture(m_irradianceAtlasHistory);
		if (!m_irradianceAtlas.isNull()) m_device->destroyTexture(m_irradianceAtlas);
		m_radianceBuffer = {};
		m_depthAtlasHistory = {};
		m_depthAtlas = {};
		m_irradianceAtlasHistory = {};
		m_irradianceAtlas = {};

		m_irradianceAtlasExtent = {};
		m_depthAtlasExtent = {};
		m_radianceExtent = {};
		m_totalProbes = 0;
		m_desc = {};
		m_device = nullptr;
	}

	void DDGIProbeVolume::swapAtlases()
	{
		std::swap(m_irradianceAtlas, m_irradianceAtlasHistory);
		std::swap(m_depthAtlas, m_depthAtlasHistory);
	}

	void DDGIProbeVolume::uploadProbePositions() const
	{
		if (m_device == nullptr || m_probePositionBuffer.isNull() || m_totalProbes == 0u)
		{
			return;
		}

		std::vector<glm::vec4> positions;
		positions.reserve(m_totalProbes);
		for (uint32_t z = 0; z < m_desc.gridDims.z; ++z)
		{
			for (uint32_t y = 0; y < m_desc.gridDims.y; ++y)
			{
				for (uint32_t x = 0; x < m_desc.gridDims.x; ++x)
				{
					const glm::vec3 position = m_desc.sceneBoundsMin
						+ glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z))
						* m_desc.probeSpacing;
					positions.emplace_back(position, 1.0f);
				}
			}
		}

		// cpuToGpu memory is persistently mapped and host-coherent on the
		// targeted devices (same contract as GPUDrivenLightResources uploads).
		void* mapped = m_device->mapBuffer(m_probePositionBuffer);
		ASSERT(mapped != nullptr, "DDGIProbeVolume probe position upload requires a mapped buffer");
		std::memcpy(mapped, positions.data(), sizeof(glm::vec4) * positions.size());
	}
} // namespace demo
