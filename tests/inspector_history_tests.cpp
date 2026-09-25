#include "ui/cinspectorpanel.h"
#include "live2dstruct/cmodeldocument.h"
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QTableWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <algorithm>

class InspectorHistoryTests : public QObject
{
    Q_OBJECT
private slots:
    void mergeTopologyDeduplicatesAndKeepsHistoryAtomic()
    {
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
        const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [](const auto& drawable) { return drawable.vertexCount >= 5 && !drawable.keyforms.isEmpty(); });
        QVERIFY(mesh != document.Drawables().cend());
        const QString id = mesh->id;
        const QVector<quint16> before{0, 2, 3, 0, 3, 4, 1, 2, 3};
        const QVector<quint16> after{0, 2, 3, 0, 3, 4};
        QVERIFY(document.SetDrawableIndices(id, before));
        const auto positions = document.DrawableKeyformPositions(id, 0);
        const auto uvs = mesh->uvs;
        const int vertexCount = mesh->vertexCount;

        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, QStringLiteral("artmesh"));
        auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
        auto* merge = panel.findChild<QPushButton*>("mergeSelectedVertices");
        QVERIFY(table && merge);
        QVERIFY(!merge->isEnabled());
        QSignalSpy changed(&panel, &CInspectorPanel::DrawableIndicesChanged);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        table->setRangeSelected(QTableWidgetSelectionRange(0, 0, 1, 2), true);
        QVERIFY(merge->isEnabled());
        merge->click();
        QCOMPARE(document.DrawableIndices(id), after);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(history.count(), 1);
        QVERIFY(document.CanUndoEdit());
        merge->click();
        QCOMPARE(document.DrawableIndices(id), after);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(history.count(), 1);
        panel.Undo();
        QCOMPARE(document.DrawableIndices(id), before);
        QVERIFY(!document.CanUndoEdit());
        panel.Redo();
        QCOMPARE(document.DrawableIndices(id), after);
        QVERIFY(!document.CanRedoEdit());
        QCOMPARE(document.DrawableKeyformPositions(id, 0), positions);
        QCOMPARE(mesh->uvs, uvs);
        QCOMPARE(mesh->vertexCount, vertexCount);

        panel.ClearHistory();
        const QVector<quint16> collapsed{0, 1, 2};
        QVERIFY(document.SetDrawableIndices(id, collapsed));
        panel.RefreshSelection();
        table->setRangeSelected(QTableWidgetSelectionRange(0, 0, 1, 2), true);
        changed.clear();
        history.clear();
        merge->click();
        QCOMPARE(document.DrawableIndices(id), collapsed);
        QVERIFY(!document.CanUndoEdit());
        QCOMPARE(changed.count(), 0);
        QCOMPARE(history.count(), 0);
    }

    void invalidTopologyLeavesDocumentUnchanged()
    {
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
        const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [](const auto& drawable) { return drawable.vertexCount >= 3; });
        QVERIFY(mesh != document.Drawables().cend());
        const QString id = mesh->id;
        const QVector<quint16> before{0, 1, 2};
        QVERIFY(document.SetDrawableIndices(id, before));
        for (const auto& invalid : {QVector<quint16>{}, QVector<quint16>{0, 1}, QVector<quint16>{0, 1, 32768}}) {
            QVERIFY(!document.SetDrawableIndices(id, invalid));
            QCOMPARE(document.DrawableIndices(id), before);
        }
        QVERIFY(!document.SetDrawableIndices(QStringLiteral("missing-artmesh"), before));
        QCOMPARE(document.DrawableIndices(id), before);
        const QVector<quint16> degenerate{0, 0, 1, 0, 0, 1};
        QVERIFY(document.SetDrawableIndices(id, degenerate));
        QCOMPARE(document.DrawableIndices(id), degenerate);
        QVERIFY(!document.CanUndoEdit());
    }

    void keyformSelectionTracksDisplayedMesh()
    {
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
        const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [](const auto& drawable) { return drawable.vertexCount > 0 && drawable.keyforms.size() >= 3; });
        QVERIFY(mesh != document.Drawables().cend());
        const auto other = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [&mesh](const auto& drawable) {
                return drawable.vertexCount > 0 && !drawable.keyforms.isEmpty() &&
                       drawable.keyforms.size() < mesh->keyforms.size();
            });
        QVERIFY(other != document.Drawables().cend());
        CInspectorPanel panel;
        panel.SetDocument(&document);
        QSignalSpy activated(&panel, &CInspectorPanel::DrawableKeyformActivated);
        QSignalSpy selected(&panel, &CInspectorPanel::DrawableKeyformSelected);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy changed(&panel, &CInspectorPanel::DrawableKeyformPositionsChanged);
        auto* selector = panel.findChild<QComboBox*>("keyformSelector");
        auto* vertices = panel.findChild<QTableWidget*>("keyformVertexTable");
        QVERIFY(selector && vertices);
        panel.SetSelection(mesh->id, QStringLiteral("artmesh"));
        QCOMPARE(activated.count(), 1);
        QCOMPARE(activated.last().at(0).toString(), mesh->id);
        QCOMPARE(activated.last().at(1).toInt(), 0);
        QCOMPARE(selected.count(), 0);
        QCOMPARE(changed.count(), 0);
        const int index = mesh->keyforms.size() / 2;
        selector->setCurrentIndex(index);
        QCOMPARE(selected.count(), 1);
        selected.clear();
        QCOMPARE(activated.last().at(1).toInt(), index);
        panel.RefreshSelection();
        QCOMPARE(selector->currentIndex(), index);
        QCOMPARE(activated.last().at(1).toInt(), index);
        QCOMPARE(history.count(), 0);
        QCOMPARE(selected.count(), 0);
        QCOMPARE(changed.count(), 0);
        QVERIFY(!document.CanUndoEdit());

        const QVector<float> before = document.DrawableKeyformPositions(mesh->id, index);
        QVERIFY(vertices->rowCount() > 0);
        vertices->item(0, 1)->setText(QString::number(before.first() + 0.25f, 'f', 6));
        const auto edited = document.DrawableKeyformPositions(mesh->id, index);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changed.last().at(1).toInt(), index);
        QVERIFY(edited != before);
        const int historyCount = history.count();
        panel.RefreshSelection();
        panel.SetSelection(mesh->id, QStringLiteral("artmesh"));
        QCOMPARE(selector->currentIndex(), index);
        QCOMPARE(history.count(), historyCount);
        panel.Undo();
        QCOMPARE(selector->currentIndex(), index);
        QCOMPARE(activated.last().at(1).toInt(), index);
        QCOMPARE(document.DrawableKeyformPositions(mesh->id, index), before);
        QVERIFY(!document.CanUndoEdit());
        panel.Redo();
        QCOMPARE(selector->currentIndex(), index);
        QCOMPARE(activated.last().at(1).toInt(), index);
        QCOMPARE(document.DrawableKeyformPositions(mesh->id, index), edited);
        QVERIFY(!document.CanRedoEdit());
        QCOMPARE(selected.count(), 0);
        selector->setCurrentIndex(index - 1);
        QCOMPARE(vertices->rowCount(), mesh->vertexCount);
        QVERIFY(vertices->isEnabled());
        auto* copyNext = panel.findChild<QPushButton*>("copyKeyformToNext");
        QVERIFY(copyNext && copyNext->isEnabled());
        selected.clear();
        copyNext->click();
        QCOMPARE(changed.last().at(1).toInt(), index);
        QCOMPARE(selector->currentIndex(), index - 1);
        QCOMPARE(activated.last().at(1).toInt(), index - 1);
        QCOMPARE(selected.count(), 0);

        selector->setCurrentIndex(mesh->keyforms.size() - 1);
        activated.clear();
        selected.clear();
        changed.clear();
        panel.SetSelection(other->id, QStringLiteral("artmesh"));
        QCOMPARE(selector->currentIndex(), 0);
        QCOMPARE(activated.count(), 1);
        QCOMPARE(activated.last().at(0).toString(), other->id);
        QCOMPARE(activated.last().at(1).toInt(), 0);
        QCOMPARE(vertices->rowCount(), other->vertexCount);
        QCOMPARE(selected.count(), 0);
        QCOMPARE(changed.count(), 0);
        panel.ClearSelection();
        activated.clear();
        panel.SetSelection(mesh->id, QStringLiteral("artmesh"));
        QCOMPARE(activated.count(), 1);
        QCOMPARE(selector->currentIndex(), 0);
        QCOMPARE(activated.last().at(1).toInt(), 0);
        QCOMPARE(selected.count(), 0);
        QCOMPARE(changed.count(), 0);
    }

    void warpKeyformSelectionSurvivesRefreshAndHistory()
    {
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
        const auto warp = std::find_if(document.Deformers().cbegin(), document.Deformers().cend(),
            [](const auto& deformer) {
                return deformer.warpKeyforms.size() > 1 && !deformer.warpKeyforms[1].positions.isEmpty();
            });
        QVERIFY(warp != document.Deformers().cend());
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(warp->id, QStringLiteral("warp"));
        auto* selector = panel.findChild<QComboBox*>("warpKeyformSelector");
        auto* vertices = panel.findChild<QTableWidget*>("warpKeyformTable");
        QVERIFY(selector && vertices);
        selector->setCurrentIndex(1);
        panel.RefreshSelection();
        QCOMPARE(selector->currentIndex(), 1);
        QVERIFY(!document.CanUndoEdit());
        const auto before = document.DeformerWarpKeyforms(warp->id);
        QVERIFY(vertices->rowCount() > 0);
        vertices->item(0, 1)->setText(QString::number(before[1].positions.first() + 0.25f, 'g', 9));
        const auto edited = document.DeformerWarpKeyforms(warp->id);
        QVERIFY(edited != before);
        panel.Undo();
        QCOMPARE(selector->currentIndex(), 1);
        QCOMPARE(document.DeformerWarpKeyforms(warp->id), before);
        QVERIFY(!document.CanUndoEdit());
        panel.Redo();
        QCOMPARE(selector->currentIndex(), 1);
        QCOMPARE(document.DeformerWarpKeyforms(warp->id), edited);
        QVERIFY(!document.CanRedoEdit());
    }

    void editsUndoAndRedo()
    {
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
        const QString id = document.Drawables().first().id;
        const auto& drawable = document.Drawables().first();
        const bool initialVisibility = drawable.visible;
        const float initialOpacity = document.DrawableOpacity(drawable);
        const int initialDrawOrder = qRound(document.DrawableDrawOrder(drawable));

        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, QStringLiteral("artmesh"));
        auto* visibility = panel.findChild<QCheckBox*>();
        auto* opacity = panel.findChild<QDoubleSpinBox*>();
        auto* drawOrder = panel.findChild<QSpinBox*>();
        QVERIFY(visibility && opacity && drawOrder);

        visibility->setChecked(!initialVisibility);
        QCOMPARE(document.Drawables().first().visible, !initialVisibility);
        panel.Undo();
        QCOMPARE(document.Drawables().first().visible, initialVisibility);
        panel.Redo();
        QCOMPARE(document.Drawables().first().visible, !initialVisibility);

        opacity->setValue(0.25);
        QCOMPARE(document.DrawableOpacity(document.Drawables().first()), 0.25f);
        panel.Undo();
        QCOMPARE(document.DrawableOpacity(document.Drawables().first()), initialOpacity);
        panel.Redo();
        QCOMPARE(document.DrawableOpacity(document.Drawables().first()), 0.25f);

        drawOrder->setValue(initialDrawOrder == 1000 ? 999 : initialDrawOrder + 1);
        const int changedDrawOrder = drawOrder->value();
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().first())), changedDrawOrder);
        panel.Undo();
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().first())), initialDrawOrder);
        panel.Redo();
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().first())), changedDrawOrder);

        auto* keyformSelector = panel.findChild<QComboBox*>("keyformSelector");
        auto* vertexTable = panel.findChild<QTableWidget*>("keyformVertexTable");
        QVERIFY(keyformSelector && vertexTable);
        QVERIFY(keyformSelector->count() > 0);
        QVERIFY(vertexTable->rowCount() > 0);
        QSignalSpy keyformSelections(&panel, &CInspectorPanel::DrawableKeyformSelected);
        if (keyformSelector->count() > 1) {
            keyformSelector->setCurrentIndex(1);
            QCOMPARE(keyformSelections.count(), 1);
            QCOMPARE(keyformSelections.last().at(0).toString(), id);
            QCOMPARE(keyformSelections.last().at(1).toInt(), 1);
            keyformSelector->setCurrentIndex(0);
        }
        const QVector<float> keyformBefore = document.DrawableKeyformPositions(id, 0);
        QVERIFY(!keyformBefore.isEmpty());
        const float editedX = keyformBefore.first() + 0.5f;
        vertexTable->item(0, 1)->setText(QString::number(editedX, 'f', 6));
        QVERIFY(qAbs(document.DrawableKeyformPositions(id, 0).first() - editedX) < 0.00001f);
        panel.Undo();
        QVERIFY(qAbs(document.DrawableKeyformPositions(id, 0).first() - keyformBefore.first()) < 0.00001f);
        panel.Redo();
        QVERIFY(qAbs(document.DrawableKeyformPositions(id, 0).first() - editedX) < 0.00001f);
        auto* moveVertices = panel.findChild<QPushButton*>("moveSelectedVertices");
        auto* deltaX = panel.findChild<QDoubleSpinBox*>("vertexDeltaX");
        auto* deltaY = panel.findChild<QDoubleSpinBox*>("vertexDeltaY");
        QVERIFY(moveVertices && deltaX && deltaY);
        const QVector<float> translateBefore = document.DrawableKeyformPositions(id, 0);
        QVERIFY(translateBefore.size() >= 4);
        vertexTable->setRangeSelected(QTableWidgetSelectionRange(0, 0, 1, 2), true);
        deltaX->setValue(0.125);
        deltaY->setValue(-0.25);
        QVERIFY(moveVertices->isEnabled());
        moveVertices->click();
        const QVector<float> translateAfter = document.DrawableKeyformPositions(id, 0);
        QCOMPARE(translateAfter[0], translateBefore[0] + 0.125f);
        QCOMPARE(translateAfter[1], translateBefore[1] - 0.25f);
        QCOMPARE(translateAfter[2], translateBefore[2] + 0.125f);
        QCOMPARE(translateAfter[3], translateBefore[3] - 0.25f);
        panel.Undo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), translateBefore);
        panel.Redo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), translateAfter);
        auto* mergeVertices = panel.findChild<QPushButton*>("mergeSelectedVertices");
        QVERIFY(mergeVertices);
        vertexTable->setRangeSelected(QTableWidgetSelectionRange(0, 0, 1, 2), true);
        const QVector<quint16> mergeBefore = document.DrawableIndices(id);
        mergeVertices->click();
        const QVector<quint16> mergeAfter = document.DrawableIndices(id);
        QVERIFY(!mergeAfter.isEmpty() && mergeAfter.size() % 3 == 0);
        QVERIFY(mergeAfter != mergeBefore);
        panel.Undo();
        QCOMPARE(document.DrawableIndices(id), mergeBefore);
        panel.Redo();
        QCOMPARE(document.DrawableIndices(id), mergeAfter);
        auto* transformVertices = panel.findChild<QPushButton*>("transformSelectedVertices");
        auto* scaleVertices = panel.findChild<QDoubleSpinBox*>("vertexScale");
        auto* rotationVertices = panel.findChild<QDoubleSpinBox*>("vertexRotation");
        QVERIFY(transformVertices && scaleVertices && rotationVertices);
        vertexTable->setRangeSelected(QTableWidgetSelectionRange(0, 0, 1, 2), true);
        const QVector<float> transformBefore = document.DrawableKeyformPositions(id, 0);
        const float centerX = (transformBefore[0] + transformBefore[2]) * 0.5f;
        const float centerY = (transformBefore[1] + transformBefore[3]) * 0.5f;
        scaleVertices->setValue(2.0);
        rotationVertices->setValue(0.0);
        transformVertices->click();
        const QVector<float> transformAfter = document.DrawableKeyformPositions(id, 0);
        QCOMPARE(transformAfter[0], centerX + (transformBefore[0] - centerX) * 2.0f);
        QCOMPARE(transformAfter[1], centerY + (transformBefore[1] - centerY) * 2.0f);
        QCOMPARE(transformAfter[2], centerX + (transformBefore[2] - centerX) * 2.0f);
        QCOMPARE(transformAfter[3], centerY + (transformBefore[3] - centerY) * 2.0f);
        panel.Undo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), transformBefore);
        panel.Redo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), transformAfter);
        const QVector<float> rotateBefore = transformAfter;
        scaleVertices->setValue(1.0);
        rotationVertices->setValue(90.0);
        vertexTable->setRangeSelected(QTableWidgetSelectionRange(0, 0, 1, 2), true);
        transformVertices->click();
        const QVector<float> rotateAfter = document.DrawableKeyformPositions(id, 0);
        const float rotateCenterX = (rotateBefore[0] + rotateBefore[2]) * 0.5f;
        const float rotateCenterY = (rotateBefore[1] + rotateBefore[3]) * 0.5f;
        QCOMPARE(rotateAfter[0], rotateCenterX - (rotateBefore[1] - rotateCenterY));
        QCOMPARE(rotateAfter[1], rotateCenterY + (rotateBefore[0] - rotateCenterX));
        panel.Undo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), rotateBefore);
        panel.ClearHistory();
        QString deformerId;
        int deformerParentChoice = -1;
        for (int i = 0; i < document.Deformers().size() && deformerId.isEmpty(); ++i) {
            for (int j = 0; j < document.Deformers().size(); ++j) {
                if (i == j || document.Deformers()[i].parentPartIndex != document.Deformers()[j].parentPartIndex ||
                    document.DeformerParentIndex(document.Deformers()[i].id) == j) continue;
                deformerId = document.Deformers()[i].id;
                deformerParentChoice = j;
                break;
            }
        }
        QVERIFY(!deformerId.isEmpty() && deformerParentChoice >= 0);
        QString deformerType = QStringLiteral("warp");
        for (const auto& deformer : document.Deformers())
            if (deformer.id == deformerId && deformer.type == live2dstruct::CModelDeformer::Rotation)
                deformerType = QStringLiteral("rotation");
        panel.SetSelection(deformerId, deformerType);
        auto* deformerParent = panel.findChild<QComboBox*>("deformerParentSelector");
        QVERIFY(deformerParent && deformerParent->count() > 1);
        const int parentRow = deformerParent->findData(deformerParentChoice);
        QVERIFY(parentRow >= 0);
        const int oldDeformerParent = document.DeformerParentIndex(deformerId);
        deformerParent->setCurrentIndex(parentRow);
        QCOMPARE(document.DeformerParentIndex(deformerId), deformerParentChoice);
        panel.Undo();
        QCOMPARE(document.DeformerParentIndex(deformerId), oldDeformerParent);
        panel.Redo();
        QCOMPARE(document.DeformerParentIndex(deformerId), deformerParentChoice);
        if (deformerType == QLatin1String("rotation")) {
            auto* rotationTable = panel.findChild<QTableWidget*>("rotationKeyformTable");
            QVERIFY(rotationTable && rotationTable->rowCount() > 0 && rotationTable->isEnabled());
            const auto rotationBefore = document.DeformerRotationKeyforms(deformerId);
            const float angleBefore = rotationBefore.first().angle;
            rotationTable->item(0, 1)->setText(QString::number(angleBefore + 6.0f));
            const auto rotationAfter = document.DeformerRotationKeyforms(deformerId);
            QCOMPARE(rotationAfter.first().angle, angleBefore + 6.0f);
            panel.Undo();
            QCOMPARE(document.DeformerRotationKeyforms(deformerId).first().angle, angleBefore);
            panel.Redo();
            QCOMPARE(document.DeformerRotationKeyforms(deformerId).first().angle, angleBefore + 6.0f);
        } else {
            auto* warpSelector = panel.findChild<QComboBox*>("warpKeyformSelector");
            auto* warpTable = panel.findChild<QTableWidget*>("warpKeyformTable");
            QVERIFY(warpSelector && warpTable && warpSelector->count() > 0 && warpTable->rowCount() > 0 && warpTable->isEnabled());
            const auto warpBefore = document.DeformerWarpKeyforms(deformerId);
            const float xBefore = warpBefore.first().positions.first();
            warpTable->item(0, 1)->setText(QString::number(xBefore + 2.5f));
            QCOMPARE(document.DeformerWarpKeyforms(deformerId).first().positions.first(), xBefore + 2.5f);
            panel.Undo();
            QCOMPARE(document.DeformerWarpKeyforms(deformerId).first().positions.first(), xBefore);
            panel.Redo();
            QCOMPARE(document.DeformerWarpKeyforms(deformerId).first().positions.first(), xBefore + 2.5f);
        }
        QString maskedId;
        for (const auto& drawableCandidate : document.Drawables()) {
            if (!drawableCandidate.maskIndices.isEmpty()) { maskedId = drawableCandidate.id; break; }
        }
        if (!maskedId.isEmpty()) {
            panel.SetSelection(maskedId, QStringLiteral("artmesh"));
            auto* maskInverted = panel.findChild<QCheckBox*>("maskInverted");
            QVERIFY(maskInverted && maskInverted->isEnabled());
            const bool beforeMaskInverted = document.DrawableMaskInverted(maskedId);
            maskInverted->setChecked(!beforeMaskInverted);
            QCOMPARE(document.DrawableMaskInverted(maskedId), !beforeMaskInverted);
            panel.Undo();
            QCOMPARE(document.DrawableMaskInverted(maskedId), beforeMaskInverted);
            panel.Redo();
            QCOMPARE(document.DrawableMaskInverted(maskedId), !beforeMaskInverted);
        }
        panel.SetSelection(id, QStringLiteral("artmesh"));
        auto* mirrorHorizontal = panel.findChild<QPushButton*>("mirrorHorizontal");
        QVERIFY(mirrorHorizontal && mirrorHorizontal->isEnabled());
        const QVector<float> mirrorBefore = document.DrawableKeyformPositions(id, 0);
        float minX = mirrorBefore.first();
        float maxX = minX;
        for (int i = 0; i < mirrorBefore.size(); i += 2) {
            minX = qMin(minX, mirrorBefore[i]);
            maxX = qMax(maxX, mirrorBefore[i]);
        }
        const float mirrorCenter = (minX + maxX) * 0.5f;
        mirrorHorizontal->click();
        const QVector<float> mirrorAfter = document.DrawableKeyformPositions(id, 0);
        QVERIFY(qAbs(mirrorAfter.first() - (2.0f * mirrorCenter - mirrorBefore.first())) < 0.00001f);
        panel.Undo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), mirrorBefore);
        panel.Redo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), mirrorAfter);
        auto* interpolatePrevious = panel.findChild<QPushButton*>("interpolatePreviousKeyform");
        QVERIFY(interpolatePrevious);
        if (interpolatePrevious->isEnabled() && keyformSelector->count() > 1) {
            keyformSelector->setCurrentIndex(1);
            const QVector<float> current = document.DrawableKeyformPositions(id, 1);
            const QVector<float> previous = document.DrawableKeyformPositions(id, 0);
            QVERIFY(current.size() == previous.size() && !current.isEmpty());
            interpolatePrevious->click();
            const QVector<float> interpolated = document.DrawableKeyformPositions(id, 1);
            QVERIFY(qAbs(interpolated.first() - (current.first() + previous.first()) * 0.5f) < 0.00001f);
            panel.Undo();
            QCOMPARE(document.DrawableKeyformPositions(id, 1), current);
            panel.Redo();
            QCOMPARE(document.DrawableKeyformPositions(id, 1), interpolated);
            keyformSelector->setCurrentIndex(0);
        }
        auto* copyToNext = panel.findChild<QPushButton*>("copyKeyformToNext");
        auto* resetKeyform = panel.findChild<QPushButton*>("resetKeyform");
        auto* autoTriangulate = panel.findChild<QPushButton*>("autoTriangulate");
        QVERIFY(copyToNext && resetKeyform && autoTriangulate);
        if (copyToNext->isEnabled() && keyformSelector->count() > 1) {
            const QVector<float> nextBefore = document.DrawableKeyformPositions(id, 1);
            const QVector<float> current = document.DrawableKeyformPositions(id, 0);
            copyToNext->click();
            QCOMPARE(document.DrawableKeyformPositions(id, 1), current);
            panel.Undo();
            QCOMPARE(document.DrawableKeyformPositions(id, 1), nextBefore);
            panel.Redo();
            QCOMPARE(document.DrawableKeyformPositions(id, 1), current);
            keyformSelector->setCurrentIndex(1);
            resetKeyform->click();
            QCOMPARE(document.DrawableKeyformPositions(id, 1), document.Drawables().first().keyforms.at(1).positions);
            panel.Undo();
            QCOMPARE(document.DrawableKeyformPositions(id, 1), current);
            keyformSelector->setCurrentIndex(0);
        }
        const QVector<quint16> topologyBefore = document.DrawableIndices(id);
        QSignalSpy topologyChanges(&panel, &CInspectorPanel::DrawableIndicesChanged);
        autoTriangulate->click();
        QVERIFY(topologyChanges.count() > 0);
        const QVector<quint16> topologyAfter = document.DrawableIndices(id);
        QVERIFY(!topologyAfter.isEmpty() && topologyAfter.size() % 3 == 0);
        QVERIFY(topologyAfter != topologyBefore);
        panel.Undo();
        QCOMPARE(document.DrawableIndices(id), topologyBefore);
        panel.Redo();
        QCOMPARE(document.DrawableIndices(id), topologyAfter);
        panel.ClearHistory();

        const QString secondId = document.Drawables().at(1).id;
        const float secondOpacity = document.DrawableOpacity(document.Drawables().at(1));
        const int secondDrawOrder = qRound(document.DrawableDrawOrder(document.Drawables().at(1)));
        const bool secondVisibility = document.Drawables().at(1).visible;
        const int currentDrawOrder = qRound(document.DrawableDrawOrder(document.Drawables().first()));
        const bool firstVisibilityBefore = document.Drawables().first().visible;
        panel.SetSelections({id, secondId}, {QStringLiteral("artmesh"), QStringLiteral("artmesh")});
        const bool targetVisibility = !document.Drawables().first().visible;
        const int visibilityChanges = (document.Drawables().first().visible != targetVisibility ? 1 : 0) +
            (document.Drawables().at(1).visible != targetVisibility ? 1 : 0);
        QVERIFY(visibility->isEnabled());
        visibility->setCheckState(targetVisibility ? Qt::Checked : Qt::Unchecked);
        QCOMPARE(document.Drawables().first().visible, targetVisibility);
        QCOMPARE(document.Drawables().at(1).visible, targetVisibility);
        QVERIFY(visibilityChanges > 0);
        for (int i = 0; i < visibilityChanges; ++i) panel.Undo();
        QCOMPARE(document.Drawables().first().visible, firstVisibilityBefore);
        QCOMPARE(document.Drawables().at(1).visible, secondVisibility);
        for (int i = 0; i < visibilityChanges; ++i) panel.Redo();
        panel.ClearHistory();

        const float firstOpacityBefore = document.DrawableOpacity(document.Drawables().first());
        const float secondOpacityBefore = document.DrawableOpacity(document.Drawables().at(1));
        opacity->setValue(0.33);
        QCOMPARE(document.DrawableOpacity(document.Drawables().first()), 0.33f);
        QCOMPARE(document.DrawableOpacity(document.Drawables().at(1)), 0.33f);
        const int opacityChanges = (qFuzzyCompare(firstOpacityBefore + 1.0f, 1.33f) ? 0 : 1) +
            (qFuzzyCompare(secondOpacityBefore + 1.0f, 1.33f) ? 0 : 1);
        QVERIFY(opacityChanges > 0);
        for (int i = 0; i < opacityChanges; ++i) panel.Undo();
        QCOMPARE(document.DrawableOpacity(document.Drawables().first()), firstOpacityBefore);
        QCOMPARE(document.DrawableOpacity(document.Drawables().at(1)), secondOpacity);
        for (int i = 0; i < opacityChanges; ++i) panel.Redo();
        panel.ClearHistory();

        const int firstDrawOrderBefore = qRound(document.DrawableDrawOrder(document.Drawables().first()));
        const int secondDrawOrderBefore = qRound(document.DrawableDrawOrder(document.Drawables().at(1)));
        drawOrder->setValue(321);
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().first())), 321);
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().at(1))), 321);
        const int drawOrderChanges = (firstDrawOrderBefore == 321 ? 0 : 1) +
            (secondDrawOrderBefore == 321 ? 0 : 1);
        QVERIFY(drawOrderChanges > 0);
        for (int i = 0; i < drawOrderChanges; ++i) panel.Undo();
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().first())), currentDrawOrder);
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().at(1))), secondDrawOrder);
        for (int i = 0; i < drawOrderChanges; ++i) panel.Redo();
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().first())), 321);
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().at(1))), 321);
        panel.ClearHistory();
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().first())), 321);
        panel.SetSelections({id}, {QStringLiteral("artmesh")});
        panel.SetSelections({}, {});
        QVERIFY(!visibility->isEnabled());
        panel.SetSelection(id, QStringLiteral("artmesh"));
        panel.SetLockedState({QStringLiteral("artmesh|") + id});
        QVERIFY(!visibility->isEnabled());
        QVERIFY(!opacity->isEnabled());
        QVERIFY(!drawOrder->isEnabled());
        panel.SetLockedState({});
        QVERIFY(visibility->isEnabled());
    }
};

QTEST_MAIN(InspectorHistoryTests)
#include "inspector_history_tests.moc"
