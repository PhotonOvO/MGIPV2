#include "BatchUploadContext.h"

#include "../common/Common.h"

#include <algorithm>
#include <cstring>
#include <execution>
#include <numeric>

namespace demo
{
	namespace
	{
		[[nodiscard]] uint64_t alignUp(uint64_t value, uint64_t alignment)
		{
			const uint64_t safeAlignment = alignment == 0 ? 1 : alignment;
			const uint64_t mask = safeAlignment - 1;
			return (value + mask) & ~mask;
		}
	} // namespace

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
		const uint64_t offset = alignUp(m_head, alignment);
		ASSERT(offset + size <= m_capacity, "BatchUploadContext staging allocation exceeded capacity");

		Slice slice{};
		slice.cpuPtr = static_cast<std::byte*>(m_mappedData) + offset;
		slice.offset = offset;
		slice.size = size;

		m_head = offset + size;
		return slice;
	}

	std::vector<BatchUploadContext::Slice> BatchUploadContext::allocateSlices(const std::vector<uint64_t>& sizes,
	                                                                          uint64_t alignment)
	{
		return reserveSlices(sizes, alignment);
	}

	BatchUploadContext::Slice BatchUploadContext::mapReservedSlice(uint64_t offset, uint64_t size) const
	{
		ASSERT(offset + size <= m_capacity, "BatchUploadContext reserved slice exceeded capacity");

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
			const uint64_t offset = alignUp(cursor, alignment);
			ASSERT(offset + size <= m_capacity, "BatchUploadContext staging reservation exceeded capacity");
			slices.push_back(mapReservedSlice(offset, size));
			cursor = offset + size;
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
		std::for_each(std::execution::par, indices.begin(), indices.end(), [&](size_t index)
		{
			const Slice& slice = slices[index];
			const std::span<const std::byte> source = sources[index];
			ASSERT(source.size_bytes() <= slice.size, "BatchUploadContext source does not fit reserved slice");
			if (source.empty())
			{
				return;
			}
			std::memcpy(slice.cpuPtr, source.data(), source.size_bytes());
		});
	}

	void BatchUploadContext::recordTextureUpload(const Slice& slice,
	                                             rhi::TextureHandle dstImage,
	                                             const rhi::BufferTextureCopyDesc& region)
	{
		ASSERT(region.bufferOffset <= slice.size, "Texture upload region offset exceeds reserved slice size");
		UploadOperation op{};
		op.type = UploadType::image;
		op.dstImage = dstImage;
		op.imageRegion = region;
		op.imageRegion.texture = dstImage;
		op.imageRegion.bufferOffset += slice.offset;
		op.imageRegion.buffer = m_stagingBuffer;
		m_pendingUploads.push_back(op);
	}

	void BatchUploadContext::recordBufferUpload(const Slice& slice, rhi::BufferHandle dstBuffer, uint64_t dstOffset,
	                                            uint64_t size)
	{
		UploadOperation op{};
		op.type = UploadType::buffer;
		op.dstBuffer = dstBuffer;
		op.dstOffset = dstOffset;
		op.size = size;
		op.imageRegion.bufferOffset = slice.offset;
		ASSERT(size <= slice.size, "Buffer upload region exceeds reserved slice size");
		m_pendingUploads.push_back(op);
	}

	void BatchUploadContext::executeUploads(rhi::CommandBuffer& cmd) const
	{
		if (m_stagingBuffer.isNull())
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
