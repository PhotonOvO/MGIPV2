#include "BatchUploadContext.h"

#include "../common/Common.h"

#include <algorithm>
#include <array>
#include <cstring>
#if !defined(__ANDROID__) && !defined(ANDROID)
#include <execution>
#endif
#include <limits>
#include <numeric>
#include <utility>

namespace demo
{
	namespace
	{
		constexpr std::array<uint8_t, 12> kKtx2Identifier{{
			0xABu, 0x4Bu, 0x54u, 0x58u, 0x20u, 0x32u,
			0x30u, 0xBBu, 0x0Du, 0x0Au, 0x1Au, 0x0Au,
		}};

		struct Ktx2RangeHeader
		{
			uint8_t identifier[12]{};
			uint32_t format{0u};
			uint32_t typeSize{0u};
			uint32_t width{0u};
			uint32_t height{0u};
			uint32_t depth{0u};
			uint32_t layerCount{0u};
			uint32_t faceCount{0u};
			uint32_t levelCount{0u};
			uint32_t supercompression{0u};
			uint32_t dfdOffset{0u};
			uint32_t dfdLength{0u};
			uint32_t kvdOffset{0u};
			uint32_t kvdLength{0u};
			uint64_t sgdOffset{0u};
			uint64_t sgdLength{0u};
		};

		struct Ktx2RangeLevel
		{
			uint64_t offset{0u};
			uint64_t length{0u};
			uint64_t uncompressedLength{0u};
		};

		static_assert(sizeof(Ktx2RangeHeader) == 80u);
		static_assert(sizeof(Ktx2RangeLevel) == 24u);

		struct FormatBlockInfo
		{
			uint32_t width{1};
			uint32_t height{1};
			uint32_t bytes{0};
			rhi::TextureAspect aspect{rhi::TextureAspect::color};
		};

		[[nodiscard]] bool tryAdd(uint64_t left, uint64_t right, uint64_t& out) noexcept
		{
			if (right > std::numeric_limits<uint64_t>::max() - left)
			{
				return false;
			}
			out = left + right;
			return true;
		}

		[[nodiscard]] bool tryMultiply(uint64_t left, uint64_t right, uint64_t& out) noexcept
		{
			if (left != 0u && right > std::numeric_limits<uint64_t>::max() / left)
			{
				return false;
			}
			out = left * right;
			return true;
		}

		[[nodiscard]] bool tryAlignUp(uint64_t value, uint64_t alignment, uint64_t& out) noexcept
		{
			const uint64_t safeAlignment = alignment == 0 ? 1 : alignment;
			if ((safeAlignment & (safeAlignment - 1u)) != 0u)
			{
				return false;
			}
			const uint64_t mask = safeAlignment - 1;
			uint64_t rounded = 0;
			if (!tryAdd(value, mask, rounded))
			{
				return false;
			}
			out = rounded & ~mask;
			return true;
		}

		[[nodiscard]] uint32_t mipExtent(uint32_t extent, uint32_t mipLevel) noexcept
		{
			return mipLevel >= 32u ? 1u : std::max(extent >> mipLevel, 1u);
		}

		[[nodiscard]] FormatBlockInfo formatBlockInfo(rhi::TextureFormat format) noexcept
		{
			switch (format)
			{
			case rhi::TextureFormat::rgba8Unorm:
			case rhi::TextureFormat::rgba8Srgb:
			case rhi::TextureFormat::bgra8Unorm:
			case rhi::TextureFormat::rgba8Snorm:
			case rhi::TextureFormat::rg16Sfloat:
			case rhi::TextureFormat::r32Sfloat:
			case rhi::TextureFormat::r11g11b10Ufloat:
				return {.bytes = 4u};
			case rhi::TextureFormat::rgba16Sfloat:
				return {.bytes = 8u};
			case rhi::TextureFormat::r16Sfloat:
				return {.bytes = 2u};
			case rhi::TextureFormat::d16Unorm:
				return {.bytes = 2u, .aspect = rhi::TextureAspect::depth};
			case rhi::TextureFormat::d32Sfloat:
				return {.bytes = 4u, .aspect = rhi::TextureAspect::depth};
			case rhi::TextureFormat::d24UnormS8:
				return {.bytes = 4u, .aspect = rhi::TextureAspect::depthStencil};
			case rhi::TextureFormat::d32SfloatS8:
				return {.bytes = 8u, .aspect = rhi::TextureAspect::depthStencil};
			case rhi::TextureFormat::bc6hUfloatBlock:
			case rhi::TextureFormat::bc6hSfloatBlock:
			case rhi::TextureFormat::bc7UnormBlock:
			case rhi::TextureFormat::bc7SrgbBlock:
				return {.width = 4u, .height = 4u, .bytes = 16u};
			case rhi::TextureFormat::undefined:
			default:
				return {};
			}
		}
	} // namespace

