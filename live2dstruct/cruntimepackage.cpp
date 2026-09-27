#include "cruntimepackage.h"
#include "cdeformerdisplaynames.h"
#include "cartworkpackage.h"
#include "cmocdeformerorder.h"
#include "cmodeluserdata.h"
#include "cparameterreferenceeditor.h"
#include "cphysicsdocument.h"
#include "std/cexportcontrol.h"
#include "std/cmodeljson.h"
#include "std/cmodelresourcepath.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <cmath>
#include <exception>

namespace live2dstruct {
namespace {
using Package = CRuntimePackage;
using Editor = CParameterReferenceEditor;

bool fail(QString* error, const QString& reason)
{
    if (error) *error = reason;
    return false;
}

bool text(const QString& value) { return !value.isEmpty() && !value.contains(QChar::Null); }

QString owned(const QString& value) { return QString(value.constData(), value.size()); }
QByteArray owned(const QByteArray& value) { return QByteArray(value.constData(), value.size()); }
QSet<QString> owned(const QSet<QString>& values)
{
    QSet<QString> result;
    for (const auto& value : values) result.insert(owned(value));
    return result;
}

void freezeEndpoint(CGeometryObjectPairs::Endpoint& endpoint)
{
    endpoint.object.id = owned(endpoint.object.id);
    endpoint.triangles = QVector<quint16>(endpoint.triangles.cbegin(), endpoint.triangles.cend());
    for (auto& axis : endpoint.axes) {
        axis.parameterId = owned(axis.parameterId);
        axis.keys = QVector<float>(axis.keys.cbegin(), axis.keys.cend());
    }
}

bool validateRequestStorage(const Package::Request& request, QString* error)
{
    qint64 total = 0, metadata = 0;
    QMap<QByteArray, const QByteArray*> blobs;
    const auto reserve = [&](qint64 bytes) {
        if (bytes > request.maximumTotalBytes - metadata) return fail(error, QStringLiteral("模型包请求元数据超过预算"));
        metadata += bytes; return true;
    };
    const auto bytes = [&](const QByteArray& value) {
        if (value.size() > request.maximumFileBytes) return fail(error, QStringLiteral("模型包请求文件超过大小预算"));
        const auto digest = QCryptographicHash::hash(value, QCryptographicHash::Sha256);
        const auto previous = blobs.constFind(digest);
        if (previous != blobs.cend()) return **previous == value || fail(error, QStringLiteral("模型包快照摘要对应不同内容"));
        if (value.size() > request.maximumTotalBytes - total) return fail(error, QStringLiteral("模型包请求文件快照超过总预算"));
        total += value.size();
        blobs.insert(digest, &value); return true;
    };
    const auto string = [&](const QString& value) { return reserve(qint64(value.size()) * sizeof(QChar)); };
    const auto ids = [&](const QSet<QString>& values) { for (const auto& value : values) if (!string(value)) return false; return true; };
    if (!string(request.modelJsonPath) || !string(request.registeredUserDataPath) || !string(request.timelineGroup) ||
        !string(request.timelineSoundPath) || !bytes(request.mocBytes) || !bytes(request.loadedModelSettingsBytes)) return false;
    for (const auto* replacement : {&request.physics, &request.pose, &request.userData, &request.timeline})
        if (*replacement && !bytes(**replacement)) return false;
    if (request.expectedSourceDigests.size() > request.maximumFiles || request.parameterMetadata.size() > 100000 ||
        !CParameterResourceEdits::Validate(request.references, error)) return fail(error, QStringLiteral("模型包请求目录无效或超过数量限制"));
    for (auto it = request.expectedSourceDigests.cbegin(); it != request.expectedSourceDigests.cend(); ++it)
        if (it->size() != 32 || !string(it.key())) return fail(error, QStringLiteral("模型包来源摘要无效或超过预算"));
    const auto& references = request.references;
    if (!string(references.modelPath) || !bytes(references.modelSource) || !ids(references.modelSuppressedIds) ||
        !ids(references.breath) || !ids(references.mouseFollow)) return false;
    for (auto it = references.resources.cbegin(); it != references.resources.cend(); ++it)
        if (!string(it.key()) || !bytes(it->source) || !ids(it->suppressedIds)) return false;
    for (auto it = request.parameterMetadata.cbegin(); it != request.parameterMetadata.cend(); ++it)
        if (!string(it.key()) || !string(it->displayName)) return false;
    if (request.deformerDisplayNames) {
        if (!CDeformerDisplayNames::Validate(*request.deformerDisplayNames, error)) return false;
        for (auto it = request.deformerDisplayNames->cbegin(); it != request.deformerDisplayNames->cend(); ++it)
            if (!string(it.key()) || !string(it.value())) return false;
    }
    CParameterOrganization::State organization;
    CGeometryObjectPairs::State pairs;
    if (!CParameterOrganization::Normalize(request.parameterOrganization, organization, error) ||
        !CGeometryObjectPairs::Normalize(request.geometryObjectPairs, pairs, error)) return false;
    for (const auto& node : organization.nodes)
        if (!string(node.id) || !string(node.parentFolder) || !string(node.name) || !string(node.color)) return false;
    for (const auto& pair : organization.pairs) if (!string(pair.first) || !string(pair.second)) return false;
    for (const auto& pair : pairs.pairs) {
        if (!string(pair.id) || !reserve(qint64(pair.firstVertexToSecondVertex.size()) * sizeof(int))) return false;
        for (const auto* endpoint : {&pair.first, &pair.second}) {
            if (!string(endpoint->object.id) || !reserve(qint64(endpoint->triangles.size()) * sizeof(quint16))) return false;
            for (const auto& axis : endpoint->axes)
                if (!string(axis.parameterId) || !reserve(qint64(axis.keys.size()) * sizeof(float))) return false;
        }
        for (const auto& axis : pair.axes) if (!reserve(qint64(axis.firstKeyToSecondKey.size()) * sizeof(int))) return false;
    }
    if (request.textureSources) {
        if (!CTextureAssets::ValidateCatalog(*request.textureSources, error)) return false;
        for (const auto& source : *request.textureSources) if (!string(source.path) || !bytes(source.png)) return false;
    }
    for (const auto& png : request.textureState) if (!bytes(png)) return false;
    if (request.textureWorkspace) {
        if (!CTextureWorkspace::Validate(*request.textureSources, *request.textureWorkspace, error)) return false;
        for (const auto& atlas : request.textureWorkspace->atlases)
            if (!string(atlas.id) || !string(atlas.name) || !bytes(atlas.png)) return false;
    }
    if (request.artwork) {
        CModelArtwork::Usage usage;
        if (!CModelArtwork::Validate(*request.artwork, &usage, error) || !reserve(usage.metadataBytes)) return false;
        for (const auto& image : request.artwork->images) if (!bytes(image.png)) return false;
        for (const auto& revision : request.artwork->revisions)
            if (!bytes(revision.sourceBytes) || !bytes(revision.iccProfile)) return false;
    }
    return true;
}

bool freezeRequest(const Package::Request& input, Package::Request& result, QString* error)
{
    if (!validateRequestStorage(input, error)) return false;
    Package::Request candidate = input;
    candidate.modelJsonPath = owned(input.modelJsonPath);
    candidate.modelSettingsDigest = owned(input.modelSettingsDigest);
    candidate.sourceMocDigest = owned(input.sourceMocDigest);
    candidate.mocBytes = owned(input.mocBytes);
    candidate.loadedModelSettingsBytes = owned(input.loadedModelSettingsBytes);
    candidate.registeredUserDataPath = owned(input.registeredUserDataPath);
    candidate.timelineGroup = owned(input.timelineGroup);
    candidate.timelineSoundPath = owned(input.timelineSoundPath);
    if (input.physics) candidate.physics = owned(*input.physics);
    if (input.pose) candidate.pose = owned(*input.pose);
    if (input.userData) candidate.userData = owned(*input.userData);
    if (input.timeline) candidate.timeline = owned(*input.timeline);
    candidate.expectedSourceDigests.clear();
    for (auto it = input.expectedSourceDigests.cbegin(); it != input.expectedSourceDigests.cend(); ++it)
        candidate.expectedSourceDigests.insert(owned(it.key()), owned(it.value()));
    candidate.references.modelPath = owned(input.references.modelPath);
    candidate.references.modelSource = owned(input.references.modelSource);
    candidate.references.modelSuppressedIds = owned(input.references.modelSuppressedIds);
    candidate.references.breath = owned(input.references.breath);
    candidate.references.mouseFollow = owned(input.references.mouseFollow);
    candidate.references.resources.clear();
    for (auto it = input.references.resources.cbegin(); it != input.references.resources.cend(); ++it)
        candidate.references.resources.insert(owned(it.key()), {it->kind, owned(it->source), owned(it->suppressedIds)});
    candidate.parameterMetadata.clear();
    for (auto it = input.parameterMetadata.cbegin(); it != input.parameterMetadata.cend(); ++it) {
        auto metadata = it.value(); metadata.displayName = owned(metadata.displayName);
        candidate.parameterMetadata.insert(owned(it.key()), metadata);
    }
    if (input.deformerDisplayNames) {
        candidate.deformerDisplayNames.emplace();
        for (auto name = input.deformerDisplayNames->cbegin(); name != input.deformerDisplayNames->cend(); ++name)
            candidate.deformerDisplayNames->insert(owned(name.key()), owned(name.value()));
    }
    for (auto& node : candidate.parameterOrganization.nodes) {
        node.id = owned(node.id); node.parentFolder = owned(node.parentFolder);
        node.name = owned(node.name); node.color = owned(node.color);
    }
    for (auto& pair : candidate.parameterOrganization.pairs) {
        pair.first = owned(pair.first); pair.second = owned(pair.second);
    }
    candidate.geometryObjectPairs.sourceDigest = owned(input.geometryObjectPairs.sourceDigest);
    for (auto& pair : candidate.geometryObjectPairs.pairs) {
        pair.id = owned(pair.id); freezeEndpoint(pair.first); freezeEndpoint(pair.second);
        pair.firstVertexToSecondVertex = QVector<int>(pair.firstVertexToSecondVertex.cbegin(), pair.firstVertexToSecondVertex.cend());
        for (auto& axis : pair.axes)
            axis.firstKeyToSecondKey = QVector<int>(axis.firstKeyToSecondKey.cbegin(), axis.firstKeyToSecondKey.cend());
    }
    if (input.textureSources) {
        if (!CTextureAssets::ValidateCatalog(*input.textureSources, error)) return false;
        QMap<QByteArray, QByteArray> blobs;
        for (auto& source : *candidate.textureSources) {
            source.path = owned(source.path); source.digest = owned(source.digest);
            const auto previous = blobs.constFind(source.digest);
            if (previous != blobs.cend() && *previous != source.png)
                return fail(error, QStringLiteral("纹理来源摘要对应不同内容"));
            if (previous == blobs.cend()) blobs.insert(source.digest, owned(source.png));
            source.png = blobs.value(source.digest);
        }
    }
    for (auto& bytes : candidate.textureState) bytes = owned(bytes);
    if (input.textureWorkspace && !CTextureWorkspace::Freeze(*candidate.textureSources, *input.textureWorkspace,
                                                            *candidate.textureWorkspace, error)) return false;
    if (input.artwork && !CModelArtwork::Freeze(*input.artwork, *candidate.artwork, error)) return false;
    result = std::move(candidate);
    return true;
}

bool object(const QByteArray& bytes, QJsonObject& result, QString* error)
{
    if (bytes.size() > 16 * 1024 * 1024) return fail(error, QStringLiteral("资源 JSON 大小超出限制"));
    QJsonParseError parse;
    const auto json = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !json.isObject()) return fail(error, QStringLiteral("资源 JSON 无效：%1").arg(parse.errorString()));
    result = json.object();
    return true;
}

bool array(const QJsonObject& root, const QString& key, QJsonArray& result, QString* error)
{
    if (!root.value(key).isArray()) return fail(error, QStringLiteral("资源字段必须是数组：%1").arg(key));
    result = root.value(key).toArray();
    if (result.size() > 100000) return fail(error, QStringLiteral("资源条目超出限制：%1").arg(key));
    return true;
}

bool reference(const QJsonValue& value, QString& result, QString* error, bool optional = false)
{
    if (optional && value.isUndefined()) { result.clear(); return true; }
    if (!value.isString() || value.toString().contains(QChar::Null) || (!optional && value.toString().isEmpty()))
        return fail(error, QStringLiteral("模型资源路径必须是有效字符串"));
    result = value.toString();
    return true;
}

QString suffix(const QString& path)
{
    const auto extension = QFileInfo(path).suffix().toLower();
    if (extension.isEmpty() || extension.size() > 16) return QStringLiteral(".bin");
    for (const auto character : extension)
        if (!((character >= QLatin1Char('a') && character <= QLatin1Char('z')) ||
              (character >= QLatin1Char('0') && character <= QLatin1Char('9')))) return QStringLiteral(".bin");
    return QLatin1Char('.') + extension;
}

class Collector
{
public:
    Collector(const Package::Request& request, Package::Prepared& result, CExportControl* control)
        : m_request(request), m_result(result), m_control(control), m_home(QFileInfo(request.modelJsonPath).absolutePath()) {}

