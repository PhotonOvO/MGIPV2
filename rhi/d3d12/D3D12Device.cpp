#include "D3D12Device.h"

#include "D3D12CommandAllocator.h"
#include "D3D12Queue.h"
#include "D3D12Surface.h"
#include "D3D12Swapchain.h"

#include "../../common/HandlePool.h"

#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <array>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>

namespace demo::rhi::d3d12 {
namespace {

template<typename T>
void releaseNative(void*& object) noexcept
{
  if(object != nullptr)
  {
    static_cast<T*>(object)->Release();
    object = nullptr;
  }
}

void CALLBACK reportD3D12Message(
  D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
  D3D12_MESSAGE_ID id, LPCSTR description, void*)
{
  if(severity > D3D12_MESSAGE_SEVERITY_WARNING ||
     id == D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE ||
     id == D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE)
    return;
  std::fprintf(stderr, "[D3D12-VALIDATION] severity=%u id=%u %s\n",
               static_cast<unsigned>(severity), static_cast<unsigned>(id),
               description != nullptr ? description : "<no description>");
  std::fflush(stderr);
}

void checkHresult(HRESULT result, const char* operation)
{
  if(FAILED(result))
  {
    std::ostringstream message;
    message << operation << " failed (HRESULT=0x" << std::hex << static_cast<uint32_t>(result) << ')';
    throw std::runtime_error(message.str());
  }
}

std::string narrow(const wchar_t* value)
{
  if(value == nullptr || value[0] == L'\0')
    return {};

  const int required = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if(required <= 1)
    return {};

  std::string result(static_cast<size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), required, nullptr, nullptr);
  result.pop_back();
  return result;
}

DXGI_FORMAT toDxgiFormat(TextureFormat format)
{
  switch(format)
  {
  case TextureFormat::rgba8Unorm:       return DXGI_FORMAT_R8G8B8A8_UNORM;
  case TextureFormat::rgba8Srgb:        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  case TextureFormat::d16Unorm:        return DXGI_FORMAT_D16_UNORM;
  case TextureFormat::d32Sfloat:       return DXGI_FORMAT_D32_FLOAT;
  case TextureFormat::d24UnormS8:      return DXGI_FORMAT_D24_UNORM_S8_UINT;
  case TextureFormat::d32SfloatS8:     return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  case TextureFormat::bgra8Unorm:       return DXGI_FORMAT_B8G8R8A8_UNORM;
  case TextureFormat::rgba16Sfloat:     return DXGI_FORMAT_R16G16B16A16_FLOAT;
  case TextureFormat::rg16Sfloat:       return DXGI_FORMAT_R16G16_FLOAT;
  case TextureFormat::r32Sfloat:        return DXGI_FORMAT_R32_FLOAT;
  case TextureFormat::r16Sfloat:        return DXGI_FORMAT_R16_FLOAT;
  case TextureFormat::rgba8Snorm:       return DXGI_FORMAT_R8G8B8A8_SNORM;
  case TextureFormat::r11g11b10Ufloat:  return DXGI_FORMAT_R11G11B10_FLOAT;
  case TextureFormat::bc6hUfloatBlock:  return DXGI_FORMAT_BC6H_UF16;
  case TextureFormat::bc6hSfloatBlock:  return DXGI_FORMAT_BC6H_SF16;
  case TextureFormat::bc7UnormBlock:    return DXGI_FORMAT_BC7_UNORM;
  case TextureFormat::bc7SrgbBlock:     return DXGI_FORMAT_BC7_UNORM_SRGB;
  default:                              return DXGI_FORMAT_UNKNOWN;
  }
}

bool isDepthFormat(TextureFormat format)
{
  return format == TextureFormat::d16Unorm ||
         format == TextureFormat::d32Sfloat ||
         format == TextureFormat::d24UnormS8 ||
         format == TextureFormat::d32SfloatS8;
}

bool hasStencil(TextureFormat format)
{
  return format == TextureFormat::d24UnormS8 ||
         format == TextureFormat::d32SfloatS8;
}

DXGI_FORMAT toResourceFormat(TextureFormat format, bool shaderReadable)
{
  if(!shaderReadable)
    return toDxgiFormat(format);

  switch(format)
  {
  case TextureFormat::d16Unorm:    return DXGI_FORMAT_R16_TYPELESS;
  case TextureFormat::d32Sfloat:   return DXGI_FORMAT_R32_TYPELESS;
  case TextureFormat::d24UnormS8:  return DXGI_FORMAT_R24G8_TYPELESS;
  case TextureFormat::d32SfloatS8: return DXGI_FORMAT_R32G8X24_TYPELESS;
  default:                         return toDxgiFormat(format);
  }
}

DXGI_FORMAT toSrvFormat(TextureFormat format)
{
  switch(format)
  {
  case TextureFormat::d16Unorm:    return DXGI_FORMAT_R16_UNORM;
  case TextureFormat::d32Sfloat:   return DXGI_FORMAT_R32_FLOAT;
  case TextureFormat::d24UnormS8:  return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
  case TextureFormat::d32SfloatS8: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  default:                         return toDxgiFormat(format);
  }
}

DXGI_FORMAT toDsvFormat(TextureFormat format)
{
  return isDepthFormat(format) ? toDxgiFormat(format) : DXGI_FORMAT_UNKNOWN;
}

TextureFormat fromDxgiFormat(DXGI_FORMAT format)
{
  switch(format)
  {
  case DXGI_FORMAT_R8G8B8A8_UNORM: return TextureFormat::rgba8Unorm;
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return TextureFormat::rgba8Srgb;
  case DXGI_FORMAT_B8G8R8A8_UNORM: return TextureFormat::bgra8Unorm;
  case DXGI_FORMAT_R16G16B16A16_FLOAT: return TextureFormat::rgba16Sfloat;
  case DXGI_FORMAT_D16_UNORM: return TextureFormat::d16Unorm;
  case DXGI_FORMAT_D32_FLOAT: return TextureFormat::d32Sfloat;
  case DXGI_FORMAT_D24_UNORM_S8_UINT: return TextureFormat::d24UnormS8;
  default: return TextureFormat::undefined;
  }
}

bool hasUsage(TextureUsageFlags value, TextureUsageFlags bit)
{
  return static_cast<uint32_t>(value & bit) != 0;
}

D3D12_TEXTURE_ADDRESS_MODE toD3D12AddressMode(AddressMode mode)
{
  switch(mode)
  {
  case AddressMode::clampToEdge:    return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  case AddressMode::clampToBorder:  return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
  case AddressMode::mirroredRepeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
  case AddressMode::repeat:
  default:                          return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  }
}

D3D12_COMPARISON_FUNC toD3D12Comparison(CompareOp op)
{
  return static_cast<D3D12_COMPARISON_FUNC>(static_cast<uint32_t>(op) + 1u);
}

D3D12_FILTER toD3D12Filter(const SamplerDesc& desc)
{
  if(desc.anisotropyEnable)
    return desc.compareEnable ? D3D12_FILTER_COMPARISON_ANISOTROPIC
                              : D3D12_FILTER_ANISOTROPIC;

  uint32_t value = 0;
  if(desc.mipmapMode == MipmapMode::linear)
    value |= 0x1u;
  if(desc.magFilter == Filter::linear)
    value |= 0x4u;
  if(desc.minFilter == Filter::linear)
    value |= 0x10u;
  if(desc.compareEnable)
    value |= 0x80u;
  return static_cast<D3D12_FILTER>(value);
}

bool argumentUsesResourceHeap(ArgumentType type)
{
  return type != ArgumentType::sampler;
}

bool argumentUsesSamplerHeap(ArgumentType type)
{
  return type == ArgumentType::sampler || type == ArgumentType::combinedImageSampler;
}

uint32_t descriptorCountForHeap(const ArgumentBinding& binding, bool)
{
  return binding.arrayCount;
}
UINT componentMapping(const ComponentMapping& mapping)
{
  const auto component = [](ComponentSwizzle swizzle, UINT identity) -> UINT
  {
    switch(swizzle)
    {
    case ComponentSwizzle::zero: return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
    case ComponentSwizzle::one:  return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
    case ComponentSwizzle::r:    return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0;
    case ComponentSwizzle::g:    return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1;
    case ComponentSwizzle::b:    return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2;
    case ComponentSwizzle::a:    return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3;
    default:                     return identity;
    }
  };

  return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
    component(mapping.r, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0),
    component(mapping.g, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1),
    component(mapping.b, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2),
    component(mapping.a, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3));
}

void setDebugName(ID3D12Object* object, const char* debugName)
{
  if(debugName == nullptr || debugName[0] == '\0')
    return;

  const int count = MultiByteToWideChar(CP_UTF8, 0, debugName, -1, nullptr, 0);
  if(count <= 1)
    return;

  std::wstring name(static_cast<size_t>(count), L'\0');
  if(MultiByteToWideChar(CP_UTF8, 0, debugName, -1, name.data(), count) > 0)
    object->SetName(name.c_str());
}

}  // namespace

QueueInfo D3D12Device::NativeQueueInfo::toRhi() const
{
  return {
    .queueClass = queueClass,
    .capabilities = {
      .supportsTimestamps = true,
      .supportsPresent = queueClass == QueueClass::graphics,
      .supportsSparseBinding = false,
    },
    .dedicated = dedicated,
    .available = commandQueue != nullptr,
  };
}

D3D12Device::~D3D12Device()
{
  deinit();
}

void D3D12Device::init(const DeviceCreateInfo& createInfo)
{
  if(m_initialized)
    throw std::runtime_error("D3D12Device::init called twice");

  m_handleGeneration = demo::detail::encodeHandleGeneration(
    demo::detail::acquireHandlePoolOwner(), 1);
  m_createInfo = createInfo;

  try
  {
    UINT factoryFlags = 0;
    if(createInfo.enableValidationLayers)
    {
      ID3D12Debug* debugController = nullptr;
      checkHresult(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)),
                   "D3D12GetDebugInterface");
      debugController->EnableDebugLayer();
      debugController->Release();
      m_validationEnabled = true;
      factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }

    IDXGIFactory4* factory = nullptr;
    HRESULT factoryResult = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory));
    if(FAILED(factoryResult) && factoryFlags != 0)
      factoryResult = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    checkHresult(factoryResult, "CreateDXGIFactory2");
    m_dxgiFactory = factory;

    selectD3D12Adapter();
    initD3D12Device();

    auto* nativeDevice = static_cast<ID3D12Device*>(m_d3d12Device);
    const auto createIndirectSignature =
      [nativeDevice](D3D12_INDIRECT_ARGUMENT_TYPE argumentType,
                     UINT byteStride, void** output)
    {
      const D3D12_INDIRECT_ARGUMENT_DESC argument{.Type = argumentType};
      const D3D12_COMMAND_SIGNATURE_DESC signature{
        .ByteStride = byteStride,
        .NumArgumentDescs = 1,
        .pArgumentDescs = &argument,
        .NodeMask = 0,
      };
      ID3D12CommandSignature* nativeSignature = nullptr;
      checkHresult(nativeDevice->CreateCommandSignature(
                     &signature, nullptr, IID_PPV_ARGS(&nativeSignature)),
                   "ID3D12Device::CreateCommandSignature");
      *output = nativeSignature;
    };
    createIndirectSignature(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW,
                            sizeof(D3D12_DRAW_ARGUMENTS),
                            &m_drawSignature);
    createIndirectSignature(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED,
                            sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
                            &m_drawIndexedSignature);
    createIndirectSignature(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH,
                            sizeof(D3D12_DISPATCH_ARGUMENTS),
                            &m_dispatchSignature);

    if(m_validationEnabled)
    {
      ID3D12InfoQueue1* infoQueue = nullptr;
      checkHresult(static_cast<ID3D12Device*>(m_d3d12Device)->QueryInterface(IID_PPV_ARGS(&infoQueue)),
                   "ID3D12Device::QueryInterface(ID3D12InfoQueue1)");
      DWORD callbackCookie = 0;
      checkHresult(infoQueue->RegisterMessageCallback(
                     reportD3D12Message, D3D12_MESSAGE_CALLBACK_FLAG_NONE,
                     nullptr, &callbackCookie),
                   "ID3D12InfoQueue1::RegisterMessageCallback");
      m_infoQueue = infoQueue;
      m_infoQueueCallbackCookie = callbackCookie;
    }
    initD3D12Queues();

    m_graphicsQueueApi = std::make_unique<D3D12Queue>();
    m_computeQueueApi = std::make_unique<D3D12Queue>();
    m_transferQueueApi = std::make_unique<D3D12Queue>();
    m_graphicsQueueApi->init(
      m_d3d12Device, m_graphicsQueue.commandQueue, D3D12_COMMAND_LIST_TYPE_DIRECT,
      QueueIdentity{1, m_handleGeneration}, m_graphicsQueue.toRhi());
    m_computeQueueApi->init(
      m_d3d12Device, m_computeQueue.commandQueue, D3D12_COMMAND_LIST_TYPE_COMPUTE,
      QueueIdentity{2, m_handleGeneration}, m_computeQueue.toRhi());
    m_transferQueueApi->init(
      m_d3d12Device, m_transferQueue.commandQueue, D3D12_COMMAND_LIST_TYPE_COPY,
      QueueIdentity{3, m_handleGeneration}, m_transferQueue.toRhi());
    const std::array<D3D12Queue*, 3> queueRegistry{
      m_graphicsQueueApi.get(), m_computeQueueApi.get(), m_transferQueueApi.get()
    };
    m_graphicsQueueApi->setQueueRegistry(queueRegistry);
    m_computeQueueApi->setQueueRegistry(queueRegistry);
    m_transferQueueApi->setQueueRegistry(queueRegistry);

    initDescriptorHeaps();
    detectD3D12Capabilities();
    validateD3D12Capabilities();

    m_initialized = true;
  }
  catch(...)
  {
    deinit();
    throw;
  }
}

