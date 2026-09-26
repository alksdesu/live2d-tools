#include "clinearinterpolation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
using namespace live2dstruct;

struct WeightedKeyform
{
    size_t index;
    double weight;
};

bool weightsFor(C2DTreeNode& node, size_t formCount, std::vector<WeightedKeyform>& output)
{
    if (formCount == 0 || formCount > 1000000) return false;
    size_t stride = 1;
    std::vector<WeightedKeyform> weights{{0, 1.0}};
    for (C2DTreeNode* father : node.getFathers()) {
        auto* parameter = dynamic_cast<CParameterNode*>(father);
        if (!parameter || parameter->GetType() != 0) continue;
        const auto& keys = parameter->GetKeys();
        const float value = parameter->GetCurValue();
        if (!std::isfinite(value) || keys.empty() || keys.size() > formCount / stride) return false;
        for (size_t index = 0; index < keys.size(); ++index)
            if (!std::isfinite(keys[index]) || (index > 0 && keys[index] <= keys[index - 1])) return false;
        if (value < keys.front() || value > keys.back()) return false;
        std::vector<WeightedKeyform> choices;
        const auto upper = std::lower_bound(keys.cbegin(), keys.cend(), value);
        if (*upper == value)
            choices.push_back({static_cast<size_t>(upper - keys.cbegin()), 1.0});
        else {
            const size_t high = static_cast<size_t>(upper - keys.cbegin());
            const double t = (static_cast<double>(value) - keys[high - 1]) /
                             (static_cast<double>(keys[high]) - keys[high - 1]);
            if (t < 1.0) choices.push_back({high - 1, 1.0 - t});
            if (t > 0.0) choices.push_back({high, t});
        }
        std::vector<WeightedKeyform> expanded;
        expanded.reserve(weights.size() * choices.size());
        for (const auto& choice : choices)
            for (const auto& existing : weights) {
                const double weight = choice.weight * existing.weight;
                if (weight > 0.0) expanded.push_back({existing.index + choice.index * stride, weight});
            }
        weights = std::move(expanded);
        stride *= keys.size();
    }
    if (stride != formCount || weights.empty()) return false;
    output = std::move(weights);
    return true;
}

template <typename Value>
bool interpolateValue(const std::vector<WeightedKeyform>& weights, const Value& source, float& output)
{
    double value = 0.0;
    double minimum = std::numeric_limits<double>::max();
    double maximum = -minimum;
    for (const auto& entry : weights) {
        const double component = source(entry.index);
        if (!std::isfinite(component)) return false;
        value += entry.weight * component;
        minimum = std::min(minimum, component);
        maximum = std::max(maximum, component);
    }
    if (!std::isfinite(value)) return false;
    value = std::clamp(value, minimum, maximum);
    if (std::abs(value) > std::numeric_limits<float>::max()) return false;
    output = static_cast<float>(value);
    return true;
}

template <typename Keyform>
bool interpolatePositions(std::vector<Keyform>& forms, const std::vector<WeightedKeyform>& weights,
                          std::vector<float>& positions)
{
    const size_t count = forms[weights.front().index].GetPosition().size();
    if (count == 0 || count > 2000000 || count % 2 != 0) return false;
    for (const auto& entry : weights)
        if (forms[entry.index].GetPosition().size() != count) return false;
    positions.resize(count);
    for (size_t coordinate = 0; coordinate < count; ++coordinate)
        if (!interpolateValue(weights, [&](size_t index) { return forms[index].GetPosition()[coordinate]; },
                              positions[coordinate])) return false;
    return true;
}

bool interpolateRotation(CRotationNode& node)
{
    auto& forms = node.GetKeyFormS();
    std::vector<WeightedKeyform> weights;
    std::vector<float> positions;
    if (!weightsFor(node, forms.size(), weights) || !interpolatePositions(forms, weights, positions) ||
        positions.size() != 2) return false;
    const auto first = weights.front().index;
    const bool reflectX = forms[first].Getis_reflect_x();
    const bool reflectY = forms[first].Getis_reflect_y();
    for (const auto& entry : weights)
        if (forms[entry.index].Getis_reflect_x() != reflectX || forms[entry.index].Getis_reflect_y() != reflectY) return false;
    float opacity, angle, scale;
    if (!interpolateValue(weights, [&](size_t index) { return forms[index].Getopacities(); }, opacity) ||
        !interpolateValue(weights, [&](size_t index) { return forms[index].Getangles(); }, angle) ||
        !interpolateValue(weights, [&](size_t index) { return forms[index].Getscales(); }, scale)) return false;
    if (weights.size() == 1) node.SetCurKeyForm(forms[first]);
    else {
        CRotationNodeKeyform candidate(opacity, angle, positions[0], positions[1], scale, reflectX, reflectY);
        node.SetCurKeyForm(candidate);
    }
    return true;
}

bool interpolateWarp(CWarpNode& node)
{
    auto& forms = node.GetKeyFormS();
    std::vector<WeightedKeyform> weights;
    std::vector<float> positions;
    float opacity;
    if (!weightsFor(node, forms.size(), weights) || !interpolatePositions(forms, weights, positions) ||
        !interpolateValue(weights, [&](size_t index) { return forms[index].GetOpacities(); }, opacity)) return false;
    if (weights.size() == 1) node.SetCurKeyForm(forms[weights.front().index]);
    else {
        CWarpNodeKeyform candidate(opacity, positions);
        node.SetCurKeyForm(candidate);
    }
    return true;
}

bool interpolateMesh(CArtMeshNode& node)
{
    auto& forms = node.GetKeyFormS();
    std::vector<WeightedKeyform> weights;
    std::vector<float> positions;
    float opacity, drawOrder;
    if (!weightsFor(node, forms.size(), weights) || !interpolatePositions(forms, weights, positions) ||
        !interpolateValue(weights, [&](size_t index) { return forms[index].getOpacities(); }, opacity) ||
        !interpolateValue(weights, [&](size_t index) { return forms[index].getDraw_orders(); }, drawOrder)) return false;
    if (weights.size() == 1) node.SetCurMeshKey(forms[weights.front().index]);
    else {
        CArtMeshKeyForm candidate(opacity, drawOrder, positions);
        node.SetCurMeshKey(candidate);
    }
    return true;
}
}

namespace live2dstruct {

CLinearInterpolation::CLinearInterpolation() = default;

bool CLinearInterpolation::SetCKeyFormObject(C2DTreeNode* node)
{
    if (auto* rotation = dynamic_cast<CRotationNode*>(node)) return interpolateRotation(*rotation);
    if (auto* warp = dynamic_cast<CWarpNode*>(node)) return interpolateWarp(*warp);
    if (auto* mesh = dynamic_cast<CArtMeshNode*>(node)) return interpolateMesh(*mesh);
    return false;
}

}
