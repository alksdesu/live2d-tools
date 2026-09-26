#include "ckeyformgridsnapshot.h"
#include <utility>

namespace live2dstruct {
bool CKeyformGridSnapshot::Pose(int keyform, QMap<QString, float>& values, QString* error) const
{
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (!m_bindings) return fail(QStringLiteral("尚未加载关键形网格"));
    QMap<QString, float> candidate;
    if (!m_bindings->Pose(m_type, objectId, keyform, candidate, error)) return false;
    for (auto value = candidate.cbegin(); value != candidate.cend(); ++value) {
        const auto range = m_ranges.constFind(value.key());
        if (range == m_ranges.cend()) return fail(QStringLiteral("关键形引用的参数不存在：%1").arg(value.key()));
        if (value.value() < range->first || value.value() > range->second)
            return fail(QStringLiteral("关键形的参数值超出当前范围：%1 = %2").arg(value.key()).arg(value.value()));
    }
    values = std::move(candidate);
    if (error) error->clear();
    return true;
}
}
