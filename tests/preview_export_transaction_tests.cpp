#include "ui/cpreviewexporttransaction.h"
#include <QtTest>
#include <QEventLoop>
#include <QTimer>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
const QStringList allStages{QStringLiteral("pause"), QStringLiteral("begin"), QStringLiteral("export"),
                            QStringLiteral("end"), QStringLiteral("restore"), QStringLiteral("resume")};

QString stageLabel(const QString& stage)
{
    const QMap<QString, QString> labels{{QStringLiteral("pause"), QStringLiteral("暂停预览")},
        {QStringLiteral("begin"), QStringLiteral("进入临时模型")},
        {QStringLiteral("export"), QStringLiteral("导出文件")},
        {QStringLiteral("end"), QStringLiteral("恢复原模型")},
        {QStringLiteral("restore"), QStringLiteral("恢复预览状态")},
        {QStringLiteral("resume"), QStringLiteral("恢复计时器与输入")}};
    return labels.value(stage);
}

struct Preview {
    CPreviewExportTransaction transaction;
    QStringList calls;
    QMap<QString, int> faults;
    std::function<void(const QString&)> onCall;
    bool frozen = false;
    int model = 0;
    int view = 7;
    int exportCount = 0;

    CPreviewExportTransaction::Steps steps(bool temporary = true)
    {
        CPreviewExportTransaction::Steps result;
        result.pause = step(QStringLiteral("pause"));
        if (temporary) {
            result.beginTemporary = step(QStringLiteral("begin"));
            result.endTemporary = step(QStringLiteral("end"));
        }
        result.exportFile = step(QStringLiteral("export"));
        result.restorePreview = step(QStringLiteral("restore"));
        result.resume = step(QStringLiteral("resume"));
        return result;
    }

    CPreviewExportTransaction::Step step(const QString& name)
    {
        return [this, name](QString* error) {
            calls.append(name);
            if (onCall) onCall(name);
            if (name == QStringLiteral("pause")) frozen = true;
            if (name == QStringLiteral("begin")) { model = 1; view = 99; }
            if (name == QStringLiteral("export")) { ++exportCount; view = 88; }
            switch (faults.value(name)) {
            case 1: *error = QStringLiteral("故障：") + name; return false;
            case 2: throw std::runtime_error("callback exception");
            case 3: throw 42;
            case 4: return false;
            default: break;
            }
            if (name == QStringLiteral("end")) model = 0;
            if (name == QStringLiteral("restore")) view = 7;
            if (name == QStringLiteral("resume")) frozen = false;
            return true;
        };
    }
};

void retainSnapshot(CPreviewExportTransaction::Steps& steps, const std::shared_ptr<int>& snapshot)
{
    for (auto* step : {&steps.pause, &steps.beginTemporary, &steps.exportFile,
                       &steps.endTemporary, &steps.restorePreview, &steps.resume}) {
        if (*step) *step = [previous = std::move(*step), snapshot](QString* error) { return previous(error); };
    }
}
}

class PreviewExportTransactionTests : public QObject
{
    Q_OBJECT
private slots:
    void normalOrderAndSameModel_data()
    {
        QTest::addColumn<bool>("temporary");
        QTest::newRow("temporary-model") << true;
        QTest::newRow("same-model") << false;
    }

    void normalOrderAndSameModel()
    {
        QFETCH(bool, temporary);
        Preview preview;
        QString error = QStringLiteral("previous error");
        QVERIFY(preview.transaction.Run(preview.steps(temporary), &error));
        QVERIFY(error.isEmpty());
        QCOMPARE(preview.calls, temporary ? allStages : QStringList({"pause", "export", "restore", "resume"}));
        QCOMPARE(preview.model, 0);
        QCOMPARE(preview.view, 7);
        QVERIFY(!preview.frozen);
        QVERIFY(!preview.transaction.IsBusy());
        QVERIFY(!preview.transaction.NeedsRecovery());
        QVERIFY(preview.transaction.Recover(&error));
        QCOMPARE(preview.exportCount, 1);
        QVERIFY(preview.transaction.Run(preview.steps(temporary)));
        QCOMPARE(preview.exportCount, 2);
    }

