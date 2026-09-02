# linux-sandbox agent notes（日本語版）

- このリポジトリはGPL-2.0-onlyであり、kobox2 controllerとは別processとしてbuildします。
- pinした本物のLinux headerを使い、Linux構造体offsetを数値定数として埋め込みません。
- upstream Linuxのpathとhistoryを維持します。kobox固有loader、host contract、manifest、
  integration codeだけを`kobox/`へ置きます。
- Linux primitiveとsubsystemにはupstream実装を優先します。成功を偽装するstubは禁止し、
  unsupported featureは生成manifestを通じて拒否またはabortさせます。
- 最初のruntime fixtureからSMPを有効にします。kthread、workqueue、RCU、lockを
  cooperative coroutineで近似しません。
- PachaOS service名、role policy、package path、syscall番号を置きません。
  host固有adapterはhost OSリポジトリが所有します。
- Apache-2.0のkobox2 controller codeをGPL-2.0-only processへlinkしません。
  共有protocol fileには独立したMIT licenseを使います。

実際にagentへ適用される正本は[`AGENTS.md`](./AGENTS.md)です。
