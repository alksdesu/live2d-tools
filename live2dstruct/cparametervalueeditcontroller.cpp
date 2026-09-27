#include "cparametervalueeditcontroller.h"
#include "cmodeldocument.h"
#include "cparameterorganization.h"
#include <QScopeGuard>
#include <QSet>
#include <cmath>
#include <optional>
#include <utility>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}
bool validValues(const CModelDocument& document, const QMap<QString, float>& values, QString* error)
{
    if (values.isEmpty()) return fail(error, QStringLiteral("请选择要修改的参数"));
    QHash<QString, QPair<float, float>> ranges;
    const auto& overrides = document.ParameterMetadataOverrides();
    for (const auto& parameter : document.Parameters()) {
        const auto metadata = overrides.constFind(parameter.id);
        ranges.insert(parameter.id, metadata == overrides.cend() ? qMakePair(parameter.minimum, parameter.maximum)
            : qMakePair(metadata->minimum, metadata->maximum));
    }
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const auto range = ranges.constFind(it.key());
        if (range == ranges.cend()) return fail(error, QStringLiteral("参数已不存在：%1").arg(it.key()));
        if (!std::isfinite(it.value()) || it.value() < range->first || it.value() > range->second)
            return fail(error, QStringLiteral("参数值超出有效范围：%1").arg(it.key()));
    }
    return true;
}
}

struct CParameterValueEditController::State
{
    CModelDocument* document = nullptr;
    Publisher publisher;
    Expander expander;
    BaselineProvider baseline;
    ContextDigestProvider context;
    bool alive = true;
    bool busy = false;
    quint64 token = 0;
    quint64 revision = 0;
    QByteArray sourceDigest, contextDigest;
    QStringList drivers;
    QMap<QString, float> before, accepted, initialDrivers;

    void clearGesture()
    {
        token = 0;
        revision = 0;
        sourceDigest.clear(); contextDigest.clear(); drivers.clear();
        before.clear(); accepted.clear(); initialDrivers.clear();
    }
};

CParameterValueEditController::CParameterValueEditController() : m_state(std::make_shared<State>()) {}
CParameterValueEditController::~CParameterValueEditController() { m_state->alive = false; m_state->document = nullptr; }
void CParameterValueEditController::SetDocument(CModelDocument* document)
{
    if (m_state->busy || m_state->document == document) return;
    m_state->clearGesture(); m_state->document = document;
}
void CParameterValueEditController::SetPublisher(Publisher publisher)
{
    if (!m_state->busy && !IsActive()) m_state->publisher = std::move(publisher);
}
void CParameterValueEditController::SetExpander(Expander expander)
{
    if (!m_state->busy && !IsActive()) m_state->expander = std::move(expander);
}
void CParameterValueEditController::SetBaselineProvider(BaselineProvider provider)
{
    if (!m_state->busy && !IsActive()) m_state->baseline = std::move(provider);
}
void CParameterValueEditController::SetContextDigestProvider(ContextDigestProvider provider)
{
    if (!m_state->busy && !IsActive()) m_state->context = std::move(provider);
}
bool CParameterValueEditController::IsActive() const { return m_state->token != 0; }
bool CParameterValueEditController::HasChanges() const { return IsActive() && m_state->before != m_state->accepted; }
quint64 CParameterValueEditController::GestureId() const { return m_state->token; }
void CParameterValueEditController::Reset() { if (!m_state->busy) m_state->clearGesture(); }

