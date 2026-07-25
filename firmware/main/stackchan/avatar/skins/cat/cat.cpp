/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "cat.h"
#include "../common/speech_bubble.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>

using namespace uitk;
using namespace uitk::lvgl_cpp;

namespace stackchan::avatar {

namespace {

struct Point {
    int x;
    int y;
};

struct FeaturePose {
    Vector2i position;
    int rotation = 0;
    int size     = 0;
    int weight   = 0;
    bool visible = true;
};

struct DrawContext {
    lv_layer_t* layer;
    int origin_x;
    int origin_y;
};

constexpr float kPi       = 3.14159265358979323846f;
constexpr int kFaceTop    = 40;
constexpr int kFaceHeight = 160;

Point transform_point(Point point, Point pivot, const FeaturePose& pose)
{
    const int offset_x = pose.position.x * 16 / 100;
    const int offset_y = pose.position.y * 16 / 100;
    if (pose.size == 0 && (pose.rotation == 0 || pose.rotation == 3600)) {
        return {point.x + offset_x, point.y + offset_y};
    }

    const float scale  = 1.0f + static_cast<float>(pose.size) * 0.0025f;
    const float angle  = static_cast<float>(pose.rotation) * (kPi / 1800.0f);
    const float sine   = std::sin(angle);
    const float cosine = std::cos(angle);
    const float x      = static_cast<float>(point.x - pivot.x) * scale;
    const float y      = static_cast<float>(point.y - pivot.y) * scale;

    return {
        pivot.x + static_cast<int>(std::lround(x * cosine - y * sine)) + offset_x,
        pivot.y + static_cast<int>(std::lround(x * sine + y * cosine)) + offset_y,
    };
}

int transform_width(int width, const FeaturePose& pose)
{
    if (pose.size == 0) {
        return width;
    }

    const float scale = 1.0f + static_cast<float>(pose.size) * 0.0025f;
    return std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * scale)));
}

void draw_line(const DrawContext& context, Point from, Point to, int width, lv_color_t color = lv_color_white())
{
    lv_draw_line_dsc_t descriptor;
    lv_draw_line_dsc_init(&descriptor);
    descriptor.color       = color;
    descriptor.opa         = LV_OPA_COVER;
    descriptor.width       = width;
    descriptor.round_start = 1;
    descriptor.round_end   = 1;
    descriptor.p1          = {context.origin_x + from.x, context.origin_y + from.y};
    descriptor.p2          = {context.origin_x + to.x, context.origin_y + to.y};
    lv_draw_line(context.layer, &descriptor);
}

void draw_line(const DrawContext& context, Point from, Point to, int width, const FeaturePose& pose, Point pivot,
               lv_color_t color = lv_color_white())
{
    draw_line(context, transform_point(from, pivot, pose), transform_point(to, pivot, pose),
              transform_width(width, pose), color);
}

template <size_t PointCount>
void draw_polyline(const DrawContext& context, const Point (&points)[PointCount], int width, const FeaturePose& pose,
                   Point pivot, lv_color_t color = lv_color_white())
{
    static_assert(PointCount >= 2);
    for (size_t index = 1; index < PointCount; ++index) {
        draw_line(context, points[index - 1], points[index], width, pose, pivot, color);
    }
}

void draw_ring(const DrawContext& context, Point center, int outer_radius, int width, const FeaturePose& pose,
               Point pivot, lv_color_t color = lv_color_white())
{
    center = transform_point(center, pivot, pose);

    lv_draw_arc_dsc_t descriptor;
    lv_draw_arc_dsc_init(&descriptor);
    descriptor.color       = color;
    descriptor.opa         = LV_OPA_COVER;
    descriptor.width       = transform_width(width, pose);
    descriptor.center      = {context.origin_x + center.x, context.origin_y + center.y};
    descriptor.radius      = static_cast<uint16_t>(std::max(1, transform_width(outer_radius, pose)));
    descriptor.start_angle = 0;
    descriptor.end_angle   = 360;
    descriptor.rounded     = 1;
    lv_draw_arc(context.layer, &descriptor);
}