	const char* ktx2ContainerValidationErrorName(Ktx2ContainerValidationError error) noexcept
	{
		switch (error)
		{
		case Ktx2ContainerValidationError::none:                     return "none";
		case Ktx2ContainerValidationError::truncatedHeader:          return "truncated-header";
		case Ktx2ContainerValidationError::invalidIdentifier:        return "invalid-identifier";
		case Ktx2ContainerValidationError::levelTableOverflow:       return "level-table-overflow";
		case Ktx2ContainerValidationError::levelTableOutOfRange:     return "level-table-out-of-range";
		case Ktx2ContainerValidationError::levelRangeOutOfRange:     return "level-range-out-of-range";
		case Ktx2ContainerValidationError::emptyLevel:               return "empty-level";
		case Ktx2ContainerValidationError::overlappingLevels:        return "overlapping-levels";
		case Ktx2ContainerValidationError::aggregatePayloadOverflow: return "aggregate-payload-overflow";
		default:                                                    return "unknown";
		}
	}

	Ktx2ContainerValidationResult validateKtx2ContainerRanges(
		std::span<const uint8_t> container)
	{
		Ktx2ContainerValidationResult result{};
		const auto fail = [&](Ktx2ContainerValidationError error)
		{
			result.error = error;
			return result;
		};
		if (container.size() < sizeof(Ktx2RangeHeader))
		{
			return fail(Ktx2ContainerValidationError::truncatedHeader);
		}
		Ktx2RangeHeader header{};
		std::memcpy(&header, container.data(), sizeof(header));
		if (!std::equal(kKtx2Identifier.begin(), kKtx2Identifier.end(), header.identifier))
		{
			return fail(Ktx2ContainerValidationError::invalidIdentifier);
		}
		result.levelCount = std::max(header.levelCount, 1u);
		uint64_t tableBytes = 0u;
		uint64_t tableEnd = 0u;
		if (!tryMultiply(result.levelCount, sizeof(Ktx2RangeLevel), tableBytes) ||
			!tryAdd(sizeof(Ktx2RangeHeader), tableBytes, tableEnd))
		{
			return fail(Ktx2ContainerValidationError::levelTableOverflow);
		}
		if (tableEnd > container.size())
		{
			return fail(Ktx2ContainerValidationError::levelTableOutOfRange);
		}

		std::vector<std::pair<uint64_t, uint64_t>> ranges;
		ranges.reserve(result.levelCount);
		for (uint32_t mip = 0u; mip < result.levelCount; ++mip)
		{
			Ktx2RangeLevel level{};
			std::memcpy(&level,
			            container.data() + sizeof(Ktx2RangeHeader) +
				            static_cast<size_t>(mip) * sizeof(Ktx2RangeLevel),
			            sizeof(level));
			if (level.length == 0u)
			{
				return fail(Ktx2ContainerValidationError::emptyLevel);
			}
			uint64_t levelEnd = 0u;
			if (level.offset < tableEnd || !tryAdd(level.offset, level.length, levelEnd) ||
				levelEnd > container.size())
			{
				return fail(Ktx2ContainerValidationError::levelRangeOutOfRange);
			}
			if (!tryAdd(result.totalPayloadBytes, level.length, result.totalPayloadBytes))
			{
				return fail(Ktx2ContainerValidationError::aggregatePayloadOverflow);
			}
			ranges.emplace_back(level.offset, levelEnd);
		}
		std::sort(ranges.begin(), ranges.end());
		for (size_t index = 1u; index < ranges.size(); ++index)
		{
			if (ranges[index].first < ranges[index - 1u].second)
			{
				return fail(Ktx2ContainerValidationError::overlappingLevels);
			}
		}
		return result;
	}

