# StackChan 部分構成デバイス対応 仕様書 v2

> **変更履歴**
> - v1 → v2 での主な変更点は末尾の「§17 変更差分サマリー」を参照。

---

## 1. 目的

- スタックちゃんの既存機能を維持したまま、接続されているデバイスだけを有効化する。
- サーボ、RGB LED、頭部タッチ、IMU、RTC、カメラ、NFC、IR などの一部が未接続・故障・省略されていても、ファームウェア全体が停止しないようにする。
- フル構成の StackChan では、既存の AI Agent、アニメーション、ESP-NOW リモコン、モバイルアプリ連携、OTA などの機能を従来どおり動作させる。

---

## 2. 対象範囲

- 対象コード
  - `firmware/main/hal`
  - `firmware/main/stackchan`
  - `firmware/main/apps`
  - `firmware/main/main.cpp`
- 対象ビルド環境
  - ESP-IDF ベースの現行ビルドフローを維持する。
  - `idf.py build` / `idf.py flash`
- 対象ハードウェア構成

| 構成 | Display | Mic/Spk | Servo | RGB | IMU | RTC | Camera | HeadTouch |
|---|---|---|---|---|---|---|---|---|
| CoreS3 単体 | ✓ | ✓ | - | - | - | - | - | - |
| CoreS3 + 顔表示のみ | ✓ | ✓ | - | - | - | - | - | - |
| CoreS3 + サーボのみ | ✓ | ✓ | ✓ | - | - | - | - | - |
| CoreS3 + サーボなし頭部のみ | ✓ | ✓ | - | ✓ | ✓ | ✓ | - | ✓ |
| CoreS3 + 一部センサーなし | ✓ | ✓ | ✓ | ✓ | optional | optional | optional | optional |
| フル構成 StackChan | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |

> 注: 「CoreS3 + サーボなし頭部のみ」の具体的なハードウェア接続は実機で確認すること。
> 頭部 PCB とサーボボードの物理的な依存関係があれば §2 の表を修正する。

---

## 3. 基本方針

- 起動時に optional デバイスを「必須」として初期化しない。
- 各 optional デバイスの有無は `Available / Unavailable` の 2 状態で管理する。
- **すべての capability 吸収は HAL / StackChan 層で行う。アプリ層は capability を意識しない。**
  - アプリは HAL / StackChan の API を呼ぶだけでよい。
  - optional デバイスが未接続なら、該当 API は no-op で返る。
  - アプリが明示的に `hasCapability()` を呼ぶ必要はない。
- 既存 API の呼び出しシグネチャを変更しない。
- フル構成時の動作・レスポンス速度・表示・通信プロトコルを変更しない。

### 3.1 必須デバイス

以下は起動に必須であり、初期化失敗は boot error 画面への遷移を引き起こす。

- NVS
- Display / LVGL
- Microphone
- Speaker
- 基本 system timer

### 3.2 任意デバイス

以下は未接続・初期化失敗でも boot が継続する。

- ServoYaw、ServoPitch
- RgbLed
- HeadTouch
- IMU
- RTC
- Camera
- IR (Tx / Rx)
- NFC

### 3.3 通信サービス

以下はハードウェアではなく通信 capability として扱う。初期化成功で `Available`。

- Wi-Fi、BLE、ESP-NOW、OTA

---

## 4. 現状の課題

- `Hal::init()` が複数のハードウェア初期化を一括実行しているため、部分構成時の責務分離が弱い。
- `firmware/main/hal` には `hal_servo.cpp`、`hal_io_expander.cpp`、`hal_head_touch.cpp`、`hal_imu.cpp`、`hal_rtc.cpp` など、デバイス依存処理が分散している。
- 一部モジュールは既に失敗時の無効化に近い実装がある。
  - IO Expander は初期化失敗時に `_io_expander.reset()` しており、RGB / サーボ電源制御は `_io_expander` がなければ return する。
  - IMU と RTC は `begin()` 失敗時にインスタンスを reset して return する。
