#ifndef CMOTIONTIMELINE_H
#define CMOTIONTIMELINE_H

#include <QElapsedTimer>
#include <QMap>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <QVector>
#include <QSet>
#include <QMetaType>
#include <optional>
#include <functional>

enum class CMotionTarget { Model, Parameter, PartOpacity };

struct CMotionTrack
{
    CMotionTarget target = CMotionTarget::Parameter;
    QString id;
    bool operator<(const CMotionTrack& other) const {
        return target != other.target ? target < other.target : id < other.id;
    }
    bool operator==(const CMotionTrack& other) const { return target == other.target && id == other.id; }
    bool operator!=(const CMotionTrack& other) const { return !(*this == other); }
    QString TargetName() const;
};

struct CMotionPreviewFrame
{
    QMap<QString, float> parameters;
    QMap<QString, float> model;
    QMap<QString, float> parts;
    bool IsFinite() const;
    bool IsEmpty() const { return parameters.isEmpty() && model.isEmpty() && parts.isEmpty(); }
    QMap<QString, float> ResolveParameters(const QSet<QString>& eyeBlinkIds, const QSet<QString>& lipSyncIds) const;
};

struct CMotionFadeTimes
{
    std::optional<double> fadeIn;
    std::optional<double> fadeOut;
};

struct CMotionTiming
{
    double fps = 30.0;
    double duration = 0.0;
};

struct CMotionEventOccurrence
{
    double time = 0.0;
    QString value;
    int sourceIndex = 0;
    quint64 cycle = 0;
};

struct CMotionEventBatch
{
    QVector<CMotionEventOccurrence> occurrences;
    quint64 omittedCount = 0;
    // 饱和后 omittedCount 仅表示下限。
    bool countSaturated = false;
    bool valid = true;
};

class CMotionTimeline
{
public:
    class ParameterTracks
    {
        friend class CMotionTimeline;
    public:
        bool IsEmpty() const;
        QSet<QString> Ids() const;
    private:
        struct Track {
            bool present = false;
            bool segmentsPresent = false;
            bool controlsPresent = false;
            QVector<QPointF> points;
            QVector<int> segments;
            QVector<QVector<QPointF>> controls;
            QJsonObject source;
            int sourceIndex = -1;
        };
        QMap<QString, Track> m_tracks;
        QJsonValue m_durationBefore, m_durationAfter;
        bool m_promotedDuration = false;
        bool m_metaPresent = false;
    };
    bool RemoveParameterTracks(const QSet<QString>& ids, ParameterTracks& removed, QString* error = nullptr);
    bool RestoreParameterTracks(const ParameterTracks& tracks, QString* error = nullptr);
    void SwapContent(CMotionTimeline& other) noexcept;

