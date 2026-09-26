#ifndef CPHYSICSBAKE_H
#define CPHYSICSBAKE_H

#include "live2dstruct/cmotiontimeline.h"
#include <QByteArray>
#include <QMap>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QVector>
#include <memory>

class CPhysicsBake
{
public:
    struct Request {
        QByteArray mocBytes;
        QByteArray physicsBytes;
        QByteArray expectedMocDigest;
        QByteArray expectedPhysicsDigest;
        QMap<QString, float> baseParameters;
        CMotionTimeline timeline;
        QSet<QString> outputIds;
        QSet<QString> eyeBlinkIds;
        QSet<QString> lipSyncIds;
        bool useFades = true;
        bool loop = false;
        bool newAnimation = false;
        double fps = 30.0;
        double duration = 0.0;
        int warmupCycles = 1;
    };
    struct Result {
        QMap<QString, QVector<QPointF>> curves;
        double endTime = 0.0;
        double fps = 0.0;
    };

    CPhysicsBake();
    ~CPhysicsBake();
    CPhysicsBake(const CPhysicsBake&) = delete;
    CPhysicsBake& operator=(const CPhysicsBake&) = delete;

    bool Prepare(const Request& request, QString* error = nullptr);
    bool Step(int maximumFrames = 16, QString* error = nullptr);
    void Cancel() noexcept;
    bool IsFinished() const noexcept;
    bool IsCanceled() const noexcept;
    bool Failed() const noexcept;
    qint64 CompletedSteps() const noexcept;
    qint64 TotalSteps() const noexcept;
    QString Error() const;
    bool CaptureResult(Result& result, QString* error = nullptr) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif
