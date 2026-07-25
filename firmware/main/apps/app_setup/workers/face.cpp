/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "workers.h"
#include "common.h"
#include <stackchan/avatar/avatar.h>
#include <mooncake_log.h>

using namespace setup_workers;
using namespace uitk::lvgl_cpp;

namespace {

constexpr char kTag[] = "Setup-Face";

}  // namespace

FaceSetupWorker::FaceSetupWorker() : _selected_skin_id(stackchan::avatar::loadSelectedSkinId())
{
    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->setSize(320, 240);
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setBgColor(lv_color_hex(0xEDF4FF));
    _panel->setBorderWidth(0);
    _panel->setRadius(0);
    _panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _label_title = std::make_unique<Label>(_panel->get());
    _label_title->setText("Face");
    _label_title->setTextFont(&lv_font_montserrat_20);
    _label_title->setTextColor(lv_color_hex(0x26206A));
    _label_title->align(LV_ALIGN_TOP_MID, 0, 34);

    _panel_skins = std::make_unique<Container>(_panel->get());
    _panel_skins->setSize(296, 88);
    _panel_skins->align(LV_ALIGN_CENTER, 0, -10);
    _panel_skins->setBgColor(lv_color_hex(0xD2E3FF));
    _panel_skins->setBorderWidth(0);
    _panel_skins->setRadius(18);
    _panel_skins->setPadding(0, 0, 0, 0);
    _panel_skins->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    auto make_skin_button = [this](std::string_view label, int x_offset) {
        auto button = std::make_unique<Button>(_panel_skins->get());
        button->setSize(128, 56);
        button->align(LV_ALIGN_CENTER, x_offset, 0);
        button->setBorderWidth(0);
        button->setShadowWidth(0);
        button->setRadius(12);
        button->label().setTextFont(&lv_font_montserrat_16);
        button->label().setText(label);
        return button;
    };

    _btn_default = make_skin_button("Default", -70);
    _btn_cat     = make_skin_button("Cat", 70);

    _btn_default->onClick().connect([this]() {
        _selected_skin_id = stackchan::avatar::kDefaultSkinId;
        update_button_styles();
    });
    _btn_cat->onClick().connect([this]() {
        _selected_skin_id = stackchan::avatar::kCatSkinId;
        update_button_styles();
    });

    _btn_confirm = std::make_unique<Button>(_panel->get());
    apply_button_common_style(*_btn_confirm);
    _btn_confirm->setSize(180, 48);
    _btn_confirm->align(LV_ALIGN_BOTTOM_MID, 0, -27);
    _btn_confirm->label().setText("Confirm");
    _btn_confirm->onClick().connect([this]() { _confirm_flag = true; });

    update_button_styles();
}

FaceSetupWorker::~FaceSetupWorker() = default;

void FaceSetupWorker::update()
{
    if (!_confirm_flag) {
        return;
    }
    _confirm_flag = false;

    if (!stackchan::avatar::saveSelectedSkinId(_selected_skin_id)) {
        mclog::tagError(kTag, "failed to save skin: {}", _selected_skin_id);
        return;
    }

    mclog::tagInfo(kTag, "selected skin: {}", _selected_skin_id);
    _is_done = true;
}

void FaceSetupWorker::update_button_styles()
{
    auto style_button = [](Button& button, bool selected) {
        if (selected) {
            button.setBgColor(lv_color_hex(0x615B9E));
            button.label().setTextColor(lv_color_hex(0xFFFFFF));
        } else {
            button.setBgColor(lv_color_hex(0xB8D3FD));
            button.label().setTextColor(lv_color_hex(0x26206A));
        }
    };

    style_button(*_btn_default, _selected_skin_id == stackchan::avatar::kDefaultSkinId);
    style_button(*_btn_cat, _selected_skin_id == stackchan::avatar::kCatSkinId);
}
