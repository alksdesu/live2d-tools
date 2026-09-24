#include "ui/cparampanel.h"
#include "live2dstruct/cmodeldocument.h"
#include <QtTest>
#include <QComboBox>
#include <QSignalSpy>
#include <QToolButton>
#include <limits>

class ParamPanelTests : public QObject
{
    Q_OBJECT
private:
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
