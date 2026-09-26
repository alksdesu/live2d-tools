#include "ui/cworkspacelayouts.h"
#include "ui/cdocktoggle.h"
#include "ui/theme.h"
#include <QtTest>
#include <QDockWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QTabBar>
#include <QTemporaryDir>
#include <QToolBar>
#include <stdexcept>
#include <memory>

namespace {
class WorkspaceWindow
{
public:
    QMainWindow window;
    QMap<QString, QDockWidget*> docks;
    QMap<QString, QPushButton*> buttons;
    QToolBar* toolbar = nullptr;
    int defaults = 0;

    WorkspaceWindow()
    {
        window.setCentralWidget(new QWidget);
        window.setDockOptions(QMainWindow::AllowTabbedDocks | QMainWindow::AllowNestedDocks);
        toolbar = window.addToolBar("Main");
        toolbar->setObjectName("mainToolBar");
        CreateDocks();
        window.resize(1000, 700);
        window.show();
        QCoreApplication::processEvents();
    }

    void CreateDocks()
    {
        for (const QString& id : {"tree", "param", "inspector", "physics", "motion", "blend", "texture", "tags"}) {
            auto* dock = new QDockWidget(id, &window);
            dock->setObjectName(id);
            auto* scroll = new QScrollArea;
            scroll->setWidgetResizable(true);
            scroll->setWidget(new QWidget);
            dock->setWidget(scroll);
            window.addDockWidget(id == "tree" ? Qt::LeftDockWidgetArea :
                                 id == "param" || id == "inspector" || id == "physics" ? Qt::RightDockWidgetArea : Qt::BottomDockWidgetArea, dock);
            docks.insert(id, dock);
            auto* button = new QPushButton(id);
            button->setCheckable(true);
            toolbar->addWidget(button);
            ConnectDockToggle(button, dock);
            buttons.insert(id, button);
        }
        window.tabifyDockWidget(docks["param"], docks["inspector"]);
        window.tabifyDockWidget(docks["param"], docks["physics"]);
        for (const auto& id : {"blend", "texture", "tags"}) window.tabifyDockWidget(docks["motion"], docks[id]);
    }

    void RemoveDocks()
    {
        for (auto* dock : docks) { window.removeDockWidget(dock); delete dock; }
        docks.clear();
        buttons.clear();
        toolbar->clear();
    }

    void Default(int mode)
    {
        ++defaults;
        for (auto item = docks.cbegin(); item != docks.cend(); ++item) {
            const QString id = item.key();
            item.value()->setVisible(id == "tree" || (mode == 2 ? id == "physics" : id == "param" || id == "inspector") ||
                                    (mode == 1 && id == "motion"));
        }
        docks[mode == 2 ? "physics" : "param"]->raise();
        if (mode == 1) docks["motion"]->raise();
    }

    QVariantMap Shape() const
    {
        QVariantMap result;
        for (auto item = docks.cbegin(); item != docks.cend(); ++item) {
            QStringList group;
            for (auto* dock : window.tabifiedDockWidgets(item.value())) group.append(dock->objectName());
            group.sort();
            result.insert(item.key(), QVariantList{int(window.dockWidgetArea(item.value())), item.value()->isHidden(),
                                                  item.value()->isFloating(), group});
        }
        return result;
    }

    QString CurrentTab(const QString& member) const
    {
        for (auto* bar : window.findChildren<QTabBar*>()) {
            bool found = false;
            for (int tab = 0; tab < bar->count(); ++tab) found |= bar->tabText(tab) == member;
            if (found && bar->isVisible()) return bar->tabText(bar->currentIndex());
        }
        return {};
    }
};
}

