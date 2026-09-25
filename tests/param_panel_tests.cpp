#include "ui/cparampanel.h"
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
#include <QtEndian>
#include <cmath>
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
            const bool blend = row.paramId == first || row.paramId == second || row.paramId == third;
            QCOMPARE(row.blendShape, blend);
            QCOMPARE(row.nameLabel->objectName() == QStringLiteral("blendParamTag"), blend);
        }
        QCOMPARE(panel.m_rows[0].nameLabel->text(), QStringLiteral("[融合] Third name"));
        QCOMPARE(panel.m_rows[1].nameLabel->text(), QStringLiteral("[融合] Document name"));
        panel.m_search->setText(QStringLiteral("Third name"));
        for (const auto& row : panel.m_rows) QCOMPARE(row.widget->isHidden(), row.paramId != third);
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
