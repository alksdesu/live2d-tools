#include "cdeformerdisplaynames.h"

namespace live2dstruct {
namespace {
constexpr qsizetype MaximumEntries = 1000000;
constexpr qint64 MaximumBytes = 16 * 1024 * 1024;
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}
bool validUnicode(const QString& text)
{
    return QString::fromUtf8(text.toUtf8()) == text;
}
QString owned(const QString& value) { return QString(value.constData(), value.size()); }
bool validateEntry(const QString& id, const QString& name, qint64& bytes, QString* error)
{
    if (id.isEmpty() || id.size() > 64 || id.contains(QChar::Null) || !validUnicode(id) || id.toUtf8().size() > 64)
        return fail(error, QStringLiteral("变形器名称包含无效 ID"));
    if (!CDeformerDisplayNames::ValidateName(name, error)) return false;
    bytes += id.toUtf8().size() + name.toUtf8().size();
    if (bytes > MaximumBytes) return fail(error, QStringLiteral("变形器名称数据超出限制"));
    return true;
}
}

bool CDeformerDisplayNames::ValidateName(const QString& name, QString* error)
{
    if (error) error->clear();
    if (name.isEmpty() || name.trimmed().isEmpty()) return fail(error, QStringLiteral("请输入变形器名称"));
    if (name.size() > MaximumNameLength) return fail(error, QStringLiteral("变形器名称过长"));
    if (!validUnicode(name)) return fail(error, QStringLiteral("变形器名称包含无效 Unicode 字符"));
    for (const auto character : name) {
        const auto category = character.category();
        if (category == QChar::Other_Control || category == QChar::Separator_Line || category == QChar::Separator_Paragraph ||
            character.unicode() == 0xfffe || character.unicode() == 0xffff)
            return fail(error, QStringLiteral("变形器名称必须是单行文本，不能包含控制字符"));
    }
    return true;
}

bool CDeformerDisplayNames::Validate(const State& names, QString* error)
{
    if (error) error->clear();
    if (names.size() > MaximumEntries) return fail(error, QStringLiteral("变形器名称数量超出限制"));
    qint64 bytes = 0;
    for (auto item = names.cbegin(); item != names.cend(); ++item)
        if (!validateEntry(item.key(), item.value(), bytes, error)) return false;
    return true;
}

bool CDeformerDisplayNames::Normalize(const State& names, State& result, QString* error)
{
    if (!Validate(names, error)) return false;
    State candidate;
    for (auto item = names.cbegin(); item != names.cend(); ++item)
        if (item.key() != item.value()) candidate.insert(owned(item.key()), owned(item.value()));
    result.swap(candidate);
    return true;
}

bool CDeformerDisplayNames::ToJson(const State& names, QJsonObject& result, QString* error)
{
    State normalized;
    if (!Normalize(names, normalized, error)) return false;
    QJsonObject candidate;
    for (auto item = normalized.cbegin(); item != normalized.cend(); ++item) candidate.insert(item.key(), item.value());
    result = std::move(candidate);
    return true;
}

bool CDeformerDisplayNames::FromJson(const QJsonObject& object, State& result, QString* error)
{
    if (error) error->clear();
    if (object.size() > MaximumEntries) return fail(error, QStringLiteral("变形器名称数量超出限制"));
    State candidate;
    qint64 bytes = 0;
    for (auto item = object.constBegin(); item != object.constEnd(); ++item) {
        if (!item.value().isString()) return fail(error, QStringLiteral("变形器名称必须是文本"));
        const auto name = item.value().toString();
        if (!validateEntry(item.key(), name, bytes, error)) return false;
        candidate.insert(owned(item.key()), owned(name));
    }
    result.swap(candidate);
    return true;
}
}
