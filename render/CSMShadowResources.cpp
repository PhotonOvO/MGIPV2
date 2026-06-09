#include "CSMShadowResources.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

namespace demo
{
	namespace
	{
		// Shadow parameters (consistent with single shadow system)
		constexpr float kDefaultMaxShadowDistance = 100.0f;
		constexpr float kReceiverBias = 0.0015f;
		constexpr float kDepthBiasConstant = 1.25f;
		constexpr float kDepthBiasSlope = 1.75f;
		constexpr float kShadowIntensity = 1.0f;
		constexpr float kShadowKernelRadius = 1.0f; // 1 => 3x3 PCF
		constexpr float kCascadeBiasScaleFactor = 0.5f; // Bias decreases for farther cascades
		constexpr float kCascadeNearPlanePadding = 25.0f; // Padding for near plane in ortho projection
		constexpr float kCascadeCasterDepthPaddingScale = 0.02f;
		constexpr float kCascadeCasterExtrusionPaddingScale = 0.25f;
		constexpr float kCascadeCasterMinGuardTexels = 8.0f;

		[[nodiscard]] float extractNearPlane(const glm::mat4& projection,
		                                     const clipspace::ProjectionConvention& convention)
		{
			return clipspace::extractPerspectiveNearPlane(projection, convention);
		}

		[[nodiscard]] float extractFarPlane(const glm::mat4& projection,
		                                    const clipspace::ProjectionConvention& convention)
		{
			return clipspace::extractPerspectiveFarPlane(projection, convention);
		}

		[[nodiscard]] glm::vec3 safeNormalize(const glm::vec3& value, const glm::vec3& fallback)
		{
			const float lengthSq = glm::dot(value, value);
			return lengthSq > 1e-6f ? value * glm::inversesqrt(lengthSq) : fallback;
		}

		[[nodiscard]] glm::mat4 resolveCascadeCameraViewProjection(const shaderio::CameraUniforms& camera)
		{
			const glm::mat4& unjittered = camera.unjitteredViewProjection;
			const float determinant = glm::determinant(unjittered);
			return std::abs(determinant) > 1.0e-6f ? unjittered : camera.viewProjection;
		}

