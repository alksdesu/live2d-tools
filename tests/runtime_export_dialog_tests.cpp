#include "ui/cruntimeexportdialog.h"
#include "ui/theme.h"
#include <QtTest>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QScopeGuard>
#include <limits>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyleFactory>
#include <QTemporaryDir>

namespace {
template<class T> T* control(CRuntimeExportDialog& dialog, const char* name)
{
    return dialog.findChild<T*>(QLatin1String(name));
}
QPushButton* button(CRuntimeExportDialog& dialog, QDialogButtonBox::StandardButton which)
{
    return control<QDialogButtonBox>(dialog, "runtimeExportButtons")->button(which);
}
bool show(CRuntimeExportDialog& dialog)
{
    dialog.show();
    dialog.activateWindow();
    return QTest::qWaitForWindowExposed(&dialog);
}
}

class RuntimeExportDialogTests : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qApp->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    }
    void cleanup() { qApp->setStyleSheet({}); }

    void defaultsAreExplicitAndTimelineRequiresTracks_data()
    {
        QTest::addColumn<bool>("tracks");
        QTest::newRow("without-tracks") << false;
        QTest::newRow("with-tracks") << true;
    }
    void defaultsAreExplicitAndTimelineRequiresTracks()
    {
        QFETCH(bool, tracks);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        CRuntimeExportDialog dialog(directory.filePath("model.model3.json"), tracks);
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QVERIFY(!dialog.UseCurrentValues());
        QVERIFY(!dialog.IncludeTimeline());
        QVERIFY(!dialog.IncludeArtwork());
        auto* artwork = control<QCheckBox>(dialog, "runtimeExportArtwork"); QVERIFY(artwork && artwork->isHidden() && !artwork->isEnabled());
        auto* timeline = control<QCheckBox>(dialog, "runtimeExportTimeline");
        auto* current = control<QCheckBox>(dialog, "runtimeExportCurrentValues");
        QVERIFY(timeline && current);
        QCOMPARE(timeline->isEnabled(), tracks);
        timeline->setChecked(true);
        QCOMPARE(dialog.IncludeTimeline(), tracks);
        current->setChecked(true);
        QVERIFY(dialog.UseCurrentValues());
        QVERIFY(dialog.TargetDirectory().endsWith(QStringLiteral("model-runtime")));
        QVERIFY(QDir(directory.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }

    void artworkSummaryAndChoiceDoNotChangeOtherOptions_data()
    { QTest::addColumn<bool>("tracks"); QTest::newRow("no-tracks") << false; QTest::newRow("with-tracks") << true; }
    void artworkSummaryAndChoiceDoNotChangeOtherOptions()
    {
        QFETCH(bool, tracks); const auto previous = QLocale(); QLocale::setDefault(QLocale::c()); const auto restore = qScopeGuard([previous] { QLocale::setDefault(previous); });
        QTemporaryDir directory; CRuntimeExportDialog dialog(directory.filePath("missing-model.model3.json"), tracks);
        dialog.SetInitialDirectory(directory.filePath("new-model")); dialog.SetArtworkSummary(3, 64LL * 1024 * 1024);
        auto* artwork = control<QCheckBox>(dialog, "runtimeExportArtwork"); auto* summary = control<QLabel>(dialog, "runtimeExportArtworkSummary"); auto* notice = control<QLabel>(dialog, "runtimeExportArtworkNotice");
        QVERIFY(artwork->isEnabled() && !artwork->isHidden()); QVERIFY(!artwork->isChecked() && !dialog.IncludeArtwork());
        QVERIFY(summary->text().contains("3")); QVERIFY(summary->text().contains("64.0 MB")); QVERIFY(summary->text().contains("旧版本") && summary->text().contains("未关联"));
        QCOMPARE(summary->textFormat(), Qt::PlainText); QCOMPARE(notice->textFormat(), Qt::PlainText); QVERIFY(notice->text().contains("只导出可运行模型"));
        auto* current = control<QCheckBox>(dialog, "runtimeExportCurrentValues"); auto* timeline = control<QCheckBox>(dialog, "runtimeExportTimeline");
        current->setChecked(true); timeline->setChecked(true); QSignalSpy currentChanges(current, &QCheckBox::toggled), timelineChanges(timeline, &QCheckBox::toggled);
        artwork->setChecked(true); QVERIFY(dialog.IncludeArtwork()); QVERIFY(notice->text().contains("模型包将保存来源版本与关联")); QVERIFY(!notice->text().contains("本次只导出"));
        QVERIFY(dialog.UseCurrentValues()); QCOMPARE(dialog.IncludeTimeline(), tracks); QVERIFY(currentChanges.isEmpty() && timelineChanges.isEmpty());
        dialog.SetArtworkSummary(4, 96LL * 1024 * 1024); QVERIFY(dialog.IncludeArtwork()); QVERIFY(summary->text().contains("96.0 MB"));
        artwork->setChecked(false); QVERIFY(!dialog.IncludeArtwork()); QVERIFY(notice->text().contains("另存工作会话"));
        artwork->setChecked(true); dialog.accept(); QCOMPARE(dialog.result(), int(QDialog::Accepted)); QVERIFY(dialog.IncludeArtwork());
        QVERIFY(QDir(directory.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }

    void absentOrInvalidArtworkClearsOnlyItsChoice_data()
    {
        QTest::addColumn<int>("count"); QTest::addColumn<qint64>("bytes");
        QTest::newRow("empty") << 0 << qint64(0); QTest::newRow("no-revisions") << 0 << qint64(100);
        QTest::newRow("invalid-count") << -1 << qint64(100); QTest::newRow("invalid-bytes") << 2 << qint64(-1);
    }
    void absentOrInvalidArtworkClearsOnlyItsChoice()
    {
        QFETCH(int, count); QFETCH(qint64, bytes); CRuntimeExportDialog dialog("/missing/model.model3.json", true);
        dialog.SetArtworkSummary(2, 1024); auto* artwork = control<QCheckBox>(dialog, "runtimeExportArtwork"); artwork->setChecked(true);
        control<QCheckBox>(dialog, "runtimeExportCurrentValues")->setChecked(true); control<QCheckBox>(dialog, "runtimeExportTimeline")->setChecked(true);
        dialog.SetArtworkSummary(count, bytes); QVERIFY(!dialog.IncludeArtwork()); QVERIFY(artwork->isHidden() && !artwork->isEnabled() && !artwork->isChecked());
        QVERIFY(control<QLabel>(dialog, "runtimeExportArtworkSummary")->isHidden()); QVERIFY(control<QLabel>(dialog, "runtimeExportArtworkNotice")->isHidden());
        QVERIFY(dialog.UseCurrentValues()); QVERIFY(dialog.IncludeTimeline()); dialog.SetArtworkSummary(2, 2048); QVERIFY(!dialog.IncludeArtwork());
    }

    void largeArtworkSummaryNeedsNoSourceFilesOrDecodedImages()
    {
        CRuntimeExportDialog dialog("/does-not-exist/model.model3.json", false); dialog.SetArtworkSummary(100000, std::numeric_limits<qint64>::max());
        auto* summary = control<QLabel>(dialog, "runtimeExportArtworkSummary"); QVERIFY(!summary->isHidden()); QVERIFY(!summary->text().contains(QLatin1Char('-')));
        QVERIFY(control<QCheckBox>(dialog, "runtimeExportArtwork")->isEnabled()); QVERIFY(!dialog.IncludeArtwork());
        dialog.SetArtworkSummary(1, 0); QVERIFY(!summary->isHidden()); QVERIFY(!summary->text().isEmpty());
    }

    void legacyNoticeDoesNotPretendAnAttachmentWasProvided()
    {
        CRuntimeExportDialog dialog("/missing/model.model3.json", false); auto* artwork = control<QCheckBox>(dialog, "runtimeExportArtwork");
        dialog.SetArtworkNotice(true); QVERIFY(!control<QLabel>(dialog, "runtimeExportArtworkNotice")->isHidden()); QVERIFY(artwork->isHidden() && !artwork->isEnabled());
        artwork->setChecked(true); QVERIFY(!dialog.IncludeArtwork()); dialog.SetArtworkNotice(false); QVERIFY(control<QLabel>(dialog, "runtimeExportArtworkNotice")->isHidden());
        dialog.SetArtworkSummary(1, 100); QVERIFY(!dialog.IncludeArtwork());
    }

    void keyboardCanChooseArtworkWithoutAnExtraConfirmation()
    {
        QTemporaryDir directory; CRuntimeExportDialog dialog(directory.filePath("model.model3.json"), true); dialog.SetInitialDirectory(directory.filePath("new-model")); dialog.SetArtworkSummary(2, 2048);
        QVERIFY(show(dialog)); auto* timeline = control<QCheckBox>(dialog, "runtimeExportTimeline"); auto* artwork = control<QCheckBox>(dialog, "runtimeExportArtwork");
        timeline->setFocus(); QTRY_VERIFY(timeline->hasFocus()); QTest::keyClick(timeline, Qt::Key_Tab); QTRY_VERIFY(artwork->hasFocus());
        QTest::keyClick(artwork, Qt::Key_Space); QVERIFY(dialog.IncludeArtwork()); QTest::keyClick(artwork, Qt::Key_Tab); QTRY_VERIFY(button(dialog, QDialogButtonBox::Ok)->hasFocus());
        QSignalSpy accepted(&dialog, &QDialog::accepted); QTest::keyClick(button(dialog, QDialogButtonBox::Ok), Qt::Key_Return); QCOMPARE(accepted.size(), 1); QVERIFY(dialog.IncludeArtwork());
        QVERIFY(!QFileInfo::exists(dialog.TargetDirectory()));
    }

    void artworkOptionsWrapAndRemainReachable_data()
    {
        QTest::addColumn<bool>("dark"); QTest::addColumn<int>("width");
        for (bool dark : {false, true}) for (int width : {360, 580}) QTest::newRow(qPrintable(QString("%1-%2").arg(dark).arg(width))) << dark << width;
    }
    void artworkOptionsWrapAndRemainReachable()
    {
        QFETCH(bool, dark); QFETCH(int, width); qApp->setStyleSheet(Theme::globalStyleSheet(dark)); QTemporaryDir directory;
        CRuntimeExportDialog dialog(directory.filePath("model.model3.json"), true); dialog.SetInitialDirectory(directory.filePath("new-model")); dialog.SetArtworkSummary(12, 1536LL * 1024 * 1024);
        dialog.resize(width, width == 360 ? 280 : 520); QVERIFY(show(dialog)); auto* scroll = control<QScrollArea>(dialog, "runtimeExportScroll");
        auto* artwork = control<QCheckBox>(dialog, "runtimeExportArtwork"); auto* notice = control<QLabel>(dialog, "runtimeExportArtworkNotice");
        const auto directoryName = qEnvironmentVariable("RUNTIME_EXPORT_ARTWORK_QA_DIR"); if (!directoryName.isEmpty()) QVERIFY(QDir().mkpath(directoryName));
        for (bool checked : {false, true}) {
            artwork->setChecked(checked); scroll->ensureWidgetVisible(notice); QTest::qWait(100);
            QCOMPARE(scroll->widget()->width(), scroll->viewport()->width()); QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
            for (auto* label : {notice, control<QLabel>(dialog, "runtimeExportArtworkSummary")}) {
                QVERIFY(label->wordWrap()); QVERIFY(label->geometry().right() < scroll->widget()->width()); QVERIFY(label->height() >= label->heightForWidth(label->width()));
            }
            QVERIFY(dialog.rect().contains(QRect(button(dialog, QDialogButtonBox::Ok)->mapTo(&dialog, QPoint()), button(dialog, QDialogButtonBox::Ok)->size())));
            if (!directoryName.isEmpty()) QVERIFY(dialog.grab().save(QDir(directoryName).filePath(QString("artwork-%1-%2-%3.png").arg(dark ? "dark" : "light").arg(width).arg(checked ? "included" : "model-only"))));
        }
        scroll->ensureWidgetVisible(artwork); artwork->setFocus(); QTRY_VERIFY(artwork->hasFocus()); QTest::keyClick(artwork, Qt::Key_Space); QVERIFY(!dialog.IncludeArtwork());
        dialog.reject();
    }

    void nonexistentTargetIsAcceptedWithoutCreatingAnything()
    {
        QTemporaryDir directory;
        const auto target = directory.filePath(QStringLiteral("新模型 Unicode"));
        CRuntimeExportDialog dialog(directory.filePath("model.model3.json"), true);
        dialog.SetInitialDirectory(directory.path() + QStringLiteral("/./新模型 Unicode"));
        QCOMPARE(dialog.TargetDirectory(), target);
        QVERIFY(show(dialog));
        QVERIFY(button(dialog, QDialogButtonBox::Ok)->isEnabled());
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        QTest::mouseClick(button(dialog, QDialogButtonBox::Ok), Qt::LeftButton);
        QCOMPARE(accepted.size(), 1);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QVERIFY(!QFileInfo::exists(target));
        QVERIFY(QDir(directory.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }

    void invalidTargetsRemainOpenAndUnaccepted_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* name : {"empty", "whitespace", "relative", "missing-parent", "directory", "file"})
            QTest::newRow(name) << QString::fromLatin1(name);
#ifdef Q_OS_UNIX
        QTest::newRow("symlink") << QStringLiteral("symlink");
        QTest::newRow("dangling-symlink") << QStringLiteral("dangling-symlink");
#endif
    }
    void invalidTargetsRemainOpenAndUnaccepted()
    {
        QFETCH(QString, kind);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString path;
        if (kind == "whitespace") path = "   ";
        if (kind == "relative") path = "relative-folder";
        if (kind == "missing-parent") path = directory.filePath("missing/new");
        if (kind == "directory") { path = directory.filePath("existing"); QVERIFY(QDir().mkdir(path)); }
        if (kind == "file" || kind == "symlink") {
            QFile file(directory.filePath("source")); QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("preserved"), qint64(9)); file.close(); path = file.fileName();
        }
        if (kind == "symlink" || kind == "dangling-symlink") {
            const auto target = path.isEmpty() ? directory.filePath("missing-source") : path;
            path = directory.filePath("link");
            QVERIFY(QFile::link(target, path));
            QVERIFY(QFileInfo(path).isSymLink());
        }
        const auto before = QDir(directory.path()).entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot);
        CRuntimeExportDialog dialog(directory.filePath("model.model3.json"), true);
        dialog.SetInitialDirectory(path);
        QVERIFY(show(dialog));
        auto* error = control<QLabel>(dialog, "runtimeExportError");
        QVERIFY(error && error->isVisible());
        QCOMPARE(error->textFormat(), Qt::PlainText);
        QVERIFY(!error->text().isEmpty());
        QVERIFY(!button(dialog, QDialogButtonBox::Ok)->isEnabled());
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        dialog.accept();
        dialog.done(QDialog::Accepted);
        QCOMPARE(accepted.size(), 0);
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QVERIFY(dialog.isVisible());
        QCOMPARE(QDir(directory.path()).entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot), before);
    }

    void acceptanceRechecksWhetherTheTargetAppeared()
    {
        QTemporaryDir directory;
        const auto target = directory.filePath("new-model");
        CRuntimeExportDialog dialog(directory.filePath("model.model3.json"), true);
        dialog.SetInitialDirectory(target);
        QVERIFY(show(dialog));
        QVERIFY(button(dialog, QDialogButtonBox::Ok)->isEnabled());
        QVERIFY(QDir().mkdir(target));
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        QTest::mouseClick(button(dialog, QDialogButtonBox::Ok), Qt::LeftButton);
        QCOMPARE(accepted.size(), 0);
        QVERIFY(dialog.isVisible());
        QVERIFY(!button(dialog, QDialogButtonBox::Ok)->isEnabled());
        QCOMPARE(QDir(target).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);
    }

    void cancelEscapeAndCloseNeverAccept_data()
    {
        QTest::addColumn<int>("method");
        QTest::newRow("cancel-button") << 0;
        QTest::newRow("escape") << 1;
        QTest::newRow("close") << 2;
    }
    void cancelEscapeAndCloseNeverAccept()
    {
        QFETCH(int, method);
        QTemporaryDir directory;
        CRuntimeExportDialog dialog(directory.filePath("model.model3.json"), true);
        dialog.SetInitialDirectory(directory.filePath("new-model"));
        QVERIFY(show(dialog));
        QSignalSpy accepted(&dialog, &QDialog::accepted), rejected(&dialog, &QDialog::rejected);
        if (method == 0) QTest::mouseClick(button(dialog, QDialogButtonBox::Cancel), Qt::LeftButton);
        if (method == 1) QTest::keyClick(&dialog, Qt::Key_Escape);
        if (method == 2) dialog.close();
        QCOMPARE(accepted.size(), 0);
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QVERIFY(QDir(directory.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }

    void keyboardCanReachEveryOptionAndAccept()
    {
        QTemporaryDir directory;
        CRuntimeExportDialog dialog(directory.filePath("model.model3.json"), true);
        dialog.SetInitialDirectory(directory.filePath("new-model"));
        QVERIFY(show(dialog));
        auto* path = control<QLineEdit>(dialog, "runtimeExportTarget");
        auto* browse = control<QPushButton>(dialog, "runtimeExportBrowse");
        auto* current = control<QCheckBox>(dialog, "runtimeExportCurrentValues");
        auto* timeline = control<QCheckBox>(dialog, "runtimeExportTimeline");
        path->setFocus(); QTRY_VERIFY(path->hasFocus());
        QTest::keyClick(path, Qt::Key_Tab); QTRY_VERIFY(browse->hasFocus());
        QTest::keyClick(browse, Qt::Key_Tab); QTRY_VERIFY(current->hasFocus());
        QTest::keyClick(current, Qt::Key_Space); QVERIFY(dialog.UseCurrentValues());
        QTest::keyClick(current, Qt::Key_Tab); QTRY_VERIFY(timeline->hasFocus());
        QTest::keyClick(timeline, Qt::Key_Space); QVERIFY(dialog.IncludeTimeline());
        QTest::keyClick(timeline, Qt::Key_Tab); QTRY_VERIFY(button(dialog, QDialogButtonBox::Ok)->hasFocus());
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        QTest::keyClick(button(dialog, QDialogButtonBox::Ok), Qt::Key_Return);
        QCOMPARE(accepted.size(), 1);
        QVERIFY(!QFileInfo::exists(dialog.TargetDirectory()));
    }

    void shortThemedDialogKeepsButtonsVisibleAndContentScrollable_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void shortThemedDialogKeepsButtonsVisibleAndContentScrollable()
    {
        QFETCH(bool, dark);
        qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        QTemporaryDir directory;
        CRuntimeExportDialog dialog(directory.filePath(QStringLiteral("很长的中文模型.model3.json")), true);
        dialog.SetInitialDirectory(directory.filePath(QStringLiteral("新模型_图形和动作_完整运行时内容")));
        dialog.resize(360, 250);
        QVERIFY(show(dialog));
        auto* scroll = control<QScrollArea>(dialog, "runtimeExportScroll");
        QVERIFY(scroll);
        QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
        for (auto which : {QDialogButtonBox::Ok, QDialogButtonBox::Cancel}) {
            auto* item = button(dialog, which);
            QVERIFY(item->isVisible());
            QVERIFY(dialog.rect().contains(QRect(item->mapTo(&dialog, QPoint()), item->size())));
            QVERIFY(!scroll->isAncestorOf(item));
        }
        const auto submitImage = button(dialog, QDialogButtonBox::Ok)->grab().toImage();
        const auto colors = Theme::tokens(dark);
        const QColor fill(colors.value(QStringLiteral("@accentFill@"))), hover(colors.value(QStringLiteral("@accentHover@")));
        int primaryPixels = 0;
        for (int y = 0; y < submitImage.height(); ++y) for (int x = 0; x < submitImage.width(); ++x)
            if (submitImage.pixelColor(x, y) == fill || submitImage.pixelColor(x, y) == hover) ++primaryPixels;
        QVERIFY(primaryPixels > submitImage.width() * submitImage.height() / 4);
        const auto location = qEnvironmentVariable("RUNTIME_EXPORT_QA_DIR");
        if (!location.isEmpty()) {
            QVERIFY(QDir().mkpath(location));
            QVERIFY(dialog.grab().save(QDir(location).filePath(dark ? "dark-short-top.png" : "light-short-top.png")));
        }
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        auto* current = control<QCheckBox>(dialog, "runtimeExportCurrentValues");
        scroll->ensureWidgetVisible(current);
        current->setFocus(); QTRY_VERIFY(current->hasFocus());
        QTest::keyClick(current, Qt::Key_Space);
        QVERIFY(dialog.UseCurrentValues());
        if (!location.isEmpty()) {
            QVERIFY(QDir().mkpath(location));
            QVERIFY(dialog.grab().save(QDir(location).filePath(dark ? "dark-short.png" : "light-short.png")));
            dialog.resize(580, 480);
            scroll->verticalScrollBar()->setValue(0);
            QCoreApplication::processEvents();
            QVERIFY(dialog.grab().save(QDir(location).filePath(dark ? "dark-full.png" : "light-full.png")));
        }
        QTest::mouseClick(button(dialog, QDialogButtonBox::Cancel), Qt::LeftButton);
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QVERIFY(!QFileInfo::exists(dialog.TargetDirectory()));
    }
};

QTEST_MAIN(RuntimeExportDialogTests)
#include "runtime_export_dialog_tests.moc"
