#include "ui/theme.h"
#include "ui/cxyparameterpad.h"
#include "ui/cphysicsgraph.h"
#include "ui/cmotioncurveview.h"

#include <QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListView>
#include <QStandardItemModel>
#include <QMainWindow>
#include <QMouseEvent>
#include <QHoverEvent>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QSlider>
#include <QStyleFactory>
#include <QStyleHints>
#include <QScopeGuard>
#include <QTabBar>
#include <QTableWidget>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <cmath>
#include <memory>

namespace {
double luminance(const QColor& color)
{
    const auto channel = [](double value) {
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return channel(color.redF()) * 0.2126 + channel(color.greenF()) * 0.7152 + channel(color.blueF()) * 0.0722;
}

QColor over(const QColor& foreground, const QColor& background)
{
    const double alpha = foreground.alphaF();
    return QColor::fromRgbF(foreground.redF() * alpha + background.redF() * (1 - alpha),
                           foreground.greenF() * alpha + background.greenF() * (1 - alpha),
                           foreground.blueF() * alpha + background.blueF() * (1 - alpha));
}

double contrast(const QColor& foreground, const QColor& background)
{
    const double first = luminance(over(foreground, background));
    const double second = luminance(background);
    return (std::max(first, second) + 0.05) / (std::min(first, second) + 0.05);
}

int pixelsOf(const QImage& image, const QColor& color)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixelColor(x, y).rgba() == color.rgba()) ++count;
    return count;
}

int changedPixels(const QImage& before, const QImage& after)
{
    if (before.size() != after.size()) return -1;
    int count = 0;
    for (int y = 0; y < before.height(); ++y)
        for (int x = 0; x < before.width(); ++x) count += before.pixel(x, y) != after.pixel(x, y);
    return count;
}

bool applyTheme(const QString& styleName, bool dark)
{
    auto* style = QStyleFactory::create(styleName);
    if (!style) return false;
    qApp->setStyleSheet({});
    qApp->setStyle(style);
    qApp->setPalette(style->standardPalette());
    qApp->setStyleSheet(Theme::globalStyleSheet(dark));
    return true;
}

void activateTestWindow(QWidget& host)
{
    host.show();
    host.activateWindow();
    // Cocoa can keep CLI tests in the background, so activate Qt's focus state before sending keys.
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    QApplication::setActiveWindow(&host);
    QT_WARNING_POP
}

void hoverWidget(QWidget& widget, const QPointF& position)
{
    const auto global = widget.mapToGlobal(position.toPoint());
    QEnterEvent enter(position, widget.mapTo(widget.window(), position.toPoint()), global);
    QCoreApplication::sendEvent(&widget, &enter);
    QMouseEvent move(QEvent::MouseMove, position, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &move);
    QHoverEvent hover(QEvent::HoverMove, position, global, QPointF(), Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &hover);
}

void leaveWidget(QWidget& widget)
{
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(&widget, &leave);
    const QPoint outside(-1, -1);
    QHoverEvent hover(QEvent::HoverLeave, outside, widget.mapToGlobal(outside), QPointF(), Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &hover);
}

QStringList styles()
{
    static const QString nativeStyle = qApp->style()->objectName();
    QStringList result;
    const auto available = QStyleFactory::keys();
    if (QGuiApplication::platformName() == QLatin1String("cocoa") && available.contains(QStringLiteral("macOS")))
        result.append(QStringLiteral("macOS"));
    else if (QGuiApplication::platformName() != QLatin1String("offscreen") &&
             QGuiApplication::platformName() != QLatin1String("minimal")) result.append(nativeStyle);
    if (!result.contains(QStringLiteral("Fusion"), Qt::CaseInsensitive)) result.append(QStringLiteral("Fusion"));
    return result;
}

void styleRows()
{
    QTest::addColumn<QString>("styleName");
    QTest::addColumn<bool>("dark");
    for (const auto& style : styles())
        for (bool dark : {false, true})
            QTest::newRow(qPrintable(style + (dark ? QStringLiteral("-dark") : QStringLiteral("-light")))) << style << dark;
}

