#include "MetalDevice.h"

#include "MetalCommandAllocator.h"
#include "MetalCommon.h"
#include "MetalPipelines.h"
#include "MetalSurface.h"
#include "MetalSwapchain.h"

#include "../RHIDebugCounters.h"

#include <Metal/Metal.hpp>
#include <objc/message.h>
#include <objc/runtime.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace demo::rhi::metal {
namespace {

template <typename T>
NS::Object* asObject(T* object)
{
  return reinterpret_cast<NS::Object*>(object);
}

template <typename Handle, typename Fn>
std::vector<Handle> collectHandles(Fn&& enumerate)
{
  std::vector<Handle> result;
  enumerate([&result](Handle handle, auto&) { result.push_back(handle); });
  return result;
}

bool hasFormatFeature(FormatFeatureFlag value, FormatFeatureFlag requested)
{
  return (static_cast<uint32_t>(value) & static_cast<uint32_t>(requested)) != 0;
}

const void* immutableDataBytes(const NS::Data* data)
{
  // The metal-cpp Foundation wrapper only exposes mutableBytes(), while
  // MTL4CounterHeap returns an immutable NSData on macOS 26.
  using BytesMessage = const void* (*)(id, SEL);
  const auto sendBytes = reinterpret_cast<BytesMessage>(objc_msgSend);
  return sendBytes(reinterpret_cast<id>(const_cast<NS::Data*>(data)),
                   sel_registerName("bytes"));
}

MTL::TextureSwizzle resolveSwizzle(
  ComponentSwizzle value, MTL::TextureSwizzle identity)
{
  if(value == ComponentSwizzle::identity)
    return identity;
  return toMetalSwizzle(value);
}

MTL::TextureSwizzleChannels makeSwizzle(const ComponentMapping& mapping)
{
  return MTL::TextureSwizzleChannels::Make(
    resolveSwizzle(mapping.r, MTL::TextureSwizzleRed),
    resolveSwizzle(mapping.g, MTL::TextureSwizzleGreen),
    resolveSwizzle(mapping.b, MTL::TextureSwizzleBlue),
    resolveSwizzle(mapping.a, MTL::TextureSwizzleAlpha));
}

const ArgumentBinding* findBinding(
  const MetalArgumentLayoutRecord& layout, uint32_t binding)
{
  const auto found = std::find_if(
    layout.bindings.begin(), layout.bindings.end(),
    [binding](const ArgumentBinding& value) { return value.binding == binding; });
  return found == layout.bindings.end() ? nullptr : &*found;
}

bool usesTexture(ArgumentType type)
{
  return type == ArgumentType::sampledTexture ||
         type == ArgumentType::storageTexture ||
         type == ArgumentType::combinedImageSampler;
}

bool usesSampler(ArgumentType type)
{
  return type == ArgumentType::sampler ||
         type == ArgumentType::combinedImageSampler;
}

bool usesBuffer(ArgumentType type)
{
  return type == ArgumentType::uniformBuffer ||
         type == ArgumentType::storageBuffer;
}

bool isDepthFormat(TextureFormat format)
{
  return format == TextureFormat::d16Unorm ||
         format == TextureFormat::d32Sfloat ||
         format == TextureFormat::d24UnormS8 ||
         format == TextureFormat::d32SfloatS8;
}

bool isIdentityMapping(const ComponentMapping& mapping)
{
  return mapping.r == ComponentSwizzle::identity &&
         mapping.g == ComponentSwizzle::identity &&
         mapping.b == ComponentSwizzle::identity &&
         mapping.a == ComponentSwizzle::identity;
}

TextureFormat resolvedViewFormat(
  TextureFormat requested, TextureFormat parent)
{
  return requested == TextureFormat::undefined ? parent : requested;
}

void enableMetalValidationEnvironment()
{
  setenv("MTL_DEBUG_LAYER", "1", 1);
  setenv("MTL_DEBUG_LAYER_ERROR_MODE", "assert", 1);
  setenv("MTL_SHADER_VALIDATION", "1", 1);
  setenv("MTL_SHADER_VALIDATION_DEFAULT_STATE", "all", 1);
  setenv("MTL_SHADER_VALIDATION_REPORT_TO_STDERR", "1", 1);
}

}  // namespace

MetalDevice::MetalDevice() : m_resources(std::make_unique<MetalResourceTable>())
{
}

MetalDevice::~MetalDevice()
{
  deinit();
}

void MetalDevice::init(const DeviceCreateInfo& createInfo)
{
  if(m_initialized)
    throw std::runtime_error("MetalDevice::init called more than once");
  m_createInfo = createInfo;
  if(createInfo.enableValidationLayers)
    enableMetalValidationEnvironment();
  m_device = MTL::CreateSystemDefaultDevice();
  if(m_device == nullptr)
    throw std::runtime_error("Metal 4 requires a Metal-capable Apple GPU");

  try
  {
    auto* compilerDesc = MTL4::CompilerDescriptor::alloc()->init();
    compilerDesc->setLabel(nsString("MGIP Metal4 compiler"));
    NS::Error* error = nullptr;
    m_compiler = m_device->newCompiler(compilerDesc, &error);
    compilerDesc->release();
    if(m_compiler == nullptr)
      throw std::runtime_error(errorMessage("MTLDevice::newCompiler", error));

    m_blitPipeline = compileMetalBlitPipeline(
      *m_compiler, createInfo.enableValidationLayers);

    auto* poolDesc = MTL::ResourceViewPoolDescriptor::alloc()->init();
    poolDesc->setLabel(nsString("MGIP Metal4 texture-view pool"));
    poolDesc->setResourceViewCount(m_textureViewCapacity);
    m_textureViewPool = m_device->newTextureViewPool(poolDesc, &error);
    poolDesc->release();
    if(m_textureViewPool == nullptr)
      throw std::runtime_error(errorMessage("MTLDevice::newTextureViewPool", error));

    constexpr std::array<QueueClass, 3> queueClasses{
      QueueClass::graphics, QueueClass::compute, QueueClass::transfer};
    constexpr std::array<const char*, 3> queueLabels{
      "MGIP Metal4 graphics queue", "MGIP Metal4 compute queue",
      "MGIP Metal4 transfer queue"};
    for(uint32_t index = 0; index < m_queues.size(); ++index)
    {
      auto* queueDesc = MTL4::CommandQueueDescriptor::alloc()->init();
      queueDesc->setLabel(nsString(queueLabels[index]));
      MTL4::CommandQueue* nativeQueue =
        m_device->newMTL4CommandQueue(queueDesc, &error);
      queueDesc->release();
      MTL::SharedEvent* event = m_device->newSharedEvent();
      if(nativeQueue == nullptr || event == nullptr)
      {
        if(nativeQueue != nullptr) nativeQueue->release();
        if(event != nullptr) event->release();
        throw std::runtime_error(errorMessage("MTLDevice::newMTL4CommandQueue", error));
      }
      event->setLabel(nsString(queueLabels[index]));
      m_queues[index] = std::make_unique<MetalQueue>();
      m_queues[index]->init(
        nativeQueue, event, QueueIdentity{index + 1, m_queueGeneration},
        QueueInfo{
          .queueClass = queueClasses[index],
          .capabilities = {
            .supportsTimestamps = true,
            .supportsPresent = index == 0,
            .supportsSparseBinding = false,
          },
          .dedicated = true,
          .available = true,
        });
    }
    const std::array<MetalQueue*, 3> registry{
      m_queues[0].get(), m_queues[1].get(), m_queues[2].get()};
    for(auto& queue : m_queues)
      queue->setQueueRegistry(registry);

    m_physicalDeviceInfo.deviceName = m_device->name() != nullptr
      ? m_device->name()->utf8String()
      : "Metal 4 device";
    m_physicalDeviceInfo.nativeApiVersion = 4;
#if defined(DEMO_PLATFORM_IOS)
    m_physicalDeviceInfo.deviceType = 1u;
#else
    m_physicalDeviceInfo.deviceType = m_device->isLowPower() ? 1u : 2u;
#endif
    m_featureInfo = DeviceFeatureInfo{
      .timelineSemaphore = true,
      .synchronization2 = true,
      .dynamicRendering = true,
      .maintenance5 = false,
      .maintenance6 = false,
    };
    m_capabilities = CapabilityReport{
      .coreGraphics = true,
      .coreCompute = true,
      .coreBindless = true,
      .extensionAsyncCompute = true,
      .extensionMeshShader = false,
      .extensionRayTracing = m_device->supportsRaytracing(),
      .descriptorHeap = true,
      .explicitResidency = ExplicitResidencyLevel::managed,
      .pipelineCompiler = true,
      .multiQueue = true,
    };
    const RHICapabilityError capabilityError =
      evaluateCapabilityRequirements(m_capabilities,
                                     createInfo.capabilityRequirements);
    if(capabilityError != RHICapabilityError::None)
      throw std::runtime_error(std::string("Metal capability requirement failed: ") +
                               toString(capabilityError));
    m_memoryProperties.memoryTypes.push_back(MemoryTypeInfo{});
    m_memoryProperties.memoryHeaps.push_back(MemoryHeapInfo{
      .size = m_device->recommendedMaxWorkingSetSize(),
      .flags = m_device->hasUnifiedMemory() ? 1u : 0u,
    });
    m_initialized = true;
  }
  catch(...)
  {
    deinit();
    throw;
  }
}

