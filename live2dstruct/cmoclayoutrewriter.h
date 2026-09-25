#ifndef CMOC_LAYOUT_REWRITER_H
#define CMOC_LAYOUT_REWRITER_H

#include <QByteArray>
#include <QMap>
#include <QString>

namespace live2dstruct {
class CMocLayoutRewriter
{
public:
    static bool Rewrite(const QByteArray& source, const QByteArray& candidate,
                        const QMap<int, qint64>& relocatedColumnByteSizes,
                        QByteArray& result, QString* error = nullptr);
};
}

#endif
