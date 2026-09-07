# upstream boot runtime統合

既存のmachine／task／time portを、boot-rooted Linux coreへ統合する工程です。
subsystemを手動初期化するfixtureの追加ではありません。工程3のboot／service Gateと、
同じ環境での2 CPU IRQ／time Gateが通りました。driver runtime全体の完成ではありません。

現行boot profileは実shmem／tmpfsを必須にします。canonicalのbaseと`task/config`の後に
`boot/config`をmergeしてcanonical imageを再ビルドし、変更したidentityには新しいhosted／output
directoryを使います。必須の[正規boot memory Gate](memory-gate-jp.md)は、fixture初期化なしで
page／SLUB／per-CPU／vmapと実shmemを検証します。`--all`ではこれと第1章の全Gateを一度のbootで実行し、
CTestに`kobox2.linux_full_foundation_gate`を追加します。

第2章の[VFS寿命Gate](vfs-gate-jp.md)は`--vfs`または`--all`で実行し、実tmpfs mount・名前付きfileのI/O・
Linux FDのclose／unlink・file／inode／folioの独立した参照・task-workと遅延fput・inodeのRCU解放・
superblock破棄を検証します。回収後の実allocatorによる再利用を必須にし、runtime portや上位実装の置換は追加しません。

[shmem／page cache Gate](shmem-gate-jp.md)は`--shmem`または`--all`で実行し、2 CPUの共有folio操作・
疎な範囲／部分pageのゼロ化・サイズ変更・truncate／hole punch競合・事前確保の巻き戻し・
block／inode／commitの正確な会計・pinしたpageの実回収を、既存のboot coreとportで検証します。

`build_boot_runtime.py`はcanonical `vmlinux.a`の全built-in objectを、入力順、init text、
initcall tableを保持してcompileします。`linux-boot-inputs.json`が認定するのはcompile入力だけで、
coreのlinkや実行ではありません。driver moduleがcoreのsource objectを選ぶことはありません。

compileにはupstreamの全treeターゲット`vmlinux_o`を使います。古いlibraryを再利用し得る
単一ファイル用の`.a`ターゲットは使いません。boot版ではupstreamのstatic-call型と実装を保持し、
独立provider向けの間接呼出しheader選択は使用しません。

boot専用の`asm/sync_core.h` portは、kernel-modelのアドレスを埋め込むnative IRET-to-selfの
代わりに、CPL 3で実行可能なserializing命令`CPUID`を使います。これはlocalな命令列のbarrierで、
CPU間の同期はLinuxに残します。並行text patchの同期経路は別途統合する対象です。

固定したnative assembly入力には、`patches/`の小さい位置独立化patchを適用します。
変更はアドレスの作り方であり、サービスの処理やinitcall構成ではありません。生成コピーだけに
適用し、記録されたKbuild compiler・objtoolコマンドを再実行して、全global定義の保持を検査します。
source・patch・生成source・objectのhashは`machine-bindings.json`に記録します。
このnativeコードを保持することは、特権入口がhost process上で実行可能になったという意味ではありません。

Cの境界patchはLinuxの例外decoder・報告処理を保持し、nativeの高位アドレス判定を
core textの所有範囲判定へ変更します。register dumpではhost所有のCR／MSR／debug registerを
読みません。`init/main.c`への変更は、通常のboot終端とsysctl引数処理の**後**、userspace initの
実行前にPID 1をhosted入口へ渡す箇所だけです。initcall、rootfs設定、サービス起動を置き換えず、
exec成功を偽装しません。

`runtime.lds.S`はinitcall、per-CPU data、scheduler classの順序、初期task stackにLinuxの
linker macroを使用します。独立ELF配置試験では実init領域、試験用initcall entryの再配置、
RAM image境界、`dlopen()`後のnative TLSを確認します。fixtureはruntimeへリンクせず、
この試験でLinux bootやサービス進行を認定することもありません。

