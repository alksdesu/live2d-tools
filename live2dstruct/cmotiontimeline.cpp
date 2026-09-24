#include "cmotiontimeline.h"

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

namespace {
constexpr qint64 maximumMotionBytes = 16 * 1024 * 1024;
constexpr int maximumMotionPoints = 100000;

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

bool validBezierControls(const QVector<QPointF>& controls, const QPointF& start, const QPointF& end)
{
    return controls.size() == 2 && std::isfinite(controls.at(0).x()) && std::isfinite(controls.at(0).y()) &&
           std::isfinite(controls.at(1).x()) && std::isfinite(controls.at(1).y()) &&
           controls.at(0).x() >= start.x() && controls.at(0).x() <= end.x() &&
           controls.at(1).x() >= start.x() && controls.at(1).x() <= end.x();
}

QVector<QPointF> retargetBezier(const QVector<QPointF>& controls, const QPointF& oldStart,
                                const QPointF& oldEnd, const QPointF& start, const QPointF& end)
{
    if (oldStart == start && oldEnd == end) return controls;
    if (controls.size() != 2 || oldEnd.x() <= oldStart.x()) return defaultBezierControls(start, end);
    QVector<QPointF> result;
    for (int i = 0; i < 2; ++i) {
        const double ratio = qBound(0.0, (controls[i].x() - oldStart.x()) / (oldEnd.x() - oldStart.x()), 1.0);
        const double offset = i == 0 ? start.y() - oldStart.y() : end.y() - oldEnd.y();
        const QPointF point(start.x() + ratio * (end.x() - start.x()), controls[i].y() + offset);
        if (!finiteFloat(point.x()) || !finiteFloat(point.y())) return defaultBezierControls(start, end);
        result.append(point);
    }
    return result;
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
}

void CMotionTimeline::StartRecording()
{
    m_recordStartTime = Duration();
    m_recording = true;
    m_clock.start();
}

void CMotionTimeline::StopRecording()
{
    m_recording = false;
}

void CMotionTimeline::Clear()
{
    StopRecording();
    m_source = {};
    m_curves.clear();
    m_segmentTypes.clear();
    m_bezierControls.clear();
}

void CMotionTimeline::Record(const QString& parameterId, float value)
{
    if (!m_recording || !m_clock.isValid()) return;
    AddPoint(parameterId, m_recordStartTime + m_clock.elapsed() / 1000.0f, value);
}

bool CMotionTimeline::AddPoint(const CMotionTrack& track, float time, float value)
{
    if (track.TargetName().isEmpty() || track.id.trimmed().isEmpty() || track.id.contains(QChar::Null) || !std::isfinite(time) || time < 0.0f || !std::isfinite(value)) return false;
    if (TotalPointCount() >= maximumMotionPoints) return false;
    QVector<QPointF>& points = m_curves[track];
    QVector<int>& segmentTypes = m_segmentTypes[track];
    QVector<QVector<QPointF>>& bezierControls = m_bezierControls[track];
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
    return true;
}

bool CMotionTimeline::UpdatePoint(const CMotionTrack& track, int index, float time, float value)
{
    if (track.TargetName().isEmpty() || track.id.trimmed().isEmpty() || track.id.contains(QChar::Null) || !std::isfinite(time) || time < 0.0f || !std::isfinite(value)) return false;
    auto curve = m_curves.find(track);
    if (curve == m_curves.end() || index < 0 || index >= curve->size()) return false;
    const QPointF current = curve->at(index);
    const QPointF updated(static_cast<float>(current.x()) == time ? current.x() : time,
                          static_cast<float>(current.y()) == value ? current.y() : value);
    if (updated == current) return true;
    for (int i = 0; i < curve->size(); ++i) {
        if (i != index && static_cast<float>(curve->at(i).x()) == time) return false;
    }
    struct Entry { QPointF point; int originalIndex; };
    QVector<Entry> entries;
    const auto before = *curve;
    const auto types = m_segmentTypes.value(track);
    const auto controls = m_bezierControls.value(track);
    for (int i = 0; i < curve->size(); ++i)
        entries.append({i == index ? updated : curve->at(i), i});
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right){
        return left.point.x() < right.point.x();
    });
    for (int i = 0; i < entries.size(); ++i) {
        const int original = entries[i].originalIndex;
        (*curve)[i] = entries[i].point;
        m_segmentTypes[track][i] = i == 0 ? 0 : types.value(original, 0);
        m_bezierControls[track][i] = (i > 0 && original > 0 && types.value(original) == 1)
            ? retargetBezier(controls.value(original), before[original - 1], before[original],
                              entries[i - 1].point, entries[i].point) : QVector<QPointF>();
    }
    return true;
}