void D3D12Device::deinit()
{
  if(m_initialized)
  {
    try
    {
      waitIdle();
    }
    catch(...)
    {
      // Destructors must remain non-throwing; native objects are still released below.
    }
  }

  releaseNativeObjects();
  m_initialized = false;
  m_createInfo = {};
  m_apiVersion = 0;
  m_d3dFeatureLevel = 0;
  m_shaderModel = 0;
  m_resourceBindingTier = 0;
  m_physicalDeviceInfo = {};
  m_featureInfo = {};
  m_capabilities = {};
  m_memoryProperties = {};
  m_supportsMeshShaders = false;
  m_supportsRayTracing = false;
  m_supportsEnhancedBarriers = false;
  m_validationEnabled = false;
}

BackendInfo D3D12Device::getBackendInfo() const
{
  uint32_t minor = 0;
  if(m_d3dFeatureLevel >= static_cast<uint32_t>(D3D_FEATURE_LEVEL_12_2))
    minor = 2;
  else if(m_d3dFeatureLevel >= static_cast<uint32_t>(D3D_FEATURE_LEVEL_12_1))
    minor = 1;
  return BackendInfo{
    .type = BackendType::d3d12,
    .apiName = "Direct3D 12",
    .version = BackendVersion{
      .major = 12,
      .minor = minor,
      .nativeValue = m_d3dFeatureLevel,
    },
  };
}

const char* D3D12Device::getDeviceName() const
{
  return m_physicalDeviceInfo.deviceName.c_str();
}

const PhysicalDeviceInfo& D3D12Device::getPhysicalDeviceInfo() const
{
  return m_physicalDeviceInfo;
}

const DeviceFeatureInfo& D3D12Device::getEnabledFeatureInfo() const
{
  return m_featureInfo;
}

CapabilityReport D3D12Device::queryCapabilities() const
{
  return m_capabilities;
}

bool D3D12Device::supports(CapabilityTier tier) const
{
  return supportsTier(m_capabilities, tier);
}

