#include "opengl/cmeshbounds.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmodelsession.h"
#include "std/cmodeljson.h"
#include "live2dstruct/csectiontable.h"
#include <QtTest>
#include <QRegularExpression>
#include <limits>
#include <QTemporaryDir>

class ModelDataTests : public QObject
{
    Q_OBJECT
private slots:
    void drawableBounds()
    {
        const float vertices[] = {-1, -1, 1, -1, 1, 1, -1, 1};
        QCOMPARE(MeshBounds::Model(vertices, 4), QRectF(-1, -1, 2, 2));
        const auto identity = [](float x, float y) { return QPointF(x, y); };
        QCOMPARE(MeshBounds::Viewport(vertices, 4, identity, QSize(100, 80)), QRect(0, 0, 100, 80));
        const auto translated = [](float x, float y) { return QPointF(x + 1, y + 1); };
        QCOMPARE(MeshBounds::Viewport(vertices, 4, translated, QSize(100, 80)), QRect(50, 40, 50, 40));
        const auto outside = [](float x, float y) { return QPointF(x + 3, y + 3); };
        QVERIFY(MeshBounds::Viewport(vertices, 4, outside, QSize(100, 80)).isEmpty());
        QVERIFY(MeshBounds::Model(nullptr, 4).isEmpty());
    }

    void fractionalBounds()
    {
        const float vertices[] = {-0.905f, -0.905f, 0.905f, 0.905f};
        const auto identity = [](float x, float y) { return QPointF(x, y); };
        QCOMPARE(MeshBounds::Viewport(vertices, 2, identity, QSize(100, 100)), QRect(4, 4, 92, 92));
        const float invalid[] = {0, std::numeric_limits<float>::quiet_NaN(), 1, 1};
        QVERIFY(MeshBounds::Model(invalid, 2).isEmpty());
    }

    void rejectsInvalidMoc()
    {
        live2dstruct::CReader reader(QStringLiteral("/does/not/exist.moc3"));
        QVERIFY(!reader.IsValid());
        live2dstruct::CSectionTable table(reader);
        QVERIFY(!table.IsValid());
    }

    void parsesSamples_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QSize>("canvas");
        QTest::newRow("Mark") << QStringLiteral("Mark") << QSize(2400, 3000);
        QTest::newRow("Natori") << QStringLiteral("Natori") << QSize(4000, 7000);
        QTest::newRow("Rice") << QStringLiteral("Rice") << QSize(4500, 3000);
        QTest::newRow("Wanko") << QStringLiteral("Wanko") << QSize(1200, 1200);
    }

    void parsesSamples()
    {
        QFETCH(QString, name);
        QFETCH(QSize, canvas);
        live2dstruct::CReader reader(QString(TEST_RESOURCES) + "/" + name + "/" + name + ".moc3");
        live2dstruct::CSectionTable table(reader);
        QVERIFY2(table.IsValid(), qPrintable(reader.Error()));
        QVERIFY(table.count_info->art_meshes > 0);
        QCOMPARE(table.art_meshes.size(), table.count_info->art_meshes);
        QCOMPARE(QSize(qRound(table.canvas_info->width), qRound(table.canvas_info->height)), canvas);
        for (const auto* info : table.glue_info) QVERIFY(info->position_indices >= 0);
        live2dstruct::CModelDocument document;
        QString documentError;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/" + name + "/" + name + ".moc3", &documentError), qPrintable(documentError));
        QVERIFY2(document.Validate().isEmpty(), qPrintable(name));
    }

    void documentSnapshot()
    {
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
        QCOMPARE(document.CanvasPixels(), QSize(2400, 3000));
        QCOMPARE(document.Parts().size(), 17);
        QCOMPARE(document.Deformers().size(), 28);
        QCOMPARE(document.Drawables().size(), 30);
        QVERIFY(document.Validate().isEmpty());
    }

    void modelJsonCanBeReused()
    {
        CModelJson model;
        const QString path = QString(TEST_RESOURCES) + "/Mark/Mark.model3.json";
        model.Parse(path);
        QVERIFY(model.IsOpen());
        const int textures = model.m_Textures.size();
        const int motions = model.m_Motions.size();
        model.Parse(path);
        QCOMPARE(model.m_Textures.size(), textures);
        QCOMPARE(model.m_Motions.size(), motions);
        QVERIFY(!model.getAllZipFiles().contains(QRegularExpression("/(null|$)")));
    }

    void sessionRoundTrip()
    {
        live2dstruct::CModelSession source;
        source.sourceMoc = QStringLiteral("/tmp/Model.moc3");
        source.parameters.insert(QStringLiteral("ParamAngleX"), -12.5f);
        source.visibility.insert(QStringLiteral("artmesh|Body"), false);
        source.viewScale = 1.75f;
        source.viewPosition = QPointF(0.25, -0.5);

        live2dstruct::CModelSession decoded;
        QString error;
        QVERIFY2(live2dstruct::CModelSession::FromJson(source.ToJson(), &decoded, &error), qPrintable(error));
        QCOMPARE(decoded.sourceMoc, source.sourceMoc);
        QCOMPARE(decoded.parameters, source.parameters);
        QCOMPARE(decoded.visibility, source.visibility);
        QCOMPARE(decoded.viewScale, source.viewScale);
        QCOMPARE(decoded.viewPosition, source.viewPosition);

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("state.live2d-session.json"));
        QVERIFY2(source.Save(path, &error), qPrintable(error));
        live2dstruct::CModelSession loaded;
        QVERIFY2(live2dstruct::CModelSession::Load(path, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.parameters, source.parameters);
    }

};

QTEST_GUILESS_MAIN(ModelDataTests)
#include "model_data_tests.moc"
