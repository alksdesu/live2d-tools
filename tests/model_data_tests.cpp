#include "opengl/cmeshbounds.h"
#include "live2dstruct/csectiontable.h"
#include <QtTest>
#include <limits>

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
    }

};

QTEST_GUILESS_MAIN(ModelDataTests)
#include "model_data_tests.moc"
