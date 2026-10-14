#include <mgif/scene_editor/AnimationPlayback.h>

#include <mgif/scene_editor/GltfDocument.h>

#include <tiny_gltf.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace demo::editor {

namespace detail {

// GltfDocument intentionally keeps TinyGLTF behind its private authority
// methods.  This narrow friend bridge is local to this translation unit; the
// public adapter still exposes only MGIF-owned typed views.
class GltfDocumentAccess final {
public:
  [[nodiscard]] static const tinygltf::Model& model(
      const GltfDocument& document) noexcept {
    return document.authorityModel();
  }
};

} // namespace detail

struct GltfAnimationAdapter::Impl final {
  static constexpr std::size_t kInvalidPayload =
      std::numeric_limits<std::size_t>::max();

  std::size_t animationIndex{std::numeric_limits<std::size_t>::max()};
  AnimationSampleStatus status{AnimationSampleStatus::invalidAdapter};
  std::vector<AnimationSamplerView> samplers;
  std::vector<AnimationChannelView> channels;
  std::vector<std::vector<float>> payloads;
  std::vector<std::size_t> payloadSlots;
  std::vector<AnimationAccessorView> accessors;
  AnimationClipView clip{};

  void rebuildViews() noexcept {
    for (std::size_t index = 0; index < accessors.size(); ++index) {
      const std::size_t slot = payloadSlots[index];
      if (slot == kInvalidPayload || slot >= payloads.size()) {
        accessors[index] = AnimationAccessorView{};
        continue;
      }
      accessors[index].values = payloads[slot];
    }
    clip.samplers = samplers;
    clip.channels = channels;
  }
};

