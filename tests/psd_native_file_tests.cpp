#include "psd/psdSamples/PsdPch.h"
#include "psd/psdSamples/Interfaces/PsdMallocAllocator.h"
#include "psd/psdSamples/Interfaces/PsdNativeFile.h"
#include <QtTest>
#include <QTemporaryDir>
#include <clocale>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>

namespace {
class ScopedCLocale
{
public:
    ScopedCLocale()
        : m_previous(std::setlocale(LC_CTYPE, nullptr))
    {
        std::setlocale(LC_CTYPE, "C");
    }

    ~ScopedCLocale()
    {
        std::setlocale(LC_CTYPE, m_previous.c_str());
    }

private:
    std::string m_previous;
};
}

class PsdNativeFileTests : public QObject
{
    Q_OBJECT
private slots:
    void unicodePathsInCLocale()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        ScopedCLocale locale;
        QCOMPARE(std::setlocale(LC_CTYPE, nullptr), "C");
        const QString directory = temp.filePath(QString::fromUtf8("模型_é_🎨"));
        QVERIFY(QDir().mkpath(directory));
        const QString path = QDir(directory).filePath(QString::fromUtf8("图层_😀.psd"));
        const std::wstring widePath = path.toStdWString();
        const QByteArray data = QByteArray::fromHex("38425053000100ff10203040");

        psd::MallocAllocator allocator;
        psd::NativeFile file(&allocator);
        QVERIFY(file.OpenWrite(widePath.c_str()));
        auto write = file.Write(data.constData(), data.size(), 0);
        QVERIFY(file.WaitForWrite(write));
        QVERIFY(write == nullptr);
        QVERIFY(file.Close());
        QVERIFY(QFile::exists(path));

