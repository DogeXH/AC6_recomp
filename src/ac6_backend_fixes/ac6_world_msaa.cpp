#include <atomic>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(ac6_world_msaa_1x, false, "AC6/Performance",
                    "Use one sample per pixel for the main scene instead of two. "
                    "Keeps scene/HUD resolution; reduces edge antialiasing.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// The first registration in 821D51C0 creates the main scene color/depth
// group. Wrapper 82331F48 forwards r9 to the manager's r10. In 82349E28
// its low two bits select the sample mode, passed to BOTH 82346D68 (color)
// and 82346F68 (depth). Those calculate EDRAM allocation and tile rectangles
// from the sample mode before creating surfaces. Resolve textures retain the
// requested 1280x720 dimensions. Change the registration, not GPU registers,
// so allocation, surface headers and the engine's tile loop agree.
//
// The 82331F20 wrapper's r6 is a format selector, NOT a sample count. It is
// deliberately untouched, as are the second scene group, effects and menus.
PPC_EXTERN_FUNC(__imp__rex_sub_82331F48);
PPC_FUNC_IMPL(rex_sub_82331F48) {
    PPC_FUNC_PROLOGUE();

    if (REXCVAR_GET(ac6_world_msaa_1x) && uint32_t(ctx.lr) == 0x821D5204 &&
        ctx.r3.u32 == 1 && ctx.r5.u32 == 0x8001 &&
        ctx.r6.u32 == 1280 && ctx.r7.u32 == 720 &&
        ctx.r8.u32 == 0x00030001 && ctx.r9.u32 == 1) {
        ctx.r9.u64 = 0;
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            REXLOG_ERROR("AC6 main scene: 1280x720 color/depth registration "
                         "uses 1x MSAA (was 2x), guest tile layout recalculated");
        }
    }
    __imp__rex_sub_82331F48(ctx, base);
}
