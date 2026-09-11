# Pressure Gate の allocation 契約、2026-09-10

## 修正

従来の direct reclaim 正例は、最初の `__GFP_NORETRY` allocation 失敗で
停止していました。しかし、その失敗はテスト cache の回収不能を意味しません。
upstream の reclaim pass は、別 cache で進捗を得てテスト shrinker に scan
budget を割り当てる前に戻り、その後の allocation は失敗する場合があります。
`mm/shrinker.c:do_shrink_slab()` は count と scan を区別し、
`mm/vmscan.c:do_try_to_free_pages()` は回収目標到達時に pass を終了します。
count callback と `NULL` のどちらも、この cache の scan 実施を証明しません。

`require_direct_reclaim()` は、実 allocation 成功と実 `direct_freed > 0` の
両方を観測するまで、実際の allocation 要求を繰り返します。成功した全 page
を保持します。`__GFP_NORETRY`、`__GFP_NOWARN` と
`__GFP_KSWAPD_RECLAIM` の無効化は維持します。8192 要求または既存の
10 秒 Gate 区間の先着で正例を失敗させます。この上限はテストの制限であり、
Linux shrinker の scan 順序を保証するものではありません。

続く負例は従来の allocation flag を維持し、実 allocation 失敗、shmem の
`-ENOMEM`、データと pin の保持、pressure page 解放後の復帰を要求します。
最後の slab scan 実施と OOM kill 不発の検査も維持します。shrinker callback
の手動呼出、回収 policy 変更、失敗の成功扱いはありません。
公開 report、host 契約、OS kernel、ABI に変更はありません。

## Linux 検証

候補 ELF:

```text
.artifacts/kobox2-pressure-contract-runtime/linux-boot-runtime.so
6d3104e247811ffae5d6d1864b4acfde998e798bd58831bf6244ea24fc3d0344
```

既存の失敗時専用診断を含みます。735 個の canonical object、config、initcall
inventory は変更せず、`linux-boot-inputs.json` は保存済み canonical manifest
と byte 単位で一致します。canonical ELF は上書きしておらず、SHA256 は
`83a85d89568e35039491f1ff584fc4fa6d0bcd2bd6ca3aaedb96ee7ee6aae8fd`
のままです。

現行 source から再 build した既存 Linux launcher の結果:

| mode | 結果 |
| --- | --- |
| `--pressure`、独立 process 3 回 | PASS。direct 回収 7296/6400/7552、kswapd 回収 896/1792/640。各回 failures 2、recovery 1、warnings 0 |
| `--alloc-failure` | PASS。注入失敗 13、rollback 11、sweep 5 |
| `--all` | PASS。boot/SMP、memory、VFS、shmem、timed wait、RCU、workqueue、cleanup |
| `--vm-probe-pressure-fault` | PASS。実 delayed fault、pressure phase 6、direct 回収 7296、kswapd 回収 896、alias revoke 2、warnings 0 |

ログは外側 workspace の `.artifacts/tests/kobox2-pressure-contract/` にあります。
これは Linux 回帰検証であり、PachaOS adapter の完了や中断した pressure
ケースの解決を証明するものではありません。`--all` には独立 mode の pressure
と allocation-failure は含まれません。`-Werror` build/link/ELF 検査は成功し、
checkpatch は errors/warnings とも 0 でした。

## 再現

外側 workspace root から実行します。

```sh
python3 kobox2/linux-sandbox/kobox/boot/build_boot_runtime.py \
  --source-tree kobox2/linux-sandbox \
  --canonical-build-dir .artifacts/kobox2-client-exec-canonical \
  --provider-build-dir .artifacts/kobox2-client-elf-hosted-provider \
  --output-dir .artifacts/kobox2-pressure-contract-runtime \
  --link --with-gates --jobs 4
cmake --build .artifacts/kobox2-client-elf-gcc-build \
  --target kobox_linux_boot_test kobox_posix_vm_client -j 4
```

`.artifacts/kobox2-client-elf-gcc-build/linux-sandbox/kobox/kobox_linux_boot_test`
へ候補 ELF の絶対パスと上表の mode を渡します。
`--vm-probe-pressure-fault` には同じ build directory の
`kobox_posix_vm_client` の絶対パスを追加します。外側 timeout は 90 秒とし、
各 process の出力と終了 status を保存します。

pressure source SHA256:
`18288532b7d5927b0829fbd001e3940c90ebb3db2bdddb579b106a7f85976181`。
再 build した launcher SHA256:
`a75461b0a943f4798f367d3dab61e38581fd1c0344d84f68003d049aebf69450`。
