#pragma once

#include <cstdint>

namespace demo::rhi {

enum class RHIErrorCode : uint8_t
{
  none = 0,
  unsupported,
  invalidArgument,
  invalidHandle,
  wrongDevice,
  invalidState,
  outOfMemory,
  backendFailure,
  deviceLost,
  timeout,
};

struct RHIError
{
  RHIErrorCode code{RHIErrorCode::none};
  const char* message{nullptr};

  [[nodiscard]] constexpr bool isError() const noexcept
  {
    return code != RHIErrorCode::none;
  }
};

template <typename T>
struct Result
{
  T value{};
  RHIError error{};

  [[nodiscard]] constexpr bool succeeded() const noexcept { return !error.isError(); }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return succeeded(); }

  [[nodiscard]] static constexpr Result ok(T result) noexcept
  {
    return Result{result, {}};
  }

  [[nodiscard]] static constexpr Result fail(RHIErrorCode code, const char* message) noexcept
  {
    return Result{{}, RHIError{code, message}};
  }
};

template <>
struct Result<void>
{
  RHIError error{};

  [[nodiscard]] constexpr bool succeeded() const noexcept { return !error.isError(); }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return succeeded(); }

  [[nodiscard]] static constexpr Result ok() noexcept { return {}; }
  [[nodiscard]] static constexpr Result fail(RHIErrorCode code, const char* message) noexcept
  {
    return Result{RHIError{code, message}};
  }
};

using RHIResult = Result<void>;

}  // namespace demo::rhi
