#ifndef CMODELSESSION_H
#define CMODELSESSION_H

#include <QMap>
#include <QHash>
#include <QByteArray>
#include <QPointF>
#include <QString>
#include <QVector>
#include "live2dstruct/cmodeldocument.h"

namespace live2dstruct {

struct CModelSession
{
    int version = 1;
    QString sourceMoc;
    QByteArray sourceDigest;
    QMap<QString, float> parameters;
    QHash<QString, CModelParameterMetadata> parameterMetadata;
    QMap<QString, bool> visibility;
    QMap<QString, bool> locked;
    QHash<QString, float> drawableOpacity;
    QHash<QString, int> drawableDrawOrder;
    QHash<QString, QVector<QVector<float>>> drawableKeyformPositions;
    QHash<QString, QVector<quint16>> drawableIndices;
    QHash<QString, QVector<int>> drawableMaskIndices;
    QHash<QString, int> deformerParentIndices;
    QHash<QString, QVector<CModelRotationKeyform>> deformerRotationKeyforms;
    QHash<QString, QVector<CModelWarpKeyform>> deformerWarpKeyforms;
    QHash<QString, bool> drawableMaskInverted;
    float viewScale = 1.0f;
    QPointF viewPosition;

    QByteArray ToJson() const;
    static bool FromJson(const QByteArray& data, CModelSession* session, QString* error = nullptr);
    static QByteArray ComputeSourceDigest(const QString& path);
    bool Save(const QString& path, QString* error = nullptr) const;
    static bool Load(const QString& path, CModelSession* session, QString* error = nullptr);
};

}

#endif