void MetalDevice::deinit()
{
  if(m_device == nullptr)
    return;
  if(m_initialized)
  {
    try { waitIdle(); } catch(...) {}
    releaseActiveResources();
    collectGarbage();
  }
  for(NativeRetirement& retirement : m_retirements)
  {
    if(retirement.secondary != nullptr) retirement.secondary->release();
    if(retirement.primary != nullptr) retirement.primary->release();
  }
  m_retirements.clear();
  for(auto& queue : m_queues)
    queue.reset();
  if(m_blitPipeline != nullptr) m_blitPipeline->release();
  if(m_textureViewPool != nullptr) m_textureViewPool->release();
  if(m_compiler != nullptr) m_compiler->release();
  m_textureViewPool = nullptr;
  m_blitPipeline = nullptr;
  m_compiler = nullptr;
  m_device->release();
  m_device = nullptr;
  m_resources = std::make_unique<MetalResourceTable>();
  m_physicalDeviceInfo = {};
  m_featureInfo = {};
  m_capabilities = {};
  m_memoryProperties = {};
  m_nextTextureViewIndex = 0;
  m_freeTextureViewIndices.clear();
  m_initialized = false;
}

BackendInfo MetalDevice::getBackendInfo() const
{
  return BackendInfo{
    .type = BackendType::metal,
    .apiName = "Metal 4",
    .version = BackendVersion{.major = 4},
  };
}

const char* MetalDevice::getDeviceName() const
{
  return m_physicalDeviceInfo.deviceName.c_str();
}

const PhysicalDeviceInfo& MetalDevice::getPhysicalDeviceInfo() const
{
  return m_physicalDeviceInfo;
}

const DeviceFeatureInfo& MetalDevice::getEnabledFeatureInfo() const
{
  return m_featureInfo;
}

CapabilityReport MetalDevice::queryCapabilities() const
{
  return m_capabilities;
}

bool MetalDevice::supports(CapabilityTier tier) const
{
  return supportsTier(m_capabilities, tier);
}

const MemoryProperties& MetalDevice::getPhysicalMemoryProperties() const
{
  return m_memoryProperties;
}

void MetalDevice::validateInitialized(const char* operation) const
{
  if(!m_initialized || m_device == nullptr)
    throw std::runtime_error(std::string(operation) + " requires an initialized Metal device");
}

Queue* MetalDevice::getQueue(QueueClass queueClass)
{
  validateInitialized("MetalDevice::getQueue");
  return m_queues[static_cast<uint32_t>(queueClass)].get();
}

std::unique_ptr<CommandAllocator> MetalDevice::createCommandAllocator(
  QueueClass queueClass)
{
  validateInitialized("MetalDevice::createCommandAllocator");
  return std::make_unique<MetalCommandAllocator>(
    *m_queues[static_cast<uint32_t>(queueClass)], m_device, m_resources.get(),
    m_blitPipeline);
}

void MetalDevice::waitIdle()
{
  validateInitialized("MetalDevice::waitIdle");
  for(const auto& queue : m_queues)
    queue->waitIdle();
  collectGarbage();
}

std::array<SubmissionToken, 3> MetalDevice::completedTokens() const
{
  std::array<SubmissionToken, 3> result{};
  for(uint32_t index = 0; index < m_queues.size(); ++index)
  {
    if(m_queues[index] != nullptr)
      result[index] = SubmissionToken{m_queues[index]->identity(),
                                      m_queues[index]->completedValue()};
  }
  return result;
}

bool MetalDevice::dependenciesComplete(
  const SubmissionTokenSet& dependencies) const
{
  const auto completed = completedTokens();
  return dependencies.isSatisfiedBy(completed);
}

void MetalDevice::enqueueRetirement(
  NS::Object* primary, SubmissionTokenSet dependencies, NS::Object* secondary,
  RetirementKind kind, uint32_t textureViewPoolIndex)
{
  if(primary == nullptr && secondary == nullptr &&
     textureViewPoolIndex == kInvalidMetalBinding)
    return;
  if(dependenciesComplete(dependencies))
  {
    if(textureViewPoolIndex != kInvalidMetalBinding && m_textureViewPool != nullptr)
    {
      m_textureViewPool->setTextureView(nullptr, textureViewPoolIndex);
      m_freeTextureViewIndices.push_back(textureViewPoolIndex);
    }
    if(secondary != nullptr) secondary->release();
    if(primary != nullptr) primary->release();
    return;
  }
  m_retirements.push_back(NativeRetirement{
    .primary = primary,
    .secondary = secondary,
    .dependencies = dependencies,
    .kind = kind,
    .textureViewPoolIndex = textureViewPoolIndex,
  });
}

