/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "CubismModel.hpp"
#include "framework/Rendering/CubismRenderer.hpp"
#include "framework/Id/CubismId.hpp"
#include "framework/Id/CubismIdManager.hpp"
#include "live2dstruct/cdraworder.h"
#include <QSet>
#include <QByteArray>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace Live2D { namespace Cubism { namespace Framework {

namespace {
constexpr int MaximumRuntimeCount = 1000000;
constexpr int MaximumRuntimeIndices = 3000000;

bool runtimeStateError(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool validRuntimeCount(int count, int maximum = MaximumRuntimeCount)
{
    return count >= 0 && count <= maximum;
}

template<typename T>
bool equalRuntimeValue(T left, T right) { return left == right; }

bool equalRuntimeValue(float left, float right)
{
    return std::isfinite(left) && std::isfinite(right) && left == right;
}

bool equalRuntimeValue(const char* left, const char* right)
{
    return left && right && left[0] && right[0] && std::strcmp(left, right) == 0;
}

bool equalRuntimeValue(Core::csmVector2 left, Core::csmVector2 right)
{
    return equalRuntimeValue(left.X, right.X) && equalRuntimeValue(left.Y, right.Y);
}

template<typename T>
bool equalRuntimeArray(const T* left, const T* right, int count)
{
    if (count && (!left || !right)) return false;
    for (int i = 0; i < count; ++i)
        if (!equalRuntimeValue(left[i], right[i])) return false;
    return true;
}

bool finiteRuntimeColor(Core::csmVector4 color)
{
    return std::isfinite(color.X) && std::isfinite(color.Y) &&
           std::isfinite(color.Z) && std::isfinite(color.W);
}

bool finiteRuntimeColor(const Rendering::CubismRenderer::CubismTextureColor& color)
{
    return std::isfinite(color.R) && std::isfinite(color.G) &&
           std::isfinite(color.B) && std::isfinite(color.A);
}

float boundedRuntimeValue(float value, float minimum, float maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

bool uniqueRuntimeIds(const char* const* ids, int count)
{
    if (count && !ids) return false;
    QSet<QByteArray> seen;
    for (int index = 0; index < count; ++index) {
        if (!ids[index] || !ids[index][0]) return false;
        const QByteArray id(ids[index]);
        if (seen.contains(id)) return false;
        seen.insert(id);
    }
    return true;
}

struct DrawableIdentityMap
{
    QVector<int> candidateToSource;
    int sourceCount = 0;
    int candidateCount = 0;
    bool appended = false;
};

bool buildDrawableIdentityMap(const CubismModel& candidate, const CubismModel& source,
                              bool allowAppend, DrawableIdentityMap& result, QString* error)
{
    const int candidateCount = candidate.GetDrawableCount();
    const int sourceCount = source.GetDrawableCount();
    if (!validRuntimeCount(candidateCount) || !validRuntimeCount(sourceCount) ||
        (!allowAppend && candidateCount != sourceCount))
        return runtimeStateError(error, QStringLiteral("Drawable count differs or is invalid"));
    const auto* candidateIds = Core::csmGetDrawableIds(candidate.GetModel());
    const auto* sourceIds = Core::csmGetDrawableIds(source.GetModel());
    if (!uniqueRuntimeIds(candidateIds, candidateCount) || !uniqueRuntimeIds(sourceIds, sourceCount))
        return runtimeStateError(error, QStringLiteral("Drawable IDs are empty or duplicated"));
    QHash<QByteArray, int> sourceIndices;
    for (int index = 0; index < sourceCount; ++index) sourceIndices.insert(QByteArray(sourceIds[index]), index);
    result.candidateToSource.fill(-1, candidateCount);
    for (int index = 0; index < candidateCount; ++index) {
        const auto found = sourceIndices.constFind(QByteArray(candidateIds[index]));
        if (found != sourceIndices.cend()) result.candidateToSource[index] = *found;
    }
    if (allowAppend) {
        const int sharedCount = std::min(candidateCount, sourceCount);
        for (int index = 0; index < sharedCount; ++index)
            if (result.candidateToSource[index] != index)
                return runtimeStateError(error, QStringLiteral("Existing drawable order or identity changed"));
    } else {
        for (int index = 0; index < candidateCount; ++index)
            if (result.candidateToSource[index] != index)
                return runtimeStateError(error, QStringLiteral("Ordered drawable IDs differ"));
    }
    result.sourceCount = sourceCount;
    result.candidateCount = candidateCount;
    result.appended = candidateCount > sourceCount;
    return true;
}

bool readValueDefinition(const Core::csmModel* model, CubismModel::RuntimeValueSnapshot& state, QString* error)
{
    if (!model) return runtimeStateError(error, QStringLiteral("Missing Core model"));
    const int parameters = Core::csmGetParameterCount(model), parts = Core::csmGetPartCount(model);
    const auto* parameterIds = Core::csmGetParameterIds(model);
    const auto* partIds = Core::csmGetPartIds(model);
    const auto* minimums = Core::csmGetParameterMinimumValues(model);
    const auto* maximums = Core::csmGetParameterMaximumValues(model);
    const auto* types = Core::csmGetParameterTypes(model);
    if (!validRuntimeCount(parameters) || !validRuntimeCount(parts) ||
        (parameters && (!parameterIds || !minimums || !maximums || !types)) || (parts && !partIds))
        return runtimeStateError(error, QStringLiteral("Runtime value definition is invalid"));
    const auto copyIds = [](const char* const* ids, int count, QStringList& target) {
        QSet<QString> unique;
        target.reserve(count);
        for (int index = 0; index < count; ++index) {
            if (!ids[index] || !ids[index][0]) return false;
            const QString id = QString::fromUtf8(ids[index]);
            if (unique.contains(id)) return false;
            unique.insert(id);
            target.append(id);
        }
        return true;
    };
    if (!copyIds(parameterIds, parameters, state.parameterIds) || !copyIds(partIds, parts, state.partIds))
        return runtimeStateError(error, QStringLiteral("Runtime value IDs are empty or duplicated"));
    state.minimums.reserve(parameters);
    state.maximums.reserve(parameters);
    for (int index = 0; index < parameters; ++index) {
        if (!std::isfinite(minimums[index]) || !std::isfinite(maximums[index]) || minimums[index] > maximums[index])
            return runtimeStateError(error, QStringLiteral("Parameter range is invalid"));
        state.minimums.append(minimums[index]);
        state.maximums.append(maximums[index]);
        state.parameterTypes.append(static_cast<int>(types[index]));
    }
    return true;
}

bool validVirtualValues(const QMap<QString, int>& ids, const QMap<int, float>& values,
                        const QStringList& realIds)
{
    if (ids.size() != values.size() || ids.size() > std::numeric_limits<int>::max() - realIds.size()) return false;
    QSet<QString> real(realIds.cbegin(), realIds.cend());
    QSet<int> indices;
    for (auto id = ids.cbegin(); id != ids.cend(); ++id) {
        if (id.key().isEmpty() || real.contains(id.key()) || id.value() < realIds.size() ||
            id.value() >= realIds.size() + ids.size() || indices.contains(id.value()) ||
            !values.contains(id.value()) || !std::isfinite(values.value(id.value()))) return false;
        indices.insert(id.value());
    }
    return true;
}

bool validParameterValue(const ParameterRuntimeValue& value)
{
    return std::isfinite(value.current) && (!value.saved || std::isfinite(*value.saved)) &&
        std::isfinite(value.minimum) && std::isfinite(value.maximum) && value.minimum <= value.maximum;
}

bool validSavedState(const CubismModel::RuntimeValueSnapshot& state)
{
    if (state.savedParameters.size() > state.parameterIds.size() ||
        (!state.savedPresence.isEmpty() && state.savedPresence.size() != state.savedParameters.size())) return false;
    for (auto present : state.savedPresence) if (present > 1) return false;
    for (float value : state.savedParameters) if (!std::isfinite(value)) return false;
    const QSet<QString> realIds(state.parameterIds.cbegin(), state.parameterIds.cend());
    if (state.retiredParameters.size() > MaximumRuntimeCount) return false;
    for (auto it = state.retiredParameters.cbegin(); it != state.retiredParameters.cend(); ++it)
        if (it.key().isEmpty() || realIds.contains(it.key()) || !validParameterValue(it.value())) return false;
    return true;
}
}

QMap<QString, float> CubismModel::RuntimeValueSnapshot::Parameters() const
{
    QMap<QString, float> result;
    if (!valid || parameterIds.size() != parameterValues.size()) return result;
    for (int index = 0; index < parameterIds.size(); ++index) result.insert(parameterIds[index], parameterValues[index]);
    return result;
}

bool CubismModel::RuntimeValueSnapshot::HasSavedParameter(int index) const
{
    return index >= 0 && index < savedParameters.size() &&
        (savedPresence.isEmpty() || (savedPresence.size() == savedParameters.size() && savedPresence[index] == 1));
}

bool CubismModel::RuntimeValueSnapshot::SetSavedParameter(int index, float value)
{
    if (index < 0 || index >= parameterIds.size() || !std::isfinite(value) || savedParameters.size() > parameterIds.size() ||
        (!savedPresence.isEmpty() && savedPresence.size() != savedParameters.size())) return false;
    if (index >= savedParameters.size()) {
        if (savedPresence.isEmpty()) savedPresence.fill(1, savedParameters.size());
        savedParameters.resize(index + 1);
        savedPresence.resize(index + 1);
    }
    savedParameters[index] = value;
    if (!savedPresence.isEmpty()) savedPresence[index] = 1;
    return true;
}

ParameterRuntimeValue CubismModel::RuntimeValueSnapshot::Parameter(int index) const
{
    ParameterRuntimeValue value;
    value.current = parameterValues.at(index);
    if (HasSavedParameter(index)) value.saved = savedParameters[index];
    value.type = parameterTypes.at(index);
    value.minimum = minimums.at(index);
    value.maximum = maximums.at(index);
    return value;
}

bool CubismModel::CaptureRuntimeValues(RuntimeValueSnapshot& snapshot, QString* error) const
{
    if (error) error->clear();
    try {
        RuntimeValueSnapshot candidate;
        if (!readValueDefinition(_model, candidate, error)) return false;
        const int parameters = candidate.parameterIds.size(), parts = candidate.partIds.size();
        if ((parameters && !_parameterValues) || (parts && !_partOpacities) || _savedParameters.size() > parameters ||
            _parameterIds.GetSize() != parameters || _partIds.GetSize() != parts || !std::isfinite(_modelOpacity))
            return runtimeStateError(error, QStringLiteral("Runtime values are not initialized"));
        candidate.parameterValues.reserve(parameters);
        candidate.partOpacities.reserve(parts);
        for (int index = 0; index < parameters; ++index) {
            if (!std::isfinite(_parameterValues[index])) return runtimeStateError(error, QStringLiteral("Current parameter is non-finite"));
            candidate.parameterValues.append(_parameterValues[index]);
        }
        for (int index = 0; index < parts; ++index) {
            if (!std::isfinite(_partOpacities[index])) return runtimeStateError(error, QStringLiteral("Part opacity is non-finite"));
            candidate.partOpacities.append(_partOpacities[index]);
        }
        for (float value : _savedParameters)
            if (!std::isfinite(value)) return runtimeStateError(error, QStringLiteral("Saved parameter is non-finite"));
        const auto copyVirtualIds = [](const auto& source, QMap<QString, int>& target) {
            for (auto it = source.cbegin(); it != source.cend(); ++it) {
                if (!it.key()) return false;
                const QString name = QString::fromUtf8(it.key()->GetString().GetRawString());
                if (target.contains(name)) return false;
                target.insert(name, it.value());
            }
            return true;
        };
        if (!copyVirtualIds(_notExistParameterId, candidate.virtualParameterIds) ||
            !copyVirtualIds(_notExistPartId, candidate.virtualPartIds))
            return runtimeStateError(error, QStringLiteral("Virtual value IDs are invalid"));
        candidate.virtualParameterValues = _notExistParameterValues;
        candidate.virtualPartOpacities = _notExistPartOpacities;
        if (!validVirtualValues(candidate.virtualParameterIds, candidate.virtualParameterValues, candidate.parameterIds) ||
            !validVirtualValues(candidate.virtualPartIds, candidate.virtualPartOpacities, candidate.partIds))
            return runtimeStateError(error, QStringLiteral("Virtual runtime values are invalid"));
        candidate.savedParameters = _savedParameters;
        candidate.savedPresence = _savedPresence;
        candidate.retiredParameters = _retiredParameters;
        if (!validSavedState(candidate)) return runtimeStateError(error, QStringLiteral("Saved parameter state is invalid"));
        candidate.modelOpacity = _modelOpacity;
        candidate.valid = true;
        snapshot = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        return runtimeStateError(error, QStringLiteral("Unable to capture runtime values"));
    }
}

bool CubismModel::RestoreRuntimeValues(const RuntimeValueSnapshot& snapshot, QString* error)
{
    if (error) error->clear();
    try {
        RuntimeValueSnapshot definition;
        if (!readValueDefinition(_model, definition, error)) return false;
        const int parameters = definition.parameterIds.size(), parts = definition.partIds.size();
        if (!snapshot.valid || !validSavedState(snapshot) || snapshot.parameterIds != definition.parameterIds || snapshot.partIds != definition.partIds ||
            snapshot.parameterTypes != definition.parameterTypes ||
            snapshot.minimums != definition.minimums || snapshot.maximums != definition.maximums ||
            snapshot.parameterValues.size() != parameters || snapshot.partOpacities.size() != parts ||
            snapshot.savedParameters.size() > parameters || (parameters && !_parameterValues) ||
            (parts && !_partOpacities) || !std::isfinite(snapshot.modelOpacity))
            return runtimeStateError(error, QStringLiteral("Runtime snapshot does not match this model"));
        for (const auto* values : {&snapshot.parameterValues, &snapshot.savedParameters, &snapshot.partOpacities})
            for (float value : *values)
                if (!std::isfinite(value)) return runtimeStateError(error, QStringLiteral("Runtime snapshot contains non-finite values"));
        if (!validVirtualValues(snapshot.virtualParameterIds, snapshot.virtualParameterValues, snapshot.parameterIds) ||
            !validVirtualValues(snapshot.virtualPartIds, snapshot.virtualPartOpacities, snapshot.partIds))
            return runtimeStateError(error, QStringLiteral("Runtime snapshot virtual values are invalid"));
        QMap<CubismIdHandle, int> parameterIds, partIds;
        auto* manager = CubismFramework::GetIdManager();
        if (!manager && (!snapshot.virtualParameterIds.isEmpty() || !snapshot.virtualPartIds.isEmpty()))
            return runtimeStateError(error, QStringLiteral("Missing runtime ID manager"));
        const auto resolve = [manager](const auto& source, auto& target) {
            for (auto it = source.cbegin(); it != source.cend(); ++it) {
                const auto* id = manager->GetId(it.key().toUtf8().constData());
                if (!id || target.contains(id)) return false;
                target.insert(id, it.value());
            }
            return true;
        };
        if (!resolve(snapshot.virtualParameterIds, parameterIds) || !resolve(snapshot.virtualPartIds, partIds))
            return runtimeStateError(error, QStringLiteral("Unable to resolve snapshot IDs"));
        auto parameterValues = snapshot.virtualParameterValues;
        auto partOpacities = snapshot.virtualPartOpacities;
        auto savedParameters = snapshot.savedParameters;
        auto savedPresence = snapshot.savedPresence;
        auto retiredParameters = snapshot.retiredParameters;
        _notExistParameterId.swap(parameterIds);
        _notExistPartId.swap(partIds);
        _notExistParameterValues.swap(parameterValues);
        _notExistPartOpacities.swap(partOpacities);
        _savedParameters.swap(savedParameters);
        _savedPresence.swap(savedPresence);
        _retiredParameters.swap(retiredParameters);
        for (int index = 0; index < parameters; ++index) _parameterValues[index] = snapshot.parameterValues[index];
        for (int index = 0; index < parts; ++index) _partOpacities[index] = snapshot.partOpacities[index];
        _modelOpacity = snapshot.modelOpacity;
        return true;
    } catch (const std::exception&) {
        return runtimeStateError(error, QStringLiteral("Unable to restore runtime values"));
    }
}

bool CubismModel::RebaseRuntimeValues(const RuntimeValueSnapshot& snapshot, RuntimeValueSnapshot& result,
                                     QString* error) const
{
    if (error) error->clear();
    try {
        RuntimeValueSnapshot definition;
        if (!readValueDefinition(_model, definition, error)) return false;
        const int count = definition.parameterIds.size();
        if (!snapshot.valid || !validSavedState(snapshot) || snapshot.parameterIds != definition.parameterIds ||
            snapshot.parameterTypes != definition.parameterTypes || snapshot.partIds != definition.partIds ||
            snapshot.parameterValues.size() != count || snapshot.minimums.size() != count ||
            snapshot.maximums.size() != count || snapshot.savedParameters.size() > count ||
            snapshot.partOpacities.size() != definition.partIds.size() || !std::isfinite(snapshot.modelOpacity) ||
            !validVirtualValues(snapshot.virtualParameterIds, snapshot.virtualParameterValues, snapshot.parameterIds) ||
            !validVirtualValues(snapshot.virtualPartIds, snapshot.virtualPartOpacities, snapshot.partIds))
            return runtimeStateError(error, QStringLiteral("Runtime snapshot cannot be migrated to this definition"));
        for (const auto* values : {&snapshot.parameterValues, &snapshot.savedParameters, &snapshot.partOpacities})
            for (float value : *values)
                if (!std::isfinite(value)) return runtimeStateError(error, QStringLiteral("Runtime snapshot contains non-finite values"));
        RuntimeValueSnapshot candidate = snapshot;
        for (int index = 0; index < count; ++index) {
            if (!std::isfinite(snapshot.minimums[index]) || !std::isfinite(snapshot.maximums[index]) ||
                snapshot.minimums[index] > snapshot.maximums[index])
                return runtimeStateError(error, QStringLiteral("Runtime snapshot range is invalid"));
            if (snapshot.minimums[index] == definition.minimums[index] &&
                snapshot.maximums[index] == definition.maximums[index]) continue;
            candidate.parameterValues[index] = boundedRuntimeValue(candidate.parameterValues[index],
                definition.minimums[index], definition.maximums[index]);
            if (candidate.HasSavedParameter(index))
                candidate.savedParameters[index] = boundedRuntimeValue(candidate.savedParameters[index],
                    definition.minimums[index], definition.maximums[index]);
        }
        candidate.minimums = std::move(definition.minimums);
        candidate.maximums = std::move(definition.maximums);
        result = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        return runtimeStateError(error, QStringLiteral("Unable to migrate runtime snapshot"));
    }
}

bool CubismModel::MigrateRuntimeValues(const RuntimeValueSnapshot& source, RuntimeValueSnapshot& result,
                                      const ParameterStructureSeeds& seeds, QString* error) const
{
    if (error) error->clear();
    try {
        RuntimeValueSnapshot candidate;
        if (!readValueDefinition(_model, candidate, error)) return false;
        const int oldCount = source.parameterIds.size(), count = candidate.parameterIds.size();
        if (!source.valid || !validRuntimeCount(oldCount) || !validSavedState(source) ||
            source.parameterValues.size() != oldCount || source.parameterTypes.size() != oldCount ||
            source.minimums.size() != oldCount || source.maximums.size() != oldCount ||
            source.partIds != candidate.partIds || source.partOpacities.size() != candidate.partIds.size() ||
            !std::isfinite(source.modelOpacity) ||
            !validVirtualValues(source.virtualParameterIds, source.virtualParameterValues, source.parameterIds) ||
            !validVirtualValues(source.virtualPartIds, source.virtualPartOpacities, source.partIds))
            return runtimeStateError(error, QStringLiteral("Runtime snapshot cannot migrate parameter structure"));
        QHash<QString, int> oldIndices, newIndices;
        for (int index = 0; index < oldCount; ++index) {
            const QString& id = source.parameterIds[index];
            if (id.isEmpty() || oldIndices.contains(id) || !validParameterValue(source.Parameter(index)))
                return runtimeStateError(error, QStringLiteral("Source parameter state is invalid"));
            oldIndices.insert(id, index);
        }
        for (int index = 0; index < count; ++index) newIndices.insert(candidate.parameterIds[index], index);
        for (auto it = seeds.cbegin(); it != seeds.cend(); ++it) {
            if (!newIndices.contains(it.key()) || oldIndices.contains(it.key()) ||
                (it->mode != ParameterStructureSeed::Mode::Defaults && it->mode != ParameterStructureSeed::Mode::Values))
                return runtimeStateError(error, QStringLiteral("Parameter seed must refer to an added real parameter: %1").arg(it.key()));
        }
        QMap<QString, float> virtualValues;
        for (auto it = source.virtualParameterIds.cbegin(); it != source.virtualParameterIds.cend(); ++it)
            if (!newIndices.contains(it.key())) virtualValues.insert(it.key(), source.virtualParameterValues[it.value()]);
        candidate.retiredParameters = source.retiredParameters;
        for (int index = 0; index < oldCount; ++index) {
            const QString& id = source.parameterIds[index];
            if (newIndices.contains(id)) continue;
            const auto value = source.Parameter(index);
            candidate.retiredParameters.insert(id, value);
            virtualValues.insert(id, value.current);
        }
        const auto* defaults = Core::csmGetParameterDefaultValues(_model);
        if (count && !defaults) return runtimeStateError(error, QStringLiteral("Missing parameter defaults"));
        candidate.parameterValues.reserve(count);
        for (int index = 0; index < count; ++index) {
            const QString& id = candidate.parameterIds[index];
            const auto old = oldIndices.constFind(id);
            const auto seed = seeds.constFind(id);
            ParameterRuntimeValue value;
            value.current = defaults[index];
            value.saved = defaults[index];
            value.type = candidate.parameterTypes[index];
            value.minimum = candidate.minimums[index];
            value.maximum = candidate.maximums[index];
            if (!std::isfinite(defaults[index]) || defaults[index] < value.minimum || defaults[index] > value.maximum)
                return runtimeStateError(error, QStringLiteral("Invalid candidate parameter default"));
            if (old != oldIndices.cend()) value = source.Parameter(*old);
            else if (seed != seeds.cend()) {
                if (seed->mode == ParameterStructureSeed::Mode::Values) value = seed->runtime;
            } else {
                const auto retired = source.retiredParameters.constFind(id);
                if (retired != source.retiredParameters.cend()) value = *retired;
            }
            if (!validParameterValue(value) || value.type != candidate.parameterTypes[index])
                return runtimeStateError(error, QStringLiteral("Parameter seed has invalid values or type: %1").arg(id));
            if (value.minimum != candidate.minimums[index] || value.maximum != candidate.maximums[index]) {
                value.current = boundedRuntimeValue(value.current, candidate.minimums[index], candidate.maximums[index]);
                if (value.saved) value.saved = boundedRuntimeValue(*value.saved, candidate.minimums[index], candidate.maximums[index]);
            }
            candidate.parameterValues.append(value.current);
            if (value.saved) candidate.SetSavedParameter(index, *value.saved);
            candidate.retiredParameters.remove(id);
        }
        if (std::all_of(candidate.savedPresence.cbegin(), candidate.savedPresence.cend(), [](quint8 value) { return value == 1; }))
            candidate.savedPresence.clear();
        if (virtualValues.size() > MaximumRuntimeCount || virtualValues.size() > std::numeric_limits<int>::max() - count)
            return runtimeStateError(error, QStringLiteral("Too many virtual parameters"));
        int index = count;
        for (auto it = virtualValues.cbegin(); it != virtualValues.cend(); ++it, ++index) {
            candidate.virtualParameterIds.insert(it.key(), index);
            candidate.virtualParameterValues.insert(index, it.value());
        }
        candidate.partOpacities = source.partOpacities;
        for (float value : candidate.partOpacities)
            if (!std::isfinite(value)) return runtimeStateError(error, QStringLiteral("Part opacity is non-finite"));
        candidate.virtualPartIds = source.virtualPartIds;
        candidate.virtualPartOpacities = source.virtualPartOpacities;
        candidate.modelOpacity = source.modelOpacity;
        candidate.valid = true;
        if (!validSavedState(candidate)) return runtimeStateError(error, QStringLiteral("Migrated parameter archive is invalid"));
        result = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        return runtimeStateError(error, QStringLiteral("Unable to migrate parameter structure"));
    }
}

bool CubismModel::ApplyParameterPose(const QMap<QString, float>& pose, QString* error)
{
    if (error) error->clear();
    try {
        RuntimeValueSnapshot definition;
        if (!readValueDefinition(_model, definition, error)) return false;
        if (pose.size() != definition.parameterIds.size() || (!pose.isEmpty() && !_parameterValues))
            return runtimeStateError(error, QStringLiteral("Editor pose must contain every real parameter"));
        QVector<float> values;
        values.reserve(pose.size());
        for (int index = 0; index < definition.parameterIds.size(); ++index) {
            const auto found = pose.constFind(definition.parameterIds[index]);
            if (found == pose.cend() || !std::isfinite(*found) || *found < definition.minimums[index] || *found > definition.maximums[index])
                return runtimeStateError(error, QStringLiteral("Editor parameter %1 is missing or outside its range").arg(definition.parameterIds[index]));
            values.append(*found);
        }
        for (int index = 0; index < values.size(); ++index) _parameterValues[index] = values[index];
        return true;
    } catch (const std::exception&) {
        return runtimeStateError(error, QStringLiteral("Unable to apply editor parameter pose"));
    }
}

bool CubismModel::IsCompatibleForKeyformUpdate(const CubismModel& source, QString* error) const
{
    return IsCompatibleForKeyformUpdate(source, CubismModelUpdatePolicy::Strict, error);
}

bool CubismModel::IsCompatibleForKeyformUpdate(const CubismModel& source, CubismModelUpdatePolicy policy, QString* error) const
{
    if (error) error->clear();
    if (static_cast<unsigned int>(policy) & ~63u)
        return runtimeStateError(error, QStringLiteral("Invalid model update policy"));
    if (!_model || !source._model) return runtimeStateError(error, QStringLiteral("Missing Core model"));
    const bool structureEdits = AllowsModelUpdate(policy, CubismModelUpdatePolicy::AllowParameterStructureEdits);
    const bool parameterEdits = AllowsModelUpdate(policy, CubismModelUpdatePolicy::AllowParameterDefinitionEdits);
    const bool drawableTopology = AllowsModelUpdate(policy, CubismModelUpdatePolicy::AllowDrawableTopologyEdits);
    RuntimeValueSnapshot definition, sourceDefinition;
    if (!readValueDefinition(_model, definition, error) || !readValueDefinition(source._model, sourceDefinition, error)) return false;
    const int parameters = GetParameterCount(), sourceParameters = source.GetParameterCount();
    const int parts = GetPartCount(), drawables = GetDrawableCount();
    DrawableIdentityMap drawableMap;
    if (!validRuntimeCount(drawables) || (!structureEdits && parameters != sourceParameters) ||
        parts != source.GetPartCount())
        return runtimeStateError(error, QStringLiteral("Parameter, part or drawable count differs or is invalid"));
    if (!buildDrawableIdentityMap(*this, source, drawableTopology, drawableMap, error)) return false;
    const int sourceDrawables = drawableMap.sourceCount;
    const int sharedDrawables = std::min(drawables, sourceDrawables);
    if (_parameterIds.GetSize() != parameters || source._parameterIds.GetSize() != sourceParameters ||
        _partIds.GetSize() != parts || source._partIds.GetSize() != parts ||
        _drawableIds.GetSize() != drawables || source._drawableIds.GetSize() != sourceDrawables ||
        (parameters && !_parameterValues) || (sourceParameters && !source._parameterValues) ||
        (parts && (!_partOpacities || !source._partOpacities)))
        return runtimeStateError(error, QStringLiteral("Model runtime arrays are not initialized"));
    if ((!structureEdits && definition.parameterIds != sourceDefinition.parameterIds) ||
        definition.partIds != sourceDefinition.partIds)
        return runtimeStateError(error, QStringLiteral("Ordered parameter, part or drawable IDs differ"));

    Core::csmVector2 size{}, origin{}, sourceSize{}, sourceOrigin{};
    float ppu = 0, sourcePpu = 0;
    Core::csmReadCanvasInfo(_model, &size, &origin, &ppu);
    Core::csmReadCanvasInfo(source._model, &sourceSize, &sourceOrigin, &sourcePpu);
    if (!equalRuntimeValue(size, sourceSize) || !equalRuntimeValue(origin, sourceOrigin) ||
        !equalRuntimeValue(ppu, sourcePpu) || size.X <= 0 || size.Y <= 0 || ppu <= 0)
        return runtimeStateError(error, QStringLiteral("Canvas definition differs or is invalid"));

    const auto* defaults = Core::csmGetParameterDefaultValues(_model);
    const auto* sourceDefaults = Core::csmGetParameterDefaultValues(source._model);
    const auto* keyCounts = Core::csmGetParameterKeyCounts(_model);
    const auto* sourceKeyCounts = Core::csmGetParameterKeyCounts(source._model);
    const auto* keys = Core::csmGetParameterKeyValues(_model);
    const auto* sourceKeys = Core::csmGetParameterKeyValues(source._model);
    const auto validKeys = [&](const RuntimeValueSnapshot& state, const float* values,
                               const int* counts, const float* const* arrays) {
        if (!state.parameterIds.isEmpty() && (!values || !counts)) return false;
        for (int i = 0; i < state.parameterIds.size(); ++i) {
            if (!std::isfinite(values[i]) || values[i] < state.minimums[i] || values[i] > state.maximums[i] ||
                !validRuntimeCount(counts[i]) || (counts[i] && (!arrays || !arrays[i]))) return false;
            for (int key = 0; key < counts[i]; ++key) if (!std::isfinite(arrays[i][key])) return false;
        }
        return true;
    };
    if (!validKeys(definition, defaults, keyCounts, keys) ||
        !validKeys(sourceDefinition, sourceDefaults, sourceKeyCounts, sourceKeys))
        return runtimeStateError(error, QStringLiteral("Parameter keys or range are invalid"));
    QHash<QString, int> sourceParameterIndices;
    for (int i = 0; i < sourceParameters; ++i) sourceParameterIndices.insert(sourceDefinition.parameterIds[i], i);
    for (int i = 0; i < parameters; ++i) {
        const auto previous = sourceParameterIndices.constFind(definition.parameterIds[i]);
        if (previous == sourceParameterIndices.cend()) continue;
        const int old = *previous;
        if (definition.parameterTypes[i] != sourceDefinition.parameterTypes[old] ||
            (!parameterEdits && (definition.minimums[i] != sourceDefinition.minimums[old] ||
                definition.maximums[i] != sourceDefinition.maximums[old] || defaults[i] != sourceDefaults[old])) ||
            (!structureEdits && (keyCounts[i] != sourceKeyCounts[old] ||
                (keyCounts[i] && !equalRuntimeArray(keys[i], sourceKeys[old], keyCounts[i])))))
            return runtimeStateError(error, QStringLiteral("Parameter %1 definition differs").arg(definition.parameterIds[i]));
    }

    const auto* partParents = Core::csmGetPartParentPartIndices(_model);
    const auto* drawableParents = Core::csmGetDrawableParentPartIndices(_model);
    const auto* vertices = Core::csmGetDrawableVertexCounts(_model);
    const auto* sourceVertices = Core::csmGetDrawableVertexCounts(source._model);
    const auto* indexCounts = Core::csmGetDrawableIndexCounts(_model);
    const auto* sourceIndexCounts = Core::csmGetDrawableIndexCounts(source._model);
    const auto* maskCounts = Core::csmGetDrawableMaskCounts(_model);
    const auto* sourceMaskCounts = Core::csmGetDrawableMaskCounts(source._model);
    const auto* textures = Core::csmGetDrawableTextureIndices(_model);
    const auto* flags = Core::csmGetDrawableConstantFlags(_model);
    const auto* sourceFlags = Core::csmGetDrawableConstantFlags(source._model);
    const bool clippingEdits = AllowsModelUpdate(policy, CubismModelUpdatePolicy::AllowClippingEdits);
    const bool geometryEdits = AllowsModelUpdate(policy, CubismModelUpdatePolicy::AllowDrawableGeometryEdits);
    const bool materialEdits = AllowsModelUpdate(policy, CubismModelUpdatePolicy::AllowDrawableMaterialEdits);
    const bool topologyEdits = geometryEdits || AllowsModelUpdate(policy, CubismModelUpdatePolicy::AllowDrawableTopologyEdits);
    const auto* sourceTextures = Core::csmGetDrawableTextureIndices(source._model);
    const auto* sourceDrawableParents = Core::csmGetDrawableParentPartIndices(source._model);
    if (!equalRuntimeArray(partParents, Core::csmGetPartParentPartIndices(source._model), parts) ||
        (drawables && (!drawableParents || !vertices || !indexCounts || !maskCounts || !textures || !flags)) ||
        (sourceDrawables && (!sourceDrawableParents || !sourceVertices || !sourceIndexCounts || !sourceMaskCounts || !sourceTextures || !sourceFlags)) ||
        !equalRuntimeArray(drawableParents, sourceDrawableParents, sharedDrawables) ||
        (!geometryEdits && !equalRuntimeArray(vertices, sourceVertices, sharedDrawables)) ||
        (!topologyEdits && !equalRuntimeArray(indexCounts, sourceIndexCounts, sharedDrawables)) ||
        (!clippingEdits && !equalRuntimeArray(maskCounts, sourceMaskCounts, sharedDrawables)) ||
        (!materialEdits && !equalRuntimeArray(textures, sourceTextures, sharedDrawables)))
        return runtimeStateError(error, QStringLiteral("Part hierarchy or drawable definition differs or is invalid"));
    if (!HasValidClippingDefinitions(error) || !source.HasValidClippingDefinitions(error)) return false;
    for (int i = 0; i < parts; ++i)
        if (partParents[i] < -1 || partParents[i] >= parts || partParents[i] == i)
            return runtimeStateError(error, QStringLiteral("Part %1 parent is invalid").arg(i));
    const auto* uvs = Core::csmGetDrawableVertexUvs(_model);
    const auto* sourceUvs = Core::csmGetDrawableVertexUvs(source._model);
    const auto* indices = Core::csmGetDrawableIndices(_model);
    const auto* sourceIndices = Core::csmGetDrawableIndices(source._model);
    const auto* masks = Core::csmGetDrawableMasks(_model);
    const auto* sourceMasks = Core::csmGetDrawableMasks(source._model);
    const auto validIndices = [](const csmUint16* values, int count, int vertexCount) {
        if (!validRuntimeCount(count, MaximumRuntimeIndices) || count % 3 || (count && !values)) return false;
        for (int index = 0; index < count; ++index)
            if (values[index] >= vertexCount) return false;
        return true;
    };
    for (int i = 0; i < drawables; ++i) {
        const int sourceIndex = drawableMap.candidateToSource.value(i, -1);
        const unsigned int allowedFlags = clippingEdits ? Core::csmIsInvertedMask : 0;
        if (sourceIndex >= 0 && (flags[i] ^ sourceFlags[sourceIndex]) & ~allowedFlags)
            return runtimeStateError(error, QStringLiteral("Drawable %1 constant flags differ").arg(i));
        if (!validRuntimeCount(vertices[i]) || (sourceIndex >= 0 && !validRuntimeCount(sourceVertices[sourceIndex])) ||
            !validRuntimeCount(maskCounts[i], clippingEdits ? MaximumRuntimeIndices : drawables) || textures[i] < 0 ||
            (sourceIndex >= 0 && sourceTextures[sourceIndex] < 0) ||
            drawableParents[i] < -1 || drawableParents[i] >= parts ||
            (vertices[i] && (!uvs || !uvs[i] || !equalRuntimeArray(uvs[i], uvs[i], vertices[i]))) ||
            (sourceIndex >= 0 && sourceVertices[sourceIndex] &&
             (!sourceUvs || !sourceUvs[sourceIndex] || !equalRuntimeArray(sourceUvs[sourceIndex], sourceUvs[sourceIndex], sourceVertices[sourceIndex]))) ||
            (sourceIndex >= 0 && !geometryEdits && !materialEdits && vertices[i] &&
             !equalRuntimeArray(uvs[i], sourceUvs[sourceIndex], vertices[i])) ||
            !validIndices(indices ? indices[i] : nullptr, indexCounts[i], vertices[i]) ||
            (sourceIndex >= 0 && !validIndices(sourceIndices ? sourceIndices[sourceIndex] : nullptr,
                                                sourceIndexCounts[sourceIndex], sourceVertices[sourceIndex])) ||
            (sourceIndex >= 0 && !topologyEdits && indexCounts[i] &&
             !equalRuntimeArray(indices[i], sourceIndices[sourceIndex], indexCounts[i])) ||
            (sourceIndex >= 0 && !clippingEdits && maskCounts[i] &&
             (!masks || !sourceMasks || !equalRuntimeArray(masks[i], sourceMasks[sourceIndex], maskCounts[i]))))
            return runtimeStateError(error, QStringLiteral("Drawable %1 UVs, topology or masks differ or are invalid").arg(i));
    }
    return true;
}

bool CubismModel::HasValidClippingDefinitions(QString* error) const
{
    if (error) error->clear();
    if (!_model) return runtimeStateError(error, QStringLiteral("Missing Core model"));
    const int count = GetDrawableCount();
    if (!validRuntimeCount(count)) return runtimeStateError(error, QStringLiteral("Invalid drawable count"));
    const auto* counts = Core::csmGetDrawableMaskCounts(_model);
    const auto* masks = Core::csmGetDrawableMasks(_model);
    if (count && !counts) return runtimeStateError(error, QStringLiteral("Missing drawable mask counts"));
    try {
        for (int drawable = 0; drawable < count; ++drawable) {
            if (!validRuntimeCount(counts[drawable], MaximumRuntimeIndices) || (counts[drawable] && (!masks || !masks[drawable])))
                return runtimeStateError(error, QStringLiteral("Drawable %1 mask array is invalid").arg(drawable));
            for (int index = 0; index < counts[drawable]; ++index) {
                const int mask = masks[drawable][index];
                if (mask < 0 || mask >= count)
                    return runtimeStateError(error, QStringLiteral("Drawable %1 mask index is outside the model").arg(drawable));
            }
        }
        return true;
    } catch (const std::exception&) {
        return runtimeStateError(error, QStringLiteral("Unable to validate drawable masks"));
    }
}

bool CubismModel::HasFiniteDrawableState(QString* error) const
{
    if (error) error->clear();
    if (!_model) return runtimeStateError(error, QStringLiteral("Missing Core model"));
    const int count = GetDrawableCount();
    if (!validRuntimeCount(count)) return runtimeStateError(error, QStringLiteral("Invalid drawable count"));
    const auto* counts = Core::csmGetDrawableVertexCounts(_model);
    const auto* positions = Core::csmGetDrawableVertexPositions(_model);
    const auto* uvs = Core::csmGetDrawableVertexUvs(_model);
    const auto* opacities = Core::csmGetDrawableOpacities(_model);
    const auto* multiply = Core::csmGetDrawableMultiplyColors(_model);
    const auto* screen = Core::csmGetDrawableScreenColors(_model);
    if (count && (!counts || !opacities || !multiply || !screen))
        return runtimeStateError(error, QStringLiteral("Missing drawable state arrays"));
    for (int i = 0; i < count; ++i) {
        if (!validRuntimeCount(counts[i]) || !std::isfinite(opacities[i]) ||
            !finiteRuntimeColor(multiply[i]) || !finiteRuntimeColor(screen[i]) ||
            (counts[i] && (!positions || !uvs || !positions[i] || !uvs[i])))
            return runtimeStateError(error, QStringLiteral("Drawable %1 state is invalid").arg(i));
        for (int j = 0; j < counts[i]; ++j)
            if (!std::isfinite(positions[i][j].X) || !std::isfinite(positions[i][j].Y) ||
                !std::isfinite(uvs[i][j].X) || !std::isfinite(uvs[i][j].Y))
                return runtimeStateError(error, QStringLiteral("Drawable %1 vertex %2 is non-finite").arg(i).arg(j));
    }
    return true;
}

bool CubismModel::CopyRuntimeStateFrom(const CubismModel& source, QString* error)
{
    return CopyRuntimeStateFrom(source, CubismModelUpdatePolicy::Strict, error);
}

bool CubismModel::CopyRuntimeStateFrom(const CubismModel& source, CubismModelUpdatePolicy policy, QString* error)
{
    return CopyRuntimeStateFrom(source, policy, {}, error);
}

bool CubismModel::CopyRuntimeStateFrom(const CubismModel& source, CubismModelUpdatePolicy policy,
                                      const ParameterStructureSeeds& seeds, QString* error)
{
    if (!IsCompatibleForKeyformUpdate(source, policy, error)) return false;
    const int parts = GetPartCount(), drawables = GetDrawableCount();
    DrawableIdentityMap drawableMap;
    if (!buildDrawableIdentityMap(*this, source,
            AllowsModelUpdate(policy, CubismModelUpdatePolicy::AllowDrawableTopologyEdits), drawableMap, error)) return false;
    const int sourceDrawables = drawableMap.sourceCount;
    const auto arraysMatch = [](const CubismModel& model, int expectedDrawables, int expectedParts) {
        return model._userMultiplyColors.GetSize() == expectedDrawables && model._userScreenColors.GetSize() == expectedDrawables &&
               model._userCullings.GetSize() == expectedDrawables && model._userPartMultiplyColors.GetSize() == expectedParts &&
               model._userPartScreenColors.GetSize() == expectedParts;
    };
    if (!arraysMatch(*this, drawables, parts) || !arraysMatch(source, sourceDrawables, parts) || source._savedParameters.size() > source.GetParameterCount() ||
        !std::isfinite(source._modelOpacity))
        return runtimeStateError(error, QStringLiteral("Runtime override arrays or model opacity are invalid"));
    for (int i = 0; i < source.GetParameterCount(); ++i)
        if (!std::isfinite(source._parameterValues[i]))
            return runtimeStateError(error, QStringLiteral("Parameter %1 current value is non-finite").arg(i));
    for (float value : source._savedParameters)
        if (!std::isfinite(value)) return runtimeStateError(error, QStringLiteral("Saved parameter value is non-finite"));
    for (int i = 0; i < parts; ++i)
        if (!std::isfinite(source._partOpacities[i]) || !finiteRuntimeColor(source._userPartMultiplyColors[i].Color) ||
            !finiteRuntimeColor(source._userPartScreenColors[i].Color))
            return runtimeStateError(error, QStringLiteral("Part %1 user state is non-finite").arg(i));
    for (int i = 0; i < sourceDrawables; ++i) {
        if (!finiteRuntimeColor(source._userMultiplyColors[i].Color) || !finiteRuntimeColor(source._userScreenColors[i].Color))
            return runtimeStateError(error, QStringLiteral("Drawable %1 user color is non-finite").arg(i));
        const auto* indices = source._drawableIndexOverrides.Find(i);
        if (indices && !live2dstruct::CMeshTopology::IsValid(*indices, source.GetDrawableVertexCount(i)))
            return runtimeStateError(error, QStringLiteral("Drawable %1 index override is invalid").arg(i));
    }
    RuntimeValueSnapshot values, migrated;
    if (!source.CaptureRuntimeValues(values, error)) return false;
    if (AllowsModelUpdate(policy, CubismModelUpdatePolicy::AllowParameterStructureEdits)) {
        if (!MigrateRuntimeValues(values, migrated, seeds, error)) return false;
    } else {
        if (!seeds.isEmpty()) return runtimeStateError(error, QStringLiteral("Parameter seeds require structural migration"));
        if (!RebaseRuntimeValues(values, migrated, error)) return false;
    }
    auto indexOverrides = source._drawableIndexOverrides;
    if (AllowsModelUpdate(policy, CubismModelUpdatePolicy::AllowDrawableGeometryEdits)) indexOverrides.Clear();
    if (!RestoreRuntimeValues(migrated, error)) return false;
    std::swap(_drawableIndexOverrides, indexOverrides);
    for (int i = 0; i < parts; ++i) {
        _partOpacities[i] = source._partOpacities[i];
        _userPartMultiplyColors[i] = source._userPartMultiplyColors[i];
        _userPartScreenColors[i] = source._userPartScreenColors[i];
    }
    for (int i = 0; i < drawables; ++i) {
        const int sourceIndex = drawableMap.candidateToSource.value(i, -1);
        if (sourceIndex < 0) continue;
        _userMultiplyColors[i] = source._userMultiplyColors[sourceIndex];
        _userScreenColors[i] = source._userScreenColors[sourceIndex];
        _userCullings[i] = source._userCullings[sourceIndex];
    }
    _isOverwrittenModelMultiplyColors = source._isOverwrittenModelMultiplyColors;
    _isOverwrittenModelScreenColors = source._isOverwrittenModelScreenColors;
    _isOverwrittenCullings = source._isOverwrittenCullings;
    _modelOpacity = source._modelOpacity;
    return true;
}

static csmInt32 IsBitSet(const csmUint8 byte, const csmUint8 mask)
{
    return ((byte & mask) == mask);
}

CubismModel::CubismModel(Core::csmModel* model)
    : _model(model)
    , _parameterValues(NULL)
    , _parameterMaximumValues(NULL)
    , _parameterMinimumValues(NULL)
    , _partOpacities(NULL)
    , _isOverwrittenModelMultiplyColors(false)
    , _isOverwrittenModelScreenColors(false)
    , _isOverwrittenCullings(false)
    , _modelOpacity(1.0f)
{ }

CubismModel::~CubismModel()
{

    m_vetDrawMesh.clear();
    CSM_FREE_ALLIGNED(_model);
}

csmFloat32 CubismModel::GetParameterValue(CubismIdHandle parameterId)
{
    // 高速化のためにParameterIndexを取得できる機構になっているが、外部からの設定の時は呼び出し頻度が低いため不要
    const csmInt32 parameterIndex = GetParameterIndex(parameterId);
    return GetParameterValue(parameterIndex);
}

void CubismModel::SetParameterValue(CubismIdHandle parameterId, csmFloat32 value, csmFloat32 weight)
{
    const csmInt32 index = GetParameterIndex(parameterId);
    SetParameterValue(index, value, weight);
}

void CubismModel::AddParameterValue(CubismIdHandle parameterId, csmFloat32 value, csmFloat32 weight)
{
    const csmInt32 index = GetParameterIndex(parameterId);
    AddParameterValue(index, value, weight);
}

void CubismModel::AddParameterValue(csmInt32 parameterIndex, csmFloat32 value, csmFloat32 weight)
{
    SetParameterValue(parameterIndex, (GetParameterValue(parameterIndex) + (value * weight)));
}

void CubismModel::MultiplyParameterValue(CubismIdHandle parameterId, csmFloat32 value, csmFloat32 weight)
{
    const csmInt32 index = GetParameterIndex(parameterId);
    MultiplyParameterValue(index, value, weight);
}

void CubismModel::MultiplyParameterValue(csmInt32 parameterIndex, csmFloat32 value, csmFloat32 weight)
{
    SetParameterValue(parameterIndex, (GetParameterValue(parameterIndex) * (1.0f + (value - 1.0f) * weight)));
}

void CubismModel::InitMesh()
{
    qDebug() << "CubismModel::InitMesh()";
    m_vetDrawMesh.clear();
    const csmInt32 drawableCount = GetDrawableCount();
    const auto* positions = Core::csmGetDrawableVertexPositions(_model);
    const auto* uvs = Core::csmGetDrawableVertexUvs(_model);
    for(int index = 0 ;index < drawableCount;index++){
        const CubismIdHandle drawableId = GetDrawableId(index);
        if (!drawableId) continue;
        QString drawName(drawableId->GetString().GetRawString());

        int indexCount = GetDrawableVertexIndexCount(index);//点的数量
        const csmUint16* rawIndexArray = GetDrawableVertexIndices(index);
        int drawableVertexCount = GetDrawableVertexCount(index);

        if (indexCount < 0 || indexCount % 3 != 0 || indexCount > 3000000 ||
            drawableVertexCount < 0 || drawableVertexCount > 1000000 ||
            (!drawableVertexCount && indexCount) || (indexCount && !rawIndexArray)) continue;
        const float* vertexArray = drawableVertexCount && positions ? reinterpret_cast<const float*>(positions[index]) : nullptr;
        const float* uvArray = drawableVertexCount && uvs ? reinterpret_cast<const float*>(uvs[index]) : nullptr;
        if (drawableVertexCount && (!vertexArray || !uvArray)) continue;
        bool indicesValid = true;
        for (int i = 0; i < indexCount; ++i) {
            if (rawIndexArray[i] >= drawableVertexCount) {
                indicesValid = false;
                break;
            }
        }
        if (!indicesValid) continue;
        auto drawMesh = std::make_unique<CDrawMesh>(indexCount,rawIndexArray,drawableVertexCount*2,uvArray,vertexArray);
        m_vetDrawMesh.insert(drawName,drawMesh.get());
        drawMesh.release();
    }


    QStringList names;
    for (int ind : GetSortedDrawableIndices()) {
        const CubismIdHandle drawableId = GetDrawableId(ind);
        if (!drawableId) continue;
        names.append(QString::fromUtf8(drawableId->GetString().GetRawString()));
    }
    m_vetDrawMesh.SetRenderOrder(names);


}



void CubismModel::DrawMesh(const QString& drawKey,const QMatrix4x4 &mvp)
{
    m_vetDrawMesh.DrawMesh(drawKey,mvp);
}

void CubismModel::Update() const
{
    // Update model.
    Core::csmUpdateModel(_model);

    // Reset dynamic drawable flags.
    Core::csmResetDrawableDynamicFlags(_model);
}

void CubismModel::SetPartOpacity(CubismIdHandle partId, csmFloat32 opacity)
{
    // 高速化のためにPartIndexを取得できる機構になっているが、外部からの設定の時は呼び出し頻度が低いため不要
    const csmInt32 index = GetPartIndex(partId);

    if (index < 0)
    {
        return; // パーツが無いのでスキップ
    }

    SetPartOpacity(index, opacity);
}

void CubismModel::SetPartOpacity(csmInt32 partIndex, csmFloat32 opacity)
{
    if (_notExistPartOpacities.contains(partIndex))
    {
        _notExistPartOpacities[partIndex] = opacity;
        return;
    }

    //インデックスの範囲内検知
    CSM_ASSERT(0 <= partIndex && partIndex < GetPartCount());

    _partOpacities[partIndex] = opacity;
}

csmFloat32 CubismModel::GetPartOpacity(CubismIdHandle partId)
{
    // 高速化のためにPartIndexを取得できる機構になっているが、外部からの設定の時は呼び出し頻度が低いため不要
    const csmInt32 index = GetPartIndex(partId);

    if (index < 0)
    {
        return 0; //パーツが無いのでスキップ
    }

    return GetPartOpacity(index);
}

csmFloat32 CubismModel::GetPartOpacity(csmInt32 partIndex)
{
    if (_notExistPartOpacities.contains(partIndex))
    {
        // モデルに存在しないパーツIDの場合、非存在パーツリストから不透明度を返す
        return _notExistPartOpacities[partIndex];
    }

    //インデックスの範囲内検知
    CSM_ASSERT(0 <= partIndex && partIndex < GetPartCount());

    return _partOpacities[partIndex];
}

csmInt32 CubismModel::GetParameterCount() const
{
    return Core::csmGetParameterCount(_model);
}

Core::csmParameterType CubismModel::GetParameterType(csmUint32 parameterIndex) const
{
    return Core::csmGetParameterTypes(_model)[parameterIndex];
}

csmFloat32 CubismModel::GetParameterDefaultValue(csmUint32 parameterIndex) const
{
    return Core::csmGetParameterDefaultValues(_model)[parameterIndex];
}

csmFloat32 CubismModel::GetParameterMaximumValue(csmUint32 parameterIndex) const
{
    return Core::csmGetParameterMaximumValues(_model)[parameterIndex];
}

csmFloat32 CubismModel::GetParameterMinimumValue(csmUint32 parameterIndex) const
{
    return Core::csmGetParameterMinimumValues(_model)[parameterIndex];
}

csmInt32 CubismModel::FindRealParameterIndex(CubismIdHandle parameterId) const
{
    if (!parameterId) return -1;
    for (int index = 0; index < _parameterIds.GetSize(); ++index)
        if (_parameterIds[index] == parameterId) return index;
    return -1;
}

csmInt32 CubismModel::GetParameterIndex(CubismIdHandle parameterId)
{
    csmInt32            parameterIndex;
    const csmInt32      idCount = Core::csmGetParameterCount(_model);


    for (parameterIndex = 0; parameterIndex < idCount; ++parameterIndex)
    {
        if (parameterId != _parameterIds[parameterIndex])
        {
            continue;
        }

        return parameterIndex;
    }

    // モデルに存在していない場合、非存在パラメータIDリスト内を検索し、そのインデックスを返す
    if (_notExistParameterId.contains(parameterId))
    {
        return _notExistParameterId[parameterId];
    }

    // 非存在パラメータIDリストにない場合、新しく要素を追加する
    parameterIndex = Core::csmGetParameterCount(_model) + _notExistParameterId.size();

    _notExistParameterId[parameterId] = parameterIndex;
    _notExistParameterValues.insert(parameterIndex, 0.0f);

    return parameterIndex;
}

CubismIdHandle CubismModel::GetParameterId(csmUint32 parameterIndex)
{
    CSM_ASSERT(0 <= parameterIndex && parameterIndex < _parameterIds.GetSize());
    return _parameterIds[parameterIndex];
}

const CubismIdHandle CubismModel::GetParameterId(csmUint32 parameterIndex) const
{
    CSM_ASSERT(0 <= parameterIndex && parameterIndex < _parameterIds.GetSize());
    return _parameterIds[parameterIndex];
}

csmFloat32 CubismModel::GetParameterValue(csmInt32 parameterIndex)
{
    if (_notExistParameterValues.contains(parameterIndex))
    {
        return _notExistParameterValues[parameterIndex];
    }

    //インデックスの範囲内検知
    CSM_ASSERT(0 <= parameterIndex && parameterIndex < GetParameterCount());

    return _parameterValues[parameterIndex];
}

void CubismModel::SetParameterValue(csmInt32 parameterIndex, csmFloat32 value, csmFloat32 weight)
{
    if (_notExistParameterValues.contains(parameterIndex))
    {
        _notExistParameterValues[parameterIndex] = (weight == 1)
                                                         ? value
                                                         : (_notExistParameterValues[parameterIndex] * (1 - weight)) +
                                                         (value * weight);
        return;
    }

    //インデックスの範囲内検知
    CSM_ASSERT(0 <= parameterIndex && parameterIndex < GetParameterCount());

    if (Core::csmGetParameterMaximumValues(_model)[parameterIndex] < value)
    {
        value = Core::csmGetParameterMaximumValues(_model)[parameterIndex];
    }
    if (Core::csmGetParameterMinimumValues(_model)[parameterIndex] > value)
    {
        value = Core::csmGetParameterMinimumValues(_model)[parameterIndex];
    }

    _parameterValues[parameterIndex] = (weight == 1)
                                      ? value
                                      : _parameterValues[parameterIndex] = (_parameterValues[parameterIndex] * (1 - weight)) + (value * weight);
}

csmFloat32 CubismModel::GetCanvasWidthPixel() const
{
    if (_model == NULL)
    {
        return 0.0f;
    }

    Core::csmVector2 tmpSizeInPixels;
    Core::csmVector2 tmpOriginInPixels;
    csmFloat32 tmpPixelsPerUnit;

    Core::csmReadCanvasInfo(_model, &tmpSizeInPixels, &tmpOriginInPixels, &tmpPixelsPerUnit);

    return tmpSizeInPixels.X;
}

csmFloat32 CubismModel::GetCanvasHeightPixel() const
{
    if (_model == NULL)
    {
        return 0.0f;
    }

    Core::csmVector2 tmpSizeInPixels;
    Core::csmVector2 tmpOriginInPixels;
    csmFloat32 tmpPixelsPerUnit;

    Core::csmReadCanvasInfo(_model, &tmpSizeInPixels, &tmpOriginInPixels, &tmpPixelsPerUnit);

    return tmpSizeInPixels.Y;
}

csmFloat32 CubismModel::GetPixelsPerUnit() const
{
    if (_model == NULL)
    {
        return 0.0f;
    }

    Core::csmVector2 tmpSizeInPixels;
    Core::csmVector2 tmpOriginInPixels;
    csmFloat32 tmpPixelsPerUnit;

    Core::csmReadCanvasInfo(_model, &tmpSizeInPixels, &tmpOriginInPixels, &tmpPixelsPerUnit);

    return tmpPixelsPerUnit;
}

csmFloat32 CubismModel::GetCanvasWidth() const
{
    if (_model == NULL)
    {
        return 0.0f;
    }

    Core::csmVector2 tmpSizeInPixels;
    Core::csmVector2 tmpOriginInPixels;
    csmFloat32 tmpPixelsPerUnit;

    Core::csmReadCanvasInfo(_model, &tmpSizeInPixels, &tmpOriginInPixels, &tmpPixelsPerUnit);

    return tmpSizeInPixels.X / tmpPixelsPerUnit;
}

csmFloat32 CubismModel::GetCanvasPixelsWidth() const
{
    if (_model == NULL)
    {
        return 0.0f;
    }

    Core::csmVector2 tmpSizeInPixels;
    Core::csmVector2 tmpOriginInPixels;
    csmFloat32 tmpPixelsPerUnit;
    Core::csmReadCanvasInfo(_model, &tmpSizeInPixels, &tmpOriginInPixels, &tmpPixelsPerUnit);
    return tmpSizeInPixels.X;
}


csmFloat32  CubismModel::GetCanvasOriginInPixelsWidth() const{
    if (_model == NULL)
    {
        return 0.0f;
    }

    Core::csmVector2 tmpSizeInPixels;
    Core::csmVector2 tmpOriginInPixels;
    csmFloat32 tmpPixelsPerUnit;

    Core::csmReadCanvasInfo(_model, &tmpSizeInPixels, &tmpOriginInPixels, &tmpPixelsPerUnit);

    return tmpOriginInPixels.X;

}
csmFloat32  CubismModel::GetCanvasOriginInPixelsHeight() const{
    if (_model == NULL)
    {
        return 0.0f;
    }

    Core::csmVector2 tmpSizeInPixels;
    Core::csmVector2 tmpOriginInPixels;
    csmFloat32 tmpPixelsPerUnit;

    Core::csmReadCanvasInfo(_model, &tmpSizeInPixels, &tmpOriginInPixels, &tmpPixelsPerUnit);

    return tmpOriginInPixels.Y;
}

csmFloat32 CubismModel::GetCanvasHeight() const
{
    if (_model == NULL)
    {
        return 0.0f;
    }

    Core::csmVector2 tmpSizeInPixels;
    Core::csmVector2 tmpOriginInPixels;
    csmFloat32 tmpPixelsPerUnit;

    Core::csmReadCanvasInfo(_model, &tmpSizeInPixels, &tmpOriginInPixels, &tmpPixelsPerUnit);

    return tmpSizeInPixels.Y / tmpPixelsPerUnit;
}


csmFloat32 CubismModel::GetCanvasPixelsHeight() const
{
    if (_model == NULL)
    {
        return 0.0f;
    }

    Core::csmVector2 tmpSizeInPixels;
    Core::csmVector2 tmpOriginInPixels;
    csmFloat32 tmpPixelsPerUnit;

    Core::csmReadCanvasInfo(_model, &tmpSizeInPixels, &tmpOriginInPixels, &tmpPixelsPerUnit);

    return tmpSizeInPixels.Y ;
}


csmInt32 CubismModel::GetDrawableIndex(CubismIdHandle drawableId) const
{
    const csmInt32 drawableCount = Core::csmGetDrawableCount(_model);

    for (csmInt32 drawableIndex = 0; drawableIndex < drawableCount; ++drawableIndex)
    {
        if (_drawableIds[drawableIndex] == drawableId)
        {
            return drawableIndex;
        }
    }

    return -1;
}

const csmFloat32* CubismModel::GetDrawableVertices(csmInt32 drawableIndex) const
{
    return reinterpret_cast<const csmFloat32*>(GetDrawableVertexPositions(drawableIndex));
}

csmInt32 CubismModel::GetPartIndex(CubismIdHandle partId)
{
    csmInt32            partIndex;
    const csmInt32      idCount = Core::csmGetPartCount(_model);

    for (partIndex = 0; partIndex < idCount; ++partIndex)
    {
        if (partId == _partIds[partIndex])
        {
            return partIndex;
        }
    }

    const csmInt32 partCount = Core::csmGetPartCount(_model);

    // モデルに存在していない場合、非存在パーツIDリスト内にあるかを検索し、そのインデックスを返す
    if (_notExistPartId.contains(partId))
    {
        return _notExistPartId[partId];
    }

    // 非存在パーツIDリストにない場合、新しく要素を追加する
    partIndex = partCount + _notExistPartId.size();

    _notExistPartId[partId] = partIndex;
    _notExistPartOpacities.insert(partIndex, 0.0f);

    return partIndex;
}

void CubismModel::Initialize()
{
    CSM_ASSERT(_model);

    _parameterValues = Core::csmGetParameterValues(_model);
    _partOpacities = Core::csmGetPartOpacities(_model);
    _parameterMaximumValues = Core::csmGetParameterMaximumValues(_model);
    _parameterMinimumValues = Core::csmGetParameterMinimumValues(_model);

    {
        const csmChar** parameterIds = Core::csmGetParameterIds(_model);
        const csmInt32  parameterCount = Core::csmGetParameterCount(_model);

        _parameterIds.PrepareCapacity(parameterCount);
        for (csmInt32 i = 0; i < parameterCount; ++i)
        {
            _parameterIds.PushBack(CubismFramework::GetIdManager()->GetId(parameterIds[i]));
        }
    }

    const csmInt32  partCount = Core::csmGetPartCount(_model);
    {
        const csmChar** partIds = Core::csmGetPartIds(_model);

        _partIds.PrepareCapacity(partCount);
        for (csmInt32 i = 0; i < partCount; ++i)
        {
            _partIds.PushBack(CubismFramework::GetIdManager()->GetId(partIds[i]));
        }

        _userPartMultiplyColors.PrepareCapacity(partCount);
        _userPartScreenColors.PrepareCapacity(partCount);
        _partChildDrawables.Resize(partCount);
    }

    {
        const csmChar** drawableIds = Core::csmGetDrawableIds(_model);
        const csmInt32  drawableCount = Core::csmGetDrawableCount(_model);

        _drawableIds.PrepareCapacity(drawableCount);
        _userMultiplyColors.PrepareCapacity(drawableCount);
        _userScreenColors.PrepareCapacity(drawableCount);
        _userCullings.PrepareCapacity(drawableCount);

        // カリング設定
        DrawableCullingData userCulling;
        userCulling.IsOverwritten = false;
        userCulling.IsCulling = 0;

        // 乗算色
        Rendering::CubismRenderer::CubismTextureColor multiplyColor;
        multiplyColor.R = 1.0f;
        multiplyColor.G = 1.0f;
        multiplyColor.B = 1.0f;
        multiplyColor.A = 1.0f;

        // スクリーン色
        Rendering::CubismRenderer::CubismTextureColor screenColor;
        screenColor.R = 0.0f;
        screenColor.G = 0.0f;
        screenColor.B = 0.0f;
        screenColor.A = 1.0f;

        // Parts
        {
            // 乗算色
            PartColorData userMultiplyColor;
            userMultiplyColor.IsOverwritten = false;
            userMultiplyColor.Color = multiplyColor;

            // スクリーン色
            PartColorData userScreenColor;
            userScreenColor.IsOverwritten = false;
            userScreenColor.Color = screenColor;

            for (csmInt32 i = 0; i < partCount; ++i)
            {
                _userPartMultiplyColors.PushBack(userMultiplyColor);
                _userPartScreenColors.PushBack(userScreenColor);
            }
        }

        // Drawables
        {
            // 乗算色
            DrawableColorData userMultiplyColor;
            userMultiplyColor.IsOverwritten = false;
            userMultiplyColor.Color = multiplyColor;

            // スクリーン色
            DrawableColorData userScreenColor;
            userScreenColor.IsOverwritten = false;
            userScreenColor.Color = screenColor;

            for (csmInt32 i = 0; i < drawableCount; ++i)
            {
                _drawableIds.PushBack(CubismFramework::GetIdManager()->GetId(drawableIds[i]));
                _userMultiplyColors.PushBack(userMultiplyColor);
                _userScreenColors.PushBack(userScreenColor);
                _userCullings.PushBack(userCulling);

                csmInt32 parentIndex = Core::csmGetDrawableParentPartIndices(_model)[i];
                if (parentIndex >= 0)
                {
                    _partChildDrawables[parentIndex].PushBack(i);
                }
            }
        }
    }
}

CubismIdHandle CubismModel::GetDrawableId(csmInt32 drawableIndex) const
{
    CSM_ASSERT(0 <= drawableIndex && drawableIndex < _drawableIds.GetSize());
    return _drawableIds[drawableIndex];
}

CubismIdHandle CubismModel::GetPartId(csmUint32 partIndex)
{
    CSM_ASSERT(0 <= partIndex && partIndex < _partIds.GetSize());
    return _partIds[partIndex];
}

csmInt32 CubismModel::GetPartCount() const
{
    const csmInt32 partCount = Core::csmGetPartCount(_model);
    return partCount;
}

const csmInt32* CubismModel::GetDrawableRenderOrders() const
{
    const csmInt32* renderOrders = Core::csmGetDrawableRenderOrders(_model);
    return renderOrders;
}

const csmInt32* CubismModel::GetDrawableDrawOrders() const
{
    return Core::csmGetDrawableDrawOrders(_model);
}

QVector<int> CubismModel::GetSortedDrawableIndices(const QHash<QString, int>& overrides) const
{
    return GetSortedDrawableIndices(overrides, {}, {});
}

QVector<int> CubismModel::GetSortedDrawableIndices(
    const QHash<QString, int>& overrides,
    const QHash<QString, int>& groupIndices,
    const QVector<live2dstruct::CDrawableOrderGroup>& groups) const
{
    const csmInt32* drawOrders = GetDrawableDrawOrders();
    const csmInt32* renderOrders = GetDrawableRenderOrders();
    QVector<live2dstruct::CDrawableOrder> entries;
    const int count = GetDrawableCount();
    entries.reserve(count);
    for (int index = 0; index < count; ++index) {
        const CubismIdHandle id = GetDrawableId(index);
        entries.append({id ? QString::fromUtf8(id->GetString().GetRawString()) : QString(),
                        drawOrders ? drawOrders[index] : 0,
                        renderOrders ? renderOrders[index] : index});
    }
    return live2dstruct::ResolveDrawableOrder(entries, drawOrders ? overrides : QHash<QString, int>(),
                                              groupIndices, groups);
}

csmInt32 CubismModel::GetDrawableCount() const
{
    const csmInt32 drawableCount = Core::csmGetDrawableCount(_model);
    return drawableCount;
}

csmInt32 CubismModel::GetDrawableTextureIndices(csmInt32 drawableIndex) const
{
    return GetDrawableTextureIndex(drawableIndex);
}

csmInt32 CubismModel::GetDrawableTextureIndex(csmInt32 drawableIndex) const
{
    const csmInt32* textureIndices = Core::csmGetDrawableTextureIndices(_model);
    return textureIndices[drawableIndex];
}

csmInt32 CubismModel::GetDrawableVertexIndexCount(csmInt32 drawableIndex) const
{
    if (const auto* indices = _drawableIndexOverrides.Find(drawableIndex)) return indices->size();
    const csmInt32* indexCounts = Core::csmGetDrawableIndexCounts(_model);
    return indexCounts[drawableIndex];
}

csmInt32 CubismModel::GetDrawableVertexCount(csmInt32 drawableIndex) const
{
    const csmInt32* vertexCounts = Core::csmGetDrawableVertexCounts(_model);
    return vertexCounts[drawableIndex];
}

const csmUint16* CubismModel::GetDrawableVertexIndices(csmInt32 drawableIndex) const
{
    if (const auto* indices = _drawableIndexOverrides.Find(drawableIndex)) return indices->constData();
    const csmUint16** indicesArray = Core::csmGetDrawableIndices(_model);
    return indicesArray ? indicesArray[drawableIndex] : nullptr;
}

bool CubismModel::SetDrawableIndicesOverride(csmInt32 drawableIndex, const QVector<quint16>& indices)
{
    if (!_model || drawableIndex < 0 || drawableIndex >= GetDrawableCount()) return false;
    const int vertexCount = GetDrawableVertexCount(drawableIndex);
    if (!live2dstruct::CMeshTopology::IsValid(indices, vertexCount)) return false;
    const auto id = GetDrawableId(drawableIndex);
    if (!id || !m_vetDrawMesh.SetMeshIndices(QString::fromUtf8(id->GetString().GetRawString()), indices))
        return false;
    return _drawableIndexOverrides.Set(drawableIndex, vertexCount, indices);
}

void CubismModel::ClearDrawableIndicesOverrides()
{
    _drawableIndexOverrides.Clear();
}

const Core::csmVector2* CubismModel::GetDrawableVertexPositions(csmInt32 drawableIndex) const
{
    const Core::csmVector2** verticesArray = Core::csmGetDrawableVertexPositions(_model);
    return verticesArray ? verticesArray[drawableIndex] : nullptr;
}

const Core::csmVector2* CubismModel::GetDrawableVertexUvs(csmInt32 drawableIndex) const
{
    const Core::csmVector2** uvsArray = Core::csmGetDrawableVertexUvs(_model);
    return uvsArray ? uvsArray[drawableIndex] : nullptr;
}

csmFloat32 CubismModel::GetDrawableOpacity(csmInt32 drawableIndex) const
{

    const csmFloat32* opacities = Core::csmGetDrawableOpacities(_model);
    csmFloat32 ofs = opacities[drawableIndex];

    // if(LAppDelegate::GetInstance()->m_IsPrintALl){
    //      if(ofs == 0.0f){
    //          ofs = 1.0f;
    //      }
    // }
    return ofs;//模型不透明度默认值
}

csmFloat32 CubismModel::GetDrawableOpacityAll(csmInt32 drawableIndex, bool IsALL) const
{
    const csmFloat32* opacities = Core::csmGetDrawableOpacities(_model);
    csmFloat32 ofs = opacities[drawableIndex];

    if(IsALL){
         if(ofs == 0.0f){
             ofs = 1.0f;
         }
    }
    return ofs;//模型不透明度默认值
}

Core::csmVector4 CubismModel::GetDrawableMultiplyColor(csmInt32 drawableIndex) const
{
    const Core::csmVector4* multiplyColors = Core::csmGetDrawableMultiplyColors(_model);
    return multiplyColors[drawableIndex];
}

Core::csmVector4 CubismModel::GetDrawableScreenColor(csmInt32 drawableIndex) const
{
    const Core::csmVector4* screenColors = Core::csmGetDrawableScreenColors(_model);
    return screenColors[drawableIndex];
}

csmInt32 CubismModel::GetDrawableParentPartIndex(csmUint32 drawableIndex) const
{
    return Core::csmGetDrawableParentPartIndices(_model)[drawableIndex];
}

csmBool CubismModel::GetDrawableDynamicFlagIsVisible(csmInt32 drawableIndex) const
{
    const Core::csmFlags* dynamicFlags = Core::csmGetDrawableDynamicFlags(_model);
    return IsBitSet(dynamicFlags[drawableIndex], Core::csmIsVisible)!=0 ? true : false;
}

csmBool CubismModel::GetDrawableDynamicFlagVisibilityDidChange(csmInt32 drawableIndex) const
{
    const Core::csmFlags* dynamicFlags = Core::csmGetDrawableDynamicFlags(_model);
    return IsBitSet(dynamicFlags[drawableIndex], Core::csmVisibilityDidChange)!=0 ? true : false;
}

csmBool CubismModel::GetDrawableDynamicFlagOpacityDidChange(csmInt32 drawableIndex) const
{
    const Core::csmFlags* dynamicFlags = Core::csmGetDrawableDynamicFlags(_model);
    return IsBitSet(dynamicFlags[drawableIndex], Core::csmOpacityDidChange) != 0 ? true : false;
}

csmBool CubismModel::GetDrawableDynamicFlagDrawOrderDidChange(csmInt32 drawableIndex) const
{
    const Core::csmFlags* dynamicFlags = Core::csmGetDrawableDynamicFlags(_model);
    return IsBitSet(dynamicFlags[drawableIndex], Core::csmDrawOrderDidChange) != 0 ? true : false;
}

csmBool CubismModel::GetDrawableDynamicFlagRenderOrderDidChange(csmInt32 drawableIndex) const
{
    const Core::csmFlags* dynamicFlags = Core::csmGetDrawableDynamicFlags(_model);
    return IsBitSet(dynamicFlags[drawableIndex], Core::csmRenderOrderDidChange) != 0 ? true : false;
}

csmBool CubismModel::GetDrawableDynamicFlagVertexPositionsDidChange(csmInt32 drawableIndex) const
{
    const Core::csmFlags* dynamicFlags = Core::csmGetDrawableDynamicFlags(_model);
    return IsBitSet(dynamicFlags[drawableIndex], Core::csmVertexPositionsDidChange) != 0 ? true : false;
}

csmBool CubismModel::GetDrawableDynamicFlagBlendColorDidChange(csmInt32 drawableIndex) const
{
    const Core::csmFlags* dynamicFlags = Core::csmGetDrawableDynamicFlags(_model);
    return IsBitSet(dynamicFlags[drawableIndex], Core::csmBlendColorDidChange) != 0 ? true : false;
}

Rendering::CubismRenderer::CubismBlendMode CubismModel::GetDrawableBlendMode(csmInt32 drawableIndex) const
{
    const csmInt32 mode = GetDrawableBlendModeValue(drawableIndex);
    return (mode == 1 || mode == 3)
               ? Rendering::CubismRenderer::CubismBlendMode_Additive
               : (mode == 2 || mode == 6)
               ? Rendering::CubismRenderer::CubismBlendMode_Multiplicative
               : Rendering::CubismRenderer::CubismBlendMode_Normal;
}

csmInt32 CubismModel::GetDrawableBlendModeValue(csmInt32 drawableIndex) const
{
#if LIVE2D_CORE_API_VERSION >= 6
    const csmInt32* modes = Core::csmGetDrawableBlendModes(_model);
    if (!modes || drawableIndex < 0 || drawableIndex >= GetDrawableCount()) return 0;
    return modes[drawableIndex];
#else
    const csmUint8* flags = Core::csmGetDrawableConstantFlags(_model);
    if (!flags || drawableIndex < 0 || drawableIndex >= GetDrawableCount()) return 0;
    return IsBitSet(flags[drawableIndex], Core::csmBlendAdditive) ? 1 :
        IsBitSet(flags[drawableIndex], Core::csmBlendMultiplicative) ? 2 : 0;
#endif
}

csmInt32 CubismModel::GetOffscreenCount() const
{
#if LIVE2D_CORE_API_VERSION >= 6
    return _model ? Core::csmGetOffscreenCount(_model) : 0;
#else
    return 0;
#endif
}

const csmInt32* CubismModel::GetOffscreenRenderOrders() const
{
#if LIVE2D_CORE_API_VERSION >= 6
    if (!_model) return nullptr;
    const auto* orders = Core::csmGetRenderOrders(_model);
    return orders ? orders + GetDrawableCount() : nullptr;
#else
    return nullptr;
#endif
}

const csmInt32* CubismModel::GetOffscreenBlendModes() const
{
#if LIVE2D_CORE_API_VERSION >= 6
    return _model ? Core::csmGetOffscreenBlendModes(_model) : nullptr;
#else
    return nullptr;
#endif
}

const csmFloat32* CubismModel::GetOffscreenOpacities() const
{
#if LIVE2D_CORE_API_VERSION >= 6
    return _model ? Core::csmGetOffscreenOpacities(_model) : nullptr;
#else
    return nullptr;
#endif
}

const csmInt32* CubismModel::GetOffscreenOwnerIndices() const
{
#if LIVE2D_CORE_API_VERSION >= 6
    return _model ? Core::csmGetOffscreenOwnerIndices(_model) : nullptr;
#else
    return nullptr;
#endif
}

const csmInt32* CubismModel::GetPartOffscreenIndices() const
{
#if LIVE2D_CORE_API_VERSION >= 6
    return _model ? Core::csmGetPartOffscreenIndices(_model) : nullptr;
#else
    return nullptr;
#endif
}

const Core::csmVector4* CubismModel::GetOffscreenMultiplyColors() const
{
#if LIVE2D_CORE_API_VERSION >= 6
    return _model ? Core::csmGetOffscreenMultiplyColors(_model) : nullptr;
#else
    return nullptr;
#endif
}

const Core::csmVector4* CubismModel::GetOffscreenScreenColors() const
{
#if LIVE2D_CORE_API_VERSION >= 6
    return _model ? Core::csmGetOffscreenScreenColors(_model) : nullptr;
#else
    return nullptr;
#endif
}

const Core::csmFlags* CubismModel::GetOffscreenConstantFlags() const
{
#if LIVE2D_CORE_API_VERSION >= 6
    return _model ? Core::csmGetOffscreenConstantFlags(_model) : nullptr;
#else
    return nullptr;
#endif
}

const csmInt32* CubismModel::GetOffscreenMaskCounts() const
{
#if LIVE2D_CORE_API_VERSION >= 6
    return _model ? Core::csmGetOffscreenMaskCounts(_model) : nullptr;
#else
    return nullptr;
#endif
}

const csmInt32** CubismModel::GetOffscreenMasks() const
{
#if LIVE2D_CORE_API_VERSION >= 6
    return _model ? Core::csmGetOffscreenMasks(_model) : nullptr;
#else
    return nullptr;
#endif
}

csmBool CubismModel::GetDrawableInvertedMask(csmInt32 drawableIndex) const
{
    const csmUint8* constantFlags = Core::csmGetDrawableConstantFlags(_model);
    return IsBitSet(constantFlags[drawableIndex], Core::csmIsInvertedMask) != 0 ? true : false;
}

const csmInt32** CubismModel::GetDrawableMasks() const
{
    const csmInt32** masks = Core::csmGetDrawableMasks(_model);
    return masks;
}

const csmInt32* CubismModel::GetDrawableMaskCounts() const
{
    const csmInt32* maskCounts = Core::csmGetDrawableMaskCounts(_model);
    return maskCounts;
}

void CubismModel::LoadParameters()
{
    csmInt32       parameterCount = Core::csmGetParameterCount(_model);
    const csmInt32 savedParameterCount = static_cast<csmInt32>(_savedParameters.size());

    if (parameterCount > savedParameterCount)
    {
        parameterCount = savedParameterCount;
    }

    if (!_savedPresence.isEmpty() && _savedPresence.size() != _savedParameters.size()) return;
    for (csmInt32 i = 0; i < parameterCount; ++i)
    {
        if (_savedPresence.isEmpty() || _savedPresence[i] == 1) _parameterValues[i] = _savedParameters[i];
    }
}

void CubismModel::SaveParameters()
{
    const csmInt32 parameterCount = Core::csmGetParameterCount(_model);
    QVector<float> saved;
    saved.reserve(parameterCount);
    for (int index = 0; index < parameterCount; ++index) saved.append(_parameterValues[index]);
    _savedParameters.swap(saved);
    _savedPresence.clear();
}

Rendering::CubismRenderer::CubismTextureColor CubismModel::GetMultiplyColor(csmInt32 drawableIndex) const
{
    if (GetOverwriteFlagForModelMultiplyColors() || GetOverwriteFlagForDrawableMultiplyColors(drawableIndex))
    {
        return _userMultiplyColors[drawableIndex].Color;
    }

    const Core::csmVector4 color = GetDrawableMultiplyColor(drawableIndex);

    return Rendering::CubismRenderer::CubismTextureColor(color.X, color.Y, color.Z, color.W);
}

Rendering::CubismRenderer::CubismTextureColor CubismModel::GetScreenColor(csmInt32 drawableIndex) const
{
    if (GetOverwriteFlagForModelScreenColors() || GetOverwriteFlagForDrawableScreenColors(drawableIndex))
    {
        return _userScreenColors[drawableIndex].Color;
    }

    const Core::csmVector4 color = GetDrawableScreenColor(drawableIndex);
    return Rendering::CubismRenderer::CubismTextureColor(color.X, color.Y, color.Z, color.W);
}

void CubismModel::SetMultiplyColor(csmInt32 drawableIndex, const Rendering::CubismRenderer::CubismTextureColor& color)
{
    SetMultiplyColor(drawableIndex, color.R, color.G, color.B, color.A);
}

void CubismModel::SetMultiplyColor(csmInt32 drawableIndex, csmFloat32 r, csmFloat32 g, csmFloat32 b, csmFloat32 a)
{
    _userMultiplyColors[drawableIndex].Color.R = r;
    _userMultiplyColors[drawableIndex].Color.G = g;
    _userMultiplyColors[drawableIndex].Color.B = b;
    _userMultiplyColors[drawableIndex].Color.A = a;
}

void CubismModel::SetScreenColor(csmInt32 drawableIndex, const Rendering::CubismRenderer::CubismTextureColor& color)
{
    SetScreenColor(drawableIndex, color.R, color.G, color.B, color.A);
}

void CubismModel::SetScreenColor(csmInt32 drawableIndex, csmFloat32 r, csmFloat32 g, csmFloat32 b, csmFloat32 a)
{
    _userScreenColors[drawableIndex].Color.R = r;
    _userScreenColors[drawableIndex].Color.G = g;
    _userScreenColors[drawableIndex].Color.B = b;
    _userScreenColors[drawableIndex].Color.A = a;
}

Rendering::CubismRenderer::CubismTextureColor CubismModel::GetPartMultiplyColor(csmInt32 partIndex) const
{
    return _userPartMultiplyColors[partIndex].Color;
}

Rendering::CubismRenderer::CubismTextureColor CubismModel::GetPartScreenColor(csmInt32 partIndex) const
{
    return _userPartScreenColors[partIndex].Color;
}

void CubismModel::SetPartMultiplyColor(csmInt32 partIndex, const Rendering::CubismRenderer::CubismTextureColor& color)
{
    SetPartMultiplyColor(partIndex, color.R, color.G, color.B, color.A);
}

void CubismModel::SetPartColor(
    csmUint32 partIndex,
    csmFloat32 r, csmFloat32 g, csmFloat32 b, csmFloat32 a,
    csmVector<PartColorData>& partColors,
    csmVector <DrawableColorData>& drawableColors)
{
    partColors[partIndex].Color.R = r;
    partColors[partIndex].Color.G = g;
    partColors[partIndex].Color.B = b;
    partColors[partIndex].Color.A = a;

    if (partColors[partIndex].IsOverwritten)
    {
        for (csmUint32 i = 0; i < _partChildDrawables[partIndex].GetSize(); i++)
        {
            csmUint32 drawableIndex = _partChildDrawables[partIndex][i];
            drawableColors[drawableIndex].Color.R = r;
            drawableColors[drawableIndex].Color.G = g;
            drawableColors[drawableIndex].Color.B = b;
            drawableColors[drawableIndex].Color.A = a;
        }
    }
}

void CubismModel::SetPartMultiplyColor(csmInt32 partIndex, csmFloat32 r, csmFloat32 g, csmFloat32 b, csmFloat32 a)
{
    SetPartColor(partIndex, r, g, b, a, _userPartMultiplyColors, _userMultiplyColors);
}

void CubismModel::SetPartScreenColor(csmInt32 partIndex, const Rendering::CubismRenderer::CubismTextureColor& color)
{
    SetPartScreenColor(partIndex, color.R, color.G, color.B, color.A);
}

void CubismModel::SetPartScreenColor(csmInt32 partIndex, csmFloat32 r, csmFloat32 g, csmFloat32 b, csmFloat32 a)
{
    SetPartColor(partIndex, r, g, b, a, _userPartScreenColors, _userScreenColors);
}

csmBool CubismModel::GetOverwriteFlagForModelMultiplyColors() const
{
    return _isOverwrittenModelMultiplyColors;
}

csmBool CubismModel::GetOverwriteFlagForModelScreenColors() const
{
    return _isOverwrittenModelScreenColors;
}

void CubismModel::SetOverwriteFlagForModelMultiplyColors(csmBool value)
{
    _isOverwrittenModelMultiplyColors = value;
}

void CubismModel::SetOverwriteFlagForModelScreenColors(csmBool value)
{
    _isOverwrittenModelScreenColors = value;
}

csmBool CubismModel::GetOverwriteFlagForDrawableMultiplyColors(csmInt32 drawableIndex) const
{
    return _userMultiplyColors[drawableIndex].IsOverwritten;
}

csmBool CubismModel::GetOverwriteFlagForDrawableScreenColors(csmInt32 drawableIndex) const
{
    return _userScreenColors[drawableIndex].IsOverwritten;
}

void CubismModel::SetOverwriteFlagForDrawableMultiplyColors(csmUint32 drawableIndex, csmBool value)
{
    _userMultiplyColors[drawableIndex].IsOverwritten = value;
}

void CubismModel::SetOverwriteFlagForDrawableScreenColors(csmUint32 drawableIndex, csmBool value)
{
    _userScreenColors[drawableIndex].IsOverwritten = value;
}

csmBool CubismModel::GetOverwriteColorForPartMultiplyColors(csmInt32 partIndex) const
{
    return _userPartMultiplyColors[partIndex].IsOverwritten;
}

csmBool CubismModel::GetOverwriteColorForPartScreenColors(csmInt32 partIndex) const
{
    return _userPartScreenColors[partIndex].IsOverwritten;
}

void CubismModel::SetOverwriteColorForPartColors(
    csmUint32 partIndex,
    csmBool value,
    csmVector<PartColorData>& partColors,
    csmVector <DrawableColorData>& drawableColors)
{
    partColors[partIndex].IsOverwritten = value;

    for (csmUint32 i = 0; i < _partChildDrawables[partIndex].GetSize(); i++)
    {
        csmUint32 drawableIndex = _partChildDrawables[partIndex][i];
        drawableColors[drawableIndex].IsOverwritten = value;
        if (value)
        {
            drawableColors[drawableIndex].Color.R = partColors[partIndex].Color.R;
            drawableColors[drawableIndex].Color.G = partColors[partIndex].Color.G;
            drawableColors[drawableIndex].Color.B = partColors[partIndex].Color.B;
            drawableColors[drawableIndex].Color.A = partColors[partIndex].Color.A;
        }
    }
}

void CubismModel::SetOverwriteColorForPartMultiplyColors(csmUint32 partIndex, csmBool value)
{
    _userPartMultiplyColors[partIndex].IsOverwritten = value;
    SetOverwriteColorForPartColors(partIndex, value, _userPartMultiplyColors, _userMultiplyColors);
}

void CubismModel::SetOverwriteColorForPartScreenColors(csmUint32 partIndex, csmBool value)
{
    _userPartScreenColors[partIndex].IsOverwritten = value;
    SetOverwriteColorForPartColors(partIndex, value, _userPartScreenColors, _userScreenColors);
}

csmInt32 CubismModel::GetDrawableCulling(csmInt32 drawableIndex) const
{
    if (GetOverwriteFlagForModelCullings() || GetOverwriteFlagForDrawableCullings(drawableIndex))
    {
        return _userCullings[drawableIndex].IsCulling;
    }

    const Core::csmFlags* constantFlags = Core::csmGetDrawableConstantFlags(_model);
    return !IsBitSet(constantFlags[drawableIndex], Core::csmIsDoubleSided);
}

void CubismModel::SetDrawableCulling(csmInt32 drawableIndex, csmInt32 isCulling)
{
    _userCullings[drawableIndex].IsCulling = isCulling;
}

csmBool CubismModel::GetOverwriteFlagForModelCullings() const
{
    return _isOverwrittenCullings;
}

void CubismModel::SetOverwriteFlagForModelCullings(csmBool value)
{
    _isOverwrittenCullings = value;
}

csmBool CubismModel::GetOverwriteFlagForDrawableCullings(csmInt32 drawableIndex) const
{
    return _userCullings[drawableIndex].IsOverwritten;
}

void CubismModel::SetOverwriteFlagForDrawableCullings(csmUint32 drawableIndex, csmBool value)
{
    _userCullings[drawableIndex].IsOverwritten = value;
}

csmFloat32 CubismModel::GetModelOpacity()
{
    return _modelOpacity;
}

void CubismModel::SetModelOpacity(csmFloat32 value)
{
    _modelOpacity = value;
}

Core::csmModel* CubismModel::GetModel() const
{
    return _model;
}



csmBool CubismModel::IsUsingMasking() const
{
    for (csmInt32 d = 0; d < Core::csmGetDrawableCount(_model); ++d)
    {
        if (Core::csmGetDrawableMaskCounts(_model)[d] <= 0)
        {
            continue;
        }
        return true;
    }

    return false;
}

}}}