std::unique_ptr<QWidget> control(const QString& kind)
{
    if (kind == QLatin1String("tab")) {
        auto tabs = std::make_unique<QTabBar>();
        tabs->addTab(QStringLiteral("Selected HHHHHH"));
        tabs->addTab(QStringLiteral("Other"));
        return tabs;
    }
    if (kind == QLatin1String("checkbox")) return std::make_unique<QCheckBox>(QStringLiteral("Checked HHHHHH"));
    if (kind == QLatin1String("radio")) return std::make_unique<QRadioButton>(QStringLiteral("Radio HHHHHH"));
    if (kind == QLatin1String("slider")) {
        auto slider = std::make_unique<QSlider>(Qt::Horizontal);
        slider->setValue(40);
        return slider;
    }
    if (kind == QLatin1String("combo")) {
        auto combo = std::make_unique<QComboBox>();
        combo->addItems({QStringLiteral("Selected HHHHHH"), QStringLiteral("Other")});
        return combo;
    }
    if (kind == QLatin1String("line")) return std::make_unique<QLineEdit>(QStringLiteral("Editor HHHHHH"));
    if (kind == QLatin1String("plain")) {
        auto text = std::make_unique<QPlainTextEdit>(QStringLiteral("Read only HHHHHH")); text->setReadOnly(true); return text;
    }
    if (kind == QLatin1String("tree")) {
        auto tree = std::make_unique<QTreeWidget>();
        tree->setHeaderHidden(true);
        new QTreeWidgetItem(tree.get(), {QStringLiteral("Selected HHHHHH")});
        tree->setCurrentItem(tree->topLevelItem(0));
        return tree;
    }
    if (kind == QLatin1String("list")) {
        auto list = std::make_unique<QListWidget>();
        list->addItem(QStringLiteral("Selected HHHHHH"));
        list->setCurrentRow(0);
        return list;
    }
    if (kind == QLatin1String("listview")) {
        auto list = std::make_unique<QListView>(); auto* model = new QStandardItemModel(list.get());
        model->appendRow(new QStandardItem(QStringLiteral("Selected HHHHHH"))); list->setModel(model); list->setCurrentIndex(model->index(0, 0)); return list;
    }
    if (kind == QLatin1String("table")) {
        auto table = std::make_unique<QTableWidget>(1, 1);
        table->setItem(0, 0, new QTableWidgetItem(QStringLiteral("Selected HHHHHH")));
        table->setCurrentCell(0, 0);
        return table;
    }
    auto button = std::make_unique<QPushButton>(QStringLiteral("Button HHHHHH"));
    if (kind == QLatin1String("primary")) button->setObjectName(QStringLiteral("primaryBtn"));
    if (kind == QLatin1String("danger")) button->setObjectName(QStringLiteral("dangerBtn"));
    return button;
}
}

class ThemeTests : public QObject
{
    Q_OBJECT

    static void verifyPair(const QImage& image, const QColor& foreground, const QColor& background)
    {
        QVERIFY2(pixelsOf(image, over(foreground, background)) > 0,
                 qPrintable(QStringLiteral("Rendered foreground missing: %1").arg(foreground.name())));
        QVERIFY2(pixelsOf(image, background) > 0,
                 qPrintable(QStringLiteral("Rendered background missing: %1").arg(background.name())));
        QVERIFY2(contrast(foreground, background) >= 4.5,
                 qPrintable(QStringLiteral("Text contrast %1 on %2 is %3:1")
                    .arg(foreground.name(), background.name()).arg(contrast(foreground, background), 0, 'f', 4)));
    }

private slots:
    void cleanup()
    {
        QToolTip::hideText();
        qApp->setStyleSheet({});
    }

    void selectedItemsTabsAndButtonsHaveReadableRenderedText_data() { styleRows(); }