void MetalDevice::collectGarbage()
{
  if(m_device == nullptr)
    return;
  const auto completed = completedTokens();
  const auto newEnd = std::remove_if(
    m_retirements.begin(), m_retirements.end(),
    [this, &completed](NativeRetirement& retirement) {
      if(!retirement.dependencies.isSatisfiedBy(completed))
        return false;
      if(retirement.textureViewPoolIndex != kInvalidMetalBinding &&
         m_textureViewPool != nullptr)
      {
        m_textureViewPool->setTextureView(nullptr,
                                          retirement.textureViewPoolIndex);
        m_freeTextureViewIndices.push_back(retirement.textureViewPoolIndex);
      }
      if(retirement.secondary != nullptr) retirement.secondary->release();
      if(retirement.primary != nullptr) retirement.primary->release();
      retirement = {};
      return true;
    });
  m_retirements.erase(newEnd, m_retirements.end());
}

float MetalDevice::getTimestampPeriodNs() const
{
  return 1.0f;
}

bool MetalDevice::isFormatSupported(
  TextureFormat format, FormatFeatureFlag feature) const
{
  if(m_device == nullptr || toMetalPixelFormat(format) == MTL::PixelFormatInvalid)
    return false;
  if(isBlockCompressed(format) && !m_device->supportsBCTextureCompression())
    return false;
  const bool depth = format == TextureFormat::d16Unorm ||
                     format == TextureFormat::d32Sfloat ||
                     format == TextureFormat::d24UnormS8 ||
                     format == TextureFormat::d32SfloatS8;
  if(hasFormatFeature(feature, FormatFeatureFlag::depthStencilAttachment))
    return depth && (format != TextureFormat::d24UnormS8 ||
                     m_device->isDepth24Stencil8PixelFormatSupported());
  if(hasFormatFeature(feature, FormatFeatureFlag::colorAttachment) ||
     hasFormatFeature(feature, FormatFeatureFlag::storageImage))
    return !depth && !isBlockCompressed(format);
  return hasFormatFeature(feature, FormatFeatureFlag::sampledImage);
}

void MetalDevice::initSurface(Surface& surface, const WindowHandle& window)
{
  validateInitialized("MetalDevice::initSurface");
  auto* metalSurface = dynamic_cast<MetalSurface*>(&surface);
  if(metalSurface == nullptr)
    throw std::invalid_argument("MetalDevice::initSurface received another backend's surface");
  metalSurface->init(m_device, window);
}

std::unique_ptr<Swapchain> MetalDevice::createSwapchain(
  Surface& surface, bool vSync)
{
  validateInitialized("MetalDevice::createSwapchain");
  auto* metalSurface = dynamic_cast<MetalSurface*>(&surface);
  if(metalSurface == nullptr || metalSurface->layer() == nullptr)
    throw std::invalid_argument("MetalDevice::createSwapchain requires an initialized Metal surface");
  auto result = std::make_unique<MetalSwapchain>();
  result->init(*this, *m_queues[0], *metalSurface, vSync, 3);
  return result;
}

TextureHandle MetalDevice::createTexture(const TextureDesc& desc)
{
  validateInitialized("MetalDevice::createTexture");
  if(desc.extent.width == 0 || desc.extent.height == 0 || desc.extent.depth == 0 ||
     desc.mipLevels == 0 || desc.arrayLayers == 0)
  {
    throw std::invalid_argument("Metal texture requires nonzero extent, mip, and layer counts");
  }
  if(desc.usage == TextureUsageFlags::none)
    throw std::invalid_argument("Metal texture requires at least one usage flag");
  if(toMetalPixelFormat(desc.format) == MTL::PixelFormatInvalid)
    throw std::invalid_argument("Metal texture format is unsupported");
  if(desc.dimension == TextureDimension::e3D && desc.arrayLayers != 1)
    throw std::invalid_argument("Metal 3D texture requires one array layer");
  if(desc.dimension != TextureDimension::e3D && desc.extent.depth != 1)
    throw std::invalid_argument("Metal non-3D texture requires depth one");
  if(desc.dimension == TextureDimension::e2D && desc.arrayLayers != 1)
    throw std::invalid_argument("Metal 2D texture requires one array layer");
  if(desc.dimension == TextureDimension::eCube &&
     (desc.arrayLayers != 6 || desc.extent.width != desc.extent.height))
  {
    throw std::invalid_argument("Metal cube texture requires six square faces");
  }
  if(hasUsage(desc.usage, TextureUsageFlags::depthAttachment) !=
     isDepthFormat(desc.format))
  {
    if(hasUsage(desc.usage, TextureUsageFlags::depthAttachment) ||
       hasUsage(desc.usage, TextureUsageFlags::colorAttachment))
    {
      throw std::invalid_argument("Metal texture attachment usage and format are incompatible");
    }
  }
  if(isDepthFormat(desc.format) && hasUsage(desc.usage, TextureUsageFlags::storage))
    throw std::invalid_argument("Metal depth textures cannot use storage-image access");
  if(desc.memoryUsage == MemoryUsage::transientAttachment &&
     !hasUsage(desc.usage, TextureUsageFlags::colorAttachment) &&
     !hasUsage(desc.usage, TextureUsageFlags::depthAttachment))
  {
    throw std::invalid_argument("Metal memoryless texture requires attachment usage");
  }

  auto* nativeDesc = MTL::TextureDescriptor::alloc()->init();
  nativeDesc->setTextureType(toMetalTextureType(desc.dimension, desc.sampleCount));
  nativeDesc->setPixelFormat(toMetalPixelFormat(desc.format));
  nativeDesc->setWidth(desc.extent.width);
  nativeDesc->setHeight(desc.extent.height);
  nativeDesc->setDepth(desc.dimension == TextureDimension::e3D ? desc.extent.depth : 1);
  nativeDesc->setMipmapLevelCount(desc.mipLevels);
  nativeDesc->setArrayLength(desc.dimension == TextureDimension::e2DArray
                               ? desc.arrayLayers
                               : 1);
  nativeDesc->setSampleCount(static_cast<uint32_t>(desc.sampleCount));
  nativeDesc->setStorageMode(toMetalStorageMode(desc.memoryUsage));
  nativeDesc->setHazardTrackingMode(MTL::HazardTrackingModeUntracked);
  // Metal 4 has no texture-fill command. clearColorTexture lowers to a
  // load-action clear render pass, so color transfer destinations need the
  // native render-target capability even when the portable descriptor does
  // not advertise attachment usage.
  const bool supportsRenderPassClear =
    !isDepthFormat(desc.format) && !isBlockCompressed(desc.format);
  nativeDesc->setUsage(
    toMetalTextureUsage(desc.usage, supportsRenderPassClear));
  MTL::Texture* texture = m_device->newTexture(nativeDesc);
  nativeDesc->release();
  if(texture == nullptr)
    throw std::runtime_error("Metal failed to create texture");
  if(desc.debugName != nullptr)
    texture->setLabel(nsString(desc.debugName));

  TextureDesc storedDesc = desc;
  storedDesc.debugName = nullptr;
  return m_resources->registerTexture(MetalTextureRecord{
    .nativeTexture = texture,
    .desc = storedDesc,
    .debugName = desc.debugName != nullptr ? desc.debugName : "",
    .owned = true,
  });
}

