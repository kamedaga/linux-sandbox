# POSIX host境界

このdirectoryは最下層Kobox host境界のLinux PoC実装です。提供するのは次だけです。

- pthreadの生成・join
- monotonic clockを使うcounting permit
- `CLOCK_MONOTONIC`とboot時のwall clock
- 再arm可能なone-shot timer
- anonymous mappingと、共有backingを固定aliasするwindow
- POSIX realtime signalによる非同期tick/IRQ通知

Linux task、scheduler class、waitqueue、mutex、workqueue、timer、IRQ subsystemは実装しません。

各logical CPUはexecution tokenを一つ持ちます。threadはLinux code実行前にそのdomainへenterします。
switchは指定された次taskへtokenを渡し、前のpthreadをparkします。hostはtaskを選択しません。
同じlogical CPUへの進入は直列化され、異なるCPU domainは独立します。tickとIRQは
`pthread_kill`で現在のownerへ送るため、CPU-bound ownerにも配送できます。owner不在時とlocal IRQ
disable中も通知countを保持し、最後のIRQ enableがreturnする前にpending countを配送します。
callbackはlogical IRQ禁止状態で入り、同じ禁止状態でreturnします。hostはmachineのIRQ returnと
同様に割込み前のIRQ状態を復元します。Linuxがsoftirq／callback実行中にIRQを許可すれば、
既にpendingのIRQも後から届くIRQも直ちに入れ子で配送できます。native signalのmaskはhostの
管理操作中に限定し、Linux callback全体には掛けません。Linuxによるtask migrationに備え、
return時にCPU所有権を再確認します。これは未freezeの通知契約の修正で、callbackのsignatureや
wire layoutは変更しません。callback全体の再入禁止はLinuxのIRQ enable規約を壊し、
RCU callbackへのtimer割込みも止めてしまうためです。
idle待機は所有権lock内で通知sequenceとpending countの両方を確認します。
signal配送より先に新しいsequenceを読んでも、wakeupを失いません。

notification callbackはsignal contextで実行します。allocation、mapping、thread生成、joinなど
async-signal-safeでないhost操作では、予約した二つの通知signalをmaskします。CPU所有権のlock操作も
maskします。parkしたthreadにはactive CPUがないため、遅着signalからLinuxへ再進入しません。
これは[POSIX sigaction](https://pubs.opengroup.org/onlinepubs/9799919799/functions/sigaction.html)の
signal masking規則に従うものです。Linuxやcallbackから任意のlibc呼び出しは許可しません。
通知元は`pthread_kill`まで所有権lockを保持し、宛先取得と配送の間のexit／joinを防ぎます。
producerとownerになり得る全threadを停止してからCPU objectをdestroyします。

one-shot timerはtimed wait期限切れ後に現在の設定期限を再確認します。timer threadがlockを
再取得する前にcancelやrearmが先行する場合があるためです。cancelは既に実行中のcallbackの終了を
待ちません。destroyはproducerとその全callbackの終了を待ちます。Linux clockevent deviceは
対象CPU domainで現在のarmed状態と期限を確認し、古い通知による誤発火を防ぎます。

`kobox2.posix_host_gate`は2 logical CPUで直列進入、CPU間並行実行、CPU-bound threadへのtick、IRQ
保留、wake-before-park、idle待機前に既にpendingだった通知、native taskのhandoff／exit／join、
二つのwindowから同じbackingへのaliasを証明します。
両CPUの入れ子IRQ入口／出口も検証します。保留IRQはenableのreturn前に、後から発生するIRQは
親callbackの終了前に配送されなければなりません。
timer試験はcancel後の再利用と、期限を前後両方向へ変更する場合も検証します。
`kobox2.posix_host_surface_gate`は未宣言importとLinux固有の
`futex`、`eventfd`、`timerfd` shortcutを拒否します。

全state objectはinit/start前にzero初期化し、active中はcopyしません。

別libraryの`kobox_posix_vm`／`vm*.c` address-space transportは**Linux固有**であり、上記の制限付き
POSIX archiveには含めません。ptraceとeventfd／signalfdで実際の外部processを制御します。
[MM portの契約](../../mm/README-jp.md)を参照してください。
VM完了通知も同じIRQ禁止／CPU domain規則に従い、両CPUでの配送保留をhost testで検証します。