- 一方で、サーボや頭部タッチは未接続時の検出・無効化・代替処理が不足している。
- `StackChan` の `motion()` が `_motion` を直接 dereference する構造の場合、サーボ未接続時にクラッシュまたは不正動作が発生しうる。
- **サーボ未検出時でも UART RX が有効なままになると、浮きラインのノイズが割り込みとして上がる可能性がある。**

---

## 5. 追加する概念

### 5.1 Device Capability

`DeviceCapability` enum を追加する。

```cpp
enum class DeviceCapability : uint8_t {
    Display,
    Microphone,
    Speaker,
    Camera,
    ServoYaw,
    ServoPitch,
    ServoPower,
    RgbLed,
    HeadTouch,
    Imu,
    Rtc,
    IrTx,
    IrRx,
    Nfc,
    EspNow,
    Ble,
    Wifi,
    Ota,
    _Count
};
```

各 capability の状態は `Available` / `Unavailable` の 2 値のみ。`Unavailable` は「未接続」「初期化失敗」「config による強制無効化」のすべてを包含する。詳細は起動ログで区別する。

### 5.2 Device Registry

```cpp
class DeviceRegistry {
public:
    void set(DeviceCapability cap, bool available);
    bool isAvailable(DeviceCapability cap) const;
    void printSummary() const;
private:
    // write-once at boot, read-only after initOptionalDevices() 完了
    std::bitset<static_cast<size_t>(DeviceCapability::_Count)> _available;
};
```

- `Hal` 内に `DeviceRegistry` インスタンスを保持する。
- 各 HAL 初期化関数は結果を registry に登録する。
- registry は **boot 中のみ書き込み可**。`initCommunicationServices()` 完了後は読み取り専用になる。
- `hasCapability()` は HAL / StackChan 内部からのみ呼ぶ。アプリ層は直接呼ばない。

### 5.3 Optional Device Driver

- 各デバイス初期化関数は `bool` または `esp_err_t` を返す。
- optional device の失敗で `ESP_ERROR_CHECK()` による停止を起こさない。
- 未接続時は `ESP_LOGW` を出して `Unavailable` を登録し、継続する。
- 必須デバイスの失敗のみ `ESP_LOGE` + boot error 画面へ遷移する。

---

## 6. HAL 改修作業

### 6.1 `Hal::init()` の分割

```
Hal::init()
├── initCoreSystem()          // NVS, board/bridge, NVS からの force_disabled 読み込み
├── initDisplay()             // Display / LVGL / タッチ (必須)
├── initAudio()               // Microphone / Speaker (必須)
├── initI2cBus()              // I2C バス初期化
├── initIoExpander()          // IO Expander → ServoPower / RgbLed capability 決定
├── initPowerRails()          // ServoPower が Available なら servo 5V on
├── initI2cSensors()          // IMU / RTC / HeadTouch
├── initServos()              // UART プローブ + per-axis 検出
├── initCommunicationServices()  // Wi-Fi / BLE / ESP-NOW / OTA
└── printCapabilitySummary()
```

`Hal::init()` は各段階の失敗を握りつぶさず、capability 状態として記録する。

### 6.2 `initCoreSystem()` — NVS force_disabled 読み込み

`initCoreSystem()` 内で NVS を初期化した直後に、force_disabled テーブルを読み込む。

```cpp
// NVS キー例: "cap_fd_servo_yaw" (bool)
// true の場合、該当 capability を Unavailable に事前登録し、
// 後続の検出ステップでその capability の probe を skip する。
```

対象キーのリストは §9.2 を参照。

### 6.3 IO Expander

- 既存の `_io_expander` null guard を正式仕様化する。
- IO Expander がない場合は以下を `Unavailable` に登録する。
  - `ServoPower`
  - `RgbLed`
- `setServoPowerEnabled()`、`setRgbColor()`、`refreshRgb()`、`showRgbColor()` は registry を確認して no-op で返る。

