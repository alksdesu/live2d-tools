#include "cmo3/ccaffarchive.h"
#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <cstdio>

#ifdef Q_OS_UNIX
#include <csignal>
#include <sys/resource.h>
#endif

namespace {
QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QStringList directoryEntries(const QString& path)
{
    return QDir(path).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                               QDir::Name);
}

int limitedSave(const QString& mode, const QString& path)
{
#ifdef Q_OS_UNIX
    struct rlimit limit {};
    if (getrlimit(RLIMIT_FSIZE, &limit) != 0 || limit.rlim_max < 64) return 77;
    limit.rlim_cur = 64;
    if (std::signal(SIGXFSZ, SIG_IGN) == SIG_ERR || setrlimit(RLIMIT_FSIZE, &limit) != 0) return 77;
    cmo3::CCaffArchive archive;
    archive.AddEntry(QStringLiteral("main.xml"), QByteArray(mode == QStringLiteral("write") ? 262144 : 4096, 'x'));
    QString error;
    const bool saved = archive.Save(path, &error);
    const QByteArray report = QJsonDocument(QJsonObject{{QStringLiteral("saved"), saved},
                                                       {QStringLiteral("error"), error}}).toJson(QJsonDocument::Compact);
    std::fwrite(report.constData(), 1, static_cast<size_t>(report.size()), stdout);
    return 0;
#else
    Q_UNUSED(mode);
    Q_UNUSED(path);
    return 77;
#endif
}
}

class CaffArchiveTests : public QObject
{
    Q_OBJECT
private slots:
    void createsAndReplacesArchives_data()
    {
        QTest::addColumn<int>("compression");
        QTest::addColumn<bool>("replace");
        for (int compression : {cmo3::CCaffArchive::CompressRaw, cmo3::CCaffArchive::CompressFast,
                                cmo3::CCaffArchive::CompressSmall}) {
            for (bool replace : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(compression).arg(replace))) << compression << replace;
        }
    }

    void createsAndReplacesArchives()
    {
        QFETCH(int, compression);
        QFETCH(bool, replace);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("工程_😀.cmo3"));
        if (replace) QVERIFY(writeFile(path, QByteArray(65536, 'o')));
        cmo3::CCaffArchive archive;
        archive.SetObfuscateKey(-1234567);
        const QByteArray xml = QStringLiteral("<model name=\"模型😀\"/>").toUtf8();
        const QByteArray pixels = QByteArray::fromHex("0011ff804433aa00");
        archive.AddEntry(QStringLiteral("main.xml"), xml, QStringLiteral("main_xml"), compression);
        archive.AddEntry(QStringLiteral("画像/texture.bin"), pixels, {}, compression);
        QString error;
        QVERIFY2(archive.Save(path, &error), qPrintable(error));
        QCOMPARE(directoryEntries(temporary.path()), QStringList{QFileInfo(path).fileName()});

        cmo3::CCaffArchive restored;
        QVERIFY2(restored.Load(path, &error), qPrintable(error));
        QCOMPARE(restored.ObfuscateKey(), archive.ObfuscateKey());
        QCOMPARE(restored.Entries().size(), 2);
        QVERIFY(restored.FindByTag(QStringLiteral("main_xml")));
        QCOMPARE(restored.FindByTag(QStringLiteral("main_xml"))->content, xml);
        QVERIFY(restored.FindByPath(QStringLiteral("画像/texture.bin")));
        QCOMPARE(restored.FindByPath(QStringLiteral("画像/texture.bin"))->content, pixels);
        QCOMPARE(restored.Entries()[0].compress, compression);
        QCOMPARE(restored.Entries()[1].compress, compression);

        const QByteArray firstSave = readFile(path);
        QVERIFY2(archive.Save(path, &error), qPrintable(error));
        if (compression == cmo3::CCaffArchive::CompressRaw) QCOMPARE(readFile(path), firstSave);
        QVERIFY2(restored.Load(path, &error), qPrintable(error));
        QCOMPARE(restored.Entries().size(), 2);
        QCOMPARE(restored.Entries().first().content, xml);
        cmo3::CCaffArchive empty;
        QVERIFY2(empty.Save(path, &error), qPrintable(error));
        QVERIFY(readFile(path).size() < firstSave.size());
        QVERIFY2(restored.Load(path, &error), qPrintable(error));
        QVERIFY(restored.Entries().isEmpty());
    }

    void openFailureAllowsRetry()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("missing/retry.cmo3"));
        cmo3::CCaffArchive archive;
        archive.AddEntry(QStringLiteral("main.xml"), QByteArray("retained"));
        QString error;
        QVERIFY(!archive.Save(path, &error));
        QVERIFY(error.contains(path));
        QVERIFY(error.startsWith(QStringLiteral("无法创建输出文件")));
        QVERIFY(!QFile::exists(path));
        QCOMPARE(archive.Entries().first().content, QByteArray("retained"));
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QVERIFY2(archive.Save(path, &error), qPrintable(error));
        cmo3::CCaffArchive restored;
        QVERIFY2(restored.Load(path, &error), qPrintable(error));
        QCOMPARE(restored.Entries().first().content, QByteArray("retained"));
    }

    void directoryDestinationIsNotChanged()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("existing.cmo3"));
        QVERIFY(QDir().mkdir(path));
        const QString child = QDir(path).filePath(QStringLiteral("keep"));
        QVERIFY(writeFile(child, QByteArray("original")));
        cmo3::CCaffArchive archive;
        archive.AddEntry(QStringLiteral("main.xml"), QByteArray("replacement"));
        QString error;
        QVERIFY(!archive.Save(path, &error));
        QVERIFY(error.contains(path));
        QCOMPARE(readFile(child), QByteArray("original"));
        QCOMPARE(directoryEntries(temporary.path()), QStringList{QStringLiteral("existing.cmo3")});
        QCOMPARE(directoryEntries(path), QStringList{QStringLiteral("keep")});
    }

    void unwritableDirectoryDoesNotFallBackToDirectWrite()
    {
#ifdef Q_OS_UNIX
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("existing.cmo3"));
        const QByteArray original("original-file");
        QVERIFY(writeFile(path, original));
        const QFile::Permissions previous = QFile::permissions(temporary.path());
        const auto restore = qScopeGuard([&] { QFile::setPermissions(temporary.path(), previous); });
        QVERIFY(QFile::setPermissions(temporary.path(), QFile::ReadOwner | QFile::ExeOwner));
        {
            QTemporaryFile probe(temporary.filePath(QStringLiteral("probe.XXXXXX")));
            if (probe.open()) QSKIP("Current user or filesystem permits creating files in a non-writable directory");
        }
        cmo3::CCaffArchive archive;
        archive.AddEntry(QStringLiteral("main.xml"), QByteArray("replacement"));
        QString error;
        QVERIFY(!archive.Save(path, &error));
        QVERIFY(error.contains(path));
        QCOMPARE(readFile(path), original);
        QCOMPARE(directoryEntries(temporary.path()), QStringList{QStringLiteral("existing.cmo3")});
