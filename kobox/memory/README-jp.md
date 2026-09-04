# Linux early-memory gate

このgateは、POSIX host上の共有backingをLinux RAMとして使い、upstream Linuxの
`memblock`、sparse vmemmap、buddy、SLUB、per-CPU allocator、`vmap`までを起動します。

host固有なのは次の境界だけです。

- 同一backingをdirect map、vmemmap、vmalloc aliasへmapする操作
- `page_offset_base`、`vmemmap_base`、`vmalloc_base`、`phys_base`のhost配置
- early boot中のcurrent CPU、IRQ-disable状態、bootstrap task
- host page tableを変更しないためのcache/TLB hook

`linux-early-memory-gate.so`に入るallocator本体はLinux object由来です。inventoryは各gate symbolの
source objectを固定します。task、workqueue、RCU、IRQ、reclaim/OOMなど後続phaseへのimportは
`linux-early-memory-boundary.so`へ分離します。このtest fixtureは関数呼び出しを診断終了させ、dataを
`PROT_NONE`にするため、memory gateが未完成phaseへ入ると成功せず停止します。early boot中の
`__cond_resched()`もreschedule要求がない場合だけ通し、要求を検出すると停止します。

Gateは次を実行して確認します。

- `mm_core_init()`とSLUB availability
- `alloc_pages()`とpage/PFN/direct-map往復
- `kmalloc()`と独自`kmem_cache`
- static/dynamic per-CPU領域を2 logical CPUで分離
- page allocatorとradix treeのearly CPU-hotplug callbackを後続phase用に保持
- 2 pageを同じRAM backingから`vmap()`し、両方向のalias書き込み

`KOBOX_LINUX_BOOT_BUILD_DIR`をcanonical Linux buildへ指定すると、通常のCMake buildとCTestに
`kobox2.linux_early_memory_gate`が追加されます。このgateはtask-port、reclaim、timer、RCU、
workqueueの成立を主張せず、最終的なLinux boot-core DSOそのものでもありません。
