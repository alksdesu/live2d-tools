#include "ui/cmeshtoolbar.h"
#include "ui/theme.h"
#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QMenu>
#include <QDir>
#include <QSignalSpy>
#include <QStyle>
#include <QStyleFactory>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace {
QAction* action(CMeshToolBar& bar, const char* name) { return bar.findChild<QAction*>(QString::fromLatin1(name)); }
QToolButton* button(CMeshToolBar& bar, const char* name) { return qobject_cast<QToolButton*>(bar.widgetForAction(action(bar, name))); }
QStringList names() { return {"meshAutoGenerate", "meshToolsEnabled", "meshSelect", "meshInsert", "meshConnect", "meshDelete", "meshPreserveBoundary", "meshRetriangulate"}; }
double luminance(const QColor& color)
{
    const auto linear = [](double c) { return c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4); };
    return .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) + .0722 * linear(color.blueF());
}
double contrast(const QColor& a, const QColor& b)
{
    const double first = luminance(a), second = luminance(b);
    return (std::max(first, second) + .05) / (std::min(first, second) + .05);
}
int colorPixels(const QImage& image, const QColor& color)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) if (image.pixelColor(x, y).rgb() == color.rgb()) ++count;
    return count;
}
QColor backgroundColor(const QImage& image)
{
    QMap<QRgb, int> counts;
    for (int y = 1; y + 1 < image.height(); ++y) for (int x = 1; x + 1 < image.width(); ++x) ++counts[image.pixel(x, y)];
    QRgb selected = 0; int maximum = 0;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) if (it.value() > maximum) { selected = it.key(); maximum = it.value(); }
    return QColor::fromRgba(selected);
}
QImage renderedChild(QWidget& parent, QWidget& child)
{
    const auto rendered = parent.grab().toImage(); const qreal ratio = rendered.devicePixelRatio();
    const QPoint origin = child.mapTo(&parent, QPoint());
    return rendered.copy(QRect(qRound(origin.x() * ratio), qRound(origin.y() * ratio), qRound(child.width() * ratio), qRound(child.height() * ratio)));
}
int visibleArrowPixels(const QImage& image)
{
    const auto background = backgroundColor(image); const qreal ratio = image.devicePixelRatio(); int ink = 0;
    for (int y = image.height() / 3; y < image.height() * 2 / 3; ++y)
        for (int x = qRound(3 * ratio); x < image.width() - qRound(3 * ratio); ++x)
            ink += contrast(image.pixelColor(x, y), background) >= 3;
    return ink;
}
struct Host {
    QWidget window;
    CMeshToolBar* bar;
    explicit Host(int width = 900)
    {
        auto* layout = new QVBoxLayout(&window); layout->setContentsMargins(0, 0, 0, 0);
        bar = new CMeshToolBar(&window); layout->addWidget(bar); layout->addStretch(); window.resize(width, 180);
        window.show(); window.activateWindow(); QApplication::processEvents();
    }
};
}

