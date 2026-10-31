# 1つのBonDriverで全方式を切り替える

地上波、スカパー光（J.83B）、BS、CS110、ISDB-S3を1つのBonDriverで選局する。カードモードを書き換えず、グラフも作り直さず、
TBS6812のチップのコアだけを切り替える。Linux版の[tbs6812_drv](https://github.com/otya128/tbs6812_drv)にカードモードの概念がないことから試した方式。
**両入力に地上波・衛星の混合信号が来ている環境が前提。**

## 構成

```text
BonDriver_TBS6812.dll（BonDriver_dantto4k.dllのコピー）+ config/BonDriver_TBS6812.ini
 └ tbs6812-all/BonDriver_TBS6812_Inner.dll（BonDriver_BDA.dllの別名）+ config/BonDriver_TBS6812_Inner.ini + TBS6812.dll
```

`BonDriver_TBS6812.ini`の`smartCardReaderName`はS3の視聴に合わせて設定する。
空間0～3（UHF、Opticast、BS、CS110）のTSはそのまま渡し、空間4（BS4K8K）だけをTLVからTSへ変換する。

配置は`tools/deploy-tbs.ps1 -Destination <フォルダー> -DanttoWrapperPath <BonDriver_dantto4k.dll>`で行う。実機試験は`tools/test-all-tbs6812.ps1 -RunHardwareTests -BonDriver <DLL>`。
衛星信号がない環境は`config/BonDriver_TBS6812_T.ini`（地上波とJ.83B、入力は地上モード）を使う。

開く時に両入力が衛星モードでなく、カードが空いていれば、`2,2`を一度だけ書き込む（`AutoBoardMode=0`で無効）。カードが再起動する。
`BonDriver_BDA`を使わないアプリの受信は検出できず、止めてしまう。以後は書き込まない。

**`BonDriver_TBS6812_T.ini`のような地上モードの構成とは同居できない。** 統合版を開くと`2,2`に書き換わり、地上モードの構成は開けなくなる。
両方を使う場合は`AutoBoardMode=0`にして`tbs6812-board-mode.exe`で管理する。書き込みの保存先は未確認なので、頻繁な切替は避ける。

## 動作

- `DeliverySystem="ISDB-T/J.83B/ISDB-S/ISDB-S3"`で有効になり、各空間の`DeliverySystem`で方式を決める。地上モードの入力は開かない。
- 最初の選局だけ公式ドライバーで準備する。地上波とJ.83Bの時は`BootstrapSatelliteRFKHz`、`BootstrapSatelliteTSID`（既定はBS01/TS0）を代わりに使うので、衛星信号が必要。
- 切替はその入力のチップだけを変更するので、もう一方の入力の受信に影響しない。
- ロック後`DirectSettleMs`（既定300 ms）待ってからバッファを消去する。これがないと、選局直後の約600 KiBに前の方式のデータや不安定なデータが混ざる。
- 閉じる時は公式ドライバーが準備したコアと搬送波へ戻す。異常終了で別のコアが残った時は、次に開く時に戻す。

## 検証（2026-10-31）

- 全方式の往復は約0.5～1.3秒（ロック後の待ちを含む）。最初の選局は地上波とJ.83Bが2～7秒、S3が約8秒。
- 2プロセスで全方式を同時に切り替える試験で、全区間の先頭からTEI、同期、連続性エラーなし（最初のパケット境界までの端数は除く）、TSIDも一致。
- 地上波100秒とBS14（8K）100秒の受信、地上波とJ.83Bの強制終了からの復旧、S3の強制終了の後の起動、閉じた後の既存2K用・S3用BonDriver。
- TVTestでの視聴をユーザーが確認。Write_MMTSで地上波は`.ts`、4Kは`.mmts`として録画できることも確認した。

`DirectSettleMs`を入れて整理した後の構成でも、TVTestで問題なく動くことをユーザーが確認した。

**未検証:** EDCBでの録画、長時間録画、PC再起動後のモードの保持、切替途中のI2C失敗からの復旧。