bool D3D12Device::isFormatSupported(
  TextureFormat format, FormatFeatureFlag feature) const
{
  if(!m_initialized)
    return false;
  const DXGI_FORMAT nativeFormat = toDxgiFormat(format);
  if(nativeFormat == DXGI_FORMAT_UNKNOWN)
    return false;

  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{
    .Format = nativeFormat,
  };
  if(FAILED(static_cast<ID3D12Device*>(m_d3d12Device)->CheckFeatureSupport(
              D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
    return false;

  const uint32_t requested = static_cast<uint32_t>(feature);
  if((requested & static_cast<uint32_t>(FormatFeatureFlag::sampledImage)) != 0 &&
     (support.Support1 & (D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE |
                          D3D12_FORMAT_SUPPORT1_SHADER_LOAD)) == 0)
    return false;
  if((requested & static_cast<uint32_t>(FormatFeatureFlag::storageImage)) != 0 &&
     (support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) == 0)
    return false;
  if((requested & static_cast<uint32_t>(FormatFeatureFlag::colorAttachment)) != 0 &&
     (support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) == 0)
    return false;
  if((requested & static_cast<uint32_t>(FormatFeatureFlag::depthStencilAttachment)) != 0 &&
     (support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) == 0)
    return false;
  return true;
}

const MemoryProperties& D3D12Device::getPhysicalMemoryProperties() const
{
  return m_memoryProperties;
}

Queue* D3D12Device::getQueue(QueueClass queueClass)
{
  switch(queueClass)
  {
  case QueueClass::graphics: return m_graphicsQueueApi.get();
  case QueueClass::compute: return m_computeQueueApi.get();
  case QueueClass::transfer: return m_transferQueueApi.get();
  }
  return nullptr;
}

std::unique_ptr<CommandAllocator> D3D12Device::createCommandAllocator(
  QueueClass queueClass)
{
  auto* queue = dynamic_cast<D3D12Queue*>(getQueue(queueClass));
  if(queue == nullptr)
    throw std::runtime_error(
      "D3D12Device::createCommandAllocator requires an available queue");
  return std::make_unique<D3D12CommandAllocator>(*this, *queue);
}

void D3D12Device::collectGarbage()
{
  std::erase_if(m_retiredBuffers, [this](RetiredBuffer& retired) {
    if(!isRetirementComplete(retired.retirementDependencies)) return false;
    releaseRetiredBuffer(retired.record);
    return true;
  });
  std::erase_if(m_retiredTextures, [this](RetiredTexture& retired) {
    if(!isRetirementComplete(retired.retirementDependencies)) return false;
    releaseRetiredTexture(retired.record);
    return true;
  });
  std::erase_if(m_retiredTextureViews, [this](RetiredTextureView& retired) {
    if(!isRetirementComplete(retired.retirementDependencies)) return false;
    releaseRetiredTextureView(retired.record);
    return true;
  });
  std::erase_if(m_retiredQueryPools, [this](RetiredQueryPool& retired) {
    if(!isRetirementComplete(retired.retirementDependencies)) return false;
    releaseRetiredQueryPool(retired.record);
    return true;
  });
  std::erase_if(m_retiredPipelines, [this](RetiredPipeline& retired) {
    if(!isRetirementComplete(retired.retirementDependencies)) return false;
    releaseRetiredPipeline(retired.record);
    return true;
  });
}

SubmissionTokenSet D3D12Device::retirementDependencies() const
{
  SubmissionTokenSet dependencies{};
  const std::array<D3D12Queue*, 3> queues{
    m_graphicsQueueApi.get(), m_computeQueueApi.get(), m_transferQueueApi.get()};
  for(const D3D12Queue* queue : queues)
  {
    if(queue != nullptr && !dependencies.record(queue->lastSubmittedToken()))
      throw std::runtime_error("D3D12 retirement dependency capacity exceeded");
  }
  return dependencies;
}

bool D3D12Device::isRetirementComplete(
  const SubmissionTokenSet& dependencies) const
{
  const std::array<D3D12Queue*, 3> queues{
    m_graphicsQueueApi.get(), m_computeQueueApi.get(), m_transferQueueApi.get()};
  for(uint32_t dependencyIndex = 0;
      dependencyIndex < dependencies.count; ++dependencyIndex)
  {
    const SubmissionToken dependency = dependencies.tokens[dependencyIndex];
    bool complete = false;
    for(const D3D12Queue* queue : queues)
    {
      if(queue == nullptr || queue->identity().index != dependency.queue.index)
        continue;
      if(queue->identity() != dependency.queue)
      {
        // Fence recreation waits the old queue idle before changing identity.
        complete = true;
      }
      else
      {
        complete = queue->completedValue() >= dependency.value;
      }
      break;
    }
    if(!complete)
      return false;
  }
  return true;
}

void D3D12Device::releaseRetiredBuffer(BufferRecord& record) noexcept
{
  auto* resource = static_cast<ID3D12Resource*>(record.resource);
  if(resource == nullptr)
    return;
  if(record.mapped != nullptr)
    resource->Unmap(0, nullptr);
  resource->Release();
  record = {};
}

void D3D12Device::releaseRetiredTexture(TextureRecord& record) noexcept
{
  if(record.owned && record.resource != nullptr)
    static_cast<ID3D12Resource*>(record.resource)->Release();
  record = {};
}

void D3D12Device::releaseRetiredTextureView(TextureViewRecord& record) noexcept
{
  if(record.ownsResourceReference && record.resource != nullptr)
    static_cast<ID3D12Resource*>(record.resource)->Release();
  if(record.ownsDescriptor)
    m_freeTextureViewDescriptors.push_back(record.descriptorIndex);
  if(record.ownsAttachmentDescriptor)
  {
    const bool depthView = record.desc.aspect != TextureAspect::color;
    (depthView ? m_freeDsvDescriptors : m_freeRtvDescriptors)
      .push_back(record.attachmentDescriptorIndex);
  }
  record = {};
}

void D3D12Device::releaseRetiredQueryPool(QueryPoolRecord& record) noexcept
{
  if(record.readbackBuffer != nullptr)
    static_cast<ID3D12Resource*>(record.readbackBuffer)->Release();
  if(record.queryHeap != nullptr)
    static_cast<ID3D12QueryHeap*>(record.queryHeap)->Release();
  record = {};
}
void D3D12Device::releaseRetiredPipeline(PipelineRecord& record) noexcept
{
  if(record.indexedIndirectSignature != nullptr)
    static_cast<ID3D12CommandSignature*>(record.indexedIndirectSignature)->Release();
  if(record.pipelineState != nullptr)
    static_cast<ID3D12PipelineState*>(record.pipelineState)->Release();
  if(record.rootSignature != nullptr)
    static_cast<ID3D12RootSignature*>(record.rootSignature)->Release();
  record = {};
}

void D3D12Device::drainRetirements() noexcept
{
  for(RetiredBuffer& retired : m_retiredBuffers)
    releaseRetiredBuffer(retired.record);
  for(RetiredTexture& retired : m_retiredTextures)
    releaseRetiredTexture(retired.record);
  for(RetiredTextureView& retired : m_retiredTextureViews)
    releaseRetiredTextureView(retired.record);
  for(RetiredQueryPool& retired : m_retiredQueryPools)
    releaseRetiredQueryPool(retired.record);
  for(RetiredPipeline& retired : m_retiredPipelines)
    releaseRetiredPipeline(retired.record);
  m_retiredBuffers.clear();
  m_retiredTextures.clear();
  m_retiredTextureViews.clear();
  m_retiredQueryPools.clear();
  m_retiredPipelines.clear();
}

void D3D12Device::initSurface(Surface& surface, const WindowHandle& window)
{
  if(!m_initialized)
    throw std::runtime_error("D3D12Device::initSurface called before device initialization");

  auto* d3dSurface = dynamic_cast<D3D12Surface*>(&surface);
  if(d3dSurface == nullptr)
    throw std::runtime_error("D3D12Device::initSurface received a surface from another backend");

  d3dSurface->initD3D12(m_dxgiFactory, m_dxgiAdapter, window);
}

std::unique_ptr<Swapchain> D3D12Device::createSwapchain(Surface& surface, bool vSync)
{
  if(!m_initialized)
    throw std::runtime_error("D3D12Device::createSwapchain called before device initialization");

  auto* d3dSurface = dynamic_cast<D3D12Surface*>(&surface);
  if(d3dSurface == nullptr || d3dSurface->nativeWindow() == nullptr)
    throw std::runtime_error("D3D12Device::createSwapchain requires an initialized D3D12 surface");

  auto swapchain = std::make_unique<D3D12Swapchain>();
  swapchain->init(this, m_dxgiFactory, m_d3d12Device, m_graphicsQueue.commandQueue,
                  d3dSurface->nativeWindow(), vSync);
  return swapchain;
}


void D3D12Device::waitIdle()
{
  if(!m_initialized)
    return;

  ID3D12Device* device = static_cast<ID3D12Device*>(m_d3d12Device);
  ID3D12Fence* fence = nullptr;
  checkHresult(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
               "ID3D12Device::CreateFence");

  HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if(eventHandle == nullptr)
  {
    fence->Release();
    throw std::runtime_error("CreateEventW failed while waiting for D3D12 queues");
  }

  try
  {
    const std::array<NativeQueueInfo*, 3> queues{
      &m_graphicsQueue,
      &m_computeQueue,
      &m_transferQueue,
    };

    uint64_t fenceValue = 0;
    for(const NativeQueueInfo* queueInfo : queues)
    {
      if(queueInfo->commandQueue == nullptr)
        continue;

      ++fenceValue;
      auto* queue = static_cast<ID3D12CommandQueue*>(queueInfo->commandQueue);
      checkHresult(queue->Signal(fence, fenceValue), "ID3D12CommandQueue::Signal");

      if(fence->GetCompletedValue() < fenceValue)
      {
        checkHresult(fence->SetEventOnCompletion(fenceValue, eventHandle),
                     "ID3D12Fence::SetEventOnCompletion");
        WaitForSingleObject(eventHandle, INFINITE);
      }
    }
  }
  catch(...)
  {
    CloseHandle(eventHandle);
    fence->Release();
    throw;
  }

  CloseHandle(eventHandle);
  fence->Release();
  drainRetirements();
}

TextureViewHandle D3D12Device::createTextureView(const TextureViewCreateDesc& desc)
{
  incrementHotPathCounter(BackendType::d3d12, HotPathCounter::textureViewCreations);
  if(!m_initialized)
    throw std::runtime_error("D3D12Device::createTextureView called before device initialization");
  if(desc.image.isNull() || desc.image.generation != m_handleGeneration)
    throw std::runtime_error("D3D12Device::createTextureView requires a valid owned texture");

  const auto texture = m_textures.find(desc.image.index);
  if(texture == m_textures.end())
    throw std::runtime_error("D3D12Device::createTextureView received a stale texture handle");
  const TextureDesc& textureDesc = texture->second.desc;
  const bool depthView = desc.aspect != TextureAspect::color;
  if(depthView != isDepthFormat(desc.format))
    throw std::runtime_error("D3D12Device::createTextureView aspect does not match its format");
  if(desc.format != textureDesc.format)
    throw std::runtime_error("D3D12Device::createTextureView does not support format reinterpretation");
  if(desc.levelCount == 0 || desc.baseMipLevel >= textureDesc.mipLevels ||
     desc.levelCount > textureDesc.mipLevels - desc.baseMipLevel)
    throw std::runtime_error("D3D12Device::createTextureView received an invalid mip range");
  if(desc.layerCount == 0 || desc.baseArrayLayer >= textureDesc.arrayLayers ||
     desc.layerCount > textureDesc.arrayLayers - desc.baseArrayLayer)
    throw std::runtime_error("D3D12Device::createTextureView received an invalid array range");
  if(m_nextTextureViewIndex == 0)
    throw std::runtime_error("D3D12 texture-view handle space exhausted");

  const bool shaderReadable =
    hasUsage(textureDesc.usage, TextureUsageFlags::sampled) ||
    hasUsage(textureDesc.usage, TextureUsageFlags::inputAttachment);
  const D3D12_RESOURCE_DESC nativeTextureDesc =
    static_cast<ID3D12Resource*>(texture->second.resource)->GetDesc();
  const bool attachment =
    depthView ? hasUsage(textureDesc.usage, TextureUsageFlags::depthAttachment)
              : (nativeTextureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
  if(!shaderReadable && !attachment &&
     !hasUsage(textureDesc.usage, TextureUsageFlags::storage))
    throw std::runtime_error("D3D12Device::createTextureView has no compatible texture usage");

  if(desc.viewType == ImageViewType::e2D &&
     (textureDesc.dimension == TextureDimension::e3D ||
      desc.layerCount != 1 ||
      (textureDesc.dimension == TextureDimension::e2D && desc.baseArrayLayer != 0)))
    throw std::runtime_error("D3D12Device::createTextureView received an incompatible 2D view");
  if(desc.viewType == ImageViewType::e2DArray &&
     textureDesc.dimension != TextureDimension::e2DArray &&
     textureDesc.dimension != TextureDimension::eCube)
    throw std::runtime_error("D3D12Device::createTextureView received an incompatible 2D-array view");
  if(desc.viewType == ImageViewType::eCube &&
     (textureDesc.dimension != TextureDimension::eCube ||
      desc.baseArrayLayer != 0 || desc.layerCount != 6))
    throw std::runtime_error("D3D12Device::createTextureView currently requires one complete cube");
  if(desc.viewType == ImageViewType::e3D &&
     (textureDesc.dimension != TextureDimension::e3D ||
      desc.baseArrayLayer != 0 || desc.layerCount != 1))
    throw std::runtime_error("D3D12Device::createTextureView received an incompatible 3D view");
  if(depthView && attachment &&
     (desc.viewType == ImageViewType::e3D || desc.viewType == ImageViewType::eCube))
    throw std::runtime_error("D3D12 depth attachments must be 2D or 2D-array views");

  auto* device = static_cast<ID3D12Device*>(m_d3d12Device);
  auto* resource = static_cast<ID3D12Resource*>(texture->second.resource);
  uint32_t descriptorIndex = ~0u;
  uint64_t nativeDescriptor = 0;
  uint32_t attachmentDescriptorIndex = ~0u;
  uint64_t attachmentDescriptor = 0;

  if(shaderReadable)
  {
    if(!m_freeTextureViewDescriptors.empty())
    {
      descriptorIndex = m_freeTextureViewDescriptors.back();
      m_freeTextureViewDescriptors.pop_back();
    }
    else
    {
      if(m_nextTextureViewDescriptor >= 65536)
        throw std::runtime_error("D3D12 texture-view descriptor heap exhausted");
      descriptorIndex = m_nextTextureViewDescriptor++;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc{
      .Format = toSrvFormat(desc.format),
      .Shader4ComponentMapping = componentMapping(desc.components),
    };
    switch(desc.viewType)
    {
    case ImageViewType::e2D:
      if(textureDesc.dimension == TextureDimension::e2D)
      {
        viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MostDetailedMip = desc.baseMipLevel;
        viewDesc.Texture2D.MipLevels = desc.levelCount;
        viewDesc.Texture2D.PlaneSlice = 0;
        viewDesc.Texture2D.ResourceMinLODClamp = 0.0f;
      }
      else
      {
        viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        viewDesc.Texture2DArray.MostDetailedMip = desc.baseMipLevel;
        viewDesc.Texture2DArray.MipLevels = desc.levelCount;
        viewDesc.Texture2DArray.FirstArraySlice = desc.baseArrayLayer;
        viewDesc.Texture2DArray.ArraySize = 1;
        viewDesc.Texture2DArray.PlaneSlice = 0;
        viewDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
      }
      break;
    case ImageViewType::e2DArray:
      viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      viewDesc.Texture2DArray.MostDetailedMip = desc.baseMipLevel;
      viewDesc.Texture2DArray.MipLevels = desc.levelCount;
      viewDesc.Texture2DArray.FirstArraySlice = desc.baseArrayLayer;
      viewDesc.Texture2DArray.ArraySize = desc.layerCount;
      viewDesc.Texture2DArray.PlaneSlice = 0;
      viewDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
      break;
    case ImageViewType::eCube:
      viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
      viewDesc.TextureCube.MostDetailedMip = desc.baseMipLevel;
      viewDesc.TextureCube.MipLevels = desc.levelCount;
      viewDesc.TextureCube.ResourceMinLODClamp = 0.0f;
      break;
    case ImageViewType::e3D:
      viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
      viewDesc.Texture3D.MostDetailedMip = desc.baseMipLevel;
      viewDesc.Texture3D.MipLevels = desc.levelCount;
      viewDesc.Texture3D.ResourceMinLODClamp = 0.0f;
      break;
    }

    auto* heap = static_cast<ID3D12DescriptorHeap*>(m_resourceStagingHeap);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(descriptorIndex) * m_descriptorSize;
    device->CreateShaderResourceView(resource, &viewDesc, handle);
    nativeDescriptor = static_cast<uint64_t>(handle.ptr);
  }

  if(attachment)
  {
    std::vector<uint32_t>& freeDescriptors =
      depthView ? m_freeDsvDescriptors : m_freeRtvDescriptors;
    uint32_t& nextDescriptor = depthView ? m_nextDsvDescriptor : m_nextRtvDescriptor;
    if(!freeDescriptors.empty())
    {
      attachmentDescriptorIndex = freeDescriptors.back();
      freeDescriptors.pop_back();
    }
    else
    {
      if(nextDescriptor >= 8192)
        throw std::runtime_error("D3D12 attachment descriptor heap exhausted");
      attachmentDescriptorIndex = nextDescriptor++;
    }

    auto* heap = static_cast<ID3D12DescriptorHeap*>(depthView ? m_dsvHeap : m_rtvHeap);
    const uint32_t increment = depthView ? m_dsvDescriptorSize : m_rtvDescriptorSize;
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(attachmentDescriptorIndex) * increment;

    if(depthView)
    {
      D3D12_DEPTH_STENCIL_VIEW_DESC viewDesc{
        .Format = toDsvFormat(desc.format),
        .ViewDimension = textureDesc.dimension == TextureDimension::e2D
                           ? D3D12_DSV_DIMENSION_TEXTURE2D
                           : D3D12_DSV_DIMENSION_TEXTURE2DARRAY,
        .Flags = D3D12_DSV_FLAG_NONE,
      };
      if(textureDesc.dimension == TextureDimension::e2D)
        viewDesc.Texture2D.MipSlice = desc.baseMipLevel;
      else
      {
        viewDesc.Texture2DArray.MipSlice = desc.baseMipLevel;
        viewDesc.Texture2DArray.FirstArraySlice = desc.baseArrayLayer;
        viewDesc.Texture2DArray.ArraySize = desc.layerCount;
      }
      device->CreateDepthStencilView(resource, &viewDesc, handle);
    }
    else
    {
      D3D12_RENDER_TARGET_VIEW_DESC viewDesc{.Format = toDxgiFormat(desc.format)};
      switch(desc.viewType)
      {
      case ImageViewType::e2D:
        if(textureDesc.dimension == TextureDimension::e2D)
        {
          viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
          viewDesc.Texture2D.MipSlice = desc.baseMipLevel;
          viewDesc.Texture2D.PlaneSlice = 0;
        }
        else
        {
          viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
          viewDesc.Texture2DArray.MipSlice = desc.baseMipLevel;
          viewDesc.Texture2DArray.FirstArraySlice = desc.baseArrayLayer;
          viewDesc.Texture2DArray.ArraySize = 1;
          viewDesc.Texture2DArray.PlaneSlice = 0;
        }
        break;
      case ImageViewType::e2DArray:
      case ImageViewType::eCube:
        viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        viewDesc.Texture2DArray.MipSlice = desc.baseMipLevel;
        viewDesc.Texture2DArray.FirstArraySlice = desc.baseArrayLayer;
        viewDesc.Texture2DArray.ArraySize = desc.layerCount;
        viewDesc.Texture2DArray.PlaneSlice = 0;
        break;
      case ImageViewType::e3D:
        viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        viewDesc.Texture3D.MipSlice = desc.baseMipLevel;
        viewDesc.Texture3D.FirstWSlice = 0;
        viewDesc.Texture3D.WSize =
          std::max(1u, textureDesc.extent.depth >> std::min(desc.baseMipLevel, 31u));
        break;
      }
      device->CreateRenderTargetView(resource, &viewDesc, handle);
    }
    attachmentDescriptor = static_cast<uint64_t>(handle.ptr);
  }

  const uint32_t index = m_nextTextureViewIndex;
  resource->AddRef();
  try
  {
    m_textureViews.emplace(index, TextureViewRecord{
      .resource = resource,
      .descriptorIndex = descriptorIndex,
      .nativeDescriptor = nativeDescriptor,
      .attachmentDescriptorIndex = attachmentDescriptorIndex,
      .attachmentDescriptor = attachmentDescriptor,
      .desc = desc,
      .ownsResourceReference = true,
      .ownsDescriptor = shaderReadable,
      .ownsAttachmentDescriptor = attachment,
    });
  }
  catch(...)
  {
    resource->Release();
    if(shaderReadable)
      m_freeTextureViewDescriptors.push_back(descriptorIndex);
    if(attachment)
      (depthView ? m_freeDsvDescriptors : m_freeRtvDescriptors)
        .push_back(attachmentDescriptorIndex);
    throw;
  }

  ++m_nextTextureViewIndex;
  return TextureViewHandle{{index, m_handleGeneration}};
}
TextureViewHandle D3D12Device::adoptSwapchainTextureView(uint64_t externalView)
{
  if(!m_initialized || externalView == 0)
    throw std::runtime_error("D3D12Device::adoptSwapchainTextureView requires an initialized device and descriptor");
  if(m_nextTextureViewIndex == 0)
    throw std::runtime_error("D3D12 texture-view handle space exhausted");

  const uint32_t index = m_nextTextureViewIndex++;
  m_textureViews.emplace(index, TextureViewRecord{
    .resource = nullptr,
    .descriptorIndex = ~0u,
    .nativeDescriptor = externalView,
    .attachmentDescriptorIndex = ~0u,
    .attachmentDescriptor = externalView,
    .ownsResourceReference = false,
    .ownsDescriptor = false,
  });
  return TextureViewHandle{{index, m_handleGeneration}};
}

void D3D12Device::destroyTextureView(TextureViewHandle handle)
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return;

  const auto found = m_textureViews.find(handle.index);
  if(found == m_textureViews.end())
    return;

  const SubmissionTokenSet dependencies = found->second.pendingUses;
  m_retiredTextureViews.push_back(
    RetiredTextureView{std::move(found->second), dependencies});
  m_textureViews.erase(found);
}

TextureHandle D3D12Device::createTexture(const TextureDesc& desc)
{
  if(!m_initialized)
    throw std::runtime_error("D3D12Device::createTexture called before device initialization");
  if(desc.memoryUsage != MemoryUsage::gpuOnly)
    throw std::runtime_error("D3D12Device::createTexture currently supports only gpuOnly memory");
  if(desc.extent.width == 0 || desc.extent.height == 0 || desc.extent.depth == 0 ||
     desc.mipLevels == 0 || desc.arrayLayers == 0)
  {
    throw std::runtime_error("D3D12Device::createTexture requires nonzero extent, mip, and layer counts");
  }
  if(desc.mipLevels > 65535 || desc.arrayLayers > 65535 || desc.extent.depth > 65535)
    throw std::runtime_error("D3D12Device::createTexture dimensions exceed D3D12 limits for this slice");
  if(desc.sampleCount != SampleCount::count1)
    throw std::runtime_error("D3D12Device::createTexture currently supports only sample count 1");
  if(desc.usage == TextureUsageFlags::none)
    throw std::runtime_error("D3D12Device::createTexture requires at least one usage flag");
  const bool depthAttachment = hasUsage(desc.usage, TextureUsageFlags::depthAttachment);
  if(depthAttachment && !isDepthFormat(desc.format))
    throw std::runtime_error("D3D12Device::createTexture depth attachment requires a depth format");
  if(hasUsage(desc.usage, TextureUsageFlags::colorAttachment) && isDepthFormat(desc.format))
    throw std::runtime_error("D3D12Device::createTexture color attachment cannot use a depth format");
  if(depthAttachment && hasUsage(desc.usage, TextureUsageFlags::storage))
    throw std::runtime_error("D3D12Device::createTexture does not support depth UAV resources");
  if(toDxgiFormat(desc.format) == DXGI_FORMAT_UNKNOWN)
    throw std::runtime_error("D3D12Device::createTexture received an unsupported format");
  if(desc.dimension == TextureDimension::e3D && desc.arrayLayers != 1)
    throw std::runtime_error("D3D12Device::createTexture requires one array layer for 3D textures");
  if(desc.dimension != TextureDimension::e3D && desc.extent.depth != 1)
    throw std::runtime_error("D3D12Device::createTexture requires depth 1 for non-3D textures");
  if(desc.dimension == TextureDimension::e2D && desc.arrayLayers != 1)
    throw std::runtime_error("D3D12Device::createTexture requires one array layer for 2D textures");
  if(desc.dimension == TextureDimension::eCube && desc.arrayLayers != 6)
    throw std::runtime_error("D3D12Device::createTexture currently requires exactly six cube layers");
  if(m_nextTextureIndex == 0)
    throw std::runtime_error("D3D12 texture handle space exhausted");

  D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
  if(hasUsage(desc.usage, TextureUsageFlags::storage))
    flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const bool transferClearTarget =
    !isDepthFormat(desc.format) &&
    hasUsage(desc.usage, TextureUsageFlags::transferDst) &&
    isFormatSupported(desc.format, FormatFeatureFlag::colorAttachment);
  if(hasUsage(desc.usage, TextureUsageFlags::colorAttachment) || transferClearTarget)
    flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  if(depthAttachment)
    flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

  const D3D12_HEAP_PROPERTIES heapProperties{
    .Type = D3D12_HEAP_TYPE_DEFAULT,
    .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
    .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
    .CreationNodeMask = 1,
    .VisibleNodeMask = 1,
  };
  const D3D12_RESOURCE_DESC resourceDesc{
    .Dimension = desc.dimension == TextureDimension::e3D
                   ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                   : D3D12_RESOURCE_DIMENSION_TEXTURE2D,
    .Alignment = 0,
    .Width = desc.extent.width,
    .Height = desc.extent.height,
    .DepthOrArraySize = static_cast<UINT16>(desc.dimension == TextureDimension::e3D
                                             ? desc.extent.depth
                                             : desc.arrayLayers),
    .MipLevels = static_cast<UINT16>(desc.mipLevels),
    .Format = toResourceFormat(desc.format,
                               hasUsage(desc.usage, TextureUsageFlags::sampled) ||
                               hasUsage(desc.usage, TextureUsageFlags::inputAttachment)),
    .SampleDesc = {.Count = 1, .Quality = 0},
    .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
    .Flags = flags,
  };

  D3D12_CLEAR_VALUE clearValue{};
  const D3D12_CLEAR_VALUE* optimizedClear = nullptr;
  if(depthAttachment)
  {
    clearValue.Format = toDsvFormat(desc.format);
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;
    optimizedClear = &clearValue;
  }
  else if((flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0)
  {
    clearValue.Format = toDxgiFormat(desc.format);
    clearValue.Color[0] = 0.0f;
    clearValue.Color[1] = 0.0f;
    clearValue.Color[2] = 0.0f;
    clearValue.Color[3] = 0.0f;
    optimizedClear = &clearValue;
  }

  ID3D12Resource* resource = nullptr;
  checkHresult(static_cast<ID3D12Device*>(m_d3d12Device)->CreateCommittedResource(
                 &heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc,
                 D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)),
               "ID3D12Device::CreateCommittedResource(texture)");
  setDebugName(resource, desc.debugName);

  const uint32_t index = m_nextTextureIndex;
  try
  {
    const uint32_t subresourceCount =
      desc.mipLevels * (desc.dimension == TextureDimension::e3D
                          ? 1u : desc.arrayLayers);
    m_textures.emplace(index, TextureRecord{
      .resource = resource,
      .desc = desc,
      .nativeStates = std::vector<uint32_t>(
        subresourceCount, D3D12_RESOURCE_STATE_COMMON),
      .owned = true,
    });
  }
  catch(...)
  {
    resource->Release();
    throw;
  }

  ++m_nextTextureIndex;
  return TextureHandle{index, m_handleGeneration};
}

TextureHandle D3D12Device::adoptSwapchainTexture(void* externalResource)
{
  if(!m_initialized || externalResource == nullptr)
    throw std::runtime_error("D3D12Device::adoptSwapchainTexture requires an initialized device and resource");
  if(m_nextTextureIndex == 0)
    throw std::runtime_error("D3D12 texture handle space exhausted");

  auto* resource = static_cast<ID3D12Resource*>(externalResource);
  const D3D12_RESOURCE_DESC nativeDesc = resource->GetDesc();
  TextureDesc desc{
    .dimension = TextureDimension::e2D,
    .format = fromDxgiFormat(nativeDesc.Format),
    .usage = TextureUsageFlags::colorAttachment,
    .extent = {static_cast<uint32_t>(nativeDesc.Width), nativeDesc.Height, 1},
    .mipLevels = nativeDesc.MipLevels,
    .arrayLayers = nativeDesc.DepthOrArraySize,
    .sampleCount = SampleCount::count1,
    .memoryUsage = MemoryUsage::gpuOnly,
    .debugName = "D3D12 external texture",
  };

  const uint32_t index = m_nextTextureIndex++;
  const uint32_t subresourceCount =
    desc.mipLevels * std::max(1u, desc.arrayLayers);
  m_textures.emplace(index, TextureRecord{
    .resource = resource,
    .desc = desc,
    .nativeStates = std::vector<uint32_t>(
      subresourceCount, D3D12_RESOURCE_STATE_PRESENT),
    .owned = false,
  });
  return TextureHandle{index, m_handleGeneration};
}

void D3D12Device::destroyTexture(TextureHandle handle)
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return;

  const auto found = m_textures.find(handle.index);
  if(found == m_textures.end())
    return;

  const SubmissionTokenSet dependencies = found->second.pendingUses;
  m_retiredTextures.push_back(
    RetiredTexture{std::move(found->second), dependencies});
  m_textures.erase(found);
}

BufferHandle D3D12Device::createBuffer(const BufferDesc& desc)
{
  if(!m_initialized)
    throw std::runtime_error("D3D12Device::createBuffer called before device initialization");
  if(desc.size == 0)
    throw std::runtime_error("D3D12Device::createBuffer requires a nonzero size");
  if(m_nextBufferIndex == 0)
    throw std::runtime_error("D3D12 buffer handle space exhausted");

  D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
  switch(desc.memoryUsage)
  {
  case MemoryUsage::cpuToGpu:
    heapType = D3D12_HEAP_TYPE_UPLOAD;
    initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
    break;
  case MemoryUsage::gpuToCpu:
    heapType = D3D12_HEAP_TYPE_READBACK;
    initialState = D3D12_RESOURCE_STATE_COPY_DEST;
    break;
  case MemoryUsage::gpuOnly:
  case MemoryUsage::transientAttachment:
    break;
  }

  const D3D12_HEAP_PROPERTIES heapProperties{
    .Type = heapType,
    .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
    .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
    .CreationNodeMask = 1,
    .VisibleNodeMask = 1,
  };
  const bool allowUnorderedAccess =
    desc.memoryUsage == MemoryUsage::gpuOnly &&
    static_cast<uint32_t>(desc.usage & BufferUsageFlags::storage) != 0;
  const bool uniformBuffer =
    static_cast<uint32_t>(desc.usage & BufferUsageFlags::uniform) != 0;
  const uint64_t physicalSize = uniformBuffer ? (desc.size + 255u) & ~255ull : desc.size;
  const D3D12_RESOURCE_DESC resourceDesc{
    .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
    .Alignment = 0,
    .Width = physicalSize,
    .Height = 1,
    .DepthOrArraySize = 1,
    .MipLevels = 1,
    .Format = DXGI_FORMAT_UNKNOWN,
    .SampleDesc = {.Count = 1, .Quality = 0},
    .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    .Flags = allowUnorderedAccess ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                  : D3D12_RESOURCE_FLAG_NONE,
  };

  auto* device = static_cast<ID3D12Device*>(m_d3d12Device);
  ID3D12Resource* resource = nullptr;
  checkHresult(device->CreateCommittedResource(
                 &heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, initialState, nullptr,
                 IID_PPV_ARGS(&resource)),
               "ID3D12Device::CreateCommittedResource(buffer)");

  setDebugName(resource, desc.debugName);

  const bool addressable =
    desc.allowGpuAddress ||
    static_cast<uint32_t>(desc.usage & BufferUsageFlags::shaderDeviceAddress) != 0;
  const uint32_t index = m_nextBufferIndex;
  try
  {
    m_buffers.emplace(index, BufferRecord{
      .resource = resource,
      .mapped = nullptr,
      .size = desc.size,
      .gpuAddress = addressable ? GpuPtr{resource->GetGPUVirtualAddress()} : GpuPtr{},
      .memoryUsage = desc.memoryUsage,
      .nativeState = static_cast<uint32_t>(initialState),
    });
  }
  catch(...)
  {
    resource->Release();
    throw;
  }

  ++m_nextBufferIndex;
  return BufferHandle{index, m_handleGeneration};
}

void D3D12Device::destroyBuffer(BufferHandle handle)
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return;

  const auto found = m_buffers.find(handle.index);
  if(found == m_buffers.end())
    return;

  const SubmissionTokenSet dependencies = found->second.pendingUses;
  m_retiredBuffers.push_back(
    RetiredBuffer{std::move(found->second), dependencies});
  m_buffers.erase(found);
}

GpuPtr D3D12Device::getBufferGpuAddress(BufferHandle handle) const
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return {};
  const auto found = m_buffers.find(handle.index);
  return found != m_buffers.end() ? found->second.gpuAddress : GpuPtr{};
}

void* D3D12Device::mapBuffer(BufferHandle handle)
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return nullptr;

  const auto found = m_buffers.find(handle.index);
  if(found == m_buffers.end())
    return nullptr;

  BufferRecord& record = found->second;
  if(record.memoryUsage != MemoryUsage::cpuToGpu &&
     record.memoryUsage != MemoryUsage::gpuToCpu)
  {
    return nullptr;
  }
  if(record.mapped != nullptr)
    return record.mapped;

  D3D12_RANGE readRange{0, 0};
  if(record.memoryUsage == MemoryUsage::gpuToCpu)
    readRange.End = static_cast<SIZE_T>(record.size);
  checkHresult(static_cast<ID3D12Resource*>(record.resource)->Map(0, &readRange, &record.mapped),
               "ID3D12Resource::Map(buffer)");
  return record.mapped;
}

