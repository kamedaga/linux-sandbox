# IRQ／timer／work／RCUの統合cleanup Gate

工程7の`cleanup_gate.c`は、固定`linux-boot-runtime.so`の実boot／service／SMP／time Gate後に
実行します。IRQ descriptor・threaded IRQ・timer・workqueue・Tree RCU・メモリ回収は、固定した
upstream Linuxそのものです。追加するのはdevice lifecycleの試験と検証条件であり、汎用cleanup
runtime、上位APIの置換、host操作、手動のサービス起動ではありません。

## 依存関係から決める解放順序

deviceは実`vzalloc`で確保し、RCU pointerで公開します。IRQ源と検証用objectは別に確保し、
deviceより長く保持します。通常動作の依存関係は次です。

```text
hard IRQ / IRQ thread / timer / hard hrtimer / BH / delayed work / RCU reader
                                 → normal work
normal work → timer / hard hrtimer / delayed work / 一度だけのBH・RCU callback
通常RCU callback → normal work

closing後に許可する有限の終了処理:
最後のnormal work → retirement RCU callback → final work → 解放
```

通常の投入とclosingへの遷移は**同じIRQ-safe lock**で保護します。状態確認とqueue／timer再設定／
通常RCU callback投入が一つのcritical sectionです。closing時は同じlock内でRCU公開を解除し、
lockを保持したままcallbackを待ちません。既に投入された処理は、対応する同期が完了するまで
objectを利用できます。

この依存関係に対するcleanup順序は以下です。全driver共通の固定手順ではありません。

1. 通常投入を閉じ、RCU pointerの公開を解除。
2. `disable_irq_nosync()`→`synchronize_irq()`。新規配送をmaskし、primary handlerとsleep可能な
   IRQ threadの両方を待つ。
3. `timer_shutdown_sync()`、`hrtimer_cancel()`、`disable_delayed_work_sync()`、BHの
   `disable_work_sync()`。timer shutdownでtimer↔workの再設定を永久に止める。hrtimerのcancelは
   再設定禁止ではないため、閉じた投入経路と組み合わせる。
4. `synchronize_rcu()`で旧pointerを取得した公開readerを待つ。callback完了待機ではない。
   atomic callbackを先に止め、意図的に保持したsoftirqへGPが依存する状況も避ける。
5. 最後のnormal workを許可・投入し、flushしてdisable。このworkが公開reader消滅後にretirement
   callbackを一度だけ投入する。この時点で通常のRCU callbackを増やすproducerはない。
6. `rcu_barrier()`で投入済みcallbackを完了させる。retirement callbackはfinal workを投入する。
7. **barrier後に**final workをflush／disable。private queueをdestroyし、`free_irq()`でaction／
   threadを外す。pending／active状態を確認し、実`vfree()`を一度だけ実行。
8. maskしたIRQ源は解放後も動かして観測。その後に源をcancel／同期し、IRQ descriptorを解放。
   検証用kthreadをjoinしてから、最後に検証用objectを解放。

4・6・7は別の寿命を保護します。reader GPでは投入済みcallbackは排出されず、callback barrierでは
callbackが投入したworkは排出されません。終了用の有限な投入経路は通常の投入を再開するものでは
ありません。実行中の旧workと最後の投入が重なっても、atomicな一度だけの処理でretirementの重複を
防ぎます。

## 完了条件

hard IRQ、IRQ thread、timer、hard hrtimer、normal work、BH work、delayed work、通常RCU reader、
retirement RCU callback、final workの10種類を処理中に保持し、CPUを反転して検証します。
さらに各CPUで停止時刻を変えたstressを16回実行します。合計**52ケース・同期観測20回・実解放52個**。
stressではreader以外の全callback種別が実際に実行されることを要求します。

- IRQ源は実host clockevent／IRQ経路から届くpinned hard hrtimerです。実hardirq contextで
  `generic_handle_irq()`を呼び、確保したdescriptor、`handle_level_irq()`、実効性のある
  mask／unmask／ack、`request_threaded_irq(IRQF_ONESHOT)`を通します。task contextからdevice
  handlerを直接呼ぶ試験ではありません。
- `synchronize_hardirq()`では保持中のIRQ threadが残ると報告され、`synchronize_irq()`はその完了まで
  本当に待つこと。
