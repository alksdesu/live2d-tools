// Copyright(c) Live2D Inc. All rights reserved.
// Use of this source code is governed by the Live2D Open Software license
// that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.

#include "cmotionbezier.h"
#include <cmath>

namespace {
constexpr float pi = 3.1415926535897932384626433832795f;
constexpr float epsilon = .00001f;
float range(float value, float minimum, float maximum)
{
    if (value < minimum) value = minimum;
    else if (value > maximum) value = maximum;
    return value;
}
float quadratic(float a, float b, float c)
{
    if (std::fabs(a) < epsilon) {
        if (std::fabs(b) < epsilon) return -c;
        return -c / b;
    }
    return -(b + sqrtf(b * b - 4.0f * a * c)) / (2.0f * a);
}
float cardano(float a, float b, float c, float d)
{
    if (std::fabs(a) < epsilon) return range(quadratic(b, c, d), 0.0f, 1.0f);
    const float ba = b / a, ca = c / a, da = d / a;
    const float p = (3.0f * ca - ba * ba) / 3.0f;
    const float p3 = p / 3.0f;
    const float q = (2.0f * ba * ba * ba - 9.0f * ba * ca + 27.0f * da) / 27.0f;
    const float q2 = q / 2.0f;
    const float discriminant = q2 * q2 + p3 * p3 * p3;
    const float center = .5f, threshold = center + .01f;
    if (discriminant < 0.0f) {
        const float mp3 = -p / 3.0f;
        const float mp33 = mp3 * mp3 * mp3;
        const float r = sqrtf(mp33);
        const float t = -q / (2.0f * r);
        const float cosphi = range(t, -1.0f, 1.0f);
        const float phi = acos(cosphi);
        const float crtr = cbrt(r);
        const float t1 = 2.0f * crtr;
        const float root1 = t1 * cosf(phi / 3.0f) - ba / 3.0f;
        if (std::abs(root1 - center) < threshold) return range(root1, 0.0f, 1.0f);
        const float root2 = t1 * cosf((phi + 2.0f * pi) / 3.0f) - ba / 3.0f;
        if (std::abs(root2 - center) < threshold) return range(root2, 0.0f, 1.0f);
        const float root3 = t1 * cosf((phi + 4.0f * pi) / 3.0f) - ba / 3.0f;
        return range(root3, 0.0f, 1.0f);
    }
    if (discriminant == 0.0f) {
        const float u1 = q2 < 0.0f ? cbrt(-q2) : -cbrt(q2);
        const float root1 = 2.0f * u1 - ba / 3.0f;
        if (std::abs(root1 - center) < threshold) return range(root1, 0.0f, 1.0f);
        const float root2 = -u1 - ba / 3.0f;
        return range(root2, 0.0f, 1.0f);
    }
    const float sd = sqrtf(discriminant);
    const float u1 = cbrt(sd - q2);
    const float v1 = cbrt(sd + q2);
    const float root1 = u1 - v1 - ba / 3.0f;
    return range(root1, 0.0f, 1.0f);
}
float lerp(float a, float b, float t) { return a + ((b - a) * t); }
}

float CMotionBezier::Parameter(float start, float first, float second, float end, float time)
{
    const float a = end - 3.0f * second + 3.0f * first - start;
    const float b = 3.0f * second - 6.0f * first + 3.0f * start;
    const float c = 3.0f * first - 3.0f * start;
    const float d = start - time;
    return cardano(a, b, c, d);
}

float CMotionBezier::Evaluate(const QPointF& start, const QPointF& first, const QPointF& second,
                              const QPointF& end, float time, bool restricted)
{
    float t;
    if (restricted) {
        t = (time - static_cast<float>(start.x())) / (static_cast<float>(end.x()) - static_cast<float>(start.x()));
        if (t < 0.0f) t = 0.0f;
    } else t = Parameter(start.x(), first.x(), second.x(), end.x(), time);
    const float firstPair = lerp(start.y(), first.y(), t);
    const float middlePair = lerp(first.y(), second.y(), t);
    const float lastPair = lerp(second.y(), end.y(), t);
    return lerp(lerp(firstPair, middlePair, t), lerp(middlePair, lastPair, t), t);
}