void D3D12Device::unmapBuffer(BufferHandle handle)
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return;

  const auto found = m_buffers.find(handle.index);
  if(found == m_buffers.end() || found->second.mapped == nullptr)
    return;

  BufferRecord& record = found->second;
  D3D12_RANGE writtenRange{0, 0};
  if(record.memoryUsage == MemoryUsage::cpuToGpu)
    writtenRange.End = static_cast<SIZE_T>(record.size);
  static_cast<ID3D12Resource*>(record.resource)->Unmap(0, &writtenRange);
  record.mapped = nullptr;
}

Result<MappedBufferRange> D3D12Device::mapBufferRange(
  BufferHandle handle, const BufferMapDesc& desc)
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return Result<MappedBufferRange>::fail(RHIErrorCode::invalidHandle, "Buffer handle is stale");
  const auto found = m_buffers.find(handle.index);
  if(found == m_buffers.end())
    return Result<MappedBufferRange>::fail(RHIErrorCode::invalidHandle, "Buffer handle is stale");
  BufferRecord& record = found->second;
  if(desc.offset > record.size)
    return Result<MappedBufferRange>::fail(RHIErrorCode::invalidArgument, "Buffer map offset is out of range");
  const uint64_t size = desc.size == 0 ? record.size - desc.offset : desc.size;
  if(size > record.size - desc.offset)
    return Result<MappedBufferRange>::fail(RHIErrorCode::invalidArgument, "Buffer map size is out of range");
  void* base = mapBuffer(handle);
  if(base == nullptr)
    return Result<MappedBufferRange>::fail(RHIErrorCode::invalidState, "Buffer is not CPU visible");
  return Result<MappedBufferRange>::ok(MappedBufferRange{
    .data = static_cast<char*>(base) + desc.offset,
    .offset = desc.offset,
    .size = size,
    .coherent = true,
    .persistent = false,
  });
}

RHIResult D3D12Device::flushMappedBufferRange(
  BufferHandle handle, uint64_t offset, uint64_t size)
{
  const auto found = m_buffers.find(handle.index);
  if(handle.isNull() || handle.generation != m_handleGeneration || found == m_buffers.end())
    return RHIResult::fail(RHIErrorCode::invalidHandle, "Buffer handle is stale");
  const BufferRecord& record = found->second;
  if(record.mapped == nullptr || offset > record.size ||
     (size != 0 && size > record.size - offset))
    return RHIResult::fail(RHIErrorCode::invalidArgument, "Mapped flush range is invalid");
  return RHIResult::ok();
}