	const char* textureUploadValidationErrorName(TextureUploadValidationError error) noexcept
	{
		switch (error)
		{
		case TextureUploadValidationError::none:                       return "none";
		case TextureUploadValidationError::unsupportedFormat:          return "unsupported-format";
		case TextureUploadValidationError::aspectMismatch:             return "aspect-mismatch";
		case TextureUploadValidationError::invalidTextureDescriptor:   return "invalid-texture-descriptor";
		case TextureUploadValidationError::invalidCopyExtent:          return "invalid-copy-extent";
		case TextureUploadValidationError::mipOutOfRange:               return "mip-out-of-range";
		case TextureUploadValidationError::subresourceOutOfRange:       return "subresource-out-of-range";
		case TextureUploadValidationError::compressedRegionMisaligned: return "compressed-region-misaligned";
		case TextureUploadValidationError::arithmeticOverflow:         return "arithmetic-overflow";
		case TextureUploadValidationError::sliceRangeExceeded:         return "slice-range-exceeded";
		case TextureUploadValidationError::aggregateRangeExceeded:     return "aggregate-range-exceeded";
		default:                                                       return "unknown";
		}
	}

	TextureUploadValidationResult validateTextureUploadRegion(
		const rhi::TextureDesc& texture,
		const rhi::BufferTextureCopyDesc& region,
		uint64_t sliceOffset,
		uint64_t sliceSize,
		uint64_t stagingCapacity) noexcept
	{
		TextureUploadValidationResult result{};
		const auto fail = [&](TextureUploadValidationError error)
		{
			result.error = error;
			return result;
		};

		const FormatBlockInfo block = formatBlockInfo(texture.format);
		if (block.bytes == 0u)
		{
			return fail(TextureUploadValidationError::unsupportedFormat);
		}
		if (region.aspect != block.aspect)
		{
			return fail(TextureUploadValidationError::aspectMismatch);
		}
		if (texture.extent.width == 0u || texture.extent.height == 0u || texture.extent.depth == 0u ||
			texture.mipLevels == 0u || texture.arrayLayers == 0u)
		{
			return fail(TextureUploadValidationError::invalidTextureDescriptor);
		}
		uint32_t largestExtent = std::max(texture.extent.width, texture.extent.height);
		if (texture.dimension == rhi::TextureDimension::e3D)
		{
			largestExtent = std::max(largestExtent, texture.extent.depth);
		}
		uint32_t maximumMipLevels = 0u;
		for (uint32_t extent = largestExtent; extent != 0u; extent >>= 1u)
		{
			++maximumMipLevels;
		}
		const bool dimensionValid =
			(texture.dimension == rhi::TextureDimension::e2D && texture.extent.depth == 1u && texture.arrayLayers == 1u) ||
			(texture.dimension == rhi::TextureDimension::e2DArray && texture.extent.depth == 1u) ||
			(texture.dimension == rhi::TextureDimension::eCube && texture.extent.depth == 1u &&
				(texture.arrayLayers % 6u) == 0u) ||
			(texture.dimension == rhi::TextureDimension::e3D && texture.arrayLayers == 1u);
		if (!dimensionValid || texture.mipLevels > maximumMipLevels)
		{
			return fail(TextureUploadValidationError::invalidTextureDescriptor);
		}
		if (region.width == 0u || region.height == 0u || region.depth == 0u || region.layerCount == 0u)
		{
			return fail(TextureUploadValidationError::invalidCopyExtent);
		}
		if (region.mipLevel >= texture.mipLevels)
		{
			return fail(TextureUploadValidationError::mipOutOfRange);
		}
		if (region.textureOffset.x < 0 || region.textureOffset.y < 0 || region.textureOffset.z < 0)
		{
			return fail(TextureUploadValidationError::subresourceOutOfRange);
		}

		const uint64_t mipWidth = mipExtent(texture.extent.width, region.mipLevel);
		const uint64_t mipHeight = mipExtent(texture.extent.height, region.mipLevel);
		const uint64_t mipDepth = mipExtent(texture.extent.depth, region.mipLevel);
		const uint64_t offsetX = static_cast<uint32_t>(region.textureOffset.x);
		const uint64_t offsetY = static_cast<uint32_t>(region.textureOffset.y);
		const uint64_t offsetZ = static_cast<uint32_t>(region.textureOffset.z);
		uint64_t copyEndX = 0;
		uint64_t copyEndY = 0;
		uint64_t copyEndZ = 0;
		if (!tryAdd(offsetX, region.width, copyEndX) ||
			!tryAdd(offsetY, region.height, copyEndY) ||
			!tryAdd(offsetZ, region.depth, copyEndZ))
		{
			return fail(TextureUploadValidationError::arithmeticOverflow);
		}
		if (copyEndX > mipWidth || copyEndY > mipHeight)
		{
			return fail(TextureUploadValidationError::subresourceOutOfRange);
		}

		if (texture.dimension == rhi::TextureDimension::e3D)
		{
			if (texture.arrayLayers != 1u || region.baseArrayLayer != 0u || region.layerCount != 1u || copyEndZ > mipDepth)
			{
				return fail(TextureUploadValidationError::subresourceOutOfRange);
			}
		}
		else
		{
			uint64_t layerEnd = 0;
			if (texture.extent.depth != 1u || offsetZ != 0u || region.depth != 1u ||
				!tryAdd(region.baseArrayLayer, region.layerCount, layerEnd) || layerEnd > texture.arrayLayers)
			{
				return fail(TextureUploadValidationError::subresourceOutOfRange);
			}
		}

		if (block.width > 1u || block.height > 1u)
		{
			if ((offsetX % block.width) != 0u || (offsetY % block.height) != 0u ||
				((region.width % block.width) != 0u && copyEndX != mipWidth) ||
				((region.height % block.height) != 0u && copyEndY != mipHeight))
			{
				return fail(TextureUploadValidationError::compressedRegionMisaligned);
			}
		}

		const uint64_t blocksWide = (static_cast<uint64_t>(region.width) + block.width - 1u) / block.width;
		const uint64_t blocksHigh = (static_cast<uint64_t>(region.height) + block.height - 1u) / block.height;
		uint64_t requiredBytes = 0;
		if (!tryMultiply(blocksWide, blocksHigh, requiredBytes) ||
			!tryMultiply(requiredBytes, region.depth, requiredBytes) ||
			!tryMultiply(requiredBytes, region.layerCount, requiredBytes) ||
			!tryMultiply(requiredBytes, block.bytes, requiredBytes))
		{
			return fail(TextureUploadValidationError::arithmeticOverflow);
		}
		result.requiredBytes = requiredBytes;

		if (sliceOffset > stagingCapacity || sliceSize > stagingCapacity - sliceOffset)
		{
			return fail(TextureUploadValidationError::aggregateRangeExceeded);
		}
		if (region.bufferOffset > sliceSize || requiredBytes > sliceSize - region.bufferOffset)
		{
			return fail(TextureUploadValidationError::sliceRangeExceeded);
		}
		if (!tryAdd(region.bufferOffset, requiredBytes, result.relativeEnd) ||
			!tryAdd(sliceOffset, result.relativeEnd, result.aggregateEnd))
		{
			return fail(TextureUploadValidationError::arithmeticOverflow);
		}
		if (result.aggregateEnd > stagingCapacity)
		{
			return fail(TextureUploadValidationError::aggregateRangeExceeded);
		}
		return result;
	}

