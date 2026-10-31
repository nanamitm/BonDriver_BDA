# 同じBonDriverで地デジとJ.83 Annex Bを選ぶ

> 過去の作業記録。記載のスクリプトやINIは現在のリポジトリにない場合がある。現在の構成は[tbs6812-all.md](tbs6812-all.md)を参照。

2026-10-31、`codex/tbs6812-terrestrial-combined` で追加。
`BonDriver_BDA.T.dll`を1回開いたまま、UHFとスカパー光のチューニング空間を切り替えられる。
カード側は地デジモード（値1）のまま、取得済みの入力の復調器だけを切り替える。
DLL内でカードのモード変更や全体の再起動は行わない。

## 設定

`config/BonDriver_TBS6812_T.ini`が共用設定のテンプレートになった。
ビルドしたBonDriver_BDA.dllをBonDriver_BDA.T.dllへ改名し、同名INIとTBS6812.dllを同じ場所に置く。

```ini
[TBS6812]
DeliverySystem="ISDB-T/J.83B"
BootstrapFrequencyKHz=473143
LockTimeoutMs=3000

[Modulation]
ModulationType0SettingsAuto="ISDB-T"
ModulationType0BandWidth=6
ModulationType1SettingsAuto="J.83B-Opticast"
ModulationType1BandWidth=6

[TuningSpace00]
DeliverySystem="ISDB-T"
TuningSpaceName="UHF"
ChannelSettingsAuto="UHF"

[TuningSpace01]
DeliverySystem="J.83B"
TuningSpaceName="Opticast"
ChannelSettingsAuto="Opticast"
ChannelSettingsAutoOptions="All"
```

グローバルの共用モードと、各空間の方式指定が両方必要。
空間の方式が未指定なら、その空間の選局をエラーにする。周波数だけで方式を推測しない。
空間ごとのISDB-S/S3指定や不明な方式は拒否する。地デジ・衛星のカードモードは共用できないためである。
以前の固定設定（ISDB-T / J.83B / ISDB-S / ISDB-S3）とSatelliteSystemの後方互換は維持する。
固定設定では従来どおり全空間をその方式として扱う。

UHFは空間0、Opticastは空間1になる。Opticastは109チャンネルの周波数順。
配置後はTVTestでチャンネルスキャンを行う。
FriendlyName末尾0/1による入力固定と、末尾を省略する空き入力の自動選択は従来どおり。
1インスタンスで受信する搬送波は1つであり、空間を増やしても同じ入力で2搬送波を同時受信する機能ではない。

J.83Bはスカパー！プレミアムサービス光の5.6 Msym/s専用。
J.83Bから初めて選局する場合は、同じ入力で受信できる地デジの搬送波を準備に使う。
初期値473143 kHzはUHF13。地域に応じて受信可能な地デジ周波数へ変更する。
地デジから始める場合は、最初に正常受信した地デジ局の状態を引き継ぐ。
起動キャッシュは地デジ側と同様に無効。TSは視聴アプリへ直接渡し、dantto4kへは通さない。
限定受信・視聴処理は各サービスに対応した既存のアプリ／カード設定を使用する。

## 切り替え

最初の地デジのロック成立までは公式ドライバーで準備する。
初めてJ.83Bへ移るときは、準備した地デジキャリアへ戻して1.2秒連続した安定ロックを確認する。
公式ドライバーの準備完了を確認済みなら、以後のT→J.83Bではこの待ち時間を繰り返さない。
J.83B→Tでは、J.83Bの終了・地デジコアの起動・保存した設定の復元と再ロックを行い、目的の地デジ局へ選局する。
終了時も最初の地デジ状態へ復元する。
切り替え失敗時は失敗を返し、不確実な状態で別方式の選局を続けない。

## 検証

- Release_Static x64ビルド、CXD制御・INI振り分け・機器識別の単体テストが成功。
- 模擬I2Cで、同じオブジェクトのT→J.83B→Tを3回繰り返してコア・RF設定・復元を確認。
- 実機で地デジからの8選局、J.83Bからの6選局、未受信UHF51からJ.83B→Tへ復帰する3選局を確認。
  各試験でDLLを開いたまま往復し、正常終了を確認した。
- 安定部分のTSIDは地デジ13/14/17が0x7C11/0x7C12/0x7C10、
  光H004/H005/H030/H031が0x3017/0x3019/0x3018/0x3020で一致した。
- 初回準備後のT→J.83Bは約0.23～0.25秒、J.83B→Tは約1.5～1.6秒。
  最初のT→J.83Bは安定待ち込み約1.46秒、J.83Bから初めて起動した場合は初回選局約5.17秒。
  OpenTunerの時間は別。速度は今回の短時間試験での値。
- 16個の取得TSについて、先頭3 MiBを除いた部分の同期・TEI・連続性エラーは全て0。
  選局直後の旧データ混在は残課題のままで、固定量の破棄処理は追加していない。
- 地デジとJ.83Bを2入力で同時受信する試験、TVTestの映像音声、長時間録画、異常終了後の復旧は未検証。
  本番DLLの配置・カードモードの変更は今回行っていない。

```powershell
./tools/build-tbs.ps1
cmd /c tools\test-tbs6812.cmd
cmd /c tools\test-opticast.cmd
# 入力0が空いており、地デジ・光信号が入っている場合のみ。チャンネルは試験環境用。
./tools/test-combined-tbs6812.ps1 -RunHardwareTests
```
