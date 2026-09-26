#include "ui/cparampanel.h"
#include "ui/cxyparameterpad.h"
#include "ui/theme.h"
#include "live2dstruct/cmodeldocument.h"
#include "ui/para/ccidjson.h"
#include <QtTest>
#include <QComboBox>
#include <QGroupBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>
#include <QApplication>
#include <QMainWindow>
#include <QClipboard>
#include <QInputDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QCheckBox>
#include <QFormLayout>
#include <QStyleFactory>
#include <QStyleOptionButton>
#include <QTimer>
#include <QMimeData>
#include <QListWidget>
#include <QTreeWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QScopeGuard>
#include <stdexcept>
#include <memory>
#include <QtEndian>
#include <cmath>
#include <limits>
#include <cstring>

class ParamPanelTests : public QObject
{
    Q_OBJECT
private:
    static int structureDialog(CParamPanel& panel, const char* buttonName,
                               const std::function<void(QDialog*)>& edit,
                               const std::function<void(QDialog*)>& retained = {}, bool save = true)
    {
        panel.resize(800, 600);
        panel.show();
        panel.activateWindow();
        QCoreApplication::processEvents();
        if (auto* toggle = panel.findChild<QToolButton*>("parameterActionsToggle")) toggle->setChecked(true);
        QCoreApplication::processEvents();
        const auto visited = std::make_shared<int>(0);
        QTimer::singleShot(0, &panel, [edit, retained, save, visited] {
            QPointer<QDialog> dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog || dialog->objectName() != QLatin1String("parameterStructureDialog")) return;
            ++*visited;
            const auto cleanup = qScopeGuard([dialog] { if (dialog && dialog->isVisible()) dialog->reject(); });
            if (edit) edit(dialog);
            if (!dialog) return;
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            QVERIFY(buttons);
            QTest::mouseClick(buttons->button(save ? QDialogButtonBox::Save : QDialogButtonBox::Cancel), Qt::LeftButton);
            if (dialog && dialog->isVisible() && retained) retained(dialog);
        });
        auto* button = panel.findChild<QToolButton*>(QString::fromLatin1(buttonName));
        if (!button || !button->isEnabled()) return 0;
        QTest::mouseClick(button, Qt::LeftButton);
        return *visited;
    }

    static int metadataDialog(CParamPanel& panel, const QString& id,
                              const std::function<void(QDialog*)>& edit = {}, bool save = true,
                              const std::function<void(QDialog*)>& retained = {}, bool keyboardSave = false)
    {
        const auto visited = std::make_shared<int>(0);
        QTimer::singleShot(0, &panel, [&panel, id, edit, save, retained, keyboardSave, visited] {
            auto* picker = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            if (!picker || picker->objectName() != QLatin1String("metadataParameterPicker")) return;
            ++*visited;
            picker->setTextValue(id);
            QTimer::singleShot(0, &panel, [edit, save, retained, keyboardSave, visited] {
                QPointer<QDialog> dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog || dialog->objectName() != QLatin1String("parameterMetadataDialog")) return;
                ++*visited;
                if (edit) edit(dialog);
                if (!dialog) return;
                auto* buttons = dialog->findChild<QDialogButtonBox*>();
                if (!buttons) { dialog->reject(); return; }
                if (save && keyboardSave)
                    QTest::keyClick(dialog->focusWidget() ? dialog->focusWidget() : dialog.data(), Qt::Key_Return);
                else QTest::mouseClick(buttons->button(save ? QDialogButtonBox::Save : QDialogButtonBox::Cancel), Qt::LeftButton);
                if (dialog && dialog->isVisible()) {
                    if (retained) retained(dialog);
                    if (dialog && dialog->isVisible()) QTest::mouseClick(buttons->button(QDialogButtonBox::Cancel), Qt::LeftButton);
                }
            });
            picker->accept();
        });
        panel.EditParameterMetadata();
        return *visited;
    }

    static void metadataText(QDialog* dialog, const char* name, const QString& text)
    {
        auto* edit = dialog->findChild<QLineEdit*>(QString::fromLatin1(name));
        QVERIFY(edit);
        edit->setText(text);
    }

    static bool sameFloat(float left, float right)
    {
        return std::memcmp(&left, &right, sizeof(float)) == 0;
    }

    static void initialize(CParamPanel& panel, live2dstruct::CModelDocument& document)
    {
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
        panel.SetDocument(&document);
        const QSignalBlocker xBlocker(panel.m_xyXSelector);
        const QSignalBlocker yBlocker(panel.m_xyYSelector);
        for (const auto& parameter : document.Parameters()) {
            const auto metadata = document.ParameterMetadata(parameter.id);
            if (metadata.minimum == metadata.maximum) continue;
            CParamPanel::ParamRow row;
            row.widget = new QWidget(panel.m_container);
            row.slider = new QSlider(Qt::Horizontal, row.widget);
            row.slider->setRange(0, 1000);
            row.spinBox = new QDoubleSpinBox(row.widget);
            row.spinBox->setRange(metadata.minimum, metadata.maximum);
            row.spinBox->setDecimals(6);
            row.resetButton = new QToolButton(row.widget);
            row.nameLabel = new QLabel(row.widget);
            row.minVal = metadata.minimum;
            row.maxVal = metadata.maximum;
            row.defaultVal = metadata.defaultValue;
            row.paramId = parameter.id;
            panel.m_rows.append(row);
            panel.m_xyXSelector->addItem(row.paramId);
            panel.m_xyYSelector->addItem(row.paramId);
            panel.ApplyExternalValue(row.paramId, row.defaultVal, false, false);
            if (panel.m_rows.size() == 2) break;
        }
        QCOMPARE(panel.m_rows.size(), 2);
        panel.m_xyXSelector->setCurrentIndex(0);
        panel.m_xyYSelector->setCurrentIndex(1);
        panel.RefreshXYControls();
        document.ClearEditHistory();
    }

    static QMap<QString, float> valuesAt(const CParamPanel& panel, float fraction)
    {
        QMap<QString, float> values;
        for (const auto& row : panel.m_rows)
            values.insert(row.paramId, row.minVal + fraction * (row.maxVal - row.minVal));
        return values;
    }

    static void restoreOnEdit(CParamPanel& panel, const QMap<QString, float>& baseline)
    {
        for (auto it = baseline.cbegin(); it != baseline.cend(); ++it)
            QVERIFY(panel.m_document->SetParameterValue(it.key(), it.value()));
        QObject::connect(&panel, &CParamPanel::ParamChanged, &panel,
                         [&panel, baseline](const QString& id, float, bool isInit){
            if (!isInit && panel.m_previewActive) panel.ClearPreviewValues(baseline, id);
        });
    }

