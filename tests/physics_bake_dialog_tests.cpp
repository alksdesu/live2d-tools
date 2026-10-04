#include "ui/cphysicsbakedialog.h"
#include "ui/theme.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStyleFactory>
#include <QtTest>

namespace {
template<class T> T* widget(CPhysicsBakeDialog& dialog, const char* name)
{
    return dialog.findChild<T*>(QLatin1String(name));
}
QPushButton* button(CPhysicsBakeDialog& dialog, QDialogButtonBox::StandardButton kind)
{
    return widget<QDialogButtonBox>(dialog, "physicsBakeButtons")->button(kind);
}
bool show(CPhysicsBakeDialog& dialog)
{
    dialog.show();
    dialog.activateWindow();
    return QTest::qWaitForWindowExposed(&dialog);
}
CMotionTimeline timeline()
{
    CMotionTimeline result;
    result.AddPoint("Hair", 0, 0);
    result.AddPoint("Hair", 1, 1);
    result.AddPoint("Other", 0, 0);
    result.AddPoint("Other", 1, .5f);
    result.AddPoint({CMotionTarget::PartOpacity, "Hair"}, 0, 1);
    result.SetTiming(29.97, 1);
    result.SetEvents(QJsonArray{QJsonObject{{"Time", .5}, {"Value", "event"}}});
    return result;
}
}

