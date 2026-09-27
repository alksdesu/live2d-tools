#include "cparameterorganization.h"
#include "cmodeldocument.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <exception>

namespace live2dstruct {
namespace {
using Organization = CParameterOrganization;
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool text(const QString& value)
{
    return !value.trimmed().isEmpty() && value.size() <= Organization::MaximumTextLength && !value.contains(QChar::Null);
}
bool color(const QString& value)
{
    if (value.isEmpty()) return true;
    if (value.size() != 7 || value[0] != QLatin1Char('#')) return false;
    for (int index = 1; index < value.size(); ++index) {
        const auto c = value[index].toUpper();
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('A') && c <= QLatin1Char('F')))) return false;
    }
    return true;
}
QJsonObject encode(const Organization::State& state)
{
    QJsonArray nodes, pairs;
    for (const auto& node : state.nodes) nodes.append(QJsonObject{
        {"kind", node.kind == Organization::Node::Folder ? "folder" : "parameter"}, {"id", node.id},
        {"parentFolder", node.parentFolder}, {"name", node.name}, {"color", node.color}});
    for (const auto& pair : state.pairs) pairs.append(QJsonObject{
        {"first", pair.first}, {"second", pair.second}, {"reversed", pair.reversed}, {"enabled", pair.enabled}});
    return {{"configured", state.configured}, {"nodes", nodes}, {"pairs", pairs}};
}
bool keys(const QJsonObject& object, const QSet<QString>& expected)
{
    const auto names = object.keys();
    return QSet<QString>(names.cbegin(), names.cend()) == expected;
}
}

