/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "../../avatar/elements/speech_bubble.h"
#include <lvgl.h>
#include <smooth_lvgl.hpp>
#include <memory>
#include <string_view>

namespace stackchan::avatar {

/**
 * Speech bubble shared by all avatar skins.
 */
class CommonSpeechBubble : public SpeechBubble {
public:
    CommonSpeechBubble(lv_obj_t* parent, lv_color_t primaryColor, lv_color_t secondaryColor, const lv_font_t* font);
    ~CommonSpeechBubble() override;

    void setSpeech(std::string_view text) override;
    void clearSpeech() override;
    void setVisible(bool visible) override;
    void setTextFont(void* font) override;

private:
    std::unique_ptr<uitk::lvgl_cpp::Container> _container;
    std::unique_ptr<uitk::lvgl_cpp::Image> _arrow;
    std::unique_ptr<uitk::lvgl_cpp::Container> _bubble;
    std::unique_ptr<uitk::lvgl_cpp::Label> _text;
};

}  // namespace stackchan::avatar