private slots:
    void compactWorkspaceKeepsControlsReachable_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<bool>("dark");
        for (const bool dark : {false, true}) {
            QTest::newRow(dark ? "dark-sidebar" : "light-sidebar") << QSize(300, 420) << dark;
            QTest::newRow(dark ? "dark-short" : "light-short") << QSize(700, 300) << dark;
        }
    }

    void compactWorkspaceKeepsControlsReachable()
    {
        QFETCH(QSize, size);
        QFETCH(bool, dark);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel;
        panel.setStyleSheet(Theme::globalStyleSheet(dark));
        panel.SetDocument(&document);
        panel.BuildParams();
        QVERIFY(panel.m_rows.size() > 2);
        panel.m_xyXSelector->setCurrentIndex(1);
        panel.m_xyYSelector->setCurrentIndex(2);
        panel.ApplyExternalValue(panel.m_rows.first().paramId, panel.m_rows.first().maxVal, true);
        const auto values = panel.CurrentValues();
        const auto revision = document.Revision();
        const bool undo = document.CanUndoEdit(), redo = document.CanRedoEdit();
        const QString x = panel.m_xyXSelector->currentText(), y = panel.m_xyYSelector->currentText();
        const float padX = panel.m_xyPad->XValue(), padY = panel.m_xyPad->YValue();
        QSignalSpy changes(&panel, &CParamPanel::ParamChanged), history(&panel, &CParamPanel::HistoryChanged);
        auto* outer = panel.findChild<QScrollArea*>("parameterWorkspaceScroll");
        auto* actions = panel.findChild<QToolButton*>("parameterActionsToggle");
        auto* xy = panel.findChild<QToolButton*>("parameterXYToggle");
        QVERIFY(outer && actions && xy);
        QCOMPARE(panel.widget(), outer);
        QCOMPARE(outer->frameShape(), QFrame::NoFrame);
        QVERIFY(outer->widgetResizable());
        panel.resize(size);
        panel.show();
        panel.activateWindow();
        QCoreApplication::processEvents();
        QCOMPARE(panel.size(), size);
        QVERIFY(!actions->isChecked() && !xy->isChecked());
        QVERIFY(panel.findChild<QWidget*>("parameterActionsSection")->isHidden());
        QVERIFY(panel.m_xyBox->isHidden());
        QVERIFY(outer->viewport()->rect().contains(panel.m_search->mapTo(outer->viewport(), panel.m_search->rect().center())));
        QVERIFY(panel.m_scroll->viewport()->height() >= panel.m_rows.first().spinBox->height());
        if (!qEnvironmentVariableIsEmpty("WORKSPACE_SCREENSHOT_DIR"))
            QVERIFY(panel.grab().save(qEnvironmentVariable("WORKSPACE_SCREENSHOT_DIR") +
                QStringLiteral("/param-%1-%2x%3-collapsed.png").arg(dark ? "dark" : "light").arg(size.width()).arg(size.height())));
        QTest::mouseClick(actions, Qt::LeftButton);
        outer->ensureWidgetVisible(xy);
        QTest::mouseClick(xy, Qt::LeftButton);
        QCoreApplication::processEvents();
        QVERIFY(actions->isChecked() && xy->isChecked());
        QCOMPARE(panel.size(), size);
        QVERIFY(outer->verticalScrollBar()->maximum() > 0);
        QVERIFY(panel.m_xyXSelector->geometry().bottom() < panel.m_xyYSelector->geometry().top());
        QVERIFY(panel.m_xyYSelector->geometry().bottom() < panel.m_xyPad->geometry().top());
        panel.m_xyYSelector->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY(panel.m_xyYSelector->hasFocus());
        QTest::keyClick(panel.m_xyYSelector, Qt::Key_Tab);
        QTRY_VERIFY(panel.m_xyPad->hasFocus());
        QVERIFY(outer->viewport()->rect().contains(panel.m_xyPad->mapTo(outer->viewport(), panel.m_xyPad->rect().center())));
        if (!qEnvironmentVariableIsEmpty("WORKSPACE_SCREENSHOT_DIR"))
            QVERIFY(panel.grab().save(qEnvironmentVariable("WORKSPACE_SCREENSHOT_DIR") +
                QStringLiteral("/param-%1-%2x%3-expanded.png").arg(dark ? "dark" : "light").arg(size.width()).arg(size.height())));
        outer->ensureWidgetVisible(xy);
        QTest::mouseClick(xy, Qt::LeftButton);
        QTest::keyClick(xy, Qt::Key_Space);
        QVERIFY(xy->isChecked());
        QCOMPARE(panel.m_xyXSelector->currentText(), x);
        QCOMPARE(panel.m_xyYSelector->currentText(), y);
        QCOMPARE(panel.m_xyPad->XValue(), padX);
        QCOMPARE(panel.m_xyPad->YValue(), padY);
        QVERIFY(panel.FocusParameter(panel.m_rows.last().paramId));
        auto* spin = panel.m_rows.last().spinBox;
        QTRY_VERIFY(spin->hasFocus());
        QCoreApplication::processEvents();
        QVERIFY(panel.m_scroll->viewport()->rect().contains(spin->mapTo(panel.m_scroll->viewport(), spin->rect().center())));
        QVERIFY(outer->viewport()->rect().contains(spin->mapTo(outer->viewport(), spin->rect().center())));
        QCOMPARE(panel.CurrentValues(), values);
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(document.CanUndoEdit(), undo);
        QCOMPARE(document.CanRedoEdit(), redo);
        QCOMPARE(changes.size(), 0);
        QCOMPARE(history.size(), 0);
    }

    void destroyingFocusedFloatingEditorDoesNotPublish_data()
    {
        QTest::addColumn<int>("destruction");
        QTest::newRow("panel") << 0;
        QTest::newRow("parent") << 1;
        QTest::newRow("remove-dock") << 2;
    }

    void destroyingFocusedFloatingEditorDoesNotPublish()
    {
        QFETCH(int, destruction);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        auto window = std::make_unique<QMainWindow>();
        QPointer<CParamPanel> panel = new CParamPanel(window.get());
        panel->SetDocument(&document);
        panel->BuildParams();
        window->addDockWidget(Qt::RightDockWidgetArea, panel);
        window->show();
        panel->setFloating(true);
        panel->resize(600, 900);
        panel->show();
        panel->activateWindow();
        auto* spin = panel->m_rows.first().spinBox;
        auto* editor = spin->findChild<QLineEdit*>();
        QVERIFY(editor);
        spin->setFocus();
        QTRY_VERIFY(spin->hasFocus());
        editor->selectAll();
        QTest::keyClicks(editor, "1");
        QTest::keyClick(editor, Qt::Key_Return);
        QVERIFY(panel->m_userLocked.contains(0));
        editor->selectAll();
        QTest::keyClicks(editor, "2");
        QVERIFY(spin->hasFocus());
        const auto revision = document.Revision();
        QSignalSpy changes(panel, &CParamPanel::ParamChanged);
        QSignalSpy history(panel, &CParamPanel::HistoryChanged);
        if (destruction == 2) window->removeDockWidget(panel);
        if (destruction == 1) window.reset();
        else delete panel.data();
        QCoreApplication::processEvents();
        QVERIFY(!panel);
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(changes.size(), 0);
        QCOMPARE(history.size(), 0);
    }

    void previewHistoryRestoresRuntimeAndDocumentBaselinesSeparately_data()
    {
        QTest::addColumn<bool>("explicitBaseline");
        QTest::newRow("different-runtime-baseline") << false;
        QTest::newRow("stored-runtime-baseline") << true;
    }

    void previewHistoryRestoresRuntimeAndDocumentBaselinesSeparately()
    {
        QFETCH(bool, explicitBaseline);
        live2dstruct::CModelDocument document;
        CParamPanel panel;
        initialize(panel, document);
        const auto runtimeBaseline = valuesAt(panel, 0.25f);
        const auto preview = valuesAt(panel, 0.75f);
        const auto edited = valuesAt(panel, 0.6f);
        const QString id = panel.m_rows.first().paramId;
        if (explicitBaseline) {
            for (auto it = runtimeBaseline.cbegin(); it != runtimeBaseline.cend(); ++it)
                QVERIFY(document.SetParameterValue(it.key(), it.value()));
        }
        const auto original = document.ParameterStructureState();
        const float documentBaseline = document.ParameterValue(id);
        panel.SetPreviewValues(preview, runtimeBaseline);
        panel.ApplyExternalValue(id, edited.value(id), true);
        live2dstruct::CModelEdit command;
        QVERIFY(document.PeekUndoEdit(&command));
        QCOMPARE(command.before.toFloat(), runtimeBaseline.value(id));
        QCOMPARE(document.ParameterValue(id), edited.value(id));
        panel.Undo();
        QCOMPARE(document.ParameterStructureState(), original);
        QCOMPARE(document.ParameterValue(id), documentBaseline);
        QCOMPARE(panel.CurrentValues().value(id), runtimeBaseline.value(id));
        panel.Redo();
        QCOMPARE(document.ParameterValue(id), edited.value(id));
        QCOMPARE(panel.CurrentValues().value(id), edited.value(id));
    }

    void recordedValuePathsPreserveStructuralHistory_data()
    {
        QTest::addColumn<int>("path");
        QTest::newRow("external") << 0;
        QTest::newRow("preset") << 1;
        QTest::newRow("spin") << 2;
        QTest::newRow("slider") << 3;
        QTest::newRow("drag") << 4;
    }

    void recordedValuePathsPreserveStructuralHistory()
    {
        QFETCH(int, path);
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        const auto original = document.ParameterStructureState();
        live2dstruct::CMocParameterStructureBuilder::CreateRequest request;
        request.parameter.id = "ParamPanelHistory";
        live2dstruct::CModelEdit create;
        QVERIFY(document.MakeCreateParameterEdit(request, {}, create, &error));
        QVERIFY(document.ExecuteEdit(create));
        panel.ReconcileParameters();
        const auto boundary = document.ParameterStructureState();
        auto& row = panel.m_rows.first();
        const QString id = row.paramId;
        const float value = row.maxVal;
        QVERIFY(value != document.ParameterValue(id));
        if (path == 0) panel.ApplyExternalValue(id, value, true);
        else if (path == 1) panel.ApplyValuesInternal({{id, value}}, true);
        else if (path == 2) row.spinBox->setValue(value);
        else if (path == 3) row.slider->setValue(row.slider->maximum());
        else {
            panel.BeginParameterDrag(0);
            row.slider->setValue(row.slider->maximum());
            panel.EndParameterDrag(0);
        }
        QCOMPARE(document.ParameterValue(id), value);
        const auto changed = document.ParameterStructureState();
        QVERIFY(changed != boundary);
        QSignalSpy rejected(&panel, &CParamPanel::EditRejected);
        panel.Undo();
        QCOMPARE(document.ParameterStructureState(), boundary);
        panel.Undo();
        QCOMPARE(document.ParameterStructureState(), original);
        panel.Redo();
        QCOMPARE(document.ParameterStructureState(), boundary);
        panel.Redo();
        QCOMPARE(document.ParameterStructureState(), changed);
        QCOMPARE(rejected.size(), 0);
    }

    void structureLongIdsKeepFieldsAndActionsReachable_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("deleting");
        QTest::addColumn<int>("width");
        for (bool dark : {false, true}) for (bool deleting : {false, true}) for (int width : {280, 560})
            QTest::newRow(qPrintable(QString("%1-%2-%3").arg(dark).arg(deleting).arg(width))) << dark << deleting << width;
    }

    void structureLongIdsKeepFieldsAndActionsReachable()
    {
        QFETCH(bool, dark);
        QFETCH(bool, deleting);
        QFETCH(int, width);
        const auto previousStyle = qApp->styleSheet();
        const auto restoreStyle = qScopeGuard([previousStyle] { qApp->setStyleSheet(previousStyle); });
        qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error));
        live2dstruct::CMocParameterStructureBuilder::CreateRequest request;
        request.parameter.id = "Param" + QString(58, QLatin1Char('N'));
        live2dstruct::CModelEdit create;
        QVERIFY(document.MakeCreateParameterEdit(request, QStringLiteral("很长的友好显示名称"), create, &error));
        QVERIFY(document.ExecuteEdit(create));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        panel.SetBindingTargets({{live2dstruct::CMocParameterStructureBuilder::Owner::ArtMesh, document.Drawables().first().id}});
        panel.SetParameterReferenceProvider([](const QString&) { return QStringList{QStringLiteral("动作草稿仍引用此参数，处理引用后可重新检查")}; });
        const auto original = document.ParameterStructureState();
        QCOMPARE(structureDialog(panel, deleting ? "deleteParameter" : "bindParameter", [&](QDialog* dialog) {
            dialog->resize(width, 430);
            auto* parameter = dialog->findChild<QComboBox*>("structureParameter");
            QVERIFY(parameter);
            parameter->setCurrentIndex(parameter->findData(request.parameter.id));
            QCoreApplication::processEvents();
            auto* scroll = dialog->findChild<QScrollArea*>();
            QVERIFY(scroll);
            QCOMPARE(dialog->width(), width);
            QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
            QCOMPARE(parameter->toolTip(), parameter->currentText());
            QVERIFY(parameter->toolTip().contains(request.parameter.id));
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            QVERIFY(dialog->rect().contains(QRect(buttons->mapTo(dialog, QPoint()), buttons->size())));
            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
            QVERIFY(buttons->button(QDialogButtonBox::Cancel)->isVisible());
        }, {}, false), 1);
        QCOMPARE(document.ParameterStructureState(), original);
    }

    void structureCreateAndBindUseOneHistoryStepAndSilentRefresh()
    {
        using Builder = live2dstruct::CMocParameterStructureBuilder;
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const auto source = document.SourceBytes(), digest = document.SourceDigest();
        const auto target = document.Drawables().front();
        const int parameterCount = document.Parameters().size();
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        panel.SetBindingTargets({{Builder::Owner::ArtMesh, target.id}});
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged), structure(&panel, &CParamPanel::StructureChanged);
        QCOMPARE(structureDialog(panel, "createParameter", [](QDialog* dialog) {
            metadataText(dialog, "structureParameterId", "ParamPanelCreated");
            metadataText(dialog, "structureDisplayName", "新建参数");
        }), 1);
        QCOMPARE(document.Parameters().size(), parameterCount + 1);
        QCOMPARE(document.Parameters().last().id, QString("ParamPanelCreated"));
        QCOMPARE(document.ParameterMetadata("ParamPanelCreated").displayName, QString("新建参数"));
        QCOMPARE(document.Drawables().front().keyforms.size(), target.keyforms.size() * 3);
        for (int key = 0; key < 3; ++key) for (int local = 0; local < target.keyforms.size(); ++local) {
            const int frame = key * target.keyforms.size() + local;
            QCOMPARE(document.DrawableKeyformPositions(target.id, frame), target.keyforms[local].positions);
            QMap<QString, float> pose;
            QVERIFY(document.KeyformParameterValues(target.id, "artmesh", frame, pose));
            QCOMPARE(pose.value("ParamPanelCreated"), float(key - 1));
        }
        QCOMPARE(panel.m_rows.last().modelIndex, parameterCount);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(structure.size(), 1);
        panel.Undo();
        QCOMPARE(document.Parameters().size(), parameterCount);
        QCOMPARE(document.Drawables().front().keyforms.size(), target.keyforms.size());
        QVERIFY(!document.CanUndoEdit());
        panel.Redo();
        QCOMPARE(document.Parameters().size(), parameterCount + 1);
        QCOMPARE(structure.size(), 3);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(document.SourceBytes(), source);
        QCOMPARE(document.SourceDigest(), digest);
    }

    void structurePublisherFailureRetainsInputUntilSuccessfulRetry_data()
    {
        QTest::addColumn<bool>("exception");
        QTest::newRow("false") << false;
        QTest::newRow("exception") << true;
    }

    void structurePublisherFailureRetainsInputUntilSuccessfulRetry()
    {
        QFETCH(bool, exception);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        const auto revision = document.Revision();
        const int count = document.Parameters().size();
        int submissions = 0;
        int guardCalls = 0;
        panel.SetPreEditGuard([&](QString*) { ++guardCalls; return false; });
        panel.SetStructureEditHandler([&](const auto&, QString* error) {
            ++submissions;
            if (exception) throw std::runtime_error("candidate failed");
            if (error) *error = "candidate rejected";
            return false;
        });
        connect(&panel, &CParamPanel::StructureChanged, &panel, [&] { panel.ReconcileParameters(); });
        QSignalSpy published(&panel, &CParamPanel::StructureChanged), rejected(&panel, &CParamPanel::EditRejected);
        QCOMPARE(structureDialog(panel, "createParameter", [](QDialog* dialog) {
            metadataText(dialog, "structureParameterId", "ParamRetry");
            metadataText(dialog, "structureDisplayName", "保留输入");
        }, [&](QDialog* dialog) {
            QCOMPARE(document.Revision(), revision);
            QCOMPARE(document.Parameters().size(), count);
            QVERIFY(!document.CanUndoEdit());
            QCOMPARE(published.size(), 0);
            QCOMPARE(dialog->findChild<QLineEdit*>("structureDisplayName")->text(), QString("保留输入"));
            auto* error = dialog->findChild<QLabel*>("structureError");
            QVERIFY(error && error->isVisible() && !error->text().isEmpty());
            panel.SetStructureEditHandler([&](const auto& edit, QString*) { ++submissions; return document.ExecuteEdit(edit); });
            auto* id = dialog->findChild<QLineEdit*>("structureParameterId");
            id->setFocus();
            QTest::keyClick(id, Qt::Key_Return);
        }), 1);
        QCOMPARE(submissions, 2);
        QCOMPARE(guardCalls, 0);
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(published.size(), 1);
        QCOMPARE(document.Parameters().size(), count + 1);
        QCOMPARE(panel.m_rows.last().paramId, QString("ParamRetry"));
    }

    void structureInvalidInputDoesNotPublish_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QString>("value");
        QTest::newRow("empty-id") << QString("structureParameterId") << QString();
        QTest::newRow("utf8-overlong") << QString("structureParameterId") << QString(22, QChar(0x65b0));
        QTest::newRow("embedded-nul") << QString("structureParameterId") << QString("A") + QChar::Null + QString("B");
        QTest::newRow("nan") << QString("structureMinimum") << QString("nan");
        QTest::newRow("infinity") << QString("structureMaximum") << QString("inf");
        QTest::newRow("overflow") << QString("structureDefault") << QString("1e39");
        QTest::newRow("underflow") << QString("structureDefault") << QString("1e-99");
        QTest::newRow("outside-default") << QString("structureDefault") << QString("2");
    }

    void structureInvalidInputDoesNotPublish()
    {
        QFETCH(QString, field);
        QFETCH(QString, value);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        const auto revision = document.Revision();
        int calls = 0;
        panel.SetStructureEditHandler([&](const auto&, QString*) { ++calls; return true; });
        bool retained = false;
        QCOMPARE(structureDialog(panel, "createParameter", [&](QDialog* dialog) {
            dialog->findChild<QLineEdit*>(field)->setText(value);
        }, [&](QDialog* dialog) {
            retained = true;
            QCOMPARE(dialog->findChild<QLineEdit*>(field)->text(), value);
            QVERIFY(dialog->findChild<QLabel*>("structureError")->isVisible());
        }), 1);
        QVERIFY(retained);
        QCOMPARE(calls, 0);
        QCOMPARE(document.Revision(), revision);
        QVERIFY(!document.CanUndoEdit());
    }

    void structureUtf8BoundaryAndUnboundDeleteHistory()
    {
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const int count = document.Parameters().size();
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        const QString id(21, QChar(0x65b0));
        QCOMPARE(id.toUtf8().size(), 63);
        QCOMPARE(structureDialog(panel, "createParameter", [&](QDialog* dialog) { metadataText(dialog, "structureParameterId", id); }), 1);
        QCOMPARE(document.Parameters().last().id, id);
        panel.SetParameterReferenceProvider([](const QString&) { return QStringList{}; });
        QCOMPARE(structureDialog(panel, "deleteParameter", [&](QDialog* dialog) {
            auto* selected = dialog->findChild<QComboBox*>("structureParameter");
            selected->setCurrentIndex(selected->findData(id));
            QVERIFY(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->isEnabled());
        }), 1);
        QCOMPARE(document.Parameters().size(), count);
        panel.Undo();
        QCOMPARE(document.Parameters().last().id, id);
        panel.Undo();
        QCOMPARE(document.Parameters().size(), count);
        QVERIFY(!document.CanUndoEdit());
    }

    void existingParameterBindingUsesTwoKeysAndReportsItsDeleteReferences()
    {
        using Builder = live2dstruct::CMocParameterStructureBuilder;
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const auto target = document.Drawables().front();
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        QCOMPARE(structureDialog(panel, "createParameter", [](QDialog* dialog) {
            metadataText(dialog, "structureParameterId", "ParamBoundLater");
        }), 1);
        panel.SetBindingTargets({{Builder::Owner::ArtMesh, target.id}});
        QCOMPARE(structureDialog(panel, "bindParameter", [](QDialog* dialog) {
            auto* parameter = dialog->findChild<QComboBox*>("structureParameter");
            parameter->setCurrentIndex(parameter->findData("ParamBoundLater"));
            dialog->findChild<QComboBox*>("structureKeyCount")->setCurrentIndex(0);
            QCOMPARE(dialog->findChild<QLineEdit*>("structureKey0")->text(), QString("-1"));
            QCOMPARE(dialog->findChild<QLineEdit*>("structureKey1")->text(), QString("1"));
        }), 1);
        QCOMPARE(document.Drawables().front().keyforms.size(), target.keyforms.size() * 2);
        panel.SetParameterReferenceProvider([](const QString&) { return QStringList{}; });
        QCOMPARE(structureDialog(panel, "deleteParameter", [&](QDialog* dialog) {
            auto* parameter = dialog->findChild<QComboBox*>("structureParameter");
            parameter->setCurrentIndex(parameter->findData("ParamBoundLater"));
            QVERIFY(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->isEnabled());
            auto* references = dialog->findChild<QTreeWidget*>("structureReferences");
            bool found = false;
            for (int row = 0; row < references->topLevelItemCount(); ++row)
                found = found || references->topLevelItem(row)->text(1).contains(target.id);
            QVERIFY(found);
        }, {}, false), 1);
        panel.Undo();
        QCOMPARE(document.Drawables().front().keyforms.size(), target.keyforms.size());
        QCOMPARE(document.Parameters().last().id, QString("ParamBoundLater"));
        panel.Redo();
        QCOMPARE(document.Drawables().front().keyforms.size(), target.keyforms.size() * 2);
    }

    void boundDeleteFreezesCurrentValueAndUsesTypedReferencePlan_data()
    {
        QTest::addColumn<bool>("throwFirst");
        QTest::newRow("reject-retry") << false;
        QTest::newRow("throw-retry") << true;
    }

    void boundDeleteFreezesCurrentValueAndUsesTypedReferencePlan()
    {
        QFETCH(bool, throwFirst);
        using Builder = live2dstruct::CMocParameterStructureBuilder;
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel;
        panel.SetDocument(&document); panel.BuildParams();
        const auto target = document.Drawables().first();
        Builder::CreateRequest request; request.parameter.id = "ParamFreeze";
        request.bindings.append({{Builder::Owner::ArtMesh, target.id}, {-1, 1}});
        live2dstruct::CModelEdit created;
        QVERIFY(document.MakeCreateParameterEdit(request, {}, created)); QVERIFY(document.ExecuteEdit(created));
        auto maximum = document.DrawableKeyformPositions(target.id, target.keyforms.size()); maximum[0] += 2;
        QVERIFY(document.SetDrawableKeyformPositions(target.id, target.keyforms.size(), maximum));
        QVERIFY(document.SetParameterValue(request.parameter.id, 0.25f));
        panel.ReconcileParameters(); panel.ApplyExternalValue(request.parameter.id, 0.25f, false, false, false);
        const auto first = document.DrawableKeyformPositions(target.id, 0);
        const auto original = document.ParameterStructureState();
        const auto revision = document.Revision();
        int guardCalls = 0, submissions = 0;
        bool accepted = false;
        panel.SetPreEditGuard([&](QString*) { ++guardCalls; return false; });
        panel.SetParameterReferenceProvider([](const QString&) { return QStringList{"legacy blocker"}; });
        panel.SetDeleteReferenceProvider([](const QString&, CParamPanel::DeleteReferenceResult& result, QString*) {
            result.removals = {QStringLiteral("当前时间轴轨道"), QStringLiteral("物理输入项")}; return true;
        });
        panel.SetStructureEditHandler([&](const auto& edit, QString* error) {
            ++submissions;
            if (!accepted) { if (throwFirst) throw std::runtime_error("controlled"); if (error) *error = "受控拒绝"; return false; }
            return document.ExecuteEdit(edit);
        });
        connect(&panel, &CParamPanel::StructureChanged, &panel, [&] { panel.ReconcileParameters(); });
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged), structure(&panel, &CParamPanel::StructureChanged);
        QCOMPARE(structureDialog(panel, "deleteParameter", [&](QDialog* dialog) {
            auto* parameters = dialog->findChild<QComboBox*>("structureParameter");
            parameters->setCurrentIndex(parameters->findData(request.parameter.id));
            auto* freeze = dialog->findChild<QLineEdit*>("structureFreezeValue"); QVERIFY(freeze);
            QCOMPARE(freeze->text(), QString("0.25"));
            QTest::mouseClick(dialog->findChild<QPushButton*>("structureFreezeDefault"), Qt::LeftButton);
            QCOMPARE(freeze->text(), QString("0"));
            QTest::mouseClick(dialog->findChild<QPushButton*>("structureFreezeCurrent"), Qt::LeftButton);
            QCOMPARE(freeze->text(), QString("0.25"));
            auto* references = dialog->findChild<QTreeWidget*>("structureReferences");
            int removals = 0; bool targetFound = false;
            for (int row = 0; row < references->topLevelItemCount(); ++row) {
                const auto* item = references->topLevelItem(row);
                if (item->text(0) == QStringLiteral("一并移除")) { ++removals; QVERIFY(item->text(2).contains(QStringLiteral("撤销"))); }
                targetFound = targetFound || item->text(1).contains(target.id);
            }
            QCOMPARE(removals, 2); QVERIFY(targetFound);
            auto* details = dialog->findChild<QLabel*>("structureReferenceDetails");
            QVERIFY(details);
            QCOMPARE(details->textFormat(), Qt::PlainText);
            QVERIFY(details->wordWrap());
            QVERIFY(references->isColumnHidden(2));
            auto* selected = references->topLevelItem(references->topLevelItemCount() - 1);
            references->setCurrentItem(selected);
            QCOMPARE(details->text(), selected->text(2));
            QCOMPARE(details->toolTip(), selected->toolTip(1));
            QVERIFY(details->text().contains(QStringLiteral("物理输入项")));
            QVERIFY(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->isEnabled());
        }, [&](QDialog* dialog) {
            QCOMPARE(submissions, 1); QCOMPARE(document.ParameterStructureState(), original); QCOMPARE(document.Revision(), revision);
            QCOMPARE(structure.size(), 0); QCOMPARE(changed.size(), 0);
            auto* freeze = dialog->findChild<QLineEdit*>("structureFreezeValue"); QCOMPARE(freeze->text(), QString("0.25"));
            QVERIFY(dialog->findChild<QLabel*>("structureError")->isVisible());
            accepted = true;
            QTest::keyClick(freeze, Qt::Key_Return);
        }), 1);
        QCOMPARE(submissions, 2); QCOMPARE(guardCalls, 0); QCOMPARE(structure.size(), 1); QCOMPARE(changed.size(), 0);
        live2dstruct::CModelEdit command; QVERIFY(document.PeekUndoEdit(&command));
        QCOMPARE(command.parameterChange, live2dstruct::CModelEdit::ParameterChange::Delete);
        QVERIFY(command.freezeValue); QCOMPARE(*command.freezeValue, 0.25f);
        QCOMPARE(document.DrawableKeyformPositions(target.id, 0)[0], float(double(first[0]) * 0.375 + double(maximum[0]) * 0.625));
        const auto frozen = document.ParameterStructureState();
        QVERIFY(document.UndoEdit()); QCOMPARE(document.ParameterStructureState(), original);
        QVERIFY(document.RedoEdit()); QCOMPARE(document.ParameterStructureState(), frozen);
    }

    void deleteFreezeValidationRetainsInputUntilCorrected()
    {
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel; panel.SetDocument(&document); panel.BuildParams();
        const QString id = document.Parameters().first().id;
        const auto original = document.ParameterStructureState();
        panel.SetDeleteReferenceProvider([](const QString&, CParamPanel::DeleteReferenceResult&, QString*) { return true; });
        int submissions = 0;
        panel.SetStructureEditHandler([&](const auto& edit, QString*) { ++submissions; return document.ExecuteEdit(edit); });
        connect(&panel, &CParamPanel::StructureChanged, &panel, [&] { panel.ReconcileParameters(); });
        QCOMPARE(structureDialog(panel, "deleteParameter", [&](QDialog* dialog) {
            auto* parameters = dialog->findChild<QComboBox*>("structureParameter"); parameters->setCurrentIndex(parameters->findData(id));
            metadataText(dialog, "structureFreezeValue", "not-a-number");
        }, [&](QDialog* dialog) {
            QCOMPARE(submissions, 0); QCOMPARE(document.ParameterStructureState(), original);
            auto* field = dialog->findChild<QLineEdit*>("structureFreezeValue"); QCOMPARE(field->text(), QString("not-a-number"));
            QVERIFY(dialog->findChild<QLabel*>("structureError")->isVisible());
            field->setFocus(); field->selectAll(); QTest::keyClicks(field, "0");
            QTest::mouseClick(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save), Qt::LeftButton);
        }), 1);
        QCOMPARE(submissions, 1);
        QVERIFY(!document.ParameterMetadataState(id).contains("value"));
    }

    void typedDeleteReferenceFailuresDoNotPublish_data()
    {
        QTest::addColumn<int>("mode");
        for (int mode = 0; mode < 4; ++mode) QTest::newRow(qPrintable(QString::number(mode))) << mode;
    }

    void typedDeleteReferenceFailuresDoNotPublish()
    {
        QFETCH(int, mode);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel; panel.SetDocument(&document); panel.BuildParams();
        int submissions = 0;
        if (mode != 3) panel.SetStructureEditHandler([&](const auto&, QString*) { ++submissions; return true; });
        panel.SetDeleteReferenceProvider([mode](const QString&, CParamPanel::DeleteReferenceResult& result, QString* error) {
            result.removals = {QStringLiteral("当前轨道")};
            if (mode == 0) result.blockers = {QStringLiteral("无法读取动作文件")};
            if (mode == 1) { if (error) *error = "reference failed"; return false; }
            if (mode == 2) throw std::runtime_error("controlled");
            return true;
        });
        const auto original = document.ParameterStructureState();
        QCOMPARE(structureDialog(panel, "deleteParameter", [&](QDialog* dialog) {
            QVERIFY(!dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->isEnabled());
            QVERIFY(dialog->findChild<QTreeWidget*>("structureReferences")->topLevelItemCount() > 0);
        }, {}, false), 1);
        QCOMPARE(submissions, 0); QCOMPARE(document.ParameterStructureState(), original);
    }

    void structureExternalReferencesAndMissingProviderBlockDeletion()
    {
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        QCOMPARE(structureDialog(panel, "createParameter", [](QDialog* dialog) { metadataText(dialog, "structureParameterId", "ParamReferenced"); }), 1);
        const auto revision = document.Revision();
        for (int mode = 0; mode < 3; ++mode) {
            if (mode == 1) panel.SetParameterReferenceProvider([](const QString&) { return QStringList{"Physics 输入：Hair"}; });
            if (mode == 2) panel.SetParameterReferenceProvider([](const QString&) -> QStringList { throw std::runtime_error("failed"); });
            bool inspected = false;
            QCOMPARE(structureDialog(panel, "deleteParameter", [&](QDialog* dialog) {
                auto* selected = dialog->findChild<QComboBox*>("structureParameter");
                selected->setCurrentIndex(selected->findData("ParamReferenced"));
                auto* save = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save);
                QVERIFY(!save->isEnabled());
                QVERIFY(dialog->findChild<QTreeWidget*>("structureReferences")->topLevelItemCount() > 0);
                inspected = true;
            }, {}, false), 1);
            QVERIFY(inspected);
            QCOMPARE(document.Revision(), revision);
        }
    }

    void structureStaleSelectionAndDocumentCannotPublish()
    {
        using Builder = live2dstruct::CMocParameterStructureBuilder;
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        panel.SetBindingTargets({{Builder::Owner::ArtMesh, document.Drawables().front().id}});
        int calls = 0;
        panel.SetStructureEditHandler([&](const auto&, QString*) { ++calls; return true; });
        for (int mode = 0; mode < 2; ++mode) {
            bool retained = false;
            QCOMPARE(structureDialog(panel, "createParameter", [&](QDialog*) {
                if (mode == 0) panel.SetBindingTargets({{Builder::Owner::ArtMesh, document.Drawables()[1].id}});
                else QVERIFY(document.SetParameterValue(document.Parameters().front().id, document.Parameters().front().defaultValue));
            }, [&](QDialog* dialog) {
                retained = true;
                QVERIFY(dialog->findChild<QLabel*>("structureError")->text().contains(QStringLiteral("改变")));
            }), 1);
            QVERIFY(retained);
            QCOMPARE(calls, 0);
            QVERIFY(!document.CanUndoEdit());
        }
    }

    void adjacentFloatKeysStayEditableAfterValidationFailure()
    {
        using Builder = live2dstruct::CMocParameterStructureBuilder;
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        panel.SetBindingTargets({{Builder::Owner::ArtMesh, document.Drawables().front().id}});
        const float maximum = std::nextafter(1.0f, 2.0f);
        const auto revision = document.Revision();
        bool retained = false;
        QCOMPARE(structureDialog(panel, "createParameter", [&](QDialog* dialog) {
            metadataText(dialog, "structureParameterId", "ParamAdjacent");
            metadataText(dialog, "structureMinimum", "1");
            metadataText(dialog, "structureMaximum", QString::number(maximum, 'g', 9));
            metadataText(dialog, "structureDefault", "1");
            auto* count = dialog->findChild<QComboBox*>("structureKeyCount");
            count->setCurrentIndex(0);
            count->setCurrentIndex(1);
        }, [&](QDialog* dialog) {
            retained = true;
            QCOMPARE(document.Revision(), revision);
            QVERIFY(!document.CanUndoEdit());
            dialog->findChild<QComboBox*>("structureKeyCount")->setCurrentIndex(0);
            QTest::mouseClick(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save), Qt::LeftButton);
        }), 1);
        QVERIFY(retained);
        QCOMPARE(document.Parameters().last().id, QString("ParamAdjacent"));
        QCOMPARE(document.Parameters().last().minimum, 1.0f);
        QCOMPARE(document.Parameters().last().maximum, maximum);
    }

    void publishingCannotReenterOrDismissTheStructureDialog()
    {
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        int calls = 0;
        panel.SetStructureEditHandler([&](const auto& edit, QString*) {
            ++calls;
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) return false;
            dialog->reject();
            panel.CreateParameter();
            if (!dialog->isVisible()) return false;
            return document.ExecuteEdit(edit);
        });
        connect(&panel, &CParamPanel::StructureChanged, &panel, [&] { panel.ReconcileParameters(); });
        QCOMPARE(structureDialog(panel, "createParameter", [](QDialog* dialog) {
            metadataText(dialog, "structureParameterId", "ParamSingleSubmit");
        }), 1);
        QCOMPARE(calls, 1);
        QCOMPARE(document.Parameters().last().id, QString("ParamSingleSubmit"));
        live2dstruct::CModelEdit edit;
        QVERIFY(document.UndoEdit(&edit));
        QVERIFY(!document.CanUndoEdit());
    }

    void reconcilePreservesHistoryLocksPreviewXySearchAndIndices()
    {
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        const QString first = panel.m_rows[0].paramId, second = panel.m_rows[1].paramId;
        const float before = document.ParameterValue(first), after = panel.m_rows[0].maxVal;
        const live2dstruct::CModelEdit edit{first, "parameter", live2dstruct::CModelEdit::ParameterValue, before, after, -1, false};
        QVERIFY(document.ExecuteEdit(edit));
        panel.m_userLocked.insert(1);
        auto baseline = panel.CurrentValues(), preview = baseline;
        preview[first] = after;
        panel.SetPreviewValues(preview, baseline);
        panel.m_xyXSelector->setCurrentText(second);
        panel.m_xyYSelector->setCurrentText(first);
        panel.m_search->setText(first);
        const auto revision = document.Revision();
        QSignalSpy values(&panel, &CParamPanel::ParamChanged), histories(&panel, &CParamPanel::HistoryChanged);
        panel.ReconcileParameters();
        QCOMPARE(panel.CurrentValues(), preview);
        QCOMPARE(panel.m_previewBaseline, baseline);
        QVERIFY(panel.m_previewActive);
        QVERIFY(panel.m_userLocked.contains(1));
        QCOMPARE(panel.m_xyXSelector->currentText(), second);
        QCOMPARE(panel.m_xyYSelector->currentText(), first);
        QCOMPARE(panel.m_search->text(), first);
        QVERIFY(panel.m_rows[1].widget->isHidden());
        for (const auto& row : panel.m_rows) QCOMPARE(document.Parameters()[row.modelIndex].id, row.paramId);
        QCOMPARE(document.Revision(), revision);
        QVERIFY(document.CanUndoEdit());
        QCOMPARE(values.size(), 0);
        QCOMPARE(histories.size(), 0);
    }

    void reconcileKeepsCdiGroupsAndFocusedInput()
    {
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString first = document.Parameters().last().id, second = document.Parameters().first().id;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile file(directory.filePath("groups.cdi3.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QJsonDocument names(QJsonObject{{"Parameters", QJsonArray{
            QJsonObject{{"Id", first}, {"Name", "末尾参数"}, {"GroupId", "FirstGroup"}},
            QJsonObject{{"Id", second}, {"Name", "首个参数"}, {"GroupId", "OtherGroup"}}}},
            {"ParameterGroups", QJsonArray{QJsonObject{{"Id", "FirstGroup"}, {"Name", "显示组"}},
                                           QJsonObject{{"Id", "OtherGroup"}, {"Name", "其他组"}}}}});
        QCOMPARE(file.write(names.toJson()), names.toJson().size());
        file.close();
        CCidJson cid;
        QVERIFY(cid.Parse(file.fileName()));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams(&cid);
        panel.resize(760, 600);
        panel.show();
        panel.activateWindow();
        panel.raise();
        QVERIFY(panel.FocusParameter(first));
        auto found = std::find_if(panel.m_rows.begin(), panel.m_rows.end(), [&](const auto& row) { return row.paramId == first; });
        QVERIFY(found != panel.m_rows.end());
        QTRY_VERIFY(found->spinBox->hasFocus());
        auto* input = found->spinBox->findChild<QLineEdit*>();
        QVERIFY(input);
        input->setCursorPosition(1);
        const QString text = input->text();
        for (auto* button : panel.m_groupButtons)
            if (button->property("parameterGroupId").toString() == "OtherGroup") button->setChecked(false);
        panel.ReconcileParameters();
        found = std::find_if(panel.m_rows.begin(), panel.m_rows.end(), [&](const auto& row) { return row.paramId == first; });
        QVERIFY(found != panel.m_rows.end());
        QTRY_VERIFY(found->spinBox->hasFocus());
        QCOMPARE(found->nameLabel->text(), QString("末尾参数"));
        QCOMPARE(found->spinBox->findChild<QLineEdit*>()->text(), text);
        QCOMPARE(found->spinBox->findChild<QLineEdit*>()->cursorPosition(), std::min(1, int(text.size())));
        QCOMPARE(found->modelIndex, document.Parameters().size() - 1);
        bool collapsed = false;
        for (auto* button : panel.m_groupButtons)
            if (button->property("parameterGroupId").toString() == "OtherGroup") collapsed = !button->isChecked();
        QVERIFY(collapsed);
        QVERIFY(!document.CanUndoEdit());
    }

    void rejectedUserEditsRestoreControlsBeforePublishing_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("exception");
        for (bool exception : {false, true}) {
            for (const char* path : {"external", "slider-key", "slider-drag", "spin-step", "spin-key", "keyboard-commit",
                                     "keyboard-tracking", "reset-one", "reset-all", "values", "preset", "xy", "xy-key", "paste"}) {
                const QString name = QString::fromLatin1(path) + (exception ? "-exception" : "-false");
                QTest::newRow(qPrintable(name)) << QString::fromLatin1(path) << exception;
            }
        }
    }

    void rejectedUserEditsRestoreControlsBeforePublishing()
    {
        QFETCH(QString, path);
        QFETCH(bool, exception);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        QVERIFY(panel.m_rows.size() >= 2);
        panel.ApplyValues(valuesAt(panel, 0.75f));
        const QString first = panel.m_rows[0].paramId, second = panel.m_rows[1].paramId;
        panel.ApplyExternalValue(first, valuesAt(panel, 0.65f).value(first), true);
        panel.ApplyExternalValue(second, valuesAt(panel, 0.6f).value(second), true);
        panel.Undo();
        panel.m_userLocked.clear();
        panel.RefreshXYControls();
        QVERIFY(document.CanUndoEdit());
        QVERIFY(document.CanRedoEdit());
        const auto before = panel.CurrentValues();
        const auto documentBefore = document.ParameterValues();
        const auto revision = document.Revision();
        live2dstruct::CModelEdit undoBefore, redoBefore;
        QVERIFY(document.PeekUndoEdit(&undoBefore));
        QVERIFY(document.PeekRedoEdit(&redoBefore));
        QVector<int> sliders;
        QVector<double> spins;
        for (const auto& row : panel.m_rows) {
            sliders.append(row.slider->value());
            spins.append(row.spinBox->value());
        }
        const float xyX = panel.m_xyPad->XValue(), xyY = panel.m_xyPad->YValue();
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged);
        QSignalSpy history(&panel, &CParamPanel::HistoryChanged);
        QSignalSpy rejected(&panel, &CParamPanel::EditRejected);
        int calls = 0;
        bool ranBeforeMutation = true;
        panel.SetPreEditGuard([&](QString* error) {
            ++calls;
            ranBeforeMutation = ranBeforeMutation && document.ParameterValues() == documentBefore &&
                                document.Revision() == revision && changed.isEmpty();
            if (exception) throw std::runtime_error("controlled guard failure");
            *error = "cannot leave edit pose";
            return false;
        });
        const auto requested = valuesAt(panel, 0.25f);
        auto& row = panel.m_rows[0];
        if (path == "external") panel.ApplyExternalValue(first, requested.value(first), true);
        if (path == "slider-key") QTest::keyClick(row.slider, Qt::Key_Right);
        if (path == "slider-drag") {
            row.slider->setSliderDown(true);
            row.slider->setValue(250);
            row.slider->setSliderDown(false);
        }
        if (path == "spin-step") row.spinBox->stepUp();
        if (path == "spin-key") QTest::keyClick(row.spinBox, Qt::Key_Down);
        if (path.startsWith("keyboard-")) {
            row.spinBox->setKeyboardTracking(path == "keyboard-tracking");
            row.spinBox->selectAll();
            QTest::keyClicks(row.spinBox, row.spinBox->locale().toString(requested.value(first), 'f', row.spinBox->decimals()));
            QTest::keyClick(row.spinBox, Qt::Key_Return);
        }
        if (path == "reset-one") row.resetButton->click();
        if (path == "reset-all") panel.ResetAll();
        if (path == "values") panel.ApplyValues(requested);
        if (path == "preset") panel.ApplyValuesInternal(requested, true);
        if (path == "xy") panel.ApplyXYPad(requested.value(first), requested.value(second));
        if (path == "xy-key") QTest::keyClick(panel.m_xyPad, Qt::Key_Right);
        if (path == "paste") {
            auto* clipboard = QApplication::clipboard();
            auto original = std::make_unique<QMimeData>();
            if (const auto* data = clipboard->mimeData())
                for (const auto& format : data->formats()) original->setData(format, data->data(format));
            const auto restore = qScopeGuard([&] { clipboard->setMimeData(original.release()); });
            QJsonObject preset{{"format", "live2d2psd-parameters-v1"}};
            for (auto it = requested.cbegin(); it != requested.cend(); ++it) preset.insert(it.key(), it.value());
            clipboard->setText(QString::fromUtf8(QJsonDocument(preset).toJson()));
            panel.PasteValues();
        }
        QVERIFY(calls > 0);
        QVERIFY(ranBeforeMutation);
        QCOMPARE(rejected.size(), calls);
        if (!exception) QCOMPARE(rejected.first().first().toString(), QStringLiteral("cannot leave edit pose"));
        else QVERIFY(!rejected.first().first().toString().isEmpty());
        QCOMPARE(panel.CurrentValues(), before);
        QCOMPARE(document.ParameterValues(), documentBefore);
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(history.size(), 0);
        QVERIFY(panel.m_userLocked.isEmpty());
        QVERIFY(panel.m_userDragging.isEmpty());
        QVERIFY(panel.m_changedDrags.isEmpty());
        QVERIFY(panel.m_dragStarts.isEmpty());
        for (int index = 0; index < panel.m_rows.size(); ++index) {
            QCOMPARE(panel.m_rows[index].slider->value(), sliders[index]);
            QCOMPARE(panel.m_rows[index].spinBox->value(), spins[index]);
        }
        QCOMPARE(panel.m_xyPad->XValue(), xyX);
        QCOMPARE(panel.m_xyPad->YValue(), xyY);
        live2dstruct::CModelEdit undo, redo;
        QVERIFY(document.PeekUndoEdit(&undo));
        QVERIFY(document.PeekRedoEdit(&redo));
        QCOMPARE(undo.name, undoBefore.name);
        QCOMPARE(undo.before, undoBefore.before);
        QCOMPARE(undo.after, undoBefore.after);
        QCOMPARE(redo.name, redoBefore.name);
        QCOMPARE(redo.before, redoBefore.before);
        QCOMPARE(redo.after, redoBefore.after);
    }

    void approvedBatchesCheckOnceBeforeAnyParameterChanges_data()
    {
        QTest::addColumn<int>("path");
        QTest::newRow("reset") << 0;
        QTest::newRow("values") << 1;
        QTest::newRow("preset") << 2;
        QTest::newRow("xy") << 3;
    }

    void approvedBatchesCheckOnceBeforeAnyParameterChanges()
    {
        QFETCH(int, path);
        live2dstruct::CModelDocument document;
        CParamPanel panel;
        initialize(panel, document);
        panel.ApplyValues(valuesAt(panel, 0.75f));
        const auto before = document.ParameterValues();
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged);
        QSignalSpy rejected(&panel, &CParamPanel::EditRejected);
        int calls = 0;
        bool first = false;
        panel.SetPreEditGuard([&](QString*) {
            ++calls;
            first = changed.isEmpty() && document.ParameterValues() == before;
            return true;
        });
        const auto requested = valuesAt(panel, 0.25f);
        if (path == 0) panel.ResetAll();
        if (path == 1) panel.ApplyValues(requested);
        if (path == 2) panel.ApplyValuesInternal(requested, true);
        if (path == 3) panel.ApplyXYPad(requested.value(panel.m_rows[0].paramId), requested.value(panel.m_rows[1].paramId));
        QCOMPARE(calls, 1);
        QVERIFY(first);
        QCOMPARE(rejected.size(), 0);
        QCOMPARE(changed.size(), panel.m_rows.size());
        for (const auto& row : panel.m_rows) {
            const float expected = path == 0 ? row.defaultVal : requested.value(row.paramId);
            QCOMPARE(panel.CurrentValues().value(row.paramId), expected);
            QCOMPARE(document.ParameterValue(row.paramId), expected);
        }
    }

    void silentPreviewUpdatesBypassTheUserGuard()
    {
        live2dstruct::CModelDocument document;
        CParamPanel panel;
        initialize(panel, document);
        const auto before = document.ParameterValues();
        const auto baseline = valuesAt(panel, 0.25f), preview = valuesAt(panel, 0.75f);
        int calls = 0;
        panel.SetPreEditGuard([&](QString*) { ++calls; return false; });
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged);
        QSignalSpy rejected(&panel, &CParamPanel::EditRejected);
        panel.SetPreviewValues(preview, baseline);
        QCOMPARE(panel.CurrentValues(), preview);
        panel.ClearPreviewValues();
        QCOMPARE(panel.CurrentValues(), baseline);
        QCOMPARE(document.ParameterValues(), before);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(rejected.size(), 0);
        QCOMPARE(calls, 0);
        panel.ApplyExternalValue(panel.m_rows[0].paramId, preview.value(panel.m_rows[0].paramId), false, false);
        QCOMPARE(calls, 0);
        QCOMPARE(document.ParameterValue(panel.m_rows[0].paramId), preview.value(panel.m_rows[0].paramId));
    }

    void guardCanRestoreTheBaseBeforeAUserDragStartsChangingValues()
    {
        live2dstruct::CModelDocument document;
        CParamPanel panel;
        initialize(panel, document);
        const auto baseline = valuesAt(panel, 0.25f), preview = valuesAt(panel, 0.75f), edited = valuesAt(panel, 0.6f);
        panel.ApplyValues(baseline);
        for (auto it = preview.cbegin(); it != preview.cend(); ++it)
            panel.ApplyExternalValue(it.key(), it.value(), false, false, false);
        QVERIFY(!panel.m_previewActive);
        int calls = 0;
        panel.SetPreEditGuard([&](QString*) {
            ++calls;
            panel.ClearPreviewValues(baseline);
            return true;
        });
        panel.BeginParameterDrag(0);
        QVERIFY(panel.ApplyValue(0, edited.value(panel.m_rows[0].paramId), false));
        panel.EndParameterDrag(0);
        QCOMPARE(calls, 1);
        live2dstruct::CModelEdit edit;
        QVERIFY(document.PeekUndoEdit(&edit));
        QCOMPARE(edit.before.toFloat(), baseline.value(panel.m_rows[0].paramId));
        QCOMPARE(edit.after.toFloat(), edited.value(panel.m_rows[0].paramId));
        QVERIFY(!panel.m_previewActive);
    }

    void approvedKeyboardInputSurvivesBaselineRestore_data()
    {
        QTest::addColumn<bool>("restoreBaseline");
        QTest::newRow("guard-only") << false;
        QTest::newRow("restore-baseline") << true;
    }

    void approvedKeyboardInputSurvivesBaselineRestore()
    {
        QFETCH(bool, restoreBaseline);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString id = document.Parameters().front().id;
        const float original = 0.12345679f;
        QVERIFY(document.SetParameterValue(id, original));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        const auto baseline = panel.CurrentValues();
        if (restoreBaseline) panel.ApplyExternalValue(id, 0.75f, false, false, false);
        int calls = 0;
        panel.SetPreEditGuard([&](QString*) {
            if (++calls == 1 && restoreBaseline) panel.ClearPreviewValues(baseline);
            return true;
        });
        auto* spinBox = panel.m_rows.front().spinBox;
        spinBox->setKeyboardTracking(true);
        spinBox->selectAll();
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged);
        QSignalSpy rejected(&panel, &CParamPanel::EditRejected);
        const QString input = spinBox->locale().toString(0.375, 'f', 3);
        for (int index = 0; index < input.size(); ++index) {
            QTest::keyClicks(spinBox, input.mid(index, 1));
            QCOMPARE(spinBox->text(), input.left(index + 1));
        }
        QTest::keyClick(spinBox, Qt::Key_Return);
        QCOMPARE(panel.CurrentValues().value(id), 0.375f);
        QCOMPARE(document.ParameterValue(id), 0.375f);
        QCOMPARE(rejected.size(), 0);
        QCOMPARE(calls, changed.size());
        QVERIFY(changed.size() > 1);
        const int edits = changed.size();
        panel.SetPreEditGuard({});
        for (int index = 0; index < edits; ++index) panel.Undo();
        QCOMPARE(document.ParameterValue(id), original);
        QVERIFY(!document.CanUndoEdit());
        for (int index = 0; index < edits; ++index) panel.Redo();
        QCOMPARE(document.ParameterValue(id), 0.375f);
    }

    void deniedHistoryDoesNotMoveTheHistoryCursor()
    {
        live2dstruct::CModelDocument document;
        CParamPanel panel;
        initialize(panel, document);
        panel.ApplyExternalValue(panel.m_rows[0].paramId, valuesAt(panel, 0.65f).value(panel.m_rows[0].paramId), true);
        const auto current = panel.CurrentValues();
        const auto revision = document.Revision();
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged);
        QSignalSpy rejected(&panel, &CParamPanel::EditRejected);
        panel.SetPreEditGuard([](QString* error) { *error = "blocked"; return false; });
        panel.Undo();
        QCOMPARE(panel.CurrentValues(), current);
        QCOMPARE(document.Revision(), revision);
        QVERIFY(document.CanUndoEdit());
        QVERIFY(!document.CanRedoEdit());
        QCOMPARE(changed.size(), 0);
        QCOMPARE(rejected.size(), 1);
        panel.SetPreEditGuard({});
        panel.Undo();
        const auto undone = panel.CurrentValues();
        const auto undoRevision = document.Revision();
        changed.clear();
        panel.SetPreEditGuard([](QString*) -> bool { throw std::runtime_error("blocked"); });
        panel.Redo();
        QCOMPARE(panel.CurrentValues(), undone);
        QCOMPARE(document.Revision(), undoRevision);
        QVERIFY(!document.CanUndoEdit());
        QVERIFY(document.CanRedoEdit());
        QCOMPARE(changed.size(), 0);
        QCOMPARE(rejected.size(), 2);
    }

    void reentrantUserEditsCannotBypassTheGuard()
    {
        live2dstruct::CModelDocument document;
        CParamPanel panel;
        initialize(panel, document);
        const auto before = document.ParameterValues();
        const auto shown = panel.CurrentValues();
        const auto requested = valuesAt(panel, 0.75f);
        int calls = 0;
        panel.SetPreEditGuard([&](QString*) {
            ++calls;
            panel.ApplyValues(requested);
            panel.SetPreEditGuard([](QString*) { return true; });
            return false;
        });
        QSignalSpy rejected(&panel, &CParamPanel::EditRejected);
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged);
        panel.ApplyExternalValue(panel.m_rows[0].paramId, requested.value(panel.m_rows[0].paramId), true);
        QCOMPARE(calls, 1);
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(document.ParameterValues(), before);
        QCOMPARE(panel.CurrentValues(), shown);
        QVERIFY(!document.CanUndoEdit());
        panel.ApplyValues(requested);
        QCOMPARE(calls, 2);
        QCOMPARE(document.ParameterValues(), before);
    }

    void rejectedMetadataDialogDoesNotWriteTheDocument()
    {
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        const auto revision = document.Revision();
        const auto metadata = document.ParameterMetadataOverrides();
        int dialogs = 0;
        QTimer::singleShot(0, &panel, [&] {
            auto* picker = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            if (!picker) { QApplication::closeAllWindows(); return; }
            ++dialogs;
            picker->accept();
            QTimer::singleShot(0, &panel, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog) { QApplication::closeAllWindows(); return; }
                ++dialogs;
                metadataText(dialog, "metadataName", QStringLiteral("Changed name"));
                dialog->accept();
            });
        });
        QSignalSpy rejected(&panel, &CParamPanel::EditRejected);
        panel.SetPreEditGuard([](QString* error) { *error = "metadata blocked"; return false; });
        panel.EditParameterMetadata();
        QCOMPARE(dialogs, 2);
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(document.ParameterMetadataOverrides(), metadata);
        QVERIFY(!document.CanUndoEdit());
    }

    void metadataCancelAndNoOpAreCompletelySilent_data()
    {
        QTest::addColumn<bool>("save");
        QTest::newRow("cancel") << false;
        QTest::newRow("unchanged-save") << true;
    }

    void metadataCancelAndNoOpAreCompletelySilent()
    {
        QFETCH(bool, save);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        const QString id = panel.m_rows.front().paramId;
        const auto before = document.ParameterMetadataState(id);
        const auto revision = document.Revision();
        int guarded = 0, submitted = 0;
        panel.SetPreEditGuard([&](QString*) { ++guarded; return false; });
        panel.SetMetadataEditHandler([&](const auto&, QString*) { ++submitted; return false; });
        QSignalSpy rejected(&panel, &CParamPanel::EditRejected);
        QSignalSpy history(&panel, &CParamPanel::HistoryChanged);
        QSignalSpy metadata(&panel, &CParamPanel::ParameterMetadataChanged);
        QSignalSpy values(&panel, &CParamPanel::ParamChanged);
        QCOMPARE(metadataDialog(panel, id, {}, save), 2);
        QCOMPARE(document.ParameterMetadataState(id), before);
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(guarded, 0);
        QCOMPARE(submitted, 0);
        QCOMPARE(rejected.size(), 0);
        QCOMPARE(history.size(), 0);
        QCOMPARE(metadata.size(), 0);
        QCOMPARE(values.size(), 0);
    }

    void metadataSubmissionIsGatedAndDoesNotUseTheValueGuard_data()
    {
        QTest::addColumn<int>("outcome");
        QTest::newRow("reject") << 0;
        QTest::newRow("throw") << 1;
        QTest::newRow("accept") << 2;
    }

    void metadataSubmissionIsGatedAndDoesNotUseTheValueGuard()
    {
        QFETCH(int, outcome);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        const QString id = panel.m_rows.front().paramId;
        const auto before = document.ParameterMetadataState(id);
        const auto shown = panel.CurrentValues();
        const auto revision = document.Revision();
        int guarded = 0, submitted = 0;
        panel.SetPreEditGuard([&](QString*) { ++guarded; return false; });
        panel.SetMetadataEditHandler([&](const auto& edit, QString* error) {
            ++submitted;
            panel.ApplyExternalValue(id, panel.m_rows[0].maxVal, true);
            panel.EditParameterMetadata();
            if (outcome == 0) { *error = QStringLiteral("candidate refused"); return false; }
            if (outcome == 1) throw std::runtime_error("candidate exception");
            return document.ExecuteEdit(edit);
        });
        QSignalSpy rejected(&panel, &CParamPanel::EditRejected);
        QSignalSpy history(&panel, &CParamPanel::HistoryChanged);
        QSignalSpy metadata(&panel, &CParamPanel::ParameterMetadataChanged);
        QSignalSpy values(&panel, &CParamPanel::ParamChanged);
        QCOMPARE(metadataDialog(panel, id, [](QDialog* dialog) { metadataText(dialog, "metadataName", "<b>Accepted name</b>"); }), 2);
        QCOMPARE(guarded, 0);
        QCOMPARE(submitted, 1);
        QCOMPARE(panel.CurrentValues(), shown);
        QCOMPARE(values.size(), 0);
        QCOMPARE(rejected.size(), outcome == 2 ? 0 : 1);
        QCOMPARE(history.size(), outcome == 2 ? 1 : 0);
        QCOMPARE(metadata.size(), outcome == 2 ? 1 : 0);
        QCOMPARE(document.CanUndoEdit(), outcome == 2);
        if (outcome == 2) {
            QCOMPARE(document.Revision(), revision + 1);
            QCOMPARE(panel.m_rows[0].nameLabel->text(), QStringLiteral("<b>Accepted name</b>"));
            QCOMPARE(panel.m_rows[0].nameLabel->textFormat(), Qt::PlainText);
        } else {
            QCOMPARE(document.Revision(), revision);
            QCOMPARE(document.ParameterMetadataState(id), before);
        }
    }

    void metadataDialogRejectsStaleDocument_data()
    {
        QTest::addColumn<QString>("change");
        QTest::newRow("picker-revision") << QStringLiteral("picker");
        QTest::newRow("editor-revision") << QStringLiteral("revision");
        QTest::newRow("replacement") << QStringLiteral("replacement");
        QTest::newRow("source-reload") << QStringLiteral("source");
    }

    void metadataDialogRejectsStaleDocument()
    {
        QFETCH(QString, change);
        live2dstruct::CModelDocument document, replacement;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        QVERIFY(replacement.Load(QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3"));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        const QString id = panel.m_rows.front().paramId;
        int submitted = 0;
        panel.SetMetadataEditHandler([&](const auto&, QString*) { ++submitted; return true; });
        QSignalSpy rejected(&panel, &CParamPanel::EditRejected);
        QSignalSpy history(&panel, &CParamPanel::HistoryChanged);
        if (change == QLatin1String("picker")) {
            QTimer::singleShot(0, &panel, [&] {
                auto* picker = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
                if (!picker) return;
                document.SetParameterValue(id, document.ParameterValue(id));
                picker->accept();
            });
            panel.EditParameterMetadata();
        } else {
            QCOMPARE(metadataDialog(panel, id, [&](QDialog* dialog) {
                metadataText(dialog, "metadataName", "stale edit");
                if (change == QLatin1String("replacement")) panel.SetDocument(&replacement);
                else if (change == QLatin1String("source")) document.Load(QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3");
                else document.SetParameterValue(id, document.ParameterValue(id));
            }), 2);
        }
        QCOMPARE(submitted, 0);
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(history.size(), 0);
        QVERIFY(document.ParameterMetadataOverrides().isEmpty());
        QVERIFY(replacement.ParameterMetadataOverrides().isEmpty());
        QVERIFY(!document.CanUndoEdit());
    }

    void deletedPanelDoesNotLeaveADialogOrResumeACommit_data()
    {
        QTest::addColumn<int>("stage");
        QTest::newRow("picker") << 0;
        QTest::newRow("editor") << 1;
        QTest::newRow("submitter") << 2;
        QTest::newRow("legacy-guard") << 3;
        QTest::newRow("validation-error") << 4;
    }

    void deletedPanelDoesNotLeaveADialogOrResumeACommit()
    {
        QFETCH(int, stage);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        QPointer<CParamPanel> panel = new CParamPanel;
        panel->SetDocument(&document);
        panel->BuildParams();
        const QString id = panel->m_rows.front().paramId;
        const auto revision = document.Revision();
        if (stage == 0) {
            QTimer::singleShot(0, qApp, [panel] { delete panel.data(); });
            panel->EditParameterMetadata();
        } else {
            if (stage == 2) panel->SetMetadataEditHandler([panel](const auto&, QString*) {
                delete panel.data();
                return true;
            });
            if (stage == 3) panel->SetPreEditGuard([panel](QString*) {
                delete panel.data();
                return false;
            });
            if (stage == 4) connect(panel, &CParamPanel::EditRejected, this, [panel] { delete panel.data(); });
            QCOMPARE(metadataDialog(*panel, id, [panel, stage](QDialog* dialog) {
                metadataText(dialog, "metadataName", "delete during edit");
                if (stage == 4) metadataText(dialog, "metadataMaximum", "invalid");
                if (stage == 1) delete panel.data();
            }), 2);
        }
        QVERIFY(!panel);
        QVERIFY(!QApplication::activeModalWidget());
        QCOMPARE(document.Revision(), revision);
        QVERIFY(!document.CanUndoEdit());
    }

    void metadataDecimalsAndRepeatControlsUseTheSameCompoundHistory()
    {
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        const QString id = panel.m_rows.front().paramId;
        const auto before = document.ParameterMetadataState(id);
        const auto source = document.ParameterMetadata(id);
        const int decimals = source.decimalPlaces == 9 ? 8 : 9;
        const float value = panel.CurrentValues().value(id);
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged);
        QCOMPARE(metadataDialog(panel, id, [decimals](QDialog* dialog) {
            auto* number = dialog->findChild<QSpinBox*>("metadataDecimals");
            auto* repeat = dialog->findChild<QCheckBox*>("metadataRepeat");
            QVERIFY(number && repeat);
            number->setValue(decimals);
            QStyleOptionButton option;
            option.initFrom(repeat);
            const QRect indicator = repeat->style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, repeat);
            QTest::mouseClick(repeat, Qt::LeftButton, Qt::NoModifier, indicator.center());
        }), 2);
        QCOMPARE(document.ParameterMetadata(id).decimalPlaces, decimals);
        QCOMPARE(document.ParameterMetadata(id).repeat, !source.repeat);
        QCOMPARE(panel.m_rows[0].spinBox->decimals(), decimals);
        QCOMPARE(panel.CurrentValues().value(id), value);
        panel.Undo();
        QCOMPARE(document.ParameterMetadataState(id), before);
        QCOMPARE(panel.CurrentValues().value(id), value);
        QCOMPARE(changed.size(), 0);
    }

    void metadataShrinkAndHistoryRestoreOneCompoundState()
    {
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString id = document.Parameters().front().id;
        const auto source = document.ParameterMetadata(id);
        auto expanded = source;
        expanded.maximum += 20;
        QVERIFY(document.SetParameterMetadata(id, expanded));
        QVERIFY(document.SetParameterValue(id, source.maximum + 10));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        panel.m_userLocked.insert(0);
        panel.m_xyXSelector->setCurrentIndex(1);
        const QString selectedX = panel.m_xyXSelector->currentText();
        const auto before = document.ParameterMetadataState(id);
        const auto revision = document.Revision();
        int historyCalls = 0;
        panel.SetMetadataEditHandler([&](const auto& edit, QString*) { return document.ExecuteEdit(edit); });
        panel.SetHistoryHandler([&](bool undo, auto& edit, QString*) {
            ++historyCalls;
            return undo ? document.UndoEdit(&edit) : document.RedoEdit(&edit);
        });
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged);
        QCOMPARE(metadataDialog(panel, id, [&](QDialog* dialog) {
            metadataText(dialog, "metadataMaximum", QString::number(source.maximum, 'g', 9));
        }), 2);
        const auto after = document.ParameterMetadataState(id);
        QCOMPARE(document.Revision(), revision + 1);
        QCOMPARE(panel.CurrentValues().value(id), source.maximum);
        QCOMPARE(document.ParameterValue(id), source.maximum);
        QVERIFY(panel.m_userLocked.contains(0));
        QCOMPARE(panel.m_xyXSelector->currentText(), selectedX);
        panel.Undo();
        QCOMPARE(document.ParameterMetadataState(id), before);
        QCOMPARE(panel.CurrentValues().value(id), source.maximum + 10);
        QVERIFY(!document.CanUndoEdit());
        panel.Redo();
        QCOMPARE(document.ParameterMetadataState(id), after);
        QCOMPARE(panel.CurrentValues().value(id), source.maximum);
        QCOMPARE(historyCalls, 2);
        QCOMPARE(changed.size(), 0);
    }

    void metadataDefaultEditPreservesValueAndRestoresCdiNameOnUndo()
    {
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString id = document.Parameters().front().id;
        const auto source = document.ParameterMetadata(id);
        QTemporaryDir directory;
        QFile file(directory.filePath("display.cdi3.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QJsonObject cdi{{"Version", 3}, {"Parameters", QJsonArray{QJsonObject{{"Id", id}, {"Name", "<b>CDI name</b>"}}}}};
        QVERIFY(file.write(QJsonDocument(cdi).toJson()) > 0);
        file.close();
        CCidJson names;
        QVERIFY(names.Parse(file.fileName()));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams(&names);
        QCOMPARE(panel.m_rows[0].nameLabel->text(), QStringLiteral("<b>CDI name</b>"));
        const auto before = document.ParameterMetadataState(id);
        const float current = panel.CurrentValues().value(id);
        const float nextDefault = source.defaultValue == source.maximum ? source.minimum : source.maximum;
        QVERIFY(nextDefault != current);
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged);
        QCOMPARE(metadataDialog(panel, id, [&](QDialog* dialog) {
            QCOMPARE(dialog->findChild<QLineEdit*>("metadataName")->text(), QStringLiteral("<b>CDI name</b>"));
            metadataText(dialog, "metadataDefault", QString::number(nextDefault, 'g', 9));
        }), 2);
        QCOMPARE(document.ParameterMetadata(id).displayName, id);
        QCOMPARE(panel.m_rows[0].nameLabel->text(), QStringLiteral("<b>CDI name</b>"));
        QCOMPARE(document.ParameterValue(id), current);
        QCOMPARE(panel.CurrentValues().value(id), current);
        QVERIFY(document.ParameterMetadataState(id).value("hasValueOverride").toBool());
        panel.Undo();
        QCOMPARE(document.ParameterMetadataState(id), before);
        QCOMPARE(panel.m_rows[0].nameLabel->text(), QStringLiteral("<b>CDI name</b>"));
        QCOMPARE(panel.m_rows[0].nameLabel->textFormat(), Qt::PlainText);
        QCOMPARE(changed.size(), 0);
        QVERIFY(!document.CanUndoEdit());
        panel.Redo();
        QCOMPARE(panel.m_rows[0].nameLabel->text(), QStringLiteral("<b>CDI name</b>"));
        const auto revision = document.Revision();
        QCOMPARE(metadataDialog(panel, id), 2);
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(document.ParameterMetadata(id).displayName, id);
        QCOMPARE(metadataDialog(panel, id, [](QDialog* dialog) { metadataText(dialog, "metadataName", "自定义名称"); }), 2);
        QCOMPARE(panel.m_rows[0].nameLabel->text(), QStringLiteral("自定义名称"));
        const auto custom = document.ParameterMetadataState(id);
        QCOMPARE(metadataDialog(panel, id, [](QDialog* dialog) {
            QCOMPARE(dialog->findChild<QLineEdit*>("metadataName")->text(), QStringLiteral("自定义名称"));
            dialog->findChild<QSpinBox*>("metadataDecimals")->setValue(9);
        }), 2);
        QCOMPARE(document.ParameterMetadata(id).displayName, QStringLiteral("自定义名称"));
        QCOMPARE(panel.m_rows[0].nameLabel->text(), QStringLiteral("自定义名称"));
        panel.Undo();
        QCOMPARE(document.ParameterMetadataState(id), custom);
        panel.Undo();
        QCOMPARE(panel.m_rows[0].nameLabel->text(), QStringLiteral("<b>CDI name</b>"));
    }

    void metadataNameOnlyPreservesEveryFloatBit_data()
    {
        QTest::addColumn<float>("minimum");
        QTest::addColumn<float>("maximum");
        QTest::addColumn<float>("defaultValue");
        const float huge = std::numeric_limits<float>::max();
        const float tiny = std::numeric_limits<float>::denorm_min();
        QTest::newRow("full-range") << -huge << huge << tiny;
        QTest::newRow("subnormal") << tiny << tiny * 8 << tiny * 4;
        QTest::newRow("fixed") << huge << huge << huge;
        QTest::newRow("signed-zero") << -1.0f << 1.0f << -0.0f;
        QTest::newRow("fraction") << -.14434664f << .9876543f << .1234567f;
    }

    void metadataNameOnlyPreservesEveryFloatBit()
    {
        QFETCH(float, minimum);
        QFETCH(float, maximum);
        QFETCH(float, defaultValue);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString id = document.Parameters().front().id;
        auto metadata = document.ParameterMetadata(id);
        metadata.minimum = minimum;
        metadata.maximum = maximum;
        metadata.defaultValue = defaultValue;
        QVERIFY(document.SetParameterMetadata(id, metadata));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        QCOMPARE(metadataDialog(panel, id, [&](QDialog* dialog) {
            QCOMPARE(dialog->findChild<QLineEdit*>("metadataMinimum")->text(), QString::number(minimum, 'g', 9));
            QCOMPARE(dialog->findChild<QLineEdit*>("metadataMaximum")->text(), QString::number(maximum, 'g', 9));
            QCOMPARE(dialog->findChild<QLineEdit*>("metadataDefault")->text(), QString::number(defaultValue, 'g', 9));
            metadataText(dialog, "metadataName", "precise name");
        }), 2);
        const auto after = document.ParameterMetadata(id);
        QVERIFY(sameFloat(after.minimum, minimum));
        QVERIFY(sameFloat(after.maximum, maximum));
        QVERIFY(sameFloat(after.defaultValue, defaultValue));
        QCOMPARE(after.displayName, QStringLiteral("precise name"));
        panel.Undo();
        QVERIFY(document.ParameterMetadata(id) == metadata);
    }

    void metadataScientificTextUsesFloatRoundingAtTheRepresentableLimits()
    {
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString id = document.Parameters().front().id;
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        QSignalSpy rejected(&panel, &CParamPanel::EditRejected);
        QCOMPARE(metadataDialog(panel, id, [](QDialog* dialog) {
            metadataText(dialog, "metadataMinimum", "-3.40282347e38");
            metadataText(dialog, "metadataMaximum", "3.40282347e38");
            metadataText(dialog, "metadataDefault", "1.40129846e-45");
        }), 2);
        QCOMPARE(rejected.size(), 0);
        QCOMPARE(document.ParameterMetadata(id).minimum, -std::numeric_limits<float>::max());
        QCOMPARE(document.ParameterMetadata(id).maximum, std::numeric_limits<float>::max());
        QCOMPARE(document.ParameterMetadata(id).defaultValue, std::numeric_limits<float>::denorm_min());
    }

    void metadataInvalidNumbersLeaveStateAndHistoryUntouched_data()
    {
        QTest::addColumn<QString>("text");
        for (const QString& value : {QStringLiteral("nan"), QStringLiteral("inf"), QStringLiteral("1e9999"),
                QStringLiteral("1e-9999"), QStringLiteral("3.5e38"), QStringLiteral("1.2x"), QString()})
            QTest::newRow(qPrintable(value.isEmpty() ? QStringLiteral("empty") : value)) << value;
    }

    void metadataInvalidNumbersLeaveStateAndHistoryUntouched()
    {
        QFETCH(QString, text);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        const QString id = panel.m_rows.front().paramId;
        const auto before = document.ParameterMetadataState(id);
        const auto revision = document.Revision();
        int calls = 0;
        panel.SetMetadataEditHandler([&](const auto&, QString*) { ++calls; return true; });
        QSignalSpy rejected(&panel, &CParamPanel::EditRejected);
        bool retained = false;
        QCOMPARE(metadataDialog(panel, id, [&](QDialog* dialog) { metadataText(dialog, "metadataMaximum", text); }, true,
            [&](QDialog* dialog) {
                retained = true;
                auto* edit = dialog->findChild<QLineEdit*>("metadataMaximum");
                auto* error = dialog->findChild<QLabel*>("metadataError");
                QVERIFY(edit && error);
                QCOMPARE(edit->text(), text);
                QTRY_VERIFY(edit->hasFocus());
                QVERIFY(error->isVisible() && !error->text().isEmpty());
                QCOMPARE(error->textFormat(), Qt::PlainText);
                QCOMPARE(document.ParameterMetadataState(id), before);
                QCOMPARE(document.Revision(), revision);
            }), 2);
        QVERIFY(retained);
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(calls, 0);
        QCOMPARE(document.ParameterMetadataState(id), before);
        QCOMPARE(document.Revision(), revision);
        QVERIFY(!document.CanUndoEdit());
    }

    void metadataInvalidInputCanBeCorrectedOrCanceled_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QString>("invalid");
        QTest::addColumn<bool>("keyboard");
        QTest::addColumn<bool>("cancel");
        QTest::newRow("minimum-click") << QStringLiteral("metadataMinimum") << QStringLiteral("nan") << false << false;
        QTest::newRow("maximum-return") << QStringLiteral("metadataMaximum") << QStringLiteral("1e999") << true << false;
        QTest::newRow("default-return") << QStringLiteral("metadataDefault") << QStringLiteral("1000") << true << false;
        QTest::newRow("inverted-range") << QStringLiteral("metadataMinimum") << QStringLiteral("1000") << false << false;
        QTest::newRow("cancel-click") << QStringLiteral("metadataMaximum") << QStringLiteral("invalid") << false << true;
        QTest::newRow("cancel-escape") << QStringLiteral("metadataDefault") << QStringLiteral("invalid") << true << true;
    }

    void metadataInvalidInputCanBeCorrectedOrCanceled()
    {
        QFETCH(QString, field);
        QFETCH(QString, invalid);
        QFETCH(bool, keyboard);
        QFETCH(bool, cancel);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        const QString id = panel.m_rows.front().paramId;
        const auto before = document.ParameterMetadataState(id);
        const auto revision = document.Revision();
        const auto metadata = document.ParameterMetadata(id);
        int submissions = 0;
        panel.SetMetadataEditHandler([&](const auto& edit, QString*) {
            ++submissions;
            return document.ExecuteEdit(edit);
        });
        QSignalSpy rejected(&panel, &CParamPanel::EditRejected);
        QSignalSpy history(&panel, &CParamPanel::HistoryChanged);
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged);
        bool retained = false;
        QCOMPARE(metadataDialog(panel, id, [&](QDialog* dialog) {
            metadataText(dialog, "metadataName", "保留输入后保存");
            auto* input = dialog->findChild<QLineEdit*>(field);
            QVERIFY(input);
            input->setText(invalid);
            input->setFocus();
        }, true, [&](QDialog* dialog) {
            retained = true;
            auto* input = dialog->findChild<QLineEdit*>(field);
            auto* error = dialog->findChild<QLabel*>("metadataError");
            auto* name = dialog->findChild<QLineEdit*>("metadataName");
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            QVERIFY(input && error && name && buttons);
            QVERIFY(dialog->isVisible());
            QCOMPARE(input->text(), invalid);
            QCOMPARE(name->text(), QStringLiteral("保留输入后保存"));
            QTRY_VERIFY(input->hasFocus());
            QVERIFY(error->isVisible() && !error->text().isEmpty());
            QCOMPARE(error->textFormat(), Qt::PlainText);
            QCOMPARE(document.ParameterMetadataState(id), before);
            QCOMPARE(document.Revision(), revision);
            QCOMPARE(submissions, 0);
            QCOMPARE(history.size(), 0);
            if (cancel) {
                if (keyboard) QTest::keyClick(input, Qt::Key_Escape);
                else QTest::mouseClick(buttons->button(QDialogButtonBox::Cancel), Qt::LeftButton);
            } else {
                const float corrected = field == QLatin1String("metadataMinimum") ? metadata.minimum
                    : field == QLatin1String("metadataMaximum") ? metadata.maximum : metadata.defaultValue;
                input->selectAll();
                QTest::keyClicks(input, QString::number(corrected, 'g', 9));
                if (keyboard) QTest::keyClick(input, Qt::Key_Return);
                else QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
            }
            QVERIFY(!dialog->isVisible());
        }, keyboard), 2);
        QVERIFY(retained);
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(submissions, cancel ? 0 : 1);
        QCOMPARE(history.size(), cancel ? 0 : 1);
        QCOMPARE(changed.size(), 0);
        if (cancel) {
            QCOMPARE(document.ParameterMetadataState(id), before);
            QCOMPARE(document.Revision(), revision);
        } else {
            QCOMPARE(document.ParameterMetadata(id).displayName, QStringLiteral("保留输入后保存"));
            QCOMPARE(document.Revision(), revision + 1);
            panel.Undo();
            QCOMPARE(document.ParameterMetadataState(id), before);
            QVERIFY(!document.CanUndoEdit());
        }
    }

    void staleMetadataAfterInvalidSaveCannotBeSubmitted()
    {
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        const QString id = panel.m_rows.front().paramId;
        const auto before = document.ParameterMetadataState(id);
        const auto maximum = document.ParameterMetadata(id).maximum;
        int submissions = 0;
        panel.SetMetadataEditHandler([&](const auto&, QString*) { ++submissions; return true; });
        QSignalSpy rejected(&panel, &CParamPanel::EditRejected);
        bool retried = false;
        QCOMPARE(metadataDialog(panel, id, [](QDialog* dialog) {
            metadataText(dialog, "metadataMaximum", "invalid");
        }, true, [&](QDialog* dialog) {
            retried = true;
            QVERIFY(document.SetParameterValue(id, document.ParameterValue(id)));
            auto* input = dialog->findChild<QLineEdit*>("metadataMaximum");
            input->setText(QString::number(maximum, 'g', 9));
            QTest::keyClick(input, Qt::Key_Return);
            QVERIFY(!dialog->isVisible());
        }), 2);
        QVERIFY(retried);
        QCOMPARE(rejected.size(), 2);
        QCOMPARE(submissions, 0);
        QCOMPARE(document.ParameterMetadata(id).displayName, before.value("displayName").toString());
        QVERIFY(document.ParameterMetadataOverrides().isEmpty());
        QVERIFY(!document.CanUndoEdit());
    }

    void metadataFieldsGrowWithTheDialogAndRemainInsideNarrowWindows_data()
    {
        QTest::addColumn<QString>("styleName");
        for (const auto& name : QStyleFactory::keys())
            if (name == QLatin1String("Fusion") || name == QLatin1String("macOS"))
                QTest::newRow(qPrintable(name)) << name;
    }

    void metadataFieldsGrowWithTheDialogAndRemainInsideNarrowWindows()
    {
        QFETCH(QString, styleName);
        std::unique_ptr<QStyle> style(QStyleFactory::create(styleName));
        QVERIFY(style);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel;
        panel.setStyle(style.get());
        panel.SetDocument(&document);
        panel.BuildParams();
        const QString id = panel.m_rows.front().paramId;
        const auto before = document.ParameterMetadataState(id);
        QCOMPARE(metadataDialog(panel, id, [](QDialog* dialog) {
            auto* layout = qobject_cast<QFormLayout*>(dialog->layout());
            QVERIFY(layout);
            dialog->resize(280, dialog->height());
            layout->activate();
            const int narrow = dialog->width();
            QMap<QString, int> widths;
            for (const QString& name : {QStringLiteral("metadataName"), QStringLiteral("metadataMinimum"),
                    QStringLiteral("metadataMaximum"), QStringLiteral("metadataDefault")}) {
                auto* input = dialog->findChild<QLineEdit*>(name);
                QVERIFY(input);
                widths.insert(name, input->width());
                QVERIFY(dialog->rect().contains(QRect(input->mapTo(dialog, QPoint()), input->size())));
            }
            dialog->resize(560, dialog->height());
            layout->activate();
            const int growth = dialog->width() - narrow;
            QVERIFY(growth > 0);
            for (auto entry = widths.cbegin(); entry != widths.cend(); ++entry) {
                auto* input = dialog->findChild<QLineEdit*>(entry.key());
                QVERIFY(input->width() - entry.value() >= growth * .9);
                QVERIFY(dialog->rect().contains(QRect(input->mapTo(dialog, QPoint()), input->size())));
            }
        }, false), 2);
        QCOMPARE(document.ParameterMetadataState(id), before);
        QVERIFY(!document.CanUndoEdit());
    }

    void metadataHistoryFailureLeavesEverythingAccepted_data()
    {
        QTest::addColumn<bool>("throwError");
        QTest::newRow("reject") << false;
        QTest::newRow("throw") << true;
    }

    void metadataHistoryFailureLeavesEverythingAccepted()
    {
        QFETCH(bool, throwError);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        const QString id = panel.m_rows.front().paramId;
        QCOMPARE(metadataDialog(panel, id, [](QDialog* dialog) { metadataText(dialog, "metadataName", "accepted"); }), 2);
        const auto before = document.ParameterMetadataState(id);
        const auto revision = document.Revision();
        const auto values = panel.CurrentValues();
        panel.SetHistoryHandler([&](bool, auto&, QString* error) -> bool {
            if (throwError) throw std::runtime_error("history exception");
            *error = QStringLiteral("history refused");
            return false;
        });
        QSignalSpy rejected(&panel, &CParamPanel::EditRejected);
        QSignalSpy history(&panel, &CParamPanel::HistoryChanged);
        QSignalSpy metadata(&panel, &CParamPanel::ParameterMetadataChanged);
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged);
        panel.Undo();
        QCOMPARE(document.ParameterMetadataState(id), before);
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(panel.CurrentValues(), values);
        QVERIFY(document.CanUndoEdit());
        QVERIFY(!document.CanRedoEdit());
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(history.size(), 0);
        QCOMPARE(metadata.size(), 0);
        QCOMPARE(changed.size(), 0);
    }

    void metadataPreviewBaselineAndDisplayedValuesRemainSeparate()
    {
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString id = document.Parameters().front().id;
        auto metadata = document.ParameterMetadata(id);
        metadata.maximum += 20;
        QVERIFY(document.SetParameterMetadata(id, metadata));
        const float base = metadata.maximum - 2;
        QVERIFY(document.SetParameterValue(id, base));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        const QString other = panel.m_rows[1].paramId;
        const float otherPreview = panel.m_rows[1].minVal;
        const auto ordinary = panel.CurrentValues();
        const float displayed = metadata.minimum;
        panel.SetPreviewValues({{id, displayed}, {other, otherPreview}}, ordinary);
        const float narrowed = metadata.maximum - 10;
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged);
        QCOMPARE(metadataDialog(panel, id, [&](QDialog* dialog) {
            metadataText(dialog, "metadataMaximum", QString::number(narrowed, 'g', 9));
        }), 2);
        QVERIFY(panel.m_previewActive);
        QCOMPARE(panel.CurrentValues().value(id), displayed);
        QCOMPARE(panel.m_previewBaseline.value(id), narrowed);
        QCOMPARE(panel.CurrentValues().value(other), otherPreview);
        panel.Undo();
        QCOMPARE(panel.CurrentValues().value(id), displayed);
        QCOMPARE(panel.m_previewBaseline.value(id), base);
        QCOMPARE(panel.CurrentValues().value(other), otherPreview);
        QCOMPARE(changed.size(), 0);
    }

    void documentBuildCreatesAllRowsWithoutRuntime_data()
    {
        QTest::addColumn<QString>("model");
        QTest::addColumn<int>("expectedCount");
        QTest::newRow("Mark") << QStringLiteral("Mark") << 21;
        QTest::newRow("Natori") << QStringLiteral("Natori") << 96;
        QTest::newRow("Rice") << QStringLiteral("Rice") << 96;
        QTest::newRow("Wanko") << QStringLiteral("Wanko") << 25;
    }

    void documentBuildCreatesAllRowsWithoutRuntime()
    {
        QFETCH(QString, model);
        QFETCH(int, expectedCount);
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3", &error), qPrintable(error));
        const auto storedValues = document.ParameterValues();
        CParamPanel panel;
        panel.SetDocument(&document);
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged);
        panel.BuildParams();
        QCOMPARE(panel.m_rows.size(), expectedCount);
        QCOMPARE(document.Parameters().size(), expectedCount);
        QCOMPARE(panel.m_xyXSelector->count(), expectedCount);
        QCOMPARE(panel.m_xyYSelector->count(), expectedCount);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(document.ParameterValues(), storedValues);
        QVERIFY(!document.CanUndoEdit());
        bool hasNonzeroDefault = false;
        for (int index = 0; index < expectedCount; ++index) {
            const auto& source = document.Parameters()[index];
            const auto metadata = document.ParameterMetadata(source.id);
            const auto& row = panel.m_rows[index];
            QCOMPARE(row.paramId, source.id);
            QCOMPARE(row.modelIndex, index);
            QCOMPARE(row.minVal, metadata.minimum);
            QCOMPARE(row.maxVal, metadata.maximum);
            QCOMPARE(row.defaultVal, metadata.defaultValue);
            QCOMPARE(row.spinBox->decimals(), metadata.decimalPlaces);
            QCOMPARE(panel.CurrentValues().value(source.id), metadata.defaultValue);
            const double tolerance = std::pow(10.0, -metadata.decimalPlaces);
            QVERIFY(std::abs(row.spinBox->value() - double(metadata.defaultValue)) <= tolerance);
            hasNonzeroDefault = hasNonzeroDefault || metadata.defaultValue != 0;
        }
        QVERIFY(hasNonzeroDefault);
    }

    void documentBuildUsesEditedMetadataAndCurrentValuesSilently()
    {
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
        const QString id = document.Parameters().front().id;
        auto metadata = document.ParameterMetadata(id);
        metadata.displayName = QStringLiteral("编辑后的参数");
        metadata.minimum = -4;
        metadata.maximum = 6;
        metadata.defaultValue = 1.25f;
        metadata.decimalPlaces = 6;
        metadata.repeat = true;
        QVERIFY(document.SetParameterMetadata(id, metadata));
        QVERIFY(document.SetParameterValue(id, 2.375f));
        const live2dstruct::CModelEdit edit{id, QStringLiteral("parameter"),
            live2dstruct::CModelEdit::ParameterValue, 2.375f, 3.125f};
        QVERIFY(document.ExecuteEdit(edit));
        QVERIFY(document.CanUndoEdit());
        const auto before = document.ParameterValues();
        CParamPanel panel;
        panel.SetDocument(&document);
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged);
        panel.BuildParams();
        QCOMPARE(panel.m_rows.size(), document.Parameters().size());
        const auto& row = panel.m_rows.front();
        QCOMPARE(row.paramId, id);
        QCOMPARE(row.modelIndex, 0);
        QCOMPARE(row.minVal, -4.0f);
        QCOMPARE(row.maxVal, 6.0f);
        QCOMPARE(row.defaultVal, 1.25f);
        QCOMPARE(row.nameLabel->text(), metadata.displayName);
        QCOMPARE(row.spinBox->decimals(), 6);
        QCOMPARE(row.spinBox->minimum(), -4.0);
        QCOMPARE(row.spinBox->maximum(), 6.0);
        QCOMPARE(row.spinBox->value(), 3.125);
        QCOMPARE(row.slider->value(), 713);
        QCOMPARE(panel.CurrentValues().value(id), 3.125f);
        QCOMPARE(document.ParameterValues(), before);
        QVERIFY(document.ParameterMetadata(id) == metadata);
        QVERIFY(!document.CanUndoEdit());
        QCOMPARE(changed.count(), 0);
    }

    void documentBuildKeepsSourceIndicesThroughCidGroupsAndBlendTags()
    {
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        QVERIFY(document.Parameters().size() >= 4);
        const QString first = document.Parameters()[0].id;
        const QString second = document.Parameters()[1].id;
        const QString third = document.Parameters()[2].id;
        auto metadata = document.ParameterMetadata(first);
        metadata.displayName = QStringLiteral("Document name");
        QVERIFY(document.SetParameterMetadata(first, metadata));
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QJsonObject content{
            {"ParameterGroups", QJsonArray{QJsonObject{{"Id", "A"}, {"Name", "Group A"}},
                                           QJsonObject{{"Id", "B"}, {"Name", "Group B"}}}},
            {"Parameters", QJsonArray{QJsonObject{{"Id", third}, {"Name", "Third name"}, {"GroupId", "A"}},
                                      QJsonObject{{"Id", first}, {"Name", "CDI name"}, {"GroupId", "A"}},
                                      QJsonObject{{"Id", "missing"}, {"Name", "Missing"}, {"GroupId", "B"}},
                                      QJsonObject{{"Id", third}, {"Name", "Third name"}, {"GroupId", "B"}}}},
            {"CombinedParameters", QJsonArray{QJsonArray{first, third}}}
        };
        const QString path = temporary.filePath("groups.cdi3.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray bytes = QJsonDocument(content).toJson();
        QCOMPARE(file.write(bytes), qint64(bytes.size()));
        file.close();
        CCidJson cid;
        QString error;
        QVERIFY2(cid.Parse(path, &error), qPrintable(error));
        CParamPanel panel;
        panel.SetDocument(&document);
        const QSet<QString> explicitBlend{second};
        panel.BuildParams(&cid, &explicitBlend);
        QCOMPARE(panel.m_rows.size(), document.Parameters().size());
        QCOMPARE(panel.m_rows[0].paramId, third);
        QCOMPARE(panel.m_rows[0].modelIndex, 2);
        QCOMPARE(panel.m_rows[1].paramId, first);
        QCOMPARE(panel.m_rows[1].modelIndex, 0);
        QCOMPARE(panel.m_rows[2].paramId, second);
        QCOMPARE(panel.m_rows[2].modelIndex, 1);
        QSet<QString> seen;
        for (const auto& row : panel.m_rows) {
            QVERIFY(!seen.contains(row.paramId));
            seen.insert(row.paramId);
            QCOMPARE(document.Parameters()[row.modelIndex].id, row.paramId);
            const bool blend = row.paramId == second;
            QCOMPARE(row.blendShape, blend);
            QCOMPARE(row.nameLabel->objectName() == QStringLiteral("blendParamTag"), blend);
        }
        QCOMPARE(panel.m_rows[0].nameLabel->text(), QStringLiteral("Third name"));
        QCOMPARE(panel.m_rows[1].nameLabel->text(), QStringLiteral("Document name"));
        panel.m_search->setText(QStringLiteral("Third name"));
        for (const auto& row : panel.m_rows) QCOMPARE(row.widget->isHidden(), row.paramId != third);
        panel.show();
        panel.activateWindow();
        QApplication::processEvents();
        auto* group = panel.m_groupButtons.value(panel.m_rows[1].widget->parentWidget());
        QVERIFY(group);
        group->setChecked(false);
        panel.m_search->setText(QStringLiteral("no matching parameter"));
        const auto before = panel.CurrentValues();
        const auto revision = document.Revision();
        QSignalSpy changes(&panel, &CParamPanel::ParamChanged);
        QSignalSpy history(&panel, &CParamPanel::HistoryChanged);
        QVERIFY(panel.FocusParameter(first));
        QVERIFY(group->isChecked());
        QVERIFY(panel.m_search->text().isEmpty());
        QVERIFY(panel.m_rows[1].widget->isVisibleTo(&panel));
        QCOMPARE(panel.CurrentValues(), before);
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(changes.size(), 0);
        QCOMPARE(history.size(), 0);
        float value = 42;
        QVERIFY(panel.CurrentValue(first, value));
        QCOMPARE(value, before.value(first));
        value = 42;
        QVERIFY(!panel.CurrentValue(QStringLiteral("missing"), value));
        QCOMPARE(value, 42.f);
        QVERIFY(!panel.FocusParameter(QStringLiteral("missing")));
    }

    void sourceBlendShapeTypeIsRecognizedWithoutCidHints()
    {
        QFile file(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray bytes = file.readAll();
        const int counts = qFromLittleEndian<qint32>(bytes.constData() + 0x40);
        const int parameters = qFromLittleEndian<qint32>(bytes.constData() + counts + 5 * 4);
        QVERIFY(parameters > 1);
        const int offset = (bytes.size() + 63) & ~63;
        bytes.append(QByteArray(offset + parameters * 4 - bytes.size(), '\0'));
        qToLittleEndian<qint32>(offset, bytes.data() + 0x208);
        qToLittleEndian<qint32>(1, bytes.data() + offset);
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(bytes, "typed.moc3", &error), qPrintable(error));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        QCOMPARE(panel.m_rows.size(), parameters);
        for (int row = 0; row < panel.m_rows.size(); ++row) {
            QCOMPARE(panel.m_rows[row].blendShape, row == 0);
            QCOMPARE(panel.m_rows[row].nameLabel->objectName() == QStringLiteral("blendParamTag"), row == 0);
        }
        const auto values = panel.CurrentValues();
        panel.SetPreEditGuard([](QString* issue) { *issue = QStringLiteral("rejected edit"); return false; });
        panel.ApplyExternalValue(document.Parameters().front().id, 1, true);
        float actual = 0;
        QVERIFY(panel.CurrentValue(document.Parameters().front().id, actual));
        QCOMPARE(actual, values.value(document.Parameters().front().id));
        QSignalSpy metadata(&panel, &CParamPanel::ParameterMetadataChanged);
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged);
        panel.RefreshParameterMetadata();
        QCOMPARE(metadata.size(), 1);
        QCOMPARE(changed.size(), 0);
    }

    void invalidSourceMetadataNeverReachesWidgetsOrRuntime()
    {
        QFile file(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray bytes = file.readAll();
        const int defaultOffset = qFromLittleEndian<qint32>(bytes.constData() + 0x40 + 53 * 4);
        QVERIFY(defaultOffset > 0 && defaultOffset <= bytes.size() - 4);
        qToLittleEndian<quint32>(0x7fc00000u, reinterpret_cast<uchar*>(bytes.data() + defaultOffset));
        live2dstruct::CModelDocument document;
        QVERIFY(document.LoadBytes(bytes, file.fileName()));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        QCOMPARE(panel.m_rows.size(), document.Parameters().size() - 1);
        QVERIFY(!panel.CurrentValues().contains(document.Parameters().front().id));
        for (const auto& row : panel.m_rows) {
            QVERIFY(std::isfinite(row.minVal) && std::isfinite(row.maxVal) && std::isfinite(row.defaultVal));
            QCOMPARE(document.Parameters()[row.modelIndex].id, row.paramId);
        }
    }

    void restorationUsesBoundedSliderMath_data()
    {
        QTest::addColumn<float>("minimum");
        QTest::addColumn<float>("maximum");
        QTest::addColumn<float>("defaultValue");
        QTest::addColumn<float>("restoredValue");
        QTest::addColumn<int>("defaultSlider");
        QTest::addColumn<int>("restoredSlider");
        const float largest = std::numeric_limits<float>::max();
        const float tiny = std::numeric_limits<float>::denorm_min();
        QTest::newRow("full-float-range") << -largest << largest << 0.0f << largest * 0.75f << 500 << 875;
        QTest::newRow("positive-edge") << largest * 0.5f << largest << largest * 0.5f << largest << 0 << 1000;
        QTest::newRow("subnormal") << 0.0f << tiny * 8 << tiny * 4 << tiny * 6 << 500 << 750;
        QTest::newRow("fixed") << 3.5f << 3.5f << 3.5f << 3.5f << 0 << 0;
    }

    void restorationUsesBoundedSliderMath()
    {
        QFETCH(float, minimum);
        QFETCH(float, maximum);
        QFETCH(float, defaultValue);
        QFETCH(float, restoredValue);
        QFETCH(int, defaultSlider);
        QFETCH(int, restoredSlider);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString id = document.Parameters().front().id;
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        auto metadata = document.ParameterMetadata(id);
        metadata.minimum = minimum;
        metadata.maximum = maximum;
        metadata.defaultValue = defaultValue;
        metadata.decimalPlaces = 9;
        QVERIFY(document.SetParameterMetadata(id, metadata));
        const QMap<QString, float> restored{{id, restoredValue}};
        panel.RefreshParameterMetadata();
        panel.ResetAll();
        QCOMPARE(panel.m_rows.front().slider->value(), defaultSlider);
        QCOMPARE(panel.CurrentValues().value(id), defaultValue);
        panel.ApplyValues(restored);
        auto& row = panel.m_rows.front();
        QCOMPARE(row.slider->value(), restoredSlider);
        QCOMPARE(panel.CurrentValues().value(id), restoredValue);
        QCOMPARE(document.ParameterValue(id), restoredValue);
        QVERIFY(std::isfinite(row.spinBox->value()));
        QVERIFY(std::isfinite(row.spinBox->singleStep()));
        QVERIFY(!document.CanUndoEdit());
        row.slider->setValue(1000);
        QCOMPARE(panel.CurrentValues().value(id), maximum);
        QCOMPARE(document.ParameterValue(id), maximum);
        row.slider->setValue(0);
        QCOMPARE(panel.CurrentValues().value(id), minimum);
        QCOMPARE(document.ParameterValue(id), minimum);
    }

    void narrowedSessionRangeClampsOldUiValuesBeforeRestoration()
    {
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString id = document.Parameters().front().id;
        auto metadata = document.ParameterMetadata(id);
        metadata.minimum = -std::numeric_limits<float>::max();
        metadata.maximum = std::numeric_limits<float>::max();
        QVERIFY(document.SetParameterMetadata(id, metadata));
        QVERIFY(document.SetParameterValue(id, metadata.maximum));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        live2dstruct::CModelDocument candidate;
        QVERIFY(candidate.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        metadata.minimum = -1;
        metadata.maximum = 1;
        metadata.defaultValue = 0.25f;
        QVERIFY(candidate.SetParameterMetadata(id, metadata));
        QVERIFY(candidate.SetParameterValue(id, -0.75f));
        document = std::move(candidate);
        const QMap<QString, float> values{{id, document.ParameterValue(id)}};
        panel.SetDocument(nullptr);
        panel.SetDocument(&document);
        panel.RefreshParameterMetadata();
        QCOMPARE(panel.CurrentValues().value(id), 1.0f);
        QCOMPARE(panel.m_rows.front().slider->value(), 1000);
        QCOMPARE(document.ParameterValue(id), -0.75f);
        panel.ResetAll();
        QCOMPARE(panel.m_rows.front().slider->value(), 625);
        panel.ApplyValues(values);
        QCOMPARE(panel.CurrentValues().value(id), -0.75f);
        QCOMPARE(document.ParameterValue(id), -0.75f);
        QCOMPARE(panel.m_rows.front().slider->value(), 125);
        QVERIFY(!document.CanUndoEdit());
    }

    void directSubnormalEditingKeepsRealFloatHistory()
    {
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString id = document.Parameters().front().id;
        const float tiny = std::numeric_limits<float>::denorm_min();
        auto metadata = document.ParameterMetadata(id);
        metadata.minimum = 0;
        metadata.maximum = tiny * 8;
        metadata.defaultValue = 0;
        metadata.decimalPlaces = 9;
        QVERIFY(document.SetParameterMetadata(id, metadata));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        panel.m_rows.front().slider->setValue(125);
        QCOMPARE(panel.CurrentValues().value(id), tiny);
        QCOMPARE(document.ParameterValue(id), tiny);
        live2dstruct::CModelEdit command;
        QVERIFY(document.PeekUndoEdit(&command));
        QCOMPARE(command.before.toFloat(), 0.0f);
        QCOMPARE(command.after.toFloat(), tiny);
        panel.Undo();
        QCOMPARE(panel.CurrentValues().value(id), 0.0f);
        QCOMPARE(panel.m_rows.front().slider->value(), 0);
        panel.Redo();
        QCOMPARE(panel.CurrentValues().value(id), tiny);
        QCOMPARE(panel.m_rows.front().slider->value(), 125);
    }

    void unchangedRoundedDisplayDoesNotRewriteStoredPrecision()
    {
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString id = document.Parameters().front().id;
        const float value = 0.12345679f;
        QVERIFY(document.SetParameterValue(id, value));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged);
        QCOMPARE(panel.m_rows.front().spinBox->value(), 0.123);
        QTest::keyClick(panel.m_rows.front().spinBox, Qt::Key_Return);
        QCOMPARE(panel.CurrentValues().value(id), value);
        QCOMPARE(document.ParameterValue(id), value);
        QCOMPARE(changed.count(), 0);
        QVERIFY(!document.CanUndoEdit());

        const float updated = 0.45678912f;
        panel.ApplyExternalValue(id, updated, false, false);
        changed.clear();
        QCOMPARE(panel.m_rows.front().spinBox->value(), 0.457);
        QTest::keyClick(panel.m_rows.front().spinBox, Qt::Key_Return);
        QCOMPARE(panel.CurrentValues().value(id), updated);
        QCOMPARE(document.ParameterValue(id), updated);
        QCOMPARE(changed.count(), 0);
        QVERIFY(!document.CanUndoEdit());
    }

    void roundedSpinBoxStillAcceptsStepsAndKeyboardEdits_data()
    {
        QTest::addColumn<int>("editKind");
        QTest::addColumn<float>("expected");
        QTest::newRow("step-up") << 0 << 0.223f;
        QTest::newRow("step-down") << 1 << 0.023f;
        QTest::newRow("up-key") << 2 << 0.223f;
        QTest::newRow("down-key") << 3 << 0.023f;
        QTest::newRow("keyboard-tracking") << 4 << 0.375f;
        QTest::newRow("keyboard-commit") << 5 << 0.375f;
    }

    void roundedSpinBoxStillAcceptsStepsAndKeyboardEdits()
    {
        QFETCH(int, editKind);
        QFETCH(float, expected);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString id = document.Parameters().front().id;
        const float original = 0.12345679f;
        QVERIFY(document.SetParameterValue(id, original));
        CParamPanel panel;
        panel.SetDocument(&document);
        panel.BuildParams();
        auto* spinBox = panel.m_rows.front().spinBox;
        spinBox->setSingleStep(0.1);
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged);
        switch (editKind) {
        case 0: spinBox->stepUp(); break;
        case 1: spinBox->stepDown(); break;
        case 2: QTest::keyClick(spinBox, Qt::Key_Up); break;
        case 3: QTest::keyClick(spinBox, Qt::Key_Down); break;
        default:
            spinBox->setKeyboardTracking(editKind == 4);
            spinBox->selectAll();
            QTest::keyClicks(spinBox, spinBox->locale().toString(0.375, 'f', 3));
            QTest::keyClick(spinBox, Qt::Key_Return);
            break;
        }
        QCOMPARE(panel.CurrentValues().value(id), expected);
        QCOMPARE(document.ParameterValue(id), expected);
        QVERIFY(changed.count() > 0);
        QVERIFY(document.CanUndoEdit());
        const int editCount = changed.count();
        QTest::keyClick(spinBox, Qt::Key_Return);
        QCOMPARE(changed.count(), editCount);
        for (int index = 0; index < editCount; ++index) panel.Undo();
        QCOMPARE(panel.CurrentValues().value(id), original);
        QCOMPARE(document.ParameterValue(id), original);
        QVERIFY(!document.CanUndoEdit());
        for (int index = 0; index < editCount; ++index) panel.Redo();
        QCOMPARE(panel.CurrentValues().value(id), expected);
        QCOMPARE(document.ParameterValue(id), expected);
    }

    void previewIsSilentAndRestoresBaseline()
    {
        live2dstruct::CModelDocument document;
        CParamPanel panel;
        initialize(panel, document);
        const auto original = document.ParameterValues();
        const auto baseline = valuesAt(panel, 0.25f);
        const auto preview = valuesAt(panel, 0.75f);
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged);
        QSignalSpy history(&panel, &CParamPanel::HistoryChanged);
        panel.SetPreviewValues(preview, baseline);
        QCOMPARE(panel.CurrentValues(), preview);
        QCOMPARE(document.ParameterValues(), original);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(history.count(), 0);
        panel.SetPreviewValues(valuesAt(panel, 0.9f), valuesAt(panel, 0.5f));
        panel.ClearPreviewValues();
        QCOMPARE(panel.CurrentValues(), baseline);
        QCOMPARE(document.ParameterValues(), original);
        QCOMPARE(changed.count(), 0);
        QVERIFY(!document.CanUndoEdit());
    }

    void externalEditPreservesUserInputAndRecordsBase()
    {
        live2dstruct::CModelDocument document;
        CParamPanel panel;
        initialize(panel, document);
        const auto baseline = valuesAt(panel, 0.25f);
        const auto edited = valuesAt(panel, 0.6f);
        const QString id = panel.m_rows.first().paramId;
        const QString otherId = panel.m_rows.last().paramId;
        restoreOnEdit(panel, baseline);
        panel.SetPreviewValues(valuesAt(panel, 0.75f), baseline);
        panel.ApplyExternalValue(id, edited.value(id), true);
        live2dstruct::CModelEdit command;
        QVERIFY(document.PeekUndoEdit(&command));
        QCOMPARE(command.before.toFloat(), baseline.value(id));
        QCOMPARE(command.after.toFloat(), edited.value(id));
        QCOMPARE(panel.CurrentValues().value(id), edited.value(id));
        QCOMPARE(panel.CurrentValues().value(otherId), baseline.value(otherId));
        QVERIFY(!panel.m_previewActive);
        panel.Undo();
        QCOMPARE(document.ParameterValue(id), baseline.value(id));
        panel.Redo();
        QCOMPARE(document.ParameterValue(id), edited.value(id));
    }

    void editPathsRecordBaseline_data()
    {
        QTest::addColumn<int>("path");
        QTest::newRow("value") << 0;
        QTest::newRow("reset-all") << 1;
        QTest::newRow("preset") << 2;
    }

    void untouchedSliderDoesNotCommitPreview()
    {
        live2dstruct::CModelDocument document;
        CParamPanel panel;
        initialize(panel, document);
        const auto original = document.ParameterValues();
        const auto baseline = valuesAt(panel, 0.25f);
        const auto preview = valuesAt(panel, 0.75f);
        panel.SetPreviewValues(preview, baseline);
        QSignalSpy changed(&panel, &CParamPanel::ParamChanged);
        QSignalSpy history(&panel, &CParamPanel::HistoryChanged);
        panel.BeginParameterDrag(0);
        panel.EndParameterDrag(0);
        QCOMPARE(panel.CurrentValues(), preview);
        QCOMPARE(document.ParameterValues(), original);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(history.count(), 0);
        QVERIFY(!document.CanUndoEdit());
        QVERIFY(!panel.m_userLocked.contains(0));
        QVERIFY(panel.m_userDragging.isEmpty());
        QVERIFY(panel.m_changedDrags.isEmpty());
        QVERIFY(panel.m_dragStarts.isEmpty());
        QVERIFY(panel.m_previewActive);
        panel.BeginParameterDrag(0);
        panel.ApplyValue(0, preview.value(panel.m_rows.first().paramId), false);
        panel.EndParameterDrag(0);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(history.count(), 0);
        QCOMPARE(document.ParameterValues(), original);
        QVERIFY(!panel.m_userLocked.contains(0));
    }

    void changedSliderCommitsBaselineOnce_data()
    {
        QTest::addColumn<bool>("returnToPreview");
        QTest::newRow("new-value") << false;
        QTest::newRow("back-to-preview") << true;
    }

    void changedSliderCommitsBaselineOnce()
    {
        QFETCH(bool, returnToPreview);
        live2dstruct::CModelDocument document;
        CParamPanel panel;
        initialize(panel, document);
        const QString id = panel.m_rows.first().paramId;
        const auto baseline = valuesAt(panel, 0.25f);
        const auto preview = valuesAt(panel, 0.75f);
        restoreOnEdit(panel, baseline);
        panel.SetPreviewValues(preview, baseline);
        panel.BeginParameterDrag(0);
        panel.ApplyValue(0, valuesAt(panel, 0.6f).value(id), false);
        QVERIFY(!document.CanUndoEdit());
        QVERIFY(!panel.m_previewActive);
        const float finalValue = returnToPreview ? preview.value(id) : valuesAt(panel, 0.65f).value(id);
        panel.ApplyValue(0, finalValue, false);
        QVERIFY(!document.CanUndoEdit());
        panel.EndParameterDrag(0);
        live2dstruct::CModelEdit command;
        QVERIFY(document.UndoEdit(&command));
        QCOMPARE(command.before.toFloat(), baseline.value(id));
        QCOMPARE(command.after.toFloat(), finalValue);
        QCOMPARE(document.ParameterValue(id), baseline.value(id));
        QVERIFY(!document.CanUndoEdit());
        QVERIFY(panel.m_userLocked.contains(0));
        QVERIFY(panel.m_userDragging.isEmpty());
        QVERIFY(panel.m_changedDrags.isEmpty());
        QVERIFY(panel.m_dragStarts.isEmpty());
    }

    void resetDiscardsPendingSliderLifecycle()
    {
        live2dstruct::CModelDocument document;
        CParamPanel panel;
        initialize(panel, document);
        panel.BeginParameterDrag(0);
        panel.ApplyValue(0, valuesAt(panel, 0.6f).value(panel.m_rows.first().paramId), false);
        QVERIFY(panel.m_changedDrags.contains(0));
        panel.ResetAll();
        QVERIFY(panel.m_userDragging.isEmpty());
        QVERIFY(panel.m_changedDrags.isEmpty());
        QVERIFY(panel.m_dragStarts.isEmpty());
        QSignalSpy history(&panel, &CParamPanel::HistoryChanged);
        panel.EndParameterDrag(0);
        QCOMPARE(history.count(), 0);
        QVERIFY(!panel.m_userLocked.contains(0));
    }

    void editPathsRecordBaseline()
    {
        QFETCH(int, path);
        live2dstruct::CModelDocument document;
        CParamPanel panel;
        initialize(panel, document);
        const auto baseline = valuesAt(panel, 0.25f);
        const auto edited = valuesAt(panel, 0.6f);
        restoreOnEdit(panel, baseline);
        panel.SetPreviewValues(valuesAt(panel, 0.75f), baseline);
        if (path == 0) panel.ApplyValue(0, edited.value(panel.m_rows.first().paramId), true);
        if (path == 1) panel.ResetAll();
        if (path == 2) panel.ApplyValuesInternal(edited, true);
        int count = 0;
        live2dstruct::CModelEdit command;
        while (document.UndoEdit(&command)) {
            QCOMPARE(command.before.toFloat(), baseline.value(command.name));
            QCOMPARE(document.ParameterValue(command.name), baseline.value(command.name));
            ++count;
        }
        QCOMPARE(count, path == 0 ? 1 : 2);
        QVERIFY(!panel.m_previewActive);
    }

    void xyEditRecordsBothBaselineValues()
    {
        live2dstruct::CModelDocument document;
        CParamPanel panel;
        initialize(panel, document);
        const auto baseline = valuesAt(panel, 0.25f);
        const auto edited = valuesAt(panel, 0.6f);
        restoreOnEdit(panel, baseline);
        panel.SetPreviewValues(valuesAt(panel, 0.75f), baseline);
        panel.ApplyXYPad(edited.value(panel.m_rows.first().paramId), edited.value(panel.m_rows.last().paramId));
        live2dstruct::CModelEdit command;
        QVERIFY(document.PeekUndoEdit(&command));
        QCOMPARE(command.field, live2dstruct::CModelEdit::ParameterValues);
        QCOMPARE((command.before.value<QMap<QString, float>>()), baseline);
        QCOMPARE(panel.CurrentValues(), edited);
        QVERIFY(document.UndoEdit());
        for (auto it = baseline.cbegin(); it != baseline.cend(); ++it)
            QCOMPARE(document.ParameterValue(it.key()), it.value());
    }

    void xyReturningToBaselineStopsPreviewWithoutHistory()
    {
        live2dstruct::CModelDocument document;
        CParamPanel panel;
        initialize(panel, document);
        const auto baseline = valuesAt(panel, 0.25f);
        restoreOnEdit(panel, baseline);
        panel.SetPreviewValues(valuesAt(panel, 0.75f), baseline);
        panel.ApplyXYPad(baseline.value(panel.m_rows.first().paramId), baseline.value(panel.m_rows.last().paramId));
        QCOMPARE(panel.CurrentValues(), baseline);
        QVERIFY(!panel.m_previewActive);
        QVERIFY(!document.CanUndoEdit());
    }

    void previewRejectsNonFiniteValuesAndClearsOnDocumentChange()
    {
        live2dstruct::CModelDocument document;
        CParamPanel panel;
        initialize(panel, document);
        const auto baseline = valuesAt(panel, 0.25f);
        const auto preview = valuesAt(panel, 0.75f);
        panel.SetPreviewValues(preview, baseline);
        panel.ApplyExternalValue(panel.m_rows.first().paramId, std::numeric_limits<float>::infinity(), true);
        QCOMPARE(panel.CurrentValues(), preview);
        QVERIFY(!document.CanUndoEdit());
        panel.SetDocument(nullptr);
        QVERIFY(!panel.m_previewActive);
        QVERIFY(panel.m_previewBaseline.isEmpty());
    }
};

QTEST_MAIN(ParamPanelTests)
#include "param_panel_tests.moc"
