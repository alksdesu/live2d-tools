#ifndef CMESHTOPOLOGY_H
#define CMESHTOPOLOGY_H

#include <QSet>
#include <QVector>
#include <QtGlobal>
#include <limits>

namespace live2dstruct {

class CMeshTopology
{
public:
    static constexpr int MaximumIndex = std::numeric_limits<quint16>::max();
    static constexpr int IndexCapacity = MaximumIndex + 1;
    static bool IsValid(const QVector<quint16>& indices, int vertexCount);
    static bool CleanTriangles(const QVector<quint16>& indices, int vertexCount,
                               QVector<quint16>* output);
    static bool MergeVertices(const QVector<quint16>& indices, const QSet<int>& selected,
                              int vertexCount, QVector<quint16>* output);
};

}

#endif
