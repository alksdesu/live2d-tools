#include "cpreviewexporttransaction.h"
#include <QScopedValueRollback>
#include <exception>
#include <utility>

namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}
}

CPreviewExportTransaction::~CPreviewExportTransaction()
{
    m_executing = true;
    m_steps = {};
}

bool CPreviewExportTransaction::Run(Steps steps, QString* error)
{
    if (error) error->clear();
    if (IsBusy()) return fail(error, QStringLiteral("导出事务正在执行或等待恢复"));
    if (!steps.pause || !steps.exportFile || !steps.restorePreview || !steps.resume)
        return fail(error, QStringLiteral("导出事务缺少暂停、导出或恢复步骤"));
    if (static_cast<bool>(steps.beginTemporary) != static_cast<bool>(steps.endTemporary))
        return fail(error, QStringLiteral("临时模型进入和恢复步骤必须同时提供"));

    const QScopedValueRollback<bool> executing(m_executing, true);
    m_steps = std::move(steps);
    m_recoveryStage = RecoveryStage::RestorePreview;
    QString operationError;
    bool exported = false;
    if (Invoke(m_steps.pause, QStringLiteral("暂停预览"), &operationError)) {
        bool ready = true;
        if (m_steps.beginTemporary) {
            m_recoveryStage = RecoveryStage::EndTemporary;
            ready = Invoke(m_steps.beginTemporary, QStringLiteral("进入临时模型"), &operationError);
        }
        if (ready) exported = Invoke(m_steps.exportFile, QStringLiteral("导出文件"), &operationError);
    }
    m_operationError = operationError;
    QString recoveryError;
    const bool restored = Restore(&recoveryError);
    if (!exported || !restored) return fail(error, CombinedError(operationError, recoveryError));
    return true;
}

bool CPreviewExportTransaction::Recover(QString* error)
{
    if (error) error->clear();
    if (m_executing) return fail(error, QStringLiteral("导出事务正在执行，不能重复恢复"));
    if (!m_needsRecovery) return true;
    const QScopedValueRollback<bool> executing(m_executing, true);
    QString recoveryError;
    if (!Restore(&recoveryError)) return fail(error, CombinedError(m_operationError, recoveryError));
    return true;
}

bool CPreviewExportTransaction::IsBusy() const
{
    return m_executing || m_needsRecovery;
}

bool CPreviewExportTransaction::NeedsRecovery() const
{
    return m_needsRecovery;
}

bool CPreviewExportTransaction::ResetAfterPreviewClosed()
{
    if (m_executing) return false;
    const QScopedValueRollback<bool> executing(m_executing, true);
    Clear();
    return true;
}

bool CPreviewExportTransaction::Invoke(const Step& step, const QString& stage, QString* error)
{
    QString reason;
    try {
        if (step(&reason)) return true;
    } catch (const std::exception& exception) {
        reason = QString::fromUtf8(exception.what());
    } catch (...) {
        reason = QStringLiteral("发生未知异常");
    }
    return fail(error, reason.isEmpty() ? stage + QStringLiteral("失败")
                                       : QStringLiteral("%1失败：%2").arg(stage, reason));
}

QString CPreviewExportTransaction::CombinedError(const QString& operation, const QString& recovery)
{
    if (operation.isEmpty()) return recovery;
    if (recovery.isEmpty()) return operation;
    return operation + QLatin1Char('\n') + recovery;
}

bool CPreviewExportTransaction::Restore(QString* error)
{
    while (m_recoveryStage != RecoveryStage::None) {
        bool restored = false;
        switch (m_recoveryStage) {
        case RecoveryStage::EndTemporary:
            restored = Invoke(m_steps.endTemporary, QStringLiteral("恢复原模型"), error);
            if (restored) m_recoveryStage = RecoveryStage::RestorePreview;
            break;
        case RecoveryStage::RestorePreview:
            restored = Invoke(m_steps.restorePreview, QStringLiteral("恢复预览状态"), error);
            if (restored) m_recoveryStage = RecoveryStage::Resume;
            break;
        case RecoveryStage::Resume:
            restored = Invoke(m_steps.resume, QStringLiteral("恢复计时器与输入"), error);
            if (restored) m_recoveryStage = RecoveryStage::None;
            break;
        case RecoveryStage::None:
            break;
        }
        if (!restored) {
            m_needsRecovery = true;
            return false;
        }
    }
    Clear();
    return true;
}

void CPreviewExportTransaction::Clear()
{
    m_steps = {};
    m_operationError.clear();
    m_recoveryStage = RecoveryStage::None;
    m_needsRecovery = false;
}
