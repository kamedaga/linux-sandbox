# VM適合性試験プログラム

`vm_program.c`はnative hostの試験frontendで共用するLinux guest workloadを持つ。
FD継承、private pageのCOW、共有mapping、clone thread、group exit、自走実行が対象。
syscallの意味はLinuxが所有する。frontendが提供する`kobox_vm_program_syscall`は
引数と結果を運び、これらの処理を独自実装しない。

x86のstack access命令列は`arch/x86_64/vm_program.S`に置く。
native OSの起動、syscall捕捉、fault配送、private bootstrap状態は共通workloadの外側。
fork frontendは、Linuxが実際に継承するstackで子へ入り、複製されないnative dispatcher
stackを使わない。clone frontendはLinuxが指定した子stackで復帰し、そこにnative CALLの
戻り先が存在することを仮定しない。

これはGPLの適合性試験clientであり、controllerのwire APIやLinux process／FD実装の
代用品ではない。FS/GSがguest管理へ移ったclientでは、compilerによるnative TLS依存の
計装を実行できない。
