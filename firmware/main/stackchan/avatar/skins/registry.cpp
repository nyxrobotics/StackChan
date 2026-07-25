/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "registry.h"
#include "cat/cat.h"
#include "default/default.h"
#include <settings.h>
#include <esp_log.h>

namespace stackchan::avatar {
namespace {

constexpr char kTag[]               = "AvatarSkin";
constexpr char kSettingsNamespace[] = "avatar";
constexpr char kSettingsKey[]       = "skin";

constexpr std::array<SkinInfo, 2> kAvailableSkins = {{
    {kDefaultSkinId, "Default"},
    {kCatSkinId, "Cat"},
}};

template <typename Skin>
std::unique_ptr<Avatar> createSkin(lv_obj_t* parent, const lv_font_t* font)
{
    auto skin = std::make_unique<Skin>();
    skin->init(parent, font);
    return skin;
}

}  // namespace

const std::array<SkinInfo, 2>& availableSkins()
{
    return kAvailableSkins;
}

bool isKnownSkinId(std::string_view id)
{
    for (const auto& skin : kAvailableSkins) {
        if (skin.id == id) {
            return true;
        }
    }
    return false;
}

std::string loadSelectedSkinId()
{
    Settings settings(kSettingsNamespace, false);
    auto id = settings.GetString(kSettingsKey, std::string(kDefaultSkinId));
    if (!isKnownSkinId(id)) {
        ESP_LOGW(kTag, "Unknown skin '%s', falling back to default", id.c_str());
        return std::string(kDefaultSkinId);
    }
    return id;
}

bool saveSelectedSkinId(std::string_view id)
{
    if (!isKnownSkinId(id)) {
        const std::string unknown_id(id);
        ESP_LOGW(kTag, "Refusing to save unknown skin '%s'", unknown_id.c_str());
        return false;
    }

    Settings settings(kSettingsNamespace, true);
    settings.SetString(kSettingsKey, std::string(id));
    return true;
}

std::unique_ptr<Avatar> createAvatar(std::string_view id, lv_obj_t* parent, const lv_font_t* font)
{
    if (id == kCatSkinId) {
        return createSkin<CatAvatar>(parent, font);
    }
    if (id != kDefaultSkinId) {
        const std::string unknown_id(id);
        ESP_LOGW(kTag, "Unknown skin '%s', creating default", unknown_id.c_str());
    }
    return createSkin<DefaultAvatar>(parent, font);
}

std::unique_ptr<Avatar> createSelectedAvatar(lv_obj_t* parent, const lv_font_t* font)
{
    return createAvatar(loadSelectedSkinId(), parent, font);
}

}  // namespace stackchan::avatar
