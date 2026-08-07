#include "CSMShadowResources.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

namespace demo
{
	namespace
	{
		// Shadow parameters (consistent with single shadow system)
		constexpr float kDefaultMaxShadowDistance = 100.0f;
		constexpr float kShadowIntensity = 1.0f;
		constexpr float kCascadeStableNearPlane = 0.1f;
		constexpr float kCascadeDepthPadding = 25.0f; // Fixed world-space padding on both light-depth sides
		constexpr float kCascadeCasterMinGuardTexels = 8.0f;
		constexpr float kCascadeCasterMinGuardWorld = 0.05f;
		constexpr float kLightBasisVectorEpsilon = 1.0e-8f;
		// Keep the original 16-epsilon numerical pole core, but replace its hard
		// output edge with a C2 annulus. The 1024-epsilon outer chord is 2^-13:
		// still a tiny angular neighborhood, while the selected chart's O(1/chord)
		// sensitivity to an epsilon-scale direction perturbation has fallen to about
		// 2^-10 radians. Both bounds are exact power-of-two precision multiples,
		// rather than scene-tuned thresholds.
		constexpr double kLightBasisRegularizationInnerChord =
			16.0 * static_cast<double>(std::numeric_limits<float>::epsilon());
		constexpr double kLightBasisRegularizationInnerChordSq =
			kLightBasisRegularizationInnerChord * kLightBasisRegularizationInnerChord;
		constexpr double kLightBasisRegularizationOuterChord =
			1024.0 * static_cast<double>(std::numeric_limits<float>::epsilon());
		constexpr double kLightBasisRegularizationOuterChordSq =
			kLightBasisRegularizationOuterChord * kLightBasisRegularizationOuterChord;

		[[nodiscard]] float extractCameraNearPlane(const glm::mat4& projection,
		                                           const clipspace::ProjectionConvention& convention)
		{
			return clipspace::extractNearPlane(projection, convention);
		}

		[[nodiscard]] float extractCameraFarPlane(const glm::mat4& projection,
		                                          const clipspace::ProjectionConvention& convention)
		{
			return clipspace::extractFarPlane(projection, convention);
		}

