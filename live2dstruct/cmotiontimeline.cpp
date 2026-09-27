#include "cmotiontimeline.h"
#include "cmotionbezier.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <exception>
#include <set>

namespace {
constexpr qint64 maximumMotionBytes = 16 * 1024 * 1024;
constexpr int maximumMotionPoints = CMotionTimeline::MaximumPointCount;
constexpr int maximumMotionEvents = 100000;
constexpr int maximumEventOccurrences = 10000;

quint64 completeCycles(double advance, double duration, bool* saturated)
{
    if (advance >= std::ldexp(duration, 64)) {
        *saturated = true;
        return std::numeric_limits<quint64>::max();
    }
    quint64 count = 0;
    for (int bit = 63; bit >= 0; --bit) {
        const double span = std::ldexp(duration, bit);
        if (advance < span) continue;
        advance -= span;
        count |= quint64(1) << bit;
    }
    return count;
}

void addOccurrences(quint64 count, quint64 repeat, quint64* total, bool* saturated)
{
    if (count == 0 || repeat == 0) return;
    const auto maximum = std::numeric_limits<quint64>::max();
    if (count > (maximum - *total) / repeat) {
        *total = maximum;
        *saturated = true;
    } else {
        *total += count * repeat;
    }
}

bool finiteFloat(double value)
{
    return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
}

QVector<QPointF> defaultBezierControls(const QPointF& start, const QPointF& end)
{
    return {QPointF(start.x() + (end.x() - start.x()) / 3.0,
                    start.y() + (end.y() - start.y()) / 3.0),
            QPointF(start.x() + (end.x() - start.x()) * 2.0 / 3.0,
                    start.y() + (end.y() - start.y()) * 2.0 / 3.0)};
}

QVector<QPointF> canonicalRestrictedControls(const QVector<QPointF>& controls, const QPointF& start, const QPointF& end)
{
    auto result = defaultBezierControls(start, end);
    if (controls.size() == 2) for (int index = 0; index < 2; ++index) result[index].setY(controls[index].y());
    return result;
}

bool validBezierControls(const QVector<QPointF>& controls, const QPointF& start, const QPointF& end)
{
    return controls.size() == 2 && std::isfinite(controls.at(0).x()) && std::isfinite(controls.at(0).y()) &&
           std::isfinite(controls.at(1).x()) && std::isfinite(controls.at(1).y()) &&
           controls.at(0).x() >= start.x() && controls.at(0).x() <= end.x() &&
           controls.at(1).x() >= start.x() && controls.at(1).x() <= end.x();
}

bool validMotionPoint(const QPointF& point)
{
    return finiteFloat(point.x()) && point.x() >= 0 && finiteFloat(point.y());
}

bool retargetBatchBezier(const QVector<QPointF>& controls, const QPointF& oldStart,
                         const QPointF& oldEnd, const QPointF& start, const QPointF& end,
                         QVector<QPointF>& result)
{
    if (oldStart == start && oldEnd == end) { result = controls; return true; }
    if (controls.size() != 2 || oldEnd.x() <= oldStart.x()) return false;
    QVector<QPointF> candidate;
    for (int i = 0; i < 2; ++i) {
        const double ratio = qBound(0.0, (controls[i].x() - oldStart.x()) / (oldEnd.x() - oldStart.x()), 1.0);
        const double offset = i == 0 ? start.y() - oldStart.y() : end.y() - oldEnd.y();
        const QPointF point(start.x() + ratio * (end.x() - start.x()), controls[i].y() + offset);
        if (!validMotionPoint(point)) return false;
        candidate.append(point);
    }
    result.swap(candidate);
    return true;
}

struct ParsedCurve
{
    QVector<QPointF> points;
    QVector<int> types;
    QVector<QVector<QPointF>> controls;
    int totalPoints = 1;
};

bool parseCurve(const QJsonObject& curve, bool restricted, ParsedCurve* result, bool draft = false)
{
    const QJsonValue raw = curve.value(QStringLiteral("Segments"));
    if (!raw.isArray()) return false;
    const QJsonArray segments = raw.toArray();
    if (segments.size() < (draft ? 2 : 5) || segments.size() > maximumMotionPoints * 7) return false;
    bool valid = true;
    const auto number = [&segments, &valid](int index) {
        const auto value = segments.at(index);
        const double parsed = value.toDouble();
        if (!value.isDouble() || !finiteFloat(parsed)) valid = false;
        return parsed;
    };
    const double time = number(0);
    const double value = number(1);
    if (!valid || time < 0.0) return false;
    result->points.append(QPointF(time, value));
    result->types.append(0);
    result->controls.append(QVector<QPointF>());
    int index = 2;
    while (index < segments.size()) {
        const double typeNumber = number(index++);
        if (!valid || typeNumber < 0 || typeNumber > 3 || std::floor(typeNumber) != typeNumber) return false;
        const int type = static_cast<int>(typeNumber);
        const int count = type == 1 ? 3 : 1;
        if (index + count * 2 > segments.size()) return false;
        QVector<QPointF> segmentPoints;
        for (int point = 0; point < count; ++point) {
            const double pointTime = number(index++);
            const double pointValue = number(index++);
            if (!valid || pointTime < 0.0) return false;
            segmentPoints.append(QPointF(pointTime, pointValue));
        }
        const QPointF endpoint = segmentPoints.last();
        if (static_cast<float>(endpoint.x()) <= static_cast<float>(result->points.last().x())) return false;
        const QVector<QPointF> controls = type == 1
            ? QVector<QPointF>{segmentPoints.at(0), segmentPoints.at(1)} : QVector<QPointF>{};
        if (type == 1 && !restricted && !validBezierControls(controls, result->points.last(), endpoint)) return false;
        result->points.append(endpoint);
        result->types.append(type);
        result->controls.append(controls);
        result->totalPoints += count;
        if (result->points.size() > maximumMotionPoints) return false;
    }
    return true;
}

bool validFadeTimes(const QJsonObject& object)
{
    for (const QString& field : {QStringLiteral("FadeInTime"), QStringLiteral("FadeOutTime")}) {
        if (!object.contains(field)) continue;
        const auto value = object.value(field);
        if (!value.isDouble() || !finiteFloat(value.toDouble())) return false;
    }
    return true;
}

bool validFadeTimes(const CMotionFadeTimes& fades)
{
    return (!fades.fadeIn || finiteFloat(*fades.fadeIn)) &&
           (!fades.fadeOut || finiteFloat(*fades.fadeOut));
}

CMotionFadeTimes readFadeTimes(const QJsonObject& object)
{
    CMotionFadeTimes fades;
    if (object.contains(QStringLiteral("FadeInTime"))) fades.fadeIn = object.value(QStringLiteral("FadeInTime")).toDouble();
    if (object.contains(QStringLiteral("FadeOutTime"))) fades.fadeOut = object.value(QStringLiteral("FadeOutTime")).toDouble();
    return fades;
}

void writeFadeTimes(QJsonObject* object, const CMotionFadeTimes& fades)
{
    if (fades.fadeIn) object->insert(QStringLiteral("FadeInTime"), *fades.fadeIn);
    else object->remove(QStringLiteral("FadeInTime"));
    if (fades.fadeOut) object->insert(QStringLiteral("FadeOutTime"), *fades.fadeOut);
    else object->remove(QStringLiteral("FadeOutTime"));
}

bool validEvents(const QJsonArray& events, QString* error)
{
    if (events.size() > maximumMotionEvents) {
        if (error) *error = QStringLiteral("motion3 事件过多");
        return false;
    }
    qint64 bytes = 0;
    for (const auto& value : events) {
        const auto event = value.toObject();
        const auto time = event.value(QStringLiteral("Time"));
        const auto text = event.value(QStringLiteral("Value"));
        if (!value.isObject() || !time.isDouble() || !finiteFloat(time.toDouble()) || time.toDouble() < 0.0 ||
            !text.isString() || text.toString().contains(QChar::Null)) {
            if (error) *error = QStringLiteral("motion3 事件无效");
            return false;
        }
        bytes += text.toString().toUtf8().size();
        if (bytes > maximumMotionBytes) {
            if (error) *error = QStringLiteral("motion3 事件文本过大");
            return false;
        }
    }
    return true;
}
}

CMotionTimeline::CMotionTimeline(std::function<qint64()> recordingClock)
    : m_recordingClock(std::move(recordingClock)) {}

void CMotionTimeline::SwapContent(CMotionTimeline& other) noexcept
{
    m_source.swap(other.m_source);
    m_cachedEvents.swap(other.m_cachedEvents);
    m_sortedEvents.swap(other.m_sortedEvents);
    m_curves.swap(other.m_curves);
    m_segmentTypes.swap(other.m_segmentTypes);
    m_bezierControls.swap(other.m_bezierControls);
    m_tangents.swap(other.m_tangents);
}

bool CMotionTimeline::ParameterTracks::IsEmpty() const
{
    for (const auto& track : m_tracks) if (track.present || track.sourceIndex >= 0) return false;
    return true;
}

QSet<QString> CMotionTimeline::ParameterTracks::Ids() const
{
    QSet<QString> ids;
    for (auto it = m_tracks.cbegin(); it != m_tracks.cend(); ++it) ids.insert(it.key());
    return ids;
}