		// Compute cascade split distances using practical split scheme
		// Lambda blends logarithmic (better for perspective) and uniform (better for uniform distribution)
		void computeCascadeSplits(float* splits, uint32_t count, float nearDist, float farDist, float lambda)
		{
			for (uint32_t i = 0; i < count; ++i)
			{
				const float fraction = static_cast<float>(i + 1) / static_cast<float>(count);
				const float uniformSplit = nearDist + (farDist - nearDist) * fraction;
				const float logSplit = nearDist * std::pow(farDist / nearDist, fraction);
				splits[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
			}
		}

		// Compute frustum corners for a slice between sliceNear and sliceFar distances
		[[nodiscard]] std::array<glm::vec3, 8> computeFrustumSliceCornersWorld(
			const shaderio::CameraUniforms& camera,
			const clipspace::ProjectionConvention& projectionConvention,
			float sliceNear,
			float sliceFar)
		{
			const glm::mat4 invViewProjection = glm::inverse(resolveCascadeCameraViewProjection(camera));
			const float cameraNear = std::max(0.01f, extractNearPlane(camera.projection, projectionConvention));
			const float cameraFar = std::max(cameraNear + 0.01f,
			                                 extractFarPlane(camera.projection, projectionConvention));

			// Clamp slice distances to valid range
			sliceNear = glm::clamp(sliceNear, cameraNear, cameraFar);
			sliceFar = glm::clamp(sliceFar, sliceNear + 0.01f, cameraFar);

			// Compute lerp factors for near and far planes of the slice
			const float nearLerp = (sliceNear - cameraNear) / std::max(0.01f, cameraFar - cameraNear);
			const float farLerp = (sliceFar - cameraNear) / std::max(0.01f, cameraFar - cameraNear);

			const std::array<glm::vec2, 4> ndcCorners = {
				glm::vec2(-1.0f, -1.0f),
				glm::vec2(1.0f, -1.0f),
				glm::vec2(1.0f, 1.0f),
				glm::vec2(-1.0f, 1.0f),
			};

			std::array<glm::vec3, 4> cameraNearCorners{};
			std::array<glm::vec3, 4> cameraFarCorners{};

			// Transform NDC corners to world space
			for (size_t i = 0; i < ndcCorners.size(); ++i)
			{
				glm::vec4 nearCorner = invViewProjection *
					glm::vec4(ndcCorners[i], projectionConvention.ndcNearZ, 1.0f);
				glm::vec4 farCorner = invViewProjection * glm::vec4(ndcCorners[i], projectionConvention.ndcFarZ, 1.0f);
				nearCorner /= nearCorner.w;
				farCorner /= farCorner.w;
				cameraNearCorners[i] = glm::vec3(nearCorner);
				cameraFarCorners[i] = glm::vec3(farCorner);
			}

			// Interpolate corners to slice boundaries
			std::array<glm::vec3, 8> sliceCorners{};
			for (size_t i = 0; i < ndcCorners.size(); ++i)
			{
				const glm::vec3 ray = cameraFarCorners[i] - cameraNearCorners[i];
				sliceCorners[i] = cameraNearCorners[i] + ray * nearLerp; // Near corners of slice
				sliceCorners[i + 4] = cameraNearCorners[i] + ray * farLerp; // Far corners of slice
			}

			return sliceCorners;
		}

		// Compute bounding sphere from frustum corners (rotation-stable)
		struct BoundingSphere
		{
			glm::vec3 center;
			float radius;
		};

		[[nodiscard]] BoundingSphere computeBoundingSphere(const std::array<glm::vec3, 8>& corners)
		{
			// Compute center as average of corners
			glm::vec3 center(0.0f);
			for (const glm::vec3& corner : corners)
			{
				center += corner;
			}
			center /= static_cast<float>(corners.size());

			// Compute radius as maximum distance from center
			float radius = 0.0f;
			for (const glm::vec3& corner : corners)
			{
				radius = std::max(radius, glm::length(corner - center));
			}

			return {center, radius};
		}

		// Snap projection bounds to texel grid for shadow stability
		void snapToTexelGrid(float& left, float& right, float& bottom, float& top, uint32_t resolution)
		{
			const float diameter = std::max(right - left, top - bottom);
			const float texelSize = diameter / static_cast<float>(resolution);

			if (texelSize > 0.0f)
			{
				left = std::floor(left / texelSize) * texelSize;
				right = std::ceil(right / texelSize) * texelSize;
				bottom = std::floor(bottom / texelSize) * texelSize;
				top = std::ceil(top / texelSize) * texelSize;
			}
		}

		[[nodiscard]] glm::vec2 snapCenterToTexelGrid(const glm::vec2& center, float diameter, uint32_t resolution)
		{
			const float texelSize = diameter / static_cast<float>(std::max(resolution, 1u));
			if (texelSize <= 0.0f)
			{
				return center;
			}
			return glm::floor(center / texelSize + glm::vec2(0.5f)) * texelSize;
		}

		[[nodiscard]] bool isValidBounds(const glm::vec3& boundsMin, const glm::vec3& boundsMax)
		{
			return glm::all(glm::lessThanEqual(boundsMin, boundsMax));
		}

		[[nodiscard]] std::array<glm::vec3, 8> computeAabbCorners(const glm::vec3& boundsMin,
		                                                          const glm::vec3& boundsMax)
		{
			return {
				glm::vec3(boundsMin.x, boundsMin.y, boundsMin.z),
				glm::vec3(boundsMax.x, boundsMin.y, boundsMin.z),
				glm::vec3(boundsMin.x, boundsMax.y, boundsMin.z),
				glm::vec3(boundsMax.x, boundsMax.y, boundsMin.z),
				glm::vec3(boundsMin.x, boundsMin.y, boundsMax.z),
				glm::vec3(boundsMax.x, boundsMin.y, boundsMax.z),
				glm::vec3(boundsMin.x, boundsMax.y, boundsMax.z),
				glm::vec3(boundsMax.x, boundsMax.y, boundsMax.z),
			};
		}

		[[nodiscard]] glm::vec4 makeNormalizedPlane(const glm::vec4& plane)
		{
			const float length = glm::length(glm::vec3(plane));
			return length > 0.0f ? plane / length : plane;
		}

		[[nodiscard]] std::array<glm::vec4, shaderio::LGPUCullingFrustumPlaneCount> makeOrthoCullingPlanes(
			const glm::mat4& lightView,
			float left,
			float right,
			float bottom,
			float top,
			float nearPlane,
			float farPlane)
		{
			const glm::mat4 worldToLight = lightView;
			const glm::mat4 lightToWorld = glm::inverse(worldToLight);
			const glm::vec3 lightRight = glm::normalize(glm::vec3(lightToWorld[0]));
			const glm::vec3 lightUp = glm::normalize(glm::vec3(lightToWorld[1]));
			const glm::vec3 lightForward = -glm::normalize(glm::vec3(lightToWorld[2]));
			const glm::vec3 lightOrigin = glm::vec3(lightToWorld[3]);
			const glm::vec3 nearCenter = lightOrigin + lightForward * nearPlane;
			const glm::vec3 farCenter = lightOrigin + lightForward * farPlane;

			return {
				makeNormalizedPlane(glm::vec4(lightRight, -(glm::dot(lightRight, lightOrigin) + left))),
				makeNormalizedPlane(glm::vec4(-lightRight, (glm::dot(lightRight, lightOrigin) + right))),
				makeNormalizedPlane(glm::vec4(lightUp, -(glm::dot(lightUp, lightOrigin) + bottom))),
				makeNormalizedPlane(glm::vec4(-lightUp, (glm::dot(lightUp, lightOrigin) + top))),
				makeNormalizedPlane(glm::vec4(lightForward, -glm::dot(lightForward, nearCenter))),
				makeNormalizedPlane(glm::vec4(-lightForward, glm::dot(lightForward, farCenter))),
			};
		}
	} // namespace

