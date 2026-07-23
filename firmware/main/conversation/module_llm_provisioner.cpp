#include "module_llm_provisioner.h"
#include "module_llm_client.h"

#include <esp_log.h>
#include <lvgl.h>   // M5Stack LVGL (CoreS3 の LCD 描画に使用)

static const char* TAG = "ModLLMProv";

// =============================================================================
// diagnose()
// =============================================================================
ModuleLLMDiagnosis ModuleLLMProvisioner::diagnose() {
    ModuleLLMDiagnosis d;

    // Step 1: UART 開通 + sys.ping
    ESP_LOGI(TAG, "Step 1: UART ping...");
    d.uart_ok = client_->connect();
    if (!d.uart_ok) {
        ESP_LOGI(TAG, "Module LLM not connected (UART no response)");
        return d;
    }
    ESP_LOGI(TAG, "UART OK");

    // Step 2: モデルロード + パイプライン構築を試みる
    // loadModelsAndPipeline() はモデルが apt インストールされていないと
    // waitForAck() がタイムアウトして false を返す。
    ESP_LOGI(TAG, "Step 2: Loading models...");
    bool pipelineOk = client_->loadModelsAndPipeline();
    d.models_ok   = pipelineOk;
    d.pipeline_ok = pipelineOk;

    if (!pipelineOk) {
        ESP_LOGW(TAG, "Model load failed — Module LLM may need provisioning");
        // NOTE: モデルが入っていない場合のエラーと、他の原因 (ストレージ不足等)
        // を区別するには StackFlow のエラーレスポンスを詳しく見る必要がある。
        // 現時点では「接続あり・ロード失敗」= セットアップ未完了 と見なす。
    } else {
        ESP_LOGI(TAG, "Pipeline ready");
    }

    return d;
}

// =============================================================================
// showSetupGuide()
// LVGL で CoreS3 の LCD にセットアップ案内を表示する
// =============================================================================
void ModuleLLMProvisioner::showSetupGuide(const ModuleLLMDiagnosis& diag) {
    ESP_LOGI(TAG, "Showing setup guide on LCD");

    // --- 全画面オーバーレイを作成 ---
    lv_obj_t* overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x1A1A2E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(overlay, 12, LV_PART_MAIN);

    // --- アイコン (Module LLM のイメージが無ければ絵文字風テキスト) ---
    lv_obj_t* icon_label = lv_label_create(overlay);
    lv_label_set_text(icon_label, LV_SYMBOL_WARNING "  Module LLM");
    lv_obj_set_style_text_color(icon_label, lv_color_hex(0xFFD700), LV_PART_MAIN);
    lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(icon_label, LV_ALIGN_TOP_MID, 0, 8);

    // --- タイトル ---
    lv_obj_t* title = lv_label_create(overlay);
    lv_label_set_text(title, "セットアップが必要です");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align_to(title, icon_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);

    // --- 区切り線 ---
    lv_obj_t* line = lv_obj_create(overlay);
    lv_obj_set_size(line, LV_HOR_RES - 24, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x4A4A6A), LV_PART_MAIN);
    lv_obj_align_to(line, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    // --- 手順テキスト ---
    const char* guide =
        "PC で次のコマンドを実行\n"
        "\n"
        "1. Module LLM の USB-C を\n"
        "   PC に接続する\n"
        "\n"
        "2. リポジトリ内で実行:\n"
        "   scripts/provision_module_llm.sh\n"
        "\n"
        "3. 完了後 StackChan を再起動";

    lv_obj_t* body = lv_label_create(overlay);
    lv_label_set_text(body, guide);
    lv_obj_set_style_text_color(body, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LV_HOR_RES - 24);
    lv_obj_align_to(body, line, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);

    // --- ステータスバー (下部) ---
    lv_obj_t* status = lv_label_create(overlay);
    lv_label_set_text(status, diag.summary().c_str());
    lv_obj_set_style_text_color(status, lv_color_hex(0xFF6B6B), LV_PART_MAIN);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(status, LV_ALIGN_BOTTOM_LEFT, 0, -4);
}
