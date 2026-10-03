#ifndef CMOCBLENDSHAPEGEOMETRYBUILDER_H
#define CMOCBLENDSHAPEGEOMETRYBUILDER_H

#include <QByteArray>
#include <QString>
#include <QVector>

namespace live2dstruct {
class CMocBlendShapeGeometryBuilder
{
public:
    struct Key {
        QString meshId;
        int shapeIndex = -1;
        int bindingOffset = -1;
        int keyformOffset = -1;
    };
    struct Frame {
        Key key;
        QByteArray sourceDigest;
        int sourceKeyformIndex = -1;
        QVector<float> deltas;
    };
    struct Request {
        Frame before;
        QVector<float> deltas;
    };
    struct Result {
        QByteArray bytes;
        Frame frame;
    };
    static bool Capture(const QByteArray& bytes, const Key& key, Frame& result, QString* error = nullptr);
    static bool Build(const QByteArray& bytes, const Request& request, Result& result, QString* error = nullptr);
};
}
#endif
