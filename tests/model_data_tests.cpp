#include "opengl/cmeshbounds.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/cmotiontimeline.h"
#include <QJsonDocument>
#include <QJsonArray>
#include "std/cmodeljson.h"
#include "live2dstruct/csectiontable.h"
#include <QtTest>
#include <QRegularExpression>
#include <limits>
#include <QTemporaryDir>
#include <algorithm>

class ModelDataTests : public QObject
{
    Q_OBJECT
private slots:
    void emptyMasksRetainInversionAcrossHistoryAndSessions_data()
    {
        QTest::addColumn<bool>("sourceInverted");
        QTest::newRow("source-inverted") << true;
        QTest::newRow("user-inverted") << false;
    }

    void emptyMasksRetainInversionAcrossHistoryAndSessions()
    {
        QFETCH(bool, sourceInverted);
        using namespace live2dstruct;
        CModelDocument document;
        QString error;
        const QString path = QString(TEST_RESOURCES) + QStringLiteral("/Rice/Rice.moc3");
        QVERIFY2(document.Load(path, &error), qPrintable(error));
        const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [sourceInverted](const CModelDrawable& drawable) {
                return !drawable.maskIndices.isEmpty() && ((drawable.flags & 0x08) != 0) == sourceInverted;
            });
        QVERIFY(mesh != document.Drawables().cend());
        const QString id = mesh->id;
        const QVector<int> masks = document.DrawableMaskIndices(id);
        QCOMPARE(document.DrawableMaskInverted(id), sourceInverted);
        if (sourceInverted) {
            QVERIFY(document.ExecuteEdit({id, "artmesh", CModelEdit::MaskInverted, true, false, -1, false}));
            QVERIFY(document.UndoEdit());
        } else {
            QVERIFY(document.ExecuteEdit({id, "artmesh", CModelEdit::MaskInverted, false, true, -1, false}));
        }
        QVERIFY(document.DrawableMaskInvertedOverrides().value(id));
        document.ClearEditHistory();
        const CModelEdit clearMasks{id, "artmesh", CModelEdit::MaskIndices,
            QVariant::fromValue(masks), QVariant::fromValue(QVector<int>()), -1, false};
        QVERIFY(document.ExecuteEdit(clearMasks));
        QVERIFY(document.DrawableMaskIndices(id).isEmpty());
        QVERIFY(document.DrawableMaskInverted(id));
        QVERIFY(document.Validate().isEmpty());
        CModelEdit undone;
        QVERIFY(document.UndoEdit(&undone));
        QCOMPARE(undone.field, CModelEdit::MaskIndices);
        QCOMPARE(document.DrawableMaskIndices(id), masks);
        QVERIFY(document.DrawableMaskInverted(id));
        QVERIFY(!document.CanUndoEdit());
        QVERIFY(document.RedoEdit());
        QVERIFY(document.DrawableMaskIndices(id).isEmpty());
        QVERIFY(document.DrawableMaskInverted(id));
        QVERIFY(!document.CanRedoEdit());
        QVERIFY(document.Validate().isEmpty());