bool CMotionTimeline::RemoveParameterTracks(const QSet<QString>& ids, ParameterTracks& removed, QString* error)
{
    if (error) error->clear();
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    try {
        if (ids.size() > maximumMotionPoints) return fail(QStringLiteral("待移除参数轨道数量过多"));
        CMotionTimeline candidate = *this;
        ParameterTracks archive;
        archive.m_restricted = AreBeziersRestricted();
        const double duration = Timing().duration;
        for (const auto& id : ids) {
            if (id.isEmpty() || id.contains(QChar::Null)) return fail(QStringLiteral("参数轨道 ID 无效"));
            const CMotionTrack key{CMotionTarget::Parameter, id};
            ParameterTracks::Track record;
            record.present = m_curves.contains(key);
            record.segmentsPresent = m_segmentTypes.contains(key);
            record.controlsPresent = m_bezierControls.contains(key);
            record.points = m_curves.value(key);
            record.segments = m_segmentTypes.value(key);
            record.controls = m_bezierControls.value(key);
            record.tangents = m_tangents.value(key);
            archive.m_tracks.insert(id, std::move(record));
            candidate.m_curves.remove(key);
            candidate.m_segmentTypes.remove(key);
            candidate.m_bezierControls.remove(key);
            candidate.m_tangents.remove(key);
        }
        const auto sourceCurves = m_source.value(QStringLiteral("Curves")).toArray();
        QJsonArray retained;
        bool sourceChanged = false;
        for (int index = 0; index < sourceCurves.size(); ++index) {
            const auto curve = sourceCurves[index].toObject();
            const auto id = curve.value(QStringLiteral("Id")).toString();
            if (curve.value(QStringLiteral("Target")) == QJsonValue(QStringLiteral("Parameter")) && ids.contains(id)) {
                auto& record = archive.m_tracks[id];
                if (record.sourceIndex >= 0) return fail(QStringLiteral("源参数曲线 ID 重复：%1").arg(id));
                record.source = curve;
                record.sourceIndex = index;
                sourceChanged = true;
            } else retained.append(sourceCurves[index]);
        }
        if (sourceChanged) candidate.m_source.insert(QStringLiteral("Curves"), retained);
        if (!std::isfinite(duration)) return fail(QStringLiteral("动画时长无效"));
        if (candidate.Timing().duration < duration) {
            auto meta = candidate.m_source.value(QStringLiteral("Meta")).toObject();
            archive.m_metaPresent = candidate.m_source.contains(QStringLiteral("Meta"));
            archive.m_durationBefore = meta.value(QStringLiteral("Duration"));
            archive.m_durationAfter = duration;
            archive.m_promotedDuration = true;
            meta.insert(QStringLiteral("Duration"), duration);
            candidate.m_source.insert(QStringLiteral("Meta"), meta);
        }
        if (!candidate.ValidateEditableState(error)) return false;
        *this = std::move(candidate);
        removed = std::move(archive);
        return true;
    } catch (const std::exception&) { return fail(QStringLiteral("无法准备参数轨道移除")); }
}

bool CMotionTimeline::RestoreParameterTracks(const ParameterTracks& tracks, QString* error)
{
    if (error) error->clear();
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    try {
        CMotionTimeline candidate = *this;
        if (!tracks.m_restricted && AreBeziersRestricted()) {
            bool bezier = false;
            for (const auto& track : tracks.m_tracks) bezier = bezier || (track.present && track.segments.contains(1));
            if (bezier) candidate.UnrestrictBezierControls();
        }
        auto curves = m_source.value(QStringLiteral("Curves")).toArray();
        QSet<QString> sourceIds;
        for (const auto& value : curves) {
            const auto curve = value.toObject();
            if (curve.value(QStringLiteral("Target")) == QJsonValue(QStringLiteral("Parameter")))
                sourceIds.insert(curve.value(QStringLiteral("Id")).toString());
        }
        QVector<QPair<int, QJsonObject>> sources;
        double requiredDuration = 0;
        for (auto it = tracks.m_tracks.cbegin(); it != tracks.m_tracks.cend(); ++it) {
            const CMotionTrack key{CMotionTarget::Parameter, it.key()};
            if (m_curves.contains(key) || sourceIds.contains(it.key()))
                return fail(QStringLiteral("同名参数轨道已重新创建，无法覆盖：%1").arg(it.key()));
            if (it->present) {
                candidate.m_curves.insert(key, it->points);
                candidate.m_tangents.insert(key, it->tangents);
                if (!it->points.isEmpty()) requiredDuration = qMax(requiredDuration, it->points.last().x());
            }
            if (it->segmentsPresent) candidate.m_segmentTypes.insert(key, it->segments);
            if (it->controlsPresent) candidate.m_bezierControls.insert(key, it->controls);
            if (tracks.m_restricted && !candidate.AreBeziersRestricted() && it->present) {
                auto& controls = candidate.m_bezierControls[key];
                controls.resize(it->points.size());
                for (int index = 1; index < it->points.size(); ++index)
                    if (it->segments.value(index) == 1)
                        controls[index] = canonicalRestrictedControls(it->controls.value(index), it->points[index - 1], it->points[index]);
                auto& tangents = candidate.m_tangents[key];
                for (int index = 0; index < tangents.size(); ++index) {
                    auto& tangent = tangents[index];
                    if (tangent.mode != TangentMode::Aligned && tangent.mode != TangentMode::Flat) continue;
                    if (it->segments.value(index) == 1) tangent.incomingFraction = 1.0 / 3;
                    if (it->segments.value(index + 1) == 1) tangent.outgoingFraction = 1.0 / 3;
                }
            }
            if (it->sourceIndex >= 0) sources.append(qMakePair(it->sourceIndex, it->source));
        }
        const auto currentMeta = m_source.value(QStringLiteral("Meta")).toObject();
        if (currentMeta.value(QStringLiteral("Duration")).isDouble() && Timing().duration < requiredDuration)
            return fail(QStringLiteral("当前动画时长短于待恢复轨道，请先增加时长"));
        std::sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& source : sources) curves.insert(qMin(source.first, int(curves.size())), source.second);
        if (!sources.isEmpty()) candidate.m_source.insert(QStringLiteral("Curves"), curves);
        if (tracks.m_promotedDuration && currentMeta.value(QStringLiteral("Duration")) == tracks.m_durationAfter) {
            auto meta = candidate.m_source.value(QStringLiteral("Meta")).toObject();
            if (tracks.m_durationBefore.isUndefined()) meta.remove(QStringLiteral("Duration"));
            else meta.insert(QStringLiteral("Duration"), tracks.m_durationBefore);
            if (!tracks.m_metaPresent && meta.isEmpty()) candidate.m_source.remove(QStringLiteral("Meta"));
            else candidate.m_source.insert(QStringLiteral("Meta"), meta);
        }
        if (!candidate.ValidateEditableState(error)) return false;
        *this = std::move(candidate);
        return true;
    } catch (const std::exception&) { return fail(QStringLiteral("无法准备参数轨道恢复")); }
}

std::optional<qint64> CMotionTimeline::RecordingElapsedMilliseconds() const
{
    qint64 elapsed = 0;
    if (m_recordingClock) {
        qint64 now;
        try { now = m_recordingClock(); }
        catch (...) { return std::nullopt; }
        if (now < m_recordClockOrigin || (m_recordClockOrigin < 0 &&
            now > std::numeric_limits<qint64>::max() + m_recordClockOrigin)) return std::nullopt;
        elapsed = now - m_recordClockOrigin;
    } else {
        if (!m_clock.isValid()) return std::nullopt;
        elapsed = m_clock.elapsed();
    }
    return elapsed >= m_recordLastMs ? std::optional<qint64>(elapsed) : std::nullopt;
}

void CMotionTimeline::StartRecording()
{
    qint64 origin = 0;
    if (m_recordingClock) {
        try { origin = m_recordingClock(); }
        catch (...) { return; }
    }
    m_recordStartTime = Duration();
    m_recordClockOrigin = origin;
    m_recordSkippedMs = m_recordPauseMs = m_recordLastMs = 0;
    m_recordClockPaused = false;
    m_recording = true;
    m_clock.start();
}

void CMotionTimeline::StopRecording()
{
    m_recording = false;
    m_recordClockPaused = false;
    m_recordSkippedMs = m_recordPauseMs = m_recordLastMs = 0;
    m_recordClockOrigin = 0;
    m_clock.invalidate();
}

bool CMotionTimeline::PauseRecordingClock()
{
    if (!m_recording || m_recordClockPaused) return false;
    const auto elapsed = RecordingElapsedMilliseconds();
    if (!elapsed || *elapsed < m_recordSkippedMs) return false;
    m_recordPauseMs = m_recordLastMs = *elapsed;
    m_recordClockPaused = true;
    return true;
}

bool CMotionTimeline::ResumeRecordingClock()
{
    if (!m_recording || !m_recordClockPaused) return false;
    const auto elapsed = RecordingElapsedMilliseconds();
    if (!elapsed || *elapsed < m_recordPauseMs || m_recordSkippedMs > m_recordPauseMs) return false;
    m_recordSkippedMs += *elapsed - m_recordPauseMs;
    m_recordLastMs = *elapsed;
    m_recordClockPaused = false;
    return true;
}

void CMotionTimeline::Clear()
{
    StopRecording();
    m_source = {};
    m_cachedEvents = {};
    m_sortedEvents = {};
    m_curves.clear();
    m_segmentTypes.clear();
    m_bezierControls.clear();
    m_tangents.clear();
}

void CMotionTimeline::Record(const QString& parameterId, float value)
{
    RecordValues({{parameterId, value}});
}

