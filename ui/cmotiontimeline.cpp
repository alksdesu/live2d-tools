#include "cmotiontimeline.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
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
           controls.at(1).x() >= controls.at(0).x() && controls.at(1).x() <= end.x();
}
}

void CMotionTimeline::StartRecording()
{
    m_recording = true;
    m_clock.start();
}

void CMotionTimeline::StopRecording()
{
    m_recording = false;
}

void CMotionTimeline::Clear()
{
    m_curves.clear();
    m_segmentTypes.clear();
    m_bezierControls.clear();
}

void CMotionTimeline::Record(const QString& parameterId, float value)
{
    if (!m_recording || !m_clock.isValid()) return;
    AddPoint(parameterId, m_clock.elapsed() / 1000.0f, value);
}

bool CMotionTimeline::AddPoint(const QString& parameterId, float time, float value)
{
    if (parameterId.isEmpty() || !std::isfinite(time) || time < 0.0f || !std::isfinite(value)) return false;
    QVector<QPointF>& points = m_curves[parameterId];
    QVector<int>& segmentTypes = m_segmentTypes[parameterId];
    QVector<QVector<QPointF>>& bezierControls = m_bezierControls[parameterId];
    if (!points.isEmpty()) {
        if (time < points.last().x()) time = points.last().x();
        if (qFuzzyCompare(time + 1.0, points.last().x() + 1.0) &&
            !qFuzzyCompare(points.last().y() + 1.0, static_cast<double>(value) + 1.0))
            time = std::nextafterf(static_cast<float>(points.last().x()), std::numeric_limits<float>::infinity());
        if (qFuzzyCompare(points.last().y() + 1.0, static_cast<double>(value) + 1.0) &&
            qFuzzyCompare(points.last().x() + 1.0, static_cast<double>(time) + 1.0)) return false;
    }
    if (PointCount() >= 100000) return false;
    points.append(QPointF(time, value));
    segmentTypes.append(0);
    bezierControls.append(QVector<QPointF>());
    return true;
}

bool CMotionTimeline::UpdatePoint(const QString& parameterId, int index, float time, float value)
{
    if (parameterId.isEmpty() || !std::isfinite(time) || time < 0.0f || !std::isfinite(value)) return false;
    auto curve = m_curves.find(parameterId);
    if (curve == m_curves.end() || index < 0 || index >= curve->size()) return false;
    for (int i = 0; i < curve->size(); ++i) {
        if (i != index && qFuzzyCompare(curve->at(i).x() + 1.0, static_cast<double>(time) + 1.0)) return false;
    }
    QVector<QPair<QPointF, QPair<int, QVector<QPointF>>>> entries;
    const QVector<int> segmentTypes = m_segmentTypes.value(parameterId);
    const QVector<QVector<QPointF>> bezierControls = m_bezierControls.value(parameterId);
    for (int i = 0; i < curve->size(); ++i)
        entries.append(qMakePair(i == index ? QPointF(time, value) : curve->at(i),
                                 qMakePair(segmentTypes.value(i, 0), bezierControls.value(i))));
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right){
        return left.first.x() < right.first.x();
    });
    for (int i = 0; i < entries.size(); ++i) {
        (*curve)[i] = entries[i].first;
        m_segmentTypes[parameterId][i] = entries[i].second.first;
        m_bezierControls[parameterId][i] = entries[i].second.second;
    }
    return true;
}

bool CMotionTimeline::RemovePoint(const QString& parameterId, int index)
{
    auto curve = m_curves.find(parameterId);
    if (curve == m_curves.end() || index < 0 || index >= curve->size()) return false;
    curve->removeAt(index);
    if (m_segmentTypes.contains(parameterId)) m_segmentTypes[parameterId].removeAt(index);
    if (m_bezierControls.contains(parameterId)) m_bezierControls[parameterId].removeAt(index);
    const bool empty = curve->isEmpty();
    if (empty) {
        m_curves.erase(curve);
        m_segmentTypes.remove(parameterId);
        m_bezierControls.remove(parameterId);
    }
    return true;
}

bool CMotionTimeline::SetSegmentType(const QString& parameterId, int pointIndex, int segmentType)
{
    if (pointIndex <= 0 || segmentType < 0 || segmentType > 3) return false;
    auto curve = m_curves.find(parameterId);
    if (curve == m_curves.end() || pointIndex >= curve->size()) return false;
    QVector<int>& types = m_segmentTypes[parameterId];
    types.resize(curve->size());
    m_bezierControls[parameterId].resize(curve->size());
    types[pointIndex] = segmentType;
    return true;
}

int CMotionTimeline::SegmentType(const QString& parameterId, int pointIndex) const
{
    return m_segmentTypes.value(parameterId).value(pointIndex, 0);
}

float CMotionTimeline::Duration() const
{
    float duration = 0.0f;
    for (const auto& points : m_curves)
        if (!points.isEmpty()) duration = qMax(duration, static_cast<float>(points.last().x()));
    return duration;
}