void draw_filled_circle(const DrawContext& context, Point center, int diameter, const FeaturePose& pose, Point pivot,
                        lv_color_t color = lv_color_white())
{
    center               = transform_point(center, pivot, pose);
    const int size       = transform_width(diameter, pose);
    const int half       = size / 2;
    const lv_area_t area = {
        context.origin_x + center.x - half,
        context.origin_y + center.y - half,
        context.origin_x + center.x - half + size - 1,
        context.origin_y + center.y - half + size - 1,
    };

    lv_draw_rect_dsc_t descriptor;
    lv_draw_rect_dsc_init(&descriptor);
    descriptor.bg_color = color;
    descriptor.bg_opa   = LV_OPA_COVER;
    descriptor.radius   = LV_RADIUS_CIRCLE;
    lv_draw_rect(context.layer, &descriptor, &area);
}

void draw_triangle(const DrawContext& context, Point first, Point second, Point third,
                   lv_color_t color = lv_color_white())
{
    lv_draw_triangle_dsc_t descriptor;
    lv_draw_triangle_dsc_init(&descriptor);
    descriptor.color = color;
    descriptor.opa   = LV_OPA_COVER;
    descriptor.p[0]  = {context.origin_x + first.x, context.origin_y + first.y};
    descriptor.p[1]  = {context.origin_x + second.x, context.origin_y + second.y};
    descriptor.p[2]  = {context.origin_x + third.x, context.origin_y + third.y};
    lv_draw_triangle(context.layer, &descriptor);
}

void draw_triangle(const DrawContext& context, Point first, Point second, Point third, const FeaturePose& pose,
                   Point pivot, lv_color_t color = lv_color_white())
{
    draw_triangle(context, transform_point(first, pivot, pose), transform_point(second, pivot, pose),
                  transform_point(third, pivot, pose), color);
}

void draw_whiskers(const DrawContext& context, Point left_top_from, Point left_top_to, Point left_bottom_from,
                   Point left_bottom_to, Point right_top_from, Point right_top_to, Point right_bottom_from,
                   Point right_bottom_to, const FeaturePose& pose, Point pivot)
{
    draw_line(context, left_top_from, left_top_to, 3, pose, pivot);
    draw_line(context, left_bottom_from, left_bottom_to, 3, pose, pivot);
    draw_line(context, right_top_from, right_top_to, 3, pose, pivot);
    draw_line(context, right_bottom_from, right_bottom_to, 3, pose, pivot);
}

void draw_nose(const DrawContext& context, Point center, const FeaturePose& pose, Point pivot)
{
    draw_line(context, {center.x - 2, center.y}, {center.x + 2, center.y}, 7, pose, pivot);
}

void draw_cat_smile(const DrawContext& context, Point center, int half_width, const FeaturePose& pose, Point pivot)
{
    const Point left[] = {
        {center.x - half_width, center.y},
        {center.x - half_width + 3, center.y + 6},
        {center.x - 9, center.y + 8},
        {center.x - 4, center.y + 7},
        center,
    };
    const Point right[] = {
        center,
        {center.x + 4, center.y + 7},
        {center.x + 9, center.y + 8},
        {center.x + half_width - 3, center.y + 6},
        {center.x + half_width, center.y},
    };
    draw_polyline(context, left, 4, pose, pivot);
    draw_polyline(context, right, 4, pose, pivot);
}

void draw_open_mouth(const DrawContext& context, Point top_center, int base_half_width, int base_bottom,
                     const FeaturePose& pose, Point pivot)
{
    const float openness = std::clamp(static_cast<float>(pose.weight) / 65.0f, 0.35f, 1.23f);
    const int half_width = std::max(12, static_cast<int>(std::lround(base_half_width * (0.68f + 0.32f * openness))));
    const int bottom =
        top_center.y + static_cast<int>(std::lround(static_cast<float>(base_bottom - top_center.y) * openness));

    draw_cat_smile(context, top_center, half_width, pose, pivot);

    const Point outline[] = {
        {top_center.x - half_width + 2, top_center.y + 5},
        {top_center.x - half_width, top_center.y + 15},
        {top_center.x - half_width + 1, bottom - 12},
        {top_center.x - half_width + 7, bottom - 4},
        {top_center.x - 10, bottom},
        {top_center.x, bottom + 2},
        {top_center.x + 10, bottom},
        {top_center.x + half_width - 7, bottom - 4},
        {top_center.x + half_width - 1, bottom - 12},
        {top_center.x + half_width, top_center.y + 15},
        {top_center.x + half_width - 2, top_center.y + 5},
    };
    draw_polyline(context, outline, 4, pose, pivot);
}

