#ifndef CUBISMPARAMETERREFERENCEMASK_HPP
#define CUBISMPARAMETERREFERENCEMASK_HPP

#include "CubismParameterReferences.hpp"
#include <QVector>

namespace Live2D { namespace Cubism { namespace Framework {

template<class IdAt>
bool PrepareParameterReferenceMask(const QByteArray& digest, const ParameterResourceReferences& references,
                                   int count, IdAt idAt, QVector<quint8>& result, QString* error)
{
    if (count < 0 || count > 1000000 || ((!references.sourceDigest.isEmpty() || !references.suppressedParameters.isEmpty()) &&
        (references.sourceDigest.size() != 32 || references.sourceDigest != digest))) {
        if (error) *error = QStringLiteral("参数引用的资源内容已改变或数量无效");
        return false;
    }
    QVector<quint8> candidate(count, 0);
    for (auto it = references.suppressedParameters.cbegin(); it != references.suppressedParameters.cend(); ++it) {
        if (it.key() < 0 || it.key() >= count || it.value().isEmpty() || it.value() != idAt(it.key())) {
            if (error) *error = QStringLiteral("参数引用行不存在、类型不符或 ID 已改变：%1").arg(it.value());
            return false;
        }
        candidate[it.key()] = 1;
    }
    result.swap(candidate);
    return true;
}

template<class IdAt>
bool PrepareParameterGroupMask(const QSet<QString>& suppressed, int count, IdAt idAt,
                               QVector<quint8>& result, QString* error)
{
    if (count < 0 || count > 1000000) return false;
    auto missing = suppressed;
    QVector<quint8> candidate(count, 0);
    if (suppressed.isEmpty()) { result.swap(candidate); return true; }
    for (int index = 0; index < count; ++index) {
        const QString id = idAt(index);
        if (!suppressed.contains(id)) continue;
        candidate[index] = 1;
        missing.remove(id);
    }
    if (!missing.isEmpty()) {
        if (error) *error = QStringLiteral("参数驱动项不存在：%1").arg(*missing.cbegin());
        return false;
    }
    result.swap(candidate);
    return true;
}

}}}

#endif