```
IO Expander init 成功 → ServoPower = Available, RgbLed = Available
IO Expander init 失敗 → ServoPower = Unavailable, RgbLed = Unavailable
```

### 6.4 サーボ

#### 6.4.1 検出フロー

`bool Hal::initServos()` で以下を行う。

1. `initI2cBus()` 完了後、`initPowerRails()` でサーボ電源を on にする（ServoPower が Available な場合のみ）。
2. UART driver を install して `SCSCL::ReadPos(id)` で各軸を probe する。
   - タイムアウト: **1 軸あたり最大 200 ms、再試行なし**（起動時間へのインパクトを最小化）。
   - Servo ID はパラメータ化する（デフォルト: yaw=1, pitch=2）。
3. 応答があった軸を `Available` に登録する。
4. **両軸とも Unavailable の場合: UART driver を uninstall し、TX/RX ピンを GPIO に戻して `GPIO_MODE_INPUT` + プルアップを設定する。** これにより浮きラインからの RX 割り込みを防止する。
5. 片軸のみ Available の場合: UART driver は install したままとし、未応答の軸を `Unavailable` として記録する。

#### 6.4.2 Motion クラスの改修

`Motion` クラスに `bool _hasYaw` / `bool _hasPitch` フラグを追加する。別クラスは作らない。

```cpp
class Motion {
public:
    // 既存 API は変更しない
    void moveYaw(float angle);
    void movePitch(float angle);
    float getCurrentYawAngle() const;
    float getCurrentPitchAngle() const;
    bool isMoving() const;
    void setTorqueEnabled(bool enabled);

    // 新規
    bool hasYaw() const { return _hasYaw; }
    bool hasPitch() const { return _hasPitch; }

private:
    bool _hasYaw = false;
    bool _hasPitch = false;
    // ... 既存フィールド
};
```

各メソッドの動作:

| メソッド | `_hasYaw / _hasPitch` が false の場合 |
|---|---|
| `moveYaw()` | no-op |
| `movePitch()` | no-op |
| `getCurrentYawAngle()` | 最後に命令した目標値を返す（デフォルト 0）|
| `getCurrentPitchAngle()` | 同上 |
| `isMoving()` | `false` |
| `setTorqueEnabled()` | no-op |

> `getCurrent*Angle()` は「最後の目標値」を返す。ゼロ固定では avatar アニメとの表示ズレが生じるため。

### 6.5 頭部タッチ

- `head_touch_init()` を `bool` 返却に変更する。
- SI12T 初期化・設定に失敗した場合は更新タスクを作成しない。
- 未接続時は `onHeadPetGesture` イベントを発火しない。

```
init 成功 → HeadTouch = Available
init 失敗 → HeadTouch = Unavailable, タスク不起動
```

### 6.6 IMU

- 既存の `begin()` 失敗時 reset / return 方針を維持し、capability 登録を追加する。
- IMU `Unavailable` 時は shake / pickup イベントを発火しない。
- 通常表情・通信・UI は継続する。

### 6.7 RTC

- 既存の `begin()` 失敗時 reset / return 方針を維持し、capability 登録を追加する。
- RTC `Unavailable` 時は system time のみで動作する。
- `syncRtcTimeToSystem()`、`syncSystemTimeToRtc()` は no-op 継続。
- SNTP が利用可能な場合は RTC なしでも時刻同期を許可する。
  - 注: no-RTC + no-SNTP の構成では、再起動のたびにシステム時刻がリセットされる。時刻保証は不要。

### 6.8 RGB LED

- IO Expander `Unavailable` 時、RGB 関連 API は no-op で返る。
- `updateNeonLightFromJson()` はクラッシュしない。
- BLE / WebSocket から RGB command を受けても ACK 相当の処理を維持する。

### 6.9 カメラ

- `hal_bridge::board_get_camera()` が null の場合は capture を実行しない。
- `Camera = Unavailable` の時に StartCameraStream 要求を受けた場合:
  - ストリーミング状態を有効化しない。
  - 既存の log / text response packet type を再利用して `camera unavailable` を通知する（新規 packet type を追加しない = §11 の互換性維持）。
  - avatar / call / text message / motion control は継続する。

