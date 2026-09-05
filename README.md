# Spresense 192kHz / 24bit ハイレゾ USB DAC (UAC2) プロジェクト

Sony Spresense (CXD5602 + CXD5247) を **USB Audio Class 2.0 (UAC2) 準拠の 192kHz / 24bit ハイレゾ USB DAC** として動作させるフルスクラッチ・デバイスドライバおよびファームウェア開発プロジェクトです。

---

## 1. ターゲット仕様

| 項目 | 仕様 | 備考 |
|---|---|---|
| **USB 規格** | USB 2.0 High-Speed (480 Mbps) | CXD5602 内蔵 USB PHY |
| **オーディオ規格** | USB Audio Class 2.0 (UAC2) | **Linux (ALSA), Raspberry Pi, Volumio, Android 等で完全動作** |
| **ストリーミング構成** | **Alt 0 単一ストリーミング** | CXD5602 シリコン制約に準拠した独自アーキテクチャ |
| **サンプリング周波数** | **192.0 kHz** (ハイレゾ) | 48.0 kHz / 192.0 kHz 対応 |
| **ビット深度** | **24-bit** (PCM) | 32-bit コンテナ (Subslot: 4 bytes) パディング |
| **チャンネル数** | 2ch (ステレオ L/R) | |
| **同期方式** | **Adaptive (適応型)** / **Asynchronous** | ホスト主導レート同期 / ジッター吸収リングバッファ |
| **DAC / 出力** | Sony CXD5247 (内蔵 DAC + S-Master アンプ) | 3.5mm ステレオミニジャック出力 |

---

## 2. 【最重要】CXD5602 ハードウェア (UDC IP) の仕様と制約

実機での厳密な USB バスモニタ解析（`usbmon`）および **CXD5602 User Manual 第3.18節「USB」（p.1118〜1124）** の精査により、コントローラ IP（Synopsys DesignWare `DWC_d20ahb`）に関する決定的な物理仕様が判明・実証されました。

### 2.1 シリコン固定パラメータ（User Manual p.1122〜1123）
CXD5602 のシリコン製造時に焼き付けられた合成パラメータ（Configuration Parameters）は以下の通りです：

1. **Alternate Setting 数の上限値（p.1123 Table USB-46）**:
   - `Max Alternate Setting in Interfaces 0..14 Configuration 1` = **`1 (every case)`**
   - 各インターフェースがサポートする Alternate Setting の総数は **「1 個（Alt 0 のみ）」** にハードコードされています。
2. **ハードウェアによる自律 STALL 動作仕様（p.1122 Table USB-45）**:
   - *"The UDC20-AHB Subsystem issues a STALL handshake for command interfaces [and settings] not supported in Configuration 1."*
   - サポート範囲外（Alt > 0）の `SET_INTERFACE` リクエストを受信した場合、UDC ハードウェアプロトコルエンジンは **CPU（NuttX / DCD）に割り込みを一切上げることなく、137〜250 µs で自律的に STALL（-32 / EPIPE）を返します。**

### 2.2 OS ごとの対応と設計方針
- **Windows 10/11 (`usbaudio2.sys`) の物理的不可能性**:
  - Microsoft 公式仕様上、AS（Audio Streaming）インターフェースは「帯域ゼロの Alt 0 から始まり、ストリーミング時に必ず Alt > 0 へ切り替える（Alt 0 単独ストリーミングは非対応）」と定められています。
  - そのため、Alt > 0 をハードウェアが自律 STALL する Spresense では、Windows 標準ドライバで再生ピンを開くことが物理的に不可能です。
- **Linux (ALSA / `snd-usb-audio`) での完全対応（本プロジェクトの主軸）**:
  - Linux ALSA は **「Alt 0 単一ストリーミング（帯域ゼロの Alt 0 を作らず、Alt 0 に直接 Isochronous EP を配置する構成）」** を標準で完全にサポートしています。
  - 本プロジェクトでは `UAC2_SINGLE_ALT0_STREAMING = 1` を採用し、Linux / Raspberry Pi 等のオーディオトランスポート環境で最速・最高音質の 192kHz/24bit DAC を実現します。

---

## 3. 帯域・パケット計算