	void BatchUploadContext::init(rhi::Device& device, uint64_t totalSize)
	{
		destroy();

		m_device = &device;
		m_capacity = totalSize;
		m_head = 0;

		if (totalSize == 0)
		{
			return;
		}

		m_stagingBuffer = upload::createMappedUploadStagingBuffer(device, totalSize);
		m_mappedData = device.mapBuffer(m_stagingBuffer);
	}

	BatchUploadContext::Slice BatchUploadContext::allocate(uint64_t size, uint64_t alignment)
	{
		uint64_t offset = 0;
		uint64_t end = 0;
		ASSERT(tryAlignUp(m_head, alignment, offset), "BatchUploadContext staging alignment overflowed");
		ASSERT(tryAdd(offset, size, end) && end <= m_capacity,
		       "BatchUploadContext staging allocation exceeded capacity");

		Slice slice{};
		slice.cpuPtr = static_cast<std::byte*>(m_mappedData) + offset;
		slice.offset = offset;
		slice.size = size;

		m_head = end;
		return slice;
	}

	std::vector<BatchUploadContext::Slice> BatchUploadContext::allocateSlices(const std::vector<uint64_t>& sizes,
	                                                                          uint64_t alignment)
	{
		return reserveSlices(sizes, alignment);
	}

	BatchUploadContext::Slice BatchUploadContext::mapReservedSlice(uint64_t offset, uint64_t size) const
	{
		uint64_t end = 0;
		ASSERT(tryAdd(offset, size, end) && end <= m_capacity,
		       "BatchUploadContext reserved slice exceeded capacity");

		Slice slice{};
		slice.cpuPtr = static_cast<std::byte*>(m_mappedData) + offset;
		slice.offset = offset;
		slice.size = size;
		return slice;
	}

