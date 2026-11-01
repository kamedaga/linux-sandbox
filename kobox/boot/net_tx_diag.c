// SPDX-License-Identifier: GPL-2.0-only
/* Diagnostic link wrappers: upstream sources and module ABIs stay intact.
 * Durations are inclusive and may nest; do not sum these as disjoint CPU time.
 * In particular, Linux task switches include the native handoff/park interval.
 */
#include "net_perf.h"
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/hrtimer.h>
#include <linux/completion.h>
#include <linux/timekeeping.h>
#include <linux/printk.h>

#define VOID_WRAP(name, part, args, call) \
 extern void __real_##name args; \
 void __wrap_##name args { \
  u64 begin=kobox_net_tx_probe_begin(); __real_##name call; \
  kobox_net_tx_probe_end(part,begin); }
#define RET_WRAP(type, name, part, args, call) \
 extern type __real_##name args; \
 type __wrap_##name args { \
  u64 begin=kobox_net_tx_probe_begin(); type result=__real_##name call; \
  kobox_net_tx_probe_end(part,begin); return result; }
VOID_WRAP(schedule,KOBOX_TX_SCHEDULE,(void),())
RET_WRAP(long,schedule_timeout,KOBOX_TX_TIMEOUT,(long timeout),(timeout))
VOID_WRAP(mutex_lock,KOBOX_TX_MUTEX,(struct mutex *lock),(lock))
RET_WRAP(int,mutex_lock_interruptible,KOBOX_TX_MUTEX,(struct mutex *lock),(lock))
VOID_WRAP(__local_bh_enable_ip,KOBOX_TX_BH_ENABLE,(unsigned long ip,unsigned int cnt),(ip,cnt))
VOID_WRAP(__do_softirq,KOBOX_TX_SOFTIRQ,(void),())
VOID_WRAP(msleep,KOBOX_TX_MSLEEP,(unsigned int msecs),(msecs))
VOID_WRAP(usleep_range_state,KOBOX_TX_USLEEP,(unsigned long min,unsigned long max,unsigned int state),(min,max,state))
VOID_WRAP(__const_udelay,KOBOX_TX_UDELAY,(unsigned long loops),(loops))
VOID_WRAP(__udelay,KOBOX_TX_UDELAY,(unsigned long usecs),(usecs))
VOID_WRAP(wait_for_completion,KOBOX_TX_COMPLETION,(struct completion *x),(x))
RET_WRAP(unsigned long,wait_for_completion_timeout,KOBOX_TX_COMPLETION,(struct completion *x,unsigned long timeout),(x,timeout))
VOID_WRAP(hrtimer_start_range_ns,KOBOX_TX_TIMER,(struct hrtimer *timer,ktime_t tim,unsigned long delta,enum hrtimer_mode mode),(timer,tim,delta,mode))
extern int __real_wake_up_process(struct task_struct *p);
int __wrap_wake_up_process(struct task_struct *p)
{
 u64 begin=kobox_net_tx_probe_begin();
 kobox_net_tx_event(1,p,READ_ONCE(p->__state));
 int result=__real_wake_up_process(p);kobox_net_tx_probe_end(KOBOX_TX_WAKE,begin);return result;
}
extern bool __real_queue_work_on(int cpu,struct workqueue_struct *wq,struct work_struct *work);
bool __wrap_queue_work_on(int cpu,struct workqueue_struct *wq,struct work_struct *work)
{
 u64 begin=kobox_net_tx_probe_begin();kobox_net_tx_event(2,work,cpu);
 bool result=__real_queue_work_on(cpu,wq,work);kobox_net_tx_probe_end(KOBOX_TX_WORK,begin);return result;
}
extern struct task_struct *__real_kobox_task_switch(struct task_struct *previous,struct task_struct *next);
struct task_struct *__wrap_kobox_task_switch(struct task_struct *previous,struct task_struct *next)
{
 u64 begin=kobox_net_tx_probe_begin();kobox_net_tx_event(3,next,READ_ONCE(previous->__state));
 struct task_struct *result=__real_kobox_task_switch(previous,next);
 kobox_net_tx_probe_end(KOBOX_TX_SWITCH,begin);return result;
}
/* Exclude the diagnostic collector's own lock to prevent instrumentation
 * recursion. Contended classification observes the lock before acquiring it;
 * duration includes preemption and does not prove that all time was spinning. */
#define SPIN_WRAP(name,args,call) \
 extern void __real_##name args; \
 void __wrap_##name args { \
  u64 begin=kobox_net_tx_probe_skip_lock(lock)?0:kobox_net_tx_probe_begin(); \
  unsigned part=raw_spin_is_locked(lock)?KOBOX_TX_SPIN_CONTENDED:KOBOX_TX_SPIN; \
  __real_##name call;kobox_net_tx_probe_end(part,begin); }
SPIN_WRAP(_raw_spin_lock,(raw_spinlock_t *lock),(lock))
SPIN_WRAP(_raw_spin_lock_bh,(raw_spinlock_t *lock),(lock))
SPIN_WRAP(_raw_spin_lock_irq,(raw_spinlock_t *lock),(lock))
extern unsigned long __real__raw_spin_lock_irqsave(raw_spinlock_t *lock);
unsigned long __wrap__raw_spin_lock_irqsave(raw_spinlock_t *lock)
{
 u64 begin=kobox_net_tx_probe_skip_lock(lock)?0:kobox_net_tx_probe_begin();
 unsigned part=raw_spin_is_locked(lock)?KOBOX_TX_SPIN_CONTENDED:KOBOX_TX_SPIN;
 unsigned long flags=__real__raw_spin_lock_irqsave(lock);
 kobox_net_tx_probe_end(part,begin);return flags;
}

RET_WRAP(ktime_t,ktime_get,KOBOX_TX_CLOCK,(void),())
RET_WRAP(ktime_t,ktime_get_with_offset,KOBOX_TX_CLOCK,(enum tk_offsets offs),(offs))
RET_WRAP(int,vprintk_emit,KOBOX_TX_PRINTK,(int facility,int level,const struct dev_printk_info *dev_info,const char *fmt,va_list args),(facility,level,dev_info,fmt,args))

RET_WRAP(int,vprintk,KOBOX_TX_PRINTK,(const char *fmt,va_list args),(fmt,args))
