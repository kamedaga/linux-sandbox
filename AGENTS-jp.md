# linux-sandbox agent notes（日本語版）

- `kobox/`配下のkobox所有C codeを含め、upstream Linuxの
  [`Documentation/process/coding-style.rst`](./Documentation/process/coding-style.rst)に
  従います。repository既存の`.clang-format`と`.editorconfig`を使い、upstream codeを
  一括formatしません。
- このリポジトリはGPL-2.0-onlyであり、kobox2 controllerとは別processとしてbuildします。
- pinした本物のLinux headerを使い、Linux構造体offsetを数値定数として埋め込みません。
- upstream Linuxのpathとpinしたbaseline provenanceを維持します。standalone repositoryには
  upstreamの全履歴ではなく、tag付きの完全なsource snapshotをimportします。kobox固有
  loader、host contract、manifest、integration codeだけを`kobox/`へ置きます。
- Linux primitiveとsubsystemにはupstream実装を優先します。成功を偽装するstubは禁止し、
  unsupported featureは生成manifestを通じて拒否またはabortさせます。
- 最初のruntime fixtureからSMPを有効にします。kthread、workqueue、RCU、lockを
  cooperative coroutineで近似しません。
- PachaOS service名、role policy、package path、syscall番号を置きません。
  host固有adapterはhost OSリポジトリが所有します。
- Apache-2.0のkobox2 controller codeをGPL-2.0-only processへlinkしません。
  共有protocol fileには独立したMIT licenseを使います。
- kobox host contractと共有protocolを`dev` interfaceとして扱います。明示的にfreezeするまで
  ABI番号と互換性を割り当てません。

実際にagentへ適用される正本は[`AGENTS.md`](./AGENTS.md)です。