class WorkspaceLayoutTests : public QObject
{
    Q_OBJECT
private slots:
    void modeLayoutsAndRepeatedActivationPreserveUserChoices_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void modeLayoutsAndRepeatedActivationPreserveUserChoices()
    {
        QFETCH(bool, dark);
        WorkspaceWindow ui;
        ui.window.setStyleSheet(Theme::globalStyleSheet(dark));
        CWorkspaceLayouts layouts;
        layouts.Attach(ui.window);
        const auto defaults = [&](int mode) { ui.Default(mode); };
        QVERIFY(layouts.Apply(ui.window, 0, defaults));
        ui.window.addDockWidget(Qt::LeftDockWidgetArea, ui.docks["param"]);
        ui.docks["inspector"]->hide();
        ui.docks["tags"]->show();
        ui.docks["tags"]->raise();
        QCoreApplication::processEvents();
        const auto modeling = ui.Shape();
        const auto state = ui.window.saveState(CWorkspaceLayouts::LayoutVersion);
        QVERIFY(layouts.Apply(ui.window, 0, defaults));
        QCOMPARE(ui.defaults, 1);
        QCOMPARE(ui.Shape(), modeling);
        QCOMPARE(ui.window.saveState(CWorkspaceLayouts::LayoutVersion), state);
        QVERIFY(layouts.Apply(ui.window, 1, defaults));
        ui.docks["tree"]->hide();
        ui.docks["motion"]->setFloating(true);
        QCoreApplication::processEvents();
        const auto animation = ui.Shape();
        QVERIFY(layouts.Apply(ui.window, 2, defaults));
        ui.docks["texture"]->show();
        QCoreApplication::processEvents();
        const auto physics = ui.Shape();
        for (int repeat = 0; repeat < 3; ++repeat) {
            QVERIFY(layouts.Apply(ui.window, 0, defaults));
            QCOMPARE(ui.Shape(), modeling);
            QVERIFY(layouts.Apply(ui.window, 1, defaults));
            QCOMPARE(ui.Shape(), animation);
            QVERIFY(layouts.Apply(ui.window, 2, defaults));
            QCOMPARE(ui.Shape(), physics);
        }
        QCOMPARE(ui.defaults, 3);
        for (auto it = ui.docks.cbegin(); it != ui.docks.cend(); ++it)
            QCOMPARE(ui.buttons[it.key()]->isChecked(), !it.value()->isHidden());
    }

    void closeAndReopenModelRetainsLayout()
    {
        WorkspaceWindow ui;
        CWorkspaceLayouts layouts;
        const auto defaults = [&](int mode) { ui.Default(mode); };
        layouts.Attach(ui.window);
        QVERIFY(layouts.Apply(ui.window, 0, defaults));
        ui.window.addDockWidget(Qt::LeftDockWidgetArea, ui.docks["param"]);
        ui.docks["inspector"]->hide();
        ui.docks["texture"]->show();
        ui.docks["texture"]->raise();
        QCoreApplication::processEvents();
        const auto before = ui.Shape();
        layouts.Detach(ui.window);
        QCOMPARE(layouts.ActiveMode(), -1);
        ui.RemoveDocks();
        layouts.Capture(ui.window);
        ui.CreateDocks();
        layouts.Attach(ui.window);
        QVERIFY(layouts.Apply(ui.window, 0, defaults));
        QCoreApplication::processEvents();
        QCOMPARE(ui.Shape(), before);
        QCOMPARE(ui.defaults, 1);
        QCOMPARE(ui.CurrentTab("texture"), QStringLiteral("texture"));
    }

