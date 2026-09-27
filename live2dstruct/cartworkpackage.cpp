#include "cartworkpackage.h"
#include "cartworkreconciler.h"
#include "cmaterialresourcebudget.h"
#include "std/cexportcontrol.h"
#include "std/cmodeljson.h"
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
using Package = CArtworkPackage;
using Artwork = CModelArtwork;
bool fail(QString* error, const QString& reason) { if (error) *error = reason; return false; }
QByteArray hash(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
QByteArray owned(const QByteArray& value) { return QByteArray(value.constData(), value.size()); }
QString owned(const QString& value) { return QString(value.constData(), value.size()); }
bool checkpoint(CExportControl* control, QString* error)
{ return !control || control->Checkpoint() || fail(error, control->Error()); }
bool limitsValid(const Package::Limits& limits)
{ return limits.maximumFileBytes > 0 && limits.maximumTotalBytes > 0 && limits.maximumIndexBytes > 0 && limits.maximumFiles > 0; }
bool fields(const QJsonObject& object, const QSet<QString>& names)
{ const auto keys = object.keys(); return QSet<QString>(keys.cbegin(), keys.cend()) == names; }
bool number(const QJsonValue& value, qint64 minimum, qint64 maximum, qint64& result)
{
    const double n = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(n) || std::floor(n) != n || n < double(minimum) || n > double(maximum) || n >= 9223372036854775808.0) return false;
    result = qint64(n); return true;
}
bool digest(const QJsonValue& value, QByteArray& result)
{
    if (!value.isString()) return false;
    const auto text = value.toString().toLatin1(), decoded = QByteArray::fromHex(text);
    if (text.size() != 64 || decoded.size() != 32 || decoded.toHex() != text) return false;
    result = decoded; return true;
}
bool relative(const QString& path)
{
    if (path.isEmpty() || path.toUtf8().size() > 4096 || QDir::isAbsolutePath(path) || path.contains('\\') || path.contains(QChar::Null)) return false;
    for (const auto& part : path.split('/')) {
        if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') || part.endsWith(' ')) return false;
        for (auto ch : part) if (ch.unicode() < 32 || QStringLiteral(":*?\"<>|").contains(ch)) return false;
        const auto stem = part.section('.', 0, 0).toUpper();
        if (QSet<QString>{"CON", "PRN", "AUX", "NUL"}.contains(stem) ||
            (stem.size() == 4 && (stem.startsWith("COM") || stem.startsWith("LPT")) && stem[3] >= '0' && stem[3] <= '9')) return false;
    }
    return true;
}
class Paths
{
public:
    bool add(const QString& path, const QByteArray& digest, bool shared, QString* error) {
        QString prefix; const auto components = path.split('/');
        for (int index = 0; index < components.size(); ++index) {
            if (!prefix.isEmpty()) prefix += '/'; prefix += components[index];
            const auto key = prefix.normalized(QString::NormalizationForm_C).toCaseFolded().normalized(QString::NormalizationForm_C);
            const bool directory = index + 1 < components.size(); const auto previous = m_entries.constFind(key);
            if (previous != m_entries.cend()) {
                if (previous->path != prefix || previous->directory != directory || (!directory && (!shared || previous->digest != digest)))
                    return fail(error, QStringLiteral("导出资源路径或大小写身份冲突：%1").arg(path));
            } else m_entries.insert(key, {prefix, directory, directory ? QByteArray{} : digest});
        }
        return true;
    }
private:
    struct Entry { QString path; bool directory; QByteArray digest; };
    QMap<QString, Entry> m_entries;
};
bool json(const QByteArray& bytes, QJsonObject& result, QString* error)
{
    QJsonParseError parse; const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) return fail(error, QStringLiteral("作者附件 JSON 无效"));
    result = document.object(); return true;
}
bool settings(const QByteArray& bytes, QJsonObject& references, QString* error)
{
    if (bytes.size() > 16 * 1024 * 1024 || !CModelJson::ValidateRuntimeSettings(bytes, error)) return false;
    references = QJsonDocument::fromJson(bytes).object().value("FileReferences").toObject();
    if (!relative(references.value("Moc").toString()) || !references.value("Textures").isArray() ||
        references.value("Textures").toArray().size() > CTextureAssets::MaximumSlots) return fail(error, QStringLiteral("作者附件需要安全的模型和纹理目录"));
    for (const auto& value : references.value("Textures").toArray())
        if (!value.isString() || (!value.toString().isEmpty() && !relative(value.toString()))) return fail(error, QStringLiteral("模型纹理路径无效"));
    return true;
}
CTextureAssets::Catalog freezeSources(const CTextureAssets::Catalog& sources)
{
    CTextureAssets::Catalog result; QMap<QByteArray, QByteArray> blobs;
    for (auto it = sources.cbegin(); it != sources.cend(); ++it) {
        if (!blobs.contains(it->digest)) blobs.insert(owned(it->digest), owned(it->png));
        result.insert(it.key(), {owned(it->path), blobs[it->digest], owned(it->digest), it->size});
    }
    return result;
}
bool validateModel(const QByteArray& moc, const CModelAtlasState& state, CModelDocument& document, QString* error)
{
    if (!document.LoadBytes(moc, {}, error)) return false;
    CModelMaterialState actual; if (!document.CaptureDrawableMaterials(actual, error)) return false;
    CModelMaterialOverrides expected, captured;
    for (const auto& value : state.materials) { if (expected.contains(value.id)) return fail(error, QStringLiteral("作者附件网格目录重复")); expected.insert(value.id, value); }
    for (const auto& value : actual) captured.insert(value.id, value);
    if (expected != captured) return fail(error, QStringLiteral("作者附件与导出 MOC 的材质不一致"));
    CArtworkReconciler::Triangles triangles;
    for (const auto& mesh : document.Drawables()) {
        triangles.insert(mesh.id, document.DrawableIndices(mesh.id));
        if (mesh.vertexCount && !state.workspace.atlases.contains(mesh.textureIndex)) return fail(error, QStringLiteral("作者附件缺少网格使用的图集"));
    }
    return CArtworkReconciler::ValidateLinks(state.artwork, state.workspace, actual, triangles, error);
}
QString objectPath(const QByteArray& key, const QByteArray& bytes, const QSet<QByteArray>& icc)
{
    const auto extension = bytes.startsWith("\x89PNG\r\n\x1a\n") ? QStringLiteral("png") : bytes.startsWith("8BPS") ? QStringLiteral("psd") : icc.contains(key) ? QStringLiteral("icc") : QStringLiteral("bin");
    return QStringLiteral("authoring/artwork/objects/") + QString::fromLatin1(key.toHex()) + '.' + extension;
}
class Reader
{
public:
    Reader(QString root, Package::Limits limits, CExportControl* control) : m_root(std::move(root)), m_limits(limits), m_control(control) {}
    bool safe(const QString& path, QString& absolute, QString* error) const {
        if (!relative(path)) return fail(error, QStringLiteral("作者附件使用了不安全的相对路径"));
        QString current = m_root;
        for (const auto& part : path.split('/')) {
            current = QDir(current).filePath(part); const QFileInfo info(current);
            if (info.isSymLink()) return fail(error, QStringLiteral("作者附件不能经过符号链接"));
        }
        absolute = current; return true;
    }
    bool read(const QString& path, QByteArray& bytes, QString* error, qint64 maximum = -1) {
        QString absolute; if (!safe(path, absolute, error)) return false;
        const auto previous = m_files.constFind(path);
        if (previous != m_files.cend()) {
            if (maximum >= 0 && previous->size > maximum) return fail(error, QStringLiteral("作者附件索引过大"));
            return readChecked(path, absolute, previous.value(), bytes, error);
        }
        const QFileInfo before(absolute);
        if (!before.isFile() || before.canonicalFilePath() != absolute || before.size() < 0 ||
            before.size() > m_limits.maximumFileBytes || (maximum >= 0 && before.size() > maximum) ||
            before.size() > m_limits.maximumTotalBytes - m_total || m_files.size() >= m_limits.maximumFiles)
            return fail(error, QStringLiteral("作者附件文件缺失或超过读取预算：%1").arg(path));
        Stamp stamp{before.size(), before.lastModified(), before.metadataChangeTime(), {}};
        QByteArray data;
        if (!readChecked(path, absolute, stamp, data, error)) return false;
        stamp.digest = hash(data); m_total += data.size(); m_files.insert(path, stamp); bytes = std::move(data); return true;
    }
    bool finish(QString* error) {
        for (auto it = m_files.cbegin(); it != m_files.cend(); ++it) {
            QString absolute; if (!safe(it.key(), absolute, error)) return false;
            QFile file(absolute); const QFileInfo info(absolute);
            if (!same(info, it.value()) || !file.open(QIODevice::ReadOnly)) return fail(error, QStringLiteral("作者附件在读取期间发生变化"));
            QCryptographicHash digest(QCryptographicHash::Sha256); qint64 amount = 0;
            while (!file.atEnd()) {
                if (!checkpoint(m_control, error)) return false;
                const auto part = file.read(256 * 1024);
                if (file.error() != QFileDevice::NoError || part.isEmpty() || part.size() > it->size - amount) return fail(error, QStringLiteral("作者附件复核读取失败"));
                amount += part.size(); digest.addData(part);
            }
            if (amount != it->size || digest.result() != it->digest || !same(QFileInfo(absolute), it.value())) return fail(error, QStringLiteral("作者附件内容在读取期间被替换"));
        }
        for (auto it = m_files.cbegin(); it != m_files.cend(); ++it) {
            QString absolute;
            if (!safe(it.key(), absolute, error) || !same(QFileInfo(absolute), it.value()))
                return fail(error, QStringLiteral("作者附件在最终复核期间发生变化"));
        }
        return true;
    }
private:
    struct Stamp { qint64 size; QDateTime modified, changed; QByteArray digest; };
    bool same(const QFileInfo& info, const Stamp& stamp) const {
        return info.isFile() && !info.isSymLink() && info.canonicalFilePath() == info.absoluteFilePath() && info.size() == stamp.size &&
            info.lastModified() == stamp.modified && info.metadataChangeTime() == stamp.changed;
    }
    bool readChecked(const QString& path, const QString& absolute, const Stamp& stamp, QByteArray& bytes, QString* error) {
        QFile file(absolute); if (!same(QFileInfo(absolute), stamp) || !file.open(QIODevice::ReadOnly)) return fail(error, QStringLiteral("无法读取作者附件：%1").arg(path));
        QByteArray data;
        while (!file.atEnd()) {
            if (!checkpoint(m_control, error)) return false;
            const auto part = file.read(256 * 1024);
            if (file.error() != QFileDevice::NoError || part.isEmpty() || part.size() > stamp.size - data.size()) return fail(error, QStringLiteral("作者附件读取失败或大小已改变"));
            data.append(part);
        }
        QString checked;
        if (!safe(path, checked, error) || !same(QFileInfo(absolute), stamp) || data.size() != stamp.size ||
            (!stamp.digest.isEmpty() && hash(data) != stamp.digest)) return fail(error, QStringLiteral("作者附件在读取期间发生变化"));
        bytes = std::move(data); return true;
    }
    QString m_root; Package::Limits m_limits; CExportControl* m_control; qint64 m_total = 0; QMap<QString, Stamp> m_files;
};
}