RHIResult D3D12Device::invalidateMappedBufferRange(
  BufferHandle handle, uint64_t offset, uint64_t size)
{
  const auto found = m_buffers.find(handle.index);
  if(handle.isNull() || handle.generation != m_handleGeneration || found == m_buffers.end())
    return RHIResult::fail(RHIErrorCode::invalidHandle, "Buffer handle is stale");
  const BufferRecord& record = found->second;
  if(record.mapped == nullptr || offset > record.size ||
     (size != 0 && size > record.size - offset))
    return RHIResult::fail(RHIErrorCode::invalidArgument, "Mapped invalidate range is invalid");
  return RHIResult::ok();
}
QueryPoolHandle D3D12Device::createQueryPool(uint32_t queryCount)
{
  if(!m_initialized || queryCount == 0)
    throw std::runtime_error("D3D12Device::createQueryPool requires an initialized device and queries");
  if(m_nextQueryPoolIndex == 0)
    throw std::runtime_error("D3D12 query-pool handle space exhausted");

  auto* device = static_cast<ID3D12Device*>(m_d3d12Device);
  ID3D12QueryHeap* queryHeap = nullptr;
  const D3D12_QUERY_HEAP_DESC heapDesc{
    .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
    .Count = queryCount,
    .NodeMask = 0,
  };
  checkHresult(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&queryHeap)),
               "ID3D12Device::CreateQueryHeap(timestamp)");

  const D3D12_HEAP_PROPERTIES readbackHeap{
    .Type = D3D12_HEAP_TYPE_READBACK,
    .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
    .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
    .CreationNodeMask = 1,
    .VisibleNodeMask = 1,
  };
  const D3D12_RESOURCE_DESC readbackDesc{
    .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
    .Alignment = 0,
    .Width = static_cast<UINT64>(queryCount) * sizeof(uint64_t),
    .Height = 1,
    .DepthOrArraySize = 1,
    .MipLevels = 1,
    .Format = DXGI_FORMAT_UNKNOWN,
    .SampleDesc = {.Count = 1, .Quality = 0},
    .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    .Flags = D3D12_RESOURCE_FLAG_NONE,
  };

  ID3D12Resource* readback = nullptr;
  try
  {
    checkHresult(device->CreateCommittedResource(
                   &readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)),
                 "ID3D12Device::CreateCommittedResource(timestamp readback)");
    setDebugName(queryHeap, "PassGpuTimestampQueryHeap");
    setDebugName(readback, "PassGpuTimestampReadback");

    const uint32_t index = m_nextQueryPoolIndex++;
    m_queryPools.emplace(index, QueryPoolRecord{
      .queryHeap = queryHeap,
      .readbackBuffer = readback,
      .count = queryCount,
      .available = std::vector<uint8_t>(queryCount, 0),
    });
    return QueryPoolHandle{index, m_handleGeneration};
  }
  catch(...)
  {
    if(readback != nullptr)
      readback->Release();
    queryHeap->Release();
    throw;
  }
}

void D3D12Device::destroyQueryPool(QueryPoolHandle handle)
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return;
  const auto found = m_queryPools.find(handle.index);
  if(found == m_queryPools.end())
    return;
  const SubmissionTokenSet dependencies = found->second.pendingUses;
  m_retiredQueryPools.push_back(
    RetiredQueryPool{std::move(found->second), dependencies});
  m_queryPools.erase(found);
}

uint64_t D3D12Device::getQueryPoolResult(QueryPoolHandle handle, uint32_t queryIndex)
{
  uint64_t pair[2]{};
  return getQueryPoolResultsWithAvailability(handle, queryIndex, pair) && pair[1] != 0
    ? pair[0] : 0;
}

bool D3D12Device::getQueryPoolResultsWithAvailability(
  QueryPoolHandle handle, uint32_t firstQuery,
  std::span<uint64_t> outValueAvailabilityPairs)
{
  if(handle.isNull() || handle.generation != m_handleGeneration ||
     outValueAvailabilityPairs.empty() ||
     (outValueAvailabilityPairs.size() % 2u) != 0)
    return false;
  const uint32_t queryCount =
    static_cast<uint32_t>(outValueAvailabilityPairs.size() / 2u);
  const auto found = m_queryPools.find(handle.index);
  if(found == m_queryPools.end() || firstQuery >= found->second.count ||
     queryCount > found->second.count - firstQuery)
    return false;

  QueryPoolRecord& record = found->second;
  auto* readback = static_cast<ID3D12Resource*>(record.readbackBuffer);
  const SIZE_T begin = static_cast<SIZE_T>(firstQuery) * sizeof(uint64_t);
  const SIZE_T end = static_cast<SIZE_T>(firstQuery + queryCount) * sizeof(uint64_t);
  void* mapped = nullptr;
  const D3D12_RANGE readRange{begin, end};
  if(FAILED(readback->Map(0, &readRange, &mapped)) || mapped == nullptr)
    return false;

  const auto* values = static_cast<const uint64_t*>(mapped);
  for(uint32_t index = 0; index < queryCount; ++index)
  {
    const uint32_t query = firstQuery + index;
    outValueAvailabilityPairs[index * 2u] = values[query];
    outValueAvailabilityPairs[index * 2u + 1u] =
      record.available[query] != 0 ? 1u : 0u;
  }
  const D3D12_RANGE writtenRange{0, 0};
  readback->Unmap(0, &writtenRange);
  return true;
}

float D3D12Device::getTimestampPeriodNs() const
{
  if(m_graphicsQueue.commandQueue == nullptr)
    return 0.0f;
  UINT64 frequency = 0;
  if(FAILED(static_cast<ID3D12CommandQueue*>(m_graphicsQueue.commandQueue)
              ->GetTimestampFrequency(&frequency)) || frequency == 0)
    return 0.0f;
  return static_cast<float>(1000000000.0 / static_cast<double>(frequency));
}

void D3D12Device::resetQueryPool(
  QueryPoolHandle handle, uint32_t firstQuery, uint32_t queryCount)
{
  if(handle.isNull() || handle.generation != m_handleGeneration || queryCount == 0)
    throw std::runtime_error("D3D12Device::resetQueryPool received an invalid range");
  const auto found = m_queryPools.find(handle.index);
  if(found == m_queryPools.end() || firstQuery >= found->second.count ||
     queryCount > found->second.count - firstQuery)
    throw std::runtime_error("D3D12Device::resetQueryPool received a stale handle or invalid range");
  std::fill(found->second.available.begin() + firstQuery,
            found->second.available.begin() + firstQuery + queryCount, 0);
}

void D3D12Device::writeTimestamp(
  void* commandList, QueryPoolHandle handle, uint32_t queryIndex)
{
  if(commandList == nullptr || handle.isNull() || handle.generation != m_handleGeneration)
    throw std::runtime_error("D3D12Device::writeTimestamp received invalid input");
  const auto found = m_queryPools.find(handle.index);
  if(found == m_queryPools.end() || queryIndex >= found->second.count)
    throw std::runtime_error("D3D12Device::writeTimestamp received a stale handle or invalid index");

  QueryPoolRecord& record = found->second;
  auto* list = static_cast<ID3D12GraphicsCommandList*>(commandList);
  auto* heap = static_cast<ID3D12QueryHeap*>(record.queryHeap);
  auto* readback = static_cast<ID3D12Resource*>(record.readbackBuffer);
  list->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, queryIndex);
  list->ResolveQueryData(heap, D3D12_QUERY_TYPE_TIMESTAMP, queryIndex, 1,
                         readback, static_cast<UINT64>(queryIndex) * sizeof(uint64_t));
  record.available[queryIndex] = 1;
}
SamplerHandle D3D12Device::createSampler(const SamplerDesc& desc)
{
  if(!m_initialized)
    throw std::runtime_error("D3D12Device::createSampler called before device initialization");
  if(m_nextSamplerDescriptor >= 2048 || m_nextSamplerIndex == 0)
    throw std::runtime_error("D3D12 sampler descriptor heap exhausted");

  D3D12_SAMPLER_DESC native{
    .Filter = toD3D12Filter(desc),
    .AddressU = toD3D12AddressMode(desc.addressModeU),
    .AddressV = toD3D12AddressMode(desc.addressModeV),
    .AddressW = toD3D12AddressMode(desc.addressModeW),
    .MipLODBias = desc.mipLodBias,
    .MaxAnisotropy = static_cast<UINT>(std::max(1.0f, desc.maxAnisotropy)),
    .ComparisonFunc = desc.compareEnable ? toD3D12Comparison(desc.compareOp)
                                         : D3D12_COMPARISON_FUNC_NEVER,
    .BorderColor = {0.0f, 0.0f, 0.0f, 0.0f},
    .MinLOD = desc.minLod,
    .MaxLOD = desc.maxLod,
  };

  auto* heap = static_cast<ID3D12DescriptorHeap*>(m_samplerStagingHeap);
  D3D12_CPU_DESCRIPTOR_HANDLE destination = heap->GetCPUDescriptorHandleForHeapStart();
  destination.ptr += static_cast<SIZE_T>(m_nextSamplerDescriptor) * m_samplerDescriptorSize;
  static_cast<ID3D12Device*>(m_d3d12Device)->CreateSampler(&native, destination);

  const uint32_t index = m_nextSamplerIndex++;
  m_samplers.emplace(index, SamplerRecord{m_nextSamplerDescriptor++});
  return {index, m_handleGeneration};
}

void D3D12Device::destroySampler(SamplerHandle handle)
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return;
  m_samplers.erase(handle.index);
}

ArgumentLayoutHandle D3D12Device::createArgumentLayout(const ArgumentLayoutDesc& desc)
{
  incrementHotPathCounter(BackendType::d3d12, HotPathCounter::argumentLayoutCreations);
  if(!m_initialized)
    throw std::runtime_error("D3D12Device::createArgumentLayout called before device initialization");
  if(m_nextArgumentLayoutIndex == 0)
    throw std::runtime_error("D3D12 argument-layout handle space exhausted");

  ArgumentLayoutRecord record{};
  record.bindings.reserve(static_cast<uint32_t>(desc.bindings.size()));
  for(uint32_t index = 0; index < static_cast<uint32_t>(desc.bindings.size()); ++index)
  {
    const ArgumentBinding& binding = desc.bindings[index];
    if(binding.arrayCount == 0)
      throw std::runtime_error("D3D12 argument binding requires a nonzero array count");
    if(findArgumentBinding(record, binding.binding) != nullptr)
      throw std::runtime_error("D3D12 argument layout contains a duplicate binding");

    ArgumentBindingPlacement placement{.binding = binding};
    if(argumentUsesResourceHeap(binding.type))
    {
      placement.resourceOffset = record.resourceDescriptorCount;
      record.resourceDescriptorCount += descriptorCountForHeap(binding, false);
    }
    if(argumentUsesSamplerHeap(binding.type))
    {
      placement.samplerOffset = record.samplerDescriptorCount;
      record.samplerDescriptorCount += descriptorCountForHeap(binding, true);
    }
    record.bindings.push_back(placement);
  }

  const uint32_t index = m_nextArgumentLayoutIndex++;
  m_argumentLayouts.emplace(index, std::move(record));
  return {index, m_handleGeneration};
}

void D3D12Device::destroyArgumentLayout(ArgumentLayoutHandle handle)
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return;
  m_argumentLayouts.erase(handle.index);
}

ArgumentTableHandle D3D12Device::createArgumentTable(const ArgumentTableCreateDesc& desc)
{
  incrementHotPathCounter(BackendType::d3d12, HotPathCounter::descriptorAllocations);
  if(!m_initialized || desc.layout.isNull() || desc.layout.generation != m_handleGeneration)
    throw std::runtime_error("D3D12Device::createArgumentTable requires a valid layout");
  const auto layoutIt = m_argumentLayouts.find(desc.layout.index);
  if(layoutIt == m_argumentLayouts.end())
    throw std::runtime_error("D3D12Device::createArgumentTable received a stale layout");
  if(m_nextArgumentTableIndex == 0)
    throw std::runtime_error("D3D12 argument-table handle space exhausted");

  const ArgumentLayoutRecord& layoutRecord = layoutIt->second;
  if(layoutRecord.resourceDescriptorCount > 65536u - m_nextTextureViewDescriptor)
    throw std::runtime_error("D3D12 resource descriptor heap exhausted");
  if(layoutRecord.samplerDescriptorCount > 2048u - m_nextSamplerDescriptor)
    throw std::runtime_error("D3D12 sampler descriptor heap exhausted");

  ArgumentTableRecord record{.layout = desc.layout, .lifetime = desc.lifetime};
  if(layoutRecord.resourceDescriptorCount != 0)
  {
    record.resourceBase = m_nextTextureViewDescriptor;
    m_nextTextureViewDescriptor += layoutRecord.resourceDescriptorCount;
  }
  if(layoutRecord.samplerDescriptorCount != 0)
  {
    record.samplerBase = m_nextSamplerDescriptor;
    m_nextSamplerDescriptor += layoutRecord.samplerDescriptorCount;
  }

  const uint32_t index = m_nextArgumentTableIndex++;
  m_argumentTables.emplace(index, record);
  return {index, m_handleGeneration};
}

void D3D12Device::destroyArgumentTable(ArgumentTableHandle handle)
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return;
  // Descriptor ranges are monotonic for this device generation, so erasing the
  // logical record invalidates the handle while the native range remains valid
  // for already-submitted command lists until device teardown.
  m_argumentTables.erase(handle.index);
}