void draw_brow(const DrawContext& context, Point start, Point rise, Point peak, Point end, int width,
               const FeaturePose& pose, Point transform_pivot)
{
    const Point brow[] = {start, rise, peak, end};
    draw_polyline(context, brow, width, pose, transform_pivot);
}

void draw_closed_blink(const DrawContext& context, Point center, const FeaturePose& pose)
{
    draw_line(context, {center.x - 28, center.y}, {center.x + 28, center.y}, 12, pose, center);
}

bool is_blinking(const FeaturePose& eye)
{
    return eye.weight <= 30;
}

}  // namespace

class CatFaceState {
public:
    lv_obj_t* surface = nullptr;
    Emotion emotion   = Emotion::Neutral;
    FeaturePose left_eye;
    FeaturePose right_eye;
    FeaturePose mouth;

    void invalidate() const
    {
        if (surface) {
            lv_obj_invalidate(surface);
        }
    }
};

namespace {

class CatEye final : public Feature {
public:
    CatEye(std::shared_ptr<CatFaceState> state, bool left) : _state(std::move(state)), _left(left)
    {
        Feature::setWeight(100);
        sync_pose();
    }

    void setPosition(const Vector2i& position) override
    {
        Element::setPosition(position);
        sync_pose();
    }

    void setWeight(int weight) override
    {
        Feature::setWeight(weight);
        sync_pose();
    }

    void setRotation(int rotation) override
    {
        Element::setRotation(rotation);
        sync_pose();
    }

    void setEmotion(const Emotion& emotion) override
    {
        if (getIgnoreEmotion()) {
            return;
        }

        _state->emotion = emotion;
        if (emotion == Emotion::Sleepy) {
            Feature::setWeight(35);
        } else if (emotion == Emotion::Wink && !_left) {
            Feature::setWeight(25);
        } else {
            Feature::setWeight(100);
        }
        sync_pose();
    }

    void setVisible(bool visible) override
    {
        Element::setVisible(visible);
        sync_pose();
    }

    void setSize(int size) override
    {
        Feature::setSize(size);
        sync_pose();
    }

private:
    void sync_pose()
    {
        FeaturePose& pose = _left ? _state->left_eye : _state->right_eye;
        pose.position     = _position;
        pose.rotation     = _rotation;
        pose.size         = _size;
        pose.weight       = _weight;
        pose.visible      = _visible;
        _state->invalidate();
    }

    std::shared_ptr<CatFaceState> _state;
    bool _left;
};

class CatMouth final : public Feature {
public:
    explicit CatMouth(std::shared_ptr<CatFaceState> state) : _state(std::move(state))
    {
        Feature::setWeight(0);
        sync_pose();
    }

    void setPosition(const Vector2i& position) override
    {
        Element::setPosition(position);
        sync_pose();
    }

    void setWeight(int weight) override
    {
        Feature::setWeight(weight);
        sync_pose();
    }

    void setRotation(int rotation) override
    {
        Element::setRotation(rotation);
        sync_pose();
    }

    void setEmotion(const Emotion& emotion) override
    {
        if (getIgnoreEmotion()) {
            return;
        }

        _state->emotion = emotion;
        if (emotion == Emotion::Happy || emotion == Emotion::Cute || emotion == Emotion::Sleepy) {
            Feature::setWeight(65);
        } else {
            Feature::setWeight(0);
        }
        sync_pose();
    }

    void setVisible(bool visible) override
    {
        Element::setVisible(visible);
        sync_pose();
    }

