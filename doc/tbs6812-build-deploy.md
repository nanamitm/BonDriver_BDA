# TBS6812用のx64ビルドと試験用配置

この構成は既存のTBSベンダープロパティによる選局を使う基準版です。
CXD2857ERのI²C直接制御による高速選局はまだ含みません。

## ビルド

Visual StudioのC++、Windows SDK、ATLを使用します。既定はv143・14.44.35207です。
本体とTBS拡張、BaseClasses、muparserを同じツールセットのRelease_Static/x64でビルドします。
ソリューションにはこのフォークで配布されていない別製品向けプロジェクトもあるため、必要な4プロジェクトだけをビルドします。

```powershell
./tools/build-tbs.ps1
```

成果物は `x64/Release_Static/BonDriver_BDA.dll` と `TBS.dll` です。
生成物とログはGitの管理対象外です。

## 設定

`config/BonDriver_BDA_4k.ini` はTuner 0／Capture 0を明示し、既存のBS4K/8Kチャンネル配列を維持します。
このソース版に存在しない `DefaultNetwork="4KBS/CS"` の代わりに、
`DefaultNetwork="BS/CS110"` と `ModulationType0SymbolRate=33756` を明示します。
単位はksym/sです。TLVを壊すTSヌルパケット除去は無効にします。

## 配置

対象のTVTestを終了してから実行します。別フォルダーに新しい本体・TBS.dll・INIを置き、
外側の `BonDriver_dantto4k.ini` は有効な `bondriverPath` の1行だけを変更します。
既存の `4k8kdrver` は上書きしません。

```powershell
./tools/deploy-tbs.ps1 -TVTestDirectory 'C:\path\to\TVTest'
```

変更前のINIバイト列は `work/deployment-backups/`、配置先・変更行・SHA256は `work/deployment.json` に保存します。
戻すときはTVTestを終了し、記録されたBackupをDanttoConfigへコピーします。

## 実機確認

```powershell
cmd /c tools\build-probe.cmd
./work/bon-driver-probe.exe 'C:\path\to\TVTest\4k8kdrver-built\BonDriver_BDA_4k.dll' 'BS07/TS0' 6 ./work/test.mmts
```

プローブはBonDriverのOpenTuner・SetChannel・GetTsStreamを使って受信します。
取得サイズだけで成功判定せず、別途TLV同期、TLVストリームID、サービス名、変換後のTSを確認してください。

coaxの直接制御試験後に既存BonDriverもロックしなくなった場合、単なるDLL差し替えでは直りません。
試験ではTBS6812のモードを `[2,2] → [1,2] → [2,2]` としてドライバーに再初期化させると復帰しました。
この操作はカード全体を再起動するため、自動配置スクリプトでは行いません。録画中の操作は禁止です。

TVTestでのライブ表示・操作確認は、BonDriver APIを使ったファイル取得試験とは別に扱います。
