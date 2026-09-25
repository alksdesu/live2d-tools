#include "std/cexportcontrol.h"

#include <QtTest>
#include <QSemaphore>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <thread>

class ExportControlTests : public QObject
{
    Q_OBJECT
private slots:
    void progressAndCancellationBeforeCommit()
    {
        CExportControl control;
        int callbacks = 0;
        CExportControl::Progress observed;
        QVERIFY(control.SetHandler([&](const auto& progress, QString*) {
            ++callbacks;
            observed = progress;
            return true;
        }));
        QVERIFY(control.Report(CExportControl::Phase::Writing, 7, 10, QStringLiteral("layer")));
        QCOMPARE(callbacks, 1);
        QCOMPARE(observed.phase, CExportControl::Phase::Writing);
        QCOMPARE(observed.current, 7);
        QCOMPARE(observed.total, 10);
        QCOMPARE(observed.detail, QStringLiteral("layer"));
        control.Cancel();
        QVERIFY(control.IsCancellationRequested());
        QVERIFY(!control.BeginCommit());
        QVERIFY(control.WasCanceled());
        QVERIFY(!control.IsCommitted());
        QCOMPARE(callbacks, 1);
        const QString error = control.Error();
        QVERIFY(!error.isEmpty());
        control.CommitFinished(true);
        QCOMPARE(control.Error(), error);
        QVERIFY(!control.Checkpoint());
        QVERIFY(!control.SetHandler({}));
    }

    void restorationFailureAfterObservedCancellationRemainsARealFailure()
    {
        CExportControl control;
        control.Cancel();
        QVERIFY(!control.Checkpoint());
        QVERIFY(control.WasCanceled());
        const QString cancellation = control.Error();
        control.Fail(QStringLiteral("restore framebuffer failed"));
        QVERIFY(!control.WasCanceled());
        QVERIFY(!control.IsCommitted());
        QVERIFY(control.Error().contains(cancellation));
        QVERIFY(control.Error().contains(QStringLiteral("restore framebuffer failed")));
        const QString failure = control.Error();
        control.Cancel();
        QVERIFY(!control.Checkpoint());
        control.Fail(QStringLiteral("secondary error"));
        QCOMPARE(control.Error(), failure);
        QVERIFY(!control.WasCanceled());
    }

    void cancelFromCommitNotificationStillPreventsCommit()
    {
        CExportControl control;
        bool commitReported = false;
        QVERIFY(control.SetHandler([&](const auto& progress, QString*) {
            if (progress.phase == CExportControl::Phase::Committing) {
                commitReported = true;
                control.Cancel();
            }
            return true;
        }));
        QVERIFY(!control.BeginCommit());
        QVERIFY(commitReported);
        QVERIFY(control.WasCanceled());
        QVERIFY(!control.IsCommitted());
    }

    void lateCancellationDoesNotUndoSuccessfulCommit()
    {
        CExportControl control;
        QVERIFY(control.BeginCommit());
        control.Cancel();
        QVERIFY(control.IsCancellationRequested());
        QVERIFY(control.Checkpoint());
        QVERIFY(!control.WasCanceled());
        QVERIFY(!control.BeginCommit());
        control.CommitFinished(true);
        QVERIFY(control.IsCommitted());
        QVERIFY(control.Error().isEmpty());
        QVERIFY(control.Checkpoint());
        control.Fail(QStringLiteral("too late"));
        control.CommitFinished(false, QStringLiteral("too late"));
        QVERIFY(control.IsCommitted());
        QVERIFY(control.Error().isEmpty());
    }

    void genuineFailureWinsOverPendingCancel_data()
    {
        QTest::addColumn<bool>("duringCommit");
        QTest::newRow("worker-failure") << false;
        QTest::newRow("commit-failure") << true;
    }

    void genuineFailureWinsOverPendingCancel()
    {
        QFETCH(bool, duringCommit);
        CExportControl control;
        if (duringCommit) QVERIFY(control.BeginCommit());
        control.Cancel();
        if (duringCommit) control.CommitFinished(false, QStringLiteral("disk failure"));
        else control.Fail(QStringLiteral("disk failure"));
        QVERIFY(!control.Checkpoint());
        QVERIFY(!control.WasCanceled());
        QVERIFY(!control.IsCommitted());
        QCOMPARE(control.Error(), QStringLiteral("disk failure"));
        control.Fail(QStringLiteral("secondary failure"));
        QCOMPARE(control.Error(), QStringLiteral("disk failure"));
    }

