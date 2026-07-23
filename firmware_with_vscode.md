# VS Code でのファームウェアビルドと書き込み手順

---

## 必要なもの

- [Visual Studio Code](https://code.visualstudio.com/)
- [ESP-IDF v5.5.4](https://dl.espressif.com/dl/esp-idf/) （インストーラー版推奨）
- [ESP-IDF VS Code Extension](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension)
- Python 3.x
- Git
- データ転送対応 USB-C ケーブル

---

## Step 1 — ESP-IDF のインストール

1. [ESP-IDF v5.5.4 Windows インストーラー](https://dl.espressif.com/dl/esp-idf/) をダウンロードして実行
2. インストール先はデフォルト（`C:\Espressif`）のまま進める
3. インストール完了後、スタートメニューに **ESP-IDF 5.5.4 PowerShell** が追加される

---

## Step 2 — VS Code 拡張のインストールと設定

1. VS Code を起動し、Extensions（`Ctrl+Shift+X`）で `esp-idf` を検索
2. **ESP-IDF** 拡張（Espressif Systems 製）をインストール
3. コマンドパレット（`Ctrl+Shift+P`）から `ESP-IDF: Configure ESP-IDF Extension` を実行
4. **Use existing setup** を選択し、IDF パスに `C:\Espressif\frameworks\esp-idf-v5.5.4` を指定

---

## Step 3 — リポジトリのクローン

通常の PowerShell またはターミナルで実行します：

```powershell
git clone --branch feature/claude_devel https://github.com/nyxrobotics/StackChan.git
```

---

## Step 4 — VS Code でプロジェクトを開く

```
ファイル → フォルダーを開く → StackChan\firmware を選択
```

---

## Step 5 — ESP-IDF ターミナルを開く

コマンドパレット（`Ctrl+Shift+P`）から：

```
ESP-IDF: Open ESP-IDF Terminal
```

ESP-IDF の環境変数が設定済みのターミナルが開きます。以降のコマンドはすべてこのターミナルで実行してください。

---

## Step 6 — 依存リポジトリの取得

ESP-IDF ターミナルで実行します：

```powershell
python fetch_repos.py
```

`xiaozhi-esp32` などのサブリポジトリとパッチが自動で適用されます。

---

## Step 7 — ターゲットの設定

コマンドパレット（`Ctrl+Shift+P`）から：

```
ESP-IDF: Set Espressif Device Target
```

**esp32s3** を選択します。

選択後に OpenOCD インターフェースの選択肢が3つ表示されます。**ESP32-S3 chip (via builtin USB-JTAG)** を選んでください。

---

## Step 8 — ビルド

コマンドパレット（`Ctrl+Shift+P`）から：

```
ESP-IDF: Build your Project
```

または VS Code 下部ステータスバーの 🔨 アイコンをクリックします。

初回ビルドは数分かかります。`Project build complete.` が表示されれば成功です。

> **ビルドエラーが出た場合**  
> Step 6 の `fetch_repos.py` が完了しているか確認してください。

---

## Step 9 — 書き込み（Flash）

1. CoreS3 を USB-C ケーブルで PC に接続する
2. コマンドパレット（`Ctrl+Shift+P`）から：

```
ESP-IDF: Select Port to Use
```

デバイスマネージャーで確認した COM ポートを選択します。

3. コマンドパレット（`Ctrl+Shift+P`）から：

```
ESP-IDF: Flash your Project
```

または VS Code 下部ステータスバーの ⚡ アイコンをクリックします。

書き込みモード（UART / JTAG）は **UART** を選択してください。

---

## Step 10 — シリアルモニター（任意）

ログを確認したい場合はコマンドパレット（`Ctrl+Shift+P`）から：

```
ESP-IDF: Monitor your Device
```

`Ctrl+]` でモニターを終了します。

---

## ビルド・書き込み・モニターを一括実行

コマンドパレット（`Ctrl+Shift+P`）から：

```
ESP-IDF: Build, Flash and Start a Monitor on your Device
```

---

## よくある問題

**`fetch_repos.py` でエラーが出る**  
Git が PATH に通っているか確認してください。`git --version` で確認できます。

**書き込み時に `Failed to connect` が出る**  
CoreS3 の電源ボタンを押しながら USB を抜き差しして、書き込みモードで起動してみてください。

**ビルド時に `idf.py` が見つからないと言われる**  
VS Code の拡張設定で IDF パスが正しく設定されているか確認してください。
