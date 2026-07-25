#include "workers.h"
#include "common.h"
#include "module_llm_preferences.h"
#include <mooncake_log.h>
#include <nvs_flash.h>
#include <nvs.h>

using namespace setup_workers;
using namespace uitk::lvgl_cpp;

namespace prefs = module_llm_preferences;

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
    if (nvs_open(prefs::kNvsNamespace, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, prefs::kThinkingKey,   &v)  == ESP_OK) _thinking  = (v != 0);
        uint8_t vad = 1;
        if (nvs_get_u8(h, prefs::kVadEnabledKey, &vad) == ESP_OK) _vad      = (vad != 0);
        uint8_t lang = 0;
        if (nvs_get_u8(h, prefs::kTtsLangKey,   &lang) == ESP_OK) _tts_lang = lang;
        uint8_t volume = prefs::kDefaultTtsVolumePercent;
        if (nvs_get_u8(h, prefs::kTtsVolumePercentKey, &volume) == ESP_OK) {
            volume = prefs::clampTtsVolumePercent(volume);
            _tts_volume_percent = static_cast<uint8_t>(
                ((volume + prefs::kTtsVolumePercentStep / 2) /
                 prefs::kTtsVolumePercentStep) *
                prefs::kTtsVolumePercentStep);
            _tts_volume_percent =
                prefs::clampTtsVolumePercent(_tts_volume_percent);
        }
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

    // ---------- Local TTS volume ----------
    _panel_tts_volume = make_row(_panel.get(), 124, 76);

    _label_tts_volume = std::make_unique<Label>(_panel_tts_volume->get());
    _label_tts_volume->setText("Local TTS Volume");
    _label_tts_volume->setTextFont(&lv_font_montserrat_14);
    _label_tts_volume->setTextColor(lv_color_hex(0x26206A));
    _label_tts_volume->align(LV_ALIGN_TOP_LEFT, 12, 8);

    _label_tts_volume_value =
        std::make_unique<Label>(_panel_tts_volume->get());
    _label_tts_volume_value->setText(
        std::to_string(_tts_volume_percent) + "%");
    _label_tts_volume_value->setTextFont(&lv_font_montserrat_14);
    _label_tts_volume_value->setTextColor(lv_color_hex(0x26206A));
    _label_tts_volume_value->align(LV_ALIGN_TOP_RIGHT, -12, 8);

    _slider_tts_volume =
        std::make_unique<Slider>(_panel_tts_volume->get());
    _slider_tts_volume->align(LV_ALIGN_BOTTOM_MID, 0, -10);
    _slider_tts_volume->setRange(
        0, prefs::kMaxTtsVolumePercent / prefs::kTtsVolumePercentStep);
    _slider_tts_volume->setSize(254, 14);
    _slider_tts_volume->setBgColor(
        lv_color_hex(0x615B9E), LV_PART_KNOB);
    _slider_tts_volume->setBgColor(
        lv_color_hex(0x615B9E), LV_PART_INDICATOR);
    _slider_tts_volume->setBgColor(
        lv_color_hex(0xB8D3FD), LV_PART_MAIN);
    _slider_tts_volume->setBgOpa(255);
    _slider_tts_volume->setValue(
        _tts_volume_percent / prefs::kTtsVolumePercentStep);
    _slider_tts_volume->onValueChanged().connect([this](int32_t value) {
        _tts_volume_percent = static_cast<uint8_t>(
            value * prefs::kTtsVolumePercentStep);
        _label_tts_volume_value->setText(
            std::to_string(_tts_volume_percent) + "%");
    });

    // ---------- TTS Language ----------
    _panel_lang = make_row(_panel.get(), 208, 52);

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
    _btn_confirm->align(LV_ALIGN_TOP_MID, 0, 269);
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
    if (nvs_open(prefs::kNvsNamespace, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, prefs::kThinkingKey,   _thinking  ? 1 : 0);
        nvs_set_u8(h, prefs::kVadEnabledKey, _vad       ? 1 : 0);
        nvs_set_u8(h, prefs::kTtsLangKey,    _tts_lang);
        nvs_set_u8(
            h, prefs::kTtsVolumePercentKey, _tts_volume_percent);
        nvs_commit(h);
        nvs_close(h);
    }

    mclog::info(
        "ModLLMSettings: thinking={} vad={} tts_lang={} tts_volume={}%",
        _thinking, _vad, (int)_tts_lang, (int)_tts_volume_percent);
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
