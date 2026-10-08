# upstream VFS objectの寿命管理Gate

第2章・工程2は、固定の実shmem boot coreでupstreamのmount・pathname・file・inode・address_space経路を
実行します。**runtime port・host操作・VFS実装・filesystem操作のwrapper・初期化fixtureは追加しません。**
工程1のcanonical設定とbuilt-in object 599個／initcall target 129個を維持します。
builderはVFSのsource ownerも必須にし、VFS・fput・iput・task-workの置換を拒否します。

後述のstorage portは別profileへの追加であり、この寿命管理のworkloadは変更しません。

実行は`kobox_linux_boot_test CORE.so --vfs`、CTestは`kobox2.linux_vfs_lifetime_gate`です。
`--all`でも、必須のboot／service／SMP／memory確認の後、第1章の詳細回帰試験の前に実行します。
新Gateを含めてcoreを再ビルドする必要があり、旧memory入口だけを持つELFは更新したload検査を通りません。

## 操作と参照

2 CPU × task-work／kthread delayed-fput × FD closeとunlinkの順序で8ケースを実行します。各ケースで：

- bootで登録済みのtmpfsを取得し、`kern_mount()`する。filesystem登録・cache初期化・サービス起動は行わない。
- `file_open_root_mnt()`で名前付きfileを生成。`kernel_write()`で先頭に穴を残した3 page分のpatternを書き、
  別々にopenしたfileから`kernel_read()`で初期ゼロ・内容・位置・EOFを確認。
- file／inode／folio参照を一切保持せず名前付きfileを最終closeし、task work実行後に同じ名前をreopen。
  内容が残っていることを確認してからunlink。
- 実Linux FDを登録し、`fget()`／`get_file()`の参照数を確認。FD close後も別に保持したfile参照でI/Oできること。
- open中にunlinkすると名前からは取得できなくなるが、既存のfile参照は使えること。
  同名再作成では別inodeとなり、古いfileの内容を置き換えないこと。
- `-EEXIST`、存在しない名前のlookup／unlink、再closeの`-EBADF`、read-onlyへの書き込み拒否を確認。
  最後の確認はupstream `vfs_write()`のmode検査にNULL user bufferを渡すもので、user copyの検証ではない。
  `kernel_write()`は書き込み可能なcallerを要求し、誤用時には警告する。
- 明示的な`mntget()`参照とopen fileを残してownerの`kern_unmount()`を行い、mountが使えること。
  保持したmountの最終putは依存するinode参照の解放後に行う。

`address_space`はinodeに埋め込まれ、独立した参照countを持ちません。実`igrab()`参照を保持して、fileの
最終解放後もmappingの対応・page cache・内容が維持されることを検証します。
さらに独立したfolio参照を保持し、最後の`iput()`でinodeがevictされpage cacheから除去されてもpageが生存する
ことを確認します。folio参照はinodeやmappingを生存させません。最後のfolio put前には内容が残り、mappingが
なく、試験の参照だけになっている必要があります。

## 遅延処理と実回収

通常task側は、実際に`PF_KTHREAD`でないhosted PID 1から最終`fput()`します。task workが登録され、明示的な
`task_work_run()`の前はdentryによるinode参照が残り、実行後に減ることを確認します。
これはkernel側harnessの処理境界であり、userspace syscall-return経路の実装済みという意味ではありません。

kthread側は、反対CPUの実upstream kthreadから最終`fput()`します。実際の遅延workerがinode参照を減らした
ことを先に観測し、その後`flush_delayed_fput()`で実行中の処理も完了させます。controllerはこれらprivate fileの
unmount／releaseに必要なlockや依存を保持しません。試験callbackによるworkerの代行はしません。

最終`iput()`の前から別kthreadがRCU read-side sectionを保持します。eviction後もinodeのstorageを保護した状態で、
`I_FREEING | I_CLEAR`と空のmappingを観測します。投入済みmarker callbackはまだ実行されないことを要求します。
readerはRCU section内で自発的にsleepせず、異常時の期限を持ちます。解放・join後、`rcu_barrier()`でcallbackを
完了させます。このmarkerだけで実回収済みとは判定しません。