	void CSMShadowResources::init(rhi::Device& device, rhi::CommandBuffer& cmd, const CreateInfo& createInfo)
	{
		m_device = &device;
		m_cascadeCount = createInfo.cascadeCount;
		m_cascadeResolution = createInfo.cascadeResolution;
		m_shadowFormat = createInfo.shadowFormat;
		m_projectionConvention = createInfo.projectionConvention;

		assert(m_cascadeCount <= shaderio::LCascadeCount && "Cascade count exceeds maximum");

		m_cascadeArray = device.createTexture(rhi::TextureDesc{
			.dimension = rhi::TextureDimension::e2DArray,
			.format = m_shadowFormat,
			.usage = rhi::TextureUsageFlags::depthAttachment | rhi::TextureUsageFlags::sampled,
			.extent = {m_cascadeResolution, m_cascadeResolution, 1},
			.mipLevels = 1,
			.arrayLayers = m_cascadeCount,
			.sampleCount = rhi::SampleCount::count1,
			.memoryUsage = rhi::MemoryUsage::gpuOnly,
			.debugName = "CSM_CascadeArray",
		});

		m_shadowUniformBuffer = device.createBuffer(rhi::BufferDesc{
			.size = sizeof(shaderio::ShadowUniforms),
			.usage = rhi::BufferUsageFlags::uniform,
			.memoryUsage = rhi::MemoryUsage::cpuToGpu,
			.debugName = "CSM_ShadowUniformBuffer",
		});
		m_shadowUniformMapped = device.mapBuffer(m_shadowUniformBuffer);

		// Initialize shadow uniforms with defaults
		m_shadowUniformsData.lightDirectionAndIntensity = glm::vec4(0.0f, -1.0f, 0.0f, kShadowIntensity);
		m_shadowUniformsData.shadowMapMetrics = glm::vec4(
			1.0f / static_cast<float>(m_cascadeResolution),
			kDefaultMaxShadowDistance,
			0.0f,
			static_cast<float>(m_cascadeCount));
		m_shadowUniformsData.cascadeBiasScale = glm::vec4(
			kDepthBiasConstant,
			kDepthBiasSlope,
			kCascadeBiasScaleFactor,
			0.0f); // normal bias placeholder

		std::memcpy(m_shadowUniformMapped, &m_shadowUniformsData, sizeof(m_shadowUniformsData));
		const rhi::TextureBarrier initBarrier{
			.texture = m_cascadeArray,
			.before = rhi::ResourceState::Undefined,
			.after = rhi::ResourceState::General,
			.range = {.aspect = rhi::TextureAspect::depth, .levelCount = 1, .layerCount = m_cascadeCount},
		};
		cmd.resourceBarrier(&initBarrier, 1, nullptr, 0);
	}