bool CParameterValueEditController::Apply(const Request& request, Result& result, QString* error)
{
    if (error) error->clear();
    const auto state = m_state;
    if (state->busy) return fail(error, QStringLiteral("参数值事务正在提交"));
    if (!state->document || !state->document->IsLoaded()) return fail(error, QStringLiteral("尚未载入参数模型"));
    state->busy = true;
    const auto release = qScopeGuard([state] { state->busy = false; });
    try {
        const bool start = request.phase == Phase::Single || request.phase == Phase::Begin;
        const bool valuesPhase = start || request.phase == Phase::Preview;
        switch (request.phase) {
        case Phase::Single: case Phase::Begin: case Phase::Preview: case Phase::Finish: case Phase::Cancel: break;
        default: return fail(error, QStringLiteral("参数值事务阶段无效"));
        }
        if (start && state->token) return fail(error, QStringLiteral("请先完成当前参数手势"));
        if ((request.phase == Phase::Single && request.gestureId != 0) ||
            (request.phase != Phase::Single && request.gestureId == 0) ||
            (!start && request.gestureId != state->token))
            return fail(error, QStringLiteral("参数手势已失效"));
        if (!valuesPhase && !request.drivers.isEmpty()) return fail(error, QStringLiteral("结束参数手势时不能追加输入"));
        auto* document = state->document;
        const auto revision = document->Revision();
        const auto sourceDigest = document->SourceDigest();
        const auto contextProvider = state->context;
        const auto contextDigest = contextProvider ? contextProvider() : QByteArray{};
        if (!state->alive || state->document != document) return fail(error, QStringLiteral("参数面板已关闭或更换模型"));
        if (!start && (revision != state->revision || sourceDigest != state->sourceDigest || contextDigest != state->contextDigest))
            return fail(error, QStringLiteral("模型、参数范围或配对规则已改变，请重新开始手势"));
        if (request.phase == Phase::Preview && request.drivers.keys() != state->drivers)
            return fail(error, QStringLiteral("参数手势的输入对象已改变"));
        QMap<QString, float> after;
        if (valuesPhase) {
            if (!validValues(*document, request.drivers, error)) return false;
            const auto expander = state->expander;
            if (!(expander ? expander(*document, request.drivers, after, error)
                           : CParameterOrganization::ExpandUserValues(*document, request.drivers, after, error))) return false;
            if (!state->alive || state->document != document) return fail(error, QStringLiteral("参数面板已关闭或更换模型"));
            if (!validValues(*document, after, error)) return false;
            for (auto it = request.drivers.cbegin(); it != request.drivers.cend(); ++it)
                if (!after.contains(it.key()) || after.value(it.key()) != it.value())
                    return fail(error, QStringLiteral("配对计算不能改写明确输入的参数：%1").arg(it.key()));
        } else after = request.phase == Phase::Cancel ? state->before : state->accepted;
        QMap<QString, float> before;
        if (start) {
            QMap<QString, float> baseline;
            const auto provider = state->baseline;
            if (provider && !provider(baseline, error)) return false;
            if (!state->alive || state->document != document) return fail(error, QStringLiteral("参数面板已关闭或更换模型"));
            if (!provider) {
                const auto& overrides = document->ParameterMetadataOverrides();
                for (const auto& parameter : document->Parameters()) {
                    const auto metadata = overrides.constFind(parameter.id);
                    const float fallback = metadata == overrides.cend() ? parameter.defaultValue : metadata->defaultValue;
                    baseline.insert(parameter.id, document->ParameterValues().value(parameter.id, fallback));
                }
            }
            for (auto it = after.cbegin(); it != after.cend(); ++it) {
                if (!baseline.contains(it.key())) return fail(error, QStringLiteral("参数基础值缺失：%1").arg(it.key()));
                before.insert(it.key(), baseline.value(it.key()));
            }
            if (!validValues(*document, before, error)) return false;
            if (request.phase == Phase::Begin) after = before;
        } else {
            before = state->before;
            if (after.keys() != before.keys()) return fail(error, QStringLiteral("参数配对的影响范围已改变"));
        }
        const bool returnedToStart = request.phase == Phase::Preview && request.drivers == state->initialDrivers;
        if (returnedToStart) after = before;
        const bool changed = before != after;
        const bool restoreContext = returnedToStart || request.phase == Phase::Cancel || (request.phase == Phase::Finish && !changed);
        const bool commit = request.phase == Phase::Single || request.phase == Phase::Finish;
        std::optional<CModelDocument> candidate;
        if (commit && changed) {
            candidate.emplace(*document);
            const CModelEdit edit{QStringLiteral("values"), QStringLiteral("parameters"), CModelEdit::ParameterValues,
                QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
            if (!candidate->ExecuteEdit(edit)) return fail(error, QStringLiteral("参数批量修改未通过模型校验"));
        }
        auto driverKeys = request.drivers.keys();
        auto initialDrivers = request.drivers;
        Publication publication{request.phase, request.gestureId, before, after, changed, restoreContext};
        Result accepted{after, changed, bool(candidate), restoreContext};
        const auto publisher = state->publisher;
        if (document->Revision() != revision || document->SourceDigest() != sourceDigest)
            return fail(error, QStringLiteral("参数值准备期间模型已改变"));
        if (publisher && !publisher(publication, error)) {
            if (error && error->isEmpty()) *error = QStringLiteral("参数预览未通过校验");
            return false;
        }
        if (!state->alive || state->document != document) return fail(error, QStringLiteral("参数面板已关闭或更换模型"));
        if (document->Revision() != revision || document->SourceDigest() != sourceDigest)
            return fail(error, QStringLiteral("参数发布器修改了文档，批量提交已取消"));
        if (candidate) document->CommitCandidate(std::move(*candidate));
        if (request.phase == Phase::Begin) {
            state->revision = revision; state->sourceDigest = sourceDigest; state->contextDigest = contextDigest;
            state->drivers.swap(driverKeys); state->before.swap(before); state->accepted.swap(after);
            state->initialDrivers.swap(initialDrivers);
            state->token = request.gestureId;
        } else if (request.phase == Phase::Preview) state->accepted.swap(after);
        else if (request.phase == Phase::Finish || request.phase == Phase::Cancel) state->clearGesture();
        result = std::move(accepted);
        return true;
    } catch (...) { return fail(error, QStringLiteral("参数值事务失败，未提交文档或历史")); }
}
}