namespace {

constexpr double kQuaternionEpsilon = 1.0e-12;

[[nodiscard]] bool checkedMultiply(std::uint64_t left, std::uint64_t right,
                                   std::uint64_t& result) noexcept;
[[nodiscard]] bool checkedAdd(std::uint64_t left, std::uint64_t right,
                              std::uint64_t& result) noexcept;

[[nodiscard]] AnimationAccessorShape gltfAccessorShape(int type) noexcept {
  switch (type) {
  case TINYGLTF_TYPE_SCALAR:
    return AnimationAccessorShape::scalar;
  case TINYGLTF_TYPE_VEC3:
    return AnimationAccessorShape::vec3;
  case TINYGLTF_TYPE_VEC4:
    return AnimationAccessorShape::vec4;
  default:
    return AnimationAccessorShape::invalid;
  }
}

[[nodiscard]] AnimationInterpolation gltfInterpolation(
    const std::string& interpolation) noexcept {
  if (interpolation.empty() || interpolation == "LINEAR")
    return AnimationInterpolation::linear;
  if (interpolation == "STEP")
    return AnimationInterpolation::step;
  if (interpolation == "CUBICSPLINE")
    return AnimationInterpolation::cubicSpline;
  return AnimationInterpolation::invalid;
}

[[nodiscard]] AnimationTargetPath gltfTargetPath(
    const std::string& path) noexcept {
  if (path == "translation")
    return AnimationTargetPath::translation;
  if (path == "rotation")
    return AnimationTargetPath::rotation;
  if (path == "scale")
    return AnimationTargetPath::scale;
  return AnimationTargetPath::invalid;
}

[[nodiscard]] int unsignedComponentBytes(int componentType) noexcept {
  switch (componentType) {
  case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
    return 1;
  case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
    return 2;
  case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
    return 4;
  default:
    return 0;
  }
}

[[nodiscard]] bool readViewBits(const tinygltf::Model& model, int viewIndex,
                                std::uint64_t relativeOffset,
                                std::size_t byteCount,
                                std::uint32_t& bits) noexcept {
  if (viewIndex < 0
      || static_cast<std::size_t>(viewIndex) >= model.bufferViews.size()
      || byteCount == 0 || byteCount > sizeof(bits))
    return false;
  const tinygltf::BufferView& view =
      model.bufferViews[static_cast<std::size_t>(viewIndex)];
  if (view.buffer < 0
      || static_cast<std::size_t>(view.buffer) >= model.buffers.size()
      || relativeOffset > view.byteLength
      || byteCount > static_cast<std::uint64_t>(view.byteLength)
                         - relativeOffset)
    return false;
  std::uint64_t absoluteOffset = 0;
  if (!checkedAdd(static_cast<std::uint64_t>(view.byteOffset), relativeOffset,
                  absoluteOffset))
    return false;
  const std::vector<unsigned char>& data =
      model.buffers[static_cast<std::size_t>(view.buffer)].data;
  if (absoluteOffset > data.size()
      || byteCount > static_cast<std::uint64_t>(data.size()) - absoluteOffset)
    return false;
  bits = 0;
  const std::size_t offset = static_cast<std::size_t>(absoluteOffset);
  for (std::size_t byte = 0; byte < byteCount; ++byte)
    bits |= static_cast<std::uint32_t>(data[offset + byte]) << (byte * 8u);
  return true;
}

[[nodiscard]] bool readFloatFromView(const tinygltf::Model& model,
                                     int viewIndex,
                                     std::uint64_t relativeOffset,
                                     float& value) noexcept {
  if (viewIndex < 0
      || static_cast<std::size_t>(viewIndex) >= model.bufferViews.size())
    return false;
  const tinygltf::BufferView& view =
      model.bufferViews[static_cast<std::size_t>(viewIndex)];
  std::uint64_t absoluteOffset = 0;
  if (!checkedAdd(static_cast<std::uint64_t>(view.byteOffset), relativeOffset,
                  absoluteOffset)
      || (absoluteOffset % alignof(float)) != 0)
    return false;
  std::uint32_t bits = 0;
  if (!readViewBits(model, viewIndex, relativeOffset, sizeof(float), bits))
    return false;
  value = std::bit_cast<float>(bits);
  return std::isfinite(value);
}

[[nodiscard]] bool readSparseIndex(const tinygltf::Model& model,
                                   const tinygltf::Accessor& accessor,
                                   std::uint64_t ordinal,
                                   std::uint64_t& value) noexcept {
  const int byteCount =
      unsignedComponentBytes(accessor.sparse.indices.componentType);
  if (byteCount == 0)
    return false;
  std::uint64_t ordinalOffset = 0;
  std::uint64_t relativeOffset = 0;
  if (!checkedMultiply(ordinal, static_cast<std::uint64_t>(byteCount),
                       ordinalOffset)
      || !checkedAdd(
          static_cast<std::uint64_t>(accessor.sparse.indices.byteOffset),
          ordinalOffset, relativeOffset))
    return false;
  if (accessor.sparse.indices.bufferView < 0
      || static_cast<std::size_t>(accessor.sparse.indices.bufferView)
          >= model.bufferViews.size())
    return false;
  const tinygltf::BufferView& view = model.bufferViews[
      static_cast<std::size_t>(accessor.sparse.indices.bufferView)];
  std::uint64_t absoluteOffset = 0;
  if (!checkedAdd(static_cast<std::uint64_t>(view.byteOffset), relativeOffset,
                  absoluteOffset)
      || (absoluteOffset % static_cast<std::uint64_t>(byteCount)) != 0)
    return false;
  std::uint32_t bits = 0;
  if (!readViewBits(model, accessor.sparse.indices.bufferView, relativeOffset,
                    static_cast<std::size_t>(byteCount), bits))
    return false;
  value = bits;
  return true;
}

[[nodiscard]] AnimationSampleStatus decodeGltfAccessor(
    const tinygltf::Model& model, int accessorIndex,
    const AnimationSamplingLimits& limits, std::uint64_t& totalValues,
    AnimationAccessorView& metadata, std::vector<float>& decoded) {
  if (accessorIndex < 0
      || static_cast<std::size_t>(accessorIndex) >= model.accessors.size()
      || static_cast<std::size_t>(accessorIndex) >= limits.maxAccessors)
    return AnimationSampleStatus::invalidAccessorIndex;
  const tinygltf::Accessor& accessor =
      model.accessors[static_cast<std::size_t>(accessorIndex)];
  const AnimationAccessorShape shape = gltfAccessorShape(accessor.type);
  const std::size_t components = shape == AnimationAccessorShape::scalar
      ? 1u
      : (shape == AnimationAccessorShape::vec3
             ? 3u
             : (shape == AnimationAccessorShape::vec4 ? 4u : 0u));
  if (shape == AnimationAccessorShape::invalid
      || accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT
      || accessor.normalized)
    return AnimationSampleStatus::invalidAccessorType;
  if (accessor.bufferView < -1
      || (accessor.bufferView == -1 && accessor.byteOffset != 0))
    return AnimationSampleStatus::invalidAccessorPayload;
  if (accessor.count == 0 || accessor.count > limits.maxKeyframes)
    return accessor.count == 0 ? AnimationSampleStatus::invalidAccessorCount
                               : AnimationSampleStatus::limitExceeded;
  std::uint64_t valueCount = 0;
  if (!checkedMultiply(static_cast<std::uint64_t>(accessor.count),
                       static_cast<std::uint64_t>(components), valueCount)
      || valueCount > std::numeric_limits<std::size_t>::max()
      || !checkedAdd(totalValues, valueCount, totalValues)
      || totalValues > limits.maxValues)
    return AnimationSampleStatus::limitExceeded;

  decoded.assign(static_cast<std::size_t>(valueCount), 0.0F);
  const std::uint64_t elementBytes =
      static_cast<std::uint64_t>(components) * sizeof(float);
  if (accessor.bufferView >= 0) {
    if (static_cast<std::size_t>(accessor.bufferView) >= model.bufferViews.size())
      return AnimationSampleStatus::invalidAccessor;
    const tinygltf::BufferView& view =
        model.bufferViews[static_cast<std::size_t>(accessor.bufferView)];
    const std::uint64_t stride = view.byteStride == 0
        ? elementBytes
        : static_cast<std::uint64_t>(view.byteStride);
    if (stride < elementBytes || (stride % sizeof(float)) != 0)
      return AnimationSampleStatus::invalidAccessorPayload;
    for (std::uint64_t element = 0; element < accessor.count; ++element) {
      std::uint64_t elementOffset = 0;
      std::uint64_t relativeOffset = 0;
      if (!checkedMultiply(element, stride, elementOffset)
          || !checkedAdd(static_cast<std::uint64_t>(accessor.byteOffset),
                         elementOffset, relativeOffset))
        return AnimationSampleStatus::invalidAccessorPayload;
      for (std::size_t component = 0; component < components; ++component) {
        std::uint64_t componentOffset = 0;
        if (!checkedAdd(relativeOffset, component * sizeof(float),
                        componentOffset)
            || !readFloatFromView(
                model, accessor.bufferView, componentOffset,
                decoded[static_cast<std::size_t>(element) * components
                        + component]))
          return AnimationSampleStatus::invalidAccessorPayload;
      }
    }
  } else if (!accessor.sparse.isSparse) {
    return AnimationSampleStatus::invalidAccessor;
  }

  if (accessor.sparse.isSparse) {
    if (accessor.sparse.count <= 0
        || static_cast<std::uint64_t>(accessor.sparse.count) > accessor.count
        || accessor.sparse.values.bufferView < 0
        || static_cast<std::size_t>(accessor.sparse.values.bufferView)
            >= model.bufferViews.size())
      return AnimationSampleStatus::invalidAccessorPayload;
    const tinygltf::BufferView& valuesView = model.bufferViews[
        static_cast<std::size_t>(accessor.sparse.values.bufferView)];
    if (valuesView.byteStride != 0)
      return AnimationSampleStatus::invalidAccessorPayload;
    if (accessor.sparse.indices.bufferView < 0
        || static_cast<std::size_t>(accessor.sparse.indices.bufferView)
            >= model.bufferViews.size()
        || model.bufferViews[static_cast<std::size_t>(
               accessor.sparse.indices.bufferView)].byteStride != 0)
      return AnimationSampleStatus::invalidAccessorPayload;

    std::uint64_t previous = 0;
    bool havePrevious = false;
    for (std::uint64_t ordinal = 0;
         ordinal < static_cast<std::uint64_t>(accessor.sparse.count);
         ++ordinal) {
      std::uint64_t target = 0;
      if (!readSparseIndex(model, accessor, ordinal, target)
          || target >= accessor.count
          || (havePrevious && target <= previous))
        return AnimationSampleStatus::invalidAccessorPayload;
      previous = target;
      havePrevious = true;
      std::uint64_t elementOffset = 0;
      std::uint64_t relativeOffset = 0;
      if (!checkedMultiply(ordinal, elementBytes, elementOffset)
          || !checkedAdd(
              static_cast<std::uint64_t>(accessor.sparse.values.byteOffset),
              elementOffset, relativeOffset))
        return AnimationSampleStatus::invalidAccessorPayload;
      for (std::size_t component = 0; component < components; ++component) {
        std::uint64_t componentOffset = 0;
        if (!checkedAdd(relativeOffset, component * sizeof(float),
                        componentOffset)
            || !readFloatFromView(
                model, accessor.sparse.values.bufferView, componentOffset,
                decoded[static_cast<std::size_t>(target) * components
                        + component]))
          return AnimationSampleStatus::invalidAccessorPayload;
      }
    }
  }

  metadata = AnimationAccessorView{
      shape, AnimationAccessorComponentType::float32, accessor.count, {}};
  return AnimationSampleStatus::success;
}

template <typename AdapterStorage>
[[nodiscard]] AnimationSampleStatus buildGltfAnimationAdapter(
    const GltfDocument& document, std::size_t animationIndex,
    const AnimationSamplingLimits& limits, AdapterStorage& result) {
  const tinygltf::Model& model = detail::GltfDocumentAccess::model(document);
  if (animationIndex >= model.animations.size())
    return AnimationSampleStatus::invalidAnimationIndex;
  if (model.accessors.size() > limits.maxAccessors
      || model.accessors.size()
          > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
    return AnimationSampleStatus::limitExceeded;
  const tinygltf::Animation& animation = model.animations[animationIndex];
  if (animation.samplers.empty() || animation.channels.empty())
    return AnimationSampleStatus::invalidClip;
  if (animation.samplers.size() > limits.maxSamplers
      || animation.channels.size() > limits.maxChannels
      || model.nodes.size() > limits.maxNodeCount
      || animation.samplers.size()
          > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())
      || model.nodes.size()
          > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
    return AnimationSampleStatus::limitExceeded;

  result.animationIndex = animationIndex;
  result.samplers.resize(animation.samplers.size());
  result.channels.resize(animation.channels.size());
  result.accessors.resize(model.accessors.size());
  result.payloadSlots.assign(model.accessors.size(),
                             AdapterStorage::kInvalidPayload);

  std::vector<bool> needed(model.accessors.size(), false);
  for (std::size_t samplerIndex = 0;
       samplerIndex < animation.samplers.size(); ++samplerIndex) {
    const tinygltf::AnimationSampler& sampler = animation.samplers[samplerIndex];
    const AnimationInterpolation interpolation =
        gltfInterpolation(sampler.interpolation);
    if (sampler.input < 0 || sampler.output < 0
        || static_cast<std::size_t>(sampler.input) >= model.accessors.size()
        || static_cast<std::size_t>(sampler.output) >= model.accessors.size())
      return AnimationSampleStatus::invalidAccessorIndex;
    if (interpolation == AnimationInterpolation::invalid)
      return AnimationSampleStatus::invalidInterpolation;
    if (interpolation == AnimationInterpolation::cubicSpline)
      return AnimationSampleStatus::unsupportedCubicSpline;
    result.samplers[samplerIndex] = AnimationSamplerView{
        sampler.input, sampler.output, interpolation};
    needed[static_cast<std::size_t>(sampler.input)] = true;
    needed[static_cast<std::size_t>(sampler.output)] = true;
  }

  for (std::size_t channelIndex = 0;
       channelIndex < animation.channels.size(); ++channelIndex) {
    const tinygltf::AnimationChannel& channel = animation.channels[channelIndex];
    if (channel.target_node < 0
        || static_cast<std::size_t>(channel.target_node) >= model.nodes.size()
        || channel.sampler < 0
        || static_cast<std::size_t>(channel.sampler) >= animation.samplers.size())
      return AnimationSampleStatus::invalidChannel;
    const AnimationTargetPath path = gltfTargetPath(channel.target_path);
    if (path == AnimationTargetPath::invalid)
      return AnimationSampleStatus::invalidChannel;
    result.channels[channelIndex] = AnimationChannelView{
        static_cast<std::uint32_t>(channel.target_node), channel.sampler, path};
  }

  std::uint64_t totalValues = 0;
  for (std::size_t accessorIndex = 0; accessorIndex < needed.size();
       ++accessorIndex) {
    if (!needed[accessorIndex])
      continue;
    std::vector<float>& payload = result.payloads.emplace_back();
    AnimationAccessorView metadata{};
    const AnimationSampleStatus status = decodeGltfAccessor(
        model, static_cast<int>(accessorIndex), limits, totalValues, metadata,
        payload);
    if (status != AnimationSampleStatus::success)
      return status;
    metadata.values = payload;
    result.payloadSlots[accessorIndex] = result.payloads.size() - 1u;
    result.accessors[accessorIndex] = metadata;
  }
  result.clip.nodeCount = model.nodes.size();
  result.rebuildViews();
  return AnimationSampleStatus::success;
}

struct PreparedSampler final {
  AnimationAccessorView input{};
  AnimationAccessorView output{};
  AnimationInterpolation interpolation{AnimationInterpolation::invalid};
};

struct PreparedChannel final {
  std::uint32_t nodeIndex{0};
  std::size_t samplerIndex{0};
  AnimationTargetPath path{AnimationTargetPath::invalid};
};

struct PreparedClip final {
  AnimationClipInfo info{};
  std::vector<PreparedSampler> samplers;
  std::vector<PreparedChannel> channels;
};

[[nodiscard]] bool finite(double value) noexcept {
  return std::isfinite(value);
}

[[nodiscard]] bool checkedMultiply(std::uint64_t left, std::uint64_t right,
                                   std::uint64_t& result) noexcept {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
    return false;
  result = left * right;
  return true;
}

[[nodiscard]] bool checkedAdd(std::uint64_t left, std::uint64_t right,
                              std::uint64_t& result) noexcept {
  if (right > std::numeric_limits<std::uint64_t>::max() - left)
    return false;
  result = left + right;
  return true;
}

[[nodiscard]] bool validInterpolation(AnimationInterpolation interpolation) noexcept {
  return interpolation == AnimationInterpolation::linear
      || interpolation == AnimationInterpolation::step
      || interpolation == AnimationInterpolation::cubicSpline;
}

[[nodiscard]] AnimationAccessorShape shapeForPath(AnimationTargetPath path) noexcept {
  switch (path) {
  case AnimationTargetPath::translation:
  case AnimationTargetPath::scale:
    return AnimationAccessorShape::vec3;
  case AnimationTargetPath::rotation:
    return AnimationAccessorShape::vec4;
  case AnimationTargetPath::invalid:
    return AnimationAccessorShape::invalid;
  }
  return AnimationAccessorShape::invalid;
}

[[nodiscard]] bool validPath(AnimationTargetPath path) noexcept {
  return path == AnimationTargetPath::translation
      || path == AnimationTargetPath::rotation
      || path == AnimationTargetPath::scale;
}

[[nodiscard]] AnimationSampleStatus validateAccessor(
    const AnimationAccessorView& accessor,
    const AnimationSamplingLimits& limits,
    std::uint64_t& valueBudget) noexcept {
  if (accessor.componentType != AnimationAccessorComponentType::float32
      || accessor.componentCount() == 0) {
    return AnimationSampleStatus::invalidAccessorType;
  }
  if (accessor.count == 0) {
    return AnimationSampleStatus::invalidAccessorCount;
  }
  if (accessor.count > limits.maxKeyframes) {
    return AnimationSampleStatus::limitExceeded;
  }

  std::uint64_t expectedValues = 0;
  if (!checkedMultiply(accessor.count,
                       static_cast<std::uint64_t>(accessor.componentCount()),
                       expectedValues)
      || expectedValues > std::numeric_limits<std::size_t>::max()
      || accessor.values.size() != static_cast<std::size_t>(expectedValues)) {
    return AnimationSampleStatus::invalidAccessorPayload;
  }
  if (expectedValues > limits.maxValues) {
    return AnimationSampleStatus::limitExceeded;
  }
  if (!checkedAdd(valueBudget, expectedValues, valueBudget)
      || valueBudget > limits.maxValues) {
    return AnimationSampleStatus::limitExceeded;
  }
  for (const float value : accessor.values) {
    if (!std::isfinite(value)) {
      return AnimationSampleStatus::invalidAccessorPayload;
    }
  }
  return AnimationSampleStatus::success;
}

[[nodiscard]] AnimationSampleStatus validateInput(
    const AnimationAccessorView& input,
    const AnimationSamplingLimits& limits,
    std::uint64_t& valueBudget, double& start, double& end) noexcept {
  if (input.shape != AnimationAccessorShape::scalar) {
    return AnimationSampleStatus::invalidAccessorType;
  }
  const AnimationSampleStatus status =
      validateAccessor(input, limits, valueBudget);
  if (status != AnimationSampleStatus::success)
    return status;

  start = static_cast<double>(input.values.front());
  end = start;
  if (start < 0.0)
    return AnimationSampleStatus::invalidAccessorPayload;
  for (std::size_t index = 0; index < input.values.size(); ++index) {
    const double value = static_cast<double>(input.values[index]);
    if (!finite(value))
      return AnimationSampleStatus::invalidAccessorPayload;
    if (index != 0 && !(value > end)) {
      return AnimationSampleStatus::invalidAccessorPayload;
    }
    end = value;
  }
  return AnimationSampleStatus::success;
}

[[nodiscard]] AnimationSampleStatus validateOutput(
    const AnimationAccessorView& output, AnimationTargetPath path,
    std::uint64_t expectedCount) noexcept {
  if (output.shape != shapeForPath(path))
    return AnimationSampleStatus::invalidAccessorType;
  if (output.count != expectedCount)
    return AnimationSampleStatus::invalidAccessorCount;

  if (path == AnimationTargetPath::rotation) {
    for (std::size_t index = 0; index < output.values.size(); index += 4) {
      const double x = static_cast<double>(output.values[index + 0]);
      const double y = static_cast<double>(output.values[index + 1]);
      const double z = static_cast<double>(output.values[index + 2]);
      const double w = static_cast<double>(output.values[index + 3]);
      const double norm = std::sqrt(x * x + y * y + z * z + w * w);
      if (!finite(norm) || norm <= kQuaternionEpsilon)
        return AnimationSampleStatus::invalidAccessorPayload;
    }
  }
  return AnimationSampleStatus::success;
}

[[nodiscard]] AnimationSampleStatus prepareClip(
    const AnimationClipView& clip, const AnimationAccessorAdapter& adapter,
    const AnimationSamplingLimits& limits, PreparedClip& prepared) {
  if (limits.maxAccessors == 0 || limits.maxSamplers == 0
      || limits.maxChannels == 0 || limits.maxKeyframes == 0
      || limits.maxValues == 0 || limits.maxNodeCount == 0) {
    return AnimationSampleStatus::limitExceeded;
  }
  if (clip.samplers.empty() || clip.channels.empty())
    return AnimationSampleStatus::invalidClip;
  if (clip.samplers.size() > limits.maxSamplers
      || clip.channels.size() > limits.maxChannels
      || (clip.nodeCount != 0 && clip.nodeCount > limits.maxNodeCount)) {
    return AnimationSampleStatus::limitExceeded;
  }

  const std::size_t adapterCount = adapter.accessorCount();
  if (adapterCount > limits.maxAccessors)
    return AnimationSampleStatus::limitExceeded;

  prepared.info.samplerCount = clip.samplers.size();
  prepared.info.channelCount = clip.channels.size();
  prepared.samplers.resize(clip.samplers.size());
  prepared.channels.reserve(clip.channels.size());

  std::uint64_t valueBudget = 0;
  bool haveRange = false;
  for (std::size_t samplerIndex = 0; samplerIndex < clip.samplers.size();
       ++samplerIndex) {
    const AnimationSamplerView& source = clip.samplers[samplerIndex];
    if (source.inputAccessor < 0 || source.outputAccessor < 0)
      return AnimationSampleStatus::invalidAccessorIndex;
    const std::size_t inputIndex = static_cast<std::size_t>(source.inputAccessor);
    const std::size_t outputIndex = static_cast<std::size_t>(source.outputAccessor);
    if ((adapterCount != 0 && (inputIndex >= adapterCount || outputIndex >= adapterCount))
        || inputIndex >= limits.maxAccessors || outputIndex >= limits.maxAccessors) {
      return AnimationSampleStatus::invalidAccessorIndex;
    }
    if (!validInterpolation(source.interpolation))
      return AnimationSampleStatus::invalidInterpolation;
    if (source.interpolation == AnimationInterpolation::cubicSpline)
      return AnimationSampleStatus::unsupportedCubicSpline;

    const std::optional<AnimationAccessorView> input =
        adapter.readAccessor(inputIndex);
    const std::optional<AnimationAccessorView> output =
        adapter.readAccessor(outputIndex);
    if (!input || !output)
      return AnimationSampleStatus::invalidAccessor;

    double start = 0.0;
    double end = 0.0;
    const AnimationSampleStatus inputStatus =
        validateInput(*input, limits, valueBudget, start, end);
    if (inputStatus != AnimationSampleStatus::success)
      return inputStatus;
    // Output shape is channel-dependent, so only the common typed payload
    // checks happen here.  Each channel validates the expected shape/count.
    if (output->componentType != AnimationAccessorComponentType::float32
        || output->componentCount() == 0) {
      return AnimationSampleStatus::invalidAccessorType;
    }
    if (output->count == 0 || output->count > limits.maxKeyframes)
      return output->count == 0 ? AnimationSampleStatus::invalidAccessorCount
                                : AnimationSampleStatus::limitExceeded;
    std::uint64_t outputValues = 0;
    if (!checkedMultiply(output->count,
                         static_cast<std::uint64_t>(output->componentCount()),
                         outputValues)
        || outputValues > std::numeric_limits<std::size_t>::max()
        || output->values.size() != static_cast<std::size_t>(outputValues)) {
      return AnimationSampleStatus::invalidAccessorPayload;
    }
    if (!checkedAdd(valueBudget, outputValues, valueBudget)
        || valueBudget > limits.maxValues) {
      return AnimationSampleStatus::limitExceeded;
    }
    for (const float value : output->values) {
      if (!std::isfinite(value))
        return AnimationSampleStatus::invalidAccessorPayload;
    }
    if (output->count != input->count)
      return AnimationSampleStatus::invalidAccessorCount;

    if (input->count > std::numeric_limits<std::size_t>::max())
      return AnimationSampleStatus::limitExceeded;
    const std::size_t keyCount = static_cast<std::size_t>(input->count);
    if (keyCount > limits.maxKeyframes
        || prepared.info.keyframeCount > limits.maxKeyframes - keyCount)
      return AnimationSampleStatus::limitExceeded;
    prepared.info.keyframeCount += keyCount;
    if (!haveRange) {
      prepared.info.startTime = start;
      prepared.info.endTime = end;
      haveRange = true;
    } else {
      prepared.info.startTime = std::min(prepared.info.startTime, start);
      prepared.info.endTime = std::max(prepared.info.endTime, end);
    }
    prepared.samplers[samplerIndex] =
        PreparedSampler{*input, *output, source.interpolation};
  }

  if (!haveRange || !prepared.info.valid())
    return AnimationSampleStatus::invalidTimeRange;

  std::unordered_set<std::uint64_t> seenChannels;
  seenChannels.reserve(clip.channels.size());
  for (const AnimationChannelView& source : clip.channels) {
    if (!validPath(source.path) || source.samplerIndex < 0
        || static_cast<std::size_t>(source.samplerIndex) >= prepared.samplers.size()
        || (clip.nodeCount != 0 && source.nodeIndex >= clip.nodeCount)) {
      return AnimationSampleStatus::invalidChannel;
    }
    const std::uint64_t channelKey =
        (static_cast<std::uint64_t>(source.nodeIndex) << 8u)
        | static_cast<std::uint64_t>(source.path);
    if (!seenChannels.insert(channelKey).second)
      return AnimationSampleStatus::duplicateChannel;

    const PreparedSampler& sampler =
        prepared.samplers[static_cast<std::size_t>(source.samplerIndex)];
    const AnimationSampleStatus outputStatus = validateOutput(
        sampler.output, source.path, sampler.input.count);
    if (outputStatus != AnimationSampleStatus::success)
      return outputStatus;
    prepared.channels.push_back(PreparedChannel{
        source.nodeIndex, static_cast<std::size_t>(source.samplerIndex),
        source.path});
  }
  return AnimationSampleStatus::success;
}

[[nodiscard]] bool normalizeQuaternion(const std::array<double, 4>& input,
                                       std::array<double, 4>& output) noexcept {
  const double norm = std::sqrt(input[0] * input[0] + input[1] * input[1]
                                + input[2] * input[2] + input[3] * input[3]);
  if (!finite(norm) || norm <= kQuaternionEpsilon)
    return false;
  for (std::size_t component = 0; component < 4; ++component)
    output[component] = input[component] / norm;
  return true;
}

[[nodiscard]] bool readVec3(const AnimationAccessorView& output,
                             std::size_t key,
                             std::array<double, 3>& value) noexcept {
  if (key >= output.count || key > std::numeric_limits<std::size_t>::max() / 3u)
    return false;
  const std::size_t offset = key * 3u;
  if (offset > output.values.size() || output.values.size() - offset < 3u)
    return false;
  for (std::size_t component = 0; component < 3; ++component)
    value[component] = static_cast<double>(output.values[offset + component]);
  return true;
}

[[nodiscard]] bool readVec4(const AnimationAccessorView& output,
                             std::size_t key,
                             std::array<double, 4>& value) noexcept {
  if (key >= output.count || key > std::numeric_limits<std::size_t>::max() / 4u)
    return false;
  const std::size_t offset = key * 4u;
  if (offset > output.values.size() || output.values.size() - offset < 4u)
    return false;
  for (std::size_t component = 0; component < 4; ++component)
    value[component] = static_cast<double>(output.values[offset + component]);
  return true;
}

[[nodiscard]] bool sampleKeyIndex(const AnimationAccessorView& input,
                                  double time, std::size_t& index,
                                  double& factor) noexcept {
  if (input.count == 0 || input.count > std::numeric_limits<std::size_t>::max())
    return false;
  const std::size_t count = static_cast<std::size_t>(input.count);
  if (count == 1) {
    index = 0;
    factor = 0.0;
    return true;
  }
  const double first = static_cast<double>(input.values.front());
  const double last = static_cast<double>(input.values[count - 1]);
  if (time <= first) {
    index = 0;
    factor = 0.0;
    return true;
  }
  if (time >= last) {
    index = count - 1;
    factor = 0.0;
    return true;
  }
  const std::span<const float> values = input.values.first(count);
  const auto begin = values.begin();
  const auto upper = std::upper_bound(
      begin, values.end(), time,
      [](double value, float element) {
        return value < static_cast<double>(element);
      });
  if (upper == begin || upper == values.end())
    return false;
  index = static_cast<std::size_t>(upper - begin - 1);
  const double start = static_cast<double>(input.values[index]);
  const double end = static_cast<double>(input.values[index + 1]);
  const double delta = end - start;
  if (!(delta > 0.0) || !finite(delta))
    return false;
  factor = std::clamp((time - start) / delta, 0.0, 1.0);
  return finite(factor);
}

[[nodiscard]] bool lerpQuaternion(const std::array<double, 4>& first,
                                  const std::array<double, 4>& second,
                                  double factor,
                                  std::array<double, 4>& result) noexcept {
  std::array<double, 4> q0{};
  std::array<double, 4> q1{};
  if (!normalizeQuaternion(first, q0) || !normalizeQuaternion(second, q1))
    return false;
  double dot = q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3];
  if (dot < 0.0) {
    dot = -dot;
    for (double& component : q1)
      component = -component;
  }
  dot = std::clamp(dot, -1.0, 1.0);
  std::array<double, 4> blended{};
  if (dot > 0.9995) {
    for (std::size_t component = 0; component < 4; ++component)
      blended[component] = q0[component]
          + factor * (q1[component] - q0[component]);
  } else {
    const double theta = std::acos(dot);
    const double sine = std::sin(theta);
    if (!finite(sine) || std::abs(sine) <= kQuaternionEpsilon)
      return false;
    const double weight0 = std::sin((1.0 - factor) * theta) / sine;
    const double weight1 = std::sin(factor * theta) / sine;
    for (std::size_t component = 0; component < 4; ++component)
      blended[component] = weight0 * q0[component] + weight1 * q1[component];
  }
  return normalizeQuaternion(blended, result);
}

[[nodiscard]] bool sampleChannel(const PreparedSampler& sampler,
                                 AnimationTargetPath path, double time,
                                 std::optional<std::array<double, 3>>& vec3,
                                 std::optional<std::array<double, 4>>& vec4) noexcept {
  std::size_t key = 0;
  double factor = 0.0;
  if (!sampleKeyIndex(sampler.input, time, key, factor))
    return false;
  const bool useStep = sampler.interpolation == AnimationInterpolation::step;
  if (path == AnimationTargetPath::rotation) {
    std::array<double, 4> first{};
    if (!readVec4(sampler.output, key, first))
      return false;
    std::array<double, 4> value{};
    if (useStep || sampler.input.count == 1
        || key + 1 >= sampler.input.count) {
      if (!normalizeQuaternion(first, value))
        return false;
    } else {
      std::array<double, 4> second{};
      if (!readVec4(sampler.output, key + 1, second)
          || !lerpQuaternion(first, second, factor, value))
        return false;
    }
    vec4 = value;
    return true;
  }

  std::array<double, 3> first{};
  if (!readVec3(sampler.output, key, first))
    return false;
  std::array<double, 3> value = first;
  if (!useStep && sampler.input.count > 1
      && key + 1 < sampler.input.count) {
    std::array<double, 3> second{};
    if (!readVec3(sampler.output, key + 1, second))
      return false;
    for (std::size_t component = 0; component < 3; ++component)
      value[component] = first[component]
          + factor * (second[component] - first[component]);
  }
  for (const double component : value) {
    if (!finite(component))
      return false;
  }
  vec3 = value;
  return true;
}

} // namespace

