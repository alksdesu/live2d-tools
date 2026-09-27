#include "cmaterialeditcontroller.h"
#include <QScopedValueRollback>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}
bool changes(const CModelDocument& before, const CModelDocument& after, QSet<QString>& result, QString* error)
{
    CModelMaterialState first, second;
    if (!before.CaptureDrawableMaterials(first, error) || !after.CaptureDrawableMaterials(second, error)) return false;
    if (first.size() != second.size()) return fail(error, QStringLiteral("UV 编辑期间网格目录已改变"));
    QSet<QString> ids;
    for (qsizetype index = 0; index < first.size(); ++index) {
        if (first[index].id != second[index].id) return fail(error, QStringLiteral("UV 编辑期间网格顺序已改变"));
        if (first[index] != second[index]) ids.insert(first[index].id);
    }
    result.swap(ids);
    return true;
}
}

struct CMaterialEditController::State
{
    struct Draft {
        CModelDocument baseline;
        CModelDocument preview;
        quint64 revision = 0;
    };
    CModelDocument* document = nullptr;
    QByteArray source;
    Publisher publisher;
    std::unique_ptr<Draft> draft;
    bool alive = true;
    bool publishing = false;

    bool Ready(QString* error) const
    {
        if (!alive) return fail(error, QStringLiteral("UV 编辑已关闭"));
        if (publishing) return fail(error, QStringLiteral("UV 预览正在提交，请稍后重试"));
        if (!document || !document->IsLoaded() || document->SourceDigest() != source)
            return fail(error, QStringLiteral("UV 编辑的模型来源已改变"));
        if (!publisher) return fail(error, QStringLiteral("UV 编辑尚未连接模型预览"));
        return true;
    }

    bool DraftReady(QString* error) const
    {
        if (!Ready(error)) return false;
        if (!draft) return fail(error, QStringLiteral("请先打开 UV 编辑"));
        if (document->Revision() != draft->revision || document->SourcePath() != draft->baseline.SourcePath())
            return fail(error, QStringLiteral("模型已改变，当前 UV 草稿不能继续提交"));
        return true;
    }

    bool Publish(const CModelDocument& before, const CModelDocument& after, QString* error)
    {
        if (!Ready(error)) return false;
        QSet<QString> ids;
        if (!changes(before, after, ids, error)) return false;
        auto* target = document;
        const auto revision = target->Revision(); const auto path = target->SourcePath();
        const auto handler = publisher;
        const QScopedValueRollback<bool> guard(publishing, true);
        try {
            if (!handler(before, after, ids, error)) return false;
        } catch (...) {
            return fail(error, QStringLiteral("UV 预览准备失败，草稿保持不变"));
        }
        if (!alive || document != target) return fail(error, QStringLiteral("UV 编辑已关闭或更换模型"));
        if (target->Revision() != revision || target->SourceDigest() != source || target->SourcePath() != path)
            return fail(error, QStringLiteral("UV 预览提交期间文档已改变"));
        return true;
    }

    bool Prepare(const CModelMaterialState& requestedState, CModelDocument& candidate, QString* error) const
    {
        if (!DraftReady(error)) return false;
        CModelMaterialState before, after, baseline;
        if (!draft->baseline.CaptureDrawableMaterials(baseline, error)) return false;
        QHash<QString, CModelDrawableMaterial> byId;
        for (const auto& value : baseline) byId.insert(value.id, value);
        QSet<QString> ids;
        for (const auto& requested : requestedState) {
            if (ids.contains(requested.id)) return fail(error, QStringLiteral("UV 草稿包含重复网格：%1").arg(requested.id));
            ids.insert(requested.id);
            const auto current = byId.constFind(requested.id);
            if (current == byId.cend()) return fail(error, QStringLiteral("UV 草稿引用未知网格：%1").arg(requested.id));
            if (*current != requested) { before.append(*current); after.append(requested); }
        }
        auto result = draft->baseline;
        if (!after.isEmpty()) {
            CModelEdit edit;
            if (!result.MakeMaterialEdit(before, after, edit, error)) return false;
            if (!result.ExecuteEdit(edit)) return fail(error, QStringLiteral("UV 草稿未通过文档校验"));
        }
        candidate = std::move(result);
        return true;
    }
};