void MetalDevice::destroyTexture(TextureHandle handle)
{
  if(handle.isNull() || m_resources == nullptr)
    return;
  MetalTextureRecord record = m_resources->removeTexture(handle);
  if(record.owned && record.nativeTexture != nullptr)
    enqueueRetirement(asObject(record.nativeTexture), record.pendingUses);
}

TextureViewHandle MetalDevice::createTextureView(
  const TextureViewCreateDesc& desc)
{
  validateInitialized("MetalDevice::createTextureView");
  incrementHotPathCounter(BackendType::metal, HotPathCounter::textureViewCreations);
  MetalTextureRecord* parent = m_resources->tryGetTexture(desc.image);
  if(parent == nullptr || parent->nativeTexture == nullptr)
    throw std::invalid_argument("Metal texture view references a stale texture");
  const TextureFormat format = resolvedViewFormat(desc.format, parent->desc.format);
  if(toMetalPixelFormat(format) == MTL::PixelFormatInvalid)
    throw std::invalid_argument("Metal texture view format is unsupported");
  if((desc.aspect != TextureAspect::color) != isDepthFormat(format))
    throw std::invalid_argument("Metal texture view aspect does not match its format");
  if(desc.levelCount == 0 || desc.baseMipLevel >= parent->desc.mipLevels ||
     desc.levelCount > parent->desc.mipLevels - desc.baseMipLevel)
  {
    throw std::out_of_range("Metal texture view mip range is invalid");
  }
  if(desc.layerCount == 0 || desc.baseArrayLayer >= parent->desc.arrayLayers ||
     desc.layerCount > parent->desc.arrayLayers - desc.baseArrayLayer)
  {
    throw std::out_of_range("Metal texture view layer range is invalid");
  }
  if(desc.viewType == ImageViewType::eCube &&
     (parent->desc.dimension != TextureDimension::eCube ||
      desc.baseArrayLayer != 0 || desc.layerCount != 6))
  {
    throw std::invalid_argument("Metal cube view requires one complete cube texture");
  }
  if(desc.viewType == ImageViewType::e3D &&
     (parent->desc.dimension != TextureDimension::e3D ||
      desc.baseArrayLayer != 0 || desc.layerCount != 1))
  {
    throw std::invalid_argument("Metal 3D view is incompatible with its texture");
  }

  const bool implicitView =
    format == parent->desc.format && desc.baseMipLevel == 0 &&
    desc.levelCount == parent->desc.mipLevels && desc.baseArrayLayer == 0 &&
    desc.layerCount == parent->desc.arrayLayers && isIdentityMapping(desc.components) &&
    ((desc.viewType == ImageViewType::e2D &&
      parent->desc.dimension == TextureDimension::e2D) ||
     (desc.viewType == ImageViewType::e2DArray &&
      parent->desc.dimension == TextureDimension::e2DArray) ||
     (desc.viewType == ImageViewType::eCube &&
      parent->desc.dimension == TextureDimension::eCube) ||
     (desc.viewType == ImageViewType::e3D &&
      parent->desc.dimension == TextureDimension::e3D));

  MTL::Texture* nativeView = nullptr;
  MTL::ResourceID resourceId{};
  uint32_t poolIndex = kInvalidMetalBinding;
  if(implicitView)
  {
    nativeView = parent->nativeTexture;
    nativeView->retain();
    resourceId = nativeView->gpuResourceID();
  }
  else
  {
    if(!m_freeTextureViewIndices.empty())
    {
      poolIndex = m_freeTextureViewIndices.back();
      m_freeTextureViewIndices.pop_back();
    }
    else
    {
      if(m_nextTextureViewIndex >= m_textureViewCapacity)
        throw std::runtime_error("Metal texture-view pool is exhausted");
      poolIndex = m_nextTextureViewIndex++;
    }

    auto* viewDesc = MTL::TextureViewDescriptor::alloc()->init();
    viewDesc->setPixelFormat(toMetalPixelFormat(format));
    viewDesc->setTextureType(toMetalTextureType(desc.viewType));
    viewDesc->setLevelRange(NS::Range::Make(desc.baseMipLevel, desc.levelCount));
    viewDesc->setSliceRange(NS::Range::Make(desc.baseArrayLayer, desc.layerCount));
    viewDesc->setSwizzle(makeSwizzle(desc.components));
    resourceId = m_textureViewPool->setTextureView(
      parent->nativeTexture, viewDesc, poolIndex);
    nativeView = parent->nativeTexture->newTextureView(viewDesc);
    viewDesc->release();
    if(nativeView == nullptr || resourceId._impl == 0)
    {
      if(nativeView != nullptr) nativeView->release();
      m_textureViewPool->setTextureView(nullptr, poolIndex);
      m_freeTextureViewIndices.push_back(poolIndex);
      throw std::runtime_error("Metal failed to create texture view");
    }
  }
  if(desc.debugName != nullptr && nativeView != nullptr)
    nativeView->setLabel(nsString(desc.debugName));

  TextureViewCreateDesc storedDesc = desc;
  storedDesc.format = format;
  storedDesc.debugName = nullptr;
  return m_resources->registerTextureView(MetalTextureViewRecord{
    .nativeTextureView = nativeView,
    .parentTexture = desc.image,
    .desc = storedDesc,
    .resourceId = resourceId._impl,
    .poolIndex = poolIndex,
    .debugName = desc.debugName != nullptr ? desc.debugName : "",
    .ownsNativeView = true,
  });
}

void MetalDevice::destroyTextureView(TextureViewHandle handle)
{
  if(handle.isNull() || m_resources == nullptr)
    return;
  MetalTextureViewRecord record = m_resources->removeTextureView(handle);
  enqueueRetirement(
    record.ownsNativeView ? asObject(record.nativeTextureView) : nullptr,
    record.pendingUses, nullptr, RetirementKind::generic, record.poolIndex);
}