### 6.10 スピーカー・マイク（必須デバイス）

Display / Mic / Speaker は起動必須。初期化失敗は boot error 画面へ遷移する。degraded 起動はしない。optional 化は対象外。

### 6.11 BLE / Wi-Fi / ESP-NOW

- `Available` の判定: **subsystem の init() が成功した時点**。接続済みかどうかは別 API (`isWifiConnected()` 等) で管理する。
- `startBleServer()`、`startAppConfigServer()`、`startEspNow()`、`startNetwork()` は二重初期化・初期化失敗に耐える。
- ESP-NOW アプリは motion / laser が `Unavailable` でも送受信・UI 表示が動く（HAL 層で吸収）。

---

## 7. StackChan 層の改修作業

### 7.1 Motion の安全化

- `StackChan::motion()` が常に有効な参照を返すようにする。`_motion` が未初期化の場合でも dereference しない構造にする。
- 別クラス（NullMotion / NullServo）は追加しない。`Motion` クラス内部の `_hasYaw` / `_hasPitch` フラグで軸別の有無を管理する。
- サーボ未検出時でも `Motion` インスタンスを生成し `StackChan` に attach する。フラグが false の軸への命令は `Motion` 内部で no-op になる。
- 便宜関数として以下を追加する:

```cpp
bool StackChan::hasYawServo() const;   // motion()._hasYaw と等価
bool StackChan::hasPitchServo() const; // motion()._hasPitch と等価
bool StackChan::hasMotion() const;     // hasYawServo() || hasPitchServo()
```

### 7.2 Avatar と Motion の独立性確保

- Avatar 表示は Motion 未接続でも必ず動く。
- Motion command JSON は、motion capability がない軸のみ no-op にする。

Dance sequence の degrade:

| Avatar | Servo | RGB | 動作 |
|---|---|---|---|
| あり | あり | あり | フル再生 |
| あり | なし | あり | 表情・口・目・テキスト + LED |
| あり | あり | なし | 表情・口・目・テキスト + モーション |
| あり | なし | なし | 表情・口・目・テキストのみ |
| なし | あり | - | モーションのみ |
| なし | なし | - | ログのみ（Display は必須なので UI toast も可） |

### 7.3 Modifier の安全化

- Modifier が avatar / motion / neon light を参照する前に、HAL registry の状態を確認する。
- capability がない場合は即 return し、modifier pool の更新周期を阻害しない。
- Modifier 内での capability チェックは HAL/StackChan 層の責務として行う（Modifier が app 層に相当する場合は HAL API に委ねる）。

---

## 8. Apps 層の改修方針

**アプリは capability を意識しない。**
- アプリは HAL / StackChan / LVGL の API を普通に呼ぶ。
- capability に基づく分岐は HAL / StackChan / Modifier が吸収済みなので、アプリ側に guard は不要。
- アプリの既存コードは、「optional デバイスが absent な場合に API が no-op で返る」だけで動作継続できることを確認する。

### 8.1 App Launcher

すべてのアプリを常に起動可能として表示する。対応デバイスがない場合、アプリは起動するが機能が degrade する。「アプリは何も知らない」ポリシーと完全一致し、フル構成時の表示と完全に同じになる。

- アプリ一覧に capability 条件を持たせない。
- disabled 表示・greyed-out は追加しない。
- 起動後に動かない機能があっても、それは HAL / StackChan 層の no-op として透過される。

### 8.2 App Avatar

既存コードの確認ポイント:

- Motion command 受信時 → `StackChan::motion().moveYaw()` etc. を呼ぶだけでよい。`Motion` 内部で no-op になる。
- Camera stream 開始要求時 → HAL が `Camera = Unavailable` なら no-op + text 通知を返す。アプリ側の分岐不要。
- 音声入出力は必須なので分岐不要。

### 8.3 App Dance

