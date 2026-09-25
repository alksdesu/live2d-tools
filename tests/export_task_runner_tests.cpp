#include "ui/cexporttaskrunner.h"
#include "std/cexportcontrol.h"

#include <QtTest>
#include <QEventLoop>
#include <QPushButton>
#include <QSemaphore>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <stdexcept>
#include <thread>

class ExportTaskRunnerTests : public QObject
{
    Q_OBJECT
private slots:
    void cpuRunsOnWorkerAndGuiStepsAreSerial()
    {
        QThread* const mainThread = QThread::currentThread();
        bool workerThread = false, guiThread = true, foreignRejected = false;
        int active = 0, maximumActive = 0, sum = 0, lateCalls = 0;
        QStringList steps;
        CExportTaskRunner::GuiCall retained;
        QString error;
        QVERIFY2(CExportTaskRunner::Run([&](const auto& gui, QString* taskError) {
            retained = gui;
            workerThread = QThread::currentThread() != mainThread;
            for (int index = 0; index < 5; ++index) {
                sum += index;
                if (!gui([&, index](QString*) {
                    guiThread = guiThread && QThread::currentThread() == mainThread;
                    ++active;
                    maximumActive = qMax(maximumActive, active);
                    steps.append(QString::number(index));
                    QString rejected;
                    foreignRejected = !gui([&](QString*) { ++lateCalls; return true; }, &rejected) && !rejected.isEmpty();
                    QEventLoop events;
                    QTimer::singleShot(0, &events, &QEventLoop::quit);
                    events.exec();
                    --active;
                    return true;
                }, taskError)) return false;
            }
            return true;
        }, nullptr, &error), qPrintable(error));
        QVERIFY(error.isEmpty());
        QVERIFY(workerThread);
        QVERIFY(guiThread);
        QVERIFY(foreignRejected);
        QCOMPARE(maximumActive, 1);
        QCOMPARE(active, 0);
        QCOMPARE(sum, 10);
        QCOMPARE(steps, QStringList({"0", "1", "2", "3", "4"}));
        QVERIFY(!retained([&](QString*) { ++lateCalls; return true; }, &error));
        QVERIFY(!error.isEmpty());
        bool expiredRejected = false;
        std::thread outsider([&] {
            QString reason;
            expiredRejected = !retained([&](QString*) { ++lateCalls; return true; }, &reason) && !reason.isEmpty();
        });
        outsider.join();
        QVERIFY(expiredRejected);
        QCOMPARE(lateCalls, 0);
    }

    void buttonCancellationKeepsEventLoopResponsiveAndAllowsGuiCleanup_data()
    {
        QTest::addColumn<int>("cleanupFault");
        QTest::newRow("clean-restoration") << 0;
        QTest::newRow("restoration-failure") << 1;
        QTest::newRow("restoration-exception") << 2;
    }

    void buttonCancellationKeepsEventLoopResponsiveAndAllowsGuiCleanup()
    {
        QFETCH(int, cleanupFault);
        QPushButton cancel(QStringLiteral("取消导出"));
        cancel.show();
        CExportControl control;
        QSemaphore clicked;
        std::atomic<bool> workerActive{false};
        bool buttonOnGui = false, buttonDuringWork = false, workerOnCpu = false, cleanedOnGui = false;
        int resources = 0, cleanupCalls = 0;
        QThread* const mainThread = QThread::currentThread();
        QObject::connect(&cancel, &QPushButton::clicked, &cancel, [&] {
            buttonOnGui = QThread::currentThread() == mainThread;
            buttonDuringWork = workerActive.load();
            control.Cancel();
            clicked.release();
        });
        QString error;
        QVERIFY(!CExportTaskRunner::Run([&](const auto& gui, QString* taskError) {
            workerActive = true;
            workerOnCpu = QThread::currentThread() != mainThread;
            if (!gui([&](QString*) {
                ++resources;
                QTimer::singleShot(0, &cancel, [&] { QTest::mouseClick(&cancel, Qt::LeftButton); });
                return true;
            }, taskError)) return false;
            const bool receivedClick = clicked.tryAcquire(1, 5000);
            const bool mayContinue = control.Checkpoint();
            const bool cleaned = gui([&](QString* reason) {
                cleanedOnGui = QThread::currentThread() == mainThread;
                ++cleanupCalls;
                if (cleanupFault == 1) {
                    *reason = QStringLiteral("restoration failure");
                    return false;
                }
                if (cleanupFault == 2) throw std::runtime_error("restoration exception");
                --resources;
                return true;
            }, taskError);
            workerActive = false;
            return receivedClick && cleaned && mayContinue;
        }, &control, &error));
        QVERIFY(buttonOnGui);
        QVERIFY(buttonDuringWork);
        QVERIFY(workerOnCpu);
        QVERIFY(cleanedOnGui);
        QCOMPARE(resources, cleanupFault == 0 ? 0 : 1);
        QCOMPARE(cleanupCalls, 1);
        QCOMPARE(control.WasCanceled(), cleanupFault == 0);
        QCOMPARE(error, control.Error());
        if (cleanupFault == 1) QVERIFY(error.contains(QStringLiteral("restoration failure")));
        if (cleanupFault == 2) QVERIFY(error.contains(QStringLiteral("restoration exception")));
    }