bool CMotionTimeline::RemovePoint(const CMotionTrack& track, int index)
{
    auto curve = m_curves.find(track);
    if (curve == m_curves.end() || index < 0 || index >= curve->size()) return false;
    if (index > 0 && index + 1 < curve->size() && SegmentType(track, index + 1) == 1) {
        m_bezierControls[track][index + 1] = retargetBezier(
            m_bezierControls.value(track).value(index + 1), curve->at(index), curve->at(index + 1),
            curve->at(index - 1), curve->at(index + 1));
    }
    curve->removeAt(index);
    if (m_segmentTypes.contains(track)) m_segmentTypes[track].removeAt(index);
    if (m_bezierControls.contains(track)) m_bezierControls[track].removeAt(index);
    if (!curve->isEmpty()) {
        m_segmentTypes[track][0] = 0;
        m_bezierControls[track][0].clear();
    }
    const bool empty = curve->isEmpty();
    if (empty) {
        m_curves.erase(curve);
        m_segmentTypes.remove(track);
        m_bezierControls.remove(track);
    }
    return true;
}

bool CMotionTimeline::SetSegmentType(const CMotionTrack& track, int pointIndex, int segmentType)
{
    if (pointIndex <= 0 || segmentType < 0 || segmentType > 3) return false;
    auto curve = m_curves.find(track);
    if (curve == m_curves.end() || pointIndex >= curve->size()) return false;
    QVector<int>& types = m_segmentTypes[track];
    types.resize(curve->size());
    m_bezierControls[track].resize(curve->size());
    types[pointIndex] = segmentType;
    return true;
}

int CMotionTimeline::SegmentType(const CMotionTrack& track, int pointIndex) const
{
    return m_segmentTypes.value(track).value(pointIndex, 0);
}

float CMotionTimeline::Duration() const
{
    float duration = static_cast<float>(m_source.value(QStringLiteral("Meta")).toObject()
                                           .value(QStringLiteral("Duration")).toDouble());
    for (const auto& points : m_curves)
        if (!points.isEmpty()) duration = qMax(duration, static_cast<float>(points.last().x()));
    return duration;
}

float CMotionTimeline::Fps() const
{
    return static_cast<float>(m_source.value(QStringLiteral("Meta")).toObject()
                                  .value(QStringLiteral("Fps")).toDouble(30.0));
}

bool CMotionTimeline::Loop() const
{
    return m_source.value(QStringLiteral("Meta")).toObject().value(QStringLiteral("Loop")).toBool();
}