class MeshToolbarTests : public QObject
{
    Q_OBJECT
private slots:
    void init()
    {
        qApp->setStyleSheet({}); qApp->setStyle(QStyleFactory::create("Fusion"));
        qApp->setPalette(qApp->style()->standardPalette());
        qApp->setStyleSheet(Theme::globalStyleSheet(false));
    }
    void defaultsAndAuthoritativeUpdatesDoNotEmitRequests()
    {
        Host host; auto& bar = *host.bar;
        QSignalSpy editing(&bar, &CMeshToolBar::EditingRequested), tools(&bar, &CMeshToolBar::ToolRequested), optimize(&bar, &CMeshToolBar::RetriangulateRequested);
        QCOMPARE(bar.CurrentTool(), CMeshToolBar::Tool::Select); QVERIFY(!bar.IsEditing());
        for (const auto& name : names()) QVERIFY(!action(bar, qPrintable(name))->isEnabled());
        auto* context = bar.findChild<QLabel*>("meshToolContext"); QVERIFY(context); QCOMPARE(context->text(), QStringLiteral("尚未选择网格"));
        bar.SetEditing(true); QVERIFY(!bar.IsEditing());
        bar.SetContext("Hair", 120, 3, true); QVERIFY(action(bar, "meshToolsEnabled")->isEnabled()); QVERIFY(!action(bar, "meshInsert")->isEnabled());
        bar.SetEditing(true); QVERIFY(bar.IsEditing());
        QApplication::processEvents(); QVERIFY(context->isVisibleTo(&bar)); QVERIFY(context->width() > 0);
        for (const auto& name : names()) QVERIFY(action(bar, qPrintable(name))->isEnabled());
        for (const auto tool : {CMeshToolBar::Tool::Select, CMeshToolBar::Tool::Insert, CMeshToolBar::Tool::Connect, CMeshToolBar::Tool::Delete}) {
            bar.SetTool(tool); QCOMPARE(bar.CurrentTool(), tool);
            int checked = 0; for (const char* name : {"meshSelect", "meshInsert", "meshConnect", "meshDelete"}) checked += action(bar, name)->isChecked(); QCOMPARE(checked, 1);
        }
        bar.SetTool(CMeshToolBar::Tool(99)); QCOMPARE(bar.CurrentTool(), CMeshToolBar::Tool::Delete);
        bar.SetEditing(false); QVERIFY(!bar.IsEditing()); QVERIFY(!action(bar, "meshRetriangulate")->isEnabled());
        QCOMPARE(editing.count(), 0); QCOMPARE(tools.count(), 0); QCOMPARE(optimize.count(), 0);
    }
    void actualClicksHaveOneRequestAndSynchronousStateFeedbackDoesNotRecurse()
    {
        Host host; auto& bar = *host.bar; bar.SetContext("Mesh", 10, 0, true);
        QSignalSpy editing(&bar, &CMeshToolBar::EditingRequested), tools(&bar, &CMeshToolBar::ToolRequested), optimize(&bar, &CMeshToolBar::RetriangulateRequested);
        connect(&bar, &CMeshToolBar::EditingRequested, &bar, [&bar](bool value) { bar.SetEditing(value); });
        connect(&bar, &CMeshToolBar::ToolRequested, &bar, [&bar](auto tool) { bar.SetTool(tool); });
        QTest::mouseClick(button(bar, "meshToolsEnabled"), Qt::LeftButton); QCOMPARE(editing.count(), 1); QCOMPARE(editing.first().first().toBool(), true); QVERIFY(bar.IsEditing());
        QTest::mouseClick(button(bar, "meshInsert"), Qt::LeftButton); QCOMPARE(tools.count(), 1); QCOMPARE(bar.CurrentTool(), CMeshToolBar::Tool::Insert);
        QTest::mouseClick(button(bar, "meshRetriangulate"), Qt::LeftButton); QCOMPARE(optimize.count(), 1);
        QTest::mouseClick(button(bar, "meshToolsEnabled"), Qt::LeftButton); QCOMPARE(editing.count(), 2); QVERIFY(!bar.IsEditing());
        QTest::mouseClick(button(bar, "meshDelete"), Qt::LeftButton); QCOMPARE(tools.count(), 1);
    }
    void automaticMeshDoesNotRequireManualToolsAndTracksAvailability()
    {
        Host host; auto& bar = *host.bar;
        QSignalSpy automatic(&bar, &CMeshToolBar::AutoGenerateRequested);
        QVERIFY(!action(bar, "meshAutoGenerate")->isEnabled());
        bar.SetContext("Mesh", 4, 0, true); QVERIFY(!bar.IsEditing()); QVERIFY(action(bar, "meshAutoGenerate")->isEnabled());
        connect(&bar, &CMeshToolBar::AutoGenerateRequested, &bar, [&bar] { bar.SetContext("Mesh", 4, 0, false, QStringLiteral("正在生成网格")); });
        QTest::mouseClick(button(bar, "meshAutoGenerate"), Qt::LeftButton); QCOMPARE(automatic.count(), 1); QVERIFY(!bar.IsEditing());
        QVERIFY(!action(bar, "meshAutoGenerate")->isEnabled());
        bar.SetContext("Mesh", 4, 0, true); QVERIFY(action(bar, "meshAutoGenerate")->isEnabled());
    }
    void boundaryProtectionIsExplicitAndSurvivesContextChanges()
    {
        Host host; auto& bar = *host.bar;
        auto* protection = action(bar, "meshPreserveBoundary"); QVERIFY(protection);
        QVERIFY(bar.PreservesBoundary()); QVERIFY(protection->isCheckable()); QVERIFY(!protection->isEnabled());
        bar.SetContext("Hair", 12, 4, true); bar.SetEditing(true);
        QVERIFY(protection->isEnabled());
        QSignalSpy tools(&bar, &CMeshToolBar::ToolRequested), editing(&bar, &CMeshToolBar::EditingRequested);
        QTest::mouseClick(button(bar, "meshPreserveBoundary"), Qt::LeftButton);
        QVERIFY(!bar.PreservesBoundary()); QCOMPARE(bar.CurrentTool(), CMeshToolBar::Tool::Select);
        bar.SetContext("Eye", 8, 0, true); bar.SetTool(CMeshToolBar::Tool::Delete);
        QVERIFY(!bar.PreservesBoundary());
        bar.SetContext("Eye", 8, 0, false, QStringLiteral("先解除网格锁定")); bar.SetEditing(false);
        QVERIFY(!protection->isEnabled()); QVERIFY(!bar.PreservesBoundary());
        QTest::mouseClick(button(bar, "meshPreserveBoundary"), Qt::LeftButton); QVERIFY(!bar.PreservesBoundary());
        bar.SetContext("Eye", 8, 0, true); bar.SetEditing(true);
        button(bar, "meshPreserveBoundary")->setFocus(); QTest::keyClick(button(bar, "meshPreserveBoundary"), Qt::Key_Space);
        QVERIFY(bar.PreservesBoundary()); QCOMPARE(bar.CurrentTool(), CMeshToolBar::Tool::Delete);
        QCOMPARE(tools.count(), 0); QCOMPARE(editing.count(), 0);
    }
    void rejectedRequestsRestoreThePreviousToolWithoutAnotherRequest()
    {
        Host host; auto& bar = *host.bar; bar.SetContext("Mesh", 10, 2, true); bar.SetEditing(true); bar.SetTool(CMeshToolBar::Tool::Connect);
        QSignalSpy tools(&bar, &CMeshToolBar::ToolRequested);
        connect(&bar, &CMeshToolBar::ToolRequested, &bar, [&bar] { bar.SetTool(CMeshToolBar::Tool::Connect); });
        QTest::mouseClick(button(bar, "meshDelete"), Qt::LeftButton);
        QCOMPARE(tools.count(), 1); QCOMPARE(bar.CurrentTool(), CMeshToolBar::Tool::Connect); QVERIFY(action(bar, "meshConnect")->isChecked()); QVERIFY(!action(bar, "meshDelete")->isChecked());
    }
    void keyboardCanReachAndActivateTools()
    {
        Host host; auto& bar = *host.bar; bar.SetContext("Mesh", 10, 0, true); bar.SetEditing(true);
        QSignalSpy tools(&bar, &CMeshToolBar::ToolRequested);
        button(bar, "meshToolsEnabled")->setFocus(); QTRY_VERIFY(button(bar, "meshToolsEnabled")->hasFocus());
        QSet<QWidget*> visited;
        for (int i = 0; i < 12; ++i) {
            auto* focused = QApplication::focusWidget(); if (focused) visited.insert(focused);
            QTest::keyClick(focused ? focused : &host.window, Qt::Key_Tab);
        }
        QVERIFY(visited.contains(button(bar, "meshInsert"))); QVERIFY(visited.contains(button(bar, "meshConnect")));
        QVERIFY(visited.contains(button(bar, "meshPreserveBoundary")));
        button(bar, "meshInsert")->setFocus(); QTest::keyClick(button(bar, "meshInsert"), Qt::Key_Space);
        QCOMPARE(tools.count(), 1); QCOMPARE(bar.CurrentTool(), CMeshToolBar::Tool::Insert);
    }
    void targetSelectionAndLockedReasonAreLiteralText()
    {
        Host host; auto& bar = *host.bar;
        const QString name = QStringLiteral("<img src='file:///private/path'>头发 & <b>轮廓</b>");
        const QString reason = QStringLiteral("父级 <b>Warp&1</b> 已锁定");
        bar.SetContext(name, 1234, 17, true);
        auto* context = bar.findChild<QLabel*>("meshToolContext"); QVERIFY(context); QCOMPARE(context->textFormat(), Qt::PlainText);
        QVERIFY(context->text().startsWith(name)); QVERIFY(context->text().contains("1234")); QVERIFY(context->text().contains("17"));
        QVERIFY(!context->toolTip().contains("<img")); QTextDocument tooltip; tooltip.setHtml(context->toolTip()); QCOMPARE(tooltip.toPlainText(), context->text().simplified());
        bar.SetEditing(true); QSignalSpy editing(&bar, &CMeshToolBar::EditingRequested);
        bar.SetContext(name, 1234, 17, false, reason); bar.SetEditing(false);
        for (const auto& name : names()) QVERIFY(!action(bar, qPrintable(name))->isEnabled());
        tooltip.setHtml(context->toolTip()); QCOMPARE(tooltip.toPlainText(), reason);
        tooltip.setHtml(action(bar, "meshToolsEnabled")->toolTip()); QCOMPARE(tooltip.toPlainText(), reason);
        QCOMPARE(editing.count(), 0);
    }
    void narrowToolbarKeepsEveryActionReachableThroughOverflow_data()
    {
        QTest::addColumn<int>("width"); QTest::addColumn<bool>("dark");
        for (bool dark : {false, true}) for (int width : {900, 460, 260}) QTest::newRow(qPrintable(QString("%1-%2").arg(dark ? "dark" : "light").arg(width))) << width << dark;
    }
    void narrowToolbarKeepsEveryActionReachableThroughOverflow()
    {
        QFETCH(int, width); QFETCH(bool, dark); qApp->setStyleSheet(Theme::globalStyleSheet(dark)); Host host(width); auto& bar = *host.bar;
        bar.SetContext(QStringLiteral("很长的网格名称 / Hair<样本> / ") + QString(160, QChar(0x7f51)), 32768, 123, true); bar.SetEditing(true);
        QApplication::processEvents(); QCOMPARE(host.window.width(), width); QVERIFY(bar.width() <= width);
        const QString output = qEnvironmentVariable("LIVE2D_MESH_TOOLBAR_QA_DIR");
        if (!output.isEmpty()) { QDir().mkpath(output); QVERIFY(host.window.grab().save(output + QString("/long-name-%1-%2.png").arg(dark ? "dark" : "light").arg(width))); }
        QStringList hidden;
        for (const auto& name : names()) {
            auto* widget = bar.widgetForAction(action(bar, qPrintable(name))); QVERIFY(widget);
            if (!widget->isVisibleTo(&bar)) hidden.append(name);
            else QVERIFY2(bar.rect().contains(widget->geometry()), qPrintable(name));
        }
        if (hidden.isEmpty()) { QCOMPARE(width, 900); return; }
        auto* extension = bar.findChild<QToolButton*>("qt_toolbar_ext_button"); QVERIFY(extension); QVERIFY(extension->isVisibleTo(&bar)); QVERIFY(extension->isEnabled());
        QVERIFY(extension->focusPolicy() & Qt::TabFocus);
        const auto indicator = renderedChild(bar, *extension);
        QVERIFY2(visibleArrowPixels(indicator) >= 3, "The overflow arrow must be visibly rendered, not only clickable");
        if (!output.isEmpty()) {
            QVERIFY(indicator.save(output + QString("/overflow-%1-%2.png").arg(dark ? "dark" : "light").arg(width)));
        }
        QStringList reachable; bool popupShown = false; bool clicked = false;
        QSignalSpy optimize(&bar, &CMeshToolBar::RetriangulateRequested);
        QTimer::singleShot(0, &bar, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) menu = extension->menu();
            if (!menu) return;
            popupShown = menu->isVisible();
            for (auto* item : menu->actions()) reachable.append(item->objectName());
            auto* last = action(bar, "meshRetriangulate");
            if (menu->actions().contains(last) && last->isEnabled()) {
                QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(last).center()); clicked = true;
            } else menu->close();
        });
        QTest::mouseClick(extension, Qt::LeftButton);
        QVERIFY(popupShown);
        for (const auto& name : hidden) QVERIFY2(reachable.contains(name), qPrintable(name));
        if (hidden.contains("meshRetriangulate")) { QVERIFY(clicked); QCOMPARE(optimize.count(), 1); }
    }
    void lightAndDarkTextRemainReadableAndSwitchingThemeKeepsState_data()
    {
        QTest::addColumn<bool>("dark"); QTest::newRow("light") << false; QTest::newRow("dark") << true;
    }
    void lightAndDarkTextRemainReadableAndSwitchingThemeKeepsState()
    {
        QFETCH(bool, dark); qApp->setStyleSheet(Theme::globalStyleSheet(dark)); Host host;
        auto& bar = *host.bar; bar.SetContext(QStringLiteral("头发 Hair"), 42, 3, true); bar.SetEditing(true); bar.SetTool(CMeshToolBar::Tool::Insert);
        QApplication::processEvents(); const auto tokens = Theme::tokens(dark);
        const auto toolbarImage = bar.grab().toImage();
        for (const char* name : {"meshSelect", "meshInsert"}) {
            auto* widget = button(bar, name); QVERIFY(widget);
            const qreal ratio = toolbarImage.devicePixelRatio();
            const QPoint origin = widget->mapTo(&bar, QPoint());
            const auto image = toolbarImage.copy(QRect(qRound(origin.x() * ratio), qRound(origin.y() * ratio), qRound(widget->width() * ratio), qRound(widget->height() * ratio)));
            const auto foreground = QColor(tokens.value(QString(name) == "meshInsert" ? "@accentInk@" : "@textMuted@"));
            const QColor background = backgroundColor(image);
            QVERIFY2(colorPixels(image, foreground) > 0, name);
            QVERIFY2(contrast(foreground, background) >= 4.5, qPrintable(QString("%1: %2 / %3 = %4").arg(name, foreground.name(), background.name()).arg(contrast(foreground, background))));
        }
        auto* context = bar.findChild<QLabel*>("meshToolContext"); QVERIFY(context);
        QVERIFY(contrast(context->palette().color(QPalette::WindowText), QColor(tokens.value("@surface@"))) >= 4.5);
        const QString output = qEnvironmentVariable("LIVE2D_MESH_TOOLBAR_QA_DIR");
        if (!output.isEmpty()) {
            QDir().mkpath(output);
            for (int width : {900, 460, 260}) { host.window.resize(width, 180); QApplication::processEvents(); QVERIFY(host.window.grab().save(output + QString("/toolbar-%1-%2.png").arg(dark ? "dark" : "light").arg(width))); }
        }
        QSignalSpy tools(&bar, &CMeshToolBar::ToolRequested), editing(&bar, &CMeshToolBar::EditingRequested);
        qApp->setStyleSheet(Theme::globalStyleSheet(!dark)); QApplication::processEvents();
        QCOMPARE(bar.CurrentTool(), CMeshToolBar::Tool::Insert); QVERIFY(bar.IsEditing()); QVERIFY(context->text().contains("42")); QVERIFY(context->text().contains("3"));
        QCOMPARE(tools.count(), 0); QCOMPARE(editing.count(), 0);
        host.window.resize(260, 180); QApplication::processEvents();
        auto* extension = bar.findChild<QToolButton*>("qt_toolbar_ext_button"); QVERIFY(extension); QVERIFY(extension->isVisibleTo(&bar));
        QVERIFY(visibleArrowPixels(renderedChild(bar, *extension)) >= 3);
    }
};
QTEST_MAIN(MeshToolbarTests)
#include "mesh_toolbar_tests.moc"
