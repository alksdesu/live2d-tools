#ifndef CMOTIONTIMELINE_H
#define CMOTIONTIMELINE_H

#include <QElapsedTimer>
#include <QMap>
#include <QPointF>
#include <QString>
#include <QVector>

class CMotionTimeline
{
public:
    void StartRecording();
    void StopRecording();
    bool IsRecording() const { return m_recording; }
    void Clear();
    void Record(const QString& parameterId, float value);
    bool AddPoint(const QString& parameterId, float time, float value);
    bool UpdatePoint(const QString& parameterId, int index, float time, float value);
    bool RemovePoint(const QString& parameterId, int index);
    bool Save(const QString& path, float fps, bool loop, QString* error = nullptr) const;
    int PointCount() const;
    const QMap<QString, QVector<QPointF>>& Curves() const { return m_curves; }

private:
    bool m_recording = false;
    QElapsedTimer m_clock;
    QMap<QString, QVector<QPointF>> m_curves;
};

#endif
