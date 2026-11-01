// SPDX-License-Identifier: GPL-2.0-only
/* Bounded handoff diagnostics linked before modpost. The upstream i915
 * object is unchanged; every wrapped operation retains its original result. */
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_vblank.h>
#include <drm/drm_vblank_work.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/ktime.h>
#include <linux/printk.h>
#include "../../drivers/gpu/drm/i915/display/intel_display_types.h"
#include "../../drivers/gpu/drm/i915/display/intel_de.h"
#include "../../drivers/gpu/drm/i915/display/intel_dsb_regs.h"

static atomic_t waits = ATOMIC_INIT(0);
static atomic_t works = ATOMIC_INIT(0);
static atomic_t arms = ATOMIC_INIT(0);

extern void __real_drm_atomic_helper_wait_for_flip_done(struct drm_device *,
                                                       struct drm_atomic_state *);
extern int __real_drm_vblank_work_schedule(struct drm_vblank_work *, u64, bool);
extern void __real_drm_crtc_arm_vblank_event(struct drm_crtc *,
                                            struct drm_pending_vblank_event *);

static void snapshot(struct drm_atomic_state *state, int id, const char *phase)
{
    struct drm_crtc *crtc;
    struct drm_crtc_state *next;
    int index;

    for_each_new_crtc_in_state(state, crtc, next, index) {
        struct drm_crtc_commit *commit = next->commit;
        struct drm_vblank_crtc *vb = drm_crtc_vblank_crtc(crtc);
        struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
        struct intel_crtc_state *intel_state = to_intel_crtc_state(next);

        pr_info("kobox-drm: handoff id=%d phase=%s crtc=%u commit=%px flip=%d hw=%d event=%px seq=%llu enabled=%d inmodeset=%d\n",
                id, phase, crtc->base.id, commit,
                commit ? completion_done(&commit->flip_done) : -1,
                commit ? completion_done(&commit->hw_done) : -1,
                next->event, (unsigned long long)drm_crtc_vblank_count(crtc),
                vb ? READ_ONCE(vb->enabled) : -1,
                vb ? READ_ONCE(vb->inmodeset) : -1);
        if (intel_state->use_dsb) {
            struct intel_display *display = to_intel_display(crtc->dev);
            enum pipe pipe = intel_crtc->pipe;

            pr_info("kobox-drm: handoff-dsb id=%d phase=%s crtc=%u event=%px ctrl=%#x irq=%#x head=%#x tail=%#x current=%#x\n",
                    id, phase, crtc->base.id, READ_ONCE(intel_crtc->dsb_event),
                    intel_de_read_fw(display, DSB_CTRL(pipe, INTEL_DSB_0)),
                    intel_de_read_fw(display, DSB_INTERRUPT(pipe, INTEL_DSB_0)),
                    intel_de_read_fw(display, DSB_HEAD(pipe, INTEL_DSB_0)),
                    intel_de_read_fw(display, DSB_TAIL(pipe, INTEL_DSB_0)),
                    intel_de_read_fw(display, DSB_CURRENT_HEAD(pipe, INTEL_DSB_0)));
        }
    }
}

void __wrap_drm_atomic_helper_wait_for_flip_done(struct drm_device *dev,
                                                struct drm_atomic_state *state)
{
    int id = atomic_inc_return(&waits);
    u64 start = ktime_get_ns();

    if (id <= 4)
        snapshot(state, id, "before");
    __real_drm_atomic_helper_wait_for_flip_done(dev, state);
    if (id <= 4) {
        snapshot(state, id, "after");
        pr_info("kobox-drm: handoff id=%d elapsed_us=%llu\n", id,
                (unsigned long long)((ktime_get_ns() - start) / 1000));
    }
}

int __wrap_drm_vblank_work_schedule(struct drm_vblank_work *work, u64 count,
                                   bool nextonmiss)
{
    int id = atomic_inc_return(&works);
    int result = __real_drm_vblank_work_schedule(work, count, nextonmiss);

    if (id <= 12)
        pr_info("kobox-drm: vblank-work id=%d work=%px pipe=%d target=%llu result=%d enabled=%d inmodeset=%d cancelling=%d\n",
                id, work, work->vblank->pipe, (unsigned long long)count, result,
                READ_ONCE(work->vblank->enabled),
                READ_ONCE(work->vblank->inmodeset), READ_ONCE(work->cancelling));
    return result;
}

void __wrap_drm_crtc_arm_vblank_event(struct drm_crtc *crtc,
                                     struct drm_pending_vblank_event *event)
{
    int id = atomic_inc_return(&arms);

    if (id <= 12)
        pr_info("kobox-drm: vblank-arm id=%d crtc=%u event=%px completion=%px seq=%llu\n",
                id, crtc->base.id, event, event->base.completion,
                (unsigned long long)drm_crtc_vblank_count(crtc));
    __real_drm_crtc_arm_vblank_event(crtc, event);
}
