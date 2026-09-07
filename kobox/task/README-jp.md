# Linux task/SMP gate

このgateは、固定したLinux schedulerを1 Linux task = 1 native pthreadへ接続します。
runqueue、sched class、task選択、wakeup、affinity、migration、exitはLinux実装のままです。
POSIX adapterは`switch_to()`が指定したtaskへlogical CPUの実行権を渡し、task選択は行いません。

architecture portが提供するのは、task生成と初回trampoline、context switch時のpark/unpark、
切替完了後の終了通知、TLSのtask/CPU識別、IRQ maskとIPI配送、monotonic scheduler clockです。
native pthread stackとhostのFS/GSはhostが管理し、taskのkernel開始関数は同じhost address spaceで
実行します。PID 1が使う`user_mode_thread()`のkernel開始経路も回帰試験に含めます。このtaskは
user mmも`PF_KTHREAD`も持ちません。通常のuser address spaceのcloneとuser命令への復帰は未対応です。

boot fixtureは実memory bootstrap、`init_task`、`sched_init()`、`workqueue_init_early()`、
`rcu_init()`、timer/softirq初期化、host clocksourceに接続したLinux timekeepingを使います。
CPU 1のidle taskはLinuxの`fork_idle()`で生成し、実`kthreadd`とCPU stopper threadを使います。
明示的に保持するearly initcallは`cpu_stop_init`で、symbol visibilityだけを変更し、本体は変更しません。
CPU hotplug callbackの登録も、memory単体fixtureの記録処理ではなくLinux実装を使います。

runtime gateは2 logical CPUで以下をすべて要求します。

- Linux `schedule()`／`try_to_wake_up()`によるlocal・remote切替
- TLS `current`、per-CPU current、CPU番号の一致
- sleep中taskのmigrationと、実行中taskの強制migration
- affinityの適用とremote reschedule IPI
- `preempt_disable()`中に対象CPUで切り替わらないこと
- local IRQ disable中のIPI保留と、enable後の配送
- Linux task exit、`kernel_wait()`によるreap、native pthread join

IRQ/time拡張は、両CPUでさらに以下を要求します。

- upstream `irq_enter()`／`irq_exit()`のhardirq accounting、`current`／CPUの一致、
  RCU context trackingの釣り合い
- upstream `default_idle_call()`でRCU idleへ入り、IRQ入口でwatchingへ戻り、
  IRQ出口でidleへ戻り、idle退出でwatchingへ戻ること
- 登録したhost clocksourceとCPU別one-shot clockeventによる、実Linuxのtick、
  timer wheel、high-resolution hrtimerの進行
- 実際のhres-active状態と500 us期限（4 ms tick未満）、早期callbackがないこと、
  正しいhardirq／softirq context
- IRQ保留と遅延配送、期限切れ後も保留中のeventの取消、後の期限への再設定、
  古い通知を含めて誤ったcallbackがないこと
- 各CPUで同時にrunnableなbusy kthreadを2つ動かし、yieldなしで非自発的な切替を繰り返すこと
- 各busy taskの`preempt_disable()`中もtickが進み、reschedule要求が保留され、
  enableするまでtaskが切り替わらないこと

`time_port.c`はclockevent deviceの実装であり、Linux timer queueの代替ではありません。
host contractに追加するのはCPU別のmonotonic絶対期限設定、取消、最終的な通知生成元停止だけです。
POSIX timer callbackはCPUへ通知するだけで、Linuxが選んだevent handlerはそのCPUのexecution domainで
実行します。古いhost通知では、停止済みまたは期限前のdeviceを発火させません。IRQ register frameは
hostのring-3実行ではなく、論理的なLinux kernel threadを表します。`sched_clock_noinstr()`を含む
scheduler clockも同じhost monotonic clockへ接続します。

fixtureはmemory bootstrap前にupstreamのstatic key初期化を行い、`jiffies_64`、jiffies lock、
sequence counterもLinuxの実定義を使います。試験はdevice実装から`time_gate.c`へ分離しています。
検証するのはLinuxの時間semanticsであり、hostのhard real-time latency保証ではありません。

joinはLinuxのcompletionで切替完了を待ちます。architectureのpost-switch hookが、Linuxによる
`on_cpu`解除とrunqueue unlockの後に通知します。終了taskが同じlogical CPUで処理を終える前に、
hostをjoinで塞ぐ構造にはしません。

`build_task_smp.py`はcanonical config/imageのhash、source object、gate symbolの由来、architecture
定義、fail-closedなphase importを記録します。未解決関数はfixtureを終了させ、通常dataは`PROT_NONE`
で保護します。未解決per-CPU dataはbuild時に拒否します。外部guardへper-CPU offsetを加えると保護範囲を
外れ得るためです。一部のarchitecture objectは実Linuxのper-CPU sectionだけを保持し、nativeの
context-switch／FPU codeは取り込みません。overlayの内容はKbuildのcommand fingerprintに含め、
新しいheaderが既存x86 headerを覆う場合も再compileします。

memory gateに使うcanonical Linux buildへ`task/config`をmergeし、`vmlinux`を再buildします。
2 CPUと静的な`CONFIG_PREEMPT=y`を必須とし、`PREEMPT_NONE`やdynamic preemptionは拒否します。
さらに`HIGH_RES_TIMERS`、`CONTEXT_TRACKING_IDLE`、`BUG`、`RCU_EQS_DEBUG`を必須とし、
runtime検査が黙って消えないようにします。このbuild directoryを`KOBOX_LINUX_TASK_BUILD_DIR`へ指定します。

```sh
cmake --build /path/to/cmake-build --target kobox_linux_task_smp_test
ctest --test-dir /path/to/cmake-build -R 'kobox2.linux_task_smp' --output-on-failure
```

これはtask/SMP/IRQ/time gateであり、driver runtime全体の完成gateではありません。
時間付き待機API全体と残量のmatrix、RCU grace period／callback回収、workqueue実行、CPU hotplug、
user address space、runtime全体のshutdownは未検証です。Linuxのboot-service threadはfixture process
終了まで残り、native joinをRCUによるtaskの遅延回収完了とは扱いません。productionのboot-rooted coreと
upstream service／initcall統合は、この独立したsubsystem fixtureとは別です。

fixtureと実boot launcherは、host task・CPU domain・IRQ・clockeventの接続を
`posix_machine.c`で共通化します。共有するのはmachine adapterであり、fixture側の
手動Linux初期化や完了判定を実bootへ持ち込むものではありません。