QString CArtworkPackage::ManifestFile(const QString& modelFile)
{
    if (!relative(modelFile) || modelFile.contains('/')) return {};
    return modelFile + QStringLiteral(".artwork.json");
}

bool CArtworkPackage::Prepare(const Request& input, Prepared& result, CExportControl* control, QString* error) try
{
    if (error) error->clear();
    if (control && control->IsCancellationRequested()) return fail(error, QStringLiteral("作者附件导出已取消"));
    if (!limitsValid(input.limits) || ManifestFile(input.modelFile).isEmpty()) return fail(error, QStringLiteral("作者附件请求无效"));
    if (input.modelSettingsBytes.size() > 16 * 1024 * 1024 || input.mocBytes.size() > input.limits.maximumFileBytes ||
        !CModelMaterialEdit::Validate(input.state.materials, error) || !CTextureAssets::ValidateCatalog(input.sources, error)) return fail(error, error && !error->isEmpty() ? *error : QStringLiteral("作者附件输入超过预算"));
    Request request; request.modelFile = owned(input.modelFile); request.modelSettingsBytes = owned(input.modelSettingsBytes); request.mocBytes = owned(input.mocBytes);
    request.sources = freezeSources(input.sources); request.limits = input.limits;
    if (!CTextureWorkspace::Freeze(request.sources, input.state.workspace, request.state.workspace, error) || !Artwork::Freeze(input.state.artwork, request.state.artwork, error)) return false;
    for (const auto& value : input.state.materials) request.state.materials.append({owned(value.id), value.textureIndex, QVector<float>(value.uvs.cbegin(), value.uvs.cend())});
    if (!checkpoint(control, error)) return false;
    if (!CMaterialResourceBudget::Validate(request.sources, {request.state}, nullptr, error)) return false;
    QJsonObject references; CModelDocument model;
    if (!settings(request.modelSettingsBytes, references, error) || !validateModel(request.mocBytes, request.state, model, error)) return false;
    CTextureAssets::State pngs; if (!CTextureWorkspace::Effective(request.sources, request.state.workspace, pngs, error)) return false;
    const auto texturePaths = references["Textures"].toArray();
    if (texturePaths.size() != (pngs.isEmpty() ? 0 : pngs.lastKey() + 1)) return fail(error, QStringLiteral("导出纹理槽目录与作者附件不一致"));
    QJsonArray atlases; Paths paths;
    if (!paths.add(request.modelFile, hash(request.modelSettingsBytes), false, error) ||
        !paths.add(references["Moc"].toString(), hash(request.mocBytes), false, error)) return false;
    for (int slot = 0; slot < texturePaths.size(); ++slot) {
        const auto path = texturePaths[slot].toString();
        if (path.isEmpty() != !pngs.contains(slot)) return fail(error, QStringLiteral("导出空纹理槽与作者附件不一致"));
        if (path.isEmpty()) continue;
        const auto digest = hash(pngs[slot]); if (!paths.add(path, digest, true, error)) return false;
        const auto atlas = request.state.workspace.atlases[slot];
        atlases.append(QJsonObject{{"slot", slot}, {"id", atlas.id}, {"name", atlas.name}, {"path", path}, {"sha256", QString::fromLatin1(digest.toHex())}, {"width", atlas.size.width()}, {"height", atlas.size.height()}});
    }
    Artwork::Archive archive; if (!Artwork::EncodeArchive(request.state.artwork, archive, error)) return false;
    Prepared candidate; candidate.manifestFile = ManifestFile(request.modelFile); QJsonObject objects; QSet<QByteArray> icc;
    for (const auto& revision : request.state.artwork.revisions) {
        if (!revision.iccProfile.isEmpty()) icc.insert(hash(revision.iccProfile));
        if (!revision.path.isEmpty() && QFileInfo(revision.path).isAbsolute()) candidate.protectedPaths.append(revision.path);
    }
    for (const auto& source : request.sources) candidate.protectedPaths.append(source.path);
    const auto add = [&](const QString& path, const QByteArray& bytes) {
        if (candidate.entries.size() >= request.limits.maximumFiles || bytes.size() > request.limits.maximumFileBytes || bytes.size() > request.limits.maximumTotalBytes - candidate.totalBytes)
            return fail(error, QStringLiteral("作者附件超过导出预算"));
        if (!paths.add(path, hash(bytes), false, error)) return false;
        candidate.entries.append({path, bytes}); candidate.totalBytes += bytes.size(); return true;
    };
    for (auto it = archive.blobs.cbegin(); it != archive.blobs.cend(); ++it) {
        if (!checkpoint(control, error)) return false;
        const auto path = objectPath(it.key(), it.value(), icc);
        objects.insert(QString::fromLatin1(it.key().toHex()), QJsonObject{{"path", path}, {"size", qint64(it->size())}});
        if (!add(path, *it)) return false;
    }
    auto revisions = archive.metadata["revisions"].toObject();
    for (auto it = revisions.begin(); it != revisions.end(); ++it) {
        auto revision = it->toObject(); revision["path"] = objects[revision["sourceDigest"].toString()].toObject()["path"]; it.value() = revision;
    }
    archive.metadata["revisions"] = revisions;
    const QJsonObject manifest{{"version", 1}, {"model", QJsonObject{{"path", request.modelFile}, {"sha256", QString::fromLatin1(hash(request.modelSettingsBytes).toHex())}}},
        {"moc", QJsonObject{{"path", references["Moc"]}, {"sha256", QString::fromLatin1(hash(request.mocBytes).toHex())}}}, {"atlases", atlases}, {"objects", objects}, {"artwork", archive.metadata}};
    const auto bytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    if (bytes.size() > request.limits.maximumIndexBytes) return fail(error, QStringLiteral("作者附件索引超过预算"));
    if (!add(candidate.manifestFile, bytes) || !checkpoint(control, error)) return false;
    candidate.protectedPaths.removeDuplicates(); result = std::move(candidate); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法准备作者附件，原结果保持不变")); }

bool CArtworkPackage::Load(const QString& modelJsonPath, const QByteArray& settingsBytes, const CModelDocument& document,
                          Loaded& result, CExportControl* control, QString* error)
{ return Load(modelJsonPath, settingsBytes, document, Limits{}, result, control, error); }

bool CArtworkPackage::Load(const QString& inputPath, const QByteArray& inputSettings, const CModelDocument& inputDocument,
                          const Limits& inputLimits, Loaded& result, CExportControl* control, QString* error) try
{
    if (error) error->clear();
    if (control && control->IsCancellationRequested()) return fail(error, QStringLiteral("作者附件读取已取消"));
    if (inputSettings.size() > 16 * 1024 * 1024) return fail(error, QStringLiteral("模型设置超过读取预算"));
    const auto path = owned(inputPath); const auto settingsBytes = owned(inputSettings); const auto limits = inputLimits; const CModelDocument document = inputDocument;
    if (!limitsValid(limits) || !document.IsLoaded() || !document.HasTextureWorkspace()) return fail(error, QStringLiteral("加载作者附件需要已初始化的模型"));
    const auto modelFile = QFileInfo(path).fileName(), manifestFile = ManifestFile(modelFile);
    const auto root = QFileInfo(path).absoluteDir().canonicalPath();
    if (root.isEmpty() || manifestFile.isEmpty()) return fail(error, QStringLiteral("作者附件目录无效"));
    Reader reader(root, limits, control); QString manifestPath;
    if (!reader.safe(manifestFile, manifestPath, error)) return false;
    const QFileInfo manifestInfo(manifestPath);
    if (!manifestInfo.exists()) { if (!checkpoint(control, error)) return false; result = Loaded{}; return true; }
    CTextureAssets::Catalog sources = freezeSources(document.TextureSources()); CModelAtlasState baseline;
    if (!document.CaptureAtlasState(baseline, error)) return false;
    CModelAtlasState frozen; frozen.materials = baseline.materials;
    if (!CTextureWorkspace::Freeze(sources, baseline.workspace, frozen.workspace, error) || !Artwork::Freeze(baseline.artwork, frozen.artwork, error)) return false;
    QByteArray index, diskSettings;
    if (!reader.read(manifestFile, index, error, limits.maximumIndexBytes) || !reader.read(modelFile, diskSettings, error, settingsBytes.size()) || diskSettings != settingsBytes)
        return fail(error, error && !error->isEmpty() ? *error : QStringLiteral("模型设置与加载快照不一致"));
    QJsonObject manifest, references;
    if (!json(index, manifest, error) || !fields(manifest, {"version", "model", "moc", "atlases", "objects", "artwork"}) || manifest["version"] != QJsonValue(1) ||
        !manifest["model"].isObject() || !manifest["moc"].isObject() || !manifest["atlases"].isArray() || !manifest["objects"].isObject() || !manifest["artwork"].isObject() || !settings(settingsBytes, references, error))
        return fail(error, QStringLiteral("作者附件索引字段或版本无效"));
    const auto checkAnchor = [&](const QJsonObject& anchor, const QString& expectedPath, const QByteArray& expected) {
        QByteArray digest;
        return fields(anchor, {"path", "sha256"}) && anchor["path"].isString() && anchor["path"].toString() == expectedPath && ::live2dstruct::digest(anchor["sha256"], digest) && digest == hash(expected);
    };
    QByteArray moc; const auto mocPath = references["Moc"].toString();
    if (!checkAnchor(manifest["model"].toObject(), modelFile, settingsBytes) || !reader.read(mocPath, moc, error, document.SourceBytes().size()) ||
        !checkAnchor(manifest["moc"].toObject(), mocPath, moc) || moc != document.SourceBytes()) return fail(error, QStringLiteral("作者附件模型摘要不匹配"));
    QByteArray currentMoc; if (!document.BuildRuntimeMoc(currentMoc, error) || currentMoc != moc) return fail(error, QStringLiteral("已有模型编辑不能被作者附件覆盖"));
    const auto paths = references["Textures"].toArray(), anchors = manifest["atlases"].toArray();
    if (anchors.size() != sources.size()) return fail(error, QStringLiteral("作者附件图集数量不匹配"));
    Loaded candidate; candidate.present = true; candidate.state.materials = frozen.materials;
    if (!CTextureWorkspace::Initialize(sources, {}, candidate.state.workspace, error)) return false;
    if (frozen.workspace != candidate.state.workspace) return fail(error, QStringLiteral("已有图集编辑不能被作者附件覆盖"));
    QMap<QString, QString> remapped; QSet<int> registeredSlots; QMap<QString, QByteArray> loadedPaths;
    for (const auto& item : anchors) {
        if (!item.isObject()) return fail(error, QStringLiteral("作者附件图集锚点无效"));
        const auto anchor = item.toObject(); qint64 slot, width, height; QByteArray digest;
        if (!fields(anchor, {"slot", "id", "name", "path", "sha256", "width", "height"}) ||
            !number(anchor["slot"], 0, CTextureAssets::MaximumSlots - 1, slot) || registeredSlots.contains(int(slot)) || !sources.contains(int(slot)) || slot >= paths.size() ||
            !anchor["id"].isString() || anchor["id"].toString().isEmpty() || anchor["id"].toString().size() > 256 || anchor["id"].toString().contains(QChar::Null) || remapped.contains(anchor["id"].toString()) ||
            !anchor["name"].isString() || anchor["name"].toString().trimmed().isEmpty() || anchor["name"].toString().size() > 256 || anchor["name"].toString().contains(QChar::Null) ||
            !anchor["path"].isString() || anchor["path"].toString() != paths[int(slot)].toString() || !::live2dstruct::digest(anchor["sha256"], digest) ||
            !number(anchor["width"], 1, 32768, width) || !number(anchor["height"], 1, 32768, height)) return fail(error, QStringLiteral("作者附件图集锚点字段无效"));
        const auto& source = sources[int(slot)]; QString absolute; QByteArray data;
        const auto texturePath = anchor["path"].toString();
        if (!reader.safe(texturePath, absolute, error)) return false;
        const auto canonical = QFileInfo(absolute).canonicalFilePath();
        if (canonical.isEmpty() || QFileInfo(source.path).canonicalFilePath() != canonical ||
            source.digest != digest || source.size != QSize(int(width), int(height)) ||
            (loadedPaths.contains(texturePath) ? loadedPaths[texturePath] != digest : (!reader.read(texturePath, data, error, source.png.size()) || data != source.png)))
            return fail(error, QStringLiteral("作者附件图集来源不匹配"));
        loadedPaths.insert(texturePath, digest); registeredSlots.insert(int(slot));
        remapped.insert(anchor["id"].toString(), candidate.state.workspace.atlases[int(slot)].id);
        candidate.state.workspace.atlases[int(slot)].name = anchor["name"].toString();
    }
    for (int slot = 0; slot < paths.size(); ++slot)
        if (paths[slot].toString().isEmpty() != !registeredSlots.contains(slot)) return fail(error, QStringLiteral("作者附件空槽目录不匹配"));
    Artwork::Archive archive; archive.metadata = manifest["artwork"].toObject(); const auto objects = manifest["objects"].toObject(); QMap<QByteArray, QString> objectPaths;
    if (objects.size() > limits.maximumFiles - 1) return fail(error, QStringLiteral("作者附件对象数量超过预算"));
    qint64 blobBytes = 0;
    for (auto it = objects.begin(); it != objects.end(); ++it) {
        QByteArray key; qint64 size;
        if (!digest(it.key(), key) || !it->isObject() || !fields(it->toObject(), {"path", "size"})) return fail(error, QStringLiteral("作者附件对象记录无效"));
        const auto object = it->toObject(); const auto objectFile = object["path"].toString();
        const auto prefix = QStringLiteral("authoring/artwork/objects/") + it.key() + '.';
        if (!object["path"].isString() || !objectFile.startsWith(prefix) || !QSet<QString>{"psd", "png", "icc", "bin"}.contains(objectFile.mid(prefix.size())) ||
            !number(object["size"], 1, std::min(limits.maximumFileBytes, Artwork::MaximumBytes - blobBytes), size)) return fail(error, QStringLiteral("作者附件对象路径或大小无效"));
        QByteArray bytes; if (!reader.read(objectFile, bytes, error, size) || bytes.size() != size || hash(bytes) != key) return fail(error, QStringLiteral("作者附件对象内容摘要不匹配"));
        blobBytes += size; archive.blobs.insert(key, std::move(bytes)); objectPaths.insert(key, objectFile);
    }
    if (!archive.metadata["revisions"].isObject()) return fail(error, QStringLiteral("来源修订目录类型无效"));
    auto revisions = archive.metadata["revisions"].toObject();
    for (auto it = revisions.begin(); it != revisions.end(); ++it) {
        auto revision = it->toObject(); QByteArray key;
        if (!it->isObject() || !digest(revision["sourceDigest"], key) || !objectPaths.contains(key) || revision["path"].toString() != objectPaths[key] || !revision["path"].isString())
            return fail(error, QStringLiteral("来源修订路径与对象表不匹配"));
        revision["path"] = QDir(root).filePath(objectPaths[key]); it.value() = revision;
    }
    archive.metadata["revisions"] = revisions;
    if (!Artwork::DecodeArchive(archive, candidate.state.artwork, error)) return false;
    const QString tombstone = QStringLiteral("unavailable:artwork-atlas:");
    for (auto& placement : candidate.state.artwork.placements) {
        if (remapped.contains(placement.atlasId)) placement.atlasId = remapped[placement.atlasId];
        else if (!placement.atlasId.startsWith(tombstone)) placement.atlasId = tombstone + QString::fromLatin1(hash(placement.atlasId.toUtf8()).toHex());
    }
    CModelDocument checked;
    if (!CTextureWorkspace::Validate(sources, candidate.state.workspace, error) || !validateModel(moc, candidate.state, checked, error) ||
        !CMaterialResourceBudget::Validate(sources, {candidate.state}, nullptr, error) || !Artwork::Freeze(candidate.state.artwork, candidate.state.artwork, error) || !reader.finish(error)) return false;
    result = std::move(candidate); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法读取作者附件，原状态保持不变")); }
}
