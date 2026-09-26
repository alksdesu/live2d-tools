#include "cphysicsdocument.h"
#include <QJsonArray>
#include <cmath>

namespace live2dstruct {
bool CPhysicsDocument::Validate(const QJsonObject& document, QString* error)
{
    if (error) error->clear();
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const auto numberInRange = [](const QJsonObject& object, const QString& key,
                                  double minimum, double maximum) {
        const QJsonValue value = object.value(key);
        if (value.isUndefined()) return true;
        if (!value.isDouble()) return false;
        const double number = value.toDouble();
        return std::isfinite(number) && number >= minimum && number <= maximum;
    };
    const auto optionalObject = [](const QJsonObject& parent, const QString& key) {
        return !parent.contains(key) || parent.value(key).isObject();
    };
    const auto validReference = [](const QJsonObject& row, const QString& key) {
        const auto value = row.value(key).toObject();
        const auto id = value.value(QStringLiteral("Id"));
        return id.isString() && !id.toString().isEmpty() && !id.toString().contains(QChar::Null) &&
            (!value.contains(QStringLiteral("Target")) || value.value(QStringLiteral("Target")) == QLatin1String("Parameter")) &&
            (!row.contains(QStringLiteral("Reflect")) || row.value(QStringLiteral("Reflect")).isBool());
    };
    if ((document.contains(QStringLiteral("Version")) && document.value(QStringLiteral("Version")).toDouble(-1) != 3) ||
        !optionalObject(document, QStringLiteral("Meta"))) return fail(QStringLiteral("物理配置版本或 Meta 无效"));
    const QJsonObject meta = document.value(QStringLiteral("Meta")).toObject();
    if (!optionalObject(meta, QStringLiteral("EffectiveForces")) || !numberInRange(meta, QStringLiteral("Fps"), 0, 1000))
        return fail(QStringLiteral("物理帧率或力场配置无效"));
    const QJsonObject forces = meta.value(QStringLiteral("EffectiveForces")).toObject();
    for (const QString& forceName : {QStringLiteral("Gravity"), QStringLiteral("Wind")}) {
        const QJsonObject force = forces.value(forceName).toObject();
        if (!optionalObject(forces, forceName) || !numberInRange(force, QStringLiteral("X"), -100000.0, 100000.0) ||
            !numberInRange(force, QStringLiteral("Y"), -100000.0, 100000.0))
            return fail(QStringLiteral("力场数值无效：%1").arg(forceName));
    }
    const QJsonValue settingsValue = document.value(QStringLiteral("PhysicsSettings"));
    if (!settingsValue.isArray()) return fail(QStringLiteral("PhysicsSettings 必须是数组"));
    const QJsonArray settings = settingsValue.toArray();
    if (settings.size() > 10000) return fail(QStringLiteral("物理组数量超出限制"));
    qint64 rows = 0;
    for (int groupIndex = 0; groupIndex < settings.size(); ++groupIndex) {
        const QJsonValue groupValue = settings.at(groupIndex);
        if (!groupValue.isObject()) return fail(QStringLiteral("物理组 %1 不是对象").arg(groupIndex));
        const QJsonObject group = groupValue.toObject();
        for (const auto& name : {QStringLiteral("Input"), QStringLiteral("Output"), QStringLiteral("Vertices")})
            rows += group.value(name).toArray().size();
        if (rows > 1000000) return fail(QStringLiteral("物理条目数量超出限制"));
        if (!optionalObject(group, QStringLiteral("Normalization"))) return fail(QStringLiteral("物理组归一化配置无效"));
        const auto normalization = group.value(QStringLiteral("Normalization")).toObject();
        for (const auto& name : {QStringLiteral("Position"), QStringLiteral("Angle")}) {
            if (!optionalObject(normalization, name)) return fail(QStringLiteral("物理组归一化范围无效"));
            const auto range = normalization.value(name).toObject();
            for (const auto& key : {QStringLiteral("Minimum"), QStringLiteral("Maximum"), QStringLiteral("Default")})
                if (!numberInRange(range, key, -100000, 100000)) return fail(QStringLiteral("物理组归一化数值无效"));
            const double minimum = range.value(QStringLiteral("Minimum")).toDouble();
            const double maximum = range.value(QStringLiteral("Maximum")).toDouble();
            const double value = range.value(QStringLiteral("Default")).toDouble();
            if (minimum > maximum || value < minimum || value > maximum) return fail(QStringLiteral("物理组归一化范围无效"));
        }
        const QJsonValue inputsValue = group.value(QStringLiteral("Input"));
        if (!inputsValue.isUndefined() && !inputsValue.isArray()) return fail(QStringLiteral("物理组 %1 的 Input 无效").arg(groupIndex));
        for (const QJsonValue& inputValue : inputsValue.toArray()) {
            if (!inputValue.isObject()) return fail(QStringLiteral("物理组 %1 的输入无效").arg(groupIndex));
            const QJsonObject input = inputValue.toObject();
            const QString sourceId = input.value(QStringLiteral("Source")).toObject().value(QStringLiteral("Id")).toString();
            const QString inputType = input.value(QStringLiteral("Type")).toString();
            if (sourceId.isEmpty() || !validReference(input, QStringLiteral("Source")) || (inputType != QStringLiteral("X") && inputType != QStringLiteral("Y") &&
                                       inputType != QStringLiteral("Angle")) ||
                !numberInRange(input, QStringLiteral("Weight"), 0.0, 100.0))
                return fail(QStringLiteral("物理组 %1 的输入权重无效").arg(groupIndex));
        }
        const QJsonValue outputsValue = group.value(QStringLiteral("Output"));
        if (!outputsValue.isUndefined() && !outputsValue.isArray()) return fail(QStringLiteral("物理组 %1 的 Output 无效").arg(groupIndex));
        for (const QJsonValue& outputValue : outputsValue.toArray()) {
            if (!outputValue.isObject()) return fail(QStringLiteral("物理组 %1 的输出无效").arg(groupIndex));
            const QJsonObject output = outputValue.toObject();
            const QString destinationId = output.value(QStringLiteral("Destination")).toObject().value(QStringLiteral("Id")).toString();
            const QString outputType = output.value(QStringLiteral("Type")).toString();
            const QJsonValue vertexIndexValue = output.value(QStringLiteral("VertexIndex"));
            const int vertexCount = group.value(QStringLiteral("Vertices")).toArray().size();
            if (destinationId.isEmpty() || !validReference(output, QStringLiteral("Destination")) || (outputType != QStringLiteral("X") && outputType != QStringLiteral("Y") &&
                                            outputType != QStringLiteral("Angle")) ||
                !vertexIndexValue.isDouble() || vertexIndexValue.toDouble() < 0.0 ||
                vertexIndexValue.toDouble() != std::floor(vertexIndexValue.toDouble()) ||
                vertexIndexValue.toDouble() >= vertexCount ||
                !numberInRange(output, QStringLiteral("Weight"), 0.0, 100.0) ||
                !numberInRange(output, QStringLiteral("Scale"), -100000.0, 100000.0))
                return fail(QStringLiteral("物理组 %1 的输出数值无效").arg(groupIndex));
        }
        const QJsonValue verticesValue = group.value(QStringLiteral("Vertices"));
        if (!verticesValue.isUndefined() && !verticesValue.isArray()) return fail(QStringLiteral("物理组 %1 的 Vertices 无效").arg(groupIndex));
        const QJsonArray vertices = verticesValue.toArray();
        if (vertices.isEmpty()) return fail(QStringLiteral("物理组 %1 缺少根顶点").arg(groupIndex));
        for (const QJsonValue& vertexValue : vertices) {
            if (!vertexValue.isObject()) return fail(QStringLiteral("物理组 %1 的摆锤无效").arg(groupIndex));
            const QJsonObject vertex = vertexValue.toObject();
            const auto position = vertex.value(QStringLiteral("Position")).toObject();
            if (!optionalObject(vertex, QStringLiteral("Position")) ||
                !numberInRange(position, QStringLiteral("X"), -100000, 100000) ||
                !numberInRange(position, QStringLiteral("Y"), -100000, 100000) ||
                !numberInRange(vertex, QStringLiteral("Radius"), 0.0, 100000.0) ||
                !numberInRange(vertex, QStringLiteral("Mobility"), 0.0, 1.0) ||
                !numberInRange(vertex, QStringLiteral("Delay"), 0.0, 100000.0) ||
                !numberInRange(vertex, QStringLiteral("Acceleration"), 0.0, 100000.0))
                return fail(QStringLiteral("物理组 %1 的摆锤数值无效").arg(groupIndex));
        }
    }
    return true;
}


bool CPhysicsDocument::Normalize(const QJsonObject& document, QJsonObject& result, QString* error)
{
    if (error) error->clear();
    if (!Validate(document, error)) return false;
    auto candidate = document;
    auto meta = candidate.value(QStringLiteral("Meta")).toObject();
    const auto settings = candidate.value(QStringLiteral("PhysicsSettings")).toArray();
    int inputs = 0, outputs = 0, vertices = 0;
    for (const auto& value : settings) {
        const auto group = value.toObject();
        inputs += group.value(QStringLiteral("Input")).toArray().size();
        outputs += group.value(QStringLiteral("Output")).toArray().size();
        vertices += group.value(QStringLiteral("Vertices")).toArray().size();
    }
    meta.insert(QStringLiteral("PhysicsSettingCount"), settings.size());
    meta.insert(QStringLiteral("TotalInputCount"), inputs);
    meta.insert(QStringLiteral("TotalOutputCount"), outputs);
    meta.insert(QStringLiteral("VertexCount"), vertices);
    if (!meta.contains(QStringLiteral("PhysicsDictionary")) && candidate.contains(QStringLiteral("PhysicsDictionary")))
        meta.insert(QStringLiteral("PhysicsDictionary"), candidate.value(QStringLiteral("PhysicsDictionary")));
    candidate.remove(QStringLiteral("PhysicsDictionary"));
    candidate.insert(QStringLiteral("Meta"), meta);
    result = candidate;
    return true;
}
}