その後、upstreamからの新しい確保で解放したfile／inode storageが再利用されることを必須にします。
別の0 byte shmem fileを各対象slabに同居させ、slab全体の返却によってstorageが別cacheへ移動するのを防ぎます。
同居objectは別の内部shmem mountに属し、**対象objectの参照は保持しません**。allocator policyや解放処理は
変更しません。両CPUで上限付きの確保を行い、probeを同時保持します。解放済みobjectは読みません。
probe／同居fileも同期putし、RCU callbackを排出します。

`filp`の`SLAB_TYPESAFE_BY_RCU`はslabのstorageを保護し、古いfile objectの同一性を保証するものではありません。
file objectはGPを待たずに再利用できます。inode自身のRCU遅延解放とは区別します。

最後のfolio put後は実page確保で3枚すべてのPFNが再利用されることを確認します。元のmappingのGFP zone／
mobility classを使い、shmemのmovable PCPを無関係なunmovable確保で検証しようとはしません。

最後のmount参照を落とすと、superblockはupstreamの **RCU → system work → `kfree()`** で解放されます。
GPだけでは不十分なので、workの進行後に上限付きの新規確保でstorageが再利用されることを確認します。
system-wide workqueueのflushや、既に解放された可能性がある`destroy_work`へのアクセスはしません。
probe確保や回収観測に失敗した場合はGate失敗とし、解放成功として扱いません。

## 範囲と回帰試験

期待件数はmount／case 8、I/O確認56、名前付きfile再open 8、negative確認40、task-work 4、delayed-fput 4、
RCU保持8、file回収8、inode回収8、page PFN再確保24、superblock回収8です。
予期しないLinux警告は失敗です。異常時は参照の可能性があるstorageを保持してlauncherがprocessを即時終了します。

工程1の実shmem canonical設定で`build_boot_runtime.py --link`し、runtimeには新しい出力directoryを使います。
CMakeの`KOBOX_LINUX_BOOT_RUNTIME_CORE`を新coreへ更新します。Kconfigやhost／wire ABIの変更は不要です。
builder検査もVFS実装のownerを保護するよう拡張しています。

2026-09-06の確認：厳密なcore build／load（initcall target 129個を保持）、boot builder 8試験、
構成済みCTest 43件、新規processでのVFS Gate追加50回がすべて成功しました。
新GateのC／headerはcheckpatchの警告なし。VFSの全実行で上記の全件数が成立し、Linux警告もありません。
さらに新規processで`--all`を10回実行し、すべて同じboot内でVFSと第1章の全詳細Gateが成功しました。警告なし。

検証範囲はsandbox内のkernel側VFS呼び出しとLinux FDの寿命です。userspace syscall入口、user-copy fault処理、
mount namespaceの伝播、共有mapping、memory-pressure policy、GEM、外部clientのFDやDRM IPCの完成は主張しません。
それぞれ対応する工程で実装・検証します。

## storage portとnativeとの差分Gate（dev）

`fs_port.c`は`--device-profile storage`でのみ組み込むGPLのprocess-local portです。
許可済みmountへの参照と、`struct file`の参照を保持する動的xarrayを持ちます。
handleは0以外の単調増加IDで、同じport内では再利用しません。tableのmutexはlookupと
`get_file()`、削除を保護しますが、filesystem I/O中には保持しません。
dupはupstreamのflagsとoffsetを含む同じfileを共有します。offset操作は
`FMODE_ATOMIC_POS`の契約に従い、非seekable fileがmutexのunion領域を再利用することも考慮します。
I/Oが取得済みの参照は並行closeで無効になりません。callerは呼び出し中のportと不変なcredを保持し、
destroy前に全callerを停止する必要があります。

要求ごとに`override_creds()`／`revert_creds()`を使います。openat2のresolve flagsも含めて、
openはpinned Linuxの`build_open_flags()`と`do_file_open_root()`に渡します。
read/writeは必要なaccess modeを検査して`kernel_read()`／`kernel_write()`へ渡します。
他は`iterate_dir()`、`vfs_getattr()`、upstreamのparent lookupとmutation helper、
`vfs_get_link()`、`do_ftruncate()`、`vfs_fsync()`です。getdentsは固定の名前配列や独自の転送上限を
持たず、alignedな`linux_dirent64`を返します。前のrecordと最後のrecordにupstreamのcookieを設定し、
paddingはゼロにします。