    explicit CMotionTimeline(std::function<qint64()> recordingClock = {});
    void StartRecording();
    void StopRecording();
    bool IsRecording() const { return m_recording; }
    bool PauseRecordingClock();
    bool ResumeRecordingClock();
    bool IsRecordingClockPaused() const { return m_recordClockPaused; }
    void Clear();
    void Record(const QString& parameterId, float value);
    bool AddPoint(const QString& id, float time, float value) { return AddPoint({CMotionTarget::Parameter, id}, time, value); }
    bool UpdatePoint(const QString& id, int index, float time, float value) { return UpdatePoint({CMotionTarget::Parameter, id}, index, time, value); }
    bool RemovePoint(const QString& id, int index) { return RemovePoint({CMotionTarget::Parameter, id}, index); }
    bool SetSegmentType(const QString& id, int index, int type) { return SetSegmentType({CMotionTarget::Parameter, id}, index, type); }
    int SegmentType(const QString& id, int index) const { return SegmentType({CMotionTarget::Parameter, id}, index); }
    bool AddPoint(const CMotionTrack& track, float time, float value);
    bool InsertPoint(const CMotionTrack& track, float time, float value);
    bool UpdatePoint(const CMotionTrack& track, int index, float time, float value);
    bool RemovePoint(const CMotionTrack& track, int index);
    bool SetSegmentType(const CMotionTrack& track, int pointIndex, int segmentType);
    int SegmentType(const CMotionTrack& track, int pointIndex) const;
    float Duration() const;
    float Fps() const;
    CMotionTiming Timing() const;
    bool Loop() const;
    CMotionFadeTimes FadeTimes() const;
    bool SetFadeTimes(const CMotionFadeTimes& fades, QString* error = nullptr);
    CMotionFadeTimes TrackFadeTimes(const CMotionTrack& track) const;
    QMap<CMotionTrack, CMotionFadeTimes> TrackFadeTimes() const;
    bool SetTrackFadeTimes(const CMotionTrack& track, const CMotionFadeTimes& fades, QString* error = nullptr);
    QJsonArray Events() const;
    bool SetEvents(const QJsonArray& events, QString* error = nullptr);
    // 默认查询左开右闭区间，循环边界先发末尾事件，再发下一轮零时刻事件。
    CMotionEventBatch EventsBetween(double startTime, double elapsedAdvance, bool loop,
                                   bool includeStart = false, int maxOccurrences = 100) const;
    bool SetTiming(double fps, double duration, QString* error = nullptr);
    float Evaluate(const QString& id, float time, bool* ok = nullptr) const { return Evaluate({CMotionTarget::Parameter, id}, time, ok); }
    float Evaluate(const CMotionTrack& track, float time, bool* ok = nullptr) const;
    CMotionPreviewFrame EvaluateFrame(float time) const;
    QMap<QString, float> EvaluateAll(float time) const;
    bool Save(const QString& path, float fps, bool loop, QString* error = nullptr) const;
    bool ToJson(QByteArray& bytes, float fps, bool loop, QString* error = nullptr) const;
    bool Load(const QString& path, QString* error = nullptr);
    static bool RecountMetadata(QJsonObject& motion, QString* error = nullptr, bool draft = false, double* duration = nullptr);
    bool SaveState(QJsonObject* state, bool loop, double playhead, QString* error = nullptr) const;
    bool RestoreState(const QJsonObject& state, float* playhead = nullptr, QString* error = nullptr);
    bool HasCurves() const;
    bool HasAnimation() const;
    bool HasSameContent(const CMotionTimeline& other) const;
    int PointCount() const;
    int TotalPointCount() const;
    QMap<QString, QVector<QPointF>> Curves() const;
    const QMap<CMotionTrack, QVector<QPointF>>& Tracks() const { return m_curves; }

private:
    bool BuildJson(QJsonObject* root, double fps, bool loop, bool draft, QString* error) const;
    bool LoadJson(const QJsonObject& root, bool draft, QString* error, bool allowEmpty = false);
    double ContentDuration() const;
    const QVector<CMotionEventOccurrence>& SortedEvents() const;
    bool SetSource(QJsonObject source, QString* error);
    QJsonObject m_source;
    mutable QJsonArray m_cachedEvents;
    mutable QVector<CMotionEventOccurrence> m_sortedEvents;
    std::optional<qint64> RecordingElapsedMilliseconds() const;
    std::function<qint64()> m_recordingClock;
    qint64 m_recordClockOrigin = 0;
    qint64 m_recordSkippedMs = 0;
    qint64 m_recordPauseMs = 0;
    qint64 m_recordLastMs = 0;
    bool m_recordClockPaused = false;
    bool m_recording = false;
    float m_recordStartTime = 0.0f;
    QElapsedTimer m_clock;
    QMap<CMotionTrack, QVector<QPointF>> m_curves;
    QMap<CMotionTrack, QVector<int>> m_segmentTypes;
    QMap<CMotionTrack, QVector<QVector<QPointF>>> m_bezierControls;
};

Q_DECLARE_METATYPE(CMotionTrack)
Q_DECLARE_METATYPE(CMotionPreviewFrame)
Q_DECLARE_METATYPE(CMotionEventOccurrence)
Q_DECLARE_METATYPE(CMotionEventBatch)

#endif
