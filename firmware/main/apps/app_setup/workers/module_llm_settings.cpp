#include "workers.h"
#include "common.h"
#include <mooncake_log.h>
#include <nvs_flash.h>
#include <nvs.h>

using namespace setup_workers;
using namespace uitk::lvgl_cpp;

static constexpr const char* kNvsNs         = "modllm_cfg";
static constexpr const char* kThinkingKey   = "thinking";
static constexpr const char* kVadEnabledKey = "vad_enabled";
static constexpr const char* kTtsLangKey    = "tts_lang";  // 0=ja 1=zh 2=en

static std::unique_ptr<Container> make_row(Container* parent, int y, int h = 38) {
    auto row = std::make_unique<Container>(parent->get());
    row->setSize(290, h);
    row->align(LV_ALIGN_TOP_MID, 0, y);
    row->setBgColor(lv_color_hex(0xD2E3FF));
    row->setBorderWidth(0);
    row->setRadius(10);
    row->setPadding(0, 0, 0, 0);
    row->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

// ---------------------------------------------------------------------------
ModuleLLMSettingsWorker::ModuleLLMSettingsWorker()
{
    // Read from NVS
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, kThinkingKey,   &v)  == ESP_OK) _thinking  = (v != 0);
        uint8_t vad = 1;
        if (nvs_get_u8(h, kVadEnabledKey, &vad) == ESP_OK) _vad      = (vad != 0);
        uint8_t lang = 0;
        if (nvs_get_u8(h, kTtsLangKey,   &lang) == ESP_OK) _tts_lang = lang;
        nvs_close(h);
    }

    // ---------- Root panel ----------
    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->setSize(320, 240);
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setBgColor(lv_color_hex(0xEDF4FF));
    _panel->setBorderWidth(0);
    _panel->setRadius(0);
    _panel->setPadding(0, 50, 20, 16);
    _panel->setScrollDir(LV_DIR_VER);
    _panel->setScrollbarMode(LV_SCROLLBAR_MODE_ACTIVE);

    // ---------- Title ----------
    _label_title = std::make_unique<Label>(_panel->get());
    _label_title->setText("Module LLM Settings");
    _label_title->setTextFont(&lv_font_montserrat_14);
    _label_title->setTextColor(lv_color_hex(0x26206A));
    _label_title->align(LV_ALIGN_TOP_MID, 0, 6);

    // ---------- Thinking row ----------
    _panel_thinking = make_row(_panel.get(), 32);
    _label_thinking = std::make_unique<Label>(_panel_thinking->get());
    _label_thinking->setText("Thinking");
    _label_thinking->setTextFont(&lv_font_montserrat_14);
    _label_thinking->setTextColor(lv_color_hex(0x26206A));
    _label_thinking->align(LV_ALIGN_LEFT_MID, 12, 0);
    _switch_thinking = std::make_unique<Switch>(_panel_thinking->get());
    _switch_thinking->align(LV_ALIGN_RIGHT_MID, -12, 0);
    if (_thinking) lv_obj_add_state(_switch_thinking->get(), LV_STATE_CHECKED);
    _switch_thinking->onValueChanged().connect([this](bool v) { _thinking = v; });

    // ---------- VAD row ----------
    _panel_vad = make_row(_panel.get(), 78);
    _label_vad = std::make_unique<Label>(_panel_vad->get());
    _label_vad->setText("VAD");
    _label_vad->setTextFont(&lv_font_montserrat_14);
    _label_vad->setTextColor(lv_color_hex(0x26206A));
    _label_vad->align(LV_ALIGN_LEFT_MID, 12, 0);
    _switch_vad = std::make_unique<Switch>(_panel_vad->get());
    _switch_vad->align(LV_ALIGN_RIGHT_MID, -12, 0);
    if (_vad) lv_obj_add_state(_switch_vad->get(), LV_STATE_CHECKED);
    _switch_vad->onValueChanged().connect([this](bool v) { _vad = v; });

    // ---------- TTS Language ----------
    _panel_lang = make_row(_panel.get(), 124, 52);

    auto make_lang_btn = [&](const char* label, int xOffset) {
        auto btn = std::make_unique<Button>(_panel_lang->get());
        btn->setSize(84, 36);
        btn->align(LV_ALIGN_LEFT_MID, xOffset, 0);
        btn->setBorderWidth(0);
        btn->setShadowWidth(0);
        btn->setRadius(8);
        btn->label().setTextFont(&lv_font_montserrat_14);
        btn->label().setText(label);
        return btn;
    };

    _btn_ja = make_lang_btn("Japanese", 6);
    _btn_zh = make_lang_btn("Chinese",  98);
    _btn_en = make_lang_btn("English",  190);

    _btn_ja->onClick().connect([this]() { _tts_lang = 0; update_lang_buttons(); });
    _btn_zh->onClick().connect([this]() { _tts_lang = 1; update_lang_buttons(); });
    _btn_en->onClick().connect([this]() { _tts_lang = 2; update_lang_buttons(); });

    update_lang_buttons();

    // ---------- Confirm button ----------
    _btn_confirm = std::make_unique<Button>(_panel->get());
    apply_button_common_style(*_btn_confirm);
    _btn_confirm->align(LV_ALIGN_TOP_MID, 0, 185);
    _btn_confirm->setSize(290, 44);
    _btn_confirm->label().setText("Confirm");
    _btn_confirm->onClick().connect([this]() { _confirm_flag = true; });
}

ModuleLLMSettingsWorker::~ModuleLLMSettingsWorker() = default;

void ModuleLLMSettingsWorker::update()
{
    if (!_confirm_flag) return;
    _confirm_flag = false;

    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, kThinkingKey,   _thinking  ? 1 : 0);
        nvs_set_u8(h, kVadEnabledKey, _vad       ? 1 : 0);
        nvs_set_u8(h, kTtsLangKey,    _tts_lang);
        nvs_commit(h);
        nvs_close(h);
    }

    mclog::info("ModLLMSettings: thinking={} vad={} tts_lang={}",
                _thinking, _vad, (int)_tts_lang);
    _is_done = true;
}

void ModuleLLMSettingsWorker::update_lang_buttons()
{
    auto style = [](Button& btn, bool active) {
        if (active) {
            btn.setBgColor(lv_color_hex(0x615B9E));
            btn.label().setTextColor(lv_color_hex(0xFFFFFF));
        } else {
            btn.setBgColor(lv_color_hex(0xB8D3FD));
            btn.label().setTextColor(lv_color_hex(0x26206A));
        }
    };
    style(*_btn_ja, _tts_lang == 0);
    style(*_btn_zh, _tts_lang == 1);
    style(*_btn_en, _tts_lang == 2);
}
