#include "SDFLoader.h"

#include "../render/UploadUtils.h"

#include <cstddef>
#include <cstring>
#include <array>
#include <fstream>
#include <span>

namespace demo
{
	namespace
	{
		constexpr char kSDFMagic[4] = {'M', 'S', 'D', 'F'};
		constexpr uint32_t kSDFVersionV1 = 1u;
		constexpr uint32_t kSDFVersionV2 = 2u;
		constexpr size_t kHeaderSize = 4 + sizeof(uint32_t) + sizeof(uint32_t) * 3 + sizeof(float) * 6;
		constexpr uint32_t kWhiteRGBA8 = 0xffffffffu;
	} // namespace

	bool SDFLoader::parseFile(const std::filesystem::path& path, FileData& outData, std::string& outError)
	{
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file.is_open())
		{
			outError = "Could not open SDF asset: " + path.string();
			return false;
		}

		const std::streamsize fileSize = file.tellg();
		file.seekg(0, std::ios::beg);
		if (fileSize < static_cast<std::streamsize>(kHeaderSize))
		{
			outError = "SDF asset truncated (header): " + path.string();
			return false;
		}

		char magic[4] = {};
		uint32_t version = 0;
		file.read(magic, sizeof(magic));
		file.read(reinterpret_cast<char*>(&version), sizeof(version));
		if (std::memcmp(magic, kSDFMagic, sizeof(magic)) != 0)
		{
			outError = "SDF asset has bad magic: " + path.string();
			return false;
		}
		if (version != kSDFVersionV1 && version != kSDFVersionV2)
		{
			outError = "SDF asset version mismatch (" + std::to_string(version) + " not in ["
				+ std::to_string(kSDFVersionV1) + ", " + std::to_string(kSDFVersionV2) + "]): "
				+ path.string();
			return false;
		}

		uint32_t resolution[3] = {};
		float boundsMin[3] = {};
		float boundsMax[3] = {};
		file.read(reinterpret_cast<char*>(resolution), sizeof(resolution));
		file.read(reinterpret_cast<char*>(boundsMin), sizeof(boundsMin));
		file.read(reinterpret_cast<char*>(boundsMax), sizeof(boundsMax));
		if (!file.good())
		{
			outError = "SDF asset header read failed: " + path.string();
			return false;
		}

		if (resolution[0] == 0 || resolution[1] == 0 || resolution[2] == 0)
		{
			outError = "SDF asset has zero resolution: " + path.string();
			return false;
		}

		const size_t voxelCount = static_cast<size_t>(resolution[0]) * resolution[1] * resolution[2];
		const size_t payloadSize = voxelCount * sizeof(uint16_t);
		if (static_cast<size_t>(fileSize) < kHeaderSize + payloadSize)
		{
			outError = "SDF asset truncated (payload): " + path.string();
			return false;
		}

