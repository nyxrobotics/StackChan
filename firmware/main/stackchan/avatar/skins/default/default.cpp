/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "default.h"

using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

void DefaultAvatar::init(lv_obj_t* parent, const lv_font_t* font)
{
    _panel = std::make_unique<Container>(parent);
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setSize(320, 240);
    _panel->setRadius(0);
    _panel->setBorderWidth(0);
    _panel->setBgColor(secondaryColor);
    _panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _key_elements.leftEye  = std::make_unique<DefaultEyes>(_panel->get(), primaryColor, secondaryColor, true);
    _key_elements.rightEye = std::make_unique<DefaultEyes>(_panel->get(), primaryColor, secondaryColor, false);
    _key_elements.mouth    = std::make_unique<DefaultMouth>(_panel->get(), primaryColor, secondaryColor);
    _key_elements.speechBubble =
        std::make_unique<CommonSpeechBubble>(_panel->get(), primaryColor, secondaryColor, font);
}
