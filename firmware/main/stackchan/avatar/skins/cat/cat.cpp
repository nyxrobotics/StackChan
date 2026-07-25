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
#include <iterator>
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

constexpr float kPi               = 3.14159265358979323846f;
constexpr int kFaceTop            = 40;
constexpr int kFaceHeight         = 160;
constexpr int kFaceBottom         = kFaceTop + kFaceHeight - 1;
constexpr float kFaceScale        = 1.24f;
constexpr Point kFaceSourceCenter = {157, 119};
constexpr Point kFaceTargetCenter = {160, 120};
static_assert(kFaceScale > 0.0f);

Point apply_face_layout(float x, float y)
{
    return {
        kFaceTargetCenter.x + static_cast<int>(std::lround((x - static_cast<float>(kFaceSourceCenter.x)) * kFaceScale)),
        kFaceTargetCenter.y + static_cast<int>(std::lround((y - static_cast<float>(kFaceSourceCenter.y)) * kFaceScale)),
    };
}

Point transform_point(Point point, Point pivot, const FeaturePose& pose)
{
    const int offset_x = pose.position.x * 16 / 100;
    const int offset_y = pose.position.y * 16 / 100;
    if (pose.size == 0 && (pose.rotation == 0 || pose.rotation == 3600)) {
        point = apply_face_layout(static_cast<float>(point.x), static_cast<float>(point.y));
        return {point.x + offset_x, point.y + offset_y};
    }

    const float scale  = 1.0f + static_cast<float>(pose.size) * 0.0025f;
    const float angle  = static_cast<float>(pose.rotation) * (kPi / 1800.0f);
    const float sine   = std::sin(angle);
    const float cosine = std::cos(angle);
    const float x      = static_cast<float>(point.x - pivot.x) * scale;
    const float y      = static_cast<float>(point.y - pivot.y) * scale;

    point = apply_face_layout(static_cast<float>(pivot.x) + x * cosine - y * sine,
                              static_cast<float>(pivot.y) + x * sine + y * cosine);
    return {point.x + offset_x, point.y + offset_y};
}

int transform_width(int width, const FeaturePose& pose)
{
    if (pose.size == 0) {
        return std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * kFaceScale)));
    }

    const float scale = (1.0f + static_cast<float>(pose.size) * 0.0025f) * kFaceScale;
    return std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * scale)));
}

void draw_line(const DrawContext& context, Point from, Point to, int width, lv_color_t color = lv_color_white(),
               bool rounded = true)
{
    lv_draw_line_dsc_t descriptor;
    lv_draw_line_dsc_init(&descriptor);
    descriptor.color       = color;
    descriptor.opa         = LV_OPA_COVER;
    descriptor.width       = width;
    descriptor.round_start = rounded ? 1 : 0;
    descriptor.round_end   = rounded ? 1 : 0;
    descriptor.p1          = {context.origin_x + from.x, context.origin_y + from.y};
    descriptor.p2          = {context.origin_x + to.x, context.origin_y + to.y};
    lv_draw_line(context.layer, &descriptor);
}

