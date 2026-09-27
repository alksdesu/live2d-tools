#include "conionskin.h"
#include "cmotionblendpreview.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QJsonDocument>
#include <QIODevice>
#include <algorithm>
#include <cmath>
#include <limits>

namespace live2dstruct {
struct COnionSkin::Timeline::Data
{
    CMotionTimeline timeline;
    QByteArray fingerprint;
};

namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool validId(const QString& id)
{
    return !id.isEmpty() && !id.contains(QChar::Null);
}

bool validBase(const COnionSkin::Base& base)
{
    if (!std::isfinite(base.modelOpacity)) return false;
    for (const auto* values : {&base.parameters, &base.parts})
        for (auto it = values->cbegin(); it != values->cend(); ++it)
            if (!validId(it.key()) || !std::isfinite(it.value())) return false;
    for (const auto* ids : {&base.eyeBlinkIds, &base.lipSyncIds, &base.hiddenParts})
        for (const auto& id : *ids)
            if (!validId(id)) return false;
    return true;
}

QSet<QString> knownEffects(const QSet<QString>& ids, const QMap<QString, float>& parameters)
{
    QSet<QString> result;
    for (const auto& id : ids)
        if (parameters.contains(id)) result.insert(id);
    return result;
}

QMap<QString, float> copyValues(const QMap<QString, float>& values)
{
    QMap<QString, float> result;
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        result.insert(QString(it.key().constData(), it.key().size()), it.value());
    return result;
}

QByteArray fingerprint(const COnionSkin::Request& request, const COnionSkin::Plan& plan)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << request.timeline.Fingerprint() << request.sourceRevision << request.useFades << request.loop;
    stream << quint32(plan.samples.size()) << qint32(plan.omitted);
    for (const auto& sample : plan.samples)
        stream << sample.time << qint32(sample.offset) << sample.opacity
               << sample.frame.parameters << sample.frame.parts << sample.frame.model;
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
}

bool COnionSkin::Settings::operator==(const Settings& other) const
{
    return enabled == other.enabled && duringPlayback == other.duringPlayback && before == other.before
        && after == other.after && stepFrames == other.stepFrames && beforeOpacity == other.beforeOpacity
        && afterOpacity == other.afterOpacity;
}

bool COnionSkin::Status::operator==(const Status& other) const
{
    return state == other.state && message == other.message && sampleTimes == other.sampleTimes
        && omitted == other.omitted && requestedSize == other.requestedSize && renderedSize == other.renderedSize
        && reducedResolution == other.reducedResolution && generation == other.generation;
}

QByteArray COnionSkin::Timeline::Fingerprint() const
{
    return m_data ? m_data->fingerprint : QByteArray();
}

bool COnionSkin::Validate(const Settings& settings, QString* error)
{
    if (settings.before < 0 || settings.before > MaximumPerSide || settings.after < 0
        || settings.after > MaximumPerSide || settings.stepFrames < 1
        || !std::isfinite(settings.beforeOpacity) || !std::isfinite(settings.afterOpacity)
        || settings.beforeOpacity < 0 || settings.beforeOpacity > 1
        || settings.afterOpacity < 0 || settings.afterOpacity > 1)
        return fail(error, QStringLiteral("洋葱皮的帧数、间隔或透明度无效"));
    return true;
}

bool COnionSkin::CaptureTimeline(const CMotionTimeline& timeline, Timeline& result, QString* error)
{
    QJsonObject state;
    if (!timeline.SaveState(&state, timeline.Loop(), 0, error)) return false;
    const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
    auto candidate = std::make_shared<Timeline::Data>();
    if (!candidate->timeline.RestoreState(QJsonDocument::fromJson(bytes).object(), nullptr, error)) return false;
    candidate->fingerprint = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    result.m_data = std::move(candidate);
    return true;
}

