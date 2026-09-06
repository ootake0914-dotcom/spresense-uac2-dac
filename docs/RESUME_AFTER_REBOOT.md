# 再起動後再開メモ（2026-09-05 15時台、FW Rev11動作中）

## 適用済みFW変更
- `src/uac2_driver.c`: Clock Source `UAC2_CS_RANGE` を44.1k/48k/88.2k/96k/176.4k/192kの6 discreteレンジ化（74B応答）
- `src/uac2_desc.c`: Feature Unit `bmaControls(0/1/2)` を全て `0x0F`（Mute+Volume R/W）に有効化
- `src/uac2_desc.c`: `wTotalLength` 静的値を `0x93`（adaptive 147B）/`0x9A`（async 154B）に修正
- `include/uac2_desc.h`: `bcdDevice 0x0102`（Windowsキャッシュ切り分け用）
- `src/uac2_driver.c`: SET_CONFIG時にUSB速度をprintf（2=FULL, 3=HIGH）
- `src/uac2_main.c`: 起動表示 `FW Rev11`

## 検証済み事実
- USB速度 HIGH(480M) — 帯域不足説は否定
- RANGE応答74B、FU per-ch応答をNSHログで確認
- Windows: `ProblemCode 0`、usbaudio2新規Event16なし、レジストリ既定値48B/48B適用済み
  （`fix_shared_mode.py --apply --rate 192000` 実行済み）
- 未解決: `mmsys.cpl` 詳細タブなし・レベル枠のみ、共有/排他ともピン不発（USB側SET/Alt1なし）

## 対象デバイス特定子
- USB: `VID_054C PID_0CED`（REV_0102）、MI_00 `…7&22DDB294&0&0000`
- Endpoint GUID: `{4405314d-0355-47a4-b3a3-1ed746ba17f3}`（ACTIVE スピーカー）
- 旧PIDゴースト6件（0x0CE6/0x0CE7/0x0CE9/0x0CEA/0x0CEB/0x0CEC）は無害・放置可

## 再起動後の手順
1. `mmsys.cpl` → Spresense →「詳細」「レベル」確認
   - 直れば終了（実音は別途Step5 CXD5247復帰が必要。`uac2_audio_dma.c`は現在スタブ）
2. ~~改善なしなら対話ターミナルで実行し出力全文を貼る~~（当該スクリプトは公開ツリーから削除。Windows標準ドライバ経路は放棄済み）
3. 併せてCOM6（115200bps）のNSHログ（`SET_CONFIG`・`SET_CUR`・`Alt:1`の有無）を取得

## 追記4（12vs1未解決・次手確定）
- 新規インスタンス{35976339}でもsink=bridgeのみ。open試行後の再照会でも変化なし
- waveOutOpenは16/48k等すべてWAVERR_BADFORMAT(32)、KS交差も全不発
- FW側：記述子195B機械検証OK、全EP0応答ret>=0、HIGH確定
- 候補：(B) Linux/Mac対照があれば2分で確定 (A) USBPcapでwire真実取得 (C) FW側EP0計装
- Wireshark/USBPcapは未導入を確認済み

## 追記5（Linux対照＝ファーム無罪確定）
- Linux対照機で `054c:0ced` high speed認識、ALSA card1+PCM0生成。パース完全成功
- よって不具合はWindows usbaudio2固有の検証規則

## 追記7（Rev15でNSH完全可視化）
- Rev15で停止せず生存。SET_CUR時計→SR:48000反映、Volume SET、RANGE74全応答
- しかしSET_INTERFACE Alt1/2が一度も来ない（`applying alt`なし）→ALSAがAlt選択前にEPIPE
- ALSA stream0/hwはAlt1(S16)/Alt2(S32)＋6レートを完全認識しているのに、である

## 追記10（自己破壊の自白とクリーン再試験）
- cap_serial.pyがDTRアサート＝毎回MCUリブートしていた。12↔1・番号上昇・EPIPE嵐の主因は自 rig
- cap_serial.pyを受動化（cap_pulse.pyのみ明示リブート用）。以後リブートなしで計測
- 12レンジ1回は「無擾乱・ settled 状態」の真値の可能性。4分静置後に再照会する

