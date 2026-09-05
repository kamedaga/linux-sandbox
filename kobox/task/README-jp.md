# Linux task/SMP gate

このgateは、固定したLinux schedulerを1 Linux task = 1 native pthreadへ接続します。
runqueue、sched class、task選択、wakeup、affinity、migration、exitはLinux実装のままです。
POSIX adapterは`switch_to()`が指定したtaskへlogical CPUの実行権を渡し、task選択は行いません。

architecture portが提供するのは、task生成と初回trampoline、context switch時のpark/unpark、
切替完了後の終了通知、TLSのtask/CPU識別、IRQ maskとIPI配送、monotonic scheduler clockです。
native pthread stackとhostのFS/GSはhostが管理します。このgateのtaskは同じhost address spaceを
共有するkernel threadだけであり、user task生成は拒否します。

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
このbuild directoryを`KOBOX_LINUX_TASK_BUILD_DIR`へ指定して実行します。

```sh
cmake --build /path/to/cmake-build --target kobox_linux_task_smp_test
ctest --test-dir /path/to/cmake-build -R 'kobox2.linux_task_smp' --output-on-failure
```

これはtask/SMP gateであり、driver runtime全体の完成gateではありません。周期scheduler tick、
timeout待機、RCU grace period／callbackの進行、workqueue実行、CPU hotplug、user address space、
runtime全体のshutdownは検証対象に含めません。Linuxのboot-service threadはfixture process終了まで
parkしたままで、native joinをRCUによるtaskの遅延回収完了とは扱いません。
