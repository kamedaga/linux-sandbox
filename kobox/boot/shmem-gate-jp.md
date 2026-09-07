# Upstream shmem／folio／page cache Gate

第2章・工程3のGateです。実`start_kernel()`と必須のboot／service・SMP／time・memory Gateの
後に実行します。`--shmem`で単独選択し、`--all`でVFS・shmem・第1章の全Gateを同じboot内で
実行します。サービスの手動初期化・runtime置換・machine operation・host／wire ABIの追加は
ありません。固定coreは引き続きcanonical built-in 599 objectとinitcall 129対象を保持します。

## 所有権と会計

`shmem_gate.c`はupstreamの`fs_context_for_mount()`／`fc_mount()`で、16 page分の容量制限と
inode制限を持つ、namespaceへ未接続の通常tmpfs mountを作ります。`SB_KERNMOUNT`の内部tmpfsと
異なり、実際のblock・inode会計を通します。制限はmount optionとして渡し、Linuxの会計fieldを
書き換えません。`shmem_file_setup_with_mnt()`でunlink済みfileを作り、`dentry_open()`で同じ
inode・address spaceを持つ別のopen fileを作ります。file解放までmountの所有権を保持します。

制御taskのCPU 0／1と、upstreamの2種類の会計を組み合わせた4ケースです。

- `VM_NORESERVE`：backing pageの確保に応じてcommitを計上します。
- flags 0：backing確保より先にfile size分を予約します。サイズ変更は`vfs_truncate()`→
  `notify_change()`→shmem setattrへ通し、書込みは予約範囲内に限定します。事前確保やEOFを
  越える書込み競合は増分会計のtmpfsで検証し、固定サイズ・事前予約型へ誤適用しません。

各操作が静止した境界で、`i_size`・mappingの`nrpages`・shmemの`alloced`／`swapped`・
`i_blocks`・mountの`used_blocks`の正確なper-CPU合計・inodeの`free_ispace`・global commitを
照合します。inodeの課金量は実会計の差分で測定し、非公開の定数を複製しません。最後のclose後は
mountのdata／inode会計とcommitがfile生成前へ戻ることを要求します。変更が並行する途中の
counter群に、atomic snapshotであることは要求しません。

## 必須の動作

- 疎な範囲のreadはpageを確保せず0を返します。`SGP_READ`・`SGP_NOALLOC`・範囲外の
  `SGP_CACHE`を区別し、明示的に確保したcache folioは全体が0でuptodateであることを確認します。
- 別々のopenからpage境界を越えるpatternとEOFが一致し、read先の未使用範囲が変更されません。
- 4 pageにまたがる非整列hole punchで、内側の2 pageを除去し、端の指定byteだけを0にします。
  sizeと範囲外dataは保持します。EOFより後のpunchは拡張も確保もしません。
- hole内・非0の確保済みpage途中・page境界でtruncateし、対象pageと会計を回収します。
  再拡張では、部分pageの切り捨てた末尾も含め、以前のdataではなく0が見えます。
- 不正なtruncate長・fallocateのoffset／長さ／mode／overflow・folio取得条件を拒否し、
  会計が変化しないことを確認します。
- 2 CPUにbindした実kthreadが同じ未確保offsetを取得し、folio lock下で計256回加算します。
  同じPFNを取得し、共有backingに256回分が残ることを要求します。
- 同じshmem pageの異なる2本の実`vmap()` aliasとfile I/Oでdataが一致します。truncateで
  page cacheから外れ会計が戻っても、aliasと明示的なfolio参照がRAMを保持します。
- 片CPUでfolioをlockし、remote truncate／hole punchが実際にuninterruptible待機へ入ることを
  確認します。その間は完了・対象folioの除去を許さず、unlock後に完了と回収を要求します。
  timing依存のstressに加えた、確実に競合を起こす試験です。
- 各ケース64回、2 kthreadのread／writeと制御taskのtruncate／再拡張／punchを競合させます。
  readでは正当な短い結果やbyte単位の旧／新／0を許し、保証されないatomic snapshotを
  要求しません。全操作の完了と、最後の縮小／再拡張で空・0へ戻ることを確認します。
- 増分会計のtmpfsで、`KEEP_SIZE`によるEOF外の事前確保はsizeを保持し、拡張後は0を読めます。
  別fileで残容量を1 pageにして複数pageのfallocateを途中で`ENOSPC`にし、新規pageとcommitの
  巻き戻し・既存のdirty data／size／別fileの確保量の保持を確認します。別fileのcloseで容量が戻ります。

## 回収判定と範囲

hole punch・truncate・alias・最終closeではfolio参照を明示的に保持し、mapping除去とdata保持を
確認します。alias解除とupstreamの遅延alias処理後に最後の参照を離し、元のmappingと同じGFP classで
両CPUのbuddyから新規確保して、保存したPFNを実際に再取得することを要求します。解放済みobjectの
参照やallocator置換はしません。1ケース5 page、1回のGateで20 pageの実回収が必須です。
mapping参照が減っただけでは通りません。

workerは各操作の完了後に状態を再利用し、fileやworker storageを解放する前にstop／joinします。
試験専用file参照の解放にはupstreamの`__fput_sync()`を使います。遅延fputとinode／superblockの
寿命は別のVFS Gateで検証します。失敗時は生存中かもしれないstorageを保持し、launcherが即座に
processを終了します。

現行のorder-0・swapなしprofileが対象です。THP／swap有効時は、異なる意味の会計にこの判定を
流用せず、Gate入口で拒否します。swap・memcg・global memory pressure・user VMAのfault／unmap・
別process共有mapping・GEM・外部DRM clientの成立は主張しません。ここでのaliasはpinしたkernel
mappingであり、truncateが無効化するuser VMAではありません。

upstreamの所有元は[shmem](../../mm/shmem.c)・[VFS truncate／fallocate](../../fs/open.c)・
[page cache](../../mm/filemap.c)です。builder testで置換を拒否し、load auditでも実symbolと
Gate入口の保持を要求します。

## 検証

`kobox_linux_boot_test /absolute/path/linux-boot-runtime.so --shmem`と`--all`、またはCTestの
`kobox2.linux_shmem_pagecache_gate`と`kobox2.linux_full_foundation_gate`で実行します。
boot-core load auditが前提ですが、link／load成功だけをruntimeの証明にはしません。
Linuxの警告が1件でもあれば失敗です。

検証artifactはworkspaceの`.artifacts/kobox2-shmem-pagecache-runtime/linux-boot-runtime.so`、
SHA-256は`d2a1d0a56892bcf27a71c95e9c81b6a289551c1494711f7ccfedbc51f02ef746`です。
変更していないcanonical configのhashは
`5e8618e196f9096b3a149a22ac6869b8f2e2b671a2508ec0ca7a75ccf0a09d60`です。
厳格build／load audit・builder test 8件・checkpatch・CTest全44件が成功しました。
shmem単独50回の全てで、4ケース・I/O 64点・会計98点・共有16点・異常系32点・加算1,024回・
競合256回・lock待機8件・実page再回収20件・確保失敗の巻き戻し2件・警告0件を確認しました。
追加の`--all` 5回も、各boot内でVFS→shmem→時間付き待機→RCU→workqueue→cleanupが全て成功し、
警告0件でした。検証logはartifact directoryの`repeat-1.log`〜`repeat-50.log`と
`all-1.log`〜`all-5.log`、全suiteのlogは`.artifacts/kobox2-shmem-pagecache-ctest.log`です。
