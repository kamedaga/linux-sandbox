# upstream Tree RCU／SRCU Gate

工程5は同じboot-rooted `linux-boot-runtime.so`内でboot／service／SMP／time Gateの後に
`rcu_gate.c`を実行します。Tree RCU、Tree SRCU、GP worker、callback、barrier、
task／context tracking hookは固定upstream実装です。Gate自身でserviceを起動せず、
quiescent stateも与えません。builderは上位RCU／SRCU APIとscheduler hookの置換を拒否します。

## 検証matrixと観測

対象profileはonline CPU 2個、preemptible Tree RCU、Tree SRCU、periodic tick、
high-resolution timer、idle context trackingです。boot終了済みで、RCUをnormal-only／
expedited-onlyに強制していないことを確認します。各行をnormalと明示的expeditedの両方で、
readerの初期CPUを反転して実行します。

| read-sideケース | Tree RCU | dynamic SRCU | static SRCU |
| --- | --- | --- | --- |
| busy reader | 検証 | 検証 | 検証 |
| nested reader、外側を保持したまま内側だけunlock | 検証 | 検証 | 検証 |
| 高優先度taskがreaderをpreempt | 検証 | 検証 | 検証 |
| readerをmigration、元CPUがRCU idleへ入る | 検証 | 検証 | 検証 |
| readerがsleep、そのCPUがRCU idleへ入る | 不正なので除外 | 検証 | 検証 |

計56ケースです。通常RCUのreaderは自発的にsleepしません。SRCUは
`init_srcu_struct()`／`cleanup_srcu_struct()`と`DEFINE_STATIC_SRCU()`の双方を使います。
試験taskの生成・affinity・schedule・stop／joinはすべてupstream kthread経路です。

readerは`rcu_dereference()`または`srcu_dereference()`で3 objectを取得し、payloadを
繰り返し確認します。read-side区間に入ってからcontrollerがpointerの公開を取り下げ、
両CPUからcallbackを登録し、同期writerとbarrierを起動します。reader保持中はGP完了、
callback、barrierのreturn、メモリ回収を認めません。nestedケースは内側unlock後も確認します。

preemptケースではupstream FIFO taskを使い、readerがoff-CPUで非自発的switch countが増えた
ことを観測します。Tree RCUではLinux自身のblocked-reader markerと継続中のnestingも確認します。
migrationは`set_cpus_allowed_ptr()`を使い、reader自身が移動先CPUを観測します。
idleはupstream context trackingのidle状態とRCU-watching bitのclearを読み取ります。
readerの`current`はper-CPU currentと一致し、RCUがwatchingでなければなりません。
試験側でtask状態・CPU配置・RCU状態を捏造しません。

writerは`synchronize_rcu()`／`synchronize_srcu()`または明示的expedited APIを呼びます。
GP進行は公開polling cookieで確認し、Tree RCUにはfull cookieを使います。
SRCUでは先行するnormal callbackにより正規のidle自動expediteを防ぎ、upstreamの
expedited要求sequenceが変化したかも観測します。そのstate machineは書き換えません。

## callback・barrier・実メモリ回収

reader unlock後、同期writerが1 objectを、実RCU／SRCU callback二つが残りを
`kmem_cache_free()`で解放します。unlock前のreaderの書込みが回収側に見えることも確認します。
早すぎる回収を検出した場合は失敗を記録し、readerがアクセスし得るobjectを解放しません。

CPU 0のcallback一つをsleepせず短時間保持します。preemptは禁止しますがIRQは許可し、
pinned hard hrtimerが期限後に実際に割り込むことを要求します。CPU 1ではwitnessがbarrier taskを
明示的にwakeし、callbackがまだ実行中にそのtaskが実際に実行され、再びblockすることを確認します。
これにより、誤ったbarrierが単にCPU時間を得られず成功扱いになることを防ぎます。
callbackはhard IRQとbarrier再確認の双方が済んでから終了します。`rcu_barrier()`または
`srcu_barrier()`は両callbackの完了前にreturnしてはいけません。

ケースごとに非mergeの専用SLUB cacheを使います。試験taskをjoin後、3 objectすべての解放、
pollによるGP完了、dynamic SRCUのcleanup、空cacheのshrink／destroyを要求し、Linux警告も
失敗とします。非同期ケースの失敗時は状態を保持して即process終了とし、未完了callbackの下で
そのstorageを解放しません。

## 最下層IRQ境界の修正

このGateはPOSIX dispatcherの実際の欠陥を検出しました。callback全体のsignal maskと
再入禁止により、LinuxがIRQを許可してもRCU softirq callbackへhard timerが割り込めませんでした。
修正箇所はRCU内部ではなくmachineのIRQ入口／出口です。callbackはIRQ禁止状態で出入りし、
hostが割込み前の状態を復元します。LinuxのIRQ enableに従って保留済み・後着IRQの入れ子配送を
許可し、host管理操作だけを保護します。task migrationに備え、return時にCPU所有権を再確認します。
[host契約](../host/posix/README-jp.md)を参照してください。

独立した2 CPUのhost回帰試験は旧dispatcherで失敗し、enable時の同期配送と、後から届く非同期の
入れ子配送を検証します。callback signature、wire layout、上位Linux実装は追加していません。

## 実行と対象範囲

`build_boot_runtime.py --link`でbuildし、[boot README](README-jp.md)に従ってCMakeを設定します。

```sh
ctest --test-dir /home/kamer/os/.artifacts/kobox2-memory-gate \
  -R 'linux_rcu_gate' --output-on-failure
```

launcherの`kobox_linux_boot_test CORE.so --rcu`でも実行できます。process内の診断reportには
flavor、scenario、CPU、phase、ソース行、error bitを出力します。
成功1回あたり56ケース、normal／expedited同期各28回、callback 112回、実行中callbackを伴う
barrier probe 56回、実object解放168個、強制preempt観測12回、migration 12回、idle観測20回です。

IRQ修正後の実行結果：RCU Gateを新規processで10回連続成功。設定済みCTest全39件、
boot／service 100回、時間付き待機10回、独立POSIX host 100回も成功し、Linux警告は0件です。
host GateのASan／UBSanも成功。新規RCU Gateソースのcheckpatchは警告・エラーとも0件です。

対象はこの2 CPU profileであり、rcutortureの代替や全RCU構成の保証ではありません。
CPU hot-unplug、NOCB、no-HZ-full、PREEMPT_RT、Tasks RCU、module unload、統合cleanup競合は
対象外です。workqueueのcancel／flush／rescuerは別の[工程6 Gate](workqueue-gate-jp.md)、IRQ／timer／work／RCUを
組み合わせたcleanup競合は別の[工程7 Gate](cleanup-gate-jp.md)で検証します。GPU動作の認定ではありません。
