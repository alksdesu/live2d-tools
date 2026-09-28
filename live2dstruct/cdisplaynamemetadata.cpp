#include "cdisplaynamemetadata.h"

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}
bool payload(const QJsonValue& value, QJsonObject& names, QString* error)
{
    const auto object = value.toObject();
    if (!value.isObject() || object.value(QStringLiteral("Version")) != QJsonValue(1) ||
        !object.value(QStringLiteral("Names")).isObject()) return fail(error, QStringLiteral("对象名称扩展格式或版本无效"));
    names = object.value(QStringLiteral("Names")).toObject();
    return true;
}
}

bool CDisplayNameMetadata::Read(const QJsonObject& root, Snapshot& result, QString* error)
{
    if (error) error->clear();
    Snapshot candidate;
    const auto full = root.value(QStringLiteral("Live2DToolsDisplayNames"));
    const auto legacy = root.value(QStringLiteral("Live2DToolsDeformerNames"));
    QJsonObject names;
    if (!full.isUndefined()) {
        candidate.names.emplace();
        if (!payload(full, names, error) || !CModelDisplayNames::FromJson(names, *candidate.names, error)) return false;
    }
    if (!legacy.isUndefined()) {
        candidate.deformerNames.emplace();
        if (!payload(legacy, names, error) || !CModelDisplayNames::MapFromJson(names, *candidate.deformerNames, error)) return false;
    }
    if (candidate.names && candidate.deformerNames) {
        CModelDisplayNames::Map current, previous;
        if (!CModelDisplayNames::NormalizeMap(candidate.names->deformers, current, error) ||
            !CModelDisplayNames::NormalizeMap(*candidate.deformerNames, previous, error)) return false;
        if (current != previous) return fail(error, QStringLiteral("对象名称与变形器名称扩展不一致"));
    }
    result = std::move(candidate);
    return true;
}

bool CDisplayNameMetadata::Write(const CModelDisplayNames::State& names, QJsonObject& root, QString* error)
{
    if (error) error->clear();
    QJsonObject full, deformers;
    if (!CModelDisplayNames::ToJson(names, full, error) || !CModelDisplayNames::MapToJson(names.deformers, deformers, error)) return false;
    auto candidate = root;
    candidate.insert(QStringLiteral("Live2DToolsDisplayNames"), QJsonObject{{QStringLiteral("Version"), 1}, {QStringLiteral("Names"), full}});
    candidate.insert(QStringLiteral("Live2DToolsDeformerNames"), QJsonObject{{QStringLiteral("Version"), 1}, {QStringLiteral("Names"), deformers}});
    root = std::move(candidate);
    return true;
}
}