    bool read(const QString& reference, QByteArray& bytes, QString* error)
    {
        const auto path = CModelResourcePath::Resolve(m_home, reference);
        if (path.isEmpty()) return fail(error, QStringLiteral("模型资源路径为空或无效"));
        const QFileInfo before(path);
        const auto canonical = before.canonicalFilePath();
        if (!before.isFile() || canonical.isEmpty()) return fail(error, QStringLiteral("模型资源不存在或不是文件：%1").arg(path));
        m_observed.insert(path, canonical);
        const auto found = m_sources.constFind(canonical);
        if (found != m_sources.cend()) { bytes = found->bytes; return expected(path, found->digest, error); }
        if (m_sources.size() + m_snapshots.size() >= m_request.maximumFiles || before.size() > m_request.maximumFileBytes ||
            before.size() > m_request.maximumTotalBytes - m_readBytes)
            return fail(error, QStringLiteral("模型资源数量或大小超出限制：%1").arg(path));
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return fail(error, QStringLiteral("无法读取模型资源 %1：%2").arg(path, file.errorString()));
        QByteArray data;
        while (!file.atEnd()) {
            if (!report(QStringLiteral("收集 %1").arg(before.fileName()), error)) return false;
            const auto part = file.read(256 * 1024);
            if (file.error() != QFileDevice::NoError || (part.isEmpty() && !file.atEnd()))
                return fail(error, QStringLiteral("读取模型资源失败：%1").arg(path));
            if (part.size() > m_request.maximumFileBytes - data.size() || part.size() > m_request.maximumTotalBytes - m_readBytes - data.size())
                return fail(error, QStringLiteral("模型资源读取大小超出限制：%1").arg(path));
            data.append(part);
        }
        const QFileInfo after(path);
        if (!after.isFile() || after.canonicalFilePath() != canonical || before.size() != data.size() || after.size() != data.size() ||
            before.lastModified() != after.lastModified() || before.metadataChangeTime() != after.metadataChangeTime())
            return fail(error, QStringLiteral("模型资源在读取时发生变化：%1").arg(path));
        const auto digest = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
        if (!expected(path, digest, error)) return false;
        m_readBytes += data.size();
        m_sources.insert(canonical, {data, digest, after.lastModified(), after.metadataChangeTime()});
        m_result.sources.append({path, digest, data.size()});
        m_result.protectedPaths.append(path);
        bytes.swap(data);
        return true;
    }

