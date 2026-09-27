#include "catlaseditcontroller.h"
#include <QScopedValueRollback>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}
bool changedMaterials(const CModelDocument& before, const CModelDocument& after, QSet<QString>& result, QString* error)
{
    CModelMaterialState first, second;
    if (!before.CaptureDrawableMaterials(first, error) || !after.CaptureDrawableMaterials(second, error)) return false;
    if (first.size() != second.size()) return fail(error, QStringLiteral("图集编辑期间网格目录已改变"));
    QSet<QString> ids;
    for (qsizetype index = 0; index < first.size(); ++index) {
        if (first[index].id != second[index].id) return fail(error, QStringLiteral("图集编辑期间网格顺序已改变"));
        if (first[index] != second[index]) ids.insert(first[index].id);
    }
    result.swap(ids);
    return true;
}
}

struct CAtlasEditController::State
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
        if (!alive) return fail(error, QStringLiteral("图集编辑已关闭"));
        if (publishing) return fail(error, QStringLiteral("图集预览正在提交，请稍后重试"));
        if (!document || !document->IsLoaded() || document->SourceDigest() != source)
            return fail(error, QStringLiteral("图集编辑的模型来源已改变"));
        if (!document->HasTextureWorkspace()) return fail(error, QStringLiteral("模型图集尚未就绪"));
        if (!publisher) return fail(error, QStringLiteral("图集编辑尚未连接模型预览"));
        return true;
    }

    bool DraftReady(QString* error) const
    {
        if (!Ready(error)) return false;
        if (!draft) return fail(error, QStringLiteral("请先打开图集编辑"));
        if (document->Revision() != draft->revision || document->SourcePath() != draft->baseline.SourcePath() ||
            document->TextureSources() != draft->baseline.TextureSources())
            return fail(error, QStringLiteral("模型或素材来源已改变，当前图集草稿不能继续提交"));
        return true;
    }

    bool Publish(const CModelDocument& before, const CModelDocument& after, QString* error)
    {
        if (!Ready(error)) return false;
        QSet<QString> ids;
        if (!changedMaterials(before, after, ids, error)) return false;
        auto* target = document;
        const auto revision = target->Revision();
        const auto path = target->SourcePath();
        const auto sources = target->TextureSources();
        const auto handler = publisher;
        const QScopedValueRollback<bool> guard(publishing, true);
        try {
            if (!handler(before, after, ids, error)) return false;
        } catch (...) {
            return fail(error, QStringLiteral("图集预览准备失败，草稿保持不变"));
        }
        if (!alive || document != target) return fail(error, QStringLiteral("图集编辑已关闭或更换模型"));
        if (target->Revision() != revision || target->SourceDigest() != source || target->SourcePath() != path ||
            target->TextureSources() != sources)
            return fail(error, QStringLiteral("图集预览提交期间文档或素材来源已改变"));
        return true;
    }

    bool Prepare(const CModelDocument& baseline, const CModelAtlasState& requested, CModelDocument& result, QString* error) const
    {
        CModelAtlasState before;
        if (!baseline.CaptureAtlasState(before, error)) return false;
        if (requested.materials.size() != before.materials.size())
            return fail(error, QStringLiteral("图集草稿必须包含全部网格材质"));
        for (qsizetype index = 0; index < before.materials.size(); ++index)
            if (requested.materials[index].id != before.materials[index].id)
                return fail(error, QStringLiteral("图集草稿的网格目录或顺序不一致"));
        auto candidate = baseline;
        if (before != requested) {
            CModelEdit edit;
            if (!candidate.MakeAtlasEdit(before, requested, edit, error)) return false;
            if (!candidate.ExecuteEdit(edit)) return fail(error, QStringLiteral("图集草稿未通过文档校验"));
        }
        result = std::move(candidate);
        return true;
    }
};

CAtlasEditController::CAtlasEditController() : m_state(std::make_shared<State>()) {}
CAtlasEditController::~CAtlasEditController()
{
    m_state->alive = false;
    m_state->document = nullptr;
    m_state->publisher = {};
}

void CAtlasEditController::SetDocument(CModelDocument* document)
{
    const auto state = m_state;
    if (state->publishing || state->draft) return;
    auto source = document ? document->SourceDigest() : QByteArray();
    state->document = document;
    state->source.swap(source);
}

