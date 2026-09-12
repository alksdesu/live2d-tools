#include "cdraworder.h"
#include <algorithm>

namespace live2dstruct {

QVector<int> ResolveDrawableOrder(const QVector<CDrawableOrder>& drawables,
                                  const QHash<QString, int>& overrides,
                                  const QHash<QString, int>& groupIndices,
                                  const QVector<CDrawableOrderGroup>& groups)
{
    QVector<int> indices;
    QVector<int> effectiveOrders;
    QVector<int> groupOrders;
    indices.reserve(drawables.size());
    effectiveOrders.reserve(drawables.size());
    groupOrders.reserve(drawables.size());
    bool hasEdits = false;
    for (int i = 0; i < drawables.size(); ++i) {
        indices.append(i);
        const auto overrideIt = overrides.constFind(drawables[i].id);
        const int order = overrideIt == overrides.cend()
            ? drawables[i].drawOrder : qBound(0, overrideIt.value(), 1000);
        effectiveOrders.append(order);
        groupOrders.append(groupIndices.value(drawables[i].id, -1));
        hasEdits = hasEdits || order != drawables[i].drawOrder;
    }

    std::stable_sort(indices.begin(), indices.end(), [&](int left, int right) {
        if (hasEdits) {
            const int leftGroup = groupOrders[left];
            const int rightGroup = groupOrders[right];
            const bool leftGrouped = leftGroup >= 0 && leftGroup < groups.size();
            const bool rightGrouped = rightGroup >= 0 && rightGroup < groups.size();
            if (leftGrouped || rightGrouped) {
                const auto groupAnchor = [&](int group) {
                    return groups[group].minimumDrawOrder;
                };
                const int leftAnchor = leftGrouped ? groupAnchor(leftGroup) : effectiveOrders[left];
                const int rightAnchor = rightGrouped ? groupAnchor(rightGroup) : effectiveOrders[right];
                if (leftAnchor != rightAnchor) return leftAnchor < rightAnchor;
                if (leftGrouped != rightGrouped) return leftGrouped;
                if (leftGrouped && leftGroup != rightGroup) return leftGroup < rightGroup;
            }
            if (effectiveOrders[left] != effectiveOrders[right])
                return effectiveOrders[left] < effectiveOrders[right];
        }
        return drawables[left].renderOrder < drawables[right].renderOrder;
    });
    return indices;
}

QVector<CLayerOrderItem> BuildGroupedLayerOrder(const QVector<CModelPart>& parts,
                                              const QVector<CModelDrawable>& drawables,
                                              const QVector<int>& sortedIndices)
{
    QVector<int> parents;
    parents.reserve(parts.size());
    for (int part = 0; part < parts.size(); ++part) {
        const int parent = parts[part].parentIndex;
        parents.append(parent >= 0 && parent < parts.size() && parent != part ? parent : -1);
    }
    QVector<int> states(parts.size(), 0);
    for (int part = 0; part < parts.size(); ++part) {
        if (states[part] != 0) continue;
        QVector<int> path;
        int ancestor = part;
        while (ancestor >= 0 && states[ancestor] == 0) {
            states[ancestor] = 1;
            path.append(ancestor);
            ancestor = parents[ancestor];
        }
        if (ancestor >= 0 && states[ancestor] == 1) {
            int member = ancestor;
            do {
                const int parent = parents[member];
                parents[member] = -1;
                member = parent;
            } while (member != ancestor);
        }
        for (int member : path) states[member] = 2;
    }

    QVector<CLayerOrderItem> result;
    QVector<int> openParts;
    QSet<int> added;
    for (int drawable : sortedIndices) {
        if (drawable < 0 || drawable >= drawables.size() || added.contains(drawable)) continue;
        added.insert(drawable);
        QVector<int> path;
        int ancestor = drawables[drawable].parentPartIndex;
        while (ancestor >= 0 && ancestor < parts.size()) {
            path.append(ancestor);
            ancestor = parents[ancestor];
        }
        std::reverse(path.begin(), path.end());
        int common = 0;
        while (common < openParts.size() && common < path.size() && openParts[common] == path[common]) ++common;
        while (openParts.size() > common)
            result.append({CLayerOrderItem::GroupEnd, parts[openParts.takeLast()].id, -1});
        for (int i = common; i < path.size(); ++i) {
            openParts.append(path[i]);
            result.append({CLayerOrderItem::GroupStart, parts[path[i]].id, -1});
        }
        result.append({CLayerOrderItem::Layer, drawables[drawable].id, drawable});
    }
    while (!openParts.isEmpty())
        result.append({CLayerOrderItem::GroupEnd, parts[openParts.takeLast()].id, -1});
    return result;
}

}
