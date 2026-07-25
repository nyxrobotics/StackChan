/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "../../avatar/avatar.h"
#include <lvgl.h>
#include <memory>

namespace stackchan::avatar {

class CatFaceState;

/**
 * Cat face skin drawn entirely with LVGL primitives.
 *
 * The 400 x 200 reference geometry is pre-mapped at 0.8 scale into the
 * centered 320 x 160 region (y = 40..199) of the 320 x 240 display.
 */
class CatAvatar : public Avatar {
public:
    CatAvatar() = default;
    ~CatAvatar() override;

    void init(lv_obj_t* parent, const lv_font_t* font = &lv_font_montserrat_16);
    bool hasNativeSleepIndicator() const override
    {
        return true;
    }

private:
    std::shared_ptr<CatFaceState> _state;
    lv_obj_t* _face = nullptr;
};

}  // namespace stackchan::avatar
