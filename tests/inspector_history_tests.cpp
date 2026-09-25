#include "ui/cinspectorpanel.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/ckeyformeditcontroller.h"
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QTableWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QDialog>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QTimer>
#include <QApplication>
#include <QClipboard>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QMessageBox>
#include <QtEndian>
#include <stdexcept>
#include <algorithm>

class InspectorHistoryTests : public QObject
{
    Q_OBJECT
private slots:
    void controllerBackedEditsAndHistoryPublishExactlyOnce_data()
    {
        QTest::addColumn<bool>("copyToNext");
        QTest::newRow("vertex-table") << false;
        QTest::newRow("copy-next") << true;
    }

    void controllerBackedEditsAndHistoryPublishExactlyOnce()
    {
        QFETCH(bool, copyToNext);
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [](const auto& drawable){ return drawable.vertexCount && drawable.keyforms.size() >= 3; });
        QVERIFY(mesh != document.Drawables().cend());
        const QString id = mesh->id;
        const auto previewPositions = document.DrawableKeyformPositions(id, 1);
        auto nextPositions = previewPositions;
        nextPositions[0] += 0.5f;
        QVERIFY(document.SetDrawableKeyformPositions(id, 2, nextPositions));
        const int editedKeyform = copyToNext ? 2 : 1;
        const auto before = document.DrawableKeyformPositions(id, editedKeyform);
        QMap<QString, float> currentValues;
        for (const auto& parameter : document.Parameters())
            currentValues.insert(parameter.id, float(double(parameter.minimum) +
                (double(parameter.maximum) - parameter.minimum) * 0.37));
        QMap<QString, float> corner;
        QVERIFY(document.KeyformParameterValues(id, QStringLiteral("artmesh"), 1, corner));
        auto expectedPose = currentValues;
        for (auto it = corner.cbegin(); it != corner.cend(); ++it) expectedPose.insert(it.key(), it.value());
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, QStringLiteral("artmesh"));
        auto* combo = panel.findChild<QComboBox*>("keyformSelector");
        auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
        auto* copy = panel.findChild<QPushButton*>("copyKeyformToNext");
        QVERIFY(combo && table && copy);
        combo->setCurrentIndex(1);
        CKeyformEditController controller;
        controller.SetDocument(&document);
        bool acceptPublication = false;
        int publications = 0, builtCandidates = 0, editCalls = 0, historyCalls = 0, activationCalls = 0;
        QMap<QString, float> publishedPose;
        QSignalSpy changed(&panel, &CInspectorPanel::DrawableKeyformPositionsChanged);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy activated(&panel, &CInspectorPanel::DrawableKeyformActivated);
        QSignalSpy selected(&panel, &CInspectorPanel::DrawableKeyformSelected);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        controller.SetPublisher([&](const CModelDocument& candidate, const QMap<QString, float>& pose,
                                    bool editing, QString* error) {
            ++publications;
            QByteArray bytes;
            if (!editing || !candidate.BuildPreviewMoc(bytes, error) || bytes.isEmpty()) return false;
            ++builtCandidates;
            publishedPose = pose;
            if (!acceptPublication) { *error = QStringLiteral("controlled preview rejection"); return false; }
            return true;
        }, [](QString*) { return true; });
        const auto refreshAcceptedSelection = [&] {
            const QSignalBlocker blocker(&panel);
            panel.RefreshSelection();
            panel.SyncHistoryState();
        };
        panel.SetEditHandler([&](const CModelEdit& edit, QString* error) {
            ++editCalls;
            if (!controller.Submit(edit, currentValues, panel.GetActiveKeyformIndex(), error)) return false;
            refreshAcceptedSelection();
            return true;
        });
        panel.SetHistoryHandler([&](bool undo, CModelEdit& edit, QString* error) {
            ++historyCalls;
            if (!controller.History(undo, edit, error)) return false;
            refreshAcceptedSelection();
            return true;
        });
        panel.SetKeyformHandler([&](const QString& target, int keyform, QString* error) {
            ++activationCalls;
            return controller.Activate(target, keyform, currentValues, error);
        });
        const auto editFromUi = [&] {
            if (copyToNext) copy->click();
            else table->item(0, 1)->setText(QString::number(previewPositions[0] + 0.25f, 'g', 9));
        };
        const quint64 startRevision = document.Revision();
        editFromUi();
        QCOMPARE(publications, 1);
        QCOMPARE(builtCandidates, 1);
        QCOMPARE(editCalls, 1);
        QCOMPARE(document.Revision(), startRevision);
        QCOMPARE(document.DrawableKeyformPositions(id, editedKeyform), before);
        QVERIFY(!document.CanUndoEdit() && !document.CanRedoEdit());
        QVERIFY(!controller.IsActive());
        QCOMPARE(panel.GetActiveKeyformIndex(), 1);
        QCOMPARE(combo->currentIndex(), 1);
        QCOMPARE(table->item(0, 1)->text(), QString::number(previewPositions[0], 'f', 6));
        QCOMPARE(changed.count(), 0);
        QCOMPARE(history.count(), 0);
        QCOMPARE(activated.count(), 0);
        QCOMPARE(rejected.count(), 1);
        acceptPublication = true;
        editFromUi();
        const auto after = document.DrawableKeyformPositions(id, editedKeyform);
        QVERIFY(after != before);
        if (copyToNext) QCOMPARE(after, previewPositions);
        QCOMPARE(publications, 2);
        QCOMPARE(editCalls, 2);
        QCOMPARE(document.Revision(), startRevision + 1);
        QCOMPARE(publishedPose, expectedPose);
        QCOMPARE(controller.Pose(), expectedPose);
        QCOMPARE(controller.Target(), id);
        QCOMPARE(controller.Keyform(), 1);
        QCOMPARE(panel.GetActiveKeyformIndex(), 1);
        QCOMPARE(combo->currentIndex(), 1);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(history.count(), 1);
        QVERIFY(document.CanUndoEdit() && !document.CanRedoEdit());
        CModelEdit pending;
        QVERIFY(document.PeekUndoEdit(&pending));
        QCOMPARE(pending.index, editedKeyform);
        QCOMPARE(pending.before.value<QVector<float>>(), before);
        QCOMPARE(pending.after.value<QVector<float>>(), after);
        acceptPublication = false;
        const int successActivations = activated.count();
        panel.Undo();
        QCOMPARE(publications, 3);
        QCOMPARE(document.Revision(), startRevision + 1);
        QCOMPARE(document.DrawableKeyformPositions(id, editedKeyform), after);
        QVERIFY(document.CanUndoEdit() && !document.CanRedoEdit());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(history.count(), 1);
        QCOMPARE(activated.count(), successActivations);
        QCOMPARE(rejected.count(), 2);
        acceptPublication = true;
        panel.Undo();
        QCOMPARE(publications, 4);
        QCOMPARE(document.Revision(), startRevision + 2);
        QCOMPARE(document.DrawableKeyformPositions(id, editedKeyform), before);
        QVERIFY(!document.CanUndoEdit() && document.CanRedoEdit());
        QCOMPARE(changed.count(), 2);
        QCOMPARE(history.count(), 2);
        panel.Undo();
        QCOMPARE(publications, 4);
        QCOMPARE(document.Revision(), startRevision + 2);
        acceptPublication = false;
        const int undoActivations = activated.count();
        panel.Redo();
        QCOMPARE(publications, 5);
        QCOMPARE(document.Revision(), startRevision + 2);
        QCOMPARE(document.DrawableKeyformPositions(id, editedKeyform), before);
        QVERIFY(!document.CanUndoEdit() && document.CanRedoEdit());
        QCOMPARE(changed.count(), 2);
        QCOMPARE(history.count(), 2);
        QCOMPARE(activated.count(), undoActivations);
        QCOMPARE(rejected.count(), 3);
        acceptPublication = true;
        panel.Redo();
        QCOMPARE(publications, 6);
        QCOMPARE(builtCandidates, 6);
        QCOMPARE(historyCalls, 4);
        QCOMPARE(document.Revision(), startRevision + 3);
        QCOMPARE(document.DrawableKeyformPositions(id, editedKeyform), after);
        QVERIFY(document.CanUndoEdit() && !document.CanRedoEdit());
        QCOMPARE(changed.count(), 3);
        QCOMPARE(history.count(), 3);
        QCOMPARE(publishedPose, expectedPose);
        refreshAcceptedSelection();
        QCoreApplication::processEvents();
        QCOMPARE(publications, 6);
        QCOMPARE(document.Revision(), startRevision + 3);
        QCOMPARE(activationCalls, 0);
        QCOMPARE(selected.count(), 0);
        QCOMPARE(panel.GetSelectionId(), id);
        QCOMPARE(panel.GetActiveKeyformIndex(), 1);
        QCOMPARE(combo->currentIndex(), 1);
        QCOMPARE(controller.Keyform(), 1);
        QCOMPARE(history.count(), 3);
        for (const auto& notification : activated) {
            QCOMPARE(notification[0].toString(), id);
            QCOMPARE(notification[1].toInt(), 1);
        }
    }

    void keyformToolsUseInjectedCommitter_data()
    {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<bool>("accepted");
        const QStringList operations{QStringLiteral("cell"), QStringLiteral("mirrorHorizontal"),
            QStringLiteral("mirrorVertical"), QStringLiteral("interpolatePreviousKeyform"),
            QStringLiteral("copyKeyformToNext"), QStringLiteral("resetKeyform"),
            QStringLiteral("pasteKeyform"), QStringLiteral("moveSelectedVertices"),
            QStringLiteral("transformSelectedVertices")};
        for (const auto& operation : operations)
            for (bool accepted : {false, true})
                QTest::newRow(qPrintable(operation + (accepted ? QStringLiteral("-accept") : QStringLiteral("-reject"))))
                    << operation << accepted;
    }

    void keyformToolsUseInjectedCommitter()
    {
        QFETCH(QString, operation);
        QFETCH(bool, accepted);
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [](const auto& value){ return value.vertexCount >= 5 && value.keyforms.size() >= 3; });
        QVERIFY(mesh != document.Drawables().cend());
        const QString id = mesh->id;
        auto current = document.DrawableKeyformPositions(id, 1);
        current[0] += 0.3f;
        current[1] += 0.15f;
        auto previous = current;
        previous[0] += 0.5f;
        auto next = current;
        next[0] -= 0.5f;
        QVERIFY(document.SetDrawableKeyformPositions(id, 0, previous));
        QVERIFY(document.SetDrawableKeyformPositions(id, 1, current));
        QVERIFY(document.SetDrawableKeyformPositions(id, 2, next));
        const auto originalOverrides = document.DrawableKeyformPositionOverrides();
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, QStringLiteral("artmesh"));
        auto* combo = panel.findChild<QComboBox*>("keyformSelector");
        auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
        QVERIFY(combo && table);
        combo->setCurrentIndex(1);
        QCOMPARE(panel.GetActiveKeyformIndex(), 1);
        auto paste = current;
        paste[0] += 0.75f;
        QJsonArray pastedValues;
        for (float value : paste) pastedValues.append(double(value));
        const QJsonObject clipboard{{QStringLiteral("format"), QStringLiteral("live2d2psd-keyform-v1")},
                                    {QStringLiteral("vertexCount"), current.size() / 2},
                                    {QStringLiteral("positions"), pastedValues}};
        QSignalSpy changed(&panel, &CInspectorPanel::DrawableKeyformPositionsChanged);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        QSignalSpy selected(&panel, &CInspectorPanel::DrawableKeyformSelected);
        int calls = 0;
        bool calledBeforePublishing = false;
        CModelEdit submitted;
        panel.SetEditHandler([&](const CModelEdit& edit, QString* error) {
            ++calls;
            submitted = edit;
            calledBeforePublishing = changed.isEmpty() && history.isEmpty() &&
                                     document.DrawableKeyformPositionOverrides() == originalOverrides;
            if (!accepted) { *error = QStringLiteral("candidate rejected"); return false; }
            return document.ExecuteEdit(edit);
        });
        if (operation == QLatin1String("cell")) {
            table->item(0, 1)->setText(QString::number(current[0] + 0.25f, 'g', 9));
        } else {
            if (operation == QLatin1String("pasteKeyform")) {
                auto* copy = panel.findChild<QPushButton*>("copyKeyform");
                QVERIFY(copy && copy->isEnabled());
                copy->click();
                QCOMPARE(calls, 0);
                QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(clipboard).toJson()));
            }
            if (operation == QLatin1String("moveSelectedVertices") || operation == QLatin1String("transformSelectedVertices")) {
                table->setRangeSelected(QTableWidgetSelectionRange(0, 0, 1, 2), true);
                panel.findChild<QDoubleSpinBox*>("vertexDeltaX")->setValue(0.125);
                panel.findChild<QDoubleSpinBox*>("vertexDeltaY")->setValue(-0.25);
                panel.findChild<QDoubleSpinBox*>("vertexScale")->setValue(1.5);
                panel.findChild<QDoubleSpinBox*>("vertexRotation")->setValue(30);
            }
            auto* button = panel.findChild<QPushButton*>(operation);
            QVERIFY(button && button->isEnabled());
            button->click();
        }
        QCOMPARE(calls, 1);
        QVERIFY(calledBeforePublishing);
        QCOMPARE(submitted.field, CModelEdit::KeyformPositions);
        QCOMPARE(submitted.name, id);
        QCOMPARE(submitted.index, operation == QLatin1String("copyKeyformToNext") ? 2 : 1);
        QCOMPARE(panel.GetSelectionId(), id);
        QCOMPARE(panel.GetActiveKeyformIndex(), 1);
        QCOMPARE(combo->currentIndex(), 1);
        QCOMPARE(selected.count(), 0);
        if (accepted) {
            QCOMPARE(document.DrawableKeyformPositions(id, submitted.index), submitted.after.value<QVector<float>>());
            QVERIFY(document.CanUndoEdit());
            QCOMPARE(changed.count(), 1);
            QCOMPARE(history.count(), 1);
            QCOMPARE(rejected.count(), 0);
        } else {
            QCOMPARE(document.DrawableKeyformPositionOverrides(), originalOverrides);
            QVERIFY(!document.CanUndoEdit());
            QCOMPARE(changed.count(), 0);
            QCOMPARE(history.count(), 0);
            QCOMPARE(rejected.count(), 1);
            QCOMPARE(rejected.first().first().toString(), QStringLiteral("candidate rejected"));
            QCOMPARE(table->item(0, 1)->text(), QString::number(current[0], 'f', 6));
        }
    }

    void basicPropertiesUseInjectedCommitter_data()
    {
        QTest::addColumn<int>("field");
        QTest::addColumn<bool>("accepted");
        for (int field = 0; field < 3; ++field)
            for (bool accepted : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(field).arg(accepted))) << field << accepted;
    }

    void basicPropertiesUseInjectedCommitter()
    {
        QFETCH(int, field);
        QFETCH(bool, accepted);
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString id = document.Drawables().front().id;
        const bool beforeVisibility = document.Drawables().front().visible;
        const float beforeOpacity = document.DrawableOpacity(document.Drawables().front());
        const int beforeOrder = qRound(document.DrawableDrawOrder(document.Drawables().front()));
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, QStringLiteral("artmesh"));
        auto* visibility = panel.findChild<QCheckBox*>();
        auto* opacity = panel.findChild<QDoubleSpinBox*>();
        auto* order = panel.findChild<QSpinBox*>();
        QVERIFY(visibility && opacity && order);
        QSignalSpy visibleChanged(&panel, &CInspectorPanel::VisibilityChanged);
        QSignalSpy opacityChanged(&panel, &CInspectorPanel::DrawableOpacityChanged);
        QSignalSpy orderChanged(&panel, &CInspectorPanel::DrawableDrawOrderChanged);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        int calls = 0;
        panel.SetEditHandler([&](const CModelEdit& edit, QString* error) {
            ++calls;
            if (!accepted) { *error = QStringLiteral("rejected property"); return false; }
            return document.ExecuteEdit(edit);
        });
        if (field == 0) visibility->click();
        if (field == 1) opacity->setValue(beforeOpacity == 0.25f ? 0.75 : 0.25);
        if (field == 2) order->setValue(beforeOrder == 321 ? 322 : 321);
        QCOMPARE(calls, 1);
        QCOMPARE(history.count(), accepted ? 1 : 0);
        QCOMPARE(rejected.count(), accepted ? 0 : 1);
        QCOMPARE(visibleChanged.count() + opacityChanged.count() + orderChanged.count(), accepted ? 1 : 0);
        QCOMPARE(document.CanUndoEdit(), accepted);
        if (!accepted) {
            QCOMPARE(document.Drawables().front().visible, beforeVisibility);
            QCOMPARE(document.DrawableOpacity(document.Drawables().front()), beforeOpacity);
            QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().front())), beforeOrder);
            QCOMPARE(visibility->isChecked(), beforeVisibility);
            QCOMPARE(opacity->value(), double(beforeOpacity));
            QCOMPARE(order->value(), beforeOrder);
        }
    }

    void historyCommitterRejectsThenCommitsUndoAndRedo()
    {
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString id = document.Drawables().front().id;
        const auto before = document.DrawableKeyformPositions(id, 0);
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, QStringLiteral("artmesh"));
        auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
        QVERIFY(table && table->rowCount());
        table->item(0, 1)->setText(QString::number(before[0] + 0.25f, 'g', 9));
        const auto after = document.DrawableKeyformPositions(id, 0);
        QVERIFY(after != before);
        bool accepted = false;
        QList<bool> directions;
        panel.SetHistoryHandler([&](bool undo, CModelEdit& edit, QString* error) {
            directions.append(undo);
            if (!accepted) { *error = QStringLiteral("history candidate rejected"); return false; }
            return undo ? document.UndoEdit(&edit) : document.RedoEdit(&edit);
        });
        QSignalSpy changed(&panel, &CInspectorPanel::DrawableKeyformPositionsChanged);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        panel.Undo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), after);
        QVERIFY(document.CanUndoEdit() && !document.CanRedoEdit());
        QCOMPARE(changed.count(), 0);
        QCOMPARE(history.count(), 0);
        QCOMPARE(rejected.count(), 1);
        accepted = true;
        panel.Undo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), before);
        QVERIFY(!document.CanUndoEdit() && document.CanRedoEdit());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(history.count(), 1);
        accepted = false;
        panel.Redo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), before);
        QVERIFY(!document.CanUndoEdit() && document.CanRedoEdit());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(history.count(), 1);
        QCOMPARE(rejected.count(), 2);
        accepted = true;
        panel.Redo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), after);
        QCOMPARE(changed.count(), 2);
        QCOMPARE(history.count(), 2);
        QCOMPARE(directions, QList<bool>({true, true, false, false}));
    }

    void keyformCommitterRunsOnlyForUserChoicesWhileBrowsingRemainsAvailable()
    {
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [](const auto& value){ return value.vertexCount && value.keyforms.size() >= 3; });
        QVERIFY(mesh != document.Drawables().cend());
        const auto other = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [&mesh](const auto& value){ return value.id != mesh->id && value.vertexCount && !value.keyforms.isEmpty(); });
        QVERIFY(other != document.Drawables().cend());
        for (const auto& parameter : document.Parameters()) {
            const auto metadata = document.ParameterMetadata(parameter.id);
            const float value = float(double(metadata.minimum) + (double(metadata.maximum) - metadata.minimum) * 0.37);
            QVERIFY(document.SetParameterValue(parameter.id, value));
        }
        const auto parameterValues = document.ParameterValues();
        CInspectorPanel panel;
        panel.SetDocument(&document);
        auto* combo = panel.findChild<QComboBox*>("keyformSelector");
        auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
        QVERIFY(combo && table);
        bool accepted = false;
        QVector<QPair<QString, int>> requests;
        QMap<QString, float> requestedPose;
        panel.SetKeyformHandler([&](const QString& id, int index, QString* error) {
            requests.append({id, index});
            if (!accepted) { *error = QStringLiteral("pose candidate rejected"); return false; }
            return document.KeyformParameterValues(id, QStringLiteral("artmesh"), index, requestedPose, error);
        });
        QSignalSpy selected(&panel, &CInspectorPanel::DrawableKeyformSelected);
        QSignalSpy activated(&panel, &CInspectorPanel::DrawableKeyformActivated);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        panel.SetSelection(mesh->id, QStringLiteral("artmesh"));
        QCOMPARE(requests.size(), 0);
        QCOMPARE(panel.GetSelectionId(), mesh->id);
        QCOMPARE(panel.GetActiveKeyformIndex(), 0);
        QCOMPARE(combo->currentIndex(), 0);
        QCOMPARE(selected.count(), 0);
        QCOMPARE(activated.count(), 1);
        QCOMPARE(rejected.count(), 0);
        panel.RefreshSelection();
        panel.SetSelection(mesh->id, QStringLiteral("artmesh"));
        panel.SetDocument(&document);
        panel.SetLockedState({});
        QCOMPARE(requests.size(), 0);
        combo->setCurrentIndex(2);
        QCoreApplication::processEvents();
        QCOMPARE(requests.size(), 0);
        QCOMPARE(combo->currentIndex(), 0);
        QCOMPARE(panel.GetActiveKeyformIndex(), 0);
        activated.clear();
        QTest::keyClick(combo, Qt::Key_Down);
        QCOMPARE(requests.size(), 1);
        QCOMPARE(requests.last(), qMakePair(mesh->id, 1));
        QCOMPARE(combo->currentIndex(), 0);
        QCOMPARE(panel.GetActiveKeyformIndex(), 0);
        QCOMPARE(selected.count(), 0);
        QCOMPARE(activated.count(), 0);
        accepted = true;
        QTest::keyClick(combo, Qt::Key_Down);
        QCOMPARE(requests.size(), 2);
        QCOMPARE(combo->currentIndex(), 1);
        QCOMPARE(panel.GetActiveKeyformIndex(), 1);
        QCOMPARE(selected.count(), 1);
        QCOMPARE(activated.count(), 1);
        QMap<QString, float> expectedPose;
        QVERIFY(document.KeyformParameterValues(mesh->id, QStringLiteral("artmesh"), 1, expectedPose));
        QCOMPARE(requestedPose, expectedPose);
        const auto selectedPositions = document.DrawableKeyformPositions(mesh->id, 1);
        QCOMPARE(table->item(0, 1)->text(), QString::number(selectedPositions[0], 'f', 6));
        panel.RefreshSelection();
        QVERIFY(QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, 1)));
        QCOMPARE(requests.size(), 2);
        accepted = false;
        const int acceptedSignals = activated.count();
        panel.SetLockedState({QStringLiteral("artmesh|") + other->id});
        const int refreshedSignals = activated.count();
        QVERIFY(refreshedSignals >= acceptedSignals);
        panel.SetSelection(other->id, QStringLiteral("artmesh"));
        QCOMPARE(requests.size(), 2);
        QCOMPARE(panel.GetSelectionId(), other->id);
        QCOMPARE(panel.GetActiveKeyformIndex(), 0);
        QCOMPARE(combo->currentIndex(), 0);
        QVERIFY(!combo->isEnabled());
        QCOMPARE(activated.count(), refreshedSignals + 1);
        QCOMPARE(selected.count(), 1);
        QCOMPARE(history.count(), 0);
        QCOMPARE(rejected.count(), 1);
        QVERIFY(!document.CanUndoEdit());
        QCOMPARE(document.ParameterValues(), parameterValues);
    }

    void unsupportedBlendShapeCanBeInspectedWithoutActivatingIt()
    {
        using namespace live2dstruct;
        CModelDocument original;
        const QString path = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
        QVERIFY(original.Load(path));
        QByteArray bytes = original.SourceBytes();
        const int typeOffset = bytes.size();
        for (int index = 0; index < original.Parameters().size(); ++index) {
            char value[4];
            qToLittleEndian<quint32>(1, reinterpret_cast<uchar*>(value));
            bytes.append(value, 4);
        }
        qToLittleEndian<quint32>(typeOffset, reinterpret_cast<uchar*>(bytes.data() + 0x208));
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(bytes, path, &error), qPrintable(error));
        QVERIFY(std::all_of(document.Parameters().cbegin(), document.Parameters().cend(),
                            [](const auto& parameter){ return parameter.type == 1; }));
        const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [](const auto& drawable){ return drawable.vertexCount && drawable.keyforms.size() > 1; });
        QVERIFY(mesh != document.Drawables().cend());
        QMap<QString, float> pose;
        QVERIFY(!document.KeyformParameterValues(mesh->id, QStringLiteral("artmesh"), 0, pose, &error));
        QVERIFY2(error.contains(QStringLiteral("类型 1")), qPrintable(error));
        CInspectorPanel panel;
        panel.SetDocument(&document);
        int requests = 0;
        panel.SetKeyformHandler([&](const QString& id, int keyform, QString* error) {
            ++requests;
            return document.KeyformParameterValues(id, QStringLiteral("artmesh"), keyform, pose, error);
        });
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        panel.SetSelection(mesh->id, QStringLiteral("artmesh"));
        QCOMPARE(panel.GetSelectionId(), mesh->id);
        QCOMPARE(panel.GetActiveKeyformIndex(), 0);
        QCOMPARE(requests, 0);
        QCOMPARE(rejected.count(), 0);
        auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
        auto* combo = panel.findChild<QComboBox*>("keyformSelector");
        QVERIFY(table && combo);
        QCOMPARE(table->rowCount(), mesh->vertexCount);
        QTest::keyClick(combo, Qt::Key_Down);
        QCOMPARE(requests, 1);
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(combo->currentIndex(), 0);
        QCOMPARE(panel.GetSelectionId(), mesh->id);
        QVERIFY(!document.CanUndoEdit());
    }

    void otherEditPathsUseInjectedCommitter_data()
    {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<bool>("accepted");
        const QStringList operations{QStringLiteral("rotation"), QStringLiteral("warp"), QStringLiteral("parent"),
            QStringLiteral("inversion"), QStringLiteral("masks"), QStringLiteral("pasteTopology"),
            QStringLiteral("mergeSelectedVertices"), QStringLiteral("autoTriangulate")};
        for (const auto& operation : operations)
            for (bool accepted : {false, true})
                QTest::newRow(qPrintable(operation + (accepted ? QStringLiteral("-accept") : QStringLiteral("-reject"))))
                    << operation << accepted;
    }

    void otherEditPathsUseInjectedCommitter()
    {
        QFETCH(QString, operation);
        QFETCH(bool, accepted);
        using namespace live2dstruct;
        CModelDocument document;
        const bool masks = operation == QLatin1String("masks") || operation == QLatin1String("inversion");
        QVERIFY(document.Load(QString(TEST_RESOURCES) + (masks ? "/Rice/Rice.moc3" : "/Mark/Mark.moc3")));
        QString id, type = QStringLiteral("artmesh");
        if (operation == QLatin1String("rotation") || operation == QLatin1String("warp") || operation == QLatin1String("parent")) {
            const auto deformer = std::find_if(document.Deformers().cbegin(), document.Deformers().cend(), [&](const auto& value) {
                if (operation == QLatin1String("rotation")) return !value.rotationKeyforms.isEmpty();
                if (operation == QLatin1String("warp")) return !value.warpKeyforms.isEmpty();
                return value.parentDeformerIndex >= 0 && !value.warpKeyforms.isEmpty();
            });
            QVERIFY(deformer != document.Deformers().cend());
            id = deformer->id;
            type = operation == QLatin1String("rotation") ? QStringLiteral("rotation") : QStringLiteral("warp");
        } else {
            const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(), [&](const auto& value) {
                return value.vertexCount >= 5 && !value.keyforms.isEmpty() && (!masks || !value.maskIndices.isEmpty());
            });
            QVERIFY(mesh != document.Drawables().cend());
            id = mesh->id;
            if (!masks) QVERIFY(document.SetDrawableIndices(id, {0, 2, 3, 0, 3, 4, 1, 2, 3}));
            if (operation == QLatin1String("autoTriangulate")) {
                auto positions = document.DrawableKeyformPositions(id, 0);
                for (int i = 0; i < positions.size() / 2; ++i) {
                    positions[i * 2] = float(i) * 0.01f;
                    positions[i * 2 + 1] = float(i) * float(i) * 0.001f;
                }
                QVERIFY(document.SetDrawableKeyformPositions(id, 0, positions));
                QVERIFY(document.SetDrawableIndices(id, {0, 1, 2}));
            }
        }
        const auto rotationBefore = document.DeformerRotationKeyforms(id);
        const auto warpBefore = document.DeformerWarpKeyforms(id);
        const auto parentBefore = document.DeformerParentIndex(id);
        const auto topologyBefore = document.DrawableIndices(id);
        const auto masksBefore = document.DrawableMaskIndices(id);
        const auto inversionBefore = document.DrawableMaskInverted(id);
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, type);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        QSignalSpy topology(&panel, &CInspectorPanel::DrawableIndicesChanged);
        QSignalSpy maskChanges(&panel, &CInspectorPanel::DrawableMaskIndicesChanged);
        int calls = 0;
        CModelEdit submitted;
        panel.SetEditHandler([&](const CModelEdit& edit, QString* error) {
            ++calls;
            submitted = edit;
            if (!accepted) { *error = QStringLiteral("rejected candidate"); return false; }
            return document.ExecuteEdit(edit);
        });
        if (operation == QLatin1String("rotation")) {
            auto* table = panel.findChild<QTableWidget*>("rotationKeyformTable");
            QVERIFY(table && table->rowCount());
            table->item(0, 1)->setText(QString::number(rotationBefore[0].angle + 1.0f, 'g', 9));
        } else if (operation == QLatin1String("warp")) {
            auto* table = panel.findChild<QTableWidget*>("warpKeyformTable");
            QVERIFY(table && table->rowCount());
            table->item(0, 1)->setText(QString::number(warpBefore[0].positions[0] + 0.25f, 'g', 9));
        } else if (operation == QLatin1String("parent")) {
            auto* selector = panel.findChild<QComboBox*>("deformerParentSelector");
            QVERIFY(selector && selector->findData(-1) >= 0);
            selector->setCurrentIndex(selector->findData(-1));
        } else if (operation == QLatin1String("inversion")) {
            auto* checkbox = panel.findChild<QCheckBox*>("maskInverted");
            QVERIFY(checkbox && checkbox->isEnabled());
            checkbox->click();
        } else if (operation == QLatin1String("masks")) {
            bool answered = false;
            QTimer::singleShot(0, &panel, [&] {
                auto* dialog = panel.findChild<QDialog*>();
                if (!dialog) return;
                auto* list = dialog->findChild<QListWidget*>();
                if (!list) { dialog->reject(); return; }
                for (int row = 0; row < list->count(); ++row) list->item(row)->setCheckState(Qt::Unchecked);
                answered = true;
                dialog->accept();
            });
            panel.findChild<QPushButton*>("editMasks")->click();
            QVERIFY(answered);
        } else if (operation == QLatin1String("pasteTopology")) {
            panel.findChild<QPushButton*>("copyTopology")->click();
            QCOMPARE(calls, 0);
            QJsonObject object = QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8()).object();
            object.insert(QStringLiteral("indices"), QJsonArray{0, 1, 2});
            QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(object).toJson()));
            panel.findChild<QPushButton*>("pasteTopology")->click();
        } else if (operation == QLatin1String("autoTriangulate")) {
            QTimer dialogGuard;
            dialogGuard.setSingleShot(true);
            connect(&dialogGuard, &QTimer::timeout, &panel, [&] {
                if (auto* dialog = panel.findChild<QMessageBox*>()) dialog->reject();
            });
            dialogGuard.start(0);
            panel.findChild<QPushButton*>("autoTriangulate")->click();
        } else {
            auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
            table->setRangeSelected(QTableWidgetSelectionRange(0, 0, 1, 2), true);
            panel.findChild<QPushButton*>("mergeSelectedVertices")->click();
        }
        QCOMPARE(calls, 1);
        QCOMPARE(history.count(), accepted ? 1 : 0);
        QCOMPARE(rejected.count(), accepted ? 0 : 1);
        QCOMPARE(document.CanUndoEdit(), accepted);
        if (accepted) {
            CModelEdit committed;
            QVERIFY(document.PeekUndoEdit(&committed));
            QCOMPARE(committed.name, submitted.name);
            QCOMPARE(committed.field, submitted.field);
        } else {
            QCOMPARE(document.DeformerRotationKeyforms(id), rotationBefore);
            QCOMPARE(document.DeformerWarpKeyforms(id), warpBefore);
            QCOMPARE(document.DeformerParentIndex(id), parentBefore);
            QCOMPARE(document.DrawableIndices(id), topologyBefore);
            QCOMPARE(document.DrawableMaskIndices(id), masksBefore);
            QCOMPARE(document.DrawableMaskInverted(id), inversionBefore);
            QCOMPARE(topology.count(), 0);
            QCOMPARE(maskChanges.count(), 0);
        }
    }

    void multiSelectionStopsAtFirstRejectedCommit()
    {
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        QVERIFY(document.Drawables().size() >= 3);
        QStringList ids;
        QVector<float> before;
        for (int i = 0; i < 3; ++i) {
            ids.append(document.Drawables()[i].id);
            before.append(document.DrawableOpacity(document.Drawables()[i]));
        }
        const float after = std::all_of(before.cbegin(), before.cend(), [](float value){ return value != 0.25f; }) ? 0.25f : 0.75f;
        QVERIFY(std::all_of(before.cbegin(), before.cend(), [after](float value){ return value != after; }));
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelections(ids, {QStringLiteral("artmesh"), QStringLiteral("artmesh"), QStringLiteral("artmesh")});
        QStringList submitted;
        panel.SetEditHandler([&](const CModelEdit& edit, QString* error) {
            submitted.append(edit.name);
            if (submitted.size() == 2) { *error = QStringLiteral("second candidate failed"); return false; }
            return document.ExecuteEdit(edit);
        });
        QSignalSpy changed(&panel, &CInspectorPanel::DrawableOpacityChanged);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        panel.findChild<QDoubleSpinBox*>()->setValue(after);
        QCOMPARE(submitted, ids.mid(0, 2));
        QCOMPARE(document.DrawableOpacity(document.Drawables()[0]), after);
        QCOMPARE(document.DrawableOpacity(document.Drawables()[1]), before[1]);
        QCOMPARE(document.DrawableOpacity(document.Drawables()[2]), before[2]);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(history.count(), 1);
        QCOMPARE(rejected.count(), 1);
        panel.Undo();
        QCOMPARE(document.DrawableOpacity(document.Drawables()[0]), before[0]);
        QVERIFY(!document.CanUndoEdit());
    }

    void rejectedEditExceptionRestoresTableWithoutPublishing()
    {
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString id = document.Drawables().front().id;
        const auto positions = document.DrawableKeyformPositions(id, 0);
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, QStringLiteral("artmesh"));
        panel.SetEditHandler([](const CModelEdit&, QString*) -> bool { throw std::runtime_error("upload failed"); });
        auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        QSignalSpy changed(&panel, &CInspectorPanel::DrawableKeyformPositionsChanged);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        table->item(0, 1)->setText(QString::number(positions[0] + 1.0f));
        QCOMPARE(rejected.count(), 1);
        QVERIFY(rejected.first().first().toString().contains(QStringLiteral("upload failed")));
        QCOMPARE(changed.count(), 0);
        QCOMPARE(history.count(), 0);
        QCOMPARE(document.DrawableKeyformPositions(id, 0), positions);
        QCOMPARE(table->item(0, 1)->text(), QString::number(positions[0], 'f', 6));
        QVERIFY(!document.CanUndoEdit());
        panel.SetEditHandler({});
        table->item(0, 1)->setText(QString::number(positions[0] + 1.0f));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(history.count(), 1);
        QVERIFY(document.CanUndoEdit());
    }

    void clearingMasksPreservesInversionInOneHistoryEntry_data()
    {
        QTest::addColumn<bool>("sourceInverted");
        QTest::newRow("source-inverted") << true;
        QTest::newRow("user-inverted") << false;
    }

    void clearingMasksPreservesInversionInOneHistoryEntry()
    {
        QFETCH(bool, sourceInverted);
        using namespace live2dstruct;
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Rice/Rice.moc3", &error), qPrintable(error));
        const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [sourceInverted](const CModelDrawable& drawable) {
                return !drawable.maskIndices.isEmpty() && ((drawable.flags & 0x08) != 0) == sourceInverted;
            });
        QVERIFY(mesh != document.Drawables().cend());
        const QString id = mesh->id;
        const QVector<int> originalMasks = document.DrawableMaskIndices(id);
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, QStringLiteral("artmesh"));
        auto* inverted = panel.findChild<QCheckBox*>("maskInverted");
        auto* editMasks = panel.findChild<QPushButton*>("editMasks");
        QVERIFY(inverted && editMasks && inverted->isEnabled() && editMasks->isEnabled());
        QCOMPARE(inverted->isChecked(), sourceInverted);
        inverted->click();
        if (sourceInverted) panel.Undo();
        QVERIFY(document.DrawableMaskInverted(id));
        QVERIFY(inverted->isChecked());
        panel.ClearHistory();
        QVERIFY(!document.CanUndoEdit());
        QSignalSpy changed(&panel, &CInspectorPanel::DrawableMaskIndicesChanged);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        bool clearedInDialog = false;
        QTimer responder;
        responder.setSingleShot(true);
        connect(&responder, &QTimer::timeout, &panel, [&] {
            auto* dialog = panel.findChild<QDialog*>();
            if (!dialog) return;
            auto* list = dialog->findChild<QListWidget*>();
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            if (!list || !buttons || !buttons->button(QDialogButtonBox::Ok)) {
                dialog->reject();
                return;
            }
            for (int row = 0; row < list->count(); ++row) list->item(row)->setCheckState(Qt::Unchecked);
            clearedInDialog = true;
            buttons->button(QDialogButtonBox::Ok)->click();
        });
        responder.start(0);
        editMasks->click();
        QVERIFY(clearedInDialog);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(history.size(), 1);
        QVERIFY(document.DrawableMaskIndices(id).isEmpty());
        QVERIFY(document.DrawableMaskInverted(id));
        QVERIFY(inverted->isChecked());
        QVERIFY(!inverted->isEnabled());
        QVERIFY(document.Validate().isEmpty());
        panel.Undo();
        QCOMPARE(document.DrawableMaskIndices(id), originalMasks);
        QVERIFY(inverted->isChecked() && inverted->isEnabled());
        QVERIFY(!document.CanUndoEdit());
        QCOMPARE(changed.size(), 2);
        panel.Redo();
        QVERIFY(document.DrawableMaskIndices(id).isEmpty());
        QVERIFY(inverted->isChecked() && !inverted->isEnabled());
        QVERIFY(document.DrawableMaskInverted(id));
        QVERIFY(!document.CanRedoEdit());
        QCOMPARE(changed.size(), 3);
        QVERIFY(document.Validate().isEmpty());
    }

    void inspectorPreservesNativeInvertedMasksThroughHistory()
    {
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Rice/Rice.moc3", &error), qPrintable(error));
        const QString maskedId = QStringLiteral("ArtMesh50");
        QVERIFY(!document.DrawableMaskIndices(maskedId).isEmpty());
        QVERIFY(document.DrawableMaskInverted(maskedId));
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(maskedId, QStringLiteral("artmesh"));
        auto* inverted = panel.findChild<QCheckBox*>("maskInverted");
        QVERIFY(inverted && inverted->isEnabled() && inverted->isChecked());
        inverted->click();
        QVERIFY(!document.DrawableMaskInverted(maskedId));
        QVERIFY(!inverted->isChecked());
        panel.Undo();
        QVERIFY(document.DrawableMaskInverted(maskedId));
        QVERIFY(inverted->isChecked());
        panel.Redo();
        QVERIFY(!document.DrawableMaskInverted(maskedId));
        QVERIFY(!inverted->isChecked());
        const QString unmaskedId = QStringLiteral("ArtMesh168");
        QVERIFY(document.DrawableMaskIndices(unmaskedId).isEmpty());
        panel.SetSelection(unmaskedId, QStringLiteral("artmesh"));
        QVERIFY(inverted->isChecked());
        QVERIFY(!inverted->isEnabled());
    }

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