bool CMotionTimeline::RecordValues(const QMap<QString, float>& values, QString* error)
{
    if (error) error->clear();
    const auto fail = [error](const QString& message) { if (error) *error = message; return false; };
    if (!m_recording || m_recordClockPaused) return fail(QStringLiteral("参数录制未开始或录制时钟已暂停"));
    if (values.isEmpty()) return fail(QStringLiteral("没有待录制的参数"));
    try {
        for (auto value = values.cbegin(); value != values.cend(); ++value)
            if (value.key().trimmed().isEmpty() || value.key().contains(QChar::Null) || !std::isfinite(value.value()))
                return fail(QStringLiteral("录制参数的 ID 或数值无效"));
        const auto elapsed = RecordingElapsedMilliseconds();
        if (!elapsed || *elapsed < m_recordSkippedMs) return fail(QStringLiteral("参数录制时钟无效"));
        const double sampleTime = double(m_recordStartTime) + double(*elapsed - m_recordSkippedMs) / 1000.0;
        if (!finiteFloat(sampleTime) || sampleTime < 0) return fail(QStringLiteral("参数录制时间超出范围"));
        float time = static_cast<float>(sampleTime);
        float maximumLast = -1;
        bool unchanged = true;
        for (auto value = values.cbegin(); value != values.cend(); ++value) {
            const auto curve = m_curves.constFind({CMotionTarget::Parameter, value.key()});
            if (curve == m_curves.cend() || curve->isEmpty()) { unchanged = false; continue; }
            const auto last = curve->last();
            maximumLast = std::max(maximumLast, static_cast<float>(last.x()));
            if (time > static_cast<float>(last.x()) || value.value() != static_cast<float>(last.y())) unchanged = false;
        }
        if (unchanged) { m_recordLastMs = *elapsed; return true; }
        if (qint64(TotalPointCount()) + values.size() > MaximumPointCount)
            return fail(QStringLiteral("录制后关键帧总数超过 100000"));
        if (time <= maximumLast) time = std::nextafterf(maximumLast, std::numeric_limits<float>::infinity());
        if (!std::isfinite(time)) return fail(QStringLiteral("无法为整组参数分配共同的下一采样时间"));
        auto curves = m_curves;
        auto segmentTypes = m_segmentTypes;
        auto controls = m_bezierControls;
        auto tangents = m_tangents;
        for (auto value = values.cbegin(); value != values.cend(); ++value) {
            const CMotionTrack track{CMotionTarget::Parameter, value.key()};
            curves[track].append({time, value.value()});
            segmentTypes[track].append(0);
            controls[track].append(QVector<QPointF>{});
            tangents[track].append(TangentState{});
        }
        auto candidate = *this;
        candidate.m_curves.swap(curves); candidate.m_segmentTypes.swap(segmentTypes); candidate.m_bezierControls.swap(controls); candidate.m_tangents.swap(tangents);
        if (candidate.HasAuthoringTangents() && !candidate.ValidateEditableState(error)) return false;
        SwapContent(candidate);
        m_recordLastMs = *elapsed;
        return true;
    } catch (...) { return fail(QStringLiteral("无法录制整组参数，原关键点保持不变")); }
}

bool CMotionTimeline::AddPoint(const CMotionTrack& track, float time, float value)
{
    if (track.TargetName().isEmpty() || track.id.trimmed().isEmpty() || track.id.contains(QChar::Null) || !std::isfinite(time) || time < 0.0f || !std::isfinite(value)) return false;
    if (TotalPointCount() >= maximumMotionPoints) return false;
    auto candidate = *this;
    QVector<QPointF>& points = candidate.m_curves[track];
    QVector<int>& segmentTypes = candidate.m_segmentTypes[track];
    QVector<QVector<QPointF>>& bezierControls = candidate.m_bezierControls[track];
    if (!points.isEmpty()) {
        const float lastTime = static_cast<float>(points.last().x());
        time = qMax(time, lastTime);
        if (time == lastTime) {
            if (value == static_cast<float>(points.last().y())) return false;
            time = std::nextafterf(lastTime, std::numeric_limits<float>::infinity());
        }
    }
    if (!std::isfinite(time)) return false;
    points.append(QPointF(time, value));
    segmentTypes.append(0);
    bezierControls.append(QVector<QPointF>());
    candidate.m_tangents[track].append(TangentState{});
    if (candidate.HasAuthoringTangents() && !candidate.ValidateEditableState(nullptr)) return false;
    SwapContent(candidate); return true;
}

bool CMotionTimeline::ReplaceParameterSamples(const QMap<QString, QVector<QPointF>>& samples, QString* error,
                                              bool resetFades)
{
    if (error) error->clear();
    const auto fail = [error](const QString& message) { if (error) *error = message; return false; };
    if (m_recording || samples.isEmpty()) return fail(QStringLiteral("请结束录制并提供待写入的参数曲线"));
    try {
        qint64 count = TotalPointCount();
        for (auto curve = samples.cbegin(); curve != samples.cend(); ++curve) {
            if (curve.key().trimmed().isEmpty() || curve.key().contains(QChar::Null) || curve->isEmpty())
                return fail(QStringLiteral("参数曲线 ID 为空或没有采样点"));
            count -= m_curves.value({CMotionTarget::Parameter, curve.key()}).size();
            count += curve->size();
        }
        if (count > MaximumPointCount) return fail(QStringLiteral("写入后关键帧总数超过 100000"));
        for (auto curve = samples.cbegin(); curve != samples.cend(); ++curve) {
            float previous = -1;
            for (const auto& point : *curve) {
                if (!finiteFloat(point.x()) || !finiteFloat(point.y()) || point.x() < 0 ||
                    static_cast<float>(point.x()) <= previous)
                    return fail(QStringLiteral("采样时间必须递增，且时间与参数值必须为有限数值：%1").arg(curve.key()));
                previous = static_cast<float>(point.x());
            }
        }
        const double previousDuration = Timing().duration;
        CMotionTimeline candidate = *this;
        for (auto curve = samples.cbegin(); curve != samples.cend(); ++curve) {
            const CMotionTrack track{CMotionTarget::Parameter, curve.key()};
            candidate.m_curves.insert(track, *curve);
            candidate.m_segmentTypes.insert(track, QVector<int>(curve->size(), 0));
            candidate.m_bezierControls.insert(track, QVector<QVector<QPointF>>(curve->size()));
            candidate.m_tangents.insert(track, QVector<TangentState>(curve->size()));
        }
        if (resetFades) {
            auto curves = candidate.m_source.value(QStringLiteral("Curves")).toArray();
            QSet<QString> updated;
            for (int i = 0; i < curves.size(); ++i) {
                auto curve = curves.at(i).toObject();
                const auto id = curve.value(QStringLiteral("Id")).toString();
                if (curve.value(QStringLiteral("Target")).toString() != QStringLiteral("Parameter") ||
                    !samples.contains(id)) continue;
                writeFadeTimes(&curve, {0.0, 0.0});
                curves.replace(i, curve);
                updated.insert(id);
            }
            for (auto sample = samples.cbegin(); sample != samples.cend(); ++sample) {
                if (updated.contains(sample.key())) continue;
                curves.append(QJsonObject{{QStringLiteral("Target"), QStringLiteral("Parameter")},
                    {QStringLiteral("Id"), sample.key()}, {QStringLiteral("FadeInTime"), 0.0},
                    {QStringLiteral("FadeOutTime"), 0.0}});
            }
            candidate.m_source.insert(QStringLiteral("Curves"), curves);
        }
        if (candidate.Timing().duration < previousDuration) {
            auto meta = candidate.m_source.value(QStringLiteral("Meta")).toObject();
            meta.insert(QStringLiteral("Duration"), previousDuration);
            candidate.m_source.insert(QStringLiteral("Meta"), meta);
        }
        if (!candidate.ValidateEditableState(error)) return false;
        SwapContent(candidate);
        return true;
    } catch (const std::exception&) {
        return fail(QStringLiteral("无法准备参数采样曲线，原动画保持不变"));
    }
}

bool CMotionTimeline::AreBeziersRestricted() const
{
    return m_source.value(QStringLiteral("Meta")).toObject().value(QStringLiteral("AreBeziersRestricted")).toBool();
}

QVector<QPointF> CMotionTimeline::BezierControls(const CMotionTrack& track, int pointIndex) const
{
    const auto curve = m_curves.constFind(track);
    if (curve == m_curves.cend() || pointIndex <= 0 || pointIndex >= curve->size() || SegmentType(track, pointIndex) != 1) return {};
    const auto controls = m_bezierControls.value(track).value(pointIndex);
    const auto defaults = defaultBezierControls(curve->at(pointIndex - 1), curve->at(pointIndex));
    if (AreBeziersRestricted()) return canonicalRestrictedControls(controls, curve->at(pointIndex - 1), curve->at(pointIndex));
    return validBezierControls(controls, curve->at(pointIndex - 1), curve->at(pointIndex)) ? controls : defaults;
}

void CMotionTimeline::UnrestrictBezierControls()
{
    for (auto curve = m_curves.cbegin(); curve != m_curves.cend(); ++curve) {
        auto& controls = m_bezierControls[curve.key()];
        controls.resize(curve->size());
        for (int index = 1; index < curve->size(); ++index)
            if (SegmentType(curve.key(), index) == 1)
                controls[index] = canonicalRestrictedControls(controls[index], curve->at(index - 1), curve->at(index));
    }
    for (auto curve = m_tangents.begin(); curve != m_tangents.end(); ++curve)
        for (int index = 0; index < curve->size(); ++index) {
            auto& tangent = (*curve)[index];
            if (tangent.mode != TangentMode::Aligned && tangent.mode != TangentMode::Flat) continue;
            if (SegmentType(curve.key(), index) == 1) tangent.incomingFraction = 1.0 / 3;
            if (SegmentType(curve.key(), index + 1) == 1) tangent.outgoingFraction = 1.0 / 3;
        }
    auto meta = m_source.value(QStringLiteral("Meta")).toObject();
    meta.insert(QStringLiteral("AreBeziersRestricted"), false);
    m_source.insert(QStringLiteral("Meta"), meta);
}

bool CMotionTimeline::ConvertToUnrestricted(QString* error)
{
    if (error) error->clear();
    const auto fail = [error](const QString& message) { if (error) *error = message; return false; };
    if (m_recording) return fail(QStringLiteral("请结束录制后再转换曲线"));
    if (!AreBeziersRestricted()) return true;
    try {
        auto candidate = *this;
        candidate.UnrestrictBezierControls();
        if (!candidate.ValidateEditableState(error)) return false;
        SwapContent(candidate);
        return true;
    } catch (const std::exception&) { return fail(QStringLiteral("无法转换曲线，原动画保持不变")); }
}

