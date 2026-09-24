#include "cmotionblendpreview.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
double fadeDuration(const std::optional<double>& value, double inherited)
{
    return value && *value >= 0.0 ? *value : inherited;
}

double fadeWeight(double elapsed, double duration)
{
    if (duration == 0.0) return 1.0;
    const double ratio = std::clamp(elapsed / duration, 0.0, 1.0);
    return 0.5 - 0.5 * std::cos(ratio * std::acos(-1.0));
}

float blendValue(double source, double target, double weight)
{
    const double value = source * (1.0 - weight) + target * weight;
    constexpr double limit = std::numeric_limits<float>::max();
    return static_cast<float>(std::clamp(value, -limit, limit));
}

bool validId(const QString& id)
{
    return !id.isEmpty() && !id.contains(QChar::Null);
}
}

CMotionPreviewFrame CMotionBlendPreview::Evaluate(const CMotionTimeline& timeline, double time,
                                                 const QMap<QString, float>& baseline,
                                                 const QSet<QString>& eyeBlinkIds,
                                                 const QSet<QString>& lipSyncIds,
                                                 bool loop)
{
    if (!std::isfinite(time)) return {};
    for (auto it = baseline.cbegin(); it != baseline.cend(); ++it)
        if (!validId(it.key()) || !std::isfinite(it.value())) return {};
    for (const auto* ids : {&eyeBlinkIds, &lipSyncIds})
        for (const auto& id : *ids)
            if (!validId(id)) return {};

    const double duration = timeline.Timing().duration;
    time = std::max(0.0, time);
    if (loop && duration > 0.0) time = std::fmod(time, duration);
    else if (duration > 0.0) time = std::min(time, duration);
    const float sampleTime = static_cast<float>(std::min(time, static_cast<double>(std::numeric_limits<float>::max())));
    CMotionPreviewFrame frame = timeline.EvaluateFrame(sampleTime);
    if (!frame.IsFinite()) return {};

    const auto globalFades = timeline.FadeTimes();
    const double fadeIn = fadeDuration(globalFades.fadeIn, 1.0);
    const double fadeOut = fadeDuration(globalFades.fadeOut, 1.0);
    const auto weightFor = [&](double in, double out) {
        return fadeWeight(time, in) * (loop || duration <= 0.0 ? 1.0 : fadeWeight(duration - time, out));
    };
    const double globalWeight = weightFor(fadeIn, fadeOut);
    const auto trackFades = timeline.TrackFadeTimes();
    const auto eye = frame.model.constFind(QStringLiteral("EyeBlink"));
    const auto lip = frame.model.constFind(QStringLiteral("LipSync"));
    const QSet<QString> explicitParameters(frame.parameters.keyBegin(), frame.parameters.keyEnd());

    for (auto it = frame.parameters.begin(); it != frame.parameters.end(); ++it) {
        double target = it.value();
        if (eye != frame.model.cend() && eyeBlinkIds.contains(it.key())) target *= eye.value();
        if (lip != frame.model.cend() && lipSyncIds.contains(it.key())) target += lip.value();
        const auto fades = trackFades.value({CMotionTarget::Parameter, it.key()});
        const double weight = weightFor(fadeDuration(fades.fadeIn, fadeIn), fadeDuration(fades.fadeOut, fadeOut));
        it.value() = blendValue(baseline.value(it.key()), target, weight);
    }
    if (eye != frame.model.cend()) {
        for (const auto& id : eyeBlinkIds)
            if (!explicitParameters.contains(id))
                frame.parameters.insert(id, blendValue(baseline.value(id), eye.value(), globalWeight));
    }
    if (lip != frame.model.cend()) {
        for (const auto& id : lipSyncIds)
            if (!explicitParameters.contains(id))
                frame.parameters.insert(id, blendValue(frame.parameters.value(id, baseline.value(id)), lip.value(), globalWeight));
    }
    frame.model.remove(QStringLiteral("EyeBlink"));
    frame.model.remove(QStringLiteral("LipSync"));
    return frame;
}
