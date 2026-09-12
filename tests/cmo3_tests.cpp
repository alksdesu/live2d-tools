#include "cmo3/ccaffarchive.h"
#include "cmo3/ccmo3builder.h"
#include "cmo3/cxsxmlwriter.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/util/cfilereader.h"
#include <QtTest>
#include <QFileInfo>
#include <QTemporaryDir>
#include <algorithm>

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

    void buildUsesKeyformOverride()
    {
        const QString mocPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(mocPath, &error), qPrintable(error));
        const auto meshIt = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
                                         [](const live2dstruct::CModelDrawable& mesh){ return !mesh.keyforms.isEmpty(); });
        QVERIFY(meshIt != document.Drawables().cend());
        const QString meshId = meshIt->id;
        const QVector<float> original = document.DrawableKeyformPositions(meshId, 0);
        QVERIFY(!original.isEmpty());
        QVector<float> edited = original;

        live2dstruct::CReader reader(mocPath);
        live2dstruct::CSectionTable table(reader);
        QVERIFY(table.IsValid());
        float expected = 123.456f;
        if (meshIt->parentDeformerIndex < 0) {
            expected = (123.456f + table.canvas_info->origin_x) * table.canvas_info->pixels_per_unit;
            edited[0] = 123.456f;
        } else {
            edited[0] = expected;
        }
        QVERIFY(document.SetDrawableKeyformPositions(meshId, 0, edited));

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        cmo3::CCmo3Builder::Options options;
        options.mocPath = mocPath;
        options.outPath = temp.filePath(QStringLiteral("Wanko-override.cmo3"));
        options.drawableKeyformPositionOverrides = document.DrawableKeyformPositionOverrides();
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* mainXml = archive.FindByPath(QStringLiteral("main.xml"));
        QVERIFY(mainXml);
        QVERIFY(mainXml->content.contains(cmo3::CXsXmlWriter::FormatFloat(expected).toUtf8()));
    }
};

QTEST_GUILESS_MAIN(Cmo3Tests)
#include "cmo3_tests.moc"