- **サンプリングレート Fs**: 192,000 Hz
- **フレームあたりのデータ長**: 2 ch * 4 Bytes (32bit container) = 8 Bytes
- **総転送ビットレート**: 192,000 * 8 * 8 = 12.288 Mbps
- **USB High-Speed マイクロフレーム周期**: 125 us (毎秒 8,000 回)
- **1 マイクロフレームあたりのサンプル数**: 192,000 / 8,000 = 24 samples/uframe
- **1 マイクロフレームあたりのペイロードサイズ**: 24 * 8 = 192 Bytes
- **wMaxPacketSize**: 200 Bytes（ジッター・クロックドリフト耐性マージンを含む）

---

## 4. システムアーキテクチャ

```
+---------------------------------------------------------------+
|                       Host PC / Raspberry Pi                  |
|                 Linux ALSA (snd-usb-audio driver)             |
+---------------------------------------------------------------+
                              |  USB 2.0 High-Speed (480 Mbps)
                              |  SET_INTERFACE (Interface 1, Alt 0) -> ACK
                              v
+---------------------------------------------------------------+
|                 Sony Spresense (CXD5602 Main Core)            |
|                                                               |
|  [USB Controller (cxd56_usbdev.c)]                            |
|       |                                                       |
|       +--> EP0 Control (UAC2 AudioControl & ClockSource)      |
|       +--> EP2 OUT Isochronous (192kHz/24bit Audio Stream)    |
|       |                                                       |
|  [UAC2 Class Driver (uac2_driver.c)]                          |
|       |                                                       |
|  [Jitter-Absorbing Lock-Free Ring Buffer (uac2_ringbuf.c)]    |
|       |                                                       |
|  [Audio Subsystem / CXD5247 DMA Bridge (uac2_audio_dma.c)]    |
|       | (/dev/pcm0 - 192kHz / 24-in-32bit Slot)               |
+-------|-------------------------------------------------------+
        v
+---------------------------------------------------------------+
|                 Sony CXD5247 Audio PMIC / DAC                 |
|  - 24.576 / 49.152 MHz Master Clock                           |
|  - S-Master Digital Audio Amplifier / Modulator               |
|  - Integrated Low-Noise Headphone Amplifier                   |
+---------------------------------------------------------------+
        |
        v
    [3.5mm Headphone Jack Output]
```

---

## 5. 開発フェーズ (ロードマップ)

### Phase 1: USB 認識と UAC2 ディスクリプタの確立【完了】
- NuttX `usbdevclass_driver_s` の骨格作成
- UAC2 ディスクリプタツリー（IAD, AC, AS, ClockSource, Terminals）の構築
- Linux ALSA (`snd-usb-audio`) での PCM デバイス完全認識（カード番号付与、192kHz/24bit 認識）

### Phase 2: シリコン制約の解明とハードウェア検証【完了】
- Alt > 0 での自律 STALL 現象を `usbmon` 生ログにて厳密実証（137〜250 µs）
- CXD5602 User Manual（p.1122〜1123 Table USB-45/46）の精査により、シリコンの「Alternate Setting 0 のみ対応」仕様を完全証明
- `UAC2_SINGLE_ALT0_STREAMING = 1` による Alt 0 単一ストリーミング構成を確定

### Phase 3: Isochronous ストリーミング受信とリングバッファ供給【完了】
- EP2 OUT（Adaptive Isochronous）のアロケーションとパケット受信コールバックの実装
- 192 バイト/125us の Isochronous パケットの継続受信とロックフリーリングバッファへの蓄積

### Phase 4: オーディオサブシステム (CXD5247 / S-Master) 結合【現在地】
- CXD5247 S-Master DAC（`/dev/pcm0`）を 192kHz / 24bit モードで起動
- リングバッファからオーディオ DMA への実音声データ転送パイプラインの結合
- Linux ホストからの `aplay`（1kHz ハイレゾ音源）によるヘッドホン実音出力確認

---

## 6. ディレクトリ構成

- `include/`: UAC2 規格定義、ディスクリプタ構造体、リングバッファ定義
- `src/`: UAC2 デバイスドライバ、ディスクリプタ実体、DMA ブリッジ、アプリ
- `docs/`: 技術仕様書、クロック同期理論、再起動後ログ
- `tools/`: Python ベースの検証・音源生成スクリプト、ログ解析ツール
- `test_logs/`: ハードウェア自律 STALL 挙動を実証した生ログ（`usbmon`, シリアル）