	std::vector<BatchUploadContext::Slice> BatchUploadContext::reserveSlices(const std::vector<uint64_t>& sizes,
	                                                                         uint64_t alignment)
	{
		std::vector<Slice> slices;
		slices.reserve(sizes.size());

		uint64_t cursor = m_head;
		for (const uint64_t size : sizes)
		{
			uint64_t offset = 0;
			uint64_t end = 0;
			ASSERT(tryAlignUp(cursor, alignment, offset), "BatchUploadContext staging reservation alignment overflowed");
			ASSERT(tryAdd(offset, size, end) && end <= m_capacity,
			       "BatchUploadContext staging reservation exceeded capacity");
			slices.push_back(mapReservedSlice(offset, size));
			cursor = end;
		}

		m_head = cursor;
		return slices;
	}

	void BatchUploadContext::copyToSlices(std::span<const Slice> slices,
	                                      std::span<const std::span<const std::byte>> sources)
	{
		ASSERT(slices.size() == sources.size(), "BatchUploadContext slice/source count mismatch");

		std::vector<size_t> indices(slices.size());
		std::iota(indices.begin(), indices.end(), size_t{0});
		auto copySlice = [&](size_t index)
		{
			const Slice& slice = slices[index];
			const std::span<const std::byte> source = sources[index];
			ASSERT(source.size_bytes() <= slice.size, "BatchUploadContext source does not fit reserved slice");
			if (source.empty())
			{
				return;
			}
			std::memcpy(slice.cpuPtr, source.data(), source.size_bytes());
		};
#if defined(__ANDROID__) || defined(ANDROID)
		std::for_each(indices.begin(), indices.end(), copySlice);
#else
		std::for_each(std::execution::par, indices.begin(), indices.end(), copySlice);
#endif
	}

	TextureUploadValidationResult BatchUploadContext::recordTextureUpload(
		const Slice& slice,
		rhi::TextureHandle dstImage,
		const rhi::TextureDesc& texture,
		const rhi::BufferTextureCopyDesc& region)
	{
		const TextureUploadValidationResult validation = validateTextureUploadRegion(
			texture, region, slice.offset, slice.size, m_capacity);
		if (!validation.valid())
		{
			return validation;
		}
		UploadOperation op{};
		op.type = UploadType::image;
		op.dstImage = dstImage;
		op.imageRegion = region;
		op.imageRegion.texture = dstImage;
		op.imageRegion.bufferOffset += slice.offset;
		op.imageRegion.buffer = m_stagingBuffer;
		m_pendingUploads.push_back(op);
		return validation;
	}