bool CMotionTimeline::EditPoints(const BatchEdit& request, BatchResult& result, QString* error)
{
    if (error) error->clear();
    const auto fail = [error](const QString& message) { if (error) *error = message; return false; };
    if (m_recording) return fail(QStringLiteral("请结束录制后再编辑关键帧"));
    if (request.updates.size() > MaximumPointCount || request.removes.size() > MaximumPointCount ||
        request.insertions.size() > MaximumPointCount || request.segments.size() > MaximumPointCount ||
        request.tangents.size() > MaximumPointCount || request.handles.size() > MaximumPointCount ||
        (request.transform && request.transform->selection.size() > MaximumPointCount) ||
        request.updates.size() + request.removes.size() > MaximumPointCount)
        return fail(QStringLiteral("待编辑的关键帧数量超过限制"));
    try {
        const auto validTrack = [](const CMotionTrack& track) {
            return !track.TargetName().isEmpty() && !track.id.trimmed().isEmpty() && !track.id.contains(QChar::Null);
        };
        const auto validRef = [&](const CMotionPointRef& ref) {
            const auto curve = m_curves.constFind(ref.track);
            return validTrack(ref.track) && curve != m_curves.cend() && ref.index >= 0 && ref.index < curve->size();
        };
        QMap<CMotionPointRef, QPointF> updates;
        QMap<CMotionPointRef, SegmentEdit> segments;
        QMap<CMotionPointRef, TangentModeEdit> tangentEdits;
        QMap<CMotionPointRef, HandleEdit> handleEdits;
        QSet<CMotionPointRef> removes, transformed;
        std::set<CMotionTrack> affected;
        for (const auto& update : request.updates) {
            if (!validRef(update.ref) || !validMotionPoint(update.point) || updates.contains(update.ref))
                return fail(QStringLiteral("待移动的关键帧已失效、重复或坐标无效"));
            updates.insert(update.ref, update.point);
            affected.insert(update.ref.track);
        }
        if (request.transform) {
            const auto& transform = *request.transform;
            if (!std::isfinite(transform.timeScale) || transform.timeScale <= 0 || !std::isfinite(transform.valueScale) ||
                !std::isfinite(transform.pivot.x()) || !std::isfinite(transform.pivot.y()) || !std::isfinite(transform.offset.x()) || !std::isfinite(transform.offset.y()))
                return fail(QStringLiteral("关键帧仿射变换参数无效"));
            for (const auto& ref : transform.selection) {
                if (!validRef(ref) || transformed.contains(ref) || updates.contains(ref)) return fail(QStringLiteral("变换关键点已失效或重复"));
                const auto original = m_curves[ref.track][ref.index];
                const QPointF point(transform.pivot.x() + (original.x() - transform.pivot.x()) * transform.timeScale + transform.offset.x(),
                                    transform.pivot.y() + (original.y() - transform.pivot.y()) * transform.valueScale + transform.offset.y());
                if (!validMotionPoint(point)) return fail(QStringLiteral("变换后的关键点超出运行时数值范围"));
                transformed.insert(ref); updates.insert(ref, point); affected.insert(ref.track);
            }
        }
        for (const auto& ref : request.removes) {
            if (!validRef(ref) || removes.contains(ref) || updates.contains(ref))
                return fail(QStringLiteral("待删除的关键帧已失效、重复或同时被移动"));
            removes.insert(ref);
            affected.insert(ref.track);
        }
        for (const auto& segment : request.segments) {
            if (!validRef(segment.ref) || segments.contains(segment.ref) || removes.contains(segment.ref) ||
                segment.type < 0 || segment.type > 3 || (segment.controls.size() != 0 && segment.controls.size() != 2) ||
                (!segment.controls.isEmpty() && segment.type != 1))
                return fail(QStringLiteral("待编辑的曲线段已失效、重复或插值无效"));
            for (const auto& point : segment.controls)
                if (!finiteFloat(point.x()) || !finiteFloat(point.y())) return fail(QStringLiteral("Bézier 控制柄坐标无效"));
            segments.insert(segment.ref, segment);
            affected.insert(segment.ref.track);
        }
        for (const auto& edit : request.tangents) {
            if (!validRef(edit.ref) || tangentEdits.contains(edit.ref) || removes.contains(edit.ref) || !ValidateTangent(TangentState{edit.mode}, error) ||
                (edit.referenceSide && *edit.referenceSide != HandleSide::Incoming && *edit.referenceSide != HandleSide::Outgoing))
                return fail(QStringLiteral("切线模式请求重复、已失效或无效"));
            tangentEdits.insert(edit.ref, edit); affected.insert(edit.ref.track);
        }
        for (const auto& edit : request.handles) {
            if (!validRef(edit.ref) || handleEdits.contains(edit.ref) || tangentEdits.contains(edit.ref) || removes.contains(edit.ref) ||
                (edit.side != HandleSide::Incoming && edit.side != HandleSide::Outgoing) || !validMotionPoint(edit.position))
                return fail(QStringLiteral("切线手柄请求重复、冲突或无效"));
            handleEdits.insert(edit.ref, edit); affected.insert(edit.ref.track);
        }
        QMap<CMotionTrack, QVector<int>> insertions;
        const bool restricted = AreBeziersRestricted();
        for (int index = 0; index < request.insertions.size(); ++index) {
            const auto& insertion = request.insertions[index];
            if (!validTrack(insertion.track) || !validMotionPoint(insertion.point) ||
                insertion.segmentType < 0 || insertion.segmentType > 3 ||
                (insertion.controls.size() != 0 && insertion.controls.size() != 2) ||
                (!insertion.controls.isEmpty() && (insertion.segmentType != 1 || !insertion.segmentStart)))
                return fail(QStringLiteral("待插入的关键帧、插值或控制柄无效"));
            if (insertion.tangent && !ValidateTangent(*insertion.tangent, error)) return false;
            if (insertion.segmentStart && (!validMotionPoint(*insertion.segmentStart) ||
                static_cast<float>(insertion.segmentStart->x()) >= static_cast<float>(insertion.point.x())))
                return fail(QStringLiteral("待插入曲线的起点必须早于终点"));
            for (const auto& control : insertion.controls)
                if (!validMotionPoint(control)) return fail(QStringLiteral("待插入的控制柄坐标无效"));
            if (!insertion.controls.isEmpty() && !restricted &&
                !validBezierControls(insertion.controls, *insertion.segmentStart, insertion.point))
                return fail(QStringLiteral("Bézier 控制柄时间必须位于原曲线段内"));
            insertions[insertion.track].append(index);
            affected.insert(insertion.track);
        }
        if (qint64(TotalPointCount()) - removes.size() + request.insertions.size() > MaximumPointCount)
            return fail(QStringLiteral("编辑后关键帧总数超过 100000"));
        BatchResult output;
        output.inserted.resize(request.insertions.size());
        for (auto curve = m_curves.cbegin(); curve != m_curves.cend(); ++curve) {
            auto& mapping = output.oldToNew[curve.key()];
            mapping.resize(curve->size());
            for (int index = 0; index < curve->size(); ++index) mapping[index] = index;
        }
        auto candidate = *this;
        for (const auto& track : affected) {
            struct Entry { QPointF point; int oldIndex = -1; int insertion = -1; };
            QVector<Entry> entries;
            const auto before = m_curves.value(track);
            auto& mapping = output.oldToNew[track];
            mapping.fill(-1, before.size());
            entries.reserve(before.size() + insertions.value(track).size());
            for (int index = 0; index < before.size(); ++index) {
                const CMotionPointRef ref{track, index};
                if (!removes.contains(ref)) entries.append({updates.value(ref, before[index]), index, -1});
            }
            for (const int index : insertions.value(track)) entries.append({request.insertions[index].point, -1, index});
            std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) { return left.point.x() < right.point.x(); });
            QVector<QPointF> points;
            QVector<int> types;
            QVector<QVector<QPointF>> controls;
            QVector<TangentState> tangents;
            points.reserve(entries.size()); types.reserve(entries.size()); controls.reserve(entries.size());
            float previous = -1;
            for (int index = 0; index < entries.size(); ++index) {
                const auto& entry = entries[index];
                const float time = static_cast<float>(entry.point.x());
                if (time <= previous) return fail(QStringLiteral("轨道 %1 的关键帧时间重叠或低于运行时精度").arg(track.id));
                previous = time;
                if (entry.oldIndex >= 0) mapping[entry.oldIndex] = index;
                else output.inserted[entry.insertion] = {track, index};
                int type = 0;
                QVector<QPointF> handles;
                const auto segment = segments.constFind({track, entry.oldIndex});
                if (entry.oldIndex >= 0 && segment != segments.cend()) {
                    if (index == 0) return fail(QStringLiteral("首个关键帧没有可编辑的入段"));
                    type = segment->type;
                    if (type == 1) {
                        handles = defaultBezierControls(entries[index - 1].point, entry.point);
                        if (restricted) {
                            const auto stored = m_bezierControls.value(track).value(entry.oldIndex);
                            for (int handle = 0; handle < 2; ++handle) {
                                if (stored.size() == 2) handles[handle].setX(stored[handle].x());
                                if (!segment->controls.isEmpty()) handles[handle].setY(segment->controls[handle].y());
                            }
                        } else if (!segment->controls.isEmpty()) {
                            if (!validBezierControls(segment->controls, entries[index - 1].point, entry.point))
                                return fail(QStringLiteral("Bézier 控制柄时间必须位于最终曲线段内"));
                            handles = segment->controls;
                        }
                    }
                } else if (index > 0 && entry.oldIndex > 0) {
                    type = SegmentType(track, entry.oldIndex);
                    if (type == 1) {
                        const auto stored = m_bezierControls.value(track).value(entry.oldIndex);
                        const auto original = restricted && stored.size() == 2 ? stored : BezierControls(track, entry.oldIndex);
                        if (!retargetBatchBezier(original, before[entry.oldIndex - 1], before[entry.oldIndex],
                            entries[index - 1].point, entry.point, handles))
                            return fail(QStringLiteral("移动后 Bézier 控制柄超出数值范围"));
                    }
                } else if (index > 0 && entry.insertion >= 0) {
                    const auto& insertion = request.insertions[entry.insertion];
                    type = insertion.segmentType;
                    if (type == 1) {
                        const auto start = insertion.segmentStart.value_or(entries[index - 1].point);
                        const auto original = insertion.controls.isEmpty() ? defaultBezierControls(start, insertion.point) : insertion.controls;
                        if (!retargetBatchBezier(original, start, insertion.point, entries[index - 1].point, entry.point, handles))
                            return fail(QStringLiteral("插入后 Bézier 控制柄超出数值范围"));
                    }
                }
                auto tangent = entry.oldIndex >= 0 ? Tangent(track, entry.oldIndex) : request.insertions[entry.insertion].tangent.value_or(TangentState{});
                if (request.transform && entry.oldIndex >= 0 && transformed.contains({track, entry.oldIndex}) && tangent.mode == TangentMode::Aligned) {
                    tangent.slope = (tangent.slope * request.transform->valueScale) / request.transform->timeScale;
                    if (!ValidateTangent(tangent, error)) return false;
                }
                if (request.transform && type == 1 && handles.size() == 2 && entry.oldIndex > 0 && segment == segments.cend()) {
                    const auto original = BezierControls(track, entry.oldIndex);
                    for (int handle = 0; handle < 2; ++handle) {
                        const int owner = entry.oldIndex - (handle == 0 ? 1 : 0);
                        if (transformed.contains({track, owner})) handles[handle].ry() +=
                            (request.transform->valueScale - 1) * (original[handle].y() - before[owner].y());
                        if (!validMotionPoint(handles[handle])) return fail(QStringLiteral("变换后的控制柄超出运行时数值范围"));
                    }
                }
                points.append(entry.point); types.append(type); controls.append(std::move(handles)); tangents.append(tangent);
            }
            if (points.isEmpty()) {
                candidate.m_curves.remove(track); candidate.m_segmentTypes.remove(track); candidate.m_bezierControls.remove(track); candidate.m_tangents.remove(track);
            } else {
                candidate.m_curves.insert(track, std::move(points)); candidate.m_segmentTypes.insert(track, std::move(types));
                candidate.m_bezierControls.insert(track, std::move(controls)); candidate.m_tangents.insert(track, std::move(tangents));
            }
        }
        for (const auto& edit : tangentEdits) {
            auto mapped = edit; mapped.ref.index = output.oldToNew[edit.ref.track][edit.ref.index];
            if (!candidate.EditTangent(mapped, error)) return false;
        }
        QSet<CMotionPointRef> segmentTargets;
        QMap<CMotionPointRef, QVector<QPointF>> suppliedControls;
        for (const auto& segment : segments) {
            const CMotionPointRef ref{segment.ref.track, output.oldToNew[segment.ref.track][segment.ref.index]};
            segmentTargets.insert(ref);
            if (!segment.controls.isEmpty()) suppliedControls.insert(ref, candidate.BezierControls(ref.track, ref.index));
        }
        QSet<CMotionTrack> affectedTracks;
        for (const auto& track : affected) affectedTracks.insert(track);
        if (!candidate.ApplyTangentConstraints(affectedTracks, error)) return false;
        for (auto segment = suppliedControls.cbegin(); segment != suppliedControls.cend(); ++segment) {
            const auto actual = candidate.BezierControls(segment.key().track, segment.key().index);
            for (int handle = 0; handle < 2; ++handle) {
                const int owner = segment.key().index - (handle == 0 ? 1 : 0);
                if (candidate.Tangent(segment.key().track, owner).mode != TangentMode::Broken &&
                    (actual[handle].x() != (*segment)[handle].x() || actual[handle].y() != (*segment)[handle].y()))
                    return fail(QStringLiteral("受切线模式约束的控制柄请通过手柄编辑请求修改"));
            }
        }
        for (const auto& edit : handleEdits) {
            auto mapped = edit; mapped.ref.index = output.oldToNew[edit.ref.track][edit.ref.index];
            const int segmentIndex = mapped.ref.index + (edit.side == HandleSide::Outgoing ? 1 : 0);
            if (segmentTargets.contains({mapped.ref.track, segmentIndex}))
                return fail(QStringLiteral("同一曲线段不能同时直接编辑插值与手柄"));
            if (!candidate.EditHandle(mapped, error)) return false;
        }
        if (!candidate.ApplyTangentConstraints(affectedTracks, error)) return false;
        for (const auto& track : affectedTracks) {
            const auto points = candidate.m_curves.value(track); QVector<int> inverse(points.size(), -1);
            const auto mapping = output.oldToNew.value(track);
            for (int old = 0; old < mapping.size(); ++old) if (mapping[old] >= 0) inverse[mapping[old]] = old;
            for (int index = 0; index < points.size(); ++index) {
                const int old = inverse[index]; bool changed = old < 0;
                if (!changed) {
                    const auto before = m_curves[track][old];
                    changed = before.x() != points[index].x() || before.y() != points[index].y() || Tangent(track, old) != candidate.Tangent(track, index);
                    for (auto side : {HandleSide::Incoming, HandleSide::Outgoing}) {
                        const auto a = TangentHandle({track, old}, side), b = candidate.TangentHandle({track, index}, side);
                        changed = changed || a.has_value() != b.has_value() || (a && b && (a->x() != b->x() || a->y() != b->y()));
                    }
                }
                if (changed) output.affected.append({track, index});
            }
        }
        const double previousDuration = Timing().duration;
        if (candidate.Timing().duration < previousDuration) {
            auto meta = candidate.m_source.value(QStringLiteral("Meta")).toObject();
            meta.insert(QStringLiteral("Duration"), previousDuration);
            candidate.m_source.insert(QStringLiteral("Meta"), meta);
        }
        QJsonObject before, after;
        if (!BuildJson(&before, Timing().fps, Loop(), true, error) ||
            !candidate.BuildJson(&after, candidate.Timing().fps, candidate.Loop(), true, error)) return false;
        QJsonObject authoring;
        if (candidate.HasAuthoringTangents() && !candidate.SaveState(&authoring, candidate.Loop(), 0, error)) return false;
        output.changed = before != after || m_tangents != candidate.m_tangents;
        if (output.changed) SwapContent(candidate);
        result = std::move(output);
        return true;
    } catch (const std::exception&) { return fail(QStringLiteral("无法准备关键帧编辑，原动画保持不变")); }
}

