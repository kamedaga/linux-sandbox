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
idle待機は所有権lock内で通知sequenceとpending countの両方を確認します。
signal配送より先に新しいsequenceを読んでも、wakeupを失いません。

notification callbackはsignal contextで実行します。allocation、mapping、thread生成、joinなど
async-signal-safeでないhost操作では、予約した二つの通知signalをmaskします。CPU所有権のlock操作も
maskします。parkしたthreadにはactive CPUがないため、遅着signalからLinuxへ再進入しません。
これは[POSIX sigaction](https://pubs.opengroup.org/onlinepubs/9799919799/functions/sigaction.html)の
signal masking規則に従うものです。Linuxやcallbackから任意のlibc呼び出しは許可しません。
通知元は`pthread_kill`まで所有権lockを保持し、宛先取得と配送の間のexit／joinを防ぎます。
producerとownerになり得る全threadを停止してからCPU objectをdestroyします。

`kobox2.posix_host_gate`は2 logical CPUで直列進入、CPU間並行実行、CPU-bound threadへのtick、IRQ
保留、wake-before-park、idle待機前に既にpendingだった通知、native taskのhandoff／exit／join、
二つのwindowから同じbackingへのaliasを証明します。
`kobox2.posix_host_surface_gate`は未宣言importとLinux固有の
`futex`、`eventfd`、`timerfd` shortcutを拒否します。

全state objectはinit/start前にzero初期化し、active中はcopyしません。
