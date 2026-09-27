#ifndef CONIONSKIN_H
#define CONIONSKIN_H

#include "cmotiontimeline.h"
#include <QSize>
#include <QString>
#include <memory>

namespace live2dstruct {
class COnionSkin
{
public:
    struct Settings {
        bool enabled = false;
        bool duringPlayback = false;
        int before = 1, after = 1, stepFrames = 1;
        float beforeOpacity = .25f, afterOpacity = .25f;
        bool operator==(const Settings& other) const;
        bool operator!=(const Settings& other) const { return !(*this == other); }
    };
    class Timeline {
    public:
        bool IsValid() const { return bool(m_data); }
        QByteArray Fingerprint() const;
    private:
        struct Data;
        std::shared_ptr<const Data> m_data;
        friend class COnionSkin;
    };
    struct Request {
        Settings settings;
        Timeline timeline;
        double time = 0;
        bool useFades = false, loop = false, playing = false, recording = false;
        bool suspended = false;
        QString suspensionReason;
        quint64 sourceRevision = 0;
    };
    struct Base {
        QMap<QString, float> parameters, parts;
        QSet<QString> eyeBlinkIds, lipSyncIds, hiddenParts;
        float modelOpacity = 1;
    };
    struct Sample {
        double time = 0;
        int offset = 0;
        float opacity = 0;
        CMotionPreviewFrame frame;
    };
    struct Plan {
        QVector<Sample> samples;
        QByteArray fingerprint;
        int omitted = 0;
    };
    struct Status {
        enum class State { Disabled, Suspended, Preparing, Ready, Failed };
        State state = State::Disabled;
        QString message;
        QVector<double> sampleTimes;
        int omitted = 0;
        QSize requestedSize, renderedSize;
        bool reducedResolution = false;
        quint64 generation = 0;
        bool operator==(const Status& other) const;
        bool operator!=(const Status& other) const { return !(*this == other); }
    };
    static constexpr int MaximumPerSide = 4;
    static constexpr qint64 MaximumGpuBytes = 128LL * 1024 * 1024;
    static bool Validate(const Settings& settings, QString* error = nullptr);
    static bool CaptureTimeline(const CMotionTimeline& timeline, Timeline& result, QString* error = nullptr);
    static bool Build(const Request& request, const Base& base, Plan& result, QString* error = nullptr);
    static bool SelectResolution(const QSize& requested, int maximumDimension, qint64 reservedBytes,
                                 QSize& result, QString* error = nullptr);
};
}
Q_DECLARE_METATYPE(live2dstruct::COnionSkin::Settings)
Q_DECLARE_METATYPE(live2dstruct::COnionSkin::Status)
#endif
