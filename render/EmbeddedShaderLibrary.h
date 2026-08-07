#pragma once

#include "../rhi/RHIDevice.h"

#include <cstddef>
#include <cstdint>

namespace demo
{

inline rhi::ShaderLibraryHandle loadEmbeddedSpirvLibrary(
	rhi::Device& device,
	const uint32_t* words,
	size_t byteSize,
	const char* debugName)
{
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