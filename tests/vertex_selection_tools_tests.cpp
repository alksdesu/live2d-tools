#include "ui/cvertexselectiontools.h"
#include "ui/theme.h"
#include <QtTest>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyleFactory>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
#include <limits>

namespace {
using Selection = live2dstruct::CVertexSelection;
using Tools = CVertexSelectionTools;
Tools::Context context()
{
    Tools::Context c; c.target = QStringLiteral("头发 <R>"); c.type = "artmesh"; c.available = c.editing = true; c.hardCount = 2; c.affectedCount = 7; return c;
}
QAction* action(Tools& tools, const char* name) { return tools.findChild<QAction*>(QString::fromLatin1(name)); }
QLineEdit* field(Tools& tools, const char* name) { return tools.findChild<QLineEdit*>(QString::fromLatin1(name)); }
void submit(QLineEdit* edit) { QMetaObject::invokeMethod(edit, "editingFinished", Qt::DirectConnection); }
void accept(Tools& tools) { QObject::connect(&tools, &Tools::SettingsRequested, &tools, [&tools](const auto& settings) { tools.SetSettings(settings); }); }
void settle() { for (int i = 0; i < 5; ++i) { QApplication::processEvents(); QTest::qWait(10); } }
struct Host {
    QWidget window; Tools* tools;
    explicit Host(int width = 900) {
        auto* layout = new QVBoxLayout(&window); layout->setContentsMargins(0, 0, 0, 0); tools = new Tools(&window); layout->addWidget(tools); layout->addStretch();
        window.resize(width, 180); tools->SetContext(context()); window.show(); settle();
    }
};
}
class VertexSelectionToolsTests : public QObject
{
    Q_OBJECT
    QString m_nativeStyle;
private slots:
    void initTestCase() { m_nativeStyle = qApp->style()->objectName(); }
    void init() { qApp->setStyleSheet({}); qApp->setStyle(QStyleFactory::create(m_nativeStyle)); qApp->setPalette(qApp->style()->standardPalette()); qApp->setStyleSheet(Theme::globalStyleSheet(false)); }
    void authoritativeStateIsSilentAndEditingIsExplicit()
    {
        Tools tools; QSignalSpy requests(&tools, &Tools::SettingsRequested), editing(&tools, &Tools::EditingRequested);
        QVERIFY(!action(tools, "vertexEditing")->isEnabled()); QVERIFY(!action(tools, "vertexTool3")->isEnabled());
        auto c = context(); c.editing = false; tools.SetContext(c); QVERIFY(action(tools, "vertexEditing")->isEnabled()); QVERIFY(!action(tools, "vertexTool3")->isEnabled());
        action(tools, "vertexEditing")->trigger(); QCOMPARE(editing.size(), 1); QVERIFY(!action(tools, "vertexEditing")->isChecked());
        c.editing = true; tools.SetContext(c); auto settings = tools.Settings(); settings.tool = Selection::Tool::Lasso; settings.soft.enabled = true;
        QVERIFY(tools.SetSettings(settings)); QCOMPARE(requests.size(), 0); QVERIFY(action(tools, "vertexTool2")->isChecked()); QVERIFY(action(tools, "vertexSoftEnabled")->isChecked());
        c.type = "warp"; tools.SetContext(c); QVERIFY(action(tools, "vertexTool3")->isEnabled());
        c.type = "rotation"; c.reason = QStringLiteral("旋转使用角度手柄"); tools.SetContext(c); QVERIFY(!action(tools, "vertexEditing")->isEnabled()); QVERIFY(!action(tools, "vertexOptions")->isEnabled());
        QCOMPARE(tools.findChild<QLabel*>("vertexSelectionStatus")->text(), c.reason);
    }
    void actualToolClickRequestsOnceAndRejectionRestoresCheckedState()
    {
        Host host(1400); auto& tools = *host.tools; QSignalSpy requests(&tools, &Tools::SettingsRequested);
        auto* toolbar = tools.findChild<QToolBar*>(); auto* lasso = qobject_cast<QToolButton*>(toolbar->widgetForAction(action(tools, "vertexTool2")));
        QVERIFY(lasso->isVisible()); QTest::mouseClick(lasso, Qt::LeftButton); QCOMPARE(requests.size(), 1); QVERIFY(action(tools, "vertexTool0")->isChecked());
        accept(tools); QTest::mouseClick(lasso, Qt::LeftButton); QCOMPARE(requests.size(), 2); QCOMPARE(tools.Settings().tool, Selection::Tool::Lasso);
        action(tools, "vertexCombine1")->trigger(); QCOMPARE(tools.Settings().combine, Selection::Combine::Add); QCOMPARE(requests.size(), 3);
    }
    void repeatedStateRefreshPreservesDraftAndError()
    {
        Host host; auto& tools = *host.tools; action(tools, "vertexOptions")->trigger(); settle();
        auto* radius = field(tools, "vertexSoftRadius"); radius->setText("-"); tools.SetError(QStringLiteral("尚未完成输入 <radius>"));
        tools.SetSettings(tools.Settings()); auto c = context(); c.affectedCount = 12; tools.SetContext(c);
        QCOMPARE(radius->text(), QString("-")); QCOMPARE(tools.findChild<QLabel*>("vertexSelectionError")->text(), QStringLiteral("尚未完成输入 <radius>"));
        auto settings = tools.Settings(); settings.soft.radius = 135; QVERIFY(tools.SetSettings(settings)); QCOMPARE(radius->text(), QString("135"));
        QVERIFY(tools.findChild<QLabel*>("vertexSelectionError")->text().isEmpty());
        radius->setText("-"); c.target = "Other"; tools.SetContext(c); QVERIFY(!tools.findChild<QFrame*>("vertexSelectionPopup")->isVisible()); QCOMPARE(radius->text(), QString("135"));
    }
    void untouchedPercentagesDoNotLosePrecision()
    {
        Tools tools; tools.SetContext(context()); accept(tools);
        for (double value : {.1, 1.0 / 3, std::nextafter(.8765432109876543, 1.0), 0.000000000000123456789}) {
            auto settings = tools.Settings(); settings.tool = Selection::Tool::Brush; settings.brush.strength = value; settings.brush.hardness = std::nextafter(value, 1.0); QVERIFY(tools.SetSettings(settings));
            auto* radius = field(tools, "vertexBrushRadius"); radius->setText("321.125"); submit(radius);
            QCOMPARE(tools.Settings().brush.radius, 321.125); QCOMPARE(tools.Settings().brush.strength, settings.brush.strength); QCOMPARE(tools.Settings().brush.hardness, settings.brush.hardness);
        }
    }
    void fullNumericRangeAndInvalidInputAreExplicit()
    {
        Tools tools; tools.SetContext(context()); accept(tools); QSignalSpy requests(&tools, &Tools::SettingsRequested);
        auto* radius = field(tools, "vertexSoftRadius");
        for (const char* value : {"0", "1000000", "1e-100"}) { radius->setText(value); submit(radius); QCOMPARE(tools.Settings().soft.radius, QString(value).toDouble()); }
        const auto accepted = tools.Settings();
        for (const char* value : {"-1", "nan", "inf", "1000001", "unfinished"}) { const int before = requests.size(); radius->setText(value); submit(radius); QCOMPARE(requests.size(), before); QVERIFY(tools.Settings() == accepted); QCOMPARE(radius->text(), QString(value)); QVERIFY(!tools.findChild<QLabel*>("vertexSelectionError")->text().isEmpty()); }
        auto settings = accepted; settings.tool = Selection::Tool::Brush; tools.SetSettings(settings);
        auto* brush = field(tools, "vertexBrushRadius"); brush->setText("0"); const int before = requests.size(); submit(brush); QCOMPARE(requests.size(), before);
        settings.brush.radius = std::numeric_limits<double>::infinity(); QString error; QVERIFY(!tools.SetSettings(settings, &error)); QVERIFY(!error.isEmpty());
    }
    void paintedWeightsStayDistinctAndCannotEnableDistance()
    {
        Tools tools; tools.SetContext(context()); auto settings = tools.Settings(); settings.soft.enabled = true; tools.SetSettings(settings);
        auto c = context(); c.source = Selection::Source::Painted; c.hardCount = 0; c.affectedCount = 0; tools.SetContext(c);
        QVERIFY(!action(tools, "vertexSoftEnabled")->isEnabled()); QVERIFY(!action(tools, "vertexSoftEnabled")->isChecked());
        QVERIFY(!field(tools, "vertexSoftRadius")->isEnabled()); QVERIFY(tools.findChild<QLabel*>("vertexSelectionStatus")->text().contains(QStringLiteral("笔刷权重 · 硬选 0 · 影响 0")));
        QSignalSpy requests(&tools, &Tools::SettingsRequested); field(tools, "vertexSoftRadius")->setText("60"); submit(field(tools, "vertexSoftRadius")); QCOMPARE(requests.size(), 0);
        settings.tool = Selection::Tool::Brush; tools.SetSettings(settings); QVERIFY(field(tools, "vertexBrushRadius")->isEnabled()); QVERIFY(action(tools, "vertexCombine0")->text().contains(QStringLiteral("重画")));
        settings.tool = Selection::Tool::Move; tools.SetSettings(settings); c.source = Selection::Source::Distance; c.hardCount = 2; c.affectedCount = 7; tools.SetContext(c);
        QVERIFY(action(tools, "vertexSoftEnabled")->isEnabled()); QVERIFY(action(tools, "vertexSoftEnabled")->isChecked()); QVERIFY(tools.findChild<QLabel*>("vertexSelectionStatus")->text().contains(QStringLiteral("另影响 5")));
    }
    void busyAndStrokeHistoryAreIndependent()
    {
        Tools tools; auto c = context(); c.canUndoStroke = true; tools.SetContext(c);
        QSignalSpy undo(&tools, &Tools::UndoStrokeRequested), redo(&tools, &Tools::RedoStrokeRequested), settings(&tools, &Tools::SettingsRequested);
        action(tools, "vertexUndoStroke")->trigger(); QCOMPARE(undo.size(), 1); QVERIFY(!action(tools, "vertexRedoStroke")->isEnabled()); QCOMPARE(settings.size(), 0);
        c.canRedoStroke = true; c.busy = true; tools.SetContext(c); QVERIFY(!action(tools, "vertexUndoStroke")->isEnabled()); QVERIFY(!action(tools, "vertexTool0")->isEnabled()); QVERIFY(!field(tools, "vertexSoftRadius")->isEnabled()); QVERIFY(action(tools, "vertexOptions")->isEnabled());
        c.busy = false; tools.SetContext(c); action(tools, "vertexRedoStroke")->trigger(); QCOMPARE(redo.size(), 1);
    }
    void popupDismissalAndRejectedDraft()
    {
        Host host; auto& tools = *host.tools; action(tools, "vertexOptions")->trigger(); auto* popup = tools.findChild<QFrame*>("vertexSelectionPopup"); settle(); QVERIFY(popup->isVisible());
        auto* radius = field(tools, "vertexSoftRadius"); radius->setText("191"); connect(&tools, &Tools::SettingsRequested, &tools, [&] { tools.SetError(QStringLiteral("手势尚未结束")); }); submit(radius);
        QCOMPARE(tools.Settings().soft.radius, 80.0); QCOMPARE(radius->text(), QString("191")); QVERIFY(tools.findChild<QLabel*>("vertexSelectionError")->text().contains(QStringLiteral("手势")));
        popup->hide(); QCOMPARE(radius->text(), QString("80")); QVERIFY(tools.findChild<QLabel*>("vertexSelectionError")->text().isEmpty());
    }
    void ownerCanBeDestroyedByRequests()
    {
        QPointer<Tools> tools = new Tools; tools->SetContext(context()); connect(tools, &Tools::SettingsRequested, tools, [tools] { delete tools; }); action(*tools, "vertexTool2")->trigger(); QVERIFY(!tools);
        tools = new Tools; tools->SetContext(context()); connect(tools, &Tools::EditingRequested, tools, [tools] { delete tools; }); action(*tools, "vertexEditing")->trigger(); QVERIFY(!tools);
    }
    void keyboardCommitsNumbersAndOverflowActionsRemainReachable()
    {
        Host host(320); auto& tools = *host.tools; accept(tools); auto c = context(); c.canUndoStroke = c.canRedoStroke = true; tools.SetContext(c);
        auto* toolbar = tools.findChild<QToolBar*>(); auto* extension = toolbar->findChild<QToolButton*>("qt_toolbar_ext_button");
        QVERIFY(extension && extension->isVisible()); QStringList hidden, reachable;
        for (auto* item : toolbar->actions()) if (toolbar->widgetForAction(item)->isHidden()) hidden.append(item->objectName());
        QVERIFY(hidden.contains("vertexOptions")); bool popupShown = false; QSignalSpy redo(&tools, &Tools::RedoStrokeRequested);
        QTimer::singleShot(0, &tools, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()); if (!menu) menu = extension->menu(); if (!menu) return;
            popupShown = menu->isVisible(); for (auto* item : menu->actions()) reachable.append(item->objectName());
            auto* item = action(tools, "vertexRedoStroke");
            if (menu->actions().contains(item)) QTest::mouseClick(menu, Qt::LeftButton, {}, menu->actionGeometry(item).center()); else menu->close();
        });
        QTest::mouseClick(extension, Qt::LeftButton); QVERIFY(popupShown); for (const auto& name : hidden) QVERIFY2(reachable.contains(name), qPrintable(name)); QCOMPARE(redo.size(), 1);
        action(tools, "vertexOptions")->trigger(); settle(); auto* radius = field(tools, "vertexSoftRadius"); radius->selectAll();
        QTest::keyClicks(radius, "123.5"); QTest::keyClick(radius, Qt::Key_Return); QCOMPARE(tools.Settings().soft.radius, 123.5);
        auto* popup = tools.findChild<QFrame*>("vertexSelectionPopup"); QTest::keyClick(popup, Qt::Key_Escape); settle(); QVERIFY(!popup->isVisible());
    }
    void narrowNativeLayout_data()
    {
        QTest::addColumn<bool>("dark"); QTest::addColumn<int>("width");
        for (bool dark : {false, true}) for (int width : {900, 560, 320}) QTest::newRow(qPrintable(QString("%1-%2").arg(dark ? "dark" : "light").arg(width))) << dark << width;
    }
    void narrowNativeLayout()
    {
        QFETCH(bool, dark); QFETCH(int, width); qApp->setStyleSheet(Theme::globalStyleSheet(dark)); const int requested = std::min(width, qApp->primaryScreen()->availableGeometry().width()); Host host(requested); auto& tools = *host.tools;
        auto c = context(); c.target = QStringLiteral("很长的模型对象名称，包含透明素材 <R> ").repeated(8); c.canUndoStroke = c.canRedoStroke = true; tools.SetContext(c); settle();
        QCOMPARE(host.window.width(), requested); QVERIFY(tools.minimumSizeHint().width() <= requested);
        QVERIFY(tools.findChild<QLabel*>("vertexSelectionStatus")->text().startsWith(QStringLiteral("仅选点 · 硬选 2 · 另影响 5")));
        auto* toolbar = tools.findChild<QToolBar*>(); QVERIFY(toolbar->actions().contains(action(tools, "vertexOptions"))); QVERIFY(action(tools, "vertexCombine")->menu());
        action(tools, "vertexOptions")->trigger(); settle(); auto* popup = tools.findChild<QFrame*>("vertexSelectionPopup"); auto* scroll = popup->findChild<QScrollArea*>();
        QVERIFY(popup->isVisible()); QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0); QVERIFY(popup->width() <= popup->screen()->availableGeometry().width()); QVERIFY(popup->height() <= popup->screen()->availableGeometry().height());
        QVERIFY(field(tools, "vertexSoftRadius")->isVisible()); QVERIFY(!field(tools, "vertexSoftRadius")->accessibleName().isEmpty());
        const QString directory = qEnvironmentVariable("VERTEX_UI_SCREENSHOTS");
        if (!directory.isEmpty() && width == 560) {
            QDir().mkpath(directory);
            const QJsonObject geometry{{"requestedWidth", requested}, {"windowWidth", host.window.width()}, {"screenWidth", host.window.screen()->availableGeometry().width()}, {"screenHeight", host.window.screen()->availableGeometry().height()}, {"dpr", host.window.devicePixelRatioF()}, {"popupWidth", popup->width()}, {"popupHeight", popup->height()}, {"horizontalScrollMaximum", scroll->horizontalScrollBar()->maximum()}, {"nativeStyle", m_nativeStyle}};
            QFile file(directory + QString("/geometry-%1-560.json").arg(dark ? "dark" : "light")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(geometry).toJson());
            host.window.grab().save(directory + QString("/tools-%1-560.png").arg(dark ? "dark" : "light")); popup->grab().save(directory + QString("/distance-%1-560.png").arg(dark ? "dark" : "light"));
            popup->hide(); auto settings = tools.Settings(); settings.tool = Selection::Tool::Brush; tools.SetSettings(settings); action(tools, "vertexOptions")->trigger(); settle(); popup->grab().save(directory + QString("/brush-%1-560.png").arg(dark ? "dark" : "light"));
        }
    }
};
QTEST_MAIN(VertexSelectionToolsTests)
#include "vertex_selection_tools_tests.moc"