	void CSMShadowResources::deinit()
	{
		if (m_device != nullptr && !m_shadowUniformBuffer.isNull())
		{
			m_device->unmapBuffer(m_shadowUniformBuffer);
			m_device->destroyBuffer(m_shadowUniformBuffer);
		}

		if (m_device != nullptr && !m_cascadeArray.isNull())
		{
			m_device->destroyTexture(m_cascadeArray);
		}

		*this = CSMShadowResources{};
	}

	void CSMShadowResources::updateCascadeMatrices(const shaderio::CameraUniforms& camera, const glm::vec3& lightDir)
	{
		updateCascadeMatrices(camera, lightDir, kDefaultMaxShadowDistance);
	}

	void CSMShadowResources::updateCascadeMatrices(const shaderio::CameraUniforms& camera,
	                                               const glm::vec3& lightDir,
	                                               float maxShadowDistance)
	{
		updateCascadeMatrices(camera, lightDir, maxShadowDistance, glm::vec3(0.0f), glm::vec3(0.0f), false);
	}

	void CSMShadowResources::updateCascadeMatrices(const shaderio::CameraUniforms& camera,
	                                               const glm::vec3& lightDir,
	                                               float requestedMaxShadowDistance,
	                                               const glm::vec3& casterBoundsMin,
	                                               const glm::vec3& casterBoundsMax,
	                                               bool casterBoundsValid)
	{
		const float cameraFar = std::max(1.0f, extractFarPlane(camera.projection, m_projectionConvention));
		const float cameraNear = std::max(0.01f, extractNearPlane(camera.projection, m_projectionConvention));
		const float requestedShadowDistance = requestedMaxShadowDistance > 0.0f
			                                      ? requestedMaxShadowDistance
			                                      : kDefaultMaxShadowDistance;
		const float maxShadowDistance = glm::clamp(requestedShadowDistance, cameraNear + 0.01f, cameraFar);
		const glm::vec3 lightDirection = safeNormalize(lightDir, glm::vec3(0.0f, -1.0f, 0.0f));
		const bool useCasterBounds = casterBoundsValid && isValidBounds(casterBoundsMin, casterBoundsMax);
		const std::array<glm::vec3, 8> casterCorners =
			useCasterBounds ? computeAabbCorners(casterBoundsMin, casterBoundsMax) : std::array<glm::vec3, 8>{};
		const float casterDepthPadding = useCasterBounds
			                                 ? std::max(
				                                 1.0f, glm::length(casterBoundsMax - casterBoundsMin) *
				                                 kCascadeCasterDepthPaddingScale)
			                                 : 0.0f;

		m_frameData = FrameData{};
		m_frameData.cascadeCount = m_cascadeCount;
		m_frameData.lightDirection = lightDirection;
		m_frameData.maxShadowDistance = maxShadowDistance;
		m_frameData.casterBoundsMin = casterBoundsMin;
		m_frameData.casterBoundsMax = casterBoundsMax;
		m_frameData.casterBoundsValid = useCasterBounds;

		// Compute cascade split distances using practical split scheme
		float cascadeSplits[shaderio::LCascadeCount];
		computeCascadeSplits(cascadeSplits, m_cascadeCount, cameraNear, maxShadowDistance,
		                     shaderio::LCascadeSplitLambda);

		// Store split distances in uniform data
		m_shadowUniformsData.cascadeSplitDistances = glm::vec4(
			cascadeSplits[0],
			m_cascadeCount > 1 ? cascadeSplits[1] : 0.0f,
			m_cascadeCount > 2 ? cascadeSplits[2] : 0.0f,
			m_cascadeCount > 3 ? cascadeSplits[3] : 0.0f);
		m_frameData.splitDistances = m_shadowUniformsData.cascadeSplitDistances;

		// Choose world-up vector for light view matrix (avoid near-parallel with light direction)
		const glm::vec3 worldUp = std::abs(lightDirection.y) > 0.95f
			                          ? glm::vec3(0.0f, 0.0f, 1.0f)
			                          : glm::vec3(0.0f, 1.0f, 0.0f);

		// Compute each cascade's view-projection matrix
		float prevSplitDistance = cameraNear;
		for (uint32_t cascadeIndex = 0; cascadeIndex < m_cascadeCount; ++cascadeIndex)
		{
			const float splitDistance = cascadeSplits[cascadeIndex];
			CascadeData& cascadeData = m_frameData.cascades[cascadeIndex];
			cascadeData.splitNear = prevSplitDistance;
			cascadeData.splitFar = splitDistance;

			// Get frustum corners for this cascade slice
			const std::array<glm::vec3, 8> sliceCorners =
				computeFrustumSliceCornersWorld(camera, m_projectionConvention, prevSplitDistance, splitDistance);
			cascadeData.receiverCornersWorld = sliceCorners;

			// Compute bounding sphere (rotation-stable)
			const BoundingSphere boundingSphere = computeBoundingSphere(sliceCorners);
			cascadeData.receiverCenter = boundingSphere.center;
			cascadeData.receiverRadius = boundingSphere.radius;

			const float diameter = boundingSphere.radius * 2.0f;

			// Position the cascade on a stable light-space texel grid. This keeps static shadows
			// from swimming when the camera moves by sub-texel amounts.
			const glm::mat4 stableLightView = glm::lookAt(glm::vec3(0.0f), lightDirection, worldUp);
			glm::vec3 stableCenterLightSpace = glm::vec3(stableLightView * glm::vec4(boundingSphere.center, 1.0f));
			const glm::vec2 snappedCenterXY =
				snapCenterToTexelGrid(glm::vec2(stableCenterLightSpace), diameter, m_cascadeResolution);
			stableCenterLightSpace.x = snappedCenterXY.x;
			stableCenterLightSpace.y = snappedCenterXY.y;
			const glm::vec3 stableCenterWorld =
				glm::vec3(glm::inverse(stableLightView) * glm::vec4(stableCenterLightSpace, 1.0f));
			glm::vec3 lightPosition = stableCenterWorld - lightDirection * boundingSphere.radius;
			glm::mat4 lightView = glm::lookAt(lightPosition, stableCenterWorld, worldUp);

			// Transform corners to light space to find ortho bounds
			glm::vec3 minLightSpace(std::numeric_limits<float>::max());
			glm::vec3 maxLightSpace(std::numeric_limits<float>::lowest());

			glm::vec3 casterMinLightSpace = minLightSpace;
			glm::vec3 casterMaxLightSpace = maxLightSpace;
			const auto updateLightSpaceBounds = [&]
			{
				minLightSpace = glm::vec3(std::numeric_limits<float>::max());
				maxLightSpace = glm::vec3(std::numeric_limits<float>::lowest());
				for (const glm::vec3& corner : sliceCorners)
				{
					const glm::vec3 lightSpaceCorner = glm::vec3(lightView * glm::vec4(corner, 1.0f));
					minLightSpace = glm::min(minLightSpace, lightSpaceCorner);
					maxLightSpace = glm::max(maxLightSpace, lightSpaceCorner);
				}

				casterMinLightSpace = minLightSpace;
				casterMaxLightSpace = maxLightSpace;
				if (useCasterBounds)
				{
					casterMinLightSpace = glm::vec3(std::numeric_limits<float>::max());
					casterMaxLightSpace = glm::vec3(std::numeric_limits<float>::lowest());
					for (const glm::vec3& corner : casterCorners)
					{
						const glm::vec3 lightSpaceCorner = glm::vec3(lightView * glm::vec4(corner, 1.0f));
						casterMinLightSpace = glm::min(casterMinLightSpace, lightSpaceCorner);
						casterMaxLightSpace = glm::max(casterMaxLightSpace, lightSpaceCorner);
					}
				}
			};

			updateLightSpaceBounds();

			if (useCasterBounds)
			{
				const float nearCasterGuard = std::max(kCascadeNearPlanePadding, casterDepthPadding);
				const float lightCameraShift = std::max(0.0f, casterMaxLightSpace.z + nearCasterGuard);
				if (lightCameraShift > 0.0f)
				{
					lightPosition -= lightDirection * lightCameraShift;
					lightView = glm::lookAt(lightPosition, stableCenterWorld, worldUp);
					updateLightSpaceBounds();
				}
			}

			cascadeData.lightPosition = lightPosition;
			cascadeData.lightView = lightView;
			cascadeData.receiverMinLightSpace = minLightSpace;
			cascadeData.receiverMaxLightSpace = maxLightSpace;
			cascadeData.casterMinLightSpace = casterMinLightSpace;
			cascadeData.casterMaxLightSpace = casterMaxLightSpace;

			// Use a square projection for rotation stability, then snap the bounds to the texel grid.
			const float projectionDiameter = diameter;
			const float texelSize = projectionDiameter / static_cast<float>(std::max(m_cascadeResolution, 1u));
			const float halfSize = projectionDiameter * 0.5f + texelSize;
			const glm::vec2 centerXY = glm::vec2(lightView * glm::vec4(stableCenterWorld, 1.0f));

			float left = centerXY.x - halfSize;
			float right = centerXY.x + halfSize;
			float bottom = centerXY.y - halfSize;
			float top = centerXY.y + halfSize;

			// Snap bounds to texel grid for stability
			snapToTexelGrid(left, right, bottom, top, m_cascadeResolution);

			const float casterDepthRange = std::max(0.0f, casterMaxLightSpace.z - casterMinLightSpace.z);
			const float casterGuard = std::max(texelSize * kCascadeCasterMinGuardTexels,
			                                   casterDepthRange * kCascadeCasterExtrusionPaddingScale);
			const float cullLeft = useCasterBounds ? std::min(left, casterMinLightSpace.x - casterGuard) : left;
			const float cullRight = useCasterBounds ? std::max(right, casterMaxLightSpace.x + casterGuard) : right;
			const float cullBottom = useCasterBounds ? std::min(bottom, casterMinLightSpace.y - casterGuard) : bottom;
			const float cullTop = useCasterBounds ? std::max(top, casterMaxLightSpace.y + casterGuard) : top;

			// Compute near/far planes with padding
			const float depthPadding = useCasterBounds
				                           ? std::max(kCascadeNearPlanePadding, casterDepthPadding)
				                           : std::max(kCascadeNearPlanePadding, boundingSphere.radius);
			const float nearPlane = std::max(0.1f, -casterMaxLightSpace.z - depthPadding);
			const float farPlane = std::max(nearPlane + 1.0f, -casterMinLightSpace.z + depthPadding);
			cascadeData.nearPlane = nearPlane;
			cascadeData.farPlane = farPlane;
			cascadeData.texelSize = texelSize;

			// Create orthographic projection
			const glm::mat4 lightProjection = clipspace::makeOrthographicProjection(
				left, right, bottom, top, nearPlane, farPlane, m_projectionConvention);
			cascadeData.lightProjection = lightProjection;

			const glm::mat4 lightViewProjection = lightProjection * lightView;
			const glm::mat4 lightCullingProjection = clipspace::makeOrthographicProjection(
				cullLeft, cullRight, cullBottom, cullTop, nearPlane, farPlane, m_projectionConvention);
			const glm::mat4 lightCullingViewProjection = lightCullingProjection * lightView;
			cascadeData.viewProjection = lightViewProjection;
			cascadeData.cullingViewProjection = lightCullingViewProjection;
			cascadeData.worldToShadowTexture =
				clipspace::makeNdcToShadowTextureMatrix(m_projectionConvention) * lightViewProjection;
			cascadeData.cullingPlanes = makeOrthoCullingPlanes(
				lightView, cullLeft, cullRight, cullBottom, cullTop, nearPlane, farPlane);

			// Store cascade matrices
			m_shadowUniformsData.cascadeViewProjection[cascadeIndex] = lightViewProjection;
			m_shadowUniformsData.cascadeWorldToShadowTexture[cascadeIndex] = cascadeData.worldToShadowTexture;

			prevSplitDistance = splitDistance;
		}

		// Update light direction
		m_shadowUniformsData.lightDirectionAndIntensity = glm::vec4(lightDirection, kShadowIntensity);

		// Update shadow map metrics
		m_shadowUniformsData.shadowMapMetrics = glm::vec4(
			1.0f / static_cast<float>(m_cascadeResolution),
			maxShadowDistance,
			0.0f,
			static_cast<float>(m_cascadeCount));

		// Upload to GPU
		std::memcpy(m_shadowUniformMapped, &m_shadowUniformsData, sizeof(m_shadowUniformsData));
	}
} // namespace demo
