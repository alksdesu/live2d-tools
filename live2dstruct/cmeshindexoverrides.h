#ifndef CMESHINDEXOVERRIDES_H
#define CMESHINDEXOVERRIDES_H

#include "cmeshtopology.h"
#include <QHash>

namespace live2dstruct {

class CMeshIndexOverrides
{
public:
    bool IsEmpty() const { return m_indices.isEmpty(); }
    bool Set(int drawableIndex, int vertexCount, const QVector<quint16>& indices)
    {
        if (drawableIndex < 0 || !CMeshTopology::IsValid(indices, vertexCount)) return false;
        const auto existing = m_indices.constFind(drawableIndex);
        if (existing != m_indices.cend() && existing.value() == indices) return true;
        m_indices.insert(drawableIndex, indices);
        ++m_revision;
        return true;
    }

    const QVector<quint16>* Find(int drawableIndex) const
    {
        const auto entry = m_indices.constFind(drawableIndex);
        return entry == m_indices.cend() ? nullptr : &entry.value();
    }

    bool Remove(int drawableIndex)
    {
        if (m_indices.remove(drawableIndex) == 0) return false;
        ++m_revision;
        return true;
    }

    void Clear()
    {
        if (m_indices.isEmpty()) return;
        m_indices.clear();
        ++m_revision;
    }

    quint64 Revision() const { return m_revision; }

private:
    QHash<int, QVector<quint16>> m_indices;
    quint64 m_revision = 0;
};

}

#endif
