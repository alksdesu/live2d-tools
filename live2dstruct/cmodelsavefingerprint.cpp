#include "cmodelsavefingerprint.h"
#include "cmodeldocument.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QJsonDocument>
#include <QIODevice>
#include <algorithm>
#include <exception>

namespace live2dstruct {
bool CModelSaveFingerprint::Capture(const CModelDocument& document, QByteArray& result, QString* error) try
{
    if (error) error->clear();
    QByteArray runtime;
    QJsonObject organization, pairs;
    if (!document.IsLoaded()) {
        if (error) *error = QStringLiteral("尚未载入模型，无法计算保存状态");
        return false;
    }
    if (!document.BuildRuntimeMoc(runtime, error) ||
        !CParameterOrganization::ToJson(document.ParameterOrganization(), organization, error) ||
        !CGeometryObjectPairs::ToJson(document.GeometryObjectPairs(), pairs, error)) return false;
    QByteArray encoded;
    QDataStream stream(&encoded, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream << document.SourcePath() << document.SourceDigest()
           << QCryptographicHash::hash(runtime, QCryptographicHash::Sha256);
    const auto& metadata = document.ParameterMetadataOverrides();
    const auto& values = document.ParameterValues();
    stream << qint64(document.Parameters().size());
    for (const auto& parameter : document.Parameters()) {
        const auto override = metadata.constFind(parameter.id);
        const CModelParameterMetadata effective = override == metadata.cend()
            ? CModelParameterMetadata{parameter.id, parameter.minimum, parameter.maximum, parameter.defaultValue,
                                      parameter.decimalPlaces, parameter.repeat} : *override;
        stream << parameter.id << values.value(parameter.id, effective.defaultValue)
               << (effective.displayName.isEmpty() ? parameter.id : effective.displayName)
               << effective.minimum << effective.maximum << effective.defaultValue << effective.decimalPlaces << effective.repeat;
    }
    // Visibility baking can conceal an authored opacity change.
    const auto& opacity = document.DrawableOpacityOverrides();
    stream << qint64(document.Drawables().size());
    for (const auto& mesh : document.Drawables()) {
        stream << mesh.id << qint64(mesh.keyforms.size());
        for (const auto& form : mesh.keyforms) stream << opacity.value(mesh.id, form.opacity);
    }
    auto locks = document.LockedKeys().values();
    std::sort(locks.begin(), locks.end());
    auto sourceDeformers = document.HasDeformerStructure() ? document.SourceDeformerIds().values() : QStringList{};
    std::sort(sourceDeformers.begin(), sourceDeformers.end());
    stream << document.VisibilityState() << locks << document.HasMeshStructure() << document.HasDeformerStructure() << document.HasMaterialStructure()
           << sourceDeformers << QJsonDocument(organization).toJson(QJsonDocument::Compact)
           << QJsonDocument(pairs).toJson(QJsonDocument::Compact);
    stream << document.TextureWorkspaceFingerprint() << document.ArtworkFingerprint();
    stream << document.DeformerDisplayNames();
    if (stream.status() != QDataStream::Ok) {
        if (error) *error = QStringLiteral("无法计算模型保存状态");
        return false;
    }
    result = QCryptographicHash::hash(encoded, QCryptographicHash::Sha256);
    return true;
}
catch (const std::exception&) {
    if (error) *error = QStringLiteral("无法计算模型保存状态");
    return false;
}
}