bool CMotionTimeline::UpdatePoint(const CMotionTrack& track, int index, float time, float value)
{
    const auto curve = m_curves.constFind(track);
    if (curve == m_curves.cend() || index < 0 || index >= curve->size()) return false;
    const auto current = curve->at(index);
    const QPointF point(static_cast<float>(current.x()) == time ? current.x() : time,
                        static_cast<float>(current.y()) == value ? current.y() : value);
    BatchEdit request; request.updates.append({{track, index}, point});
    BatchResult result;
    return EditPoints(request, result);
}

bool CMotionTimeline::RemovePoint(const CMotionTrack& track, int index)
{
    BatchEdit request; request.removes.append({track, index});
    BatchResult result;
    return EditPoints(request, result);
}

bool CMotionTimeline::SetSegmentType(const CMotionTrack& track, int pointIndex, int segmentType)
{
    BatchEdit request; request.segments.append({{track, pointIndex}, segmentType, {}});
    BatchResult result; return EditPoints(request, result);
}

int CMotionTimeline::SegmentType(const CMotionTrack& track, int pointIndex) const
{
    return m_segmentTypes.value(track).value(pointIndex, 0);
}

float CMotionTimeline::Duration() const
{
    return static_cast<float>(Timing().duration);
}

double CMotionTimeline::ContentDuration() const
{
    double duration = 0.0;
    for (const auto& points : m_curves)
        if (!points.isEmpty()) duration = qMax(duration, points.last().x());
    const auto& events = SortedEvents();
    if (!events.isEmpty()) duration = qMax(duration, events.last().time);
    return duration;
}

float CMotionTimeline::Fps() const
{
    return static_cast<float>(m_source.value(QStringLiteral("Meta")).toObject()
                                  .value(QStringLiteral("Fps")).toDouble(30.0));
}

CMotionTiming CMotionTimeline::Timing() const
{
    const auto meta = m_source.value(QStringLiteral("Meta")).toObject();
    return {meta.value(QStringLiteral("Fps")).toDouble(30.0),
            qMax(ContentDuration(), meta.value(QStringLiteral("Duration")).toDouble())};
}

bool CMotionTimeline::Loop() const
{
    return m_source.value(QStringLiteral("Meta")).toObject().value(QStringLiteral("Loop")).toBool();
}

CMotionFadeTimes CMotionTimeline::FadeTimes() const
{
    return readFadeTimes(m_source.value(QStringLiteral("Meta")).toObject());
}

bool CMotionTimeline::SetFadeTimes(const CMotionFadeTimes& fades, QString* error)
{
    if (!validFadeTimes(fades)) {
        if (error) *error = QStringLiteral("动作淡入淡出时间无效");
        return false;
    }
    QJsonObject source = m_source;
    QJsonObject meta = source.value(QStringLiteral("Meta")).toObject();
    const auto before = meta;
    writeFadeTimes(&meta, fades);
    if (meta == before) return true;
    source.insert(QStringLiteral("Meta"), meta);
    return SetSource(std::move(source), error);
}

CMotionFadeTimes CMotionTimeline::TrackFadeTimes(const CMotionTrack& track) const
{
    if (!m_curves.contains(track)) return {};
    for (const auto& value : m_source.value(QStringLiteral("Curves")).toArray()) {
        const auto curve = value.toObject();
        if (curve.value(QStringLiteral("Target")).toString() == track.TargetName() &&
            curve.value(QStringLiteral("Id")).toString() == track.id) return readFadeTimes(curve);
    }
    return {};
}

QMap<CMotionTrack, CMotionFadeTimes> CMotionTimeline::TrackFadeTimes() const
{
    QMap<CMotionTrack, CMotionFadeTimes> result;
    for (auto it = m_curves.cbegin(); it != m_curves.cend(); ++it) result.insert(it.key(), {});
    for (const auto& value : m_source.value(QStringLiteral("Curves")).toArray()) {
        const auto curve = value.toObject();
        const auto target = curve.value(QStringLiteral("Target")).toString();
        const auto type = target == QLatin1String("Parameter") ? CMotionTarget::Parameter
            : target == QLatin1String("Model") ? CMotionTarget::Model : CMotionTarget::PartOpacity;
        auto found = result.find({type, curve.value(QStringLiteral("Id")).toString()});
        if (found != result.end()) *found = readFadeTimes(curve);
    }
    return result;
}