    void handlerErrorsAndExceptionsBecomeStickyFailures_data()
    {
        QTest::addColumn<int>("fault");
        QTest::newRow("error") << 0;
        QTest::newRow("empty-error") << 1;
        QTest::newRow("exception") << 2;
        QTest::newRow("unknown-exception") << 3;
    }

    void handlerErrorsAndExceptionsBecomeStickyFailures()
    {
        QFETCH(int, fault);
        CExportControl control;
        int calls = 0;
        QVERIFY(control.SetHandler([&](const auto&, QString* error) -> bool {
            ++calls;
            if (fault == 0) *error = QStringLiteral("progress failure");
            if (fault == 2) throw std::runtime_error("progress exception");
            if (fault == 3) throw 37;
            return false;
        }));
        QVERIFY(!control.Checkpoint());
        QCOMPARE(calls, 1);
        QVERIFY(!control.WasCanceled());
        QVERIFY(!control.Error().isEmpty());
        if (fault == 0) QCOMPARE(control.Error(), QStringLiteral("progress failure"));
        if (fault == 2) QCOMPARE(control.Error(), QStringLiteral("progress exception"));
        const QString firstError = control.Error();
        control.Cancel();
        QVERIFY(!control.Checkpoint());
        QCOMPARE(calls, 1);
        QCOMPARE(control.Error(), firstError);
        QVERIFY(!control.WasCanceled());
    }

    void recursiveCheckpointFailsAndHandlerReplacementIsRejected()
    {
        CExportControl control;
        bool replacement = true, recursive = true;
        int calls = 0;
        QVERIFY(control.SetHandler([&](const auto&, QString*) {
            ++calls;
            replacement = control.SetHandler({});
            recursive = control.Checkpoint();
            return true;
        }));
        QVERIFY(!control.Checkpoint());
        QVERIFY(!replacement);
        QVERIFY(!recursive);
        QCOMPARE(calls, 1);
        QVERIFY(!control.Error().isEmpty());
        QVERIFY(!control.WasCanceled());
    }

    void concurrentCancelDuringHandlerDoesNotDeadlockOrPublishSuccess()
    {
        CExportControl control;
        QSemaphore entered, proceed;
        std::atomic<bool> callbackTimedOut{false};
        QVERIFY(control.SetHandler([&](const auto&, QString*) {
            entered.release();
            callbackTimedOut = !proceed.tryAcquire(1, 5000);
            return true;
        }));
        bool accepted = true;
        std::thread worker([&] { accepted = control.Report(CExportControl::Phase::Encoding, 2, 8); });
        const bool enteredHandler = entered.tryAcquire(1, 5000);
        const auto progress = control.Snapshot();
        control.Cancel();
        proceed.release();
        worker.join();
        QVERIFY(enteredHandler);
        QVERIFY(!callbackTimedOut);
        QCOMPARE(progress.phase, CExportControl::Phase::Encoding);
        QCOMPARE(progress.current, 2);
        QCOMPARE(progress.total, 8);
        QVERIFY(!accepted);
        QVERIFY(control.WasCanceled());
        QVERIFY(!control.BeginCommit());
    }

    void progressCountsAreBoundedWithoutChangingDetail()
    {
        CExportControl control;
        QVERIFY(control.Report(CExportControl::Phase::Preparing, -1, -7, QStringLiteral("source")));
        QCOMPARE(control.Snapshot().current, 0);
        QCOMPARE(control.Snapshot().total, 0);
        QCOMPARE(control.Snapshot().detail, QStringLiteral("source"));
        QVERIFY(control.Report(CExportControl::Phase::Writing, std::numeric_limits<qint64>::max(),
                               std::numeric_limits<qint64>::max()));
        QCOMPARE(control.Snapshot().current, std::numeric_limits<qint64>::max());
    }
};

QTEST_GUILESS_MAIN(ExportControlTests)
#include "export_control_tests.moc"