#else
        QSKIP("Requires POSIX directory write permissions");
#endif
    }

    void fileSizeLimitPreservesDestination_data()
    {
        QTest::addColumn<QString>("stage");
        QTest::addColumn<bool>("existing");
        for (const QString& stage : {QStringLiteral("write"), QStringLiteral("commit")})
            for (bool existing : {false, true})
                QTest::newRow(qPrintable(stage + (existing ? QStringLiteral("-existing") : QStringLiteral("-new")))) << stage << existing;
    }

    void fileSizeLimitPreservesDestination()
    {
#ifdef Q_OS_UNIX
        QFETCH(QString, stage);
        QFETCH(bool, existing);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("output.cmo3"));
        const QByteArray original("original-file\0unchanged", 23);
        if (existing) QVERIFY(writeFile(path, original));
        const QStringList entries = directoryEntries(temporary.path());
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--limited-save"), stage, path});
        QVERIFY2(child.waitForStarted(), qPrintable(child.errorString()));
        QVERIFY2(child.waitForFinished(30000), qPrintable(child.errorString()));
        const QByteArray output = child.readAllStandardOutput();
        const QByteArray diagnostic = output + '\n' + child.readAllStandardError();
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        if (child.exitCode() == 77) QSKIP("This process cannot set RLIMIT_FSIZE or ignore SIGXFSZ");
        QVERIFY2(child.exitCode() == 0, diagnostic.constData());
        QJsonParseError parseError;
        const QJsonDocument report = QJsonDocument::fromJson(output, &parseError);
        QVERIFY2(parseError.error == QJsonParseError::NoError && report.isObject(), diagnostic.constData());
        QVERIFY2(report.object().contains(QStringLiteral("saved")), diagnostic.constData());
        QVERIFY2(!report.object().value(QStringLiteral("saved")).toBool(true), diagnostic.constData());
        const QString error = report.object().value(QStringLiteral("error")).toString();
        QVERIFY2(error.contains(path), diagnostic.constData());
        QVERIFY2(error.startsWith(stage == QStringLiteral("write") ? QStringLiteral("写入文件失败")
                                                                   : QStringLiteral("提交文件失败")), diagnostic.constData());
        QCOMPARE(QFile::exists(path), existing);
        if (existing) QCOMPARE(readFile(path), original);
        QCOMPARE(directoryEntries(temporary.path()), entries);
#else
        QSKIP("Requires a POSIX child process with RLIMIT_FSIZE");
#endif
    }
};

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() == 4 && arguments[1] == QStringLiteral("--limited-save"))
        return limitedSave(arguments[2], arguments[3]);
    CaffArchiveTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "caff_archive_tests.moc"