既存コードの確認ポイント:

- BLE から受信した motion data → `StackChan::motion().move*()` を呼ぶ。no-op 吸収済み。
- RGB data → `Hal::setRgbColor()` を呼ぶ。no-op 吸収済み。
- Avatar data → `StackChan::avatar()` を呼ぶ。Avatar は Display が必須なので必ず動作する。

### 8.4 App ESP-NOW Remote

既存コードの確認ポイント:

- receiver: motion command 受信 → `StackChan::motion().move*()` no-op 吸収済み。
- receiver: laser GPIO → `Hal::setLaserEnabled()` 内で capability 確認、no-op 吸収。
- sender: motion がない構成では motion payload の意味がないが、送信自体は継続してよい（receiver 側で吸収）。
- sender: motion がない場合に送信 UI を disabled 表示するかは §8.1 の方針に準じる。

### 8.5 App AI Agent

既存コードの確認ポイント:

- Display / Mic / Speaker は必須なので、Xiaozhi 起動前の音声チェックは不要。
- `requestXiaozhiStart()` フローは現行のまま維持。
- Xiaozhi 側の board 初期化失敗時はアプリ全体を落とさず、エラー表示画面へ遷移する（既存方針を維持）。

---

## 9. 設定ファイル・ビルド設定

### 9.1 Kconfig

`Kconfig.projbuild` に追加:

```kconfig
menu "StackChan Device Configuration"

    config STACKCHAN_AUTO_DETECT_DEVICES
        bool "Auto-detect optional devices at boot"
        default y

    config STACKCHAN_ENABLE_SERVO
        bool "Enable Servo (requires auto-detect or manual enable)"
        default y

    config STACKCHAN_ENABLE_HEAD_TOUCH
        bool "Enable Head Touch sensor"
        default y

    config STACKCHAN_ENABLE_RGB
        bool "Enable RGB LED"
        default y

    config STACKCHAN_ENABLE_IMU
        bool "Enable IMU"
        default y

    config STACKCHAN_ENABLE_RTC
        bool "Enable RTC"
        default y

    config STACKCHAN_ENABLE_CAMERA
        bool "Enable Camera"
        default y

    config STACKCHAN_ENABLE_IR
        bool "Enable IR (Tx/Rx)"
        default y

    config STACKCHAN_ENABLE_NFC
        bool "Enable NFC"
        default y

endmenu
```

仕様:

- デフォルトはフル構成互換。
- `AUTO_DETECT_DEVICES=y` の場合、実機検出を優先する。
- `ENABLE_XXX=n` の場合、`initCoreSystem()` 内で force_disabled として NVS に事前登録し、後続の検出をスキップする。

### 9.2 NVS force_disabled

NVS に以下のキーで `uint8_t 1` を書くと、該当 capability の検出をスキップして `Unavailable` にする。

| NVS キー | 対象 capability |
|---|---|
| `cap_fd_servo_yaw` | ServoYaw |
| `cap_fd_servo_pitch` | ServoPitch |
| `cap_fd_rgb` | RgbLed |
| `cap_fd_head_touch` | HeadTouch |
| `cap_fd_imu` | Imu |
| `cap_fd_rtc` | Rtc |
| `cap_fd_camera` | Camera |
| `cap_fd_ir_tx` | IrTx |
| `cap_fd_ir_rx` | IrRx |
| `cap_fd_nfc` | Nfc |

- factory reset 時は force_disabled エントリも削除する。
- `Available` への強制書き換えはサポートしない（壊れたデバイスを動作させようとするリスクを避ける）。
- NVS schema が変更された場合は `cap_schema_ver` キーでバージョン管理し、OTA 後に不整合が出ないようにする。

### 9.3 ログ

起動時に以下を出力する。