	void BatchUploadContext::recordTextureUpload(
		const Slice& slice,
		rhi::TextureHandle dstImage,
		const rhi::BufferTextureCopyDesc& tightlyPackedRgba8Region)
	{
		// Compatibility for the built-in color-grading LUT, the sole legacy
		// caller: accept only an exact, tightly packed, base-level RGBA8 copy.
		const rhi::TextureDesc texture{
			.dimension = rhi::TextureDimension::e2D,
			.format = rhi::TextureFormat::rgba8Unorm,
			.extent = {tightlyPackedRgba8Region.width, tightlyPackedRgba8Region.height, 1u},
			.mipLevels = 1u,
			.arrayLayers = 1u,
		};
		const TextureUploadValidationResult validation = validateTextureUploadRegion(
			texture, tightlyPackedRgba8Region, slice.offset, slice.size, m_capacity);
		ASSERT(validation.valid() && validation.requiredBytes == slice.size,
		       "Legacy texture upload must be an exact tightly packed RGBA8 footprint");
		if (!validation.valid() || validation.requiredBytes != slice.size)
		{
			return;
		}
		const TextureUploadValidationResult recorded = recordTextureUpload(
			slice, dstImage, texture, tightlyPackedRgba8Region);
		ASSERT(recorded.valid(), "Validated legacy RGBA8 upload changed before recording");
	}

	void BatchUploadContext::recordBufferUpload(const Slice& slice, rhi::BufferHandle dstBuffer, uint64_t dstOffset,
	                                            uint64_t size)
	{
		ASSERT(size <= slice.size, "Buffer upload region exceeds reserved slice size");
		UploadOperation op{};
		op.type = UploadType::buffer;
		op.dstBuffer = dstBuffer;
		op.dstOffset = dstOffset;
		op.size = size;
		op.imageRegion.bufferOffset = slice.offset;
		m_pendingUploads.push_back(op);
	}

	void BatchUploadContext::executeUploads(rhi::CommandBuffer& cmd) const
	{
		if (m_stagingBuffer.isNull() || m_pendingUploads.empty())
		{
			return;
		}

		rhi::ComputeEncoder* copy = cmd.beginComputePass();
		for (const UploadOperation& op : m_pendingUploads)
		{
			if (op.type == UploadType::buffer)
			{
				copy->copyBuffer(m_stagingBuffer, op.imageRegion.bufferOffset, op.dstBuffer, op.dstOffset, op.size);
			}
			else
			{
				copy->copyBufferToTexture(op.imageRegion);
			}
		}
		cmd.endEncoding();
		// Upload boundary: transfer copies from the staging buffer must be visible to
		// later graphics/compute consumers; there is no pass graph edge for batch uploads.
		cmd.barrier(rhi::StageFlags::transfer,
		            rhi::StageFlags::all,
		            rhi::HazardFlags::bufferWrites | rhi::HazardFlags::textureWrites);
	}

	rhi::BufferHandle BatchUploadContext::releaseStagingBuffer()
	{
		rhi::BufferHandle buffer = m_stagingBuffer;
		m_stagingBuffer = {};
		m_mappedData = nullptr;
		m_capacity = 0;
		m_head = 0;
		m_pendingUploads.clear();
		return buffer;
	}

	void BatchUploadContext::destroy()
	{
		if (m_device != nullptr && !m_stagingBuffer.isNull())
		{
			m_device->destroyBuffer(m_stagingBuffer);
		}

		m_device = nullptr;
		m_stagingBuffer = {};
		m_mappedData = nullptr;
		m_capacity = 0;
		m_head = 0;
		m_pendingUploads.clear();
	}
} // namespace demo