bool D3D12Device::isResidencyResourceAlive(
  ResidencyResource resource) const noexcept
{
  if(!resource.isValid() || resource.generation != m_handleGeneration)
    return false;
  switch(resource.kind)
  {
  case ResidencyResourceKind::buffer:
    return m_buffers.find(resource.index) != m_buffers.end();
  case ResidencyResourceKind::texture:
    return m_textures.find(resource.index) != m_textures.end();
  case ResidencyResourceKind::textureView:
    return m_textureViews.find(resource.index) != m_textureViews.end();
  case ResidencyResourceKind::sampler:
    return m_samplers.find(resource.index) != m_samplers.end();
  case ResidencyResourceKind::shaderLibrary:
    return m_shaderLibraries.find(resource.index) != m_shaderLibraries.end();
  case ResidencyResourceKind::pipeline:
    return m_pipelines.find(resource.index) != m_pipelines.end();
  case ResidencyResourceKind::argumentTable:
    return m_argumentTables.find(resource.index) != m_argumentTables.end();
  }
  return false;
}

bool D3D12Device::validateArgumentTableForSubmit(
  ArgumentTableHandle table) const noexcept
{
  if(table.isNull() || table.generation != m_handleGeneration)
    return false;
  const auto found = m_argumentTables.find(table.index);
  if(found == m_argumentTables.end())
    return false;
  return std::all_of(
    found->second.referencedResources.begin(),
    found->second.referencedResources.end(),
    [this](const ArgumentResourceReference& reference) {
      return isResidencyResourceAlive(reference.resource);
    });
}

bool D3D12Device::isPipelineValid(PipelineHandle pipeline) const noexcept
{
  return !pipeline.isNull() && pipeline.generation == m_handleGeneration &&
         m_pipelines.find(pipeline.index) != m_pipelines.end();
}

bool D3D12Device::isBufferValid(BufferHandle buffer) const noexcept
{
  return !buffer.isNull() && buffer.generation == m_handleGeneration &&
         m_buffers.find(buffer.index) != m_buffers.end();
}

bool D3D12Device::isTextureValid(TextureHandle texture) const noexcept
{
  return !texture.isNull() && texture.generation == m_handleGeneration &&
         m_textures.find(texture.index) != m_textures.end();
}

bool D3D12Device::isTextureViewValid(TextureViewHandle view) const noexcept
{
  return !view.isNull() && view.generation == m_handleGeneration &&
         m_textureViews.find(view.index) != m_textureViews.end();
}

bool D3D12Device::isQueryPoolValid(QueryPoolHandle pool) const noexcept
{
  return !pool.isNull() && pool.generation == m_handleGeneration &&
         m_queryPools.find(pool.index) != m_queryPools.end();
}

void D3D12Device::markArgumentTableSubmitted(
  ArgumentTableHandle table, SubmissionToken token)
{
  if(table.isNull() || table.generation != m_handleGeneration || !token.isValid())
    throw std::runtime_error("D3D12 command buffer references an invalid argument table");
  const auto found = m_argumentTables.find(table.index);
  if(found == m_argumentTables.end())
    throw std::runtime_error("D3D12 command buffer references a stale argument table");
  if(!found->second.pendingUses.record(token))
    throw std::runtime_error("D3D12 argument-table queue dependency capacity exceeded");
  for(const ArgumentResourceReference& reference : found->second.referencedResources)
  {
    const ResidencyResource resource = reference.resource;
    switch(resource.kind)
    {
    case ResidencyResourceKind::buffer:
      markBufferSubmitted(BufferHandle{resource.index, resource.generation}, token);
      break;
    case ResidencyResourceKind::texture:
      markTextureSubmitted(TextureHandle{resource.index, resource.generation}, token);
      break;
    case ResidencyResourceKind::textureView:
      markTextureViewSubmitted(
        TextureViewHandle{resource.index, resource.generation}, token);
      break;
    case ResidencyResourceKind::pipeline:
      markPipelineSubmitted(PipelineHandle{resource.index, resource.generation}, token);
      break;
    case ResidencyResourceKind::sampler:
    case ResidencyResourceKind::shaderLibrary:
    case ResidencyResourceKind::argumentTable:
      break;
    }
  }
}

void D3D12Device::markPipelineSubmitted(
  PipelineHandle pipeline, SubmissionToken token)
{
  const auto found = m_pipelines.find(pipeline.index);
  if(!isPipelineValid(pipeline) || !found->second.pendingUses.record(token))
    throw std::runtime_error("D3D12 pipeline submission tracking failed");
}

void D3D12Device::markBufferSubmitted(
  BufferHandle buffer, SubmissionToken token)
{
  const auto found = m_buffers.find(buffer.index);
  if(!isBufferValid(buffer) || !found->second.pendingUses.record(token))
    throw std::runtime_error("D3D12 buffer submission tracking failed");
}

void D3D12Device::markTextureSubmitted(
  TextureHandle texture, SubmissionToken token)
{
  const auto found = m_textures.find(texture.index);
  if(!isTextureValid(texture) || !found->second.pendingUses.record(token))
    throw std::runtime_error("D3D12 texture submission tracking failed");
}

void D3D12Device::markTextureViewSubmitted(
  TextureViewHandle view, SubmissionToken token)
{
  const auto found = m_textureViews.find(view.index);
  if(!isTextureViewValid(view) || !found->second.pendingUses.record(token))
    throw std::runtime_error("D3D12 texture-view submission tracking failed");
  if(found->second.desc.image.isValid())
    markTextureSubmitted(found->second.desc.image, token);
}

void D3D12Device::markQueryPoolSubmitted(
  QueryPoolHandle pool, SubmissionToken token)
{
  const auto found = m_queryPools.find(pool.index);
  if(!isQueryPoolValid(pool) || !found->second.pendingUses.record(token))
    throw std::runtime_error("D3D12 query-pool submission tracking failed");
}

void D3D12Device::updateArgumentTable(ArgumentTableHandle table, ArgumentWriteBatch writes)
{
  incrementHotPathCounter(BackendType::d3d12, HotPathCounter::descriptorUpdates, writes.size());
  if(writes.empty())
    return;
  if(table.isNull() || table.generation != m_handleGeneration)
    throw std::runtime_error("D3D12Device::updateArgumentTable requires a valid table");

  const auto tableIt = m_argumentTables.find(table.index);
  if(tableIt == m_argumentTables.end())
    throw std::runtime_error("D3D12Device::updateArgumentTable received a stale table");
  if(tableIt->second.pendingUses.count != 0)
  {
    if(!isRetirementComplete(tableIt->second.pendingUses))
      throw std::runtime_error(
        "D3D12Device::updateArgumentTable rejected a table with pending GPU work");
    tableIt->second.pendingUses = {};
  }
  const auto layoutIt = m_argumentLayouts.find(tableIt->second.layout.index);
  if(layoutIt == m_argumentLayouts.end())
    throw std::runtime_error("D3D12 argument table refers to a stale layout");

  auto* device = static_cast<ID3D12Device*>(m_d3d12Device);
  auto* resourceHeap = static_cast<ID3D12DescriptorHeap*>(m_cbvSrvUavHeap);
  auto* samplerHeap = static_cast<ID3D12DescriptorHeap*>(m_samplerHeap);
  const D3D12_CPU_DESCRIPTOR_HANDLE resourceStart =
    resourceHeap->GetCPUDescriptorHandleForHeapStart();
  const D3D12_CPU_DESCRIPTOR_HANDLE samplerStart =
    samplerHeap->GetCPUDescriptorHandleForHeapStart();
  const D3D12_CPU_DESCRIPTOR_HANDLE samplerStagingStart =
    static_cast<ID3D12DescriptorHeap*>(m_samplerStagingHeap)
      ->GetCPUDescriptorHandleForHeapStart();
  const auto recordReferencedResource =
    [&tableIt](uint32_t binding, uint32_t arrayElement, ResidencyResource resource)
    {
      auto& resources = tableIt->second.referencedResources;
      const auto existing = std::find_if(
        resources.begin(), resources.end(),
        [binding, arrayElement, resource](
          const ArgumentResourceReference& reference)
        {
          return reference.binding == binding &&
                 reference.arrayElement == arrayElement &&
                 reference.resource.kind == resource.kind;
        });
      if(existing != resources.end())
        existing->resource = resource;
      else
        resources.push_back(
          ArgumentResourceReference{binding, arrayElement, resource});
    };

  for(size_t index = 0; index < writes.size(); ++index)
  {
    const ArgumentWrite& write = writes[index];
    const ArgumentBindingPlacement* placement =
      findArgumentBinding(layoutIt->second, write.binding);
    if(placement == nullptr || placement->binding.type != write.type)
      throw std::runtime_error("D3D12 argument write does not match its layout");
    if(write.arrayElement >= placement->binding.arrayCount)
      throw std::runtime_error("D3D12 argument write array element is out of range");
    if(write.type == ArgumentType::uniformBuffer ||
       write.type == ArgumentType::storageBuffer)
    {
      tableIt->second.buffers[write.binding] = {
        write.buffer, write.offset, write.size};
    }


    D3D12_CPU_DESCRIPTOR_HANDLE resourceDestination{};
    if(placement->resourceOffset != ~0u)
    {
      resourceDestination = resourceStart;
      resourceDestination.ptr += static_cast<SIZE_T>(
        tableIt->second.resourceBase + placement->resourceOffset + write.arrayElement) *
        m_descriptorSize;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE samplerDestination{};
    if(placement->samplerOffset != ~0u)
    {
      const uint32_t samplerElement = write.arrayElement;
      samplerDestination = samplerStart;
      samplerDestination.ptr += static_cast<SIZE_T>(
        tableIt->second.samplerBase + placement->samplerOffset + samplerElement) *
        m_samplerDescriptorSize;
    }

    switch(write.type)
    {
    case ArgumentType::uniformBuffer:
    {
      const auto bufferIt = m_buffers.find(write.buffer.index);
      if(write.buffer.generation != m_handleGeneration || bufferIt == m_buffers.end())
        throw std::runtime_error("D3D12 uniform-buffer write received an invalid buffer");
      recordReferencedResource(write.binding, write.arrayElement, residencyResource(write.buffer));
      const BufferRecord& buffer = bufferIt->second;
      const uint64_t available = write.offset < buffer.size ? buffer.size - write.offset : 0;
      const uint64_t requested = write.size == 0 ? available : std::min(write.size, available);
      if(requested == 0)
        throw std::runtime_error("D3D12 uniform-buffer write has an empty range");
      const UINT size = static_cast<UINT>((requested + 255u) & ~255ull);
      const D3D12_CONSTANT_BUFFER_VIEW_DESC view{
        .BufferLocation = static_cast<ID3D12Resource*>(buffer.resource)->GetGPUVirtualAddress() +
                          write.offset,
        .SizeInBytes = size,
      };
      device->CreateConstantBufferView(&view, resourceDestination);
      break;
    }
    case ArgumentType::storageBuffer:
    {
      const auto bufferIt = m_buffers.find(write.buffer.index);
      if(write.buffer.generation != m_handleGeneration || bufferIt == m_buffers.end())
        throw std::runtime_error("D3D12 storage-buffer write received an invalid buffer");
      recordReferencedResource(write.binding, write.arrayElement, residencyResource(write.buffer));
      const BufferRecord& buffer = bufferIt->second;
      const uint64_t available = write.offset < buffer.size ? buffer.size - write.offset : 0;
      const uint64_t requested = write.size == 0 ? available : std::min(write.size, available);
      if(requested < 4 || (write.offset & 3u) != 0)
        throw std::runtime_error("D3D12 storage-buffer write requires a 4-byte-aligned range");
      auto* resource = static_cast<ID3D12Resource*>(buffer.resource);
      if(write.accessIntent == ArgumentAccessIntent::readWrite)
      {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{
          .Format = DXGI_FORMAT_R32_TYPELESS,
          .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
          .Buffer = {
            .FirstElement = write.offset / 4u,
            .NumElements = static_cast<UINT>(requested / 4u),
            .StructureByteStride = 0,
            .CounterOffsetInBytes = 0,
            .Flags = D3D12_BUFFER_UAV_FLAG_RAW,
          },
        };
        device->CreateUnorderedAccessView(resource, nullptr, &view, resourceDestination);
      }
      else
      {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{
          .Format = DXGI_FORMAT_R32_TYPELESS,
          .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
          .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
          .Buffer = {
            .FirstElement = write.offset / 4u,
            .NumElements = static_cast<UINT>(requested / 4u),
            .StructureByteStride = 0,
            .Flags = D3D12_BUFFER_SRV_FLAG_RAW,
          },
        };
        device->CreateShaderResourceView(resource, &view, resourceDestination);
      }
      break;
    }
    case ArgumentType::sampledTexture:
    case ArgumentType::combinedImageSampler:
    {
      const auto viewIt = m_textureViews.find(write.textureView.index);
      if(write.textureView.generation != m_handleGeneration || viewIt == m_textureViews.end() ||
         viewIt->second.nativeDescriptor == 0)
      {
        throw std::runtime_error("D3D12 sampled-texture write received an invalid view");
      }
      recordReferencedResource(write.binding, write.arrayElement, residencyResource(write.textureView));
      D3D12_CPU_DESCRIPTOR_HANDLE source{
        static_cast<SIZE_T>(viewIt->second.nativeDescriptor)};
      device->CopyDescriptorsSimple(1, resourceDestination, source,
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      if(write.type == ArgumentType::sampledTexture)
        break;
      [[fallthrough]];
    }
    case ArgumentType::sampler:
    {
      const auto samplerIt = m_samplers.find(write.sampler.index);
      if(write.sampler.generation != m_handleGeneration || samplerIt == m_samplers.end())
        throw std::runtime_error("D3D12 sampler write received an invalid sampler");
      recordReferencedResource(write.binding, write.arrayElement, ResidencyResource{
        ResidencyResourceKind::sampler,
        write.sampler.index,
        write.sampler.generation,
      });
      D3D12_CPU_DESCRIPTOR_HANDLE source = samplerStagingStart;
      source.ptr += static_cast<SIZE_T>(samplerIt->second.descriptorIndex) *
                    m_samplerDescriptorSize;
      device->CopyDescriptorsSimple(1, samplerDestination, source,
                                    D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
      break;
    }
    case ArgumentType::storageTexture:
    {
      const auto viewIt = m_textureViews.find(write.textureView.index);
      if(write.textureView.generation != m_handleGeneration || viewIt == m_textureViews.end() ||
         viewIt->second.resource == nullptr)
      {
        throw std::runtime_error("D3D12 storage-texture write received an invalid view");
      }
      recordReferencedResource(write.binding, write.arrayElement, residencyResource(write.textureView));
      auto* resource = static_cast<ID3D12Resource*>(viewIt->second.resource);
      const D3D12_RESOURCE_DESC resourceDesc = resource->GetDesc();
      D3D12_UNORDERED_ACCESS_VIEW_DESC view{
        .Format = resourceDesc.Format,
      };
      if(resourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
      {
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        view.Texture3D.MipSlice = viewIt->second.desc.baseMipLevel;
        view.Texture3D.FirstWSlice = 0;
        view.Texture3D.WSize = std::max<UINT>(
          1u,
          static_cast<UINT>(resourceDesc.DepthOrArraySize) >>
            std::min(viewIt->second.desc.baseMipLevel, 31u));
      }
      else if(resourceDesc.DepthOrArraySize > 1)
      {
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipSlice = viewIt->second.desc.baseMipLevel;
        view.Texture2DArray.FirstArraySlice = viewIt->second.desc.baseArrayLayer;
        view.Texture2DArray.ArraySize = viewIt->second.desc.layerCount;
      }
      else
      {
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = viewIt->second.desc.baseMipLevel;
      }
      device->CreateUnorderedAccessView(resource, nullptr, &view, resourceDestination);
      break;
    }
    }
  }
}

ArgumentLayoutHandle D3D12Device::getArgumentTableLayout(ArgumentTableHandle table) const
{
  if(table.isNull() || table.generation != m_handleGeneration)
    return {};
  const auto found = m_argumentTables.find(table.index);
  return found == m_argumentTables.end() ? ArgumentLayoutHandle{}
                                         : found->second.layout;
}
uint64_t D3D12Device::resolveArgumentTableResourceGpu(ArgumentTableHandle handle) const
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return 0;
  const auto found = m_argumentTables.find(handle.index);
  if(found == m_argumentTables.end() || found->second.resourceBase == ~0u)
    return 0;
  D3D12_GPU_DESCRIPTOR_HANDLE descriptor =
    static_cast<ID3D12DescriptorHeap*>(m_cbvSrvUavHeap)->GetGPUDescriptorHandleForHeapStart();
  descriptor.ptr += static_cast<UINT64>(found->second.resourceBase) * m_descriptorSize;
  return descriptor.ptr;
}

uint64_t D3D12Device::resolveArgumentTableSamplerGpu(ArgumentTableHandle handle) const
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return 0;
  const auto found = m_argumentTables.find(handle.index);
  if(found == m_argumentTables.end() || found->second.samplerBase == ~0u)
    return 0;
  D3D12_GPU_DESCRIPTOR_HANDLE descriptor =
    static_cast<ID3D12DescriptorHeap*>(m_samplerHeap)->GetGPUDescriptorHandleForHeapStart();
  descriptor.ptr += static_cast<UINT64>(found->second.samplerBase) * m_samplerDescriptorSize;
  return descriptor.ptr;
}

const D3D12Device::ArgumentBindingPlacement* D3D12Device::findArgumentBinding(
  const ArgumentLayoutRecord& layout, uint32_t binding) const
{
  const auto found = std::find_if(layout.bindings.begin(), layout.bindings.end(),
    [binding](const ArgumentBindingPlacement& placement)
    {
      return placement.binding.binding == binding;
    });
  return found == layout.bindings.end() ? nullptr : &*found;
}
void* D3D12Device::resolveTexture(TextureHandle handle) const
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return nullptr;
  const auto found = m_textures.find(handle.index);
  return found == m_textures.end() ? nullptr : found->second.resource;
}

uint64_t D3D12Device::resolveTextureViewDescriptor(TextureViewHandle handle) const
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return 0;
  const auto found = m_textureViews.find(handle.index);
  return found == m_textureViews.end() ? 0 : found->second.nativeDescriptor;
}