    bool textureSnapshot(const CTextureAssets::Source& source, QString* error)
    {
        const auto path = CModelResourcePath::Resolve(m_home, source.path);
        if (path.isEmpty() || !expected(path, source.digest, error)) return false;
        if (m_snapshots.contains(path)) return true;
        const auto observed = m_observed.constFind(path);
        if (observed != m_observed.cend() && m_sources.value(*observed).digest != source.digest)
            return fail(error, QStringLiteral("同一路径的资源快照不一致：%1").arg(path));
        if (m_sources.size() + m_snapshots.size() >= m_request.maximumFiles ||
            source.png.size() > m_request.maximumFileBytes || source.png.size() > m_request.maximumTotalBytes - m_readBytes)
            return fail(error, QStringLiteral("纹理源快照数量或大小超出限制：%1").arg(path));
        m_snapshots.insert(path, source.digest);
        m_readBytes += source.png.size();
        m_result.sources.append({path, source.digest, source.png.size()});
        m_result.protectedPaths.append(path);
        return report(QStringLiteral("收集纹理快照 %1").arg(QFileInfo(path).fileName()), error);
    }

    bool artworkSnapshots(QString* error)
    {
        if (!m_request.artwork) return true;
        for (const auto& revision : m_request.artwork->revisions) {
            if (revision.path.isEmpty()) continue;
            const auto path = CModelResourcePath::Resolve(m_home, revision.path);
            if (path.isEmpty()) return fail(error, QStringLiteral("作者来源路径无效"));
            auto& digests = m_artworkSnapshots[path];
            if (digests.contains(revision.sourceDigest)) continue;
            digests.insert(revision.sourceDigest);
            m_result.sources.append({path, revision.sourceDigest, revision.sourceBytes.size()});
            m_result.protectedPaths.append(path);
        }
        return report(QStringLiteral("收集作者来源快照"), error);
    }

    bool appendArtwork(const CArtworkPackage::Prepared& artwork, QString* error)
    {
        QSet<QString> paths;
        for (const auto& entry : m_result.entries) paths.insert(entry.relativePath.normalized(QString::NormalizationForm_C).toCaseFolded());
        for (const auto& entry : artwork.entries) {
            const auto path = entry.relativePath.normalized(QString::NormalizationForm_C).toCaseFolded();
            if (paths.contains(path)) return fail(error, QStringLiteral("作者附件与模型包文件名冲突：%1").arg(entry.relativePath));
            if (entry.bytes.size() > m_request.maximumFileBytes || entry.bytes.size() > m_request.maximumTotalBytes - m_result.totalBytes ||
                m_result.entries.size() >= m_request.maximumFiles) return fail(error, QStringLiteral("作者附件超过模型包大小或数量预算"));
            paths.insert(path); m_result.entries.append(entry); m_result.totalBytes += entry.bytes.size();
        }
        m_result.protectedPaths.append(artwork.protectedPaths);
        return true;
    }

    bool expected(const QString& path, const QByteArray& digest, QString* error) const
    {
        const auto snapshot = m_snapshots.constFind(path);
        if (snapshot != m_snapshots.cend() && *snapshot != digest)
            return fail(error, QStringLiteral("同一路径的资源快照不一致：%1").arg(path));
        for (auto it = m_request.expectedSourceDigests.cbegin(); it != m_request.expectedSourceDigests.cend(); ++it)
            if (CModelResourcePath::Resolve(m_home, it.key()) == path && (it->size() != 32 || *it != digest))
                return fail(error, QStringLiteral("模型资源与已加载版本不一致：%1").arg(path));
        return true;
    }

    bool effective(const QString& path, Editor::Kind kind, QByteArray& bytes, QString* error)
    {
        if (!read(path, bytes, error)) return false;
        const auto key = CModelResourcePath::Identity(m_home, path);
        const auto archived = m_request.references.resources.constFind(key);
        if (archived != m_request.references.resources.cend() && (archived->kind != kind || archived->source != bytes))
            return fail(error, QStringLiteral("模型资源与编辑时的来源不一致：%1").arg(path));
        Editor::Prepared prepared;
        if (!Editor::Prepare(kind, bytes, archived == m_request.references.resources.cend() ? QSet<QString>{} : archived->suppressedIds, prepared, error)) return false;
        bytes = prepared.effectiveBytes;
        return true;
    }

