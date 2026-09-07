# 正規bootに統合したmemory・実shmem Gate

第2章の工程1は、単独のmemory／task closureではなく**固定boot-rooted core**を拡張します。
canonicalのbaseと`kobox/task/config`の後に`kobox/boot/config`をmergeし、canonical Linux imageと
全体のhosted coreを再ビルドします。MMUと`SHMEM`・`TMPFS`・`MEMFD_CREATE`を有効化し、builderは
SLUBとsparse vmemmapも必須にします。tiny-shmem、必須機能の欠落／module化、上位VFS／MMの置換は拒否します。

`SHMEM=y`はramfsによるtiny版ではなくupstreamの完全なshmem実装を選びます。`TMPFS=y`はfilesystem操作を
提供し、memfdも選択します。現行profileにblock／swap backendはありません。実shmemの有効化をswapや
memfdのuserspace syscall入口の成立とは扱いません。

## bootの責任範囲

memoryとschedulerの初期化は引き続きLinuxの`start_kernel()`が担当します。`vfs_caches_init()`から
`mnt_init()`へ進み、shmemとrootfsを初期化します。kswapd等のサービスもupstream initcallで起動します。
machine portは既存のRAM・arch・CPU・IRQ・clock接続だけを提供します。
試験が`shm_mnt`の代替mountを作ったり、`shmem_init()`・`mnt_init()`・`pagecache_init()`・SLUB初期化等を
呼んだりはしません。これらの入口もport／試験側から呼べないようlink監査で禁止します。

`memory_gate.c`は**すべての正規boot launcher実行で必須**です。既存のboot／service／task／SMP／time検証の
後、第1章の詳細Gateの前に実行します。PID 1、`SYSTEM_RUNNING`、RCU boot完了、2 CPU online、upstreamの
root mount、SLUB／RAM、upstream kswapd taskの存在を要求します。kswapdの存在だけで圧力下の回収を
検証済みとは扱いません。

## 必須の確認

- 各CPUのorder 0〜4 page確保で、ゼロ化・alignment・page／PFN／direct-map／物理addressの往復を確認。
- 各CPUで1〜65,537 byteの6種類のheap確保。専用の実SLUB cacheで32個を同時保持し、独立した内容を確認。
  すべてLinuxの解放経路へ戻す。
- static per-CPUと最大unitサイズのdynamic確保4個へ、**実際に両CPU上で**upstream SMP callからアクセス。
  CPU／`current`、領域分離、先頭・末尾を確認。最低一つはdynamic vmalloc領域にあることを要求し、初期の
  embedded chunkだけでは合格させない。reportはvmalloc-backedな確保数であり、異なるchunk object数ではない。
- 2 pageのvmapとdirect map間で、両CPUから双方向の内容共有と実Linux pageへの対応を確認。
  unmapとlazy alias排出後にfaultとなることを確認してからpageを解放。
- 5 pageの仮想領域の0・2・4ページだけを配置。穴を含む広いmap／cache flushが成立し、2ページ目の除去を挟む
  広いpre-unmap／TLB flushと全windowのTLB flushでも0・4ページ目は残ること。
  穴と除去済み領域はfaultとなること。
- 各CPUで`shmem_file_setup()`がboot済みの実shmem mountを使い、`shmem_mapping()`がtrueとなること。
  folioの確保でpage cacheへ登録され、swap-backed／uptodate／初期ゼロを確認。再lookupで同じ内容が得られ、
  このprivateな内部fileの同期的な最終putでmappingから除去されること。検証中はfolio参照を保持し、最後に
  `folio_put()`で解放する。解放済みinodeは参照しない。

通常の件数はpage 10、heap 12、SLUB object 64、per-CPU観測8、alias確認11、shmem 2です。
現行profileではdynamic per-CPUの4確保すべてがvmalloc-backedになります。予期しないLinux警告は失敗です。
失敗時は生存の可能性がある試験storageを保持し、processを即時終了します。

## このGateが発見した最下層mappingの修正

