# host上のuser address space

`port.c`が置き換えるのはx86のmm切替とTLB無効化だけです。固定boot-rooted coreにはupstreamの
mm生成・破棄、VMA操作、page table、shmem、rmap、fault処理を残します。builderは既存のlocal
symbol `do_user_addr_fault`の可視性だけを変更し、処理自体は変更しません。
MM／fault／VMA実装の自作置換は、所有権のnegative testで拒否します。

外部host process一つに実Linux `mm`を一つ対応させ、kernel service taskがupstreamの
`kthread_use_mm`／`kthread_unuse_mm`で利用します。host processに入るのはnative access clientで、
Linux coreの複製ではありません。管理対象mapping領域はclientのnative bootstrap code・stack・
control pageと分離します。Linux test clientは`0x4000000000`から16 pageを予約し、領域外操作は
拒否します。勝手に別のアドレスへ割り当てません。

## mappingと寿命の契約

- `vm_mmap`、native `mprotect`、`vm_munmap`、`do_user_addr_fault`はLinuxが実行します。
  portがpage選択やVMA規則を作り直すことはありません。
- 実faultの解決後、`mmap_lock`とpage-table lockの下で実PTEを読み直します。
  PROT_NONE／nonpresent PTEからはmappingを公開しません。hostのstoreではLinux PTEのdirty bitが
  更新されないため、host write許可にはdirtyかつwritableなPTEを要求します。
  large leafとRAM外PFNは`-EOPNOTSUPP`で拒否し、map成功とは扱いません。
- PTE公開とhost resetはbindingごとのtranslation lockで直列化します。`flush_tlb_mm_range`は
  host無効化を完了してからLinuxの遅延page解放経路へ戻ります。archのbatched flushは全bindingを
  保守的にresetし、別のrmap subsystemを実装しません。
  公開時はtranslation lock保持中の全区間でLinux IRQを禁止します。VM IRQ配送が取得するregistry
  lockを、並行flushがtranslation lock待ちの間に保持するためです。native host呼出し中だけの
  IRQ禁止では足りません。
- binding破棄前に、callerが利用中の全service taskをjoinします。registryは並行IRQ／flushから
  bindingを保護します。実clientの終了・回収後にregistryから外し、upstream `mmput`へ渡します。
  host無効化の失敗をflush成功へ変換しません。

## Linux host transport

`posix.c`はprocess-local machine interfaceを、別libraryのLinux固有実装
`host/posix/vm*.c`へ変換します。ptrace、eventfd／signalfd、bootstrap用futexを使いますが、
制限付きportable library `kobox_posix_host`には含めません。hostの正errnoはこのadapterでLinuxの
負errnoへ変換します。controller、native descriptor、core pointerを共有wire protocolへ追加しません。

全ptrace操作を専用native threadが所有します。同期map／resetはpage-table lock保持中やIRQ禁止中も
Linux taskの進行を必要としません。client実行は非同期です。完了通知は既存の論理CPU IRQ入口を通り、
upstream waitqueueを起こします。task／CPUの選択は引き続きLinux schedulerです。
VM通知は未freezeのprocess-local interfaceを使用し、wire ABIには含めません。

managerは他のthread生成前に作り、SIGCHLD禁止状態を継承させます。全callerのjoinとremoteのclose後に、
生成元threadで破棄します。close済みhandleはmanager破棄までtombstoneとして残し、sequenceで古い
実行再開を拒否します。通常のclient syscallは実行前に捕捉し、許可されたnative signal returnだけを
通すため、継承したRAM descriptorでmapping権限を迂回できません。

PTRACE_INTERRUPTは無限user loopも停止します。その直前に発生済みだったfaultが、注入したsyscallの
実行前に届く場合があります。この停止を識別し、mapping操作後に保存した元のアクセス命令を再試行します。
syscall trampolineのfaultとして誤配送しません。fault取得中・signal return中の割込みを反復試験します。
process終了をreset完了とみなすのは実際の回収後だけです。ESRCHや応答不在だけでは成功にしません。
停止中clientの死亡も、terminal completionとして一度だけ報告します。

## 結合試験

`boot/vm_gate.c`は結合試験を提供します。各試験はupstream bootから始め、
実2 processと異なるmm／service taskを2 logical CPUで動かします。実read／write fault、共有write、
mapping別の読み取り専用保護、部分unmap、独立したpeer mapping、LinuxによるSIGSEGV判断、
truncate後のSIGBUSを確認します。拒否されたclientはcleanupまで停止したままです。観測するのはLinuxが
queueへ入れたsignal判断であり、Linux userspace signal handlerの実行は試験対象ではありません。

MM GateはCTestの`linux-mm-vma` label全件と、host transport／service試験、基盤の回帰試験を
まとめて判定します。単独probeの通過だけではGate全体の合格とはしません。

無効化／再利用試験では、truncate中の2つ目の実host reset完了応答を保留します。別CPUからpageを
確保しますが、試験用の旧folio参照は保持しません。応答後にはbuddyから同じPFNを再取得して上書きし、
両processの旧aliasへの実アクセスがLinuxによって拒否されることを確認します。

公開競合試験は、archの全体flush中のVM IRQ、公開PTEのlock保持中の実`vfs_truncate`、公開完了前
または遅らせたfault再開前のhost close／回収を対象にします。truncateするcaseでは、clientの再開を
許可する前に旧PFNを再取得して上書きします。終了済みbindingは古いmap・resumeの双方を拒否します。

寿命試験は、通常の破棄、予期しないprocess終了によるLinux待機taskの起床、途中まで設定した状態での
shmemへの`MAP_SYNC`の実拒否を対象にします。全試験でmmを借用するtaskをjoinし、nativeのlazy-mm参照、
task work、遅延fput、RCUを排出します。観測用参照を外してから、旧mm／task／file／inodeのcache slotと
残っていたfile PFNをnative allocatorから再取得します。raw slab確保は領域再利用の検査であり、
Linux objectの代用品ではありません。

新しいfull coreとnative client executableを使って実行します。

```text
kobox_linux_boot_test CORE --vm-probe CLIENT
kobox_linux_boot_test CORE --vm-probe-ro CLIENT
kobox_linux_boot_test CORE --vm-probe-reuse CLIENT
kobox_linux_boot_test CORE --vm-probe-irq CLIENT
kobox_linux_boot_test CORE --vm-probe-truncate CLIENT
kobox_linux_boot_test CORE --vm-probe-exit-publish CLIENT
kobox_linux_boot_test CORE --vm-probe-exit CLIENT
kobox_linux_boot_test CORE --vm-probe-lifetime CLIENT
kobox_linux_boot_test CORE --vm-probe-death CLIENT
kobox_linux_boot_test CORE --vm-probe-rollback CLIENT
```