bool CParameterOrganization::Normalize(const State& state, State& result, QString* error) try
{
    if (error) error->clear();
    if ((!state.configured && (!state.nodes.isEmpty() || !state.pairs.isEmpty())) ||
        state.nodes.size() > MaximumNodes || state.pairs.size() > MaximumNodes / 2)
        return fail(error, QStringLiteral("参数组织状态或条目数量无效"));
    State candidate = state;
    QSet<QString> folders, parameters;
    for (auto& node : candidate.nodes) {
        if (!text(node.id) || !color(node.color)) return fail(error, QStringLiteral("参数组织 ID 或颜色无效"));
        node.color = node.color.toUpper();
        if (node.kind == Node::Folder) {
            if (!node.parentFolder.isEmpty() || !text(node.name) || folders.contains(node.id))
                return fail(error, QStringLiteral("参数文件夹必须位于根级，名称和 ID 不得为空或重复"));
            folders.insert(node.id);
        } else if (node.kind == Node::Parameter) {
            if (!node.name.isEmpty() || parameters.contains(node.id) ||
                (!node.parentFolder.isEmpty() && !text(node.parentFolder)))
                return fail(error, QStringLiteral("参数节点名称、位置或 ID 无效"));
            parameters.insert(node.id);
        } else return fail(error, QStringLiteral("参数组织节点类型无效"));
    }
    for (const auto& node : candidate.nodes)
        if (!node.parentFolder.isEmpty() && !folders.contains(node.parentFolder))
            return fail(error, QStringLiteral("参数所属文件夹不存在：%1").arg(node.parentFolder));
    QSet<QString> paired;
    for (const auto& pair : candidate.pairs) {
        if (pair.first == pair.second || !parameters.contains(pair.first) || !parameters.contains(pair.second) ||
            paired.contains(pair.first) || paired.contains(pair.second))
            return fail(error, QStringLiteral("参数配对必须引用两个不同的参数，每个参数只能属于一对"));
        paired.insert(pair.first); paired.insert(pair.second);
    }
    if (QJsonDocument(encode(candidate)).toJson(QJsonDocument::Compact).size() > MaximumBytes)
        return fail(error, QStringLiteral("参数组织数据超出支持大小"));
    result = std::move(candidate);
    return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法准备参数组织状态")); }

bool CParameterOrganization::Validate(const State& state, const QSet<QString>& parameterIds, QString* error)
{
    State candidate;
    if (!Normalize(state, candidate, error)) return false;
    if (!state.configured) return true;
    QSet<QString> contained;
    for (const auto& node : state.nodes) if (node.kind == Node::Parameter) contained.insert(node.id);
    if (contained != parameterIds) return fail(error, QStringLiteral("参数组织必须恰好包含当前模型的每个参数"));
    return true;
}

bool CParameterOrganization::Reconcile(const State& state, const QStringList& beforeIds, const QStringList& afterIds,
                                      State& result, QString* error) try
{
    const QSet<QString> before(beforeIds.cbegin(), beforeIds.cend()), after(afterIds.cbegin(), afterIds.cend());
    if (before.size() != beforeIds.size() || after.size() != afterIds.size())
        return fail(error, QStringLiteral("参数目录含有重复 ID"));
    for (const auto& id : beforeIds) if (!text(id)) return fail(error, QStringLiteral("原参数目录 ID 无效"));
    for (const auto& id : afterIds) if (!text(id)) return fail(error, QStringLiteral("新参数目录 ID 无效"));
    if (!Validate(state, before, error)) return false;
    State candidate;
    if (!Normalize(state, candidate, error)) return false;
    if (candidate.configured) {
        candidate.nodes.erase(std::remove_if(candidate.nodes.begin(), candidate.nodes.end(), [&](const Node& node) {
            return node.kind == Node::Parameter && !after.contains(node.id);
        }), candidate.nodes.end());
        candidate.pairs.erase(std::remove_if(candidate.pairs.begin(), candidate.pairs.end(), [&](const ParameterPair& pair) {
            return !after.contains(pair.first) || !after.contains(pair.second);
        }), candidate.pairs.end());
        for (const auto& id : afterIds) if (!before.contains(id)) candidate.nodes.append({Node::Parameter, id, {}, {}, {}});
    }
    if (!Validate(candidate, after, error)) return false;
    result = std::move(candidate);
    return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法同步参数组织，原状态保持不变")); }

bool CParameterOrganization::ExpandUserValues(const CModelDocument& document, const QMap<QString, float>& explicitValues,
                                             QMap<QString, float>& expanded, QString* error) try
{
    if (error) error->clear();
    if (!document.IsLoaded() || explicitValues.isEmpty()) return fail(error, QStringLiteral("没有可编辑的模型参数值"));
    struct Range { float minimum, maximum; };
    QHash<QString, Range> ranges;
    const auto& overrides = document.ParameterMetadataOverrides();
    for (const auto& parameter : document.Parameters()) {
        if (ranges.contains(parameter.id)) return fail(error, QStringLiteral("参数 ID 重复"));
        const auto metadata = overrides.constFind(parameter.id);
        ranges.insert(parameter.id, metadata == overrides.cend() ? Range{parameter.minimum, parameter.maximum}
                                                               : Range{metadata->minimum, metadata->maximum});
    }
    QSet<QString> ids;
    for (auto range = ranges.cbegin(); range != ranges.cend(); ++range) ids.insert(range.key());
    const auto& organization = document.ParameterOrganization();
    if (!Validate(organization, ids, error)) return false;
    for (auto value = explicitValues.cbegin(); value != explicitValues.cend(); ++value) {
        const auto range = ranges.constFind(value.key());
        if (range == ranges.cend() || !std::isfinite(value.value()) || !std::isfinite(range->minimum) ||
            !std::isfinite(range->maximum) || range->minimum > range->maximum ||
            value.value() < range->minimum || value.value() > range->maximum)
            return fail(error, QStringLiteral("参数 %1 的输入值无效或超出范围").arg(value.key()));
    }
    auto candidate = explicitValues;
    for (const auto& pair : organization.pairs) {
        if (!pair.enabled || (!explicitValues.contains(pair.first) && !explicitValues.contains(pair.second))) continue;
        const auto first = ranges.value(pair.first), second = ranges.value(pair.second);
        if (!std::isfinite(first.minimum) || !std::isfinite(first.maximum) || !std::isfinite(second.minimum) ||
            !std::isfinite(second.maximum) || first.minimum >= first.maximum || second.minimum >= second.maximum)
            return fail(error, QStringLiteral("常量参数或无效范围无法进行配对归一化"));
        const auto mapped = [&](float value, Range source, Range target) {
            double normalized = (double(value) - source.minimum) / (double(source.maximum) - source.minimum);
            if (pair.reversed) normalized = 1.0 - normalized;
            return static_cast<float>(double(target.minimum) + normalized * (double(target.maximum) - target.minimum));
        };
        if (explicitValues.contains(pair.first) && explicitValues.contains(pair.second)) {
            const float forward = mapped(explicitValues.value(pair.first), first, second);
            if (forward != explicitValues.value(pair.second))
                return fail(error, QStringLiteral("配对参数 %1 与 %2 的显式输入冲突").arg(pair.first, pair.second));
        } else if (explicitValues.contains(pair.first)) {
            candidate.insert(pair.second, mapped(explicitValues.value(pair.first), first, second));
        } else candidate.insert(pair.first, mapped(explicitValues.value(pair.second), second, first));
    }
    expanded.swap(candidate);
    return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法准备配对参数值，原状态保持不变")); }

bool CParameterOrganization::ToJson(const State& state, QJsonObject& result, QString* error)
{
    State candidate;
    if (!Normalize(state, candidate, error)) return false;
    result = encode(candidate);
    return true;
}

bool CParameterOrganization::FromJson(const QJsonObject& object, State& result, QString* error) try
{
    if (!keys(object, {"configured", "nodes", "pairs"}) || !object["configured"].isBool() ||
        !object["nodes"].isArray() || !object["pairs"].isArray() ||
        object["nodes"].toArray().size() > MaximumNodes || object["pairs"].toArray().size() > MaximumNodes / 2 ||
        QJsonDocument(object).toJson(QJsonDocument::Compact).size() > MaximumBytes)
        return fail(error, QStringLiteral("参数组织字段或大小无效"));
    State candidate; candidate.configured = object["configured"].toBool();
    for (const auto& value : object["nodes"].toArray()) {
        if (!value.isObject()) return fail(error, QStringLiteral("参数组织节点不是对象"));
        const auto row = value.toObject();
        if (!keys(row, {"kind", "id", "parentFolder", "name", "color"})) return fail(error, QStringLiteral("参数组织节点字段无效"));
        for (const auto& field : row.keys()) if (!row[field].isString()) return fail(error, QStringLiteral("参数组织节点字段必须是文本"));
        const QString kind = row["kind"].toString();
        if (kind != QLatin1String("folder") && kind != QLatin1String("parameter")) return fail(error, QStringLiteral("参数组织节点类型无效"));
        candidate.nodes.append({kind == QLatin1String("folder") ? Node::Folder : Node::Parameter,
            row["id"].toString(), row["parentFolder"].toString(), row["name"].toString(), row["color"].toString()});
    }
    for (const auto& value : object["pairs"].toArray()) {
        if (!value.isObject()) return fail(error, QStringLiteral("参数配对条目不是对象"));
        const auto row = value.toObject();
        if (!keys(row, {"first", "second", "reversed", "enabled"}) || !row["first"].isString() || !row["second"].isString() ||
            !row["reversed"].isBool() || !row["enabled"].isBool()) return fail(error, QStringLiteral("参数配对字段无效"));
        candidate.pairs.append({row["first"].toString(), row["second"].toString(), row["reversed"].toBool(), row["enabled"].toBool()});
    }
    return Normalize(candidate, result, error);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法读取参数组织状态")); }
}