BufferHandle MetalDevice::createBuffer(const BufferDesc& desc)
{
  validateInitialized("MetalDevice::createBuffer");
  if(desc.size == 0)
    throw std::invalid_argument("Metal buffer size must be nonzero");
  if(desc.memoryUsage == MemoryUsage::transientAttachment)
    throw std::invalid_argument("Metal buffers cannot use transient attachment memory");
  MTL::Buffer* buffer = m_device->newBuffer(desc.size,
                                             toMetalBufferOptions(desc.memoryUsage));
  if(buffer == nullptr)
    throw std::runtime_error("Metal failed to create buffer");
  if(desc.debugName != nullptr)
    buffer->setLabel(nsString(desc.debugName));
  BufferDesc storedDesc = desc;
  storedDesc.debugName = nullptr;
  return m_resources->registerBuffer(MetalBufferRecord{
    .nativeBuffer = buffer,
    .desc = storedDesc,
    .debugName = desc.debugName != nullptr ? desc.debugName : "",
    .owned = true,
  });
}

void MetalDevice::destroyBuffer(BufferHandle handle)
{
  if(handle.isNull() || m_resources == nullptr)
    return;
  MetalBufferRecord record = m_resources->removeBuffer(handle);
  if(record.owned && record.nativeBuffer != nullptr)
    enqueueRetirement(asObject(record.nativeBuffer), record.pendingUses);
}

GpuPtr MetalDevice::getBufferGpuAddress(BufferHandle handle) const
{
  const MetalBufferRecord* record = m_resources != nullptr
    ? m_resources->tryGetBuffer(handle)
    : nullptr;
  return record != nullptr && record->nativeBuffer != nullptr
    ? GpuPtr{record->nativeBuffer->gpuAddress()}
    : GpuPtr{};
}

void* MetalDevice::mapBuffer(BufferHandle handle)
{
  const auto result = mapBufferRange(handle, {});
  return result ? result.value.data : nullptr;
}

void MetalDevice::unmapBuffer(BufferHandle handle)
{
  if(m_resources == nullptr || m_resources->tryGetBuffer(handle) == nullptr)
    throw std::invalid_argument("MetalDevice::unmapBuffer received a stale buffer");
}

Result<MappedBufferRange> MetalDevice::mapBufferRange(
  BufferHandle handle, const BufferMapDesc& desc)
{
  MetalBufferRecord* record = m_resources != nullptr
    ? m_resources->tryGetBuffer(handle)
    : nullptr;
  if(record == nullptr || record->nativeBuffer == nullptr)
    return Result<MappedBufferRange>::fail(RHIErrorCode::invalidHandle,
                                            "Metal buffer handle is stale");
  if(record->desc.memoryUsage != MemoryUsage::cpuToGpu &&
     record->desc.memoryUsage != MemoryUsage::gpuToCpu)
  {
    return Result<MappedBufferRange>::fail(RHIErrorCode::invalidState,
                                            "Metal private buffer is not CPU visible");
  }
  if(desc.offset > record->desc.size)
    return Result<MappedBufferRange>::fail(RHIErrorCode::invalidArgument,
                                            "Metal buffer map offset is out of range");
  const uint64_t size = desc.size == 0 ? record->desc.size - desc.offset : desc.size;
  if(size > record->desc.size - desc.offset)
    return Result<MappedBufferRange>::fail(RHIErrorCode::invalidArgument,
                                            "Metal buffer map range is out of bounds");
  void* contents = record->nativeBuffer->contents();
  if(contents == nullptr)
    return Result<MappedBufferRange>::fail(RHIErrorCode::backendFailure,
                                            "Metal buffer has no CPU mapping");
  return Result<MappedBufferRange>::ok(MappedBufferRange{
    .data = static_cast<std::byte*>(contents) + desc.offset,
    .offset = desc.offset,
    .size = size,
    .coherent = true,
    .persistent = true,
  });
}

RHIResult MetalDevice::flushMappedBufferRange(
  BufferHandle handle, uint64_t offset, uint64_t size)
{
  MetalBufferRecord* record = m_resources != nullptr
    ? m_resources->tryGetBuffer(handle)
    : nullptr;
  if(record == nullptr)
    return RHIResult::fail(RHIErrorCode::invalidHandle, "Metal buffer handle is stale");
  if(record->desc.memoryUsage != MemoryUsage::cpuToGpu &&
     record->desc.memoryUsage != MemoryUsage::gpuToCpu)
    return RHIResult::fail(RHIErrorCode::invalidState, "Metal buffer is not mapped");
  if(offset > record->desc.size ||
     (size != 0 && size > record->desc.size - offset))
    return RHIResult::fail(RHIErrorCode::invalidArgument, "Metal flush range is invalid");
  return RHIResult::ok();
}

RHIResult MetalDevice::invalidateMappedBufferRange(
  BufferHandle handle, uint64_t offset, uint64_t size)
{
  return flushMappedBufferRange(handle, offset, size);
}

SamplerHandle MetalDevice::createSampler(const SamplerDesc& desc)
{
  validateInitialized("MetalDevice::createSampler");
  if(desc.minLod > desc.maxLod || desc.maxAnisotropy < 1.0f)
    throw std::invalid_argument("Metal sampler LOD or anisotropy range is invalid");
  auto* nativeDesc = MTL::SamplerDescriptor::alloc()->init();
  nativeDesc->setMagFilter(toMetalFilter(desc.magFilter));
  nativeDesc->setMinFilter(toMetalFilter(desc.minFilter));
  nativeDesc->setMipFilter(toMetalMipFilter(desc.mipmapMode));
  nativeDesc->setSAddressMode(toMetalAddressMode(desc.addressModeU));
  nativeDesc->setTAddressMode(toMetalAddressMode(desc.addressModeV));
  nativeDesc->setRAddressMode(toMetalAddressMode(desc.addressModeW));
  nativeDesc->setLodBias(desc.mipLodBias);
  nativeDesc->setLodMinClamp(desc.minLod);
  nativeDesc->setLodMaxClamp(desc.maxLod);
  nativeDesc->setMaxAnisotropy(desc.anisotropyEnable
                                ? static_cast<NS::UInteger>(std::ceil(desc.maxAnisotropy))
                                : 1);
  nativeDesc->setCompareFunction(desc.compareEnable
                                   ? toMetalCompare(desc.compareOp)
                                   : MTL::CompareFunctionNever);
  nativeDesc->setSupportArgumentBuffers(true);
  if(desc.debugName != nullptr)
    nativeDesc->setLabel(nsString(desc.debugName));
  MTL::SamplerState* sampler = m_device->newSamplerState(nativeDesc);
  nativeDesc->release();
  if(sampler == nullptr || sampler->gpuResourceID()._impl == 0)
  {
    if(sampler != nullptr) sampler->release();
    throw std::runtime_error("Metal failed to create argument-table sampler");
  }
  return m_resources->registerSampler(MetalSamplerRecord{
    .nativeSampler = sampler,
    .debugName = desc.debugName != nullptr ? desc.debugName : "",
  });
}

void MetalDevice::destroySampler(SamplerHandle handle)
{
  if(handle.isNull() || m_resources == nullptr)
    return;
  MetalSamplerRecord record = m_resources->removeSampler(handle);
  if(record.nativeSampler != nullptr)
    enqueueRetirement(asObject(record.nativeSampler), record.pendingUses);
}

