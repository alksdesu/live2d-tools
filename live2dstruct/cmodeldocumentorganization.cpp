#include "cmodeldocument.h"
#include <exception>

namespace live2dstruct {
bool CModelDocument::SetParameterOrganization(const CParameterOrganization::State& state, QString* error) try
{
    if (error) error->clear();
    QSet<QString> ids;
    for (const auto& parameter : m_parameters) ids.insert(parameter.id);
    if (!m_loaded || ids.size() != m_parameters.size()) {
        if (error) *error = QStringLiteral("模型尚未加载或参数 ID 重复");
        return false;
    }
    CParameterOrganization::State candidate;
    if (!CParameterOrganization::Normalize(state, candidate, error) || !CParameterOrganization::Validate(candidate, ids, error)) return false;
    if (candidate == m_parameterOrganization) return true;
    m_parameterOrganization = std::move(candidate);
    ++m_revision;
    return true;
}
catch (const std::exception&) { if (error) *error = QStringLiteral("无法设置参数组织，原状态保持不变"); return false; }

bool CModelDocument::MakeParameterOrganizationEdit(const CParameterOrganization::State& state, CModelEdit& edit, QString* error) const try
{
    auto candidate = *this;
    if (!candidate.SetParameterOrganization(state, error)) return false;
    CModelEdit command{QStringLiteral("organization"), QStringLiteral("parameters"), CModelEdit::ParameterOrganization,
        QVariant::fromValue(m_parameterOrganization), QVariant::fromValue(candidate.m_parameterOrganization), -1, false};
    edit = std::move(command);
    return true;
}
catch (const std::exception&) { if (error) *error = QStringLiteral("无法准备参数组织修改"); return false; }
}
