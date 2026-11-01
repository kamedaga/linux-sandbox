// SPDX-License-Identifier: GPL-2.0-only
/* Owned diagnostic linked into a staged i915 input before modpost. */
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include "../../drivers/gpu/drm/i915/gt/intel_gt.h"
#include "../../drivers/gpu/drm/i915/gt/intel_rps.h"
#include "../../drivers/gpu/drm/i915/gt/uc/intel_uc.h"

#define RPS_TRACE_SAMPLES 600
struct rps_trace {
    struct list_head link;
    struct delayed_work work;
    struct intel_rps *rps;
    unsigned samples;
};
static LIST_HEAD(traces);
static DEFINE_MUTEX(traces_lock);

static void rps_trace_work(struct work_struct *work)
{
    struct rps_trace *trace = container_of(to_delayed_work(work), struct rps_trace, work);
    struct intel_rps *rps = trace->rps;
    struct intel_gt *gt = container_of(rps, struct intel_gt, rps);
    intel_wakeref_t wakeref;
    u32 actual = 0, requested = 0;

    /* Taking an existing wakeref keeps the sample coherent across suspend
     * without waking an idle device just to measure it. A suspended sample
     * is explicitly labelled, rather than mistaken for a stuck 0 MHz GT. */
    wakeref = intel_runtime_pm_get_if_in_use(gt->uncore->rpm);
    if (wakeref) {
        mutex_lock(&rps->lock);
        actual = intel_rps_read_actual_frequency(rps);
        requested = intel_rps_get_requested_frequency(rps);
        mutex_unlock(&rps->lock);
    }
    pr_info("kobox-drm: rps gt=%u sample=%u time_ms=%llu pm=%u actual_mhz=%u requested_mhz=%u rpn_mhz=%u rp0_mhz=%u min_mhz=%u max_mhz=%u slpc=%u active=%u interrupts=%u\n",
            gt->info.id, ++trace->samples, (unsigned long long)(ktime_get_ns() / 1000000),
            !!wakeref, actual, requested, intel_rps_get_rpn_frequency(rps),
            intel_rps_get_rp0_frequency(rps), intel_rps_get_min_frequency(rps),
            intel_rps_get_max_frequency(rps), intel_uc_uses_guc_slpc(&gt->uc),
            intel_rps_is_active(rps), intel_rps_has_interrupts(rps));
    if (wakeref)
        intel_runtime_pm_put(gt->uncore->rpm, wakeref);
    if (trace->samples < RPS_TRACE_SAMPLES)
        schedule_delayed_work(&trace->work, HZ);
}

extern void __real_intel_rps_driver_register(struct intel_rps *rps);
extern void __real_intel_rps_driver_unregister(struct intel_rps *rps);

void __wrap_intel_rps_driver_register(struct intel_rps *rps)
{
    struct rps_trace *trace;

    __real_intel_rps_driver_register(rps);
    trace = kzalloc(sizeof(*trace), GFP_KERNEL);
    if (!trace) {
        pr_warn("kobox-drm: rps diagnostic allocation failed\n");
        return;
    }
    trace->rps = rps;
    INIT_DELAYED_WORK(&trace->work, rps_trace_work);
    mutex_lock(&traces_lock);
    list_add(&trace->link, &traces);
    schedule_delayed_work(&trace->work, HZ);
    mutex_unlock(&traces_lock);
}

void __wrap_intel_rps_driver_unregister(struct intel_rps *rps)
{
    struct rps_trace *trace, *found = NULL;

    mutex_lock(&traces_lock);
    list_for_each_entry(trace, &traces, link) {
        if (trace->rps == rps) {
            list_del(&trace->link);
            found = trace;
            break;
        }
    }
    mutex_unlock(&traces_lock);
    if (found) {
        /* Driver unregister precedes GT destruction. Join running work and
         * cancel its self-requeue before the borrowed rps pointer can die.
         * The worker never needs traces_lock, so cancellation cannot deadlock. */
        cancel_delayed_work_sync(&found->work);
        kfree(found);
    }
    __real_intel_rps_driver_unregister(rps);
}
