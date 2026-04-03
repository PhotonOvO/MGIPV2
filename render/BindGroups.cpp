#include "BindGroups.h"

namespace demo {

static_assert(demo::rhi::isValidResourceIndex(kMaterialBindlessTexturesIndex), "Material bindless logical index must be valid");
static_assert(demo::rhi::isValidResourceIndex(kSceneBindlessInfoIndex), "Scene bindless logical index must be valid");

}  // namespace demo