    void selectedItemsTabsAndButtonsHaveReadableRenderedText()
    {
        QFETCH(QString, styleName);
        QFETCH(bool, dark);
        QVERIFY(applyTheme(styleName, dark));
        const auto colors = Theme::tokens(dark);
        QWidget host;
        host.setFocusPolicy(Qt::StrongFocus);
        auto* layout = new QVBoxLayout(&host);
        QVector<QWidget*> widgets;
        for (const QString& kind : {QStringLiteral("tree"), QStringLiteral("list"), QStringLiteral("listview"), QStringLiteral("table"),
                                   QStringLiteral("tab"), QStringLiteral("primary"), QStringLiteral("danger")}) {
            auto widget = control(kind);
            widget->setProperty("auditKind", kind);
            widgets.append(widget.get());
            layout->addWidget(widget.release());
        }
        host.resize(460, 800);
        activateTestWindow(host);
        host.setFocus(Qt::TabFocusReason);
        QCoreApplication::processEvents();
        for (auto* widget : widgets) {
            const auto kind = widget->property("auditKind").toString();
            if (kind == QLatin1String("primary") || kind == QLatin1String("danger")) {
                verifyPair(widget->grab().toImage(), widget->palette().color(QPalette::ButtonText), widget->palette().color(QPalette::Button));
            } else {
                const QImage image = qobject_cast<QAbstractItemView*>(widget)
                    ? qobject_cast<QAbstractItemView*>(widget)->viewport()->grab().toImage() : widget->grab().toImage();
                verifyPair(image, QColor(colors.value(QStringLiteral("@accentInk@"))), QColor(colors.value(QStringLiteral("@accentSoft@"))));
            }
        }
    }

    void documentTabsKeepThemeColorsAcrossLifecycle_data()
    {
        QTest::addColumn<QString>("styleName");
        QTest::addColumn<bool>("themeBeforeCreation");
        for (const auto& style : styles())
            for (bool themeFirst : {false, true})
                QTest::newRow(qPrintable(style + (themeFirst ? "-theme-first" : "-widgets-first")))
                    << style << themeFirst;
    }

    void documentTabsKeepThemeColorsAcrossLifecycle()
    {
        QFETCH(QString, styleName);
        QFETCH(bool, themeBeforeCreation);
        QVERIFY(applyTheme(styleName, false));
        if (!themeBeforeCreation) qApp->setStyleSheet({});
        QMainWindow host;
        auto* content = new QWidget(&host);
        auto* layout = new QVBoxLayout(content);
        auto* documentTabs = new QTabWidget(content);
        auto* ordinaryTabs = new QTabWidget(content);
        for (auto* tabs : {documentTabs, ordinaryTabs}) {
            tabs->addTab(new QLineEdit(QStringLiteral("Preserved input")), QStringLiteral("Selected HHHHHH"));
            tabs->addTab(new QLabel(QStringLiteral("Second page")), QStringLiteral("Other HHHHHH"));
            layout->addWidget(tabs);
        }
        Theme::setDocumentMode(*documentTabs);
        host.setCentralWidget(content);
        auto* firstDock = new QDockWidget(QStringLiteral("First HHHHHH"), &host);
        auto* secondDock = new QDockWidget(QStringLiteral("Second HHHHHH"), &host);
        firstDock->setWidget(new QLabel(QStringLiteral("First dock")));
        secondDock->setWidget(new QLabel(QStringLiteral("Second dock")));
        host.addDockWidget(Qt::BottomDockWidgetArea, firstDock);
        host.addDockWidget(Qt::BottomDockWidgetArea, secondDock);
        host.tabifyDockWidget(firstDock, secondDock);
        host.resize(720, 480);
        activateTestWindow(host);
        QCoreApplication::processEvents();
        const auto dockBars = host.findChildren<QTabBar*>(QString(), Qt::FindDirectChildrenOnly);
        QCOMPARE(dockBars.size(), 1);
        auto* dockTabs = dockBars.first();
        QVERIFY(!dockTabs->documentMode());
        const bool originalDockBase = dockTabs->drawBase();
        const bool originalOrdinaryBase = ordinaryTabs->tabBar()->drawBase();
        QSignalSpy changed(documentTabs, &QTabWidget::currentChanged);
        const int dockIndex = dockTabs->currentIndex();

        for (bool dark : {false, true, false, true}) {
            qApp->setStyleSheet(Theme::globalStyleSheet(dark));
            Theme::setDocumentMode(*documentTabs, false);
            Theme::setDocumentMode(*documentTabs);
            QCoreApplication::processEvents();
            QVERIFY(documentTabs->documentMode());
            QVERIFY(!documentTabs->tabBar()->drawBase());
            QCOMPARE(ordinaryTabs->tabBar()->drawBase(), originalOrdinaryBase);
            QCOMPARE(dockTabs->drawBase(), originalDockBase);
            QCOMPARE(dockTabs->currentIndex(), dockIndex);
            QCOMPARE(documentTabs->currentIndex(), 0);
            QCOMPARE(qobject_cast<QLineEdit*>(documentTabs->widget(0))->text(), QStringLiteral("Preserved input"));
            QVERIFY(changed.isEmpty());
            const auto colors = Theme::tokens(dark);
            for (auto* bar : {documentTabs->tabBar(), ordinaryTabs->tabBar(), dockTabs}) {
                const int selected = bar->currentIndex();
                const int other = selected == 0 ? 1 : 0;
                leaveWidget(*bar);
                verifyPair(bar->grab(bar->tabRect(selected)).toImage(),
                           QColor(colors.value(QStringLiteral("@accentInk@"))),
                           QColor(colors.value(QStringLiteral("@accentSoft@"))));
                verifyPair(bar->grab(bar->tabRect(other)).toImage(),
                           QColor(colors.value(QStringLiteral("@textMuted@"))),
                           QColor(colors.value(QStringLiteral("@window@"))));
                hoverWidget(*bar, bar->tabRect(other).center());
                verifyPair(bar->grab(bar->tabRect(other)).toImage(),
                           QColor(colors.value(QStringLiteral("@text@"))),
                           QColor(colors.value(QStringLiteral("@surfaceHover@"))));
                verifyPair(bar->grab(bar->tabRect(selected)).toImage(),
                           QColor(colors.value(QStringLiteral("@accentInk@"))),
                           QColor(colors.value(QStringLiteral("@accentSoft@"))));
                QCOMPARE(bar->currentIndex(), selected);
                QCOMPARE(qobject_cast<QLineEdit*>(documentTabs->widget(0))->text(), QStringLiteral("Preserved input"));
                QVERIFY(changed.isEmpty());
                leaveWidget(*bar);
                verifyPair(bar->grab(bar->tabRect(other)).toImage(),
                           QColor(colors.value(QStringLiteral("@textMuted@"))),
                           QColor(colors.value(QStringLiteral("@window@"))));
            }
            auto* bar = documentTabs->tabBar();
            const QRect unused(bar->tabRect(bar->count() - 1).right() + 1, 0,
                               bar->width() - bar->tabRect(bar->count() - 1).right() - 1, bar->height());
            QVERIFY(unused.width() > 40);
            const auto background = bar->grab(unused).toImage();
            QCOMPARE(pixelsOf(background, QColor(colors.value(QStringLiteral("@window@")))),
                     background.width() * background.height());
        }
        auto* bar = documentTabs->tabBar();
        bar->setFocus(Qt::TabFocusReason);
        QTest::keyClick(bar, Qt::Key_Right);
        QCOMPARE(documentTabs->currentIndex(), 1);
        QCOMPARE(changed.size(), 1);
    }

