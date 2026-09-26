#ifndef CARTMESHGEOMETRYLAYOUT_H
#define CARTMESHGEOMETRYLAYOUT_H

#include <QSet>
#include <QStringList>

namespace live2dstruct {
class CSectionTable;

class CArtMeshGeometryLayout
{
public:
    enum class Pool { Uvs, Indices };
    static bool CheckIsolation(const CSectionTable& table, const QSet<int>& changed, Pool pool,
                               QStringList& conflicts, QString* error = nullptr);
};
}

#endif
