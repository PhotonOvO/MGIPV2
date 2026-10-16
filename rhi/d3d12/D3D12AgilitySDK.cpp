// Direct3D 12 Agility SDK version advertisement.
//
// The system d3d12.dll acts as a loader: when the executable exports D3D12SDKVersion and
// D3D12SDKPath, the loader resolves D3D12Core.dll from that path beside the executable
// instead of using the runtime built into Windows, and reports the paired version through
// ID3D12Device::GetFeatureSupport / the SDK configuration interface. Exports are what put
// the vendored runtime in effect, so they have to be part of the executable.
//
// Compiled into the executable rather than the D3D12 backend sources on purpose: these
// symbols are never referenced by any code, so a static library would drop them and the
// loader would silently fall back to the operating system runtime.

#include <cstdint>

#if !defined(D3D12_AGILITY_SDK_VERSION)
#error "D3D12_AGILITY_SDK_VERSION must be defined by the build"
#endif
#if !defined(D3D12_AGILITY_SDK_PATH)
#error "D3D12_AGILITY_SDK_PATH must be defined by the build"
#endif

extern "C"
{
__declspec(dllexport) extern const unsigned int D3D12SDKVersion =
  static_cast<unsigned int>(D3D12_AGILITY_SDK_VERSION);

// The loader requires a path relative to the executable, terminated with a separator.
__declspec(dllexport) extern const char* D3D12SDKPath = D3D12_AGILITY_SDK_PATH "\\";
}