CMaterialEditController::CMaterialEditController() : m_state(std::make_shared<State>()) {}
CMaterialEditController::~CMaterialEditController()
{
    m_state->alive = false; m_state->document = nullptr; m_state->publisher = {};
}

void CMaterialEditController::SetDocument(CModelDocument* document)
{
    const auto state = m_state;
    if (state->publishing || state->draft) return;
    auto source = document ? document->SourceDigest() : QByteArray();
    state->document = document; state->source.swap(source);
}

void CMaterialEditController::SetPublisher(Publisher publisher)
{
    const auto state = m_state;
    if (!state->publishing && !state->draft) state->publisher = std::move(publisher);
}

bool CMaterialEditController::IsActive() const { return bool(m_state->draft); }
const CModelDocument* CMaterialEditController::PreviewDocument() const { return m_state->draft ? &m_state->draft->preview : nullptr; }

bool CMaterialEditController::Begin(QString* error)
try
{
    const auto state = m_state;
    if (!state->Ready(error)) return false;
    if (state->draft) return fail(error, QStringLiteral("已有 UV 草稿尚未结束"));
    auto candidate = std::make_unique<State::Draft>(State::Draft{*state->document, *state->document, state->document->Revision()});
    if (!state->Publish(candidate->baseline, candidate->preview, error)) return false;
    state->draft.swap(candidate);
    return true;
}
catch (...) { return fail(error, QStringLiteral("无法准备 UV 编辑，当前状态保持不变")); }

bool CMaterialEditController::Preview(const CModelMaterialState& requested, QString* error)
try
{
    const auto state = m_state;
    CModelDocument candidate;
    if (!state->Prepare(requested, candidate, error) || !state->Publish(state->draft->preview, candidate, error)) return false;
    state->draft->preview = std::move(candidate);
    return true;
}
catch (...) { return fail(error, QStringLiteral("无法准备 UV 编辑，当前状态保持不变")); }

bool CMaterialEditController::Commit(const CModelMaterialState& requested, QString* error)
try
{
    const auto state = m_state;
    CModelDocument candidate;
    if (!state->Prepare(requested, candidate, error) || !state->Publish(state->draft->preview, candidate, error)) return false;
    if (candidate.Revision() != state->draft->revision) state->document->CommitCandidate(std::move(candidate));
    state->draft.reset();
    return true;
}
catch (...) { return fail(error, QStringLiteral("无法准备 UV 编辑，当前状态保持不变")); }

bool CMaterialEditController::Cancel(QString* error)
try
{
    const auto state = m_state;
    if (!state->draft) return state->Ready(error);
    if (!state->DraftReady(error) || !state->Publish(state->draft->preview, state->draft->baseline, error)) return false;
    state->draft.reset();
    return true;
}
catch (...) { return fail(error, QStringLiteral("无法准备 UV 编辑，当前状态保持不变")); }

bool CMaterialEditController::History(bool undo, CModelEdit& edit, QString* error)
try
{
    const auto state = m_state;
    if (!state->Ready(error)) return false;
    if (state->draft) return fail(error, QStringLiteral("请先保存或取消当前 UV 草稿"));
    CModelEdit pending;
    if (!(undo ? state->document->PeekUndoEdit(&pending) : state->document->PeekRedoEdit(&pending)) || pending.field != CModelEdit::DrawableMaterials)
        return fail(error, QStringLiteral("当前历史记录不是 UV 或纹理槽修改"));
    auto candidate = *state->document;
    if (!(undo ? candidate.UndoEdit(&pending) : candidate.RedoEdit(&pending))) return fail(error, QStringLiteral("UV 历史记录与当前文档不一致"));
    if (!state->Publish(*state->document, candidate, error)) return false;
    state->document->CommitCandidate(std::move(candidate));
    edit = std::move(pending);
    return true;
}
catch (...) { return fail(error, QStringLiteral("无法准备 UV 编辑，当前状态保持不变")); }
}