bool COnionSkin::Build(const Request& request, const Base& base, Plan& result, QString* error)
{
    if (!Validate(request.settings, error)) return false;
    Plan candidate;
    if (!request.settings.enabled || request.suspended || request.recording
        || (request.playing && !request.settings.duringPlayback)) {
        candidate.fingerprint = fingerprint(request, candidate);
        result = std::move(candidate);
        return true;
    }
    if (!request.timeline.IsValid() || !validBase(base))
        return fail(error, QStringLiteral("洋葱皮的动画或基础姿态无效"));
    const auto& timeline = request.timeline.m_data->timeline;
    const auto timing = timeline.Timing();
    if (!std::isfinite(request.time) || request.time < 0 || request.time > std::numeric_limits<float>::max()
        || !std::isfinite(timing.fps) || timing.fps <= 0 || !std::isfinite(timing.duration)
        || timing.duration < 0 || timing.duration > std::numeric_limits<float>::max()
        || (!request.loop && request.time > timing.duration))
        return fail(error, QStringLiteral("洋葱皮的时间范围无效"));
    if (!timeline.HasCurves() || timing.duration == 0) {
        candidate.fingerprint = fingerprint(request, candidate);
        result = std::move(candidate);
        return true;
    }
    const double cursor = request.loop ? std::fmod(request.time, timing.duration) : request.time;
    const double step = static_cast<double>(request.settings.stepFrames) / timing.fps;
    if (!std::isfinite(step) || step <= 0)
        return fail(error, QStringLiteral("洋葱皮的采样间隔超出范围"));
    const auto eyeBlinkIds = knownEffects(base.eyeBlinkIds, base.parameters);
    const auto lipSyncIds = knownEffects(base.lipSyncIds, base.parameters);
    const auto sampleKey = [&](double time) { return request.useFades ? time : double(static_cast<float>(time)); };
    QSet<double> sampledTimes{sampleKey(cursor)};
    for (int distance = 1; distance <= std::max(request.settings.before, request.settings.after); ++distance) {
        for (int direction : {-1, 1}) {
            if (distance > (direction < 0 ? request.settings.before : request.settings.after)) continue;
            const float opacity = direction < 0 ? request.settings.beforeOpacity : request.settings.afterOpacity;
            if (opacity == 0) continue;
            double time = cursor + direction * distance * step;
            if (!std::isfinite(time)) return fail(error, QStringLiteral("洋葱皮的采样时间超出范围"));
            if (request.loop) {
                time = std::fmod(time, timing.duration);
                if (time < 0) time += timing.duration;
                if (time >= timing.duration) time = 0;
            } else if (time < 0 || time > timing.duration) {
                ++candidate.omitted;
                continue;
            }
            const float sampleTime = static_cast<float>(time);
            if (sampledTimes.contains(sampleKey(time))) {
                ++candidate.omitted;
                continue;
            }
            sampledTimes.insert(sampleKey(time));
            const auto evaluated = request.useFades
                ? CMotionBlendPreview::Evaluate(timeline, time, base.parameters, eyeBlinkIds, lipSyncIds, request.loop)
                : timeline.EvaluateFrame(sampleTime);
            if (!evaluated.IsFinite()) return fail(error, QStringLiteral("洋葱皮的曲线求值无效"));
            Sample sample;
            sample.time = time;
            sample.offset = direction * distance;
            sample.opacity = opacity;
            sample.frame.parameters = copyValues(base.parameters);
            sample.frame.parts = copyValues(base.parts);
            sample.frame.model.insert(QStringLiteral("Opacity"), std::clamp(base.modelOpacity, 0.f, 1.f));
            const auto parameters = request.useFades ? evaluated.parameters
                : evaluated.ResolveParameters(eyeBlinkIds, lipSyncIds);
            for (auto it = parameters.cbegin(); it != parameters.cend(); ++it)
                if (sample.frame.parameters.contains(it.key())) sample.frame.parameters[it.key()] = it.value();
            for (auto it = evaluated.parts.cbegin(); it != evaluated.parts.cend(); ++it)
                if (sample.frame.parts.contains(it.key())) sample.frame.parts[it.key()] = it.value();
            for (auto it = sample.frame.parts.begin(); it != sample.frame.parts.end(); ++it)
                it.value() = base.hiddenParts.contains(it.key()) ? 0.f : std::clamp(it.value(), 0.f, 1.f);
            if (evaluated.model.contains(QStringLiteral("Opacity")))
                sample.frame.model[QStringLiteral("Opacity")] = std::clamp(evaluated.model.value(QStringLiteral("Opacity")), 0.f, 1.f);
            candidate.samples.append(std::move(sample));
        }
    }
    std::stable_sort(candidate.samples.begin(), candidate.samples.end(), [](const Sample& a, const Sample& b) {
        return std::abs(a.offset) > std::abs(b.offset);
    });
    candidate.fingerprint = fingerprint(request, candidate);
    result = std::move(candidate);
    return true;
}

bool COnionSkin::SelectResolution(const QSize& requested, int maximumDimension, qint64 reservedBytes,
                                QSize& result, QString* error)
{
    if (requested.width() <= 0 || requested.height() <= 0 || maximumDimension <= 0
        || reservedBytes < 0 || reservedBytes > MaximumGpuBytes - 12)
        return fail(error, QStringLiteral("洋葱皮的画布尺寸或显存预算无效"));
    const qint64 availablePixels = (MaximumGpuBytes - reservedBytes) / 12;
    const double width = requested.width(), height = requested.height();
    const double scale = std::min({1.0, maximumDimension / width, maximumDimension / height,
                                   std::sqrt(availablePixels / (width * height))});
    const qint64 outputWidth = static_cast<qint64>(std::floor(width * scale));
    const qint64 outputHeight = static_cast<qint64>(std::floor(height * scale));
    if (outputWidth <= 0 || outputHeight <= 0 || outputWidth > maximumDimension || outputHeight > maximumDimension
        || outputWidth * outputHeight > availablePixels)
        return fail(error, QStringLiteral("洋葱皮的显存预算不足以容纳此画布比例"));
    result = QSize(static_cast<int>(outputWidth), static_cast<int>(outputHeight));
    return true;
}
}