void CAtlasEditController::SetPublisher(Publisher publisher)
{
    const auto state = m_state;
    if (!state->publishing && !state->draft) state->publisher = std::move(publisher);
}

bool CAtlasEditController::IsActive() const { return bool(m_state->draft); }
const CModelDocument* CAtlasEditController::PreviewDocument() const { return m_state->draft ? &m_state->draft->preview : nullptr; }

bool CAtlasEditController::Begin(QString* error) try
{
    const auto state = m_state;
    if (!state->Ready(error)) return false;
    if (state->draft) return fail(error, QStringLiteral("已有图集草稿尚未结束"));
    auto candidate = std::make_unique<State::Draft>(State::Draft{*state->document, *state->document, state->document->Revision()});
    if (!state->Publish(candidate->baseline, candidate->preview, error)) return false;
    state->draft.swap(candidate);
    return true;
}
catch (...) { return fail(error, QStringLiteral("无法准备图集编辑，当前状态保持不变")); }

bool CAtlasEditController::Preview(const CModelAtlasState& requested, QString* error) try
{
    const auto state = m_state;
    if (!state->DraftReady(error)) return false;
    CModelDocument candidate;
    if (!state->Prepare(state->draft->baseline, requested, candidate, error) ||
        !state->Publish(state->draft->preview, candidate, error)) return false;
    state->draft->preview = std::move(candidate);
    return true;
}
catch (...) { return fail(error, QStringLiteral("无法准备图集编辑，当前状态保持不变")); }

bool CAtlasEditController::Commit(const CModelAtlasState& requested, QString* error) try
{
    const auto state = m_state;
    if (!state->DraftReady(error)) return false;
    CModelDocument candidate;
    if (!state->Prepare(state->draft->baseline, requested, candidate, error) ||
        !state->Publish(state->draft->preview, candidate, error)) return false;
    if (candidate.Revision() != state->draft->revision) state->document->CommitCandidate(std::move(candidate));
    state->draft.reset();
    return true;
}
catch (...) { return fail(error, QStringLiteral("无法准备图集编辑，当前状态保持不变")); }

bool CAtlasEditController::Submit(const CModelAtlasState& requested, QString* error) try
{
    const auto state = m_state;
    if (!state->Ready(error)) return false;
    if (state->draft) return fail(error, QStringLiteral("请先保存或取消当前图集草稿"));
    const auto revision = state->document->Revision();
    CModelDocument candidate;
    if (!state->Prepare(*state->document, requested, candidate, error) ||
        !state->Publish(*state->document, candidate, error)) return false;
    if (candidate.Revision() != revision) state->document->CommitCandidate(std::move(candidate));
    return true;
}
catch (...) { return fail(error, QStringLiteral("无法提交图集修改，当前状态保持不变")); }

bool CAtlasEditController::Cancel(QString* error) try
{
    const auto state = m_state;
    if (!state->draft) return state->Ready(error);
    if (!state->DraftReady(error) || !state->Publish(state->draft->preview, state->draft->baseline, error)) return false;
    state->draft.reset();
    return true;
}
catch (...) { return fail(error, QStringLiteral("无法恢复进入时图集，请保留草稿并重试")); }

bool CAtlasEditController::History(bool undo, CModelEdit& edit, QString* error) try
{
    const auto state = m_state;
    if (!state->Ready(error)) return false;
    if (state->draft) return fail(error, QStringLiteral("请先保存或取消当前图集草稿"));
    CModelEdit pending;
    if (!(undo ? state->document->PeekUndoEdit(&pending) : state->document->PeekRedoEdit(&pending)) || pending.field != CModelEdit::AtlasLayout)
        return fail(error, QStringLiteral("当前历史记录不是图集修改"));
    auto candidate = *state->document;
    if (!(undo ? candidate.UndoEdit(&pending) : candidate.RedoEdit(&pending)))
        return fail(error, QStringLiteral("图集历史记录与当前文档不一致"));
    if (!state->Publish(*state->document, candidate, error)) return false;
    state->document->CommitCandidate(std::move(candidate));
    edit = std::move(pending);
    return true;
}
catch (...) { return fail(error, QStringLiteral("无法恢复图集历史，当前状态保持不变")); }
}