`--link`では厳格な`linux-boot-runtime.so`もbuildします。レビュー済みのsymbol／所有元
allowlistでmachine置換を制限し、想定外の上位APIやdataの置換を拒否します。未解決symbol用の
boundary libraryは生成しません。現在は全coreのlink・loadが通り、外部importはnative TLS取得だけです。
`load_test.py`はPT_LOADのページ単位の非重複、RWX・動的text relocationがないこと、全initcall表
（現在129対象）の保持と実行可能な参照先を検査します。initcall自体は呼びません。ELF program headerを
明示して、隣のmetadata領域が再配置先のページをread-onlyへ再mapする問題を防ぎます。

`image.c`はCPU進入前に、再配置後DSOの実PT_LOADページを予約済みLinux RAM backingへaliasします。
権限・ELF・TLSを保持し、数値上だけ対応する物理領域を予約する方式にはしません。image試験では
変更前の容量不足拒否、双方向のdata共有、code byte一致、再map後の実行とTLSを確認します。
実行可能mapに対応しないbackingは、DSOページを置き換える前に拒否します。
boot入口でもdataとdirect-map側が実際に同じstorageを共有していることを検査します。

boot専用runtime-constant headerはupstreamの書換え箇所・値・table走査を保持し、命令への書込みだけを
RAMの書込可能aliasとlocal命令列barrierへ接続します。実行側はRXのままです。並行text patchの
同期方式ではありません。boot終端ではtext・rodata・ro-after-initをimage側とdirect RAM側の
両方で保護します。init解放はimage aliasを先に無効化し、upstreamの`free_reserved_area()`で
実Linux pageをpoison・解放・計上します。direct RAMはbuddyの再利用用に残します。
image試験では実際の書込み／アクセス違反とRWX拒否を確認します。

`boot_test.c`は独立task fixtureと共通のPOSIX machine接続（`task/posix_machine.c`）から、実際の
`start_kernel()`へ進入します。実`mm_core_init()`、PID 1、CPUHP、initcall、boot終端の解放・保護を
通って`SYSTEM_RUNNING`へ到達し、PID 1で`service_gate.c`を実行します。
coreはphase-boundary DSOや成功stubを使わず厳格にlinkします。

hostの同期faultはLinuxのexception tableとWARN／BUG経路へ接続します。INT3はupstreamの
text-poke handlerとdie-notifier chainを使い、alternativesのself-testも実行します。
CPU初期化で公開するのは共通のx86-64 FP／SSE命令であり、hostのAPIC、vendor firmware、XCR0の
所有権は公開しません。FPU状態の初期化、task割当サイズ決定、最小FP状態のcloneはLinuxに残し、
実行register contextはnative pthreadが保持します。upstreamのreal-mode platform hookから、
代わりにpthread AP入口を登録します。legacy PIC／APICやnative PCI config portへのアクセスはなく、
device resourceとMSI domainは後続のhost bridge統合から提供する必要があります。

upstreamの`start_kernel()`、PID 1と`kthreadd`生成、early／通常initcall、CPUHP state machineを
保持します。特に`workqueue_init()`はpre-SMP initcallに先行し、pre-SMP initcallはAPがonlineに
なる前に実行します。hostがAPのpthreadを開始するのはupstream CPU起動経路から要求された時だけです。

memory portはhost配置の登録と`setup_arch()`処理を分離します。`setup_per_cpu_areas()`と
`mm_core_init()`を呼ぶ責任はupstream bootに残します。task-portはPID 1に必要な、kernel開始関数を
持ちuser mmのない`user_mode_thread()`を受け入れます。通常のuserspace cloneやuser命令への復帰まで
実装したという意味ではありません。

boot版からは、独立fixtureの手動memory／scheduler初期化入口とAP idle loopを除外します。
`kobox_linux_task_verify_boot()`はLinuxが`SYSTEM_RUNNING`へ到達してからSMP／time試験を
再利用します。`kthreadd`生成、`cpu_stop_init()`呼出し、scheduler service初期化は行いません。

