#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#if defined(DEMO_PLATFORM_IOS)
#include <Foundation/Foundation.hpp>
#include <TargetConditionals.h>

// The iOS 26 simulator Metal stub omits a few constants that metal-cpp's
// aggregate header exposes. Use metal-cpp's dynamic lookup path there so an
// unavailable optional constant resolves to nullptr instead of a link error.
#if TARGET_OS_SIMULATOR
#include <Metal/MTLPrivate.hpp>
#undef _MTL_PRIVATE_DEF_CONST
#define _MTL_PRIVATE_DEF_CONST(type, symbol)                                      \
	_MTL_EXTERN type const MTL##symbol;                                           \
	type const MTL::symbol = MTL::Private::LoadSymbol<type>("MTL" #symbol)
#endif
#endif

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