    void setSize(int size) override
    {
        Feature::setSize(size);
        sync_pose();
    }

private:
    void sync_pose()
    {
        _state->mouth.position = _position;
        _state->mouth.rotation = _rotation;
        _state->mouth.size     = _size;
        _state->mouth.weight   = _weight;
        _state->mouth.visible  = _visible;
        _state->invalidate();
    }

    std::shared_ptr<CatFaceState> _state;
};

void draw_default_eyes(const DrawContext& context, const CatFaceState& state)
{
    constexpr Point left_center  = {102, 120};
    constexpr Point right_center = {211, 120};

    if (state.left_eye.visible) {
        draw_line(context, {125, 68}, {135, 68}, 9, state.left_eye, left_center);
        if (is_blinking(state.left_eye)) {
            draw_closed_blink(context, left_center, state.left_eye);
        } else {
            draw_ring(context, left_center, 37, 11, state.left_eye, left_center);
        }
    }
    if (state.right_eye.visible) {
        draw_line(context, {178, 68}, {188, 68}, 9, state.right_eye, right_center);
        if (is_blinking(state.right_eye)) {
            draw_closed_blink(context, right_center, state.right_eye);
        } else {
            draw_ring(context, right_center, 37, 11, state.right_eye, right_center);
        }
    }
}

void draw_happy_eyes(const DrawContext& context, const CatFaceState& state)
{
    constexpr Point left_center  = {102, 106};
    constexpr Point right_center = {214, 106};
    const Point left_curve[]     = {
        {73, 98}, {81, 96}, {91, 94}, {102, 95}, {113, 100}, {123, 108}, {131, 119},
    };
    const Point right_curve[] = {
        {185, 119}, {191, 109}, {201, 100}, {212, 95}, {223, 94}, {233, 96}, {242, 98},
    };

    if (state.left_eye.visible) {
        draw_brow(context, {126, 68}, {129, 66}, {132, 66}, {135, 68}, 8, state.left_eye, left_center);
        if (is_blinking(state.left_eye)) {
            draw_closed_blink(context, {102, 111}, state.left_eye);
        } else {
            draw_polyline(context, left_curve, 14, state.left_eye, left_center);
        }
    }
    if (state.right_eye.visible) {
        draw_brow(context, {179, 68}, {182, 66}, {186, 66}, {189, 68}, 8, state.right_eye, right_center);
        if (is_blinking(state.right_eye)) {
            draw_closed_blink(context, {214, 111}, state.right_eye);
        } else {
            draw_polyline(context, right_curve, 14, state.right_eye, right_center);
        }
    }
}

void draw_angry_eyes(const DrawContext& context, const CatFaceState& state)
{
    constexpr Point left_center  = {106, 121};
    constexpr Point right_center = {215, 121};
    if (state.left_eye.visible) {
        draw_line(context, {131, 76}, {137, 81}, 9, state.left_eye, left_center);
        if (is_blinking(state.left_eye)) {
            draw_closed_blink(context, left_center, state.left_eye);
        } else {
            draw_ring(context, left_center, 37, 11, state.left_eye, left_center);
        }
    }
    if (state.right_eye.visible) {
        draw_line(context, {184, 81}, {190, 76}, 9, state.right_eye, right_center);
        if (is_blinking(state.right_eye)) {
            draw_closed_blink(context, right_center, state.right_eye);
        } else {
            draw_ring(context, right_center, 37, 11, state.right_eye, right_center);
        }

        draw_line(context, {244, 64}, {249, 67}, 4, state.right_eye, right_center);
        draw_line(context, {249, 67}, {254, 64}, 4, state.right_eye, right_center);
        draw_line(context, {239, 69}, {242, 74}, 4, state.right_eye, right_center);
        draw_line(context, {242, 74}, {239, 79}, 4, state.right_eye, right_center);
        draw_line(context, {260, 69}, {257, 74}, 4, state.right_eye, right_center);
        draw_line(context, {257, 74}, {260, 79}, 4, state.right_eye, right_center);
        draw_line(context, {244, 84}, {249, 81}, 4, state.right_eye, right_center);
        draw_line(context, {249, 81}, {254, 84}, 4, state.right_eye, right_center);
    }
}

void draw_cute_eyes(const DrawContext& context, const CatFaceState& state)
{
    constexpr Point left_center  = {105, 110};
    constexpr Point right_center = {214, 110};

    if (state.left_eye.visible) {
        draw_brow(context, {128, 68}, {131, 66}, {135, 66}, {138, 68}, 8, state.left_eye, left_center);
        if (is_blinking(state.left_eye)) {
            draw_closed_blink(context, left_center, state.left_eye);
        } else {
            draw_ring(context, left_center, 37, 5, state.left_eye, left_center);
            draw_filled_circle(context, {94, 98}, 24, state.left_eye, left_center);
            draw_filled_circle(context, {122, 126}, 8, state.left_eye, left_center);
        }
    }
    if (state.right_eye.visible) {
        draw_brow(context, {182, 68}, {185, 66}, {188, 66}, {191, 68}, 8, state.right_eye, right_center);
        if (is_blinking(state.right_eye)) {
            draw_closed_blink(context, right_center, state.right_eye);
        } else {
            draw_ring(context, right_center, 37, 5, state.right_eye, right_center);
            draw_filled_circle(context, {203, 98}, 24, state.right_eye, right_center);
            draw_filled_circle(context, {231, 126}, 8, state.right_eye, right_center);
        }
    }
}

void draw_dizzy_eyes(const DrawContext& context, const CatFaceState& state)
{
    constexpr Point left_center  = {104, 120};
    constexpr Point right_center = {214, 120};
    const Point left_spiral[]    = {
        {104, 120}, {108, 117}, {112, 119}, {113, 124}, {110, 129}, {104, 132}, {97, 130},
        {92, 125},  {91, 117},  {94, 109},  {101, 104}, {111, 104}, {120, 110}, {124, 120},
        {122, 131}, {114, 140}, {102, 143}, {89, 138},  {81, 128},  {79, 115},  {84, 102},
    };
    const Point right_spiral[] = {
        {214, 120}, {210, 123}, {206, 121}, {205, 116}, {208, 111}, {214, 108}, {221, 110},
        {226, 115}, {227, 123}, {224, 131}, {217, 136}, {207, 136}, {198, 130}, {194, 120},
        {196, 109}, {204, 100}, {216, 97},  {229, 102}, {237, 112}, {239, 125}, {234, 138},
    };

    if (state.left_eye.visible) {
        draw_brow(context, {128, 68}, {131, 68}, {134, 67}, {137, 66}, 9, state.left_eye, left_center);
        if (is_blinking(state.left_eye)) {
            draw_closed_blink(context, left_center, state.left_eye);
        } else {
            draw_ring(context, left_center, 37, 5, state.left_eye, left_center);
            draw_polyline(context, left_spiral, 5, state.left_eye, left_center);
        }
    }
    if (state.right_eye.visible) {
        draw_brow(context, {181, 66}, {184, 67}, {187, 67}, {190, 68}, 9, state.right_eye, right_center);
        if (is_blinking(state.right_eye)) {
            draw_closed_blink(context, right_center, state.right_eye);
        } else {
            draw_ring(context, right_center, 37, 5, state.right_eye, right_center);
            draw_polyline(context, right_spiral, 5, state.right_eye, right_center);
        }
    }
}

void draw_sleepy_eyes(const DrawContext& context, const CatFaceState& state)
{
    constexpr Point left_center  = {101, 116};
    constexpr Point right_center = {211, 116};
    if (state.left_eye.visible) {
        draw_brow(context, {124, 78}, {127, 77}, {131, 77}, {134, 78}, 9, state.left_eye, left_center);
        draw_line(context, {72, 120}, {129, 112}, 16, state.left_eye, left_center);
    }
    if (state.right_eye.visible) {
        draw_brow(context, {177, 77}, {180, 76}, {184, 76}, {187, 77}, 9, state.right_eye, right_center);
        draw_line(context, {182, 113}, {239, 120}, 16, state.right_eye, right_center);

        const Point zed[] = {{225, 74}, {238, 76}, {227, 82}, {237, 87}, {224, 88}};
        draw_polyline(context, zed, 4, state.right_eye, right_center);
    }
}

void draw_wink_eyes(const DrawContext& context, const CatFaceState& state)
{
    constexpr Point left_center  = {97, 127};
    constexpr Point right_center = {201, 108};
    if (state.left_eye.visible) {
        draw_brow(context, {114, 70}, {117, 68}, {120, 68}, {123, 70}, 9, state.left_eye, left_center);
        if (is_blinking(state.left_eye)) {
            draw_closed_blink(context, left_center, state.left_eye);
        } else {
            draw_ring(context, left_center, 38, 5, state.left_eye, left_center);
        }
    }
    if (state.right_eye.visible) {
        draw_brow(context, {166, 63}, {168, 61}, {171, 60}, {175, 61}, 9, state.right_eye, right_center);
        draw_line(context, {177, 120}, {224, 89}, 14, state.right_eye, right_center);
        draw_line(context, {177, 120}, {224, 126}, 14, state.right_eye, right_center);

        draw_triangle(context, {242, 96}, {244, 99}, {242, 100}, state.right_eye, right_center);
        draw_triangle(context, {244, 99}, {246, 100}, {242, 100}, state.right_eye, right_center);
        draw_triangle(context, {246, 100}, {242, 100}, {242, 103}, state.right_eye, right_center);
        draw_triangle(context, {242, 100}, {238, 100}, {240, 99}, state.right_eye, right_center);
    }
}

void draw_default_mouth(const DrawContext& context, const CatFaceState& state)
{
    if (!state.mouth.visible) {
        return;
    }
    constexpr Point mouth_pivot = {157, 149};
    draw_nose(context, {157, 140}, state.mouth, mouth_pivot);
    if (state.mouth.weight > 25) {
        draw_open_mouth(context, {157, 149}, 21, 181, state.mouth, mouth_pivot);
    } else {
        draw_cat_smile(context, {157, 149}, 16, state.mouth, mouth_pivot);
    }
    draw_whiskers(context, {56, 151}, {64, 151}, {56, 162}, {64, 160}, {250, 151}, {258, 151}, {250, 160}, {258, 162},
                  state.mouth, mouth_pivot);
}

void draw_happy_mouth(const DrawContext& context, const CatFaceState& state)
{
    if (!state.mouth.visible) {
        return;
    }
    constexpr Point mouth_pivot = {157, 134};
    draw_nose(context, {157, 126}, state.mouth, mouth_pivot);
    if (state.mouth.weight > 25) {
        draw_open_mouth(context, {157, 134}, 21, 177, state.mouth, mouth_pivot);
    } else {
        draw_cat_smile(context, {157, 134}, 16, state.mouth, mouth_pivot);
    }
    draw_whiskers(context, {56, 136}, {64, 136}, {56, 147}, {64, 145}, {251, 136}, {259, 136}, {251, 145}, {259, 147},
                  state.mouth, mouth_pivot);
}

void draw_cute_mouth(const DrawContext& context, const CatFaceState& state)
{
    if (!state.mouth.visible) {
        return;
    }
    constexpr Point mouth_pivot = {159, 134};
    draw_nose(context, {159, 126}, state.mouth, mouth_pivot);
    if (state.mouth.weight > 25) {
        draw_open_mouth(context, {159, 134}, 21, 177, state.mouth, mouth_pivot);
    } else {
        draw_cat_smile(context, {159, 134}, 16, state.mouth, mouth_pivot);
    }
    draw_whiskers(context, {59, 136}, {67, 136}, {59, 147}, {67, 145}, {253, 136}, {261, 136}, {253, 145}, {261, 147},
                  state.mouth, mouth_pivot);
}

void draw_sad_mouth(const DrawContext& context, const CatFaceState& state)
{
    if (!state.mouth.visible) {
        return;
    }
    constexpr Point mouth_pivot = {159, 139};
    draw_nose(context, {159, 126}, state.mouth, mouth_pivot);
    if (state.mouth.weight > 25) {
        draw_open_mouth(context, {159, 139}, 20, 175, state.mouth, mouth_pivot);
    } else {
        const Point frown[] = {{146, 146}, {159, 138}, {172, 146}};
        draw_polyline(context, frown, 4, state.mouth, mouth_pivot);
    }
    draw_whiskers(context, {59, 136}, {67, 136}, {59, 147}, {67, 145}, {253, 136}, {261, 136}, {253, 145}, {261, 147},
                  state.mouth, mouth_pivot);
}

void draw_angry_mouth(const DrawContext& context, const CatFaceState& state)
{
    if (!state.mouth.visible) {
        return;
    }
    constexpr Point mouth_pivot = {160, 154};
    draw_nose(context, {160, 140}, state.mouth, mouth_pivot);
    if (state.mouth.weight > 25) {
        draw_open_mouth(context, {160, 151}, 20, 180, state.mouth, mouth_pivot);
    } else {
        const Point frown[] = {{147, 159}, {160, 152}, {175, 160}};
        draw_polyline(context, frown, 4, state.mouth, mouth_pivot);
    }
    draw_whiskers(context, {60, 151}, {68, 151}, {60, 162}, {68, 160}, {254, 151}, {262, 151}, {254, 160}, {262, 162},
                  state.mouth, mouth_pivot);
}

void draw_dizzy_mouth(const DrawContext& context, const CatFaceState& state)
{
    if (!state.mouth.visible) {
        return;
    }
    constexpr Point mouth_pivot = {159, 163};
    draw_nose(context, {159, 140}, state.mouth, mouth_pivot);
    if (state.mouth.weight > 25) {
        draw_open_mouth(context, {159, 151}, 20, 180, state.mouth, mouth_pivot);
    } else {
        const Point left[]  = {{159, 155}, {159, 163}, {154, 171}, {148, 169}};
        const Point right[] = {{159, 163}, {164, 172}, {170, 169}};
        draw_polyline(context, left, 3, state.mouth, mouth_pivot);
        draw_polyline(context, right, 3, state.mouth, mouth_pivot);
    }
    const Point left_top[]     = {{57, 151}, {59, 149}, {62, 152}, {65, 150}};
    const Point left_bottom[]  = {{57, 162}, {59, 160}, {62, 163}, {65, 161}};
    const Point right_top[]    = {{252, 150}, {255, 152}, {258, 149}, {260, 151}};
    const Point right_bottom[] = {{252, 161}, {255, 163}, {258, 160}, {260, 162}};
    draw_polyline(context, left_top, 3, state.mouth, mouth_pivot);
    draw_polyline(context, left_bottom, 3, state.mouth, mouth_pivot);
    draw_polyline(context, right_top, 3, state.mouth, mouth_pivot);
    draw_polyline(context, right_bottom, 3, state.mouth, mouth_pivot);
}

void draw_sleepy_mouth(const DrawContext& context, const CatFaceState& state)
{
    if (!state.mouth.visible) {
        return;
    }
    constexpr Point mouth_pivot = {155, 134};
    draw_nose(context, {155, 126}, state.mouth, mouth_pivot);
    if (state.mouth.weight > 25) {
        draw_open_mouth(context, {155, 134}, 22, 172, state.mouth, mouth_pivot);
    } else {
        draw_cat_smile(context, {155, 134}, 16, state.mouth, mouth_pivot);
    }
    draw_whiskers(context, {53, 147}, {61, 147}, {56, 156}, {63, 158}, {250, 147}, {258, 147}, {248, 156}, {255, 158},
                  state.mouth, mouth_pivot);

    const lv_color_t bubble_fill  = LV_COLOR_MAKE(9, 47, 76);
    const lv_color_t bubble_glint = LV_COLOR_MAKE(140, 179, 207);
    draw_filled_circle(context, {139, 135}, 30, state.mouth, mouth_pivot, bubble_fill);
    draw_ring(context, {139, 135}, 15, 2, state.mouth, mouth_pivot, bubble_glint);
    const Point upper_glint[] = {{128, 128}, {130, 124}, {135, 122}, {140, 124}};
    const Point lower_glint[] = {{128, 138}, {134, 142}, {142, 142}, {149, 137}};
    draw_polyline(context, upper_glint, 2, state.mouth, mouth_pivot, bubble_glint);
    draw_polyline(context, lower_glint, 2, state.mouth, mouth_pivot, bubble_glint);
}

void draw_wink_mouth(const DrawContext& context, const CatFaceState& state)
{
    if (!state.mouth.visible) {
        return;
    }
    constexpr Point mouth_pivot = {154, 147};
    draw_nose(context, {154, 139}, state.mouth, mouth_pivot);
    if (state.mouth.weight > 25) {
        draw_open_mouth(context, {154, 147}, 20, 178, state.mouth, mouth_pivot);
    } else {
        draw_cat_smile(context, {154, 147}, 15, state.mouth, mouth_pivot);
    }
    draw_whiskers(context, {56, 164}, {64, 164}, {57, 174}, {65, 171}, {250, 138}, {258, 138}, {250, 148}, {258, 148},
                  state.mouth, mouth_pivot);
}

void draw_face(lv_event_t* event)
{
    auto* state = static_cast<CatFaceState*>(lv_event_get_user_data(event));
    if (!state) {
        return;
    }

    lv_obj_t* object = lv_event_get_target_obj(event);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    const DrawContext context = {lv_event_get_layer(event), area.x1, area.y1 - kFaceTop};

    switch (state->emotion) {
        case Emotion::Happy:
            draw_happy_eyes(context, *state);
            draw_happy_mouth(context, *state);
            break;
        case Emotion::Angry:
            draw_angry_eyes(context, *state);
            draw_angry_mouth(context, *state);
            break;
        case Emotion::Cute:
            draw_cute_eyes(context, *state);
            draw_cute_mouth(context, *state);
            break;
        case Emotion::Sad:
            draw_cute_eyes(context, *state);
            draw_sad_mouth(context, *state);
            break;
        case Emotion::Doubt:
        case Emotion::Dizzy:
            draw_dizzy_eyes(context, *state);
            draw_dizzy_mouth(context, *state);
            break;
        case Emotion::Sleepy:
            draw_sleepy_eyes(context, *state);
            draw_sleepy_mouth(context, *state);
            break;
        case Emotion::Wink:
            draw_wink_eyes(context, *state);
            draw_wink_mouth(context, *state);
            break;
        case Emotion::Neutral:
        default:
            draw_default_eyes(context, *state);
            draw_default_mouth(context, *state);
            break;
    }
}

}  // namespace

