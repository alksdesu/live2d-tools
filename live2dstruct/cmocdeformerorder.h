#ifndef CMOCDEFORMERORDER_H
#define CMOCDEFORMERORDER_H

#include <QByteArray>
#include <QString>

namespace live2dstruct {
class CMocDeformerOrder
{
public:
    static bool Normalize(const QByteArray& runtimeMoc, QByteArray& result, QString* error = nullptr);
};
}

#endif
