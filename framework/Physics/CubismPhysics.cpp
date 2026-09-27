/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "CubismPhysics.hpp"
#include "CubismPhysicsInternal.hpp"
#include "CubismPhysicsJson.hpp"
#include "framework/Model/CubismModel.hpp"
#include "framework/Model/CubismParameterReferences.hpp"
#include "live2dstruct/cphysicsdocument.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include "framework/Utils/CubismString.hpp"
#include "framework/Math/CubismMath.hpp"
#include "framework/Math/CubismVector2.hpp"
#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>
#include <limits>
#include <type_traits>

namespace Live2D { namespace Cubism { namespace Framework {

/// physics constants
namespace {

/// physics types tags.
const csmChar* PhysicsTypeTagX = "X";
const csmChar* PhysicsTypeTagY = "Y";
const csmChar* PhysicsTypeTagAngle = "Angle";

/// Constant of air resistance.
const csmFloat32 AirResistance = 5.0f;

/// Constant of maximum weight of input and output ratio.
const csmFloat32 MaximumWeight = 100.0f;

/// Constant of threshold of movement.
const csmFloat32 MovementThreshold = 0.001f;

/// Constant of maximum allowed delta time
const csmFloat32 MaxDeltaTime = 5.0f;

bool PhysicsError(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

void NormalizeDirection(CubismVector2& direction, const CubismVector2& fallback)
{
    if (direction.X == 0 && direction.Y == 0) direction = fallback;
    if (direction.X == 0 && direction.Y == 0) direction = CubismVector2(0, 1);
    direction.Normalize();
}

QByteArray PhysicsTopology(const QJsonObject& document)
{
    QJsonArray groups;
    QSet<QString> ids;
    for (const auto& value : document.value(QStringLiteral("PhysicsSettings")).toArray())
    {
        const auto group = value.toObject();
        const auto id = group.value(QStringLiteral("Id"));
        if (!id.isString() || id.toString().isEmpty() || id.toString().contains(QChar::Null) || ids.contains(id.toString())) return {};
        ids.insert(id.toString());
        QJsonArray inputs, outputs, particles;
        for (const auto& item : group.value(QStringLiteral("Input")).toArray())
        {
            const auto row = item.toObject();
            inputs.append(QJsonArray{row.value(QStringLiteral("Source")).toObject().value(QStringLiteral("Id")),
                row.value(QStringLiteral("Type")), row.value(QStringLiteral("Reflect")).toBool()});
        }
        for (const auto& item : group.value(QStringLiteral("Output")).toArray())
        {
            const auto row = item.toObject();
            outputs.append(QJsonArray{row.value(QStringLiteral("Destination")).toObject().value(QStringLiteral("Id")),
                row.value(QStringLiteral("Type")), row.value(QStringLiteral("Reflect")).toBool(), row.value(QStringLiteral("VertexIndex"))});
        }
        for (const auto& item : group.value(QStringLiteral("Vertices")).toArray())
        {
            const auto row = item.toObject();
            const auto position = row.value(QStringLiteral("Position")).toObject();
            particles.append(QJsonArray{row.value(QStringLiteral("Radius")).toDouble(),
                position.value(QStringLiteral("X")).toDouble(), position.value(QStringLiteral("Y")).toDouble()});
        }
        groups.append(QJsonArray{id, inputs, outputs, particles});
    }
    return QJsonDocument(QJsonArray{document.value(QStringLiteral("Meta")).toObject().value(QStringLiteral("Fps")).toDouble(), groups})
        .toJson(QJsonDocument::Compact);
}

csmFloat32 NormalizeParameterValue(
    csmFloat32 value,
    csmFloat32 parameterMinimum,
    csmFloat32 parameterMaximum,
    csmFloat32 parameterDefault,
    csmFloat32 normalizedMinimum,
    csmFloat32 normalizedMaximum,
    csmFloat32 normalizedDefault,
    csmInt32 isInverted)
{
    Q_UNUSED(parameterDefault);
    const double minimum = std::min(double(parameterMinimum), double(parameterMaximum));
    const double maximum = std::max(double(parameterMinimum), double(parameterMaximum));
    const double middle = minimum + (maximum - minimum) * 0.5;
    const double bounded = std::max(minimum, std::min(maximum, double(value)));
    const double relative = bounded - middle;
    double result = normalizedDefault;
    if (relative != 0.0) {
        const double endpoint = relative > 0.0 ? maximum : minimum;
        const double normalizedEndpoint = relative > 0.0
            ? std::max(double(normalizedMinimum), double(normalizedMaximum))
            : std::min(double(normalizedMinimum), double(normalizedMaximum));
        const double fraction = relative / (endpoint - middle);
        result = double(normalizedDefault) + fraction * (normalizedEndpoint - double(normalizedDefault));
    }
    return static_cast<csmFloat32>(isInverted ? result : -result);
}

void GetInputTranslationXFromNormalizedParameterValue(CubismVector2* targetTranslation, csmFloat32* targetAngle, csmFloat32 value,
    csmFloat32 parameterMinimumValue, csmFloat32 parameterMaximumValue,
    csmFloat32 parameterDefaultValue,
    CubismPhysicsNormalization* normalizationPosition,
    CubismPhysicsNormalization* normalizationAngle, csmInt32 isInverted,
    csmFloat32 weight)
{
    targetTranslation->X += NormalizeParameterValue(
        value,
        parameterMinimumValue,
        parameterMaximumValue,
        parameterDefaultValue,
        normalizationPosition->Minimum,
        normalizationPosition->Maximum,
        normalizationPosition->Default,
        isInverted
    ) * weight;
}

void GetInputTranslationYFromNormalizedParameterValue(CubismVector2* targetTranslation, csmFloat32* targetAngle, csmFloat32 value,
    csmFloat32 parameterMinimumValue, csmFloat32 parameterMaximumValue,
    csmFloat32 parameterDefaultValue,
    CubismPhysicsNormalization* normalizationPosition,
    CubismPhysicsNormalization* normalizationAngle,
    csmInt32 isInverted, csmFloat32 weight)
{
    targetTranslation->Y += NormalizeParameterValue(
        value,
        parameterMinimumValue,
        parameterMaximumValue,
        parameterDefaultValue,
        normalizationPosition->Minimum,
        normalizationPosition->Maximum,
        normalizationPosition->Default,
        isInverted
    ) * weight;
}

void GetInputAngleFromNormalizedParameterValue(CubismVector2* targetTranslation, csmFloat32* targetAngle, csmFloat32 value,
    csmFloat32 parameterMinimumValue, csmFloat32 parameterMaximumValue,
    csmFloat32 parameterDefaultValue,
    CubismPhysicsNormalization* normalizationPosition,
    CubismPhysicsNormalization* normalizationAngle,
    csmInt32 isInverted, csmFloat32 weight)
{
    *targetAngle += NormalizeParameterValue(
        value,
        parameterMinimumValue,
        parameterMaximumValue,
        parameterDefaultValue,
        normalizationAngle->Minimum,
        normalizationAngle->Maximum,
        normalizationAngle->Default,
        isInverted
    ) * weight;
}

csmFloat32 GetOutputTranslationX(CubismVector2 translation, CubismPhysicsParticle* particles, csmInt32 particleIndex,
    csmInt32 isInverted, CubismVector2 parentGravity)
{
    csmFloat32 outputValue = translation.X;

    if (isInverted)
    {
        outputValue *= -1.0f;
    }

    return outputValue;
}

csmFloat32 GetOutputTranslationY(CubismVector2 translation, CubismPhysicsParticle* particles, csmInt32 particleIndex,
    csmInt32 isInverted, CubismVector2 parentGravity)
{
    csmFloat32 outputValue = translation.Y;

    if (isInverted)
    {
        outputValue *= -1.0f;
    }

    return outputValue;
}

csmFloat32 GetOutputAngle(CubismVector2 translation, CubismPhysicsParticle* particles, csmInt32 particleIndex, csmInt32 isInverted,
    CubismVector2 parentGravity)
{
    csmFloat32 outputValue;

    if (particleIndex >= 2)
    {
        parentGravity = particles[particleIndex - 1].Position - particles[particleIndex - 2].Position;
    }
    else
    {
        parentGravity *= -1.0f;
    }

    outputValue = CubismMath::DirectionToRadian(parentGravity, translation);

    if (isInverted)
    {
        outputValue *= -1.0f;
    }

    return outputValue;
}

csmFloat32 GetOutputScaleTranslationX(CubismVector2 translationScale, csmFloat32 angleScale)
{
    return translationScale.X;
}

csmFloat32 GetOutputScaleTranslationY(CubismVector2 translationScale, csmFloat32 angleScale)
{
    return translationScale.Y;
}

csmFloat32 GetOutputScaleAngle(CubismVector2 translationScale, csmFloat32 angleScale)
{
    return angleScale;
}

/// Updates particles.
///
/// @param  strand            Target array of particle.
/// @param  strandCount       Count of particle.
/// @param  totalTranslation  Total translation value.
/// @param  totalAngle        Total angle.
/// @param  windDirection              Direction of wind.
/// @param  thresholdValue    Threshold of movement.
/// @param  deltaTimeSeconds  Delta time.
/// @param  airResistance     Air resistance.
void UpdateParticles(CubismPhysicsParticle* strand, csmInt32 strandCount, CubismVector2 totalTranslation, csmFloat32 totalAngle,
    CubismVector2 windDirection, csmFloat32 thresholdValue, csmFloat32 deltaTimeSeconds, csmFloat32 airResistance)
{
    csmInt32 i;
    csmFloat32 totalRadian;
    csmFloat32 delay;
    csmFloat32 radian;
    CubismVector2 currentGravity;
    CubismVector2 direction;
    CubismVector2 velocity;
    CubismVector2 force;
    CubismVector2 newDirection;

    strand[0].Position = totalTranslation;

    totalRadian = CubismMath::DegreesToRadian(totalAngle);
    currentGravity = CubismMath::RadianToDirection(totalRadian);
    currentGravity.Normalize();

    for (i = 1; i < strandCount; ++i)
    {
        strand[i].Force = (currentGravity * strand[i].Acceleration) + windDirection;

        strand[i].LastPosition = strand[i].Position;

        delay = strand[i].Delay * deltaTimeSeconds * 30.0f;

        direction.X = strand[i].Position.X - strand[i - 1].Position.X;
        direction.Y = strand[i].Position.Y - strand[i - 1].Position.Y;

        radian = CubismMath::DirectionToRadian(strand[i].LastGravity, currentGravity) / airResistance;

        direction.X = ((CubismMath::CosF(radian) * direction.X) - (direction.Y * CubismMath::SinF(radian)));
        direction.Y = ((CubismMath::SinF(radian) * direction.X) + (direction.Y * CubismMath::CosF(radian)));

        strand[i].Position = strand[i - 1].Position + direction;

        velocity.X = strand[i].Velocity.X * delay;
        velocity.Y = strand[i].Velocity.Y * delay;
        force = strand[i].Force * delay * delay;

        strand[i].Position = strand[i].Position + velocity + force;

        newDirection = strand[i].Position - strand[i - 1].Position;

        NormalizeDirection(newDirection, direction);

        strand[i].Position = strand[i - 1].Position + (newDirection * strand[i].Radius);

        if (CubismMath::AbsF(strand[i].Position.X) < thresholdValue)
        {
            strand[i].Position.X = 0.0f;
        }

        if (delay != 0.0f)
        {
            strand[i].Velocity.X = strand[i].Position.X - strand[i].LastPosition.X;
            strand[i].Velocity.Y = strand[i].Position.Y - strand[i].LastPosition.Y;
            strand[i].Velocity /= delay;
            strand[i].Velocity *= strand[i].Mobility;
        }

        strand[i].Force = CubismVector2(0.0f, 0.0f);
        strand[i].LastGravity = currentGravity;
    }
}

/**
 * Updates particles for stabilization.
 *
 * @param strand                Target array of particle.
 * @param strandCount           Count of particle.
 * @param totalTranslation      Total translation value.
 * @param totalAngle            Total angle.
 * @param windDirection         Direction of Wind.
 * @param thresholdValue        Threshold of movement.
 */
void UpdateParticlesForStabilization(CubismPhysicsParticle* strand, csmInt32 strandCount, CubismVector2 totalTranslation, csmFloat32 totalAngle,
    CubismVector2 windDirection, csmFloat32 thresholdValue)
{
    csmInt32 i;
    csmFloat32 totalRadian;
    CubismVector2 currentGravity;
    CubismVector2 force;

    strand[0].Position = totalTranslation;

    totalRadian = CubismMath::DegreesToRadian(totalAngle);
    currentGravity = CubismMath::RadianToDirection(totalRadian);
    currentGravity.Normalize();

    for (i = 1; i < strandCount; ++i)
    {
        strand[i].Force = (currentGravity * strand[i].Acceleration) + windDirection;

        strand[i].LastPosition = strand[i].Position;

        strand[i].Velocity = CubismVector2(0.0, 0.0);

        force = strand[i].Force;
        NormalizeDirection(force, currentGravity);

        force *= strand[i].Radius;
        strand[i].Position = strand[i - 1].Position + force;

        if (CubismMath::AbsF(strand[i].Position.X) < thresholdValue)
        {
            strand[i].Position.X = 0.0f;
        }

        strand[i].Force = CubismVector2(0.0f, 0.0f);
        strand[i].LastGravity = currentGravity;
    }
}

/// Updates output parameter value.
///
/// @param  parameterValue         Target parameter value.
/// @param  parameterValueMinimum  Minimum of parameter value.
/// @param  parameterValueMaximum  Maximum of parameter value.
/// @param  translation            Translation value.
void UpdateOutputParameterValue(csmFloat32* parameterValue, csmFloat32 parameterValueMinimum, csmFloat32 parameterValueMaximum,
    csmFloat32 translation, CubismPhysicsOutput* output, PhysicsOutputObservation* observation = nullptr)
{
    csmFloat32 outputScale;
    csmFloat32 value;
    csmFloat32 weight;

    outputScale = output->GetScale(output->TranslationScale, output->AngleScale);

    value = translation * outputScale;
    if (observation) { observation->rawValue = value; observation->clipped = false; }

    if (value < parameterValueMinimum)
    {
        if (observation) observation->clipped = true;
        if (value < output->ValueBelowMinimum)
        {
            output->ValueBelowMinimum = value;
        }

        value = parameterValueMinimum;
    }
    else if (value > parameterValueMaximum)
    {
        if (observation) observation->clipped = true;
        if (value > output->ValueExceededMaximum)
        {
            output->ValueExceededMaximum = value;
        }

        value = parameterValueMaximum;
    }
    if (observation) observation->clampedValue = value;

    weight = (output->Weight / MaximumWeight);

    if (weight >= 1.0f)
    {
        *parameterValue = value;
    }
    else
    {
        value = (*parameterValue * (1.0f - weight)) + (value * weight);
        *parameterValue = value;
    }
    if (observation) { observation->value = *parameterValue; observation->sampled = true; }
}

}

CubismPhysics::CubismPhysics()
    : _physicsRig(NULL)
    , _isJsonValid(false)
{
    // set default options.
    _options.Gravity.Y = -1.0f;
    _options.Gravity.X = 0;
    _options.Wind.X = 0;
    _options.Wind.Y = 0;
    _currentRemainTime = 0.0f;
}

CubismPhysics::~CubismPhysics()
{
    CSM_DELETE(_physicsRig);
    _parameterCaches.clear();
    _parameterInputCaches.clear();
}

bool CubismPhysics::CaptureConfiguration(QJsonObject& result, QString* error) const
{
    if (error) error->clear();
    try
    {
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(_normalizedBytes, &parseError);
        if (!_physicsRig || parseError.error != QJsonParseError::NoError || !document.isObject() ||
            _activeInputs.size() != _physicsRig->Inputs.GetSize() || _activeOutputs.size() != _physicsRig->Outputs.GetSize())
            return PhysicsError(error, QStringLiteral("物理配置快照与当前运行实例不匹配"));
        auto candidate = document.object();
        auto groups = candidate.value(QStringLiteral("PhysicsSettings")).toArray();
        if (groups.size() != _physicsRig->Settings.GetSize())
            return PhysicsError(error, QStringLiteral("物理配置快照的分组数量不匹配"));
        int inputIndex = 0, outputIndex = 0;
        for (int i = 0; i < groups.size(); ++i)
        {
            auto group = groups[i].toObject();
            const auto inputs = group.value(QStringLiteral("Input")).toArray();
            const auto outputs = group.value(QStringLiteral("Output")).toArray();
            const auto& rig = _physicsRig->Settings[i];
            if (inputs.size() != rig.InputCount || outputs.size() != rig.OutputCount ||
                rig.BaseInputIndex != inputIndex || rig.BaseOutputIndex != outputIndex ||
                inputs.size() > _activeInputs.size() - inputIndex || outputs.size() > _activeOutputs.size() - outputIndex)
                return PhysicsError(error, QStringLiteral("物理配置快照的输入输出行不匹配"));
            QJsonArray activeInputs, activeOutputs;
            for (const auto& input : inputs)
            {
                const auto active = _activeInputs[inputIndex++];
                if (active > 1) return PhysicsError(error, QStringLiteral("物理输入的屏蔽状态无效"));
                if (active) activeInputs.append(input);
            }
            for (const auto& output : outputs)
            {
                const auto active = _activeOutputs[outputIndex++];
                if (active > 1) return PhysicsError(error, QStringLiteral("物理输出的屏蔽状态无效"));
                if (active) activeOutputs.append(output);
            }
            group.insert(QStringLiteral("Input"), activeInputs);
            group.insert(QStringLiteral("Output"), activeOutputs);
            groups[i] = group;
        }
        if (inputIndex != _activeInputs.size() || outputIndex != _activeOutputs.size())
            return PhysicsError(error, QStringLiteral("物理配置快照存在未归属的输入输出行"));
        candidate.insert(QStringLiteral("PhysicsSettings"), groups);
        auto meta = candidate.value(QStringLiteral("Meta")).toObject();
        auto forces = meta.value(QStringLiteral("EffectiveForces")).toObject();
        const auto updateForce = [&](const QString& key, const CubismVector2& actual, float defaultY) {
            auto values = forces.value(key).toObject();
            bool changed = false;
            if (float(values.value(QStringLiteral("X")).toDouble(0)) != actual.X) {
                values.insert(QStringLiteral("X"), double(actual.X)); changed = true;
            }
            if (float(values.value(QStringLiteral("Y")).toDouble(defaultY)) != actual.Y) {
                values.insert(QStringLiteral("Y"), double(actual.Y)); changed = true;
            }
            if (changed) forces.insert(key, values);
            return changed;
        };
        if (!std::isfinite(_options.Gravity.X) || !std::isfinite(_options.Gravity.Y) ||
            !std::isfinite(_options.Wind.X) || !std::isfinite(_options.Wind.Y))
            return PhysicsError(error, QStringLiteral("物理运行时力场不是有限数值"));
        const bool gravityChanged = updateForce(QStringLiteral("Gravity"), _options.Gravity, -1);
        const bool windChanged = updateForce(QStringLiteral("Wind"), _options.Wind, 0);
        if (gravityChanged || windChanged) {
            meta.insert(QStringLiteral("EffectiveForces"), forces);
            candidate.insert(QStringLiteral("Meta"), meta);
        }
        QJsonObject normalized;
        if (!live2dstruct::CPhysicsDocument::Normalize(candidate, normalized, error)) return false;
        result.swap(normalized);
        return true;
    }
    catch (const std::exception&)
    {
        return PhysicsError(error, QStringLiteral("无法捕获物理运行配置"));
    }
}

void CubismPhysics::InvalidateObservation() noexcept
{
    _observation.sampled = false;
    _observationModel = nullptr;
    _observationTime = 0;
}

bool CubismPhysics::SetObservationGroup(int groupIndex)
{
    if (!_physicsRig || groupIndex < -1 || groupIndex >= int(_physicsRig->Settings.GetSize())) return false;
    if (groupIndex == _observationGroup) return true;
    try
    {
        QString id;
        if (groupIndex >= 0)
        {
            const auto groups = QJsonDocument::fromJson(_normalizedBytes).object().value(QStringLiteral("PhysicsSettings")).toArray();
            if (groupIndex >= groups.size()) return false;
            id = groups[groupIndex].toObject().value(QStringLiteral("Id")).toString();
        }
        _observationGroupId.swap(id);
        _observationGroup = groupIndex;
        _observation = {};
        InvalidateObservation();
        return true;
    }
    catch (...) { return false; }
}

bool CubismPhysics::PrepareObservation(const CubismModel& model, PhysicsObservation& result, QString* error) const
{
    if (!_physicsRig || _observationGroup < 0 || _observationGroup >= int(_physicsRig->Settings.GetSize()) || !model.GetModel())
        return PhysicsError(error, QStringLiteral("尚未选择可观察的物理组"));
    const int count = model.GetParameterCount();
    const auto* minimum = Core::csmGetParameterMinimumValues(model.GetModel());
    const auto* maximum = Core::csmGetParameterMaximumValues(model.GetModel());
    const auto* defaults = Core::csmGetParameterDefaultValues(model.GetModel());
    if (count < 0 || (count && (!minimum || !maximum || !defaults)))
        return PhysicsError(error, QStringLiteral("物理观察缺少参数范围"));
    const auto& group = _physicsRig->Settings[_observationGroup];
    if (group.InputCount < 0 || group.OutputCount < 0 || group.ParticleCount < 0 ||
        group.BaseInputIndex < 0 || group.BaseOutputIndex < 0 || group.BaseParticleIndex < 0 ||
        qint64(group.BaseInputIndex) + group.InputCount > _physicsRig->Inputs.GetSize() ||
        qint64(group.BaseOutputIndex) + group.OutputCount > _physicsRig->Outputs.GetSize() ||
        qint64(group.BaseParticleIndex) + group.ParticleCount > _physicsRig->Particles.GetSize() ||
        _activeInputs.size() != _physicsRig->Inputs.GetSize() || _activeOutputs.size() != _physicsRig->Outputs.GetSize())
        return PhysicsError(error, QStringLiteral("物理观察与当前配置不匹配"));
    PhysicsObservation candidate;
    candidate.groupIndex = _observationGroup;
    candidate.groupId = _observationGroupId;
    candidate.sourceDigest = _sourceDigest;
    candidate.sequence = _observationSequence;
    candidate.time = _observationTime;
    candidate.inputs.resize(group.InputCount);
    candidate.outputs.resize(group.OutputCount);
    QHash<CubismIdHandle, int> indices;
    bool indexed = false;
    const auto fill = [&](PhysicsInputObservation& row, CubismIdHandle id, int type, bool active, int index) {
        if (!id || type < CubismPhysicsSource_X || type > CubismPhysicsSource_Angle) return false;
        row.id = QString::fromUtf8(id->GetString().GetRawString());
        row.type = type == CubismPhysicsSource_X ? QStringLiteral("X") : type == CubismPhysicsSource_Y ? QStringLiteral("Y") : QStringLiteral("Angle");
        row.active = active;
        if (index < 0 || index >= count || model.GetParameterId(index) != id)
        {
            if (!indexed)
            {
                for (int i = 0; i < count; ++i) indices.insert(model.GetParameterId(i), i);
                indexed = true;
            }
            index = indices.value(id, -1);
        }
        if (index < 0) return !active;
        row.hasRange = true;
        row.minimum = minimum[index]; row.maximum = maximum[index]; row.defaultValue = defaults[index];
        return std::isfinite(row.minimum) && std::isfinite(row.maximum) && std::isfinite(row.defaultValue) && row.minimum <= row.maximum;
    };
    for (int i = 0; i < group.InputCount; ++i)
    {
        const auto index = group.BaseInputIndex + i;
        const auto& input = _physicsRig->Inputs[index];
        if (!fill(candidate.inputs[i], input.Source.Id, input.Type, _activeInputs[index], input.SourceParameterIndex))
            return PhysicsError(error, QStringLiteral("物理观察的输入引用或范围无效"));
    }
    for (int i = 0; i < group.OutputCount; ++i)
    {
        const auto index = group.BaseOutputIndex + i;
        const auto& output = _physicsRig->Outputs[index];
        if (!fill(candidate.outputs[i], output.Destination.Id, output.Type, _activeOutputs[index], output.DestinationParameterIndex))
            return PhysicsError(error, QStringLiteral("物理观察的输出引用或范围无效"));
    }
    result = std::move(candidate);
    return true;
}

namespace {
bool SameObservationRange(const PhysicsInputObservation& first, const PhysicsInputObservation& second)
{
    return first.id == second.id && first.type == second.type && first.active == second.active && first.hasRange == second.hasRange &&
        first.minimum == second.minimum && first.maximum == second.maximum && first.defaultValue == second.defaultValue;
}
}

bool CubismPhysics::RebindObservationModel(const CubismModel& previous, const CubismModel& replacement) noexcept
{
    if (!previous.GetModel() || !replacement.GetModel() ||
        (_observation.sampled && _observationModel != &previous)) return false;
    const int count = previous.GetParameterCount();
    if (count < 0 || count > 1000000 || count != replacement.GetParameterCount()) return false;
    const auto* oldTypes = Core::csmGetParameterTypes(previous.GetModel());
    const auto* nextTypes = Core::csmGetParameterTypes(replacement.GetModel());
    const auto* oldKeys = Core::csmGetParameterKeyCounts(previous.GetModel());
    const auto* nextKeys = Core::csmGetParameterKeyCounts(replacement.GetModel());
    const auto* oldValues = Core::csmGetParameterKeyValues(previous.GetModel());
    const auto* nextValues = Core::csmGetParameterKeyValues(replacement.GetModel());
    for (const auto& pair : {std::pair{Core::csmGetParameterMinimumValues(previous.GetModel()), Core::csmGetParameterMinimumValues(replacement.GetModel())},
                            std::pair{Core::csmGetParameterMaximumValues(previous.GetModel()), Core::csmGetParameterMaximumValues(replacement.GetModel())},
                            std::pair{Core::csmGetParameterDefaultValues(previous.GetModel()), Core::csmGetParameterDefaultValues(replacement.GetModel())}}) {
        if (count && (!pair.first || !pair.second)) return false;
        for (int index = 0; index < count; ++index)
            if (!std::isfinite(pair.first[index]) || pair.first[index] != pair.second[index]) return false;
    }
    if (count && (!oldTypes || !nextTypes || !oldKeys || !nextKeys)) return false;
    for (int index = 0; index < count; ++index) {
        if (previous.GetParameterId(index) != replacement.GetParameterId(index) || oldTypes[index] != nextTypes[index] ||
            oldKeys[index] < 0 || oldKeys[index] > 1000000 || oldKeys[index] != nextKeys[index]) return false;
        if (oldKeys[index] && (!oldValues || !nextValues || !oldValues[index] || !nextValues[index])) return false;
        for (int key = 0; key < oldKeys[index]; ++key)
            if (!std::isfinite(oldValues[index][key]) || oldValues[index][key] != nextValues[index][key]) return false;
    }
    if (_observationModel == &previous) _observationModel = &replacement;
    return true;
}

bool CubismPhysics::CaptureObservation(const CubismModel& model, PhysicsObservation& result, QString* error) const
{
    if (error) error->clear();
    try
    {
        if (_observation.sampled && _observationModel != &model)
            return PhysicsError(error, QStringLiteral("物理观察样本属于其他模型"));
        PhysicsObservation candidate;
        if (!PrepareObservation(model, candidate, error)) return false;
        if (_observation.sampled)
        {
            if (_observation.inputs.size() != candidate.inputs.size() || _observation.outputs.size() != candidate.outputs.size())
                return PhysicsError(error, QStringLiteral("物理观察样本已失效"));
            for (int i = 0; i < candidate.inputs.size(); ++i)
                if (!SameObservationRange(candidate.inputs[i], _observation.inputs[i]))
                    return PhysicsError(error, QStringLiteral("物理观察的输入定义已改变"));
            for (int i = 0; i < candidate.outputs.size(); ++i)
                if (!SameObservationRange(candidate.outputs[i], _observation.outputs[i]))
                    return PhysicsError(error, QStringLiteral("物理观察的输出定义已改变"));
            candidate = _observation;
        }
        result = std::move(candidate);
        return true;
    }
    catch (...) { return PhysicsError(error, QStringLiteral("无法读取物理观察样本")); }
}

bool CubismPhysics::BeginObservation(const CubismModel& model, csmFloat32 deltaTime, PhysicsObservation& result) noexcept
{
    if (_observationGroup < 0 || _observationSequence == std::numeric_limits<quint64>::max()) return false;
    try
    {
        PhysicsObservation candidate;
        if (!PrepareObservation(model, candidate)) return false;
        if (_observation.sampled && _observationModel == &model && _observation.inputs.size() == candidate.inputs.size())
            for (int i = 0; i < candidate.inputs.size(); ++i)
                if (SameObservationRange(candidate.inputs[i], _observation.inputs[i]))
                {
                    candidate.inputs[i].value = _observation.inputs[i].value;
                    candidate.inputs[i].sampled = _observation.inputs[i].sampled;
                }
        candidate.sequence = _observationSequence + 1;
        candidate.time = _observationTime + double(deltaTime);
        if (!std::isfinite(candidate.time)) return false;
        candidate.particles.resize(_physicsRig->Settings[_observationGroup].ParticleCount);
        result = std::move(candidate);
        return true;
    }
    catch (...) { return false; }
}

void CubismPhysics::FinishObservation(const CubismModel& model, PhysicsObservation& result) noexcept
{
    const auto& group = _physicsRig->Settings[_observationGroup];
    for (int i = 0; i < group.ParticleCount; ++i)
    {
        const auto& position = _physicsRig->Particles[group.BaseParticleIndex + i].Position;
        if (!std::isfinite(position.X) || !std::isfinite(position.Y)) return;
        result.particles[i] = QPointF(position.X, position.Y);
    }
    for (const auto& input : result.inputs)
        if (input.sampled && !std::isfinite(input.value)) return;
    for (const auto& output : result.outputs)
        if (output.sampled && (!std::isfinite(output.value) || !std::isfinite(output.rawValue) || !std::isfinite(output.clampedValue))) return;
    result.sampled = true;
    _observationSequence = result.sequence;
    _observationTime = result.time;
    static_assert(std::is_nothrow_move_assignable_v<PhysicsObservation>);
    _observation = std::move(result);
    _observationModel = &model;
}

CubismPhysics::PreparedRuntimeState::PreparedRuntimeState()
    : work(nullptr, CubismPhysics::Delete)
{}

CubismPhysics::PreparedRuntimeState::~PreparedRuntimeState() = default;
CubismPhysics::PreparedRuntimeState::PreparedRuntimeState(PreparedRuntimeState&&) noexcept = default;
CubismPhysics::PreparedRuntimeState& CubismPhysics::PreparedRuntimeState::operator=(PreparedRuntimeState&&) noexcept = default;

bool CubismPhysics::PrepareModelBinding(const CubismModel& model, const ParameterPhysicsReferences& references,
                                       ParameterDefinitionState& result, QString* error) const
{
    if (!model.GetModel()) return PhysicsError(error, QStringLiteral("物理绑定缺少模型"));
    const int count = model.GetParameterCount();
    const auto* values = Core::csmGetParameterValues(model.GetModel());
    if (count < 0 || (count && !values)) return PhysicsError(error, QStringLiteral("物理绑定缺少参数值"));
    for (int i = 0; i < count; ++i)
        if (!std::isfinite(values[i]) || !std::isfinite(model.GetParameterDefaultValue(i)))
            return PhysicsError(error, QStringLiteral("物理绑定的参数值无效"));
    return PrepareParameterStructureChange(model, model, result, error, &references);
}

bool CubismPhysics::HasFiniteRuntimeState(QString* error) const
{
    const auto finiteVector = [](const CubismVector2& value) { return std::isfinite(value.X) && std::isfinite(value.Y); };
    if (!_physicsRig || !std::isfinite(_currentRemainTime) || _currentRemainTime < 0 ||
        !finiteVector(_options.Gravity) || !finiteVector(_options.Wind) ||
        _activeInputs.size() != _physicsRig->Inputs.GetSize() || _activeOutputs.size() != _physicsRig->Outputs.GetSize() ||
        _currentRigOutputs.GetSize() != _physicsRig->Settings.GetSize() ||
        _previousRigOutputs.GetSize() != _physicsRig->Settings.GetSize())
        return PhysicsError(error, QStringLiteral("物理运行状态与配置不匹配"));
    for (const auto* values : {&_parameterCaches, &_parameterInputCaches})
        for (float value : *values) if (!std::isfinite(value)) return PhysicsError(error, QStringLiteral("物理缓存含非有限值"));
    for (int i = 0; i < _physicsRig->Particles.GetSize(); ++i)
    {
        const auto& particle = _physicsRig->Particles[i];
        for (const auto* value : {&particle.InitialPosition, &particle.Position, &particle.LastPosition,
                                 &particle.LastGravity, &particle.Force, &particle.Velocity})
            if (!finiteVector(*value)) return PhysicsError(error, QStringLiteral("物理摆锤含非有限值"));
    }
    for (int i = 0; i < _physicsRig->Settings.GetSize(); ++i)
    {
        const auto count = _physicsRig->Settings[i].OutputCount;
        if (_currentRigOutputs[i].outputs.GetSize() != count || _previousRigOutputs[i].outputs.GetSize() != count)
            return PhysicsError(error, QStringLiteral("物理输出缓存与配置不匹配"));
        for (int j = 0; j < count; ++j)
            if (!std::isfinite(_currentRigOutputs[i].outputs[j]) || !std::isfinite(_previousRigOutputs[i].outputs[j]))
                return PhysicsError(error, QStringLiteral("物理输出缓存含非有限值"));
    }
    for (int i = 0; i < _physicsRig->Outputs.GetSize(); ++i)
        if (!std::isfinite(_physicsRig->Outputs[i].ValueBelowMinimum) || !std::isfinite(_physicsRig->Outputs[i].ValueExceededMaximum))
            return PhysicsError(error, QStringLiteral("物理输出统计含非有限值"));
    return true;
}

void CubismPhysics::CopyDynamicStateFrom(const CubismPhysics& previous)
{
    _currentRemainTime = previous._currentRemainTime;
    for (int i = 0; i < _physicsRig->Particles.GetSize(); ++i)
    {
        auto& target = _physicsRig->Particles[i];
        const auto& source = previous._physicsRig->Particles[i];
        target.Position = source.Position;
        target.LastPosition = source.LastPosition;
        target.LastGravity = source.LastGravity;
        target.Velocity = source.Velocity;
        target.Force = source.Force;
    }
    for (int i = 0; i < _physicsRig->Settings.GetSize(); ++i)
        for (int j = 0; j < _physicsRig->Settings[i].OutputCount; ++j)
        {
            _currentRigOutputs[i].outputs[j] = previous._currentRigOutputs[i].outputs[j];
            _previousRigOutputs[i].outputs[j] = previous._previousRigOutputs[i].outputs[j];
        }
    for (int i = 0; i < _physicsRig->Outputs.GetSize(); ++i)
    {
        _physicsRig->Outputs[i].ValueBelowMinimum = previous._physicsRig->Outputs[i].ValueBelowMinimum;
        _physicsRig->Outputs[i].ValueExceededMaximum = previous._physicsRig->Outputs[i].ValueExceededMaximum;
    }
}

bool CubismPhysics::PrepareRuntimeState(const CubismPhysics* previous, const CubismModel& previousModel,
                                       const CubismModel& replacementModel, const ParameterPhysicsReferences& references,
                                       PreparedRuntimeState& result, StatePolicy policy, QString* error) const
{
    if (error) error->clear();
    if ((policy != StatePolicy::PreserveCompatible && policy != StatePolicy::Stabilize) || _normalizedBytes.isEmpty())
        return PhysicsError(error, QStringLiteral("物理运行状态准备请求无效"));
    if (!HasFiniteRuntimeState(error)) return false;
    try
    {
        PreparedRuntimeState candidate;
        candidate.work.reset(Create(reinterpret_cast<const csmByte*>(_normalizedBytes.constData()), _normalizedBytes.size(), error));
        if (!candidate.work) return false;
        auto& work = *candidate.work;
        work._sourceDigest = _sourceDigest;
        work._options = _options;
        ParameterDefinitionState binding;
        if (!work.PrepareModelBinding(replacementModel, references, binding, error)) return false;
        work.CommitParameterDefinitionChange(binding);
        const bool compatible = policy == StatePolicy::PreserveCompatible && previous && !_topology.isEmpty() &&
            _topology == previous->_topology && work._activeInputs == previous->_activeInputs && work._activeOutputs == previous->_activeOutputs;
        if (compatible)
        {
            if (!previous->HasFiniteRuntimeState(error)) return false;
            ParameterDefinitionState migration;
            if (!previous->PrepareParameterStructureChange(previousModel, replacementModel, migration, error)) return false;
            work._parameterCaches.swap(migration.parameters);
            work._parameterInputCaches.swap(migration.inputs);
            work.CopyDynamicStateFrom(*previous);
        }
        else
        {
            QVector<float> values;
            if (!work.StabilizeValues(replacementModel, values, error)) return false;
            candidate.stabilized = true;
        }
        if (!work.HasFiniteRuntimeState(error)) return false;
        candidate.owner = this;
        candidate.revision = _stateRevision;
        result = std::move(candidate);
        return true;
    }
    catch (const std::exception&)
    {
        return PhysicsError(error, QStringLiteral("无法准备物理运行状态"));
    }
}

bool CubismPhysics::ValidatePreparedRuntimeState(const PreparedRuntimeState& state) const noexcept
{
    return state.owner == this && state.revision == _stateRevision && state.work && state.work->_physicsRig;
}

bool CubismPhysics::CommitRuntimeState(PreparedRuntimeState& state) noexcept
{
    if (!ValidatePreparedRuntimeState(state)) return false;
    auto& work = *state.work;
    std::swap(_physicsRig, work._physicsRig);
    _currentRigOutputs.Swap(work._currentRigOutputs);
    _previousRigOutputs.Swap(work._previousRigOutputs);
    std::swap(_currentRemainTime, work._currentRemainTime);
    _parameterCaches.swap(work._parameterCaches);
    _parameterInputCaches.swap(work._parameterInputCaches);
    _activeInputs.swap(work._activeInputs);
    _activeOutputs.swap(work._activeOutputs);
    InvalidateObservation();
    ++_stateRevision;
    state.owner = nullptr;
    return true;
}

bool CubismPhysics::PrepareParameterDefinitionChange(const CubismModel& previous, const CubismModel& replacement,
                                                     ParameterDefinitionState& result, QString* error) const
{
    if (error) error->clear();
    const auto fail = [&](const QString& reason) { if (error) *error = reason; return false; };
    const int count = previous.GetParameterCount();
    if (count < 0 || replacement.GetParameterCount() != count ||
        _parameterCaches.size() > count || _parameterInputCaches.size() > count)
        return fail(QStringLiteral("物理参数缓存与模型定义不匹配"));
    try {
        ParameterDefinitionState candidate;
        candidate.parameters.reserve(_parameterCaches.size());
        candidate.inputs.reserve(_parameterInputCaches.size());
        for (int index = 0; index < count; ++index) {
            const float oldMinimum = previous.GetParameterMinimumValue(index), oldMaximum = previous.GetParameterMaximumValue(index);
            const float minimum = replacement.GetParameterMinimumValue(index), maximum = replacement.GetParameterMaximumValue(index);
            if (previous.GetParameterId(index) != replacement.GetParameterId(index) ||
                !std::isfinite(oldMinimum) || !std::isfinite(oldMaximum) || oldMinimum > oldMaximum ||
                !std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum)
                return fail(QStringLiteral("物理参数身份或范围无效"));
            const bool changed = oldMinimum != minimum || oldMaximum != maximum;
            const auto append = [&](const auto& source, QVector<float>& target) {
                if (index >= source.size()) return true;
                float value = source[index];
                if (!std::isfinite(value)) return false;
                if (changed) {
                    if (value < minimum) value = minimum;
                    else if (value > maximum) value = maximum;
                }
                target.append(value);
                return true;
            };
            if (!append(_parameterCaches, candidate.parameters) || !append(_parameterInputCaches, candidate.inputs))
                return fail(QStringLiteral("物理参数缓存含非有限值"));
        }
        candidate.owner = this;
        result = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        return fail(QStringLiteral("无法准备物理参数缓存"));
    }
}

bool CubismPhysics::PrepareParameterStructureChange(const CubismModel& previous, const CubismModel& replacement,
                                                    ParameterDefinitionState& result, QString* error,
                                                    const ParameterPhysicsReferences* references) const
{
    if (error) error->clear();
    const auto fail = [&](const QString& reason) { if (error) *error = reason; return false; };
    const int oldCount = previous.GetParameterCount(), count = replacement.GetParameterCount();
    if (!_physicsRig || oldCount < 0 || count < 0 || _parameterCaches.size() > oldCount || _parameterInputCaches.size() > oldCount)
        return fail(QStringLiteral("物理参数缓存与模型结构不匹配"));
    try {
        ParameterDefinitionState candidate;
        if (_activeInputs.size() != _physicsRig->Inputs.GetSize() || _activeOutputs.size() != _physicsRig->Outputs.GetSize())
            return fail(QStringLiteral("物理引用状态与原始配置不匹配"));
        candidate.activeInputs = _activeInputs;
        candidate.activeOutputs = _activeOutputs;
        if (references) {
            if ((!references->sourceDigest.isEmpty() || !references->suppressedInputs.isEmpty() || !references->suppressedOutputs.isEmpty()) &&
                (references->sourceDigest.size() != 32 || references->sourceDigest != _sourceDigest))
                return fail(QStringLiteral("物理引用状态与原始文件摘要不匹配"));
            const auto prepareMask = [&](const QMap<int, QString>& suppressed, const auto& rows, QVector<quint8>& active, bool input) {
                active.fill(1, rows.GetSize());
                for (auto row = suppressed.cbegin(); row != suppressed.cend(); ++row) {
                    if (row.key() < 0 || row.key() >= rows.GetSize()) return false;
                    const auto id = input ? _physicsRig->Inputs[row.key()].Source.Id : _physicsRig->Outputs[row.key()].Destination.Id;
                    if (!id || row.value().isEmpty() || row.value() != QString::fromUtf8(id->GetString().GetRawString())) return false;
                    active[row.key()] = 0;
                }
                return true;
            };
            if (!prepareMask(references->suppressedInputs, _physicsRig->Inputs, candidate.activeInputs, true) ||
                !prepareMask(references->suppressedOutputs, _physicsRig->Outputs, candidate.activeOutputs, false))
                return fail(QStringLiteral("物理引用的原始行或参数 ID 不匹配"));
        }
        QHash<CubismIdHandle, int> newIndices;
        for (int i = 0; i < count; ++i) {
            const auto id = replacement.GetParameterId(i);
            if (!id || newIndices.contains(id)) return fail(QStringLiteral("候选物理参数 ID 无效或重复"));
            const float minimum = replacement.GetParameterMinimumValue(i), maximum = replacement.GetParameterMaximumValue(i);
            if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum)
                return fail(QStringLiteral("候选物理参数范围无效"));
            newIndices.insert(id, i);
        }
        for (int i = 0; i < _physicsRig->Inputs.GetSize(); ++i) {
            const int index = candidate.activeInputs[i] ? newIndices.value(_physicsRig->Inputs[i].Source.Id, -1) : -1;
            if (candidate.activeInputs[i] && index < 0) return fail(QStringLiteral("不能删除物理输入参数：%1").arg((_physicsRig->Inputs[i].Source.Id ? QString::fromUtf8(_physicsRig->Inputs[i].Source.Id->GetString().GetRawString()) : QStringLiteral("<missing>"))));
            candidate.inputIndices.append(index);
        }
        for (int i = 0; i < _physicsRig->Outputs.GetSize(); ++i) {
            const int index = candidate.activeOutputs[i] ? newIndices.value(_physicsRig->Outputs[i].Destination.Id, -1) : -1;
            if (candidate.activeOutputs[i] && index < 0) return fail(QStringLiteral("不能删除物理输出参数：%1").arg((_physicsRig->Outputs[i].Destination.Id ? QString::fromUtf8(_physicsRig->Outputs[i].Destination.Id->GetString().GetRawString()) : QStringLiteral("<missing>"))));
            candidate.outputIndices.append(index);
        }
        QHash<CubismIdHandle, int> oldIndices;
        for (int i = 0; i < oldCount; ++i) oldIndices.insert(previous.GetParameterId(i), i);
        const auto* current = Core::csmGetParameterValues(replacement.GetModel());
        if (count && !current) return fail(QStringLiteral("候选模型参数值缺失"));
        const auto migrate = [&](const QVector<float>& source, QVector<float>& target) {
            for (float value : source) if (!std::isfinite(value)) return false;
            if (source.isEmpty()) return true;
            target.reserve(count);
            for (int i = 0; i < count; ++i) {
                const auto old = oldIndices.constFind(replacement.GetParameterId(i));
                float value = current[i];
                if (old != oldIndices.cend() && *old < source.size()) {
                    value = source[*old];
                    const float minimum = replacement.GetParameterMinimumValue(i), maximum = replacement.GetParameterMaximumValue(i);
                    if (previous.GetParameterMinimumValue(*old) != minimum || previous.GetParameterMaximumValue(*old) != maximum) {
                        if (value < minimum) value = minimum;
                        else if (value > maximum) value = maximum;
                    }
                }
                if (!std::isfinite(value)) return false;
                target.append(value);
            }
            return true;
        };
        if (!migrate(_parameterCaches, candidate.parameters) || !migrate(_parameterInputCaches, candidate.inputs))
            return fail(QStringLiteral("物理参数缓存含非有限值"));
        candidate.structure = true;
        candidate.owner = this;
        result = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        return fail(QStringLiteral("无法准备物理参数结构"));
    }
}

void CubismPhysics::CommitParameterDefinitionChange(ParameterDefinitionState& state) noexcept
{
    if (state.owner != this) return;
    if (state.structure) {
        if (!_physicsRig || state.inputIndices.size() != _physicsRig->Inputs.GetSize() ||
            state.outputIndices.size() != _physicsRig->Outputs.GetSize() ||
            state.activeInputs.size() != state.inputIndices.size() || state.activeOutputs.size() != state.outputIndices.size()) return;
        for (int i = 0; i < state.inputIndices.size(); ++i) _physicsRig->Inputs[i].SourceParameterIndex = state.inputIndices.at(i);
        for (int i = 0; i < state.outputIndices.size(); ++i) _physicsRig->Outputs[i].DestinationParameterIndex = state.outputIndices.at(i);
        _activeInputs.swap(state.activeInputs);
        _activeOutputs.swap(state.activeOutputs);
    } else if (state.parameters.size() != _parameterCaches.size() || state.inputs.size() != _parameterInputCaches.size()) return;
    _parameterCaches.swap(state.parameters);
    _parameterInputCaches.swap(state.inputs);
    InvalidateObservation();
    ++_stateRevision;
    state.owner = nullptr;
}

bool CubismPhysics::ResolveParameterIndices(CubismModel* model)
{
    if (!model || !model->GetModel() || !_physicsRig) return false;
    const int count = model->GetParameterCount();
    if (count < 0 || (count && (!Core::csmGetParameterValues(model->GetModel()) ||
        !Core::csmGetParameterMinimumValues(model->GetModel()) || !Core::csmGetParameterMaximumValues(model->GetModel()) ||
        !Core::csmGetParameterDefaultValues(model->GetModel())))) return false;
    const auto resolve = [&](CubismIdHandle id, int& index) {
        if (index < 0 || index >= count || model->GetParameterId(index) != id) index = model->FindRealParameterIndex(id);
        return index >= 0 && index < count;
    };
    if (_activeInputs.size() != _physicsRig->Inputs.GetSize() || _activeOutputs.size() != _physicsRig->Outputs.GetSize()) return false;
    for (int i = 0; i < _physicsRig->Inputs.GetSize(); ++i)
        if (_activeInputs[i] && !resolve(_physicsRig->Inputs[i].Source.Id, _physicsRig->Inputs[i].SourceParameterIndex)) return false;
    for (int i = 0; i < _physicsRig->Outputs.GetSize(); ++i)
        if (_activeOutputs[i] && !resolve(_physicsRig->Outputs[i].Destination.Id, _physicsRig->Outputs[i].DestinationParameterIndex)) return false;
    return true;
}

/// Initializes physics.
///
/// @param  physics  Target rig.
void CubismPhysics::Initialize()
{
    CubismPhysicsParticle* strand;
    CubismPhysicsSubRig* currentSetting;
    csmInt32 i, settingIndex;
    CubismVector2 radius;

    for (settingIndex = 0; settingIndex < _physicsRig->SubRigCount; ++settingIndex)
    {
        currentSetting = &_physicsRig->Settings[settingIndex];
        strand = &_physicsRig->Particles[currentSetting->BaseParticleIndex];

        // Initialize the top of particle.
        strand[0].InitialPosition = CubismVector2(0.0f, 0.0f);
        strand[0].LastPosition = strand[0].InitialPosition;
        strand[0].LastGravity = CubismVector2(0.0f, -1.0f);
        strand[0].LastGravity.Y *= -1.0f;
        strand[0].Velocity = CubismVector2(0.0f, 0.0f);
        strand[0].Force = CubismVector2(0.0f, 0.0f);

        // Initialize particles.
        for (i = 1; i < currentSetting->ParticleCount; ++i)
        {
            radius = CubismVector2(0.0f, 0.0f);
            radius.Y = strand[i].Radius;
            strand[i].InitialPosition = strand[i - 1].InitialPosition + radius;
            strand[i].Position = strand[i].InitialPosition;
            strand[i].LastPosition = strand[i].InitialPosition;
            strand[i].LastGravity = CubismVector2(0.0f, -1.0f);
            strand[i].LastGravity.Y *= -1.0f;
            strand[i].Velocity = CubismVector2(0.0f, 0.0f);
            strand[i].Force = CubismVector2(0.0f, 0.0f);
        }
    }
}

/// Reset the physics states.
void CubismPhysics::Reset()
{
    InvalidateObservation();
    ++_stateRevision;
    // set default options.
    _options.Gravity.Y = -1.0f;
    _options.Gravity.X = 0.0f;
    _options.Wind.X = 0.0f;
    _options.Wind.Y = 0.0f;

    _physicsRig->Gravity.X = 0.0f;
    _physicsRig->Gravity.Y = 0.0f;
    _physicsRig->Wind.X = 0.0f;
    _physicsRig->Wind.Y = 0.0f;

    Initialize();
}

CubismPhysics* CubismPhysics::Create(const csmByte* buffer, csmSizeInt size, QString* error)
{
    if (error) error->clear();
    if (!buffer || size <= 0 || size > 16 * 1024 * 1024)
    {
        PhysicsError(error, QStringLiteral("物理配置数据为空或过大"));
        return nullptr;
    }
    try
    {
        const QByteArray source(reinterpret_cast<const char*>(buffer), size);
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(source, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            PhysicsError(error, QStringLiteral("物理配置不是有效的 JSON 对象"));
            return nullptr;
        }
        QJsonObject normalized;
        if (!live2dstruct::CPhysicsDocument::Normalize(document.object(), normalized, error)) return nullptr;
        auto groups = normalized.value(QStringLiteral("PhysicsSettings")).toArray();
        for (qsizetype i = 0; i < groups.size(); ++i)
        {
            auto group = groups[i].toObject();
            for (const auto& field : {QStringLiteral("Input"), QStringLiteral("Output")})
                if (!group.contains(field)) group.insert(field, QJsonArray{});
            groups[i] = group;
        }
        normalized.insert(QStringLiteral("PhysicsSettings"), groups);
        const auto bytes = QJsonDocument(normalized).toJson(QJsonDocument::Compact);
        if (bytes.size() > 16 * 1024 * 1024)
        {
            PhysicsError(error, QStringLiteral("规范化的物理配置过大"));
            return nullptr;
        }
        std::unique_ptr<CubismPhysics, void (*)(CubismPhysics*)> ret(CSM_NEW CubismPhysics(), Delete);
        ret->Parse(reinterpret_cast<const csmByte*>(bytes.constData()), bytes.size());
        if (!ret->_isJsonValid)
        {
            PhysicsError(error, QStringLiteral("无法解析物理配置"));
            return nullptr;
        }
        const auto forces = normalized.value(QStringLiteral("Meta")).toObject().value(QStringLiteral("EffectiveForces")).toObject();
        const auto gravity = forces.value(QStringLiteral("Gravity")).toObject();
        const auto wind = forces.value(QStringLiteral("Wind")).toObject();
        ret->_options.Gravity = CubismVector2(gravity.value(QStringLiteral("X")).toDouble(0), gravity.value(QStringLiteral("Y")).toDouble(-1));
        ret->_options.Wind = CubismVector2(wind.value(QStringLiteral("X")).toDouble(0), wind.value(QStringLiteral("Y")).toDouble(0));
        ret->_sourceDigest = QCryptographicHash::hash(source, QCryptographicHash::Sha256);
        ret->_normalizedBytes = bytes;
        ret->_topology = PhysicsTopology(normalized);
        return ret.release();
    }
    catch (const std::exception&)
    {
        PhysicsError(error, QStringLiteral("无法创建物理配置"));
        return nullptr;
    }
}

void CubismPhysics::Delete(CubismPhysics* physics)
{
    CSM_DELETE_SELF(CubismPhysics, physics);
}

void CubismPhysics::Parse(const csmByte* physicsJson, csmSizeInt size)
{
    _physicsRig = CSM_NEW CubismPhysicsRig;

    const auto deleteJson = [](CubismPhysicsJson* value) { CSM_DELETE(value); };
    const std::unique_ptr<CubismPhysicsJson, decltype(deleteJson)> json(CSM_NEW CubismPhysicsJson(physicsJson, size), deleteJson);

    _isJsonValid = json->IsValid();

    if (!_isJsonValid)
    {
        return;
    }

    _physicsRig->Gravity = json->GetGravity();
    _physicsRig->Wind = json->GetWind();
    _physicsRig->SubRigCount = json->GetSubRigCount();

    _physicsRig->Fps = json->GetFps();

    _physicsRig->Settings.UpdateSize(_physicsRig->SubRigCount, CubismPhysicsSubRig(), true);
    _physicsRig->Inputs.UpdateSize(json->GetTotalInputCount(), CubismPhysicsInput(), true);
    _physicsRig->Outputs.UpdateSize(json->GetTotalOutputCount(), CubismPhysicsOutput(), true);
    _activeInputs.fill(1, _physicsRig->Inputs.GetSize());
    _activeOutputs.fill(1, _physicsRig->Outputs.GetSize());
    _physicsRig->Particles.UpdateSize(json->GetVertexCount(), CubismPhysicsParticle(), true);

    _currentRigOutputs.Clear();
    _previousRigOutputs.Clear();

    csmInt32 inputIndex = 0, outputIndex = 0, particleIndex = 0;
    for (csmUint32 i = 0; i < _physicsRig->Settings.GetSize(); ++i)
    {
        _physicsRig->Settings[i].NormalizationPosition.Minimum = json->GetNormalizationPositionMinimumValue(i);
        _physicsRig->Settings[i].NormalizationPosition.Maximum = json->GetNormalizationPositionMaximumValue(i);
        _physicsRig->Settings[i].NormalizationPosition.Default = json->GetNormalizationPositionDefaultValue(i);

        _physicsRig->Settings[i].NormalizationAngle.Minimum = json->GetNormalizationAngleMinimumValue(i);
        _physicsRig->Settings[i].NormalizationAngle.Maximum = json->GetNormalizationAngleMaximumValue(i);
        _physicsRig->Settings[i].NormalizationAngle.Default = json->GetNormalizationAngleDefaultValue(i);

        // Input
        _physicsRig->Settings[i].InputCount = json->GetInputCount(i);
        _physicsRig->Settings[i].BaseInputIndex = inputIndex;
        for (csmInt32 j = 0; j < _physicsRig->Settings[i].InputCount; ++j)
        {
            _physicsRig->Inputs[inputIndex + j].SourceParameterIndex = -1;
            _physicsRig->Inputs[inputIndex + j].Weight = json->GetInputWeight(i, j);
            _physicsRig->Inputs[inputIndex + j].Reflect = json->GetInputReflect(i, j);

            if (strcmp(json->GetInputType(i, j), PhysicsTypeTagX) == 0)
            {
                _physicsRig->Inputs[inputIndex + j].Type = CubismPhysicsSource_X;
                _physicsRig->Inputs[inputIndex + j].GetNormalizedParameterValue = GetInputTranslationXFromNormalizedParameterValue;
            }
            else if (strcmp(json->GetInputType(i, j), PhysicsTypeTagY) == 0)
            {
                _physicsRig->Inputs[inputIndex + j].Type = CubismPhysicsSource_Y;
                _physicsRig->Inputs[inputIndex + j].GetNormalizedParameterValue = GetInputTranslationYFromNormalizedParameterValue;
            }
            else if (strcmp(json->GetInputType(i, j), PhysicsTypeTagAngle) == 0)
            {
                _physicsRig->Inputs[inputIndex + j].Type = CubismPhysicsSource_Angle;
                _physicsRig->Inputs[inputIndex + j].GetNormalizedParameterValue = GetInputAngleFromNormalizedParameterValue;
            }

            _physicsRig->Inputs[inputIndex + j].Source.TargetType = CubismPhysicsTargetType_Parameter;
            _physicsRig->Inputs[inputIndex + j].Source.Id = json->GetInputSourceId(i, j);
        }
        inputIndex += _physicsRig->Settings[i].InputCount;

        // Output
        _physicsRig->Settings[i].OutputCount = json->GetOutputCount(i);
        _physicsRig->Settings[i].BaseOutputIndex = outputIndex;

        PhysicsOutput currentRigOutput;
        currentRigOutput.outputs.Resize(_physicsRig->Settings[i].OutputCount);
        _currentRigOutputs.PushBack(currentRigOutput);

        PhysicsOutput previousRigOutput;
        previousRigOutput.outputs.Resize(_physicsRig->Settings[i].OutputCount);
        _previousRigOutputs.PushBack(previousRigOutput);

        for (csmInt32 j = 0; j < _physicsRig->Settings[i].OutputCount; ++j)
        {
            _physicsRig->Outputs[outputIndex + j].DestinationParameterIndex = -1;
            _physicsRig->Outputs[outputIndex + j].VertexIndex = json->GetOutputVertexIndex(i, j);
            _physicsRig->Outputs[outputIndex + j].AngleScale = json->GetOutputAngleScale(i, j);
            _physicsRig->Outputs[outputIndex + j].Weight = json->GetOutputWeight(i, j);
            _physicsRig->Outputs[outputIndex + j].Destination.TargetType = CubismPhysicsTargetType_Parameter;

            _physicsRig->Outputs[outputIndex + j].Destination.Id = json->GetOutputsDestinationId(i, j);
            if (strcmp(json->GetOutputType(i, j), PhysicsTypeTagX) == 0)
            {
                _physicsRig->Outputs[outputIndex + j].TranslationScale.X = json->GetOutputAngleScale(i, j);
                _physicsRig->Outputs[outputIndex + j].Type = CubismPhysicsSource_X;
                _physicsRig->Outputs[outputIndex + j].GetValue = GetOutputTranslationX;
                _physicsRig->Outputs[outputIndex + j].GetScale = GetOutputScaleTranslationX;
            }
            else if (strcmp(json->GetOutputType(i, j), PhysicsTypeTagY) == 0)
            {
                _physicsRig->Outputs[outputIndex + j].TranslationScale.Y = json->GetOutputAngleScale(i, j);
                _physicsRig->Outputs[outputIndex + j].Type = CubismPhysicsSource_Y;
                _physicsRig->Outputs[outputIndex + j].GetValue = GetOutputTranslationY;
                _physicsRig->Outputs[outputIndex + j].GetScale = GetOutputScaleTranslationY;
            }
            else if (strcmp(json->GetOutputType(i, j), PhysicsTypeTagAngle) == 0)
            {
                _physicsRig->Outputs[outputIndex + j].Type = CubismPhysicsSource_Angle;
                _physicsRig->Outputs[outputIndex + j].GetValue = GetOutputAngle;
                _physicsRig->Outputs[outputIndex + j].GetScale = GetOutputScaleAngle;
            }

            _physicsRig->Outputs[outputIndex + j].Reflect = json->GetOutputReflect(i, j);
        }
        outputIndex += _physicsRig->Settings[i].OutputCount;

        // Particle
        _physicsRig->Settings[i].ParticleCount = json->GetParticleCount(i);
        _physicsRig->Settings[i].BaseParticleIndex = particleIndex;
        for (csmInt32 j = 0; j < _physicsRig->Settings[i].ParticleCount; ++j)
        {
            _physicsRig->Particles[particleIndex + j].Mobility = json->GetParticleMobility(i, j);
            _physicsRig->Particles[particleIndex + j].Delay = json->GetParticleDelay(i, j);
            _physicsRig->Particles[particleIndex + j].Acceleration = json->GetParticleAcceleration(i, j);
            _physicsRig->Particles[particleIndex + j].Radius = json->GetParticleRadius(i, j);
            _physicsRig->Particles[particleIndex + j].Position = json->GetParticlePosition(i, j);
        }

        particleIndex += _physicsRig->Settings[i].ParticleCount;
    }

    Initialize();

}


void CubismPhysics::Stabilization(CubismModel* model)
{
    if (!model || !HasFiniteRuntimeState()) return;
    try
    {
        ParameterPhysicsReferences references;
        references.sourceDigest = _sourceDigest;
        for (int i = 0; i < _activeInputs.size(); ++i)
            if (!_activeInputs[i]) references.suppressedInputs.insert(i, QString::fromUtf8(_physicsRig->Inputs[i].Source.Id->GetString().GetRawString()));
        for (int i = 0; i < _activeOutputs.size(); ++i)
            if (!_activeOutputs[i]) references.suppressedOutputs.insert(i, QString::fromUtf8(_physicsRig->Outputs[i].Destination.Id->GetString().GetRawString()));
        PreparedRuntimeState prepared;
        prepared.work.reset(Create(reinterpret_cast<const csmByte*>(_normalizedBytes.constData()), _normalizedBytes.size()));
        if (!prepared.work) return;
        auto& work = *prepared.work;
        work._sourceDigest = _sourceDigest;
        work._options = _options;
        ParameterDefinitionState binding;
        if (!work.PrepareModelBinding(*model, references, binding)) return;
        work.CommitParameterDefinitionChange(binding);
        work.CopyDynamicStateFrom(*this);
        QVector<float> output;
        if (!work.StabilizeValues(*model, output)) return;
        auto* values = Core::csmGetParameterValues(model->GetModel());
        if (model->GetParameterCount() && !values) return;
        prepared.owner = this;
        prepared.revision = _stateRevision;
        prepared.stabilized = true;
        if (!CommitRuntimeState(prepared)) return;
        for (int i = 0; i < output.size(); ++i) values[i] = output.at(i);
    }
    catch (const std::exception&) {}
}

bool CubismPhysics::StabilizeValues(const CubismModel& model, QVector<float>& values, QString* error)
{
    csmFloat32 totalAngle;
    csmFloat32 weight;
    csmFloat32 radAngle;
    csmFloat32 outputValue;
    CubismVector2 totalTranslation;
    csmInt32 i, settingIndex, particleIndex;
    CubismPhysicsSubRig* currentSetting;
    CubismPhysicsInput* currentInputs;
    CubismPhysicsOutput* currentOutputs;
    CubismPhysicsParticle* currentParticles;

    csmFloat32* parameterValues;
    const csmFloat32* parameterMaximumValues;
    const csmFloat32* parameterMinimumValues;
    const csmFloat32* parameterDefaultValues;

    const auto* sourceValues = Core::csmGetParameterValues(model.GetModel());
    if (model.GetParameterCount() && !sourceValues) return PhysicsError(error, QStringLiteral("物理稳定化缺少参数值"));
    values.resize(model.GetParameterCount());
    for (int index = 0; index < values.size(); ++index)
    {
        if (!std::isfinite(sourceValues[index])) return PhysicsError(error, QStringLiteral("物理稳定化的参数值无效"));
        values[index] = sourceValues[index];
    }
    parameterValues = values.data();
    parameterMaximumValues = Core::csmGetParameterMaximumValues(model.GetModel());
    parameterMinimumValues = Core::csmGetParameterMinimumValues(model.GetModel());
    parameterDefaultValues = Core::csmGetParameterDefaultValues(model.GetModel());

    if (_parameterCaches.size() < model.GetParameterCount())
    {
        _parameterCaches.resize(model.GetParameterCount());
    }
    if (_parameterInputCaches.size() < model.GetParameterCount())
    {
        _parameterInputCaches.resize(model.GetParameterCount());
    }

    for (int j = 0; j < model.GetParameterCount(); ++j)
    {
        _parameterCaches[j] = parameterValues[j];
        _parameterInputCaches[j] = parameterValues[j];
    }

    for (settingIndex = 0; settingIndex < _physicsRig->SubRigCount; ++settingIndex)
    {
        totalAngle = 0.0f;
        totalTranslation.X = 0.0f;
        totalTranslation.Y = 0.0f;
        currentSetting = &_physicsRig->Settings[settingIndex];
        currentInputs = currentSetting->InputCount ? &_physicsRig->Inputs[currentSetting->BaseInputIndex] : nullptr;
        currentOutputs = currentSetting->OutputCount ? &_physicsRig->Outputs[currentSetting->BaseOutputIndex] : nullptr;
        currentParticles = &_physicsRig->Particles[currentSetting->BaseParticleIndex];

        // Load input parameters
        for (i = 0; i < currentSetting->InputCount; ++i)
        {
            if (!_activeInputs[currentSetting->BaseInputIndex + i]) continue;
            weight = currentInputs[i].Weight / MaximumWeight;

            if (currentInputs[i].SourceParameterIndex == -1)
            {
                currentInputs[i].SourceParameterIndex = model.FindRealParameterIndex(currentInputs[i].Source.Id);
            }

            currentInputs[i].GetNormalizedParameterValue(
                &totalTranslation,
                &totalAngle,
                parameterValues[currentInputs[i].SourceParameterIndex],
                parameterMinimumValues[currentInputs[i].SourceParameterIndex],
                parameterMaximumValues[currentInputs[i].SourceParameterIndex],
                parameterDefaultValues[currentInputs[i].SourceParameterIndex],
                &currentSetting->NormalizationPosition,
                &currentSetting->NormalizationAngle,
                currentInputs[i].Reflect,
                weight
            );

            _parameterCaches[currentInputs[i].SourceParameterIndex] =
                parameterValues[currentInputs[i].SourceParameterIndex];
        }

        radAngle = CubismMath::DegreesToRadian(-totalAngle);

        totalTranslation.X = (totalTranslation.X * CubismMath::CosF(radAngle) - totalTranslation.Y * CubismMath::SinF(radAngle));
        totalTranslation.Y = (totalTranslation.X * CubismMath::SinF(radAngle) + totalTranslation.Y * CubismMath::CosF(radAngle));

        // Calculate particles position.
        UpdateParticlesForStabilization(
            currentParticles,
            currentSetting->ParticleCount,
            totalTranslation,
            totalAngle,
            _options.Wind,
            MovementThreshold * currentSetting->NormalizationPosition.Maximum
        );

        // Update output parameters.
        for (i = 0; i < currentSetting->OutputCount; ++i)
        {
            if (!_activeOutputs[currentSetting->BaseOutputIndex + i]) continue;
            particleIndex = currentOutputs[i].VertexIndex;

            if (currentOutputs[i].DestinationParameterIndex == -1)
            {
                currentOutputs[i].DestinationParameterIndex = model.FindRealParameterIndex(
                    currentOutputs[i].Destination.Id);
            }

            if (particleIndex < 1 || particleIndex >= currentSetting->ParticleCount)
            {
                continue;
            }

            CubismVector2 translation;
            translation.X = currentParticles[particleIndex].Position.X - currentParticles[particleIndex - 1].Position.X;
            translation.Y = currentParticles[particleIndex].Position.Y - currentParticles[particleIndex - 1].Position.Y;

            outputValue = currentOutputs[i].GetValue(
                translation,
                currentParticles,
                particleIndex,
                currentOutputs[i].Reflect,
                _options.Gravity
            );

            _currentRigOutputs[settingIndex].outputs[i] = outputValue;
            _previousRigOutputs[settingIndex].outputs[i] = outputValue;

            UpdateOutputParameterValue(
                &parameterValues[currentOutputs[i].DestinationParameterIndex],
                parameterMinimumValues[currentOutputs[i].DestinationParameterIndex],
                parameterMaximumValues[currentOutputs[i].DestinationParameterIndex],
                outputValue,
                &currentOutputs[i]);

            _parameterCaches[currentOutputs[i].DestinationParameterIndex] = parameterValues[currentOutputs[i].DestinationParameterIndex];
        }
    }
    for (float value : values)
        if (!std::isfinite(value)) return PhysicsError(error, QStringLiteral("物理稳定化产生非有限参数值"));
    return HasFiniteRuntimeState(error);
}

/// Pendulum interpolation weights
///
/// 振り子の計算結果は保存され、パラメータへの出力は保存された前回の結果で補間されます。
/// The result of the pendulum calculation is saved and
/// the output to the parameters is interpolated with the saved previous result of the pendulum calculation.
///
/// 図で示すと[1]と[2]で補間されます。
/// The figure shows the interpolation between [1] and [2].
///
/// 補間の重みは最新の振り子計算タイミングと次回のタイミングの間で見た現在時間で決定する。
/// The weight of the interpolation are determined by the current time seen between
/// the latest pendulum calculation timing and the next timing.
///
/// 図で示すと[2]と[4]の間でみた(3)の位置の重みになる。
/// Figure shows the weight of position (3) as seen between [2] and [4].
///
/// 解釈として振り子計算のタイミングと重み計算のタイミングがズレる。
/// As an interpretation, the pendulum calculation and weights are misaligned.
///
/// physics3.jsonにFPS情報が存在しない場合は常に前の振り子状態で設定される。
/// If there is no FPS information in physics3.json, it is always set in the previous pendulum state.
///
/// この仕様は補間範囲を逸脱したことが原因の震えたような見た目を回避を目的にしている。
/// The purpose of this specification is to avoid the quivering appearance caused by deviations from the interpolation range.
///
/// ------------ time -------------->
///
///    　　　　　　　　|+++++|------| <- weight
/// ==[1]====#=====[2]---(3)----(4)
///          ^ output contents
///
/// 1:_previousRigOutputs
/// 2:_currentRigOutputs
/// 3:_currentRemainTime (now rendering)
/// 4:next particles timing
///
/// @param model
/// @param deltaTimeSeconds  rendering delta time.
void CubismPhysics::Evaluate(CubismModel* model, csmFloat32 deltaTimeSeconds)
{
    if (!std::isfinite(deltaTimeSeconds) || deltaTimeSeconds <= 0 || !ResolveParameterIndices(model))
    {
        _observation.sampled = false;
        _observationModel = nullptr;
        return;
    }
    ++_stateRevision;
    csmFloat32 totalAngle;
    csmFloat32 weight;
    csmFloat32 radAngle;
    csmFloat32 outputValue;
    CubismVector2 totalTranslation;
    csmInt32 i, settingIndex, particleIndex;
    CubismPhysicsSubRig* currentSetting;
    CubismPhysicsInput* currentInputs;
    CubismPhysicsOutput* currentOutputs;
    CubismPhysicsParticle* currentParticles;

    if (0.0f >= deltaTimeSeconds)
    {
        return;
    }

    csmFloat32* parameterValues;
    const csmFloat32* parameterMaximumValues;
    const csmFloat32* parameterMinimumValues;
    const csmFloat32* parameterDefaultValues;

    csmFloat32 physicsDeltaTime;
    _currentRemainTime += deltaTimeSeconds;
    if (_currentRemainTime > MaxDeltaTime)
    {
        _currentRemainTime = 0.0f;
    }

    parameterValues = Core::csmGetParameterValues(model->GetModel());
    parameterMaximumValues = Core::csmGetParameterMaximumValues(model->GetModel());
    parameterMinimumValues = Core::csmGetParameterMinimumValues(model->GetModel());
    parameterDefaultValues = Core::csmGetParameterDefaultValues(model->GetModel());
    PhysicsObservation observation;
    const bool observing = BeginObservation(*model, deltaTimeSeconds, observation);
    _observation.sampled = false;
    _observationModel = nullptr;

    if (_parameterCaches.size() < model->GetParameterCount())
    {
        _parameterCaches.resize(model->GetParameterCount());
    }
    if (_parameterInputCaches.size() < model->GetParameterCount())
    {
        _parameterInputCaches.resize(model->GetParameterCount());
        for (int j = 0; j < model->GetParameterCount(); ++j) {
            _parameterInputCaches[j] = parameterValues[j];
        }
    }

    if (_physicsRig->Fps > 0.0f)
    {
        physicsDeltaTime = 1.0f / _physicsRig->Fps;
    }
    else
    {
        physicsDeltaTime = deltaTimeSeconds;
    }

    while (_currentRemainTime >= physicsDeltaTime)
    {
        // copyRigOutputs _currentRigOutputs to _previousRigOutputs
        for (settingIndex = 0; settingIndex < _physicsRig->SubRigCount; ++settingIndex)
        {
            currentSetting = &_physicsRig->Settings[settingIndex];
            currentOutputs = currentSetting->OutputCount ? &_physicsRig->Outputs[currentSetting->BaseOutputIndex] : nullptr;
            for (i = 0; i < currentSetting->OutputCount; ++i)
            {
                if (!_activeOutputs[currentSetting->BaseOutputIndex + i]) continue;
                _previousRigOutputs[settingIndex].outputs[i] = _currentRigOutputs[settingIndex].outputs[i];
            }
        }

        // 入力キャッシュとパラメータで線形補間してUpdateParticlesするタイミングでの入力を計算する。
        // Calculate the input at the timing to UpdateParticles by linear interpolation with the _parameterInputCaches and parameterValues.
        // _parameterCachesはグループ間での値の伝搬の役割があるので_parameterInputCachesとの分離が必要。
        // _parameterCaches needs to be separated from _parameterInputCaches because of its role in propagating values between groups.
        float inputWeight =  physicsDeltaTime / _currentRemainTime;
        for (csmInt32 j = 0; j < model->GetParameterCount(); ++j)
        {
            _parameterCaches[j] = _parameterInputCaches[j] * (1.0f - inputWeight) + parameterValues[j] * inputWeight;
            _parameterInputCaches[j] = _parameterCaches[j];
        }

        for (settingIndex = 0; settingIndex < _physicsRig->SubRigCount; ++settingIndex)
        {
            totalAngle = 0.0f;
            totalTranslation.X = 0.0f;
            totalTranslation.Y = 0.0f;
            currentSetting = &_physicsRig->Settings[settingIndex];
            currentInputs = currentSetting->InputCount ? &_physicsRig->Inputs[currentSetting->BaseInputIndex] : nullptr;
            currentOutputs = currentSetting->OutputCount ? &_physicsRig->Outputs[currentSetting->BaseOutputIndex] : nullptr;
            currentParticles = &_physicsRig->Particles[currentSetting->BaseParticleIndex];

            // Load input parameters.
            for (i = 0; i < currentSetting->InputCount; ++i)
            {
                if (!_activeInputs[currentSetting->BaseInputIndex + i]) continue;
                weight = currentInputs[i].Weight / MaximumWeight;

                if (currentInputs[i].SourceParameterIndex == -1)
                {
                    currentInputs[i].SourceParameterIndex = model->FindRealParameterIndex(currentInputs[i].Source.Id);
                }

                if (observing && settingIndex == _observationGroup)
                {
                    observation.inputs[i].value = _parameterCaches[currentInputs[i].SourceParameterIndex];
                    observation.inputs[i].sampled = true;
                }

                currentInputs[i].GetNormalizedParameterValue(
                    &totalTranslation,
                    &totalAngle,
                    _parameterCaches[currentInputs[i].SourceParameterIndex],
                    parameterMinimumValues[currentInputs[i].SourceParameterIndex],
                    parameterMaximumValues[currentInputs[i].SourceParameterIndex],
                    parameterDefaultValues[currentInputs[i].SourceParameterIndex],
                    &currentSetting->NormalizationPosition,
                    &currentSetting->NormalizationAngle,
                    currentInputs[i].Reflect,
                    weight
                );
            }

            radAngle = CubismMath::DegreesToRadian(-totalAngle);

            totalTranslation.X = (totalTranslation.X * CubismMath::CosF(radAngle) - totalTranslation.Y * CubismMath::SinF(radAngle));
            totalTranslation.Y = (totalTranslation.X * CubismMath::SinF(radAngle) + totalTranslation.Y * CubismMath::CosF(radAngle));

            // Calculate particles position.
            UpdateParticles(
                currentParticles,
                currentSetting->ParticleCount,
                totalTranslation,
                totalAngle,
                _options.Wind,
                MovementThreshold * currentSetting->NormalizationPosition.Maximum,
                physicsDeltaTime,
                AirResistance
            );

            // Update output parameters.
            for (i = 0; i < currentSetting->OutputCount; ++i)
            {
                if (!_activeOutputs[currentSetting->BaseOutputIndex + i]) continue;
                particleIndex = currentOutputs[i].VertexIndex;

                if (currentOutputs[i].DestinationParameterIndex == -1)
                {
                    currentOutputs[i].DestinationParameterIndex = model->FindRealParameterIndex(currentOutputs[i].Destination.Id);
                }

                if (particleIndex < 1 || particleIndex >= currentSetting->ParticleCount)
                {
                    continue;
                }

                CubismVector2 translation;
                translation.X = currentParticles[particleIndex].Position.X - currentParticles[particleIndex - 1].Position.X;
                translation.Y = currentParticles[particleIndex].Position.Y - currentParticles[particleIndex - 1].Position.Y;

                outputValue = currentOutputs[i].GetValue(
                    translation,
                    currentParticles,
                    particleIndex,
                    currentOutputs[i].Reflect,
                    _options.Gravity
                );

                _currentRigOutputs[settingIndex].outputs[i] = outputValue;

                UpdateOutputParameterValue(
                        &_parameterCaches[currentOutputs[i].DestinationParameterIndex],
                        parameterMinimumValues[currentOutputs[i].DestinationParameterIndex],
                        parameterMaximumValues[currentOutputs[i].DestinationParameterIndex],
                        outputValue,
                        &currentOutputs[i]);
            }
        }

        _currentRemainTime -= physicsDeltaTime;
    }

    const float alpha = _currentRemainTime / physicsDeltaTime;
    Interpolate(model, alpha, observing ? &observation : nullptr);
    if (observing) FinishObservation(*model, observation);
}

void CubismPhysics::Interpolate(CubismModel* model, csmFloat32 weight, PhysicsObservation* observation)
{
    if (!ResolveParameterIndices(model)) return;
    csmInt32 i, settingIndex;
    CubismPhysicsOutput* currentOutputs;
    CubismPhysicsSubRig* currentSetting;
    csmFloat32* parameterValues;
    const csmFloat32* parameterMaximumValues;
    const csmFloat32* parameterMinimumValues;

    parameterValues = Core::csmGetParameterValues(model->GetModel());
    parameterMaximumValues = Core::csmGetParameterMaximumValues(model->GetModel());
    parameterMinimumValues = Core::csmGetParameterMinimumValues(model->GetModel());

    for (settingIndex = 0; settingIndex < _physicsRig->SubRigCount; ++settingIndex)
    {
        currentSetting = &_physicsRig->Settings[settingIndex];
        currentOutputs = currentSetting->OutputCount ? &_physicsRig->Outputs[currentSetting->BaseOutputIndex] : nullptr;

        // Load input parameters.
        for (i = 0; i < currentSetting->OutputCount; ++i)
        {
            if (!_activeOutputs[currentSetting->BaseOutputIndex + i]) continue;
            if (currentOutputs[i].DestinationParameterIndex == -1)
            {
                continue;
            }

            UpdateOutputParameterValue(
                &parameterValues[currentOutputs[i].DestinationParameterIndex],
                parameterMinimumValues[currentOutputs[i].DestinationParameterIndex],
                parameterMaximumValues[currentOutputs[i].DestinationParameterIndex],
                _previousRigOutputs[settingIndex].outputs[i] * (1 - weight) + _currentRigOutputs[settingIndex].outputs[i] * weight,
                &currentOutputs[i], observation && settingIndex == _observationGroup ? &observation->outputs[i] : nullptr
            );
        }
    }
}

void CubismPhysics::SetOptions(const Options& options)
{
    if (!std::isfinite(options.Gravity.X) || !std::isfinite(options.Gravity.Y) ||
        !std::isfinite(options.Wind.X) || !std::isfinite(options.Wind.Y)) return;
    InvalidateObservation();
    ++_stateRevision;
    _options = options;
}

const CubismPhysics::Options& CubismPhysics::GetOptions() const
{
    return _options;
}

}}}