dynamic per-CPU確保により、旧portの「`flush_cache_vmap()`の範囲は全page配置済み」という仮定が破れました。
CPU unit間の未配置領域も通知範囲に含まれるため、Linux page tableに存在するpageだけをhostへmapします。
また、**pre-unmap** cache flush時点ではLinuxのPTEがまだ残り、範囲内に別の生存mappingもあり得るため、
この時点で全範囲のhost aliasを無効化する処理を除きました。

無効化はPTE除去後の`flush_tlb_kernel_range()`／`__flush_tlb_all()`へ接続します。除去済み領域だけを
アクセス不能にし、PTEが存在する領域は保持します。host mapと無効化を直列化し、広いflushが並行publisherの
新規host mappingを消さないようにします。host map／resetはguest IRQ／preempt禁止中にも呼べるleaf memory
操作であり、hosted Linuxへ再入したり、そのworkを待ったりしないことをcontractに明記しました。
host操作のsignatureやwire ABIは変更しません。

全windowの検証では、終端を含む`VMALLOC_END`と終端を含まないTLB範囲の境界不一致も検出しました。
追加した回帰試験は修正前にtrapし、port側で終端を明示的に変換するよう修正しました。

Linux vmallocは正当にTLB無効化を遅延・集約します。このGateと第1章のcleanup Gateでは、upstreamの
`vm_unmap_aliases()`を呼んでから解放済みaliasへのアクセスがfaultになることを要求します。
cleanupの投入遮断・IRQ／timer／work／RCU同期・解放後producer観測は維持します。検証をupstreamの契約へ
合わせる変更であり、排出後のアクセス不能という条件は弱めません。
page table・vmalloc／per-CPU allocator・回収policyはupstreamのままです。一般のuser-mm／VMA／fault／
保護変更の完成ではありません。

## ビルドと回帰試験

設定変更には新しいartifact directoryを使い、第1章の旧成果物は上書きしません。既存canonicalのbaseからの
手順は[英語版](memory-gate.md#build-and-regression)を参照してください。merge先directoryを先に作成し、
通常のnative Kbuild依存（objtool用libelf header／library等）を用意します。
CMakeの`KOBOX_LINUX_BOOT_RUNTIME_CORE`を新coreへ向けます。単独memory／task fixtureのpathは過去の回帰入力として
残せますが、その手動初期化は今回のGateや完了根拠に利用しません。

`kobox_linux_boot_test CORE.so --all`は**一度のLinux boot**で、boot／service／SMP／time、このmemory Gate、
時間付き待機、Tree RCU／SRCU、workqueue、統合cleanupを順に実行します。
CTestに`kobox2.linux_full_foundation_gate`を追加し、第1章の個別Gateも診断用に維持します。
現行canonicalはbuilt-in object 599個、initcall target 129個を保持します。追加objectはmemfdであり、
既存`mm/shmem.o`内でtiny版が完全版に切り替わります。closureによるcoreの選別はしません。

2026-09-06にこの実shmem profileで確認した結果：

- native canonical imageと厳密なhosted coreのビルドが成功。ELF検査はinitcall target 129個の保持と、
  host実装のimport拒否を確認。
- boot builderの8試験が成功。必須memory機能のtiny版／欠落／module化、上位subsystemや初期化の置換を拒否。
- mapping境界の両修正後に、構成済みCTest 42件すべて成功。
- 一度のbootで上記memory件数に加え、時間付き待機350件、RCU／SRCU 56件（callback 112・解放168）、
  workqueue 211件（callback 1,456・両CPUでrescuer進行）、cleanup 52件（同期保持probe 20・解放52）が成功。
  Linux警告なし。
- 同じ最終coreで新規processを追加で10回起動し、すべて`--all`が成功。
  疎なmappingと全windowのTLB回帰試験も含む。

これらが成功して完了するのはboot／構成の工程1だけです。詳細VFS操作、shmemのtruncate／pressure、
user mapping、GEM、外部clientとの共有には、第2章・第3章の各Gateが必要です。
