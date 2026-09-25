#include "cexportcontrol.h"

#include <exception>
#include <utility>

void CExportControl::Cancel() noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cancelRequested = true;
}

bool CExportControl::IsCancellationRequested() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cancelRequested;
}

bool CExportControl::WasCanceled() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state == State::Canceled;
}

bool CExportControl::IsCommitted() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state == State::Committed;
}

bool CExportControl::Checkpoint()
{
    std::shared_ptr<Handler> handler;
    Progress progress;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state == State::Committing || m_state == State::Committed) return true;
        if (m_state != State::Active) return false;
        if (m_cancelRequested) {
            m_state = State::Canceled;
            m_error = QStringLiteral("导出已取消，未写出文件");
            return false;
        }
        if (!m_handler) return true;
        if (m_inHandler) {
            m_state = State::Failed;
            m_error = QStringLiteral("导出检查点不能重入");
            return false;
        }
        handler = m_handler;
        progress = m_progress;
        m_inHandler = true;
    }
    bool accepted = false;
    QString reason;
    try {
        accepted = (*handler)(progress, &reason);
    } catch (const std::exception& exception) {
        reason = QString::fromUtf8(exception.what());
    } catch (...) {
        reason = QStringLiteral("导出检查点发生未知异常");
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_inHandler = false;
    if (m_state != State::Active) return false;
    if (!accepted) {
        m_state = State::Failed;
        m_error = reason.isEmpty() ? QStringLiteral("导出检查点失败") : reason;
        return false;
    }
    if (m_cancelRequested) {
        m_state = State::Canceled;
        m_error = QStringLiteral("导出已取消，未写出文件");
        return false;
    }
    return true;
}

bool CExportControl::Report(Phase phase, qint64 current, qint64 total, const QString& detail)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state != State::Active) return m_state == State::Committed;
        m_progress = {phase, qMax(qint64(0), current), qMax(qint64(0), total), detail};
    }
    return Checkpoint();
}

bool CExportControl::BeginCommit()
{
    if (!Report(Phase::Committing)) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != State::Active) return false;
    if (m_cancelRequested) {
        m_state = State::Canceled;
        m_error = QStringLiteral("导出已取消，未写出文件");
        return false;
    }
    m_state = State::Committing;
    return true;
}

void CExportControl::CommitFinished(bool success, const QString& error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != State::Committing) return;
    m_state = success ? State::Committed : State::Failed;
    if (!success) m_error = error.isEmpty() ? QStringLiteral("无法提交导出文件") : error;
}

void CExportControl::Fail(const QString& error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != State::Active && m_state != State::Committing && m_state != State::Canceled) return;
    const QString cancellation = m_state == State::Canceled ? m_error + QLatin1Char('\n') : QString();
    m_state = State::Failed;
    m_error = cancellation + (error.isEmpty() ? QStringLiteral("导出失败，未提供详细原因") : error);
}

QString CExportControl::Error() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_error;
}

CExportControl::Progress CExportControl::Snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_progress;
}

bool CExportControl::SetHandler(Handler handler)
{
    auto replacement = handler ? std::make_shared<Handler>(std::move(handler)) : nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_inHandler || m_state != State::Active) return false;
        m_handler.swap(replacement);
    }
    return true;
}