- sleepする同期APIでは、cleanup taskが指定phase内で実際にinactiveになること。単なる未実行や
  最後のjoin待ちを成功に数えないこと。
- busy-waitする同期APIでは、反対CPUのhard hrtimerが、指定phaseの**cleanup task自身**へ割り込み、
  対象callbackがactiveだと確認してから解放すること。
- reader保持中は公開readerのGPが完了しないこと。GP完了後でもretirement callback保持中はbarrierが
  完了せず、barrier完了後でもfinal work保持中は解放へ進まないこと。
- 保持した各通常producerがclosing後に再投入を試み、その種別自身の拒否counterが増えること。
  解放前の生存storageで直接の再設定／queueも試し、timer shutdownとwork disableの効果を確認すること。
- callback context／`current`／active数、retirement一回、final work完了一回、解放一回を確認。
  timer／workにactiveやpendingが残らないこと。解放後もIRQ試行とmask拒否は増えるが、device callback数は
  増えないこと。

deviceは実Linux vmalloc領域を使い、使用前に正しく読めることを確認します。実`vfree()`とupstreamの
`vm_unmap_aliases()`で遅延TLB無効化を排出した後は、同じ
aliasへのupstream `__get_kernel_nofault`のexception-table付きloadがfaultになることを要求します。
hostのvmap境界がmappingを実際に無効化するので、埋め込んだtimer／work／RCUへの遅れたアクセスを、
まだ読めるallocator領域上のmagic値だけで検出する方式ではありません。
native x86の`copy_from_kernel_nofault()`のaddress filterはhost user addressを拒否し、hosted kernelの
vmalloc領域も対象になるため、この既知のfixture mappingにはarchのfault-safe loadを使います。
nativeのaddress-filter APIを認定したり、成功stubへ置換したりはしません。

helper threadはatomic callbackを保持する前にすべて生成します。確定的な保持試験では、upstreamの
IRQ-thread初期affinity設定と起動完了を待ち、公開scheduler affinity APIでそのthreadをcontroller CPUへ
配置します。保持したBHと同じCPUへ置くと、先行するIRQ同期に必要なthreadまで停止してしまい、BH取消を
観測する試験として不成立になるためです。stressは通常のIRQ-thread affinityを維持します。
scheduler・IRQ thread・workqueue内部の変更はありません。

## 実行方法と範囲

`kobox_linux_boot_test CORE.so --cleanup`またはCTestの`kobox2.linux_cleanup_gate`を実行します。
process-local reportにscenario／CPU／phase／失敗行／error bits／完了ケース数／callback・拒否・同期観測・
解放数／Linux警告数を記録します。callback数・拒否数はタイミングで変動します。
予期しないLinux警告はすべて失敗です。非同期の失敗時はstorageを保持してlauncherが即時process終了し、
生存処理を残したまま解放して復旧しようとはしません。停止したupstream待機はprocess watchdogで制限します。

検証結果：新規processで10回連続、毎回52ケース・同期観測20回・実解放52個が成功し、Linux警告は0件です。
設定済みCTest全41件も成功し、既存memory、task／SMP、boot／time、時間付き待機、RCU／SRCU、workqueueの
Gateを含みます。新規C／headerのcheckpatchは警告・エラー0件で、builderの上位置換拒否テストも成功。
canonical boot object 598個・initcall target 129個を維持し、この工程でupstreamソースのpath、host操作、
PachaOS kernelは変更していません。

認定範囲はこの2 CPU・preemptible構成と依存関係、単一のcleanup所有者です。全driverのremove、重複する
並行remove、SRCUを含めた統合cleanup、CPU hot-unplug、PREEMPT_RT、module text unload、sandboxの
kill／revokeの証明ではありません。SRCU自身のreader／callback／barrierは工程5の別Gateで検証済みです。
実PCI／MSI-X transport、DMA fence、GPU removeは実deviceの依存関係を後続Gateへ追加する必要があります。
合成IRQ源は外部PCI backendではありません。PachaOS kernelやadapterは変更しません。

第2章の[正規boot memory Gate](memory-gate-jp.md)でmapping portの疎な範囲の扱いを修正しました。
無効化はpre-unmap cache hookではなく、PTE除去後のTLB境界に従います。そのためこのGateもfault観測前に
lazy aliasを明示的に排出します。callback同期と解放後IRQの検証は維持しています。
