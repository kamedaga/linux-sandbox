# POSIX host境界

このdirectoryは最下層Kobox host境界のLinux PoC実装です。提供するのは次だけです。

- pthreadの生成・join
- monotonic clockを使うcounting permit
- `CLOCK_MONOTONIC`時刻
- 再arm可能なone-shot timer
- anonymous mappingと、共有backingを固定aliasするwindow
- POSIX realtime signalによる非同期tick/IRQ通知

Linux task、scheduler class、waitqueue、mutex、workqueue、timer、IRQ subsystemは実装しません。

各logical CPUはexecution lockを一つ持ちます。threadはLinux code実行前にそのdomainへenterし、park前に
leaveします。同じlogical CPUへの進入は直列化され、異なるCPU domainは独立します。tickとIRQは
`pthread_kill`で現在のownerへ送るため、CPU-bound ownerにも配送できます。owner不在時とlocal IRQ
disable中も通知countを保持し、最後のIRQ enableがreturnする前にpending countを配送します。

notification callbackはsignal contextで実行されるため、async-signal-safeな操作だけを使用します。
producerとownerになり得る全threadを停止してからCPU objectをdestroyします。

`kobox2.posix_host_gate`は2 logical CPUで直列進入、CPU間並行実行、CPU-bound threadへのtick、IRQ
保留、wake-before-park、二つのwindowから同じbackingへのaliasを証明します。
`kobox2.posix_host_surface_gate`は未宣言importとLinux固有の
`futex`、`eventfd`、`timerfd` shortcutを拒否します。

全state objectはinit/start前にzero初期化し、active中はcopyしません。