float CMotionTimeline::Evaluate(const QString& parameterId, float time, bool* ok) const
{
    const QVector<QPointF> points = m_curves.value(parameterId);
    if (points.isEmpty() || !std::isfinite(time)) {
        if (ok) *ok = false;
        return 0.0f;
    }
    if (ok) *ok = true;
    if (time <= points.first().x()) return static_cast<float>(points.first().y());
    for (int i = 1; i < points.size(); ++i) {
        const QPointF start = points[i - 1];
        const QPointF end = points[i];
        if (time > end.x()) continue;
        const float span = static_cast<float>(end.x() - start.x());
        const float ratio = span > 0.0f ? qBound(0.0f, static_cast<float>((time - start.x()) / span), 1.0f) : 1.0f;
        const int type = SegmentType(parameterId, i);
        if (type == 2) return static_cast<float>(start.y());
        if (type == 3) return static_cast<float>(end.y());
        if (type != 1) return static_cast<float>(start.y() + (end.y() - start.y()) * ratio);
        const auto cubic = [](float a, float b, float c, float d, float t) {
            const float inverse = 1.0f - t;
            return inverse * inverse * inverse * a + 3.0f * inverse * inverse * t * b +
                   3.0f * inverse * t * t * c + t * t * t * d;
        };
        const QVector<QPointF> controls = m_bezierControls.value(parameterId).value(i);
        const QVector<QPointF> fallback = defaultBezierControls(start, end);
        const QVector<QPointF> selectedControls = validBezierControls(controls, start, end) ? controls : fallback;
        const QPointF first = selectedControls.at(0);
        const QPointF second = selectedControls.at(1);
        const float firstRatio = qBound(0.0f, static_cast<float>((first.x() - start.x()) / span), 1.0f);
        const float secondRatio = qBound(0.0f, static_cast<float>((second.x() - start.x()) / span), 1.0f);
        float low = 0.0f;
        float high = 1.0f;
        for (int step = 0; step < 24; ++step) {
            const float middle = (low + high) * 0.5f;
            if (cubic(0.0f, firstRatio, secondRatio, 1.0f, middle) < ratio) low = middle;
            else high = middle;
        }
        const float valueControl = (low + high) * 0.5f;
        return cubic(static_cast<float>(start.y()), static_cast<float>(first.y()),
                     static_cast<float>(second.y()), static_cast<float>(end.y()), valueControl);
    }
    return static_cast<float>(points.last().y());
}

QMap<QString, float> CMotionTimeline::EvaluateAll(float time) const
{
    QMap<QString, float> values;
    for (auto it = m_curves.cbegin(); it != m_curves.cend(); ++it) {
        bool valid = false;
        const float value = Evaluate(it.key(), time, &valid);
        if (valid) values.insert(it.key(), value);
    }
    return values;
}

int CMotionTimeline::PointCount() const
{
    int count = 0;
    for (const auto& points : m_curves) count += points.size();
    return count;
}

