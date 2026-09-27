#include "ui/ctexturepanel.h"
#include "ui/theme.h"
#include "std/cpngimage.h"
#include <QtTest>
#include <QBuffer>
#include <QAction>
#include <QFileDialog>
#include <QGraphicsPixmapItem>
#include <QPointer>
#include <QScrollBar>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QWheelEvent>
#include <QMenu>
#include <QMainWindow>
#include <filesystem>
#include <stdexcept>

using Assets = live2dstruct::CTextureAssets;

namespace {
std::filesystem::path nativePath(const QString& path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).toStdString());
#endif
}

QByteArray png(QColor color, QSize size = QSize(32, 24))
{
    QImage image(size, QImage::Format_RGBA8888);
    image.fill(color);
    image.setText(QStringLiteral("snapshot"), QStringLiteral("literal <tag> 精确保留"));
    QBuffer output;
    if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG")) return {};
    return output.data();
}
bool write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
Assets::Catalog catalog(const QString& directory)
{
    Assets::Source first, second;
    if (!Assets::MakeSource(directory + "/原始<tag>.png", png(Qt::red), first) ||
        !Assets::MakeSource(directory + "/另一图集.png", png(Qt::green), second)) return {};
    return {{2, first}, {9, first}, {12, second}};
}
QImage displayed(CTexturePanel& panel)
{
    for (auto* item : panel.findChild<CAtlasView*>()->scene()->items())
        if (auto* image = qgraphicsitem_cast<QGraphicsPixmapItem*>(item)) return image->pixmap().toImage();
    return {};
}
struct ViewState
{
    QGraphicsScene* scene;
    QList<QGraphicsItem*> items;
    QTransform transform;
    int horizontal, vertical, slot;
};
ViewState capture(CTexturePanel& panel)
{
    auto* view = panel.findChild<CAtlasView*>();
    return {view->scene(), view->scene()->items(), view->transform(), view->horizontalScrollBar()->value(),
            view->verticalScrollBar()->value(), panel.CurrentSlot()};
}
void unchanged(CTexturePanel& panel, const ViewState& before)
{
    const auto current = capture(panel);
    QCOMPARE(current.scene, before.scene); QCOMPARE(current.items, before.items);
    QCOMPARE(current.transform, before.transform); QCOMPARE(current.horizontal, before.horizontal);
    QCOMPARE(current.vertical, before.vertical); QCOMPARE(current.slot, before.slot);
}
bool chooseFile(CTexturePanel* panel, const char* method, const QString& path, bool cancel = false,
                std::function<void()> inside = {}, bool click = false)
{
    bool opened = false;
    QTimer selector;
    selector.setSingleShot(true);
    QObject::connect(&selector, &QTimer::timeout, [&] {
        const QPointer<QFileDialog> dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        opened = true;
        if (inside) inside();
        if (!dialog) return;
        if (cancel) dialog->reject();
        else {
            dialog->setOption(QFileDialog::DontConfirmOverwrite, true);
            dialog->selectFile(path); QMetaObject::invokeMethod(dialog, "accept");
        }
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, [] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();
    });
    selector.start(0); timeout.start(3000);
    if (click) {
        const auto name = QByteArray(method) == "onReplacePng" ? "textureReplace" : "textureExport";
        if (auto* button = panel->findChild<QToolButton*>(name)) QTest::mouseClick(button, Qt::LeftButton);
    } else QMetaObject::invokeMethod(panel, method);
    return opened;
}
void editHandler(CTexturePanel& panel, const Assets::Catalog& sources, Assets::State& state, int& accepted)
{
    panel.SetEditHandler([&panel, sources, &state, &accepted](int slot, const QByteArray& bytes, QString* error) {
        Assets::State next;
        if (!Assets::Replace(sources, state, slot, bytes, next, error) || !panel.LoadTextureSnapshot(sources, next, error)) return false;
        state.swap(next); ++accepted;
        return true;
    });
    panel.SetEditingEnabled(true);
}
}

