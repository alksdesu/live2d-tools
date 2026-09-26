#include "live2dstruct/cmotiontimeline.h"
#include "framework/Math/CubismMath.hpp"
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QFile>
#include <QDirIterator>
#include <algorithm>
#include <limits>
#include <cmath>
#include <stdexcept>

class CMotionTimelineTests : public QObject
{
    Q_OBJECT
private:
    static bool writeJson(const QString& path, const QJsonObject& root)
    {
        QFile file(path);
        const QByteArray bytes = QJsonDocument(root).toJson();
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    }

    static QJsonObject readJson(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }

    static QJsonObject curve(const QString& target, const QString& id, const QJsonArray& points)
    {
        return {{"Target", target}, {"Id", id}, {"Segments", points}};
    }

private slots:

    void recountReportsEffectiveDurationWithoutMaterializingIt()
    {
        QJsonObject motion{{"Version", 3}, {"Curves", QJsonArray{
            curve("Parameter", "A", {0, 0, 0, 10, 1})}},
            {"Meta", QJsonObject{{"Fps", 30}, {"Extension", "keep"}}}};
        double duration = -17;
        QVERIFY(CMotionTimeline::RecountMetadata(motion, nullptr, true, &duration));
        QCOMPARE(duration, 10.0);
        QVERIFY(!motion.value("Meta").toObject().contains("Duration"));
        QCOMPARE(motion.value("Meta").toObject().value("Extension"), QJsonValue("keep"));
        QCOMPARE(motion.value("Meta").toObject().value("TotalPointCount").toInt(), 2);
        motion["Curves"] = QJsonArray{curve("Parameter", "A", {0, 0, 9})};
        const auto invalid = motion;
        duration = -17;
        QVERIFY(!CMotionTimeline::RecountMetadata(motion, nullptr, true, &duration));
        QCOMPARE(duration, -17.0); QCOMPARE(motion, invalid);
    }

    void parameterTrackRemovalKeepsTypedPeersAndSourceFields()
    {
        auto parameter = curve("Parameter", "A", {0, 0, 1, 0.3, 0.2, 0.7, 0.8, 1, 1});
        parameter["FadeInTime"] = 0.25; parameter["Extension"] = QJsonObject{{"unknown", 7}};
        const QJsonObject motion{{"Version", 3}, {"RootExtra", "keep"}, {"Curves", QJsonArray{
            parameter, curve("PartOpacity", "A", {0, 0.4, 0, 1, 0.6}), curve("Parameter", "B", {0, 2, 0, 1, 3})}},
            {"Meta", QJsonObject{{"Fps", 30}, {"Duration", 1}, {"MetaExtra", true}}}};
        CMotionTimeline timeline;
        QVERIFY(timeline.RestoreState({{"version", 1}, {"motion", motion}, {"playhead", 0}}));
        QJsonObject before; QVERIFY(timeline.SaveState(&before, false, 0));
        CMotionTimeline::ParameterTracks removed;
        QVERIFY(timeline.RemoveParameterTracks({"A"}, removed));
        QVERIFY(!removed.IsEmpty()); QCOMPARE(removed.Ids(), QSet<QString>{"A"});
        QVERIFY(!timeline.Tracks().contains({CMotionTarget::Parameter, "A"}));
        QVERIFY(timeline.Tracks().contains({CMotionTarget::PartOpacity, "A"}));
        QCOMPARE(timeline.PointCount(), 2);
        QVERIFY(timeline.RestoreParameterTracks(removed));
        QJsonObject restored; QVERIFY(timeline.SaveState(&restored, false, 0));
        QCOMPARE(restored, before);
        QVERIFY(timeline.RemoveParameterTracks({"A"}, removed));
        QVERIFY(timeline.AddPoint("A", 0, 9));
        QVERIFY(timeline.AddPoint("A", 1, 10));
        QVERIFY(!timeline.TrackFadeTimes({CMotionTarget::Parameter, "A"}).fadeIn);
        QJsonObject newState; QVERIFY(timeline.SaveState(&newState, false, 0));
        for (const auto& value : newState.value("motion").toObject().value("Curves").toArray()) {
            const auto row = value.toObject();
            if (row.value("Target") == "Parameter" && row.value("Id") == "A") QVERIFY(!row.contains("Extension"));
        }
        QString error;
        QVERIFY(!timeline.RestoreParameterTracks(removed, &error));
        QVERIFY(error.contains(QStringLiteral("同名")));
        QJsonObject after; QVERIFY(timeline.SaveState(&after, false, 0)); QCOMPARE(after, newState);
    }

    void parameterTrackChangesKeepRecordingClockAndDuration()
    {
        qint64 now = 1000;
        CMotionTimeline timeline([&] { return now; });
        timeline.StartRecording();
        now = 2000; timeline.Record("A", 1);
        now = 3000; timeline.Record("A", 2);
        QCOMPARE(timeline.Duration(), 2.0f);
        CMotionTimeline::ParameterTracks removed;
        QVERIFY(timeline.RemoveParameterTracks({"A"}, removed));
        QVERIFY(timeline.IsRecording()); QCOMPARE(timeline.Duration(), 2.0f);
        now = 4000; timeline.Record("B", 3);
        QCOMPARE(timeline.Tracks().value({CMotionTarget::Parameter, "B"}).first().x(), 3.0);
        QVERIFY(timeline.RestoreParameterTracks(removed));
        QVERIFY(timeline.IsRecording());
        now = 5000; timeline.Record("A", 4);
        QCOMPARE(timeline.Tracks().value({CMotionTarget::Parameter, "A"}).last().x(), 4.0);
        QVERIFY(timeline.PauseRecordingClock());
        now = 10000;
        QVERIFY(timeline.RemoveParameterTracks({"A"}, removed));
        QVERIFY(timeline.IsRecordingClockPaused());
        QVERIFY(timeline.RestoreParameterTracks(removed));
        QVERIFY(timeline.IsRecordingClockPaused());
        QVERIFY(timeline.ResumeRecordingClock());
        now = 11000; timeline.Record("B", 5);
        QCOMPARE(timeline.Tracks().value({CMotionTarget::Parameter, "B"}).last().x(), 5.0);
    }