ArgumentLayoutHandle MetalDevice::createArgumentLayout(
  const ArgumentLayoutDesc& desc)
{
  validateInitialized("MetalDevice::createArgumentLayout");
  incrementHotPathCounter(BackendType::metal, HotPathCounter::argumentLayoutCreations);
  MetalArgumentLayoutRecord record;
  record.debugName = desc.debugName != nullptr ? desc.debugName : "";
  record.bindings.reserve(desc.bindings.size());
  for(const ArgumentBinding& binding : desc.bindings)
  {
    if(binding.arrayCount == 0 || binding.visibility == ShaderStage::none)
      throw std::invalid_argument("Metal argument binding requires count and visibility");
    if(findBinding(record, binding.binding) != nullptr)
      throw std::invalid_argument("Metal argument layout contains a duplicate binding");
    if(binding.dynamicOffset && !usesBuffer(binding.type))
      throw std::invalid_argument("Metal dynamic offsets apply only to buffer bindings");
    record.bindings.push_back(binding);
  }
  return m_resources->registerArgumentLayout(std::move(record));
}

void MetalDevice::destroyArgumentLayout(ArgumentLayoutHandle handle)
{
  if(!handle.isNull() && m_resources != nullptr)
    (void)m_resources->removeArgumentLayout(handle);
}

ArgumentTableHandle MetalDevice::createArgumentTable(
  const ArgumentTableCreateDesc& desc)
{
  validateInitialized("MetalDevice::createArgumentTable");
  incrementHotPathCounter(BackendType::metal, HotPathCounter::descriptorAllocations);
  const MetalArgumentLayoutRecord* layout = m_resources->tryGetArgumentLayout(desc.layout);
  if(layout == nullptr)
    throw std::invalid_argument("Metal argument table references a stale layout");
  MetalArgumentTableRecord record{
    .layout = desc.layout,
    .lifetime = desc.lifetime,
    .debugName = desc.debugName != nullptr ? desc.debugName : "",
  };
  for(const ArgumentBinding& binding : layout->bindings)
  {
    for(uint32_t element = 0; element < binding.arrayCount; ++element)
    {
      record.values.push_back(MetalArgumentValue{
        .binding = binding.binding,
        .arrayElement = element,
        .type = binding.type,
      });
    }
  }
  return m_resources->registerArgumentTable(std::move(record));
}

void MetalDevice::destroyArgumentTable(ArgumentTableHandle handle)
{
  if(!handle.isNull() && m_resources != nullptr)
    (void)m_resources->removeArgumentTable(handle);
}

bool MetalDevice::argumentTableCanUpdate(
  const MetalArgumentTableRecord& table) const
{
  return table.pendingUses.count == 0 || dependenciesComplete(table.pendingUses);
}

void MetalDevice::updateArgumentTable(
  ArgumentTableHandle tableHandle, ArgumentWriteBatch writes)
{
  validateInitialized("MetalDevice::updateArgumentTable");
  if(writes.empty())
    return;
  MetalArgumentTableRecord* table = m_resources->tryGetArgumentTable(tableHandle);
  if(table == nullptr)
    throw std::invalid_argument("Metal argument-table handle is stale");
  if(!argumentTableCanUpdate(*table))
    throw std::runtime_error("Metal argument-table update rejected pending GPU work");
  if(table->pendingUses.count != 0)
    table->pendingUses = {};
  const MetalArgumentLayoutRecord* layout =
    m_resources->tryGetArgumentLayout(table->layout);
  if(layout == nullptr)
    throw std::runtime_error("Metal argument table references a stale layout");

  for(const ArgumentWrite& write : writes)
  {
    const ArgumentBinding* binding = findBinding(*layout, write.binding);
    if(binding == nullptr || binding->type != write.type ||
       write.arrayElement >= binding->arrayCount)
    {
      throw std::invalid_argument("Metal argument write does not match its layout");
    }
    if(usesBuffer(write.type))
    {
      const MetalBufferRecord* buffer = m_resources->tryGetBuffer(write.buffer);
      if(buffer == nullptr)
        throw std::invalid_argument("Metal argument write references a stale buffer");
      if(write.offset > buffer->desc.size ||
         (write.size != 0 && write.size > buffer->desc.size - write.offset))
      {
        throw std::out_of_range("Metal argument buffer range is invalid");
      }
    }
    if(usesTexture(write.type) &&
       m_resources->tryGetTextureView(write.textureView) == nullptr)
    {
      throw std::invalid_argument("Metal argument write references a stale texture view");
    }
    if(usesSampler(write.type) &&
       m_resources->tryGetSampler(write.sampler) == nullptr)
    {
      throw std::invalid_argument("Metal argument write references a stale sampler");
    }
    auto found = std::find_if(
      table->values.begin(), table->values.end(),
      [&write](const MetalArgumentValue& value) {
        return value.binding == write.binding &&
               value.arrayElement == write.arrayElement;
      });
    if(found == table->values.end())
      throw std::runtime_error("Metal argument table storage is inconsistent");
    *found = MetalArgumentValue{
      .binding = write.binding,
      .arrayElement = write.arrayElement,
      .type = write.type,
      .buffer = write.buffer,
      .textureView = write.textureView,
      .sampler = write.sampler,
      .offset = write.offset,
      .size = write.size,
      .accessIntent = write.accessIntent,
      .written = true,
    };
  }
  ++table->version;
  incrementHotPathCounter(BackendType::metal, HotPathCounter::descriptorUpdates,
                          writes.size());
}

ArgumentLayoutHandle MetalDevice::getArgumentTableLayout(
  ArgumentTableHandle table) const
{
  const MetalArgumentTableRecord* record = m_resources != nullptr
    ? m_resources->tryGetArgumentTable(table)
    : nullptr;
  return record != nullptr ? record->layout : ArgumentLayoutHandle{};
}

PipelineHandle MetalDevice::createGraphicsPipeline(
  const GraphicsPipelineDesc& desc)
{
  validateInitialized("MetalDevice::createGraphicsPipeline");
  MetalPipelineRecord record = compileMetalGraphicsPipeline(
    *m_device, *m_compiler, desc, *m_resources,
    m_createInfo.enableValidationLayers);
  incrementHotPathCounter(BackendType::metal, HotPathCounter::pipelineCreations);
  return m_resources->registerPipeline(std::move(record));
}

PipelineHandle MetalDevice::createComputePipeline(
  const ComputePipelineDesc& desc)
{
  validateInitialized("MetalDevice::createComputePipeline");
  MetalPipelineRecord record = compileMetalComputePipeline(
    *m_compiler, desc, *m_resources, m_createInfo.enableValidationLayers);
  incrementHotPathCounter(BackendType::metal, HotPathCounter::pipelineCreations);
  return m_resources->registerPipeline(std::move(record));
}