bool AnimationAccessorView::valid() const noexcept {
  const std::size_t components = componentCount();
  if (componentType != AnimationAccessorComponentType::float32 || components == 0
      || count == 0 || count > std::numeric_limits<std::size_t>::max())
    return false;
  std::uint64_t expected = 0;
  return checkedMultiply(count, static_cast<std::uint64_t>(components), expected)
      && expected <= std::numeric_limits<std::size_t>::max()
      && values.size() == static_cast<std::size_t>(expected);
}

bool AnimationClipInfo::valid() const noexcept {
  return finite(startTime) && finite(endTime) && endTime >= startTime
      && samplerCount != 0 && channelCount != 0 && keyframeCount != 0;
}

std::optional<AnimationAccessorView> SpanAnimationAccessorAdapter::accessor(
    std::size_t index) const noexcept {
  if (index >= m_accessors.size())
    return std::nullopt;
  return m_accessors[index];
}

GltfAnimationAdapter::GltfAnimationAdapter(
    const GltfDocument& document, std::size_t animationIndex,
    const AnimationSamplingLimits& limits) noexcept
    : m_animationIndex(animationIndex) {
  try {
    auto candidate = std::make_unique<Impl>();
    m_status = buildGltfAnimationAdapter(document, animationIndex, limits,
                                         *candidate);
    candidate->status = m_status;
    if (m_status == AnimationSampleStatus::success)
      m_impl = std::move(candidate);
  } catch (const std::bad_alloc&) {
    m_status = AnimationSampleStatus::allocationFailure;
  } catch (...) {
    m_status = AnimationSampleStatus::invalidAdapter;
  }
}