    void everyFailureAndExceptionPreservesRecoveryOrder_data()
    {
        QTest::addColumn<QString>("stage");
        QTest::addColumn<int>("fault");
        for (const QString& stage : allStages)
            for (int fault : {1, 2, 3, 4})
                QTest::newRow(qPrintable(stage + QString::number(fault))) << stage << fault;
    }

    void everyFailureAndExceptionPreservesRecoveryOrder()
    {
        QFETCH(QString, stage);
        QFETCH(int, fault);
        Preview preview;
        preview.faults.insert(stage, fault);
        QString error;
        QVERIFY(!preview.transaction.Run(preview.steps(), &error));
        QVERIFY2(error.startsWith(stageLabel(stage)), qPrintable(error));
        if (fault == 1) QVERIFY(error.contains(QStringLiteral("故障：") + stage));
        if (fault == 2) QVERIFY(error.contains(QStringLiteral("callback exception")));
        if (fault == 3) QVERIFY(error.contains(QStringLiteral("未知异常")));
        const int failedIndex = allStages.indexOf(stage);
        if (failedIndex < 3) {
            QStringList expected = allStages;
            if (stage == QStringLiteral("pause")) expected = {"pause", "restore", "resume"};
            if (stage == QStringLiteral("begin")) expected = {"pause", "begin", "end", "restore", "resume"};
            QCOMPARE(preview.calls, expected);
            QCOMPARE(preview.model, 0);
            QCOMPARE(preview.view, 7);
            QVERIFY(!preview.frozen);
            QVERIFY(!preview.transaction.IsBusy());
            QVERIFY(!preview.transaction.NeedsRecovery());
        } else {
            QCOMPARE(preview.calls, allStages.mid(0, failedIndex + 1));
            QVERIFY(preview.frozen);
            QVERIFY(preview.transaction.IsBusy());
            QVERIFY(preview.transaction.NeedsRecovery());
            QCOMPARE(preview.model, stage == QStringLiteral("end") ? 1 : 0);
            QCOMPARE(preview.view, stage == QStringLiteral("resume") ? 7 : 88);
            const QStringList beforeRetry = preview.calls;
            QVERIFY(!preview.transaction.Run(preview.steps(), &error));
            QCOMPARE(preview.calls, beforeRetry);
            preview.faults.clear();
            QVERIFY2(preview.transaction.Recover(&error), qPrintable(error));
            QVERIFY(error.isEmpty());
            QCOMPARE(preview.calls, beforeRetry + allStages.mid(failedIndex));
            QCOMPARE(preview.model, 0);
            QCOMPARE(preview.view, 7);
            QVERIFY(!preview.frozen);
            QVERIFY(!preview.transaction.IsBusy());
            QVERIFY(!preview.transaction.NeedsRecovery());
            QCOMPARE(preview.exportCount, 1);
        }
    }

    void failuresDuringPauseAndBeginStillRestoreOriginalState()
    {
        Preview preview;
        preview.faults = {{"pause", 1}, {"restore", 1}};
        QString error;
        QVERIFY(!preview.transaction.Run(preview.steps(), &error));
        QCOMPARE(preview.calls, QStringList({"pause", "restore"}));
        QVERIFY(preview.frozen);
        QVERIFY(error.contains(QStringLiteral("暂停预览失败")));
        QVERIFY(error.contains(QStringLiteral("恢复预览状态失败")));
        preview.faults.clear();
        QVERIFY(preview.transaction.Recover(&error));
        QCOMPARE(preview.calls, QStringList({"pause", "restore", "restore", "resume"}));
        QCOMPARE(preview.exportCount, 0);
        QVERIFY(!preview.frozen);

        preview.calls.clear();
        preview.faults = {{"begin", 2}, {"end", 1}};
        QVERIFY(!preview.transaction.Run(preview.steps(), &error));
        QCOMPARE(preview.calls, QStringList({"pause", "begin", "end"}));
        QCOMPARE(preview.model, 1);
        QVERIFY(preview.frozen);
        QVERIFY(error.contains(QStringLiteral("进入临时模型失败")));
        QVERIFY(error.contains(QStringLiteral("恢复原模型失败")));
        preview.faults.clear();
        QVERIFY(preview.transaction.Recover(&error));
        QCOMPARE(preview.model, 0);
        QCOMPARE(preview.view, 7);
        QVERIFY(!preview.frozen);
        QCOMPARE(preview.exportCount, 0);
    }

