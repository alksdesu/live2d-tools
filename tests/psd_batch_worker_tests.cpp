#include "ui/psdbatchworker.h"
#include "ui/psdbatchjob.h"
#include <QtTest>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QEventLoop>
#include <memory>

class PsdBatchWorkerTests : public QObject
{
    Q_OBJECT
private slots:
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