float CMotionTimeline::Evaluate(const CMotionTrack& track, float time, bool* ok) const
{
    const QVector<QPointF> points = m_curves.value(track);
    if (points.isEmpty() || !std::isfinite(time)) {
        if (ok) *ok = false;
        return 0.0f;
    }
    if (ok) *ok = true;
    if (time < static_cast<float>(points.first().x())) return static_cast<float>(points.first().y());
    for (int i = 1; i < points.size(); ++i) {
        const QPointF start = points[i - 1];
        const QPointF end = points[i];
        if (time >= static_cast<float>(end.x())) continue;
        const double span = end.x() - start.x();
        const double ratio = span > 0.0 ? qBound(0.0, (time - start.x()) / span, 1.0) : 1.0;
        const int type = SegmentType(track, i);
        if (type == 2) return static_cast<float>(start.y());
        if (type == 3) return static_cast<float>(end.y());
        if (type != 1) return static_cast<float>(start.y() + (end.y() - start.y()) * ratio);
        const auto cubic = [](double a, double b, double c, double d, double t) {
            const double inverse = 1.0 - t;
            return inverse * inverse * inverse * a + 3.0f * inverse * inverse * t * b +
                   3.0f * inverse * t * t * c + t * t * t * d;
        };
        const bool restricted = m_source.value(QStringLiteral("Meta")).toObject()
                                    .value(QStringLiteral("AreBeziersRestricted")).toBool();
        const QVector<QPointF> controls = m_bezierControls.value(track).value(i);
        const QVector<QPointF> fallback = defaultBezierControls(start, end);
        const QVector<QPointF> selectedControls = (restricted && controls.size() == 2) || validBezierControls(controls, start, end)
            ? controls : fallback;
        const QPointF first = selectedControls.at(0);
        const QPointF second = selectedControls.at(1);
        const float firstRatio = qBound(0.0f, static_cast<float>((first.x() - start.x()) / span), 1.0f);
        const float secondRatio = qBound(0.0f, static_cast<float>((second.x() - start.x()) / span), 1.0f);
        double low = restricted ? ratio : 0.0;
        double high = restricted ? ratio : 1.0;
        for (int step = 0; !restricted && step < 24; ++step) {
            const double middle = (low + high) * 0.5;
            if (cubic(0.0f, firstRatio, secondRatio, 1.0f, middle) < ratio) low = middle;
            else high = middle;
        }
        const double valueControl = (low + high) * 0.5;
        return cubic(static_cast<float>(start.y()), static_cast<float>(first.y()),
                     static_cast<float>(second.y()), static_cast<float>(end.y()), valueControl);
    }
    return static_cast<float>(points.last().y());
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

bool CMotionTimeline::BuildJson(QJsonObject* output, float fps, bool loop, bool draft, QString* error) const
{
    const double outputFps = std::isfinite(fps) && fps > 0.0f ? fps : 30.0;
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
    if (!draft && curves.isEmpty()) {
        if (error) *error = QStringLiteral("没有可导出的曲线");
        return false;
    }
    QJsonObject meta = m_source.value(QStringLiteral("Meta")).toObject();
    int totalSegments = 0, totalPoints = 0, totalKeys = 0;
    double duration = meta.value(QStringLiteral("Duration")).toDouble();
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

bool CMotionTimeline::Save(const QString& path, float fps, bool loop, QString* error) const
{
    if (path.isEmpty()) {
        if (error) *error = QStringLiteral("motion3 保存路径为空");
        return false;
    }
    QJsonObject root;
    if (!BuildJson(&root, fps, loop, false, error)) return false;
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
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

bool CMotionTimeline::LoadJson(const QJsonObject& root, bool draft, QString* error)
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
    for (const auto& value : root.value(QStringLiteral("UserData")).toArray()) {
        const auto event = value.toObject();
        const auto time = event.value(QStringLiteral("Time"));
        const auto text = event.value(QStringLiteral("Value"));
        if (!value.isObject() || !time.isDouble() || !finiteFloat(time.toDouble()) || time.toDouble() < 0.0 ||
            !text.isString() || text.toString().contains(QChar::Null)) {
            if (error) *error = QStringLiteral("motion3 事件无效");
            return false;
        }
    }
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
    if (!draft && identities.isEmpty()) {
        if (error) *error = QStringLiteral("motion3 没有曲线");
        return false;
    }
    StopRecording();
    m_source = root;
    m_curves.swap(curves);
    m_segmentTypes.swap(segmentTypes);
    m_bezierControls.swap(bezierControls);
    return true;
}

bool CMotionTimeline::HasCurves() const
{
    return !m_curves.isEmpty();
}

bool CMotionTimeline::HasSameContent(const CMotionTimeline& other) const
{
    return m_source == other.m_source && m_curves == other.m_curves &&
           m_segmentTypes == other.m_segmentTypes && m_bezierControls == other.m_bezierControls;
}

bool CMotionTimeline::SaveState(QJsonObject* state, bool loop, float playhead, QString* error) const
{
    if (!state || !std::isfinite(playhead)) {
        if (error) *error = QStringLiteral("动画草稿目标或播放头无效");
        return false;
    }
    QJsonObject motion;
    if (!BuildJson(&motion, Fps(), loop, true, error)) return false;
    const QJsonObject result{{QStringLiteral("version"), 1}, {QStringLiteral("motion"), motion},
                             {QStringLiteral("playhead"), qBound(0.0f, playhead, Duration())}};
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
    if (state.value(QStringLiteral("version")).toDouble(-1) != 1.0 ||
        !state.value(QStringLiteral("motion")).isObject() || !time.isDouble() ||
        !finiteFloat(time.toDouble()) || time.toDouble() < 0.0 ||
        QJsonDocument(state).toJson(QJsonDocument::Compact).size() > maximumMotionBytes) {
        if (error) *error = QStringLiteral("动画草稿格式或播放头无效");
        return false;
    }
    CMotionTimeline candidate;
    if (!candidate.LoadJson(state.value(QStringLiteral("motion")).toObject(), true, error)) return false;
    if (time.toDouble() > candidate.Duration()) {
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
    if (track.TargetName().isEmpty() || track.id.trimmed().isEmpty() || track.id.contains(QChar::Null) ||
        !std::isfinite(time) || time < 0 || !std::isfinite(value) || TotalPointCount() >= maximumMotionPoints) return false;
    const auto points = m_curves.value(track);
    for (const auto& point : points) if (static_cast<float>(point.x()) == time) return false;
    const int index = static_cast<int>(std::lower_bound(points.cbegin(), points.cend(), time,
        [](const QPointF& point, float at){ return point.x() < at; }) - points.cbegin());
    m_curves[track].insert(index, QPointF(time, value));
    m_segmentTypes[track].insert(index, 0);
    m_bezierControls[track].insert(index, QVector<QPointF>());
    if (index + 1 < m_curves[track].size() && m_segmentTypes[track].value(index + 1) == 1 && index > 0) {
        m_bezierControls[track][index + 1] = retargetBezier(m_bezierControls[track][index + 1],
            points[index - 1], points[index], m_curves[track][index], m_curves[track][index + 1]);
    }
    return true;
}

bool CMotionPreviewFrame::IsFinite() const
{
    for (const auto* values : {&parameters, &model, &parts})
        for (auto it = values->cbegin(); it != values->cend(); ++it)
            if (it.key().isEmpty() || it.key().contains(QChar::Null) || !std::isfinite(it.value())) return false;
    return true;
}