    bool add(const QString& category, const QString& extension, const QByteArray& bytes, QString& path, QString* error)
    {
        const auto identity = category.toUtf8() + '\0' + extension.toUtf8() + '\0' + QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        const auto previous = m_outputs.constFind(identity);
        if (previous != m_outputs.cend()) { path = *previous; return true; }
        if (bytes.size() > m_request.maximumFileBytes || bytes.size() > m_request.maximumTotalBytes - m_result.totalBytes ||
            m_result.entries.size() >= m_request.maximumFiles) return fail(error, QStringLiteral("导出模型包大小或数量超出限制"));
        const int index = m_counts.value(category);
        m_counts[category] = index + 1;
        path = category + '/' + QString::number(index).rightJustified(4, '0') + extension;
        m_outputs.insert(identity, path);
        m_result.entries.append({path, bytes});
        m_result.totalBytes += bytes.size();
        return report(QStringLiteral("准备 %1").arg(path), error);
    }

    bool finish(QString* error) const
    {
        if (!report(QStringLiteral("模型包准备完成"), error)) return false;
        for (auto it = m_request.expectedSourceDigests.cbegin(); it != m_request.expectedSourceDigests.cend(); ++it) {
            const auto path = CModelResourcePath::Resolve(m_home, it.key());
            if (path.isEmpty() || (!m_observed.contains(path) && !m_snapshots.contains(path) && !m_artworkSnapshots.value(path).contains(it.value())))
                return fail(error, QStringLiteral("已加载的资源未登记在模型包中：%1").arg(it.key()));
        }
        for (auto it = m_sources.cbegin(); it != m_sources.cend(); ++it) {
            const QFileInfo current(it.key());
            if (!current.isFile() || current.canonicalFilePath() != it.key() || current.size() != it->bytes.size() ||
                current.lastModified() != it->modified || current.metadataChangeTime() != it->changed)
                return fail(error, QStringLiteral("模型资源在准备期间发生变化：%1").arg(it.key()));
        }
        for (auto it = m_observed.cbegin(); it != m_observed.cend(); ++it)
            if (QFileInfo(it.key()).canonicalFilePath() != it.value())
                return fail(error, QStringLiteral("模型资源链接在准备期间发生变化：%1").arg(it.key()));
        return true;
    }

private:
    struct Captured { QByteArray bytes, digest; QDateTime modified, changed; };
    bool report(const QString& detail, QString* error) const
    {
        if (!m_control || m_control->Report(CExportControl::Phase::Preparing, m_readBytes, 0, detail)) return true;
        return fail(error, m_control->Error());
    }
    const Package::Request& m_request;
    Package::Prepared& m_result;
    CExportControl* m_control;
    QString m_home;
    QMap<QString, Captured> m_sources;
    QMap<QByteArray, QString> m_outputs;
    QMap<QString, int> m_counts;
    QMap<QString, QString> m_observed;
    QMap<QString, QByteArray> m_snapshots;
    QMap<QString, QSet<QByteArray>> m_artworkSnapshots;
    qint64 m_readBytes = 0;
};

bool displayInfo(const QByteArray& source, const CModelDocument& original, const CModelDocument& current,
                 const QHash<QString, CModelParameterMetadata>& metadata,
                 const CParameterOrganization::State& organization, const CGeometryObjectPairs::State& geometryPairs,
                 const std::optional<QMap<QString, QString>>& deformerNames, QByteArray& result, QString* error)
{
    QJsonObject root;
    if (!source.isEmpty() && !object(source, root, error)) return false;
    const auto originalRoot = root;
    const auto originalBytes = source;
    if (source.isEmpty()) root = {{QStringLiteral("Version"), 3}, {QStringLiteral("Parameters"), QJsonArray{}},
        {QStringLiteral("ParameterGroups"), QJsonArray{}}, {QStringLiteral("Parts"), QJsonArray{}}};
    QSet<QString> oldIds, currentIds, present;
    for (const auto& p : original.Parameters()) oldIds.insert(p.id);
    for (const auto& p : current.Parameters()) currentIds.insert(p.id);
    const auto removed = oldIds - currentIds;
    QJsonArray parameters, kept;
    if (root.contains(QStringLiteral("Parameters")) && !array(root, QStringLiteral("Parameters"), parameters, error)) return false;
    for (const auto& value : parameters) {
        if (!value.isObject()) return fail(error, QStringLiteral("显示信息参数不是对象"));
        auto row = value.toObject();
        const auto idValue = row.value(QStringLiteral("Id"));
        if (!idValue.isString() || !text(idValue.toString())) return fail(error, QStringLiteral("显示信息参数 ID 无效"));
        const auto id = idValue.toString();
        if (removed.contains(id)) continue;
        const auto name = metadata.value(id).displayName;
        if (!name.isEmpty() && name != id) row.insert(QStringLiteral("Name"), name);
        kept.append(row);
        present.insert(id);
    }
    for (const auto& parameter : current.Parameters()) if (!present.contains(parameter.id) &&
        (!oldIds.contains(parameter.id) || metadata.contains(parameter.id))) {
        const auto found = metadata.constFind(parameter.id);
        kept.append(QJsonObject{{QStringLiteral("Id"), parameter.id}, {QStringLiteral("GroupId"), QString()},
            {QStringLiteral("Name"), found == metadata.cend() || found->displayName.isEmpty() ? parameter.id : found->displayName}});
    }
    if (root.contains(QStringLiteral("Parameters")) || !kept.isEmpty()) root.insert(QStringLiteral("Parameters"), kept);
    if (organization.configured) {
        QMap<QString, QJsonObject> parameterRows, groupRows;
        QJsonArray orphanParameters, sourceGroups;
        QSet<QString> orphanGroups;
        for (const auto& value : kept) {
            const auto row = value.toObject();
            const auto id = row.value(QStringLiteral("Id")).toString();
            if (parameterRows.contains(id)) return fail(error, QStringLiteral("显示信息参数 ID 重复：%1").arg(id));
            parameterRows.insert(id, row);
            if (!currentIds.contains(id)) {
                const auto group = row.value(QStringLiteral("GroupId"));
                if (!group.isUndefined() && !group.isString()) return fail(error, QStringLiteral("孤立显示参数的文件夹 ID 无效：%1").arg(id));
                if (!group.toString().isEmpty()) orphanGroups.insert(group.toString());
                orphanParameters.append(row);
            }
        }
        if (root.contains(QStringLiteral("ParameterGroups")) &&
            !array(root, QStringLiteral("ParameterGroups"), sourceGroups, error)) return false;
        for (const auto& value : sourceGroups) {
            if (!value.isObject()) return fail(error, QStringLiteral("显示信息文件夹不是对象"));
            const auto row = value.toObject();
            const auto id = row.value(QStringLiteral("Id"));
            if (!id.isString() || !text(id.toString()) || groupRows.contains(id.toString()))
                return fail(error, QStringLiteral("显示信息文件夹 ID 无效或重复"));
            groupRows.insert(id.toString(), row);
        }
        QMap<QString, QVector<const CParameterOrganization::Node*>> children;
        for (const auto& node : organization.nodes)
            if (node.kind == CParameterOrganization::Node::Parameter && !node.parentFolder.isEmpty())
                children[node.parentFolder].append(&node);
        QJsonArray orderedParameters, orderedGroups;
        QSet<QString> configuredGroups;
        const auto addParameter = [&](const CParameterOrganization::Node& node) {
            auto row = parameterRows.value(node.id);
            row.insert(QStringLiteral("Id"), node.id);
            row.insert(QStringLiteral("GroupId"), node.parentFolder);
            if (!row.contains(QStringLiteral("Name"))) {
                const auto name = metadata.value(node.id).displayName;
                row.insert(QStringLiteral("Name"), name.isEmpty() ? node.id : name);
            }
            orderedParameters.append(row);
        };
        for (const auto& node : organization.nodes) {
            if (!node.parentFolder.isEmpty()) continue;
            if (node.kind == CParameterOrganization::Node::Parameter) addParameter(node);
            else {
                auto group = groupRows.value(node.id);
                group.insert(QStringLiteral("Id"), node.id);
                group.insert(QStringLiteral("Name"), node.name);
                orderedGroups.append(group);
                configuredGroups.insert(node.id);
                for (const auto* parameter : children.value(node.id)) addParameter(*parameter);
            }
        }
        for (const auto& value : orphanParameters) orderedParameters.append(value);
        for (const auto& value : sourceGroups) {
            const auto id = value.toObject().value(QStringLiteral("Id")).toString();
            if (orphanGroups.contains(id) && !configuredGroups.contains(id)) orderedGroups.append(value);
        }
        QJsonObject state;
        if (!CParameterOrganization::ToJson(organization, state, error)) return false;
        root.insert(QStringLiteral("Parameters"), orderedParameters);
        root.insert(QStringLiteral("ParameterGroups"), orderedGroups);
        root.insert(QStringLiteral("Live2DToolsParameterOrganization"),
            QJsonObject{{QStringLiteral("Version"), 1}, {QStringLiteral("State"), state}});
    } else root.remove(QStringLiteral("Live2DToolsParameterOrganization"));
    if (!geometryPairs.sourceDigest.isEmpty() || !geometryPairs.pairs.isEmpty()) {
        QJsonObject state;
        if (!CGeometryObjectPairs::ToJson(geometryPairs, state, error)) return false;
        root.insert(QStringLiteral("Live2DToolsGeometryObjectPairs"),
            QJsonObject{{QStringLiteral("Version"), 1}, {QStringLiteral("State"), state}});
    } else root.remove(QStringLiteral("Live2DToolsGeometryObjectPairs"));
    if (root.contains(QStringLiteral("CombinedParameters"))) {
        QJsonArray pairs, retained;
        if (!array(root, QStringLiteral("CombinedParameters"), pairs, error)) return false;
        for (const auto& value : pairs) {
            if (!value.isArray()) return fail(error, QStringLiteral("显示信息联动参数不是数组"));
            bool deleted = false;
            for (const auto& id : value.toArray()) {
                if (!id.isString() || !text(id.toString())) return fail(error, QStringLiteral("显示信息联动参数 ID 无效"));
                deleted = deleted || removed.contains(id.toString());
            }
            if (!deleted) retained.append(value);
        }
        root.insert(QStringLiteral("CombinedParameters"), retained);
    }
    if (deformerNames) {
        QJsonObject names;
        if (!CDeformerDisplayNames::ToJson(*deformerNames, names, error)) return false;
        root.insert(QStringLiteral("Live2DToolsDeformerNames"), QJsonObject{{QStringLiteral("Version"), 1}, {QStringLiteral("Names"), names}});
        if (root.contains(QStringLiteral("Deformers"))) {
            QJsonArray rows, retained;
            if (!array(root, QStringLiteral("Deformers"), rows, error)) return false;
            QSet<QString> currentDeformers, named;
            for (const auto& node : current.Deformers()) currentDeformers.insert(node.id);
            for (const auto& value : rows) {
                if (!value.isObject()) return fail(error, QStringLiteral("显示信息变形器不是对象"));
                auto row = value.toObject();
                const auto id = row.value(QStringLiteral("Id"));
                if (!id.isString() || !text(id.toString())) return fail(error, QStringLiteral("显示信息变形器 ID 无效"));
                if (!currentDeformers.contains(id.toString()) || named.contains(id.toString())) continue;
                row.insert(QStringLiteral("Name"), deformerNames->value(id.toString(), id.toString()));
                named.insert(id.toString()); retained.append(row);
            }
            for (auto name = deformerNames->cbegin(); name != deformerNames->cend(); ++name)
                if (!named.contains(name.key())) retained.append(QJsonObject{{QStringLiteral("Id"), name.key()}, {QStringLiteral("Name"), name.value()}});
            root.insert(QStringLiteral("Deformers"), retained);
        }
    }
    result = !originalBytes.isEmpty() && root == originalRoot ? originalBytes : QJsonDocument(root).toJson(QJsonDocument::Indented);
    return true;
}

bool auditParameters(Editor::Kind kind, const QByteArray& bytes, const QSet<QString>& removed, QString* error)
{
    Editor::Prepared audited;
    if (!Editor::Prepare(kind, bytes, removed, audited, error)) return false;
    return audited.references.isEmpty() || fail(error, QStringLiteral("导出资源仍引用已删除的模型参数"));
}

bool physics(QByteArray& bytes, const QSet<QString>& parameters, QString* error)
{
    QJsonObject root, normalized;
    if (!object(bytes, root, error) || !CPhysicsDocument::Normalize(root, normalized, error)) return false;
    for (const auto& value : normalized.value(QStringLiteral("PhysicsSettings")).toArray()) {
        const auto group = value.toObject();
        for (const auto& direction : {QStringLiteral("Input"), QStringLiteral("Output")})
            for (const auto& entry : group.value(direction).toArray()) {
                const auto key = direction == QLatin1String("Input") ? QStringLiteral("Source") : QStringLiteral("Destination");
                const auto id = entry.toObject().value(key).toObject().value(QStringLiteral("Id")).toString();
                if (!parameters.contains(id)) return fail(error, QStringLiteral("物理配置引用不存在的模型参数：%1").arg(id));
            }
    }
    if (normalized != root) bytes = QJsonDocument(normalized).toJson(QJsonDocument::Indented);
    return true;
}

bool pose(const QByteArray& bytes, const CModelDocument& model, QString* error)
{
    QJsonObject root;
    QJsonArray groups;
    if (!object(bytes, root, error) || !array(root, QStringLiteral("Groups"), groups, error)) return false;
    const auto fade = root.value(QStringLiteral("FadeInTime"));
    if (!fade.isUndefined() && (!fade.isDouble() || !std::isfinite(fade.toDouble()) || fade.toDouble() < 0))
        return fail(error, QStringLiteral("Pose 淡入时间无效"));
    QSet<QString> ids;
    for (const auto& part : model.Parts()) ids.insert(part.id);
    for (const auto& value : groups) {
        if (!value.isArray()) return fail(error, QStringLiteral("Pose 分组必须是数组"));
        for (const auto& entry : value.toArray()) {
            if (!entry.isObject()) return fail(error, QStringLiteral("Pose 条目必须是对象"));
            const auto item = entry.toObject();
            const auto id = item.value(QStringLiteral("Id"));
            if (!id.isString() || !text(id.toString()) || !ids.contains(id.toString()))
                return fail(error, QStringLiteral("Pose 引用不存在的 Part：%1").arg(id.toString()));
            if (!item.contains(QStringLiteral("Link"))) continue;
            QJsonArray links;
            if (!array(item, QStringLiteral("Link"), links, error)) return false;
            for (const auto& link : links)
                if (!link.isString() || !text(link.toString()) || !ids.contains(link.toString()))
                    return fail(error, QStringLiteral("Pose Link 引用不存在的 Part：%1").arg(link.toString()));
        }
    }
    return true;
}

bool motionSync(const QByteArray& bytes, const QSet<QString>& removed, QString* error)
{
    QJsonObject root;
    QJsonArray settings;
    if (!object(bytes, root, error) || !array(root, QStringLiteral("Settings"), settings, error)) return false;
    const auto check = [&](const QJsonValue& value) {
        if (!value.isObject()) return fail(error, QStringLiteral("MotionSync 参数条目无效"));
        const auto id = value.toObject().value(QStringLiteral("Id"));
        if (!id.isString() || !text(id.toString())) return fail(error, QStringLiteral("MotionSync 参数 ID 无效"));
        return !removed.contains(id.toString()) || fail(error, QStringLiteral("MotionSync 仍引用已删除的参数：%1，请先调整语音映射").arg(id.toString()));
    };
    for (const auto& value : settings) {
        if (!value.isObject()) return fail(error, QStringLiteral("MotionSync 设置不是对象"));
        QJsonArray parameters, mappings;
        const auto setting = value.toObject();
        if (!array(setting, QStringLiteral("CubismParameters"), parameters, error) ||
            !array(setting, QStringLiteral("Mappings"), mappings, error)) return false;
        for (const auto& parameter : parameters) if (!check(parameter)) return false;
        for (const auto& mapping : mappings) {
            if (!mapping.isObject()) return fail(error, QStringLiteral("MotionSync 映射不是对象"));
            QJsonArray targets;
            if (!array(mapping.toObject(), QStringLiteral("Targets"), targets, error)) return false;
            for (const auto& target : targets) if (!check(target)) return false;
        }
    }
    return true;
}

bool registeredUserData(const Package::Request& request, const QByteArray& current)
{
    if (!request.userData || request.registeredUserDataPath.isEmpty() || request.loadedModelSettingsBytes.isEmpty() ||
        QCryptographicHash::hash(request.loadedModelSettingsBytes, QCryptographicHash::Sha256) != request.modelSettingsDigest) return false;
    QString reference;
    if (!CModelJson::IsUserDataRegistration(request.loadedModelSettingsBytes, current, &reference)) return false;
    const auto home = QFileInfo(request.modelJsonPath).absolutePath();
    const auto path = CModelResourcePath::Resolve(home, reference);
    if (path.isEmpty() || path != CModelResourcePath::Resolve(home, request.registeredUserDataPath)) return false;
    bool loaded = false;
    for (auto it = request.expectedSourceDigests.cbegin(); it != request.expectedSourceDigests.cend(); ++it)
        if (CModelResourcePath::Resolve(home, it.key()) == path && it->size() == 32) loaded = true;
    return loaded;
}
}