```
I [StackChan] Firmware version: x.y.z
I [StackChan] Board type: CoreS3
I [StackChan] === Device Capability Summary ===
I [StackChan]   ServoYaw     : Available
I [StackChan]   ServoPitch   : Unavailable (not detected, UART disabled)
I [StackChan]   RgbLed       : Unavailable (IO expander not found)
I [StackChan]   Camera       : Available
I [StackChan]   Imu          : Available
W [StackChan]   Rtc          : Unavailable (PCF8563 begin failed)
I [StackChan]   HeadTouch    : Unavailable (disabled by config)
I [StackChan] ===================================
```

- `Unavailable` の理由を括弧内に書く（「not detected」「init failed」「disabled by config」を区別）。
- `Available` の capability は `I` (INFO)、`Unavailable` は `W` (WARN) レベルで出す。

---

## 10. エラー処理仕様

- optional device の初期化失敗で `abort()`、`assert()`、`ESP_ERROR_CHECK()` による停止を発生させない。
- required device の失敗のみ boot error 画面へ遷移する。
- optional device 失敗時の UI 表示
  - 初回起動時: 小さな warning toast（任意実装）。
  - 設定画面: device status 一覧（§8 App Setup が担当）。
  - 通常操作中: 使えない機能を使おうとした時のみ通知。
- 通信プロトコル上は、既存 command を拒否せず、実行不能な部分だけ skip する。
- **optional device への retry は行わない。** 起動時の検出が唯一の判定機会であり、capability は boot 後に変化しない。

---

## 11. 互換性要件

- フル構成 StackChan では既存動作と同一にする。
- 既存の BLE motion / avatar / rgb JSON を変更しない。
- 既存の WebSocket packet type を変更しない（`camera unavailable` 通知は既存 log / text 型を再利用）。
- ESP-NOW の既存 payload 形式を変更しない。
- 既存アプリの起動順・表示名・アイコン・テーマ色を変更しない（フル構成時）。
- 既存の OTA / app center / setup flow を壊さない。

---

## 12. テスト仕様

### 12.1 構成別テスト

§2 の構成表に対応させる。

| テスト構成 | 合格条件 |
|---|---|
| フル構成 | すべての現行機能が動くこと |
| CoreS3 単体 | 起動する / UI が表示される / サーボ・RGB・頭部タッチなしでクラッシュしない / UART RX 割り込みが発生しない |
| サーボなし | Avatar / AI Agent / BLE / WebSocket が動く / motion command 受信でクラッシュしない |
| RGB なし | Dance / BLE RGB command でクラッシュしない |
| IMU なし | shake event が発生しないだけで通常動作する |
| RTC なし | SNTP または system time で継続する |
| Camera なし | WebSocket avatar は動作し、camera stream のみ unavailable になる |
| Head touch なし | touch event が発生しないだけで通常動作する |

### 12.2 回帰テスト（フル構成）

- 起動時間: v1 比 **+20% 以内**（主にサーボ検出 timeout が増加するが両軸応答あれば無変化）
- Avatar 表示・サーボ motion・Dance・ESP-NOW remote・BLE app config・WebSocket video/call/text/dance・OTA が従来どおり動作する

### 12.3 異常系テスト

- I2C デバイス未応答
- UART サーボ未応答（UART RX 割り込みが上がらないことを確認）
- Wi-Fi 未接続
- WebSocket 接続失敗
- BLE 接続断
- カメラ null
- RGB LED 更新要求のみ連続受信
- motion command 連続受信中に servo 検出なし
- force_disabled NVS キー設定後の再起動

---

## 13. 実装順序

1. `DeviceCapability` / `DeviceRegistry` を追加する。
2. `Hal::init()` を §6.1 の段階的初期化に分割する。
3. IO Expander / IMU / RTC の既存 graceful failure を capability 登録に接続する。
4. **サーボ検出・UART 制御・Motion フラグ化** を一体で実装する（§6.4 + §7.1）。
   - `Motion` に `_hasYaw` / `_hasPitch` 追加
   - `initServos()` で probe → flag 設定
   - 両軸 Unavailable 時の UART uninstall + GPIO pullup
