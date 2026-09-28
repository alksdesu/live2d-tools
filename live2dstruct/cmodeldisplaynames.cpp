#include "cmodeldisplaynames.h"

namespace live2dstruct {
namespace {
constexpr qsizetype MaximumEntries = 1000000;
constexpr qint64 MaximumBytes = 16 * 1024 * 1024;
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
QString owned(const QString& value) { return QString(value.constData(), value.size()); }
bool entry(const QString& id, const QString& name, qint64& bytes, QString* error)
{
    if (id.isEmpty() || id.size() > 64 || id.contains(QChar::Null) || !id.isValidUtf16() || id.toUtf8().size() > 64)
        return fail(error, QStringLiteral("对象名称包含无效 ID"));
    if (!CModelDisplayNames::ValidateName(name, error)) return false;
    bytes += id.toUtf8().size() + name.toUtf8().size();
    return bytes <= MaximumBytes || fail(error, QStringLiteral("对象名称数据超出限制"));
}
bool map(const CModelDisplayNames::Map& names, qint64& bytes, qsizetype& count, QString* error)
{
    if (names.size() > MaximumEntries - count) return fail(error, QStringLiteral("对象名称数量超出限制"));
    count += names.size();
    for (auto item = names.cbegin(); item != names.cend(); ++item)
        if (!entry(item.key(), item.value(), bytes, error)) return false;
    return true;
}
CModelDisplayNames::Map normalized(const CModelDisplayNames::Map& names)
{
    CModelDisplayNames::Map result;
    for (auto item = names.cbegin(); item != names.cend(); ++item)
        if (item.key() != item.value()) result.insert(owned(item.key()), owned(item.value()));
    return result;
}
QJsonObject json(const CModelDisplayNames::Map& names)
{
    QJsonObject result;
    for (auto item = names.cbegin(); item != names.cend(); ++item) result.insert(item.key(), item.value());
    return result;
}
}

bool CModelDisplayNames::ValidateName(const QString& name, QString* error)
{
    if (error) error->clear();
    if (name.isEmpty() || name.trimmed().isEmpty()) return fail(error, QStringLiteral("请输入对象名称"));
    if (name.size() > MaximumNameLength) return fail(error, QStringLiteral("对象名称过长"));
    if (!name.isValidUtf16()) return fail(error, QStringLiteral("对象名称包含无效 Unicode 字符"));
    for (const auto character : name) {
        const auto category = character.category();
        if (category == QChar::Other_Control || category == QChar::Separator_Line || category == QChar::Separator_Paragraph ||
            character.unicode() == 0xfffe || character.unicode() == 0xffff)
            return fail(error, QStringLiteral("对象名称必须是单行文本，不能包含控制字符"));
    }
    return true;
}

bool CModelDisplayNames::ValidateMap(const Map& names, QString* error)
{
    if (error) error->clear();
    qint64 bytes = 0; qsizetype count = 0;
    return map(names, bytes, count, error);
}

bool CModelDisplayNames::NormalizeMap(const Map& names, Map& result, QString* error)
{
    if (!ValidateMap(names, error)) return false;
    auto candidate = normalized(names);
    result.swap(candidate);
    return true;
}

bool CModelDisplayNames::MapFromJson(const QJsonObject& object, Map& result, QString* error)
{
    if (error) error->clear();
    if (object.size() > MaximumEntries) return fail(error, QStringLiteral("对象名称数量超出限制"));
    Map candidate; qint64 bytes = 0;
    for (auto item = object.constBegin(); item != object.constEnd(); ++item) {
        if (!item.value().isString()) return fail(error, QStringLiteral("对象名称必须是文本"));
        const auto name = item.value().toString();
        if (!entry(item.key(), name, bytes, error)) return false;
        candidate.insert(owned(item.key()), owned(name));
    }
    result.swap(candidate);
    return true;
}

bool CModelDisplayNames::MapToJson(const Map& names, QJsonObject& result, QString* error)
{
    Map candidate;
    if (!NormalizeMap(names, candidate, error)) return false;
    result = json(candidate);
    return true;
}

bool CModelDisplayNames::Validate(const State& names, QString* error)
{
    if (error) error->clear();
    qint64 bytes = 0; qsizetype count = 0;
    return map(names.parts, bytes, count, error) && map(names.artMeshes, bytes, count, error) &&
        map(names.deformers, bytes, count, error);
}

bool CModelDisplayNames::Normalize(const State& names, State& result, QString* error)
{
    if (!Validate(names, error)) return false;
    State candidate{normalized(names.parts), normalized(names.artMeshes), normalized(names.deformers)};
    result = std::move(candidate);
    return true;
}

bool CModelDisplayNames::FromJson(const QJsonObject& object, State& result, QString* error)
{
    if (object.size() != 3 || !object.value("parts").isObject() || !object.value("artMeshes").isObject() ||
        !object.value("deformers").isObject()) return fail(error, QStringLiteral("对象名称目录字段无效"));
    State candidate;
    if (!MapFromJson(object.value("parts").toObject(), candidate.parts, error) ||
        !MapFromJson(object.value("artMeshes").toObject(), candidate.artMeshes, error) ||
        !MapFromJson(object.value("deformers").toObject(), candidate.deformers, error) || !Validate(candidate, error)) return false;
    result = std::move(candidate);
    return true;
}

bool CModelDisplayNames::ToJson(const State& names, QJsonObject& result, QString* error)
{
    State candidate;
    if (!Normalize(names, candidate, error)) return false;
    result = {{"parts", json(candidate.parts)}, {"artMeshes", json(candidate.artMeshes)}, {"deformers", json(candidate.deformers)}};
    return true;
}

QString CModelDisplayNames::EffectiveName(const State& names, const QString& type, const QString& id)
{
    if (type == QLatin1String("part")) return names.parts.value(id, id);
    if (type == QLatin1String("artmesh")) return names.artMeshes.value(id, id);
    if (type == QLatin1String("warp") || type == QLatin1String("rotation")) return names.deformers.value(id, id);
    return id;
}
}