bool CMotionTimeline::Save(const QString& path, float fps, bool loop, QString* error) const
{
    if (path.isEmpty() || m_curves.isEmpty()) {
        if (error) *error = QStringLiteral("没有可导出的关键帧");
        return false;
    }
    const float outputFps = std::isfinite(fps) && fps > 0.0f ? fps : 30.0f;
    QJsonArray curves;
    int totalSegments = 0;
    int totalPoints = 0;
    float duration = 0.0f;
    for (auto it = m_curves.cbegin(); it != m_curves.cend(); ++it) {
        if (it.value().isEmpty()) continue;
        QVector<QPointF> points = it.value();
        QVector<QVector<QPointF>> bezierControls = m_bezierControls.value(it.key());
        if (points.size() == 1) {
            const float endpoint = qMax(1.0f / outputFps, static_cast<float>(points.first().x()));
            points.append(QPointF(endpoint, points.first().y()));
            bezierControls.append(QVector<QPointF>());
        }
        QJsonArray segments;
        segments.append(points.first().x());
        segments.append(points.first().y());
        int curvePointCount = 1;
        for (int i = 1; i < points.size(); ++i) {
            const int segmentType = m_segmentTypes.value(it.key()).value(i, 0);
            segments.append(segmentType);
            if (segmentType == 1) {
                const QPointF& start = points[i - 1];
                const QPointF& end = points[i];
                const QVector<QPointF> fallback = defaultBezierControls(start, end);
                const QVector<QPointF> controls = bezierControls.value(i).size() == 2
                    ? bezierControls.value(i) : fallback;
                const QPointF first = controls.at(0);
                const QPointF second = controls.at(1);
                segments.append(first.x()); segments.append(first.y());
                segments.append(second.x()); segments.append(second.y());
                segments.append(end.x()); segments.append(end.y());
                curvePointCount += 3;
            } else {
                segments.append(points[i].x());
                segments.append(points[i].y());
                ++curvePointCount;
            }
        }
        QJsonObject curve;
        curve.insert(QStringLiteral("Target"), QStringLiteral("Parameter"));
        curve.insert(QStringLiteral("Id"), it.key());
        curve.insert(QStringLiteral("Segments"), segments);
        curves.append(curve);
        totalPoints += curvePointCount;
        totalSegments += points.size() - 1;
        duration = qMax(duration, static_cast<float>(points.last().x()));
    }
    if (curves.isEmpty()) {
        if (error) *error = QStringLiteral("没有有效的参数曲线");
        return false;
    }
    QJsonObject meta;
    meta.insert(QStringLiteral("Duration"), duration);
    meta.insert(QStringLiteral("Fps"), outputFps);
    meta.insert(QStringLiteral("Loop"), loop);
    meta.insert(QStringLiteral("AreBeziersRestricted"), true);
    meta.insert(QStringLiteral("CurveCount"), curves.size());
    meta.insert(QStringLiteral("TotalSegmentCount"), totalSegments);
    meta.insert(QStringLiteral("TotalPointCount"), totalPoints);
    QJsonObject root;
    root.insert(QStringLiteral("Version"), 3);
    root.insert(QStringLiteral("Meta"), meta);
    root.insert(QStringLiteral("Curves"), curves);
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
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = file.errorString();
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("motion3 JSON 无效：%1").arg(parseError.errorString());
        return false;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("Version")).toInt() != 3 || !root.value(QStringLiteral("Curves")).isArray()) {
        if (error) *error = QStringLiteral("不支持的 motion3 版本或曲线字段");
        return false;
    }
    QMap<QString, QVector<QPointF>> curves;
    QMap<QString, QVector<int>> segmentTypes;
    QMap<QString, QVector<QVector<QPointF>>> bezierControlsMap;
    const QJsonArray sourceCurves = root.value(QStringLiteral("Curves")).toArray();
    for (const QJsonValue& curveValue : sourceCurves) {
        if (!curveValue.isObject()) {
            if (error) *error = QStringLiteral("motion3 曲线不是对象");
            return false;
        }
        const QJsonObject curve = curveValue.toObject();
        if (curve.value(QStringLiteral("Target")).toString() != QStringLiteral("Parameter")) continue;
        const QString id = curve.value(QStringLiteral("Id")).toString();
        const QJsonArray segments = curve.value(QStringLiteral("Segments")).toArray();
        if (id.isEmpty() || segments.size() < 2 || curves.contains(id)) {
            if (error) *error = QStringLiteral("motion3 参数曲线无效或重复：%1").arg(id);
            return false;
        }
        bool valid = true;
        QVector<QPointF> points;
        QVector<int> types;
        QVector<QVector<QPointF>> controls;
        const auto number = [&segments](int index, bool* ok) {
            if (index < 0 || index >= segments.size() || !segments.at(index).isDouble()) {
                *ok = false;
                return 0.0;
            }
            const double value = segments.at(index).toDouble();
            *ok = std::isfinite(value);
            return value;
        };
        bool ok = true;
        const double firstTime = number(0, &ok);
        const double firstValue = number(1, &ok);
        if (!ok || firstTime < 0.0) valid = false;
        if (valid) {
            points.append(QPointF(firstTime, firstValue));
            types.append(0);
            controls.append(QVector<QPointF>());
        }
        int index = 2;
        while (valid && index < segments.size()) {
            const double segmentNumber = number(index++, &ok);
            const int type = static_cast<int>(segmentNumber);
            if (!ok || segmentNumber != type || type < 0 || type > 3) { valid = false; break; }
            const int values = type == 1 ? 6 : 2;
            if (index + values > segments.size()) { valid = false; break; }
            QVector<QPointF> segmentPoints;
            for (int point = 0; point < values / 2; ++point) {
                const double time = number(index++, &ok);
                const double value = number(index++, &ok);
                if (!ok || time < 0.0) { valid = false; break; }
                segmentPoints.append(QPointF(time, value));
            }
            if (!valid) break;
            const QPointF endpoint = segmentPoints.last();
            if (endpoint.x() <= points.last().x()) { valid = false; break; }
            if (type == 1 && !validBezierControls({segmentPoints.at(0), segmentPoints.at(1)}, points.last(), endpoint)) {
                valid = false;
                break;
            }
            points.append(endpoint);
            types.append(type);
            controls.append(type == 1 ? QVector<QPointF>{segmentPoints.at(0), segmentPoints.at(1)} : QVector<QPointF>{});
        }
        if (!valid || points.size() < 2) {
            if (error) *error = QStringLiteral("motion3 曲线段无效：%1").arg(id);
            return false;
        }
        curves.insert(id, points);
        segmentTypes.insert(id, types);
        bezierControlsMap.insert(id, controls);
    }
    if (curves.isEmpty()) {
        if (error) *error = QStringLiteral("motion3 没有参数曲线");
        return false;
    }
    m_curves.swap(curves);
    m_segmentTypes.swap(segmentTypes);
    m_bezierControls.swap(bezierControlsMap);
    return true;
}
