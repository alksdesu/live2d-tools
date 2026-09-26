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