void MetalDevice::destroyPipeline(PipelineHandle handle)
{
  if(handle.isNull() || m_resources == nullptr)
    return;
  MetalPipelineRecord record = m_resources->removePipeline(handle);
  NS::Object* primary = record.kind == MetalPipelineKind::graphics
    ? asObject(record.nativeRenderPipeline)
    : asObject(record.nativeComputePipeline);
  enqueueRetirement(primary, record.pendingUses,
                    asObject(record.nativeDepthStencilState),
                    RetirementKind::pipeline);
}

QueryPoolHandle MetalDevice::createQueryPool(uint32_t queryCount)
{
  validateInitialized("MetalDevice::createQueryPool");
  if(queryCount == 0)
    throw std::invalid_argument("Metal query pool requires at least one query");
  auto* desc = MTL4::CounterHeapDescriptor::alloc()->init();
  desc->setType(MTL4::CounterHeapTypeTimestamp);
  desc->setCount(queryCount);
  NS::Error* error = nullptr;
  MTL4::CounterHeap* heap = m_device->newCounterHeap(desc, &error);
  desc->release();
  if(heap == nullptr)
    throw std::runtime_error(errorMessage("MTLDevice::newCounterHeap", error));
  heap->setLabel(nsString("Metal4 timestamp queries"));
  heap->invalidateCounterRange(NS::Range::Make(0, queryCount));
  return m_resources->registerQueryPool(MetalQueryPoolRecord{
    .nativeHeap = heap,
    .count = queryCount,
  });
}

void MetalDevice::destroyQueryPool(QueryPoolHandle handle)
{
  if(handle.isNull() || m_resources == nullptr)
    return;
  MetalQueryPoolRecord record = m_resources->removeQueryPool(handle);
  if(record.nativeHeap != nullptr)
    enqueueRetirement(asObject(record.nativeHeap), record.pendingUses);
}

uint64_t MetalDevice::getQueryPoolResult(
  QueryPoolHandle handle, uint32_t queryIndex)
{
  MetalQueryPoolRecord* record = m_resources != nullptr
    ? m_resources->tryGetQueryPool(handle)
    : nullptr;
  if(record == nullptr || queryIndex >= record->count)
    throw std::out_of_range("Metal query-pool result index is invalid");
  NS::Data* data = record->nativeHeap->resolveCounterRange(
    NS::Range::Make(queryIndex, 1));
  if(data == nullptr || data->length() < sizeof(MTL4::TimestampHeapEntry))
    return 0;
  const auto* value = static_cast<const MTL4::TimestampHeapEntry*>(
    immutableDataBytes(data));
  return value != nullptr ? value->timestamp : 0;
}

bool MetalDevice::getQueryPoolResultsWithAvailability(
  QueryPoolHandle handle, uint32_t firstQuery,
  std::span<uint64_t> outValueAvailabilityPairs)
{
  MetalQueryPoolRecord* record = m_resources != nullptr
    ? m_resources->tryGetQueryPool(handle)
    : nullptr;
  if(record == nullptr || (outValueAvailabilityPairs.size() & 1u) != 0)
    return false;
  const uint32_t count = static_cast<uint32_t>(
    outValueAvailabilityPairs.size() / 2);
  if(firstQuery > record->count || count > record->count - firstQuery)
    return false;
  if(count == 0)
    return true;
  NS::Data* data = record->nativeHeap->resolveCounterRange(
    NS::Range::Make(firstQuery, count));
  if(data == nullptr || data->length() < count * sizeof(MTL4::TimestampHeapEntry))
    return false;
  const auto* values = static_cast<const MTL4::TimestampHeapEntry*>(
    immutableDataBytes(data));
  if(values == nullptr)
    return false;
  for(uint32_t index = 0; index < count; ++index)
  {
    outValueAvailabilityPairs[index * 2] = values[index].timestamp;
    outValueAvailabilityPairs[index * 2 + 1] = values[index].timestamp != 0 ? 1 : 0;
  }
  return true;
}