uint64_t D3D12Device::resolveTextureViewAttachmentDescriptor(TextureViewHandle handle) const
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return 0;
  const auto found = m_textureViews.find(handle.index);
  return found == m_textureViews.end() ? 0 : found->second.attachmentDescriptor;
}

uint64_t D3D12Device::resolveTextureAttachmentDescriptor(
  TextureHandle texture, const TextureSubresourceRange& range) const
{
  if(texture.isNull() || texture.generation != m_handleGeneration)
    return 0;

  for(const auto& [index, view] : m_textureViews)
  {
    (void)index;
    if(!view.ownsAttachmentDescriptor ||
       view.desc.image.index != texture.index ||
       view.desc.image.generation != texture.generation ||
       view.desc.aspect != range.aspect ||
       view.desc.baseMipLevel != range.baseMipLevel ||
       view.desc.baseArrayLayer != range.baseArrayLayer)
      continue;

    const uint32_t levelCount = range.levelCount == 0 ? view.desc.levelCount : range.levelCount;
    const uint32_t layerCount = range.layerCount == 0 ? view.desc.layerCount : range.layerCount;
    if(levelCount <= view.desc.levelCount && layerCount <= view.desc.layerCount)
      return view.attachmentDescriptor;
  }
  return 0;
}

bool D3D12Device::textureViewHasStencil(TextureViewHandle handle) const
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return false;
  const auto found = m_textureViews.find(handle.index);
  return found != m_textureViews.end() && hasStencil(found->second.desc.format);
}

void* D3D12Device::resolveBuffer(BufferHandle handle) const
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return nullptr;
  const auto found = m_buffers.find(handle.index);
  return found == m_buffers.end() ? nullptr : found->second.resource;
}

void D3D12Device::transitionBuffer(
  void* nativeCommandList, BufferHandle handle, uint32_t targetStateValue)
{
  if(nativeCommandList == nullptr || handle.isNull() || handle.generation != m_handleGeneration)
    throw std::runtime_error("D3D12Device::transitionBuffer received invalid input");
  const auto found = m_buffers.find(handle.index);
  if(found == m_buffers.end())
    throw std::runtime_error("D3D12Device::transitionBuffer received a stale handle");

  BufferRecord& record = found->second;
  const auto targetState = static_cast<D3D12_RESOURCE_STATES>(targetStateValue);
  const auto currentState = static_cast<D3D12_RESOURCE_STATES>(record.nativeState);
  if(record.memoryUsage == MemoryUsage::cpuToGpu)
  {
    const D3D12_RESOURCE_STATES allowed =
      D3D12_RESOURCE_STATE_GENERIC_READ |
      D3D12_RESOURCE_STATE_COPY_SOURCE |
      D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
      D3D12_RESOURCE_STATE_INDEX_BUFFER |
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
      D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    if((targetState & ~allowed) != 0)
      throw std::runtime_error("D3D12 upload-heap buffer cannot enter the requested state");
    return;
  }
  if(record.memoryUsage == MemoryUsage::gpuToCpu)
  {
    if(targetState != D3D12_RESOURCE_STATE_COPY_DEST)
      throw std::runtime_error("D3D12 readback buffer must remain in COPY_DEST");
    return;
  }
  if(currentState == targetState)
    return;

  const D3D12_RESOURCE_BARRIER barrier{
    .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
    .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
    .Transition = {
      .pResource = static_cast<ID3D12Resource*>(record.resource),
      .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
      .StateBefore = currentState,
      .StateAfter = targetState,
    },
  };
  static_cast<ID3D12GraphicsCommandList*>(nativeCommandList)->ResourceBarrier(1, &barrier);
  record.nativeState = targetStateValue;
}
void D3D12Device::prepareBufferForQueueHandoff(
  void* nativeCommandList, BufferHandle handle)
{
  if(nativeCommandList == nullptr || handle.isNull() || handle.generation != m_handleGeneration)
    throw std::runtime_error(
      "D3D12Device::prepareBufferForQueueHandoff received invalid input");
  const auto found = m_buffers.find(handle.index);
  if(found == m_buffers.end())
    throw std::runtime_error(
      "D3D12Device::prepareBufferForQueueHandoff received a stale handle");
  if(found->second.memoryUsage == MemoryUsage::gpuOnly)
  {
    transitionBuffer(
      nativeCommandList, handle,
      static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON));
  }
}

void D3D12Device::transitionTexture(
  void* nativeCommandList, TextureHandle handle,
  const TextureSubresourceRange& range, uint32_t targetStateValue)
{
  if(nativeCommandList == nullptr || handle.isNull() || handle.generation != m_handleGeneration)
    throw std::runtime_error("D3D12Device::transitionTexture received invalid input");
  const auto found = m_textures.find(handle.index);
  if(found == m_textures.end())
    throw std::runtime_error("D3D12Device::transitionTexture received a stale handle");

  TextureRecord& record = found->second;
  auto* resource = static_cast<ID3D12Resource*>(record.resource);
  const D3D12_RESOURCE_DESC resourceDesc = resource->GetDesc();
  const uint32_t totalMipLevels = resourceDesc.MipLevels;
  const uint32_t totalArrayLayers =
    resourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
      ? 1u : resourceDesc.DepthOrArraySize;
  if(range.baseMipLevel >= totalMipLevels ||
     range.baseArrayLayer >= totalArrayLayers)
    throw std::runtime_error(
      "D3D12Device::transitionTexture received an invalid texture range");

  const uint32_t levelCount =
    range.levelCount == 0 || range.levelCount == ~0u
      ? totalMipLevels - range.baseMipLevel : range.levelCount;
  const uint32_t layerCount =
    range.layerCount == 0 || range.layerCount == ~0u
      ? totalArrayLayers - range.baseArrayLayer : range.layerCount;
  if(levelCount > totalMipLevels - range.baseMipLevel ||
     layerCount > totalArrayLayers - range.baseArrayLayer)
    throw std::runtime_error(
      "D3D12Device::transitionTexture range exceeds the resource");

  std::vector<D3D12_RESOURCE_BARRIER> barriers;
  barriers.reserve(static_cast<size_t>(levelCount) * layerCount);
  for(uint32_t layer = 0; layer < layerCount; ++layer)
  {
    for(uint32_t mip = 0; mip < levelCount; ++mip)
    {
      const uint32_t subresource =
        range.baseMipLevel + mip +
        (range.baseArrayLayer + layer) * totalMipLevels;
      if(subresource >= record.nativeStates.size())
        throw std::runtime_error(
          "D3D12Device::transitionTexture state table is incomplete");
      const uint32_t currentStateValue = record.nativeStates[subresource];
      if(currentStateValue == targetStateValue)
        continue;
      barriers.push_back({
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {
          .pResource = resource,
          .Subresource = subresource,
          .StateBefore = static_cast<D3D12_RESOURCE_STATES>(currentStateValue),
          .StateAfter = static_cast<D3D12_RESOURCE_STATES>(targetStateValue),
        },
      });
      record.nativeStates[subresource] = targetStateValue;
    }
  }
  if(!barriers.empty())
  {
    static_cast<ID3D12GraphicsCommandList*>(nativeCommandList)->ResourceBarrier(
      static_cast<UINT>(barriers.size()), barriers.data());
  }
}
void* D3D12Device::resolveBufferMappedData(BufferHandle handle) const
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return nullptr;
  const auto found = m_buffers.find(handle.index);
  return found == m_buffers.end() ? nullptr : found->second.mapped;
}

uint64_t D3D12Device::resolveBufferSize(BufferHandle handle) const
{
  if(handle.isNull() || handle.generation != m_handleGeneration)
    return 0;
  const auto found = m_buffers.find(handle.index);
  return found == m_buffers.end() ? 0 : found->second.size;
}

void D3D12Device::selectD3D12Adapter()
{
  auto* factory = static_cast<IDXGIFactory4*>(m_dxgiFactory);

  for(UINT adapterIndex = 0;; ++adapterIndex)
  {
    IDXGIAdapter1* candidate = nullptr;
    if(factory->EnumAdapters1(adapterIndex, &candidate) == DXGI_ERROR_NOT_FOUND)
      break;

    DXGI_ADAPTER_DESC1 desc{};
    const HRESULT descResult = candidate->GetDesc1(&desc);
    const bool software = SUCCEEDED(descResult) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    const bool supportsD3D12 =
      !software && SUCCEEDED(D3D12CreateDevice(candidate, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr));

    if(!supportsD3D12)
    {
      candidate->Release();
      continue;
    }

    m_dxgiAdapter = candidate;
    m_physicalDeviceInfo.deviceName = narrow(desc.Description);
    m_physicalDeviceInfo.vendorId = desc.VendorId;
    m_physicalDeviceInfo.deviceId = desc.DeviceId;
    m_physicalDeviceInfo.deviceType = static_cast<uint32_t>(desc.Flags);

    LARGE_INTEGER driverVersion{};
    if(SUCCEEDED(candidate->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driverVersion)))
      m_physicalDeviceInfo.driverVersion = static_cast<uint32_t>(driverVersion.LowPart);

    if(desc.DedicatedVideoMemory != 0)
      m_memoryProperties.memoryHeaps.push_back({static_cast<uint64_t>(desc.DedicatedVideoMemory), 1u});
    if(desc.SharedSystemMemory != 0)
      m_memoryProperties.memoryHeaps.push_back({static_cast<uint64_t>(desc.SharedSystemMemory), 0u});
    return;
  }

  throw std::runtime_error("No hardware adapter supporting D3D feature level 12_0 was found");
}

