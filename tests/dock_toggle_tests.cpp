#include "ui/cdocktoggle.h"
#include "ui/theme.h"
#include <QtTest>
#include <QApplication>
#include <QMainWindow>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTabBar>
#include <QToolBar>
#include <memory>
#include <vector>

namespace {
class DockWorkspace
{
public:
    QMainWindow window;
    QVector<QDockWidget*> docks;
    QVector<QPushButton*> buttons;

    DockWorkspace(bool animated, bool themed)
    {
        window.setDockOptions(QMainWindow::AllowTabbedDocks | QMainWindow::AllowNestedDocks |
                              (animated ? QMainWindow::AnimatedDocks : QMainWindow::DockOptions{}));
        if (themed) window.setStyleSheet(Theme::globalStyleSheet(true));
        window.setCentralWidget(new QWidget);
        auto* toolbar = window.addToolBar("Panels");
        toolbar->setObjectName("panelToolbar");
        for (const auto& name : {"First", "Second", "Third"}) {
            auto* dock = new QDockWidget(QString::fromLatin1(name), &window);
            dock->setObjectName(QString::fromLatin1(name));
            auto* content = new QScrollArea;
            content->setWidgetResizable(true);
            content->setWidget(new QWidget);
            dock->setWidget(content);
            window.addDockWidget(Qt::RightDockWidgetArea, dock);
            if (!docks.isEmpty()) window.tabifyDockWidget(docks.first(), dock);
            auto* button = new QPushButton(QString::fromLatin1(name));
            button->setCheckable(true);
            toolbar->addWidget(button);
            ConnectDockToggle(button, dock);
            docks.append(dock);
            buttons.append(button);
        }
        window.resize(800, 500);
        window.show();
        QCoreApplication::processEvents();
    }

    QTabBar* tabBar() const
    {
        const auto bars = window.findChildren<QTabBar*>();
        for (auto* bar : bars) if (bar->count() > 1 && bar->isVisible()) return bar;
        return nullptr;
    }
};
}

class DockToggleTests : public QObject
{
    Q_OBJECT
private slots:
    void tabSelectionKeepsOpenPanels_data()
    {
        QTest::addColumn<bool>("animated");
        QTest::addColumn<bool>("themed");
        QTest::newRow("plain-still") << false << false;
        QTest::newRow("plain-animated") << true << false;
        QTest::newRow("theme-still") << false << true;
        QTest::newRow("theme-animated") << true << true;
    }

