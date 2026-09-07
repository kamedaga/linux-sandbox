# upstream時間付き待機Gate

工程4はboot rootから構成した`linux-boot-runtime.so`の同じboot内で、工程3の
service／SMP／time Gateの後に`wait_gate.c`を実行します。サービスの独自初期化、
待機関数の置換、signal pending bitの捏造、host timer queueの追加は行いません。
待機・timer取消・signal・wakeは固定Linuxの実装を使い、builderは上位APIの置換を拒否します。

controllerとwaiterを別の論理CPUへ固定し、CPUを交換して全ケースを繰り返します。
waiterは実`kthread_create()`で生成し、結果確認後に`kthread_stop()`でjoinします。
非同期試験の失敗時はcallbackが参照し得る領域を解放せず、試験processを直ちに終了します。

## API範囲と戻り値

| API | 期限切れ | 早期wake成功 | signalによる中断 |
| --- | --- | --- | --- |
| `schedule_timeout`、uninterruptible／interruptible／killable／idle wrapper、`io_schedule_timeout` | 0 | 残りjiffies | stateが許す場合に残りjiffies |
| `wait_event_timeout`とinterruptible／killable版 | 条件falseなら0 | 残りjiffies、最小1 | stateが許す場合に`-ERESTARTSYS` |
| `wait_for_completion_timeout`とinterruptible／killable／I/O版 | 0 | 残りjiffies、最小1 | stateが許す場合に`-ERESTARTSYS` |
| `schedule_hrtimeout`の相対／絶対／interruptible、`schedule_hrtimeout_range_clock` | 0 | `-EINTR` | interruptible stateで`-EINTR` |
| `wait_event_hrtimeout`とinterruptible版 | `-ETIME` | 0 | interruptible版で`-ERESTARTSYS` |
| `msleep`、`usleep_range` | 戻り値なし | 明示wakeだけではsleepを終了しない | uninterruptible |
| `msleep_interruptible` | 0 | 明示wakeでは残り時間を再待機 | 残り**ミリ秒**。jiffiesではない |

相対・絶対の高精度待機はmonotonic timeを使用し、range試験は1 msのtimer slackを許します。
slackはtaskの実行開始時刻の厳密な保証ではありません。期限切れはjiffy APIならjiffies、
高精度APIなら`ktime_get()`で判定します。timer wheelにns精度を要求したり、taskの実行遅延に
根拠のない実時間の上限を置いたりはしません。

早期wakeの残量は、呼出し前後・taskのinactive確認・実wake投入のjiffiesを使い、
Linux内部の時刻取得を挟む範囲で検証します。jiffiesが進んでからwakeするため、常に最初の
timeoutを返す実装は失敗します。不要なwakeを繰り返した後に本当のeventを与えるケース
（`msleep_interruptible`ではsignal）でも元の時間を保持させます。ミリ秒の戻り値には
Linux自身の`jiffies_to_msecs()`変換を使います。

## 競合と境界条件

- 期限切れ、早期wake、timeout 0、呼出し前の条件成立／completion完了。
  jiffy APIはtimeout 0でも条件成立・完了済みなら1を返し、不成立の0と区別します。
- 確定的なwake-before-schedule：waiterがsleep stateを公開し、公開schedule APIの直前で
  停止します。別CPUの`wake_up_process()`成功後に再開させます。schedulerのpatchやstate操作の
  wrapperは追加しません。stateを再設定する便利wrapperは、このケースから除外します。
- 実際にinactiveになったtaskへのsignalと、呼出し前からpendingのsignal。
  試験kthreadは`allow_signal()`で受信を許可し、実`send_sig()`で送ります。
  通常signalはuninterruptible／killable待機を中断せず、SIGKILLはkillable待機を中断します。
  `flush_signals()`は結果記録後にのみ呼びます。
- waiterのCPUへ無関係なhard IRQを3回投入し、試験対象とは**別のcompletion**で配送を確認。
  各IRQ後の`wait_task_inactive()`のswitch countは変わらず、対象の待機は通常どおり
  期限切れになる必要があります。callbackは対象taskをwakeせず、条件も成立させません。
  独立したCPU固定のhard hrtimerから、実clockevent割込みを発生させます。
  remote `irq_work`はupstreamがidle task contextでcall-single queueを処理する場合も
  あるため、hardirq入口の証明には使えません。Gate開発中にこの合法経路と、sleep中taskの
  switch countが変化していないことを確認しました。誤起床やruntimeのIRQ不良とは区別します。
- 不要な明示task wakeではwaitqueue／completion／sleep loopが条件を再確認して再待機。
  複数回のwakeでもtimeoutを初期値へ戻してはいけません。
- 無期限の`MAX_SCHEDULE_TIMEOUT`と高精度待機のNULL deadlineは明示wakeで終了し、
  各upstream APIの戻り値を確認します。
- 待機終了後は`current`がrunnableで、固定CPU／per-CPU currentと一致し、I/O待機の印も
  残らないことを確認します。Linux警告があればGate失敗です。

## 実行

`build_boot_runtime.py --link`で固定coreをbuildし、[boot README](README-jp.md)に従って
CMakeの`KOBOX_LINUX_BOOT_RUNTIME_CORE`と`KOBOX_LINUX_TASK_BUILD_DIR`を設定します。

```sh
ctest --test-dir /home/kamer/os/.artifacts/kobox2-memory-gate \
  -R 'linux_timed_wait_gate' --output-on-failure
```

launcherの`kobox_linux_boot_test CORE.so --timed-wait`でも実行できます。失敗時には
API・ケース・CPU・ソース行・戻り値・経過nsを表示します。`wait_gate.h`はprocess内の試験結果
だけを定義するものであり、host contractやwire ABIの変更ではありません。

検証結果：22種類のAPI・両CPUで350ケース、無関係なhard IRQ 132回、Linux警告0件。
新規processで10回連続成功しました。設定済みCTestも全38件成功し、修正したboot／service
Gateも新規processで100回成功しています。

これはdriverのkernel内待機を検証するGateです。userspace syscallのrestart／copyout、
wall clock変更、suspend／freezer、RCU／workqueue全体、cleanup競合、GPU操作は別の検証対象です。
