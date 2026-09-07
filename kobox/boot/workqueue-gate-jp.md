# upstream workqueue Gate

工程6は同じ固定`linux-boot-runtime.so`の実boot／service／SMP／time Gate後に
`workqueue_gate.c`を実行します。worker生成、並行数管理、BH配送、timer、cancel、flush color、
rescuer生成、mayday配送は固定upstream Linuxのままです。host操作、上位置換、service起動の
shortcut、失敗stubは追加しません。builderのnegative testはworkqueue APIとworkerの
scheduler hookの置換を拒否します。

## matrix

per-CPU normal、per-CPU highpri、BH、BH-highpri、unbound、orderedの6種類について、
worker CPUを反転して各16ケース、計192ケースを実行します。controllerは反対CPUを使います。
threaded callbackはsleepできますが、保持中のBH callbackは期限付きbusy-waitのみです。
BHを保持すると`kthreadd`のCPUも止まり得るため、helper kthreadは保持前に生成します。

| 対象 | 必須観測 |
| --- | --- |
| 実行context | CPU／current一致、IRQ許可、threaded workerと`current_work()`、highpriの高いnice優先度、BHはhardirqではなくsoftirq |
| pending cancel | 重複queueは拒否、pendingの場合だけ非同期／同期cancelがtrue、cancel後は未実行かつ再利用可能 |
| running cancel | 非同期cancelは待たずfalse、同期cancelは戻り値がfalseでも実行中callbackを待つ |
| running＋pending | callbackが自己再投入済みの場合、同期cancelはpending分を除去し、実行中の分を待ってtrue |
| 自己再投入とのcancel競合 | 同期cancel中にcallbackが再投入を試みても、2回目の実行を残さない |
| flush | `flush_work()`と実行中の`flush_delayed_work()`はcallback終了を待ち、idle flushはfalse |
| queue flush snapshot | flushが実際にblockした後に追加したworkを保持していても、flushは完了する |
| 自己再投入 | 通常workとdelay 0のdelayed workを各32回実行し、同じitemを同時実行しない |
| drain | threaded queueは32回の連鎖再投入をdrain、BHは新規投入せず実行中callbackをdrain |
| delayed work | timer cancel／再利用、idle／pending時のmod戻り値、前後へのrearm、実期限切れ、pending timerのflush、timerからworkへの移行とcancel |

threaded同期cancelではLinux自身の一時的disable bitを読み取り、helperが実際にAPI内で
inactiveになることを要求します。未実行や最後のjoin待ちとは区別し、work dataは書き換えません。
BH同期cancelはupstreamが非hardirq atomic contextを許可しているため、呼出taskのpreemptを
禁止して実APIへ入ります。pinned hard hrtimerがそのtaskへ割り込み、Linuxがworkをdisableし、
反対CPUのcallbackがまだ実行中であることを確認してからcallbackを解放します。timer実行と
callback終了より前のAPI returnは禁止です。busy-wait cancel中の実IRQ進行も検証できます。

queue flushも、helperが実際にupstream flush内でblockしてから後続workを追加します。
全producerを停止してから待つだけではなく、flushのsnapshot規則を検証します。
callbackの入力・出力stampでqueue／flushのメモリ順序も確認します。各ケース終了時に
active／pending workとpending timerがなく、試験taskをjoinしてからstorageを再利用します。

## 配送・affinityの追加検証

- ordered FIFO：実CPU 0／1のcall-function contextから交互に16件投入し、producer CPUに
  かかわらずqueue全体の投入順序と単一active実行を維持することを要求します。
- unbound：両CPUを許可した状態で異なるworker二つが同時にactiveになります。
  古いworkを保持したままaffinityをCPU 0から1へ変更し、新しいworkはCPU 1で動き、
  古いpoolも正しくdrainすることを確認します。
- system queue 12ケース：`system_wq`、`system_highpri_wq`、BH二種、
  `system_unbound_wq`、`system_dfl_wq`へ両CPUから投入します。各16 itemが1回ずつ実行され、
  BH batchはper-CPU FIFOを保ちます。共有system queue全体はflushせず、試験itemだけを待ちます。