		[[nodiscard]] bool isFiniteVector(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		[[nodiscard]] bool isFiniteVector(const glm::dvec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		[[nodiscard]] bool tryScaleSafeNormalize(const glm::dvec3& value, glm::dvec3& normalized)
		{
			if (!isFiniteVector(value))
			{
				return false;
			}

			const glm::dvec3 absoluteValue = glm::abs(value);
			const double largestComponent =
				std::max({absoluteValue.x, absoluteValue.y, absoluteValue.z});
			if (!(largestComponent > 0.0) || !std::isfinite(largestComponent))
			{
				return false;
			}

			const glm::dvec3 scaledValue = value / largestComponent;
			const double scaledLengthSq = glm::dot(scaledValue, scaledValue);
			if (!(scaledLengthSq > 0.0) || !std::isfinite(scaledLengthSq))
			{
				return false;
			}

			normalized = scaledValue / std::sqrt(scaledLengthSq);
			return isFiniteVector(normalized);
		}

		[[nodiscard]] bool isFiniteNonZeroVector(const glm::vec3& value)
		{
			if (!isFiniteVector(value))
			{
				return false;
			}

			const glm::vec3 absoluteValue = glm::abs(value);
			return std::max({absoluteValue.x, absoluteValue.y, absoluteValue.z}) > 0.0f;
		}

		[[nodiscard]] glm::vec3 safeNormalize(const glm::vec3& value, const glm::vec3& fallback)
		{
			if (!isFiniteNonZeroVector(value))
			{
				return fallback;
			}

			const glm::vec3 absoluteValue = glm::abs(value);
			const float largestComponent =
				std::max({absoluteValue.x, absoluteValue.y, absoluteValue.z});

			// Scale before taking the squared length so every finite, non-zero vector
			// is normalized without introducing an absolute-magnitude branch.
			const glm::vec3 scaledValue = value / largestComponent;
			const float scaledLengthSq = glm::dot(scaledValue, scaledValue);
			if (!std::isfinite(scaledLengthSq) || scaledLengthSq <= 0.0f)
			{
				return fallback;
			}

			const glm::vec3 normalized = scaledValue * glm::inversesqrt(scaledLengthSq);
			return isFiniteVector(normalized) ? normalized : fallback;
		}

		[[nodiscard]] double smootherStep01(double value)
		{
			const double t = std::clamp(value, 0.0, 1.0);
			return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
		}

		[[nodiscard]] glm::vec3 makeFallbackLightUp(const glm::vec3& normalizedDirection)
		{
			const glm::vec3 absoluteDirection = glm::abs(normalizedDirection);
			const glm::vec3 fallbackAxis =
				absoluteDirection.x <= absoluteDirection.y && absoluteDirection.x <= absoluteDirection.z
					? glm::vec3(1.0f, 0.0f, 0.0f)
					: absoluteDirection.y <= absoluteDirection.z
						? glm::vec3(0.0f, 1.0f, 0.0f)
						: glm::vec3(0.0f, 0.0f, 1.0f);
			glm::vec3 up = fallbackAxis;
			up -= normalizedDirection * glm::dot(up, normalizedDirection);
			const float upLengthSq = glm::dot(up, up);
			if (!isFiniteVector(up) || !std::isfinite(upLengthSq) || upLengthSq <= kLightBasisVectorEpsilon)
			{
				return glm::vec3(1.0f, 0.0f, 0.0f);
			}
			return safeNormalize(up, glm::vec3(1.0f, 0.0f, 0.0f));
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

		struct BoundingSphere
		{
			glm::vec3 center;
			float radius;
		};

		struct FrustumSlice
		{
			std::array<glm::vec3, 8> corners{};
			BoundingSphere boundingSphere{};
		};

		// Build the slice in view space from projection rays at the requested split depths.
		// This supports off-center projections without unprojecting the camera's distant far
		// plane and interpolating back toward the near plane, which amplified floating-point
		// error when imported cameras used a very large far distance.
		[[nodiscard]] FrustumSlice computeFrustumSlice(
			const shaderio::CameraUniforms& camera,
			const clipspace::ProjectionConvention& projectionConvention,
			float sliceNear,
			float sliceFar)
		{
			const glm::mat4 inverseView = glm::inverse(camera.view);
			// CSM fitting intentionally consumes the exact projection supplied by the caller.
			// GPUDrivenRenderer prepares cascades before applying temporal jitter and marks the
			// snapshot as ready so RenderDevice does not recompute it from the jittered camera.
			const glm::mat4& cascadeProjection = camera.projection;
			const float cameraNear = std::max(
				0.01f, std::abs(extractCameraNearPlane(cascadeProjection, projectionConvention)));
			const float cameraFar = std::max(cameraNear + 0.01f,
			                                 std::abs(extractCameraFarPlane(cascadeProjection, projectionConvention)));

			sliceNear = glm::clamp(sliceNear, cameraNear, cameraFar);
			sliceFar = glm::clamp(sliceFar, sliceNear + 0.01f, cameraFar);

			const glm::mat4 inverseProjection = glm::inverse(cascadeProjection);
			const bool orthographic = clipspace::isOrthographicProjection(cascadeProjection);
			const std::array<glm::vec2, 4> ndcCorners{
				glm::vec2(-1.0f, -1.0f),
				glm::vec2(1.0f, -1.0f),
				glm::vec2(1.0f, 1.0f),
				glm::vec2(-1.0f, 1.0f),
			};

			std::array<glm::vec3, 8> viewCorners{};
			for (size_t cornerIndex = 0; cornerIndex < ndcCorners.size(); ++cornerIndex)
			{
				glm::vec4 viewNear = inverseProjection *
					glm::vec4(ndcCorners[cornerIndex], projectionConvention.ndcNearZ, 1.0f);
				viewNear /= viewNear.w;

				if (orthographic)
				{
					viewCorners[cornerIndex] = glm::vec3(viewNear.x, viewNear.y, -sliceNear);
					viewCorners[cornerIndex + 4] = glm::vec3(viewNear.x, viewNear.y, -sliceFar);
				}
				else
				{
					const float rayDepth = std::max(-viewNear.z, 1.0e-6f);
					viewCorners[cornerIndex] = glm::vec3(viewNear) * (sliceNear / rayDepth);
					viewCorners[cornerIndex + 4] = glm::vec3(viewNear) * (sliceFar / rayDepth);
				}
			}

			glm::dvec3 centerView(0.0);
			for (const glm::vec3& corner : viewCorners)
			{
				centerView += glm::dvec3(corner);
			}
			centerView /= static_cast<double>(viewCorners.size());

			double radius = 0.0;
			for (const glm::vec3& corner : viewCorners)
			{
				radius = std::max(radius, glm::length(glm::dvec3(corner) - centerView));
			}

			FrustumSlice result{};
			for (size_t cornerIndex = 0; cornerIndex < viewCorners.size(); ++cornerIndex)
			{
				result.corners[cornerIndex] = glm::vec3(
					inverseView * glm::vec4(viewCorners[cornerIndex], 1.0f));
			}
			const glm::vec3 centerWorld = glm::vec3(
				inverseView * glm::vec4(glm::vec3(centerView), 1.0f));
			result.boundingSphere = BoundingSphere{
				centerWorld,
				std::max(static_cast<float>(radius), 0.01f),
			};
			return result;
		}

		[[nodiscard]] float computeCascadeBlendWidth(float cascadeNear, float cascadeFar)
		{
			if (shaderio::LCascadeBlendRegion <= 0.0f)
			{
				return 0.0f;
			}
			return std::max(
				(cascadeFar - cascadeNear) * shaderio::LCascadeBlendRegion,
				shaderio::LCascadeBlendMinDistance);
		}

		struct StableProjectionGrid
		{
			float texelSize{0.0f};
			float halfExtent{0.0f};
		};

		[[nodiscard]] StableProjectionGrid makeStableProjectionGrid(float receiverDiameter, uint32_t resolution)
		{
			const uint32_t safeResolution = std::max(resolution, 1u);
			const uint32_t maxGuardTexels = (safeResolution - 1u) / 2u;
			const uint32_t requestedGuardTexels = static_cast<uint32_t>(std::max(shaderio::LCascadePcfGuardTexels, 0));
			const uint32_t guardTexels = std::min(requestedGuardTexels, maxGuardTexels);
			const uint32_t receiverTexels = std::max(safeResolution - guardTexels * 2u, 1u);
			const float texelSize = receiverDiameter / static_cast<float>(receiverTexels);
			return StableProjectionGrid{
				.texelSize = texelSize,
				.halfExtent = texelSize * static_cast<float>(safeResolution) * 0.5f,
			};
		}

		[[nodiscard]] float snapCoordinateToTexelGrid(float coordinate, float texelSize)
		{
			if (texelSize <= 0.0f)
			{
				return coordinate;
			}

			const double texel = static_cast<double>(texelSize);
			return static_cast<float>(std::round(static_cast<double>(coordinate) / texel) * texel);
		}

		[[nodiscard]] glm::vec2 snapCenterToTexelGrid(const glm::vec2& center, float texelSize)
		{
			return glm::vec2(
				snapCoordinateToTexelGrid(center.x, texelSize),
				snapCoordinateToTexelGrid(center.y, texelSize));
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

	void CSMShadowResources::resetLightBasisHistory() noexcept
	{
		m_previousLightDirection = glm::vec3(0.0f, -1.0f, 0.0f);
		m_previousLightUp = glm::vec3(1.0f, 0.0f, 0.0f);
		m_previousLightBasisValid = false;
		m_lightBasisProjectionChartSelected = false;
	}

	glm::vec3 CSMShadowResources::resolveStableLightUp(const glm::vec3& normalizedDirection) noexcept
	{
		glm::vec3 resolvedUp = makeFallbackLightUp(normalizedDirection);
		const bool previousBasisValid =
			m_previousLightBasisValid
			&& isFiniteVector(m_previousLightDirection)
			&& isFiniteVector(m_previousLightUp)
			&& glm::dot(m_previousLightDirection, m_previousLightDirection) > kLightBasisVectorEpsilon
			&& glm::dot(m_previousLightUp, m_previousLightUp) > kLightBasisVectorEpsilon;

		if (previousBasisValid)
		{
			glm::dvec3 previousDirection;
			glm::dvec3 previousUp;
			glm::dvec3 direction;
			if (!tryScaleSafeNormalize(glm::dvec3(m_previousLightDirection), previousDirection)
				|| !tryScaleSafeNormalize(glm::dvec3(m_previousLightUp), previousUp)
				|| !tryScaleSafeNormalize(glm::dvec3(normalizedDirection), direction))
			{
				resetLightBasisHistory();
				return resolvedUp;
			}

			previousUp -= previousDirection * glm::dot(previousUp, previousDirection);
			if (!tryScaleSafeNormalize(previousUp, previousUp))
			{
				resetLightBasisHistory();
				return resolvedUp;
			}

			// Complementary quaternion transport charts, written without a quaternion
			// division. Squared-distance scales avoid cancellation in 1 +/- dot.
			// The forward chart is singular only at the antipode. The antipodal chart
			// first rotates pi around previousUp and is singular only at no direction
			// change.
			//
			// Near its pole the selected raw vector is quadratic in the direction chord,
			// so normalizing it amplifies float direction noise as O(1 / chord). A C2
			// annulus blends its relative roll toward the well-conditioned projection
			// of previousUp. The blend is performed in sign-aligned half-angle quaternion
			// coordinates: q and -q represent the same analytic rotation, so hemisphere
			// alignment avoids the 180-degree cancellation of a direct up-vector lerp.
			// The analytic chart is recovered exactly at the outer boundary. The inner
			// common orientation bounds float-representation ambiguity at the pole. The
			// analytic chart has non-zero roll winding around that pole while commonUp
			// has zero winding, so no non-vanishing tangent basis can join them over the
			// complete disk. The principal quaternion hemisphere confines the required
			// gauge cut to the open numerical annulus; smootherstep makes its amplitude
			// collapse continuously at both boundaries. The contract is therefore local
			// numerical continuity outside that bounded topological neighborhood, not an
			// impossible path-independent global pole map.
			const glm::dvec3 forwardDifference = previousDirection + direction;
			const glm::dvec3 antipodalDifference = direction - previousDirection;
			const double forwardScale = 0.5 * glm::dot(forwardDifference, forwardDifference);
			const double antipodalScale = 0.5 * glm::dot(antipodalDifference, antipodalDifference);
			const double previousUpDotDirection = glm::dot(previousUp, direction);
			const glm::dvec3 forwardTransportRaw =
				previousUp * forwardScale - forwardDifference * previousUpDotDirection;
			const glm::dvec3 antipodalTransportRaw =
				previousUp * antipodalScale - antipodalDifference * previousUpDotDirection;

			const bool selectedAntipodalChart = m_lightBasisProjectionChartSelected;
			const glm::dvec3& selectedRaw =
				selectedAntipodalChart ? antipodalTransportRaw : forwardTransportRaw;
			const glm::dvec3& alternateRaw =
				selectedAntipodalChart ? forwardTransportRaw : antipodalTransportRaw;
			const glm::dvec3& selectedPoleDifference =
				selectedAntipodalChart ? antipodalDifference : forwardDifference;
			const double selectedPoleChordLengthSq =
				glm::dot(selectedPoleDifference, selectedPoleDifference);

			glm::dvec3 analyticUp;
			bool analyticUpValid = tryScaleSafeNormalize(selectedRaw, analyticUp);
			if (analyticUpValid)
			{
				analyticUp -= direction * glm::dot(analyticUp, direction);
				analyticUpValid = tryScaleSafeNormalize(analyticUp, analyticUp);
			}

			glm::dvec3 transportedUp;
			if (selectedPoleChordLengthSq <= kLightBasisRegularizationInnerChordSq)
			{
				glm::dvec3 commonUp = previousUp - direction * glm::dot(previousUp, direction);
				if (tryScaleSafeNormalize(commonUp, commonUp))
				{
					transportedUp = commonUp;
				}
				else if (analyticUpValid)
				{
					transportedUp = analyticUp;
				}
				else if (!tryScaleSafeNormalize(alternateRaw, transportedUp))
				{
					resetLightBasisHistory();
					return resolvedUp;
				}
				m_lightBasisProjectionChartSelected = !selectedAntipodalChart;
			}
			else if (analyticUpValid
			         && selectedPoleChordLengthSq < kLightBasisRegularizationOuterChordSq)
			{
				glm::dvec3 commonUp = previousUp - direction * glm::dot(previousUp, direction);
				if (!tryScaleSafeNormalize(commonUp, commonUp))
				{
					commonUp = analyticUp;
				}

				glm::dvec3 commonRight = glm::cross(direction, commonUp);
				if (!tryScaleSafeNormalize(commonRight, commonRight))
				{
					resetLightBasisHistory();
					return resolvedUp;
				}

				const double innerChord = kLightBasisRegularizationInnerChord;
				const double outerChord = kLightBasisRegularizationOuterChord;
				const double selectedPoleChord = std::sqrt(selectedPoleChordLengthSq);
				const double analyticWeight = smootherStep01(
					(selectedPoleChord - innerChord) / (outerChord - innerChord));

				const double rollCosine = std::clamp(glm::dot(commonUp, analyticUp), -1.0, 1.0);
				const double rollSine = std::clamp(glm::dot(commonRight, analyticUp), -1.0, 1.0);
				const double analyticHalfCosine =
					std::sqrt(std::max(0.0, 0.5 * (1.0 + rollCosine)));
				const double analyticHalfSineMagnitude =
					std::sqrt(std::max(0.0, 0.5 * (1.0 - rollCosine)));
				const double analyticHalfSine =
					std::copysign(analyticHalfSineMagnitude, rollSine);

				// Nlerp the identity roll quaternion toward the analytic roll quaternion.
				// analyticHalfCosine is non-negative, so both endpoints share a hemisphere
				// and their weighted sum cannot suffer antipodal cancellation.
				double blendedHalfCosine =
					(1.0 - analyticWeight) + analyticWeight * analyticHalfCosine;
				double blendedHalfSine = analyticWeight * analyticHalfSine;
				const double blendedHalfLength =
					std::hypot(blendedHalfCosine, blendedHalfSine);
				if (!(blendedHalfLength > 0.0) || !std::isfinite(blendedHalfLength))
				{
					resetLightBasisHistory();
					return resolvedUp;
				}
				blendedHalfCosine /= blendedHalfLength;
				blendedHalfSine /= blendedHalfLength;

				const double blendedRollCosine =
					blendedHalfCosine * blendedHalfCosine
					- blendedHalfSine * blendedHalfSine;
				const double blendedRollSine =
					2.0 * blendedHalfCosine * blendedHalfSine;
				transportedUp =
					commonUp * blendedRollCosine + commonRight * blendedRollSine;
			}
			else if (analyticUpValid)
			{
				transportedUp = analyticUp;
			}
			else
			{
				if (!tryScaleSafeNormalize(alternateRaw, transportedUp))
				{
					resetLightBasisHistory();
					return resolvedUp;
				}
				m_lightBasisProjectionChartSelected = !selectedAntipodalChart;
			}

			transportedUp -= direction * glm::dot(transportedUp, direction);
			if (!tryScaleSafeNormalize(transportedUp, transportedUp))
			{
				resetLightBasisHistory();
				return resolvedUp;
			}
			resolvedUp = glm::vec3(transportedUp);
		}

		resolvedUp -= normalizedDirection * glm::dot(resolvedUp, normalizedDirection);
		resolvedUp = safeNormalize(resolvedUp, makeFallbackLightUp(normalizedDirection));
		m_previousLightDirection = normalizedDirection;
		m_previousLightUp = resolvedUp;
		m_previousLightBasisValid = true;
		return resolvedUp;
	}

	void CSMShadowResources::init(rhi::Device& device, rhi::CommandBuffer& cmd, const CreateInfo& createInfo)
	{
		resetLightBasisHistory();
		m_frameData = FrameData{};
		m_device = &device;
		assert(createInfo.cascadeCount > 0 && createInfo.cascadeCount <= shaderio::LCascadeCount
			&& "Cascade count must be within the shader contract");
		m_cascadeCount = std::clamp(
			createInfo.cascadeCount, 1u, static_cast<uint32_t>(shaderio::LCascadeCount));
		m_cascadeResolution = clampCascadeResolution(createInfo.cascadeResolution);
		assert(m_cascadeResolution >= getMinimumCascadeResolution()
			&& "Cascade resolution must preserve the full PCF guard");
		m_shadowFormat = createInfo.shadowFormat;
		m_projectionConvention = createInfo.projectionConvention;

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

		// Initialize shadow uniforms with defaults
		m_shadowUniformsData.lightDirectionAndIntensity = glm::vec4(0.0f, -1.0f, 0.0f, kShadowIntensity);
		m_shadowUniformsData.shadowMapMetrics = glm::vec4(
			1.0f / static_cast<float>(m_cascadeResolution),
			kDefaultMaxShadowDistance,
			0.0f,
			static_cast<float>(m_cascadeCount));
		m_shadowUniformsData.cascadeSplitDistances.invDepthRange = glm::vec4(0.0f);
		m_shadowUniformsData.cascadeSplitDistances.worldTexelSize = glm::vec4(0.0f);

		const rhi::TextureBarrier initBarrier{
			.texture = m_cascadeArray,
			.before = rhi::ResourceState::Undefined,
			.after = rhi::ResourceState::General,
			.range = {.aspect = rhi::TextureAspect::depth, .levelCount = 1, .layerCount = m_cascadeCount},
		};
		cmd.resourceBarrier(std::span{&initBarrier, 1}, {});
	}

	void CSMShadowResources::deinit()
	{
		resetLightBasisHistory();
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
	                                               bool casterBoundsValid,
	                                               float requestedNormalBiasWorld)
	{
		const float cameraFar = std::max(1.0f, extractCameraFarPlane(camera.projection, m_projectionConvention));
		const float cameraNear = std::max(0.01f, extractCameraNearPlane(camera.projection, m_projectionConvention));
		const float requestedShadowDistance = requestedMaxShadowDistance > 0.0f
			                                      ? requestedMaxShadowDistance
			                                      : kDefaultMaxShadowDistance;
		const float maxShadowDistance = glm::clamp(requestedShadowDistance, cameraNear + 0.01f, cameraFar);
		const bool lightDirectionInputValid = isFiniteNonZeroVector(lightDir);
		if (!lightDirectionInputValid)
		{
			resetLightBasisHistory();
		}
		const glm::vec3 lightDirection = lightDirectionInputValid
			                                 ? safeNormalize(lightDir, glm::vec3(0.0f, -1.0f, 0.0f))
			                                 : glm::vec3(0.0f, -1.0f, 0.0f);
		const float normalBiasWorld =
			std::isfinite(requestedNormalBiasWorld) ? std::max(requestedNormalBiasWorld, 0.0f) : 0.0f;
		const bool useCasterBounds = casterBoundsValid && isValidBounds(casterBoundsMin, casterBoundsMax);
		const std::array<glm::vec3, 8> casterCorners =
			useCasterBounds ? computeAabbCorners(casterBoundsMin, casterBoundsMax) : std::array<glm::vec3, 8>{};

		m_frameData = FrameData{};
		m_frameData.cascadeCount = m_cascadeCount;
		m_frameData.lightDirection = lightDirection;
		m_frameData.maxShadowDistance = maxShadowDistance;
		m_frameData.normalBiasWorld = normalBiasWorld;
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
		m_shadowUniformsData.cascadeSplitDistances.invDepthRange = glm::vec4(0.0f);
		m_shadowUniformsData.cascadeSplitDistances.worldTexelSize = glm::vec4(0.0f);
		m_frameData.splitDistances = m_shadowUniformsData.cascadeSplitDistances;

		const glm::vec3 lightUp = resolveStableLightUp(lightDirection);
		const glm::mat4 lightRotationView = glm::lookAt(glm::vec3(0.0f), lightDirection, lightUp);
		float stableCasterMinZ = std::numeric_limits<float>::max();
		float stableCasterMaxZ = std::numeric_limits<float>::lowest();
		if (useCasterBounds)
		{
			for (const glm::vec3& corner : casterCorners)
			{
				const glm::vec3 lightSpaceCorner = glm::vec3(lightRotationView * glm::vec4(corner, 1.0f));
				stableCasterMinZ = std::min(stableCasterMinZ, lightSpaceCorner.z);
				stableCasterMaxZ = std::max(stableCasterMaxZ, lightSpaceCorner.z);
			}
		}

		// Compute each cascade's view-projection matrix
		float prevSplitDistance = cameraNear;
		for (uint32_t cascadeIndex = 0; cascadeIndex < m_cascadeCount; ++cascadeIndex)
		{
			const float splitDistance = cascadeSplits[cascadeIndex];
			CascadeData& cascadeData = m_frameData.cascades[cascadeIndex];
			cascadeData.splitNear = prevSplitDistance;
			cascadeData.splitFar = splitDistance;

			// Get frustum corners and a rotation-independent bounding sphere for this slice.
			float receiverSliceNear = prevSplitDistance;
			if (cascadeIndex > 0)
			{
				const float previousCascadeNear = cascadeIndex > 1 ? cascadeSplits[cascadeIndex - 2] : 0.0f;
				const float blendWidth = computeCascadeBlendWidth(previousCascadeNear, prevSplitDistance);
				receiverSliceNear = std::max(cameraNear, prevSplitDistance - blendWidth);
			}
			const FrustumSlice frustumSlice =
				computeFrustumSlice(camera, m_projectionConvention, receiverSliceNear, splitDistance);
			const std::array<glm::vec3, 8>& sliceCorners = frustumSlice.corners;
			cascadeData.receiverCornersWorld = sliceCorners;

			const BoundingSphere& boundingSphere = frustumSlice.boundingSphere;
			cascadeData.receiverCenter = boundingSphere.center;
			cascadeData.receiverRadius = boundingSphere.radius;

			const float diameter = boundingSphere.radius * 2.0f;
			const StableProjectionGrid projectionGrid =
				makeStableProjectionGrid(diameter, m_cascadeResolution);

			// Position the cascade on a stable light-space texel grid. This keeps static shadows
			// from swimming when the camera moves by sub-texel amounts.
			glm::vec3 stableCenterLightSpace = glm::vec3(lightRotationView * glm::vec4(boundingSphere.center, 1.0f));
			const glm::vec2 snappedCenterXY =
				snapCenterToTexelGrid(glm::vec2(stableCenterLightSpace), projectionGrid.texelSize);
			stableCenterLightSpace.x = snappedCenterXY.x;
			stableCenterLightSpace.y = snappedCenterXY.y;
			const float nearPlane = kCascadeStableNearPlane;
			const float stableDepthSpan = useCasterBounds
				                              ? std::max(0.0f, stableCasterMaxZ - stableCasterMinZ)
				                              : boundingSphere.radius * 2.0f;
			const float depthRange = std::max(1.0f, stableDepthSpan + kCascadeDepthPadding * 2.0f);
			const float farPlane = nearPlane + depthRange;
			const float depthAnchorMax = useCasterBounds
				                             ? stableCasterMaxZ
				                             : stableCenterLightSpace.z + boundingSphere.radius;
			const float lightViewZ = -(nearPlane + kCascadeDepthPadding) - depthAnchorMax;
			// Compose the light-view translation in light space so the snapped center maps
			// exactly to (0, 0), while Z is anchored only to static caster bounds (or the
			// receiver sphere fallback). Camera motion therefore cannot change depth span.
			glm::mat4 lightView = glm::translate(
				glm::mat4(1.0f),
				glm::vec3(
					-stableCenterLightSpace.x,
					-stableCenterLightSpace.y,
					lightViewZ)) * lightRotationView;

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

			const glm::vec3 lightPosition = glm::vec3(glm::inverse(lightView)[3]);
			cascadeData.lightPosition = lightPosition;
			cascadeData.lightView = lightView;
			cascadeData.receiverMinLightSpace = minLightSpace;
			cascadeData.receiverMaxLightSpace = maxLightSpace;
			cascadeData.casterMinLightSpace = casterMinLightSpace;
			cascadeData.casterMaxLightSpace = casterMaxLightSpace;

			// The PCF guard and the receiver fit were resolved before snapping, so these
			// bounds use one exact texel size on both axes. There is deliberately no second
			// floor/ceil pass here: that was the source of the 1024/1025-texel projection jump.
			const float texelSize = projectionGrid.texelSize;
			const float halfSize = projectionGrid.halfExtent;
			const float left = -halfSize;
			const float right = halfSize;
			const float bottom = -halfSize;
			const float top = halfSize;

			// Extruding a directional-light caster toward the light changes only light-space Z.
			// Keep XY culling tied to this cascade's stable receiver projection instead of
			// expanding every cascade to the full-scene caster AABB.
			const float cullingGuard = std::max({
				texelSize * kCascadeCasterMinGuardTexels,
				kCascadeCasterMinGuardWorld,
				normalBiasWorld,
			});
			const float cullLeft = left - cullingGuard;
			const float cullRight = right + cullingGuard;
			const float cullBottom = bottom - cullingGuard;
			const float cullTop = top + cullingGuard;

			// near/far use a fixed world-space span: static caster bounds when available,
			// otherwise the rotation-stable receiver sphere. Receiver motion cannot rescale Z.
			cascadeData.nearPlane = nearPlane;
			cascadeData.farPlane = farPlane;
			cascadeData.depthRange = depthRange;
			cascadeData.invDepthRange = 1.0f / depthRange;
			cascadeData.texelSize = texelSize;
			cascadeData.cullingGuardWorld = cullingGuard;
			m_shadowUniformsData.cascadeSplitDistances.invDepthRange[cascadeIndex] = cascadeData.invDepthRange;
			m_shadowUniformsData.cascadeSplitDistances.worldTexelSize[cascadeIndex] = texelSize;

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

		// ShadowUniforms remains a pure CPU snapshot for the active LightParams path.
	}
} // namespace demo