		outData.resolution = {resolution[0], resolution[1], resolution[2]};
		outData.boundsMin = {boundsMin[0], boundsMin[1], boundsMin[2]};
		outData.boundsMax = {boundsMax[0], boundsMax[1], boundsMax[2]};
		outData.version = version;
		outData.halfTexels.resize(voxelCount);
		file.read(reinterpret_cast<char*>(outData.halfTexels.data()),
		          static_cast<std::streamsize>(payloadSize));
		if (!file.good())
		{
			outError = "SDF asset payload read failed: " + path.string();
			return false;
		}
		outData.albedoTexels.assign(voxelCount, kWhiteRGBA8);
		if (version >= kSDFVersionV2)
		{
			const size_t albedoPayloadSize = voxelCount * sizeof(uint32_t);
			if (static_cast<size_t>(fileSize) < kHeaderSize + payloadSize + albedoPayloadSize)
			{
				outError = "SDF asset truncated (albedo payload): " + path.string();
				return false;
			}
			file.read(reinterpret_cast<char*>(outData.albedoTexels.data()),
			          static_cast<std::streamsize>(albedoPayloadSize));
			if (!file.good())
			{
				outError = "SDF asset albedo payload read failed: " + path.string();
				return false;
			}
			outData.hasAlbedoPayload = true;
		}
		return true;
	}

	SDFLoadResult SDFLoader::load(const std::filesystem::path& path, rhi::Device& device,
	                              rhi::CommandBuffer& cmd,
	                              std::vector<rhi::BufferHandle>& outStagingBuffers)
	{
		SDFLoadResult result{};

		FileData fileData;
		if (!parseFile(path, fileData, result.errorMessage))
		{
			return result;
		}

		const glm::uvec3 resolution = fileData.resolution;
		const rhi::TextureHandle sdfTexture = device.createTexture(rhi::TextureDesc{
			.dimension = rhi::TextureDimension::e3D,
			.format = rhi::TextureFormat::r16Sfloat,
			.usage = rhi::TextureUsageFlags::sampled | rhi::TextureUsageFlags::transferDst,
			.extent = {resolution.x, resolution.y, resolution.z},
			.mipLevels = 1,
			.arrayLayers = 1,
			.debugName = "MeshSDF_Volume",
		});
		if (!sdfTexture.isValid())
		{
			result.errorMessage = "Failed to create R16F Texture3D for SDF asset: " + path.string();
			return result;
		}
		const rhi::TextureHandle albedoTexture = device.createTexture(rhi::TextureDesc{
			.dimension = rhi::TextureDimension::e3D,
			.format = rhi::TextureFormat::rgba8Unorm,
			.usage = rhi::TextureUsageFlags::sampled | rhi::TextureUsageFlags::transferDst,
			.extent = {resolution.x, resolution.y, resolution.z},
			.mipLevels = 1,
			.arrayLayers = 1,
			.debugName = "MeshSDF_Albedo",
		});
		if (!albedoTexture.isValid())
		{
			device.destroyTexture(sdfTexture);
			result.errorMessage = "Failed to create RGBA8 Texture3D for SDF albedo asset: " + path.string();
			return result;
		}

		const rhi::TextureSubresourceRange volumeRange{
			.aspect = rhi::TextureAspect::color,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		};

		// Undefined -> TransferDst before the staging copy.
		const std::array<rhi::TextureBarrier, 2> uploadBeginBarriers{
			{
				rhi::TextureBarrier{
					.texture = sdfTexture,
					.before = rhi::ResourceState::Undefined,
					.after = rhi::ResourceState::TransferDst,
					.range = volumeRange,
				},
				rhi::TextureBarrier{
					.texture = albedoTexture,
					.before = rhi::ResourceState::Undefined,
					.after = rhi::ResourceState::TransferDst,
					.range = volumeRange,
				},
			}
		};
		cmd.resourceBarrier(uploadBeginBarriers, {});

		const std::span<const std::byte> payload(
			reinterpret_cast<const std::byte*>(fileData.halfTexels.data()),
			fileData.halfTexels.size() * sizeof(uint16_t));
		const rhi::BufferHandle stagingBuffer = upload::createUploadStagingBuffer(device, payload);
		const std::span<const std::byte> albedoPayload(
			reinterpret_cast<const std::byte*>(fileData.albedoTexels.data()),
			fileData.albedoTexels.size() * sizeof(uint32_t));
		const rhi::BufferHandle albedoStagingBuffer = upload::createUploadStagingBuffer(device, albedoPayload);

		rhi::ComputeEncoder* copy = cmd.beginComputePass();
		copy->copyBufferToTexture(rhi::BufferTextureCopyDesc{
			.buffer = stagingBuffer,
			.bufferOffset = 0,
			.texture = sdfTexture,
			.aspect = rhi::TextureAspect::color,
			.mipLevel = 0,
			.baseArrayLayer = 0,
			.layerCount = 1,
			.width = resolution.x,
			.height = resolution.y,
			.depth = resolution.z,
		});
		copy->copyBufferToTexture(rhi::BufferTextureCopyDesc{
			.buffer = albedoStagingBuffer,
			.bufferOffset = 0,
			.texture = albedoTexture,
			.aspect = rhi::TextureAspect::color,
			.mipLevel = 0,
			.baseArrayLayer = 0,
			.layerCount = 1,
			.width = resolution.x,
			.height = resolution.y,
			.depth = resolution.z,
		});
		cmd.endEncoding();

		// Transfer writes must be visible to later compute/graphics consumers
		// (same contract as BatchUploadContext::executeUploads).
		cmd.barrier(rhi::StageFlags::transfer, rhi::StageFlags::all, rhi::HazardFlags::textureWrites);

		// TransferDst -> General for sampling by later DDGI passes.
		const std::array<rhi::TextureBarrier, 2> uploadEndBarriers{
			{
				rhi::TextureBarrier{
					.texture = sdfTexture,
					.before = rhi::ResourceState::TransferDst,
					.after = rhi::ResourceState::General,
					.range = volumeRange,
				},
				rhi::TextureBarrier{
					.texture = albedoTexture,
					.before = rhi::ResourceState::TransferDst,
					.after = rhi::ResourceState::General,
					.range = volumeRange,
				},
			}
		};
		cmd.resourceBarrier(uploadEndBarriers, {});

		// Caller retires the staging buffer after the command buffer is submitted.
		outStagingBuffers.push_back(stagingBuffer);
		outStagingBuffers.push_back(albedoStagingBuffer);

		result.asset.worldBoundsMin = fileData.boundsMin;
		result.asset.worldBoundsMax = fileData.boundsMax;
		result.asset.resolution = resolution;
		result.asset.normalizedSDFHandle = sdfTexture;
		result.asset.albedoHandle = albedoTexture;
		result.asset.fileVersion = fileData.version;
		result.asset.hasAlbedoPayload = fileData.hasAlbedoPayload;
		result.asset.isValid = true;
		return result;
	}
} // namespace demo
