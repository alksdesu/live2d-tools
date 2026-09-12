#include "cmo3/ccaffarchive.h"
#include "cmo3/ccmo3builder.h"
#include <QtTest>
#include <QFileInfo>
#include <QTemporaryDir>

class Cmo3Tests : public QObject
{
    Q_OBJECT
private slots:
    void buildAndReadArchive()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        cmo3::CCmo3Builder::Options options;
        options.mocPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
        options.outPath = temp.filePath(QStringLiteral("Wanko.cmo3"));
        options.cdiPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.cdi3.json";
        options.physicsPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.physics3.json";
        QString error;
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(options.outPath));

        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const cmo3::CCaffArchive::Entry* mainXml = archive.FindByPath(QStringLiteral("main.xml"));
        QVERIFY(mainXml);
        QVERIFY(mainXml->content.contains("CModel"));
        int pngCount = 0;
        for (const auto& entry : archive.Entries()) if (entry.path.endsWith(QStringLiteral(".png"))) ++pngCount;
        QVERIFY(pngCount > 0);
    }
};

QTEST_GUILESS_MAIN(Cmo3Tests)
#include "cmo3_tests.moc"