    void listChecksUseThemeColors_data() { styleRows(); }

    void listChecksUseThemeColors()
    {
        QFETCH(QString, styleName);
        QFETCH(bool, dark);
        QVERIFY(applyTheme(styleName, dark));
        const auto colors = Theme::tokens(dark);
        QListWidget list;
        auto* item = new QListWidgetItem(QStringLiteral("Binding target"), &list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        list.resize(320, 160);
        activateTestWindow(list);
        list.setCurrentItem(nullptr);
        QCoreApplication::processEvents();
        const auto unchecked = list.viewport()->grab().toImage();
        QVERIFY(pixelsOf(unchecked, QColor(colors.value(QStringLiteral("@borderStrong@")))) > 8);
        item->setCheckState(Qt::Checked);
        QCoreApplication::processEvents();
        const auto checked = list.viewport()->grab().toImage();
        QVERIFY(pixelsOf(checked, QColor(colors.value(QStringLiteral("@accent@")))) > 32);
        QVERIFY(changedPixels(unchecked, checked) > 32);
    }

    void virtualListsRenderChecksHoverAndThemeChanges_data() { styleRows(); }
    void virtualListsRenderChecksHoverAndThemeChanges()
    {
        QFETCH(QString, styleName); QFETCH(bool, dark); QVERIFY(applyTheme(styleName, dark));
        QListView view; QStandardItemModel model; auto* item = new QStandardItem(QStringLiteral("Binding HHHHHH")); item->setCheckable(true); model.appendRow(item);
        model.appendRow(new QStandardItem(QStringLiteral("Hover HHHHHH"))); view.setModel(&model); view.setMouseTracking(true); view.resize(360, 180); activateTestWindow(view);
        view.clearSelection(); view.setCurrentIndex({}); QCoreApplication::processEvents(); const auto colors = Theme::tokens(dark);
        const auto unchecked = view.viewport()->grab().toImage(); QVERIFY(pixelsOf(unchecked, QColor(colors.value(QStringLiteral("@borderStrong@")))) > 8);
        item->setCheckState(Qt::Checked); QCoreApplication::processEvents(); const auto checked = view.viewport()->grab().toImage();
        QVERIFY(pixelsOf(checked, QColor(colors.value(QStringLiteral("@accent@")))) > 32); QVERIFY(changedPixels(unchecked, checked) > 32);
        const QPointF position = view.visualRect(model.index(1, 0)).center();
        hoverWidget(*view.viewport(), position);
        const auto hovered = view.viewport()->grab().toImage(); QVERIFY(pixelsOf(hovered, QColor(colors.value(QStringLiteral("@surfaceHover@")))) > 32);
        view.setCurrentIndex(model.index(0, 0)); qApp->setStyleSheet(Theme::globalStyleSheet(!dark)); QCoreApplication::processEvents();
        QCOMPARE(item->checkState(), Qt::Checked); QCOMPARE(view.currentIndex(), model.index(0, 0)); const auto next = Theme::tokens(!dark);
        verifyPair(view.viewport()->grab().toImage(), QColor(next.value(QStringLiteral("@accentInk@"))), QColor(next.value(QStringLiteral("@accentSoft@"))));
    }

    void readonlyTextHasReadableSelectionWithoutChangingContent_data() { styleRows(); }
    void readonlyTextHasReadableSelectionWithoutChangingContent()
    {
        QFETCH(QString, styleName); QFETCH(bool, dark); QVERIFY(applyTheme(styleName, dark)); const auto colors = Theme::tokens(dark);
        QPlainTextEdit text(QStringLiteral("Read only HHHHHH")); text.setReadOnly(true); text.resize(360, 150); activateTestWindow(text); text.setFocus(Qt::TabFocusReason); text.selectAll();
        QCoreApplication::processEvents(); verifyPair(text.viewport()->grab().toImage(), QColor(colors.value(QStringLiteral("@accentInk@"))), QColor(colors.value(QStringLiteral("@accentSoft@"))));
        QTest::keyClicks(&text, "replacement"); QCOMPARE(text.toPlainText(), QStringLiteral("Read only HHHHHH"));
        qApp->setStyleSheet(Theme::globalStyleSheet(!dark)); QCoreApplication::processEvents(); const auto next = Theme::tokens(!dark);
        verifyPair(text.viewport()->grab().toImage(), QColor(next.value(QStringLiteral("@accentInk@"))), QColor(next.value(QStringLiteral("@accentSoft@"))));
    }

    void tooltipsAndPlaceholdersHaveReadableRenderedText_data() { styleRows(); }

    void tooltipsAndPlaceholdersHaveReadableRenderedText()
    {
        QFETCH(QString, styleName);
        QFETCH(bool, dark);
        QVERIFY(applyTheme(styleName, dark));
        const auto colors = Theme::tokens(dark);
        QWidget host;
        host.setFocusPolicy(Qt::StrongFocus);
        auto* layout = new QVBoxLayout(&host);
        auto* line = new QLineEdit;
        line->setPlaceholderText(QStringLiteral("Placeholder HHHHHH"));
        layout->addWidget(line);
        host.resize(460, 140);
        activateTestWindow(host);
        host.setFocus(Qt::TabFocusReason);
        QCoreApplication::processEvents();
        verifyPair(line->grab().toImage(), line->palette().color(QPalette::PlaceholderText), line->palette().color(QPalette::Base));
        QToolTip::showText(host.mapToGlobal(QPoint(30, 40)), QStringLiteral("Tooltip HHHHHH"), &host);
        QTRY_VERIFY(QToolTip::isVisible());
        QWidget* tooltip = nullptr;
        for (auto* widget : QApplication::topLevelWidgets())
            if (widget->inherits("QTipLabel") && widget->isVisible()) { tooltip = widget; break; }
        QVERIFY(tooltip);
        verifyPair(tooltip->grab().toImage(), QColor(colors.value(QStringLiteral("@window@"))), QColor(colors.value(QStringLiteral("@text@"))));
    }

    void realTabFocusChangesTheControlRendering_data()
    {
        QTest::addColumn<QString>("styleName");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<QString>("kind");
        for (const auto& style : styles()) for (bool dark : {false, true})
            for (const QString& kind : {QStringLiteral("primary"), QStringLiteral("normal"), QStringLiteral("danger"),
                                       QStringLiteral("tab"), QStringLiteral("tree"), QStringLiteral("list"), QStringLiteral("listview"), QStringLiteral("table"),
                                       QStringLiteral("line"), QStringLiteral("plain"), QStringLiteral("combo"), QStringLiteral("checkbox"),
                                       QStringLiteral("radio"), QStringLiteral("slider")})
                QTest::newRow(qPrintable(style + (dark ? "-dark-" : "-light-") + kind)) << style << dark << kind;
    }

    void realTabFocusChangesTheControlRendering()
    {
        QFETCH(QString, styleName);
        QFETCH(bool, dark);
        QFETCH(QString, kind);
        QVERIFY(applyTheme(styleName, dark));
        const auto previousTabBehavior = QGuiApplication::styleHints()->tabFocusBehavior();
        QGuiApplication::styleHints()->setTabFocusBehavior(Qt::TabFocusAllControls);
        const auto restore = qScopeGuard([&] { QGuiApplication::styleHints()->setTabFocusBehavior(previousTabBehavior); });
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        layout->setSpacing(28);
        auto* anchor = new QLineEdit(QStringLiteral("Before"));
        auto widget = control(kind);
        auto* target = widget.get();
        auto* after = new QLineEdit(QStringLiteral("After"));
        layout->addWidget(anchor);
        layout->addWidget(widget.release());
        layout->addWidget(after);
        QWidget::setTabOrder(anchor, target);
        QWidget::setTabOrder(target, after);
        host.resize(460, 300);
        activateTestWindow(host);
        anchor->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY(anchor->hasFocus());
        QCoreApplication::processEvents();
        const auto capture = [&] {
            const auto image = host.grab().toImage();
            const qreal dpr = image.devicePixelRatio();
            const QRect area = QRect(target->mapTo(&host, QPoint()), target->size()).adjusted(-12, -12, 12, 12);
            return image.copy(QRect(qRound(area.x() * dpr), qRound(area.y() * dpr),
                                    qRound(area.width() * dpr), qRound(area.height() * dpr)));
        };
        const auto before = capture();
        const auto initialSize = target->size();
        QTest::keyClick(anchor, Qt::Key_Tab);
        QTRY_VERIFY(target->hasFocus());
        const auto focused = capture();
        QVERIFY2(changedPixels(before, focused) > 0, qPrintable(kind + QStringLiteral(" has no visible keyboard focus")));
        after->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY(after->hasFocus());
        QCoreApplication::processEvents();
        QCOMPARE(target->size(), initialSize);
        QCOMPARE(capture(), before);
    }

    void switchingThemePreservesInteractiveState_data() { styleRows(); }

    void switchingThemePreservesInteractiveState()
    {
        QFETCH(QString, styleName);
        QFETCH(bool, dark);
        QVERIFY(applyTheme(styleName, dark));
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* tree = new QTreeWidget;
        auto* parent = new QTreeWidgetItem(tree, {QStringLiteral("Parent")});
        auto* child = new QTreeWidgetItem(parent, {QStringLiteral("Selected child")});
        parent->setExpanded(true);
        tree->setCurrentItem(child);
        auto* tabs = new QTabBar;
        tabs->addTab(QStringLiteral("First")); tabs->addTab(QStringLiteral("Second")); tabs->setCurrentIndex(1);
        auto* slider = new QSlider(Qt::Horizontal); slider->setRange(0, 100); slider->setValue(73);
        auto* check = new QCheckBox(QStringLiteral("Enabled")); check->setChecked(true);
        auto* line = new QLineEdit(QStringLiteral("Preserved input"));
        auto* combo = new QComboBox; combo->addItems({QStringLiteral("First"), QStringLiteral("Second")}); combo->setCurrentIndex(1);
        for (auto* widget : QList<QWidget*>{tree, tabs, slider, check, line, combo}) layout->addWidget(widget);
        host.show();
        QSignalSpy values(slider, &QSlider::valueChanged);
        QSignalSpy checks(check, &QCheckBox::toggled);
        QSignalSpy text(line, &QLineEdit::textChanged);
        QSignalSpy choices(combo, &QComboBox::currentIndexChanged);
        QSignalSpy selected(tabs, &QTabBar::currentChanged);
        for (bool next : {!dark, dark, !dark}) {
            qApp->setStyleSheet(Theme::globalStyleSheet(next));
            QCoreApplication::processEvents();
            QCOMPARE(tree->currentItem(), child);
            QCOMPARE(tree->selectedItems(), QList<QTreeWidgetItem*>{child});
            QVERIFY(parent->isExpanded());
            QCOMPARE(tabs->currentIndex(), 1);
            QCOMPARE(slider->value(), 73);
            QVERIFY(check->isChecked());
            QCOMPARE(line->text(), QStringLiteral("Preserved input"));
            QCOMPARE(combo->currentIndex(), 1);
        }
        QVERIFY(values.isEmpty() && checks.isEmpty() && text.isEmpty() && choices.isEmpty() && selected.isEmpty());
    }

    void selectionColorsDoNotReplaceCustomWidgetAccent_data() { styleRows(); }

    void selectionColorsDoNotReplaceCustomWidgetAccent()
    {
        QFETCH(QString, styleName);
        QFETCH(bool, dark);
        QVERIFY(applyTheme(styleName, dark));
        QWidget plain;
        QTreeWidget tree;
        plain.ensurePolished(); tree.ensurePolished();
        QCOMPARE(plain.palette().color(QPalette::Highlight), qApp->palette().color(QPalette::Highlight));
        QVERIFY(plain.palette().color(QPalette::Highlight) != tree.palette().color(QPalette::Highlight));
    }

    void productionGraphsRenderDistinctMarkersAndEventNumbers_data() { styleRows(); }

    void productionGraphsRenderDistinctMarkersAndEventNumbers()
    {
        QFETCH(QString, styleName);
        QFETCH(bool, dark);
        QVERIFY(applyTheme(styleName, dark));
        CMotionTimeline timeline;
        QVERIFY(timeline.SetTiming(30, 1));
        QVERIFY(timeline.SetEvents({QJsonObject{{"Time", 0.5}, {"Value", "first"}},
                                    QJsonObject{{"Time", 0.5}, {"Value", "second"}}}));
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* xy = new CXYParameterPad;
        xy->SetValues(0.5f, -0.25f);
        auto* physics = new CPhysicsGraphWidget;
        physics->SetGroup(QJsonObject{{"Input", QJsonArray{QJsonObject{{"Type", "Angle"}}}},
                                     {"Output", QJsonArray{QJsonObject{{"Type", "Angle"}}}}});
        auto* curve = new CMotionCurveView;
        curve->SetTimeline(&timeline);
        for (auto* widget : QList<QWidget*>{xy, physics, curve}) layout->addWidget(widget);
        host.resize(460, 600);
        host.show();
        QCoreApplication::processEvents();
        for (auto* widget : QList<QWidget*>{xy, physics, curve})
            verifyPair(widget->grab().toImage(), widget->palette().color(QPalette::Highlight), widget->palette().color(QPalette::Base));
        const auto image = curve->grab().toImage();
        const auto accent = curve->palette().color(QPalette::Highlight);
        const auto number = curve->palette().color(QPalette::HighlightedText);
        QVERIFY(contrast(number, accent) >= 4.5);
        QRect marker;
        const int laneBottom = qRound(40 * image.devicePixelRatio());
        for (int y = 0; y < std::min(image.height(), laneBottom); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (image.pixelColor(x, y) == accent) marker = marker.united(QRect(x, y, 1, 1));
        QVERIFY(!marker.isEmpty());
        QVERIFY(timeline.SetEvents({QJsonObject{{"Time", 0.5}, {"Value", "first"}},
                                    QJsonObject{{"Time", 0.5}, {"Value", "second"}},
                                    QJsonObject{{"Time", 0.5}, {"Value", "third"}}}));
        curve->SetTimeline(&timeline);
        const auto updated = curve->grab().toImage();
        const int inset = qRound(2 * image.devicePixelRatio());
        const auto interior = marker.adjusted(inset, inset, -inset, -inset);
        QVERIFY(!interior.isEmpty());
        QVERIFY2(changedPixels(image.copy(interior), updated.copy(interior)) > 0,
                 "Event cluster count does not change when its count changes from two to three");
    }
};

QTEST_MAIN(ThemeTests)
#include "theme_tests.moc"