bool CMotionTimeline::SetTrackFadeTimes(const CMotionTrack& track, const CMotionFadeTimes& fades, QString* error)
{
    if (!m_curves.contains(track) || !validFadeTimes(fades)) {
        if (error) *error = QStringLiteral("曲线不存在或淡入淡出时间无效");
        return false;
    }
    QJsonArray curves = m_source.value(QStringLiteral("Curves")).toArray();
    int index = 0;
    for (; index < curves.size(); ++index) {
        const auto curve = curves.at(index).toObject();
        if (curve.value(QStringLiteral("Target")).toString() == track.TargetName() &&
            curve.value(QStringLiteral("Id")).toString() == track.id) break;
    }
    QJsonObject curve = index < curves.size() ? curves.at(index).toObject() : QJsonObject{};
    const auto before = curve;
    writeFadeTimes(&curve, fades);
    if (curve == before) return true;
    curve.insert(QStringLiteral("Target"), track.TargetName());
    curve.insert(QStringLiteral("Id"), track.id);
    if (index < curves.size()) curves.replace(index, curve);
    else curves.append(curve);
    QJsonObject source = m_source;
    source.insert(QStringLiteral("Curves"), curves);
    return SetSource(std::move(source), error);
}

QJsonArray CMotionTimeline::Events() const
{
    return m_source.value(QStringLiteral("UserData")).toArray();
}

const QVector<CMotionEventOccurrence>& CMotionTimeline::SortedEvents() const
{
    const auto source = Events();
    if (source == m_cachedEvents) {
        m_cachedEvents = source;
        return m_sortedEvents;
    }
    m_cachedEvents = source;
    m_sortedEvents.clear();
    m_sortedEvents.reserve(source.size());
    for (int index = 0; index < source.size(); ++index) {
        const auto event = source.at(index).toObject();
        m_sortedEvents.append({event.value(QStringLiteral("Time")).toDouble(),
                               event.value(QStringLiteral("Value")).toString(), index, 0});
    }
    std::stable_sort(m_sortedEvents.begin(), m_sortedEvents.end(), [](const auto& first, const auto& second) {
        return first.time < second.time;
    });
    return m_sortedEvents;
}

bool CMotionTimeline::SetEvents(const QJsonArray& events, QString* error)
{
    if (!validEvents(events, error)) return false;
    if (events == Events()) return true;
    QJsonObject source = m_source;
    source.insert(QStringLiteral("UserData"), events);
    return SetSource(std::move(source), error);
}

CMotionEventBatch CMotionTimeline::EventsBetween(double startTime, double elapsedAdvance, bool loop,
                                                bool includeStart, int maxOccurrences) const
{
    CMotionEventBatch batch;
    const double duration = Timing().duration;
    if (!std::isfinite(startTime) || !std::isfinite(elapsedAdvance) || startTime < 0.0 ||
        startTime > duration || elapsedAdvance < 0.0 || maxOccurrences < 0) {
        batch.valid = false;
        return batch;
    }
    const int limit = qMin(maxOccurrences, maximumEventOccurrences);
    const auto& events = SortedEvents();
    if (events.isEmpty()) return batch;
    const auto upper = [&](auto begin, double time) {
        return std::upper_bound(begin, events.cend(), time, [](double value, const auto& event) {
            return value < event.time;
        });
    };
    const auto first = includeStart
        ? std::lower_bound(events.cbegin(), events.cend(), startTime, [](const auto& event, double time) {
              return event.time < time;
          })
        : upper(events.cbegin(), startTime);
    const auto last = std::upper_bound(first, events.cend(), elapsedAdvance, [=](double advance, const auto& event) {
        return advance < event.time - startTime;
    });
    quint64 total = static_cast<quint64>(last - first);
    const auto append = [&](auto begin, auto end, quint64 cycle) {
        while (begin != end && batch.occurrences.size() < limit) {
            auto event = *begin++;
            event.cycle = cycle;
            batch.occurrences.append(std::move(event));
        }
    };
    append(first, last, 0);
    if (loop && duration > 0.0 && elapsedAdvance >= duration - startTime) {
        quint64 cycles = completeCycles(elapsedAdvance, duration, &batch.countSaturated);
        const double remainder = std::fmod(elapsedAdvance, duration);
        const double toBoundary = duration - startTime;
        const bool extraWrap = remainder >= toBoundary;
        const double tail = extraWrap ? remainder - toBoundary
            : qMin(remainder + startTime, std::nextafter(duration, 0.0));
        if (!extraWrap && cycles > 0) --cycles;
        const auto afterZero = upper(events.cbegin(), 0.0);
        const bool skipInitialZero = startTime == duration && !includeStart;
        const auto cycleFirst = skipInitialZero ? afterZero : events.cbegin();
        if (cycles > 0) {
            addOccurrences(static_cast<quint64>(events.cend() - cycleFirst), 1, &total, &batch.countSaturated);
            addOccurrences(static_cast<quint64>(events.size()), cycles - 1, &total, &batch.countSaturated);
            append(cycleFirst, events.cend(), 1);
            for (quint64 cycle = 2; cycle <= cycles && batch.occurrences.size() < limit; ++cycle)
                append(events.cbegin(), events.cend(), cycle);
        }
        const auto tailFirst = cycles == 0 ? cycleFirst : events.cbegin();
        const auto tailLast = upper(tailFirst, tail);
        addOccurrences(static_cast<quint64>(tailLast - tailFirst), 1, &total, &batch.countSaturated);
        append(tailFirst, tailLast, cycles == std::numeric_limits<quint64>::max() ? cycles : cycles + 1);
    }
    if (batch.countSaturated) total = std::numeric_limits<quint64>::max();
    batch.omittedCount = total - static_cast<quint64>(batch.occurrences.size());
    return batch;
}

bool CMotionTimeline::SetTiming(double fps, double duration, QString* error)
{
    if (!finiteFloat(fps) || static_cast<float>(fps) <= 0.0f || !finiteFloat(duration) || duration < ContentDuration()) {
        if (error) *error = QStringLiteral("帧率必须大于 0，时长不能早于最后一个关键帧或事件");
        return false;
    }
    QJsonObject source = m_source;
    QJsonObject meta = source.value(QStringLiteral("Meta")).toObject();
    const auto timing = Timing();
    if (fps == timing.fps && duration == timing.duration) return true;
    if (fps != timing.fps) meta.insert(QStringLiteral("Fps"), fps);
    if (duration != timing.duration) meta.insert(QStringLiteral("Duration"), duration);
    source.insert(QStringLiteral("Meta"), meta);
    return SetSource(std::move(source), error);
}

bool CMotionTimeline::SetSource(QJsonObject source, QString* error)
{
    if (source == m_source) return true;
    CMotionTimeline candidate = *this;
    candidate.m_source = std::move(source);
    if (!candidate.ValidateEditableState(error)) return false;
    m_source = std::move(candidate.m_source);
    m_cachedEvents = std::move(candidate.m_cachedEvents);
    m_sortedEvents = std::move(candidate.m_sortedEvents);
    return true;
}

float CMotionTimeline::Evaluate(const CMotionTrack& track, float time, bool* ok) const
{
    const QVector<QPointF> points = m_curves.value(track);
    if (points.isEmpty() || !std::isfinite(time)) {
        if (ok) *ok = false;
        return 0.0f;
    }
    if (ok) *ok = true;
    const auto next = std::upper_bound(points.cbegin() + 1, points.cend(), time,
        [](float value, const QPointF& point) { return value < static_cast<float>(point.x()); });
    if (next == points.cend()) return static_cast<float>(points.last().y());
    const int i = static_cast<int>(next - points.cbegin());
    const QPointF start = points[i - 1];
    const QPointF end = *next;
    const double span = end.x() - start.x();
    const double ratio = span > 0.0 ? qBound(0.0, (time - start.x()) / span, 1.0) : 1.0;
    const int type = SegmentType(track, i);
    if (type == 2) return static_cast<float>(start.y());
    if (type == 3) return static_cast<float>(end.y());
    if (type != 1) return static_cast<float>(start.y() + (end.y() - start.y()) * ratio);
    const auto controls = BezierControls(track, i);
    return CMotionBezier::Evaluate(start, controls[0], controls[1], end, time, AreBeziersRestricted());
}

QMap<QString, float> CMotionTimeline::EvaluateAll(float time) const
{
    return EvaluateFrame(time).parameters;
}

CMotionPreviewFrame CMotionTimeline::EvaluateFrame(float time) const
{
    CMotionPreviewFrame frame;
    for (auto it = m_curves.cbegin(); it != m_curves.cend(); ++it) {
        bool valid = false;
        const float value = Evaluate(it.key(), time, &valid);
        if (!valid) continue;
        auto& values = it.key().target == CMotionTarget::Parameter ? frame.parameters
            : it.key().target == CMotionTarget::Model ? frame.model : frame.parts;
        values.insert(it.key().id, value);
    }
    return frame;
}

QMap<QString, QVector<QPointF>> CMotionTimeline::Curves() const
{
    QMap<QString, QVector<QPointF>> parameters;
    for (auto it = m_curves.cbegin(); it != m_curves.cend(); ++it)
        if (it.key().target == CMotionTarget::Parameter) parameters.insert(it.key().id, it.value());
    return parameters;
}

int CMotionTimeline::PointCount() const
{
    int count = 0;
    for (auto it = m_curves.cbegin(); it != m_curves.cend(); ++it)
        if (it.key().target == CMotionTarget::Parameter) count += it.value().size();
    return count;
}

int CMotionTimeline::TotalPointCount() const
{
    int count = 0;
    for (const auto& points : m_curves) count += points.size();
    return count;
}