GltfAnimationAdapter::~GltfAnimationAdapter() = default;
GltfAnimationAdapter::GltfAnimationAdapter(GltfAnimationAdapter&&) noexcept =
    default;
GltfAnimationAdapter& GltfAnimationAdapter::operator=(
    GltfAnimationAdapter&&) noexcept = default;

AnimationSampleStatus GltfAnimationAdapter::status() const noexcept {
  if (!m_impl && m_status == AnimationSampleStatus::success)
    return AnimationSampleStatus::invalidAdapter;
  return m_status;
}

std::size_t GltfAnimationAdapter::animationIndex() const noexcept {
  return m_animationIndex;
}

AnimationClipView GltfAnimationAdapter::clip() const noexcept {
  return m_impl ? m_impl->clip : AnimationClipView{};
}

std::size_t GltfAnimationAdapter::accessorCount() const noexcept {
  return m_impl ? m_impl->accessors.size() : 0;
}

std::optional<AnimationAccessorView> GltfAnimationAdapter::accessor(
    std::size_t index) const noexcept {
  if (!m_impl || index >= m_impl->accessors.size()
      || !m_impl->accessors[index].valid())
    return std::nullopt;
  return m_impl->accessors[index];
}

AnimationClipAnalysis analyzeAnimationClip(
    const AnimationClipView& clip, const AnimationAccessorAdapter& adapter,
    const AnimationSamplingLimits& limits) noexcept {
  AnimationClipAnalysis result;
  try {
    PreparedClip prepared;
    result.status = prepareClip(clip, adapter, limits, prepared);
    if (result.status == AnimationSampleStatus::success)
      result.info = prepared.info;
  } catch (const std::bad_alloc&) {
    result.status = AnimationSampleStatus::allocationFailure;
  } catch (...) {
    result.status = AnimationSampleStatus::invalidAdapter;
  }
  return result;
}

