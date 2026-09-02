# linux-sandbox

`linux-sandbox`はkobox2が利用するGPL-2.0-onlyのLinux runtimeです。Apache-2.0
controllerとは別processとして実行し、self-loader、Linux core primitive、subsystem
実装、module symbol、構造体、load済み`.ko` stateなど、Linux固有のcodeをすべて
所有します。

独自compatibility fileの集合ではなく、upstream stable Linux treeを基準にします。
対応するLinuxのバージョンは `v6.18.48`。