#include "workers.h"
#include "common.h"
#include <mooncake_log.h>
#include <conversation/conversation_manager.h>
#include <conversation/conversation_backend.h>

using namespace setup_workers;
using namespace uitk::lvgl_cpp;

static const char* _tag = "ConvModeWorker";

// ----------------------------------------------------------------------------
// Constructor
// ----------------------------------------------------------------------------

ConversationModeWorker::ConversationModeWorker()
{
    mclog::info("ConversationModeWorker start");

    // Load current mode from NVS
    auto current = ConversationManager::loadModeFromNvs();
    switch (current) {
        case ConversationMode::LocalOnly:  _selected = 1; break;
        case ConversationMode::OnlineOnly: _selected = 2; break;
        default:                           _selected = 0; break;
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
    _label_title->setText("LLM Mode");
    _label_title->setTextFont(&lv_font_montserrat_16);
    _label_title->setTextColor(lv_color_hex(0x26206A));
    _label_title->align(LV_ALIGN_TOP_MID, 0, 6);

    // ---------- Mode button panel ----------
    _panel_modes = std::make_unique<Container>(_panel->get());
    _panel_modes->setSize(296, 114);
    _panel_modes->align(LV_ALIGN_TOP_MID, 0, 34);
    _panel_modes->setBgColor(lv_color_hex(0xD2E3FF));
    _panel_modes->setBorderWidth(0);
    _panel_modes->setRadius(18);
    _panel_modes->setPadding(0, 0, 0, 0);
    _panel_modes->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    // Three mode buttons arranged vertically inside the panel
    auto make_mode_btn = [&](const char* label, int yOffset) {
        auto btn = std::make_unique<Button>(_panel_modes->get());
        btn->setSize(260, 28);
        btn->align(LV_ALIGN_TOP_MID, 0, yOffset);
        btn->setBorderWidth(0);
        btn->setShadowWidth(0);
        btn->setRadius(10);
        btn->label().setTextFont(&lv_font_montserrat_16);
        btn->label().setText(label);
        return btn;
    };

    _btn_auto   = make_mode_btn("Auto (Online \xef\x86\x93 Local \xef\x86\x93 Fallback)",  10);
    _btn_local  = make_mode_btn("Local Only (Module LLM)",                                  46);
    _btn_online = make_mode_btn("Online Only (Xiaozhi)",                                    82);

    _btn_auto->onClick().connect([this]()   { _selected = 0; update_button_styles(); });
    _btn_local->onClick().connect([this]()  { _selected = 1; update_button_styles(); });
    _btn_online->onClick().connect([this]() { _selected = 2; update_button_styles(); });

    update_button_styles();

    // ---------- Confirm button ----------
    _btn_confirm = std::make_unique<Button>(_panel->get());
    apply_button_common_style(*_btn_confirm);
    _btn_confirm->align(LV_ALIGN_TOP_MID, 0, 162);
    _btn_confirm->setSize(290, 50);
    _btn_confirm->label().setText("Confirm");
    _btn_confirm->onClick().connect([this]() { _confirm_flag = true; });
}

ConversationModeWorker::~ConversationModeWorker() = default;

// ----------------------------------------------------------------------------
// update()
// ----------------------------------------------------------------------------

void ConversationModeWorker::update()
{
    if (!_confirm_flag) return;
    _confirm_flag = false;

    ConversationMode mode;
    switch (_selected) {
        case 1:  mode = ConversationMode::LocalOnly;  break;
        case 2:  mode = ConversationMode::OnlineOnly; break;
        default: mode = ConversationMode::Auto;       break;
    }

    mclog::tagInfo(_tag, "saving ConversationMode = {}", static_cast<int>(mode));
    ConversationManager::saveModeToNvs(mode);

    _is_done = true;
}

// ----------------------------------------------------------------------------
// update_button_styles()  — highlight selected button
// ----------------------------------------------------------------------------

void ConversationModeWorker::update_button_styles()
{
    auto style_btn = [](Button& btn, bool active) {
        if (active) {
            btn.setBgColor(lv_color_hex(0x615B9E));
            btn.label().setTextColor(lv_color_hex(0xFFFFFF));
        } else {
            btn.setBgColor(lv_color_hex(0xB8D3FD));
            btn.label().setTextColor(lv_color_hex(0x26206A));
        }
    };

    style_btn(*_btn_auto,   _selected == 0);
    style_btn(*_btn_local,  _selected == 1);
    style_btn(*_btn_online, _selected == 2);
}
