#include "psd/cpsdlist.h"
#include "psd/psdSamples/Exporter/PsdExportDocument.h"
#include <QtTest>
#include <QTemporaryDir>

class PsdTests : public QObject
{
    Q_OBJECT
private slots:
    void capacityFailurePreservesDestination_data()
    {
        QTest::addColumn<int>("type");
        QTest::newRow("layer") << 0;
        QTest::newRow("group-start") << 1;
        QTest::newRow("group-end") << 2;
    }

    void capacityFailurePreservesDestination()
    {
        QFETCH(int, type);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("existing.psd");
        QFile existing(path);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("original"), 8);
        existing.close();

        CPsdList writer;
        writer.SetPsd(path, 2, 2);
        for (unsigned int i = 0; i < psd::ExportDocument::MAX_LAYER_COUNT / 2; ++i) {
            writer.AddGroupStart();
            writer.AddGroupEnd("group");
        }
        QImage image(2, 2, QImage::Format_ARGB32);
        image.fill(Qt::red);
        if (type == 0) writer.AddLayer("overflow", image, QRect(0, 0, 2, 2));
        else if (type == 1) writer.AddGroupStart();
        else writer.AddGroupEnd("overflow");
        QVERIFY(!writer.CreatePsd());
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("original"));
        existing.close();

        writer.SetPsd(path, 2, 2);
        writer.AddLayer(QString::fromUtf8("图层"), image, QRect(0, 0, 2, 2));
        QVERIFY(writer.CreatePsd());
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.read(6), QByteArray::fromHex("384250530001"));
    }

    void invalidResetCannotWritePreviousDocument()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        CPsdList writer;
        writer.SetPsd(temp.filePath("first.psd"), 2, 2);
        const QString invalidPath = temp.filePath("invalid.psd");
        writer.SetPsd(invalidPath, 0, 2);
        QVERIFY(!writer.CreatePsd());
        QVERIFY(!QFile::exists(invalidPath));
    }
};

QTEST_GUILESS_MAIN(PsdTests)
#include "psd_tests.moc"
