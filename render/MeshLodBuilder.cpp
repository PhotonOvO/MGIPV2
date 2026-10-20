#include "MeshLodBuilder.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cstring>

namespace demo
{
	namespace
	{
		// Below this many triangles a simplification saves less than the level switch that would
		// select it, so the chain is not built at all.
		constexpr size_t kMinimumTriangleCount = 128u;

		// Each level targets half the index count of the level above it.
		constexpr size_t kIndexRatioPerLevel = 2u;

		// Border vertices stay in place. Two meshes that share an edge must not pull apart when
		// only one of them switches level.
		constexpr unsigned int kSimplifyOptions = meshopt_SimplifyLockBorder;

		// Collect the attributes that take part in the metric into one packed array, which is
		// the layout meshopt_simplifyWithAttributes expects.
		size_t gatherAttributes(const float* vertices, uint32_t vertexCount, size_t vertexStride,
		                        const MeshLodAttributes& attributes, std::vector<float>& out)
		{
			const size_t floatStride = vertexStride / sizeof(float);
			const size_t attributeCount = (attributes.normals ? 3u : 0u) + (attributes.uv ? 2u : 0u);
			if (attributeCount == 0u)
			{
				out.clear();
				return 0u;
			}

			out.resize(static_cast<size_t>(vertexCount) * attributeCount);
			for (uint32_t vertex = 0u; vertex < vertexCount; ++vertex)
			{
				const float* source = vertices + static_cast<size_t>(vertex) * floatStride;
				float* destination = out.data() + static_cast<size_t>(vertex) * attributeCount;
				if (attributes.normals)
				{
					std::memcpy(destination, source + MeshLodVertexLayout::kNormalFloatOffset,
					            3u * sizeof(float));
					destination += 3u;
				}
				if (attributes.uv)
				{
					std::memcpy(destination, source + MeshLodVertexLayout::kUvFloatOffset,
					            2u * sizeof(float));
				}
			}
			return attributeCount;
		}
	}

	MeshLodChain MeshLodBuilder::build(const float* vertices, uint32_t vertexCount,
	                                   size_t vertexStride, const MeshLodAttributes& attributes,
	                                   const uint32_t* indices, size_t indexCount,
	                                   uint32_t maxLevels)
	{
		MeshLodChain chain{};
		if (maxLevels == 0u || vertices == nullptr || indices == nullptr)
			return chain;
		if (vertexCount == 0u || vertexStride < sizeof(float) || (vertexStride % sizeof(float)) != 0u)
			return chain;
		// meshopt_simplify consumes a triangle list, so a partial triangle is not usable.
		if (indexCount < kMinimumTriangleCount * 3u || (indexCount % 3u) != 0u)
			return chain;

		// Conversion factor from the relative error meshoptimizer reports to mesh units.
		const float scale = meshopt_simplifyScale(vertices, vertexCount, vertexStride);
		if (!(scale > 0.0f))
			return chain;

		std::vector<float> packedAttributes;
		const size_t attributeCount =
			gatherAttributes(vertices, vertexCount, vertexStride, attributes, packedAttributes);

		// Weights, one per cached attribute, relative to the position metric.
		std::vector<float> attributeWeights;
		if (attributeCount != 0u)
		{
			if (attributes.normals)
				attributeWeights.insert(attributeWeights.end(), 3u, kNormalAttributeWeight);
			if (attributes.uv)
				attributeWeights.insert(attributeWeights.end(), 2u, kUvAttributeWeight);
		}

		// Simplify from the previous level rather than from the source, so the chain stays
		// monotonic: no level can contain detail that its parent already dropped.
		std::vector<uint32_t> source(indices, indices + indexCount);
		std::vector<uint32_t> destination(indexCount, 0u);

		size_t targetIndexCount = indexCount;
		for (uint32_t level = 0; level < maxLevels; ++level)
		{
			targetIndexCount = std::max(
				std::max(kMinimumTriangleCount, targetIndexCount / kIndexRatioPerLevel) / 3u * 3u,
				size_t{3u});
			if (targetIndexCount >= source.size())
				break;

			float relativeError = 0.0f;
			size_t simplifiedCount = 0u;
			if (attributeCount != 0u)
			{
				simplifiedCount = meshopt_simplifyWithAttributes(
					destination.data(), source.data(), source.size(), vertices, vertexCount,
					vertexStride, packedAttributes.data(), attributeCount * sizeof(float),
					attributeWeights.data(), attributeCount, nullptr, targetIndexCount, FLT_MAX,
					kSimplifyOptions, &relativeError);
			}
			else
			{
				simplifiedCount = meshopt_simplify(
					destination.data(), source.data(), source.size(), vertices, vertexCount,
					vertexStride, targetIndexCount, FLT_MAX, kSimplifyOptions, &relativeError);
			}
			// A zero result means the mesh could not be simplified at all, and a result that is
			// not smaller than its parent carries no information for a level switch.
			if (simplifiedCount == 0u || simplifiedCount >= source.size())
				break;

			// Reorder the level for post-transform vertex cache reuse. This only permutes indices,
			// so every level keeps referencing the caller's vertex buffer.
			meshopt_optimizeVertexCache(destination.data(), destination.data(), simplifiedCount,
			                            vertexCount);

			MeshLodChain::Level entry{};
			entry.indexOffset = static_cast<uint32_t>(chain.indices.size());
			entry.indexCount = static_cast<uint32_t>(simplifiedCount);
			entry.geometricError = relativeError * scale;
			chain.levels.push_back(entry);
			chain.indices.insert(chain.indices.end(), destination.begin(),
			                     destination.begin() + static_cast<std::ptrdiff_t>(simplifiedCount));

			source.assign(destination.begin(),
			              destination.begin() + static_cast<std::ptrdiff_t>(simplifiedCount));
		}

		// One derived level alone costs a lookup and saves no draw work, so require at least two
		// before the chain is worth carrying.
		if (chain.levels.size() < 2u)
		{
			chain.levels.clear();
			chain.indices.clear();
		}
		return chain;
	}
} // namespace demo
