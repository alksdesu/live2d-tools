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
        for (const auto& parameter : document.Parameters()) {
            for (float key : parameter.keyValues) QVERIFY(std::isfinite(key));
        }
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
        QCOMPARE(document.DrawOrderGroups().size(), 1);
        const auto& drawOrderGroup = document.DrawOrderGroups().first();
        QVERIFY(drawOrderGroup.objectsValid);
        QCOMPARE(drawOrderGroup.objectCount, 30);
        QCOMPARE(drawOrderGroup.totalObjectCount, 30);
        for (const auto& object : drawOrderGroup.objects) {
            QCOMPARE(object.type, 0);
            QVERIFY(object.index >= 0 && object.index < document.Drawables().size());
            QCOMPARE(object.selfIndex, -1);
        }
        QVERIFY(document.Validate().isEmpty());
        const QString drawableId = document.Drawables().first().id;
        QVERIFY(document.SetDrawableVisible(drawableId, false));
        QVERIFY(document.HiddenIds().contains(drawableId));
        QVERIFY(document.SetDrawableVisible(drawableId, true));
        QVERIFY(!document.HiddenIds().contains(drawableId));
        const QString deformerId = document.Deformers().first().id;
        QVERIFY(document.SetDeformerVisible(deformerId, false));
        QVERIFY(document.HiddenIds().contains(deformerId));
        QVERIFY(document.SetDeformerVisible(deformerId, true));
        QVERIFY(!document.HiddenIds().contains(deformerId));
        document.SetLocked(QStringLiteral("artmesh"), drawableId, true);
        QVERIFY(document.IsLocked(QStringLiteral("artmesh"), drawableId));
        document.ClearLocked();
        QVERIFY(!document.IsLocked(QStringLiteral("artmesh"), drawableId));
    }

    void documentEditHistory()
    {
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
        const auto& drawable = document.Drawables().first();
        const bool beforeVisible = drawable.visible;
        const QString id = drawable.id;
        QVERIFY(document.ExecuteEdit({id, QStringLiteral("artmesh"), live2dstruct::CModelEdit::Visibility,
                                       beforeVisible, !beforeVisible}));
        QVERIFY(document.CanUndoEdit());
        QVERIFY(!document.CanRedoEdit());
        QCOMPARE(document.Drawables().first().visible, !beforeVisible);

        live2dstruct::CModelEdit applied;
        QVERIFY(document.UndoEdit(&applied));
        QCOMPARE(applied.name, id);
        QCOMPARE(applied.field, live2dstruct::CModelEdit::Visibility);
        QCOMPARE(document.Drawables().first().visible, beforeVisible);
        QVERIFY(document.CanRedoEdit());
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.Drawables().first().visible, !beforeVisible);

        const int currentDrawOrder = qRound(document.DrawableDrawOrder(document.Drawables().first()));
        QVERIFY(document.ExecuteEdit({id, QStringLiteral("artmesh"), live2dstruct::CModelEdit::DrawOrder,
                                       currentDrawOrder, currentDrawOrder + 1}));
        QVERIFY(document.ExecuteEdit({id, QStringLiteral("artmesh"), live2dstruct::CModelEdit::DrawOrder,
                                       currentDrawOrder + 1, currentDrawOrder + 2}));
        QVERIFY(document.UndoEdit());
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().first())), currentDrawOrder);
        QVERIFY(document.RedoEdit());
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().first())), currentDrawOrder + 2);
        QVERIFY(!document.ExecuteEdit({id, QStringLiteral("artmesh"), live2dstruct::CModelEdit::Opacity,
                                       0.2f, 1.5f}));
        const auto& parameter = document.Parameters().first();
        const float parameterBefore = parameter.defaultValue;
        const float parameterAfter = qFuzzyCompare(parameterBefore + 1.0f, parameter.maximum + 1.0f)
            ? parameter.minimum : parameter.maximum;
        QVERIFY(document.ExecuteEdit({parameter.id, QStringLiteral("parameter"), live2dstruct::CModelEdit::ParameterValue,
                                       parameterBefore, parameterAfter}));
        QCOMPARE(document.ParameterValue(parameter.id), parameterAfter);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.ParameterValue(parameter.id), parameterBefore);
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.ParameterValue(parameter.id), parameterAfter);
        QVERIFY(!document.ExecuteEdit({parameter.id, QStringLiteral("parameter"), live2dstruct::CModelEdit::ParameterValue,
                                       parameterAfter, parameter.maximum + 1.0f}));
        QVERIFY(!drawable.keyforms.isEmpty());
        const QVector<float> keyformBefore = document.DrawableKeyformPositions(id, 0);
        QVERIFY(keyformBefore.size() == drawable.vertexCount * 2);
        QVector<float> keyformAfter = keyformBefore;
        keyformAfter[0] += 0.125f;
        QVERIFY(document.ExecuteEdit({id, QStringLiteral("artmesh"), live2dstruct::CModelEdit::KeyformPositions,
                                       QVariant::fromValue(keyformBefore), QVariant::fromValue(keyformAfter), 0}));
        QCOMPARE(document.DrawableKeyformPositions(id, 0), keyformAfter);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.DrawableKeyformPositions(id, 0), keyformBefore);
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.DrawableKeyformPositions(id, 0), keyformAfter);
        QVERIFY(!document.ExecuteEdit({id, QStringLiteral("artmesh"), live2dstruct::CModelEdit::KeyformPositions,
                                       QVariant::fromValue(keyformAfter), QVariant::fromValue(QVector<float>(1, 0.0f)), 0}));
        const QVector<quint16> indexBefore = document.DrawableIndices(id);
        QVERIFY(indexBefore.size() >= 3);
        QVector<quint16> indexAfter = indexBefore;
        std::swap(indexAfter[0], indexAfter[1]);
        QVERIFY(document.ExecuteEdit({id, QStringLiteral("artmesh"), live2dstruct::CModelEdit::MeshIndices,
                                       QVariant::fromValue(indexBefore), QVariant::fromValue(indexAfter)}));
        QCOMPARE(document.DrawableIndices(id), indexAfter);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.DrawableIndices(id), indexBefore);
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.DrawableIndices(id), indexAfter);
        document.ClearEditHistory();
        QVERIFY(!document.CanUndoEdit());
        QVERIFY(!document.CanRedoEdit());
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
        source.sourceDigest = QByteArray(32, '\x42');
        source.parameters.insert(QStringLiteral("ParamAngleX"), -12.5f);
        source.visibility.insert(QStringLiteral("artmesh|Body"), false);
        source.locked.insert(QStringLiteral("artmesh|Body"), true);
        source.drawableOpacity.insert(QStringLiteral("Body"), 0.42f);
        source.drawableDrawOrder.insert(QStringLiteral("Body"), 720);
        source.drawableKeyformPositions.insert(QStringLiteral("Body"), {{0.0f, 1.0f, 2.0f, 3.0f}, {4.0f, 5.0f}});
        source.drawableIndices.insert(QStringLiteral("Body"), {0, 1, 2});
        source.viewScale = 1.75f;
        source.viewPosition = QPointF(0.25, -0.5);

        live2dstruct::CModelSession decoded;
        QString error;
        QVERIFY2(live2dstruct::CModelSession::FromJson(source.ToJson(), &decoded, &error), qPrintable(error));
        QCOMPARE(decoded.sourceMoc, source.sourceMoc);
        QCOMPARE(decoded.sourceDigest, source.sourceDigest);
        QCOMPARE(decoded.parameters, source.parameters);
        QCOMPARE(decoded.visibility, source.visibility);
        QCOMPARE(decoded.locked, source.locked);
        QCOMPARE(decoded.drawableOpacity, source.drawableOpacity);
        QCOMPARE(decoded.drawableDrawOrder, source.drawableDrawOrder);
        QCOMPARE(decoded.drawableKeyformPositions, source.drawableKeyformPositions);
        QCOMPARE(decoded.drawableIndices, source.drawableIndices);
        QCOMPARE(decoded.viewScale, source.viewScale);
        QCOMPARE(decoded.viewPosition, source.viewPosition);

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("state.live2d-session.json"));
        const QString mocPath = temp.filePath(QStringLiteral("model.moc3"));
        QFile mocFile(mocPath);
        QVERIFY(mocFile.open(QIODevice::WriteOnly));
        QVERIFY(mocFile.write("digest-test") > 0);
        mocFile.close();
        QCOMPARE(live2dstruct::CModelSession::ComputeSourceDigest(mocPath).size(), 32);
        QVERIFY2(source.Save(path, &error), qPrintable(error));
        live2dstruct::CModelSession loaded;
        QVERIFY2(live2dstruct::CModelSession::Load(path, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.parameters, source.parameters);
        QCOMPARE(loaded.locked, source.locked);
        QCOMPARE(loaded.drawableOpacity, source.drawableOpacity);
        QCOMPARE(loaded.drawableDrawOrder, source.drawableDrawOrder);
        QCOMPARE(loaded.drawableKeyformPositions, source.drawableKeyformPositions);
    }

    void sessionRejectsInvalidValues()
    {
        live2dstruct::CModelSession session;
        QString error;
        QVERIFY(!live2dstruct::CModelSession::FromJson(
            R"({"version":1,"parameters":{"ParamAngleX":"bad"}})", &session, &error));
        QVERIFY(!live2dstruct::CModelSession::FromJson(
            R"({"version":1e100})", &session, &error));
        QVERIFY(!live2dstruct::CModelSession::FromJson(
            R"({"version":1,"parameters":{"ParamAngleX":1e100}})", &session, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!live2dstruct::CModelSession::FromJson(
            R"({"version":1,"drawableOpacity":{"Body":2}})", &session, &error));
        QVERIFY(!live2dstruct::CModelSession::FromJson(
            R"({"version":1,"drawableDrawOrder":{"Body":1.5}})", &session, &error));
        QVERIFY(!live2dstruct::CModelSession::FromJson(
            R"({"version":1,"view":{"scale":0}})", &session, &error));
        QVERIFY(!live2dstruct::CModelSession::FromJson(
            R"({"version":1,"view":{"x":1e100}})", &session, &error));
        QVERIFY(!live2dstruct::CModelSession::FromJson(
            R"({"version":1,"drawableKeyformPositions":{"Body":[[0,"bad"]]}})", &session, &error));
        QVERIFY(!live2dstruct::CModelSession::FromJson(
            R"({"version":1,"drawableKeyformPositions":{"Body":"bad"}})", &session, &error));
        QVERIFY(!live2dstruct::CModelSession::FromJson(
            R"({"version":1,"locked":{"artmesh|Body":"bad"}})", &session, &error));
        QVERIFY(!live2dstruct::CModelSession::FromJson(
            R"({"version":1,"sourceDigest":"bad"})", &session, &error));
        QVERIFY(!live2dstruct::CModelSession::FromJson(
            R"({"version":1,"drawableIndices":{"Body":[0,1]}})", &session, &error));
        QVERIFY(!live2dstruct::CModelSession::FromJson(
            R"({"version":1,"drawableIndices":{"Body":[0,"bad",2]}})", &session, &error));
        QVERIFY(live2dstruct::CModelSession::FromJson(R"({"version":1})", &session, &error));
        QCOMPARE(session.viewScale, 1.0f);
        QVERIFY(session.drawableKeyformPositions.isEmpty());
        QVERIFY(session.locked.isEmpty());
    }

    void sessionSaveFailureLeavesExistingFile()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("state.json"));
        QFile existing(path);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QVERIFY(existing.write("old-state") > 0);
        existing.close();

        live2dstruct::CModelSession session;
        session.sourceMoc = QStringLiteral("/tmp/model.moc3");
        QString error;
        QVERIFY(!session.Save(temp.path(), &error));
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("old-state"));
    }

};

QTEST_GUILESS_MAIN(ModelDataTests)
#include "model_data_tests.moc"
