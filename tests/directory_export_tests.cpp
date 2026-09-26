#include "std/cdirectoryexport.h"
#include "std/cexportcontrol.h"

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <stdexcept>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

class DirectoryExportTests : public QObject
{
    Q_OBJECT
private:
    using Entry = CDirectoryExport::Entry;
    using Options = CDirectoryExport::Options;
    using Result = CDirectoryExport::Result;
    static QByteArray read(const QString& path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
    static bool write(const QString& path, const QByteArray& bytes)
    {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(bytes) == bytes.size() && file.flush();
    }
    static QStringList stages(const QString& parent)
    {
        return QDir(parent).entryList({QStringLiteral(".live2d-export-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    }
private slots:
    void publishesCompleteByteSnapshotsOnceAndRemainsMovable()
    {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const auto destination = temp.filePath("Package");
        QVector<Entry> entries{{"model.model3.json", "{\"FileReferences\":{\"Textures\":[\"textures/00.png\"]}}"},
            {"textures/00.png", QByteArray("a\0b\xff", 4)}, {QString::fromUtf8("表情/微笑.exp3.json"), "{}"}, {"empty", {}}};
        const auto expected = entries;
        CExportControl control; int commits = 0; qint64 previous = 0;
        QVERIFY(control.SetHandler([&](const auto& progress, QString*) {
            if (progress.phase == CExportControl::Phase::Writing) {
                if (progress.current < previous || progress.current > progress.total) return false;
                previous = progress.current;
                entries.clear();
            }
            if (progress.phase == CExportControl::Phase::Committing) ++commits;
            return true;
        }));
        Result result; QString error;
        QVERIFY2(CDirectoryExport::Publish(destination, entries, {}, result, &control, &error), qPrintable(error));
        QVERIFY(result.committed && control.IsCommitted()); QVERIFY(error.isEmpty()); QVERIFY(result.residualStaging.isEmpty());
        QCOMPARE(result.directory, QFileInfo(destination).canonicalFilePath()); QCOMPARE(commits, 1); QVERIFY(stages(temp.path()).isEmpty());
        for (const auto& entry : expected) QCOMPARE(read(QDir(destination).filePath(entry.relativePath)), entry.bytes);
        control.Cancel(); QVERIFY(control.IsCommitted()); QVERIFY(!control.WasCanceled());
        const auto moved = temp.filePath("Moved"); QVERIFY(QDir().rename(destination, moved));
        for (const auto& entry : expected) QCOMPARE(read(QDir(moved).filePath(entry.relativePath)), entry.bytes);
    }

    void rejectsUnsafeEntriesBeforeCreatingStaging_data()
    {
        QTest::addColumn<QString>("path");
        const QStringList paths{"", "../outside", "a/../outside", "/absolute", "C:/file", "C:file", "//host/share",
            "a\\b", "a//b", "a/./b", "a/", "a.", "a ", "a/<b>", "a:b", "a?b", "CON", "nul.txt", "COM1.json",
            "lpt9", "CONOUT$", QString::fromUtf8("com¹.txt"), QString(241, 'a'), QString::fromUtf8("汉").repeated(81),
            QString("a") + QChar::Null + "b", QString(QChar(0xd800)), QString("a") + QChar(31)};
        int index = 0;
        for (const auto& path : paths) QTest::newRow(qPrintable(QString::number(index++))) << path;
    }
    void rejectsUnsafeEntriesBeforeCreatingStaging()
    {
        QFETCH(QString, path); QTemporaryDir temp; QVERIFY(temp.isValid());
        Result result{true, "old", "old"}; QString error;
        QVERIFY(!CDirectoryExport::Publish(temp.filePath("Output"), {{path, "data"}}, {}, result, nullptr, &error));
        QVERIFY(!error.isEmpty()); QVERIFY(!result.committed); QVERIFY(result.directory.isEmpty());
        QVERIFY(result.residualStaging.isEmpty()); QVERIFY(stages(temp.path()).isEmpty()); QVERIFY(!QFileInfo::exists(temp.filePath("Output")));
    }

    void rejectsPortableFileAndDirectoryConflicts_data()
    {
        QTest::addColumn<QStringList>("paths");
        QTest::newRow("duplicate") << QStringList{"a", "a"};
        QTest::newRow("file-parent") << QStringList{"a", "a/b"};
        QTest::newRow("parent-file") << QStringList{"a/b", "a"};
        QTest::newRow("case-files") << QStringList{"A", "a"};
        QTest::newRow("case-directory") << QStringList{"Assets/a", "assets/b"};
        QTest::newRow("unicode-files") << QStringList{QString::fromUtf8("é.png"), QString::fromUtf8("é.png")};
        QTest::newRow("unicode-directories") << QStringList{QString::fromUtf8("é/a"), QString::fromUtf8("é/b")};
    }
    void rejectsPortableFileAndDirectoryConflicts()
    {
        QFETCH(QStringList, paths); QTemporaryDir temp; QVERIFY(temp.isValid());
        QVector<Entry> entries; for (const auto& path : paths) entries.append({path, "data"});
        Result result; QString error;
        QVERIFY(!CDirectoryExport::Publish(temp.filePath("Output"), entries, {}, result, nullptr, &error));
        QVERIFY(!error.isEmpty()); QVERIFY(stages(temp.path()).isEmpty());
    }

    void preservesEveryExistingTargetKind()
    {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        QVERIFY(QDir(temp.path()).mkdir("directory")); QVERIFY(write(temp.filePath("file"), "original"));
        QVERIFY(QDir(temp.path()).mkdir("populated")); QVERIFY(write(temp.filePath("populated/keep"), "keep"));
        QStringList names{"directory", "file", "populated"};
#ifdef Q_OS_UNIX
        QVERIFY(::symlink("missing", QFile::encodeName(temp.filePath("dangling")).constData()) == 0);
        names.append("dangling");
#endif
        for (const auto& name : names) {
            Result result; QVERIFY(!CDirectoryExport::Publish(temp.filePath(name), {{"new", "bytes"}}, {}, result));
            QVERIFY(!result.committed); QVERIFY(result.residualStaging.isEmpty()); QVERIFY(stages(temp.path()).isEmpty());
        }
        QCOMPARE(read(temp.filePath("file")), QByteArray("original")); QCOMPARE(read(temp.filePath("populated/keep")), QByteArray("keep"));
#ifdef Q_OS_UNIX
        QVERIFY(QFileInfo(temp.filePath("dangling")).isSymLink());
#endif
        Result result;
        QVERIFY(!CDirectoryExport::Publish(temp.filePath("FILE"), {{"new", "bytes"}}, {}, result));
        QVERIFY(!CDirectoryExport::Publish(temp.filePath("absent/output"), {{"new", "bytes"}}, {}, result));
        QVERIFY(!QFileInfo::exists(temp.filePath("absent")));
    }

    void protectsSourceIdentitiesButAllowsNewNestedExport()
    {
        QTemporaryDir temp; QVERIFY(temp.isValid()); QVERIFY(QDir(temp.path()).mkdir("source"));
        QVERIFY(QDir(temp.filePath("source")).mkdir("exports")); QVERIFY(write(temp.filePath("source/model.moc3"), "moc"));
        Options options; options.protectedPaths = {temp.filePath("source"), temp.filePath("source/model.moc3")};
        Result result;
        QVERIFY(!CDirectoryExport::Publish(temp.filePath("source"), {{"data", "x"}}, options, result));
        QVERIFY(!CDirectoryExport::Publish(temp.path(), {{"data", "x"}}, options, result));
        QVERIFY(CDirectoryExport::Publish(temp.filePath("source/exports/new"), {{"data", "x"}}, options, result));
        QCOMPARE(read(temp.filePath("source/model.moc3")), QByteArray("moc"));
        options.protectedPaths = {temp.filePath("reserved/not-yet-present.moc3")};
        QVERIFY(!CDirectoryExport::Publish(temp.filePath("reserved"), {{"data", "x"}}, options, result));
        QVERIFY(!QFileInfo::exists(temp.filePath("reserved")));
#ifdef Q_OS_UNIX
        QVERIFY(::symlink("source", QFile::encodeName(temp.filePath("alias")).constData()) == 0);
        options.protectedPaths = {temp.filePath("alias/new-source.moc3")};
        QVERIFY(!CDirectoryExport::Publish(temp.filePath("source/new-source.moc3"), {{"data", "x"}}, options, result));
        QVERIFY(CDirectoryExport::Publish(temp.filePath("alias/exports/other"), {{"data", "x"}}, options, result));
        QCOMPARE(result.directory, temp.filePath("source/exports/other"));
#endif
    }

    void cancellationAtEveryPrecommitBoundaryLeavesNoOutput_data()
    {
        QTest::addColumn<int>("boundary");
        QTest::newRow("before-start") << 0;
        QTest::newRow("first-file") << 1;
        QTest::newRow("large-file-middle") << 2;
        QTest::newRow("second-file") << 3;
        QTest::newRow("all-written") << 4;
        QTest::newRow("commit-notification") << 5;
    }
    void cancellationAtEveryPrecommitBoundaryLeavesNoOutput()
    {
        QFETCH(int, boundary); QTemporaryDir temp; QVERIFY(temp.isValid());
        const QVector<Entry> entries{{"first", QByteArray(3 * 1024 * 1024, 'a')}, {"second", "b"}};
        for (int iteration = 0; iteration < 3; ++iteration) {
            CExportControl control;
            if (!boundary) control.Cancel();
            QVERIFY(control.SetHandler([&](const auto& progress, QString*) {
                const bool writing = progress.phase == CExportControl::Phase::Writing;
                if ((boundary == 1 && writing && progress.current == 0) ||
                    (boundary == 2 && writing && progress.current >= 1024 * 1024) ||
                    (boundary == 3 && writing && progress.detail == "second") ||
                    (boundary == 4 && writing && progress.current == progress.total) ||
                    (boundary == 5 && progress.phase == CExportControl::Phase::Committing)) control.Cancel();
                return true;
            }));
            Result result; QString error;
            QVERIFY(!CDirectoryExport::Publish(temp.filePath("output"), entries, {}, result, &control, &error));
            QVERIFY(control.WasCanceled()); QVERIFY(!control.IsCommitted()); QVERIFY(!error.isEmpty());
            QVERIFY(!result.committed); QVERIFY(result.residualStaging.isEmpty()); QVERIFY(stages(temp.path()).isEmpty());
            QVERIFY(!QFileInfo::exists(temp.filePath("output")));
        }
    }

    void competingTargetAtCommitRemainsUntouched()
    {
        QTemporaryDir temp; QVERIFY(temp.isValid()); const auto target = temp.filePath("output");
        CExportControl control;
        QVERIFY(control.SetHandler([&](const auto& progress, QString*) {
            if (progress.phase == CExportControl::Phase::Committing)
                return QDir().mkdir(target) && write(QDir(target).filePath("competitor"), "keep");
            return true;
        }));
        Result result; QString error;
        QVERIFY(!CDirectoryExport::Publish(target, {{"ours", "data"}}, {}, result, &control, &error));
        QVERIFY(!control.IsCommitted() && !control.WasCanceled()); QVERIFY(!error.isEmpty());
        QCOMPARE(read(QDir(target).filePath("competitor")), QByteArray("keep"));
        QVERIFY(!QFileInfo::exists(QDir(target).filePath("ours"))); QVERIFY(stages(temp.path()).isEmpty());
    }

    void callbackExceptionAndWritingConflictCleanEarlierFiles()
    {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        for (const bool throws : {false, true}) {
            CExportControl control;
            QVERIFY(control.SetHandler([&](const auto& progress, QString*) {
                if (progress.phase == CExportControl::Phase::Writing && progress.detail == "second") {
                    if (throws) throw std::runtime_error("observer failed");
                    const auto current = stages(temp.path());
                    return current.size() == 1 && write(temp.filePath(current.first() + "/second"), "collision");
                }
                return true;
            }));
            Result result; QString error;
            QVERIFY(!CDirectoryExport::Publish(temp.filePath("output"), {{"first", "a"}, {"second", "b"}}, {}, result, &control, &error));
            QVERIFY(!error.isEmpty()); QVERIFY(!control.WasCanceled()); QVERIFY(!result.committed);
            QVERIFY(stages(temp.path()).isEmpty()); QVERIFY(!QFileInfo::exists(temp.filePath("output")));
        }
    }

    void injectedSymlinkCannotBePublishedOrDeleteItsTarget()
    {
#ifndef Q_OS_UNIX
        QSKIP("该符号链接注入回归使用 POSIX symlink；Windows 需具备 symlink 权限的独立运行。");
#else
        QTemporaryDir temp; QVERIFY(temp.isValid()); QVERIFY(write(temp.filePath("outside"), "safe"));
        CExportControl control;
        QVERIFY(control.SetHandler([&](const auto& progress, QString*) {
            if (progress.phase == CExportControl::Phase::Committing) {
                const auto current = stages(temp.path());
                return current.size() == 1 && ::symlink("../outside", QFile::encodeName(temp.filePath(current.first() + "/link")).constData()) == 0;
            }
            return true;
        }));
        Result result;
        QVERIFY(!CDirectoryExport::Publish(temp.filePath("output"), {{"data", "x"}}, {}, result, &control));
        QCOMPARE(read(temp.filePath("outside")), QByteArray("safe")); QVERIFY(stages(temp.path()).isEmpty());
#endif
    }

    void cleanupFailureIsReportedAndNeverTouchesSource()
    {
#ifndef Q_OS_UNIX
        QSKIP("目录权限故障回归使用 POSIX 权限语义。");
#else
        if (::geteuid() == 0) QSKIP("root 忽略目录写权限，不能复现此故障。");
        QTemporaryDir temp; QVERIFY(temp.isValid()); QVERIFY(write(temp.filePath("source"), "safe"));
        const auto permissions = QFile::permissions(temp.path());
        const auto restore = qScopeGuard([&] { QFile::setPermissions(temp.path(), permissions); });
        CExportControl control;
        QVERIFY(control.SetHandler([&](const auto& progress, QString*) {
            if (progress.phase == CExportControl::Phase::Committing)
                return QFile::setPermissions(temp.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner);
            return true;
        }));
        Result result; QString error;
        QVERIFY(!CDirectoryExport::Publish(temp.filePath("output"), {{"data", "x"}}, {}, result, &control, &error));
        QVERIFY(!result.committed); QVERIFY(!result.residualStaging.isEmpty());
        QVERIFY(error.contains(result.residualStaging)); QVERIFY(!control.IsCommitted() && !control.WasCanceled());
        QCOMPARE(read(temp.filePath("source")), QByteArray("safe")); QVERIFY(!QFileInfo::exists(temp.filePath("output")));
        QVERIFY(QFile::setPermissions(temp.path(), permissions)); QVERIFY(QDir(result.residualStaging).removeRecursively());
#endif
    }

    void cancellationWithCleanupFailureIsReportedAsFailure()
    {
#ifndef Q_OS_UNIX
        QSKIP("目录权限故障回归使用 POSIX 权限语义。");
#else
        if (::geteuid() == 0) QSKIP("root 忽略目录写权限，不能复现此故障。");
        QTemporaryDir temp; QVERIFY(temp.isValid());
        QString staging; QFileDevice::Permissions permissions{};
        const auto restore = qScopeGuard([&] {
            if (!staging.isEmpty()) { QFile::setPermissions(staging, permissions); QDir(staging).removeRecursively(); }
        });
        CExportControl control;
        QVERIFY(control.SetHandler([&](const auto& progress, QString*) {
            if (progress.phase == CExportControl::Phase::Writing && progress.detail == "second") {
                const auto current = stages(temp.path());
                if (current.size() != 1) return false;
                staging = temp.filePath(current.first());
                permissions = QFile::permissions(staging);
                if (!QFile::setPermissions(staging, QFileDevice::ReadOwner | QFileDevice::ExeOwner)) return false;
                control.Cancel();
            }
            return true;
        }));
        Result result; QString error;
        QVERIFY(!CDirectoryExport::Publish(temp.filePath("output"), {{"first", "data"}, {"second", "b"}}, {}, result, &control, &error));
        QCOMPARE(result.residualStaging, staging); QVERIFY(!staging.isEmpty());
        QVERIFY(!control.WasCanceled() && !control.IsCommitted()); QVERIFY(!result.committed);
        QVERIFY(error.contains(QStringLiteral("取消"))); QVERIFY(error.contains(QStringLiteral("清理")));
        QVERIFY(control.Error().contains(QStringLiteral("清理")));
        QVERIFY(!QFileInfo::exists(temp.filePath("output")));
#endif
    }

    void checkedBudgetsRejectWithoutWritingAndAllowEmptyFile()
    {
        QTemporaryDir temp; QVERIFY(temp.isValid()); Options options; Result result;
        options.maximumTotalBytes = 2;
        QVERIFY(!CDirectoryExport::Publish(temp.filePath("output"), {{"a", "aa"}, {"b", "b"}}, options, result));
        options.maximumEntries = 1;
        QVERIFY(!CDirectoryExport::Publish(temp.filePath("output"), {{"a", {}}, {"b", {}}}, options, result));
        QVERIFY(!CDirectoryExport::Publish(temp.filePath("output"), {}, options, result));
        options.maximumTotalBytes = 0;
        QVERIFY(CDirectoryExport::Publish(temp.filePath("output"), {{"a", {}}}, options, result));
        QVERIFY(QFileInfo(temp.filePath("output/a")).isFile()); QCOMPARE(QFileInfo(temp.filePath("output/a")).size(), 0);
        QVERIFY(stages(temp.path()).isEmpty());
    }
};

QTEST_GUILESS_MAIN(DirectoryExportTests)
#include "directory_export_tests.moc"
