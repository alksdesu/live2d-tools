#include "cmotiontimeline.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <limits>

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
    QVector<QPair<QPointF, int>> entries;
    const QVector<int> segmentTypes = m_segmentTypes.value(parameterId);
    for (int i = 0; i < curve->size(); ++i)
        entries.append(qMakePair(i == index ? QPointF(time, value) : curve->at(i), segmentTypes.value(i, 0)));
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right){
        return left.first.x() < right.first.x();
    });
    for (int i = 0; i < entries.size(); ++i) {
        (*curve)[i] = entries[i].first;
        m_segmentTypes[parameterId][i] = entries[i].second;
    }
    return true;
}

bool CMotionTimeline::RemovePoint(const QString& parameterId, int index)
{
    auto curve = m_curves.find(parameterId);
    if (curve == m_curves.end() || index < 0 || index >= curve->size()) return false;
    curve->removeAt(index);
    if (m_segmentTypes.contains(parameterId)) m_segmentTypes[parameterId].removeAt(index);
    const bool empty = curve->isEmpty();
    if (empty) {
        m_curves.erase(curve);
        m_segmentTypes.remove(parameterId);
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
        float low = 0.0f;
        float high = 1.0f;
        for (int step = 0; step < 24; ++step) {
            const float middle = (low + high) * 0.5f;
            if (cubic(0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f, middle) < ratio) low = middle;
            else high = middle;
        }
        const float control = (low + high) * 0.5f;
        return cubic(static_cast<float>(start.y()),
                     static_cast<float>(start.y() + (end.y() - start.y()) / 3.0),
                     static_cast<float>(start.y() + (end.y() - start.y()) * 2.0 / 3.0),
                     static_cast<float>(end.y()), control);
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
        if (points.size() == 1) {
            const float endpoint = qMax(1.0f / outputFps, static_cast<float>(points.first().x()));
            points.append(QPointF(endpoint, points.first().y()));
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
                const QPointF first(start.x() + (end.x() - start.x()) / 3.0,
                                    start.y() + (end.y() - start.y()) / 3.0);
                const QPointF second(start.x() + (end.x() - start.x()) * 2.0 / 3.0,
                                     start.y() + (end.y() - start.y()) * 2.0 / 3.0);
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