Gateは両CPUの`ksoftirqd`と実RCU／kworker thread、IRQ禁止状態のlocal／remote `irq_work`、IRQ出口から
`ksoftirqd`へ移る自己再投入tasklet、CPU別の基本work、割当メモリを解放するRCU callbackを検証します。
その後、既存のSMP／IRQ／idle／timer Gateを同じ環境で再実行し、high-resolution動作と
tickによるCPU占有taskの切替も確認します。想定外のLinux警告はGate失敗です。
試験側でサービス初期化やschedulerのtask選択は行いません。

remote `irq_work`はidleのcall-single queue flushでhardirq context外から処理される合法経路が
あります。taskletの最初のbatchはCPU固定のhard hrtimerで起動し、upstreamのidle flushの
semanticsを変えずに、本物のhardirq／IRQ出口からの進行を検証します。

[工程4の時間付き待機Gate](wait-gate-jp.md)も同じboot完了後に実行し、jiffy／高精度待機、
completion、waitqueue、sleep APIを検証します。期限切れ、早期wake、確定的なwake-before-schedule、
実signal、繰り返しwake後の残量、無関係なhard clockevent IRQを扱います。
このGateのための待機API置換やhost contract変更はありません。

[工程5のTree RCU／SRCU Gate](rcu-gate-jp.md)はreader保持中の安全性、normal／expedited GP、
preempt、migration、idle、callback／barrier順序、実SLUB回収を検証します。
ここでPOSIX machine境界のcallback全体にわたるIRQ抑止を発見・修正しました。
上位RCU実装はupstreamのままです。

[工程6のworkqueue Gate](workqueue-gate-jp.md)はnormal／highpri／BH／unbound／ordered、
pending／running cancel・flush、delayed work、自己再投入、FIFO、実行中のaffinity変更、
実RAM不足時のrescuerを検証します。hostや上位workqueue実装を加えず、試験だけを追加します。
固定upstreamの「drain中のBH再投入」の制約も明記しています。

[工程7のcleanup Gate](cleanup-gate-jp.md)は、実行中のIRQ／timer／work／RCUの依存関係について、
投入遮断・同期・reader GP・callback barrier・final work排出・実vmalloc alias無効化を検証します。
処理中の保持試験と並行IRQ試行をcleanupへぶつけます。hostや上位実装の置換はありません。

`build_boot_runtime.py --link`後、CMakeの`KOBOX_LINUX_BOOT_RUNTIME_CORE`へ生成した
`linux-boot-runtime.so`を指定します（`KOBOX_LINUX_TASK_BUILD_DIR`も必要）。CTestに
`kobox2.linux_boot_runtime_load`、`kobox2.linux_boot_service_gate`、
`kobox2.linux_timed_wait_gate`、`kobox2.linux_rcu_gate`、
`kobox2.linux_workqueue_gate`、`kobox2.linux_cleanup_gate`が追加されます。
隣の`linux-boot-inputs.json`も必要です。load検査だけでは実行Gateになりません。

工程7の検証結果：設定済みCTest全41件が成功。新規processで10回連続、毎回統合cleanupの52ケース、
同期待機観測20回、実vmalloc解放52個を確認し、Linux警告は0件です。hostや上位実装の変更はありません。

工程6の検証結果：設定済みCTest全40件が成功。新規processで10回連続、workqueueの211ケースと
callback 1,456回、両CPUの実RAM不足下のrescueを確認し、Linux警告は0件です。
この工程ではhostや上位workqueue実装を変更していません。

工程5のIRQ修正後の検証結果：設定済みCTest全39件が成功。boot／service Gateと独立したPOSIX
host Gateはそれぞれ新規processで100回連続成功。
時間付き待機Gateは22種類のAPI・両CPUにわたる350ケースと、無関係なhard IRQ 132回を、
新規processで10回連続成功しました。RCU／SRCU Gateも56ケース、callback 112回、実object解放
168個を新規processで10回連続成功。Linux警告は0件です。POSIX host GateのASan／UBSanも成功。
imageのmap／保護実装は工程4・5で変更しておらず、工程3のASan／UBSan試験でも成功しています。

実device／DMAのremove、GPU操作は後続Gateです。CPU hot-unplug、Linux userspace実行、一般のmodule text
保護・patch、完全なpanic／shutdown経路も、このGateでは認定しません。