class PhysicsBakeDialogTests : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { qApp->setStyle(QStyleFactory::create(QStringLiteral("Fusion"))); }
    void cleanup() { qApp->setStyleSheet({}); }

    void defaultsKeepSceneAndPrecisionUnchanged()
    {
        auto source = timeline();
        QVERIFY(source.SetTiming(29.97002997003, 1.234567890123));
        const auto before = source;
        CPhysicsBakeDialog dialog({{"Hair", "头发摆动"}, {"Other", "Other"}}, source);
        const auto options = dialog.Selection();
        QCOMPARE(options.outputIds, QSet<QString>({"Hair", "Other"}));
        QCOMPARE(options.fps, source.Timing().fps);
        QCOMPARE(options.duration, source.Timing().duration);
        QCOMPARE(options.warmupCycles, 1);
        QVERIFY(!options.newAnimation);
        auto* list = widget<QListWidget>(dialog, "physicsBakeOutputs");
        QCOMPARE(list->count(), 2);
        QCOMPARE(list->item(0)->text(), QStringLiteral("头发摆动（Hair）"));
        QCOMPARE(list->item(1)->text(), QStringLiteral("Other"));
        QVERIFY(source.HasSameContent(before));
        QVERIFY(button(dialog, QDialogButtonBox::Ok)->isEnabled());
    }

    void emptyTimelineAndLowFpsUseUsableDefaults()
    {
        CMotionTimeline source;
        QVERIFY(source.SetTiming(.01, 0));
        CPhysicsBakeDialog dialog({{"Output", {}}}, source);
        QCOMPARE(dialog.Selection().fps, 30.0);
        QCOMPARE(dialog.Selection().duration, 1.0);
        QVERIFY(button(dialog, QDialogButtonBox::Ok)->isEnabled());
        widget<QDoubleSpinBox>(dialog, "physicsBakeDuration")->setValue(0);
        QCOMPARE(dialog.Selection().duration, 0.0);
        QVERIFY(widget<QLabel>(dialog, "physicsBakeSummary")->text().contains(QStringLiteral("每条 1 点")));
        QVERIFY(button(dialog, QDialogButtonBox::Ok)->isEnabled());
    }

    void fpsSourceSelectsScenePhysicsAndCustomValues()
    {
        auto source = timeline();
        CPhysicsBakeDialog dialog({{"Hair", "Hair"}}, source, nullptr, 60.0);
        auto* selector = widget<QComboBox>(dialog, "physicsBakeFpsSource");
        QVERIFY(selector);
        QCOMPARE(dialog.Selection().fps, source.Timing().fps);
        QCOMPARE(dialog.Selection().fpsSource, CPhysicsBakeDialog::Options::FpsSource::Scene);
        selector->setCurrentIndex(selector->findData(int(CPhysicsBakeDialog::Options::FpsSource::Physics)));
        QCOMPARE(dialog.Selection().fps, 60.0);
        QCOMPARE(dialog.Selection().fpsSource, CPhysicsBakeDialog::Options::FpsSource::Physics);
        selector->setCurrentIndex(selector->findData(int(CPhysicsBakeDialog::Options::FpsSource::Custom)));
        widget<QDoubleSpinBox>(dialog, "physicsBakeFps")->setValue(24.0);
        QCOMPARE(dialog.Selection().fps, 24.0);
        QCOMPARE(dialog.Selection().fpsSource, CPhysicsBakeDialog::Options::FpsSource::Custom);
    }

    void selectedOutputsDetermineRetainedBudget()
    {
        const auto source = timeline();
        CPhysicsBakeDialog dialog({{"Hair", "头发"}, {"Other", "其它"}}, source);
        auto* fps = widget<QDoubleSpinBox>(dialog, "physicsBakeFps");
        auto* duration = widget<QDoubleSpinBox>(dialog, "physicsBakeDuration");
        auto* summary = widget<QLabel>(dialog, "physicsBakeSummary");
        auto* list = widget<QListWidget>(dialog, "physicsBakeOutputs");
        fps->setValue(1);
        duration->setValue(49999);
        QVERIFY(summary->text().contains(QStringLiteral("新增 100000 + 保留 1 = 100001")));
        QVERIFY(!button(dialog, QDialogButtonBox::Ok)->isEnabled());
        list->item(1)->setCheckState(Qt::Unchecked);
        QCOMPARE(dialog.Selection().outputIds, QSet<QString>{"Hair"});
        QVERIFY(summary->text().contains(QStringLiteral("新增 50000 + 保留 3 = 50003")));
        QVERIFY(button(dialog, QDialogButtonBox::Ok)->isEnabled());
        list->item(0)->setCheckState(Qt::Unchecked);
        QVERIFY(!button(dialog, QDialogButtonBox::Ok)->isEnabled());
        QVERIFY(widget<QLabel>(dialog, "physicsBakeError")->text().contains(QStringLiteral("至少选择")));
    }

    void newAnimationExcludesRetainedTracksFromBudget()
    {
        const auto source = timeline();
        const auto before = source;
        CPhysicsBakeDialog dialog({{"Hair", "头发"}, {"Other", "其它"}}, source);
        widget<QDoubleSpinBox>(dialog, "physicsBakeFps")->setValue(1);
        widget<QDoubleSpinBox>(dialog, "physicsBakeDuration")->setValue(49999);
        auto* mode = widget<QCheckBox>(dialog, "physicsBakeNewAnimation");
        auto* summary = widget<QLabel>(dialog, "physicsBakeSummary");
        QVERIFY(!button(dialog, QDialogButtonBox::Ok)->isEnabled());
        mode->setChecked(true);
        QVERIFY(dialog.Selection().newAnimation);
        QVERIFY(summary->text().contains(QStringLiteral("新增 100000 + 保留 0 = 100000")));
        QVERIFY(button(dialog, QDialogButtonBox::Ok)->isEnabled());
        QVERIFY(widget<QLabel>(dialog, "physicsBakeDestinationHint")->text().contains(QStringLiteral("不复制")));
        mode->setChecked(false);
        QVERIFY(!button(dialog, QDialogButtonBox::Ok)->isEnabled());
        QVERIFY(source.HasSameContent(before));
    }

    void copySceneModeIsDistinctFromIndependentAnimation()
    {
        CPhysicsBakeDialog dialog({{"Hair", "头发"}}, timeline());
        auto* independent = widget<QCheckBox>(dialog, "physicsBakeNewAnimation");
        auto* copy = widget<QCheckBox>(dialog, "physicsBakeCopyScene");
        auto* hint = widget<QLabel>(dialog, "physicsBakeDestinationHint");
        copy->setChecked(true);
        QVERIFY(dialog.Selection().copyScene);
        QVERIFY(!dialog.Selection().newAnimation);
        QVERIFY(hint->text().contains(QStringLiteral("复制当前动画")));
        independent->setChecked(true);
        QVERIFY(dialog.Selection().newAnimation);
        QVERIFY(!dialog.Selection().copyScene);
        QVERIFY(hint->text().contains(QStringLiteral("新动画")));
    }

    void globalEffectConflictsRequireSeparateAnimationOrDeselection()
    {
        const auto source = timeline();
        CPhysicsBakeDialog dialog({{"Hair", "头发"}, {"Other", "其它"}}, source);
        dialog.SetConflictingOutputs({"Hair", "NotAnOutput"});
        QVERIFY(show(dialog));
        QVERIFY(!button(dialog, QDialogButtonBox::Ok)->isEnabled());
        auto* error = widget<QLabel>(dialog, "physicsBakeError");
        QVERIFY(error->text().contains("Hair"));
        QVERIFY(error->text().contains(QStringLiteral("重复应用")));
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        dialog.accept();
        QCOMPARE(accepted.size(), 0);
        auto* list = widget<QListWidget>(dialog, "physicsBakeOutputs");
        list->item(0)->setCheckState(Qt::Unchecked);
        QVERIFY(button(dialog, QDialogButtonBox::Ok)->isEnabled());
        list->item(0)->setCheckState(Qt::Checked);
        QVERIFY(!button(dialog, QDialogButtonBox::Ok)->isEnabled());
        auto* mode = widget<QCheckBox>(dialog, "physicsBakeNewAnimation");
        mode->setChecked(true);
        QVERIFY(button(dialog, QDialogButtonBox::Ok)->isEnabled());
        mode->setChecked(false);
        QVERIFY(!button(dialog, QDialogButtonBox::Ok)->isEnabled());
        dialog.SetConflictingOutputs({});
        QVERIFY(button(dialog, QDialogButtonBox::Ok)->isEnabled());
        dialog.SetConflictingOutputs({"Hair"});
        mode->setChecked(true);
        dialog.accept();
        QCOMPARE(accepted.size(), 1);
        QCOMPARE(dialog.Selection().outputIds, QSet<QString>({"Hair", "Other"}));
    }

    void actualEndUsesCeilingAndFractionalFps()
    {
        CPhysicsBakeDialog dialog({{"Hair", "Hair"}}, timeline());
        widget<QDoubleSpinBox>(dialog, "physicsBakeDuration")->setValue(.11);
        QCOMPARE(dialog.Selection().fps, 29.97);
        const auto text = widget<QLabel>(dialog, "physicsBakeSummary")->text();
        QVERIFY(text.contains(QString::number(4.0 / 29.97, 'g', 12)));
        QVERIFY(text.contains(QStringLiteral("每条 5 点")));
        QVERIFY(button(dialog, QDialogButtonBox::Ok)->isEnabled());
    }

    void warmupUsesWholeTimelineAndCanBeDisabled()
    {
        auto source = timeline();
        QVERIFY(source.SetTiming(30, 10000));
        CPhysicsBakeDialog dialog({{"Hair", "Hair"}}, source);
        widget<QDoubleSpinBox>(dialog, "physicsBakeDuration")->setValue(1);
        QVERIFY(!button(dialog, QDialogButtonBox::Ok)->isEnabled());
        QVERIFY(widget<QLabel>(dialog, "physicsBakeError")->text().contains(QStringLiteral("预热")));
        widget<QSpinBox>(dialog, "physicsBakeWarmup")->setValue(0);
        QVERIFY(button(dialog, QDialogButtonBox::Ok)->isEnabled());
        QCOMPARE(dialog.Selection().warmupCycles, 0);
    }

    void invalidSelectionCannotAccept_data()
    {
        QTest::addColumn<int>("reason");
        for (int reason = 0; reason < 5; ++reason) QTest::newRow(qPrintable(QString::number(reason))) << reason;
    }
    void invalidSelectionCannotAccept()
    {
        QFETCH(int, reason);
        auto source = timeline();
        if (reason == 2) source.StartRecording();
        QMap<QString, QString> outputs{{"Hair", "Hair"}};
        if (reason == 0) outputs.clear();
        if (reason == 1) outputs.insert(QString(), "bad");
        CPhysicsBakeDialog dialog(outputs, source);
        if (reason == 3) widget<QDoubleSpinBox>(dialog, "physicsBakeDuration")->setValue(100000);
        if (reason == 4) {
            auto* list = widget<QListWidget>(dialog, "physicsBakeOutputs");
            list->item(0)->setCheckState(Qt::Unchecked);
        }
        QVERIFY(show(dialog));
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        dialog.accept();
        dialog.done(QDialog::Accepted);
        QCOMPARE(accepted.size(), 0);
        QVERIFY(dialog.isVisible());
        QVERIFY(!button(dialog, QDialogButtonBox::Ok)->isEnabled());
        QVERIFY(!widget<QLabel>(dialog, "physicsBakeError")->text().isEmpty());
    }

    void incompleteInputIsPreservedAndBlocksAcceptance()
    {
        CPhysicsBakeDialog dialog({{"Hair", "Hair"}}, timeline());
        QVERIFY(show(dialog));
        auto* fps = widget<QDoubleSpinBox>(dialog, "physicsBakeFps");
        fps->setFocus();
        auto* line = fps->findChild<QLineEdit*>();
        line->selectAll();
        QTest::keyClick(line, Qt::Key_Backspace);
        QVERIFY(!fps->hasAcceptableInput());
        const auto entered = line->text();
        QTest::keyClick(line, Qt::Key_Return);
        QCOMPARE(line->text(), entered);
        QVERIFY(dialog.isVisible());
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QCOMPARE(line->text(), entered);
        QVERIFY(dialog.isVisible());
        QVERIFY(!button(dialog, QDialogButtonBox::Ok)->isEnabled());
    }

    void checkboxesAndBulkButtonsWorkWithKeyboard()
    {
        CPhysicsBakeDialog dialog({{"Hair", "Hair"}, {"Other", "Other"}}, timeline());
        QVERIFY(show(dialog));
        auto* list = widget<QListWidget>(dialog, "physicsBakeOutputs");
        list->setFocus();
        QTRY_VERIFY(list->hasFocus());
        list->setCurrentRow(0);
        QTest::keyClick(list, Qt::Key_Space);
        QCOMPARE(list->item(0)->checkState(), Qt::Unchecked);
        QCOMPARE(dialog.Selection().outputIds, QSet<QString>{"Other"});
        QTest::mouseClick(widget<QPushButton>(dialog, "physicsBakeClear"), Qt::LeftButton);
        QVERIFY(dialog.Selection().outputIds.isEmpty());
        QTest::mouseClick(widget<QPushButton>(dialog, "physicsBakeSelectAll"), Qt::LeftButton);
        QCOMPARE(dialog.Selection().outputIds.size(), 2);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        button(dialog, QDialogButtonBox::Ok)->setFocus();
        QTest::keyClick(button(dialog, QDialogButtonBox::Ok), Qt::Key_Return);
        QCOMPARE(accepted.size(), 1);
    }

    void cancelAndEscapeNeverAccept_data()
    {
        QTest::addColumn<bool>("escape");
        QTest::newRow("button") << false;
        QTest::newRow("escape") << true;
    }
    void cancelAndEscapeNeverAccept()
    {
        QFETCH(bool, escape);
        const auto source = timeline();
        const auto before = source;
        CPhysicsBakeDialog dialog({{"Hair", "Hair"}}, source);
        QVERIFY(show(dialog));
        QSignalSpy accepted(&dialog, &QDialog::accepted), rejected(&dialog, &QDialog::rejected);
        if (escape) QTest::keyClick(&dialog, Qt::Key_Escape);
        else QTest::mouseClick(button(dialog, QDialogButtonBox::Cancel), Qt::LeftButton);
        QCOMPARE(accepted.size(), 0);
        QCOMPARE(rejected.size(), 1);
        QVERIFY(source.HasSameContent(before));
    }

    void shortDialogScrollsAndKeepsButtonsVisible_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void shortDialogScrollsAndKeepsButtonsVisible()
    {
        QFETCH(bool, dark);
        qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        QMap<QString, QString> names;
        for (int index = 0; index < 24; ++index)
            names.insert(QString("ParamPhysicsOutput%1").arg(index, 2, 10, QLatin1Char('0')),
                         QStringLiteral("很长的中文输出名称，说明关联头发与服装摆动 %1").arg(index));
        CPhysicsBakeDialog dialog(names, timeline());
        dialog.resize(360, 280);
        QVERIFY(show(dialog));
        auto* scroll = widget<QScrollArea>(dialog, "physicsBakeScroll");
        QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
        for (auto kind : {QDialogButtonBox::Ok, QDialogButtonBox::Cancel}) {
            auto* item = button(dialog, kind);
            const QRect bounds(item->mapTo(&dialog, QPoint()), item->size());
            QVERIFY(dialog.rect().contains(bounds));
            QVERIFY(item->isVisible());
        }
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        auto* fps = widget<QDoubleSpinBox>(dialog, "physicsBakeFps");
        scroll->ensureWidgetVisible(fps);
        QCoreApplication::processEvents();
        const QRect bounds(fps->mapTo(scroll->viewport(), QPoint()), fps->size());
        QVERIFY(bounds.width() <= scroll->viewport()->width());
    }

    void screenshots_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void screenshots()
    {
        QFETCH(bool, dark);
        qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        CPhysicsBakeDialog dialog({{"ParamHairFront", "前发摆动"}, {"ParamHairSide", "侧发摆动"},
                                   {"ParamHairBack", "后发摆动"}}, timeline());
        dialog.resize(560, 560);
        QVERIFY(show(dialog));
        QTest::qWait(30);
        auto image = dialog.grab();
        QVERIFY(!image.isNull());
        const auto directory = qEnvironmentVariable("PHYSICS_BAKE_SCREENSHOTS");
        if (!directory.isEmpty()) {
            QVERIFY(QDir().mkpath(directory));
            QVERIFY(image.save(QDir(directory).filePath(dark ? "physics-bake-dark.png" : "physics-bake-light.png")));
        }
    }
};

QTEST_MAIN(PhysicsBakeDialogTests)
#include "physics_bake_dialog_tests.moc"