        CModelSession session;
        session.sourceMoc = path;
        session.drawableMaskIndices = document.DrawableMaskIndexOverrides();
        session.drawableMaskInverted = document.DrawableMaskInvertedOverrides();
        CModelSession decoded;
        QVERIFY2(CModelSession::FromJson(session.ToJson(), &decoded, &error), qPrintable(error));
        QVERIFY(decoded.drawableMaskIndices.contains(id));
        QVERIFY(decoded.drawableMaskIndices.value(id).isEmpty());
        QVERIFY(decoded.drawableMaskInverted.contains(id));
        QVERIFY(decoded.drawableMaskInverted.value(id));
        CModelDocument restored;
        QVERIFY2(restored.Load(decoded.sourceMoc, &error), qPrintable(error));
        for (auto it = decoded.drawableMaskIndices.cbegin(); it != decoded.drawableMaskIndices.cend(); ++it)
            QVERIFY(restored.SetDrawableMaskIndices(it.key(), it.value()));
        for (auto it = decoded.drawableMaskInverted.cbegin(); it != decoded.drawableMaskInverted.cend(); ++it)
            QVERIFY(restored.SetDrawableMaskInverted(it.key(), it.value()));
        QVERIFY(restored.DrawableMaskIndices(id).isEmpty());
        QVERIFY(restored.DrawableMaskInverted(id));
        QVERIFY(restored.Validate().isEmpty());
        QVERIFY(restored.ExecuteEdit({id, "artmesh", CModelEdit::MaskIndices,
            QVariant::fromValue(QVector<int>()), QVariant::fromValue(masks), -1, false}));
        QCOMPARE(restored.DrawableMaskIndices(id), masks);
        QVERIFY(restored.DrawableMaskInverted(id));
        QVERIFY(restored.UndoEdit());
        QVERIFY(restored.DrawableMaskIndices(id).isEmpty());
        QVERIFY(restored.DrawableMaskInverted(id));
        QVERIFY(!restored.CanUndoEdit());
        QVERIFY(restored.RedoEdit());
        QCOMPARE(restored.DrawableMaskIndices(id), masks);
        QVERIFY(restored.DrawableMaskInverted(id));
        QVERIFY(restored.Validate().isEmpty());
    }

    void sparseKeyformOverridesPreserveOtherForms()
    {
        live2dstruct::CModelDocument document;
        QString error;
        const QString mocPath = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
        QVERIFY2(document.Load(mocPath, &error), qPrintable(error));
        const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [](const auto& drawable) { return drawable.vertexCount > 0 && drawable.keyforms.size() >= 3; });
        QVERIFY(mesh != document.Drawables().cend());
        const QString id = mesh->id;
        const int editedIndex = mesh->keyforms.size() / 2;
        const QVector<float> original = mesh->keyforms[editedIndex].positions;
        QVERIFY(!original.isEmpty());
        QVector<float> edited = original;
        edited[0] += 0.25f;
        const live2dstruct::CModelEdit command{id, QStringLiteral("artmesh"),
            live2dstruct::CModelEdit::KeyformPositions, QVariant::fromValue(original),
            QVariant::fromValue(edited), editedIndex, false};
        QVERIFY(document.ExecuteEdit(command));
        for (int index = 0; index < mesh->keyforms.size(); ++index)
            QCOMPARE(document.DrawableKeyformPositions(id, index),
                     index == editedIndex ? edited : mesh->keyforms[index].positions);
        QVERIFY(document.UndoEdit());
        for (int index = 0; index < mesh->keyforms.size(); ++index)
            QCOMPARE(document.DrawableKeyformPositions(id, index), mesh->keyforms[index].positions);
        QVERIFY(!document.CanUndoEdit());
        QVERIFY(document.RedoEdit());
        QVERIFY(!document.CanRedoEdit());

        const auto overrides = document.DrawableKeyformPositionOverrides().value(id);
        QCOMPARE(overrides.size(), mesh->keyforms.size());
        for (int index = 0; index < overrides.size(); ++index)
            QCOMPARE(overrides[index], index == editedIndex ? edited : QVector<float>());
        live2dstruct::CModelSession session;
        session.sourceMoc = mocPath;
        session.drawableKeyformPositions = document.DrawableKeyformPositionOverrides();
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("keyforms.live2d-session.json");
        QVERIFY2(session.Save(path, &error), qPrintable(error));
        live2dstruct::CModelSession restoredSession;
        QVERIFY2(live2dstruct::CModelSession::Load(path, &restoredSession, &error), qPrintable(error));
        QCOMPARE(restoredSession.drawableKeyformPositions, session.drawableKeyformPositions);
        live2dstruct::CModelDocument restored;
        QVERIFY2(restored.Load(restoredSession.sourceMoc, &error), qPrintable(error));
        for (auto it = restoredSession.drawableKeyformPositions.cbegin();
             it != restoredSession.drawableKeyformPositions.cend(); ++it) {
            for (int index = 0; index < it->size(); ++index) {
                if (it->at(index).isEmpty()) continue;
                QVERIFY(restored.SetDrawableKeyformPositions(it.key(), index, it->at(index)));
            }
        }
        QVERIFY(restored.Validate().isEmpty());
        for (int index = 0; index < mesh->keyforms.size(); ++index)
            QCOMPARE(restored.DrawableKeyformPositions(id, index),
                     index == editedIndex ? edited : mesh->keyforms[index].positions);
        QVERIFY(restored.DrawableKeyformPositions(id, -1).isEmpty());
        QVERIFY(restored.DrawableKeyformPositions(id, mesh->keyforms.size()).isEmpty());
        QVERIFY(restored.DrawableKeyformPositions(QStringLiteral("missing"), editedIndex).isEmpty());
    }

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

    void failedLoadPreservesDocument()
    {
        live2dstruct::CModelDocument document;
        QString error;
        const QString mocPath = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
        QVERIFY2(document.Load(mocPath, &error), qPrintable(error));
        const QString drawableId = document.Drawables().first().id;
        document.SetLocked(QStringLiteral("artmesh"), drawableId, true);
        const bool beforeVisible = document.Drawables().first().visible;
        QVERIFY(document.ExecuteEdit({drawableId, QStringLiteral("artmesh"),
                                      live2dstruct::CModelEdit::Visibility,
                                      beforeVisible, !beforeVisible}));
        const int partCount = document.Parts().size();
        const int drawableCount = document.Drawables().size();
        const QString sourcePath = document.SourcePath();
        const bool locked = document.IsLocked(QStringLiteral("artmesh"), drawableId);
        QVERIFY(!document.Load(QStringLiteral("/does/not/exist.moc3"), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(document.IsLoaded());
        QCOMPARE(document.SourcePath(), sourcePath);
        QCOMPARE(document.Parts().size(), partCount);
        QCOMPARE(document.Drawables().size(), drawableCount);
        QCOMPARE(document.Drawables().first().visible, !beforeVisible);
        QCOMPARE(document.IsLocked(QStringLiteral("artmesh"), drawableId), locked);
        QVERIFY(document.CanUndoEdit());
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

    void validatesRuntimeNumericState()
    {
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));

        auto& drawables = const_cast<QVector<live2dstruct::CModelDrawable>&>(document.Drawables());
        const float originalDrawOrder = drawables.first().drawOrder;
        drawables.first().drawOrder = std::numeric_limits<float>::quiet_NaN();
        const auto drawOrderDiagnostics = document.Validate();
        QVERIFY(std::any_of(drawOrderDiagnostics.cbegin(), drawOrderDiagnostics.cend(),
                            [](const live2dstruct::CModelDiagnostic& diagnostic){
                                return diagnostic.code == QStringLiteral("draw_order_nan");
                            }));
        drawables.first().drawOrder = originalDrawOrder;

        auto& deformers = const_cast<QVector<live2dstruct::CModelDeformer>&>(document.Deformers());
        const auto warpIt = std::find_if(deformers.cbegin(), deformers.cend(),
                                         [](const live2dstruct::CModelDeformer& deformer){
                                             return deformer.type == live2dstruct::CModelDeformer::Warp;
                                         });
        QVERIFY(warpIt != deformers.cend());
        const int warpIndex = static_cast<int>(warpIt - deformers.cbegin());
        const int originalRows = deformers[warpIndex].rows;
        deformers[warpIndex].rows = -1;
        const auto warpDiagnostics = document.Validate();
        QVERIFY(std::any_of(warpDiagnostics.cbegin(), warpDiagnostics.cend(),
                            [](const live2dstruct::CModelDiagnostic& diagnostic){
                                return diagnostic.code == QStringLiteral("warp_grid");
                            }));
        deformers[warpIndex].rows = originalRows;

        const float originalAngle = deformers[warpIndex].baseAngle;
        deformers[warpIndex].baseAngle = std::numeric_limits<float>::quiet_NaN();
        const auto angleDiagnostics = document.Validate();
        QVERIFY(std::any_of(angleDiagnostics.cbegin(), angleDiagnostics.cend(),
                            [](const live2dstruct::CModelDiagnostic& diagnostic){
                                return diagnostic.code == QStringLiteral("deformer_angle");
                            }));
        deformers[warpIndex].baseAngle = originalAngle;
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
        const auto metadataBefore = document.ParameterMetadata(parameter.id);
        auto metadataAfter = metadataBefore;
        metadataAfter.displayName = QStringLiteral("测试参数");
        metadataAfter.decimalPlaces = qMin(metadataBefore.decimalPlaces + 1, 9);
        const auto metadataVariant = [](const live2dstruct::CModelParameterMetadata& metadata) {
            QVariantMap map;
            map.insert(QStringLiteral("displayName"), metadata.displayName);
            map.insert(QStringLiteral("minimum"), metadata.minimum);
            map.insert(QStringLiteral("maximum"), metadata.maximum);
            map.insert(QStringLiteral("defaultValue"), metadata.defaultValue);
            map.insert(QStringLiteral("decimalPlaces"), metadata.decimalPlaces);
            map.insert(QStringLiteral("repeat"), metadata.repeat);
            return map;
        };
        QVERIFY(document.ExecuteEdit({parameter.id, QStringLiteral("parameter"), live2dstruct::CModelEdit::ParameterMetadata,
                                      metadataVariant(metadataBefore), metadataVariant(metadataAfter)}));
        QCOMPARE(document.ParameterMetadata(parameter.id).displayName, QStringLiteral("测试参数"));
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.ParameterMetadata(parameter.id).displayName, metadataBefore.displayName);
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.ParameterMetadata(parameter.id).displayName, QStringLiteral("测试参数"));
        const QString secondParameterId = document.Parameters().at(1).id;
        const QMap<QString, float> pairBefore{{parameter.id, document.ParameterValue(parameter.id)},
                                              {secondParameterId, document.ParameterValue(secondParameterId)}};
        const QMap<QString, float> pairAfter{{parameter.id, parameter.minimum},
                                             {secondParameterId, document.Parameters().at(1).maximum}};
        QVERIFY(document.ExecuteEdit({QStringLiteral("xy"), QStringLiteral("parameters"),
                                      live2dstruct::CModelEdit::ParameterValues,
                                      QVariant::fromValue(pairBefore), QVariant::fromValue(pairAfter), -1, false}));
        QCOMPARE(document.ParameterValue(parameter.id), pairAfter.value(parameter.id));
        QCOMPARE(document.ParameterValue(secondParameterId), pairAfter.value(secondParameterId));
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.ParameterValue(parameter.id), pairBefore.value(parameter.id));
        QCOMPARE(document.ParameterValue(secondParameterId), pairBefore.value(secondParameterId));
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.ParameterValue(parameter.id), pairAfter.value(parameter.id));
        QCOMPARE(document.ParameterValue(secondParameterId), pairAfter.value(secondParameterId));
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
        int selfIndex = -1;
        for (int i = 0; i < document.Drawables().size(); ++i) if (document.Drawables()[i].id == id) { selfIndex = i; break; }
        QVERIFY(selfIndex >= 0);
        const int maskIndex = selfIndex == 0 ? 1 : 0;
        const QVector<int> masksBefore = document.DrawableMaskIndices(id);
        const QVector<int> masksAfter = {maskIndex};
        QVERIFY(document.SetDrawableMaskIndices(id, masksAfter));
        QCOMPARE(document.DrawableMaskIndices(id), masksAfter);
        QVERIFY(document.ExecuteEdit({id, QStringLiteral("artmesh"), live2dstruct::CModelEdit::MaskIndices,
                                       QVariant::fromValue(masksAfter), QVariant::fromValue(masksBefore)}));
        QCOMPARE(document.DrawableMaskIndices(id), masksBefore);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.DrawableMaskIndices(id), masksAfter);
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.DrawableMaskIndices(id), masksBefore);
        QVERIFY(!document.SetDrawableMaskIndices(id, {0, 0}));
        QVERIFY(!document.SetDrawableMaskIndices(id, {selfIndex}));
        QVERIFY(document.SetDrawableMaskIndices(id, masksAfter));
        QVERIFY(document.SetDrawableMaskInverted(id, true));
        QVERIFY(document.DrawableMaskInverted(id));
        QVERIFY(document.SetDrawableMaskInverted(id, false));
        QVERIFY(!document.DrawableMaskInverted(id));
        QVERIFY(document.SetDrawableMaskIndices(id, {}));
        QVERIFY(document.SetDrawableMaskInverted(id, true));
        QVERIFY(document.DrawableMaskInverted(id));
        QVERIFY(document.SetDrawableMaskIndices(id, masksAfter));
        QVERIFY(document.DrawableMaskInverted(id));
        QVERIFY(!document.SetDrawableMaskInverted(QStringLiteral("missing-mesh"), true));
        int deformerIndex = -1;
        int parentDeformerIndex = -1;
        for (int i = 0; i < document.Deformers().size(); ++i) {
            for (int j = 0; j < document.Deformers().size(); ++j) {
                if (i != j && document.Deformers()[i].parentPartIndex == document.Deformers()[j].parentPartIndex &&
                    document.DeformerParentIndex(document.Deformers()[i].id) != j) {
                    deformerIndex = i;
                    parentDeformerIndex = j;
                    break;
                }
            }
            if (deformerIndex >= 0) break;
        }
        QVERIFY(deformerIndex >= 0);
        const QString deformerId = document.Deformers()[deformerIndex].id;
        const int originalParent = document.DeformerParentIndex(deformerId);
        QVERIFY(document.SetDeformerParentIndex(deformerId, parentDeformerIndex));
        QCOMPARE(document.DeformerParentIndex(deformerId), parentDeformerIndex);
        QVERIFY(!document.SetDeformerParentIndex(deformerId, deformerIndex));
        const QString secondDeformerId = document.Deformers()[parentDeformerIndex].id;
        const int secondOriginalParent = document.DeformerParentIndex(secondDeformerId);
        QVERIFY(!document.SetDeformerParentIndices({{deformerId, parentDeformerIndex},
                                                    {secondDeformerId, deformerIndex}}));
        QCOMPARE(document.DeformerParentIndex(deformerId), parentDeformerIndex);
        QCOMPARE(document.DeformerParentIndex(secondDeformerId), secondOriginalParent);
        QVERIFY(document.SetDeformerParentIndices({{deformerId, -1}, {secondDeformerId, deformerIndex}}));
        QCOMPARE(document.DeformerParentIndex(deformerId), -1);
        QCOMPARE(document.DeformerParentIndex(secondDeformerId), deformerIndex);
        QVERIFY(document.SetDeformerParentIndices({{secondDeformerId, secondOriginalParent},
                                                   {deformerId, parentDeformerIndex}}));
        QVERIFY(document.ExecuteEdit({deformerId, QStringLiteral("warp"), live2dstruct::CModelEdit::DeformerParent,
                                       parentDeformerIndex, originalParent}));
        QCOMPARE(document.DeformerParentIndex(deformerId), originalParent);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.DeformerParentIndex(deformerId), parentDeformerIndex);
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.DeformerParentIndex(deformerId), originalParent);
        auto rotationIt = std::find_if(document.Deformers().cbegin(), document.Deformers().cend(),
                                       [](const live2dstruct::CModelDeformer& deformer){
                                           return deformer.type == live2dstruct::CModelDeformer::Rotation &&
                                                  !deformer.rotationKeyforms.isEmpty();
                                       });
        QVERIFY(rotationIt != document.Deformers().cend());
        const QString rotationId = rotationIt->id;
        const QVector<live2dstruct::CModelRotationKeyform> rotationBefore = document.DeformerRotationKeyforms(rotationId);
        QVector<live2dstruct::CModelRotationKeyform> rotationAfter = rotationBefore;
        rotationAfter[0].angle += 5.0f;
        QVERIFY(document.ExecuteEdit({rotationId, QStringLiteral("rotation"), live2dstruct::CModelEdit::RotationKeyforms,
                                       QVariant::fromValue(rotationBefore), QVariant::fromValue(rotationAfter)}));
        QCOMPARE(document.DeformerRotationKeyforms(rotationId), rotationAfter);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.DeformerRotationKeyforms(rotationId), rotationBefore);
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.DeformerRotationKeyforms(rotationId), rotationAfter);
        auto warpIt = std::find_if(document.Deformers().cbegin(), document.Deformers().cend(),
                                   [](const live2dstruct::CModelDeformer& deformer){
                                       return deformer.type == live2dstruct::CModelDeformer::Warp &&
                                              !deformer.warpKeyforms.isEmpty() &&
                                              !deformer.warpKeyforms.first().positions.isEmpty();
                                   });
        QVERIFY(warpIt != document.Deformers().cend());
        const QString warpId = warpIt->id;
        const QVector<live2dstruct::CModelWarpKeyform> warpBefore = document.DeformerWarpKeyforms(warpId);
        QVector<live2dstruct::CModelWarpKeyform> warpAfter = warpBefore;
        warpAfter[0].positions[0] += 3.0f;
        QVERIFY(document.ExecuteEdit({warpId, QStringLiteral("warp"), live2dstruct::CModelEdit::WarpKeyforms,
                                      QVariant::fromValue(warpBefore), QVariant::fromValue(warpAfter)}));
        QCOMPARE(document.DeformerWarpKeyforms(warpId), warpAfter);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.DeformerWarpKeyforms(warpId), warpBefore);
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.DeformerWarpKeyforms(warpId), warpAfter);
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

        CModelJson mark;
        mark.Parse(QString(TEST_RESOURCES) + "/Mark/Mark.model3.json");
        QVERIFY(!mark.getAllZipFiles().filter(QRegularExpression("Mark\\.userdata3\\.json$")).isEmpty());
        CModelJson natori;
        natori.Parse(QString(TEST_RESOURCES) + "/Natori/Natori.model3.json");
        QVERIFY(!natori.getAllZipFiles().filter(QRegularExpression("Natori\\.pose3\\.json$")).isEmpty());
    }

    void sessionPreservesAnimationDrafts()
    {
        QTemporaryDir temp;
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("ParamA", 5, 0.5));
        live2dstruct::CModelSession session;
        QString error;
        QVERIFY(timeline.SaveState(&session.animation, true, 3, &error));
        const QString path = temp.filePath("single.session.json");
        QVERIFY2(session.Save(path, &error), qPrintable(error));
        live2dstruct::CModelSession restored;
        QVERIFY(live2dstruct::CModelSession::Load(path, &restored, &error));
        CMotionTimeline loaded;
        float playhead = -1;
        QVERIFY(loaded.RestoreState(restored.animation, &playhead, &error));
        QCOMPARE(loaded.PointCount(), 1);
        QCOMPARE(playhead, 3.0f);
        QVERIFY(loaded.Loop());
        QCOMPARE(loaded.Curves(), timeline.Curves());
        timeline.Clear();
        QVERIFY(timeline.SaveState(&session.animation, false, 0, &error));
        QVERIFY(session.Save(path, &error));
        QVERIFY(live2dstruct::CModelSession::Load(path, &restored, &error));
        QVERIFY(loaded.RestoreState(restored.animation, &playhead, &error));
        QVERIFY(!loaded.HasCurves());
        QCOMPARE(playhead, 0.0f);
        QVERIFY(live2dstruct::CModelSession::FromJson(R"({"version":1})", &restored, &error));
        QVERIFY(restored.animation.isEmpty());
    }

    void rejectsDamagedAnimationWithoutReplacingSession()
    {
        CMotionTimeline timeline;
        QVERIFY(timeline.AddPoint("ParamA", 0, 0.5));
        QJsonObject valid;
        QVERIFY(timeline.SaveState(&valid, false, 0));
        QTemporaryDir temp;
        const QString path = temp.filePath("existing.session.json");
        live2dstruct::CModelSession current;
        current.parameters.insert("Keep", 42);
        QVERIFY(current.Save(path));
        QFile beforeFile(path);
        QVERIFY(beforeFile.open(QIODevice::ReadOnly));
        const QByteArray before = beforeFile.readAll();
        beforeFile.close();
        const QVector<QPair<QString, QJsonValue>> badFields{
            {"version", 2}, {"playhead", "invalid"}, {"playhead", -1}, {"playhead", 5},
            {"motion", QJsonArray{}},
            {"motion", QJsonObject{{"Version", 3}, {"Curves", QJsonArray{QJsonObject{
                {"Target", "Parameter"}, {"Id", "A"}, {"Segments", QJsonArray{"bad", 0}}}}}}}
        };
        QString error;
        for (const auto& field : badFields) {
            auto snapshot = valid;
            snapshot.insert(field.first, field.second);
            const QJsonObject root{{"version", 1}, {"animation", snapshot}};
            QVERIFY(!live2dstruct::CModelSession::FromJson(QJsonDocument(root).toJson(), &current, &error));
            QCOMPARE(current.parameters.value("Keep"), 42.0f);
            live2dstruct::CModelSession invalid;
            invalid.animation = snapshot;
            QVERIFY(!invalid.Save(path, &error));
            QVERIFY(beforeFile.open(QIODevice::ReadOnly));
            QCOMPARE(beforeFile.readAll(), before);
            beforeFile.close();
        }
        QVERIFY(!live2dstruct::CModelSession::FromJson(R"({"version":1,"animation":[]})", &current, &error));
        QCOMPARE(current.parameters.value("Keep"), 42.0f);
    }

    void sessionRoundTrip()
    {
        live2dstruct::CModelSession source;
        source.sourceMoc = QStringLiteral("/tmp/Model.moc3");
        source.sourceDigest = QByteArray(32, '\x42');
        source.parameters.insert(QStringLiteral("ParamAngleX"), -12.5f);
        source.parameterMetadata.insert(QStringLiteral("ParamAngleX"), {QStringLiteral("角度 X"), -30.0f, 30.0f, 0.0f, 2, false});
        source.visibility.insert(QStringLiteral("artmesh|Body"), false);
        source.locked.insert(QStringLiteral("artmesh|Body"), true);
        source.drawableOpacity.insert(QStringLiteral("Body"), 0.42f);
        source.drawableDrawOrder.insert(QStringLiteral("Body"), 720);
        source.drawableKeyformPositions.insert(QStringLiteral("Body"), {{0.0f, 1.0f, 2.0f, 3.0f}, {4.0f, 5.0f}});
        source.drawableIndices.insert(QStringLiteral("Body"), {0, 1, 2});
        source.drawableMaskIndices.insert(QStringLiteral("Body"), {3, 4});
        source.deformerParentIndices.insert(QStringLiteral("Warp1"), 2);
        source.drawableMaskInverted.insert(QStringLiteral("Body"), true);
        source.deformerRotationKeyforms.insert(QStringLiteral("Rotation1"),
                                                {{1.0f, 12.0f, 0.25f, -0.5f, 1.1f, true, false}});
        source.deformerWarpKeyforms.insert(QStringLiteral("Warp1"),
                                           {{{0.0f, 1.0f, 2.0f, 3.0f}, 1.0f}});
        source.viewScale = 1.75f;
        source.viewPosition = QPointF(0.25, -0.5);

        live2dstruct::CModelSession decoded;
        QString error;
        QVERIFY2(live2dstruct::CModelSession::FromJson(source.ToJson(), &decoded, &error), qPrintable(error));
        QCOMPARE(decoded.sourceMoc, source.sourceMoc);
        QCOMPARE(decoded.sourceDigest, source.sourceDigest);
        QCOMPARE(decoded.parameters, source.parameters);
        QCOMPARE(decoded.parameterMetadata, source.parameterMetadata);
        QCOMPARE(decoded.visibility, source.visibility);
        QCOMPARE(decoded.locked, source.locked);
        QCOMPARE(decoded.drawableOpacity, source.drawableOpacity);
        QCOMPARE(decoded.drawableDrawOrder, source.drawableDrawOrder);
        QCOMPARE(decoded.drawableKeyformPositions, source.drawableKeyformPositions);
        QCOMPARE(decoded.drawableIndices, source.drawableIndices);
        QCOMPARE(decoded.drawableMaskIndices, source.drawableMaskIndices);
        QCOMPARE(decoded.deformerParentIndices, source.deformerParentIndices);
        QCOMPARE(decoded.drawableMaskInverted, source.drawableMaskInverted);
        QCOMPARE(decoded.deformerRotationKeyforms, source.deformerRotationKeyforms);
        QCOMPARE(decoded.deformerWarpKeyforms, source.deformerWarpKeyforms);
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
        QCOMPARE(loaded.parameterMetadata, source.parameterMetadata);
        QCOMPARE(loaded.locked, source.locked);
        QCOMPARE(loaded.drawableOpacity, source.drawableOpacity);
        QCOMPARE(loaded.drawableDrawOrder, source.drawableDrawOrder);
        QCOMPARE(loaded.drawableKeyformPositions, source.drawableKeyformPositions);
        QCOMPARE(loaded.drawableMaskIndices, source.drawableMaskIndices);
        QCOMPARE(loaded.drawableMaskInverted, source.drawableMaskInverted);
        QCOMPARE(loaded.deformerRotationKeyforms, source.deformerRotationKeyforms);
        QCOMPARE(loaded.deformerWarpKeyforms, source.deformerWarpKeyforms);
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
        QVERIFY(!live2dstruct::CModelSession::FromJson(
            R"({"version":1,"parameterMetadata":{"ParamAngleX":{"minimum":0,"maximum":1,"defaultValue":2,"decimalPlaces":2,"repeat":false}}})", &session, &error));
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
        QVERIFY(!live2dstruct::CModelSession::FromJson(
            R"({"version":1,"drawableIndices":{"Body":[32768,1,2]}})", &session, &error));
        QVERIFY(!live2dstruct::CModelSession::FromJson(
            R"({"version":1,"drawableMaskIndices":{"Body":[1,1]}})", &session, &error));
        QVERIFY(!live2dstruct::CModelSession::FromJson(
            R"({"version":1,"drawableMaskIndices":{"Body":[-1]}})", &session, &error));
        QVERIFY(!live2dstruct::CModelSession::FromJson(
            R"({"version":1,"drawableMaskInverted":{"Body":"yes"}})", &session, &error));
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
