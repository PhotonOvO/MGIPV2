#pragma once

#include "../common/Handles.h"
#include "../rhi/RHICommandBuffer.h"
#include "../rhi/RHIDevice.h"
#include "../rhi/RHIEncoder.h"
#include "UploadUtils.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace demo
{
	enum class TextureUploadValidationError : uint8_t
	{
		none = 0,
		unsupportedFormat,
		aspectMismatch,
		invalidTextureDescriptor,
		invalidCopyExtent,
		mipOutOfRange,
		subresourceOutOfRange,
		compressedRegionMisaligned,
		arithmeticOverflow,
		sliceRangeExceeded,
		aggregateRangeExceeded,
	};

	struct TextureUploadValidationResult
	{
		TextureUploadValidationError error{TextureUploadValidationError::none};
		uint64_t requiredBytes{0};
		uint64_t relativeEnd{0};
		uint64_t aggregateEnd{0};

		[[nodiscard]] constexpr bool valid() const noexcept
		{
			return error == TextureUploadValidationError::none;
		}
	};

	enum class Ktx2ContainerValidationError : uint8_t
	{
		none = 0,
		truncatedHeader,
		invalidIdentifier,
		levelTableOverflow,
		levelTableOutOfRange,
		levelRangeOutOfRange,
		emptyLevel,
		overlappingLevels,
		aggregatePayloadOverflow,
	};

	struct Ktx2ContainerValidationResult
	{
		Ktx2ContainerValidationError error{Ktx2ContainerValidationError::none};
		uint32_t levelCount{0u};
		uint64_t totalPayloadBytes{0u};

		[[nodiscard]] constexpr bool valid() const noexcept
		{
			return error == Ktx2ContainerValidationError::none;
		}
	};

	[[nodiscard]] const char* textureUploadValidationErrorName(TextureUploadValidationError error) noexcept;
	[[nodiscard]] const char* ktx2ContainerValidationErrorName(Ktx2ContainerValidationError error) noexcept;

	[[nodiscard]] Ktx2ContainerValidationResult validateKtx2ContainerRanges(
		std::span<const uint8_t> container);

	[[nodiscard]] TextureUploadValidationResult validateTextureUploadRegion(
		const rhi::TextureDesc& texture,
		const rhi::BufferTextureCopyDesc& region,
		uint64_t sliceOffset,
		uint64_t sliceSize,
		uint64_t stagingCapacity) noexcept;

	class BatchUploadContext
	{
	public:
		struct Slice
		{
			void* cpuPtr{nullptr};
			uint64_t offset{0};
			uint64_t size{0};
		};

		void init(rhi::Device& device, uint64_t totalSize);
		[[nodiscard]] bool isInitialized() const { return !m_stagingBuffer.isNull(); }
		[[nodiscard]] uint64_t usedBytes() const { return m_head; }
		[[nodiscard]] uint64_t capacityBytes() const { return m_capacity; }
		[[nodiscard]] Slice allocate(uint64_t size, uint64_t alignment);
		[[nodiscard]] std::vector<Slice> allocateSlices(const std::vector<uint64_t>& sizes, uint64_t alignment);
		[[nodiscard]] Slice mapReservedSlice(uint64_t offset, uint64_t size) const;
		[[nodiscard]] std::vector<Slice> reserveSlices(const std::vector<uint64_t>& sizes, uint64_t alignment);
		void copyToSlices(std::span<const Slice> slices, std::span<const std::span<const std::byte>> sources);

		[[nodiscard]] TextureUploadValidationResult recordTextureUpload(
			const Slice& slice,
			rhi::TextureHandle dstImage,
			const rhi::TextureDesc& texture,
			const rhi::BufferTextureCopyDesc& region);
		void recordTextureUpload(
			const Slice& slice,
			rhi::TextureHandle dstImage,
			const rhi::BufferTextureCopyDesc& tightlyPackedRgba8Region);
		void recordBufferUpload(const Slice& slice, rhi::BufferHandle dstBuffer, uint64_t dstOffset, uint64_t size);
		void executeUploads(rhi::CommandBuffer& cmd) const;

		[[nodiscard]] rhi::BufferHandle releaseStagingBuffer();
		void destroy();

	private:
		enum class UploadType
		{
			buffer,
			image,
		};

		struct UploadOperation
		{
			UploadType type{UploadType::buffer};
			rhi::BufferHandle dstBuffer{};
			rhi::TextureHandle dstImage{};
			uint64_t dstOffset{0};
			uint64_t size{0};
			rhi::BufferTextureCopyDesc imageRegion{};
		};

		rhi::Device* m_device{nullptr};
		rhi::BufferHandle m_stagingBuffer{};
		void* m_mappedData{nullptr};
		uint64_t m_capacity{0};
		uint64_t m_head{0};
		std::vector<UploadOperation> m_pendingUploads;
	};
} // namespace demo
