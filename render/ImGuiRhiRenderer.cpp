#include "ImGuiRhiRenderer.h"

#include "ArgumentTables.h"

#include "../common/Common.h"
#include "../common/Logger.h"

#include "_autogen/imgui.slang.h"

#include <imgui.h>
#ifdef __ANDROID__
#  include <backends/imgui_impl_android.h>
#else
#  include <backends/imgui_impl_glfw.h>
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace demo
{
  namespace
  {
    void drawCallbackResetRenderState(const ImDrawList*, const ImDrawCmd*) {}
    void drawCallbackSetSamplerLinear(const ImDrawList*, const ImDrawCmd*) {}
    void drawCallbackSetSamplerNearest(const ImDrawList*, const ImDrawCmd*) {}

    constexpr uint32_t kTextureArgumentSlot = static_cast<uint32_t>(ArgumentSlot::material);
    constexpr uint32_t kProjectionRootSlot = static_cast<uint32_t>(RootBindingSlot::primaryConstants);

    struct FrameBuffers
    {
      rhi::BufferHandle vertex{};
      rhi::BufferHandle index{};
      uint64_t vertexCapacity{0};
      uint64_t indexCapacity{0};
    };

    struct TextureEntry
    {
      uint32_t generation{1};
      bool alive{false};
      bool owned{false};
      rhi::TextureHandle texture{};
      rhi::TextureViewHandle view{};
      rhi::ArgumentTableHandle linearTable{};
      rhi::ArgumentTableHandle nearestTable{};
      rhi::ArgumentAccessIntent accessIntent{rhi::ArgumentAccessIntent::sampledRead};
      rhi::ResourceState state{rhi::ResourceState::Undefined};
    };
  } // namespace

  struct ImGuiRhiRenderer::Impl
  {
    rhi::Device* device{nullptr};
    uint32_t frameCount{0};
    bool initialized{false};

    rhi::ArgumentLayoutHandle emptyLayout{};
    rhi::ArgumentLayoutHandle textureLayout{};
    rhi::SamplerHandle linearSampler{};
    rhi::SamplerHandle nearestSampler{};
    rhi::PipelineHandle pipeline{};

    std::vector<FrameBuffers> frameBuffers;
    std::vector<TextureEntry> textures{1};
    std::vector<uint32_t> freeTextureIndices;

    [[nodiscard]] rhi::ArgumentTableHandle createTextureTable(
      rhi::TextureViewHandle view,
      rhi::SamplerHandle sampler,
      rhi::ArgumentAccessIntent accessIntent) const
    {
      const rhi::ArgumentTableHandle table = device->createArgumentTable(textureLayout);
      const rhi::ArgumentWrite write{
        .binding = 0,
        .type = rhi::ArgumentType::combinedImageSampler,
        .textureView = view,
        .sampler = sampler,
        .accessIntent = accessIntent,
      };
      device->updateArgumentTable(table, 1, &write);
      return table;
    }

    [[nodiscard]] TextureID addTexture(
      rhi::TextureViewHandle view,
      rhi::ArgumentAccessIntent accessIntent,
      bool owned,
      rhi::TextureHandle texture,
      rhi::ResourceState state)
    {
      uint32_t index = 0;
      if (!freeTextureIndices.empty())
      {
        index = freeTextureIndices.back();
        freeTextureIndices.pop_back();
      }
      else
      {
        index = static_cast<uint32_t>(textures.size());
        textures.emplace_back();
      }

      TextureEntry& entry = textures[index];
      entry.alive = true;
      entry.owned = owned;
      entry.texture = texture;
      entry.view = view;
      entry.accessIntent = accessIntent;
      entry.state = state;
      entry.linearTable = createTextureTable(view, linearSampler, accessIntent);
      entry.nearestTable = createTextureTable(view, nearestSampler, accessIntent);
      return ui::encodeTextureId(ui::TextureHandle{index, entry.generation});
    }

    [[nodiscard]] TextureEntry* resolveTexture(TextureID textureId)
    {
      const ui::TextureHandle handle = ui::decodeTextureId(textureId);
      if (handle.index == 0 || handle.index >= textures.size())
      {
        return nullptr;
      }
      TextureEntry& entry = textures[handle.index];
      if (!entry.alive || entry.generation != handle.generation)
      {
        return nullptr;
      }
      return &entry;
    }

    void removeTexture(TextureID textureId)
    {
      const ui::TextureHandle handle = ui::decodeTextureId(textureId);
      TextureEntry* entry = resolveTexture(textureId);
      if (entry == nullptr)
      {
        return;
      }

      if (!entry->linearTable.isNull())
      {
        device->destroyArgumentTable(entry->linearTable);
      }
      if (!entry->nearestTable.isNull())
      {
        device->destroyArgumentTable(entry->nearestTable);
      }
      if (entry->owned)
      {
        if (!entry->view.isNull())
        {
          device->destroyTextureView(entry->view);
        }
        if (!entry->texture.isNull())
        {
          device->destroyTexture(entry->texture);
        }
      }

      const uint32_t nextGeneration = entry->generation + 1u;
      *entry = TextureEntry{};
      entry->generation = nextGeneration == 0u ? 1u : nextGeneration;
      freeTextureIndices.push_back(handle.index);
    }

    void ensureFrameBuffers(FrameBuffers& buffers, uint64_t vertexBytes, uint64_t indexBytes)
    {
      if (vertexBytes > buffers.vertexCapacity)
      {
        if (!buffers.vertex.isNull())
        {
          device->destroyBuffer(buffers.vertex);
        }
        buffers.vertexCapacity = ui::nextBufferCapacity(buffers.vertexCapacity, vertexBytes);
        buffers.vertex = device->createBuffer(rhi::BufferDesc{
          .size = buffers.vertexCapacity,
          .usage = rhi::BufferUsageFlags::vertex,
          .memoryUsage = rhi::MemoryUsage::cpuToGpu,
          .debugName = "ImGuiVertexBuffer",
        });
      }

      if (indexBytes > buffers.indexCapacity)
      {
        if (!buffers.index.isNull())
        {
          device->destroyBuffer(buffers.index);
        }
        buffers.indexCapacity = ui::nextBufferCapacity(buffers.indexCapacity, indexBytes);
        buffers.index = device->createBuffer(rhi::BufferDesc{
          .size = buffers.indexCapacity,
          .usage = rhi::BufferUsageFlags::index,
          .memoryUsage = rhi::MemoryUsage::cpuToGpu,
          .debugName = "ImGuiIndexBuffer",
        });
      }
    }

    void uploadDrawData(ImDrawData& drawData, FrameBuffers& buffers)
    {
      const uint64_t vertexBytes =
        static_cast<uint64_t>(drawData.TotalVtxCount) * sizeof(ImDrawVert);
      const uint64_t indexBytes =
        static_cast<uint64_t>(drawData.TotalIdxCount) * sizeof(ImDrawIdx);
      if (vertexBytes == 0 || indexBytes == 0)
      {
        return;
      }

      ensureFrameBuffers(buffers, vertexBytes, indexBytes);
      auto* vertexDestination = static_cast<std::byte*>(device->mapBuffer(buffers.vertex));
      auto* indexDestination = static_cast<std::byte*>(device->mapBuffer(buffers.index));
      ASSERT(vertexDestination != nullptr && indexDestination != nullptr,
             "ImGui RHI buffers must be host visible");

      for (const ImDrawList* drawList : drawData.CmdLists)
      {
        const size_t drawVertexBytes =
          static_cast<size_t>(drawList->VtxBuffer.Size) * sizeof(ImDrawVert);
        const size_t drawIndexBytes =
          static_cast<size_t>(drawList->IdxBuffer.Size) * sizeof(ImDrawIdx);
        std::memcpy(vertexDestination, drawList->VtxBuffer.Data, drawVertexBytes);
        std::memcpy(indexDestination, drawList->IdxBuffer.Data, drawIndexBytes);
        vertexDestination += drawVertexBytes;
        indexDestination += drawIndexBytes;
      }

      device->unmapBuffer(buffers.vertex);
      device->unmapBuffer(buffers.index);
    }

    void uploadTexture(rhi::CommandBuffer& commandBuffer, ImTextureData& textureData)
    {
      const bool create = textureData.Status == ImTextureStatus_WantCreate;
      ASSERT(textureData.Format == ImTextureFormat_RGBA32,
             "ImGui RHI renderer currently requires RGBA32 textures");

      TextureID textureId = static_cast<TextureID>(textureData.GetTexID());
      TextureEntry* entry = resolveTexture(textureId);
      if (create)
      {
        ASSERT(textureData.GetTexID() == ImTextureID_Invalid &&
               textureData.BackendUserData == nullptr,
               "New ImGui textures must not already own backend data");

        const rhi::TextureHandle texture = device->createTexture(rhi::TextureDesc{
          .dimension = rhi::TextureDimension::e2D,
          .format = rhi::TextureFormat::rgba8Unorm,
          .usage = rhi::TextureUsageFlags::sampled | rhi::TextureUsageFlags::transferDst,
          .extent = {
            static_cast<uint32_t>(textureData.Width),
            static_cast<uint32_t>(textureData.Height),
            1,
          },
          .debugName = "ImGuiDynamicTexture",
        });
        const rhi::TextureViewHandle view = device->createTextureView(rhi::TextureViewCreateDesc{
          .image = texture,
          .format = rhi::TextureFormat::rgba8Unorm,
          .viewType = rhi::ImageViewType::e2D,
          .aspect = rhi::TextureAspect::color,
          .levelCount = 1,
          .layerCount = 1,
          .debugName = "ImGuiDynamicTextureView",
        });
        textureId = addTexture(
          view,
          rhi::ArgumentAccessIntent::sampledRead,
          true,
          texture,
          rhi::ResourceState::Undefined);
        entry = resolveTexture(textureId);
        textureData.SetTexID(static_cast<ImTextureID>(textureId));
        textureData.BackendUserData =
          reinterpret_cast<void*>(static_cast<uintptr_t>(textureId));
      }

      ASSERT(entry != nullptr, "ImGui dynamic texture ID must resolve");
      const int uploadX = create ? 0 : textureData.UpdateRect.x;
      const int uploadY = create ? 0 : textureData.UpdateRect.y;
      const int uploadWidth = create ? textureData.Width : textureData.UpdateRect.w;
      const int uploadHeight = create ? textureData.Height : textureData.UpdateRect.h;
      if (uploadWidth <= 0 || uploadHeight <= 0)
      {
        textureData.SetStatus(ImTextureStatus_OK);
        return;
      }

      const uint64_t rowBytes =
        static_cast<uint64_t>(uploadWidth) * static_cast<uint64_t>(textureData.BytesPerPixel);
      const uint64_t uploadBytes = rowBytes * static_cast<uint64_t>(uploadHeight);
      const rhi::BufferHandle staging = device->createBuffer(rhi::BufferDesc{
        .size = uploadBytes,
        .usage = rhi::BufferUsageFlags::transferSrc,
        .memoryUsage = rhi::MemoryUsage::cpuToGpu,
        .debugName = "ImGuiTextureUpload",
      });
      auto* destination = static_cast<std::byte*>(device->mapBuffer(staging));
      ASSERT(destination != nullptr, "ImGui texture staging buffer must be host visible");
      for (int row = 0; row < uploadHeight; ++row)
      {
        std::memcpy(
          destination + static_cast<uint64_t>(row) * rowBytes,
          textureData.GetPixelsAt(uploadX, uploadY + row),
          static_cast<size_t>(rowBytes));
      }
      device->unmapBuffer(staging);

      const rhi::TextureBarrier beginBarrier{
        .texture = entry->texture,
        .before = entry->state,
        .after = rhi::ResourceState::TransferDst,
      };
      commandBuffer.resourceBarrier(&beginBarrier, 1, nullptr, 0);
      rhi::ComputeEncoder* copy = commandBuffer.beginComputePass();
      ASSERT(copy != nullptr, "ImGui texture upload requires a compute/copy encoder");
      copy->copyBufferToTexture(rhi::BufferTextureCopyDesc{
        .buffer = staging,
        .texture = entry->texture,
        .textureOffset = {uploadX, uploadY, 0},
        .width = static_cast<uint32_t>(uploadWidth),
        .height = static_cast<uint32_t>(uploadHeight),
        .depth = 1,
      });
      commandBuffer.endEncoding();
      const rhi::TextureBarrier endBarrier{
        .texture = entry->texture,
        .before = rhi::ResourceState::TransferDst,
        .after = rhi::ResourceState::ShaderRead,
      };
      commandBuffer.resourceBarrier(&endBarrier, 1, nullptr, 0);
      entry->state = rhi::ResourceState::ShaderRead;
      device->destroyBuffer(staging);
      textureData.SetStatus(ImTextureStatus_OK);
    }

    void destroyTexture(ImTextureData& textureData)
    {
      const TextureID textureId = static_cast<TextureID>(textureData.GetTexID());
      if (textureId != 0u)
      {
        removeTexture(textureId);
      }
      textureData.SetTexID(ImTextureID_Invalid);
      textureData.BackendUserData = nullptr;
      textureData.SetStatus(ImTextureStatus_Destroyed);
    }

    void updateTextures(rhi::CommandBuffer& commandBuffer, ImDrawData& drawData)
    {
      if (drawData.Textures == nullptr)
      {
        return;
      }

      for (ImTextureData* texture : *drawData.Textures)
      {
        if (texture == nullptr || texture->Status == ImTextureStatus_OK)
        {
          continue;
        }
        if (texture->Status == ImTextureStatus_WantCreate ||
            texture->Status == ImTextureStatus_WantUpdates)
        {
          uploadTexture(commandBuffer, *texture);
        }
        else if (texture->Status == ImTextureStatus_WantDestroy &&
                 texture->UnusedFrames >= static_cast<int>(frameCount))
        {
          destroyTexture(*texture);
        }
      }
    }

    void setupRenderState(
      rhi::RenderEncoder& encoder,
      const ImDrawData& drawData,
      const FrameBuffers& buffers,
      uint32_t framebufferWidth,
      uint32_t framebufferHeight) const
    {
      encoder.setPipeline(pipeline);
      encoder.setViewport(rhi::Viewport{
        .width = static_cast<float>(framebufferWidth),
        .height = static_cast<float>(framebufferHeight),
      });
      const rhi::BufferHandle vertexBuffer = buffers.vertex;
      const uint64_t vertexOffset = 0;
      encoder.bindVertexBuffers(0, &vertexBuffer, &vertexOffset, 1);
      encoder.bindIndexBuffer(
        buffers.index,
        0,
        sizeof(ImDrawIdx) == sizeof(uint16_t)
          ? rhi::IndexFormat::uint16
          : rhi::IndexFormat::uint32);

      const ui::Projection projection = ui::makeProjection(
        {drawData.DisplayPos.x, drawData.DisplayPos.y},
        {drawData.DisplaySize.x, drawData.DisplaySize.y});
      encoder.setRootConstants(
        rhi::ShaderStage::vertex,
        kProjectionRootSlot,
        &projection,
        sizeof(projection));
    }

    void draw(
      rhi::RenderEncoder& encoder,
      ImDrawData& drawData,
      const FrameBuffers& buffers,
      uint32_t framebufferWidth,
      uint32_t framebufferHeight)
    {
      setupRenderState(
        encoder,
        drawData,
        buffers,
        framebufferWidth,
        framebufferHeight);

      ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
      bool nearestSampler = false;
      int32_t globalVertexOffset = 0;
      uint32_t globalIndexOffset = 0;
      for (const ImDrawList* drawList : drawData.CmdLists)
      {
        for (const ImDrawCmd& command : drawList->CmdBuffer)
        {
          if (command.UserCallback != nullptr)
          {
            if (command.UserCallback == platformIo.DrawCallback_ResetRenderState)
            {
              setupRenderState(
                encoder,
                drawData,
                buffers,
                framebufferWidth,
                framebufferHeight);
              nearestSampler = false;
            }
            else if (command.UserCallback == platformIo.DrawCallback_SetSamplerLinear)
            {
              nearestSampler = false;
            }
            else if (command.UserCallback == platformIo.DrawCallback_SetSamplerNearest)
            {
              nearestSampler = true;
            }
            else
            {
              command.UserCallback(drawList, &command);
            }
            continue;
          }

          const auto scissor = ui::makeScissor(
            {
              command.ClipRect.x,
              command.ClipRect.y,
              command.ClipRect.z,
              command.ClipRect.w,
            },
            {drawData.DisplayPos.x, drawData.DisplayPos.y},
            {drawData.FramebufferScale.x, drawData.FramebufferScale.y},
            framebufferWidth,
            framebufferHeight);
          if (!scissor.has_value())
          {
            continue;
          }

          TextureEntry* texture =
            resolveTexture(static_cast<TextureID>(command.GetTexID()));
          if (texture == nullptr)
          {
            continue;
          }
          encoder.setScissor(*scissor);
          encoder.setArgumentTable(
            rhi::ShaderStage::fragment,
            kTextureArgumentSlot,
            nearestSampler ? texture->nearestTable : texture->linearTable);
          encoder.drawIndexed(rhi::DrawIndexedDesc{
            .indexCount = command.ElemCount,
            .firstIndex = command.IdxOffset + globalIndexOffset,
            .vertexOffset = static_cast<int32_t>(command.VtxOffset) + globalVertexOffset,
          });
        }
        globalIndexOffset += static_cast<uint32_t>(drawList->IdxBuffer.Size);
        globalVertexOffset += drawList->VtxBuffer.Size;
      }
      encoder.setScissor(rhi::Rect2D{
        .extent = {framebufferWidth, framebufferHeight},
      });
    }

    void destroyResources()
    {
      for (uint32_t index = 1; index < textures.size(); ++index)
      {
        TextureEntry& entry = textures[index];
        if (entry.alive)
        {
          removeTexture(ui::encodeTextureId({index, entry.generation}));
        }
      }
      textures.clear();
      textures.resize(1);
      freeTextureIndices.clear();

      for (FrameBuffers& buffers : frameBuffers)
      {
        if (!buffers.vertex.isNull())
        {
          device->destroyBuffer(buffers.vertex);
        }
        if (!buffers.index.isNull())
        {
          device->destroyBuffer(buffers.index);
        }
      }
      frameBuffers.clear();

      if (!pipeline.isNull())
      {
        device->destroyPipeline(pipeline);
      }
      if (!linearSampler.isNull())
      {
        device->destroySampler(linearSampler);
      }
      if (!nearestSampler.isNull())
      {
        device->destroySampler(nearestSampler);
      }
      if (!textureLayout.isNull())
      {
        device->destroyArgumentLayout(textureLayout);
      }
      if (!emptyLayout.isNull())
      {
        device->destroyArgumentLayout(emptyLayout);
      }
    }
  };

  ImGuiRhiRenderer::ImGuiRhiRenderer()
    : m_impl(std::make_unique<Impl>())
  {
  }

  ImGuiRhiRenderer::~ImGuiRhiRenderer() = default;

  void ImGuiRhiRenderer::init(const InitInfo& info)
  {
    ASSERT(info.device != nullptr, "ImGui RHI renderer requires an RHI device");
    ASSERT(info.frameCount > 0, "ImGui RHI renderer requires at least one frame");
    Impl& impl = *m_impl;
    impl.device = info.device;
    impl.frameCount = info.frameCount;
    impl.frameBuffers.resize(info.frameCount);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
#ifdef __ANDROID__
    ImGui_ImplAndroid_Init(static_cast<ANativeWindow*>(info.window));
#else
    ImGui_ImplGlfw_InitForOther(static_cast<GLFWwindow*>(info.window), true);
#endif

    const rhi::ArgumentBinding textureBinding{
      .binding = 0,
      .type = rhi::ArgumentType::combinedImageSampler,
      .visibility = rhi::ShaderStage::fragment,
    };
    impl.emptyLayout = impl.device->createArgumentLayout(rhi::ArgumentLayoutDesc{
      .debugName = "ImGuiEmptyLayout",
    });
    impl.textureLayout = impl.device->createArgumentLayout(rhi::ArgumentLayoutDesc{
      .bindings = &textureBinding,
      .bindingCount = 1,
      .debugName = "ImGuiTextureLayout",
    });
    impl.linearSampler = impl.device->createSampler(rhi::SamplerDesc{
      .magFilter = rhi::Filter::linear,
      .minFilter = rhi::Filter::linear,
      .mipmapMode = rhi::MipmapMode::linear,
      .addressModeU = rhi::AddressMode::clampToEdge,
      .addressModeV = rhi::AddressMode::clampToEdge,
      .addressModeW = rhi::AddressMode::clampToEdge,
      .debugName = "ImGuiLinearSampler",
    });
    impl.nearestSampler = impl.device->createSampler(rhi::SamplerDesc{
      .magFilter = rhi::Filter::nearest,
      .minFilter = rhi::Filter::nearest,
      .mipmapMode = rhi::MipmapMode::nearest,
      .addressModeU = rhi::AddressMode::clampToEdge,
      .addressModeV = rhi::AddressMode::clampToEdge,
      .addressModeW = rhi::AddressMode::clampToEdge,
      .debugName = "ImGuiNearestSampler",
    });

    const std::array<rhi::PipelineShaderStageDesc, 2> stages{
      rhi::PipelineShaderStageDesc{
        .stage = rhi::ShaderStage::vertex,
        .spirvCode = imgui_slang,
        .spirvSize = std::size(imgui_slang) * sizeof(uint32_t),
        .entryPoint = "vertexMain",
      },
      rhi::PipelineShaderStageDesc{
        .stage = rhi::ShaderStage::fragment,
        .spirvCode = imgui_slang,
        .spirvSize = std::size(imgui_slang) * sizeof(uint32_t),
        .entryPoint = "fragmentMain",
      },
    };
    const rhi::VertexBindingDesc vertexBinding{
      .binding = 0,
      .stride = sizeof(ImDrawVert),
    };
    const std::array<rhi::VertexAttributeDesc, 3> attributes{
      rhi::VertexAttributeDesc{
        .location = 0,
        .binding = 0,
        .format = rhi::VertexFormat::r32g32Sfloat,
        .offset = offsetof(ImDrawVert, pos),
      },
      rhi::VertexAttributeDesc{
        .location = 1,
        .binding = 0,
        .format = rhi::VertexFormat::r32g32Sfloat,
        .offset = offsetof(ImDrawVert, uv),
      },
      rhi::VertexAttributeDesc{
        .location = 2,
        .binding = 0,
        .format = rhi::VertexFormat::r8g8b8a8Unorm,
        .offset = offsetof(ImDrawVert, col),
      },
    };
    const rhi::BlendAttachmentState blend{
      .blendEnable = true,
      .srcColorBlendFactor = rhi::BlendFactor::srcAlpha,
      .dstColorBlendFactor = rhi::BlendFactor::oneMinusSrcAlpha,
      .srcAlphaBlendFactor = rhi::BlendFactor::one,
      .dstAlphaBlendFactor = rhi::BlendFactor::oneMinusSrcAlpha,
    };
    const std::array<rhi::DynamicState, 2> dynamicStates{
      rhi::DynamicState::viewport,
      rhi::DynamicState::scissor,
    };
    const std::array<rhi::PipelineArgumentSlotDesc, 2> argumentSlots{
      rhi::PipelineArgumentSlotDesc{
        .slot = static_cast<uint32_t>(ArgumentSlot::passGlobals),
        .layout = impl.emptyLayout,
        .visibility = rhi::ShaderStage::fragment,
        .debugName = "ImGuiEmpty",
      },
      rhi::PipelineArgumentSlotDesc{
        .slot = kTextureArgumentSlot,
        .layout = impl.textureLayout,
        .visibility = rhi::ShaderStage::fragment,
        .debugName = "ImGuiTexture",
      },
    };
    const rhi::RootBindingDesc rootBinding{
      .slot = kProjectionRootSlot,
      .kind = rhi::RootBindingKind::constants,
      .visibility = rhi::ShaderStage::vertex,
      .size = sizeof(ui::Projection),
      .alignment = alignof(ui::Projection),
      .debugName = "ImGuiProjection",
    };
    impl.pipeline = impl.device->createGraphicsPipeline(rhi::GraphicsPipelineDesc{
      .shaderStages = stages.data(),
      .shaderStageCount = static_cast<uint32_t>(stages.size()),
      .vertexInput = {
        .bindings = &vertexBinding,
        .bindingCount = 1,
        .attributes = attributes.data(),
        .attributeCount = static_cast<uint32_t>(attributes.size()),
      },
      .rasterState = {
        .topology = rhi::PrimitiveTopology::triangleList,
        .polygonMode = rhi::PolygonMode::fill,
        .cullMode = rhi::CullMode::none,
        .frontFace = rhi::FrontFace::counterClockwise,
        .sampleCount = rhi::SampleCount::count1,
      },
      .depthState = {
        .depthTestEnable = false,
        .depthWriteEnable = false,
      },
      .blendStates = &blend,
      .blendStateCount = 1,
      .dynamicStates = dynamicStates.data(),
      .dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
      .renderingInfo = {
        .colorFormats = &info.swapchainFormat,
        .colorFormatCount = 1,
      },
      .bindingSchema = {
        .argumentSlots = argumentSlots.data(),
        .argumentSlotCount = static_cast<uint32_t>(argumentSlots.size()),
        .rootBindings = &rootBinding,
        .rootBindingCount = 1,
      },
    });

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "VKDemo_ImGuiRhiRenderer";
    io.BackendRendererUserData = this;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    platformIo.DrawCallback_ResetRenderState = drawCallbackResetRenderState;
    platformIo.DrawCallback_SetSamplerLinear = drawCallbackSetSamplerLinear;
    platformIo.DrawCallback_SetSamplerNearest = drawCallbackSetSamplerNearest;

    impl.initialized = true;
    LOGI("ImGuiRhiRenderer initialized");
  }

  void ImGuiRhiRenderer::shutdown()
  {
    Impl& impl = *m_impl;
    if (!impl.initialized)
    {
      return;
    }

    if (ImGui::GetCurrentContext() != nullptr)
    {
      for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
      {
        if (texture != nullptr && texture->BackendUserData != nullptr)
        {
          impl.destroyTexture(*texture);
        }
      }

      ImGuiIO& io = ImGui::GetIO();
      io.BackendFlags &=
        ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
      io.BackendRendererName = nullptr;
      io.BackendRendererUserData = nullptr;
      ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
      platformIo.DrawCallback_ResetRenderState = nullptr;
      platformIo.DrawCallback_SetSamplerLinear = nullptr;
      platformIo.DrawCallback_SetSamplerNearest = nullptr;
    }

    impl.destroyResources();
#ifdef __ANDROID__
    ImGui_ImplAndroid_Shutdown();
#else
    ImGui_ImplGlfw_Shutdown();
#endif
    ImGui::DestroyContext();

    impl.initialized = false;
    impl.device = nullptr;
    impl.frameCount = 0;
    LOGI("ImGuiRhiRenderer shut down");
  }

  void ImGuiRhiRenderer::newFrame()
  {
    if (!m_impl->initialized)
    {
      return;
    }
#ifdef __ANDROID__
    ImGui_ImplAndroid_NewFrame();
#else
    ImGui_ImplGlfw_NewFrame();
#endif
    ImGui::NewFrame();
  }

  void ImGuiRhiRenderer::render(
    rhi::CommandBuffer& commandBuffer,
    rhi::TextureHandle target,
    rhi::TextureViewHandle targetView,
    rhi::Extent2D targetExtent,
    uint32_t frameIndex)
  {
    Impl& impl = *m_impl;
    if (!impl.initialized || target.isNull() || targetView.isNull())
    {
      return;
    }

    ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData == nullptr)
    {
      return;
    }
    const uint32_t framebufferWidth = static_cast<uint32_t>(
      drawData->DisplaySize.x * drawData->FramebufferScale.x);
    const uint32_t framebufferHeight = static_cast<uint32_t>(
      drawData->DisplaySize.y * drawData->FramebufferScale.y);

    impl.updateTextures(commandBuffer, *drawData);
    FrameBuffers& buffers = impl.frameBuffers[frameIndex % impl.frameBuffers.size()];
    impl.uploadDrawData(*drawData, buffers);

    const rhi::TextureBarrier beginBarrier{
      .texture = target,
      .before = rhi::ResourceState::General,
      .after = rhi::ResourceState::ColorAttachment,
    };
    commandBuffer.resourceBarrier(&beginBarrier, 1, nullptr, 0);
    const rhi::RenderTargetDesc colorTarget{
      .texture = target,
      .view = targetView,
      .state = rhi::ResourceState::ColorAttachment,
      .loadOp = rhi::LoadOp::load,
      .storeOp = rhi::StoreOp::store,
    };
    const rhi::RenderPassDesc pass{
      .renderArea = {.extent = targetExtent},
      .colorTargets = &colorTarget,
      .colorTargetCount = 1,
    };
    rhi::RenderEncoder* encoder = commandBuffer.beginRenderPass(pass);
    if (encoder != nullptr && framebufferWidth > 0 && framebufferHeight > 0 &&
        drawData->TotalVtxCount > 0 && drawData->TotalIdxCount > 0)
    {
      impl.draw(
        *encoder,
        *drawData,
        buffers,
        framebufferWidth,
        framebufferHeight);
    }
    if (encoder != nullptr)
    {
      commandBuffer.endEncoding();
    }

    const rhi::TextureBarrier endBarrier{
      .texture = target,
      .before = rhi::ResourceState::ColorAttachment,
      .after = rhi::ResourceState::Present,
    };
    commandBuffer.resourceBarrier(&endBarrier, 1, nullptr, 0);
  }

  bool ImGuiRhiRenderer::isInitialized() const
  {
    return m_impl->initialized;
  }

  ImGuiRhiRenderer::TextureID ImGuiRhiRenderer::registerTexture(
    rhi::TextureViewHandle view,
    rhi::ArgumentAccessIntent accessIntent)
  {
    if (!m_impl->initialized || view.isNull())
    {
      return 0;
    }
    return m_impl->addTexture(
      view,
      accessIntent,
      false,
      {},
      accessIntent == rhi::ArgumentAccessIntent::sampledRead
        ? rhi::ResourceState::ShaderRead
        : rhi::ResourceState::General);
  }

  void ImGuiRhiRenderer::unregisterTexture(TextureID textureId)
  {
    if (textureId != 0u && m_impl->device != nullptr)
    {
      m_impl->removeTexture(textureId);
    }
  }
} // namespace demo
