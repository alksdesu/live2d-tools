#include "ui/cbindingkeysdialog.h"
#include "live2dstruct/cmodeldocument.h"
#include "ui/theme.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyleFactory>
#include <QTableView>
#include <QtTest>
#include <cmath>
#include <limits>

using namespace live2dstruct;
using Builder = CMocParameterStructureBuilder;

namespace {
CKeyformGridSnapshot grid(const QString& model = "Mark", const QString& id = "EyeL4", const QString& type = "artmesh")
{
    CModelDocument document; QString error; CKeyformGridSnapshot result;
    if (!document.Load(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3", &error)
        || !document.DescribeKeyformGrid(id, type, result, &error)) qFatal("Grid fixture: %s", qPrintable(error));
    return result;
}
template<class T> T* control(CBindingKeysDialog& dialog, const char* name) { return dialog.findChild<T*>(QLatin1String(name)); }
QTableView* table(CBindingKeysDialog& dialog) { return control<QTableView>(dialog, "bindingKeysTable"); }
QPushButton* apply(CBindingKeysDialog& dialog) { return control<QPushButton>(dialog, "bindingKeysApply"); }
QPushButton* add(CBindingKeysDialog& dialog) { return control<QPushButton>(dialog, "bindingKeysAdd"); }
QLineEdit* addValue(CBindingKeysDialog& dialog) { return control<QLineEdit>(dialog, "bindingKeysAddValue"); }
bool edit(CBindingKeysDialog& dialog, int row, const QString& value)
{
    return table(dialog)->model()->setData(table(dialog)->model()->index(row, 0), value, Qt::EditRole);
}
bool show(CBindingKeysDialog& dialog)
{
    dialog.resize(340, 440);
    if (const auto* screen = QGuiApplication::primaryScreen())
        dialog.move(screen->availableGeometry().center() - QPoint(dialog.width() / 2, dialog.height() / 2));
    dialog.show(); dialog.activateWindow();
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    QApplication::setActiveWindow(&dialog);
    QT_WARNING_POP
    return QTest::qWaitForWindowExposed(&dialog);
}
}

class BindingKeysDialogTests : public QObject
{
    Q_OBJECT
private slots:
    void cleanup() { qApp->setStyleSheet({}); }
    void unknownGridCannotCreateARequest()
    {
        CBindingKeysDialog dialog({}, -1);
        CMocParameterStructureBuilder::EditBindingKeysRequest request;
        request.parameterId = QStringLiteral("unchanged");
        QString error;
        QVERIFY(!dialog.BuildRequest(request, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(request.parameterId, QStringLiteral("unchanged"));
    }

    void initialRequestAndNoOp_data()
    {
        QTest::addColumn<QString>("id"); QTest::addColumn<QString>("type"); QTest::addColumn<int>("ownerType");
        QTest::newRow("artmesh") << "EyeL4" << "artmesh" << 0;
        QTest::newRow("warp") << "Warp6" << "warp" << 1;
        QTest::newRow("rotation") << "Rotation8" << "rotation" << 2;
    }
    void initialRequestAndNoOp()
    {
        QFETCH(QString, id); QFETCH(QString, type); QFETCH(int, ownerType);
        const auto snapshot = grid("Mark", id, type);
        CBindingKeysDialog dialog(snapshot, 1); Builder::EditBindingKeysRequest request; QString error;
        QVERIFY2(dialog.BuildRequest(request, &error), qPrintable(error));
        QCOMPARE(request.owner.id, id); QCOMPARE(int(request.owner.type), ownerType);
        QCOMPARE(request.axisIndex, 0); QCOMPARE(request.expectedKeys, snapshot.axes.first().keys);
        for (int index = 0; index < request.keys.size(); ++index) {
            QCOMPARE(request.keys[index].value, snapshot.axes.first().keys[index]);
            QCOMPARE(request.keys[index].oldKeyIndex, index);
        }
        QVERIFY(!apply(dialog)->isEnabled());
        QVERIFY(control<QLabel>(dialog, "bindingKeysSummary")->text().contains(QStringLiteral("未改变")));
        QVERIFY(edit(dialog, 1, QString::number(snapshot.axes.first().keys[1], 'f', 8)));
        QVERIFY(dialog.BuildRequest(request)); QVERIFY(!apply(dialog)->isEnabled());
    }

    void movingReordersValuesAndRetainsOldIdentity()
    {
        const auto snapshot = grid(); CBindingKeysDialog dialog(snapshot, 4);
        QVERIFY(edit(dialog, 0, "0.5"));
        Builder::EditBindingKeysRequest request; QVERIFY(dialog.BuildRequest(request));
        QCOMPARE(request.keys.size(), 3); QCOMPARE(request.keys[0].value, 0.0f); QCOMPARE(request.keys[0].oldKeyIndex, 1);
        QCOMPARE(request.keys[1].value, .5f); QCOMPARE(request.keys[1].oldKeyIndex, 0);
        QCOMPARE(request.keys[2].oldKeyIndex, 2);
        QCOMPARE(request.expectedKeys, QVector<float>({-1, 0, 1})); QVERIFY(apply(dialog)->isEnabled());
        QCOMPARE(table(dialog)->model()->index(0, 0).data().toString(), QStringLiteral("0.5"));
        QCOMPARE(snapshot.axes.first().keys, QVector<float>({-1, 0, 1}));
    }

    void insertAndCopyHaveExplicitSources()
    {
        CBindingKeysDialog dialog(grid(), 4);
        addValue(dialog)->setText("-0.5"); QTest::mouseClick(add(dialog), Qt::LeftButton);
        Builder::EditBindingKeysRequest request; QVERIFY(dialog.BuildRequest(request));
        QCOMPARE(request.keys.size(), 4); QCOMPARE(request.keys[1].value, -.5f);
        QCOMPARE(request.keys[1].oldKeyIndex, -1); QCOMPARE(request.keys[1].source, Builder::BindingKey::InterpolateOldAxis);
        QCOMPARE(request.keys[1].copyOldKeyIndex, -1);
        auto* source = control<QComboBox>(dialog, "bindingKeysSource"); source->setCurrentIndex(1);
        auto* copied = control<QComboBox>(dialog, "bindingKeysCopyFrom"); QCOMPARE(copied->currentIndex(), 1);
        addValue(dialog)->setText("0.5"); QTest::mouseClick(add(dialog), Qt::LeftButton);
        QVERIFY(dialog.BuildRequest(request)); QCOMPARE(request.keys.size(), 5);
        QCOMPARE(request.keys[3].source, Builder::BindingKey::CopyOldSlice); QCOMPARE(request.keys[3].copyOldKeyIndex, 1);
        QCOMPARE(request.keys[3].oldKeyIndex, -1);
        QVERIFY(control<QLabel>(dialog, "bindingKeysSummary")->text().contains(QStringLiteral("9 → 15")));
    }

    void outsideOldIntervalNeedsCopy()
    {
        auto snapshot = grid(); snapshot.axes[0].minimum = -2; snapshot.axes[0].maximum = 2;
        CBindingKeysDialog dialog(snapshot, -1);
        addValue(dialog)->setText("1.5"); QTest::mouseClick(add(dialog), Qt::LeftButton);
        QCOMPARE(table(dialog)->model()->rowCount(), 3);
        QVERIFY(control<QLabel>(dialog, "bindingKeysError")->text().contains(QStringLiteral("复制旧形")));
        QCOMPARE(addValue(dialog)->text(), QStringLiteral("1.5"));
        Builder::EditBindingKeysRequest request; QVERIFY(!dialog.BuildRequest(request));
        control<QComboBox>(dialog, "bindingKeysSource")->setCurrentIndex(1);
        QCOMPARE(control<QComboBox>(dialog, "bindingKeysCopyFrom")->currentIndex(), 0);
        QTest::mouseClick(add(dialog), Qt::LeftButton); QVERIFY(dialog.BuildRequest(request));
        QCOMPARE(request.keys.last().value, 1.5f); QCOMPARE(request.keys.last().copyOldKeyIndex, 0);
    }

    void deleteKeepsAtLeastOneAndKeyboardCanRemove()
    {
        CBindingKeysDialog dialog(grid(), 4); QVERIFY(show(dialog));
        auto* view = table(dialog); view->setCurrentIndex(view->model()->index(1, 0));
        QTest::keyClick(view, Qt::Key_Delete); QCOMPARE(view->model()->rowCount(), 2);
        Builder::EditBindingKeysRequest request; QVERIFY(dialog.BuildRequest(request));
        QCOMPARE(request.keys[0].oldKeyIndex, 0); QCOMPARE(request.keys[1].oldKeyIndex, 2);
        view->setCurrentIndex(view->model()->index(0, 0)); QTest::keyClick(view, Qt::Key_Delete);
        QCOMPARE(view->model()->rowCount(), 1); QVERIFY(dialog.BuildRequest(request)); QCOMPARE(request.keys.first().oldKeyIndex, 2);
        QTest::keyClick(view, Qt::Key_Delete); QCOMPARE(view->model()->rowCount(), 1);
        QVERIFY(control<QLabel>(dialog, "bindingKeysError")->text().contains(QStringLiteral("至少保留")));
    }

    void axesKeepTheirOwnDraftsAndCopyDigits()
    {
        CBindingKeysDialog dialog(grid("Wanko", "D_EFFECT_05"), 6);
        auto* axis = control<QComboBox>(dialog, "bindingKeysAxis"); auto* copied = control<QComboBox>(dialog, "bindingKeysCopyFrom");
        QCOMPARE(copied->currentIndex(), 0); QVERIFY(edit(dialog, 0, "0.25"));
        addValue(dialog)->setText("0.75");
        axis->setCurrentIndex(1); QCOMPARE(copied->currentIndex(), 1); QCOMPARE(addValue(dialog)->text(), QString());
        QVERIFY(edit(dialog, 1, "0.8"));
        axis->setCurrentIndex(2); QCOMPARE(copied->currentIndex(), 1);
        axis->setCurrentIndex(0); QCOMPARE(table(dialog)->model()->index(0, 0).data().toString(), QStringLiteral("0.25"));
        QCOMPARE(addValue(dialog)->text(), QStringLiteral("0.75"));
        QTest::mouseClick(add(dialog), Qt::LeftButton);
        Builder::EditBindingKeysRequest request; QVERIFY(dialog.BuildRequest(request)); QCOMPARE(request.axisIndex, 0);
        QCOMPARE(request.keys.size(), 3);
        axis->setCurrentIndex(1); QVERIFY(dialog.BuildRequest(request)); QCOMPARE(request.axisIndex, 1);
        QCOMPARE(request.keys.size(), 2); QCOMPARE(request.keys.last().value, .8f);
    }

    void batchRequestKeepsEveryAxisDraftAndIdentity()
    {
        const auto snapshot = grid(); QCOMPARE(snapshot.axes.size(), 2);
        CBindingKeysDialog dialog(snapshot, 4);
        auto* axes = control<QComboBox>(dialog, "bindingKeysAxis");
        QVERIFY(edit(dialog, 0, "-0.75"));
        addValue(dialog)->setText("-0.5"); add(dialog)->click();
        axes->setCurrentIndex(1);
        QVERIFY(edit(dialog, 2, "0.75"));
        addValue(dialog)->setText("0.5"); add(dialog)->click();
        for (int current : {0, 1}) {
            axes->setCurrentIndex(current);
            Builder::EditBindingKeysBatchRequest request; QString error;
            QVERIFY2(dialog.BuildBatchRequest(request, &error), qPrintable(error));
            QCOMPARE(request.owner.id, snapshot.objectId); QCOMPARE(request.owner.type, Builder::Owner::ArtMesh);
            QCOMPARE(request.parameterId, snapshot.axes[current].parameterId); QCOMPARE(request.axes.size(), 2);
            for (int index = 0; index < request.axes.size(); ++index) {
                QCOMPARE(request.axes[index].axisIndex, index);
                QCOMPARE(request.axes[index].parameterId, snapshot.axes[index].parameterId);
                QCOMPARE(request.axes[index].expectedKeys, snapshot.axes[index].keys);
                QCOMPARE(request.axes[index].keys.size(), 4);
            }
            QCOMPARE(request.axes[0].keys[0].value, -.75f); QCOMPARE(request.axes[0].keys[0].oldKeyIndex, 0);
            QCOMPARE(request.axes[0].keys[1].value, -.5f); QCOMPARE(request.axes[0].keys[1].oldKeyIndex, -1);
            QCOMPARE(request.axes[0].keys[1].source, Builder::BindingKey::InterpolateOldAxis);
            QCOMPARE(request.axes[1].keys[2].value, .5f); QCOMPARE(request.axes[1].keys[2].oldKeyIndex, -1);
            QCOMPARE(request.axes[1].keys[3].value, .75f); QCOMPARE(request.axes[1].keys[3].oldKeyIndex, 2);
            QVERIFY(apply(dialog)->isEnabled());
            QVERIFY(control<QLabel>(dialog, "bindingKeysSummary")->text().contains(QStringLiteral("9 → 16")));
        }
    }

    void hiddenAxisFailureBlocksBatchAndPreservesOutput_data()
    {
        QTest::addColumn<int>("hiddenAxis"); QTest::addColumn<bool>("pending");
        for (int axis : {0, 1}) {
            QTest::newRow(qPrintable(QStringLiteral("axis-%1-invalid").arg(axis))) << axis << false;
            QTest::newRow(qPrintable(QStringLiteral("axis-%1-pending").arg(axis))) << axis << true;
        }
    }
    void hiddenAxisFailureBlocksBatchAndPreservesOutput()
    {
        QFETCH(int, hiddenAxis); QFETCH(bool, pending);
        CBindingKeysDialog dialog(grid(), 4); auto* axes = control<QComboBox>(dialog, "bindingKeysAxis");
        axes->setCurrentIndex(hiddenAxis);
        if (pending) addValue(dialog)->setText("0.25"); else QVERIFY(edit(dialog, 1, "nan"));
        axes->setCurrentIndex(1 - hiddenAxis); QVERIFY(edit(dialog, 1, "0.25"));
        Builder::EditBindingKeysBatchRequest request;
        request.owner = {Builder::Owner::Rotation, "sentinel-owner"}; request.parameterId = "sentinel-parameter";
        request.axes = {{7, "sentinel-axis", {12}, {{13, -1, Builder::BindingKey::CopyOldSlice, 9}}}};
        QString error;
        QVERIFY(!dialog.BuildBatchRequest(request, &error)); QVERIFY(error.contains(QStringLiteral("轴 %1：").arg(hiddenAxis)));
        QVERIFY(error.contains(pending ? QStringLiteral("未加入") : QStringLiteral("有限数值")));
        QCOMPARE(request.owner.type, Builder::Owner::Rotation); QCOMPARE(request.owner.id, QStringLiteral("sentinel-owner"));
        QCOMPARE(request.parameterId, QStringLiteral("sentinel-parameter")); QCOMPARE(request.axes.size(), 1);
        QCOMPARE(request.axes[0].axisIndex, 7); QCOMPARE(request.axes[0].parameterId, QStringLiteral("sentinel-axis"));
        QCOMPARE(request.axes[0].expectedKeys, QVector<float>{12}); QCOMPARE(request.axes[0].keys.size(), 1);
        QCOMPARE(request.axes[0].keys[0].value, 13.f); QCOMPARE(request.axes[0].keys[0].oldKeyIndex, -1);
        QCOMPARE(request.axes[0].keys[0].source, Builder::BindingKey::CopyOldSlice);
        QCOMPARE(request.axes[0].keys[0].copyOldKeyIndex, 9);
        QVERIFY(!apply(dialog)->isEnabled()); QSignalSpy submitted(&dialog, &CBindingKeysDialog::ApplyRequested);
        apply(dialog)->click(); QTest::keyClick(table(dialog), Qt::Key_Return); QCOMPARE(submitted.count(), 0);
        axes->setCurrentIndex(hiddenAxis);
        if (pending) addValue(dialog)->clear(); else QVERIFY(edit(dialog, 1, "0"));
        QVERIFY2(dialog.BuildBatchRequest(request, &error), qPrintable(error)); QVERIFY(apply(dialog)->isEnabled());
    }

    void batchSummaryIncludesUneditedBlendShapeAxes()
    {
        auto snapshot = grid(); auto blend = snapshot.axes.first();
        blend.parameterId = QStringLiteral("blend"); blend.parameterType = 1;
        snapshot.axes.append(blend); snapshot.keyformCount = snapshot.expectedKeyformCount = 27;
        CBindingKeysDialog dialog(snapshot, 4); auto* axes = control<QComboBox>(dialog, "bindingKeysAxis");
        addValue(dialog)->setText("-.5"); add(dialog)->click();
        axes->setCurrentIndex(1); addValue(dialog)->setText(".5"); add(dialog)->click();
        Builder::EditBindingKeysBatchRequest request; QString error;
        QVERIFY2(dialog.BuildBatchRequest(request, &error), qPrintable(error)); QCOMPARE(request.axes.size(), 2);
        QVERIFY(control<QLabel>(dialog, "bindingKeysSummary")->text().contains(QStringLiteral("27 → 48")));
        axes->setCurrentIndex(0);
        QVERIFY(control<QLabel>(dialog, "bindingKeysSummary")->text().contains(QStringLiteral("27 → 48")));
        axes->setCurrentIndex(2); QVERIFY(!apply(dialog)->isEnabled());
        QVERIFY(!dialog.BuildBatchRequest(request, &error)); QVERIFY(error.contains(QStringLiteral("普通参数")));
    }

    void batchProductLimitIncludesUneditedBlendShapeAxes()
    {
        auto snapshot = grid(); auto prototype = snapshot.axes.first(); snapshot.axes.clear();
        prototype.keys = {-1, 1};
        for (int index = 0; index < 30; ++index) {
            auto axis = prototype; axis.parameterId = QStringLiteral("axis-%1").arg(index);
            axis.parameterType = index < 2 ? 0 : 1; snapshot.axes.append(axis);
        }
        snapshot.keyformCount = snapshot.expectedKeyformCount = 1 << 30;
        CBindingKeysDialog dialog(snapshot, -1); auto* axes = control<QComboBox>(dialog, "bindingKeysAxis");
        addValue(dialog)->setText("0"); add(dialog)->click();
        Builder::EditBindingKeysBatchRequest request; QString error;
        QVERIFY2(dialog.BuildBatchRequest(request, &error), qPrintable(error));
        axes->setCurrentIndex(1); addValue(dialog)->setText("0"); add(dialog)->click();
        QVERIFY(!dialog.BuildBatchRequest(request, &error)); QVERIFY(error.contains(QStringLiteral("关键形总数")));
        QVERIFY(!apply(dialog)->isEnabled());
        QVERIFY(control<QLabel>(dialog, "bindingKeysSummary")->text().contains(QStringLiteral("超出支持范围")));
    }

    void invalidTextIsKeptAndOutputIsAtomic_data()
    {
        QTest::addColumn<QString>("value");
        for (const auto& value : QStringList{"", "nan", "inf", "1e500", "1e-500", "1,2", "3.5e38", "-1", "4"})
            QTest::newRow(qPrintable(value.isEmpty() ? QStringLiteral("empty") : value)) << value;
    }
    void invalidTextIsKeptAndOutputIsAtomic()
    {
        QFETCH(QString, value); CBindingKeysDialog dialog(grid(), 4); QVERIFY(edit(dialog, 1, value));
        Builder::EditBindingKeysRequest request; request.parameterId = "untouched"; QString error;
        QVERIFY(!dialog.BuildRequest(request, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(request.parameterId, QStringLiteral("untouched")); QVERIFY(!apply(dialog)->isEnabled());
        QCOMPARE(table(dialog)->model()->index(1, 0).data().toString(), value);
    }

    void smallAndExtremeFloatsAreNotRoundedByTheEditor_data()
    {
        QTest::addColumn<float>("first"); QTest::addColumn<float>("middle"); QTest::addColumn<float>("last");
        QTest::addColumn<QString>("input"); QTest::addColumn<float>("expected");
        QTest::newRow("tiny") << 1e-30f << 2e-30f << 3e-30f << "2.125e-30" << 2.125e-30f;
        const float tiny = std::numeric_limits<float>::denorm_min();
        QTest::newRow("subnormal") << 0.0f << 2 * tiny << 4 * tiny << "4.20389539e-45" << 3 * tiny;
        const float maximum = std::numeric_limits<float>::max();
        QTest::newRow("maximum") << 0.0f << maximum / 2 << maximum << "3.40282347e38" << maximum;
    }
    void smallAndExtremeFloatsAreNotRoundedByTheEditor()
    {
        QFETCH(float, first); QFETCH(float, middle); QFETCH(float, last); QFETCH(QString, input); QFETCH(float, expected);
        auto snapshot = grid("Mark", "BrowR"); snapshot.axes[0].keys = {first, middle, last};
        snapshot.axes[0].minimum = first; snapshot.axes[0].maximum = last;
        CBindingKeysDialog dialog(snapshot, 1); const int row = expected == last ? 2 : 1;
        QVERIFY(edit(dialog, row, input)); Builder::EditBindingKeysRequest request; QString error;
        QVERIFY2(dialog.BuildRequest(request, &error), qPrintable(error)); QCOMPARE(request.keys[row].value, expected);
        QCOMPARE(table(dialog)->model()->index(row, 0).data().toString(), input);
    }

    void retainedOutOfRangeKeysRemainValid()
    {
        auto snapshot = grid(); snapshot.axes[0].minimum = -.5f; snapshot.axes[0].maximum = .5f;
        CBindingKeysDialog dialog(snapshot, 4); Builder::EditBindingKeysRequest request;
        QVERIFY(dialog.BuildRequest(request)); QVERIFY(!apply(dialog)->isEnabled());
        QVERIFY(edit(dialog, 0, "-0.75")); QVERIFY(!dialog.BuildRequest(request));
        QVERIFY(edit(dialog, 0, "-1.0000")); QVERIFY(dialog.BuildRequest(request));
        QVERIFY(!apply(dialog)->isEnabled());
    }

    void singletonDefaultsToExplicitCopy()
    {
        auto snapshot = grid("Mark", "BrowR"); snapshot.axes[0].keys = {0};
        snapshot.keyformCount = snapshot.expectedKeyformCount = 1;
        CBindingKeysDialog dialog(snapshot, -1);
        QCOMPARE(control<QComboBox>(dialog, "bindingKeysSource")->currentIndex(), 1);
        QVERIFY(!control<QPushButton>(dialog, "bindingKeysDelete")->isEnabled());
        addValue(dialog)->setText("0.5"); QTest::mouseClick(add(dialog), Qt::LeftButton);
        Builder::EditBindingKeysRequest request; QVERIFY(dialog.BuildRequest(request));
        QCOMPARE(request.keys.size(), 2); QCOMPARE(request.keys.last().copyOldKeyIndex, 0);
        control<QComboBox>(dialog, "bindingKeysSource")->setCurrentIndex(0);
        addValue(dialog)->setText("0.75"); QTest::mouseClick(add(dialog), Qt::LeftButton);
        QCOMPARE(table(dialog)->model()->rowCount(), 2);
        QVERIFY(control<QLabel>(dialog, "bindingKeysError")->text().contains(QStringLiteral("复制旧形")));
    }

    void equivalentZeroTextKeepsOriginalBits()
    {
        auto snapshot = grid("Mark", "BrowR"); snapshot.axes[0].keys[1] = -0.0f;
        CBindingKeysDialog dialog(snapshot, 1); QVERIFY(edit(dialog, 1, "0.000e5"));
        Builder::EditBindingKeysRequest request; QVERIFY(dialog.BuildRequest(request));
        QVERIFY(std::signbit(request.keys[1].value)); QVERIFY(!apply(dialog)->isEnabled());
        QCOMPARE(table(dialog)->model()->index(1, 0).data().toString(), QStringLiteral("0.000e5"));
    }

    void unsupportedAxisAndBrokenGridExplainTheReason()
    {
        auto snapshot = grid(); snapshot.axes[0].parameterType = 1;
        CBindingKeysDialog dialog(snapshot, 4); auto* axes = control<QComboBox>(dialog, "bindingKeysAxis");
        axes->setCurrentIndex(0); Builder::EditBindingKeysRequest request; QString error;
        QVERIFY(!dialog.BuildRequest(request, &error)); QVERIFY(error.contains(QStringLiteral("普通参数")));
        QVERIFY(!add(dialog)->isEnabled()); QVERIFY(!edit(dialog, 0, ".25"));
        axes->setCurrentIndex(1); QVERIFY(dialog.BuildRequest(request));
        snapshot = grid(); snapshot.diagnostic = QStringLiteral("对象包含尚不支持姿态反解的 BlendShape");
        CBindingKeysDialog ordinary(snapshot, 4); QVERIFY(ordinary.BuildRequest(request));
        snapshot.keyformCount = 8;
        CBindingKeysDialog broken(snapshot, 4); QVERIFY(!broken.BuildRequest(request, &error));
        QVERIFY(error.contains(QStringLiteral("不完整"))); QVERIFY(!apply(broken)->isEnabled());
    }

    void applyFailureKeepsInputAndRetryCanBeAccepted()
    {
        CBindingKeysDialog dialog(grid(), 4); QVERIFY(show(dialog)); QVERIFY(edit(dialog, 1, "0.25"));
        int calls = 0;
        connect(&dialog, &CBindingKeysDialog::ApplyRequested, &dialog, [&] {
            if (++calls == 1) dialog.SetError(QStringLiteral("候选被拒绝 <b> 保留原文")); else dialog.accept();
        });
        QTest::mouseClick(apply(dialog), Qt::LeftButton); QCOMPARE(calls, 1); QVERIFY(dialog.isVisible());
        QCOMPARE(table(dialog)->model()->index(1, 0).data().toString(), QStringLiteral("0.25"));
        auto* error = control<QLabel>(dialog, "bindingKeysError"); QCOMPARE(error->textFormat(), Qt::PlainText);
        QCOMPARE(error->text(), QStringLiteral("候选被拒绝 <b> 保留原文"));
        QVERIFY(edit(dialog, 1, "0.25")); QCOMPARE(error->text(), QStringLiteral("候选被拒绝 <b> 保留原文"));
        QTest::mouseClick(apply(dialog), Qt::LeftButton); QCOMPARE(calls, 2); QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }

    void pendingEditorAndAxisSwitchKeepText()
    {
        CBindingKeysDialog dialog(grid(), 4); QVERIFY(show(dialog));
        auto* view = table(dialog); view->setCurrentIndex(view->model()->index(1, 0)); view->setFocus();
        QTest::keyClick(view, Qt::Key_F2);
        auto* editor = control<QLineEdit>(dialog, "bindingKeysValue"); QVERIFY(editor);
        editor->selectAll(); QTest::keyClicks(editor, "2.5e-1"); editor->setCursorPosition(2);
        Builder::EditBindingKeysRequest request; QVERIFY(dialog.BuildRequest(request)); QCOMPARE(request.keys[1].value, .25f);
        QCOMPARE(editor->text(), QStringLiteral("2.5e-1")); QCOMPARE(editor->cursorPosition(), 2);
        auto* axes = control<QComboBox>(dialog, "bindingKeysAxis"); axes->setCurrentIndex(1); axes->setCurrentIndex(0);
        QCOMPARE(view->model()->index(1, 0).data().toString(), QStringLiteral("2.5e-1"));
        QVERIFY(dialog.BuildRequest(request)); QCOMPARE(request.keys[1].value, .25f);
    }

    void enterInAddFieldDoesNotAlsoApply()
    {
        CBindingKeysDialog dialog(grid(), 4); QVERIFY(show(dialog)); QSignalSpy requested(&dialog, &CBindingKeysDialog::ApplyRequested);
        addValue(dialog)->setFocus(); QTest::keyClicks(addValue(dialog), "0.5"); QTest::keyClick(addValue(dialog), Qt::Key_Return);
        QCOMPARE(table(dialog)->model()->rowCount(), 4); QCOMPARE(requested.count(), 0); QVERIFY(dialog.isVisible());
        QTest::keyClick(table(dialog), Qt::Key_Return); QCOMPARE(requested.count(), 1); QVERIFY(dialog.isVisible());
    }

    void editorReturnCannotEraseACommitFailure()
    {
        CBindingKeysDialog dialog(grid(), 4); QVERIFY(show(dialog));
        auto* view = table(dialog); view->setCurrentIndex(view->model()->index(1, 0)); QTest::keyClick(view, Qt::Key_F2);
        auto* editor = control<QLineEdit>(dialog, "bindingKeysValue"); QVERIFY(editor);
        editor->selectAll(); QTest::keyClicks(editor, ".25");
        int calls = 0;
        connect(&dialog, &CBindingKeysDialog::ApplyRequested, &dialog, [&] { ++calls; dialog.SetError(QStringLiteral("外层提交被拒绝")); });
        QTest::keyClick(editor, Qt::Key_Return);
        QTRY_COMPARE(calls, 1); QVERIFY(dialog.isVisible());
        QCoreApplication::processEvents();
        QCOMPARE(control<QLabel>(dialog, "bindingKeysError")->text(), QStringLiteral("外层提交被拒绝"));
        QCOMPARE(view->model()->index(1, 0).data().toString(), QStringLiteral(".25"));
    }

    void cancelAndOpenEditorDestructionDoNotPublish()
    {
        CModelDocument document; QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const auto revision = document.Revision(); CKeyformGridSnapshot snapshot; QVERIFY(document.DescribeKeyformGrid("EyeL4", "artmesh", snapshot));
        auto* dialog = new CBindingKeysDialog(snapshot, 4); QVERIFY(show(*dialog));
        QSignalSpy requested(dialog, &CBindingKeysDialog::ApplyRequested);
        QVERIFY(edit(*dialog, 1, ".25"));
        control<QDialogButtonBox>(*dialog, "bindingKeysButtons")->button(QDialogButtonBox::Cancel)->click();
        QCOMPARE(dialog->result(), int(QDialog::Rejected)); QCOMPARE(requested.count(), 0); QCOMPARE(document.Revision(), revision); QVERIFY(!document.CanUndoEdit());
        QVERIFY(show(*dialog)); table(*dialog)->setCurrentIndex(table(*dialog)->model()->index(1, 0));
        QTest::keyClick(table(*dialog), Qt::Key_F2); QVERIFY(control<QLineEdit>(*dialog, "bindingKeysValue"));
        delete dialog; QCOMPARE(document.Revision(), revision);
    }

    void largeListUsesOneLazyTable()
    {
        auto snapshot = grid("Mark", "BrowR"); snapshot.axes[0].keys.clear();
        for (int value = 0; value < 10000; ++value) snapshot.axes[0].keys.append(float(value));
        snapshot.axes[0].minimum = 0; snapshot.axes[0].maximum = 10000;
        snapshot.keyformCount = snapshot.expectedKeyformCount = 10000;
        CBindingKeysDialog dialog(snapshot, 5000);
        QCOMPARE(table(dialog)->model()->rowCount(), 10000); QVERIFY(table(dialog)->findChildren<QWidget*>().size() < 30);
        QCOMPARE(control<QComboBox>(dialog, "bindingKeysCopyFrom")->currentIndex(), 5000);
        QVERIFY(!table(dialog)->indexWidget(table(dialog)->model()->index(9999, 0)));
    }

    void narrowThemesAndLiteralNames_data()
    {
        QTest::addColumn<QString>("styleName"); QTest::addColumn<bool>("dark");
        for (const auto& style : QStyleFactory::keys()) if (style == "Fusion" || (QGuiApplication::platformName() == "cocoa" && style == "macOS"))
            for (bool dark : {false, true}) QTest::newRow(qPrintable(style + (dark ? "-dark" : "-light"))) << style << dark;
    }
    void narrowThemesAndLiteralNames()
    {
        QFETCH(QString, styleName); QFETCH(bool, dark);
        auto* style = QStyleFactory::create(styleName); QVERIFY(style); qApp->setStyle(style); qApp->setPalette(style->standardPalette());
        qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        auto snapshot = grid(); snapshot.axes[0].displayName = QStringLiteral("<b>很长的中文参数名称</b> & 关键值编辑名称保留");
        CBindingKeysDialog dialog(snapshot, 4); QVERIFY(show(dialog)); dialog.resize(320, 420);
        QTRY_COMPARE(dialog.width(), 320); QVERIFY(dialog.minimumSizeHint().width() <= 320);
        const auto* axis = control<QComboBox>(dialog, "bindingKeysAxis");
        QVERIFY(axis->currentText().contains(snapshot.axes[0].displayName)); QVERIFY(axis->toolTip().contains(snapshot.axes[0].displayName.toHtmlEscaped()));
        auto* scroll = control<QScrollArea>(dialog, "bindingKeysScroll"); QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
        QTRY_VERIFY2(scroll->widget()->width() <= scroll->viewport()->width(), qPrintable(QStringLiteral("content %1, viewport %2, content minimum %3")
            .arg(scroll->widget()->width()).arg(scroll->viewport()->width()).arg(scroll->widget()->minimumSizeHint().width())));
        control<QComboBox>(dialog, "bindingKeysSource")->setCurrentIndex(1);
        for (const auto* name : {"bindingKeysAxis", "bindingKeysTable", "bindingKeysDelete", "bindingKeysAddValue", "bindingKeysSource", "bindingKeysCopyFrom", "bindingKeysAdd"}) {
            auto* widget = control<QWidget>(dialog, name); QVERIFY(widget->isVisible());
            const QPoint center = widget->mapTo(scroll->widget(), widget->rect().center());
            scroll->ensureVisible(center.x(), center.y(), dialog.fontMetrics().height(), dialog.fontMetrics().height());
            QTRY_VERIFY2(scroll->viewport()->rect().contains(widget->mapTo(scroll->viewport(), widget->rect().center())), name);
            QVERIFY(widget->mapTo(scroll->viewport(), QPoint()).x() >= 0);
            QVERIFY(widget->mapTo(scroll->viewport(), QPoint(widget->width(), 0)).x() <= scroll->viewport()->width());
        }
        auto* remove = control<QPushButton>(dialog, "bindingKeysDelete"); remove->setFocus(Qt::TabFocusReason);
        QTest::keyClick(remove, Qt::Key_Tab); QTRY_COMPARE(qApp->focusWidget(), addValue(dialog));
        QTRY_VERIFY(scroll->viewport()->rect().contains(addValue(dialog)->mapTo(scroll->viewport(), addValue(dialog)->rect().center())));
        QVERIFY(apply(dialog)->geometry().isValid());
        const auto position = apply(dialog)->mapTo(&dialog, QPoint()); QVERIFY(dialog.rect().contains(QRect(position, apply(dialog)->size())));
        QVERIFY(edit(dialog, 1, ".25")); QVERIFY(apply(dialog)->isEnabled());
        const auto directory = qEnvironmentVariable("BINDING_KEYS_DIALOG_SCREENSHOTS");
        if (!directory.isEmpty()) {
            QVERIFY(QDir().mkpath(directory));
            QVERIFY(dialog.grab().save(directory + '/' + styleName + (dark ? "-dark-bottom.png" : "-light-bottom.png")));
            scroll->verticalScrollBar()->setValue(0);
            QVERIFY(dialog.grab().save(directory + '/' + styleName + (dark ? "-dark.png" : "-light.png")));
        }
    }
};

QTEST_MAIN(BindingKeysDialogTests)
#include "binding_keys_dialog_tests.moc"
