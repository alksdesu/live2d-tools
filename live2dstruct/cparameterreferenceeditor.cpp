#include "cparameterreferenceeditor.h"
#include "cmotiontimeline.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <cmath>
#include <exception>

namespace live2dstruct {
namespace {
using Editor = CParameterReferenceEditor;
constexpr int MaximumBytes = 16 * 1024 * 1024;
constexpr int MaximumRows = 100000;

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool validId(const QString& id) { return !id.isEmpty() && !id.contains(QChar::Null); }

bool array(const QJsonObject& object, const QString& field, QJsonArray& result, QString* error)
{
    if (!object.value(field).isArray()) return fail(error, QStringLiteral("引用字段必须是数组：%1").arg(field));
    result = object.value(field).toArray();
    return result.size() <= MaximumRows || fail(error, QStringLiteral("引用数量超出限制：%1").arg(field));
}

bool idField(const QJsonObject& object, const QString& field, QString& result, QString* error)
{
    const auto value = object.value(field);
    if (!value.isString() || !validId(value.toString())) return fail(error, QStringLiteral("引用标识无效：%1").arg(field));
    result = value.toString();
    return true;
}

bool groups(Editor::Prepared& result, QString* error)
{
    auto root = result.source;
    if (!root.value(QStringLiteral("FileReferences")).isObject())
        return fail(error, QStringLiteral("模型设置缺少 FileReferences"));
    if (!root.contains(QStringLiteral("Groups"))) { result.effective = root; return true; }
    QJsonArray source, effective;
    if (!array(root, QStringLiteral("Groups"), source, error)) return false;
    int entries = 0;
    QSet<QString> sdkGroups;
    for (int row = 0; row < source.size(); ++row) {
        if (!source[row].isObject()) return fail(error, QStringLiteral("模型分组不是对象"));
        auto group = source[row].toObject();
        const auto nameValue = group.value(QStringLiteral("Name"));
        if (!nameValue.isString() || nameValue.toString().contains(QChar::Null))
            return fail(error, QStringLiteral("模型分组名称无效"));
        const QString name = nameValue.toString();
        const QString target = group.value(QStringLiteral("Target")).toString();
        const bool sdk = name == QLatin1String("EyeBlink") || name == QLatin1String("LipSync");
        const bool activeSdk = sdk && !sdkGroups.contains(name);
        if (sdk) sdkGroups.insert(name);
        const bool parameter = sdk || target == QLatin1String("Parameter");
        QJsonArray ids, kept;
        if (!array(group, QStringLiteral("Ids"), ids, error)) return false;
        if (ids.size() > MaximumRows - entries) return fail(error, QStringLiteral("模型分组引用过多"));
        entries += ids.size();
        for (const auto& value : ids) {
            if (!value.isString() || value.toString().contains(QChar::Null)) return fail(error, QStringLiteral("模型分组含无效参数 ID"));
            const auto id = value.toString();
            if (!parameter && target != QLatin1String("Part") && result.suppressedIds.contains(id))
                return fail(error, QStringLiteral("无法解释自定义分组 %1 对参数 %2 的引用").arg(name, id));
            if (parameter && result.suppressedIds.contains(id)) {
                result.references.append({QStringLiteral("group"), name, id, row});
                if (activeSdk && name == QLatin1String("EyeBlink")) result.eyeBlink.insert(id);
                if (activeSdk && name == QLatin1String("LipSync")) result.lipSync.insert(id);
            } else kept.append(value);
        }
        group.insert(QStringLiteral("Ids"), kept);
        effective.append(group);
    }
    root.insert(QStringLiteral("Groups"), effective);
    result.effective = root;
    return true;
}

bool motion(Editor::Prepared& result, QString* error)
{
    const bool draft = result.kind == Editor::Kind::Timeline;
    if (draft) {
        CMotionTimeline validator;
        if (!validator.RestoreState(result.source, nullptr, error)) return false;
        if (result.source.isEmpty()) { result.effective = result.source; return true; }
    }
    auto root = draft ? result.source.value(QStringLiteral("motion")).toObject() : result.source;
    QJsonObject validated = root;
    double sourceDuration = 0;
    if (!CMotionTimeline::RecountMetadata(validated, error, draft, &sourceDuration)) return false;
    QJsonArray rows, kept;
    if (!array(root, QStringLiteral("Curves"), rows, error)) return false;
    for (int row = 0; row < rows.size(); ++row) {
        const auto curve = rows[row].toObject();
        const auto id = curve.value(QStringLiteral("Id")).toString();
        if (curve.value(QStringLiteral("Target")).toString() == QLatin1String("Parameter") && result.suppressedIds.contains(id)) {
            result.parameterRows.insert(row, id);
            result.references.append({QStringLiteral("motion"), {}, id, row});
        } else kept.append(rows[row]);
    }
    if (!result.parameterRows.isEmpty()) {
        root.insert(QStringLiteral("Curves"), kept);
        auto meta = root.value(QStringLiteral("Meta")).toObject();
        if (!meta.contains(QStringLiteral("Duration"))) {
            meta.insert(QStringLiteral("Duration"), sourceDuration);
            root.insert(QStringLiteral("Meta"), meta);
        }
        if (!CMotionTimeline::RecountMetadata(root, error, draft)) return false;
    }
    result.effective = result.source;
    if (draft) {
        result.effective.insert(QStringLiteral("motion"), root);
        CMotionTimeline validator;
        if (!validator.RestoreState(result.effective, nullptr, error)) return false;
    }
    else result.effective = root;
    return true;
}

bool expression(Editor::Prepared& result, QString* error)
{
    QJsonArray rows, kept;
    if (!array(result.source, QStringLiteral("Parameters"), rows, error)) return false;
    for (int row = 0; row < rows.size(); ++row) {
        if (!rows[row].isObject()) return fail(error, QStringLiteral("表情参数不是对象"));
        QString id;
        if (!idField(rows[row].toObject(), QStringLiteral("Id"), id, error)) return false;
        const auto value = rows[row].toObject().value(QStringLiteral("Value"));
        if (!value.isDouble() || !std::isfinite(value.toDouble()) || !std::isfinite(static_cast<float>(value.toDouble())))
            return fail(error, QStringLiteral("表情参数值无效：%1").arg(id));
        if (result.suppressedIds.contains(id)) {
            result.parameterRows.insert(row, id);
            result.references.append({QStringLiteral("expression"), {}, id, row});
        } else kept.append(rows[row]);
    }
    result.effective = result.source;
    result.effective.insert(QStringLiteral("Parameters"), kept);
    return true;
}

bool physics(Editor::Prepared& result, QString* error)
{
    QJsonArray settings, updated;
    if (!array(result.source, QStringLiteral("PhysicsSettings"), settings, error)) return false;
    if (result.source.contains(QStringLiteral("Meta")) && !result.source.value(QStringLiteral("Meta")).isObject())
        return fail(error, QStringLiteral("物理元数据必须是对象"));
    int sourceInput = 0, sourceOutput = 0, keptInput = 0, keptOutput = 0;
    for (int groupIndex = 0; groupIndex < settings.size(); ++groupIndex) {
        if (!settings[groupIndex].isObject()) return fail(error, QStringLiteral("物理组不是对象"));
        auto group = settings[groupIndex].toObject();
        const auto groupId = group.value(QStringLiteral("Id")).toString(QString::number(groupIndex + 1));
        for (bool input : {true, false}) {
            const auto field = input ? QStringLiteral("Input") : QStringLiteral("Output");
            const auto endpoint = input ? QStringLiteral("Source") : QStringLiteral("Destination");
            auto& sourceIndex = input ? sourceInput : sourceOutput;
            auto& keptCount = input ? keptInput : keptOutput;
            auto& suppressed = input ? result.inputRows : result.outputRows;
            QJsonArray rows, kept;
            if (!array(group, field, rows, error)) return false;
            if (rows.size() > MaximumRows - sourceIndex) return fail(error, QStringLiteral("物理引用数量超出限制"));
            for (const auto& row : rows) {
                if (!row.isObject() || !row.toObject().value(endpoint).isObject())
                    return fail(error, QStringLiteral("物理输入或输出目标无效"));
                const auto source = row.toObject().value(endpoint).toObject();
                QString id;
                if (!idField(source, QStringLiteral("Id"), id, error) || source.value(QStringLiteral("Target")).toString() != QLatin1String("Parameter"))
                    return fail(error, QStringLiteral("物理输入或输出必须引用参数"));
                if (result.suppressedIds.contains(id)) {
                    suppressed.insert(sourceIndex, id);
                    result.references.append({input ? QStringLiteral("physicsInput") : QStringLiteral("physicsOutput"), groupId, id, sourceIndex});
                } else { kept.append(row); ++keptCount; }
                ++sourceIndex;
            }
            group.insert(field, kept);
        }
        updated.append(group);
    }
    result.effective = result.source;
    if (!result.inputRows.isEmpty() || !result.outputRows.isEmpty()) {
        result.effective.insert(QStringLiteral("PhysicsSettings"), updated);
        auto meta = result.source.value(QStringLiteral("Meta")).toObject();
        meta.insert(QStringLiteral("TotalInputCount"), keptInput);
        meta.insert(QStringLiteral("TotalOutputCount"), keptOutput);
        result.effective.insert(QStringLiteral("Meta"), meta);
    }
    return true;
}
}

bool CParameterReferenceEditor::Prepare(Kind kind, const QByteArray& source, const QSet<QString>& suppressedIds,
                                      Prepared& prepared, QString* error)
{
    if (error) error->clear();
    if (source.size() > MaximumBytes || suppressedIds.size() > MaximumRows)
        return fail(error, QStringLiteral("参数引用数据超出大小限制"));
    for (const auto& id : suppressedIds) if (!validId(id)) return fail(error, QStringLiteral("待移除引用的参数 ID 无效"));
    try {
        QJsonParseError parse;
        const auto json = QJsonDocument::fromJson(source, &parse);
        if (parse.error != QJsonParseError::NoError || !json.isObject()) return fail(error, QStringLiteral("参数引用 JSON 无效：%1").arg(parse.errorString()));
        Prepared candidate;
        candidate.kind = kind;
        candidate.sourceBytes = source;
        candidate.sourceDigest = QCryptographicHash::hash(source, QCryptographicHash::Sha256);
        candidate.source = json.object();
        candidate.suppressedIds = suppressedIds;
        bool valid = false;
        switch (kind) {
        case Kind::ModelSettings: valid = groups(candidate, error); break;
        case Kind::Motion: case Kind::Timeline: valid = motion(candidate, error); break;
        case Kind::Expression: valid = expression(candidate, error); break;
        case Kind::Physics: valid = physics(candidate, error); break;
        default: return fail(error, QStringLiteral("未知的参数引用类型"));
        }
        if (!valid) return false;
        candidate.effectiveBytes = candidate.effective == candidate.source ? source : QJsonDocument(candidate.effective).toJson(QJsonDocument::Indented);
        if (candidate.effectiveBytes.size() > MaximumBytes) return fail(error, QStringLiteral("修改后的参数引用数据超出大小限制"));
        prepared = std::move(candidate);
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法准备参数引用修改")); }
}
}