5. 頭部タッチ初期化を失敗許容にする（§6.5）。
6. Camera / laser の capability guard を HAL に追加する（§6.9）。
7. `StackChan::hasMotion()`、`hasYawServo()`、`hasPitchServo()` を追加する。
8. App 層の確認: HAL/StackChan no-op で自然に degrade できているか確認し、クラッシュパスがあれば修正する（guard を追加するのではなく HAL 側の no-op を補完する）。
9. App Launcher / Setup に device status 表示を追加する（起動ログと設定画面用）。
10. Kconfig / NVS force_disabled を追加する。
11. フル構成回帰テスト。
12. 部分構成テスト。

---

## 14. 完了条件

- 未接続 optional device があっても boot が継続する。
- 使えないデバイスの処理だけが無効化される。
- フル構成の既存機能・レスポンスが変わらない。
- servo / rgb / camera / touch / imu / rtc の各 failure path がテストされている。
- サーボ未検出時に UART RX 割り込みが発生しない。
- 起動ログから capability 状態と Unavailable 理由を確認できる。
- 設定画面から capability 一覧を確認できる。
- 既存 BLE / WebSocket / ESP-NOW protocol に破壊的変更がない。
- `idf.py build` が通る。
- フル構成実機と最低 2 種類以上の部分構成実機で動作確認済み。

---


## 15. 参考 URL

### 15.1 リポジトリ全体

- https://github.com/m5stack/StackChan
  - StackChan 本体リポジトリ。firmware、remote、mobile app、server を含む。

- https://github.com/m5stack/StackChan/tree/main/firmware
  - ESP-IDF ベースのファームウェア実装。

- https://github.com/m5stack/StackChan/blob/main/firmware/README.md
  - ファームウェアのビルド・書き込み手順確認用。

### 15.2 ハードウェア仕様・製品仕様

- https://docs.m5stack.com/ja/StackChan/
  - StackChan の公式製品ページ。CoreS3、Wi-Fi / BLE、カメラ、IMU、スピーカー、マイク、サーボ、RGB LED、IR、頭部タッチ、NFC、RTC などの搭載デバイス確認用。

- https://docs.m5stack.com/en/StackChan/
  - 英語版の公式製品ページ。

- https://github.com/m5stack/StackChan-BSP
  - StackChan BSP。ボード定義・周辺機器初期化の確認用。

### 15.3 HAL 層

- https://github.com/m5stack/StackChan/tree/main/firmware/main/hal
  - HAL 層全体。デバイス初期化・通信・表示・センサー・サーボなどの確認対象。

- https://github.com/m5stack/StackChan/blob/main/firmware/main/hal/hal.cpp
  - `Hal::init()`、NVS 初期化、Xiaozhi 起動、LVGL 入力、StackChan 更新タスクなどの確認対象。

- https://github.com/m5stack/StackChan/blob/main/firmware/main/hal/hal.h
  - HAL API 定義。`DeviceCapability` / `DeviceRegistry` を追加する候補。

- https://github.com/m5stack/StackChan/blob/main/firmware/main/hal/hal_io_expander.cpp
  - IO Expander、サーボ電源、RGB LED 制御。graceful failure 化の参考。

- https://github.com/m5stack/StackChan/blob/main/firmware/main/hal/hal_servo.cpp
  - SCS サーボ、yaw / pitch サーボ、Motion attach 処理。サーボ個別検出、`NullMotion`、`PartialMotion` 実装の主対象。

- https://github.com/m5stack/StackChan/blob/main/firmware/main/hal/hal_head_touch.cpp
  - SI12T 頭部タッチ、ジェスチャ認識、更新タスク。未接続時にタスクを起動しないようにする対象。

- https://github.com/m5stack/StackChan/blob/main/firmware/main/hal/hal_imu.cpp
  - IMU 初期化・センサーイベント確認用。

- https://github.com/m5stack/StackChan/blob/main/firmware/main/hal/hal_rtc.cpp
  - RTC 初期化・時刻同期確認用。