class TextureReplacementPanelTests : public QObject
{
    Q_OBJECT
private slots:
    void firstOpeningABottomDockReservesAUsableAtlasArea()
    {
        QTemporaryDir directory; Assets::Source source;
        QVERIFY(Assets::MakeSource(directory.filePath("large.png"), png(Qt::red, QSize(2048, 2048)), source));
        QMainWindow host;
        host.setCentralWidget(new QLabel(QStringLiteral("模型画布"), &host));
        auto* compact = new QDockWidget(QStringLiteral("其他面板"), &host);
        compact->setWidget(new QLabel(QStringLiteral("暂无内容"), compact));
        host.addDockWidget(Qt::BottomDockWidgetArea, compact);
        auto* panel = new CTexturePanel(&host);
        QVERIFY(panel->LoadTextureSnapshot({{0, source}}, {}));
        host.addDockWidget(Qt::BottomDockWidgetArea, panel); host.tabifyDockWidget(compact, panel); panel->hide();
        host.resize(900, 700); host.show(); QTest::qWait(1);
        panel->show(); panel->raise();
        auto* view = panel->findChild<CAtlasView*>();
        QTRY_VERIFY(view->viewport()->height() >= 8 * view->fontMetrics().height());
        QTRY_VERIFY(view->transform().m11() > .05);
        QCOMPARE(host.size(), QSize(900, 700));
        QVERIFY(host.centralWidget()->height() > 0);
    }

    void hiddenSnapshotFitsAfterTheViewportHasLayout()
    {
        QTemporaryDir directory; Assets::Source source;
        QVERIFY(Assets::MakeSource(directory.filePath("large.png"), png(Qt::red, QSize(768, 512)), source));
        CTexturePanel panel; QVERIFY(panel.LoadTextureSnapshot({{0, source}}, {}));
        auto* view = panel.findChild<CAtlasView*>();
        panel.resize(620, 420); panel.show();
        const auto fit = [&] {
            const auto size = view->transform().mapRect(view->scene()->sceneRect()).size();
            const auto viewport = view->viewport()->size();
            return size.width() <= viewport.width() && size.height() <= viewport.height() &&
                std::max(size.width() / viewport.width(), size.height() / viewport.height()) > .85;
        };
        QTRY_VERIFY(fit());
        QCOMPARE(view->ZoomFactor(), view->transform().m11());
    }

