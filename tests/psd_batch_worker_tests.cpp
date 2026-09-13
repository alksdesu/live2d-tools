#include "ui/psdbatchworker.h"
#include <QtTest>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

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
};

QTEST_GUILESS_MAIN(PsdBatchWorkerTests)
#include "psd_batch_worker_tests.moc"
