#ifndef CMOCRUNTIMEBUILDER_H
#define CMOCRUNTIMEBUILDER_H

#include <QByteArray>
#include <QHash>
#include <QMap>
#include <QString>
#include <QVector>

namespace live2dstruct {
class CMocRuntimeBuilder
{
public:
    struct Overrides {
        QMap<QString, bool> visibility;
        QHash<QString, float> drawableOpacity;
        QHash<QString, int> drawableDrawOrder;
        QHash<QString, QVector<quint16>> drawableIndices;
    };

    static bool Build(const QByteArray& authoringMoc, const Overrides& overrides,
                      QByteArray& result, QString* error = nullptr);
};
}

#endif