std::optional<double> normalizeAnimationTime(double time, double startTime,
                                             double endTime,
                                             AnimationTimeMode mode) noexcept {
  if (!finite(time) || !finite(startTime) || !finite(endTime)
      || endTime < startTime
      || (mode != AnimationTimeMode::clamp
          && mode != AnimationTimeMode::loop))
    return std::nullopt;
  if (endTime == startTime)
    return startTime;
  if (mode == AnimationTimeMode::clamp)
    return std::clamp(time, startTime, endTime);
  const double duration = endTime - startTime;
  const double offset = time - startTime;
  if (!finite(duration) || !(duration > 0.0) || !finite(offset))
    return std::nullopt;
  double wrapped = std::fmod(offset, duration);
  if (!finite(wrapped))
    return std::nullopt;
  if (wrapped < 0.0)
    wrapped += duration;
  double normalized = startTime + wrapped;
  if (!finite(normalized))
    return std::nullopt;
  // Guard a possible one-ulp round-up at the upper half-open boundary.
  if (normalized >= endTime)
    normalized = startTime;
  return normalized;
}

AnimationSampleResult sampleAnimationClip(
    const AnimationClipView& clip, const AnimationAccessorAdapter& adapter,
    double time, AnimationTimeMode mode,
    const AnimationSamplingLimits& limits) noexcept {
  AnimationSampleResult result;
  try {
    PreparedClip prepared;
    result.status = prepareClip(clip, adapter, limits, prepared);
    if (result.status != AnimationSampleStatus::success)
      return result;
    result.clip = prepared.info;
    const std::optional<double> normalized = normalizeAnimationTime(
        time, prepared.info.startTime, prepared.info.endTime, mode);
    if (!normalized) {
      result.status = AnimationSampleStatus::invalidTime;
      return result;
    }
    result.normalizedTime = *normalized;

    result.transforms.reserve(prepared.channels.size());
    std::unordered_map<std::uint32_t, std::size_t> transformIndices;
    transformIndices.reserve(prepared.channels.size());
    for (const PreparedChannel& channel : prepared.channels) {
      std::size_t transformIndex = 0;
      const auto found = transformIndices.find(channel.nodeIndex);
      if (found == transformIndices.end()) {
        transformIndex = result.transforms.size();
        transformIndices.emplace(channel.nodeIndex, transformIndex);
        result.transforms.push_back(AnimationTransformSample{
            .nodeIndex = channel.nodeIndex,
            .translation = std::nullopt,
            .rotation = std::nullopt,
            .scale = std::nullopt});
      } else {
        transformIndex = found->second;
      }

      std::optional<std::array<double, 3>> value3;
      std::optional<std::array<double, 4>> value4;
      if (!sampleChannel(prepared.samplers[channel.samplerIndex], channel.path,
                         *normalized, value3, value4)) {
        result.transforms.clear();
        result.status = AnimationSampleStatus::invalidAccessorPayload;
        return result;
      }
      AnimationTransformSample& transform = result.transforms[transformIndex];
      switch (channel.path) {
      case AnimationTargetPath::translation:
        transform.translation = value3;
        break;
      case AnimationTargetPath::rotation:
        transform.rotation = value4;
        break;
      case AnimationTargetPath::scale:
        transform.scale = value3;
        break;
      case AnimationTargetPath::invalid:
        result.transforms.clear();
        result.status = AnimationSampleStatus::invalidChannel;
        return result;
      }
    }
    result.status = AnimationSampleStatus::success;
  } catch (const std::bad_alloc&) {
    result.transforms.clear();
    result.status = AnimationSampleStatus::allocationFailure;
  } catch (...) {
    result.transforms.clear();
    result.status = AnimationSampleStatus::invalidAdapter;
  }
  return result;
}