    void iniRoundTripRestoresEachModeAndPreservesWindowGeometry()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings settings(directory.filePath("layout.ini"), QSettings::IniFormat);
        settings.setValue("windowGeometry", QByteArray("unchanged geometry"));
        QVariantMap expected[3];
        {
            WorkspaceWindow ui;
            CWorkspaceLayouts layouts;
            layouts.Attach(ui.window);
            for (int mode = 0; mode < 3; ++mode) {
                QVERIFY(layouts.Apply(ui.window, mode, [&](int selected) { ui.Default(selected); }));
                ui.docks["tree"]->setVisible(mode == 1);
                ui.docks["tags"]->setVisible(mode == 2);
                expected[mode] = ui.Shape();
            }
            layouts.Detach(ui.window);
            ui.RemoveDocks();
            layouts.Capture(ui.window);
            layouts.Save(settings);
            settings.sync();
        }
        QSettings reopened(directory.filePath("layout.ini"), QSettings::IniFormat);
        CWorkspaceLayouts restored;
        restored.Load(reopened, 0);
        WorkspaceWindow ui;
        restored.Attach(ui.window);
        for (int mode = 0; mode < 3; ++mode) {
            QVERIFY(restored.Apply(ui.window, mode, [&](int selected) { ui.Default(selected); }));
            QCOMPARE(ui.Shape(), expected[mode]);
        }
        QCOMPARE(ui.defaults, 0);
        QCOMPARE(reopened.value("windowGeometry").toByteArray(), QByteArray("unchanged geometry"));
    }

    void legacyStateMigratesToNormalizedSavedMode_data()
    {
        QTest::addColumn<int>("savedMode");
        QTest::newRow("modeling") << 0;
        QTest::newRow("animation") << 1;
        QTest::newRow("physics") << 2;
        QTest::newRow("negative") << -4;
        QTest::newRow("above-range") << 99;
    }

    void legacyStateMigratesToNormalizedSavedMode()
    {
        QFETCH(int, savedMode);
        QTemporaryDir directory;
        QSettings settings(directory.filePath("layout.ini"), QSettings::IniFormat);
        WorkspaceWindow source;
        source.window.addDockWidget(Qt::LeftDockWidgetArea, source.docks["param"]);
        source.docks["inspector"]->hide();
        settings.setValue("windowState", source.window.saveState());
        const auto expected = source.Shape();
        CWorkspaceLayouts layouts;
        layouts.Load(settings, savedMode);
        layouts.Save(settings);
        QCOMPARE(settings.value("workspaceLayouts/version").toInt(), 1);
        CWorkspaceLayouts reopened;
        reopened.Load(settings, savedMode);
        WorkspaceWindow target;
        reopened.Attach(target.window);
        const int normalized = qBound(0, savedMode, 2);
        QVERIFY(reopened.Apply(target.window, savedMode, [&](int mode) { target.Default(mode); }));
        QCOMPARE(reopened.ActiveMode(), normalized);
        QCOMPARE(target.Shape(), expected);
        QCOMPARE(target.defaults, 0);
        QVERIFY(reopened.Apply(target.window, (normalized + 1) % 3, [&](int mode) { target.Default(mode); }));
        QCOMPARE(target.defaults, 1);
    }

    void invalidStateFallsBackToCleanBaseline_data()
    {
        QTest::addColumn<int>("damage");
        QTest::newRow("truncated") << 0;
        QTest::newRow("wrong-qt-version") << 1;
        QTest::newRow("oversized") << 2;
        QTest::newRow("unknown-settings-version") << 3;
    }

    void invalidStateFallsBackToCleanBaseline()
    {
        QFETCH(int, damage);
        QTemporaryDir directory;
        QSettings settings(directory.filePath("layout.ini"), QSettings::IniFormat);
        WorkspaceWindow ui;
        ui.window.addDockWidget(Qt::LeftDockWidgetArea, ui.docks["param"]);
        ui.docks["motion"]->setFloating(true);
        QByteArray damaged = ui.window.saveState(CWorkspaceLayouts::LayoutVersion);
        if (damage == 0) damaged.chop(7);
        if (damage == 2) damaged = QByteArray(16 * 1024 * 1024 + 1, 'x');
        settings.setValue("workspaceLayouts/version", damage == 3 ? 99 : 1);
        settings.setValue("workspaceLayouts/modes/1/state", damaged);
        settings.setValue("workspaceLayouts/modes/1/qtVersion", damage == 1 ? 0 : CWorkspaceLayouts::LayoutVersion);
        CWorkspaceLayouts layouts;
        layouts.Load(settings, 0);
        WorkspaceWindow target;
        layouts.Attach(target.window);
        QVERIFY(layouts.Apply(target.window, 0, [&](int mode) { target.Default(mode); }));
        target.window.addDockWidget(Qt::TopDockWidgetArea, target.docks["param"]);
        const auto modeling = target.Shape();
        QVERIFY(layouts.Apply(target.window, 1, [&](int mode) { target.Default(mode); }));
        QVERIFY(!target.docks["motion"]->isFloating());
        QCOMPARE(target.window.dockWidgetArea(target.docks["param"]), Qt::RightDockWidgetArea);
        QVERIFY(!target.docks["motion"]->isHidden());
        QCOMPARE(target.defaults, 2);
        QVERIFY(layouts.Apply(target.window, 0, [&](int mode) { target.Default(mode); }));
        QCOMPARE(target.Shape(), modeling);
    }

    void failedDefaultDoesNotPublishState()
    {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("layout.ini"), QSettings::IniFormat);
        WorkspaceWindow ui;
        CWorkspaceLayouts layouts;
        layouts.Attach(ui.window);
        QVERIFY(layouts.Apply(ui.window, 0, [&](int mode) { ui.Default(mode); }));
        ui.docks["tags"]->show();
        layouts.Capture(ui.window);
        layouts.Save(settings);
        const auto saved = settings.value("workspaceLayouts/modes/0/state");
        const auto before = ui.Shape();
        QVERIFY(!layouts.Apply(ui.window, 1, [&](int) {
            ui.docks["param"]->setFloating(true);
            ui.docks["tree"]->hide();
            throw std::runtime_error("refused");
        }));
        QCOMPARE(layouts.ActiveMode(), 0);
        QCOMPARE(ui.Shape(), before);
        layouts.Save(settings);
        QCOMPARE(settings.value("workspaceLayouts/modes/0/state"), saved);
        QVERIFY(!settings.contains("workspaceLayouts/modes/1/state"));
    }

    void defaultSizingRunsOnceAfterTheEventTurn()
    {
        WorkspaceWindow ui;
        CWorkspaceLayouts layouts;
        QObject model;
        layouts.Attach(ui.window);
        QVERIFY(layouts.Apply(ui.window, 0, [&](int mode) { ui.Default(mode); }));
        int sized = 0, saved = 0;
        layouts.DeferDefaultSizing(ui.window, &model, [&](int mode) {
            QCOMPARE(mode, 0);
            ++sized;
            ui.window.resizeDocks({ui.docks["tree"], ui.docks["param"]}, {240, 300}, Qt::Horizontal);
        }, [&] { ++saved; });
        QCOMPARE(sized, 0);
        QCoreApplication::processEvents();
        QCOMPARE(sized, 1);
        QCOMPARE(saved, 1);
        QCoreApplication::processEvents();
        QCOMPARE(sized, 1);
        QVERIFY(layouts.Apply(ui.window, 0, [&](int) { QFAIL("same mode reset layout"); }));
        QCOMPARE(sized, 1);
    }

    void changedDockPlacementTakesPriorityOverDefaultSizing_data()
    {
        QTest::addColumn<int>("change");
        QTest::newRow("move") << 0;
        QTest::newRow("float") << 1;
        QTest::newRow("hide") << 2;
        QTest::newRow("input-before-resize") << 3;
        QTest::newRow("capture-before-close") << 4;
    }

    void changedDockPlacementTakesPriorityOverDefaultSizing()
    {
        QFETCH(int, change);
        QTemporaryDir directory;
        QSettings settings(directory.filePath("layout.ini"), QSettings::IniFormat);
        WorkspaceWindow ui;
        CWorkspaceLayouts layouts;
        QObject model;
        layouts.Attach(ui.window);
        QVERIFY(layouts.Apply(ui.window, 0, [&](int mode) { ui.Default(mode); }));
        WorkspaceWindow reference;
        CWorkspaceLayouts referenceLayouts;
        referenceLayouts.Attach(reference.window);
        QVERIFY(referenceLayouts.Apply(reference.window, 0, [&](int mode) { reference.Default(mode); }));
        int sized = 0;
        layouts.DeferDefaultSizing(ui.window, &model, [&](int) { ++sized; }, [] {});
        const auto changeLayout = [&](WorkspaceWindow& target, CWorkspaceLayouts& store) {
            if (change == 0) target.window.addDockWidget(Qt::LeftDockWidgetArea, target.docks["param"]);
            if (change == 1) target.docks["param"]->setFloating(true);
            if (change == 2) target.docks["param"]->hide();
            if (change == 3) {
                QTest::mousePress(target.window.centralWidget(), Qt::LeftButton);
                target.window.resizeDocks({target.docks["param"]}, {320}, Qt::Horizontal);
                QTest::mouseRelease(target.window.centralWidget(), Qt::LeftButton);
            }
            if (change == 4) {
                target.docks["param"]->hide();
                store.Detach(target.window);
            }
        };
        changeLayout(ui, layouts);
        changeLayout(reference, referenceLayouts);
        QCoreApplication::processEvents();
        const auto userLayout = reference.Shape();
        QCOMPARE(sized, 0);
        QCOMPARE(ui.Shape(), userLayout);
        layouts.Capture(ui.window);
        layouts.Save(settings);
        CWorkspaceLayouts next;
        next.Load(settings, 0);
        WorkspaceWindow restored;
        next.Attach(restored.window);
        QVERIFY(next.Apply(restored.window, 0, [&](int) { QFAIL("user layout was discarded"); }));
        QCOMPARE(restored.Shape(), userLayout);
    }

    void rapidModesAndModelReplacementDiscardOldSizingCallbacks()
    {
        WorkspaceWindow ui;
        CWorkspaceLayouts layouts;
        auto model = std::make_unique<QObject>();
        layouts.Attach(ui.window);
        int sized = 0;
        const auto defaults = [&](int mode) { ui.Default(mode); };
        QVERIFY(layouts.Apply(ui.window, 0, defaults));
        layouts.DeferDefaultSizing(ui.window, model.get(), [&](int) { ++sized; }, [] {});
        QVERIFY(layouts.Apply(ui.window, 1, defaults));
        layouts.DeferDefaultSizing(ui.window, model.get(), [&](int) { ++sized; }, [] {});
        QVERIFY(layouts.Apply(ui.window, 0, defaults));
        const auto returned = ui.Shape();
        QCoreApplication::processEvents();
        QCOMPARE(sized, 0);
        QCOMPARE(ui.Shape(), returned);
        layouts.DeferDefaultSizing(ui.window, model.get(), [&](int) { ++sized; }, [] {});
        model.reset();
        QCoreApplication::processEvents();
        QCOMPARE(sized, 0);
        model = std::make_unique<QObject>();
        layouts.DeferDefaultSizing(ui.window, model.get(), [&](int) { ++sized; }, [] {});
        layouts.Detach(ui.window);
        ui.RemoveDocks();
        ui.CreateDocks();
        layouts.Attach(ui.window);
        QVERIFY(layouts.Apply(ui.window, 0, defaults));
        QCoreApplication::processEvents();
        QCOMPARE(sized, 0);
    }

    void pendingSizingCannotOutliveItsCoordinator()
    {
        WorkspaceWindow ui;
        QObject model;
        int sized = 0;
        {
            CWorkspaceLayouts layouts;
            layouts.Attach(ui.window);
            QVERIFY(layouts.Apply(ui.window, 0, [&](int mode) { ui.Default(mode); }));
            layouts.DeferDefaultSizing(ui.window, &model, [&](int) { ++sized; }, [] {});
        }
        QCoreApplication::processEvents();
        QCOMPARE(sized, 0);
    }

    void futureSettingsRemainUntouched()
    {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("layout.ini"), QSettings::IniFormat);
        settings.setValue("workspaceLayouts/version", 9);
        settings.setValue("workspaceLayouts/newFeature", QByteArray("preserve future layout"));
        settings.setValue("workspaceLayouts/modes/1/state", QByteArray("future encoding"));
        CWorkspaceLayouts layouts;
        layouts.Load(settings, 1);
        WorkspaceWindow ui;
        layouts.Attach(ui.window);
        QVERIFY(layouts.Apply(ui.window, 1, [&](int mode) { ui.Default(mode); }));
        layouts.Detach(ui.window);
        layouts.Save(settings);
        QCOMPARE(settings.value("workspaceLayouts/version").toInt(), 9);
        QCOMPARE(settings.value("workspaceLayouts/newFeature").toByteArray(), QByteArray("preserve future layout"));
        QCOMPARE(settings.value("workspaceLayouts/modes/1/state").toByteArray(), QByteArray("future encoding"));
        QCOMPARE(settings.allKeys().size(), 3);
    }

    void unattachedEmptyWindowCannotReplaceSavedModelLayout()
    {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("layout.ini"), QSettings::IniFormat);
        WorkspaceWindow source;
        const auto original = source.window.saveState();
        settings.setValue("windowState", original);
        CWorkspaceLayouts layouts;
        layouts.Load(settings, 2);
        QMainWindow empty;
        layouts.Capture(empty);
        QVERIFY(!layouts.Apply(empty, 2, {}));
        layouts.Detach(empty);
        layouts.Save(settings);
        QCOMPARE(settings.value("workspaceLayouts/modes/2/state").toByteArray(), original);
    }
};

QTEST_MAIN(WorkspaceLayoutTests)
#include "workspace_layout_tests.moc"
