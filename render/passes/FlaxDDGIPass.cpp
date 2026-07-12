#include "FlaxDDGIPass.h"

#include "../FlaxDDGIResources.h"
#include "../GPUDrivenRenderer.h"
#include "../ArgumentTables.h"
#include "GlobalSDFPass.h"
#include "GlobalSurfaceAtlasPass.h"

#ifdef USE_SLANG
#include "_autogen/ddgi_flax_classify.slang.h"
#include "_autogen/ddgi_flax_init_args.slang.h"
#include "_autogen/ddgi_flax_update_inactive.slang.h"
#include "_autogen/ddgi_flax_trace_rays.slang.h"
#include "_autogen/ddgi_flax_update_distance.slang.h"
#include "_autogen/ddgi_flax_update_irradiance.slang.h"
#endif

namespace demo
{

namespace
{
constexpr uint64_t kDispatchIndirectStride = 3u * sizeof(uint32_t);
constexpr uint32_t kFlaxImplementedCascadeCount = 1u;

constexpr uint64_t indirectOffset(FlaxDDGIResources::IndirectPass pass)
{
  return static_cast<uint64_t>(pass) * kDispatchIndirectStride;
}

constexpr uint64_t kTraceIndirectOffset =
  indirectOffset(FlaxDDGIResources::IndirectPass::Trace);
constexpr uint64_t kDistanceIndirectOffset =
  indirectOffset(FlaxDDGIResources::IndirectPass::Distance);
constexpr uint64_t kIrradianceIndirectOffset =
  indirectOffset(FlaxDDGIResources::IndirectPass::Irradiance);

struct FlaxDDGIInitArgsPush
{
  uint32_t maxActiveProbes{0};
  uint32_t maxUpdatedProbesPerFrame{0};
};
static_assert(sizeof(FlaxDDGIInitArgsPush) == 2 * sizeof(uint32_t));
} // namespace

FlaxDDGIPass::FlaxDDGIPass(GPUDrivenRenderer* renderer)
  : m_renderer(renderer)
{
}

void FlaxDDGIPass::initResources(rhi::Device& device)
{
  shutdownResources();
  if (!m_renderer) return;

  m_device = &device;
  m_flaxResources = &m_renderer->getFlaxDDGIResources();

  if (!m_flaxResources->isInitialized())
  {
    LOGW("FlaxDDGIPass::initResources skipped: FlaxDDGIResources not initialized");
    m_device = nullptr;
    m_flaxResources = nullptr;
    return;
  }

  const GlobalSDFPass* sdfPass = m_renderer->getGlobalSDFPass();
  if (sdfPass == nullptr || sdfPass->getVolume().sdfTexture.isNull())
  {
    LOGE("FlaxDDGIPass::initResources requires an initialized Global SDF volume");
    m_device = nullptr;
    m_flaxResources = nullptr;
    return;
  }

	const auto& ddgiConfig = m_renderer->getDDGIConfig();
	const uint32_t cascadeCount = m_flaxResources->getCascadeCount();
	const uint32_t ddgiUBOSize = sizeof(shaderio::FlaxDDGIData);

	// --- Per-frame uniform buffers ---
	for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
	{
		m_ddgiUniformBuffers[i] = device.createBuffer(rhi::BufferDesc{
			.size = ddgiUBOSize,
			.usage = rhi::BufferUsageFlags::storage,
			.memoryUsage = rhi::MemoryUsage::cpuToGpu,
			.debugName = "FlaxDDGI.Data",
		});
	}

	// --- Create texture views ---
  auto createTextureView = [&](rhi::TextureHandle tex, rhi::TextureFormat format,
                               rhi::TextureAspect aspect, const char* name) {
    rhi::TextureViewCreateDesc vd{};
    vd.image = tex;
    vd.format = format;
    vd.viewType = rhi::ImageViewType::e2D;
    vd.aspect = aspect;
    vd.baseMipLevel = 0;
    vd.levelCount = 1;
    vd.baseArrayLayer = 0;
    vd.layerCount = 1;
    vd.debugName = name;
    return device.createTextureView(vd);
  };

  const rhi::TextureFormat probesDataFormat = m_flaxResources->usesSNORM()
    ? rhi::TextureFormat::rgba8Snorm
    : rhi::TextureFormat::rgba16Sfloat;
  m_probesDataView = createTextureView(m_flaxResources->getProbesData(), probesDataFormat,
                                       rhi::TextureAspect::color, "FlaxDDGI.ProbesData.View");
  m_probesTraceView = createTextureView(m_flaxResources->getProbesTrace(), rhi::TextureFormat::rgba16Sfloat,
                                        rhi::TextureAspect::color, "FlaxDDGI.ProbesTrace.View");
  m_probesIrradianceViewA = createTextureView(m_flaxResources->getProbesIrradianceWrite(0),
                                              rhi::TextureFormat::rgba16Sfloat,
                                              rhi::TextureAspect::color, "FlaxDDGI.IrradianceA.View");
  m_probesIrradianceViewB = createTextureView(m_flaxResources->getProbesIrradianceRead(0),
                                              rhi::TextureFormat::rgba16Sfloat,
                                              rhi::TextureAspect::color, "FlaxDDGI.IrradianceB.View");
  m_probesDistanceViewA = createTextureView(m_flaxResources->getProbesDistanceWrite(0),
                                            rhi::TextureFormat::rg16Sfloat,
                                            rhi::TextureAspect::color, "FlaxDDGI.DistanceA.View");
  m_probesDistanceViewB = createTextureView(m_flaxResources->getProbesDistanceRead(0),
                                             rhi::TextureFormat::rg16Sfloat,
                                             rhi::TextureAspect::color, "FlaxDDGI.DistanceB.View");

  // The classify and trace layouts both require a sampler. Own a valid
  // linear-clamp sampler for the full Flax pass lifetime so argument-table
  // creation never depends on GlobalSDFPass initialization timing.
  m_fallbackSampler = device.createSampler(rhi::SamplerDesc{
    .magFilter = rhi::Filter::linear,
    .minFilter = rhi::Filter::linear,
    .mipmapMode = rhi::MipmapMode::nearest,
    .addressModeU = rhi::AddressMode::clampToEdge,
    .addressModeV = rhi::AddressMode::clampToEdge,
    .addressModeW = rhi::AddressMode::clampToEdge,
    .debugName = "FlaxDDGI.LinearClampSampler",
  });
  if (m_fallbackSampler.isNull())
  {
    LOGE("FlaxDDGIPass::initResources failed to create the required SDF sampler");
    shutdownResources();
    return;
  }

	// --- Argument layouts ---
	// classify: probesData(RW,u0) + activeProbes(RW,u1) + globalSDF RO(t0) + sampler(s0) + ddgiUBO(b0)
	{
		std::vector<rhi::ArgumentBinding> bindings;
		bindings.push_back({0, rhi::ArgumentType::storageTexture, rhi::ShaderStage::compute, 1});
		bindings.push_back({1, rhi::ArgumentType::storageBuffer, rhi::ShaderStage::compute, 1});
		bindings.push_back({2, rhi::ArgumentType::sampledTexture, rhi::ShaderStage::compute, 1});
		bindings.push_back({3, rhi::ArgumentType::sampler, rhi::ShaderStage::compute, 1});
		bindings.push_back({4, rhi::ArgumentType::storageBuffer, rhi::ShaderStage::compute, 1});
		m_classifyLayout = device.createArgumentLayout({bindings.data(), static_cast<uint32_t>(bindings.size()), "FlaxDDGI.Classify"});
	}

  // initArgs: activeProbes(RW,u0) + initArgs(RW,u1)
  {
    std::vector<rhi::ArgumentBinding> bindings;
    bindings.push_back({0, rhi::ArgumentType::storageBuffer, rhi::ShaderStage::compute, 1});
    bindings.push_back({1, rhi::ArgumentType::storageBuffer, rhi::ShaderStage::compute, 1});
    m_initArgsLayout = device.createArgumentLayout({bindings.data(), static_cast<uint32_t>(bindings.size()), "FlaxDDGI.InitArgs"});
  }

	// updateInactive: probesData(RW,u0) + activeProbes(RW,u1) + ddgiUBO(b0)
	{
		std::vector<rhi::ArgumentBinding> bindings;
		bindings.push_back({0, rhi::ArgumentType::storageTexture, rhi::ShaderStage::compute, 1});
		bindings.push_back({1, rhi::ArgumentType::storageBuffer, rhi::ShaderStage::compute, 1});
		bindings.push_back({2, rhi::ArgumentType::storageBuffer, rhi::ShaderStage::compute, 1});
		m_updateInactiveLayout = device.createArgumentLayout({bindings.data(), static_cast<uint32_t>(bindings.size()), "FlaxDDGI.UpdateInactive"});
	}

  // traceRays: probesData(RO,t0) + activeProbes(RW,u0) + probesTrace(RW,u1)
  //            + globalSDF RO(t1) + sampler(s0)
  //            + atlasDepth(RO,t2) + atlasLighting(RO,t3) + chunks(RW,u2) + culledObjs(RW,u3) + objects(RW,u4)
  {
    std::vector<rhi::ArgumentBinding> bindings;
    bindings.push_back({0, rhi::ArgumentType::sampledTexture, rhi::ShaderStage::compute, 1});   // probesData
    bindings.push_back({1, rhi::ArgumentType::storageBuffer, rhi::ShaderStage::compute, 1});    // activeProbes
    bindings.push_back({2, rhi::ArgumentType::storageTexture, rhi::ShaderStage::compute, 1});   // probesTrace
    bindings.push_back({3, rhi::ArgumentType::combinedImageSampler, rhi::ShaderStage::compute, 1}); // globalSDF
    bindings.push_back({4, rhi::ArgumentType::sampler, rhi::ShaderStage::compute, 1});           // linearSampler
    bindings.push_back({5, rhi::ArgumentType::sampledTexture, rhi::ShaderStage::compute, 1});   // atlasDepth
    bindings.push_back({6, rhi::ArgumentType::sampledTexture, rhi::ShaderStage::compute, 1});   // atlasLighting
    bindings.push_back({7, rhi::ArgumentType::storageBuffer, rhi::ShaderStage::compute, 1});    // chunks
    bindings.push_back({8, rhi::ArgumentType::storageBuffer, rhi::ShaderStage::compute, 1});    // culledObjs
		bindings.push_back({9, rhi::ArgumentType::storageBuffer, rhi::ShaderStage::compute, 1});    // objects
		bindings.push_back({10, rhi::ArgumentType::storageBuffer, rhi::ShaderStage::compute, 1});    // ddgiSSBO
		bindings.push_back({11, rhi::ArgumentType::sampledTexture, rhi::ShaderStage::compute, 1});   // globalAlbedo
		m_traceRaysLayout = device.createArgumentLayout({bindings.data(), static_cast<uint32_t>(bindings.size()), "FlaxDDGI.TraceRays"});
	}

	// distance: probesTrace(RO,t0) + probesData(RO,t1) + probesDistance(RW,u0) + activeProbes(RW,u1) + ddgiUBO(b0)
	{
		std::vector<rhi::ArgumentBinding> bindings;
		bindings.push_back({0, rhi::ArgumentType::sampledTexture, rhi::ShaderStage::compute, 1});
		bindings.push_back({1, rhi::ArgumentType::sampledTexture, rhi::ShaderStage::compute, 1});
		bindings.push_back({2, rhi::ArgumentType::storageTexture, rhi::ShaderStage::compute, 1});
		bindings.push_back({3, rhi::ArgumentType::storageBuffer, rhi::ShaderStage::compute, 1});
		bindings.push_back({4, rhi::ArgumentType::storageBuffer, rhi::ShaderStage::compute, 1});
		bindings.push_back({5, rhi::ArgumentType::sampledTexture, rhi::ShaderStage::compute, 1});
		m_distanceLayout = device.createArgumentLayout({bindings.data(), static_cast<uint32_t>(bindings.size()), "FlaxDDGI.Distance"});
	}

	// irradiance: probesTrace(RO,t0) + probesData(RO,t1) + probesIrradianceOut(RW,u0) + probesIrradianceHist(RO,t2) + activeProbes(RW,u1) + ddgiUBO(b0)
	{
		std::vector<rhi::ArgumentBinding> bindings;
		bindings.push_back({0, rhi::ArgumentType::sampledTexture, rhi::ShaderStage::compute, 1});
		bindings.push_back({1, rhi::ArgumentType::sampledTexture, rhi::ShaderStage::compute, 1});
		bindings.push_back({2, rhi::ArgumentType::storageTexture, rhi::ShaderStage::compute, 1});
		bindings.push_back({3, rhi::ArgumentType::sampledTexture, rhi::ShaderStage::compute, 1});
		bindings.push_back({4, rhi::ArgumentType::storageBuffer, rhi::ShaderStage::compute, 1});
		bindings.push_back({5, rhi::ArgumentType::storageBuffer, rhi::ShaderStage::compute, 1});
		m_irradianceLayout = device.createArgumentLayout({bindings.data(), static_cast<uint32_t>(bindings.size()), "FlaxDDGI.Irradiance"});
	}

  // --- Argument tables ---
  auto writeTextureRW = [&](rhi::ArgumentTableHandle table, uint32_t binding, rhi::TextureViewHandle view) {
    rhi::ArgumentWrite w{};
    w.binding = binding;
    w.type = rhi::ArgumentType::storageTexture;
    w.textureView = view;
    w.accessIntent = rhi::ArgumentAccessIntent::readWrite;
    device.updateArgumentTable(table, 1, &w);
  };
  auto writeTextureRO = [&](rhi::ArgumentTableHandle table, uint32_t binding, rhi::TextureViewHandle view) {
    rhi::ArgumentWrite w{};
    w.binding = binding;
    w.type = rhi::ArgumentType::sampledTexture;
    w.textureView = view;
    // Flax probe textures, Global SDF, and the atlas remain in General because
    // compute passes write them in place. Match the descriptor image layout.
    w.accessIntent = rhi::ArgumentAccessIntent::readWrite;
    device.updateArgumentTable(table, 1, &w);
  };
  auto writeCombinedTextureRO = [&](rhi::ArgumentTableHandle table, uint32_t binding,
                                    rhi::TextureViewHandle view, rhi::SamplerHandle sampler) {
    rhi::ArgumentWrite w{};
    w.binding = binding;
    w.type = rhi::ArgumentType::combinedImageSampler;
    w.textureView = view;
    w.sampler = sampler;
    w.accessIntent = rhi::ArgumentAccessIntent::readWrite;
    device.updateArgumentTable(table, 1, &w);
  };
  auto writeBufferRW = [&](rhi::ArgumentTableHandle table, uint32_t binding, rhi::BufferHandle buf) {
    rhi::ArgumentWrite w{};
    w.binding = binding;
    w.type = rhi::ArgumentType::storageBuffer;
    w.buffer = buf;
    w.accessIntent = rhi::ArgumentAccessIntent::readWrite;
    device.updateArgumentTable(table, 1, &w);
  };
	auto writeSampler = [&](rhi::ArgumentTableHandle table, uint32_t binding, rhi::SamplerHandle sampler) {
		rhi::ArgumentWrite w{};
		w.binding = binding;
		w.type = rhi::ArgumentType::sampler;
		w.sampler = sampler;
		w.accessIntent = rhi::ArgumentAccessIntent::sampledRead;
		device.updateArgumentTable(table, 1, &w);
	};
	auto writeUniformBuffer = [&](rhi::ArgumentTableHandle table, uint32_t binding, rhi::BufferHandle buf, uint32_t size) {
		rhi::ArgumentWrite w{};
		w.binding = binding;
		w.type = rhi::ArgumentType::storageBuffer;
		w.buffer = buf;
		w.size = size;
		device.updateArgumentTable(table, 1, &w);
	};

		// Classify table — sampler + UBO are always required
			m_classifyTable = device.createArgumentTable(m_classifyLayout);
			writeTextureRW(m_classifyTable, 0, m_probesDataView);
			writeBufferRW(m_classifyTable, 1, m_flaxResources->getActiveProbes(0));
			{
				if (sdfPass && !sdfPass->getVolume().sdfTexture.isNull())
				{
					const auto& vol = sdfPass->getVolume();
					rhi::TextureViewCreateDesc vd{};
					vd.image = vol.sdfTexture; vd.format = rhi::TextureFormat::r16Sfloat;
					vd.viewType = rhi::ImageViewType::e3D; vd.aspect = rhi::TextureAspect::color;
					vd.baseMipLevel = 0; vd.levelCount = 1;
					vd.debugName = "FlaxDDGI.SDFView";
					m_classifySDFView = device.createTextureView(vd);
					writeTextureRO(m_classifyTable, 2, m_classifySDFView);
				}
				// Prefer the Global SDF sampler, but always bind our owned fallback.
				rhi::SamplerHandle sampler = sdfPass->getMeshSDFSampler();
				if (sampler.isNull()) sampler = m_fallbackSampler;
				ASSERT(!sampler.isNull(), "Flax DDGI classify sampler must be valid");
				writeSampler(m_classifyTable, 3, sampler);
			}
			writeUniformBuffer(m_classifyTable, 4, m_ddgiUniformBuffers[0], ddgiUBOSize);

  // InitArgs table
  m_initArgsTable = device.createArgumentTable(m_initArgsLayout);
  writeBufferRW(m_initArgsTable, 0, m_flaxResources->getActiveProbes(0));
  writeBufferRW(m_initArgsTable, 1, m_flaxResources->getUpdateProbesInitArgs());

  // UpdateInactive table
	m_updateInactiveTable = device.createArgumentTable(m_updateInactiveLayout);
	writeTextureRW(m_updateInactiveTable, 0, m_probesDataView);
	writeBufferRW(m_updateInactiveTable, 1, m_flaxResources->getActiveProbes(0));
	writeUniformBuffer(m_updateInactiveTable, 2, m_ddgiUniformBuffers[0], ddgiUBOSize);

	  // TraceRays table — SDF + sampler + UBO are unconditional
	  m_traceRaysTable = device.createArgumentTable(m_traceRaysLayout);
	  writeTextureRO(m_traceRaysTable, 0, m_probesDataView);
	  writeBufferRW(m_traceRaysTable, 1, m_flaxResources->getActiveProbes(0));
	  writeTextureRW(m_traceRaysTable, 2, m_probesTraceView);
	  {
	    rhi::SamplerHandle sampler = sdfPass->getMeshSDFSampler();
	    if (sampler.isNull()) sampler = m_fallbackSampler;
	    ASSERT(!sampler.isNull(), "Flax DDGI trace sampler must be valid");
	    if (sdfPass && !sdfPass->getVolume().sdfTexture.isNull())
	    {
	      const auto& vol = sdfPass->getVolume();
	      rhi::TextureViewCreateDesc vd{};
	      vd.image = vol.sdfTexture; vd.format = rhi::TextureFormat::r16Sfloat;
	      vd.viewType = rhi::ImageViewType::e3D; vd.aspect = rhi::TextureAspect::color;
	      vd.baseMipLevel = 0; vd.levelCount = 1;
	      vd.debugName = "FlaxDDGI.TraceSDFView";
	      m_traceSDFView = device.createTextureView(vd);

	      if (!m_traceSDFView.isNull())
	        writeCombinedTextureRO(m_traceRaysTable, 3, m_traceSDFView, sampler);
	    }
	    writeSampler(m_traceRaysTable, 4, sampler);
	  }
		writeUniformBuffer(m_traceRaysTable, 10, m_ddgiUniformBuffers[0], ddgiUBOSize);
		{
			// Bind GlobalSDF albedo volume for R5 fallback hit shading
			auto* sdfPass2 = m_renderer->getGlobalSDFPass();
			if (sdfPass2 && !sdfPass2->getVolume().albedoTexture.isNull())
			{
				rhi::TextureViewCreateDesc vd{};
				vd.image = sdfPass2->getVolume().albedoTexture;
				vd.format = rhi::TextureFormat::rgba8Unorm;
				vd.viewType = rhi::ImageViewType::e3D;
				vd.aspect = rhi::TextureAspect::color;
				vd.baseMipLevel = 0; vd.levelCount = 1;
				vd.debugName = "FlaxDDGI.AlbedoView";
				m_traceAlbedoView = device.createTextureView(vd);
				writeTextureRO(m_traceRaysTable, 11, m_traceAlbedoView);
			}
		}
	  {
    // Surface Atlas bindings (optional — may be black textures if not initialized)
    auto* atlasPass = m_renderer->getSurfaceAtlasPass();
    if (atlasPass && atlasPass->isInitialized())
    {
      m_atlasDepthView = createTextureView(atlasPass->getAtlasDepth(), rhi::TextureFormat::d16Unorm,
                                           rhi::TextureAspect::depth, "Flax.AtlasDepth.View");
      m_atlasLightingView = createTextureView(atlasPass->getAtlasLighting(),
                                              rhi::TextureFormat::rgba16Sfloat,
                                              rhi::TextureAspect::color, "Flax.AtlasLighting.View");
      writeTextureRO(m_traceRaysTable, 5, m_atlasDepthView);
      writeTextureRO(m_traceRaysTable, 6, m_atlasLightingView);
      writeBufferRW(m_traceRaysTable, 7, atlasPass->getChunksBuffer());
      writeBufferRW(m_traceRaysTable, 8, atlasPass->getCulledObjectsBuffer());
	      writeBufferRW(m_traceRaysTable, 9, atlasPass->getObjectsBuffer());
	    }
	  }
	  writeUniformBuffer(m_traceRaysTable, 10, m_ddgiUniformBuffers[0], ddgiUBOSize);

		// Distance table: uses current write atlas + ActiveProbes + UBO
	m_distanceTable = device.createArgumentTable(m_distanceLayout);
	writeTextureRO(m_distanceTable, 0, m_probesTraceView);
	writeTextureRO(m_distanceTable, 1, m_probesDataView);
	writeTextureRW(m_distanceTable, 2, m_probesDistanceViewA);
	writeBufferRW(m_distanceTable, 3, m_flaxResources->getActiveProbes(0));
	writeUniformBuffer(m_distanceTable, 4, m_ddgiUniformBuffers[0], ddgiUBOSize);
	writeTextureRO(m_distanceTable, 5, m_probesDistanceViewB);

	// Irradiance table: uses current write atlas + history read + ActiveProbes + UBO
	m_irradianceTable = device.createArgumentTable(m_irradianceLayout);
	writeTextureRO(m_irradianceTable, 0, m_probesTraceView);
	writeTextureRO(m_irradianceTable, 1, m_probesDataView);
	writeTextureRW(m_irradianceTable, 2, m_probesIrradianceViewA);
	writeTextureRO(m_irradianceTable, 3, m_probesIrradianceViewB);
	writeBufferRW(m_irradianceTable, 4, m_flaxResources->getActiveProbes(0));
	writeUniformBuffer(m_irradianceTable, 5, m_ddgiUniformBuffers[0], ddgiUBOSize);

	// --- Pipelines ---
#ifdef USE_SLANG
  const auto createPipeline = [&](const uint32_t* spirv, size_t wordCount, const char* entryPoint,
                                   rhi::ArgumentLayoutHandle layout, uint32_t pushSize, uint32_t variant)
  {
    const std::array<rhi::ArgumentLayoutHandle, 1> layouts{{layout}};
    const std::array<rhi::PipelinePushConstantRange, 1> pushRanges{{
      {rhi::ShaderStage::compute, 0, pushSize}
    }};
    const rhi::ComputePipelineDesc desc{
      .shaderStage = {rhi::ShaderStage::compute, spirv, wordCount * sizeof(uint32_t), entryPoint},
      .argumentLayouts = layouts.data(),
      .argumentLayoutCount = static_cast<uint32_t>(layouts.size()),
      .pushConstantRanges = pushSize > 0 ? pushRanges.data() : nullptr,
      .pushConstantRangeCount = pushSize > 0 ? 1u : 0u,
      .specializationVariant = variant,
    };
    return device.createComputePipeline(desc);
  };

  // Only InitArgs uses push constants (small struct); others use uniform buffer.
  const uint32_t initArgsPushSize = sizeof(FlaxDDGIInitArgsPush);
  m_classifyPipeline = createPipeline(ddgi_flax_classify_slang, std::size(ddgi_flax_classify_slang), "CS_Classify", m_classifyLayout, 0, 0x1001u);
  m_initArgsPipeline = createPipeline(ddgi_flax_init_args_slang, std::size(ddgi_flax_init_args_slang), "CS_UpdateProbesInitArgs", m_initArgsLayout, initArgsPushSize, 0x1002u);
  m_updateInactivePipeline = createPipeline(ddgi_flax_update_inactive_slang, std::size(ddgi_flax_update_inactive_slang), "CS_UpdateInactive", m_updateInactiveLayout, 0, 0x1003u);
  m_traceRaysPipeline = createPipeline(ddgi_flax_trace_rays_slang, std::size(ddgi_flax_trace_rays_slang), "CS_TraceRays", m_traceRaysLayout, 0, 0x1004u);
  m_distancePipeline = createPipeline(ddgi_flax_update_distance_slang, std::size(ddgi_flax_update_distance_slang), "CS_UpdateDistance", m_distanceLayout, 0, 0x1005u);
  m_irradiancePipeline = createPipeline(ddgi_flax_update_irradiance_slang, std::size(ddgi_flax_update_irradiance_slang), "CS_UpdateIrradiance", m_irradianceLayout, 0, 0x1006u);
#endif

  m_pipelinesCreated = true;

  LOGI("FlaxDDGIPass initialized: %u cascades, %u max probes, %u max batches, 6 pipelines",
       m_flaxResources->getCascadeCount(),
       m_flaxResources->getMaxProbesPerCascade(),
       m_flaxResources->getMaxBatchesPerCascade());
}

void FlaxDDGIPass::shutdownResources()
{
  m_pipelinesCreated = false;
  m_textureLayoutsInitialized = false;
  if (!m_device) { m_flaxResources = nullptr; return; }

  auto destroyPipeline = [&](rhi::PipelineHandle& h) { if (!h.isNull()) m_device->destroyPipeline(h); h = {}; };
  auto destroyTable = [&](rhi::ArgumentTableHandle& h) { if (!h.isNull()) m_device->destroyArgumentTable(h); h = {}; };
  auto destroyLayout = [&](rhi::ArgumentLayoutHandle& h) { if (!h.isNull()) m_device->destroyArgumentLayout(h); h = {}; };
  auto destroyView = [&](rhi::TextureViewHandle& h) { if (!h.isNull()) m_device->destroyTextureView(h); h = {}; };

  destroyPipeline(m_classifyPipeline);
  destroyPipeline(m_initArgsPipeline);
  destroyPipeline(m_updateInactivePipeline);
  destroyPipeline(m_traceRaysPipeline);
  destroyPipeline(m_distancePipeline);
  destroyPipeline(m_irradiancePipeline);

  destroyTable(m_classifyTable);
  destroyTable(m_initArgsTable);
  destroyTable(m_updateInactiveTable);
  destroyTable(m_traceRaysTable);
  destroyTable(m_distanceTable);
  destroyTable(m_irradianceTable);

  destroyLayout(m_classifyLayout);
  destroyLayout(m_initArgsLayout);
  destroyLayout(m_updateInactiveLayout);
  destroyLayout(m_traceRaysLayout);
  destroyLayout(m_distanceLayout);
  destroyLayout(m_irradianceLayout);

  destroyView(m_probesDataView);
  destroyView(m_probesTraceView);
  destroyView(m_probesIrradianceViewA);
  destroyView(m_probesIrradianceViewB);
  destroyView(m_probesDistanceViewA);
  destroyView(m_probesDistanceViewB);
  destroyView(m_classifySDFView);
  destroyView(m_traceSDFView);
	destroyView(m_atlasDepthView);
	destroyView(m_atlasLightingView);
	destroyView(m_traceAlbedoView);

	for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
	{
		if (!m_ddgiUniformBuffers[i].isNull()) m_device->destroyBuffer(m_ddgiUniformBuffers[i]);
		m_ddgiUniformBuffers[i] = {};
	}
	if (!m_fallbackSampler.isNull()) { m_device->destroySampler(m_fallbackSampler); m_fallbackSampler = {}; }

	m_flaxResources = nullptr;
  m_device = nullptr;
}

PassNode::HandleSlice<PassResourceDependency> FlaxDDGIPass::getDependencies() const
{
  return {nullptr, 0};
}

// Build FlaxDDGIData from renderer state
static shaderio::FlaxDDGIData buildFlaxDDGIData(const GPUDrivenRenderer& renderer, const FlaxDDGIResources& resources)
{
  const auto& config = renderer.getDDGIConfig();
  const auto& cascades = renderer.getFlaxDDGICascadeDescs();
  const glm::vec3 camPos = renderer.getFlaxLastCameraPosition();

  shaderio::FlaxDDGIData ddgi{};
  ddgi.cascadesCount = std::min(resources.getCascadeCount(), kFlaxImplementedCascadeCount);
  ddgi.probesCounts = resources.getProbesPerCascade().empty()
    ? shaderio::uvec3{0,0,0} : shaderio::uvec3{resources.getProbesPerCascade()[0].x, resources.getProbesPerCascade()[0].y, resources.getProbesPerCascade()[0].z};
  ddgi.irradianceGamma = config.ddgiGamma;
  ddgi.probeHistoryWeight = config.probeHistoryWeight;
  ddgi.rayMaxDistance = config.maxDistance;
  ddgi.indirectLightingIntensity = config.indirectLightingIntensity;
  ddgi.viewPos = shaderio::vec3{camPos.x, camPos.y, camPos.z};
  ddgi.raysCount = config.raysPerProbe;
  ddgi.fallbackIrradiance = shaderio::vec4{config.fallbackIrradiance.x, config.fallbackIrradiance.y, config.fallbackIrradiance.z, config.fallbackIrradiance.w};

  // Populate SDF bounds from GlobalSDFPass volume
  {
    auto* sdfPass = renderer.getGlobalSDFPass();
    if (sdfPass)
    {
      const auto& vol = sdfPass->getVolume();
      ddgi.sdfBoundsMinAndVoxel = shaderio::vec4{vol.worldBoundsMin.x, vol.worldBoundsMin.y, vol.worldBoundsMin.z, vol.voxelSize};
      ddgi.sdfBoundsMaxAndRes = shaderio::vec4{vol.worldBoundsMax.x, vol.worldBoundsMax.y, vol.worldBoundsMax.z, static_cast<float>(vol.resolution)};
    }
    else
    {
      // Fallback: use config-driven bounds (giDistance around camera)
      const float halfExt = config.giDistance;
      ddgi.sdfBoundsMinAndVoxel = shaderio::vec4{-halfExt, -halfExt, -halfExt, halfExt * 2.0f / 128.0f};
		ddgi.sdfBoundsMaxAndRes = shaderio::vec4{halfExt, halfExt, halfExt, 128.0f};
	}
  }

  // Populate scene light direction/color from renderer cache
  {
	const glm::vec3& lightDir = renderer.getCachedLightDirection();
	const glm::vec3& lightColor = renderer.getCachedLightColor();
	ddgi.sceneLightDirection = shaderio::vec4{lightDir.x, lightDir.y, lightDir.z, 0.0f};
	ddgi.sceneLightColor = shaderio::vec4{lightColor.x, lightColor.y, lightColor.z, 0.0f};
  }

  const auto& radianceSources = renderer.getFlaxRadianceSources();
  const uint32_t radianceSourceCount = static_cast<uint32_t>(std::min<size_t>(
    radianceSources.size(), static_cast<size_t>(shaderio::LFlaxDDGIMaxRadianceSources)));
  ddgi.radianceSourceParams = shaderio::uvec4{radianceSourceCount, 0u, 0u, 0u};
  for (uint32_t sourceIndex = 0; sourceIndex < radianceSourceCount; ++sourceIndex)
  {
    ddgi.radianceSources[sourceIndex] = radianceSources[sourceIndex];
  }

  for (uint32_t c = 0; c < std::min(ddgi.cascadesCount, 4u); ++c)
  {
    if (c < cascades.size())
    {
      const auto& cd = cascades[c];
      ddgi.probesOriginAndSpacing[c] = shaderio::vec4{cd.snappedOrigin.x, cd.snappedOrigin.y, cd.snappedOrigin.z, cd.probeSpacing};
      ddgi.blendOrigin[c] = shaderio::vec4{cd.blendOrigin.x, cd.blendOrigin.y, cd.blendOrigin.z, 0.0f};
      ddgi.probesScrollOffsets[c] = shaderio::ivec4{cd.scrollOffset.x, cd.scrollOffset.y, cd.scrollOffset.z, 0};
    }
  }

  return ddgi;
}

void FlaxDDGIPass::writeFlaxDDGIDataToBuffer(uint32_t frameIndex) const
{
	if (frameIndex >= kMaxFramesInFlight || !m_device) return;
	shaderio::FlaxDDGIData ddgi = buildFlaxDDGIData(*m_renderer, *m_flaxResources);
	ddgi.updateParams = shaderio::uvec4{
	  m_probeUpdateOffset,
	  m_renderer->getDDGIConfig().maxUpdatedProbesPerFrame,
	  0u,
	  0u,
	};
	void* dst = m_device->mapBuffer(m_ddgiUniformBuffers[frameIndex]);
	std::memcpy(dst, &ddgi, sizeof(ddgi));
}

void FlaxDDGIPass::bindFlaxDDGIDataBuffer(uint32_t frameIndex) const
{
  if (frameIndex >= kMaxFramesInFlight || m_device == nullptr) return;

  const rhi::BufferHandle buffer = m_ddgiUniformBuffers[frameIndex];
  const uint32_t size = sizeof(shaderio::FlaxDDGIData);
  const auto write = [&](rhi::ArgumentTableHandle table, uint32_t binding)
  {
    const rhi::ArgumentWrite argument{
      .binding = binding,
      .type = rhi::ArgumentType::storageBuffer,
      .buffer = buffer,
      .size = size,
    };
    m_device->updateArgumentTable(table, 1, &argument);
  };

  write(m_classifyTable, 4);
  write(m_updateInactiveTable, 2);
  write(m_traceRaysTable, 10);
  write(m_distanceTable, 4);
  write(m_irradianceTable, 5);
}

void FlaxDDGIPass::bindFlaxHistoryParity(uint32_t parity) const
{
  if (m_device == nullptr) return;

  const bool odd = (parity & 1u) != 0u;
  const rhi::TextureViewHandle irradianceWrite = odd ? m_probesIrradianceViewB : m_probesIrradianceViewA;
  const rhi::TextureViewHandle irradianceRead = odd ? m_probesIrradianceViewA : m_probesIrradianceViewB;
  const rhi::TextureViewHandle distanceWrite = odd ? m_probesDistanceViewB : m_probesDistanceViewA;
  const rhi::TextureViewHandle distanceRead = odd ? m_probesDistanceViewA : m_probesDistanceViewB;

  const auto writeTexture = [&](rhi::ArgumentTableHandle table, uint32_t binding,
                                rhi::ArgumentType type, rhi::TextureViewHandle view)
  {
    const rhi::ArgumentWrite argument{
      .binding = binding,
      .type = type,
      .textureView = view,
      .accessIntent = rhi::ArgumentAccessIntent::readWrite,
    };
    m_device->updateArgumentTable(table, 1, &argument);
  };

  writeTexture(m_distanceTable, 2, rhi::ArgumentType::storageTexture, distanceWrite);
  writeTexture(m_distanceTable, 5, rhi::ArgumentType::sampledTexture, distanceRead);
  writeTexture(m_irradianceTable, 2, rhi::ArgumentType::storageTexture, irradianceWrite);
  writeTexture(m_irradianceTable, 3, rhi::ArgumentType::sampledTexture, irradianceRead);
}

void FlaxDDGIPass::execute(const PassContext& context) const
{
  if (!m_renderer || !m_device || !m_flaxResources || !m_flaxResources->isInitialized() || !m_pipelinesCreated)
    return;

  if (!m_renderer->isFlaxStyleDDGIRequested())
    return;

  // Honor freeze/single-step controls (R10)
  const auto& ddgiConfig = m_renderer->getDDGIConfig();
  if (ddgiConfig.flaxGIFreeze && !ddgiConfig.flaxGISingleStep)
    return;

  if (context.commandBuffer == nullptr)
    return;

  rhi::CommandBuffer& cmd = *context.commandBuffer;
  cmd.beginEvent("FlaxDDGIPass");

  if (!m_textureLayoutsInitialized)
  {
    const auto makeInitBarrier = [](rhi::TextureHandle texture)
    {
      return rhi::TextureBarrier{
        .texture = texture,
        .before = rhi::ResourceState::Undefined,
        .after = rhi::ResourceState::General,
        .range = {
          .aspect = rhi::TextureAspect::color,
          .baseMipLevel = 0,
          .levelCount = 1,
          .baseArrayLayer = 0,
          .layerCount = 1,
        },
      };
    };
    const std::array<rhi::TextureBarrier, 6> initBarriers{
      makeInitBarrier(m_flaxResources->getProbesTrace()),
      makeInitBarrier(m_flaxResources->getProbesData()),
      makeInitBarrier(m_flaxResources->getProbesIrradianceWrite(0)),
      makeInitBarrier(m_flaxResources->getProbesIrradianceRead(0)),
      makeInitBarrier(m_flaxResources->getProbesDistanceWrite(0)),
      makeInitBarrier(m_flaxResources->getProbesDistanceRead(0)),
    };
    cmd.resourceBarrier(initBarriers.data(), static_cast<uint32_t>(initBarriers.size()), nullptr, 0);

    const rhi::TextureSubresourceRange colorRange{
      .aspect = rhi::TextureAspect::color,
      .baseMipLevel = 0,
      .levelCount = 1,
      .baseArrayLayer = 0,
      .layerCount = 1,
    };
    const rhi::ClearColorValue zero{0.0f, 0.0f, 0.0f, 0.0f};
    cmd.clearColorTexture(m_flaxResources->getProbesTrace(), colorRange, zero);
    cmd.clearColorTexture(m_flaxResources->getProbesIrradianceWrite(0), colorRange, zero);
    cmd.clearColorTexture(m_flaxResources->getProbesIrradianceRead(0), colorRange, zero);
    cmd.clearColorTexture(m_flaxResources->getProbesDistanceWrite(0), colorRange, zero);
    cmd.clearColorTexture(m_flaxResources->getProbesDistanceRead(0), colorRange, zero);
    cmd.clearColorTexture(m_flaxResources->getProbesData(), colorRange,
                          {0.0f, 0.0f, 0.0f, -1.0f});
    cmd.barrier(rhi::StageFlags::transfer, rhi::StageFlags::compute,
                rhi::HazardFlags::textureWrites);
    m_textureLayoutsInitialized = true;
  }

  // Handle probe reset — clear history atlases (R10)
  if (context.params && context.params->debugOptions.flaxGIReset)
  {
	  m_probeUpdateOffset = 0u;
    cmd.clearColorTexture(m_flaxResources->getProbesIrradianceWrite(0),
                          {rhi::TextureAspect::color, 0, 1, 0, 1},
                          {0.0f, 0.0f, 0.0f, 0.0f});
    cmd.clearColorTexture(m_flaxResources->getProbesDistanceWrite(0),
                          {rhi::TextureAspect::color, 0, 1, 0, 1},
                          {0.0f, 0.0f, 0.0f, 0.0f});
    cmd.clearColorTexture(m_flaxResources->getProbesIrradianceRead(0),
                          {rhi::TextureAspect::color, 0, 1, 0, 1},
                          {0.0f, 0.0f, 0.0f, 0.0f});
    cmd.clearColorTexture(m_flaxResources->getProbesDistanceRead(0),
                          {rhi::TextureAspect::color, 0, 1, 0, 1},
                          {0.0f, 0.0f, 0.0f, 0.0f});
    // Also reset probe data to inactive
    cmd.clearColorTexture(m_flaxResources->getProbesData(),
                          {rhi::TextureAspect::color, 0, 1, 0, 1},
                          {0.0f, 0.0f, 0.0f, -1.0f});
    cmd.barrier(rhi::StageFlags::transfer, rhi::StageFlags::compute,
                rhi::HazardFlags::textureWrites);
  }

  const uint32_t cascadeCount = m_flaxResources->getCascadeCount();
  const uint32_t totalProbes = m_flaxResources->getMaxProbesPerCascade();

  if (totalProbes == 0 || cascadeCount == 0)
  {
    cmd.endEvent();
    return;
  }

  // Classification appends into ActiveProbes. Reset its counter every frame;
  // otherwise the indirect trace count grows without bound and reads beyond
  // the probe-index storage on the second frame.
  const rhi::BufferHandle activeProbes = m_flaxResources->getActiveProbes(0);
  rhi::ComputeEncoder* clear = cmd.beginComputePass();
  clear->fillBuffer(activeProbes, 0, sizeof(uint32_t), 0u);
  cmd.endEncoding();
  cmd.barrier(rhi::StageFlags::transfer, rhi::StageFlags::compute,
              rhi::HazardFlags::bufferWrites);

	// Keep the per-frame SSBO write and descriptor selection on the same frame
	// index. Binding buffer 0 permanently while rotating writes across 0/1/2
	// leaves two out of every three frames reading stale FlaxDDGIData.
	const uint32_t frameIndex = context.frameIndex % kMaxFramesInFlight;
	const uint32_t historyParity =
	  static_cast<uint32_t>(m_renderer->getTemporalFrameCounter() & 1u);
	if (!m_ddgiUniformBuffers[frameIndex].isNull())
	{
		writeFlaxDDGIDataToBuffer(frameIndex);
		bindFlaxDDGIDataBuffer(frameIndex);
		bindFlaxHistoryParity(historyParity);
	}
	else
	{
		LOGW("FlaxDDGIPass::execute: UBO buffer is null at index %u, skipping GI work", frameIndex);
		cmd.endEvent();
		return;
	}

  // --- DDGI.Classify (FGI-050) ---
  {
    cmd.beginEvent("DDGI.Classify");
    rhi::ComputeEncoder* enc = cmd.beginComputePass();
    enc->setPipeline(m_classifyPipeline);
    enc->setArgumentTable(0, m_classifyTable);

    // One thread per probe across all cascades (each probe maps to one texel in ProbesData)
    const uint32_t groupsX = (totalProbes + 31u) / 32u;
    enc->dispatch({groupsX, 1, 1});

    cmd.endEncoding();
    cmd.barrier(rhi::StageFlags::compute, rhi::StageFlags::compute,
                rhi::HazardFlags::textureWrites | rhi::HazardFlags::bufferWrites);
    cmd.endEvent();
  }

  // --- DDGI.InitArgs (FGI-051) ---
  {
    cmd.beginEvent("DDGI.InitArgs");
    rhi::ComputeEncoder* enc = cmd.beginComputePass();
    enc->setPipeline(m_initArgsPipeline);
    enc->setArgumentTable(0, m_initArgsTable);
    const FlaxDDGIInitArgsPush initArgs{
      .maxActiveProbes = totalProbes,
      .maxUpdatedProbesPerFrame = ddgiConfig.maxUpdatedProbesPerFrame,
    };
    enc->setRootConstants(kPrimaryRootConstantsSlot, &initArgs, sizeof(initArgs));

    // Single group of 1 thread (reads counter, writes dispatch args)
    enc->dispatch({1, 1, 1});

    cmd.endEncoding();
    cmd.barrier(rhi::StageFlags::compute, rhi::StageFlags::compute | rhi::StageFlags::commandInput,
                rhi::HazardFlags::bufferWrites);
    cmd.endEvent();
  }

  // --- DDGI.UpdateInactive (FGI-052) ---
  {
    cmd.beginEvent("DDGI.UpdateInactive");
    rhi::ComputeEncoder* enc = cmd.beginComputePass();
    enc->setPipeline(m_updateInactivePipeline);
    enc->setArgumentTable(0, m_updateInactiveTable);

    // One thread per probe data texel
    const auto& dataExt = m_flaxResources->getDataExtent();
    const uint32_t gx = (dataExt.width + 7u) / 8u;
    const uint32_t gy = (dataExt.height + 7u) / 8u;
    enc->dispatch({gx, gy, 1});

    cmd.endEncoding();
    cmd.barrier(rhi::StageFlags::compute, rhi::StageFlags::compute,
                rhi::HazardFlags::textureWrites);
    cmd.endEvent();
  }

  // --- DDGI.TraceRays (FGI-053) ---
  {
    cmd.beginEvent("DDGI.TraceRays");
    rhi::ComputeEncoder* enc = cmd.beginComputePass();
    enc->setPipeline(m_traceRaysPipeline);
    enc->setArgumentTable(0, m_traceRaysTable);

    // Dispatch: one group per active probe batch (256 probes per batch)
    // Indirect dispatch reads InitArgs
    rhi::DispatchIndirectDesc indirectDesc{
      m_flaxResources->getUpdateProbesInitArgs(), kTraceIndirectOffset};
    enc->dispatchIndirect(indirectDesc);

    cmd.endEncoding();
    cmd.barrier(rhi::StageFlags::compute, rhi::StageFlags::compute,
                rhi::HazardFlags::textureWrites);
    cmd.endEvent();
  }

  // --- DDGI.UpdateDistance (FGI-054) ---
  {
    cmd.beginEvent("DDGI.UpdateDistance");
    rhi::ComputeEncoder* enc = cmd.beginComputePass();
		enc->setPipeline(m_distancePipeline);
		enc->setArgumentTable(0, m_distanceTable);

		rhi::DispatchIndirectDesc indirectDist{
		  m_flaxResources->getUpdateProbesInitArgs(), kDistanceIndirectOffset};
		enc->dispatchIndirect(indirectDist);

    cmd.endEncoding();
    cmd.barrier(rhi::StageFlags::compute, rhi::StageFlags::compute,
                rhi::HazardFlags::textureWrites);
    cmd.endEvent();
  }

  // --- DDGI.UpdateIrradiance (FGI-055) ---
  {
    cmd.beginEvent("DDGI.UpdateIrradiance");
    rhi::ComputeEncoder* enc = cmd.beginComputePass();
		enc->setPipeline(m_irradiancePipeline);
		enc->setArgumentTable(0, m_irradianceTable);

		rhi::DispatchIndirectDesc indirectIrr{
		  m_flaxResources->getUpdateProbesInitArgs(), kIrradianceIndirectOffset};
		enc->dispatchIndirect(indirectIrr);

    cmd.endEncoding();
    cmd.barrier(rhi::StageFlags::compute, rhi::StageFlags::compute,
                rhi::HazardFlags::textureWrites);
    cmd.endEvent();
  }

  // Final barrier: Flax atlases → future compute/fragment reads (lighting pass R8)
  cmd.barrier(rhi::StageFlags::compute,
              rhi::StageFlags::compute | rhi::StageFlags::fragmentShader,
              rhi::HazardFlags::textureWrites | rhi::HazardFlags::bufferWrites);

  cmd.endEvent(); // FlaxDDGIPass

	const uint32_t updateBudget = ddgiConfig.maxUpdatedProbesPerFrame == 0u
	  ? totalProbes
	  : std::min(ddgiConfig.maxUpdatedProbesPerFrame, totalProbes);
	if (totalProbes > 0u)
	{
	  m_probeUpdateOffset = (m_probeUpdateOffset + updateBudget) % totalProbes;
	}

  // Disarm single-step after one frame
  if (ddgiConfig.flaxGISingleStep)
  {
    m_renderer->disarmFlaxSingleStep();
  }
}

} // namespace demo
