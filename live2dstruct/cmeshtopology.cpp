#include "cmeshtopology.h"

#include <algorithm>
#include <utility>

namespace live2dstruct {

bool CMeshTopology::IsValid(const QVector<quint16>& indices, int vertexCount)
{
    if (indices.isEmpty() || indices.size() % 3 != 0 || indices.size() > 3000000) return false;
    for (quint16 index : indices)
        if (index >= vertexCount || index > 32767) return false;
    return true;
}

bool CMeshTopology::CleanTriangles(const QVector<quint16>& indices, int vertexCount,
                                   QVector<quint16>* output)
{
    if (!output || !IsValid(indices, vertexCount)) return false;
    QVector<quint16> result;
    result.reserve(indices.size());
    QSet<quint64> seen;
    seen.reserve(indices.size() / 3);
    for (qsizetype i = 0; i < indices.size(); i += 3) {
        const quint16 triangle[]{indices[i], indices[i + 1], indices[i + 2]};
        if (triangle[0] == triangle[1] || triangle[1] == triangle[2] || triangle[2] == triangle[0]) continue;
        const int first = std::min_element(triangle, triangle + 3) - triangle;
        const quint64 key = (static_cast<quint64>(triangle[first]) << 32) |
                            (static_cast<quint64>(triangle[(first + 1) % 3]) << 16) |
                            triangle[(first + 2) % 3];
        if (seen.contains(key)) continue;
        seen.insert(key);
        for (quint16 index : triangle) result.append(index);
    }
    if (result.isEmpty()) return false;
    *output = std::move(result);
    return true;
}

bool CMeshTopology::MergeVertices(const QVector<quint16>& indices, const QSet<int>& selected,
                                  int vertexCount, QVector<quint16>* output)
{
    if (!output || selected.size() < 2 || !IsValid(indices, vertexCount)) return false;
    for (int index : selected)
        if (index < 0 || index >= vertexCount || index > 32767) return false;
    const quint16 target = static_cast<quint16>(*std::min_element(selected.cbegin(), selected.cend()));
    QVector<quint16> remapped = indices;
    for (quint16& index : remapped)
        if (selected.contains(index)) index = target;
    return CleanTriangles(remapped, vertexCount, output);
}

}