bool CRuntimePackage::Prepare(const Request& input, Prepared& prepared, CExportControl* control, QString* error)
{
    if (error) error->clear();
    try {
        if (!text(input.modelJsonPath) || input.modelSettingsDigest.size() != 32 || input.sourceMocDigest.size() != 32 ||
            input.mocBytes.isEmpty() || input.maximumFiles < 1 || input.maximumFileBytes < 1 || input.maximumTotalBytes < 1)
            return fail(error, QStringLiteral("运行时模型包请求无效"));
        if ((!input.textureSources && (!input.textureState.isEmpty() || input.textureWorkspace)) ||
            (input.textureWorkspace && !input.textureState.isEmpty()) ||
            (input.artwork && (!input.textureSources || !input.textureWorkspace)))
            return fail(error, QStringLiteral("纹理覆盖需要完整的原始纹理快照"));
        if (control && control->IsCancellationRequested()) { control->Checkpoint(); return fail(error, control->Error()); }
        Request request;
        if (!freezeRequest(input, request, error)) return false;
        Prepared candidate;
        Collector collector(request, candidate, control);
        QByteArray sourceSettings;
        if (!collector.read(QFileInfo(request.modelJsonPath).absoluteFilePath(), sourceSettings, error) || !CModelJson::ValidateRuntimeSettings(sourceSettings, error)) return false;
        const bool userDataAdded = registeredUserData(request, sourceSettings);
        if (QCryptographicHash::hash(sourceSettings, QCryptographicHash::Sha256) != request.modelSettingsDigest && !userDataAdded)
            return fail(error, QStringLiteral("模型设置与已加载的来源不一致"));
        if (!request.references.IsEmpty() && ((request.references.modelSource != sourceSettings &&
            !CModelJson::IsUserDataRegistration(request.references.modelSource, sourceSettings)) ||
            request.references.modelPath != QDir::cleanPath(QFileInfo(request.modelJsonPath).absoluteFilePath())))
            return fail(error, QStringLiteral("模型包引用状态与当前模型设置不一致"));
        Editor::Prepared settings;
        if (!Editor::Prepare(Editor::Kind::ModelSettings, sourceSettings, request.references.modelSuppressedIds, settings, error)) return false;
        auto root = settings.effective;
        auto files = root.value(QStringLiteral("FileReferences")).toObject();
        static const QSet<QString> known{QStringLiteral("Moc"), QStringLiteral("Textures"), QStringLiteral("Physics"), QStringLiteral("Pose"),
            QStringLiteral("DisplayInfo"), QStringLiteral("UserData"), QStringLiteral("MotionSync"), QStringLiteral("Expressions"), QStringLiteral("Motions")};
        for (auto it = files.begin(); it != files.end(); ++it)
            if (!known.contains(it.key())) return fail(error, QStringLiteral("尚不能收集此扩展资源字段：%1").arg(it.key()));
        QString mocPath;
        QByteArray sourceMoc;
        if (!reference(files.value(QStringLiteral("Moc")), mocPath, error) || !collector.read(mocPath, sourceMoc, error)) return false;
        if (QCryptographicHash::hash(sourceMoc, QCryptographicHash::Sha256) != request.sourceMocDigest)
            return fail(error, QStringLiteral("源模型文件在打开后发生变化"));
        QByteArray runtimeMoc;
        if (!CMocDeformerOrder::Normalize(request.mocBytes, runtimeMoc, error)) return false;
        CModelDocument original, current;
        if (!original.LoadBytes(sourceMoc, mocPath, error) || !current.LoadBytes(runtimeMoc, mocPath, error)) return false;
        if (request.deformerDisplayNames) {
            QSet<QString> deformers;
            for (const auto& node : current.Deformers()) deformers.insert(node.id);
            for (auto name = request.deformerDisplayNames->cbegin(); name != request.deformerDisplayNames->cend(); ++name)
                if (!deformers.contains(name.key())) return fail(error, QStringLiteral("名称引用了不存在的变形器：%1").arg(name.key()));
            if (!current.SetDeformerDisplayNames(*request.deformerDisplayNames, error)) return false;
        }
        QSet<QString> oldIds, currentIds;
        for (const auto& p : original.Parameters()) oldIds.insert(p.id);
        for (const auto& p : current.Parameters()) currentIds.insert(p.id);
        if (!CParameterOrganization::Validate(request.parameterOrganization, currentIds, error)) return false;
        CGeometryObjectPairs::State geometryPairs;
        if (!CGeometryObjectPairs::Normalize(request.geometryObjectPairs, geometryPairs, error)) return false;
        if (!geometryPairs.sourceDigest.isEmpty()) {
            if (geometryPairs.sourceDigest != original.SourceDigest())
                return fail(error, QStringLiteral("几何配对与模型包的源模型不匹配"));
            geometryPairs.sourceDigest = current.SourceDigest();
        }
        if (!CGeometryObjectPairs::Validate(geometryPairs, current, error)) return false;
        const auto removed = oldIds - currentIds;
        if (!auditParameters(Editor::Kind::ModelSettings, QJsonDocument(root).toJson(QJsonDocument::Compact), removed, error)) return false;
        QString output;
        if (!collector.add(QStringLiteral("model"), QStringLiteral(".moc3"), runtimeMoc, output, error)) return false;
        files.insert(QStringLiteral("Moc"), output);
        QJsonArray textures, packagedTextures;
        if (files.contains(QStringLiteral("Textures")) && !array(files, QStringLiteral("Textures"), textures, error)) return false;
        if (request.textureSources) {
            if (textures.size() > CTextureAssets::MaximumSlots)
                return fail(error, QStringLiteral("模型声明的纹理槽数量超出限制"));
            int present = 0;
            const auto home = QFileInfo(request.modelJsonPath).absolutePath();
            for (int slot = 0; slot < textures.size(); ++slot) {
                QString path;
                if (!reference(textures[slot], path, error, true)) return false;
                const auto source = request.textureSources->constFind(slot);
                if (path.isEmpty()) {
                    if (source != request.textureSources->cend()) return fail(error, QStringLiteral("空纹理槽不能带有来源快照：%1").arg(slot));
                    continue;
                }
                ++present;
                if (source == request.textureSources->cend() ||
                    CModelResourcePath::Resolve(home, source->path) != CModelResourcePath::Resolve(home, path))
                    return fail(error, QStringLiteral("纹理槽 %1 的来源与模型声明不一致").arg(slot));
            }
            if (present != request.textureSources->size())
                return fail(error, QStringLiteral("纹理来源快照包含未声明的槽"));
            if (control && !control->Checkpoint()) return fail(error, control->Error());
            if (request.textureWorkspace) {
                if (!CTextureWorkspace::Validate(*request.textureSources, *request.textureWorkspace, error)) return false;
            } else if (!CTextureAssets::ValidateCatalog(*request.textureSources, error) ||
                       !CTextureAssets::ValidateState(*request.textureSources, request.textureState, error)) return false;
        }
        CTextureAssets::State atlasPngs;
        if (request.textureWorkspace) {
            if (!CTextureWorkspace::Effective(*request.textureSources, *request.textureWorkspace, atlasPngs, error)) return false;
            for (const auto& source : *request.textureSources) if (!collector.textureSnapshot(source, error)) return false;
        }
        const int textureCount = request.textureWorkspace ? (atlasPngs.isEmpty() ? 0 : atlasPngs.lastKey() + 1) : textures.size();
        for (int slot = 0; slot < textureCount; ++slot) {
            QString path;
            if (!request.textureWorkspace && !reference(textures[slot], path, error, true)) return false;
            if (request.textureWorkspace ? !atlasPngs.contains(slot) : path.isEmpty()) { packagedTextures.append(QString()); continue; }
            QByteArray data;
            if (request.textureWorkspace) data = atlasPngs.value(slot);
            else if (request.textureSources) {
                const auto& source = request.textureSources->value(slot);
                if (!collector.textureSnapshot(source, error)) return false;
                data = request.textureState.value(slot, source.png);
            } else if (!collector.read(path, data, error)) return false;
            if (!collector.add(QStringLiteral("textures"), request.textureSources ? QStringLiteral(".png") : suffix(path), data, output, error)) return false;
            packagedTextures.append(output);
        }
        for (const auto& drawable : current.Drawables()) {
            if (!drawable.vertexCount) continue;
            if (drawable.textureIndex < 0 || drawable.textureIndex >= packagedTextures.size() ||
                packagedTextures[drawable.textureIndex].toString().isEmpty())
                return fail(error, QStringLiteral("网格 %1 引用了没有图集的纹理槽 %2").arg(drawable.id).arg(drawable.textureIndex));
        }
        if (request.textureWorkspace || files.contains(QStringLiteral("Textures"))) files.insert(QStringLiteral("Textures"), packagedTextures);
        const auto scalar = [&](const QString& field, const QString& suffix, const std::optional<QByteArray>& replacement,
                                const std::optional<Editor::Kind>& kind) {
            QString path;
            if (!reference(files.value(field), path, error, true)) return false;
            if (path.isEmpty() && !replacement) return true;
            QByteArray data;
            if (!path.isEmpty() && !(kind ? collector.effective(path, *kind, data, error) : collector.read(path, data, error))) return false;
            if (replacement) data = *replacement;
            if (kind && !auditParameters(*kind, data, removed, error)) return false;
            if (field == QLatin1String("Physics") && !physics(data, currentIds, error)) return false;
            if (field == QLatin1String("Pose") && !pose(data, current, error)) return false;
            if (field == QLatin1String("MotionSync") && !motionSync(data, removed | request.references.modelSuppressedIds, error)) return false;
            if (field == QLatin1String("DisplayInfo") && !displayInfo(data, original, current, request.parameterMetadata,
                                                                    request.parameterOrganization, geometryPairs, request.deformerDisplayNames, data, error)) return false;
            if (field == QLatin1String("UserData")) {
                CModelUserData validated;
                if (!validated.FromJson(data, error)) return false;
                QSet<QString> meshes;
                for (const auto& mesh : current.Drawables()) meshes.insert(mesh.id);
                for (const auto& entry : validated.Entries())
                    if (!meshes.contains(entry.id)) return fail(error, QStringLiteral("UserData 引用不存在的网格：%1").arg(entry.id));
            } else if (!kind && field != QLatin1String("DisplayInfo")) {
                QJsonObject validated;
                if (!object(data, validated, error)) return false;
            }
            if (!collector.add(QStringLiteral("settings"), suffix, data, output, error)) return false;
            files.insert(field, output);
            return true;
        };
        const bool needsDisplay = request.parameterOrganization.configured || !geometryPairs.sourceDigest.isEmpty() ||
            !request.parameterMetadata.isEmpty() || request.deformerDisplayNames.has_value() || oldIds != currentIds;
        std::optional<QByteArray> display;
        if (files.value(QStringLiteral("DisplayInfo")).toString().isEmpty() && needsDisplay) display = QByteArray();
        if (!scalar(QStringLiteral("Physics"), QStringLiteral(".physics3.json"), request.physics, Editor::Kind::Physics) ||
            !scalar(QStringLiteral("Pose"), QStringLiteral(".pose3.json"), request.pose, {}) ||
            !scalar(QStringLiteral("UserData"), QStringLiteral(".userdata3.json"), request.userData, {}) ||
            !scalar(QStringLiteral("DisplayInfo"), QStringLiteral(".cdi3.json"), display, {}) ||
            !scalar(QStringLiteral("MotionSync"), QStringLiteral(".motionsync3.json"), {}, {})) return false;
        if (files.contains(QStringLiteral("Expressions"))) {
            QJsonArray expressions, updated;
            if (!array(files, QStringLiteral("Expressions"), expressions, error)) return false;
            for (const auto& value : expressions) {
                if (!value.isObject()) return fail(error, QStringLiteral("表情资源条目无效"));
                auto entry = value.toObject();
                QString path;
                QByteArray data;
                if (!reference(entry.value(QStringLiteral("File")), path, error, true)) return false;
                if (!path.isEmpty() && (!collector.effective(path, Editor::Kind::Expression, data, error) ||
                    !auditParameters(Editor::Kind::Expression, data, removed, error) ||
                    !collector.add(QStringLiteral("expressions"), QStringLiteral(".exp3.json"), data, output, error))) return false;
                if (!path.isEmpty()) entry.insert(QStringLiteral("File"), output);
                updated.append(entry);
            }
            files.insert(QStringLiteral("Expressions"), updated);
        }
        auto motions = files.value(QStringLiteral("Motions")).toObject();
        if (files.contains(QStringLiteral("Motions")) && !files.value(QStringLiteral("Motions")).isObject()) return fail(error, QStringLiteral("动作资源列表无效"));
        for (auto group = motions.begin(); group != motions.end(); ++group) {
            if (!group.value().isArray() || group.value().toArray().size() > 100000) return fail(error, QStringLiteral("动作分组无效"));
            QJsonArray entries;
            for (const auto& value : group.value().toArray()) {
                if (!value.isObject()) return fail(error, QStringLiteral("动作资源条目无效"));
                auto entry = value.toObject();
                QString path, sound;
                QByteArray data;
                if (!reference(entry.value(QStringLiteral("File")), path, error, true)) return false;
                if (!path.isEmpty() && (!collector.effective(path, Editor::Kind::Motion, data, error) || !auditParameters(Editor::Kind::Motion, data, removed, error) ||
                    !collector.add(QStringLiteral("motions"), QStringLiteral(".motion3.json"), data, output, error))) return false;
                if (!path.isEmpty()) entry.insert(QStringLiteral("File"), output);
                if (!reference(entry.value(QStringLiteral("Sound")), sound, error, true)) return false;
                if (!sound.isEmpty()) {
                    if (!collector.read(sound, data, error) || !collector.add(QStringLiteral("sounds"), suffix(sound), data, output, error)) return false;
                    entry.insert(QStringLiteral("Sound"), output);
                }
                entries.append(entry);
            }
            group.value() = entries;
        }
        if (request.timeline) {
            if (!text(request.timelineGroup) || !auditParameters(Editor::Kind::Motion, *request.timeline, removed, error)) return false;
            if (!collector.add(QStringLiteral("motions"), QStringLiteral(".motion3.json"), *request.timeline, output, error)) return false;
            QJsonObject entry{{QStringLiteral("File"), output}};
            if (!request.timelineSoundPath.isEmpty()) {
                QByteArray sound;
                if (!collector.read(request.timelineSoundPath, sound, error) ||
                    !collector.add(QStringLiteral("sounds"), suffix(request.timelineSoundPath), sound, output, error)) return false;
                entry.insert(QStringLiteral("Sound"), output);
            }
            QString group = request.timelineGroup;
            for (int suffix = 2; motions.contains(group); ++suffix) group = request.timelineGroup + QStringLiteral("_%1").arg(suffix);
            motions.insert(group, QJsonArray{entry});
        }
        if (files.contains(QStringLiteral("Motions")) || request.timeline) files.insert(QStringLiteral("Motions"), motions);
        root.insert(QStringLiteral("FileReferences"), files);
        candidate.model = root;
        const auto modelBytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
        if (modelBytes.size() > request.maximumFileBytes || modelBytes.size() > request.maximumTotalBytes - candidate.totalBytes || candidate.entries.size() >= request.maximumFiles)
            return fail(error, QStringLiteral("模型包总大小或文件数量超出限制"));
        candidate.entries.append({candidate.modelFile, modelBytes});
        candidate.totalBytes += modelBytes.size();
        if (request.artwork) {
            CArtworkPackage::Request attachment;
            attachment.modelFile = candidate.modelFile; attachment.modelSettingsBytes = modelBytes; attachment.mocBytes = runtimeMoc;
            attachment.sources = *request.textureSources; attachment.state.workspace = *request.textureWorkspace;
            attachment.state.artwork = *request.artwork;
            attachment.limits.maximumFileBytes = request.maximumFileBytes;
            attachment.limits.maximumTotalBytes = request.maximumTotalBytes - candidate.totalBytes;
            attachment.limits.maximumFiles = request.maximumFiles - candidate.entries.size();
            CArtworkPackage::Prepared artwork;
            if (!current.CaptureDrawableMaterials(attachment.state.materials, error) || !collector.artworkSnapshots(error) ||
                !CArtworkPackage::Prepare(attachment, artwork, control, error) || !collector.appendArtwork(artwork, error)) return false;
        }
        candidate.protectedPaths.append(QFileInfo(request.modelJsonPath).absolutePath());
        if (!request.references.breath.isEmpty() || !request.references.mouseFollow.isEmpty())
            candidate.notes.append(QStringLiteral("呼吸、鼠标跟随及播放状态由使用模型的应用控制"));
        if (!collector.finish(error)) return false;
        candidate.protectedPaths.removeDuplicates();
        prepared = std::move(candidate);
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法准备运行时模型包")); }
}
}
