#include "cdirectoryexport.h"
#include "cexportcontrol.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTemporaryDir>
#include <cerrno>
#include <cstring>
#include <exception>
#include <memory>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef Q_OS_LINUX
#include <sys/syscall.h>
#include <linux/fs.h>
#endif
#ifdef Q_OS_DARWIN
#include <stdio.h>
#endif
#endif

namespace {
QString nameKey(const QString& name)
{
    return name.normalized(QString::NormalizationForm_C).toCaseFolded().normalized(QString::NormalizationForm_C);
}

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool validComponent(const QString& component)
{
    if (component.isEmpty() || component == QLatin1String(".") || component == QLatin1String("..") ||
        component.endsWith(QLatin1Char('.')) || component.endsWith(QLatin1Char(' ')) ||
        component.size() > 240 || component.toUtf8().size() > 240 ||
        QString::fromUtf8(component.toUtf8()) != component) return false;
    for (const auto character : component) {
        const auto value = character.unicode();
        if (value < 32 || (value >= 127 && value <= 159) || QStringLiteral("/\\<>:\"|?*").contains(character)) return false;
    }
    const auto stem = nameKey(component.section(QLatin1Char('.'), 0, 0));
    if (stem == QLatin1String("con") || stem == QLatin1String("prn") || stem == QLatin1String("aux") ||
        stem == QLatin1String("nul") || stem == QLatin1String("conin$") || stem == QLatin1String("conout$")) return false;
    if (stem.size() == 4 && (stem.startsWith(QLatin1String("com")) || stem.startsWith(QLatin1String("lpt")))) {
        const auto last = stem.back();
        if ((last >= QLatin1Char('1') && last <= QLatin1Char('9')) || QString::fromUtf8("¹²³").contains(last)) return false;
    }
    return true;
}

bool existsIncludingLink(const QString& path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

bool inside(const QString& child, const QString& parent)
{
    const QString a = nameKey(child), b = nameKey(parent);
    return a == b || a.startsWith(b.endsWith(QLatin1Char('/')) ? b : b + QLatin1Char('/'));
}

bool targetAvailable(const QString& parent, const QString& leaf, QString* error)
{
    if (existsIncludingLink(QDir(parent).filePath(leaf)))
        return fail(error, QStringLiteral("导出目标已存在，不能覆盖：%1").arg(QDir(parent).filePath(leaf)));
    const auto names = QDir(parent).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    const auto key = nameKey(leaf);
    for (const auto& name : names) if (nameKey(name) == key)
        return fail(error, QStringLiteral("导出目标与现有名称冲突：%1").arg(name));
    return true;
}

QString resolvedPath(const QString& path)
{
    if (path.isEmpty() || path.contains(QChar::Null) || !QDir::isAbsolutePath(path) || path.startsWith(QLatin1Char(':'))) return {};
    QString current = QDir::cleanPath(path);
    QStringList suffix;
    while (true) {
        const QFileInfo info(current);
        if (info.exists()) {
            auto canonical = info.canonicalFilePath();
            if (canonical.isEmpty() || (!suffix.isEmpty() && !info.isDir())) return {};
            for (const auto& component : suffix) canonical = QDir(canonical).filePath(component);
            return canonical;
        }
        if (info.isSymLink()) return {};
        const auto parent = info.dir().absolutePath();
        if (parent == current || info.fileName().isEmpty()) return {};
        suffix.prepend(info.fileName());
        current = parent;
    }
}

class DirectoryIdentity
{
public:
    ~DirectoryIdentity()
    {
#ifdef Q_OS_WIN
        if (m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle);
#else
        if (m_fd >= 0) ::close(m_fd);
#endif
    }
    DirectoryIdentity() = default;
    DirectoryIdentity(const DirectoryIdentity&) = delete;
    DirectoryIdentity& operator=(const DirectoryIdentity&) = delete;
    bool Open(const QString& path)
    {
#ifdef Q_OS_WIN
        m_handle = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        return m_handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(m_handle, &m_info) &&
            (m_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(m_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
        m_fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        return m_fd >= 0 && ::fstat(m_fd, &m_info) == 0 && S_ISDIR(m_info.st_mode);
#endif
    }
    bool Matches(const QString& path) const
    {
        DirectoryIdentity current;
        if (!current.Open(path)) return false;
#ifdef Q_OS_WIN
        return m_info.dwVolumeSerialNumber == current.m_info.dwVolumeSerialNumber &&
            m_info.nFileIndexHigh == current.m_info.nFileIndexHigh && m_info.nFileIndexLow == current.m_info.nFileIndexLow;
#else
        return m_info.st_dev == current.m_info.st_dev && m_info.st_ino == current.m_info.st_ino;
#endif
    }
    bool Rename(const QString& parent, const QString& sourceLeaf, const QString& targetLeaf, QString* error) const
    {
#ifdef Q_OS_WIN
        const auto source = QDir::toNativeSeparators(QDir(parent).filePath(sourceLeaf));
        const auto target = QDir::toNativeSeparators(QDir(parent).filePath(targetLeaf));
        if (MoveFileExW(reinterpret_cast<LPCWSTR>(source.utf16()), reinterpret_cast<LPCWSTR>(target.utf16()), 0)) return true;
        return fail(error, QStringLiteral("无法原子发布导出目录（Windows 错误 %1）").arg(GetLastError()));
#else
        Q_UNUSED(parent);
        const auto source = QFile::encodeName(sourceLeaf), target = QFile::encodeName(targetLeaf);
        int status = -1;
#ifdef Q_OS_DARWIN
        status = ::renameatx_np(m_fd, source.constData(), m_fd, target.constData(), RENAME_EXCL);
#elif defined(Q_OS_LINUX) && defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
        status = int(::syscall(SYS_renameat2, m_fd, source.constData(), m_fd, target.constData(), RENAME_NOREPLACE));
#else
        errno = ENOTSUP;
#endif
        if (status == 0) return true;
        const int code = errno;
        return fail(error, QStringLiteral("无法以不覆盖方式原子发布导出目录：%1（%2）")
                    .arg(QString::fromLocal8Bit(std::strerror(code))).arg(code));
#endif
    }
private:
#ifdef Q_OS_WIN
    HANDLE m_handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION m_info{};
#else
    int m_fd = -1;
    struct stat m_info{};
#endif
};

bool validatePlan(const QVector<CDirectoryExport::Entry>& entries, const CDirectoryExport::Options& options,
                  qint64& total, QString* error)
{
    if (entries.isEmpty() || options.maximumEntries <= 0 || options.maximumTotalBytes < 0 ||
        entries.size() > options.maximumEntries) return fail(error, QStringLiteral("导出文件数量或大小限制无效"));
    struct Node { QString path; bool directory; };
    QHash<QString, Node> paths;
    total = 0;
    for (const auto& entry : entries) {
        if (entry.relativePath.toUtf8().size() > 4096 || QDir::isAbsolutePath(entry.relativePath))
            return fail(error, QStringLiteral("导出条目必须使用安全相对路径：%1").arg(entry.relativePath));
        const auto components = entry.relativePath.split(QLatin1Char('/'));
        QString path;
        for (int index = 0; index < components.size(); ++index) {
            if (!validComponent(components[index])) return fail(error, QStringLiteral("导出文件名无效：%1").arg(entry.relativePath));
            if (!path.isEmpty()) path += QLatin1Char('/');
            path += components[index];
            const bool directory = index + 1 < components.size();
            const auto key = nameKey(path);
            const auto found = paths.constFind(key);
            if (found != paths.cend()) {
                if (!directory || !found->directory || found->path != path)
                    return fail(error, QStringLiteral("导出文件或目录名称冲突：%1 / %2").arg(found->path, path));
            } else paths.insert(key, {path, directory});
        }
        if (entry.bytes.size() > options.maximumTotalBytes - total)
            return fail(error, QStringLiteral("导出内容总大小超出限制"));
        total += entry.bytes.size();
    }
    return true;
}

bool verifyTree(const QString& stage, const QVector<CDirectoryExport::Entry>& entries, QString* error)
{
    QHash<QString, qint64> expected;
    for (const auto& entry : entries) {
        expected.insert(entry.relativePath, entry.bytes.size());
        auto parent = QFileInfo(entry.relativePath).path();
        while (parent != QLatin1String(".")) {
            expected.insert(parent, -1);
            parent = QFileInfo(parent).path();
        }
    }
    QStringList pending{QString()};
    for (qsizetype index = 0; index < pending.size(); ++index) {
        const auto directory = pending[index];
        const auto children = QDir(QDir(stage).filePath(directory)).entryInfoList(
            QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::NoSort);
        for (const auto& child : children) {
            const auto relative = directory.isEmpty() ? child.fileName() : directory + QLatin1Char('/') + child.fileName();
            const auto found = expected.constFind(relative);
            if (found == expected.cend() || child.isSymLink() ||
                (*found < 0 ? !child.isDir() : !child.isFile() || child.size() != *found))
                return fail(error, QStringLiteral("导出暂存内容被替换或与计划不符：%1").arg(relative));
            if (*found < 0) pending.append(relative);
            expected.remove(relative);
        }
    }
    return expected.isEmpty() || fail(error, QStringLiteral("导出暂存文件缺失"));
}

bool prepareDirectories(const QString& stage, const QString& relative, QString* error)
{
    const auto components = relative.split(QLatin1Char('/'));
    QString parent = stage;
    for (int index = 0; index + 1 < components.size(); ++index) {
        const QString path = QDir(parent).filePath(components[index]);
        if (!existsIncludingLink(path) && !QDir(parent).mkdir(components[index]))
            return fail(error, QStringLiteral("无法创建导出子目录：%1").arg(path));
        const QFileInfo info(path);
        if (info.isSymLink() || !info.isDir() || !inside(info.canonicalFilePath(), stage))
            return fail(error, QStringLiteral("导出子目录被替换或不是普通目录：%1").arg(path));
        parent = path;
    }
    return true;
}
}

bool CDirectoryExport::Publish(const QString& targetDirectory, const QVector<Entry>& entries, const Options& options,
                               Result& result, CExportControl* control, QString* error)
{
    result = {};
    if (error) error->clear();
    QString reason, stagePath;
    std::unique_ptr<QTemporaryDir> stage;
    DirectoryIdentity parentIdentity, stageIdentity;
    bool committed = false;
    try {
        const auto snapshot = entries;
        const auto settings = options;
        const QString requested = targetDirectory;
        const auto execute = [&]() {
            qint64 total = 0;
            if (!validatePlan(snapshot, settings, total, &reason)) return false;
            if (!QDir::isAbsolutePath(requested) || requested.startsWith(QLatin1Char(':')) || requested.contains(QChar::Null))
                return fail(&reason, QStringLiteral("导出目标必须是本地绝对目录路径"));
            const QFileInfo target(QDir::cleanPath(requested));
            const auto leaf = target.fileName();
            if (!validComponent(leaf)) return fail(&reason, QStringLiteral("导出目标目录名称无效"));
            const auto parent = target.dir().canonicalPath();
            if (parent.isEmpty() || !parentIdentity.Open(parent)) return fail(&reason, QStringLiteral("导出目标的父目录不存在或不可访问"));
            const auto destination = QDir(parent).filePath(leaf);
            for (const auto& protectedPath : settings.protectedPaths) {
                const auto source = resolvedPath(protectedPath);
                if (source.isEmpty())
                    return fail(&reason, QStringLiteral("源保护路径不存在或无效：%1").arg(protectedPath));
                if (inside(source, destination))
                    return fail(&reason, QStringLiteral("导出目标不能覆盖受保护源路径或其祖先：%1").arg(protectedPath));
            }
            if (!targetAvailable(parent, leaf, &reason)) return false;
            if (control && !control->Report(CExportControl::Phase::Preparing, 0, total, destination)) return false;
            if (!parentIdentity.Matches(parent) || !targetAvailable(parent, leaf, &reason))
                return fail(&reason, reason.isEmpty() ? QStringLiteral("导出父目录在准备期间被替换") : reason);
            stage = std::make_unique<QTemporaryDir>(QDir(parent).filePath(QStringLiteral(".live2d-export-XXXXXX")));
            if (!stage->isValid()) return fail(&reason, QStringLiteral("无法创建导出暂存目录：%1").arg(stage->errorString()));
            stage->setAutoRemove(false);
            stagePath = stage->path();
            if (!stageIdentity.Open(stagePath)) return fail(&reason, QStringLiteral("无法确认导出暂存目录身份"));
            qint64 written = 0;
            for (const auto& entry : snapshot) {
                if (control && !control->Report(CExportControl::Phase::Writing, written, total, entry.relativePath)) return false;
                if (!parentIdentity.Matches(parent) || !stageIdentity.Matches(stagePath))
                    return fail(&reason, QStringLiteral("导出目录在写入期间被替换"));
                if (!prepareDirectories(stagePath, entry.relativePath, &reason)) return false;
                QFile file(QDir(stagePath).filePath(entry.relativePath));
                if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                    return fail(&reason, QStringLiteral("无法创建导出文件 %1：%2").arg(entry.relativePath, file.errorString()));
                qint64 offset = 0;
                while (offset < entry.bytes.size()) {
                    if (control && !control->Report(CExportControl::Phase::Writing, written, total, entry.relativePath)) return false;
                    const qint64 count = qMin<qint64>(1024 * 1024, entry.bytes.size() - offset);
                    const qint64 amount = file.write(entry.bytes.constData() + offset, count);
                    if (amount <= 0) return fail(&reason, QStringLiteral("导出文件写入失败 %1：%2").arg(entry.relativePath, file.errorString()));
                    offset += amount;
                    written += amount;
                }
                if (!file.flush()) return fail(&reason, QStringLiteral("导出文件刷新失败 %1：%2").arg(entry.relativePath, file.errorString()));
                file.close();
                if (file.error() != QFileDevice::NoError) return fail(&reason, QStringLiteral("导出文件关闭失败：%1").arg(entry.relativePath));
            }
            if (control && !control->Report(CExportControl::Phase::Writing, written, total)) return false;
            if (control && !control->BeginCommit()) return false;
            if (!parentIdentity.Matches(parent) || !stageIdentity.Matches(stagePath))
                return fail(&reason, QStringLiteral("导出目录在发布前被替换"));
            if (!targetAvailable(parent, leaf, &reason) || !verifyTree(stagePath, snapshot, &reason)) return false;
            if (!parentIdentity.Rename(parent, QFileInfo(stagePath).fileName(), leaf, &reason)) return false;
            committed = true;
            result.committed = true;
            result.directory = destination;
            if (control) control->CommitFinished(true);
            return true;
        };
        execute();
    } catch (const std::exception& exception) {
        reason = QStringLiteral("目录导出异常：%1").arg(QString::fromUtf8(exception.what()));
    } catch (...) {
        reason = QStringLiteral("目录导出发生未知异常");
    }
    if (committed) return true;
    if (reason.isEmpty() && control) reason = control->Error();
    if (reason.isEmpty()) reason = QStringLiteral("目录导出未完成");
    if (!stagePath.isEmpty()) {
        if (!stageIdentity.Matches(stagePath) || !QDir(stagePath).removeRecursively()) {
            result.residualStaging = stagePath;
            reason += QStringLiteral("\n暂存目录未能安全清理：%1").arg(stagePath);
            if (control) control->Fail(reason);
        }
    }
    if (control && !control->WasCanceled()) {
        control->CommitFinished(false, reason);
        control->Fail(reason);
    }
    return fail(error, reason);
}
