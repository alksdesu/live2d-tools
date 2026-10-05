#include "cglueeditcontroller.h"
#include <QScopedValueRollback>
#include <exception>
#include <utility>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool weight(const CGlueEditController::Snapshot& glues,
            const CGlueEditController::WeightEdit& edit, float& result, QString* error)
{
    if (edit.glueIndex < 0 || edit.glueIndex >= glues.size())
        return fail(error, QStringLiteral("Glue 索引无效：%1").arg(edit.glueIndex));
    const auto& glue = glues[edit.glueIndex];
    if (edit.pairIndex < 0 || edit.pairIndex >= glue.pairs.size())
        return fail(error, QStringLiteral("Glue %1 的配对索引无效：%2").arg(glue.id).arg(edit.pairIndex));
    if (edit.side != CGlueEditController::Side::A && edit.side != CGlueEditController::Side::B)
        return fail(error, QStringLiteral("Glue 端点方向无效"));
    if (edit.side == CGlueEditController::Side::A) result = glue.pairs[edit.pairIndex].a.weight;
    else result = glue.pairs[edit.pairIndex].b.weight;
    return true;
}
}

bool CGlueEditController::Initialize(const QByteArray& base, QString* error)
{
    if (m_publishing) return fail(error, QStringLiteral("正在提交 Glue 修改"));
    Snapshot glues;
    if (!CMocMeshStructureBuilder::CaptureGlues(base, glues, error)) return false;
    CGlueEditController candidate;
    candidate.m_source = candidate.m_state = candidate.m_saved = base;
    candidate.m_glues = std::move(glues);
    candidate.m_publisher = m_publisher;
    candidate.m_revision = m_revision + 1;
    Swap(candidate);
    if (error) error->clear();
    return true;
}

void CGlueEditController::SetPublisher(Publisher publisher)
{
    if (!m_publishing) m_publisher = std::move(publisher);
}

bool CGlueEditController::SetWeight(int glueIndex, int pairIndex, Side side, float value, QString* error)
{
    return Apply({WeightEdit{glueIndex, pairIndex, side, value}}, error);
}

bool CGlueEditController::Apply(const QVector<WeightEdit>& edits, QString* error)
{
    if (m_publishing) return fail(error, QStringLiteral("正在提交 Glue 修改"));
    if (edits.isEmpty()) return fail(error, QStringLiteral("没有选定 Glue 端点"));
    Request request;
    request.glueWeights = edits;
    CMocMeshStructureBuilder::Result built;
    if (!CMocMeshStructureBuilder::Build(m_state, request, built, error)) return false;
    if (built.bytes == m_state) return fail(error, QStringLiteral("Glue 权重没有发生变化"));
    Snapshot after;
    if (!CMocMeshStructureBuilder::CaptureGlues(built.bytes, after, error)) return false;
    Request reverse;
    reverse.glueWeights.reserve(edits.size());
    for (const auto& edit : edits) {
        float previous = 0;
        if (!weight(m_glues, edit, previous, error)) return false;
        reverse.glueWeights.append({edit.glueIndex, edit.pairIndex, edit.side, previous});
    }
    auto candidate = *this;
    candidate.m_history.resize(m_index);
    candidate.m_history.append({m_state, built.bytes, m_glues, after, request, reverse});
    candidate.m_index = candidate.m_history.size();
    candidate.m_state = built.bytes;
    candidate.m_glues = std::move(after);
    candidate.m_lastRequest = request;
    ++candidate.m_revision;
    return Publish(std::move(candidate), request, error);
}

bool CGlueEditController::Undo(QString* error)
{
    if (!CanUndo()) return fail(error, QStringLiteral("当前没有可撤销的 Glue 修改"));
    auto candidate = *this;
    const auto& change = m_history[m_index - 1];
    candidate.m_state = change.beforeBytes;
    candidate.m_glues = change.beforeGlues;
    candidate.m_lastRequest = change.reverse;
    --candidate.m_index;
    ++candidate.m_revision;
    return Publish(std::move(candidate), change.reverse, error);
}

bool CGlueEditController::Redo(QString* error)
{
    if (!CanRedo()) return fail(error, QStringLiteral("当前没有可重做的 Glue 修改"));
    auto candidate = *this;
    const auto& change = m_history[m_index];
    candidate.m_state = change.afterBytes;
    candidate.m_glues = change.afterGlues;
    candidate.m_lastRequest = change.forward;
    ++candidate.m_index;
    ++candidate.m_revision;
    return Publish(std::move(candidate), change.forward, error);
}

bool CGlueEditController::Publish(CGlueEditController candidate, const Request& request, QString* error)
{
    if (!m_publisher) return fail(error, QStringLiteral("Glue 预览尚未连接"));
    const auto revision = m_revision;
    {
        const QScopedValueRollback<bool> publishing(m_publishing, true);
        try {
            if (!m_publisher(m_state, request, candidate.m_state, error)) return false;
        } catch (const std::exception&) {
            return fail(error, QStringLiteral("Glue 预览提交失败，原编辑状态保持不变"));
        }
        if (m_revision != revision) return fail(error, QStringLiteral("Glue 提交期间文档已改变"));
    }
    Swap(candidate);
    if (error) error->clear();
    return true;
}

void CGlueEditController::Swap(CGlueEditController& other) noexcept
{
    m_source.swap(other.m_source);
    m_state.swap(other.m_state);
    m_saved.swap(other.m_saved);
    m_glues.swap(other.m_glues);
    m_history.swap(other.m_history);
    m_publisher.swap(other.m_publisher);
    std::swap(m_lastRequest, other.m_lastRequest);
    std::swap(m_index, other.m_index);
    std::swap(m_revision, other.m_revision);
    std::swap(m_publishing, other.m_publishing);
}

void CGlueEditController::MarkSaved()
{
    if (!m_publishing) m_saved = m_state;
}
}