AnimationClipAnalysis analyzeAnimation(
    const GltfDocument& document, std::size_t animationIndex,
    const AnimationSamplingLimits& limits) noexcept {
  GltfAnimationAdapter adapter(document, animationIndex, limits);
  if (!adapter.valid())
    return AnimationClipAnalysis{adapter.status(), {}};
  return analyzeAnimationClip(adapter.clip(), adapter, limits);
}

AnimationSampleResult sampleAnimation(
    const GltfDocument& document, std::size_t animationIndex, double time,
    bool loop, const AnimationSamplingLimits& limits) noexcept {
  GltfAnimationAdapter adapter(document, animationIndex, limits);
  if (!adapter.valid()) {
    AnimationSampleResult result;
    result.status = adapter.status();
    return result;
  }
  return sampleAnimationClip(adapter.clip(), adapter, time,
                             loop ? AnimationTimeMode::loop
                                  : AnimationTimeMode::clamp,
                             limits);
}

AnimationAdvanceResult advanceAnimationPlayback(AnimationPlaybackState& state,
                                                double deltaSeconds,
                                                const AnimationClipInfo& clip) noexcept {
  AnimationAdvanceResult result;
  if (!clip.valid() || !finite(deltaSeconds) || !finite(state.time)
      || !finite(state.speed) || state.speed < 0.0) {
    result.status = !clip.valid() ? AnimationSampleStatus::invalidTimeRange
                                  : AnimationSampleStatus::invalidTime;
    return result;
  }
  const double scaledDelta = deltaSeconds * state.speed;
  if (!finite(scaledDelta)) {
    result.status = AnimationSampleStatus::invalidTime;
    return result;
  }
  const double candidate = state.playing ? state.time + scaledDelta : state.time;
  if (!finite(candidate)) {
    result.status = AnimationSampleStatus::invalidTime;
    return result;
  }
  const std::optional<double> normalized = normalizeAnimationTime(
      candidate, clip.startTime, clip.endTime,
      state.loop ? AnimationTimeMode::loop : AnimationTimeMode::clamp);
  if (!normalized) {
    result.status = AnimationSampleStatus::invalidTime;
    return result;
  }
  state.time = *normalized;
  result.normalizedTime = *normalized;
  result.status = AnimationSampleStatus::success;
  return result;
}

} // namespace demo::editor