void D3D12Device::initD3D12Device()
{
  auto* adapter = static_cast<IDXGIAdapter1*>(m_dxgiAdapter);
  ID3D12Device* device = nullptr;
  checkHresult(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)),
               "D3D12CreateDevice");
  m_d3d12Device = device;

  const std::array<D3D_FEATURE_LEVEL, 3> requestedLevels{
    D3D_FEATURE_LEVEL_12_2,
    D3D_FEATURE_LEVEL_12_1,
    D3D_FEATURE_LEVEL_12_0,
  };
  D3D12_FEATURE_DATA_FEATURE_LEVELS levels{
    .NumFeatureLevels = static_cast<UINT>(requestedLevels.size()),
    .pFeatureLevelsRequested = requestedLevels.data(),
    .MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL_12_0,
  };
  if(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels))))
    m_d3dFeatureLevel = static_cast<uint32_t>(levels.MaxSupportedFeatureLevel);
  else
    m_d3dFeatureLevel = static_cast<uint32_t>(D3D_FEATURE_LEVEL_12_0);

  m_apiVersion = m_d3dFeatureLevel;
  m_physicalDeviceInfo.nativeApiVersion = m_apiVersion;
}

void D3D12Device::initD3D12Queues()
{
  auto* device = static_cast<ID3D12Device*>(m_d3d12Device);

  const auto createQueue = [device](D3D12_COMMAND_LIST_TYPE type, NativeQueueInfo& target,
                                    QueueClass queueClass, bool dedicated)
  {
    const D3D12_COMMAND_QUEUE_DESC desc{
      .Type = type,
      .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
      .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
      .NodeMask = 0,
    };
    ID3D12CommandQueue* queue = nullptr;
    checkHresult(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)),
                 "ID3D12Device::CreateCommandQueue");
    target.commandQueue = queue;
    target.nodeMask = 0;
    target.queueIndex = 0;
    target.queueClass = queueClass;
    target.dedicated = dedicated;
  };

  createQueue(D3D12_COMMAND_LIST_TYPE_DIRECT, m_graphicsQueue, QueueClass::graphics, false);
  createQueue(D3D12_COMMAND_LIST_TYPE_COMPUTE, m_computeQueue, QueueClass::compute, true);
  createQueue(D3D12_COMMAND_LIST_TYPE_COPY, m_transferQueue, QueueClass::transfer, true);
}

void D3D12Device::initDescriptorHeaps()
{
  auto* device = static_cast<ID3D12Device*>(m_d3d12Device);

  const D3D12_DESCRIPTOR_HEAP_DESC resourceDesc{
    .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
    .NumDescriptors = 65536,
    .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    .NodeMask = 0,
  };
  ID3D12DescriptorHeap* resourceHeap = nullptr;
  checkHresult(device->CreateDescriptorHeap(&resourceDesc, IID_PPV_ARGS(&resourceHeap)),
               "CreateDescriptorHeap(CBV_SRV_UAV)");
  m_cbvSrvUavHeap = resourceHeap;

  const D3D12_DESCRIPTOR_HEAP_DESC samplerDesc{
    .Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
    .NumDescriptors = 2048,
    .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    .NodeMask = 0,
  };
  ID3D12DescriptorHeap* samplerHeap = nullptr;
  checkHresult(device->CreateDescriptorHeap(&samplerDesc, IID_PPV_ARGS(&samplerHeap)),
               "CreateDescriptorHeap(SAMPLER)");
  m_samplerHeap = samplerHeap;

  D3D12_DESCRIPTOR_HEAP_DESC resourceStagingDesc = resourceDesc;
  resourceStagingDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  ID3D12DescriptorHeap* resourceStagingHeap = nullptr;
  checkHresult(device->CreateDescriptorHeap(
                 &resourceStagingDesc, IID_PPV_ARGS(&resourceStagingHeap)),
               "CreateDescriptorHeap(CBV_SRV_UAV staging)");
  m_resourceStagingHeap = resourceStagingHeap;

  D3D12_DESCRIPTOR_HEAP_DESC samplerStagingDesc = samplerDesc;
  samplerStagingDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  ID3D12DescriptorHeap* samplerStagingHeap = nullptr;
  checkHresult(device->CreateDescriptorHeap(
                 &samplerStagingDesc, IID_PPV_ARGS(&samplerStagingHeap)),
               "CreateDescriptorHeap(SAMPLER staging)");
  m_samplerStagingHeap = samplerStagingHeap;

  const D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{
    .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
    .NumDescriptors = 8192,
    .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
    .NodeMask = 0,
  };
  ID3D12DescriptorHeap* rtvHeap = nullptr;
  checkHresult(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap)),
               "CreateDescriptorHeap(RTV)");
  m_rtvHeap = rtvHeap;

  const D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{
    .Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
    .NumDescriptors = 8192,
    .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
    .NodeMask = 0,
  };
  ID3D12DescriptorHeap* dsvHeap = nullptr;
  checkHresult(device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&dsvHeap)),
               "CreateDescriptorHeap(DSV)");
  m_dsvHeap = dsvHeap;

  m_samplerDescriptorSize =
    device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  m_descriptorSize =
    device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  m_rtvDescriptorSize =
    device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  m_dsvDescriptorSize =
    device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
}

void D3D12Device::detectD3D12Capabilities()
{
  auto* device = static_cast<ID3D12Device*>(m_d3d12Device);

  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  if(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
    m_resourceBindingTier = static_cast<uint32_t>(options.ResourceBindingTier);

  const std::array<D3D_SHADER_MODEL, 6> shaderModels{
    D3D_SHADER_MODEL_6_7,
    D3D_SHADER_MODEL_6_6,
    D3D_SHADER_MODEL_6_5,
    D3D_SHADER_MODEL_6_4,
    D3D_SHADER_MODEL_6_3,
    D3D_SHADER_MODEL_6_0,
  };
  for(D3D_SHADER_MODEL requested : shaderModels)
  {
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{requested};
    if(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))))
    {
      m_shaderModel = static_cast<uint32_t>(shaderModel.HighestShaderModel);
      break;
    }
  }

  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
  if(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))))
    m_supportsRayTracing = options5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;

  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
  if(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7))))
    m_supportsMeshShaders = options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;

  D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
  if(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12))))
    m_supportsEnhancedBarriers = options12.EnhancedBarriersSupported != FALSE;

  const bool bindlessHardware =
    m_resourceBindingTier >= static_cast<uint32_t>(D3D12_RESOURCE_BINDING_TIER_2) &&
    m_shaderModel >= static_cast<uint32_t>(D3D_SHADER_MODEL_6_6) &&
    m_cbvSrvUavHeap != nullptr && m_samplerHeap != nullptr;

  m_featureInfo.timelineSemaphore = true;
  m_featureInfo.synchronization2 = m_supportsEnhancedBarriers;
  m_featureInfo.dynamicRendering = true;

  m_capabilities.coreGraphics = m_graphicsQueue.commandQueue != nullptr;
  m_capabilities.coreCompute = m_computeQueue.commandQueue != nullptr;
  m_capabilities.coreBindless = bindlessHardware;
  m_capabilities.extensionAsyncCompute = m_computeQueue.dedicated;
  m_capabilities.extensionMeshShader = m_supportsMeshShaders;
  m_capabilities.extensionRayTracing = m_supportsRayTracing;
  m_capabilities.descriptorHeap = bindlessHardware;
  m_capabilities.explicitResidency = ExplicitResidencyLevel::unsupported;
  m_capabilities.pipelineCompiler = false;
  m_capabilities.multiQueue =
    m_graphicsQueue.commandQueue != nullptr &&
    m_computeQueue.commandQueue != nullptr &&
    m_transferQueue.commandQueue != nullptr;
}

void D3D12Device::validateD3D12Capabilities() const
{
  const RHICapabilityError error =
    evaluateCapabilityRequirements(m_capabilities, m_createInfo.capabilityRequirements);
  if(error != RHICapabilityError::None)
    throw std::runtime_error(std::string("D3D12 capability requirement failed: ") + toString(error));
}

void D3D12Device::releaseNativeObjects() noexcept
{
  drainRetirements();
  if(m_infoQueue != nullptr)
  {
    auto* infoQueue = static_cast<ID3D12InfoQueue1*>(m_infoQueue);
    if(m_infoQueueCallbackCookie != 0)
      infoQueue->UnregisterMessageCallback(m_infoQueueCallbackCookie);
    infoQueue->Release();
    m_infoQueue = nullptr;
    m_infoQueueCallbackCookie = 0;
  }

  m_transferQueueApi.reset();
  m_computeQueueApi.reset();
  m_graphicsQueueApi.reset();

  m_argumentTables.clear();
  for(auto& [index, record] : m_pipelines)
  {
    (void)index;
    releaseRetiredPipeline(record);
  }
  m_pipelines.clear();
  m_nextPipelineIndex = 1;
  m_shaderLibraries.clear();
  m_nextShaderLibraryIndex = 1;

  for(auto& [index, record] : m_queryPools)
  {
    (void)index;
    releaseRetiredQueryPool(record);
  }
  m_queryPools.clear();
  m_nextQueryPoolIndex = 1;

  m_argumentLayouts.clear();
  m_samplers.clear();
  m_nextArgumentTableIndex = 1;
  m_nextArgumentLayoutIndex = 1;
  m_nextSamplerIndex = 1;
  m_nextSamplerDescriptor = 0;

  for(auto& [index, record] : m_textureViews)
  {
    (void)index;
    if(record.ownsResourceReference && record.resource != nullptr)
      static_cast<ID3D12Resource*>(record.resource)->Release();
  }
  m_textureViews.clear();
  m_freeTextureViewDescriptors.clear();
  m_freeRtvDescriptors.clear();
  m_freeDsvDescriptors.clear();
  m_nextTextureViewIndex = 1;
  m_nextTextureViewDescriptor = 0;
  m_nextRtvDescriptor = 0;
  m_nextDsvDescriptor = 0;

  for(auto& [index, record] : m_textures)
  {
    (void)index;
    if(record.owned && record.resource != nullptr)
      static_cast<ID3D12Resource*>(record.resource)->Release();
  }
  m_textures.clear();
  m_nextTextureIndex = 1;

  for(auto& [index, record] : m_buffers)
  {
    (void)index;
    auto* resource = static_cast<ID3D12Resource*>(record.resource);
    if(record.mapped != nullptr)
      resource->Unmap(0, nullptr);
    resource->Release();
  }
  m_buffers.clear();
  m_nextBufferIndex = 1;

  releaseNative<ID3D12CommandSignature>(m_dispatchSignature);
  releaseNative<ID3D12CommandSignature>(m_drawIndexedSignature);
  releaseNative<ID3D12CommandSignature>(m_drawSignature);

  releaseNative<ID3D12DescriptorHeap>(m_dsvHeap);
  releaseNative<ID3D12DescriptorHeap>(m_rtvHeap);
  releaseNative<ID3D12DescriptorHeap>(m_samplerStagingHeap);
  releaseNative<ID3D12DescriptorHeap>(m_resourceStagingHeap);
  releaseNative<ID3D12DescriptorHeap>(m_samplerHeap);
  releaseNative<ID3D12DescriptorHeap>(m_cbvSrvUavHeap);
  releaseNative<ID3D12CommandQueue>(m_transferQueue.commandQueue);
  releaseNative<ID3D12CommandQueue>(m_computeQueue.commandQueue);
  releaseNative<ID3D12CommandQueue>(m_graphicsQueue.commandQueue);
  releaseNative<ID3D12Device>(m_d3d12Device);
  releaseNative<IDXGIAdapter1>(m_dxgiAdapter);
  releaseNative<IDXGIFactory4>(m_dxgiFactory);

  m_graphicsQueue = {};
  m_computeQueue = {};
  m_transferQueue = {};
  m_descriptorSize = 0;
  m_samplerDescriptorSize = 0;
  m_rtvDescriptorSize = 0;
  m_dsvDescriptorSize = 0;
}

Result<ResidencySetHandle> D3D12Device::createResidencySet(const ResidencySetDesc&)
{
  return Result<ResidencySetHandle>::fail(
    RHIErrorCode::unsupported, "D3D12 explicit residency is unavailable on this backend path");
}

RHIResult D3D12Device::destroyResidencySet(ResidencySetHandle)
{
  return RHIResult::fail(
    RHIErrorCode::unsupported, "D3D12 explicit residency is unavailable on this backend path");
}

RHIResult D3D12Device::updateResidencySet(
  ResidencySetHandle, const ResidencyUpdateBatch&)
{
  return RHIResult::fail(
    RHIErrorCode::unsupported, "D3D12 explicit residency is unavailable on this backend path");
}
}  // namespace demo::rhi::d3d12