    void userZoomCancelsPendingFitAndSurvivesHideShow_data()
    {
        QTest::addColumn<bool>("wheel"); QTest::newRow("reset") << false; QTest::newRow("wheel") << true;
    }
    void userZoomCancelsPendingFitAndSurvivesHideShow()
    {
        QFETCH(bool, wheel);
        QTemporaryDir directory; CTexturePanel panel; QVERIFY(panel.LoadTextureSnapshot(catalog(directory.path()), {}));
        auto* view = panel.findChild<CAtlasView*>();
        if (wheel) {
            QWheelEvent event(QPointF(10, 10), QPointF(10, 10), {}, QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(view->viewport(), &event);
        } else view->ZoomReset();
        const auto transform = view->transform();
        panel.resize(620, 420); panel.show(); QTest::qWait(10);
        QCOMPARE(view->transform(), transform);
        panel.hide(); panel.resize(460, 340); panel.show(); QTest::qWait(10);
        QCOMPARE(view->transform(), transform);
    }

    void destructionBeforeQueuedFitCancelsTheCallback()
    {
        QTemporaryDir directory; QPointer<CTexturePanel> panel = new CTexturePanel;
        QVERIFY(panel->LoadTextureSnapshot(catalog(directory.path()), {}));
        QSignalSpy zoom(panel->findChild<CAtlasView*>(), &CAtlasView::ZoomChanged);
        panel->resize(620, 420); panel->show(); delete panel.data();
        QCoreApplication::processEvents(); QVERIFY(panel.isNull()); QCOMPARE(zoom.size(), 0);
    }

    void snapshotSurvivesDiskChangeSwitchingAndExactExport()
    {
        QTemporaryDir directory; const auto sources = catalog(directory.path()); QVERIFY(sources.size() == 3);
        for (const auto& source : sources) QVERIFY(write(source.path, source.png));
        CTexturePanel panel; QString error;
        QVERIFY(panel.LoadTextureSnapshot(sources, {}, &error)); QCOMPARE(panel.CurrentSlot(), 2);
        QVERIFY(write(sources[2].path, png(Qt::blue, QSize(64, 64))));
        auto* selector = panel.findChild<QComboBox*>(); selector->setCurrentIndex(selector->findData(12));
        selector->setCurrentIndex(selector->findData(2));
        QCOMPARE(displayed(panel).size(), QSize(32, 24)); QCOMPARE(displayed(panel).pixelColor(0, 0), QColor(Qt::red));
        const auto target = directory.filePath("export.png");
        QVERIFY(chooseFile(&panel, "onExportPng", target)); QCOMPARE(read(target), sources[2].png);
        QVERIFY(QFile::remove(sources[2].path));
        selector->setCurrentIndex(selector->findData(12)); selector->setCurrentIndex(selector->findData(9));
        QCOMPARE(displayed(panel).pixelColor(0, 0), QColor(Qt::red));
        QVERIFY(chooseFile(&panel, "onExportPng", target)); QCOMPARE(read(target), sources[9].png);
        auto* information = panel.findChild<QLabel*>("texInfoLabel"); QVERIFY(information);
        QCOMPARE(information->textFormat(), Qt::PlainText);
        QCOMPARE(selector->itemData(selector->findData(2), Qt::ToolTipRole).toString(),
                 QStringLiteral("<qt>%1</qt>").arg(sources[2].path.toHtmlEscaped()));
    }

    void invalidSnapshotsKeepAllPreviousState_data()
    {
        QTest::addColumn<int>("kind");
        for (int kind = 0; kind < 8; ++kind) QTest::newRow(qPrintable(QString::number(kind))) << kind;
    }
    void invalidSnapshotsKeepAllPreviousState()
    {
        QFETCH(int, kind);
        QTemporaryDir directory; auto sources = catalog(directory.path());
        CTexturePanel panel; QVERIFY(panel.LoadTextureSnapshot(sources, {}));
        panel.resize(260, 300); panel.show(); QTest::qWait(1);
        panel.findChild<CAtlasView*>()->setTransform(QTransform::fromScale(4, 4));
        const auto before = capture(panel);
        Assets::State state;
        if (kind == 0) sources[2].digest = QByteArray(32, 'x');
        if (kind == 1) sources[2].size = QSize(64, 24);
        if (kind == 2) state.insert(99, png(Qt::blue));
        if (kind == 3) state.insert(2, png(Qt::blue).left(40));
        if (kind == 4) state.insert(2, png(Qt::blue, QSize(31, 24)));
        if (kind == 5) state.insert(2, sources[2].png);
        if (kind == 6) sources[2].path = "relative.png";
        if (kind == 7) { sources.insert(-1, sources[2]); }
        QString error; QVERIFY(!panel.LoadTextureSnapshot(sources, state, &error)); QVERIFY(!error.isEmpty());
        unchanged(panel, before); QCOMPARE(displayed(panel).pixelColor(0, 0), QColor(Qt::red));
    }

    void replacementRestoreAndExternalHistoryRefreshRetainViewAndSlotIsolation()
    {
        QTemporaryDir directory; const auto sources = catalog(directory.path());
        CTexturePanel panel; QVERIFY(panel.LoadTextureSnapshot(sources, {}));
        Assets::State state; int accepted = 0; editHandler(panel, sources, state, accepted);
        panel.resize(500, 340); panel.show(); QTest::qWait(1);
        auto* view = panel.findChild<CAtlasView*>(); view->ZoomReset(); view->setTransform(QTransform::fromScale(3, 3));
        const auto transform = view->transform();
        const auto replacement = png(QColor(33, 99, 177, 128)); const auto path = directory.filePath("replacement.png");
        QVERIFY(write(path, replacement));
        QSignalSpy history(&panel, &CTexturePanel::HistorySelected);
        QVERIFY(chooseFile(&panel, "onReplacePng", path));
        QVERIFY2(accepted == 1, qPrintable(panel.findChild<QLabel*>("textureError")->text()));
        QCOMPARE(state.value(2), replacement);
        QCOMPARE(history.size(), 1); QCOMPARE(view->transform(), transform); QCOMPARE(panel.CurrentSlot(), 2);
        QCOMPARE(displayed(panel).pixelColor(0, 0).alpha(), 128);
        auto* selector = panel.findChild<QComboBox*>(); selector->setCurrentIndex(selector->findData(9));
        QCOMPARE(displayed(panel).pixelColor(0, 0), QColor(Qt::red));
        selector->setCurrentIndex(selector->findData(2));
        const auto replaced = state;
        QVERIFY(panel.LoadTextureSnapshot(sources, {})); QCOMPARE(displayed(panel).pixelColor(0, 0), QColor(Qt::red));
        QVERIFY(panel.LoadTextureSnapshot(sources, replaced)); QCOMPARE(panel.CurrentSlot(), 2); QCOMPARE(history.size(), 1);
        QVERIFY(QMetaObject::invokeMethod(&panel, "onRestoreOriginal")); QCOMPARE(accepted, 2); QVERIFY(state.isEmpty());
        QCOMPARE(displayed(panel).pixelColor(0, 0), QColor(Qt::red)); QCOMPARE(history.size(), 2);
        QVERIFY(QMetaObject::invokeMethod(&panel, "onRestoreOriginal")); QCOMPARE(accepted, 2); QCOMPARE(history.size(), 2);
        QVERIFY(panel.LoadTextureSnapshot(sources, replaced));
        const auto output = directory.filePath("accepted.png"); QVERIFY(chooseFile(&panel, "onExportPng", output));
        QCOMPARE(read(output), replacement);
    }

    void rejectedCanceledAndExceptionalEditsKeepState_data()
    {
        QTest::addColumn<int>("fault");
        for (int fault = 0; fault < 6; ++fault) QTest::newRow(qPrintable(QString::number(fault))) << fault;
    }
    void rejectedCanceledAndExceptionalEditsKeepState()
    {
        QFETCH(int, fault);
        QTemporaryDir directory; const auto sources = catalog(directory.path());
        CTexturePanel panel; QVERIFY(panel.LoadTextureSnapshot(sources, {}));
        int attempts = 0;
        panel.SetEditHandler([&](int, const QByteArray&, QString* error) {
            ++attempts; if (fault == 3) throw std::runtime_error("rejected");
            if (error) *error = QStringLiteral("literal <b>拒绝</b>"); return false;
        });
        panel.SetEditingEnabled(true);
        const auto path = directory.filePath("replacement.png");
        QVERIFY(write(path, fault == 0 ? png(Qt::blue, QSize(31, 24)) : fault == 1 ? QByteArray("bad") : png(Qt::blue)));
        const auto before = capture(panel); QSignalSpy history(&panel, &CTexturePanel::HistorySelected);
        QSignalSpy rejected(&panel, &CTexturePanel::EditRejected);
        QVERIFY(chooseFile(&panel, "onReplacePng", path, fault == 4, fault == 5 ? std::function<void()>([&] {
            panel.SetEditingEnabled(false, QStringLiteral("正在导出"));
        }) : std::function<void()>{}));
        QCOMPARE(attempts, int(fault == 2 || fault == 3)); unchanged(panel, before); QCOMPARE(history.size(), 0);
        QCOMPARE(rejected.size(), int(fault != 4));
        if (fault == 0) { QVERIFY2(rejected.first()[0].toString().contains("32"), qPrintable(rejected.first()[0].toString())); QVERIFY(rejected.first()[0].toString().contains("31")); }
        if (fault == 2) QCOMPARE(panel.findChild<QLabel*>("textureError")->text(), QStringLiteral("literal <b>拒绝</b>"));
        QCOMPARE(panel.findChild<QLabel*>("textureError")->textFormat(), Qt::PlainText);
        panel.SetEditingEnabled(false, QStringLiteral("<b>暂停</b>"));
        QCOMPARE(panel.findChild<QToolButton*>("textureReplace")->defaultAction()->toolTip(),
                 QStringLiteral("<qt>&lt;b&gt;暂停&lt;/b&gt;</qt>"));
    }

    void callbackRefreshIsAllowedButNestedEditAndHandlerReplacementAreBlocked()
    {
        QTemporaryDir directory; const auto sources = catalog(directory.path());
        CTexturePanel panel; QVERIFY(panel.LoadTextureSnapshot(sources, {})); panel.SetEditingEnabled(true);
        Assets::State state; int calls = 0, nested = 0;
        panel.SetEditHandler([&](int slot, const QByteArray& bytes, QString* error) {
            ++calls;
            panel.SetEditHandler([&](int, const QByteArray&, QString*) { ++nested; return false; });
            QMetaObject::invokeMethod(&panel, "onReplacePng"); QMetaObject::invokeMethod(&panel, "onRestoreOriginal");
            Assets::State next;
            if (!Assets::Replace(sources, state, slot, bytes, next, error) || !panel.LoadTextureSnapshot(sources, next, error)) return false;
            state.swap(next); return true;
        });
        const auto path = directory.filePath("replacement.png"); QVERIFY(write(path, png(Qt::blue)));
        QVERIFY(chooseFile(&panel, "onReplacePng", path)); QCOMPARE(calls, 1); QCOMPARE(nested, 0);
        QVERIFY(QMetaObject::invokeMethod(&panel, "onRestoreOriginal")); QCOMPARE(calls, 2); QCOMPARE(nested, 0);
        QCOMPARE(displayed(panel).pixelColor(0, 0), QColor(Qt::red));
    }

    void snapshotChangeWhilePickingRejectsTheStaleSlot()
    {
        QTemporaryDir directory; auto sources = catalog(directory.path());
        CTexturePanel panel; QVERIFY(panel.LoadTextureSnapshot(sources, {}));
        int calls = 0; panel.SetEditHandler([&](int, const QByteArray&, QString*) { ++calls; return true; }); panel.SetEditingEnabled(true);
        const auto path = directory.filePath("replacement.png"); QVERIFY(write(path, png(Qt::blue)));
        auto next = sources; next[2].path = directory.filePath("new-source.png"); next[9].path = next[2].path;
        QSignalSpy rejected(&panel, &CTexturePanel::EditRejected);
        QVERIFY(chooseFile(&panel, "onReplacePng", path, false, [&] { QVERIFY(panel.LoadTextureSnapshot(next, {})); }));
        QCOMPARE(calls, 0); QCOMPARE(rejected.size(), 1); QVERIFY(rejected.first()[0].toString().contains(QStringLiteral("已改变")));
        QCOMPARE(displayed(panel).pixelColor(0, 0), QColor(Qt::red));
    }

    void handlerChangeDuringFileChoiceInvalidatesTheOldRequest()
    {
        QTemporaryDir directory; const auto sources = catalog(directory.path());
        CTexturePanel panel; QVERIFY(panel.LoadTextureSnapshot(sources, {})); panel.SetEditingEnabled(true);
        int previous = 0, current = 0;
        panel.SetEditHandler([&](int, const QByteArray&, QString*) { ++previous; return false; });
        const auto path = directory.filePath("replacement.png"); QVERIFY(write(path, png(Qt::blue)));
        const auto before = capture(panel); QSignalSpy rejected(&panel, &CTexturePanel::EditRejected);
        QVERIFY(chooseFile(&panel, "onReplacePng", path, false, [&] {
            panel.SetEditHandler([&](int, const QByteArray&, QString* error) {
                ++current; return panel.LoadTextureSnapshot(sources, {{2, png(Qt::blue)}}, error);
            });
        }));
        QCOMPARE(previous, 0); QCOMPARE(current, 0); QCOMPARE(rejected.size(), 1); unchanged(panel, before);
        QVERIFY(chooseFile(&panel, "onReplacePng", path)); QCOMPARE(previous, 0); QCOMPARE(current, 1);
    }

    void visibleButtonsUseTheEditHandlerAndRestoreTheSource()
    {
        QTemporaryDir directory; const auto sources = catalog(directory.path());
        CTexturePanel panel; QVERIFY(panel.LoadTextureSnapshot(sources, {}));
        Assets::State state; int accepted = 0; editHandler(panel, sources, state, accepted);
        panel.resize(1000, 380); panel.show(); QTest::qWait(1);
        auto* replace = panel.findChild<QToolButton*>("textureReplace");
        auto* restore = panel.findChild<QToolButton*>("textureRestore");
        QVERIFY(replace && restore && replace->isVisible()); QVERIFY(!restore->isEnabled());
        const auto path = directory.filePath("replacement.png"); QVERIFY(write(path, png(Qt::blue)));
        QVERIFY(chooseFile(&panel, "onReplacePng", path, false, {}, true)); QCOMPARE(accepted, 1);
        QVERIFY(restore->isEnabled() && restore->isVisible());
        QTest::mouseClick(restore, Qt::LeftButton); QCOMPARE(accepted, 2); QVERIFY(state.isEmpty());
        QCOMPARE(displayed(panel).pixelColor(0, 0), QColor(Qt::red));
    }

    void deletingThePanelInsidePublisherDoesNotUseItsOldChildren()
    {
        QTemporaryDir directory; const auto sources = catalog(directory.path());
        QPointer<CTexturePanel> panel = new CTexturePanel;
        QVERIFY(panel->LoadTextureSnapshot(sources, {})); panel->SetEditingEnabled(true);
        panel->SetEditHandler([&](int, const QByteArray&, QString*) { delete panel.data(); return true; });
        const auto path = directory.filePath("replacement.png"); QVERIFY(write(path, png(Qt::blue)));
        QVERIFY(chooseFile(panel, "onReplacePng", path)); QVERIFY(panel.isNull());
    }

    void deletingThePanelWhileChoosingAFileIsSafe_data()
    {
        QTest::addColumn<QByteArray>("method");
        QTest::newRow("replace") << QByteArray("onReplacePng");
        QTest::newRow("export") << QByteArray("onExportPng");
    }
    void deletingThePanelWhileChoosingAFileIsSafe()
    {
        QFETCH(QByteArray, method);
        QTemporaryDir directory; const auto sources = catalog(directory.path());
        QPointer<CTexturePanel> panel = new CTexturePanel;
        QVERIFY(panel->LoadTextureSnapshot(sources, {})); panel->SetEditingEnabled(true);
        int calls = 0; panel->SetEditHandler([&](int, const QByteArray&, QString*) { ++calls; return true; });
        QVERIFY(chooseFile(panel, method.constData(), {}, true, [&] { delete panel.data(); }));
        QVERIFY(panel.isNull()); QCOMPARE(calls, 0);
    }

    void exportProtectsAllSourcePathsAndAliases_data()
    {
        QTest::addColumn<int>("alias");
        QTest::newRow("current-source") << 0; QTest::newRow("other-slot-source") << 1;
        QTest::newRow("symlink") << 2; QTest::newRow("hardlink") << 3;
    }
    void exportProtectsAllSourcePathsAndAliases()
    {
        QFETCH(int, alias);
        QTemporaryDir directory; const auto sources = catalog(directory.path());
        for (const auto& source : sources) QVERIFY(write(source.path, source.png));
        CTexturePanel panel; QVERIFY(panel.LoadTextureSnapshot(sources, {{2, png(Qt::blue)}}));
        QString path = alias == 1 ? sources[12].path : sources[2].path;
        if (alias >= 2) {
            path = directory.filePath("alias.png");
            std::error_code error;
            if (alias == 2) std::filesystem::create_symlink(nativePath(sources[2].path), nativePath(path), error);
            else std::filesystem::create_hard_link(nativePath(sources[2].path), nativePath(path), error);
#ifdef Q_OS_WIN
            if (alias == 2 && error == std::errc::permission_denied) QSKIP("The current Windows account cannot create symbolic links.");
#endif
            QVERIFY2(!error, error.message().c_str());
        }
        QSignalSpy rejected(&panel, &CTexturePanel::EditRejected);
        QVERIFY(chooseFile(&panel, "onExportPng", path)); QCOMPARE(rejected.size(), 1);
        QCOMPARE(read(sources[2].path), sources[2].png); QCOMPARE(read(sources[12].path), sources[12].png);
    }

    void narrowToolbarKeepsActionsReachableAndFocusSelectsHistory_data()
    {
        QTest::addColumn<bool>("dark"); QTest::newRow("light") << false; QTest::newRow("dark") << true;
    }
    void narrowToolbarKeepsActionsReachableAndFocusSelectsHistory()
    {
        QFETCH(bool, dark);
        const auto previous = qApp->styleSheet(); qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        const auto restore = qScopeGuard([&] { qApp->setStyleSheet(previous); });
        QTemporaryDir directory; const auto sources = catalog(directory.path());
        CTexturePanel panel; QVERIFY(panel.LoadTextureSnapshot(sources, {}));
        Assets::State state; int accepted = 0; editHandler(panel, sources, state, accepted);
        panel.resize(260, 300); panel.show(); QTest::qWait(1);
        QVERIFY(panel.minimumSizeHint().width() <= 260);
        auto* toolbar = panel.findChild<QToolBar*>("textureActions"); QVERIFY(toolbar);
        auto* overflow = toolbar->findChild<QToolButton*>("qt_toolbar_ext_button"); QVERIFY(overflow && overflow->isVisible());
        QCOMPARE(overflow->focusPolicy(), Qt::StrongFocus);
        bool menuSeen = false;
        QTimer closeMenu; closeMenu.setSingleShot(true);
        connect(&closeMenu, &QTimer::timeout, [&] { auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()); menuSeen = menu && !menu->actions().isEmpty(); if (menu) menu->close(); });
        closeMenu.start(0); QTest::mouseClick(overflow, Qt::LeftButton); QVERIFY(menuSeen);
        panel.activateWindow(); QTRY_VERIFY(panel.isActiveWindow());
        auto* view = panel.findChild<CAtlasView*>(); view->setFocus(Qt::TabFocusReason); QTRY_VERIFY(view->hasFocus());
        QSignalSpy history(&panel, &CTexturePanel::HistorySelected);
        auto* selector = panel.findChild<QComboBox*>(); selector->setFocus(Qt::TabFocusReason); QTRY_VERIFY(selector->hasFocus()); QVERIFY(history.size() >= 1);
        history.clear(); QVERIFY(panel.LoadTextureSnapshot(sources, {{2, png(Qt::blue)}})); QCOMPARE(history.size(), 0);
        QCOMPARE(accepted, 0);
    }
};

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application(argc, argv);
    TextureReplacementPanelTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "texture_replacement_panel_tests.moc"
