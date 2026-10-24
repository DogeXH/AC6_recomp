#include "ac6_widescreen.h"

namespace ac6 {

void WidescreenInit(rex::memory::Memory*) {}

bool WidescreenPatchUiOrtho(uint32_t*, uint64_t, bool) { return false; }

void WidescreenNotifySwapSource(bool, bool) {}

bool WidescreenWantsMarkerQuadFix(uint64_t) { return false; }

void WidescreenShrinkMarkerQuads(uint8_t*, uint32_t, uint32_t, const uint8_t*, bool, uint32_t,
                                 uint32_t) {}

float WidescreenViewportShrinkX() { return 1.0f; }

}  // namespace ac6
