#include "ui/cparampanel.h"
#include "ui/cxyparameterpad.h"
#include "live2dstruct/cmodeldocument.h"
#include "ui/para/ccidjson.h"
#include <QtTest>
#include <QComboBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>
#include <QApplication>
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
        QObject::connect(&panel, &CParamPanel::ParamChanged, &panel,
                         [&panel, baseline](const QString& id, float, bool isInit){
            if (!isInit && panel.m_previewActive) panel.ClearPreviewValues(baseline, id);
        });
    }

private slots:
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
