#!/usr/bin/env bash
# =============================================================================
# provision_module_llm.sh
# M5Stack Module LLM セットアップスクリプト
#
# 使い方:
#   1. Module LLM の USB-C ポートを PC に接続する
#   2. ./scripts/provision_module_llm.sh
#
# 必要なもの:
#   - adb (Android Debug Bridge)
#       macOS  : brew install android-platform-tools
#       Ubuntu : sudo apt install adb
#       Windows: https://developer.android.com/tools/releases/platform-tools
#   - インターネット接続 (Module LLM 側で apt するため)
# =============================================================================

set -euo pipefail

# --- カラー出力 ---------------------------------------------------------------
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()    { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()      { echo -e "${GREEN}[ OK ]${NC}  $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error()   { echo -e "${RED}[ERR ]${NC}  $*"; }
die()     { error "$*"; exit 1; }

# --- インストールするパッケージ一覧 -------------------------------------------
PACKAGES_BASE=(
    lib-llm
    llm-sys
    llm-audio
    llm-whisper
    llm-llm
    llm-melotts
)
PACKAGES_MODEL=(
    llm-model-whisper-tiny
    llm-model-qwen3-0.6b-ax630c
    llm-model-melotts-ja-jp
)

# --- ADB ラッパー (エラー時に分かりやすいメッセージを出す) -------------------
ADB_RETRY_COUNT=10
ADB_RETRY_DELAY=2

adb_shell() {
    local cmd="$*"
    adb shell "export DEBIAN_FRONTEND=noninteractive APT_LISTCHANGES_FRONTEND=none; ${cmd}" 2>&1
}

# =============================================================================
# 0. 事前確認
# =============================================================================
echo ""
echo "======================================================"
echo " M5Stack Module LLM プロビジョニングスクリプト"
echo "======================================================"
echo ""

# adb がインストールされているか
command -v adb >/dev/null 2>&1 || die "adb が見つかりません。インストールしてから再実行してください。"

# デバイスが繋がっているか
info "ADB デバイスを確認中..."
adb kill-server >/dev/null 2>&1 || true
adb start-server >/dev/null 2>&1 || true

ADB_DEVICES=""
for attempt in $(seq 1 "$ADB_RETRY_COUNT"); do
    ADB_DEVICES=$(adb devices | grep -v "List of" | grep -v "^$" | grep "device$" || true)
    if [ -n "$ADB_DEVICES" ]; then
        break
    fi
    if [ "$attempt" -lt "$ADB_RETRY_COUNT" ]; then
        info "  接続待ち (${attempt}/${ADB_RETRY_COUNT})..."
        sleep "$ADB_RETRY_DELAY"
    fi
done

if [ -z "$ADB_DEVICES" ]; then
    echo ""
    error "Module LLM が ADB で認識されていません。"
    echo ""
    echo "  チェックリスト:"
    echo "  1. Module LLM の USB-C ポート (Type-C) に接続していますか？"
    echo "     (CoreS3 側の USB-C ではありません)"
    echo "  2. データ転送対応のケーブルを使っていますか？"
    echo "  3. Linux/macOS の場合: udev ルールや権限を確認してください"
    echo "     sudo adb kill-server && sudo adb start-server"
    echo ""
    exit 1
fi
ok "Module LLM を検出しました"

# =============================================================================
# 1. ディスク容量確認
# =============================================================================
info "ディスク空き容量を確認中..."
DISK_INFO=$(adb_shell df -h / | tail -1)
echo "  $DISK_INFO"

# 空き容量を MB で取得 (busybox df の出力形式に合わせた簡易チェック)
AVAIL_KB=$(adb_shell df / | tail -1 | awk '{print $4}' | tr -d 'G\r\n' || echo "0")
# 数値でなければスキップ
if [[ "$AVAIL_KB" =~ ^[0-9]+$ ]] && [ "$AVAIL_KB" -lt 1048576 ]; then
    warn "空き容量が 1GB 未満です (${AVAIL_KB}KB)。モデルのインストールに失敗する可能性があります。"
    read -rp "  続行しますか？ [y/N]: " CONT
    [[ "${CONT,,}" == "y" ]] || die "中断しました。"
fi

# =============================================================================
# 2. apt ソース登録 (初回のみ)
# =============================================================================
info "M5Stack apt リポジトリを確認中..."

SOURCES_FILE="/etc/apt/sources.list.d/StackFlow.list"
ALREADY_REGISTERED=$(adb_shell "[ -f $SOURCES_FILE ] && echo yes || echo no")
ALREADY_REGISTERED=$(echo "$ALREADY_REGISTERED" | tr -d '\r\n')

if [ "$ALREADY_REGISTERED" = "yes" ]; then
    ok "apt ソースは登録済みです"
else
    info "GPG キーを取得中..."
    adb_shell "wget -qO /etc/apt/keyrings/StackFlow.gpg \
        https://repo.llm.m5stack.com/m5stack-apt-repo/key/StackFlow.gpg" \
        || die "GPG キーの取得に失敗しました (インターネット接続を確認してください)"

    info "apt ソースリストを登録中..."
    adb_shell "echo 'deb [arch=arm64 signed-by=/etc/apt/keyrings/StackFlow.gpg] \
        https://repo.llm.m5stack.com/m5stack-apt-repo jammy ax630c' \
        > $SOURCES_FILE" \
        || die "ソースリストの書き込みに失敗しました"

    ok "apt ソースを登録しました"
fi

# =============================================================================
# 3. apt update
# =============================================================================
info "apt update 中... (時間がかかることがあります)"
adb_shell "apt-get update -y -qq" || die "apt update に失敗しました"
ok "apt update 完了"

# =============================================================================
# 4. パッケージインストール
# =============================================================================

install_package() {
    local pkg="$1"
    # すでにインストール済みか確認
    local STATUS
    STATUS=$(adb_shell "dpkg -s '$pkg' 2>/dev/null | grep '^Status:'" | tr -d '\r\n')
    if echo "$STATUS" | grep -q "installed"; then
        ok "  $pkg (スキップ: インストール済み)"
        return 0
    fi

    info "  $pkg をインストール中..."
    if adb_shell "apt-get install -y -qq '$pkg'"; then
        ok "  $pkg"
    else
        error "  $pkg のインストールに失敗しました"
        return 1
    fi
}

echo ""
info "=== ベースライブラリ・機能モジュール ==="
FAILED_BASE=()
for pkg in "${PACKAGES_BASE[@]}"; do
    install_package "$pkg" || FAILED_BASE+=("$pkg")
done

echo ""
info "=== モデルファイル (大容量・時間がかかります) ==="
FAILED_MODEL=()
for pkg in "${PACKAGES_MODEL[@]}"; do
    install_package "$pkg" || FAILED_MODEL+=("$pkg")
done

# =============================================================================
# 5. インストール結果サマリー
# =============================================================================
echo ""
echo "======================================================"
echo " インストール結果"
echo "======================================================"

ALL_PKGS=("${PACKAGES_BASE[@]}" "${PACKAGES_MODEL[@]}")
FAILED_ALL=("${FAILED_BASE[@]}" "${FAILED_MODEL[@]}")

for pkg in "${ALL_PKGS[@]}"; do
    STATUS=$(adb_shell "dpkg -s '$pkg' 2>/dev/null | grep '^Status:'" | tr -d '\r\n')
    if echo "$STATUS" | grep -q "installed"; then
        echo -e "  ${GREEN}✓${NC} $pkg"
    else
        echo -e "  ${RED}✗${NC} $pkg"
    fi
done

echo ""

if [ ${#FAILED_ALL[@]} -gt 0 ]; then
    error "以下のパッケージのインストールに失敗しました:"
    for pkg in "${FAILED_ALL[@]}"; do echo "    - $pkg"; done
    echo ""
    echo "  手動で確認してください:"
    echo "    adb shell"
    echo "    apt install ${FAILED_ALL[*]}"
    echo ""
    exit 1
fi

# =============================================================================
# 6. StackFlow 動作確認
# =============================================================================
info "StackFlow サービスの状態を確認中..."
SF_PROC=$(adb_shell "ps aux | grep stackflow | grep -v grep" | tr -d '\r\n' || true)
if [ -n "$SF_PROC" ]; then
    ok "StackFlow プロセスが動作しています"
else
    warn "StackFlow プロセスが見つかりません (reboot 後に起動します)"
fi

# =============================================================================
# 7. 再起動
# =============================================================================
echo ""
ok "全パッケージのインストールが完了しました！"
echo ""
read -rp "Module LLM を再起動しますか？ [Y/n]: " DO_REBOOT
DO_REBOOT="${DO_REBOOT:-y}"

if [[ "${DO_REBOOT,,}" == "y" ]]; then
    info "Module LLM を再起動中..."
    adb_shell "reboot" || true
    echo ""
    ok "再起動コマンドを送信しました。"
    echo "  30秒ほど待ってから StackChan を起動してください。"
else
    warn "再起動をスキップしました。手動で 'adb shell reboot' を実行してください。"
fi

echo ""
echo "======================================================"
echo " セットアップ完了！"
echo "======================================================"
echo ""
echo "  次のステップ:"
echo "  1. Module LLM が再起動したら CoreS3 に取り付ける"
echo "  2. StackChan ファームウェアを書き込む"
echo "  3. 電源を入れると Module LLM を自動検出して起動します"
echo ""