- 両CPUのBH優先度：task contextでBH／IRQ配送を禁止し、normalを先、highpriを後に投入して
  配送を許可します。highpriが先に動くことを要求し、既に実行中のBHへのpreemptは要求しません。

## 実メモリ不足下のrescuer

CPUごとに固有nice値と単一CPU affinityを持つ新規`WQ_UNBOUND | WQ_MEM_RECLAIM` queueを
作ります。upstream rescuerと全試験workは先に確保します。controllerは実Linux pageを
`alloc_page(GFP_NOWAIT | __GFP_NOWARN)`が失敗するまで保持します。fixtureのLinux RAM
256 MiBのうち約240 MiBを使い、host全体の空きメモリは消費し尽くしません。その後16 workを投入します。

通常workerは先行試験の非同期cleanupから戻る追加pageも保持し、rescue completionを待ちます。
sleepしないpage操作中はupstream `preempt_disable()`で同じCPUの実行権を保持し、rescuerが
作成中のlistを並行解放できないようにします。保持pageを解放してworkerを起床できるのは、
upstream `current_is_workqueue_rescuer()`がtrueになるcallbackだけです。
queueで選択したCPUでの実行も必須です。mayday／rescuerを直接呼ばず、pool counterを書き換えず、
allocator／worker生成関数を失敗する代替実装へ置換しません。

controllerはこの意図的かつ有界なメモリ不足区間でupstream `oom_lock`を保持します。
OOM killerと直列化し、reclaim試験がPID 1を殺したり、kill可能task不在でpanicしたりするのを
防ぐためです。allocatorの失敗・reclaim・worker retry規則は変更せず、controller自身は
lock保持中にdirect-reclaim allocationをしません。rescuer完了後にlockを解放し、16 callbackの
完了、保持page listがすべて空、通常GFP_KERNEL allocation成功、private queueのdestroyを
要求します。CPUを反転した両ケースが成功しなければなりません。

## 固定upstreamの制約・失敗時方針

次の不正な組合せをportの失敗とは扱いません。

- BH callback保持中は`TIMER_SOFTIRQ`も再入できません。BHのtimer→pending work cancel試験は
  delay 0の`mod_delayed_work_on()`で移行させます。BHの自然な期限切れは別途検証し、
  threaded queueでは別work保持中の期限切れも検証します。
- このpinの`is_chained_work()`は`current_wq_worker()`経由でtask-context workerだけを
  認識するため、`drain_workqueue()`中のBH再投入はupstreamに拒否されます。
  BHの自己再投入とdrainは別々に検証し、この組合せはサポート・認定しません。
  上位patchで隠しません。

想定外のLinux警告はすべて失敗です。非同期試験の失敗時はstorageを保持して即process終了します。
pressure待機の失敗時はOOM lockもprocess終了まで保持し、rescuerの下で並行freeしません。
失敗した試験からの復旧は行いません。

## 実行と対象範囲

`build_boot_runtime.py --link`でbuildし、[boot README](README-jp.md)に従ってCMakeを設定します。

```sh
ctest --test-dir /home/kamer/os/.artifacts/kobox2-memory-gate \
  -R 'linux_workqueue_gate' --output-on-failure
```

直接実行は`kobox_linux_boot_test CORE.so --workqueue`です。process内の診断reportには
queue、scenario、CPU、phase、ソース行、error bitを出力します。
成功1回あたり211ケース、callback 1,456回で、両CPUを対象としたqueueのrescuer実行が必須です。
page総数とrescuerが扱うcallback数は通常のbackground cleanupにより変動します。

検証結果：新規processで10回連続成功し、毎回211ケース・callback 1,456回・両CPU対象のrescueを
確認しました。設定済みCTest全40件も成功し、既存memory、task／SMP、boot／time、時間付き待機、
RCU Gateを含みます。Linux警告は0件。新規Gateソースのcheckpatchも警告・エラー0件です。
canonical boot object 598個とinitcall target 129個を維持し、upstreamソースのpathは変更しません。

対象は指定した2 CPUのpreemptible profileです。CPU hot-unplug、複数NUMA node、PREEMPT_RT、
freezer／suspend、全optional workqueue API、GPU操作は認定しません。
IRQ／timer／work／RCUを組み合わせたcleanup競合は別の[工程7 Gate](cleanup-gate-jp.md)で検証します。