bool CMotionTimeline::BuildJson(QJsonObject* output, double fps, bool loop, bool draft, QString* error) const
{
    if (!ValidateTangentState(error)) return false;
    const double outputFps = finiteFloat(fps) && static_cast<float>(fps) > 0.0f ? fps : 30.0;
    const QJsonArray originals = m_source.value(QStringLiteral("Curves")).toArray();
    QMap<CMotionTrack, QJsonObject> curveFields;
    for (const auto& value : originals) {
        const auto curve = value.toObject();
        const QString target = curve.value(QStringLiteral("Target")).toString();
        const auto type = target == QLatin1String("Parameter") ? CMotionTarget::Parameter
            : target == QLatin1String("Model") ? CMotionTarget::Model : CMotionTarget::PartOpacity;
        curveFields.insert({type, curve.value(QStringLiteral("Id")).toString()}, curve);
    }
    const bool restricted = m_source.value(QStringLiteral("Meta")).toObject()
                                .value(QStringLiteral("AreBeziersRestricted")).toBool();
    QJsonArray curves;
    for (auto it = m_curves.cbegin(); it != m_curves.cend(); ++it) {
        if (it.value().isEmpty()) continue;
        QVector<QPointF> points = it.value();
        const auto types = m_segmentTypes.value(it.key());
        const auto bezierControls = m_bezierControls.value(it.key());
        if (!draft && points.size() == 1) {
            const float start = static_cast<float>(points.first().x());
            const float endpoint = qMax(static_cast<float>(start + 1.0 / outputFps),
                                         std::nextafterf(start, std::numeric_limits<float>::infinity()));
            if (!std::isfinite(endpoint)) {
                if (error) *error = QStringLiteral("无法为单点曲线生成有效终点：%1").arg(it.key().id);
                return false;
            }
            points.append(QPointF(endpoint, points.first().y()));
        }
        QJsonArray segments{points.first().x(), points.first().y()};
        for (int i = 1; i < points.size(); ++i) {
            const int type = types.value(i, 0);
            segments.append(type);
            if (type == 1) {
                const auto controls = ((restricted && bezierControls.value(i).size() == 2) ||
                                       validBezierControls(bezierControls.value(i), points[i - 1], points[i]))
                    ? bezierControls.value(i) : defaultBezierControls(points[i - 1], points[i]);
                for (const auto& point : controls) {
                    segments.append(point.x());
                    segments.append(point.y());
                }
            }
            segments.append(points[i].x());
            segments.append(points[i].y());
        }
        QJsonObject curve = curveFields.value(it.key());
        curve.insert(QStringLiteral("Target"), it.key().TargetName());
        curve.insert(QStringLiteral("Id"), it.key().id);
        curve.insert(QStringLiteral("Segments"), segments);
        curves.append(curve);
    }
    if (!draft && curves.isEmpty() && Events().isEmpty()) {
        if (error) *error = QStringLiteral("没有可导出的曲线或事件");
        return false;
    }
    QJsonObject meta = m_source.value(QStringLiteral("Meta")).toObject();
    int totalSegments = 0, totalPoints = 0, totalKeys = 0;
    double duration = qMax(ContentDuration(), meta.value(QStringLiteral("Duration")).toDouble());
    for (const auto& value : curves) {
        ParsedCurve parsed;
        if (!parseCurve(value.toObject(), restricted, &parsed, draft)) {
            if (error) *error = QStringLiteral("motion3 曲线无法安全导出");
            return false;
        }
        totalSegments += parsed.points.size() - 1;
        totalPoints += parsed.totalPoints;
        totalKeys += parsed.points.size();
        if (totalKeys > maximumMotionPoints) {
            if (error) *error = QStringLiteral("motion3 关键帧过多");
            return false;
        }
        duration = qMax(duration, parsed.points.last().x());
    }
    meta.insert(QStringLiteral("Duration"), duration);
    meta.insert(QStringLiteral("Fps"), outputFps);
    meta.insert(QStringLiteral("Loop"), loop);
    meta.insert(QStringLiteral("AreBeziersRestricted"), restricted);
    meta.insert(QStringLiteral("CurveCount"), curves.size());
    meta.insert(QStringLiteral("TotalSegmentCount"), totalSegments);
    meta.insert(QStringLiteral("TotalPointCount"), totalPoints);
    const QJsonArray events = m_source.value(QStringLiteral("UserData")).toArray();
    qint64 eventBytes = 0;
    for (const auto& event : events) eventBytes += event.toObject().value(QStringLiteral("Value")).toString().toUtf8().size();
    meta.insert(QStringLiteral("UserDataCount"), events.size());
    meta.insert(QStringLiteral("TotalUserDataSize"), eventBytes);
    QJsonObject root = m_source;
    root.insert(QStringLiteral("Version"), 3);
    root.insert(QStringLiteral("Meta"), meta);
    root.insert(QStringLiteral("Curves"), curves);
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (data.size() > maximumMotionBytes) {
        if (error) *error = QStringLiteral("motion3 文件过大");
        return false;
    }
    if (output) *output = root;
    return true;
}

bool CMotionTimeline::ToJson(QByteArray& bytes, float fps, bool loop, QString* error) const
{
    if (error) error->clear();
    try {
        QJsonObject root;
        if (!BuildJson(&root, fps == Fps() ? Timing().fps : fps, loop, false, error)) return false;
        QByteArray candidate = QJsonDocument(root).toJson(QJsonDocument::Indented);
        bytes.swap(candidate);
        return true;
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("无法生成动作快照");
        return false;
    }
}

bool CMotionTimeline::Save(const QString& path, float fps, bool loop, QString* error) const
{
    if (path.isEmpty()) {
        if (error) *error = QStringLiteral("motion3 保存路径为空");
        return false;
    }
    QByteArray data;
    if (!ToJson(data, fps, loop, error)) return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

bool CMotionTimeline::Load(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    const QByteArray bytes = file.read(maximumMotionBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > maximumMotionBytes) {
        if (error) *error = QStringLiteral("motion3 读取失败或文件过大");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("motion3 JSON 无效：%1").arg(parseError.errorString());
        return false;
    }
    return LoadJson(document.object(), false, error);
}

bool CMotionTimeline::RecountMetadata(QJsonObject& motion, QString* error, bool draft, double* duration)
{
    if (error) error->clear();
    CMotionTimeline validator;
    if (!validator.LoadJson(motion, draft, error, true)) return false;
    const auto curves = motion.value(QStringLiteral("Curves")).toArray();
    QJsonObject meta = motion.value(QStringLiteral("Meta")).toObject();
    int segments = 0, points = 0;
    for (const auto& value : curves) {
        ParsedCurve parsed;
        if (!parseCurve(value.toObject(), meta.value(QStringLiteral("AreBeziersRestricted")).toBool(), &parsed, draft)) return false;
        segments += parsed.points.size() - 1;
        points += parsed.totalPoints;
    }
    const auto events = motion.value(QStringLiteral("UserData")).toArray();
    qint64 eventBytes = 0;
    for (const auto& event : events) eventBytes += event.toObject().value(QStringLiteral("Value")).toString().toUtf8().size();
    meta.insert(QStringLiteral("CurveCount"), curves.size());
    meta.insert(QStringLiteral("TotalSegmentCount"), segments);
    meta.insert(QStringLiteral("TotalPointCount"), points);
    meta.insert(QStringLiteral("UserDataCount"), events.size());
    meta.insert(QStringLiteral("TotalUserDataSize"), eventBytes);
    motion.insert(QStringLiteral("Meta"), meta);
    if (duration) *duration = validator.Timing().duration;
    return true;
}

bool CMotionTimeline::LoadJson(const QJsonObject& root, bool draft, QString* error, bool allowEmpty)
{
    if (root.value(QStringLiteral("Version")).toDouble(-1) != 3.0 || !root.value(QStringLiteral("Curves")).isArray()) {
        if (error) *error = QStringLiteral("不支持的 motion3 版本或曲线字段");
        return false;
    }
    const QJsonObject meta = root.value(QStringLiteral("Meta")).toObject();
    bool validMeta = !root.contains(QStringLiteral("Meta")) || root.value(QStringLiteral("Meta")).isObject();
    for (const QString& key : {QStringLiteral("Duration"), QStringLiteral("Fps")}) {
        if (!meta.contains(key)) continue;
        const auto value = meta.value(key);
        if (!value.isDouble() || !finiteFloat(value.toDouble()) || value.toDouble() < 0.0 ||
            (key == QLatin1String("Fps") && static_cast<float>(value.toDouble()) <= 0.0f)) validMeta = false;
    }
    for (const QString& key : {QStringLiteral("Loop"), QStringLiteral("AreBeziersRestricted")})
        if (meta.contains(key) && !meta.value(key).isBool()) validMeta = false;
    if (!validMeta || !validFadeTimes(root) || !validFadeTimes(meta)) {
        if (error) *error = QStringLiteral("motion3 元数据无效");
        return false;
    }
    if (root.contains(QStringLiteral("UserData")) && !root.value(QStringLiteral("UserData")).isArray()) {
        if (error) *error = QStringLiteral("motion3 事件字段无效");
        return false;
    }
    if (!validEvents(root.value(QStringLiteral("UserData")).toArray(), error)) return false;
    QMap<CMotionTrack, QVector<QPointF>> curves;
    QMap<CMotionTrack, QVector<int>> segmentTypes;
    QMap<CMotionTrack, QVector<QVector<QPointF>>> bezierControls;
    QSet<QString> identities;
    int totalKeys = 0;
    const bool restricted = meta.value(QStringLiteral("AreBeziersRestricted")).toBool();
    for (const auto& value : root.value(QStringLiteral("Curves")).toArray()) {
        const auto curve = value.toObject();
        const QString target = curve.value(QStringLiteral("Target")).toString();
        const QString id = curve.value(QStringLiteral("Id")).toString();
        const QString identity = target + QChar::Null + id;
        ParsedCurve parsed;
        if (!value.isObject() || (target != QLatin1String("Parameter") && target != QLatin1String("Model") &&
                                  target != QLatin1String("PartOpacity")) || id.trimmed().isEmpty() ||
            id.contains(QChar::Null) || identities.contains(identity) || !validFadeTimes(curve) ||
            !parseCurve(curve, restricted, &parsed, draft)) {
            if (error) *error = QStringLiteral("motion3 曲线无效或重复：%1 / %2").arg(target, id);
            return false;
        }
        identities.insert(identity);
        totalKeys += parsed.points.size();
        if (totalKeys > maximumMotionPoints) {
            if (error) *error = QStringLiteral("motion3 关键帧过多");
            return false;
        }
        const auto type = target == QLatin1String("Parameter") ? CMotionTarget::Parameter
            : target == QLatin1String("Model") ? CMotionTarget::Model : CMotionTarget::PartOpacity;
        const CMotionTrack track{type, id};
        curves.insert(track, parsed.points);
        segmentTypes.insert(track, parsed.types);
        bezierControls.insert(track, parsed.controls);
    }
    if (!draft && !allowEmpty && identities.isEmpty() && root.value(QStringLiteral("UserData")).toArray().isEmpty()) {
        if (error) *error = QStringLiteral("motion3 没有曲线或事件");
        return false;
    }
    StopRecording();
    m_source = root;
    m_curves.swap(curves);
    m_segmentTypes.swap(segmentTypes);
    m_bezierControls.swap(bezierControls);
    m_tangents.clear();
    for (auto it = m_curves.cbegin(); it != m_curves.cend(); ++it) m_tangents.insert(it.key(), QVector<TangentState>(it->size()));
    return true;
}

bool CMotionTimeline::HasCurves() const
{
    return !m_curves.isEmpty();
}

bool CMotionTimeline::HasAnimation() const
{
    return HasCurves() || !Events().isEmpty();
}

bool CMotionTimeline::HasSameContent(const CMotionTimeline& other) const
{
    if (m_source != other.m_source || m_segmentTypes != other.m_segmentTypes || m_tangents != other.m_tangents ||
        m_curves.keys() != other.m_curves.keys() || m_bezierControls.keys() != other.m_bezierControls.keys()) return false;
    const auto pointsEqual = [](const QVector<QPointF>& first, const QVector<QPointF>& second) {
        if (first.size() != second.size()) return false;
        for (qsizetype index = 0; index < first.size(); ++index)
            if (first[index].x() != second[index].x() || first[index].y() != second[index].y()) return false;
        return true;
    };
    for (auto it = m_curves.cbegin(); it != m_curves.cend(); ++it) if (!pointsEqual(*it, other.m_curves[it.key()])) return false;
    for (auto it = m_bezierControls.cbegin(); it != m_bezierControls.cend(); ++it) {
        const auto controls = other.m_bezierControls.constFind(it.key());
        if (it->size() != controls->size()) return false;
        for (qsizetype index = 0; index < it->size(); ++index) if (!pointsEqual(it->at(index), controls->at(index))) return false;
    }
    return true;
}

bool CMotionTimeline::SaveState(QJsonObject* state, bool loop, double playhead, QString* error) const
{
    if (!state || !std::isfinite(playhead)) {
        if (error) *error = QStringLiteral("动画草稿目标或播放头无效");
        return false;
    }
    QJsonObject motion;
    if (!BuildJson(&motion, Timing().fps, loop, true, error)) return false;
    QJsonObject result{{QStringLiteral("version"), HasAuthoringTangents() ? 2 : 1}, {QStringLiteral("motion"), motion},
                       {QStringLiteral("playhead"), qBound(0.0, playhead, Timing().duration)}};
    if (HasAuthoringTangents()) {
        QJsonArray tracks;
        for (auto it = m_tangents.cbegin(); it != m_tangents.cend(); ++it) {
            bool authored = false; for (const auto& tangent : *it) authored = authored || tangent != TangentState{};
            if (!authored) continue;
            QJsonArray keys;
            for (int index = 0; index < it->size(); ++index) if (it->at(index) != TangentState{})
                keys.append(QJsonObject{{"index", index}, {"state", EncodeTangent(it->at(index))}});
            tracks.append(QJsonObject{{"target", it.key().TargetName()}, {"id", it.key().id}, {"keys", keys}});
        }
        result.insert("tangents", tracks);
    }
    if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() > maximumMotionBytes) {
        if (error) *error = QStringLiteral("动画草稿过大");
        return false;
    }
    *state = result;
    return true;
}