CatAvatar::~CatAvatar()
{
    if (_state) {
        _state->surface = nullptr;
    }
    if (_face && lv_obj_is_valid(_face)) {
        lv_obj_delete(_face);
    }
    _face = nullptr;
}

void CatAvatar::init(lv_obj_t* parent, const lv_font_t* font)
{
    _panel = std::make_unique<Container>(parent);
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setSize(320, 240);
    _panel->setRadius(0);
    _panel->setBorderWidth(0);
    _panel->setBgColor(lv_color_black());
    _panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _state = std::make_shared<CatFaceState>();
    _face  = lv_obj_create(_panel->get());
    lv_obj_remove_style_all(_face);
    lv_obj_set_size(_face, 320, kFaceHeight);
    lv_obj_align(_face, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(_face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(_face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_face, draw_face, LV_EVENT_DRAW_MAIN, _state.get());
    _state->surface = _face;

    _key_elements.leftEye  = std::make_unique<CatEye>(_state, true);
    _key_elements.rightEye = std::make_unique<CatEye>(_state, false);
    _key_elements.mouth    = std::make_unique<CatMouth>(_state);
    _key_elements.speechBubble =
        std::make_unique<CommonSpeechBubble>(_panel->get(), lv_color_white(), lv_color_black(), font);
}

}  // namespace stackchan::avatar
