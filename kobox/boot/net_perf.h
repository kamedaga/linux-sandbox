/* Diagnostic keys contain no payload; no shared-memory ABI is extended. */
#ifndef KOBOX_NET_PERF_H
#define KOBOX_NET_PERF_H
static inline unsigned long long kobox_net_perf_tsc(void)
{
 unsigned a,d; __asm__ volatile("lfence;rdtsc;lfence":"=a"(a),"=d"(d)::"memory");
 return ((unsigned long long)d<<32)|a;
}
static inline unsigned long long kobox_net_perf_key(const void *frame, unsigned long length)
{
 const unsigned char *p=frame; unsigned long n=length<96?length:96;
 unsigned long long h=1469598103934665603ULL ^ length;
 for(unsigned long i=0;i<n;i++)h=(h^p[i])*1099511628211ULL;
 return h;
}
/* Private diagnostics only; no resource or module ABI changes. */
enum kobox_net_tx_part { KOBOX_TX_MMIO_READ, KOBOX_TX_MMIO_WRITE,
 KOBOX_TX_DMA_MAP, KOBOX_TX_DMA_UNMAP, KOBOX_TX_DMA_SYNC, KOBOX_TX_SCHEDULE, KOBOX_TX_TIMEOUT, KOBOX_TX_MUTEX,
 KOBOX_TX_SPIN, KOBOX_TX_SPIN_CONTENDED, KOBOX_TX_BH_ENABLE, KOBOX_TX_SOFTIRQ,
 KOBOX_TX_MSLEEP, KOBOX_TX_USLEEP, KOBOX_TX_UDELAY, KOBOX_TX_WAKE,
 KOBOX_TX_WORK, KOBOX_TX_SWITCH, KOBOX_TX_COMPLETION, KOBOX_TX_TIMER, KOBOX_TX_CLOCK, KOBOX_TX_PRINTK, KOBOX_TX_PARTS };
struct kobox_net_tx_cost { unsigned long long cycles[KOBOX_TX_PARTS];
 unsigned calls[KOBOX_TX_PARTS]; unsigned long long max_cycles, max_pc; unsigned max_part; };
#if KOBOX_NET_PERF_TRACE
unsigned long long kobox_net_tx_probe_begin(void);
int kobox_net_tx_probe_skip_lock(const void *lock);
void kobox_net_mmio_total(unsigned write, unsigned long long pc);
void kobox_net_tx_event(unsigned kind, const void *peer, unsigned long state);
void kobox_net_tx_probe_end_at(unsigned part, unsigned long long begin, unsigned long long pc);
#else
static inline unsigned long long kobox_net_tx_probe_begin(void) { return 0; }
static inline int kobox_net_tx_probe_skip_lock(const void *lock) { (void)lock; return 1; }
static inline void kobox_net_mmio_total(unsigned write, unsigned long long pc) { (void)write; (void)pc; }
static inline void kobox_net_tx_event(unsigned kind, const void *peer, unsigned long state) { (void)kind; (void)peer; (void)state; }
static inline void kobox_net_tx_probe_end_at(unsigned part, unsigned long long begin, unsigned long long pc)
{ (void)part; (void)begin; (void)pc; }
#endif
#define kobox_net_tx_probe_end(part, begin) \
 kobox_net_tx_probe_end_at((part), (begin), (unsigned long long)__builtin_return_address(0))
#endif
