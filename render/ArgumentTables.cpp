#include "ArgumentTables.h"

namespace demo
{
	static_assert(demo::rhi::isValidResourceIndex(kMaterialBindlessTexturesIndex),
	              "Material bindless logical index must be valid");
	static_assert(demo::rhi::isValidResourceIndex(kSceneBindlessInfoIndex),
	              "Scene bindless logical index must be valid");
	static_assert(isStableRootBindingSlot(RootBindingSlot::primaryConstants),
	              "Primary root constants must remain logical root slot 0");
} // namespace demo