    void nestedRunIsRejectedAndNextIndependentRunSucceeds()
    {
        bool nestedAccepted = true;
        int nestedCalls = 0;
        QString nestedError, error;
        QVERIFY2(CExportTaskRunner::Run([&](const auto& gui, QString* taskError) {
            return gui([&](QString*) {
                nestedAccepted = CExportTaskRunner::Run([&](const auto&, QString*) { ++nestedCalls; return true; }, nullptr, &nestedError);
                return true;
            }, taskError);
        }, nullptr, &error), qPrintable(error));
        QVERIFY(!nestedAccepted);
        QVERIFY(!nestedError.isEmpty());
        QCOMPARE(nestedCalls, 0);
        QVERIFY(CExportTaskRunner::Run([](const auto&, QString*) { return true; }, nullptr, &error));
        QVERIFY(error.isEmpty());
    }

    void nestedRunFromInitialCheckpointIsRejected()
    {
        CExportControl control;
        bool nestedAccepted = true;
        int nestedCalls = 0;
        QString nestedError;
        QVERIFY(control.SetHandler([&](const auto&, QString*) {
            if (QThread::currentThread() == qApp->thread())
                nestedAccepted = CExportTaskRunner::Run([&](const auto&, QString*) { ++nestedCalls; return true; }, nullptr, &nestedError);
            return true;
        }));
        QVERIFY(CExportTaskRunner::Run([](const auto&, QString*) { return true; }, &control));
        QVERIFY(!nestedAccepted);
        QVERIFY(!nestedError.isEmpty());
        QCOMPARE(nestedCalls, 0);
    }

    void workerAndGuiFailuresAreCaptured_data()
    {
        QTest::addColumn<bool>("inGui");
        QTest::addColumn<int>("fault");
        for (bool inGui : {false, true})
            for (int fault : {0, 1, 2, 3})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(inGui ? "gui" : "worker").arg(fault))) << inGui << fault;
    }

    void workerAndGuiFailuresAreCaptured()
    {
        QFETCH(bool, inGui);
        QFETCH(int, fault);
        CExportControl control;
        QString error;
        const auto operation = [fault](QString* reason) -> bool {
            if (fault == 0) *reason = QStringLiteral("operation failure");
            if (fault == 2) throw std::runtime_error("operation exception");
            if (fault == 3) throw 41;
            return false;
        };
        QVERIFY(!CExportTaskRunner::Run([&](const auto& gui, QString* taskError) {
            return inGui ? gui(operation, taskError) : operation(taskError);
        }, &control, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!control.WasCanceled());
        if (fault == 0) QCOMPARE(error, QStringLiteral("operation failure"));
        if (fault == 2) QCOMPARE(error, QStringLiteral("operation exception"));
        QCOMPARE(control.Error(), error);
        control.Cancel();
        QVERIFY(!control.Checkpoint());
        QVERIFY(!control.WasCanceled());
        QCOMPARE(control.Error(), error);
        QVERIFY(CExportTaskRunner::Run([](const auto&, QString*) { return true; }));
    }

    void emptyGuiOperationAndForeignRunAreRejected()
    {
        bool emptyAccepted = true, nestedAccepted = true;
        QString emptyError, nestedError;
        QVERIFY(CExportTaskRunner::Run([&](const auto& gui, QString*) {
            emptyAccepted = gui({}, &emptyError);
            nestedAccepted = CExportTaskRunner::Run([](const auto&, QString*) { return true; }, nullptr, &nestedError);
            return true;
        }));
        QVERIFY(!emptyAccepted);
        QVERIFY(!emptyError.isEmpty());
        QVERIFY(!nestedAccepted);
        QVERIFY(!nestedError.isEmpty());
        QString error;
        QVERIFY(!CExportTaskRunner::Run({}, nullptr, &error));
        QVERIFY(!error.isEmpty());
    }

    void preCanceledTaskNeverStarts()
    {
        CExportControl control;
        control.Cancel();
        int calls = 0;
        QString error;
        QVERIFY(!CExportTaskRunner::Run([&](const auto&, QString*) { ++calls; return true; }, &control, &error));
        QCOMPARE(calls, 0);
        QVERIFY(control.WasCanceled());
        QCOMPARE(error, control.Error());
    }

    void cancellationBeforeReturnCannotBecomeUncommittedSuccess_data()
    {
        QTest::addColumn<bool>("committed");
        QTest::newRow("before-commit") << false;
        QTest::newRow("after-commit") << true;
    }

    void cancellationBeforeReturnCannotBecomeUncommittedSuccess()
    {
        QFETCH(bool, committed);
        CExportControl control;
        QString error;
        const bool accepted = CExportTaskRunner::Run([&](const auto&, QString*) {
            if (committed) {
                if (!control.BeginCommit()) return false;
                control.CommitFinished(true);
            }
            control.Cancel();
            return true;
        }, &control, &error);
        QCOMPARE(accepted, committed);
        QCOMPARE(control.IsCommitted(), committed);
        QCOMPARE(control.WasCanceled(), !committed);
        QCOMPARE(error.isEmpty(), committed);
    }
};

QTEST_MAIN(ExportTaskRunnerTests)
#include "export_task_runner_tests.moc"
