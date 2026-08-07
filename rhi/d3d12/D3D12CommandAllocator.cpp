#include "D3D12CommandAllocator.h"

#include "D3D12CommandBuffer.h"
#include "D3D12Device.h"
#include "D3D12Queue.h"

#include <d3d12.h>

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace demo::rhi::d3d12 {
namespace {

void require(bool condition, const char* message)
{
  if(!condition)
    throw std::runtime_error(message);
}

void checkHresult(HRESULT result, const char* operation)
{
  if(FAILED(result))
  {
    std::ostringstream message;
    message << operation << " failed (HRESULT=0x" << std::hex
            << static_cast<uint32_t>(result) << ')';
    throw std::runtime_error(message.str());
  }
}

}  // namespace

D3D12CommandAllocator::D3D12CommandAllocator(
  D3D12Device& owner, D3D12Queue& queue)
  : m_owner(owner), m_queue(queue)
{
  ID3D12CommandAllocator* allocator = nullptr;
  checkHresult(static_cast<ID3D12Device*>(queue.nativeDevice())->CreateCommandAllocator(
                 static_cast<D3D12_COMMAND_LIST_TYPE>(queue.nativeType()),
                 IID_PPV_ARGS(&allocator)),
               "ID3D12Device::CreateCommandAllocator");
  m_allocator = allocator;
}

D3D12CommandAllocator::~D3D12CommandAllocator()
{
  if(!m_commandBuffers.empty())
    std::terminate();
  if(m_allocator != nullptr)
    static_cast<ID3D12CommandAllocator*>(m_allocator)->Release();
}

QueueClass D3D12CommandAllocator::queueClass() const noexcept
{
  return m_queue.info().queueClass;
}

void D3D12CommandAllocator::reset(SubmissionToken completedToken)
{
  if(m_lastSubmission.isValid())
  {
    require(completedToken.isValid(),
            "D3D12CommandAllocator::reset requires its completion token");
    require(completedToken.queue == m_lastSubmission.queue,
            "D3D12CommandAllocator::reset rejected a wrong-queue token");
    require(completedToken.value >= m_lastSubmission.value,
            "D3D12CommandAllocator::reset rejected a stale token");
    require(m_queue.isComplete(m_lastSubmission),
            "D3D12CommandAllocator::reset called before GPU completion");
  }
  else
  {
    require(!completedToken.isValid() || completedToken.queue == m_queue.identity(),
            "D3D12CommandAllocator::reset rejected a wrong-queue token");
  }

  checkHresult(static_cast<ID3D12CommandAllocator*>(m_allocator)->Reset(),
               "ID3D12CommandAllocator::Reset");
  for(D3D12CommandBuffer* commandBuffer : m_commandBuffers)
    commandBuffer->markReusable();
  m_lastSubmission = {};
}

std::unique_ptr<CommandBuffer> D3D12CommandAllocator::createCommandBuffer()
{
  ID3D12GraphicsCommandList* commandList = nullptr;
  checkHresult(static_cast<ID3D12Device*>(m_queue.nativeDevice())->CreateCommandList(
                 0, static_cast<D3D12_COMMAND_LIST_TYPE>(m_queue.nativeType()),
                 static_cast<ID3D12CommandAllocator*>(m_allocator), nullptr,
                 IID_PPV_ARGS(&commandList)),
               "ID3D12Device::CreateCommandList");
  checkHresult(commandList->Close(), "ID3D12GraphicsCommandList::Close(initial)");

  auto commandBuffer = std::make_unique<D3D12CommandBuffer>(
    *this, commandList, &m_owner);
  m_commandBuffers.push_back(commandBuffer.get());
  return commandBuffer;
}

void D3D12CommandAllocator::noteSubmitted(
  D3D12CommandBuffer& commandBuffer, SubmissionToken token)
{
  require(std::find(m_commandBuffers.begin(), m_commandBuffers.end(), &commandBuffer)
            != m_commandBuffers.end(),
          "D3D12CommandAllocator received an unknown command buffer");
  require(token.queue == m_queue.identity(),
          "D3D12CommandAllocator received a wrong-queue token");
  if(!m_lastSubmission.isValid() || token.value > m_lastSubmission.value)
    m_lastSubmission = token;
}

void D3D12CommandAllocator::releaseCommandBuffer(
  D3D12CommandBuffer& commandBuffer, void* nativeCommandList) noexcept
{
  const auto found = std::find(m_commandBuffers.begin(), m_commandBuffers.end(), &commandBuffer);
  if(found != m_commandBuffers.end())
    m_commandBuffers.erase(found);
  if(nativeCommandList != nullptr)
    static_cast<ID3D12GraphicsCommandList*>(nativeCommandList)->Release();
}

}  // namespace demo::rhi::d3d12