bool CMotionTimeline::RestoreState(const QJsonObject& state, float* playhead, QString* error)
{
    if (state.isEmpty()) {
        Clear();
        if (playhead) *playhead = 0.0f;
        return true;
    }
    const auto time = state.value(QStringLiteral("playhead"));
    const double version = state.value(QStringLiteral("version")).toDouble(-1);
    if ((version != 1 && version != 2) || (version == 1 && state.contains("tangents")) ||
        !state.value(QStringLiteral("motion")).isObject() || !time.isDouble() ||
        !finiteFloat(time.toDouble()) || time.toDouble() < 0.0 ||
        QJsonDocument(state).toJson(QJsonDocument::Compact).size() > maximumMotionBytes) {
        if (error) *error = QStringLiteral("动画草稿格式或播放头无效");
        return false;
    }
    CMotionTimeline candidate(m_recordingClock);
    if (!candidate.LoadJson(state.value(QStringLiteral("motion")).toObject(), true, error)) return false;
    if (version == 2) {
        const auto keys = state.keys();
        if (QSet<QString>(keys.cbegin(), keys.cend()) != QSet<QString>{"version", "motion", "playhead", "tangents"} || !state["tangents"].isArray()) {
            if (error) *error = QStringLiteral("动画切线草稿字段无效"); return false;
        }
        QSet<CMotionTrack> seen; const auto tracks = state["tangents"].toArray();
        if (tracks.size() > MaximumPointCount) { if (error) *error = QStringLiteral("切线轨道数量过多"); return false; }
        for (const auto& value : tracks) {
            if (!value.isObject()) { if (error) *error = QStringLiteral("切线轨道不是对象"); return false; }
            const auto row = value.toObject(); const auto names = row.keys();
            if (QSet<QString>(names.cbegin(), names.cend()) != QSet<QString>{"target", "id", "keys"} || !row["target"].isString() || !row["id"].isString() || !row["keys"].isArray()) {
                if (error) *error = QStringLiteral("切线轨道字段无效"); return false;
            }
            CMotionTrack track; track.id = row["id"].toString(); const auto target = row["target"].toString();
            if (target == "Parameter") track.target = CMotionTarget::Parameter;
            else if (target == "Model") track.target = CMotionTarget::Model;
            else if (target == "PartOpacity") track.target = CMotionTarget::PartOpacity;
            else { if (error) *error = QStringLiteral("切线轨道类型无效"); return false; }
            const auto points = candidate.m_curves.constFind(track); const auto keys = row["keys"].toArray();
            if (seen.contains(track) || points == candidate.m_curves.cend() || keys.isEmpty() || keys.size() > points->size()) {
                if (error) *error = QStringLiteral("切线轨道重复或与关键点不匹配"); return false;
            }
            seen.insert(track); QVector<TangentState> decoded(points->size()); QSet<int> indices;
            for (const auto& entry : keys) {
                if (!entry.isObject()) { if (error) *error = QStringLiteral("切线关键点不是对象"); return false; }
                const auto key = entry.toObject(); const auto names = key.keys(); const double index = key["index"].toDouble(-1);
                if (QSet<QString>(names.cbegin(), names.cend()) != QSet<QString>{"index", "state"} || !key["index"].isDouble() ||
                    !std::isfinite(index) || std::floor(index) != index || index < 0 || index >= points->size() || indices.contains(int(index)) || !key["state"].isObject()) {
                    if (error) *error = QStringLiteral("切线关键点索引重复或无效"); return false;
                }
                TangentState tangent;
                if (!DecodeTangent(key["state"].toObject(), tangent, error) || tangent == TangentState{}) {
                    if (error && error->isEmpty()) *error = QStringLiteral("默认切线状态不应重复存储"); return false;
                }
                indices.insert(int(index)); decoded[int(index)] = tangent;
            }
            candidate.m_tangents.insert(track, std::move(decoded));
        }
        if (!candidate.ValidateTangentState(error)) return false;
    }
    if (time.toDouble() > candidate.Timing().duration) {
        if (error) *error = QStringLiteral("动画播放头超出草稿时长");
        return false;
    }
    *this = std::move(candidate);
    if (playhead) *playhead = static_cast<float>(time.toDouble());
    return true;
}

QString CMotionTrack::TargetName() const
{
    switch (target) {
    case CMotionTarget::Model: return QStringLiteral("Model");
    case CMotionTarget::Parameter: return QStringLiteral("Parameter");
    case CMotionTarget::PartOpacity: return QStringLiteral("PartOpacity");
    }
    return {};
}

QMap<QString, float> CMotionPreviewFrame::ResolveParameters(const QSet<QString>& eyeBlinkIds,
                                                         const QSet<QString>& lipSyncIds) const
{
    QMap<QString, float> values = parameters;
    const auto eye = model.constFind(QStringLiteral("EyeBlink"));
    const auto lip = model.constFind(QStringLiteral("LipSync"));
    for (auto it = values.begin(); it != values.end(); ++it) {
        double value = it.value();
        if (eye != model.cend() && eyeBlinkIds.contains(it.key())) value *= eye.value();
        if (lip != model.cend() && lipSyncIds.contains(it.key())) value += lip.value();
        it.value() = static_cast<float>(qBound(-static_cast<double>(std::numeric_limits<float>::max()),
                                              value, static_cast<double>(std::numeric_limits<float>::max())));
    }
    if (eye != model.cend()) for (const auto& id : eyeBlinkIds)
        if (!parameters.contains(id)) values.insert(id, eye.value());
    if (lip != model.cend()) for (const auto& id : lipSyncIds)
        if (!parameters.contains(id)) values.insert(id, lip.value());
    return values;
}

bool CMotionTimeline::InsertPoint(const CMotionTrack& track, float time, float value)
{
    BatchEdit request; request.insertions.append({track, QPointF(time, value)});
    BatchResult result;
    return EditPoints(request, result);
}

bool CMotionPreviewFrame::IsFinite() const
{
    for (const auto* values : {&parameters, &model, &parts})
        for (auto it = values->cbegin(); it != values->cend(); ++it)
            if (it.key().isEmpty() || it.key().contains(QChar::Null) || !std::isfinite(it.value())) return false;
    return true;
}