        QVERIFY(file.OpenRead(widePath.c_str()));
        QCOMPARE(file.GetSize(), static_cast<uint64_t>(data.size()));
        QByteArray restored(data.size(), '\0');
        auto read = file.Read(restored.data(), restored.size(), 0);
        QVERIFY(file.WaitForRead(read));
        QVERIFY(read == nullptr);
        QCOMPARE(restored, data);
        QVERIFY(file.Close());
    }

    void randomAccessAndOperationLifetime()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const std::wstring path = temp.filePath("offsets.psd").toStdWString();
        psd::MallocAllocator allocator;
        psd::NativeFile file(&allocator);
        QVERIFY(file.OpenWrite(path.c_str()));

        auto write = file.Write("end", 3, 7);
        QVERIFY(file.WaitForWrite(write));
        QVERIFY(!file.WaitForWrite(write));
        write = file.Write("start", 5, 0);
        QVERIFY(file.WaitForWrite(write));
        write = file.Write("+-", 2, 5);
        QVERIFY(file.WaitForWrite(write));
        QCOMPARE(file.GetSize(), uint64_t(10));
        QVERIFY(file.Close());
        QVERIFY(file.Close());

        QVERIFY(file.OpenRead(path.c_str()));
        char tail[3]{};
        auto read = file.Read(tail, 3, 7);
        QVERIFY(file.WaitForRead(read));
        QVERIFY(!file.WaitForRead(read));
        QCOMPARE(QByteArray(tail, 3), QByteArray("end"));
        QCOMPARE(file.GetSize(), uint64_t(10));
        char all[10]{};
        read = file.Read(all, 10, 0);
        QVERIFY(file.WaitForRead(read));
        QCOMPARE(QByteArray(all, 10), QByteArray("start+-end"));

        read = file.Read(all, 10, 7);
        QVERIFY(!file.WaitForRead(read));
        QVERIFY(read == nullptr);
        QVERIFY(!file.WaitForRead(read));
        read = file.Read(all, 1, 10);
        QVERIFY(!file.WaitForRead(read));
        QVERIFY(read == nullptr);
        read = file.Read(all, 10, 0);
        QVERIFY(file.WaitForRead(read));
        QCOMPARE(QByteArray(all, 10), QByteArray("start+-end"));
        QVERIFY(file.Close());
    }

    void failedOperationsCanBeWaitedOnce()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const std::wstring path = temp.filePath("operations.psd").toStdWString();
        psd::MallocAllocator allocator;
        psd::NativeFile file(&allocator);
        char buffer[4]{};

        auto read = file.Read(buffer, sizeof(buffer), 0);
        QVERIFY(!file.WaitForRead(read));
        QVERIFY(read == nullptr);
        QVERIFY(!file.WaitForRead(read));
        auto write = file.Write("data", 4, 0);
        QVERIFY(!file.WaitForWrite(write));
        QVERIFY(write == nullptr);
        QVERIFY(!file.WaitForWrite(write));
        QVERIFY(file.Close());

        QVERIFY(file.OpenWrite(path.c_str()));
        write = file.Write("data", 4, 0);
        QVERIFY(file.WaitForWrite(write));
        read = file.Read(buffer, sizeof(buffer), 0);
        QVERIFY(!file.WaitForRead(read));
        QVERIFY(read == nullptr);
        QVERIFY(file.Close());
        QVERIFY(file.OpenRead(path.c_str()));
        write = file.Write("oops", 4, 0);
        QVERIFY(!file.WaitForWrite(write));
        QVERIFY(write == nullptr);
        read = file.Read(buffer, sizeof(buffer), 0);
        QVERIFY(file.WaitForRead(read));
        QCOMPARE(QByteArray(buffer, 4), QByteArray("data"));
        QVERIFY(file.Close());
    }

    void repeatedOpenPreservesCurrentFile()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString firstPath = temp.filePath("first.psd");
        const QString secondPath = temp.filePath("second.psd");
        QFile existing(secondPath);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("existing"), qint64(8));
        existing.close();

        psd::MallocAllocator allocator;
        psd::NativeFile file(&allocator);
        QVERIFY(file.OpenWrite(firstPath.toStdWString().c_str()));
        QVERIFY(!file.OpenWrite(secondPath.toStdWString().c_str()));
        QVERIFY(!file.OpenRead(secondPath.toStdWString().c_str()));
        QVERIFY(!file.OpenRead(temp.filePath("missing.psd").toStdWString().c_str()));
        auto write = file.Write("original", 8, 0);
        QVERIFY(file.WaitForWrite(write));
        QVERIFY(file.Close());
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("existing"));
        existing.close();
        QVERIFY(file.OpenRead(firstPath.toStdWString().c_str()));
        QVERIFY(!file.OpenWrite(secondPath.toStdWString().c_str()));
        char buffer[8]{};
        auto read = file.Read(buffer, sizeof(buffer), 0);
        QVERIFY(file.WaitForRead(read));
        QCOMPARE(QByteArray(buffer, sizeof(buffer)), QByteArray("original"));
        QVERIFY(file.Close());

        QVERIFY(!file.OpenRead(temp.filePath("missing.psd").toStdWString().c_str()));
        QVERIFY(file.OpenRead(secondPath.toStdWString().c_str()));
        QVERIFY(file.Close());
    }

    void destructionClosesAndFlushesFile()
    {
        static_assert(!std::is_copy_constructible<psd::NativeFile>::value, "File handles cannot be copied");
        static_assert(!std::is_copy_assignable<psd::NativeFile>::value, "File handles cannot be copied");
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("destruction.psd");
        psd::MallocAllocator allocator;
        {
            psd::NativeFile file(&allocator);
            QVERIFY(file.OpenWrite(path.toStdWString().c_str()));
            auto write = file.Write("flushed", 7, 0);
            QVERIFY(file.WaitForWrite(write));
        }
        QFile verify(path);
        QVERIFY(verify.open(QIODevice::ReadOnly));
        QCOMPARE(verify.readAll(), QByteArray("flushed"));
        verify.close();
        psd::NativeFile reopened(&allocator);
        QVERIFY(reopened.OpenWrite(path.toStdWString().c_str()));
        QVERIFY(reopened.Close());
    }

    void oversizedPathDoesNotOpenPrefix()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("unchanged.psd");
        QFile existing(path);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("original"), qint64(8));
        existing.close();
        const QStringList entries = QDir(temp.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
        const int padding = 4095 - temp.path().toUtf8().size() - int(std::strlen("unchanged.psd"));
        QVERIFY(padding > 0);
        const std::wstring prefix = temp.path().toStdWString() + std::wstring(padding, L'/') + L"unchanged.psd";
        const std::wstring oversized = prefix + std::wstring(8192, L'x');
        psd::MallocAllocator allocator;
        psd::NativeFile file(&allocator);
        QVERIFY(!file.OpenWrite(oversized.c_str()));
        QVERIFY(!file.OpenRead(oversized.c_str()));
        QCOMPARE(QDir(temp.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), entries);
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("original"));
        QVERIFY(file.Close());
    }

    void invalidUnicodeIsRejected()
    {
#if PSD_USE_MSVC
        QSKIP("Windows paths use native UTF-16 filename semantics");
#else
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        ScopedCLocale locale;
        psd::MallocAllocator allocator;
        psd::NativeFile file(&allocator);
        for (const uint32_t invalid : {0xD800u, 0xDC00u, 0x110000u, 0xFFFFFFFFu}) {
            if (sizeof(wchar_t) < 4 && invalid > 0xFFFF) continue;
            std::wstring path = temp.filePath("invalid-").toStdWString();
            path.push_back(static_cast<wchar_t>(invalid));
            path += L".psd";
            QVERIFY(!file.OpenWrite(path.c_str()));
            QVERIFY(!file.OpenRead(path.c_str()));
        }
        QVERIFY(QDir(temp.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
#endif
    }

    void outOfRangeOffsetsAreRejected()
    {
#if PSD_USE_MSVC
        QSKIP("POSIX off_t overflow guard");
#else
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const std::wstring path = temp.filePath("offset-range.psd").toStdWString();
        psd::MallocAllocator allocator;
        psd::NativeFile file(&allocator);
        QVERIFY(file.OpenWrite(path.c_str()));
        auto write = file.Write("data", 4, std::numeric_limits<uint64_t>::max());
        QVERIFY(!file.WaitForWrite(write));
        QVERIFY(file.Close());
        QVERIFY(file.OpenRead(path.c_str()));
        char buffer{};
        auto read = file.Read(&buffer, 1, std::numeric_limits<uint64_t>::max());
        QVERIFY(!file.WaitForRead(read));
        QVERIFY(file.Close());
#endif
    }

    void bufferedCloseFailureIsReported()
    {
#if defined(__linux__)
        if (!QFile::exists("/dev/full")) QSKIP("/dev/full is unavailable");
        psd::MallocAllocator allocator;
        psd::NativeFile file(&allocator);
        QVERIFY(file.OpenWrite(L"/dev/full"));
        auto write = file.Write("data", 4, 0);
        const bool writeSucceeded = file.WaitForWrite(write);
        const bool closeSucceeded = file.Close();
        QVERIFY(!writeSucceeded || !closeSucceeded);
        QVERIFY(file.Close());
#else
        QSKIP("Requires the Linux /dev/full failure device");
#endif
    }
};

QTEST_GUILESS_MAIN(PsdNativeFileTests)
#include "psd_native_file_tests.moc"
