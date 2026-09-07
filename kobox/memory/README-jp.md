# Linux early-memory gate

このgateは、POSIX host上の共有backingをLinux RAMとして使い、upstream Linuxの
`memblock`、sparse vmemmap、buddy、SLUB、per-CPU allocator、`vmap`までを起動します。

host固有なのは次の境界だけです。

- 同一backingをdirect map、vmemmap、vmalloc aliasへmapする操作
- `page_offset_base`、`vmemmap_base`、`vmalloc_base`、`phys_base`のhost配置
- `VMALLOC_END`を実際のhost予約範囲へ接続し、upstreamの割当と
  `is_vmalloc_addr()`／`kvfree()`の範囲判定を一致させること
- early boot中のcurrent CPU、IRQ-disable状態、bootstrap task
- Linuxのcache／PTE／TLB境界に従うhost aliasの公開・無効化

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

alias試験では、通常RAMとcore imageをvmallocアドレスとして扱わないことも確認します。
小さいhost windowにnativeの32 TiB終端を使うと、単独の`vmap()`が成功しても
通常のslab割当を誤分類し得るためです。

`KOBOX_LINUX_BOOT_BUILD_DIR`をcanonical Linux buildへ指定すると、通常のCMake buildとCTestに
`kobox2.linux_early_memory_gate`が追加されます。このgateはtask-port、reclaim、timer、RCU、
workqueueの成立を主張せず、最終的なLinux boot-core DSOそのものでもありません。

現在の[boot memory Gate](../boot/memory-gate-jp.md)は、これらの確保をupstreamの正規boot後に
検証し、dynamic per-CPUのvmalloc chunkと実shmemも使用します。この検証で、CPU unit間の穴を含む
flush範囲へのmachine portの対応を修正しました。host aliasの無効化はPTE除去後のTLB無効化に従い、
解放済みaliasのアクセス不能を要求する試験はupstreamのlazy vmalloc aliasを明示的に排出します。
単独fixtureは過去の回帰確認であり、boot／サービス初期化の完了根拠にはしません。