## 追記11（2026-09-06 Linux完走：発音＋ERR二相＋PipeWire汚染。FW Rev45）
- 方針転換済み：CXD5602はAlt>0自律STALLのためWindows不可、Linux/RPi/Volumio専用DAC化
- Rev42（SDK err分岐を停止→クリア継続化）で発音達成。ただし試聴でノイズ・途切れあり
- ERRは2相：slow相（~2-4/s、SNAP=0x03/0x13/mon0x00030000）⇔爆発相（~760/s全バッファ、SNAP=0x02/0x12/mon0x00020001）。自己限定・周期的（ビート状）。エンジン流量は両相とも不変
- 発火開始は非決定的（起動後8s/22s/70s/100s/245s）。再生は加速要因だが必須ではない。アイドルのみ240sでも発火
- H-α（in-flight16固着）→Rev43上限14でも発火継続で後退。Rev44 dither（12-14変動）でも発火。位相固定説は不十分
- H-β（autosuspend）→ power/control=on で除外確定
- 供給/需要：APB=2048B(256frame@192k/S32)。engine消費1.536MB/s。aplay192k/S32供給=一致のはず。PipeWireが48k/S32(384KB/s)で掴むとpartial-APB 81%
- Rev45でpartial/empty計装：[FEED] partial（n<2048ゼロ埋め）、empty（n==0）。aud_udrはpartialを検出しない
- Linux対照機の注意：PipeWire＋GNOME設定サウンドパネルがcard1 pcmC1D0pをRUNNING保持（48k/S32）→aplayはbusy失敗。設定を閉じれば解放のはず。python（not python3）使用
- 取得ログ：%TEMP%\opencode\rev42d/c43/c44/c45*.txt（[AUD]1s＋[REG]3s＋ERRSNAP）
- 次手：(1)設定を閉じてaplay192kでpartial計測→供給ジッタ説確定 (2)SAMPLING_FREQ受入拒否（48k誤設定防止） (3)ポンプUSB起床復活＋高速tick (4)TRMでOUT_ERR定義確認
- 一時変更の戻し忘れ注意：cxd56_audio_dma.c（err分岐・snap・errcont・snap2）、uac2_audio_dma.c（dither・上限14・partial計数）、uac2_main.c（表示）

## 追記12（2026-09-06 マイルストーン達成：Rev63で12/12＋5分連続クリーン）
- 凍結バイナリ：nuttx.rev63-clean.spk（公開ツリーからは削除。後継 nuttx.final-1.0.spk を使用）
- TRM確定：err_I2S1O=フェッチ遅延のサンプル脱落。USBよりAudio IRQ優先で消滅
- 最終構成：always-feed＋two-tier reserve＋retry再始動＋ERR無視＋192k-only＋clamp-ACK＋SILENT_DIAG=1＋USB demote(0xA0)
- 実績：12連続2秒再生＋5分連続(S32/192k)全区間クリーン、hostエラーゼロ
- 既知の残件：起動直後underrun×2程度は出るが自動REVIVED（無音区間のみ、可聴影響なし）。slow相ERRは残存するも無害（dup=0確認）。
- PREPARE ETIMEDOUTはRev60-silent以降出ず（print系wedgeの副産物だった疑い濃厚）
- 次：目戻し計数確認→掃除（診断除去）→30分soak→cold boot×10→完成宣言

## 追記13（2026-09-07 Final-1.0 完成宣言）
- 凍結バイナリ：nuttx.final-1.0.spk（235520B）。動作FW Final-1.0そのもの
- 真犯人確定：USB-ISR内printf（completion/EP0 setup/FU/START表示）。逆アセで到達＋ガードを機械確認
- 修正：UAC2_TPRINTF（up_interrupt_contextでタスク限定）＋5247 hiresは無音化のため不採用（48k-modeのままが有音実績）
- 水晶再計算：49.152MHz系で192kは整数分周exact、誤差<140ppm（soak bound）。粗い誤差なし
- 実績：YouTube連続再生ずっと正常（10秒崩壊も解消）。aplay 1kHz正常
- 残件（無害・将来用）：SDK cxd56_audio_set_hires_outputは残置未使用、diag_drain/diag_tickは未使用、refill再採番で偽dup計数が出ることがある（表示のみ）
