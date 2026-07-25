/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../avatar/avatar.h"
#include <lvgl.h>
#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace stackchan::avatar {

inline constexpr std::string_view kDefaultSkinId = "default";
inline constexpr std::string_view kCatSkinId     = "cat";

struct SkinInfo {
    std::string_view id;
    std::string_view displayName;
};

const std::array<SkinInfo, 2>& availableSkins();
bool isKnownSkinId(std::string_view id);

std::string loadSelectedSkinId();
bool saveSelectedSkinId(std::string_view id);

std::unique_ptr<Avatar> createAvatar(std::string_view id, lv_obj_t* parent,
                                     const lv_font_t* font = &lv_font_montserrat_16);
std::unique_ptr<Avatar> createSelectedAvatar(lv_obj_t* parent, const lv_font_t* font = &lv_font_montserrat_16);

}  // namespace stackchan::avatar