ShaderLibraryHandle MetalDevice::createShaderLibrary(
  const ShaderLibraryDesc& desc)
{
  validateInitialized("MetalDevice::createShaderLibrary");
  if(desc.format != ShaderIRFormat::metalLibrary || desc.data.empty())
    throw std::invalid_argument("Metal shader library requires a nonempty metallib payload");
  dispatch_data_t payload = dispatch_data_create(
    desc.data.data(), desc.data.size_bytes(),
    dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  if(payload == nullptr)
    throw std::runtime_error("Metal failed to wrap metallib data");
  NS::Error* error = nullptr;
  MTL::Library* library = m_device->newLibrary(payload, &error);
  dispatch_release(payload);
  if(library == nullptr)
    throw std::runtime_error(errorMessage("MTLDevice::newLibrary(metallib)", error));
  if(desc.debugName != nullptr)
    library->setLabel(nsString(desc.debugName));
  return m_resources->registerShaderLibrary(MetalShaderLibraryRecord{
    .nativeLibrary = library,
    .format = desc.format,
    .debugName = desc.debugName != nullptr ? desc.debugName : "",
  });
}

void MetalDevice::destroyShaderLibrary(ShaderLibraryHandle handle)
{
  if(handle.isNull() || m_resources == nullptr)
    return;
  MetalShaderLibraryRecord record = m_resources->removeShaderLibrary(handle);
  if(record.nativeLibrary != nullptr)
    enqueueRetirement(asObject(record.nativeLibrary), record.pendingUses);
}

MTL::Allocation* MetalDevice::resolveAllocation(
  ResidencyResource resource) const
{
  switch(resource.kind)
  {
  case ResidencyResourceKind::buffer:
  {
    const auto* record = m_resources->tryGetBuffer(
      BufferHandle{resource.index, resource.generation});
    return record != nullptr
      ? reinterpret_cast<MTL::Allocation*>(record->nativeBuffer)
      : nullptr;
  }
  case ResidencyResourceKind::texture:
  {
    const auto* record = m_resources->tryGetTexture(
      TextureHandle{resource.index, resource.generation});
    return record != nullptr
      ? reinterpret_cast<MTL::Allocation*>(record->nativeTexture)
      : nullptr;
  }
  case ResidencyResourceKind::textureView:
  {
    const auto* record = m_resources->tryGetTextureView(
      TextureViewHandle{resource.index, resource.generation});
    return record != nullptr
      ? reinterpret_cast<MTL::Allocation*>(record->nativeTextureView)
      : nullptr;
  }
  case ResidencyResourceKind::pipeline:
  {
    const auto* record = m_resources->tryGetPipeline(
      PipelineHandle{resource.index, resource.generation});
    if(record == nullptr)
      return nullptr;
    return record->kind == MetalPipelineKind::graphics
      ? reinterpret_cast<MTL::Allocation*>(record->nativeRenderPipeline)
      : reinterpret_cast<MTL::Allocation*>(record->nativeComputePipeline);
  }
  case ResidencyResourceKind::sampler:
  case ResidencyResourceKind::shaderLibrary:
  case ResidencyResourceKind::argumentTable:
    return nullptr;
  }
  return nullptr;
}

Result<ResidencySetHandle> MetalDevice::createResidencySet(
  const ResidencySetDesc& desc)
{
  if(!m_initialized)
    return Result<ResidencySetHandle>::fail(
      RHIErrorCode::invalidState, "Metal device is not initialized");
  if(desc.maxResources == 0)
    return Result<ResidencySetHandle>::fail(
      RHIErrorCode::invalidArgument, "Metal residency set requires capacity");
  auto* nativeDesc = MTL::ResidencySetDescriptor::alloc()->init();
  nativeDesc->setInitialCapacity(desc.maxResources);
  if(desc.debugName != nullptr)
    nativeDesc->setLabel(nsString(desc.debugName));
  NS::Error* error = nullptr;
  MTL::ResidencySet* set = m_device->newResidencySet(nativeDesc, &error);
  nativeDesc->release();
  if(set == nullptr)
    return Result<ResidencySetHandle>::fail(
      RHIErrorCode::backendFailure, "Metal failed to create residency set");
  set->commit();
  return Result<ResidencySetHandle>::ok(
    m_resources->registerResidencySet(MetalResidencySetRecord{
      .nativeSet = set,
      .maxResources = desc.maxResources,
    }));
}

RHIResult MetalDevice::destroyResidencySet(ResidencySetHandle handle)
{
  if(!m_initialized)
    return RHIResult::fail(RHIErrorCode::invalidState, "Metal device is not initialized");
  MetalResidencySetRecord record = m_resources->removeResidencySet(handle);
  if(record.nativeSet == nullptr)
    return RHIResult::fail(RHIErrorCode::invalidHandle, "Metal residency set is stale");
  record.nativeSet->endResidency();
  enqueueRetirement(asObject(record.nativeSet), record.pendingUses);
  return RHIResult::ok();
}

RHIResult MetalDevice::updateResidencySet(
  ResidencySetHandle handle, const ResidencyUpdateBatch& batch)
{
  if(!m_initialized)
    return RHIResult::fail(RHIErrorCode::invalidState, "Metal device is not initialized");
  MetalResidencySetRecord* record = m_resources->tryGetResidencySet(handle);
  if(record == nullptr || record->nativeSet == nullptr)
    return RHIResult::fail(RHIErrorCode::invalidHandle, "Metal residency set is stale");
  if(!dependenciesComplete(record->pendingUses))
    return RHIResult::fail(RHIErrorCode::invalidState,
                           "Metal residency set has pending GPU work");

  std::vector<ResidencyResource> next = record->resources;
  std::vector<const MTL::Allocation*> removals;
  std::vector<const MTL::Allocation*> additions;
  for(ResidencyResource resource : batch.remove)
  {
    const auto found = std::find(next.begin(), next.end(), resource);
    if(found == next.end())
      return RHIResult::fail(RHIErrorCode::invalidArgument,
                             "Metal residency removal is not in the set");
    MTL::Allocation* allocation = resolveAllocation(resource);
    if(allocation == nullptr)
      return RHIResult::fail(RHIErrorCode::invalidHandle,
                             "Metal residency removal is stale or unsupported");
    removals.push_back(allocation);
    next.erase(found);
  }
  for(ResidencyResource resource : batch.add)
  {
    if(std::find(next.begin(), next.end(), resource) != next.end())
      return RHIResult::fail(RHIErrorCode::invalidArgument,
                             "Metal residency addition is duplicated");
    MTL::Allocation* allocation = resolveAllocation(resource);
    if(allocation == nullptr)
      return RHIResult::fail(RHIErrorCode::unsupported,
                             "Metal residency resource is not an allocation");
    additions.push_back(allocation);
    next.push_back(resource);
  }
  if(next.size() > record->maxResources)
    return RHIResult::fail(RHIErrorCode::outOfMemory,
                           "Metal residency set capacity exceeded");
  if(!removals.empty())
    record->nativeSet->removeAllocations(removals.data(), removals.size());
  if(!additions.empty())
    record->nativeSet->addAllocations(additions.data(), additions.size());
  record->nativeSet->commit();
  record->resources = std::move(next);
  record->pendingUses = {};
  return RHIResult::ok();
}

TextureHandle MetalDevice::registerSwapchainTexture(
  MTL::Texture* texture, const TextureDesc& desc)
{
  validateInitialized("MetalDevice::registerSwapchainTexture");
  if(texture == nullptr)
    throw std::invalid_argument("Metal swapchain texture is null");
  texture->retain();
  TextureDesc storedDesc = desc;
  storedDesc.debugName = nullptr;
  return m_resources->registerTexture(MetalTextureRecord{
    .nativeTexture = texture,
    .desc = storedDesc,
    .debugName = desc.debugName != nullptr ? desc.debugName : "",
    .owned = true,
  });
}

void MetalDevice::unregisterSwapchainTexture(TextureHandle handle)
{
  destroyTexture(handle);
}

void MetalDevice::releaseActiveResources()
{
  for(const ResidencySetHandle handle : collectHandles<ResidencySetHandle>(
        [this](auto&& fn) { m_resources->forEachResidencySet(fn); }))
    (void)destroyResidencySet(handle);
  for(const ArgumentTableHandle handle : collectHandles<ArgumentTableHandle>(
        [this](auto&& fn) { m_resources->forEachArgumentTable(fn); }))
    destroyArgumentTable(handle);
  for(const PipelineHandle handle : collectHandles<PipelineHandle>(
        [this](auto&& fn) { m_resources->forEachPipeline(fn); }))
    destroyPipeline(handle);
  for(const QueryPoolHandle handle : collectHandles<QueryPoolHandle>(
        [this](auto&& fn) { m_resources->forEachQueryPool(fn); }))
    destroyQueryPool(handle);
  for(const TextureViewHandle handle : collectHandles<TextureViewHandle>(
        [this](auto&& fn) { m_resources->forEachTextureView(fn); }))
    destroyTextureView(handle);
  for(const SamplerHandle handle : collectHandles<SamplerHandle>(
        [this](auto&& fn) { m_resources->forEachSampler(fn); }))
    destroySampler(handle);
  for(const ShaderLibraryHandle handle : collectHandles<ShaderLibraryHandle>(
        [this](auto&& fn) { m_resources->forEachShaderLibrary(fn); }))
    destroyShaderLibrary(handle);
  for(const TextureHandle handle : collectHandles<TextureHandle>(
        [this](auto&& fn) { m_resources->forEachTexture(fn); }))
    destroyTexture(handle);
  for(const BufferHandle handle : collectHandles<BufferHandle>(
        [this](auto&& fn) { m_resources->forEachBuffer(fn); }))
    destroyBuffer(handle);
  for(const ArgumentLayoutHandle handle : collectHandles<ArgumentLayoutHandle>(
        [this](auto&& fn) { m_resources->forEachArgumentLayout(fn); }))
    destroyArgumentLayout(handle);
}

}  // namespace demo::rhi::metal
