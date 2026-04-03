#include "DrawStream.h"

namespace demo {

static_assert(static_cast<uint32_t>(StreamEntryType::setPipeline) == 0u);
static_assert(static_cast<uint32_t>(StreamEntryType::setMaterial) == 1u);
static_assert(static_cast<uint32_t>(StreamEntryType::setMesh) == 2u);
static_assert(static_cast<uint32_t>(StreamEntryType::setDynamicBuffer) == 3u);
static_assert(static_cast<uint32_t>(StreamEntryType::setDynamicOffset) == 4u);
static_assert(static_cast<uint32_t>(StreamEntryType::draw) == 5u);

}  // namespace demo
