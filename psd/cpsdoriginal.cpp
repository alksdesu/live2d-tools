#include "cpsdoriginal.h"
#include "std/cexportcontrol.h"
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>

namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}
}

bool CPsdOriginal::Prepare(const QByteArray& bytes, const QByteArray& expectedDigest, CPsdOriginal& result,
                           QString* error, CExportControl* control, const CPsdReadLimits& limits) try
{
    if (error) error->clear();
    if (expectedDigest.size() != 32) return fail(error, QStringLiteral("原 PSD 来源摘要无效"));
    const QByteArray expected(expectedDigest.constData(), expectedDigest.size());
    auto directory = std::make_shared<CPsdDirectory>();
    if (!CPsdReader::ReadDirectoryBytes(bytes, *directory, error, control, limits)) return false;
    if (directory->sha256 != expected) return fail(error, QStringLiteral("原 PSD 字节与保存的来源摘要不一致"));
    CPsdOriginal candidate;
    candidate.m_directory = std::move(directory);
    result = std::move(candidate);
    return true;
} catch (...) {
    return fail(error, QStringLiteral("无法准备原 PSD，原恢复结果保持不变"));
}

bool CPsdOriginal::Write(const QString& path, CExportControl* requested, QString* error) const
{
    if (error) error->clear();
    CExportControl local;
    auto& control = requested ? *requested : local;
    const auto failed = [&](const QString& reason) {
        if (!control.WasCanceled()) control.Fail(reason);
        return fail(error, control.Error().isEmpty() ? reason : control.Error());
    };
    try {
        const auto directory = m_directory;
        if (!directory) return failed(QStringLiteral("原 PSD 尚未通过完整性验证"));
        if (path.isEmpty() || path.size() > 32768 || path.contains(QChar::Null) || !path.isValidUtf16())
            return failed(QStringLiteral("原 PSD 输出路径无效"));
        const QString target(path.constData(), path.size());
        const auto safeTarget = [&] {
            const QFileInfo file(target);
            return !file.isSymLink() && !file.isDir();
        };
        if (!safeTarget()) return failed(QStringLiteral("原 PSD 输出不能是符号链接或目录"));
        if (!control.Report(CExportControl::Phase::Preparing)) return failed(control.Error());
        QSaveFile output(target);
        output.setDirectWriteFallback(false);
        if (!safeTarget() || !output.open(QIODevice::WriteOnly))
            return failed(QStringLiteral("无法打开原 PSD 输出文件：%1").arg(output.errorString()));
        const auto& bytes = directory->bytes;
        qsizetype offset = 0;
        while (offset < bytes.size()) {
            if (!control.Report(CExportControl::Phase::Writing, offset, bytes.size())) return failed(control.Error());
            const qsizetype amount = std::min(qsizetype(256 * 1024), bytes.size() - offset);
            if (output.write(bytes.constData() + offset, amount) != amount)
                return failed(QStringLiteral("原 PSD 写入失败：%1").arg(output.errorString()));
            offset += amount;
        }
        if (!safeTarget()) return failed(QStringLiteral("原 PSD 输出路径在写入期间发生变化"));
        if (!control.BeginCommit()) return failed(control.Error());
        if (!safeTarget()) {
            control.CommitFinished(false, QStringLiteral("原 PSD 输出路径在提交前发生变化"));
            return failed(control.Error());
        }
        const bool committed = output.commit();
        control.CommitFinished(committed, output.errorString());
        if (!committed) return failed(control.Error());
        return true;
    } catch (...) {
        return failed(QStringLiteral("原 PSD 写出失败，未替换已有文件"));
    }
}