    void repeatedRecoveryKeepsSnapshotAndNeverReexports()
    {
        Preview preview;
        preview.faults = {{"export", 1}, {"end", 1}};
        auto snapshot = std::make_shared<int>(23);
        const std::weak_ptr<int> lifetime = snapshot;
        auto steps = preview.steps();
        retainSnapshot(steps, snapshot);
        snapshot.reset();
        QString error;
        QVERIFY(!preview.transaction.Run(std::move(steps), &error));
        QVERIFY(!lifetime.expired());
        QVERIFY(!preview.transaction.Recover(&error));
        QVERIFY(error.contains(QStringLiteral("导出文件失败")));
        QVERIFY(error.contains(QStringLiteral("恢复原模型失败")));
        QCOMPARE(preview.calls.count(QStringLiteral("end")), 2);
        QVERIFY(!lifetime.expired());
        QVERIFY(preview.frozen);
        preview.faults.remove(QStringLiteral("end"));
        preview.faults.insert(QStringLiteral("restore"), 2);
        QVERIFY(!preview.transaction.Recover(&error));
        QCOMPARE(preview.model, 0);
        QCOMPARE(preview.calls.count(QStringLiteral("end")), 3);
        QVERIFY(!lifetime.expired());
        preview.faults.remove(QStringLiteral("restore"));
        preview.faults.insert(QStringLiteral("resume"), 1);
        QVERIFY(!preview.transaction.Recover(&error));
        QCOMPARE(preview.calls.count(QStringLiteral("end")), 3);
        QCOMPARE(preview.calls.count(QStringLiteral("restore")), 2);
        QVERIFY(preview.frozen);
        preview.faults.remove(QStringLiteral("resume"));
        QVERIFY(preview.transaction.Recover(&error));
        QVERIFY(error.isEmpty());
        QCOMPARE(preview.exportCount, 1);
        QCOMPARE(preview.calls.count(QStringLiteral("end")), 3);
        QCOMPARE(preview.calls.count(QStringLiteral("restore")), 2);
        QCOMPARE(preview.calls.count(QStringLiteral("resume")), 2);
        QVERIFY(lifetime.expired());
        QVERIFY(!preview.transaction.IsBusy());
        QVERIFY(!preview.frozen);
    }

    void failedBeginWithoutSwitchStillRunsIdempotentRestoration()
    {
        Preview preview;
        auto steps = preview.steps();
        steps.beginTemporary = [&](QString* error) {
            preview.calls.append(QStringLiteral("begin"));
            *error = QStringLiteral("候选模型无法加载");
            return false;
        };
        QString error;
        QVERIFY(!preview.transaction.Run(std::move(steps), &error));
        QVERIFY(error.contains(QStringLiteral("候选模型无法加载")));
        QCOMPARE(preview.calls, QStringList({"pause", "begin", "end", "restore", "resume"}));
        QCOMPARE(preview.model, 0);
        QCOMPARE(preview.view, 7);
        QCOMPARE(preview.exportCount, 0);
        QVERIFY(!preview.frozen);
        QVERIFY(!preview.transaction.IsBusy());
    }

