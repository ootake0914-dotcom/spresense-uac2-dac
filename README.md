# Spresense 192kHz / 24bit ハイレゾ USB DAC (UAC2) フルスクラッチ・プロジェクト

Sony Spresense (CXD5602 + CXD5247) を **USB Audio Class 2.0 (UAC2) 準拠の 192kHz / 24bit ハイレゾ USB DAC** として動作させるフルスクラッチ・デバイスドライバおよびファームウェア開発プロジェクトです。

---

## 1. ターゲット仕様

| 項目 | 仕様 | 備考 |
|---|---|---|
| **USB 規格** | USB 2.0 High-Speed (480 Mbps) | CXD5602 内蔵 USB PHY |
| **オーディオ規格** | USB Audio Class 2.0 (UAC2) | Windows 10/11, macOS, Linux, iOS, Android 標準ドライバで動作 |
| **サンプリング周波数** | **192.0 kHz** (ハイレゾ) | 将来的に 44.1k / 48k / 96k にも追従 |
| **ビット深度** | **24-bit** (PCM) | 32-bit コンテナ (Subslot: 4 bytes) パディング |
| **チャンネル数** | 2ch (ステレオ L/R) | |
| **同期方式** | **Asynchronous (非同期)** | フィードバック・エンドポイントによるジッターレス周波数同期 |
| **DAC / 出力** | Sony CXD5247 (内蔵 DAC + ヘッドホンアンプ) | 3.5mm ステレオミニジャック出力 |

---

## 2. 帯域・パケット計算

- **サンプリングレート Fs**: 192,000 Hz
- **フレームあたりのデータ長**: 2 ch * 4 Bytes (32bit container) = 8 Bytes
- **総転送ビットレート**: 192,000 * 8 * 8 = 12.288 Mbps
- **USB High-Speed マイクロフレーム周期**: 125 us (毎秒 8,000 回)
- **1 マイクロフレームあたりのサンプル数**: 192,000 / 8,000 = 24 samples/uframe
- **1 マイクロフレームあたりのペイロードサイズ**: 24 * 8 = 192 Bytes
  ※ High-Speed Isochronous エンドポイント（最大パケットサイズ 1024 Bytes）に対して約 18.75% の帯域であり、十分なマージンを確保。

---

## 3. システムアーキテクチャ

`
+---------------------------------------------------------------+
|                       Host PC / Mac / Smartphone             |
|                 Standard USB Audio Class 2.0 Driver           |
+---------------------------------------------------------------+
                              |  USB 2.0 High-Speed (480 Mbps)
                              v
+---------------------------------------------------------------+
|                 Sony Spresense (CXD5602 Main Core)            |
|                                                               |
|  [USB Controller (cxd56_usbdev.c)]                            |
|       |                                                       |
|       +--> EP0 Control (UAC2 AudioControl / Descriptor)       |
|       +--> EP1 OUT Isochronous (192kHz/24bit Audio Stream)    |
|       +<-- EP2 IN Isochronous Feedback (Clock Ratio Ff)       |
|       |                                                       |
|  [UAC2 Class Driver (uac2_driver.c)]                          |
|       |                                                       |
|  [Jitter-Absorbing Lock-Free Ring Buffer]                     |
|       |                                                       |
|  [Audio Subsystem / CXD5247 DMA Controller]                   |
|       | (I2S / Internal Audio Bus @ 192kHz / 24bit)           |
+-------|-------------------------------------------------------+
        v
+---------------------------------------------------------------+
|                 Sony CXD5247 Audio PMIC / DAC                 |
|  - 49.152 MHz Master Clock (192kHz x 256)                     |
|  - High-Resolution 24-bit D/A Converter                       |
|  - Integrated Low-Noise Headphone Amplifier                   |
+---------------------------------------------------------------+
        |
        v
    [3.5mm Headphone Jack Output]
`

---

## 4. 開発フェーズ (ロードマップ)

### Phase 1: USB 認識と UAC2 ディスクリプタの確立 (現在地)
- NuttX usbdevclass_driver_s の骨格作成
- UAC2 ディスクリプタツリー（IAD, AC, AS, ClockSource, ClockSelector, Terminals）の構築
- ホスト PC に「Spresense 192kHz/24bit USB DAC」として認識させる（列挙完了）

### Phase 2: Isochronous ストリーミング受信
- EP OUT のアロケーションとパケット受信コールバックの実装
- 192 バイト/125us の Isochronous パケットの継続受信とデータ整合性検証

### Phase 3: オーディオサブシステム (CXD5247) 結合
- CXD5247 を 192kHz / 24bit モードで初期化
- USB 受信パケット -> リングバッファ -> オーディオ DMA へのブリッジング
- PC からのテストトーン（1kHz サイン波）のヘッドホン出力確認

### Phase 4: 非同期フィードバック同期 (Asynchronous Feedback)
- SOF カウンタと CXD5247 マスタークロックの微小周波数偏差測定
- 12.13 または 10.14 形式のフィードバックパケット生成（EP IN）
- 長時間再生でのゼロバッファアンダーラン/オーバーラン達成

---

## 5. ディレクトリ構成

- include/: UAC2 規格定義、ディスクリプタ構造体、リングバッファ定義
- src/: UAC2 デバイスドライバ、ディスクリプタ実体、DMA ブリッジ、アプリ
- docs/: 技術仕様書、クロック同期理論、パケットレイアウト
- tools/: Python ベースの検証・音源生成スクリプト
