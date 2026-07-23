// =============================================================================
// module_llm_provisioner.h
//
// 起動時に Module LLM の接続状態・インストール状態を診断し、
// 未セットアップなら LCD に案内を表示するユーティリティ。
//
// conversation_manager.cpp の start() から呼び出す:
//
//   ModuleLLMProvisioner prov(llmClient_);
//   auto result = prov.diagnose();
//   if (result.needsSetup) {
//       prov.showSetupGuide();   // LCD に案内を表示
//   }
// =============================================================================
#pragma once

#include <string>
#include <vector>
#include <memory>

class ModuleLLMClient;

// ---------------------------------------------------------------------------
// 診断結果
// ---------------------------------------------------------------------------
struct ModuleLLMDiagnosis {
    bool uart_ok       = false;  // UART 開通 + sys.ping 応答
    bool models_ok     = false;  // 全モデルのロードに成功
    bool pipeline_ok   = false;  // パイプライン構築に成功

    bool needsSetup() const {
        return uart_ok && !models_ok;  // 接続はあるがモデルがない = 未プロビジョニング
    }

    bool fullyReady() const {
        return uart_ok && models_ok && pipeline_ok;
    }

    // 人間向けのステータス文字列
    std::string summary() const {
        if (!uart_ok)    return "Module LLM: 未接続";
        if (!models_ok)  return "Module LLM: セットアップ未完了";
        if (!pipeline_ok)return "Module LLM: パイプライン構築失敗";
        return "Module LLM: 準備完了";
    }

    // 未セットアップ時に画面に出すメッセージ
    std::vector<std::string> setupSteps() const {
        return {
            "Module LLM のセットアップが必要です",
            "",
            "PC で以下を実行してください:",
            "  USB-C で Module LLM を PC に接続",
            "  scripts/provision_module_llm.sh を実行",
            "",
            "再起動後に自動で認識されます",
        };
    }
};

// ---------------------------------------------------------------------------
// ModuleLLMProvisioner
// ---------------------------------------------------------------------------
class ModuleLLMProvisioner {
public:
    explicit ModuleLLMProvisioner(std::shared_ptr<ModuleLLMClient> client)
        : client_(std::move(client)) {}

    // UART 接続 → モデルロード → パイプラインを順番に試して診断結果を返す
    // ※ このメソッドは conversation_manager.cpp::start() を置き換える形で使う
    ModuleLLMDiagnosis diagnose();

    // 診断結果を LCD (M5Stack LVGL) に表示する
    // needsSetup() == true のときに呼ぶ
    void showSetupGuide(const ModuleLLMDiagnosis& diag);

private:
    std::shared_ptr<ModuleLLMClient> client_;
};
