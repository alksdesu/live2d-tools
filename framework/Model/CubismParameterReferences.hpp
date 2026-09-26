#ifndef CUBISMPARAMETERREFERENCES_HPP
#define CUBISMPARAMETERREFERENCES_HPP

#include <QByteArray>
#include <QMap>
#include <QMetaType>
#include <QSet>
#include <QString>

struct ParameterResourceReferences
{
    QByteArray sourceDigest;
    QMap<int, QString> suppressedParameters;
};

struct ParameterPhysicsReferences
{
    QString sourcePath;
    QByteArray sourceDigest;
    QMap<int, QString> suppressedInputs;
    QMap<int, QString> suppressedOutputs;
};

struct ParameterReferenceState
{
    QByteArray modelSettingsDigest;
    QMap<QString, ParameterResourceReferences> motions;
    QMap<QString, ParameterResourceReferences> expressions;
    ParameterPhysicsReferences physics;
    QSet<QString> eyeBlink;
    QSet<QString> lipSync;
    QSet<QString> breath;
    QSet<QString> mouseFollow;
};

struct ParameterReferenceCatalog
{
    QByteArray modelSettingsDigest;
    QMap<QString, QByteArray> motions;
    QMap<QString, QByteArray> expressions;
    QString physicsPath;
    QByteArray physicsDigest;
    QSet<QString> eyeBlink;
    QSet<QString> lipSync;
    QSet<QString> breath;
    QSet<QString> mouseFollow;
};

Q_DECLARE_METATYPE(ParameterResourceReferences)
Q_DECLARE_METATYPE(ParameterPhysicsReferences)
Q_DECLARE_METATYPE(ParameterReferenceState)
Q_DECLARE_METATYPE(ParameterReferenceCatalog)

#endif