    void sameModelFailureRetainsFreezeUntilPreviewRestores()
    {
        Preview preview;
        preview.faults = {{"export", 2}, {"restore", 1}};
        QString error;
        QVERIFY(!preview.transaction.Run(preview.steps(false), &error));
        QCOMPARE(preview.calls, QStringList({"pause", "export", "restore"}));
        QCOMPARE(preview.model, 0);
        QCOMPARE(preview.view, 88);
        QVERIFY(preview.frozen);
        QVERIFY(preview.transaction.NeedsRecovery());
        preview.faults.clear();
        QVERIFY(preview.transaction.Recover(&error));
        QCOMPARE(preview.calls, QStringList({"pause", "export", "restore", "restore", "resume"}));
        QCOMPARE(preview.view, 7);
        QCOMPARE(preview.exportCount, 1);
        QVERIFY(!preview.frozen);
    }

    void missingStepsAreRejectedBeforeAnyMutation_data()
    {
        QTest::addColumn<QString>("missing");
        for (const QString& stage : allStages) QTest::newRow(qPrintable(stage)) << stage;
    }

    void missingStepsAreRejectedBeforeAnyMutation()
    {
        QFETCH(QString, missing);
        Preview preview;
        auto steps = preview.steps();
        const QMap<QString, CPreviewExportTransaction::Step*> members{{"pause", &steps.pause},
            {"begin", &steps.beginTemporary}, {"export", &steps.exportFile}, {"end", &steps.endTemporary},
            {"restore", &steps.restorePreview}, {"resume", &steps.resume}};
        *members.value(missing) = {};
        QString error;
        QVERIFY(!preview.transaction.Run(std::move(steps), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(preview.calls.isEmpty());
        QVERIFY(!preview.transaction.IsBusy());
        QVERIFY(!preview.frozen);
        QCOMPARE(preview.model, 0);
        QCOMPARE(preview.view, 7);
    }

    void callbacksCannotReenterRunRecoverOrReset()
    {
        Preview preview;
        int attempts = 0;
        preview.onCall = [&](const QString&) {
            QVERIFY(preview.transaction.IsBusy());
            const auto before = preview.calls;
            QString error;
            QVERIFY(!preview.transaction.Run(preview.steps(), &error));
            QVERIFY(!error.isEmpty());
            QVERIFY(!preview.transaction.Recover(&error));
            QVERIFY(!error.isEmpty());
            QVERIFY(!preview.transaction.ResetAfterPreviewClosed());
            QCOMPARE(preview.calls, before);
            ++attempts;
        };
        QVERIFY(preview.transaction.Run(preview.steps()));
        QCOMPARE(attempts, 6);
        preview.faults.insert(QStringLiteral("end"), 1);
        QVERIFY(!preview.transaction.Run(preview.steps()));
        preview.faults.clear();
        QVERIFY(preview.transaction.Recover());
        QCOMPARE(attempts, 13);
    }

    void nestedEventLoopObservesIsolationThroughoutExportAndRecovery()
    {
        Preview preview;
        int events = 0;
        preview.onCall = [&](const QString& stage) {
            if (stage != QStringLiteral("export") && stage != QStringLiteral("end")) return;
            QEventLoop loop;
            QTimer::singleShot(0, &loop, [&] {
                QVERIFY(preview.transaction.IsBusy());
                QVERIFY(preview.frozen);
                QString error;
                QVERIFY(!preview.transaction.Run(preview.steps(), &error));
                QVERIFY(!preview.transaction.Recover(&error));
                QVERIFY(!preview.transaction.ResetAfterPreviewClosed());
                ++events;
                loop.quit();
            });
            QTimer::singleShot(1000, &loop, &QEventLoop::quit);
            loop.exec();
        };
        preview.faults.insert(QStringLiteral("end"), 1);
        QVERIFY(!preview.transaction.Run(preview.steps()));
        QCOMPARE(events, 2);
        QVERIFY(preview.transaction.NeedsRecovery());
        preview.faults.clear();
        QVERIFY(preview.transaction.Recover());
        QCOMPARE(events, 3);
        QCOMPARE(preview.exportCount, 1);
        QVERIFY(!preview.frozen);
    }

    void destructionDropsSnapshotWithoutResumingOrRestoring()
    {
        QStringList calls;
        std::weak_ptr<int> lifetime;
        {
            Preview preview;
            preview.faults.insert(QStringLiteral("end"), 1);
            preview.onCall = [&](const QString& stage) { calls.append(stage); };
            auto snapshot = std::make_shared<int>(1);
            lifetime = snapshot;
            auto steps = preview.steps();
            retainSnapshot(steps, snapshot);
            snapshot.reset();
            QVERIFY(!preview.transaction.Run(std::move(steps)));
            QVERIFY(!lifetime.expired());
        }
        QVERIFY(lifetime.expired());
        QCOMPARE(calls, QStringList({"pause", "begin", "export", "end"}));
    }

    void resetAfterClosedDropsPendingSnapshotWithoutCallbacks()
    {
        Preview preview;
        preview.faults.insert(QStringLiteral("restore"), 1);
        auto snapshot = std::make_shared<int>(1);
        const std::weak_ptr<int> lifetime = snapshot;
        auto steps = preview.steps();
        retainSnapshot(steps, snapshot);
        snapshot.reset();
        QVERIFY(!preview.transaction.Run(std::move(steps)));
        const auto calls = preview.calls;
        preview.model = -1;
        QVERIFY(preview.transaction.ResetAfterPreviewClosed());
        QVERIFY(lifetime.expired());
        QCOMPARE(preview.calls, calls);
        QCOMPARE(preview.model, -1);
        QVERIFY(!preview.transaction.IsBusy());
        QVERIFY(!preview.transaction.NeedsRecovery());
        QVERIFY(preview.transaction.ResetAfterPreviewClosed());
    }

    void snapshotCleanupCannotReenterTransaction()
    {
        Preview preview;
        preview.faults.insert(QStringLiteral("end"), 1);
        int deleted = 0;
        auto snapshot = std::shared_ptr<int>(new int(1), [&](int* value) {
            delete value;
            ++deleted;
            QVERIFY(preview.transaction.IsBusy());
            QVERIFY(!preview.transaction.ResetAfterPreviewClosed());
            QVERIFY(!preview.transaction.Run(preview.steps()));
            QVERIFY(!preview.transaction.Recover());
        });
        auto steps = preview.steps();
        retainSnapshot(steps, snapshot);
        snapshot.reset();
        QVERIFY(!preview.transaction.Run(std::move(steps)));
        QVERIFY(preview.transaction.ResetAfterPreviewClosed());
        QCOMPARE(deleted, 1);
        QVERIFY(!preview.transaction.IsBusy());
    }

    void destructionRejectsReentryFromSnapshotCleanup()
    {
        int cleanupCount = 0;
        {
            CPreviewExportTransaction transaction;
            auto snapshot = std::shared_ptr<int>(new int(1), [&](int* value) {
                delete value;
                ++cleanupCount;
                QVERIFY(transaction.IsBusy());
                QVERIFY(!transaction.Run({}));
                QVERIFY(!transaction.Recover());
                QVERIFY(!transaction.ResetAfterPreviewClosed());
            });
            CPreviewExportTransaction::Steps steps;
            const auto succeed = [](QString*) { return true; };
            steps.pause = succeed;
            steps.exportFile = succeed;
            steps.restorePreview = [](QString*) { return false; };
            steps.resume = succeed;
            retainSnapshot(steps, snapshot);
            snapshot.reset();
            QVERIFY(!transaction.Run(std::move(steps)));
            QVERIFY(transaction.NeedsRecovery());
        }
        QCOMPARE(cleanupCount, 1);
    }
};

QTEST_GUILESS_MAIN(PreviewExportTransactionTests)
#include "preview_export_transaction_tests.moc"
