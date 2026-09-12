#ifndef CMODELSESSION_H
#define CMODELSESSION_H

#include <QMap>
#include <QHash>
#include <QByteArray>
#include <QPointF>
#include <QString>

namespace live2dstruct {

struct CModelSession
{
    int version = 1;
    QString sourceMoc;
    QMap<QString, float> parameters;
    QMap<QString, bool> visibility;
    QHash<QString, float> drawableOpacity;
    QHash<QString, int> drawableDrawOrder;
    float viewScale = 1.0f;
    QPointF viewPosition;

    QByteArray ToJson() const;
    static bool FromJson(const QByteArray& data, CModelSession* session, QString* error = nullptr);
    bool Save(const QString& path, QString* error = nullptr) const;
    static bool Load(const QString& path, CModelSession* session, QString* error = nullptr);
};

}

#endif