- https://github.com/m5stack/StackChan/blob/main/firmware/main/hal/hal_ws_avatar.cpp
  - WebSocket avatar / camera stream / remote control の確認用。

### 15.4 StackChan コア層

- https://github.com/m5stack/StackChan/tree/main/firmware/main/stackchan
  - Avatar、Motion、Modifier、NeonLight、JSON 更新処理などのコア層。

- https://github.com/m5stack/StackChan/blob/main/firmware/main/stackchan/stackchan.h
  - `StackChan::motion()`、`avatar()`、`hasAvatar()`、`updateMotionFromJson()` などの確認対象。`hasMotion()` や `NullMotion` の導入候補。

- https://github.com/m5stack/StackChan/tree/main/firmware/main/stackchan/motion
  - Motion / Servo 抽象化。`NullMotion`、`PartialMotion`、軸別 capability の追加候補。

- https://github.com/m5stack/StackChan/tree/main/firmware/main/stackchan/avatar
  - Avatar 表示。サーボなし構成でも表示を維持するための確認対象。

- https://github.com/m5stack/StackChan/tree/main/firmware/main/stackchan/modifiers
  - Modifier 処理。Avatar / Motion / RGB 依存箇所の guard 追加対象。

- https://github.com/m5stack/StackChan/tree/main/firmware/main/stackchan/addons/neon_light
  - RGB LED / NeonLight 表現の確認対象。

### 15.5 Apps 層

- https://github.com/m5stack/StackChan/tree/main/firmware/main/apps
  - アプリ層全体。`app_ai_agent`、`app_avatar`、`app_dance`、`app_espnow_ctrl`、`app_launcher`、`app_setup` などが含まれる。

- https://github.com/m5stack/StackChan/tree/main/firmware/main/apps/app_avatar
  - Avatar / WebSocket / camera / motion command の capability guard 対象。

- https://github.com/m5stack/StackChan/blob/main/firmware/main/apps/app_avatar/app_avatar.cpp
  - AppAvatar の実装確認用。

- https://github.com/m5stack/StackChan/tree/main/firmware/main/apps/app_dance
  - Dance アプリ。Avatar / Motion / RGB の部分実行対応対象。

- https://github.com/m5stack/StackChan/tree/main/firmware/main/apps/app_espnow_ctrl
  - ESP-NOW リモコン。motion / laser / sender / receiver の部分構成対応対象。

- https://github.com/m5stack/StackChan/tree/main/firmware/main/apps/app_ai_agent
  - AI Agent。音声入出力・ネットワーク・Xiaozhi 起動依存の整理対象。

- https://github.com/m5stack/StackChan/tree/main/firmware/main/apps/app_launcher
  - アプリ一覧。capability による disabled 表示・制限付き起動の追加対象。

- https://github.com/m5stack/StackChan/tree/main/firmware/main/apps/app_setup
  - 設定画面。Device Status / capability 一覧表示の追加候補。

### 15.6 外部仕様・ビルド環境

- https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/
  - ESP-IDF 公式ドキュメント。NVS、Kconfig、Wi-Fi、BLE、ESP-NOW、OTA、FreeRTOS task、I2C / UART driver の確認用。

- https://docs.lvgl.io/
  - LVGL 公式ドキュメント。display / touch input / UI disabled 表示の確認用。

- https://github.com/m5stack/M5Unified
  - M5Stack デバイス抽象化ライブラリ。CoreS3 周辺機器初期化の参考。

- https://github.com/m5stack/M5GFX
  - M5Stack グラフィックライブラリ。表示周りの参考。


---

## 16. 補足メモ

- 「未接続デバイスをエラー扱いしない」が基本方針。
- capability 吸収は HAL / StackChan のみで完結する。アプリは何も知らない。
- `Available` / `Unavailable` の 2 状態だけ。詳細は起動ログで区別する。
- サーボ未検出時は UART driver を uninstall してピンを GPIO pullup に戻す（RX 割り込み防止）。
- App Launcher はすべてのアプリを常に表示する（disabled 表示なし）。

