# ウォームスタートの本番配備

> 過去の作業記録。記載のスクリプトやINIは現在のリポジトリにない場合がある。現在の構成は[tbs6812-all.md](tbs6812-all.md)を参照。

2026-10-30、ユーザーの依頼により`C:\Free Soft Ware\TVTeset4k8k`へ配備した。
同じ`BonDriver_dantto4k.dll`を2つのTVTestから使う、自動割り当て構成を継続する。

## 更新内容

- `4k8kdrver-direct/TBS6812.dll`をウォームスタート試作版へ更新。
- 既存の`4k8kdrver-direct/BonDriver_BDA_4k.ini`の[TBS6812]に次を設定。
- ベースBonDriver、dantto4k DLL・外側INI、チャンネル・自動選択設定は維持し、ハッシュでも確認した。

```ini
ExperimentalWarmStart=1
WarmStartCacheDirectory=C:\Free Soft Ware\TVTeset4k8k\4k8kdrver-direct\warm-start-cache
InspectWarmState=0
```

共有のキャッシュディレクトリー内に、チューナー別の状態ファイルが2つ作成されている。
正常終了で復元・再ロックが確認できた状態だけを次回に再利用する。
初期化が必要な場合や保存状態が使えない場合は、従来のBDA選局経路を使う。

## 設置後の実機確認

本番の同じ外側DLL・INIを使う2プロセスで、各ラウンドを同時実行した。

| ラウンド | 選局先 | 実測SetChannel |
|---|---|---:|
| 保存状態作成 | B070 / B0B0 | 8.813秒 / 8.857秒 |
| 再オープン・4K | B070 / B0B0 | 0.998秒 / 1.033秒 |
| 再オープン・8K＋4K | B0E0 / B071 | 0.683秒 / 1.043秒 |

両チューナーでウォームスタートの採用ログを確認し、全6プロセスが受信・正常終了した。
初回の保存状態作成も済んでいるため、現在は再利用可能な状態で待機している。
各試行の数値は [tbs6812-warm-start-deployment.csv](tbs6812-warm-start-deployment.csv)。

全6ファイルで目的のTSIDを確認し、同期エラー・TEI・未復号パケットは0。
4Kの2ファイルに選局直後の連番飛びが各1件あり、先頭2,256,000バイトを除くと0だった。
既存の切り替え直後の品質改善は継続課題。新しい構成でのTVTestのライブ表示と長時間録画は別途確認が必要。

## バックアップと復元

更新前のDLL・INIと元のSHA256を、次へ保存している。

`F:\VTemp\codex\BonDriver_BDA\work\deployment-backups\warm-start-20261030-220859-963`

対象TVTestを終了してから、リポジトリで次を実行すると、更新前の2ファイルに戻せる。
復元スクリプトは別の試験用ディレクトリーで実行し、ハッシュ一致まで確認済み。

```powershell
./tools/restore-warm-start.ps1 -BackupDirectory 'F:\VTemp\codex\BonDriver_BDA\work\deployment-backups\warm-start-20261030-220859-963'
```

機能だけを無効にする場合は、TVTestを終了してから内側INIの`ExperimentalWarmStart=0`へ変更する。

## 再配置と記録

現在の設定を維持してウォームスタートだけを配置するコマンド：

```powershell
./tools/build-tbs.ps1
./tools/deploy-warm-start.ps1 -TVTestDirectory 'C:\Free Soft Ware\TVTeset4k8k'
```

配備記録：`work/warm-production-deployment.json`。
受信ログ・キャプチャ：`work/warm-production-check-20261030-221031/`。
実装ソース：`70b9189`、配置先TBS6812.dllのSHA256：
`EE88835A908B18F1858180C8BEE006E3B551A2EFF5D8D3F9ADB83A0BD9FD5464`。

コールドスタート、OS再起動・復帰、別ツール操作後、キャッシュI/O失敗時などは引き続き検証対象。
試作の設計と制約は [tbs6812-warm-start-research.md](tbs6812-warm-start-research.md) を参照。
