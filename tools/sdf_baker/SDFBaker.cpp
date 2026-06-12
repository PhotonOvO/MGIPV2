#include "SDFBaker.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <sstream>
#include <thread>

namespace sdf_baker
{
	// ---------------------------------------------------------------------------
	// float -> half (IEEE 754 binary16), round to nearest even
	// ---------------------------------------------------------------------------
	uint16_t floatToHalf(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		const uint32_t sign = (bits >> 16u) & 0x8000u;
		const uint32_t exponent = (bits >> 23u) & 0xFFu;
		const uint32_t mantissa = bits & 0x7FFFFFu;

		if (exponent == 0xFFu)
		{
			// Inf / NaN
			return static_cast<uint16_t>(sign | 0x7C00u | (mantissa != 0u ? 0x200u : 0u));
		}

		const int32_t newExp = static_cast<int32_t>(exponent) - 127 + 15;
		if (newExp >= 31)
		{
			return static_cast<uint16_t>(sign | 0x7C00u); // overflow -> inf
		}
		if (newExp <= 0)
		{
			// Subnormal half or zero.
			if (newExp < -10)
			{
				return static_cast<uint16_t>(sign);
			}
			const uint32_t fullMantissa = mantissa | 0x800000u;
			const uint32_t shift = static_cast<uint32_t>(14 - newExp);
			uint16_t half = static_cast<uint16_t>(sign | (fullMantissa >> shift));
			const uint32_t remainder = fullMantissa & ((1u << shift) - 1u);
			const uint32_t halfway = 1u << (shift - 1u);
			if (remainder > halfway || (remainder == halfway && (half & 1u) != 0u))
			{
				++half; // carry into exponent is correct for RNE
			}
			return half;
		}

		uint16_t half = static_cast<uint16_t>(sign | (static_cast<uint32_t>(newExp) << 10u) | (mantissa >> 13u));
		const uint32_t remainder = mantissa & 0x1FFFu;
		if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u) != 0u))
		{
			++half;
		}
		return half;
	}

	// ---------------------------------------------------------------------------
	// Minimal OBJ parser
	// ---------------------------------------------------------------------------
	namespace
	{
		// Parses the vertex-position index out of an OBJ face corner token
		// ("3", "3/1", "3//2", "3/1/2", or negative relative indices).
		bool parseFaceCorner(const std::string& token, size_t vertexCount, uint32_t& outIndex)
		{
			if (token.empty())
			{
				return false;
			}
			const size_t slash = token.find('/');
			const std::string positionPart = (slash == std::string::npos) ? token : token.substr(0, slash);
			long parsed = 0;
			try
			{
				parsed = std::stol(positionPart);
			}
			catch (...)
			{
				return false;
			}
			if (parsed > 0)
			{
				if (static_cast<size_t>(parsed) > vertexCount)
				{
					return false;
				}
				outIndex = static_cast<uint32_t>(parsed - 1);
				return true;
			}
			if (parsed < 0)
			{
				const long resolved = static_cast<long>(vertexCount) + parsed;
				if (resolved < 0)
				{
					return false;
				}
				outIndex = static_cast<uint32_t>(resolved);
				return true;
			}
			return false; // OBJ indices are 1-based; 0 is invalid
		}
	} // namespace

	bool loadObj(const std::string& path, Mesh& outMesh, std::string& outError)
	{
		std::ifstream file(path);
		if (!file.is_open())
		{
			outError = "Could not open OBJ file: " + path;
			return false;
		}

		outMesh.positions.clear();
		outMesh.indices.clear();

		std::string line;
		size_t lineNumber = 0;
		while (std::getline(file, line))
		{
			++lineNumber;
			std::istringstream stream(line);
			std::string keyword;
			if (!(stream >> keyword))
			{
				continue;
			}
			if (keyword == "v")
			{
				glm::vec3 position{0.0f};
				if (!(stream >> position.x >> position.y >> position.z))
				{
					outError = "Malformed vertex at line " + std::to_string(lineNumber);
					return false;
				}
				outMesh.positions.push_back(position);
			}
			else if (keyword == "f")
			{
				std::vector<uint32_t> corners;
				std::string token;
				while (stream >> token)
				{
					uint32_t index = 0;
					if (!parseFaceCorner(token, outMesh.positions.size(), index))
					{
						outError = "Malformed face at line " + std::to_string(lineNumber);
						return false;
					}
					corners.push_back(index);
				}
				if (corners.size() < 3)
				{
					continue;
				}
				// Fan triangulation for polygons.
				for (size_t i = 1; i + 1 < corners.size(); ++i)
				{
					outMesh.indices.push_back(corners[0]);
					outMesh.indices.push_back(corners[i]);
					outMesh.indices.push_back(corners[i + 1]);
				}
			}
			// vt / vn / usemtl / o / g / s / mtllib are ignored.
		}

		if (outMesh.positions.empty() || outMesh.indices.empty())
		{
			outError = "OBJ contains no triangles: " + path;
			return false;
		}
		return true;
	}

	// ---------------------------------------------------------------------------
	// Geometry queries
	// ---------------------------------------------------------------------------
	namespace
	{
		// Closest point on triangle (Ericson, "Real-Time Collision Detection" 5.1.5).
		glm::vec3 closestPointOnTriangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b,
		                                 const glm::vec3& c)
		{
			const glm::vec3 ab = b - a;
			const glm::vec3 ac = c - a;
			const glm::vec3 ap = p - a;
			const float d1 = glm::dot(ab, ap);
			const float d2 = glm::dot(ac, ap);
			if (d1 <= 0.0f && d2 <= 0.0f)
			{
				return a;
			}

			const glm::vec3 bp = p - b;
			const float d3 = glm::dot(ab, bp);
			const float d4 = glm::dot(ac, bp);
			if (d3 >= 0.0f && d4 <= d3)
			{
				return b;
			}

			const float vc = d1 * d4 - d3 * d2;
			if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
			{
				const float v = d1 / (d1 - d3);
				return a + v * ab;
			}

			const glm::vec3 cp = p - c;
			const float d5 = glm::dot(ab, cp);
			const float d6 = glm::dot(ac, cp);
			if (d6 >= 0.0f && d5 <= d6)
			{
				return c;
			}

			const float vb = d5 * d2 - d1 * d6;
			if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
			{
				const float w = d2 / (d2 - d6);
				return a + w * ac;
			}

			const float va = d3 * d6 - d5 * d4;
			if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
			{
				const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
				return b + w * (c - b);
			}

			const float denom = 1.0f / (va + vb + vc);
			const float v = vb * denom;
			const float w = vc * denom;
			return a + ab * v + ac * w;
		}

		float distanceSqPointAABB(const glm::vec3& p, const glm::vec3& bmin, const glm::vec3& bmax)
		{
			const glm::vec3 clamped = glm::clamp(p, bmin, bmax);
			const glm::vec3 delta = p - clamped;
			return glm::dot(delta, delta);
		}

		// Slab test; returns true when the ray segment [0, tMax] overlaps the box.
		bool rayIntersectsAABB(const glm::vec3& origin, const glm::vec3& invDir, const glm::vec3& bmin,
		                       const glm::vec3& bmax, float tMax)
		{
			float tNear = 0.0f;
			float tFar = tMax;
			for (int axis = 0; axis < 3; ++axis)
			{
				const float t0 = (bmin[axis] - origin[axis]) * invDir[axis];
				const float t1 = (bmax[axis] - origin[axis]) * invDir[axis];
				tNear = std::max(tNear, std::min(t0, t1));
				tFar = std::min(tFar, std::max(t0, t1));
				if (tNear > tFar)
				{
					return false;
				}
			}
			return true;
		}

		// Moller-Trumbore. Returns hit parameter t in (tMin, tMax) or false.
		bool rayTriangle(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& a,
		                 const glm::vec3& b, const glm::vec3& c, float tMin, float tMax, float& outT)
		{
			constexpr float kEpsilon = 1e-9f;
			const glm::vec3 edge1 = b - a;
			const glm::vec3 edge2 = c - a;
			const glm::vec3 pvec = glm::cross(dir, edge2);
			const float det = glm::dot(edge1, pvec);
			if (std::fabs(det) < kEpsilon)
			{
				return false;
			}
			const float invDet = 1.0f / det;
			const glm::vec3 tvec = origin - a;
			const float u = glm::dot(tvec, pvec) * invDet;
			if (u < 0.0f || u > 1.0f)
			{
				return false;
			}
			const glm::vec3 qvec = glm::cross(tvec, edge1);
			const float v = glm::dot(dir, qvec) * invDet;
			if (v < 0.0f || u + v > 1.0f)
			{
				return false;
			}
			const float t = glm::dot(edge2, qvec) * invDet;
			if (t <= tMin || t >= tMax)
			{
				return false;
			}
			outT = t;
			return true;
		}

		// -----------------------------------------------------------------------
		// Median-split triangle BVH (self-contained; no third-party dependency)
		// -----------------------------------------------------------------------
		class TriangleBVH
		{
		public:
			struct Triangle
			{
				glm::vec3 v0, v1, v2;
				glm::vec3 geometricNormal; // unnormalized cross product is fine for sign tests
			};

			void build(const std::vector<glm::vec3>& positions, const std::vector<uint32_t>& indices)
			{
				const size_t triangleCount = indices.size() / 3;
				m_triangles.reserve(triangleCount);
				for (size_t i = 0; i < triangleCount; ++i)
				{
					Triangle triangle{};
					triangle.v0 = positions[indices[i * 3 + 0]];
					triangle.v1 = positions[indices[i * 3 + 1]];
					triangle.v2 = positions[indices[i * 3 + 2]];
					triangle.geometricNormal = glm::cross(triangle.v1 - triangle.v0,
					                                      triangle.v2 - triangle.v0);
					m_triangles.push_back(triangle);
				}
				m_order.resize(m_triangles.size());
				std::iota(m_order.begin(), m_order.end(), 0u);
				m_nodes.reserve(m_triangles.size() * 2);
				buildNode(0, static_cast<uint32_t>(m_order.size()));
			}

			[[nodiscard]] float closestDistance(const glm::vec3& point) const
			{
				float bestSq = std::numeric_limits<float>::max();
				closestRecurse(0, point, bestSq);
				return std::sqrt(bestSq);
			}

			// Nearest hit along [tMin, tMax]; outBackFace = ray hits the triangle
			// from behind (dot(dir, normal) > 0).
			[[nodiscard]] bool intersectNearest(const glm::vec3& origin, const glm::vec3& dir, float tMax,
			                                    bool& outBackFace) const
			{
				const glm::vec3 invDir{
					1.0f / (std::fabs(dir.x) > 1e-12f ? dir.x : std::copysign(1e-12f, dir.x)),
					1.0f / (std::fabs(dir.y) > 1e-12f ? dir.y : std::copysign(1e-12f, dir.y)),
					1.0f / (std::fabs(dir.z) > 1e-12f ? dir.z : std::copysign(1e-12f, dir.z)),
				};
				float bestT = tMax;
				int32_t bestTriangle = -1;
				intersectRecurse(0, origin, dir, invDir, bestT, bestTriangle);
				if (bestTriangle < 0)
				{
					return false;
				}
				outBackFace = glm::dot(dir, m_triangles[static_cast<size_t>(bestTriangle)].geometricNormal) > 0.0f;
				return true;
			}

		private:
			struct Node
			{
				glm::vec3 bmin{0.0f};
				glm::vec3 bmax{0.0f};
				int32_t left{-1};   // internal: child node index
				int32_t right{-1};
				uint32_t firstTriangle{0}; // leaf: range into m_order
				uint32_t triangleCount{0};
			};

			static constexpr uint32_t kLeafSize = 4;

			uint32_t buildNode(uint32_t first, uint32_t count)
			{
				const uint32_t nodeIndex = static_cast<uint32_t>(m_nodes.size());
				m_nodes.emplace_back();

				glm::vec3 bmin{std::numeric_limits<float>::max()};
				glm::vec3 bmax{std::numeric_limits<float>::lowest()};
				for (uint32_t i = first; i < first + count; ++i)
				{
					const Triangle& triangle = m_triangles[m_order[i]];
					bmin = glm::min(bmin, glm::min(triangle.v0, glm::min(triangle.v1, triangle.v2)));
					bmax = glm::max(bmax, glm::max(triangle.v0, glm::max(triangle.v1, triangle.v2)));
				}
				m_nodes[nodeIndex].bmin = bmin;
				m_nodes[nodeIndex].bmax = bmax;

				if (count <= kLeafSize)
				{
					m_nodes[nodeIndex].firstTriangle = first;
					m_nodes[nodeIndex].triangleCount = count;
					return nodeIndex;
				}

				const glm::vec3 extent = bmax - bmin;
				int axis = 0;
				if (extent.y > extent.x)
				{
					axis = 1;
				}
				if (extent.z > extent[axis])
				{
					axis = 2;
				}

				const uint32_t mid = first + count / 2;
				std::nth_element(m_order.begin() + first, m_order.begin() + mid,
				                 m_order.begin() + first + count,
				                 [&](uint32_t lhs, uint32_t rhs)
				                 {
					                 const Triangle& a = m_triangles[lhs];
					                 const Triangle& b = m_triangles[rhs];
					                 const float ca = a.v0[axis] + a.v1[axis] + a.v2[axis];
					                 const float cb = b.v0[axis] + b.v1[axis] + b.v2[axis];
					                 return ca < cb;
				                 });

				const uint32_t leftCount = mid - first;
				if (leftCount == 0 || leftCount == count)
				{
					// Degenerate split (all centroids equal): make a leaf.
					m_nodes[nodeIndex].firstTriangle = first;
					m_nodes[nodeIndex].triangleCount = count;
					return nodeIndex;
				}

				const uint32_t left = buildNode(first, leftCount);
				const uint32_t right = buildNode(mid, count - leftCount);
				m_nodes[nodeIndex].left = static_cast<int32_t>(left);
				m_nodes[nodeIndex].right = static_cast<int32_t>(right);
				return nodeIndex;
			}

			void closestRecurse(uint32_t nodeIndex, const glm::vec3& point, float& bestSq) const
			{
				const Node& node = m_nodes[nodeIndex];
				if (distanceSqPointAABB(point, node.bmin, node.bmax) >= bestSq)
				{
					return;
				}
				if (node.left < 0)
				{
					for (uint32_t i = node.firstTriangle; i < node.firstTriangle + node.triangleCount; ++i)
					{
						const Triangle& triangle = m_triangles[m_order[i]];
						const glm::vec3 closest = closestPointOnTriangle(point, triangle.v0, triangle.v1,
						                                                 triangle.v2);
						const glm::vec3 delta = point - closest;
						bestSq = std::min(bestSq, glm::dot(delta, delta));
					}
					return;
				}
				// Visit nearer child first for tighter pruning.
				const uint32_t left = static_cast<uint32_t>(node.left);
				const uint32_t right = static_cast<uint32_t>(node.right);
				const float distLeft = distanceSqPointAABB(point, m_nodes[left].bmin, m_nodes[left].bmax);
				const float distRight = distanceSqPointAABB(point, m_nodes[right].bmin, m_nodes[right].bmax);
				if (distLeft < distRight)
				{
					closestRecurse(left, point, bestSq);
					closestRecurse(right, point, bestSq);
				}
				else
				{
					closestRecurse(right, point, bestSq);
					closestRecurse(left, point, bestSq);
				}
			}

			void intersectRecurse(uint32_t nodeIndex, const glm::vec3& origin, const glm::vec3& dir,
			                      const glm::vec3& invDir, float& bestT, int32_t& bestTriangle) const
			{
				const Node& node = m_nodes[nodeIndex];
				if (!rayIntersectsAABB(origin, invDir, node.bmin, node.bmax, bestT))
				{
					return;
				}
				if (node.left < 0)
				{
					for (uint32_t i = node.firstTriangle; i < node.firstTriangle + node.triangleCount; ++i)
					{
						const uint32_t triangleIndex = m_order[i];
						const Triangle& triangle = m_triangles[triangleIndex];
						float t = 0.0f;
						if (rayTriangle(origin, dir, triangle.v0, triangle.v1, triangle.v2, 1e-6f, bestT, t))
						{
							bestT = t;
							bestTriangle = static_cast<int32_t>(triangleIndex);
						}
					}
					return;
				}
				intersectRecurse(static_cast<uint32_t>(node.left), origin, dir, invDir, bestT, bestTriangle);
				intersectRecurse(static_cast<uint32_t>(node.right), origin, dir, invDir, bestT, bestTriangle);
			}

			std::vector<Triangle> m_triangles;
			std::vector<uint32_t> m_order;
			std::vector<Node> m_nodes;
		};

		// LuxGI-style sphere direction set: sampleCount x sampleCount grid over
		// (phi, cos(theta)).
		std::vector<glm::vec3> buildSampleDirections(uint32_t sampleCount)
		{
			constexpr float kTwoPi = 6.28318530717958647692f;
			std::vector<glm::vec3> directions;
			directions.reserve(static_cast<size_t>(sampleCount) * sampleCount);
			for (uint32_t sx = 0; sx < sampleCount; ++sx)
			{
				for (uint32_t sy = 0; sy < sampleCount; ++sy)
				{
					const float u = (sampleCount > 1) ? static_cast<float>(sx) / static_cast<float>(sampleCount - 1)
					                                  : 0.0f;
					const float v = (sampleCount > 1)
						                ? static_cast<float>(sy) / static_cast<float>(sampleCount - 1) * 2.0f - 1.0f
						                : 0.0f;
					const float phi = u * kTwoPi;
					const float cosTheta = glm::clamp(v, -1.0f, 1.0f);
					const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
					directions.emplace_back(sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);
				}
			}
			return directions;
		}
	} // namespace

	// ---------------------------------------------------------------------------
	// Baking
	// ---------------------------------------------------------------------------
	bool bake(const Mesh& mesh, const SDFBakerConfig& config, BakedSDF& outSDF, std::string& outError)
	{
		if (mesh.positions.empty() || mesh.indices.size() < 3)
		{
			outError = "Mesh has no triangles";
			return false;
		}

		glm::vec3 boundsMin{std::numeric_limits<float>::max()};
		glm::vec3 boundsMax{std::numeric_limits<float>::lowest()};
		for (const glm::vec3& position : mesh.positions)
		{
			boundsMin = glm::min(boundsMin, position);
			boundsMax = glm::max(boundsMax, position);
		}

		glm::vec3 extent = boundsMax - boundsMin;
		if (glm::dot(extent, extent) <= 0.0f)
		{
			outError = "Mesh bounding box is degenerate";
			return false;
		}

		// Pad the AABB so the zero-isosurface never touches the volume border
		// (5% of each axis extent, with an absolute floor for thin meshes).
		const glm::vec3 padding = glm::max(extent * 0.05f, glm::vec3(1e-3f));
		boundsMin -= padding;
		boundsMax += padding;
		extent = boundsMax - boundsMin;

		// Per-axis adaptive resolution: voxels = extent * targetTexelPerMeter,
		// clamped to [minResolution, maxResolution].
		glm::uvec3 resolution{0u};
		for (int axis = 0; axis < 3; ++axis)
		{
			const float target = std::ceil(extent[axis] * config.targetTexelPerMeter);
			resolution[axis] = static_cast<uint32_t>(glm::clamp(target,
			                                                    static_cast<float>(config.minResolution),
			                                                    static_cast<float>(config.maxResolution)));
		}

		const float maxDistance = std::max(extent.x, std::max(extent.y, extent.z));

		TriangleBVH bvh;
		bvh.build(mesh.positions, mesh.indices);

		const std::vector<glm::vec3> sampleDirections = buildSampleDirections(config.sampleCount);
		const size_t voxelCount = static_cast<size_t>(resolution.x) * resolution.y * resolution.z;

		outSDF.resolution = resolution;
		outSDF.boundsMin = boundsMin;
		outSDF.boundsMax = boundsMax;
		outSDF.halfTexels.assign(voxelCount, 0u);

		// Voxel centers span the padded AABB inclusively (LuxGI flattening:
		// index = x + y*rx + z*rx*ry).
		const glm::vec3 cellSize = extent / glm::vec3(glm::max(resolution - glm::uvec3(1u), glm::uvec3(1u)));

		const auto bakeSlice = [&](uint32_t z)
		{
			for (uint32_t y = 0; y < resolution.y; ++y)
			{
				for (uint32_t x = 0; x < resolution.x; ++x)
				{
					const glm::vec3 voxelPos = boundsMin +
						glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)) * cellSize;

					float distance = bvh.closestDistance(voxelPos);

					// Sign by back-face voting (LuxGI SDFBaker.cpp:113-144): cast the
					// direction set, count nearest hits whose triangle faces away.
					// Majority back-face hits => the voxel is inside.
					uint32_t hitCount = 0;
					uint32_t backHitCount = 0;
					for (const glm::vec3& dir : sampleDirections)
					{
						bool backFace = false;
						if (bvh.intersectNearest(voxelPos, dir, maxDistance, backFace))
						{
							++hitCount;
							if (backFace)
							{
								++backHitCount;
							}
						}
					}
					if (hitCount > 0 &&
					    static_cast<float>(backHitCount) > static_cast<float>(sampleDirections.size()) * 0.5f)
					{
						distance = -distance;
					}

					// Normalize: d / maxDistance in [-1, 1], stored as (d + 1) / 2.
					const float normalized = glm::clamp(distance / maxDistance, -1.0f, 1.0f);
					const float encoded = (normalized + 1.0f) * 0.5f;

					const size_t index = static_cast<size_t>(x) +
						static_cast<size_t>(y) * resolution.x +
						static_cast<size_t>(z) * resolution.x * resolution.y;
					outSDF.halfTexels[index] = floatToHalf(encoded);
				}
			}
		};

		// Parallelize over Z slices (offline tool; std::thread keeps it portable).
		const uint32_t workerCount = std::max(1u, std::thread::hardware_concurrency());
		std::atomic<uint32_t> nextSlice{0u};
		std::vector<std::thread> workers;
		workers.reserve(workerCount);
		for (uint32_t w = 0; w < workerCount; ++w)
		{
			workers.emplace_back([&]
			{
				for (;;)
				{
					const uint32_t z = nextSlice.fetch_add(1u);
					if (z >= resolution.z)
					{
						return;
					}
					bakeSlice(z);
				}
			});
		}
		for (std::thread& worker : workers)
		{
			worker.join();
		}

		return true;
	}

	// ---------------------------------------------------------------------------
	// Serialization
	// ---------------------------------------------------------------------------
	bool writeBin(const std::string& path, const BakedSDF& sdf, std::string& outError)
	{
		std::ofstream file(path, std::ios::binary);
		if (!file.is_open())
		{
			outError = "Could not open output file: " + path;
			return false;
		}

		file.write(kSDFMagic, sizeof(kSDFMagic));
		file.write(reinterpret_cast<const char*>(&kSDFVersion), sizeof(kSDFVersion));
		file.write(reinterpret_cast<const char*>(&sdf.resolution), sizeof(uint32_t) * 3);
		file.write(reinterpret_cast<const char*>(&sdf.boundsMin), sizeof(float) * 3);
		file.write(reinterpret_cast<const char*>(&sdf.boundsMax), sizeof(float) * 3);
		file.write(reinterpret_cast<const char*>(sdf.halfTexels.data()),
		           static_cast<std::streamsize>(sdf.halfTexels.size() * sizeof(uint16_t)));

		if (!file.good())
		{
			outError = "Write failed: " + path;
			return false;
		}
		return true;
	}
} // namespace sdf_baker