    void tabSelectionKeepsOpenPanels()
    {
        QFETCH(bool, animated);
        QFETCH(bool, themed);
        DockWorkspace workspace(animated, themed);
        auto* tabs = workspace.tabBar();
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 3);
        std::vector<std::unique_ptr<QSignalSpy>> toggles;
        for (auto* button : workspace.buttons) {
            QVERIFY(button->isChecked());
            toggles.emplace_back(std::make_unique<QSignalSpy>(button, &QAbstractButton::toggled));
        }
        QSignalSpy visibility(workspace.docks.last(), &QDockWidget::visibilityChanged);
        for (const int selected : {0, 1, 2, 1, 0}) {
            QTest::mouseClick(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(selected).center());
            QCoreApplication::processEvents();
            QCOMPARE(tabs->currentIndex(), selected);
            QCOMPARE(tabs->count(), 3);
            for (int i = 0; i < workspace.docks.size(); ++i) {
                QVERIFY(!workspace.docks[i]->isHidden());
                QVERIFY(workspace.buttons[i]->isChecked());
                QVERIFY(workspace.docks[i]->toggleViewAction()->isChecked());
                QCOMPARE(toggles[i]->size(), 0);
            }
        }
        bool inactiveSignal = false;
        for (const auto& arguments : visibility) inactiveSignal |= !arguments.first().toBool();
        QVERIFY(inactiveSignal);
    }

    void explicitCloseOpenAndWorkspaceCycles_data() { tabSelectionKeepsOpenPanels_data(); }

    void explicitCloseOpenAndWorkspaceCycles()
    {
        QFETCH(bool, animated);
        QFETCH(bool, themed);
        DockWorkspace workspace(animated, themed);
        auto* first = workspace.docks.first();
        auto* button = workspace.buttons.first();
        QSignalSpy toggled(button, &QAbstractButton::toggled), clicked(button, &QAbstractButton::clicked);
        first->close();
        QCoreApplication::processEvents();
        QVERIFY(first->isHidden());
        QVERIFY(!button->isChecked());
        QCOMPARE(toggled.size(), 0);
        QCOMPARE(clicked.size(), 0);
        QTest::mouseClick(button, Qt::LeftButton);
        QCoreApplication::processEvents();
        QVERIFY(!first->isHidden());
        QVERIFY(button->isChecked());
        auto* tabs = workspace.tabBar();
        QVERIFY(tabs);
        QCOMPARE(tabs->tabText(tabs->currentIndex()), first->windowTitle());
        QCOMPARE(toggled.size(), 1);
        QCOMPARE(clicked.size(), 1);
        QTest::mouseClick(button, Qt::LeftButton);
        QVERIFY(first->isHidden());
        QVERIFY(!button->isChecked());
        for (int cycle = 0; cycle < 5; ++cycle) {
            for (int mode = 0; mode < 3; ++mode) {
                for (int i = 0; i < workspace.docks.size(); ++i) workspace.docks[i]->setVisible(i != mode);
                QCoreApplication::processEvents();
                for (int i = 0; i < workspace.docks.size(); ++i) {
                    QCOMPARE(workspace.docks[i]->isHidden(), i == mode);
                    QCOMPARE(workspace.buttons[i]->isChecked(), i != mode);
                }
            }
        }
        QCOMPARE(toggled.size(), 2);
        QCOMPARE(clicked.size(), 2);
        for (auto* dock : workspace.docks) dock->show();
        QCoreApplication::processEvents();
        tabs = workspace.tabBar();
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 3);
        const QByteArray state = workspace.window.saveState();
        for (auto* dock : workspace.docks) dock->hide();
        QVERIFY(workspace.window.restoreState(state));
        QCoreApplication::processEvents();
        for (int i = 0; i < workspace.docks.size(); ++i) {
            QVERIFY(!workspace.docks[i]->isHidden());
            QVERIFY(workspace.buttons[i]->isChecked());
        }
    }

    void initializationUsesOpenStateWithoutPublishing()
    {
        QMainWindow window;
        auto* dock = new QDockWidget("Dock", &window);
        dock->setWidget(new QWidget);
        window.addDockWidget(Qt::RightDockWidgetArea, dock);
        dock->hide();
        QPushButton button;
        button.setCheckable(true);
        button.setChecked(true);
        QSignalSpy changes(&button, &QAbstractButton::toggled);
        ConnectDockToggle(&button, dock);
        QVERIFY(!button.isChecked());
        QVERIFY(dock->isHidden());
        QCOMPARE(changes.size(), 0);
        window.show();
        dock->show();
        QCoreApplication::processEvents();
        QVERIFY(button.isChecked());
        QCOMPARE(changes.size(), 0);
    }

    void endpointDestructionDisconnectsSafely()
    {
        QMainWindow window;
        auto* dock = new QDockWidget("Dock", &window);
        dock->setWidget(new QWidget);
        window.addDockWidget(Qt::RightDockWidgetArea, dock);
        auto* button = new QPushButton;
        button->setCheckable(true);
        ConnectDockToggle(button, dock);
        delete button;
        dock->show();
        dock->hide();
        button = new QPushButton;
        button->setCheckable(true);
        ConnectDockToggle(button, dock);
        delete dock;
        button->click();
        delete button;
        QCoreApplication::processEvents();
    }

    void deferredDestructionFromShowDisconnectsSafely()
    {
        QMainWindow window;
        QPointer<QDockWidget> dock = new QDockWidget("Dock", &window);
        dock->setWidget(new QWidget);
        window.addDockWidget(Qt::RightDockWidgetArea, dock);
        dock->hide();
        window.show();
        QCoreApplication::processEvents();
        QPushButton button;
        button.setCheckable(true);
        ConnectDockToggle(&button, dock);
        connect(dock, &QDockWidget::visibilityChanged, &window, [&](bool visible) {
            if (visible && dock) dock->deleteLater();
        });
        button.click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!dock);
        button.click();
    }
};

QTEST_MAIN(DockToggleTests)
#include "dock_toggle_tests.moc"
