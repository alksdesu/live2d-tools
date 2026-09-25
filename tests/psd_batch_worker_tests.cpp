#include "ui/psdbatchworker.h"
#include "ui/psdbatchjob.h"
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QEventLoop>
#include <QPointer>
#include <memory>

class PsdBatchWorkerTests : public QObject
{
    Q_OBJECT
private slots:
    void canceledCurrentFileHasDistinctOutcomeAndStopsAfterAcknowledgment()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        PsdBatchJob job;
        QSignalSpy requests(&job, &PsdBatchJob::requestExport);
        QSignalSpy canceled(&job, &PsdBatchJob::cancelCurrentRequested);
        QSignalSpy ready(&job, &PsdBatchJob::resultsReady);
        QSignalSpy finished(&job, &PsdBatchJob::finished);
        QVERIFY(job.start({QStringLiteral("/first.json"), QStringLiteral("/second.json"), QStringLiteral("/third.json")}, temp.path(), true, false));
        QTRY_COMPARE(requests.size(), 1);
        const QString first = requests[0][1].toString();
        job.completeDetailed(false, first, QStringLiteral("先前文件写入失败"), PsdExportStage::Writing);
        QTRY_COMPARE(requests.size(), 2);
        const QString second = requests[1][1].toString();
        job.setStage(second, PsdExportStage::Encoding);
        QCOMPARE(job.results()[1].stage, PsdExportStage::Encoding);
        job.setStage(second, PsdExportStage::Writing);
        QCOMPARE(job.results()[1].stage, PsdExportStage::Writing);
        job.cancel();
        job.cancel();
        QCOMPARE(canceled.size(), 1);
        QCOMPARE(job.results()[1].state, PsdBatchFileState::Running);
        QCOMPARE(job.results()[2].state, PsdBatchFileState::NotRun);
        QCOMPARE(ready.size(), 0);
        const auto waiting = job.results();
        job.completeCanceled(first, PsdExportStage::Encoding, QStringLiteral("stale"));
        QVERIFY(job.results() == waiting);
        QTest::qWait(12);
        const QString reason = QStringLiteral("用户在写入时取消 <literal>");
        job.completeCanceled(second, PsdExportStage::Writing, reason);
        const auto completed = job.results();
        QCOMPARE(completed[1].state, PsdBatchFileState::Canceled);
        QCOMPARE(completed[1].stage, PsdExportStage::Writing);
        QCOMPARE(completed[1].error, reason);
        QVERIFY(!completed[1].outputCommitted);
        QVERIFY(completed[1].elapsedMs >= 10);
        QVERIFY(IsPsdBatchTerminal(completed[1].state));
        QVERIFY(IsPsdBatchResultValid(completed[1]));
        job.completeCanceled(second);
        job.completeDetailed(true, second, {});
        QVERIFY(job.results() == completed);
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(finished.size(), 1);
        QVERIFY(ready[0][1].toBool());
        QVERIFY(qvariant_cast<PsdBatchResults>(ready[0][0]) == completed);
        QCOMPARE(finished[0][0].toStringList(), QStringList{first});
        QCOMPARE(requests.size(), 2);
        job.shutdown();
        QVERIFY(job.results() == completed);
    }

    void cancelSignalSeesAllPendingStatesAndSupportsNestedAcknowledgment()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        PsdBatchJob job;
        QSignalSpy requests(&job, &PsdBatchJob::requestExport);
        QSignalSpy ready(&job, &PsdBatchJob::resultsReady);
        QSignalSpy finished(&job, &PsdBatchJob::finished);
        QSignalSpy canceled(&job, &PsdBatchJob::cancelCurrentRequested);
        QVERIFY(job.start({QStringLiteral("/first.json"), QStringLiteral("/second.json"), QStringLiteral("/third.json")}, temp.path(), true, false));
        QTRY_COMPARE(requests.size(), 1);
        const QString output = requests[0][1].toString();
        connect(&job, &PsdBatchJob::cancelCurrentRequested, &job, [&] {
            const auto snapshot = job.results();
            QCOMPARE(snapshot[0].state, PsdBatchFileState::Running);
            QCOMPARE(snapshot[1].state, PsdBatchFileState::NotRun);
            QCOMPARE(snapshot[2].state, PsdBatchFileState::NotRun);
            job.cancel();
            job.completeCanceled(output, PsdExportStage::Encoding);
            QTRY_COMPARE(ready.size(), 1);
        });
        job.cancel();
        QCOMPARE(canceled.size(), 1);
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished[0][0].toStringList().isEmpty());
        QCOMPARE(requests.size(), 1);
        const auto snapshot = qvariant_cast<PsdBatchResults>(ready[0][0]);
        QCOMPARE(snapshot[0].state, PsdBatchFileState::Canceled);
        QCOMPARE(snapshot[1].state, PsdBatchFileState::NotRun);
        QCOMPARE(snapshot[2].state, PsdBatchFileState::NotRun);
        QVERIFY(!job.isRunning());
    }

    void currentCancellationAcknowledgmentDoesNotImplicitlyCancelBatch()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        PsdBatchJob job;
        QSignalSpy requests(&job, &PsdBatchJob::requestExport);
        QSignalSpy canceled(&job, &PsdBatchJob::cancelCurrentRequested);
        QSignalSpy finished(&job, &PsdBatchJob::finished);
        QVERIFY(job.start({QStringLiteral("/first.json"), QStringLiteral("/second.json")}, temp.path(), true, false));
        job.completeCanceled(job.results()[0].outputPath);
        QCOMPARE(job.results()[0].state, PsdBatchFileState::Pending);
        QTRY_COMPARE(requests.size(), 1);
        job.completeCanceled(requests[0][1].toString());
        QTRY_COMPARE(requests.size(), 2);
        QCOMPARE(job.results()[0].state, PsdBatchFileState::Canceled);
        QVERIFY(!job.results()[0].error.isEmpty());
        QCOMPARE(job.results()[1].state, PsdBatchFileState::Running);
        QCOMPARE(canceled.size(), 0);
        job.complete(true, requests[1][1].toString());
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished[0][0].toStringList().isEmpty());
        QVERIFY(!finished[0][1].toBool());
    }

    void committedSuccessRemainsSuccessWhenCancelArrivesBeforeAcknowledgment()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        PsdBatchJob job;
        QSignalSpy requests(&job, &PsdBatchJob::requestExport);
        QSignalSpy finished(&job, &PsdBatchJob::finished);
        QVERIFY(job.start({QStringLiteral("/first.json"), QStringLiteral("/second.json")}, temp.path(), true, false));
        QTRY_COMPARE(requests.size(), 1);
        const QString output = requests[0][1].toString();
        job.setStage(output, PsdExportStage::Writing);
        job.cancel();
        job.completeDetailed(true, output, {});
        job.completeCanceled(output, PsdExportStage::Writing);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(job.results()[0].state, PsdBatchFileState::Succeeded);
        QCOMPARE(job.results()[0].stage, PsdExportStage::Done);
        QVERIFY(job.results()[0].outputCommitted);
        QCOMPARE(job.results()[1].state, PsdBatchFileState::NotRun);
        QVERIFY(finished[0][0].toStringList().isEmpty());
        QVERIFY(finished[0][1].toBool());
    }

    void cancelSignalCanSynchronouslyShutdownOrDestroyJob_data()
    {
        QTest::addColumn<bool>("destroy");
        QTest::newRow("shutdown") << false;
        QTest::newRow("destroy") << true;
    }

    void renderingAndEncodingCanAlternateBeforeFinalWriting()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        PsdBatchJob job;
        QSignalSpy requests(&job, &PsdBatchJob::requestExport);
        QVERIFY(job.start({QStringLiteral("/first.json")}, temp.path(), true, false));
        QTRY_COMPARE(requests.size(), 1);
        const QString output = requests[0][1].toString();
        for (auto stage : {PsdExportStage::Exporting, PsdExportStage::Encoding, PsdExportStage::Exporting,
                           PsdExportStage::Encoding, PsdExportStage::Writing}) {
            job.setStage(output, stage);
            QCOMPARE(job.results()[0].stage, stage);
        }
        job.setStage(output, PsdExportStage::Encoding);
        QCOMPARE(job.results()[0].stage, PsdExportStage::Writing);
        job.setStage(output, PsdExportStage::Exporting);
        QCOMPARE(job.results()[0].stage, PsdExportStage::Writing);
        job.setStage(output, PsdExportStage::Restoring);
        job.setStage(output, PsdExportStage::Writing);
        QCOMPARE(job.results()[0].stage, PsdExportStage::Restoring);
        job.shutdown();
    }

    void cancelSignalCanSynchronouslyShutdownOrDestroyJob()
    {
        QFETCH(bool, destroy);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        auto job = std::make_unique<PsdBatchJob>();
        QPointer<PsdBatchJob> guard(job.get());
        QSignalSpy requests(job.get(), &PsdBatchJob::requestExport);
        QSignalSpy finished(job.get(), &PsdBatchJob::finished);
        QSignalSpy canceled(job.get(), &PsdBatchJob::cancelCurrentRequested);
        QVERIFY(job->start({QStringLiteral("/first.json"), QStringLiteral("/second.json")}, temp.path(), true, false));
        QTRY_COMPARE(requests.size(), 1);
        connect(job.get(), &PsdBatchJob::cancelCurrentRequested, this, [&] {
            QCOMPARE(job->results()[1].state, PsdBatchFileState::NotRun);
            if (destroy) job.reset();
            else job->shutdown();
        });
        job->cancel();
        QCOMPARE(canceled.size(), 1);
        QCoreApplication::processEvents();
        QCOMPARE(requests.size(), 1);
        QCOMPARE(finished.size(), 0);
        if (destroy) QVERIFY(guard.isNull());
        else {
            QVERIFY(!job->isRunning());
            job->completeCanceled(requests[0][1].toString());
            QCOMPARE(job->results()[0].state, PsdBatchFileState::Failed);
        }
    }

    void plannedResultsAreAvailableBeforeAnyGuiEvent()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString duplicate = QStringLiteral("/input/model.model3.json");
        const QStringList sources{duplicate, duplicate, QStringLiteral("/other/MODEL.model3.json")};
        PsdBatchJob job;
        QSignalSpy changed(&job, &PsdBatchJob::fileResultChanged);
        QVERIFY(job.start(sources, temp.path(), true, false));
        const auto planned = job.results();
        QCOMPARE(planned.size(), 3);
        QCOMPARE(changed.size(), 0);
        for (int index = 0; index < planned.size(); ++index) {
            QCOMPARE(planned[index].index, index);
            QCOMPARE(planned[index].sourcePath, sources[index]);
            QCOMPARE(planned[index].state, PsdBatchFileState::Pending);
            QCOMPARE(planned[index].stage, PsdExportStage::Queued);
            QVERIFY(!planned[index].outputCommitted);
            QCOMPARE(planned[index].elapsedMs, 0);
        }
        QCOMPARE(planned[0].outputPath, temp.filePath(QStringLiteral("model.psd")));
        QCOMPARE(planned[1].outputPath, temp.filePath(QStringLiteral("model #2.psd")));
        QCOMPARE(planned[2].outputPath, temp.filePath(QStringLiteral("MODEL #3.psd")));
        job.shutdown();
        for (const auto& item : job.results()) QCOMPARE(item.state, PsdBatchFileState::NotRun);
        for (const auto& item : planned) QCOMPARE(item.state, PsdBatchFileState::Pending);
    }

    void detailedResultsKeepErrorsPhasesElapsedAndCommittedFiles()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        PsdBatchJob job;
        QSignalSpy requests(&job, &PsdBatchJob::requestExport);
        QSignalSpy changed(&job, &PsdBatchJob::fileResultChanged);
        QSignalSpy ready(&job, &PsdBatchJob::resultsReady);
        QSignalSpy finished(&job, &PsdBatchJob::finished);
        QVERIFY(job.start({QStringLiteral("/input/one.json"), QStringLiteral("/input/two.json"),
                           QStringLiteral("/input/three.json")}, temp.path(), true, false));
        const auto planned = job.results();
        QTRY_COMPARE(requests.size(), 1);
        const QString first = requests[0][1].toString();
        QCOMPARE(job.results()[0].state, PsdBatchFileState::Running);
        QCOMPARE(job.results()[0].stage, PsdExportStage::Preflight);
        job.setStage(first, PsdExportStage::Preparing);
        job.setStage(first, PsdExportStage::Exporting);
        const auto exporting = job.results();
        job.setStage(QStringLiteral("wrong"), PsdExportStage::Restoring);
        job.setStage(first, PsdExportStage::Queued);
        job.setStage(first, PsdExportStage::Preparing);
        job.completeDetailed(false, QStringLiteral("wrong"), QStringLiteral("stale"));
        QVERIFY(job.results() == exporting);
        QTest::qWait(12);
        job.completeDetailed(true, first, QStringLiteral("ignored on success"));
        const auto completed = job.results()[0];
        QCOMPARE(completed.state, PsdBatchFileState::Succeeded);
        QCOMPARE(completed.stage, PsdExportStage::Done);
        QVERIFY(completed.error.isEmpty());
        QVERIFY(completed.outputCommitted);
        QVERIFY(completed.elapsedMs >= 10);
        job.completeDetailed(false, first, QStringLiteral("duplicate"));
        QVERIFY(job.results()[0] == completed);
        QTRY_COMPARE(requests.size(), 2);
        const QString second = requests[1][1].toString();
        const QString error = QStringLiteral("模型 <bad>.json\n缺少 MOC：原始错误");
        job.completeDetailed(false, second, error, PsdExportStage::Preflight);
        QTRY_COMPARE(requests.size(), 3);
        const QString third = requests[2][1].toString();
        job.setStage(third, PsdExportStage::Restoring);
        job.completeDetailed(false, third, QStringLiteral("文件已保存，但恢复预览失败"), PsdExportStage::Restoring, true);
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(finished.size(), 1);
        const auto results = qvariant_cast<PsdBatchResults>(ready[0][0]);
        QVERIFY(results == job.results());
        QVERIFY(!ready[0][1].toBool());
        QCOMPARE(results[1].error, error);
        QCOMPARE(results[1].stage, PsdExportStage::Preflight);
        QVERIFY(!results[1].outputCommitted);
        QCOMPARE(results[2].state, PsdBatchFileState::Failed);
        QCOMPARE(results[2].stage, PsdExportStage::Restoring);
        QVERIFY(results[2].outputCommitted);
        QCOMPARE(finished[0][0].toStringList(), (QStringList{second, third}));
        for (const auto& item : planned) QCOMPARE(item.state, PsdBatchFileState::Pending);
        QCOMPARE(exporting[0].state, PsdBatchFileState::Running);
        QVERIFY(!changed.isEmpty());
        for (const auto& event : changed) {
            const auto item = qvariant_cast<PsdBatchFileResult>(event[1]);
            QCOMPARE(item.index, event[0].toInt());
            QVERIFY(IsPsdBatchResultValid(item));
        }
        job.shutdown();
        QVERIFY(job.results() == results);
        QCoreApplication::processEvents();
        QCOMPARE(ready.size(), 1);
        QCOMPARE(finished.size(), 1);
    }

    void canceledResultsKeepActualCurrentOutcome_data()
    {
        QTest::addColumn<bool>("success");
        QTest::newRow("completed-current") << true;
        QTest::newRow("failed-current") << false;
    }

    void failureStageIdentifiesExportErrorAfterSuccessfulRestore()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        PsdBatchJob job;
        QSignalSpy requests(&job, &PsdBatchJob::requestExport);
        QSignalSpy ready(&job, &PsdBatchJob::resultsReady);
        QVERIFY(job.start({QStringLiteral("/a.json")}, temp.path(), true, false));
        QTRY_COMPARE(requests.size(), 1);
        const QString output = requests[0][1].toString();
        job.setStage(output, PsdExportStage::Exporting);
        job.setStage(output, PsdExportStage::Restoring);
        job.completeDetailed(false, output, QStringLiteral("写入失败，预览已恢复"), PsdExportStage::Exporting);
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(job.results()[0].state, PsdBatchFileState::Failed);
        QCOMPARE(job.results()[0].stage, PsdExportStage::Exporting);
        QCOMPARE(job.results()[0].error, QStringLiteral("写入失败，预览已恢复"));
        QVERIFY(!job.results()[0].outputCommitted);
    }

    void canceledResultsKeepActualCurrentOutcome()
    {
        QFETCH(bool, success);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        PsdBatchJob job;
        QSignalSpy requests(&job, &PsdBatchJob::requestExport);
        QSignalSpy ready(&job, &PsdBatchJob::resultsReady);
        QSignalSpy finished(&job, &PsdBatchJob::finished);
        QVERIFY(job.start({QStringLiteral("/a.json"), QStringLiteral("/b.json"), QStringLiteral("/c.json")}, temp.path(), true, false));
        QTRY_COMPARE(requests.size(), 1);
        job.cancel();
        const auto canceled = job.results();
        QCOMPARE(canceled[0].state, PsdBatchFileState::Running);
        QCOMPARE(canceled[1].state, PsdBatchFileState::NotRun);
        QCOMPARE(canceled[2].state, PsdBatchFileState::NotRun);
        QCOMPARE(ready.size(), 0);
        job.completeDetailed(success, requests[0][1].toString(), QStringLiteral("actual failure"));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(requests.size(), 1);
        QCOMPARE(job.results()[0].state, success ? PsdBatchFileState::Succeeded : PsdBatchFileState::Failed);
        QCOMPARE(job.results()[0].outputCommitted, success);
        QVERIFY(ready[0][1].toBool());
        for (int index = 1; index < 3; ++index) {
            QCOMPARE(job.results()[index].state, PsdBatchFileState::NotRun);
            QVERIFY(!job.results()[index].outputCommitted);
            QVERIFY(!job.results()[index].error.isEmpty());
        }
    }

    void cancellationBeforeDispatchPublishesCompleteCanceledSnapshot()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        PsdBatchJob job;
        QSignalSpy requests(&job, &PsdBatchJob::requestExport);
        QSignalSpy ready(&job, &PsdBatchJob::resultsReady);
        bool observed = false;
        connect(&job, &PsdBatchJob::fileResultChanged, &job, [&](int, const PsdBatchFileResult& result) {
            if (result.state == PsdBatchFileState::Running) job.cancel();
            if (result.state == PsdBatchFileState::NotRun) {
                observed = true;
                for (const auto& item : job.results()) QCOMPARE(item.state, PsdBatchFileState::NotRun);
                QCoreApplication::processEvents();
            }
        });
        QVERIFY(job.start({QStringLiteral("/a.json"), QStringLiteral("/b.json")}, temp.path(), true, false));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(observed);
        QCOMPARE(requests.size(), 0);
        for (const auto& item : qvariant_cast<PsdBatchResults>(ready[0][0])) {
            QCOMPARE(item.state, PsdBatchFileState::NotRun);
            QVERIFY(!item.outputCommitted);
        }
    }

    void shutdownRetainsRecordsAndSuppressesFurtherSignals()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        PsdBatchJob job;
        QSignalSpy requests(&job, &PsdBatchJob::requestExport);
        QSignalSpy changed(&job, &PsdBatchJob::fileResultChanged);
        QSignalSpy ready(&job, &PsdBatchJob::resultsReady);
        QSignalSpy finished(&job, &PsdBatchJob::finished);
        QVERIFY(job.start({QStringLiteral("/a.json"), QStringLiteral("/b.json")}, temp.path(), true, false));
        QTRY_COMPARE(requests.size(), 1);
        job.setStage(requests[0][1].toString(), PsdExportStage::Exporting);
        const int emissions = changed.size();
        job.shutdown();
        const auto records = job.results();
        QCOMPARE(records[0].state, PsdBatchFileState::Failed);
        QCOMPARE(records[0].stage, PsdExportStage::Exporting);
        QVERIFY(!records[0].error.isEmpty());
        QVERIFY(!records[0].outputCommitted);
        QCOMPARE(records[1].state, PsdBatchFileState::NotRun);
        job.completeDetailed(true, requests[0][1].toString(), {});
        job.setStage(requests[0][1].toString(), PsdExportStage::Restoring);
        job.cancel();
        QCoreApplication::processEvents();
        QVERIFY(job.results() == records);
        QCOMPARE(changed.size(), emissions);
        QCOMPARE(ready.size(), 0);
        QCOMPARE(finished.size(), 0);
        QVERIFY(!job.isRunning());
    }

    void completionBeforeShutdownKeepsCommittedOutputWithoutWorkerAck()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        PsdBatchJob job;
        QSignalSpy requests(&job, &PsdBatchJob::requestExport);
        QSignalSpy finished(&job, &PsdBatchJob::finished);
        connect(&job, &PsdBatchJob::fileResultChanged, &job, [&](int, const PsdBatchFileResult& result) {
            if (result.state == PsdBatchFileState::Succeeded) job.shutdown();
        });
        QVERIFY(job.start({QStringLiteral("/a.json"), QStringLiteral("/b.json")}, temp.path(), true, false));
        QTRY_COMPARE(requests.size(), 1);
        job.completeDetailed(true, requests[0][1].toString(), {});
        QCOMPARE(job.results()[0].state, PsdBatchFileState::Succeeded);
        QVERIFY(job.results()[0].outputCommitted);
        QCOMPARE(job.results()[1].state, PsdBatchFileState::NotRun);
        QVERIFY(!job.isRunning());
        QCoreApplication::processEvents();
        QCOMPARE(finished.size(), 0);
        QCOMPARE(requests.size(), 1);
    }

    void shutdownOrDestructionFromRunningNotificationPreventsDispatch_data()
    {
        QTest::addColumn<bool>("destroy");
        QTest::newRow("shutdown") << false;
        QTest::newRow("destroy") << true;
    }

    void shutdownOrDestructionFromRunningNotificationPreventsDispatch()
    {
        QFETCH(bool, destroy);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        auto job = std::make_unique<PsdBatchJob>();
        QPointer<PsdBatchJob> guard(job.get());
        QSignalSpy requests(job.get(), &PsdBatchJob::requestExport);
        bool observed = false;
        connect(job.get(), &PsdBatchJob::fileResultChanged, this, [&](int, const PsdBatchFileResult& result) {
            if (result.state != PsdBatchFileState::Running) return;
            observed = true;
            if (destroy) job.reset();
            else job->shutdown();
        });
        QVERIFY(job->start({QStringLiteral("/a.json")}, temp.path(), true, false));
        QTRY_VERIFY(observed);
        QCoreApplication::processEvents();
        QCOMPARE(requests.size(), 0);
        if (destroy) QVERIFY(guard.isNull());
        else {
            QVERIFY(!job->isRunning());
            QCOMPARE(job->results()[0].state, PsdBatchFileState::NotRun);
        }
    }

    void avoidsExistingAndDuplicateNames()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QFile existing(temp.filePath(QStringLiteral("model.psd")));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.close();
        QFile existingSecond(temp.filePath(QStringLiteral("model #2.psd")));
        QVERIFY(existingSecond.open(QIODevice::WriteOnly));
        existingSecond.close();
        PsdBatchWorker worker;
        worker.setTask({QStringLiteral("/input/model.model3.json"),
                        QStringLiteral("/other/model.model3.json"),
                        QStringLiteral("/input/second.model3.json")}, temp.path(), true, false);
        QSignalSpy requests(&worker, &PsdBatchWorker::requestExport);
        worker.start();
        QCOMPARE(requests.size(), 1);
        QCOMPARE(requests.at(0).at(1).toString(), temp.filePath(QStringLiteral("model #3.psd")));
        worker.onOneExportFinished(true, requests.at(0).at(1).toString());
        QCOMPARE(requests.size(), 2);
        QCOMPARE(requests.at(1).at(1).toString(), temp.filePath(QStringLiteral("model #4.psd")));
        worker.onOneExportFinished(true, requests.at(1).at(1).toString());
        QCOMPARE(requests.size(), 3);
        QCOMPARE(requests.at(2).at(1).toString(), temp.filePath(QStringLiteral("second.psd")));
    }

    void treatsNamesCaseInsensitively()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QFile existing(temp.filePath(QStringLiteral("MODEL.psd")));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.close();
        PsdBatchWorker worker;
        worker.setTask({QStringLiteral("/input/Model.model3.json"),
                        QStringLiteral("/other/model.model3.json")}, temp.path(), true, false);
        QSignalSpy requests(&worker, &PsdBatchWorker::requestExport);
        worker.start();
        QCOMPARE(requests.at(0).at(1).toString(), temp.filePath(QStringLiteral("Model #2.psd")));
        worker.onOneExportFinished(true, requests.at(0).at(1).toString());
        QCOMPARE(requests.at(1).at(1).toString(), temp.filePath(QStringLiteral("model #3.psd")));
    }

    void reservesCanonicalEquivalentExistingFilesAndPlannedNames()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString composed = QString::fromUtf8("é");
        const QString decomposed = composed.normalized(QString::NormalizationForm_D);
        QVERIFY(composed != decomposed);
        const QString existingName = composed.toUpper() + QStringLiteral(".psd");
        const QString secondName = decomposed + QStringLiteral(" #2.psd");
        QFile first(temp.filePath(existingName));
        QFile second(temp.filePath(secondName));
        QVERIFY(first.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QCOMPARE(first.write("original-first"), 14);
        first.close();
        QVERIFY(second.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QCOMPARE(second.write("original-second"), 15);
        second.close();
        PsdBatchWorker worker;
        worker.setTask({QStringLiteral("/input/") + composed + QStringLiteral(".model3.json"),
                        QStringLiteral("/input/") + decomposed + QStringLiteral(".model3.json")}, temp.path(), true, false);
        const auto plan = worker.plannedResults();
        QCOMPARE(plan.size(), 2);
        QCOMPARE(plan[0].outputPath, temp.filePath(composed + QStringLiteral(" #3.psd")));
        QCOMPARE(plan[1].outputPath, temp.filePath(decomposed + QStringLiteral(" #4.psd")));
        for (int index = 0; index < plan.size(); ++index) {
            QVERIFY(!QFileInfo::exists(plan[index].outputPath));
            QFile planned(plan[index].outputPath);
            QVERIFY(planned.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            QCOMPARE(planned.write(QByteArray::number(index)), 1);
        }
        QVERIFY(first.open(QIODevice::ReadOnly));
        QCOMPARE(first.readAll(), QByteArray("original-first"));
        QVERIFY(second.open(QIODevice::ReadOnly));
        QCOMPARE(second.readAll(), QByteArray("original-second"));
        for (int index = 0; index < plan.size(); ++index) {
            QFile planned(plan[index].outputPath);
            QVERIFY(planned.open(QIODevice::ReadOnly));
            QCOMPARE(planned.readAll(), QByteArray::number(index));
        }
    }

    void reservesExistingDirectoriesAndHiddenEntries()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QDir directory(temp.path());
        QVERIFY(directory.mkdir(QStringLiteral("model.psd")));
        QVERIFY(directory.mkdir(QStringLiteral(".model.psd")));
        QFile hidden(temp.filePath(QStringLiteral(".model #2.psd")));
        QVERIFY(hidden.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QCOMPARE(hidden.write("keep hidden"), 11);
        hidden.close();
        PsdBatchWorker worker;
        worker.setTask({QStringLiteral("/input/model.model3.json"), QStringLiteral("/input/.model.model3.json")}, temp.path(), true, false);
        const auto plan = worker.plannedResults();
        QCOMPARE(plan.size(), 2);
        QCOMPARE(plan[0].outputPath, temp.filePath(QStringLiteral("model #2.psd")));
        QCOMPARE(plan[1].outputPath, temp.filePath(QStringLiteral(".model #3.psd")));
        for (const auto& item : plan) QVERIFY(!QFileInfo::exists(item.outputPath));
        QVERIFY(QFileInfo(temp.filePath(QStringLiteral("model.psd"))).isDir());
        QVERIFY(QFileInfo(temp.filePath(QStringLiteral(".model.psd"))).isDir());
        QVERIFY(hidden.open(QIODevice::ReadOnly));
        QCOMPARE(hidden.readAll(), QByteArray("keep hidden"));
    }

    void workerRejectsRepeatedStartAndStaleResults()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        PsdBatchWorker worker;
        worker.setTask({QStringLiteral("/input/first.model3.json"),
                        QStringLiteral("/input/second.model3.json")}, temp.path(), true, false);
        QSignalSpy requests(&worker, &PsdBatchWorker::requestExport);
        QSignalSpy finished(&worker, &PsdBatchWorker::finished);
        worker.onOneExportFinished(false, temp.filePath(QStringLiteral("first.psd")));
        worker.start();
        worker.start();
        QCOMPARE(requests.size(), 1);
        const QString first = requests.at(0).at(1).toString();
        worker.onOneExportFinished(false, QStringLiteral("wrong.psd"));
        QCOMPARE(requests.size(), 1);
        worker.onOneExportFinished(false, first);
        QCOMPARE(requests.size(), 2);
        worker.onOneExportFinished(false, first);
        worker.start();
        QCOMPARE(requests.size(), 2);
        QCOMPARE(finished.size(), 0);
        const QString second = requests.at(1).at(1).toString();
        worker.onOneExportFinished(true, second);
        worker.onOneExportFinished(false, second);
        worker.start();
        worker.cancel();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.at(0).at(0).toStringList(), QStringList{first});
    }

    void workerCancellationWaitsForCurrentResult()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        PsdBatchWorker worker;
        worker.setTask({QStringLiteral("/input/first.model3.json"),
                        QStringLiteral("/input/second.model3.json")}, temp.path(), true, false);
        QSignalSpy requests(&worker, &PsdBatchWorker::requestExport);
        QSignalSpy finished(&worker, &PsdBatchWorker::finished);
        worker.start();
        worker.cancel();
        worker.cancel();
        QCOMPARE(finished.size(), 0);
        worker.onOneExportFinished(true, requests.at(0).at(1).toString());
        QCOMPARE(requests.size(), 1);
        QCOMPARE(finished.size(), 1);
    }

    void jobForwardsTaskAndAcceptsOnlyCurrentResult()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        PsdBatchJob job;
        QSignalSpy requests(&job, &PsdBatchJob::requestExport);
        QSignalSpy finished(&job, &PsdBatchJob::finished);
        const QStringList models{QStringLiteral("/input/first.model3.json"),
                                 QStringLiteral("/input/second.model3.json")};
        QVERIFY(job.start(models, temp.path(), false, true, true, true));
        QVERIFY(!job.start(models, temp.path(), true, false));
        job.complete(false, temp.filePath(QStringLiteral("first.psd")));
        QTRY_COMPARE(requests.size(), 1);
        const auto firstRequest = requests.at(0);
        QCOMPARE(firstRequest.at(0).toString(), models.at(0));
        QCOMPARE(firstRequest.at(2).toBool(), false);
        QCOMPARE(firstRequest.at(3).toBool(), true);
        QCOMPARE(firstRequest.at(4).toBool(), true);
        QCOMPARE(firstRequest.at(5).toBool(), true);
        const QString first = firstRequest.at(1).toString();
        job.complete(false, QStringLiteral("wrong.psd"));
        job.complete(false, first);
        job.complete(false, first);
        QTRY_COMPARE(requests.size(), 2);
        QCOMPARE(finished.size(), 0);
        job.complete(false, first);
        const QString second = requests.at(1).at(1).toString();
        job.complete(true, second);
        job.complete(false, second);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.at(0).at(0).toStringList(), QStringList{first});
        QCOMPARE(finished.at(0).at(1).toBool(), false);
        QVERIFY(!job.isRunning());
        QVERIFY(!job.start(models, temp.path(), true, false));
        job.cancel();
        job.complete(false, second);
        QCoreApplication::processEvents();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(requests.size(), 2);
    }

    void emptyJobFinishesOnce()
    {
        PsdBatchJob job;
        QSignalSpy requests(&job, &PsdBatchJob::requestExport);
        QSignalSpy finished(&job, &PsdBatchJob::finished);
        QVERIFY(job.start({}, QString(), true, false));
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished.at(0).at(0).toStringList().isEmpty());
        QCOMPARE(finished.at(0).at(1).toBool(), false);
        QCOMPARE(requests.size(), 0);
        QVERIFY(!job.isRunning());
        job.shutdown();
        QCoreApplication::processEvents();
        QCOMPARE(finished.size(), 1);
    }

    void immediateCancellationFiltersQueuedRequests()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        PsdBatchJob job;
        QSignalSpy requests(&job, &PsdBatchJob::requestExport);
        QSignalSpy progress(&job, &PsdBatchJob::progress);
        QSignalSpy finished(&job, &PsdBatchJob::finished);
        QVERIFY(job.start({QStringLiteral("/input/first.model3.json")}, temp.path(), true, false));
        job.cancel();
        job.cancel();
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(requests.size(), 0);
        QCOMPARE(progress.size(), 0);
        QCOMPARE(finished.at(0).at(1).toBool(), true);
        QVERIFY(!job.isRunning());
    }

    void cancellationDuringNestedExportWaitsForCompletion()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        PsdBatchJob job;
        QSignalSpy requests(&job, &PsdBatchJob::requestExport);
        QSignalSpy finished(&job, &PsdBatchJob::finished);
        bool exportReturned = false;
        connect(&job, &PsdBatchJob::requestExport, &job,
                [&](const QString&, const QString& output, bool, bool, bool, bool) {
            QEventLoop nested;
            QTimer::singleShot(0, &nested, [&] {
                job.cancel();
                nested.quit();
            });
            nested.exec();
            QCOMPARE(finished.size(), 0);
            QVERIFY(job.isRunning());
            job.complete(true, output);
            exportReturned = true;
        });
        QVERIFY(job.start({QStringLiteral("/input/first.model3.json"),
                           QStringLiteral("/input/second.model3.json")}, temp.path(), true, false));
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(exportReturned);
        QCOMPARE(requests.size(), 1);
        QCOMPARE(finished.at(0).at(1).toBool(), true);
        QVERIFY(!job.isRunning());
    }

    void shutdownBeforeStartAndBeforeEventDelivery()
    {
        PsdBatchJob unopened;
        unopened.shutdown();
        QVERIFY(!unopened.start({}, QString(), true, false));
        QVERIFY(!unopened.isRunning());

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        PsdBatchJob job;
        QSignalSpy requests(&job, &PsdBatchJob::requestExport);
        QSignalSpy progress(&job, &PsdBatchJob::progress);
        QSignalSpy finished(&job, &PsdBatchJob::finished);
        QVERIFY(job.start({QStringLiteral("/input/first.model3.json")}, temp.path(), true, false));
        job.shutdown();
        job.shutdown();
        QVERIFY(!job.isRunning());
        QCoreApplication::processEvents();
        QCOMPARE(requests.size(), 0);
        QCOMPARE(progress.size(), 0);
        QCOMPARE(finished.size(), 0);
    }

    void shutdownWhileAwaitingExportIgnoresLateResults()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        PsdBatchJob job;
        QSignalSpy requests(&job, &PsdBatchJob::requestExport);
        QSignalSpy finished(&job, &PsdBatchJob::finished);
        QVERIFY(job.start({QStringLiteral("/input/first.model3.json"),
                           QStringLiteral("/input/second.model3.json")}, temp.path(), true, false));
        QTRY_COMPARE(requests.size(), 1);
        job.cancel();
        job.shutdown();
        QVERIFY(!job.isRunning());
        job.complete(true, requests.at(0).at(1).toString());
        QCoreApplication::processEvents();
        QCOMPARE(requests.size(), 1);
        QCOMPARE(finished.size(), 0);
    }

    void shutdownFromProgressSuppressesQueuedExport()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        PsdBatchJob job;
        QSignalSpy requests(&job, &PsdBatchJob::requestExport);
        QSignalSpy progress(&job, &PsdBatchJob::progress);
        QSignalSpy finished(&job, &PsdBatchJob::finished);
        connect(&job, &PsdBatchJob::progress, &job, &PsdBatchJob::shutdown);
        QVERIFY(job.start({QStringLiteral("/input/first.model3.json")}, temp.path(), true, false));
        QTRY_COMPARE(progress.size(), 1);
        QVERIFY(!job.isRunning());
        QCoreApplication::processEvents();
        QCOMPARE(requests.size(), 0);
        QCOMPARE(finished.size(), 0);
    }

    void destructionJoinsWithoutProcessingGuiEvents()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        auto immediate = std::make_unique<PsdBatchJob>();
        QSignalSpy immediateRequests(immediate.get(), &PsdBatchJob::requestExport);
        QVERIFY(immediate->start({QStringLiteral("/input/first.model3.json")}, temp.path(), true, false));
        immediate.reset();
        QCoreApplication::processEvents();
        QCOMPARE(immediateRequests.size(), 0);

        auto waiting = std::make_unique<PsdBatchJob>();
        QSignalSpy requests(waiting.get(), &PsdBatchJob::requestExport);
        QSignalSpy finished(waiting.get(), &PsdBatchJob::finished);
        QVERIFY(waiting->start({QStringLiteral("/input/first.model3.json")}, temp.path(), true, false));
        QTRY_COMPARE(requests.size(), 1);
        waiting.reset();
        QCoreApplication::processEvents();
        QCOMPARE(finished.size(), 0);
    }
};

QTEST_GUILESS_MAIN(PsdBatchWorkerTests)
#include "psd_batch_worker_tests.moc"
