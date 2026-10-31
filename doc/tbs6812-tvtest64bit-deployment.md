# TVTest64bitへの追加配置

> 過去の作業記録。記載のスクリプトやINIは現在のリポジトリにない場合がある。現在の構成は[tbs6812-all.md](tbs6812-all.md)を参照。

2026-10-30、ユーザーの依頼で `C:\Free Soft Ware\TVTest64bit` にISDB-S3直接制御・自動選択・ウォームスタートを配置した。

## 使い方と構成

こちらのTVTestでも `BonDriver_dantto4k.dll` を選択する。2プロセスで同じBonDriverを使用できる。
両インストール先が使用するTBS6812は共通のため、同時に使えるのは合計2チューナー。

- 動作確認済みの受信DLL、共通INI、ライセンスを、新しい `4k8kdrver-direct` へ配置。
- 外側INIの参照先を `.\4k8kdrver-direct\BonDriver_BDA_4k.dll` へ変更。
- 共通ダンプ先を空にし、複数プロセスや別インストール先との書き込み競合を防止。
- 内側INIの `ExperimentalWarmStart=1` を有効化。
- 保存先は `C:\Free Soft Ware\TVTest64bit\4k8kdrver-direct\warm-start-cache`。別インストール先の状態ファイルはコピーしていない。

既存のdantto4k DLL（`v20261025-10-g446ac4b`）をそのまま使用した。
既存のBonDriver_BDA DLL/INI、PX-MLT DLL/INI、dantto4kのch2もハッシュで維持を確認。
従来の `4k8kdrver` は復元先として残している。

## 設置後の確認

本番の共通外側DLL・INIを読み込む2プロセスで確認。

| ラウンド | 選局先 | SetChannel実測 |
|---|---|---:|
| 状態ファイル作成 | B070 / B0B0 | 8.854秒 / 8.822秒 |
| 再オープン・4K | B070 / B0B0 | 1.003秒 / 1.022秒 |
| 再オープン・8K＋4K | B0E0 / B071 | 0.695秒 / 1.027秒 |

全6プロセスが受信・正常終了。高速化した4プロセスのログで状態再利用を確認した。
両チューナーの状態ファイル作成も完了。
全ファイルが目的のTSIDとなり、同期エラー・TEI・未復号パケットは0だった。
4Kの1ファイルに連番飛びが1件あり、先頭2,256,000バイトを除くと0。
新構成でのTVTestライブ表示と長時間録画は、今回のAPI・ファイル試験には含まない。

別のインストール先やツールの利用後など、保存状態と実機が一致しない場合は従来の初期化へ戻る。
各試行は [tbs6812-tvtest64bit-deployment.csv](tbs6812-tvtest64bit-deployment.csv)。

## バックアップと戻し方

元の外側INI：

`F:\VTemp\codex\BonDriver_BDA\work\deployment-backups\tvtest64bit-warm-20261030-222832-482\BonDriver_dantto4k.ini`

対象TVTestを終了し、このファイルを `C:\Free Soft Ware\TVTest64bit\BonDriver_dantto4k.ini` に戻すと、元の `4k8kdrver` を使う構成に戻る。
配置記録は `work/tvtest64bit-warm-deployment.json`。

ウォームスタートだけを無効にする場合は、TVTest終了後、内側INIの `ExperimentalWarmStart=0` に変更する。
ウォームスタート有効化前の内側DLL・INIのバックアップは次に保存している。

`F:\VTemp\codex\BonDriver_BDA\work\deployment-backups\warm-start-20261030-222832-921`

確認ログ・キャプチャ：`work/tvtest64bit-warm-check-20261030-222914/`。

配置したTBS6812.dllのSHA256：
`EE88835A908B18F1858180C8BEE006E3B551A2EFF5D8D3F9ADB83A0BD9FD5464`。
