# TBS6812 ISDB-T対応

2026-10-31、ISDB-S実装 `f3d5cba` から分岐した
`codex/tbs6812-isdb-t` で追加。

## 構成

既存の `TBS6812.dll` にISDB-Tの直接選局を追加した。Windowsドライバーと
BonDriver_BDAのBDAキャプチャー経路は引き続き使用する。
ISDB-Tは188バイトTSを出力する。B-CAS処理はTVTest/EDCB側で行い、
dantto4kには通さない。

`tools/build-tbs.ps1` でRelease_Static x64をビルドし、次を同じフォルダーに置く。

- `BonDriver_BDA.dll` を用途に合わせた名前に変更したもの
- 同じベース名のINI（テンプレート `config/BonDriver_TBS6812_T.ini`）
- `TBS6812.dll`

例えば `BonDriver_BDA.T.dll` と `BonDriver_BDA.T.ini` の組み合わせで使う。
本番TVTest/EDCBにはまだ配置していない。

```ini
[TBS6812]
DeliverySystem="ISDB-T"
LockTimeoutMs=3000
```

新しい `DeliverySystem` はISDB-T / ISDB-S / ISDB-S3を受け付ける。
従来の `SatelliteSystem` も後方互換として残し、両方がある場合はDeliverySystemを優先する。
両方省略した場合は従来どおりISDB-S3になる。地デジの帯域は6 MHzに限定した。
空きチューナーの自動選択と、FriendlyName末尾の0/1による固定指定も使える。

## 地デジ／衛星モード

カードの受信モードはINIとは別の設定であり、対象入力をあらかじめ
**地デジモード（値1）** にする必要がある。衛星モードは値2。
例えば入力0を地デジ、入力1を衛星に設定すれば、T/SまたはT/S3の同時使用ができる。
両入力とも地デジならT/Tの2系統を使用できる。合計2系統であり、方式ごとに2系統ではない。

参照実装のモード変更プロパティはカード全体を再起動する。
そのため、このDLLはモードを自動変更せず、要求と違うモードは拒否する。
モード変更時はカード上の全受信を終了し、メーカーの受信モード変更ツールなどで
変更してからアプリを開き直す。INIのDeliverySystemを書き換えるだけではモードは変わらない。
試験後は、試験前と同じ **入力0=衛星、入力1=衛星** に復元済み。
設置済みBonDriver_BDA.2K.dllでBS朝日の受信も再確認した。

## 制御

1. 最初の選局は公式ドライバーに任せ、CXD2857ER ID 0x091、24 MHzクロック、
   ISDB-Tコア0x06、パラレルTS、6 MHz設定とロックを確認して引き継ぐ。
2. 以後は、FreiaのISDB-T 6 MHz用RF設定を使い、RF周波数をkHzで設定する。
   対応範囲は90～770 MHz。衛星用のTSID選択レジスターやLNB電源には書き込まない。
3. TS出力をbank 00/C3で停止し、RFを変更してソフト再捕捉後に再開する。
   公式ドライバーが準備した復調器のIF・等化器・帯域係数は保持する。
4. ロックはbank 60/10の同期・TSロック・アンロックを確認する。
   C/Nはbank 60/28から取得し、`10*log10(raw)-9.031` dBに換算する。
   0は未測定として扱い、読み取り失敗時もレジスターのfreezeを解除する。
5. 終了時は最初に正常受信した周波数と出力設定を復元する。
   コールドリセット、共有REFOUT/GPIOの再設定は行わない。

初回に放送のないチャンネルを選んだ場合は、引き継がずに失敗を返す。
次のチャンネルでも公式ドライバーで準備をやり直すため、スキャンは続行できる。
ロック前に直接制御へ移ると、遅れて動作する公式ドライバーの選局処理に
上書きされる例が実機であったため、ロック成立までは直接制御に切り替えない。

**ISDB-Tの起動キャッシュは無効。** 試作では再オープン時のロック確認に失敗し、
現在のRF状態を十分に検証できないため、ExperimentalWarmStart=1でもログに理由を記録して
地デジのキャッシュ利用を無効にする。ISDB-S/S3の既存キャッシュ動作は維持する。

## 検証

- Release_Static x64ビルドと `tools/test-tbs6812.cmd`。
- 模擬I2CでRFパケット、6 MHz用設定、TS出力、共有クロック/GPIOの保持、
  ロックの全組み合わせ、C/N換算、測定失敗時のunfreeze、部分失敗後の復元、
  範囲外入力の拒否、地デジキャッシュの拒否、衛星側の回帰テスト。
- 実機でUHF13（NHK Eテレ沖縄、TSID 0x7C11）、14（RBC、0x7C12）、
  16（琉球朝日）、17（NHK総合沖縄、0x7C10）を受信。
- 初回SetChannelは約2.9秒、以後の直接選局は約0.7～0.95秒。
  OpenTunerの所要時間は別に約2.8～4.2秒。C/N表示は約27～29 dB。
- UHF51の未受信から、同じオープンのまま13→14へ切り替えて受信できることを確認。
- T/S、T/S3の同時受信と衛星側の選局・再オープン、T/Tの同時受信を試験。
- Tが使用中で、空いている入力が衛星モードの場合、追加の地デジオープンが失敗することを確認。

選局直後のデータには旧データ・TEI・連続性エラーが混ざる場合がある。
この境界の改善はS/S3と共通の残課題。調査対象サンプルの先頭3 MiBを除いた部分は
同期・TEI・連続性エラー0だったが、固定量を捨てる対策は実装していない。
TVTestでの映像音声確認や長時間録画まで検証したという意味ではない。

両入力のモードを事前に設定したうえで、次の明示的な実機試験を実行できる。
このスクリプト自身はモードを変更しない。試験チャンネルは地域に合わせて変更する。

```powershell
cmd /c tools\build-probe.cmd work\bon-driver-probe-isdbt.exe
./tools/test-terrestrial-tbs6812.ps1 -RunHardwareTests -TerrestrialSlot 0 -PeerSystem T
# 入力0=地デジ、入力1=衛星の場合
./tools/test-terrestrial-tbs6812.ps1 -RunHardwareTests -TerrestrialSlot 0 -PeerSystem S3
```

試験用DLL/INIはwork/isdb-t-matrix、ログとストリームはwork/isdb-t-resultsに保存する。
試験プロセスの成功に加えて、取得TS/TLVの識別情報・連続性を別途解析する必要がある。
