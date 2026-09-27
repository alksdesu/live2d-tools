#include "ctextureeditcontroller.h"
#include <QScopedValueRollback>
#include <exception>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
}

bool CTextureEditController::Initialize(const Catalog& sources, const State& state, QString* error)
{
    if (m_publishing) return fail(error, QStringLiteral("正在提交纹理修改"));
    if (!CTextureAssets::ValidateCatalog(sources, error) || !CTextureAssets::ValidateState(sources, state, error)) return false;
    CTextureEditController candidate;
    candidate.m_sources = sources;
    candidate.m_state = candidate.m_saved = state;
    candidate.m_publisher = m_publisher;
    candidate.m_revision = m_revision + 1;
    Swap(candidate);
    if (error) error->clear();
    return true;
}

void CTextureEditController::SetPublisher(Publisher publisher)
{
    if (!m_publishing) m_publisher = std::move(publisher);
}

bool CTextureEditController::Replace(int slot, const QByteArray& png, QString* error)
{
    if (m_publishing) return fail(error, QStringLiteral("正在提交纹理修改"));
    State next;
    if (!CTextureAssets::Replace(m_sources, m_state, slot, png, next, error)) return false;
    if (next == m_state) return true;
    auto candidate = *this;
    candidate.m_history.resize(m_index);
    candidate.m_history.append({m_state, next, slot});
    candidate.m_index = candidate.m_history.size();
    candidate.m_state.swap(next);
    candidate.m_lastSlot = slot;
    ++candidate.m_revision;
    return Publish(std::move(candidate), error);
}

bool CTextureEditController::Undo(QString* error)
{
    if (!CanUndo()) return fail(error, QStringLiteral("当前没有可撤销的纹理修改"));
    auto candidate = *this;
    const auto& change = m_history[m_index - 1];
    candidate.m_state = change.before;
    candidate.m_lastSlot = change.slot;
    --candidate.m_index;
    ++candidate.m_revision;
    return Publish(std::move(candidate), error);
}

bool CTextureEditController::Redo(QString* error)
{
    if (!CanRedo()) return fail(error, QStringLiteral("当前没有可重做的纹理修改"));
    auto candidate = *this;
    const auto& change = m_history[m_index];
    candidate.m_state = change.after;
    candidate.m_lastSlot = change.slot;
    ++candidate.m_index;
    ++candidate.m_revision;
    return Publish(std::move(candidate), error);
}

bool CTextureEditController::PrepareRestore(const CTextureAssets::Draft& draft, CTextureEditController& result, QString* error) const
{
    if (m_publishing || &result == this) return fail(error, QStringLiteral("当前不能准备纹理恢复"));
    State next;
    if (!CTextureAssets::RestoreDraft(m_sources, draft, next, error)) return false;
    CTextureEditController candidate;
    candidate.m_sources = m_sources;
    candidate.m_state = candidate.m_saved = std::move(next);
    candidate.m_publisher = m_publisher;
    candidate.m_revision = m_revision + 1;
    result.Swap(candidate);
    if (error) error->clear();
    return true;
}

bool CTextureEditController::Publish(CTextureEditController candidate, QString* error)
{
    if (!m_publisher) return fail(error, QStringLiteral("纹理预览尚未连接"));
    const auto revision = m_revision;
    {
        const QScopedValueRollback<bool> publishing(m_publishing, true);
        try {
            if (!m_publisher(candidate.m_state, error)) return false;
        } catch (const std::exception&) { return fail(error, QStringLiteral("纹理预览提交失败，原编辑状态保持不变")); }
        if (m_revision != revision) return fail(error, QStringLiteral("纹理提交期间文档已改变"));
    }
    Swap(candidate);
    if (error) error->clear();
    return true;
}

void CTextureEditController::Swap(CTextureEditController& other) noexcept
{
    m_sources.swap(other.m_sources);
    m_state.swap(other.m_state);
    m_saved.swap(other.m_saved);
    m_history.swap(other.m_history);
    m_publisher.swap(other.m_publisher);
    std::swap(m_index, other.m_index);
    std::swap(m_lastSlot, other.m_lastSlot);
    std::swap(m_revision, other.m_revision);
    std::swap(m_publishing, other.m_publishing);
}

void CTextureEditController::MarkSaved()
{
    if (!m_publishing) m_saved = m_state;
}
}