    void parameterTrackFailuresAreAtomicAndEmptyDeltaIsValid()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("A", 0, 0)); QVERIFY(timeline.AddPoint("A", 5, 1));
        CMotionTimeline::ParameterTracks removed;
        QVERIFY(timeline.RemoveParameterTracks({"A"}, removed));
        QCOMPARE(timeline.Duration(), 5.0f);
        const auto noTrack = timeline;
        QString error;
        QVERIFY(!timeline.RemoveParameterTracks({QString()}, removed, &error));
        QVERIFY(timeline.HasSameContent(noTrack)); QCOMPARE(removed.Ids(), QSet<QString>{"A"});
        QVERIFY(timeline.SetTiming(30, 2));
        const auto shorter = timeline;
        QVERIFY(!timeline.RestoreParameterTracks(removed, &error));
        QVERIFY(timeline.HasSameContent(shorter));
        QVERIFY(timeline.SetTiming(30, 5));
        QVERIFY(timeline.RestoreParameterTracks(removed));
        QCOMPARE(timeline.PointCount(), 2);
        CMotionTimeline::ParameterTracks absent;
        const auto original = timeline;
        QVERIFY(timeline.RemoveParameterTracks({"Missing"}, absent));
        QVERIFY(absent.IsEmpty()); QCOMPARE(absent.Ids(), QSet<QString>{"Missing"});
        QVERIFY(timeline.RestoreParameterTracks(absent));
        QVERIFY(timeline.HasSameContent(original));
    }

    void sdkLoopTimeBoundsRuntimeAndPreservesEndFrame()
    {
        using Live2D::Cubism::Framework::CubismMath;
        QCOMPARE(CubismMath::GetLoopTime(0.0f, 1.0f), 0.0f);
        QCOMPARE(CubismMath::GetLoopTime(0.25f, 1.0f), 0.25f);
        QCOMPARE(CubismMath::GetLoopTime(1.0f, 1.0f), 1.0f);
        QCOMPARE(CubismMath::GetLoopTime(2.0f, 1.0f), 1.0f);
        QCOMPARE(CubismMath::GetLoopTime(10.0f, 0.5f), 0.5f);
        QCOMPARE(CubismMath::GetLoopTime(10.25f, 0.5f), 0.25f);
        const float belowEnd = std::nextafterf(1.0f, 0.0f);
        QCOMPARE(CubismMath::GetLoopTime(belowEnd, 1.0f), belowEnd);
        const float aboveEnd = std::nextafterf(1.0f, 2.0f);
        QCOMPARE(CubismMath::GetLoopTime(aboveEnd, 1.0f), aboveEnd - 1.0f);
        const float tiny = std::numeric_limits<float>::denorm_min();
        QCOMPARE(CubismMath::GetLoopTime(std::numeric_limits<float>::max(), tiny), tiny);
        const float hugeRemainder = CubismMath::GetLoopTime(std::numeric_limits<float>::max(), 0.03f);
        QVERIFY(hugeRemainder > 0.0f);
        QVERIFY(hugeRemainder <= 0.03f);
        for (float invalid : {-1.0f, std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity()}) {
            QCOMPARE(CubismMath::GetLoopTime(invalid, 1.0f), 0.0f);
            QCOMPARE(CubismMath::GetLoopTime(1.0f, invalid), 0.0f);
        }
        QCOMPARE(CubismMath::GetLoopTime(1.0f, 0.0f), 0.0f);
    }

    void eventPlaybackUsesOpenLeftBoundaryAndStableSourceOrder()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.SetEvents({QJsonObject{{"Time", 1.0}, {"Value", "first"}},
                                    QJsonObject{{"Time", 0.0}, {"Value", QStringLiteral("开场🌸")}},
                                    QJsonObject{{"Time", 1.0}, {"Value", "second"}},
                                    QJsonObject{{"Time", 2.0}, {"Value", "end"}}}));
        const auto start = timeline.EventsBetween(0, 0, false, true);
        QVERIFY(start.valid);
        QCOMPARE(start.occurrences.size(), 1);
        QCOMPARE(start.occurrences.first().value, QStringLiteral("开场🌸"));
        QCOMPARE(start.occurrences.first().sourceIndex, 1);
        QVERIFY(timeline.EventsBetween(0, 0, false).occurrences.isEmpty());
        const auto first = timeline.EventsBetween(0, 1, false);
        QCOMPARE(first.occurrences.size(), 2);
        QCOMPARE(first.occurrences.at(0).sourceIndex, 0);
        QCOMPARE(first.occurrences.at(1).sourceIndex, 2);
        QCOMPARE(first.occurrences.at(0).time, 1.0);
        QCOMPARE(first.occurrences.at(0).cycle, quint64(0));
        const auto second = timeline.EventsBetween(1, 100, false);
        QCOMPARE(second.occurrences.size(), 1);
        QCOMPARE(second.occurrences.first().value, QStringLiteral("end"));
        QCOMPARE(second.omittedCount, quint64(0));
        QVERIFY(!second.countSaturated);
        QCOMPARE(timeline.EventsBetween(1, 0, false, true).occurrences.size(), 2);
        QVERIFY(timeline.EventsBetween(2, 1, false).occurrences.isEmpty());
        QVERIFY(timeline.EventsBetween(0, std::nextafter(1.0, 0.0), false).occurrences.isEmpty());
    }

    void eventPlaybackCrossesLoopBoundariesWithoutDuplicates()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.SetEvents({QJsonObject{{"Time", 0.0}, {"Value", "start"}},
                                    QJsonObject{{"Time", 0.5}, {"Value", "middle"}},
                                    QJsonObject{{"Time", 1.0}, {"Value", "end"}}}));
        const auto first = timeline.EventsBetween(0, 1, true, true);
        QCOMPARE(first.occurrences.size(), 4);
        QCOMPARE(first.occurrences.at(2).value, QStringLiteral("end"));
        QCOMPARE(first.occurrences.at(2).cycle, quint64(0));
        QCOMPARE(first.occurrences.at(3).value, QStringLiteral("start"));
        QCOMPARE(first.occurrences.at(3).cycle, quint64(1));
        const auto continuation = timeline.EventsBetween(0, 0.5, true);
        QCOMPARE(continuation.occurrences.size(), 1);
        QCOMPARE(continuation.occurrences.first().value, QStringLiteral("middle"));
        const auto endpointContinuation = timeline.EventsBetween(1, 0.5, true);
        QCOMPARE(endpointContinuation.occurrences.size(), 1);
        QCOMPARE(endpointContinuation.occurrences.first().value, QStringLiteral("middle"));
        QCOMPARE(endpointContinuation.occurrences.first().cycle, quint64(1));
        QVERIFY(timeline.EventsBetween(1, 0, true).occurrences.isEmpty());
        const auto missed = timeline.EventsBetween(0.5, 3.75, true);
        QCOMPARE(missed.occurrences.size(), 11);
        QCOMPARE(missed.occurrences.first().value, QStringLiteral("end"));
        QCOMPARE(missed.occurrences.last().value, QStringLiteral("start"));
        QCOMPARE(missed.occurrences.last().cycle, quint64(4));
        const auto bounded = timeline.EventsBetween(0.5, 3.75, true, false, 5);
        QCOMPARE(bounded.occurrences.size(), 5);
        QCOMPARE(bounded.omittedCount, quint64(6));
        QCOMPARE(bounded.occurrences.at(4).cycle, quint64(2));
        const auto counted = timeline.EventsBetween(0.5, 3.75, true, false, 0);
        QVERIFY(counted.occurrences.isEmpty());
        QCOMPARE(counted.omittedCount, quint64(11));
    }

    void eventPlaybackUsesExactDoubleDuration()
    {
        const double duration = 1.1234567890123;
        CMotionTimeline timeline;
        QVERIFY(timeline.SetEvents({QJsonObject{{"Time", duration}, {"Value", "end"}},
                                    QJsonObject{{"Time", 0.0}, {"Value", "start"}}}));
        const auto before = timeline.EventsBetween(0, std::nextafter(duration, 0.0), true);
        QVERIFY(before.occurrences.isEmpty());
        const auto boundary = timeline.EventsBetween(0, duration, true);
        QCOMPARE(boundary.occurrences.size(), 2);
        QCOMPARE(boundary.occurrences.first().time, duration);
        QCOMPARE(boundary.occurrences.last().time, 0.0);
        QCOMPARE(boundary.occurrences.last().cycle, quint64(1));
        const auto nearSecond = timeline.EventsBetween(0, std::nextafter(duration * 2, 0.0), true);
        QCOMPARE(nearSecond.occurrences.size(), 2);
        const auto second = timeline.EventsBetween(0, duration * 2, true);
        QCOMPARE(second.occurrences.size(), 4);
    }

    void eventPlaybackRemainsFreshAfterEditsLoadsAndCopies()
    {
        const QJsonArray original{QJsonObject{{"Time", 2.0}, {"Value", "late"}},
                                  QJsonObject{{"Time", 0.0}, {"Value", "start"}}};
        const QJsonArray edited{QJsonObject{{"Time", 3.0}, {"Value", "new late"}},
                                QJsonObject{{"Time", 1.0}, {"Value", "new first"}}};
        CMotionTimeline timeline;
        QVERIFY(timeline.SetEvents(original));
        const auto cold = timeline;
        QCOMPARE(timeline.EventsBetween(0, 2, false, true).occurrences.first().value, QStringLiteral("start"));
        QCOMPARE(timeline.Timing().duration, 2.0);
        QVERIFY(timeline.HasSameContent(cold));
        QJsonObject state;
        QVERIFY(timeline.SaveState(&state, false, 0));
        QVERIFY(timeline.SetFadeTimes({0.25, 0.5}));
        QCOMPARE(timeline.EventsBetween(0, 2, false, true).occurrences.last().value, QStringLiteral("late"));
        QCOMPARE(timeline.Timing().duration, 2.0);
        CMotionTimeline copied = timeline;
        QVERIFY(timeline.SetEvents(edited));
        QCOMPARE(timeline.Timing().duration, 3.0);
        const auto changed = timeline.EventsBetween(0, 3, false, true);
        QCOMPARE(changed.occurrences.first().value, QStringLiteral("new first"));
        QCOMPARE(changed.occurrences.last().value, QStringLiteral("new late"));
        QCOMPARE(copied.EventsBetween(0, 2, false, true).occurrences.first().value, QStringLiteral("start"));
        QCOMPARE(copied.Timing().duration, 2.0);
        QVERIFY(copied.SetEvents({QJsonObject{{"Time", 4.0}, {"Value", "copy"}}}));
        QCOMPARE(copied.Timing().duration, 4.0);
        QCOMPARE(timeline.Timing().duration, 3.0);
        QVERIFY(timeline.RestoreState(state));
        QCOMPARE(timeline.EventsBetween(0, 2, false, true).occurrences.last().value, QStringLiteral("late"));
        QTemporaryDir temp;
        const auto path = temp.filePath("event-cache.motion3.json");
        QVERIFY(copied.Save(path, 30, false));
        QVERIFY(timeline.Load(path));
        QCOMPARE(timeline.Timing().duration, 4.0);
        QCOMPARE(timeline.EventsBetween(0, 4, false).occurrences.first().value, QStringLiteral("copy"));
        const auto beforeInvalid = timeline;
        QVERIFY(!timeline.SetEvents({QJsonObject{{"Time", -1.0}, {"Value", "invalid"}}}));
        QVERIFY(timeline.HasSameContent(beforeInvalid));
        QCOMPARE(timeline.EventsBetween(0, 4, false).occurrences.first().value, QStringLiteral("copy"));
        timeline.Clear();
        QVERIFY(timeline.EventsBetween(0, 0, false, true).occurrences.isEmpty());
        QCOMPARE(timeline.Timing().duration, 0.0);
        QCOMPARE(copied.EventsBetween(0, 4, false).occurrences.first().value, QStringLiteral("copy"));
    }

    void denseEventPlaybackKeepsRepeatedQueriesBounded()
    {
        QJsonArray source;
        for (int time = 99999; time >= 0; --time)
            source.append(QJsonObject{{"Time", time}, {"Value", QString::number(time)}});
        CMotionTimeline timeline;
        QVERIFY(timeline.SetEvents(source));
        QTemporaryDir temp;
        const auto path = temp.filePath("dense.motion3.json");
        for (int reload = 0; reload < 2; ++reload) {
            if (reload) {
                QVERIFY(timeline.Save(path, 30, false));
                QVERIFY(timeline.Load(path));
            }
            QCOMPARE(timeline.Timing().duration, 99999.0);
            QElapsedTimer timer;
            timer.start();
            for (int iteration = 0; iteration < 256; ++iteration) {
                const double start = iteration * 19;
                const auto events = timeline.EventsBetween(start, 32, false, false, 8);
                QCOMPARE(events.occurrences.size(), 8);
                QCOMPARE(events.omittedCount, quint64(24));
                QCOMPARE(events.occurrences.first().time, start + 1);
                QCOMPARE(events.occurrences.first().sourceIndex, 99999 - static_cast<int>(start + 1));
                QCOMPARE(timeline.Timing().duration, 99999.0);
            }
            qInfo("256 playback queries over 100000 events (reload=%d): %lld ms", reload, timer.elapsed());
        }
    }

    void eventPlaybackMatchesExpandedCycles()
    {
        CMotionTimeline timeline;
        const QVector<double> times{1.0, 0.0, 0.25, 0.5, 0.0, 1.0};
        QJsonArray source;
        for (int index = 0; index < times.size(); ++index)
            source.append(QJsonObject{{"Time", times.at(index)}, {"Value", QString::number(index)}});
        QVERIFY(timeline.SetEvents(source));
        for (int startStep = 0; startStep <= 4; ++startStep) {
            const double start = startStep / 4.0;
            for (int advanceStep = 0; advanceStep <= 20; ++advanceStep) {
                const double advance = advanceStep / 4.0;
                for (bool includeStart : {false, true}) {
                    QVector<CMotionEventOccurrence> expected;
                    for (quint64 cycle = 0; cycle <= 6; ++cycle) {
                        QVector<CMotionEventOccurrence> entries;
                        for (int index = 0; index < times.size(); ++index) {
                            const double absolute = static_cast<double>(cycle) + times.at(index);
                            if ((includeStart ? absolute >= start : absolute > start) && absolute <= start + advance)
                                entries.append({times.at(index), QString::number(index), index, cycle});
                        }
                        std::stable_sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
                            return left.time < right.time;
                        });
                        expected += entries;
                    }
                    const auto actual = timeline.EventsBetween(start, advance, true, includeStart);
                    QCOMPARE(actual.occurrences.size(), expected.size());
                    QCOMPARE(actual.omittedCount, quint64(0));
                    for (int index = 0; index < expected.size(); ++index) {
                        QCOMPARE(actual.occurrences.at(index).time, expected.at(index).time);
                        QCOMPARE(actual.occurrences.at(index).sourceIndex, expected.at(index).sourceIndex);
                        QCOMPARE(actual.occurrences.at(index).cycle, expected.at(index).cycle);
                    }
                }
            }
        }
    }

    void eventPlaybackBoundsHugeAdvancesAndRejectsInvalidInput()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.SetEvents({QJsonObject{{"Time", 0.0}, {"Value", "tick"}}}));
        QVERIFY(timeline.SetTiming(30, 1));
        const auto many = timeline.EventsBetween(0, 1000000, true, false, 4);
        QCOMPARE(many.occurrences.size(), 4);
        QCOMPARE(many.occurrences.at(0).cycle, quint64(1));
        QCOMPARE(many.occurrences.at(3).cycle, quint64(4));
        QCOMPARE(many.omittedCount, quint64(999996));
        QVERIFY(!many.countSaturated);
        const auto hardLimit = timeline.EventsBetween(0, 1000000, true, false, std::numeric_limits<int>::max());
        QCOMPARE(hardLimit.occurrences.size(), 10000);
        QCOMPARE(hardLimit.omittedCount, quint64(990000));
        const quint64 exactLargeCount = (quint64(1) << 53) + 2;
        const auto exactLarge = timeline.EventsBetween(0, static_cast<double>(exactLargeCount), true, false, 0);
        QCOMPARE(exactLarge.omittedCount, exactLargeCount);
        QVERIFY(!exactLarge.countSaturated);
        QVERIFY(timeline.SetTiming(30, std::numeric_limits<double>::denorm_min()));
        const auto huge = timeline.EventsBetween(0, std::numeric_limits<double>::max(), true, true, 7);
        QCOMPARE(huge.occurrences.size(), 7);
        QVERIFY(huge.countSaturated);
        QCOMPARE(huge.omittedCount, std::numeric_limits<quint64>::max() - 7);
        for (double value : {-1.0, std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()}) {
            QVERIFY(!timeline.EventsBetween(value, 0, true).valid);
            QVERIFY(!timeline.EventsBetween(0, value, true).valid);
        }
        QVERIFY(!timeline.EventsBetween(1, 0, true).valid);
        QVERIFY(!timeline.EventsBetween(0, 0, true, false, -1).valid);
        QVERIFY(timeline.SetTiming(30, 0));
        const auto zero = timeline.EventsBetween(0, 100, true, true);
        QCOMPARE(zero.occurrences.size(), 1);
        QVERIFY(!zero.countSaturated);
        QCOMPARE(zero.omittedCount, quint64(0));
        QVERIFY(timeline.EventsBetween(0, 100, true).occurrences.isEmpty());
        CMotionTimeline empty;
        QVERIFY(empty.EventsBetween(0, std::numeric_limits<double>::max(), true).occurrences.isEmpty());
    }

    void eventOnlyMotionRoundtripsAndEmptyExportRemainsRejected()
    {
        CMotionTimeline timeline;
        QVERIFY(!timeline.HasAnimation());
        QVERIFY(timeline.SetEvents({QJsonObject{{"Time", 0.0}, {"Value", QStringLiteral("开始🟦")}},
                                    QJsonObject{{"Time", 2.125}, {"Value", "end"}, {"Extension", "kept"}}}));
        QVERIFY(timeline.HasAnimation());
        QVERIFY(!timeline.HasCurves());
        QVERIFY(timeline.SetTiming(48, 3));
        QTemporaryDir temp;
        const auto path = temp.filePath("events-only.motion3.json");
        QVERIFY(timeline.Save(path, timeline.Fps(), true));
        const auto source = readJson(path);
        const auto meta = source.value("Meta").toObject();
        QCOMPARE(meta.value("CurveCount").toInt(), 0);
        QCOMPARE(meta.value("TotalSegmentCount").toInt(), 0);
        QCOMPARE(meta.value("TotalPointCount").toInt(), 0);
        QCOMPARE(meta.value("UserDataCount").toInt(), 2);
        QCOMPARE(meta.value("Duration").toDouble(), 3.0);
        QCOMPARE(meta.value("TotalUserDataSize").toInt(), QStringLiteral("开始🟦end").toUtf8().size());
        CMotionTimeline loaded;
        QVERIFY(loaded.Load(path));
        QVERIFY(loaded.HasAnimation());
        QVERIFY(!loaded.HasCurves());
        QVERIFY(loaded.Loop());
        QCOMPARE(loaded.Events(), timeline.Events());
        QJsonObject state;
        QVERIFY(loaded.SaveState(&state, true, 2.5));
        CMotionTimeline restored;
        QVERIFY(restored.RestoreState(state));
        QCOMPARE(restored.Events(), timeline.Events());
        QCOMPARE(restored.Timing().duration, 3.0);
        const auto before = loaded;
        QVERIFY(writeJson(path, {{"Version", 3}, {"Curves", QJsonArray{}}, {"UserData", QJsonArray{}}}));
        QVERIFY(!loaded.Load(path));
        QVERIFY(loaded.HasSameContent(before));
        QVERIFY(restored.SetEvents({}));
        QVERIFY(!restored.HasAnimation());
        QVERIFY(!restored.Save(path, 30, false));
        CMotionTimeline zero;
        QVERIFY(zero.SetEvents({QJsonObject{{"Time", 0.0}, {"Value", "start"}}}));
        for (double duration : {0.0, std::numeric_limits<double>::denorm_min()}) {
            QVERIFY(zero.SetTiming(30, duration));
            QVERIFY(zero.Save(path, 30, true));
            QVERIFY(loaded.Load(path));
            QCOMPARE(loaded.Timing().duration, duration);
            QCOMPARE(loaded.Events(), zero.Events());
        }
    }

    void bulkTrackFadesMatchIndividualFades()
    {
        CMotionTimeline timeline;
        const CMotionTrack model{CMotionTarget::Model, "Shared"};
        const CMotionTrack parameter{CMotionTarget::Parameter, "Shared"};
        const CMotionTrack part{CMotionTarget::PartOpacity, "Shared"};
        for (const auto& track : {model, parameter, part}) QVERIFY(timeline.AddPoint(track, 0, 1));
        QVERIFY(timeline.SetTrackFadeTimes(model, {0.25, std::nullopt}));
        QVERIFY(timeline.SetTrackFadeTimes(parameter, {0.0, 2.0}));
        const auto fades = timeline.TrackFadeTimes();
        QCOMPARE(fades.size(), 3);
        for (const auto& track : {model, parameter, part}) {
            const auto individual = timeline.TrackFadeTimes(track);
            QCOMPARE(fades.value(track).fadeIn, individual.fadeIn);
            QCOMPARE(fades.value(track).fadeOut, individual.fadeOut);
        }
        QVERIFY(timeline.RemovePoint(model, 0));
        QVERIFY(!timeline.TrackFadeTimes().contains(model));
    }

    void timingEditsKeepExactCounterpartAndAbsentDefaults()
    {
        CMotionTimeline empty;
        const auto emptyBefore = empty;
        QVERIFY(empty.SetTiming(30.0, 0.0));
        QVERIFY(empty.HasSameContent(emptyBefore));
        QTemporaryDir temp;
        const auto path = temp.filePath("timing.motion3.json");
        const QJsonObject base{{"Version", 3}, {"Curves", QJsonArray{curve("Parameter", "A", {0, 0, 0, 1, 1})}}};
        QVERIFY(writeJson(path, base));
        CMotionTimeline timeline;
        QVERIFY(timeline.Load(path));
        auto before = timeline;
        QCOMPARE(timeline.Timing().fps, 30.0);
        QCOMPARE(timeline.Timing().duration, 1.0);
        QVERIFY(timeline.SetTiming(timeline.Timing().fps, timeline.Timing().duration));
        QVERIFY(timeline.HasSameContent(before));
        QVERIFY(timeline.SetTiming(60.0, timeline.Timing().duration));
        auto expectedRoot = base;
        expectedRoot.insert("Meta", QJsonObject{{"Fps", 60.0}});
        QVERIFY(writeJson(path, expectedRoot));
        CMotionTimeline expected;
        QVERIFY(expected.Load(path));
        QVERIFY(timeline.HasSameContent(expected));
        timeline = before;
        QVERIFY(timeline.SetTiming(timeline.Timing().fps, 3.0));
        expectedRoot.insert("Meta", QJsonObject{{"Duration", 3.0}});
        QVERIFY(writeJson(path, expectedRoot));
        QVERIFY(expected.Load(path));
        QVERIFY(timeline.HasSameContent(expected));

        const double exactFps = 29.1234567890123;
        const double exactDuration = 5.1234567890123;
        expectedRoot.insert("Meta", QJsonObject{{"Fps", exactFps}, {"Duration", exactDuration}});
        QVERIFY(writeJson(path, expectedRoot));
        QVERIFY(timeline.Load(path));
        QCOMPARE(timeline.Timing().fps, exactFps);
        QCOMPARE(timeline.Timing().duration, exactDuration);
        before = timeline;
        QVERIFY(timeline.SetTiming(exactFps, exactDuration));
        QVERIFY(timeline.HasSameContent(before));
        QVERIFY(timeline.SetTiming(exactFps, 7.9876543210987));
        QJsonObject state;
        QVERIFY(timeline.SaveState(&state, false, 0));
        auto savedMeta = state.value("motion").toObject().value("Meta").toObject();
        QCOMPARE(savedMeta.value("Fps").toDouble(), exactFps);
        QCOMPARE(savedMeta.value("Duration").toDouble(), 7.9876543210987);
        CMotionTimeline restored;
        QVERIFY(restored.RestoreState(state));
        QCOMPARE(restored.Timing().fps, exactFps);
        QVERIFY(restored.Save(path, restored.Fps(), false));
        QCOMPARE(readJson(path).value("Meta").toObject().value("Fps").toDouble(), exactFps);
        timeline = before;
        QVERIFY(timeline.SetTiming(60.0, timeline.Timing().duration));
        QVERIFY(timeline.SaveState(&state, false, 0));
        savedMeta = state.value("motion").toObject().value("Meta").toObject();
        QCOMPARE(savedMeta.value("Fps").toDouble(), 60.0);
        QCOMPARE(savedMeta.value("Duration").toDouble(), exactDuration);
        QVERIFY(timeline.Save(path, 24.0f, false));
        QCOMPARE(readJson(path).value("Meta").toObject().value("Fps").toDouble(), 24.0);
    }

    void metadataEditsPreserveSdkFieldsAndTypedTrackIdentity()
    {
        QTemporaryDir temp;
        const auto path = temp.filePath("metadata.motion3.json");
        auto model = curve("Model", "Shared", {0, 1, 0, 1, 0.5});
        auto parameter = curve("Parameter", "Shared", {0, 0, 0, 1, 1});
        auto part = curve("PartOpacity", "Shared", {0, 1, 0, 1, 0});
        model.insert("FadeInTime", -2.0);
        model.insert("Extension", QJsonObject{{"keep", true}});
        parameter.insert("FadeOutTime", -1.0);
        const QJsonObject source{{"Version", 3}, {"FadeInTime", 42}, {"RootExtension", "retained"},
            {"Meta", QJsonObject{{"Duration", 2}, {"Fps", 30}, {"FadeInTime", -3.0}, {"MetaExtension", 99}}},
            {"Curves", QJsonArray{model, parameter, part}}};
        QVERIFY(writeJson(path, source));
        CMotionTimeline timeline;
        QVERIFY(timeline.Load(path));
        const CMotionTrack modelTrack{CMotionTarget::Model, "Shared"};
        const CMotionTrack parameterTrack{CMotionTarget::Parameter, "Shared"};
        const CMotionTrack partTrack{CMotionTarget::PartOpacity, "Shared"};
        QCOMPARE(timeline.FadeTimes().fadeIn.value(), -3.0);
        QVERIFY(!timeline.FadeTimes().fadeOut);
        QCOMPARE(timeline.TrackFadeTimes(modelTrack).fadeIn.value(), -2.0);
        QVERIFY(!timeline.TrackFadeTimes(parameterTrack).fadeIn);
        QCOMPARE(timeline.TrackFadeTimes(parameterTrack).fadeOut.value(), -1.0);
        auto before = timeline;
        QVERIFY(timeline.SetFadeTimes(timeline.FadeTimes()));
        QVERIFY(timeline.SetTrackFadeTimes(modelTrack, timeline.TrackFadeTimes(modelTrack)));
        QVERIFY(timeline.SetTrackFadeTimes(partTrack, {}));
        QVERIFY(timeline.HasSameContent(before));
        QVERIFY(timeline.SetFadeTimes({0.25, std::nullopt}));
        QVERIFY(timeline.SetTrackFadeTimes(parameterTrack, {0.0, 1.5}));
        QVERIFY(timeline.SetTrackFadeTimes(partTrack, {std::nullopt, 0.5}));
        QVERIFY(timeline.SetTiming(60, 4));
        QVERIFY(timeline.Save(path, timeline.Fps(), true));
        const auto saved = readJson(path);
        QCOMPARE(saved.value("FadeInTime").toDouble(), 42.0);
        QCOMPARE(saved.value("RootExtension"), source.value("RootExtension"));
        const auto meta = saved.value("Meta").toObject();
        QCOMPARE(meta.value("MetaExtension").toInt(), 99);
        QCOMPARE(meta.value("FadeInTime").toDouble(), 0.25);
        QVERIFY(!meta.contains("FadeOutTime"));
        QCOMPARE(meta.value("Duration").toDouble(), 4.0);
        const auto curves = saved.value("Curves").toArray();
        QCOMPARE(curves.at(0).toObject(), model);
        QCOMPARE(curves.at(1).toObject().value("FadeInTime").toDouble(), 0.0);
        QCOMPARE(curves.at(1).toObject().value("FadeOutTime").toDouble(), 1.5);
        QVERIFY(!curves.at(2).toObject().contains("FadeInTime"));
        QCOMPARE(curves.at(2).toObject().value("FadeOutTime").toDouble(), 0.5);
        QVERIFY(timeline.SetTrackFadeTimes(parameterTrack, {}));
        QVERIFY(timeline.SetFadeTimes({}));
        QVERIFY(timeline.Save(path, timeline.Fps(), false));
        QVERIFY(!readJson(path).value("Meta").toObject().contains("FadeInTime"));
        const auto resetCurve = readJson(path).value("Curves").toArray().at(1).toObject();
        QVERIFY(!resetCurve.contains("FadeInTime"));
        QVERIFY(!resetCurve.contains("FadeOutTime"));
    }

    void eventEditsPreserveUnicodeExtensionsAndExtendDuration()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("A", 0, 0));
        QVERIFY(timeline.AddPoint("A", 1, 1));
        QVERIFY(timeline.SetTiming(60, 2));
        const QJsonObject extension{{"speaker", "猫"}, {"position", QJsonArray{1, 2}}};
        QJsonArray events{QJsonObject{{"Time", 4.5}, {"Value", QStringLiteral("口型🌸")}, {"Extension", extension}},
                          QJsonObject{{"Time", 0}, {"Value", "start"}}};
        QVERIFY(timeline.SetEvents(events));
        QCOMPARE(timeline.Events(), events);
        QCOMPARE(timeline.Duration(), 4.5f);
        const auto before = timeline;
        QVERIFY(timeline.SetEvents(events));
        QVERIFY(timeline.HasSameContent(before));
        auto first = events.first().toObject();
        first.insert("Value", QStringLiteral("表情✨"));
        first.insert("Time", 5.0);
        events.replace(0, first);
        QVERIFY(timeline.SetEvents(events));
        QVERIFY(timeline.SetFadeTimes({0.5, -1.0}));
        QVERIFY(timeline.SetTrackFadeTimes({CMotionTarget::Parameter, "A"}, {0.0, 0.75}));
        QVERIFY(timeline.SetTiming(48, 6));
        QJsonObject state;
        QVERIFY(timeline.SaveState(&state, true, 5.5f));
        CMotionTimeline restored;
        float playhead = 0;
        QVERIFY(restored.RestoreState(state, &playhead));
        QCOMPARE(playhead, 5.5f);
        QCOMPARE(restored.Events(), events);
        QCOMPARE(restored.Duration(), 6.0f);
        QCOMPARE(restored.Fps(), 48.0f);
        QCOMPARE(restored.FadeTimes().fadeOut.value(), -1.0);
        QCOMPARE(restored.TrackFadeTimes({CMotionTarget::Parameter, "A"}).fadeOut.value(), 0.75);
        QTemporaryDir temp;
        const auto path = temp.filePath("events.motion3.json");
        QVERIFY(restored.Save(path, restored.Fps(), restored.Loop()));
        const auto saved = readJson(path);
        QCOMPARE(saved.value("UserData").toArray(), events);
        const auto meta = saved.value("Meta").toObject();
        QCOMPARE(meta.value("UserDataCount").toInt(), 2);
        QCOMPARE(meta.value("TotalUserDataSize").toInt(), QStringLiteral("表情✨start").toUtf8().size());
        CMotionTimeline reloaded;
        QVERIFY(reloaded.Load(path));
        QCOMPARE(reloaded.Events(), events);
        QCOMPARE(reloaded.Events().at(0).toObject().value("Extension").toObject(), extension);
        QVERIFY(reloaded.SetEvents({}));
        QCOMPARE(reloaded.Duration(), 6.0f);
        QVERIFY(reloaded.SetTiming(48, 1));
        QCOMPARE(reloaded.Duration(), 1.0f);
    }

    void rejectsInvalidMetadataEditsWithoutMutation()
    {
        CMotionTimeline timeline;
        const auto beforeEmpty = timeline;
        QVERIFY(timeline.SetEvents({}));
        QVERIFY(timeline.SetFadeTimes({}));
        QVERIFY(timeline.HasSameContent(beforeEmpty));
        QVERIFY(timeline.AddPoint("A", 0, 0));
        QVERIFY(timeline.AddPoint("A", 2, 1));
        QVERIFY(timeline.SetEvents({QJsonObject{{"Time", 3.0}, {"Value", "valid"}}}));
        QVERIFY(timeline.SetTiming(30, 4));
        timeline.StartRecording();
        const auto before = timeline;
        QString error;
        for (double invalid : {0.0, -1.0, std::numeric_limits<double>::denorm_min(),
                               std::numeric_limits<double>::infinity(), std::numeric_limits<double>::max()}) {
            QVERIFY(!timeline.SetTiming(invalid, 4, &error));
            QVERIFY(!error.isEmpty());
            QVERIFY(timeline.HasSameContent(before));
        }
        for (double invalid : {-1.0, 2.5, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(), std::numeric_limits<double>::max()}) {
            QVERIFY(!timeline.SetTiming(30, invalid, &error));
            QVERIFY(timeline.HasSameContent(before));
        }
        for (double invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::max()}) {
            QVERIFY(!timeline.SetFadeTimes({invalid, 0.0}, &error));
            QVERIFY(!timeline.SetTrackFadeTimes({CMotionTarget::Parameter, "A"}, {0.0, invalid}, &error));
            QVERIFY(timeline.HasSameContent(before));
        }
        QVERIFY(!timeline.SetTrackFadeTimes({CMotionTarget::PartOpacity, "A"}, {0.0, 1.0}, &error));
        QVERIFY(!timeline.SetTrackFadeTimes({static_cast<CMotionTarget>(99), "A"}, {}, &error));
        const QList<QJsonArray> invalidEvents{
            {true}, {QJsonObject{{"Time", "1"}, {"Value", "text"}}},
            {QJsonObject{{"Time", -1}, {"Value", "text"}}},
            {QJsonObject{{"Time", std::numeric_limits<double>::max()}, {"Value", "text"}}},
            {QJsonObject{{"Time", 1}, {"Value", 2}}},
            {QJsonObject{{"Time", 1}, {"Value", QString("embedded") + QChar::Null}}}
        };
        for (const auto& events : invalidEvents) {
            QVERIFY(!timeline.SetEvents(events, &error));
            QVERIFY(!error.isEmpty());
            QVERIFY(timeline.HasSameContent(before));
        }
        QJsonArray tooMany;
        const QJsonObject event{{"Time", 0}, {"Value", ""}};
        for (int i = 0; i < 100001; ++i) tooMany.append(event);
        QVERIFY(!timeline.SetEvents(tooMany, &error));
        QVERIFY(!timeline.SetEvents({QJsonObject{{"Time", 1}, {"Value", "short"},
                                                 {"Extension", QString(16 * 1024 * 1024, 'x')}}}, &error));
        QVERIFY(timeline.HasSameContent(before));
        QVERIFY(timeline.IsRecording());
    }

    void importedLateEventsAreIncludedInPlaybackAndExportDuration()
    {
        QTemporaryDir temp;
        const auto path = temp.filePath("late-event.motion3.json");
        const QJsonObject source{{"Version", 3}, {"Meta", QJsonObject{{"Duration", 1}, {"Fps", 30}}},
            {"Curves", QJsonArray{curve("Parameter", "A", {0, 0, 0, 1, 1})}},
            {"UserData", QJsonArray{QJsonObject{{"Time", 7}, {"Value", "late"}}}}};
        QVERIFY(writeJson(path, source));
        CMotionTimeline timeline;
        QVERIFY(timeline.Load(path));
        QCOMPARE(timeline.Duration(), 7.0f);
        QVERIFY(!timeline.SetTiming(60, 6));
        QVERIFY(timeline.SetTiming(60, 7));
        QVERIFY(timeline.Save(path, timeline.Fps(), false));
        QCOMPARE(readJson(path).value("Meta").toObject().value("Duration").toDouble(), 7.0);
    }

    void typedTracksRemainIndependentThroughEditingAndStorage()
    {
        CMotionTimeline timeline;
        const CMotionTrack model{CMotionTarget::Model, "Opacity"};
        const CMotionTrack parameter{CMotionTarget::Parameter, "Opacity"};
        const CMotionTrack part{CMotionTarget::PartOpacity, "Opacity"};
        for (const auto& track : {model, parameter, part}) {
            QVERIFY(timeline.AddPoint(track, 0, 0));
            QVERIFY(timeline.AddPoint(track, 2, 1));
        }
        QVERIFY(timeline.InsertPoint(model, 1, 0.25f));
        QVERIFY(timeline.UpdatePoint(parameter, 1, 2, 4));
        QVERIFY(timeline.SetSegmentType(part, 1, 2));
        const auto frame = timeline.EvaluateFrame(1);
        QCOMPARE(frame.model.value("Opacity"), 0.25f);
        QCOMPARE(frame.parameters.value("Opacity"), 2.0f);
        QCOMPARE(frame.parts.value("Opacity"), 0.0f);
        QCOMPARE(timeline.TotalPointCount(), 7);
        QCOMPARE(timeline.PointCount(), 2);
        QVERIFY(!timeline.InsertPoint(model, 1, 9));
        QVERIFY(!timeline.InsertPoint({static_cast<CMotionTarget>(99), "bad"}, 1, 0));
        QVERIFY(!timeline.InsertPoint(part, -1, 0));
        QVERIFY(!timeline.InsertPoint(part, 1, std::numeric_limits<float>::infinity()));
        QVERIFY(timeline.RemovePoint(parameter, 0));
        QCOMPARE(timeline.Tracks().value(model).size(), 3);
        QCOMPARE(timeline.Tracks().value(part).size(), 2);
        QJsonObject state;
        QVERIFY(timeline.SaveState(&state, true, 1));
        CMotionTimeline restored;
        float playhead = 0;
        QVERIFY(restored.RestoreState(state, &playhead));
        QCOMPARE(playhead, 1.0f);
        QCOMPARE(restored.Tracks(), timeline.Tracks());
        QCOMPARE(restored.SegmentType(part, 1), 2);
        QTemporaryDir temp;
        const auto path = temp.filePath("typed.motion3.json");
        QVERIFY(restored.Save(path, 60, true));
        CMotionTimeline exported;
        QVERIFY(exported.Load(path));
        QCOMPARE(exported.EvaluateFrame(1).model, frame.model);
        QCOMPARE(exported.EvaluateFrame(1).parts, frame.parts);
        QCOMPARE(exported.EvaluateFrame(1).parameters.value("Opacity"), 4.0f);
        const auto curves = readJson(path).value("Curves").toArray();
        QCOMPARE(curves.at(0).toObject().value("Target").toString(), QString("Model"));
        QCOMPARE(curves.at(1).toObject().value("Target").toString(), QString("Parameter"));
        QCOMPARE(curves.at(2).toObject().value("Target").toString(), QString("PartOpacity"));
    }

    void nonParameterBezierInsertionPreservesValidSegments()
    {
        QTemporaryDir temp;
        const auto path = temp.filePath("typed.motion3.json");
        for (const auto target : {CMotionTarget::Model, CMotionTarget::PartOpacity}) {
            const CMotionTrack track{target, "Opacity"};
            auto original = curve(track.TargetName(), track.id, {0, 0, 1, 0.5, 0, 1.5, 1, 2, 1});
            original.insert("FadeInTime", 0.5);
            original.insert("Extension", true);
            QVERIFY(writeJson(path, {{"Version", 3}, {"Curves", QJsonArray{original}}}));
            CMotionTimeline timeline;
            QVERIFY(timeline.Load(path));
            QVERIFY(timeline.InsertPoint(track, 1, 0.25f));
            QCOMPARE(timeline.SegmentType(track, 2), 1);
            QVERIFY(timeline.Save(path, 30, false));
            CMotionTimeline restored;
            QVERIFY(restored.Load(path));
            for (float time : {0.0f, 0.5f, 1.0f, 1.25f, 1.75f, 2.0f})
                QCOMPARE(restored.Evaluate(track, time), timeline.Evaluate(track, time));
            auto saved = readJson(path).value("Curves").toArray().first().toObject();
            QCOMPARE(saved.value("FadeInTime").toDouble(), 0.5);
            QVERIFY(saved.value("Extension").toBool());
            QVERIFY(timeline.RemovePoint(track, 1));
            QVERIFY(timeline.InsertPoint(track, 0.5f, 0.2f));
            QVERIFY(timeline.Save(path, 30, false));
            QVERIFY(restored.Load(path));
            QCOMPARE(restored.Evaluate(track, 1.5f), timeline.Evaluate(track, 1.5f));
        }
    }

    void modelEffectsComposeWithoutAccumulation()
    {
        CMotionPreviewFrame frame;
        frame.parameters = {{"EyeL", 0.8f}, {"Mouth", 0.2f}, {"Both", 0.4f}, {"Body", 5}};
        frame.model = {{"EyeBlink", 0.5f}, {"LipSync", 0.3f}, {"Opacity", 0.25f}};
        frame.parts = {{"PartA", 0.6f}};
        const QSet<QString> eyes{"EyeL", "EyeR", "Both", "Shared"};
        const QSet<QString> lips{"Mouth", "Mouth2", "Both", "Shared"};
        const auto values = frame.ResolveParameters(eyes, lips);
        QCOMPARE(values.value("EyeL"), 0.4f);
        QCOMPARE(values.value("EyeR"), 0.5f);
        QCOMPARE(values.value("Mouth"), 0.5f);
        QCOMPARE(values.value("Mouth2"), 0.3f);
        QCOMPARE(values.value("Both"), 0.5f);
        QCOMPARE(values.value("Shared"), 0.3f);
        QCOMPARE(values.value("Body"), 5.0f);
        QVERIFY(!values.contains("Opacity"));
        QVERIFY(!values.contains("PartA"));
        QCOMPARE(frame.ResolveParameters(eyes, lips), values);
        QCOMPARE(frame.parameters.value("EyeL"), 0.8f);
        frame.parameters["Both"] = std::numeric_limits<float>::max();
        frame.model["EyeBlink"] = std::numeric_limits<float>::max();
        QVERIFY(std::isfinite(frame.ResolveParameters(eyes, lips).value("Both")));
        QVERIFY(frame.IsFinite());
        frame.parts["PartA"] = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(!frame.IsFinite());
        frame.parts.clear();
        frame.model[QString("Bad") + QChar::Null] = 1;
        QVERIFY(!frame.IsFinite());
    }
    void preservesMixedMotionData()
    {
        QTemporaryDir temp;
        auto parameter = curve("Parameter", "ParamA", {0, 0, 0, 1, 10});
        parameter.insert("FadeInTime", 0.25);
        parameter.insert("FadeOutTime", 0.75);
        parameter.insert("Custom", "keep");
        const auto model = curve("Model", "Opacity", {0, 1, 0, 2, 0.5});
        const auto part = curve("PartOpacity", "PartA", {0, 0, 2, 3, 1});
        const QJsonArray events{QJsonObject{{"Time", 0.5}, {"Value", QStringLiteral("口型🌸")}}};
        const QJsonObject root{{"Version", 3}, {"FadeInTime", 0.4}, {"FadeOutTime", 0.6}, {"Custom", 42},
            {"Meta", QJsonObject{{"Duration", 5}, {"Fps", 60}, {"Loop", true}, {"AreBeziersRestricted", false},
                                 {"CurveCount", 900}, {"TotalSegmentCount", 900}, {"TotalPointCount", 900}}},
            {"Curves", QJsonArray{model, parameter, part}}, {"UserData", events}};
        const QString input = temp.filePath("input.motion3.json");
        const QString output = temp.filePath("output.motion3.json");
        QVERIFY(writeJson(input, root));
        CMotionTimeline timeline;
        QString error;
        QVERIFY2(timeline.Load(input, &error), qPrintable(error));
        QCOMPARE(timeline.Fps(), 60.0f);
        QVERIFY(timeline.Loop());
        QCOMPARE(timeline.Duration(), 5.0f);
        QVERIFY(timeline.UpdatePoint("ParamA", 1, 1.5f, 20.0f));
        QVERIFY2(timeline.Save(output, timeline.Fps(), timeline.Loop(), &error), qPrintable(error));
        const auto saved = readJson(output);
        const auto curves = saved.value("Curves").toArray();
        QCOMPARE(curves.size(), 3);
        QCOMPARE(curves.at(0), QJsonValue(model));
        QCOMPARE(curves.at(2), QJsonValue(part));
        QCOMPARE(curves.at(1).toObject().value("FadeInTime").toDouble(), 0.25);
        QCOMPARE(curves.at(1).toObject().value("FadeOutTime").toDouble(), 0.75);
        QCOMPARE(curves.at(1).toObject().value("Segments").toArray(), (QJsonArray{0, 0, 0, 1.5, 20}));
        QCOMPARE(saved.value("UserData").toArray(), events);
        QCOMPARE(saved.value("FadeInTime"), root.value("FadeInTime"));
        QCOMPARE(saved.value("FadeOutTime"), root.value("FadeOutTime"));
        QCOMPARE(saved.value("Custom"), root.value("Custom"));
        const auto meta = saved.value("Meta").toObject();
        QCOMPARE(meta.value("Duration").toDouble(), 5.0);
        QCOMPARE(meta.value("TotalPointCount").toInt(), 6);
        QCOMPARE(meta.value("TotalSegmentCount").toInt(), 3);
        QCOMPARE(meta.value("CurveCount").toInt(), 3);
        QCOMPARE(meta.value("UserDataCount").toInt(), 1);
        QCOMPARE(meta.value("TotalUserDataSize").toInt(), QStringLiteral("口型🌸").toUtf8().size());
        QVERIFY(timeline.RemovePoint("ParamA", 1));
        QVERIFY(timeline.RemovePoint("ParamA", 0));
        QVERIFY(timeline.Save(output, timeline.Fps(), false, &error));
        CMotionTimeline nonParameter;
        QVERIFY(nonParameter.Load(output, &error));
        QCOMPARE(nonParameter.PointCount(), 0);
        QCOMPARE(readJson(output).value("Curves").toArray(), (QJsonArray{model, part}));
        timeline.Clear();
        QVERIFY(timeline.AddPoint("New", 0, 1));
        QVERIFY(timeline.Save(output, timeline.Fps(), false, &error));
        QCOMPARE(readJson(output).value("Curves").toArray().size(), 1);
        QVERIFY(!readJson(output).contains("UserData"));
        QCOMPARE(timeline.Fps(), 30.0f);
    }

    void matchesSdkBezierModes_data()
    {
        QTest::addColumn<bool>("restricted");
        QTest::addColumn<double>("expected");
        QTest::newRow("unrestricted") << false << 0.5;
        QTest::newRow("restricted") << true << 0.04296875;
    }

    void matchesSdkBezierModes()
    {
        QFETCH(bool, restricted);
        QFETCH(double, expected);
        QTemporaryDir temp;
        const QString path = temp.filePath("curve.motion3.json");
        const QJsonArray points{0, 0, 1, 0, 0, 0, 1, 1, 1};
        QVERIFY(writeJson(path, {{"Version", 3}, {"Meta", QJsonObject{{"AreBeziersRestricted", restricted}}},
                                 {"Curves", QJsonArray{curve("Parameter", "A", points)}}}));
        CMotionTimeline timeline;
        QString error;
        QVERIFY2(timeline.Load(path, &error), qPrintable(error));
        QVERIFY(qAbs(timeline.Evaluate("A", 0.125f) - expected) < 0.0001);
        QVERIFY(timeline.Save(path, 30, false, &error));
        QCOMPARE(readJson(path).value("Meta").toObject().value("AreBeziersRestricted").toBool(), restricted);
        QCOMPARE(readJson(path).value("Curves").toArray().first().toObject().value("Segments").toArray(), points);
        QVERIFY(timeline.UpdatePoint("A", 1, 0.5f, 1.0f));
        QVERIFY(qAbs(timeline.Evaluate("A", 0.0625f) - expected) < 0.0001);
        QVERIFY(timeline.Save(path, 30, false, &error));
        CMotionTimeline reloaded;
        QVERIFY(reloaded.Load(path, &error));
        for (float time : {0.0f, 0.0625f, 0.125f, 0.25f, 0.499f, 0.5f})
            QVERIFY(qAbs(reloaded.Evaluate("A", time) - timeline.Evaluate("A", time)) < 0.0001f);
    }

    void acceptsCrossedBezierHandles()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("curve.motion3.json");
        const QJsonArray points{0, 0, 1, 0.8, 0, 0.2, 1, 1, 1};
        QVERIFY(writeJson(path, {{"Version", 3}, {"Curves", QJsonArray{curve("Parameter", "A", points)}}}));
        CMotionTimeline timeline;
        QString error;
        QVERIFY2(timeline.Load(path, &error), qPrintable(error));
        QVERIFY(qAbs(timeline.Evaluate("A", 0.38125f) - 0.15625f) < 0.0001f);
        QVERIFY(timeline.Save(path, 30, false, &error));
        QCOMPARE(readJson(path).value("Curves").toArray().first().toObject().value("Segments").toArray(), points);
    }

    void handlesSinglePointAndStepBoundaries()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("curve.motion3.json");
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("A", 5.0f, 1.0f));
        QVERIFY(timeline.Save(path, 30, false));
        CMotionTimeline reloaded;
        QVERIFY(reloaded.Load(path));
        QVERIFY(reloaded.Curves().value("A").last().x() > 5.0);
        timeline.Clear();
        QVERIFY(timeline.AddPoint("Step", 0, 1));
        QVERIFY(timeline.AddPoint("Step", 1, 9));
        QVERIFY(timeline.AddPoint("Step", 2, 4));
        QVERIFY(timeline.SetSegmentType("Step", 1, 2));
        QVERIFY(timeline.SetSegmentType("Step", 2, 2));
        QCOMPARE(timeline.Evaluate("Step", 0.999f), 1.0f);
        QCOMPARE(timeline.Evaluate("Step", 1), 9.0f);
        QCOMPARE(timeline.Evaluate("Step", 2), 4.0f);
        QVERIFY(timeline.SetSegmentType("Step", 1, 3));
        QCOMPARE(timeline.Evaluate("Step", 0), 9.0f);
        timeline.Clear();
        const float maximum = std::numeric_limits<float>::max();
        QVERIFY(timeline.AddPoint("A", maximum, 1));
        QVERIFY(!timeline.AddPoint("A", maximum, 2));
        QVERIFY(!timeline.Save(path, 30, false));
        QVERIFY(reloaded.Load(path));
        QVERIFY(std::isfinite(reloaded.Duration()));
    }

    void rejectsInvalidCurvesWithoutChangingDraft_data()
    {
        QTest::addColumn<QJsonArray>("segments");
        QTest::newRow("first-time-type") << QJsonArray{"bad", 0, 0, 1, 1};
        QTest::newRow("end-time-type") << QJsonArray{0, 0, 0, "bad", 1};
        QTest::newRow("start-value-type") << QJsonArray{0, "bad", 0, 1, 1};
        QTest::newRow("huge-segment") << QJsonArray{0, 0, 1e100, 1, 1};
        QTest::newRow("huge-value") << QJsonArray{0, 0, 0, 1, 1e100};
        QTest::newRow("fractional-segment") << QJsonArray{0, 0, 0.5, 1, 1};
        QTest::newRow("truncated-bezier") << QJsonArray{0, 0, 1, 0.3, 0};
        QTest::newRow("float-time-collision") << QJsonArray{1, 0, 0, 1.000000001, 1};
        QTest::newRow("negative-control") << QJsonArray{0, 0, 1, -1, 0, 0.3, 0, 1, 1};
    }

    void rejectsInvalidCurvesWithoutChangingDraft()
    {
        QFETCH(QJsonArray, segments);
        QTemporaryDir temp;
        const QString path = temp.filePath("invalid.motion3.json");
        QVERIFY(writeJson(path, {{"Version", 3}, {"Curves", QJsonArray{curve("Parameter", "A", segments)}}}));
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("Original", 0, 42));
        timeline.StartRecording();
        QString error;
        QVERIFY(!timeline.Load(path, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(timeline.PointCount(), 1);
        QCOMPARE(timeline.Evaluate("Original", 0), 42.0f);
        QVERIFY(timeline.IsRecording());
    }

    void preservesOfficialMotions()
    {
        QTemporaryDir temp;
        QDirIterator files(QString(TEST_RESOURCES), {"*.motion3.json"}, QDir::Files, QDirIterator::Subdirectories);
        int tested = 0;
        while (files.hasNext()) {
            const QString path = files.next();
            const QJsonObject original = readJson(path);
            CMotionTimeline timeline;
            QString error;
            QVERIFY2(timeline.Load(path, &error), qPrintable(path + ": " + error));
            const QString output = temp.filePath("roundtrip.motion3.json");
            QVERIFY2(timeline.Save(output, timeline.Fps(), timeline.Loop(), &error), qPrintable(error));
            const auto saved = readJson(output);
            const auto savedCurves = saved.value("Curves").toArray();
            QCOMPARE(savedCurves.size(), original.value("Curves").toArray().size());
            for (const auto& sourceCurve : original.value("Curves").toArray()) {
                const auto source = sourceCurve.toObject();
                bool matched = false;
                for (const auto& savedCurve : savedCurves) {
                    const auto target = savedCurve.toObject();
                    if (target.value("Id") == source.value("Id") && target.value("Target") == source.value("Target")) {
                        QCOMPARE(target, source);
                        matched = true;
                        break;
                    }
                }
                QVERIFY(matched);
            }
            CMotionTimeline reloaded;
            QVERIFY(reloaded.Load(output, &error));
            for (const auto& track : timeline.Tracks().keys()) {
                for (int sample = 0; sample <= 10; ++sample) {
                    const float time = timeline.Duration() * sample / 10.0f;
                    QVERIFY(qAbs(timeline.Evaluate(track, time) - reloaded.Evaluate(track, time)) < 0.0001f);
                }
            }
            ++tested;
        }
        QVERIFY(tested > 0);
    }

    void rejectsInvalidMetadataAndEvents()
    {
        const QJsonObject valid{{"Version", 3}, {"Curves", QJsonArray{curve("Parameter", "A", {0, 0, 0, 1, 1})}}};
        const QVector<QPair<QString, QJsonValue>> invalidFields{
            {"Version", 3.5}, {"Meta", QJsonArray{}},
            {"Meta", QJsonObject{{"Fps", 0}}}, {"Meta", QJsonObject{{"Duration", "bad"}}},
            {"Meta", QJsonObject{{"AreBeziersRestricted", 1}}}, {"Meta", QJsonObject{{"Loop", "true"}}},
            {"FadeInTime", "bad"}, {"FadeOutTime", 1e100}, {"UserData", QJsonObject{}},
            {"UserData", QJsonArray{QJsonObject{{"Time", "bad"}, {"Value", "text"}}}},
            {"UserData", QJsonArray{QJsonObject{{"Time", 0}, {"Value", 3}}}},
            {"Curves", QJsonArray{curve("InvalidTarget", "A", {0, 0, 0, 1, 1})}},
            {"Curves", QJsonArray{curve("Parameter", "A", {0, 0, 0, 1, 1}), curve("Parameter", "A", {0, 0, 0, 1, 1})}}
        };
        QTemporaryDir temp;
        const QString path = temp.filePath("input.json");
        CMotionTimeline timeline;
        QVERIFY(writeJson(path, valid));
        QVERIFY(timeline.Load(path));
        for (const auto& field : invalidFields) {
            auto root = valid;
            root.insert(field.first, field.second);
            QVERIFY(writeJson(path, root));
            QString error;
            QVERIFY2(!timeline.Load(path, &error), qPrintable(field.first));
            QVERIFY(!error.isEmpty());
            QCOMPARE(timeline.PointCount(), 2);
        }
    }

    void retargetsBezierAfterDeletingOrReorderingKeys()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("curve.json");
        const QJsonArray points{0, 0, 1, 0.2, 0, 0.6, 0.5, 1, 1, 1, 1.2, 1.5, 1.5, 2, 2, 2};
        QVERIFY(writeJson(path, {{"Version", 3}, {"Curves", QJsonArray{curve("Parameter", "A", points)}}}));
        for (bool remove : {true, false}) {
            CMotionTimeline timeline;
            QString error;
            QVERIFY(timeline.Load(path, &error));
            if (remove) QVERIFY(timeline.RemovePoint("A", 1));
            else QVERIFY(timeline.UpdatePoint("A", 2, 0.5, 2));
            const QString output = temp.filePath("saved.json");
            QVERIFY(timeline.Save(output, 30, false, &error));
            CMotionTimeline loaded;
            QVERIFY2(loaded.Load(output, &error), qPrintable(error));
            for (float time : {0.0f, 0.25f, 0.75f, 1.0f, 1.5f, 2.0f})
                QVERIFY(qAbs(loaded.Evaluate("A", time) - timeline.Evaluate("A", time)) < 0.0001f);
        }
    }

    void recordingClockPauseExcludesExactElapsedTime()
    {
        qint64 now = 5000;
        CMotionTimeline timeline([&] { return now; });
        QVERIFY(timeline.AddPoint("A", 5, 0));
        timeline.StartRecording();
        now = 6000;
        timeline.Record("A", 1);
        QCOMPARE(timeline.Curves()["A"].last(), QPointF(6, 1));
        now = 6500;
        const auto content = timeline;
        QVERIFY(timeline.PauseRecordingClock());
        QVERIFY(timeline.IsRecording());
        QVERIFY(timeline.IsRecordingClockPaused());
        QVERIFY(!timeline.PauseRecordingClock());
        now += 86400000;
        timeline.Record("A", 2);
        QVERIFY(timeline.HasSameContent(content));
        QVERIFY(timeline.ResumeRecordingClock());
        QVERIFY(!timeline.ResumeRecordingClock());
        now += 250;
        timeline.Record("A", 3);
        QCOMPARE(timeline.Curves()["A"].last(), QPointF(6.75, 3));
        QVERIFY(timeline.PauseRecordingClock());
        now += 172800000;
        QVERIFY(timeline.ResumeRecordingClock());
        now += 250;
        timeline.Record("A", 4);
        QCOMPARE(timeline.Curves()["A"].last(), QPointF(7, 4));
    }

    void pausedRecordingCopiesHaveIndependentClocksAndResetState()
    {
        qint64 now = 1000;
        CMotionTimeline original([&] { return now; });
        original.StartRecording();
        now = 1500;
        original.Record("A", 1);
        now = 1750;
        QVERIFY(original.PauseRecordingClock());
        CMotionTimeline copy = original;
        QVERIFY(copy.IsRecordingClockPaused());
        now = 100000;
        QVERIFY(original.ResumeRecordingClock());
        now = 100250;
        original.Record("A", 2);
        QCOMPARE(original.Curves()["A"].last(), QPointF(1, 2));
        copy.Record("A", 9);
        QCOMPARE(copy.TotalPointCount(), 1);
        now = 100500;
        QVERIFY(copy.ResumeRecordingClock());
        now = 100750;
        copy.Record("A", 3);
        original.Record("A", 4);
        QCOMPARE(copy.Curves()["A"].last(), QPointF(1, 3));
        QCOMPARE(original.Curves()["A"].last(), QPointF(1.5, 4));
        QVERIFY(copy.PauseRecordingClock());
        QJsonObject saved;
        QVERIFY(copy.SaveState(&saved, false, 0.5));
        CMotionTimeline restored;
        QVERIFY(restored.RestoreState(saved));
        QVERIFY(!restored.IsRecording());
        QVERIFY(!restored.IsRecordingClockPaused());
        QVERIFY(copy.IsRecordingClockPaused());
        copy.StopRecording();
        QVERIFY(!copy.IsRecordingClockPaused());
        QVERIFY(!copy.ResumeRecordingClock());
        QVERIFY(copy.RestoreState(saved));
        QVERIFY(!copy.IsRecording() && !copy.IsRecordingClockPaused());
        now = 200000;
        copy.StartRecording();
        now += 1000;
        copy.Record("A", 5);
        QCOMPARE(copy.Curves()["A"].last(), QPointF(2, 5));
        QVERIFY(copy.PauseRecordingClock());
        QTemporaryDir directory;
        const auto path = directory.filePath("recording.json");
        QVERIFY(copy.Save(path, 30, false));
        QVERIFY(copy.Load(path));
        QVERIFY(!copy.IsRecording() && !copy.IsRecordingClockPaused());
        copy.StartRecording();
        QVERIFY(copy.PauseRecordingClock());
        copy.Clear();
        QVERIFY(!copy.IsRecording() && !copy.IsRecordingClockPaused());
        QCOMPARE(copy.TotalPointCount(), 0);
        copy.StartRecording();
        now += 500;
        copy.Record("B", 6);
        QCOMPARE(copy.Curves()["B"].last(), QPointF(0.5, 6));
    }

    void recordingClockFailureAndRollbackKeepPauseStateAtomic()
    {
        qint64 now = 10;
        bool throws = false;
        CMotionTimeline timeline([&]() -> qint64 {
            if (throws) throw std::runtime_error("clock failure");
            return now;
        });
        throws = true;
        timeline.StartRecording();
        QVERIFY(!timeline.IsRecording());
        throws = false;
        timeline.StartRecording();
        now = 1000;
        timeline.Record("A", 1);
        const auto content = timeline;
        now = 500;
        QVERIFY(!timeline.PauseRecordingClock());
        QVERIFY(!timeline.IsRecordingClockPaused());
        timeline.Record("A", 2);
        QVERIFY(timeline.HasSameContent(content));
        now = 1100;
        QVERIFY(timeline.PauseRecordingClock());
        throws = true;
        QVERIFY(!timeline.ResumeRecordingClock());
        timeline.StartRecording();
        QVERIFY(timeline.IsRecording() && timeline.IsRecordingClockPaused());
        QVERIFY(timeline.HasSameContent(content));
        throws = false;
        now = 1000;
        QVERIFY(!timeline.ResumeRecordingClock());
        QVERIFY(timeline.IsRecordingClockPaused());
        now = 1110;
        QVERIFY(timeline.ResumeRecordingClock());
        now = 1120;
        timeline.Record("A", 3);
        QCOMPARE(float(timeline.Curves()["A"].last().x()), 1.1f);
    }

    void recordingClockDifferenceCannotOverflow()
    {
        qint64 now = std::numeric_limits<qint64>::min();
        CMotionTimeline timeline([&] { return now; });
        timeline.StartRecording();
        now = 0;
        timeline.Record("A", 1);
        QCOMPARE(timeline.TotalPointCount(), 0);
        QVERIFY(!timeline.PauseRecordingClock());
        now = -1;
        QVERIFY(timeline.PauseRecordingClock());
        now = 0;
        QVERIFY(!timeline.ResumeRecordingClock());
        QVERIFY(timeline.IsRecordingClockPaused());
        timeline.StopRecording();
        now = std::numeric_limits<qint64>::max();
        timeline.StartRecording();
        now = std::numeric_limits<qint64>::min();
        QVERIFY(!timeline.PauseRecordingClock());
        now = std::numeric_limits<qint64>::max();
        QVERIFY(timeline.PauseRecordingClock());
        QVERIFY(timeline.ResumeRecordingClock());
        timeline.Record("B", 2);
        QCOMPARE(timeline.Curves()["B"].last(), QPointF(0, 2));
    }

    void recordingResumesAfterExistingClip()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("A", 5, 0));
        timeline.StartRecording();
        QTest::qWait(20);
        timeline.Record("A", 1);
        const auto points = timeline.Curves().value("A");
        QCOMPARE(points.size(), 2);
        QVERIFY(points.last().x() >= 5.01);
        timeline.Clear();
        QVERIFY(!timeline.IsRecording());
        timeline.Record("A", 2);
        QCOMPARE(timeline.PointCount(), 0);
    }

    void snapshotPreservesCurvesAndStopsRecordingOnRestore()
    {
        const QString path = QString(TEST_RESOURCES) + "/Mark/motions/mark_m06.motion3.json";
        CMotionTimeline original;
        QVERIFY(original.Load(path));
        const QString id = original.Curves().firstKey();
        QVERIFY(original.UpdatePoint(id, 0, 0.0f, 0.25f));
        original.StartRecording();
        QJsonObject state;
        QVERIFY(original.SaveState(&state, true, original.Duration() * 0.5f));
        QVERIFY(original.IsRecording());
        CMotionTimeline restored;
        float time = 0;
        QVERIFY(restored.RestoreState(state, &time));
        QVERIFY(!restored.IsRecording());
        QCOMPARE(restored.Curves(), original.Curves());
        QJsonObject repeated;
        QVERIFY(restored.SaveState(&repeated, restored.Loop(), time));
        QCOMPARE(repeated, state);
        for (const auto& parameter : original.Curves().keys())
            QVERIFY(qAbs(original.Evaluate(parameter, time) - restored.Evaluate(parameter, time)) < 0.0001f);
    }

    void writesCubismMotionFormat()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamAngleX"), 0.0f, -1.0f));
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamAngleX"), 0.5f, 1.0f));
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamAngleY"), 0.0f, 0.25f));
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("test.motion3.json"));
        QString error;
        QVERIFY2(timeline.Save(path, 30.0f, true, &error), qPrintable(error));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(root.value(QStringLiteral("Version")).toInt(), 3);
        const QJsonObject meta = root.value(QStringLiteral("Meta")).toObject();
        QCOMPARE(meta.value(QStringLiteral("CurveCount")).toInt(), 2);
        QCOMPARE(meta.value(QStringLiteral("TotalSegmentCount")).toInt(), 2);
        QCOMPARE(meta.value(QStringLiteral("TotalPointCount")).toInt(), 4);
        QCOMPARE(root.value(QStringLiteral("Curves")).toArray().size(), 2);
    }

    void singlePointGetsValidSegment()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamA"), 0.0f, 0.5f));
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString error;
        QVERIFY(timeline.Save(temp.filePath(QStringLiteral("single.motion3.json")), 30.0f, false, &error));
        QFile file(temp.filePath(QStringLiteral("single.motion3.json")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonArray segments = QJsonDocument::fromJson(file.readAll()).object()
            .value(QStringLiteral("Curves")).toArray().first().toObject().value(QStringLiteral("Segments")).toArray();
        QCOMPARE(segments.size(), 5);
        QCOMPARE(segments.at(2).toInt(), 0);
    }

    void separatesSameTimestampValues()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamA"), 1.0f, 0.0f));
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamA"), 1.0f, 1.0f));
        const auto points = timeline.Curves().value(QStringLiteral("ParamA"));
        QCOMPARE(points.size(), 2);
        QVERIFY(points.at(1).x() > points.at(0).x());
    }

    void separatesImportedFractionalTimestamps()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("curve.json");
        QVERIFY(writeJson(path, {{"Version", 3}, {"Curves", QJsonArray{curve("Parameter", "A", {0, 0, 0, 0.1, 1})}}}));
        CMotionTimeline timeline;
        QVERIFY(timeline.Load(path));
        QVERIFY(!timeline.AddPoint("A", 0.1f, 1));
        QVERIFY(timeline.AddPoint("A", 0.1f, 2));
        const auto points = timeline.Curves().value("A");
        QVERIFY(static_cast<float>(points.last().x()) > static_cast<float>(points.at(1).x()));
        QVERIFY(timeline.Save(path, 30, false));
        CMotionTimeline loaded;
        QVERIFY(loaded.Load(path));
        QCOMPARE(loaded.PointCount(), 3);
    }

    void editsAndRemovesPoints()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamA"), 0.0f, 0.0f));
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamA"), 1.0f, 1.0f));
        QVERIFY(timeline.UpdatePoint(QStringLiteral("ParamA"), 1, 0.25f, 0.5f));
        QCOMPARE(timeline.Curves().value(QStringLiteral("ParamA")).at(1).x(), 0.25);
        QVERIFY(!timeline.UpdatePoint(QStringLiteral("ParamA"), 0, 0.25f, 0.2f));
        QVERIFY(timeline.RemovePoint(QStringLiteral("ParamA"), 1));
        QCOMPARE(timeline.PointCount(), 1);
    }

    void writesBezierSegmentMetadata()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamA"), 0.0f, 0.0f));
        QVERIFY(timeline.AddPoint(QStringLiteral("ParamA"), 1.0f, 1.0f));
        QVERIFY(timeline.SetSegmentType(QStringLiteral("ParamA"), 1, 1));
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString error;
        QVERIFY(timeline.Save(temp.filePath(QStringLiteral("bezier.motion3.json")), 30.0f, false, &error));
        QFile file(temp.filePath(QStringLiteral("bezier.motion3.json")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        const QJsonArray segments = root.value(QStringLiteral("Curves")).toArray().first().toObject()
            .value(QStringLiteral("Segments")).toArray();
        QCOMPARE(segments.at(2).toInt(), 1);
        QCOMPARE(root.value(QStringLiteral("Meta")).toObject().value(QStringLiteral("TotalPointCount")).toInt(), 4);
    }

    void evaluatesInterpolation()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint(QStringLiteral("Linear"), 0.0f, 0.0f));
        QVERIFY(timeline.AddPoint(QStringLiteral("Linear"), 1.0f, 10.0f));
        QCOMPARE(timeline.Evaluate(QStringLiteral("Linear"), 0.5f), 5.0f);
        QVERIFY(timeline.AddPoint(QStringLiteral("Step"), 0.0f, 1.0f));
        QVERIFY(timeline.AddPoint(QStringLiteral("Step"), 1.0f, 9.0f));
        QVERIFY(timeline.SetSegmentType(QStringLiteral("Step"), 1, 2));
        QCOMPARE(timeline.Evaluate(QStringLiteral("Step"), 0.5f), 1.0f);
        QCOMPARE(timeline.EvaluateAll(0.5f).size(), 2);
    }

    void loadsExportedMotion()
    {
        CMotionTimeline source;
        QVERIFY(source.AddPoint(QStringLiteral("ParamA"), 0.0f, 0.0f));
        QVERIFY(source.AddPoint(QStringLiteral("ParamA"), 1.0f, 1.0f));
        QVERIFY(source.SetSegmentType(QStringLiteral("ParamA"), 1, 2));
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("roundtrip.motion3.json"));
        QString error;
        QVERIFY(source.Save(path, 30.0f, true, &error));
        CMotionTimeline loaded;
        QVERIFY2(loaded.Load(path, &error), qPrintable(error));
        QCOMPARE(loaded.Curves().keys(), QStringList{QStringLiteral("ParamA")});
        QCOMPARE(loaded.SegmentType(QStringLiteral("ParamA"), 1), 2);
        QCOMPARE(loaded.Evaluate(QStringLiteral("ParamA"), 0.5f), 0.0f);
        QVERIFY(!loaded.Load(temp.filePath(QStringLiteral("missing.motion3.json")), &error));
        QCOMPARE(loaded.PointCount(), 2);
    }

    void preservesCustomBezierControls()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QJsonArray segments{0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0};
        QJsonObject curve;
        curve.insert(QStringLiteral("Target"), QStringLiteral("Parameter"));
        curve.insert(QStringLiteral("Id"), QStringLiteral("ParamA"));
        curve.insert(QStringLiteral("Segments"), segments);
        QJsonArray curves;
        curves.append(curve);
        QJsonObject root;
        root.insert(QStringLiteral("Version"), 3);
        root.insert(QStringLiteral("Curves"), curves);
        const QString sourcePath = temp.filePath(QStringLiteral("custom.motion3.json"));
        QFile sourceFile(sourcePath);
        QVERIFY(sourceFile.open(QIODevice::WriteOnly));
        QVERIFY(sourceFile.write(QJsonDocument(root).toJson()) > 0);
        sourceFile.close();

        CMotionTimeline loaded;
        QString error;
        QVERIFY2(loaded.Load(sourcePath, &error), qPrintable(error));
        QVERIFY(qAbs(loaded.Evaluate(QStringLiteral("ParamA"), 0.125f) - 0.5f) < 0.001f);

        const QString outputPath = temp.filePath(QStringLiteral("custom-out.motion3.json"));
        QVERIFY2(loaded.Save(outputPath, 30.0f, false, &error), qPrintable(error));
        QFile outputFile(outputPath);
        QVERIFY(outputFile.open(QIODevice::ReadOnly));
        const QJsonArray outputSegments = QJsonDocument::fromJson(outputFile.readAll()).object()
            .value(QStringLiteral("Curves")).toArray().first().toObject().value(QStringLiteral("Segments")).toArray();
        QCOMPARE(outputSegments, segments);
    }
};

QTEST_GUILESS_MAIN(CMotionTimelineTests)
#include "cmotiontimeline_tests.moc"
