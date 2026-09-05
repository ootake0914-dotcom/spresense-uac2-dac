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
2. 改善なしなら対話ターミナルで実行し出力全文を貼る：
   ```powershell
   python '\\wsl.localhost\Ubuntu\home\ootak\spresense_uac2_dac\tools\wasapi_exclusive_24bit.py'
   ```
3. 併せてCOM6（115200bps）のNSHログ（`SET_CONFIG`・`SET_CUR`・`Alt:1`の有無）を取得

## 追記4（12vs1未解決・次手確定）
- 新規インスタンス{35976339}でもsink=bridgeのみ。open試行後の再照会でも変化なし
- waveOutOpenは16/48k等すべてWAVERR_BADFORMAT(32)、KS交差も全不発
- FW側：記述子195B機械検証OK、全EP0応答ret>=0、HIGH確定
- 候補：(B) Linux/Mac対照があれば2分で確定 (A) USBPcapでwire真実取得 (C) FW側EP0計装
- Wireshark/USBPcapは未導入を確認済み

## 追記5（Linux対照＝ファーム無罪確定）
- yuhki機で `054c:0ced` high speed認識、ALSA card1+PCM0生成。パース完全成功
- よって不具合はWindows usbaudio2固有の検証規則

## 追記7（Rev15でNSH完全可視化）
- Rev15で停止せず生存。SET_CUR時計→SR:48000反映、Volume SET、RANGE74全応答
- しかしSET_INTERFACE Alt1/2が一度も来ない（`applying alt`なし）→ALSAがAlt選択前にEPIPE
- ALSA stream0/hwはAlt1(S16)/Alt2(S32)＋6レートを完全認識しているのに、である

## 追記10（自己破壊の自白とクリーン再試験）
- cap_serial.pyがDTRアサート＝毎回MCUリブートしていた。12↔1・番号上昇・EPIPE嵐の主因は自 rig
- cap_serial.pyを受動化（cap_pulse.pyのみ明示リブート用）。以後リブートなしで計測
- 12レンジ1回は「無擾乱・ settled 状態」の真値の可能性。4分静置後に再照会する
