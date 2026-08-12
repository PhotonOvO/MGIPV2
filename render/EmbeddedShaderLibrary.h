#pragma once

#include "../rhi/RHIDevice.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

#if defined(DEMO_HAS_EMBEDDED_METAL_SHADERS)
#include "_autogen/MetalShaderCatalog.h"
#endif

namespace demo
{

inline rhi::ShaderLibraryHandle loadEmbeddedSpirvLibrary(
	rhi::Device& device,
	const uint32_t* words,
	size_t byteSize,
	const char* debugName)
{
#if defined(DEMO_HAS_EMBEDDED_METAL_SHADERS)
	if(device.getBackendInfo().type == rhi::BackendType::metal)
	{
		const EmbeddedMetalLibrary metalLibrary = findEmbeddedMetalLibrary(words);
		if(metalLibrary.data == nullptr || metalLibrary.size == 0)
			throw std::runtime_error("No embedded Metal library matches the shader module");
		return device.createShaderLibrary(rhi::ShaderLibraryDesc{
			.format = rhi::ShaderIRFormat::metalLibrary,
			.data = std::span<const std::byte>{metalLibrary.data, metalLibrary.size},
			.debugName = debugName,
		});
	}
#endif
	return device.createShaderLibrary(rhi::ShaderLibraryDesc{
		.format = rhi::ShaderIRFormat::spirv,
		.data = std::as_bytes(std::span{words, byteSize / sizeof(uint32_t)}),
		.debugName = debugName,
	});
}

template <size_t WordCount>
inline rhi::ShaderLibraryHandle loadEmbeddedSpirvLibrary(
	rhi::Device& device,
	const uint32_t (&words)[WordCount],
	const char* debugName)
{
	return loadEmbeddedSpirvLibrary(
		device, words, WordCount * sizeof(uint32_t), debugName);
}

} // namespace demo