これは**wire ABIや全syscallを扱うadapterではありません**。現時点のpathは許可済みmountをrootにし、
任意のopenat directory handleを起点にはしません。getattrは実`kstat`を返し、privateな
`vfs_statx()` wrapperによるmount-IDの補完はこのinterfaceに含めません。
controllerによる資格情報の検証、要求のumask/context、完全なat系/schemaの意味論、
device許可とUUID選択、hardware package/serviceへの接続は次のcontroller統合で扱います。
Linuxのstructをcontrollerのprotocol境界には渡しません。

`manifest/profiles/storage.config`はext4・jbd2・mbcache・crc16をmoduleにします。
既存のpinned device profileとは分離しています。検証専用`storage_test.config`はそれらをbuiltinにし、
32 MiBのbrdを1個だけ追加します。既存shmem Gateに合わせ、base page／swapなしを明示します。
このcoreにはGPUやvirtio deviceを許可しないため、無関係なvirtio device Gateは含めません。
sync-fileやhardwareのstubは追加しません。

kobox2のrootから、LLVM/LLD 18.1.8と対応するKbuild用host libelf環境でビルドします。
`HOST_CC`はPOSIX検証compiler、`MUSL_CC`はstatic native比較用compilerです。
musl wrapperの場合は`REALGCC`も指定します。provider cacheはcanonical config・vmlinux・archiveの
identityを固定するため、出力先は新規directoryにします。

```sh
export HOST_CC=/usr/bin/cc
export MUSL_CC=/usr/bin/musl-gcc
export REALGCC=/usr/bin/x86_64-linux-gnu-gcc
OUT=/absolute/path/to/fresh-artifacts/fs-port
bash linux-sandbox/kobox/boot/build_fs_port_test.sh "$OUT"
python3 linux-sandbox/kobox/boot/test_fs_port.py \
  --core "$OUT/runtime/linux-boot-runtime.so" \
  --host-test "$OUT/host/linux-sandbox/kobox/kobox_linux_boot_test" \
  --native-test "$OUT/native-test" --iso /path/to/alpine-virt.iso \
  --out "$OUT/results" --new-run
```

実`start_kernel()`と必須boot/memory Gateの後に実行します。rootfsではなく**新規の使い捨てext4 image**を
実block file I/Oでbrdに載せ、`fs_context`でext4をmountします。tmpfsは別mountで検証します。
task work・delayed fput・RCUを排出し、ext4をunmountしてblock cacheをinvalidateした後にdiskを回収し、
host側でmappingをsyncします。

共通のC workloadをport経由とnative Linux syscall経由で実行します。native側はserial consoleのAlpineで、
新規ext4/tmpfs mountへchrootします。sparse/positioned I/O、truncateと拡張、append、dup offset、
open-unlinkの寿命、O_PATHエラー、UID/GID/補助groupのDAC、sticky directory、
rename/exchange/noreplace、symlinkとresolve制限、255/256文字名、短いdirectory buffer、cookie、
名前/type/重複、fsync/fdatasyncを確認します。ext4のhash seedやsyscall bufferのpaddingは異なり得るので、
列挙順やpaddingを契約とは見なしません。

port固有の検査ではFSごとに10,000 handleを同時保持し、stale handleを拒否します。
両CPUで共有offsetのreadを合計512回、readとcloseの競合を1,024回実行します。Linux警告は失敗です。
回収した両ext4 imageに`e2fsck -fn`を要求し、rename済みのdataと資格情報付きfileのdataの永続化を
debugfsで確認します。結果には実行物/sourceのsnapshot、hash、core inventory、native QEMU引数とconsole logを残します。

CTestでは`KOBOX_LINUX_STORAGE_RUNTIME_CORE`、`KOBOX_FS_NATIVE_TEST`、`KOBOX_FS_NATIVE_ISO`を設定し、
`kobox2.linux_fs_port`を実行します。実行ごとに新規の結果directoryを残します。
別途`build_fs_port_test.sh NEW_OUTPUT --modules-only`で、brdやbuiltin ext4なしの実4-module closureを
ビルド・検査できます。
