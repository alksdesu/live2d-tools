#include "cmotiontimeline.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <cmath>

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
    if (!points.isEmpty()) {
        if (time < points.last().x()) time = points.last().x();
        if (qFuzzyCompare(points.last().y() + 1.0, static_cast<double>(value) + 1.0) &&
            qFuzzyCompare(points.last().x() + 1.0, static_cast<double>(time) + 1.0)) return false;
    }
    if (PointCount() >= 100000) return false;
    points.append(QPointF(time, value));
    return true;
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
        for (int i = 1; i < points.size(); ++i) {
            segments.append(0);
            segments.append(points[i].x());
            segments.append(points[i].y());
        }
        QJsonObject curve;
        curve.insert(QStringLiteral("Target"), QStringLiteral("Parameter"));
        curve.insert(QStringLiteral("Id"), it.key());
        curve.insert(QStringLiteral("Segments"), segments);
        curves.append(curve);
        totalPoints += points.size();
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