void draw_line(const DrawContext& context, Point from, Point to, int width, const FeaturePose& pose, Point pivot,
               lv_color_t color = lv_color_white(), bool rounded = true)
{
    draw_line(context, transform_point(from, pivot, pose), transform_point(to, pivot, pose),
              transform_width(width, pose), color, rounded);
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

template <size_t PointCount>
void draw_smooth_polyline(const DrawContext& context, const Point (&points)[PointCount], int width,
                          const FeaturePose& pose, Point pivot, lv_color_t color = lv_color_white(),
                          int subdivisions = 3)
{
    static_assert(PointCount >= 2);
    subdivisions = std::max(1, subdivisions);

    Point previous = points[0];
    for (size_t index = 0; index + 1 < PointCount; ++index) {
        const Point& p0 = points[index == 0 ? index : index - 1];
        const Point& p1 = points[index];
        const Point& p2 = points[index + 1];
        const Point& p3 = points[index + 2 < PointCount ? index + 2 : index + 1];

        for (int step = 1; step <= subdivisions; ++step) {
            const float t          = static_cast<float>(step) / static_cast<float>(subdivisions);
            const float t2         = t * t;
            const float t3         = t2 * t;
            const auto interpolate = [t, t2, t3](int v0, int v1, int v2, int v3) {
                return static_cast<int>(
                    std::lround(0.5f * (2.0f * v1 + (-v0 + v2) * t + (2.0f * v0 - 5.0f * v1 + 4.0f * v2 - v3) * t2 +
                                        (-v0 + 3.0f * v1 - 3.0f * v2 + v3) * t3)));
            };
            const Point current = {
                interpolate(p0.x, p1.x, p2.x, p3.x),
                interpolate(p0.y, p1.y, p2.y, p3.y),
            };
            if (current.x != previous.x || current.y != previous.y) {
                draw_line(context, previous, current, width, pose, pivot, color);
            }
            previous = current;
        }
    }
}

int normalize_angle(int angle)
{
    angle %= 360;
    return angle < 0 ? angle + 360 : angle;
}

void draw_arc(const DrawContext& context, Point center, int outer_radius, int start_angle, int end_angle, int width,
              const FeaturePose& pose, Point pivot, lv_color_t color = lv_color_white(), bool rounded = true)
{
    center                     = transform_point(center, pivot, pose);
    const int rotation_degrees = static_cast<int>(std::lround(static_cast<float>(pose.rotation) / 10.0f));

    lv_draw_arc_dsc_t descriptor;
    lv_draw_arc_dsc_init(&descriptor);
    descriptor.color       = color;
    descriptor.opa         = LV_OPA_COVER;
    descriptor.width       = transform_width(width, pose);
    descriptor.center      = {context.origin_x + center.x, context.origin_y + center.y};
    descriptor.radius      = static_cast<uint16_t>(std::max(1, transform_width(outer_radius, pose)));
    descriptor.start_angle = normalize_angle(start_angle + rotation_degrees);
    descriptor.end_angle   = normalize_angle(end_angle + rotation_degrees);
    descriptor.rounded     = rounded ? 1 : 0;
    lv_draw_arc(context.layer, &descriptor);
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

void draw_filled_circle(const DrawContext& context, Point center, int diameter, lv_color_t color = lv_color_white())
{
    const int half       = diameter / 2;
    const lv_area_t area = {
        context.origin_x + center.x - half,
        context.origin_y + center.y - half,
        context.origin_x + center.x - half + diameter - 1,
        context.origin_y + center.y - half + diameter - 1,
    };

    lv_draw_rect_dsc_t descriptor;
    lv_draw_rect_dsc_init(&descriptor);
    descriptor.bg_color = color;
    descriptor.bg_opa   = LV_OPA_COVER;
    descriptor.radius   = LV_RADIUS_CIRCLE;
    lv_draw_rect(context.layer, &descriptor, &area);
}

void draw_filled_circle(const DrawContext& context, Point center, int diameter, const FeaturePose& pose, Point pivot,
                        lv_color_t color = lv_color_white())
{
    draw_filled_circle(context, transform_point(center, pivot, pose), transform_width(diameter, pose), color);
}

FeaturePose without_position(FeaturePose pose)
{
    pose.position = {0, 0};
    return pose;
}

Point requested_screen_offset(const FeaturePose& pose)
{
    return {pose.position.x * 16 / 100, pose.position.y * 16 / 100};
}

struct BoundedCircle {
    Point center;
    float radius;
};

Point clamp_circle_group_offset(Point requested, Point boundary_center, float boundary_radius,
                                const BoundedCircle* circles, size_t circle_count)
{
    const float requested_x         = static_cast<float>(requested.x);
    const float requested_y         = static_cast<float>(requested.y);
    const float requested_magnitude = std::sqrt(requested_x * requested_x + requested_y * requested_y);
    if (requested_magnitude <= 0.0f) {
        return {0, 0};
    }

    const float unit_x = requested_x / requested_magnitude;
    const float unit_y = requested_y / requested_magnitude;
    const auto fits    = [&](Point offset) {
        for (size_t index = 0; index < circle_count; ++index) {
            const float limit = std::max(0.0f, boundary_radius - circles[index].radius);
            const float x     = static_cast<float>(circles[index].center.x + offset.x - boundary_center.x);
            const float y     = static_cast<float>(circles[index].center.y + offset.y - boundary_center.y);
            if (x * x + y * y > limit * limit) {
                return false;
            }
        }
        return true;
    };
    if (fits(requested)) {
        return requested;
    }

    float allowed = requested_magnitude;
    for (size_t index = 0; index < circle_count; ++index) {
        const float base_x       = static_cast<float>(circles[index].center.x - boundary_center.x);
        const float base_y       = static_cast<float>(circles[index].center.y - boundary_center.y);
        const float limit        = std::max(0.0f, boundary_radius - circles[index].radius);
        const float dot          = base_x * unit_x + base_y * unit_y;
        const float discriminant = dot * dot + limit * limit - (base_x * base_x + base_y * base_y);
        if (discriminant <= 0.0f) {
            allowed = 0.0f;
            break;
        }
        allowed = std::min(allowed, -dot + std::sqrt(discriminant));
    }

    // Keep the pair rigid. Recheck the actual rounded pixel offset because
    // rounding X and Y independently can otherwise cross the circular edge.
    for (allowed = std::max(0.0f, allowed); allowed > 0.0f; allowed -= 0.5f) {
        const Point candidate = {
            static_cast<int>(std::lround(unit_x * allowed)),
            static_cast<int>(std::lround(unit_y * allowed)),
        };
        if (fits(candidate)) {
            return candidate;
        }
    }
    return {0, 0};
}

struct CircularArc {
    Point from;
    Point to;
    Point center;
    float centerline_radius;
    int start_angle;
    int end_angle;
    bool use_line;
};

int angle_to_point(Point center, Point point)
{
    return normalize_angle(static_cast<int>(std::lround(
        std::atan2(static_cast<float>(point.y - center.y), static_cast<float>(point.x - center.x)) * (180.0f / kPi))));
}

CircularArc make_tangent_arc(Point from, Point to, float start_tangent)
{
    const float dx          = static_cast<float>(to.x - from.x);
    const float dy          = static_cast<float>(to.y - from.y);
    const float normal_x    = -std::sin(start_tangent);
    const float normal_y    = std::cos(start_tangent);
    const float denominator = 2.0f * (dx * normal_x + dy * normal_y);

    CircularArc arc = {from, to, from, 0.0f, 0, 0, true};
    if (std::fabs(denominator) < 0.001f) {
        return arc;
    }

    const float signed_radius = (dx * dx + dy * dy) / denominator;
    const float center_x      = static_cast<float>(from.x) + signed_radius * normal_x;
    const float center_y      = static_cast<float>(from.y) + signed_radius * normal_y;
    arc.center                = {
        static_cast<int>(std::lround(center_x)),
        static_cast<int>(std::lround(center_y)),
    };
    arc.centerline_radius = std::fabs(signed_radius);

    int from_angle     = angle_to_point(arc.center, from);
    int to_angle       = angle_to_point(arc.center, to);
    const int sweep    = normalize_angle(to_angle - from_angle);
    const int arc_span = std::min(sweep, 360 - sweep);
    if (sweep <= 180) {
        arc.start_angle = from_angle;
        arc.end_angle   = to_angle;
    } else {
        arc.start_angle = to_angle;
        arc.end_angle   = from_angle;
    }
    arc.use_line = arc.centerline_radius > 128.0f || arc_span < 3;
    return arc;
}

CircularArc mirror_arc(const CircularArc& arc, int axis_x)
{
    CircularArc mirrored = arc;
    const auto mirror    = [axis_x](Point point) {
        point.x = 2 * axis_x - point.x;
        return point;
    };
    mirrored.from        = mirror(arc.from);
    mirrored.to          = mirror(arc.to);
    mirrored.center      = mirror(arc.center);
    mirrored.start_angle = normalize_angle(180 - arc.end_angle);
    mirrored.end_angle   = normalize_angle(180 - arc.start_angle);
    return mirrored;
}

void draw_circular_arc(const DrawContext& context, const CircularArc& arc, int width, const FeaturePose& pose,
                       Point pivot)
{
    if (arc.use_line) {
        draw_line(context, arc.from, arc.to, width, pose, pivot, lv_color_white(), false);
        return;
    }

    const int outer_radius =
        std::max(1, static_cast<int>(std::lround(arc.centerline_radius + static_cast<float>(width) * 0.5f)));
    draw_arc(context, arc.center, outer_radius, arc.start_angle, arc.end_angle, width, pose, pivot, lv_color_white(),
             false);
}

struct ArcBoundary {
    Point center;
    int radius;
    int start_angle;
    int end_angle;
};

void draw_tapered_arc(const DrawContext& context, const ArcBoundary& upper, const ArcBoundary& lower, Point outer_cap,
                      int outer_cap_diameter, const FeaturePose& pose, Point pivot)
{
    constexpr int boundary_width = 10;
    const int upper_start        = std::min(upper.start_angle, upper.end_angle);
    const int upper_end          = std::max(upper.start_angle, upper.end_angle);
    const int lower_start        = std::min(lower.start_angle, lower.end_angle);
    const int lower_end          = std::max(lower.start_angle, lower.end_angle);

    // Each stroke grows toward the eye's interior. Their widths overlap across
    // the whole eye, so the space between the two arcs needs no separate fill.
    draw_arc(context, upper.center, upper.radius, upper_start, upper_end, boundary_width, pose, pivot);
    draw_arc(context, lower.center, lower.radius + boundary_width, lower_start, lower_end, boundary_width, pose, pivot);
    draw_filled_circle(context, outer_cap, outer_cap_diameter, pose, pivot);
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
    constexpr int width         = 4;
    const int centerline_radius = std::max(1, (half_width + 1) / 2);
    const int outer_radius      = centerline_radius + width / 2;

    draw_arc(context, {center.x - centerline_radius, center.y}, outer_radius, 0, 180, width, pose, pivot);
    draw_arc(context, {center.x + centerline_radius, center.y}, outer_radius, 0, 180, width, pose, pivot);
}

void draw_open_mouth(const DrawContext& context, Point top_center, int base_half_width, int base_bottom,
                     const FeaturePose& pose, Point pivot)
{
    constexpr int width                    = 4;
    constexpr int bottom_centerline_radius = 26;
    constexpr int bottom_outer_radius      = bottom_centerline_radius + width / 2;

    const float openness = std::clamp(static_cast<float>(pose.weight) / 65.0f, 0.35f, 1.23f);
    const int half_width = std::max(12, static_cast<int>(std::lround(base_half_width * (0.68f + 0.32f * openness))));
    int bottom           = std::max(
        top_center.y + 18,
        top_center.y + static_cast<int>(std::lround(static_cast<float>(base_bottom - top_center.y) * openness)));

    while (bottom > top_center.y + 18) {
        const Point transformed_center = transform_point({top_center.x, bottom - 24}, pivot, pose);
        const int transformed_radius   = transform_width(bottom_outer_radius, pose);
        if (transformed_center.y + transformed_radius - 1 <= kFaceBottom) {
            break;
        }
        --bottom;
    }

    const Point left_top     = {top_center.x - half_width + 2, top_center.y + 5};
    const Point left_turn    = {top_center.x - half_width + 1, std::max(top_center.y + 15, bottom - 12)};
    const Point left_bottom  = {top_center.x - 10, bottom};
    const Point right_bottom = {top_center.x + 10, bottom};

    const auto chord_angle = [](Point from, Point to) {
        return std::atan2(static_cast<float>(to.y - from.y), static_cast<float>(to.x - from.x));
    };
    const float upper_chord   = chord_angle(left_top, left_turn);
    const float lower_chord   = chord_angle(left_turn, left_bottom);
    const float bottom_chord  = std::atan2(2.0f, 10.0f);
    const float lower_tangent = 2.0f * lower_chord - 2.0f * bottom_chord;
    const float upper_tangent = 2.0f * upper_chord - lower_tangent;

    const CircularArc left_upper = make_tangent_arc(left_top, left_turn, upper_tangent);
    const CircularArc left_lower = make_tangent_arc(left_turn, left_bottom, lower_tangent);
    const CircularArc bottom_arc = {
        left_bottom,
        right_bottom,
        {top_center.x, bottom - 24},
        static_cast<float>(bottom_centerline_radius),
        angle_to_point({top_center.x, bottom - 24}, right_bottom),
        angle_to_point({top_center.x, bottom - 24}, left_bottom),
        false,
    };
    const CircularArc right_lower = mirror_arc(left_lower, top_center.x);
    const CircularArc right_upper = mirror_arc(left_upper, top_center.x);

    draw_circular_arc(context, left_upper, width, pose, pivot);
    draw_circular_arc(context, left_lower, width, pose, pivot);
    draw_circular_arc(context, bottom_arc, width, pose, pivot);
    draw_circular_arc(context, right_lower, width, pose, pivot);
    draw_circular_arc(context, right_upper, width, pose, pivot);

    draw_filled_circle(context, left_turn, width, pose, pivot);
    draw_filled_circle(context, left_bottom, width, pose, pivot);
    draw_filled_circle(context, right_bottom, width, pose, pivot);
    draw_filled_circle(context, {2 * top_center.x - left_turn.x, left_turn.y}, width, pose, pivot);

    // Paint the smile last so it hides the outline's flat top caps.
    draw_cat_smile(context, top_center, half_width, pose, pivot);
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

void draw_default_eye_contents(const DrawContext& context, Point center, const FeaturePose& pose)
{
    constexpr int eye_diameter       = 74;
    constexpr int pupil_diameter     = 52;
    constexpr lv_color_t pupil_color = LV_COLOR_MAKE(0, 0, 0);
    const FeaturePose frame          = without_position(pose);
    const Point frame_center         = transform_point(center, center, frame);
    const int eye_size               = transform_width(eye_diameter, frame);
    const int pupil_size             = transform_width(pupil_diameter, frame);
    const BoundedCircle pupil        = {frame_center, static_cast<float>(pupil_size) * 0.5f};
    // LVGL centers even-diameter raster circles half a pixel left/up.
    const Point offset = clamp_circle_group_offset(requested_screen_offset(pose), frame_center,
                                                   static_cast<float>(eye_size) * 0.5f - 1.0f, &pupil, 1);

    draw_filled_circle(context, frame_center, eye_size);
    draw_filled_circle(context, {frame_center.x + offset.x, frame_center.y + offset.y}, pupil_size, pupil_color);
}

void draw_default_eyes(const DrawContext& context, const CatFaceState& state)
{
    constexpr Point left_center   = {102, 120};
    constexpr Point right_center  = {211, 120};
    const FeaturePose left_frame  = without_position(state.left_eye);
    const FeaturePose right_frame = without_position(state.right_eye);

    if (state.left_eye.visible) {
        draw_line(context, {125, 68}, {135, 68}, 9, left_frame, left_center);
        if (is_blinking(state.left_eye)) {
            draw_closed_blink(context, left_center, left_frame);
        } else {
            draw_default_eye_contents(context, left_center, state.left_eye);
        }
    }
    if (state.right_eye.visible) {
        draw_line(context, {178, 68}, {188, 68}, 9, right_frame, right_center);
        if (is_blinking(state.right_eye)) {
            draw_closed_blink(context, right_center, right_frame);
        } else {
            draw_default_eye_contents(context, right_center, state.right_eye);
        }
    }
}

void draw_happy_eyes(const DrawContext& context, const CatFaceState& state)
{
    constexpr Point left_center       = {102, 106};
    constexpr Point right_center      = {214, 106};
    constexpr ArcBoundary left_upper  = {{89, 135}, 50, 250, 335};
    constexpr ArcBoundary left_lower  = {{96, 135}, 33, 241, 337};
    constexpr ArcBoundary right_upper = {{225, 136}, 51, 289, 207};
    constexpr ArcBoundary right_lower = {{218, 135}, 33, 298, 202};

    if (state.left_eye.visible) {
        draw_arc(context, {131, 72}, 10, 218, 322, 8, state.left_eye, left_center);
        if (is_blinking(state.left_eye)) {
            draw_closed_blink(context, {102, 111}, state.left_eye);
        } else {
            draw_tapered_arc(context, left_upper, left_lower, {76, 97}, 20, state.left_eye, left_center);
        }
    }
    if (state.right_eye.visible) {
        draw_arc(context, {184, 72}, 11, 220, 320, 8, state.right_eye, right_center);
        if (is_blinking(state.right_eye)) {
            draw_closed_blink(context, {214, 111}, state.right_eye);
        } else {
            draw_tapered_arc(context, right_upper, right_lower, {238, 97}, 20, state.right_eye, right_center);
        }
    }
}

void draw_squeezed_eyes(const DrawContext& context, const CatFaceState& state)
{
    constexpr Point left_center  = {99, 122};
    constexpr Point right_center = {218, 122};
    constexpr int width          = 13;

    if (state.left_eye.visible) {
        draw_line(context, {125, 68}, {135, 68}, 9, state.left_eye, left_center);
        draw_line(context, {64, 92}, {132, 122}, width, state.left_eye, left_center);
        draw_line(context, {68, 151}, {132, 122}, width, state.left_eye, left_center);
    }
    if (state.right_eye.visible) {
        draw_line(context, {178, 68}, {188, 68}, 9, state.right_eye, right_center);
        draw_line(context, {185, 122}, {253, 92}, width, state.right_eye, right_center);
        draw_line(context, {185, 122}, {249, 151}, width, state.right_eye, right_center);
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

void draw_cute_eye_contents(const DrawContext& context, Point center, Point large_highlight, Point small_highlight,
                            const FeaturePose& pose)
{
    constexpr int eye_outer_radius         = 37;
    constexpr int eye_width                = 5;
    constexpr int large_highlight_diameter = 24;
    constexpr int small_highlight_diameter = 8;
    const FeaturePose frame                = without_position(pose);
    const Point frame_center               = transform_point(center, center, frame);
    const int large_size                   = transform_width(large_highlight_diameter, frame);
    const int small_size                   = transform_width(small_highlight_diameter, frame);
    const BoundedCircle highlights[]       = {
        {transform_point(large_highlight, center, frame), static_cast<float>(large_size) * 0.5f},
        {transform_point(small_highlight, center, frame), static_cast<float>(small_size) * 0.5f},
    };
    // Keep one pixel for the ring's even-area raster-center offset.
    const float inner_radius =
        static_cast<float>(transform_width(eye_outer_radius, frame) - transform_width(eye_width, frame)) - 1.0f;
    const Point offset = clamp_circle_group_offset(requested_screen_offset(pose), frame_center, inner_radius,
                                                   highlights, std::size(highlights));

    draw_ring(context, center, eye_outer_radius, eye_width, frame, center);
    draw_filled_circle(context, {highlights[0].center.x + offset.x, highlights[0].center.y + offset.y}, large_size);
    draw_filled_circle(context, {highlights[1].center.x + offset.x, highlights[1].center.y + offset.y}, small_size);
}

void draw_cute_eyes(const DrawContext& context, const CatFaceState& state)
{
    constexpr Point left_center   = {105, 110};
    constexpr Point right_center  = {214, 110};
    const FeaturePose left_frame  = without_position(state.left_eye);
    const FeaturePose right_frame = without_position(state.right_eye);

    if (state.left_eye.visible) {
        draw_arc(context, {133, 72}, 11, 220, 320, 8, left_frame, left_center);
        if (is_blinking(state.left_eye)) {
            draw_closed_blink(context, left_center, left_frame);
        } else {
            draw_cute_eye_contents(context, left_center, {94, 98}, {122, 126}, state.left_eye);
        }
    }
    if (state.right_eye.visible) {
        draw_arc(context, {187, 72}, 10, 218, 322, 8, right_frame, right_center);
        if (is_blinking(state.right_eye)) {
            draw_closed_blink(context, right_center, right_frame);
        } else {
            draw_cute_eye_contents(context, right_center, {203, 98}, {231, 126}, state.right_eye);
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
        draw_line(context, {128, 68}, {137, 66}, 9, state.left_eye, left_center);
        if (is_blinking(state.left_eye)) {
            draw_closed_blink(context, left_center, state.left_eye);
        } else {
            draw_ring(context, left_center, 37, 5, state.left_eye, left_center);
            draw_smooth_polyline(context, left_spiral, 5, state.left_eye, left_center, lv_color_white(), 3);
        }
    }
    if (state.right_eye.visible) {
        draw_line(context, {181, 66}, {190, 68}, 9, state.right_eye, right_center);
        if (is_blinking(state.right_eye)) {
            draw_closed_blink(context, right_center, state.right_eye);
        } else {
            draw_ring(context, right_center, 37, 5, state.right_eye, right_center);
            draw_smooth_polyline(context, right_spiral, 5, state.right_eye, right_center, lv_color_white(), 3);
        }
    }
}

void draw_sleepy_eyes(const DrawContext& context, const CatFaceState& state)
{
    constexpr Point left_center  = {101, 116};
    constexpr Point right_center = {211, 116};
    if (state.left_eye.visible) {
        draw_arc(context, {129, 88}, 16, 243, 297, 9, state.left_eye, left_center);
        draw_line(context, {72, 120}, {129, 112}, 16, state.left_eye, left_center);
    }
    if (state.right_eye.visible) {
        draw_arc(context, {182, 87}, 16, 243, 297, 9, state.right_eye, right_center);
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
        draw_arc(context, {119, 74}, 10, 218, 322, 9, state.left_eye, left_center);
        if (is_blinking(state.left_eye)) {
            draw_closed_blink(context, left_center, state.left_eye);
        } else {
            draw_default_eye_contents(context, left_center, state.left_eye);
        }
    }
    if (state.right_eye.visible) {
        draw_arc(context, {172, 67}, 11, 213, 303, 9, state.right_eye, right_center);
        draw_line(context, {177, 120}, {224, 89}, 14, state.right_eye, right_center);
        draw_line(context, {177, 120}, {224, 126}, 14, state.right_eye, right_center);
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
        draw_line(context, {159, 155}, {159, 163}, 3, state.mouth, mouth_pivot);
        draw_arc(context, {153, 165}, 8, 345, 138, 3, state.mouth, mouth_pivot);
        draw_arc(context, {165, 166}, 8, 31, 206, 3, state.mouth, mouth_pivot);
    }
    const Point left_top[]     = {{57, 151}, {59, 149}, {62, 152}, {65, 150}};
    const Point left_bottom[]  = {{57, 162}, {59, 160}, {62, 163}, {65, 161}};
    const Point right_top[]    = {{252, 150}, {255, 152}, {258, 149}, {260, 151}};
    const Point right_bottom[] = {{252, 161}, {255, 163}, {258, 160}, {260, 162}};
    draw_smooth_polyline(context, left_top, 3, state.mouth, mouth_pivot);
    draw_smooth_polyline(context, left_bottom, 3, state.mouth, mouth_pivot);
    draw_smooth_polyline(context, right_top, 3, state.mouth, mouth_pivot);
    draw_smooth_polyline(context, right_bottom, 3, state.mouth, mouth_pivot);
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
    draw_arc(context, {135, 129}, 8, 189, 314, 2, state.mouth, mouth_pivot, bubble_glint);
    draw_arc(context, {138, 129}, 14, 36, 139, 2, state.mouth, mouth_pivot, bubble_glint);
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
            draw_squeezed_eyes(context, *state);
            draw_default_mouth(context, *state);
            break;
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
