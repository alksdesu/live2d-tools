#include "ccmo3builder.h"
#include "ccaffarchive.h"
#include "ccmo3entities.h"
#include "ccmo3ids.h"
#include "cxsxmlwriter.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/cdrawableflags.h"
#include "live2dstruct/ccanvasgeometry.h"
#include "live2dstruct/cparentgraph.h"
#include "live2dstruct/cmodeluserdata.h"
#include "live2dstruct/cphysicsdocument.h"
#include "live2dstruct/util/cfilereader.h"
#include "opengl/ctextureextractor.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace live2dstruct;

namespace cmo3 {

namespace {

const QString kNoDebug = QStringLiteral("(no debug info)");

bool validSlice(int begin, int count, int size)
{
    return begin >= 0 && count >= 0 && begin <= size && count <= size - begin;
}

CModelParameterMetadata effectiveParameterMetadata(const CParameters* parameter, const CCmo3Builder::Options& options)
{
    const auto edited = options.parameterMetadata.constFind(parameter->ids);
    if (edited != options.parameterMetadata.cend()) return edited.value();
    return {parameter->ids, parameter->min_values, parameter->max_values, parameter->default_values,
            parameter->decimal_places, parameter->is_repeat};
}

bool validateParameterOptions(const CSectionTable& table, const CCmo3Builder::Options& options, QString* error)
{
    const auto fail = [&](const QString& reason) { if (error) *error = reason; return false; };
    QHash<QString, const CParameters*> parameters;
    for (const auto* parameter : table.parameters) {
        if (!parameter || parameter->ids.isEmpty() || parameters.contains(parameter->ids))
            return fail(QStringLiteral("cmo3 参数 ID 为空或重复"));
        parameters.insert(parameter->ids, parameter);
        const auto metadata = effectiveParameterMetadata(parameter, options);
        if (!std::isfinite(metadata.minimum) || !std::isfinite(metadata.maximum) ||
            !std::isfinite(metadata.defaultValue) || metadata.minimum > metadata.maximum ||
            metadata.defaultValue < metadata.minimum || metadata.defaultValue > metadata.maximum ||
            metadata.decimalPlaces < 0 || metadata.decimalPlaces > 9)
            return fail(QStringLiteral("cmo3 参数范围、默认值或精度无效：%1").arg(parameter->ids));
    }
    for (auto metadata = options.parameterMetadata.cbegin(); metadata != options.parameterMetadata.cend(); ++metadata)
        if (!parameters.contains(metadata.key())) return fail(QStringLiteral("cmo3 参数定义引用了未知 ID：%1").arg(metadata.key()));
    for (auto value = options.parameterValues.cbegin(); value != options.parameterValues.cend(); ++value) {
        const auto parameter = parameters.constFind(value.key());
        if (parameter == parameters.cend()) return fail(QStringLiteral("cmo3 当前参数值引用了未知 ID：%1").arg(value.key()));
        const auto metadata = effectiveParameterMetadata(parameter.value(), options);
        if (!std::isfinite(value.value()) || value.value() < metadata.minimum || value.value() > metadata.maximum)
            return fail(QStringLiteral("cmo3 当前参数值无效或超出范围：%1").arg(value.key()));
    }
    return true;
}

constexpr qint64 PhysicsByteLimit = 16 * 1024 * 1024;

bool validatePhysicsBytes(const QByteArray& bytes, QJsonObject& snapshot, QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error) *error = QStringLiteral("physics3 配置无效：%1").arg(message);
        return false;
    };
    if (bytes.isEmpty() || bytes.size() > PhysicsByteLimit) return fail(QStringLiteral("文件为空或超过 16 MiB"));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("physics3 JSON 无效：%1").arg(parseError.errorString());
        return false;
    }
    QJsonObject normalized;
    QString detail;
    if (!CPhysicsDocument::Normalize(document.object(), normalized, &detail)) return fail(detail);
    QSet<QString> groupIds;
    for (const auto& value : normalized.value(QStringLiteral("PhysicsSettings")).toArray()) {
        const auto group = value.toObject();
        const auto id = group.value(QStringLiteral("Id")).toString();
        if (id.isEmpty() || id.contains(QChar::Null) || groupIds.contains(id))
            return fail(QStringLiteral("物理组 ID 为空、无效或重复：%1").arg(id));
        groupIds.insert(id);
    }
    snapshot.swap(normalized);
    return true;
}

bool validatePhysicsFile(const QString& path, QJsonObject& snapshot, QString* error)
{
    QFile file(path);
    if (!QFileInfo(file).isFile() || !file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开 physics3 文件：%1").arg(file.errorString());
        return false;
    }
    if (file.size() > PhysicsByteLimit) {
        if (error) *error = QStringLiteral("physics3 文件超过 16 MiB");
        return false;
    }
    const auto bytes = file.read(PhysicsByteLimit + 1);
    if (file.error() != QFileDevice::NoError) {
        if (error) *error = QStringLiteral("无法读取 physics3 文件：%1").arg(file.errorString());
        return false;
    }
    return validatePhysicsBytes(bytes, snapshot, error);
}

bool validatePhysicsParameters(const QJsonObject& physics, const QSet<QString>& ids, QString* error)
{
    for (const auto& value : physics.value(QStringLiteral("PhysicsSettings")).toArray()) {
        const auto group = value.toObject();
        for (const auto& field : {QStringLiteral("Input"), QStringLiteral("Output")}) {
            const auto endpoint = field == QLatin1String("Input") ? QStringLiteral("Source") : QStringLiteral("Destination");
            for (const auto& row : group.value(field).toArray()) {
                const auto id = row.toObject().value(endpoint).toObject().value(QStringLiteral("Id")).toString();
                if (!ids.contains(id)) {
                    if (error) *error = QStringLiteral("physics3 引用了不存在的参数：%1").arg(id);
                    return false;
                }
            }
        }
    }
    return true;
}

bool validateDeformerSources(const CSectionTable& table, QString* error)
{
    for (int i = 0; i < table.deformers.size(); ++i) {
        const CDeformers* deformer = table.deformers[i];
        const int sourceIndex = deformer->specific_sources_indices;
        const int sourceCount = deformer->types == 0 ? table.warp_deformers.size()
                                                     : deformer->types == 1 ? table.rotation_deformers.size() : 0;
        if ((deformer->types != 0 && deformer->types != 1) ||
            sourceIndex < 0 || sourceIndex >= sourceCount) {
            if (error) {
                *error = QStringLiteral("Deformer %1 的专用数据索引无效：type=%2, index=%3")
                    .arg(deformer->ids).arg(deformer->types).arg(sourceIndex);
            }
            return false;
        }
    }
    return true;
}

bool loadModelTextures(const QDir& directory, const QString& mocPath, QVector<QString>* paths)
{
    const QStringList jsons = directory.entryList({QStringLiteral("*.model3.json")}, QDir::Files, QDir::Name);
    const QString targetMoc = QFileInfo(mocPath).canonicalFilePath().isEmpty()
        ? QFileInfo(mocPath).absoluteFilePath() : QFileInfo(mocPath).canonicalFilePath();
    QString selectedJson;
    for (const QString& jsonName : jsons) {
        QFile file(directory.filePath(jsonName));
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        const QString reference = root.value(QStringLiteral("FileReferences")).toObject()
                                      .value(QStringLiteral("Moc")).toString();
        if (reference.isEmpty()) continue;
        const QString referencedMoc = QFileInfo(directory.filePath(reference)).canonicalFilePath().isEmpty()
            ? QFileInfo(directory.filePath(reference)).absoluteFilePath()
            : QFileInfo(directory.filePath(reference)).canonicalFilePath();
        if (referencedMoc == targetMoc) {
            selectedJson = directory.filePath(jsonName);
            break;
        }
    }
    if (selectedJson.isEmpty() && jsons.size() == 1) selectedJson = directory.filePath(jsons.first());
    if (selectedJson.isEmpty()) return false;

    QFile file(selectedJson);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    const QJsonArray textures = root.value(QStringLiteral("FileReferences"))
                                    .toObject().value(QStringLiteral("Textures")).toArray();
    if (textures.isEmpty()) return false;
    for (const QJsonValue& texture : textures) {
        const QString path = texture.toString();
        if (path.isEmpty()) return false;
        paths->append(directory.filePath(path));
    }
    return true;
}

// keyform 绑定链：一个绑定源 = 参数索引 + 该参数上的 key 值序列
struct SBindingChain {
    struct SBinding {
        int paramIndex = -1;
        QVector<float> keys;
    };
    QVector<SBinding> bindings;     // 行主序：第一绑定变最快
};

// 每参数一份 shared 池 id
struct SParamIds {
    QString guid;                   // CParameterGuid xs.id
};

// 每 Part 一份
struct SPartIds {
    QString source;                 // CPartSource xs.id
    QString guid;                   // CPartGuid xs.id
    QVector<QString> childGuidRefs; // 子对象 guid id（部件/变形器/mesh 挂入）
    QVector<QString> childGuidTags; // 与 childGuidRefs 对应的类名
};

// 每变形器一份
struct SDeformerIds {
    QString guid;                   // CDeformerGuid xs.id
};

class Generator
{
public:
    Generator(const CCmo3Builder::Options& options, CSectionTable& table,
              QVector<ExtractedMaterial>& extracted, const QHash<QString, QString>& userData, const QJsonObject& physics)
        : m_opt(options), m_table(table), m_extracted(extracted), m_userData(userData),
          m_physics(physics), m_entities(m_xml, m_caff)
    {
    }

    bool Run(QString* outError);

private:
    // ---- 数据准备 ----
    SBindingChain chainFor(int keyformBindingSourceIndex) const;
    void loadDisplayNames();
    bool prepareMaterials(QString* error);
    bool prepareDeformerUnits(QString* error);

    // ---- shared 池写出 ----
    void writeParameterGuids();
    void writeDeformerGuids();
    void writePartEntities();
    void writeArtMeshEntities();
    void writeWarpDeformerEntities();
    void writeRotationDeformerEntities();
    bool rawCanvasToPixels(const QString& owner, float x, float y, float& pixelX, float& pixelY);
    void writeGlueEntities();

    // ---- 复用块 ----
    // KeyformGridSource + KeyformBindingSource 族；返回 gridSource 的 xs.id
    QString writeKeyformGrid(const SBindingChain& chain, const QVector<QString>& formGuidIds);
    void writeControllableHeader(const QString& localName, bool isVisible,
                                 const QString& parentPartGuidId, const QString& gridId,
                                 int extensionCount);
    QString deformerTargetId(int parentDeformerIndex) const;
    bool objectVisible(const QString& type, const QString& id, bool sourceVisible) const;
    int effectiveDeformerParentIndex(int deformerIndex) const;
    int partParentIndex(int partIndex) const;
    int objectPartIndex(int parentPartIndex) const;
    QVector<float> meshKeyformPositions(const CArt_meshes* mesh, int keyformIndex) const;

    // ---- main 段 ----
    void writeMain();
    void writePhysicsMain();

    QString displayName(const QString& id) const
    {
        return m_displayNames.value(id, id);
    }

    QString parameterDisplayName(const QString& id) const
    {
        const auto metadata = m_opt.parameterMetadata.constFind(id);
        if (metadata != m_opt.parameterMetadata.cend() && !metadata->displayName.isEmpty() && metadata->displayName != id)
            return metadata->displayName;
        return m_parameterDisplayNames.value(id, id);
    }

    live2dstruct::CModelParameterMetadata parameterMetadata(const CParameters* parameter) const
    {
        return effectiveParameterMetadata(parameter, m_opt);
    }

    const CCmo3Builder::Options& m_opt;
    CSectionTable& m_table;
    QVector<ExtractedMaterial>& m_extracted;
    const QHash<QString, QString>& m_userData;
    const QJsonObject& m_physics;

    CXsXmlWriter m_xml;
    CCaffArchive m_caff;
    CCmo3Entities m_entities;

    // xs.id 索引（与 moc3 各表下标对齐）
    QVector<SParamIds> m_paramIds;
    QVector<SPartIds> m_partIds;
    QVector<int> m_partParentIndices;
    QVector<SDeformerIds> m_deformerIds;        // 与 deformers 表对齐
    QVector<bool> m_hasRotationAncestor;
    QVector<QString> m_meshGuidIds;             // 与 art_meshes 表对齐
    QVector<QString> m_meshSourceIds;           // CArtMeshSource xs.id
    QVector<QString> m_deformerSourceIds;       // 与 deformers 表对齐
    QVector<QString> m_glueSourceIds;
    QVector<SLayerData> m_layers;               // 与可见提取层对齐
    QVector<int> m_layerOfMesh;                 // art_meshes 下标 → m_layers 下标（-1 无贴图）
    QVector<QVector<float>> m_materialPositions, m_materialUvs;
    QSize m_sourceCanvas;

    QString m_deformerRootGuidId;
    QString m_coordBasic;
    QString m_coordDeformerLocal;
    QString m_coordCanvas;
    SFilterGraphIds m_filterGraph;
    SLayeredImageIds m_layeredImage;
    QString m_modelImageGroupId;
    QString m_rootPartSourceId;
    QString m_rootPartGuidId;
    QString m_rootPartFormGuidId;

    QHash<QString, QString> m_displayNames;     // 内部 ID → cdi3 显示名
    QHash<QString, QString> m_parameterDisplayNames;
    QHash<QString, int> m_paramIndexById;       // 参数 idstr → parameters 表下标
    int m_canvasW = 0;
    int m_canvasH = 0;
    QString m_geometryError;
};

bool Generator::rawCanvasToPixels(const QString& owner, float x, float y, float& pixelX, float& pixelY)
{
    QPointF pixels;
    if (!CCanvasGeometry::ModelToPixels(m_table.canvas_info, QPointF(x, -double(y)), pixels) ||
        !std::isfinite(pixels.x()) || !std::isfinite(pixels.y()) ||
        std::abs(pixels.x()) > std::numeric_limits<float>::max() ||
        std::abs(pixels.y()) > std::numeric_limits<float>::max()) {
        if (m_geometryError.isEmpty()) m_geometryError = QStringLiteral("画布坐标转换无效或超出 float 范围：%1").arg(owner);
        return false;
    }
    pixelX = static_cast<float>(pixels.x());
    pixelY = static_cast<float>(pixels.y());
    return true;
}

bool Generator::prepareDeformerUnits(QString* error)
{
    QVector<int> parents;
    parents.reserve(m_table.deformers.size());
    for (int index = 0; index < m_table.deformers.size(); ++index) parents.append(effectiveDeformerParentIndex(index));
    const CParentGraph graph(parents);
    if (!graph.IsValid()) {
        if (error) *error = QStringLiteral("变形器父链无效，无法确定画布缩放单位");
        return false;
    }
    m_hasRotationAncestor.fill(false, parents.size());
    for (int index : graph.TopologicalOrder()) {
        const int parent = parents[index];
        m_hasRotationAncestor[index] = parent >= 0 && (m_table.deformers[parent]->types == 1 || m_hasRotationAncestor[parent]);
    }
    return true;
}

bool Generator::prepareMaterials(QString* error)
{
    const auto fail = [&](const QString& reason) { if (error) *error = reason; return false; };
    const int meshCount = m_table.art_meshes.size();
    m_layerOfMesh.fill(-1, meshCount);
    m_materialPositions.resize(meshCount);
    m_materialUvs.resize(meshCount);
    qint64 vertexReferences = 0, encodedPixels = 0;
    for (const auto& material : m_extracted) {
        if (material.meshIndex < 0 || material.meshIndex >= meshCount || material.image.isNull() ||
            material.atlasSize.isEmpty() || material.atlasRect.size() != material.image.size())
            return fail(QStringLiteral("cmo3 素材图片或图集尺寸无效：%1").arg(material.name));
        const int count = m_table.art_meshes[material.meshIndex]->vertex_counts;
        if (count < 0 || count > CTextureExtractor::MaximumMaterialReferences - vertexReferences)
            return fail(QStringLiteral("cmo3 素材顶点总量超过限制"));
        vertexReferences += count;
        const qint64 pixels = qint64(material.image.width()) * material.image.height();
        if (pixels > CTextureExtractor::MaximumTotalMaterialPixels - encodedPixels)
            return fail(QStringLiteral("cmo3 素材编码总像素数量超过限制"));
        encodedPixels += pixels;
        m_sourceCanvas = m_sourceCanvas.expandedTo(material.atlasSize).expandedTo(material.image.size());
    }
    for (const auto& material : m_extracted) {
        const auto* mesh = m_table.art_meshes[material.meshIndex];
        if (m_layerOfMesh[material.meshIndex] >= 0 || mesh->ids != material.name ||
            mesh->texture_nos != material.textureNo || mesh->vertex_counts <= 0 ||
            mesh->vertex_counts > std::numeric_limits<int>::max() / 2 ||
            !validSlice(mesh->uv_sources_begin_indices, mesh->vertex_counts * 2, m_table.uvs->uvs.size()))
            return fail(QStringLiteral("cmo3 素材与网格数据不匹配：%1").arg(material.name));
        SLayerData layer;
        layer.name = displayName(material.name);
        layer.image = material.image;
        layer.visible = material.visible;
        const QPoint origin(std::clamp(material.atlasRect.x(), 0, m_sourceCanvas.width() - layer.image.width()),
                            std::clamp(material.atlasRect.y(), 0, m_sourceCanvas.height() - layer.image.height()));
        layer.bounds = QRect(origin, layer.image.size());
        auto& positions = m_materialPositions[material.meshIndex];
        auto& uvs = m_materialUvs[material.meshIndex];
        positions.reserve(mesh->vertex_counts * 2);
        uvs.reserve(mesh->vertex_counts * 2);
        const qint64 envelopeWidth = ((qint64(layer.image.width()) + MaterialMipmapLevel - 1) / MaterialMipmapLevel) * MaterialMipmapLevel;
        const qint64 envelopeHeight = ((qint64(layer.image.height()) + MaterialMipmapLevel - 1) / MaterialMipmapLevel) * MaterialMipmapLevel;
        for (int v = 0; v < mesh->vertex_counts; ++v) {
            const int uvIndex = mesh->uv_sources_begin_indices + v * 2;
            const double localX = double(m_table.uvs->uvs[uvIndex]) * material.atlasSize.width() - material.atlasRect.x();
            const double localY = double(m_table.uvs->uvs[uvIndex + 1]) * material.atlasSize.height() - material.atlasRect.y();
            const float x = static_cast<float>(localX + origin.x());
            const float y = static_cast<float>(localY + origin.y());
            if (!std::isfinite(x) || !std::isfinite(y))
                return fail(QStringLiteral("cmo3 素材坐标无效：%1").arg(material.name));
            positions.append(x);
            positions.append(y);
            // Editor 的缓存 UV 按 mipmap 边界归一化，重算时读取已保存的基本坐标。
            uvs.append(static_cast<float>((double(x) - origin.x()) / envelopeWidth));
            uvs.append(static_cast<float>((double(y) - origin.y()) / envelopeHeight));
        }
        m_layerOfMesh[material.meshIndex] = m_layers.size();
        m_layers.append(layer);
    }
    if (m_layers.isEmpty()) return fail(QStringLiteral("没有任何可提取的 ArtMesh 贴图"));
    return true;
}

void Generator::loadDisplayNames()
{
    if (m_opt.cdiPath.isEmpty()) return;
    QFile file(m_opt.cdiPath);
    if (!file.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();

    for (const QString& section : { QStringLiteral("Parameters"), QStringLiteral("Parts"),
                                    QStringLiteral("ParameterGroups") }) {
        for (const QJsonValue& v : root.value(section).toArray()) {
            const QJsonObject o = v.toObject();
            const QString id = o.value(QStringLiteral("Id")).toString();
            const QString name = o.value(QStringLiteral("Name")).toString();
            if (!id.isEmpty() && !name.isEmpty()) {
                if (section == QLatin1String("Parameters")) m_parameterDisplayNames.insert(id, name);
                else m_displayNames.insert(id, name);
            }
        }
    }
}

SBindingChain Generator::chainFor(int keyformBindingSourceIndex) const
{
    SBindingChain chain;
    if (keyformBindingSourceIndex < 0 ||
        keyformBindingSourceIndex >= m_table.keyform_bindings.size()) {
        return chain;
    }
    const CKeyform_bindings* kb = m_table.keyform_bindings[keyformBindingSourceIndex];
    const int begin = kb->parameter_binding_index_sources_begin_indices;
    const int count = kb->parameter_binding_index_sources_counts;
    if (!validSlice(begin, count, m_table.parameter_binding_indices.size())) return chain;
    for (int i = 0; i < count; ++i) {
        const int pbIndex = m_table.parameter_binding_indices[begin + i]->binding_sources_indices;
        if (!validSlice(pbIndex, 1, m_table.parameter_bindings.size())) continue;
        // parameter_bindings 表与参数的对应关系：参数的 parameter_binding_sources_begin_indices 起始连续段
        SBindingChain::SBinding binding;
        for (int p = 0; p < m_table.parameters.size(); ++p) {
            const CParameters* param = m_table.parameters[p];
            const int pBegin = param->parameter_binding_sources_begin_indices;
            if (pbIndex >= pBegin && pbIndex < pBegin + param->parameter_binding_sources_counts) {
                binding.paramIndex = p;
                break;
            }
        }
        const CParameter_bindings* pb = m_table.parameter_bindings[pbIndex];
        if (!validSlice(pb->keys_sources_begin_indices, pb->keys_sources_counts,
                        static_cast<int>(m_table.keys->values.size()))) continue;
        for (int k = 0; k < pb->keys_sources_counts; ++k) {
            binding.keys.append(m_table.keys->GetData(pb->keys_sources_begin_indices + k));
        }
        chain.bindings.append(binding);
    }
    return chain;
}

void Generator::writeParameterGuids()
{
    m_paramIds.resize(m_table.parameters.size());
    for (int i = 0; i < m_table.parameters.size(); ++i) {
        m_paramIds[i].guid = m_xml.AllocId();
        m_xml.GuidObject(QStringLiteral("CParameterGuid"), NewGuid(), m_paramIds[i].guid,
                         m_table.parameters[i]->ids);
        m_paramIndexById.insert(m_table.parameters[i]->ids, i);
    }
}

void Generator::writePhysicsMain()
{
    m_xml.BeginObject(QStringLiteral("CPhysicsSettingsSourceSet"),
                      QStringLiteral("physicsSettingsSourceSet"));
    const QJsonArray settings = m_physics.value(QStringLiteral("PhysicsSettings")).toArray();
    QHash<QString, QString> groupNames;
    const QJsonArray dictionary = m_physics.value(QStringLiteral("Meta")).toObject()
        .value(QStringLiteral("PhysicsDictionary")).toArray();
    for (const auto& value : dictionary) {
        const QJsonObject entry = value.toObject();
        const QString id = entry.value(QStringLiteral("Id")).toString();
        const QString name = entry.value(QStringLiteral("Name")).toString();
        if (!id.isEmpty() && !name.isEmpty()) groupNames.insert(id, name);
    }
    m_xml.BeginCArrayList(QStringLiteral("_sourceCubismPhysics"), settings.size());

    // physics3.json Type → CPhysicsSourceType 枚举
    const auto srcType = [](const QString& type) {
        if (type == QLatin1String("X")) return QStringLiteral("SRC_TO_X");
        if (type == QLatin1String("Y")) return QStringLiteral("SRC_TO_Y");
        return QStringLiteral("SRC_TO_G_ANGLE");
    };
    const auto paramGuidRef = [this](const QString& fieldName, const QString& paramId) {
        const int idx = m_paramIndexById.value(paramId, -1);
        if (idx >= 0) {
            m_xml.Ref(QStringLiteral("CParameterGuid"), fieldName, m_paramIds[idx].guid);
        } else {
            m_xml.Null(fieldName);
        }
    };

    for (const QJsonValue& sv : settings) {
        const QJsonObject setting = sv.toObject();
        const QString id = setting.value(QStringLiteral("Id")).toString();
        m_xml.BeginObject(QStringLiteral("CPhysicsSettingsSource"));
        m_xml.S(QStringLiteral("name"), groupNames.value(id, id));
        m_xml.BeginObject(QStringLiteral("CPhysicsSettingsGuid"), QStringLiteral("guid"));
        m_xml.Attribute(QStringLiteral("uuid"), NewGuid());
        m_xml.Attribute(QStringLiteral("note"), kNoDebug);
        m_xml.EndObject();
        m_xml.BeginObject(QStringLiteral("CPhysicsSettingId"), QStringLiteral("id"));
        m_xml.Attribute(QStringLiteral("idstr"), id);
        m_xml.EndObject();

        const QJsonArray inputs = setting.value(QStringLiteral("Input")).toArray();
        m_xml.BeginCArrayList(QStringLiteral("inputs"), inputs.size());
        for (const QJsonValue& iv : inputs) {
            const QJsonObject input = iv.toObject();
            m_xml.BeginObject(QStringLiteral("CPhysicsInput"));
            m_xml.BeginObject(QStringLiteral("CPhysicsDataGuid"), QStringLiteral("guid"));
            m_xml.Attribute(QStringLiteral("uuid"), NewGuid());
            m_xml.Attribute(QStringLiteral("note"), kNoDebug);
            m_xml.EndObject();
            paramGuidRef(QStringLiteral("source"),
                         input.value(QStringLiteral("Source")).toObject()
                              .value(QStringLiteral("Id")).toString());
            m_xml.F(QStringLiteral("angleScale"), 0.0f);
            m_xml.BeginObject(QStringLiteral("GVector2"), QStringLiteral("translationScale"));
            m_xml.F(QStringLiteral("x"), 0.0f);
            m_xml.F(QStringLiteral("y"), 0.0f);
            m_xml.EndObject();
            m_xml.F(QStringLiteral("weight"),
                    static_cast<float>(input.value(QStringLiteral("Weight")).toDouble()));
            m_xml.Enum(QStringLiteral("CPhysicsSourceType"), QStringLiteral("type"),
                       srcType(input.value(QStringLiteral("Type")).toString()));
            m_xml.B(QStringLiteral("isReverse"), input.value(QStringLiteral("Reflect")).toBool());
            m_xml.EndObject();
        }
        m_xml.EndList();

        const QJsonArray outputs = setting.value(QStringLiteral("Output")).toArray();
        m_xml.BeginCArrayList(QStringLiteral("outputs"), outputs.size());
        for (const QJsonValue& ov : outputs) {
            const QJsonObject output = ov.toObject();
            m_xml.BeginObject(QStringLiteral("CPhysicsOutput"));
            m_xml.BeginObject(QStringLiteral("CPhysicsDataGuid"), QStringLiteral("guid"));
            m_xml.Attribute(QStringLiteral("uuid"), NewGuid());
            m_xml.Attribute(QStringLiteral("note"), kNoDebug);
            m_xml.EndObject();
            paramGuidRef(QStringLiteral("destination"),
                         output.value(QStringLiteral("Destination")).toObject()
                               .value(QStringLiteral("Id")).toString());
            m_xml.I(QStringLiteral("vertexIndex"), output.value(QStringLiteral("VertexIndex")).toInt());
            m_xml.BeginObject(QStringLiteral("GVector2"), QStringLiteral("translationScale"));
            m_xml.F(QStringLiteral("x"), 0.0f);
            m_xml.F(QStringLiteral("y"), 0.0f);
            m_xml.EndObject();
            m_xml.F(QStringLiteral("angleScale"),
                    static_cast<float>(output.value(QStringLiteral("Scale")).toDouble()));
            m_xml.F(QStringLiteral("weight"),
                    static_cast<float>(output.value(QStringLiteral("Weight")).toDouble()));
            m_xml.Enum(QStringLiteral("CPhysicsSourceType"), QStringLiteral("type"), QStringLiteral("SRC_TO_G_ANGLE"));
            m_xml.B(QStringLiteral("isReverse"), output.value(QStringLiteral("Reflect")).toBool());
            m_xml.EndObject();
        }
        m_xml.EndList();

        const QJsonArray vertices = setting.value(QStringLiteral("Vertices")).toArray();
        m_xml.BeginCArrayList(QStringLiteral("vertices"), vertices.size());
        for (const QJsonValue& vv : vertices) {
            const QJsonObject vertex = vv.toObject();
            const QJsonObject pos = vertex.value(QStringLiteral("Position")).toObject();
            m_xml.BeginObject(QStringLiteral("CPhysicsVertex"));
            m_xml.BeginObject(QStringLiteral("CPhysicsDataGuid"), QStringLiteral("guid"));
            m_xml.Attribute(QStringLiteral("uuid"), NewGuid());
            m_xml.Attribute(QStringLiteral("note"), kNoDebug);
            m_xml.EndObject();
            m_xml.BeginObject(QStringLiteral("GVector2"), QStringLiteral("position"));
            m_xml.F(QStringLiteral("x"), static_cast<float>(pos.value(QStringLiteral("X")).toDouble()));
            m_xml.F(QStringLiteral("y"), static_cast<float>(pos.value(QStringLiteral("Y")).toDouble()));
            m_xml.EndObject();
            m_xml.F(QStringLiteral("mobility"),
                    static_cast<float>(vertex.value(QStringLiteral("Mobility")).toDouble()));
            m_xml.F(QStringLiteral("delay"),
                    static_cast<float>(vertex.value(QStringLiteral("Delay")).toDouble()));
            m_xml.F(QStringLiteral("acceleration"),
                    static_cast<float>(vertex.value(QStringLiteral("Acceleration")).toDouble()));
            m_xml.F(QStringLiteral("radius"),
                    static_cast<float>(vertex.value(QStringLiteral("Radius")).toDouble()));
            m_xml.EndObject();
        }
        m_xml.EndList();
        const auto normalization = setting.value(QStringLiteral("Normalization")).toObject();
        const struct { const char* group; const char* value; const char* field; } ranges[] = {
            {"Position", "Minimum", "normalizedPositionValueMin"},
            {"Position", "Maximum", "normalizedPositionValueMax"},
            {"Position", "Default", "normalizedPositionDefaultValue"},
            {"Angle", "Minimum", "normalizedAngleValueMin"},
            {"Angle", "Maximum", "normalizedAngleValueMax"},
            {"Angle", "Default", "normalizedAngleDefaultValue"}
        };
        for (const auto& range : ranges)
            m_xml.F(QString::fromLatin1(range.field), static_cast<float>(normalization.value(QString::fromLatin1(range.group)).toObject()
                .value(QString::fromLatin1(range.value)).toDouble()));
        m_xml.EndObject(); // CPhysicsSettingsSource
    }
    m_xml.EndList();
    const auto fps = m_physics.value(QStringLiteral("Meta")).toObject().value(QStringLiteral("Fps"));
    if (fps.isUndefined()) m_xml.Null(QStringLiteral("settingFPS"));
    else m_xml.I(QStringLiteral("settingFPS"), static_cast<int>(static_cast<float>(fps.toDouble())));
    m_xml.EndObject(); // CPhysicsSettingsSourceSet
}

void Generator::writeDeformerGuids()
{
    m_deformerIds.resize(m_table.deformers.size());
    m_deformerSourceIds.fill(QString(), m_table.deformers.size());
    for (int i = 0; i < m_table.deformers.size(); ++i) {
        m_deformerIds[i].guid = m_xml.AllocId();
        m_xml.GuidObject(QStringLiteral("CDeformerGuid"), NewGuid(), m_deformerIds[i].guid,
                         m_table.deformers[i]->ids);
    }
}

QString Generator::deformerTargetId(int parentDeformerIndex) const
{
    if (parentDeformerIndex >= 0 && parentDeformerIndex < m_deformerIds.size()) {
        return m_deformerIds[parentDeformerIndex].guid;
    }
    return m_deformerRootGuidId;
}

int Generator::effectiveDeformerParentIndex(int deformerIndex) const
{
    if (deformerIndex < 0 || deformerIndex >= m_table.deformers.size()) return -1;
    const auto it = m_opt.deformerParentOverrides.constFind(m_table.deformers[deformerIndex]->ids);
    if (it == m_opt.deformerParentOverrides.cend()) return m_table.deformers[deformerIndex]->parent_deformer_indices;
    return it.value() >= 0 && it.value() < m_table.deformers.size() && it.value() != deformerIndex ? it.value() : -1;
}

int Generator::partParentIndex(int partIndex) const
{
    return partIndex >= 0 && partIndex < m_partParentIndices.size()
        ? m_partParentIndices[partIndex] : -1;
}

int Generator::objectPartIndex(int parentPartIndex) const
{
    return parentPartIndex >= 0 && parentPartIndex < m_partParentIndices.size()
        ? parentPartIndex : -1;
}

QVector<float> Generator::meshKeyformPositions(const CArt_meshes* mesh, int keyformIndex) const
{
    if (!mesh || keyformIndex < 0 || keyformIndex >= mesh->keyform_sources_counts) return {};
    const auto overrideIt = m_opt.drawableKeyformPositionOverrides.constFind(mesh->ids);
    if (overrideIt != m_opt.drawableKeyformPositionOverrides.cend() && keyformIndex < overrideIt->size()) {
        const QVector<float>& positions = overrideIt->at(keyformIndex);
        if (positions.size() == mesh->vertex_counts * 2) return positions;
    }
    if (!validSlice(mesh->keyform_sources_begin_indices, mesh->keyform_sources_counts,
                    m_table.art_mesh_keyforms.size())) return {};
    const CArt_mesh_keyforms* keyform = m_table.art_mesh_keyforms[
        mesh->keyform_sources_begin_indices + keyformIndex];
    if (!validSlice(keyform->keyform_position_sources_begin_indices, mesh->vertex_counts * 2,
                    static_cast<int>(m_table.keyform_positions->xys.size()))) return {};
    QVector<float> positions;
    positions.reserve(mesh->vertex_counts * 2);
    for (int v = 0; v < mesh->vertex_counts * 2; ++v) {
        positions.append(m_table.keyform_positions->GetData(
            keyform->keyform_position_sources_begin_indices + v));
    }
    return positions;
}

QString Generator::writeKeyformGrid(const SBindingChain& chain, const QVector<QString>& formGuidIds)
{
    // 先写 KeyformBindingSource 需要的 grid id（自引用 _gridSource）
    const QString gridId = m_xml.AllocId();
    QVector<QString> bindingIds;
    bindingIds.reserve(chain.bindings.size());
    for (int i = 0; i < chain.bindings.size(); ++i) {
        bindingIds.append(m_xml.AllocId());
    }

    // KeyformBindingSource 族
    for (int i = 0; i < chain.bindings.size(); ++i) {
        const SBindingChain::SBinding& b = chain.bindings[i];
        m_xml.BeginObject(QStringLiteral("KeyformBindingSource"), QString(), bindingIds[i]);
        m_xml.Ref(QStringLiteral("KeyformGridSource"), QStringLiteral("_gridSource"), gridId);
        if (b.paramIndex >= 0) {
            m_xml.Ref(QStringLiteral("CParameterGuid"), QStringLiteral("parameterGuid"),
                      m_paramIds[b.paramIndex].guid);
        } else {
            m_xml.Null(QStringLiteral("parameterGuid"));
        }
        m_xml.BeginArrayList(QStringLiteral("keys"), b.keys.size());
        for (float key : b.keys) m_xml.F(QString(), key);
        m_xml.EndList();
        m_xml.Enum(QStringLiteral("InterpolationType"), QStringLiteral("interpolationType"),
                   QStringLiteral("LINEAR"));
        m_xml.EndObject();
    }

    // KeyformGridSource：keyform 总数 = 各绑定 key 数之积，行主序第一绑定变最快
    m_xml.BeginObject(QStringLiteral("KeyformGridSource"), QString(), gridId);
    m_xml.BeginArrayList(QStringLiteral("keyformsOnGrid"), formGuidIds.size());
    for (int formIdx = 0; formIdx < formGuidIds.size(); ++formIdx) {
        m_xml.BeginObject(QStringLiteral("KeyformOnGrid"));
        m_xml.BeginObject(QStringLiteral("KeyformGridAccessKey"), QStringLiteral("accessKey"));
        m_xml.BeginArrayList(QStringLiteral("_keyOnParameterList"), chain.bindings.size());
        int rem = formIdx;
        for (int b = 0; b < chain.bindings.size(); ++b) {
            const int keyCount = chain.bindings[b].keys.size();
            const int keyIndex = (keyCount > 0) ? rem % keyCount : 0;
            if (keyCount > 0) rem /= keyCount;
            m_xml.BeginObject(QStringLiteral("KeyOnParameter"));
            m_xml.Ref(QStringLiteral("KeyformBindingSource"), QStringLiteral("binding"), bindingIds[b]);
            m_xml.I(QStringLiteral("keyIndex"), keyIndex);
            m_xml.EndObject();
        }
        m_xml.EndList();
        m_xml.EndObject(); // accessKey
        m_xml.Ref(QStringLiteral("CFormGuid"), QStringLiteral("keyformGuid"), formGuidIds[formIdx]);
        m_xml.EndObject(); // KeyformOnGrid
    }
    m_xml.EndList();
    m_xml.BeginArrayList(QStringLiteral("keyformBindings"), bindingIds.size());
    for (const QString& id : bindingIds) {
        m_xml.Ref(QStringLiteral("KeyformBindingSource"), QString(), id);
    }
    m_xml.EndList();
    m_xml.EndObject(); // KeyformGridSource
    return gridId;
}

bool Generator::objectVisible(const QString& type, const QString& id, bool sourceVisible) const
{
    const auto value = m_opt.objectVisibility.constFind(type + QLatin1Char('|') + id);
    return value != m_opt.objectVisibility.cend() ? value.value() : sourceVisible && !m_opt.hiddenIds.contains(id);
}

void Generator::writeControllableHeader(const QString& localName, bool isVisible,
                                        const QString& parentPartGuidId, const QString& gridId,
                                        int extensionCount)
{
    m_xml.BeginObject(QStringLiteral("ACParameterControllableSource"), QStringLiteral("super"));
    m_xml.S(QStringLiteral("localName"), localName);
    m_xml.B(QStringLiteral("isVisible"), isVisible);
    m_xml.B(QStringLiteral("isLocked"), false);
    if (parentPartGuidId.isEmpty()) {
        m_xml.Null(QStringLiteral("parentGuid"));
    } else {
        m_xml.Ref(QStringLiteral("CPartGuid"), QStringLiteral("parentGuid"), parentPartGuidId);
    }
    m_xml.Ref(QStringLiteral("KeyformGridSource"), QStringLiteral("keyformGridSource"), gridId);
    m_xml.BeginCArrayList(QStringLiteral("_extensions"), extensionCount);
    // 调用方补扩展并收尾（EndList + EndObject）
}

void Generator::writePartEntities()
{
    m_partParentIndices.resize(m_table.parts.size());
    for (int i = 0; i < m_table.parts.size(); ++i) {
        const int parent = m_table.parts[i]->parent_part_indices;
        m_partParentIndices[i] = parent >= 0 && parent < m_table.parts.size() && parent != i ? parent : -1;
    }
    QVector<int> states(m_partParentIndices.size(), 0);
    for (int start = 0; start < m_partParentIndices.size(); ++start) {
        if (states[start] != 0) continue;
        QVector<int> path;
        int current = start;
        while (current >= 0 && states[current] == 0) {
            states[current] = 1;
            path.append(current);
            current = m_partParentIndices[current];
        }
        if (current >= 0 && states[current] == 1) {
            int member = current;
            do {
                const int parent = m_partParentIndices[member];
                m_partParentIndices[member] = -1;
                member = parent;
            } while (member != current);
        }
        for (int member : path) states[member] = 2;
    }
    // 先分配全部 id（父子引用需要）
    m_partIds.resize(m_table.parts.size());
    for (int i = 0; i < m_table.parts.size(); ++i) {
        m_partIds[i].source = m_xml.AllocId();
        m_partIds[i].guid = m_xml.AllocId();
        m_xml.GuidObject(QStringLiteral("CPartGuid"), NewGuid(), m_partIds[i].guid,
                         m_table.parts[i]->ids);
    }

    // 根 Part（moc3 没有显式根，综合无效父级的部件挂到虚拟根）
    m_rootPartSourceId = m_xml.AllocId();
    m_rootPartGuidId = m_xml.AllocId();
    m_rootPartFormGuidId = m_xml.AllocId();
    m_xml.GuidObject(QStringLiteral("CPartGuid"), NewGuid(), m_rootPartGuidId,
                     QStringLiteral("__RootPart__"));
    m_xml.GuidObject(QStringLiteral("CFormGuid"), NewGuid(), m_rootPartFormGuidId);

    // 子 guid 归属登记（drawable/deformer 写出时再补，先记 part 树结构）
    for (int i = 0; i < m_table.parts.size(); ++i) {
        const int parent = m_partParentIndices[i];
        if (parent >= 0 && parent < m_partIds.size()) {
            m_partIds[parent].childGuidRefs.append(m_partIds[i].guid);
            m_partIds[parent].childGuidTags.append(QStringLiteral("CPartGuid"));
        }
    }
}

void Generator::writeArtMeshEntities()
{
    m_meshGuidIds.resize(m_table.art_meshes.size());
    m_meshSourceIds.resize(m_table.art_meshes.size());

    for (int i = 0; i < m_table.art_meshes.size(); ++i) {
        const CArt_meshes* mesh = m_table.art_meshes[i];
        m_meshGuidIds[i] = m_xml.AllocId();
        m_xml.GuidObject(QStringLiteral("CDrawableGuid"), NewGuid(), m_meshGuidIds[i], mesh->ids);
    }

    for (int i = 0; i < m_table.art_meshes.size(); ++i) {
        const CArt_meshes* mesh = m_table.art_meshes[i];
        const int vertexCount = mesh->vertex_counts;
        const int keyformCount = mesh->keyform_sources_counts;
        const int layerIdx = m_layerOfMesh.value(i, -1);
        bool keyformPositionsValid = vertexCount > 0 && keyformCount > 0 &&
            validSlice(mesh->keyform_sources_begin_indices, keyformCount, m_table.art_mesh_keyforms.size());
        if (keyformPositionsValid) {
            for (int k = 0; k < keyformCount; ++k) {
                const CArt_mesh_keyforms* keyform = m_table.art_mesh_keyforms[
                    mesh->keyform_sources_begin_indices + k];
                if (!validSlice(keyform->keyform_position_sources_begin_indices, vertexCount * 2,
                                static_cast<int>(m_table.keyform_positions->xys.size()))) {
                    keyformPositionsValid = false;
                    break;
                }
            }
        }
        if (vertexCount <= 0 || vertexCount > 1000000 || keyformCount <= 0 ||
            !validSlice(mesh->keyform_sources_begin_indices, keyformCount, m_table.art_mesh_keyforms.size()) ||
            !validSlice(mesh->uv_sources_begin_indices, vertexCount * 2, static_cast<int>(m_table.uvs->uvs.size())) ||
            !validSlice(mesh->position_index_sources_begin_indices, mesh->position_index_sources_counts,
                        static_cast<int>(m_table.position_indices->indices.size())) || !keyformPositionsValid) {
            continue;
        }
        bool rawIndicesValid = mesh->position_index_sources_counts >= 3 &&
            mesh->position_index_sources_counts % 3 == 0;
        if (rawIndicesValid) {
            for (int t = 0; t < mesh->position_index_sources_counts; ++t) {
                const short index = m_table.position_indices->indices[
                    mesh->position_index_sources_begin_indices + t];
                if (index < 0 || index >= vertexCount || index > 32767) {
                    rawIndicesValid = false;
                    break;
                }
            }
        }
        if (!rawIndicesValid) continue;

        // keyform form guid 池
        QVector<QString> formGuidIds;
        formGuidIds.reserve(keyformCount);
        for (int k = 0; k < keyformCount; ++k) {
            const QString id = m_xml.AllocId();
            m_xml.GuidObject(QStringLiteral("CFormGuid"), NewGuid(), id);
            formGuidIds.append(id);
        }

        const SBindingChain chain = chainFor(mesh->keyform_binding_sources_indices);
        const QString gridId = writeKeyformGrid(chain, formGuidIds);
        const QString coordTypeId = (mesh->parent_deformer_indices >= 0)
                                        ? m_coordDeformerLocal : m_coordCanvas;

        m_meshSourceIds[i] = m_xml.AllocId();
        const QString texExtId = m_xml.AllocId();

        const QString textureInputId = layerIdx >= 0 ? m_xml.AllocId() : QString();
        if (layerIdx >= 0) {
            m_xml.BeginObject(QStringLiteral("CTextureInput_ModelImage"), QString(), textureInputId);
            m_xml.BeginObject(QStringLiteral("ACTextureInput"), QStringLiteral("super"));
            m_xml.BeginObject(QStringLiteral("CAffine"), QStringLiteral("optionalTransformOnCanvas"));
            m_xml.Attribute(QStringLiteral("m00"), QStringLiteral("1.0"));
            m_xml.Attribute(QStringLiteral("m01"), QStringLiteral("0.0"));
            m_xml.Attribute(QStringLiteral("m02"), QStringLiteral("0.0"));
            m_xml.Attribute(QStringLiteral("m10"), QStringLiteral("0.0"));
            m_xml.Attribute(QStringLiteral("m11"), QStringLiteral("1.0"));
            m_xml.Attribute(QStringLiteral("m12"), QStringLiteral("0.0"));
            m_xml.EndObject();
            m_xml.Ref(QStringLiteral("CTextureInputExtension"), QStringLiteral("_owner"), texExtId);
            m_xml.EndObject(); // ACTextureInput
            m_xml.Ref(QStringLiteral("CModelImageGuid"), QStringLiteral("_modelImageGuid"),
                      m_layers[layerIdx].modelImageGuidId);
            m_xml.EndObject(); // CTextureInput_ModelImage
        }

        // CTextureInputExtension（MODEL_IMAGE 单输入）
        m_xml.BeginObject(QStringLiteral("CTextureInputExtension"), QString(), texExtId);
        m_xml.BeginObject(QStringLiteral("ACExtension"), QStringLiteral("super"));
        m_xml.BeginObject(QStringLiteral("CExtensionGuid"), QStringLiteral("guid"));
        m_xml.Attribute(QStringLiteral("uuid"), NewGuid());
        m_xml.Attribute(QStringLiteral("note"), kNoDebug);
        m_xml.EndObject();
        m_xml.Ref(QStringLiteral("CArtMeshSource"), QStringLiteral("_owner"), m_meshSourceIds[i]);
        m_xml.EndObject(); // ACExtension
        m_xml.BeginCArrayList(QStringLiteral("_textureInputs"), (layerIdx >= 0) ? 1 : 0);
        if (layerIdx >= 0) m_xml.Ref(QStringLiteral("CTextureInput_ModelImage"), QString(), textureInputId);
        m_xml.EndList();
        if (layerIdx >= 0)
            m_xml.Ref(QStringLiteral("CTextureInput_ModelImage"), QStringLiteral("currentTextureInputData"), textureInputId);
        else
            m_xml.Null(QStringLiteral("currentTextureInputData"));
        m_xml.EndObject(); // CTextureInputExtension

        // CArtMeshSource
        m_xml.BeginObject(QStringLiteral("CArtMeshSource"), QString(), m_meshSourceIds[i]);
        m_xml.BeginObject(QStringLiteral("ACDrawableSource"), QStringLiteral("super"));
        writeControllableHeader(displayName(mesh->ids), objectVisible(QStringLiteral("artmesh"), mesh->ids, mesh->is_visible),
                                objectPartIndex(mesh->parent_part_indices) >= 0
                                    ? m_partIds[objectPartIndex(mesh->parent_part_indices)].guid : m_rootPartGuidId,
                                gridId, 1);
        m_xml.Ref(QStringLiteral("CTextureInputExtension"), QString(), texExtId);
        m_xml.EndList();   // _extensions
        m_xml.EndObject(); // ACParameterControllableSource
        m_xml.BeginObject(QStringLiteral("CDrawableId"), QStringLiteral("id"));
        m_xml.Attribute(QStringLiteral("idstr"), mesh->ids);
        m_xml.EndObject();
        m_xml.Ref(QStringLiteral("CDrawableGuid"), QStringLiteral("guid"), m_meshGuidIds[i]);
        m_xml.Ref(QStringLiteral("CDeformerGuid"), QStringLiteral("targetDeformerGuid"),
                  deformerTargetId(mesh->parent_deformer_indices));
        // 裁剪列表
        QVector<int> maskIndices;
        const auto maskOverrideIt = m_opt.drawableMaskIndexOverrides.constFind(mesh->ids);
        if (maskOverrideIt != m_opt.drawableMaskIndexOverrides.cend()) {
            maskIndices = maskOverrideIt.value();
        } else if (validSlice(mesh->drawable_mask_sources_begin_indices, mesh->drawable_mask_sources_counts,
                              m_table.drawable_masks.size())) {
            for (int mi = 0; mi < mesh->drawable_mask_sources_counts; ++mi) {
                maskIndices.append(m_table.drawable_masks[
                    mesh->drawable_mask_sources_begin_indices + mi]->art_mesh_sources_indices);
            }
        }
        m_xml.BeginCArrayList(QStringLiteral("clipGuidList"), maskIndices.size());
        for (int maskIdx : maskIndices) {
            if (maskIdx >= 0 && maskIdx < m_meshGuidIds.size()) {
                m_xml.Ref(QStringLiteral("CDrawableGuid"), QString(), m_meshGuidIds[maskIdx]);
            }
        }
        m_xml.EndList();
        const bool invertMask = m_opt.drawableMaskInverted.value(mesh->ids,
            CDrawableFlags::MaskInverted(mesh->drawable_flags));
        m_xml.B(QStringLiteral("invertClippingMask"), invertMask && !maskIndices.isEmpty());
        m_xml.EndObject(); // ACDrawableSource

        // indices
        QVector<int> indices;
        const auto overrideIt = m_opt.drawableIndexOverrides.constFind(mesh->ids);
        bool useOverride = overrideIt != m_opt.drawableIndexOverrides.cend() &&
            !overrideIt.value().isEmpty() && overrideIt.value().size() % 3 == 0;
        if (useOverride) {
            indices.reserve(overrideIt.value().size());
            for (quint16 index : overrideIt.value()) {
                if (index >= vertexCount || index > 32767) { useOverride = false; break; }
                indices.append(static_cast<int>(index));
            }
        }
        if (!useOverride) {
            indices.clear();
            indices.reserve(mesh->position_index_sources_counts);
            for (int t = 0; t < mesh->position_index_sources_counts; ++t) {
                indices.append(m_table.position_indices->indices[
                    mesh->position_index_sources_begin_indices + t]);
            }
        }
        m_xml.IntArray(QStringLiteral("indices"), indices);

        // keyforms
        m_xml.BeginCArrayList(QStringLiteral("keyforms"), keyformCount);
        for (int k = 0; k < keyformCount; ++k) {
            const CArt_mesh_keyforms* kf = m_table.art_mesh_keyforms[
                mesh->keyform_sources_begin_indices + k];
            m_xml.BeginObject(QStringLiteral("CArtMeshForm"));
            m_xml.BeginObject(QStringLiteral("ACDrawableForm"), QStringLiteral("super"));
            m_xml.BeginObject(QStringLiteral("ACForm"), QStringLiteral("super"));
            m_xml.Ref(QStringLiteral("CFormGuid"), QStringLiteral("guid"), formGuidIds[k]);
            m_xml.B(QStringLiteral("isAnimatedForm"), false);
            m_xml.B(QStringLiteral("isLocalAnimatedForm"), false);
            m_xml.Ref(QStringLiteral("CArtMeshSource"), QStringLiteral("_source"), m_meshSourceIds[i]);
            m_xml.Null(QStringLiteral("name"));
            m_xml.S(QStringLiteral("notes"), QString());
            m_xml.EndObject(); // ACForm
            m_xml.I(QStringLiteral("drawOrder"), m_opt.drawableDrawOrderOverrides.value(mesh->ids, qRound(kf->draw_orders)));
            m_xml.F(QStringLiteral("opacity"), m_opt.drawableOpacityOverrides.value(mesh->ids, kf->opacities));
            m_xml.Ref(QStringLiteral("CoordType"), QStringLiteral("coordType"), coordTypeId);
            m_xml.EndObject(); // ACDrawableForm
            // keyform positions：原始局部值（父变形器坐标系 / 挂根时为像素画布系換算）
            QVector<float> positions;
            positions.reserve(vertexCount * 2);
            const QVector<float> sourcePositions = meshKeyformPositions(mesh, k);
            const bool onCanvas = (mesh->parent_deformer_indices < 0);
            for (int v = 0; v < vertexCount; ++v) {
                const float mx = sourcePositions[v * 2];
                const float my = sourcePositions[v * 2 + 1];
                if (onCanvas) {
                    float px, py;
                    if (!rawCanvasToPixels(mesh->ids, mx, my, px, py)) return;
                    positions.append(px);
                    positions.append(py);
                } else {
                    positions.append(mx);
                    positions.append(my);
                }
            }
            m_xml.FloatArray(QStringLiteral("positions"), positions);
            m_xml.EndObject(); // CArtMeshForm
        }
        m_xml.EndList();

        m_xml.FloatArray(QStringLiteral("positions"), m_materialPositions[i]);
        m_xml.FloatArray(QStringLiteral("uvs"), m_materialUvs[i]);

        m_xml.Null(QStringLiteral("texture"));
        const int blendMode = CDrawableFlags::BlendMode(mesh->drawable_flags);
        const QString blend = (blendMode == 1) ? QStringLiteral("ADD")
                            : (blendMode == 2) ? QStringLiteral("MULTIPLY")
                                               : QStringLiteral("NORMAL");
        m_xml.Enum(QStringLiteral("ColorComposition"), QStringLiteral("colorComposition"), blend);
        m_xml.B(QStringLiteral("culling"), CDrawableFlags::Culling(mesh->drawable_flags));
        m_xml.Enum(QStringLiteral("TextureState"), QStringLiteral("textureState"),
                   QStringLiteral("MODEL_IMAGE"));
        m_xml.S(QStringLiteral("userData"), m_userData.value(mesh->ids));
        m_xml.EndObject(); // CArtMeshSource

        // 挂到父 part 的 childGuids
        const int parentPart = objectPartIndex(mesh->parent_part_indices);
        if (parentPart >= 0 && parentPart < m_partIds.size()) {
            m_partIds[parentPart].childGuidRefs.append(m_meshGuidIds[i]);
            m_partIds[parentPart].childGuidTags.append(QStringLiteral("CDrawableGuid"));
        }
    }
}

void Generator::writeWarpDeformerEntities()
{
    for (int d = 0; d < m_table.deformers.size(); ++d) {
        const CDeformers* deformer = m_table.deformers[d];
        if (deformer->types != 0) continue;
        const CWarp_deformers* warp = m_table.warp_deformers[deformer->specific_sources_indices];
        const int parentDeformer = effectiveDeformerParentIndex(d);

        if (warp->rows < 0 || warp->columns < 0 || warp->rows > 1000 || warp->columns > 1000) continue;
        const qint64 gridVertCount = (static_cast<qint64>(warp->columns) + 1) *
                                     (static_cast<qint64>(warp->rows) + 1);
        if (gridVertCount <= 0 || gridVertCount > 1000000) continue;
        const int gridVerts = static_cast<int>(gridVertCount);
        const int keyformCount = warp->keyform_sources_counts;
        if (gridVerts <= 0 || gridVerts > 1000000 ||
            !validSlice(warp->keyform_sources_begin_indices, keyformCount,
                        m_table.warp_deformer_keyforms.size())) continue;
        QVector<live2dstruct::CModelWarpKeyform> keyforms;
        const auto overrideIt = m_opt.deformerWarpKeyformOverrides.constFind(deformer->ids);
        if (overrideIt != m_opt.deformerWarpKeyformOverrides.cend() && overrideIt->size() == keyformCount) {
            bool overrideValid = true;
            for (const auto& keyform : overrideIt.value()) {
                if (keyform.positions.size() != gridVerts * 2 || !std::isfinite(keyform.opacity) ||
                    keyform.opacity < 0.0f || keyform.opacity > 1.0f) { overrideValid = false; break; }
                for (float position : keyform.positions) {
                    if (!std::isfinite(position)) { overrideValid = false; break; }
                }
                if (!overrideValid) break;
            }
            if (overrideValid) keyforms = overrideIt.value();
        }
        if (keyforms.isEmpty()) {
            for (int k = 0; k < keyformCount; ++k) {
                const CWarp_deformer_keyforms* keyform = m_table.warp_deformer_keyforms[
                    warp->keyform_sources_begin_indices + k];
                if (!validSlice(keyform->keyform_position_sources_begin_indices, gridVerts * 2,
                                static_cast<int>(m_table.keyform_positions->xys.size()))) continue;
                live2dstruct::CModelWarpKeyform converted;
                converted.opacity = keyform->opacities;
                converted.positions.reserve(gridVerts * 2);
                for (int v = 0; v < gridVerts; ++v) {
                    converted.positions.append(m_table.keyform_positions->GetData(
                        keyform->keyform_position_sources_begin_indices + v * 2));
                    converted.positions.append(m_table.keyform_positions->GetData(
                        keyform->keyform_position_sources_begin_indices + v * 2 + 1));
                }
                keyforms.append(converted);
            }
            if (keyforms.size() != keyformCount) continue;
        }
        QVector<QString> formGuidIds;
        for (int k = 0; k < keyformCount; ++k) {
            const QString id = m_xml.AllocId();
            m_xml.GuidObject(QStringLiteral("CFormGuid"), NewGuid(), id);
            formGuidIds.append(id);
        }
        const SBindingChain chain = chainFor(deformer->keyform_binding_sources_indices);
        const QString gridId = writeKeyformGrid(chain, formGuidIds);
        const QString coordTypeId = (parentDeformer >= 0)
                                        ? m_coordDeformerLocal : m_coordCanvas;

        m_deformerSourceIds[d] = m_xml.AllocId();
        m_xml.BeginObject(QStringLiteral("CWarpDeformerSource"), QString(), m_deformerSourceIds[d]);
        m_xml.BeginObject(QStringLiteral("ACDeformerSource"), QStringLiteral("super"));
        writeControllableHeader(displayName(deformer->ids), objectVisible(QStringLiteral("warp"), deformer->ids, deformer->is_visible),
                                objectPartIndex(deformer->parent_part_indices) >= 0
                                    ? m_partIds[objectPartIndex(deformer->parent_part_indices)].guid : m_rootPartGuidId,
                                gridId, 0);
        m_xml.EndList();   // _extensions
        m_xml.EndObject(); // ACParameterControllableSource
        m_xml.Ref(QStringLiteral("CDeformerGuid"), QStringLiteral("guid"), m_deformerIds[d].guid);
        m_xml.BeginObject(QStringLiteral("CDeformerId"), QStringLiteral("id"));
        m_xml.Attribute(QStringLiteral("idstr"), deformer->ids);
        m_xml.EndObject();
        m_xml.Ref(QStringLiteral("CDeformerGuid"), QStringLiteral("targetDeformerGuid"),
                  deformerTargetId(parentDeformer));
        m_xml.EndObject(); // ACDeformerSource

        m_xml.I(QStringLiteral("col"), warp->columns);
        m_xml.I(QStringLiteral("row"), warp->rows);
        m_xml.B(QStringLiteral("isQuadTransform"), warp->is_quad_source);

        m_xml.BeginCArrayList(QStringLiteral("keyforms"), keyformCount);
        for (int k = 0; k < keyformCount; ++k) {
            const auto& kf = keyforms[k];
            m_xml.BeginObject(QStringLiteral("CWarpDeformerForm"));
            m_xml.BeginObject(QStringLiteral("ACDeformerForm"), QStringLiteral("super"));
            m_xml.BeginObject(QStringLiteral("ACForm"), QStringLiteral("super"));
            m_xml.Ref(QStringLiteral("CFormGuid"), QStringLiteral("guid"), formGuidIds[k]);
            m_xml.B(QStringLiteral("isAnimatedForm"), false);
            m_xml.B(QStringLiteral("isLocalAnimatedForm"), false);
            m_xml.Ref(QStringLiteral("CWarpDeformerSource"), QStringLiteral("_source"), m_deformerSourceIds[d]);
            m_xml.Null(QStringLiteral("name"));
            m_xml.S(QStringLiteral("notes"), QString());
            m_xml.EndObject(); // ACForm
            m_xml.F(QStringLiteral("opacity"), kf.opacity);
            m_xml.Ref(QStringLiteral("CoordType"), QStringLiteral("coordType"), coordTypeId);
            m_xml.EndObject(); // ACDeformerForm

            QVector<float> positions;
            positions.reserve(gridVerts * 2);
            const bool onCanvas = (parentDeformer < 0);
            for (int v = 0; v < gridVerts; ++v) {
                const float mx = kf.positions[v * 2];
                const float my = kf.positions[v * 2 + 1];
                if (onCanvas) {
                    float px, py;
                    if (!rawCanvasToPixels(deformer->ids, mx, my, px, py)) return;
                    positions.append(px);
                    positions.append(py);
                } else {
                    positions.append(mx);
                    positions.append(my);
                }
            }
            m_xml.FloatArray(QStringLiteral("positions"), positions);
            m_xml.EndObject(); // CWarpDeformerForm
        }
        m_xml.EndList();
        m_xml.EndObject(); // CWarpDeformerSource

        const int parentPart = objectPartIndex(deformer->parent_part_indices);
        if (parentPart >= 0 && parentPart < m_partIds.size()) {
            m_partIds[parentPart].childGuidRefs.append(m_deformerIds[d].guid);
            m_partIds[parentPart].childGuidTags.append(QStringLiteral("CDeformerGuid"));
        }
    }
}

void Generator::writeRotationDeformerEntities()
{
    for (int d = 0; d < m_table.deformers.size(); ++d) {
        const CDeformers* deformer = m_table.deformers[d];
        if (deformer->types != 1) continue;
        const CRotation_deformers* rot = m_table.rotation_deformers[deformer->specific_sources_indices];
        const int parentDeformer = effectiveDeformerParentIndex(d);

        QVector<live2dstruct::CModelRotationKeyform> keyforms;
        const auto overrideIt = m_opt.deformerRotationKeyformOverrides.constFind(deformer->ids);
        if (overrideIt != m_opt.deformerRotationKeyformOverrides.cend() &&
            overrideIt->size() == rot->keyform_sources_counts) {
            keyforms = overrideIt.value();
        } else if (rot->keyform_sources_counts <= 0 ||
                   !validSlice(rot->keyform_sources_begin_indices, rot->keyform_sources_counts,
                               m_table.rotation_deformer_keyforms.size())) continue;
        if (keyforms.isEmpty()) {
            if (rot->keyform_sources_counts <= 0 || !validSlice(rot->keyform_sources_begin_indices,
                                                                 rot->keyform_sources_counts,
                                                                 m_table.rotation_deformer_keyforms.size())) continue;
            keyforms.reserve(rot->keyform_sources_counts);
            for (int k = 0; k < rot->keyform_sources_counts; ++k) {
                const CRotation_deformer_keyforms* sourceKeyform = m_table.rotation_deformer_keyforms[
                    rot->keyform_sources_begin_indices + k];
                keyforms.append({sourceKeyform->opacities, sourceKeyform->angles, sourceKeyform->origin_x,
                                 sourceKeyform->origin_y, sourceKeyform->scales, sourceKeyform->is_reflect_x,
                                 sourceKeyform->is_reflect_y});
            }
        }
        const int keyformCount = keyforms.size();
        QVector<QString> formGuidIds;
        for (int k = 0; k < keyformCount; ++k) {
            const QString id = m_xml.AllocId();
            m_xml.GuidObject(QStringLiteral("CFormGuid"), NewGuid(), id);
            formGuidIds.append(id);
        }
        const SBindingChain chain = chainFor(deformer->keyform_binding_sources_indices);
        const QString gridId = writeKeyformGrid(chain, formGuidIds);
        const QString coordTypeId = (parentDeformer >= 0)
                                        ? m_coordDeformerLocal : m_coordCanvas;

        m_deformerSourceIds[d] = m_xml.AllocId();
        m_xml.BeginObject(QStringLiteral("CRotationDeformerSource"), QString(), m_deformerSourceIds[d]);
        m_xml.BeginObject(QStringLiteral("ACDeformerSource"), QStringLiteral("super"));
        writeControllableHeader(displayName(deformer->ids), objectVisible(QStringLiteral("rotation"), deformer->ids, deformer->is_visible),
                                objectPartIndex(deformer->parent_part_indices) >= 0
                                    ? m_partIds[objectPartIndex(deformer->parent_part_indices)].guid : m_rootPartGuidId,
                                gridId, 0);
        m_xml.EndList();
        m_xml.EndObject();
        m_xml.Ref(QStringLiteral("CDeformerGuid"), QStringLiteral("guid"), m_deformerIds[d].guid);
        m_xml.BeginObject(QStringLiteral("CDeformerId"), QStringLiteral("id"));
        m_xml.Attribute(QStringLiteral("idstr"), deformer->ids);
        m_xml.EndObject();
        m_xml.Ref(QStringLiteral("CDeformerGuid"), QStringLiteral("targetDeformerGuid"),
                  deformerTargetId(parentDeformer));
        m_xml.EndObject(); // ACDeformerSource

        m_xml.B(QStringLiteral("useBoneUi_testImpl"), true);
        m_xml.BeginCArrayList(QStringLiteral("keyforms"), keyformCount);
        for (int k = 0; k < keyformCount; ++k) {
            const auto& kf = keyforms[k];
            const bool onCanvas = (parentDeformer < 0);
            float px = kf.originX;
            float py = kf.originY;
            if (onCanvas) {
                if (!rawCanvasToPixels(deformer->ids, kf.originX, kf.originY, px, py)) return;
            }
            const double scale = double(kf.scale) * (m_hasRotationAncestor[d] ? 1.0 : double(m_table.canvas_info->pixels_per_unit));
            if (!std::isfinite(scale) || std::abs(scale) > std::numeric_limits<float>::max()) {
                m_geometryError = QStringLiteral("旋转变形器的画布缩放超出 float 范围：%1").arg(deformer->ids);
                return;
            }
            m_xml.BeginObject(QStringLiteral("CRotationDeformerForm"));
            m_xml.FloatAttribute(QStringLiteral("angle"), kf.angle);
            m_xml.FloatAttribute(QStringLiteral("originX"), px);
            m_xml.FloatAttribute(QStringLiteral("originY"), py);
            m_xml.FloatAttribute(QStringLiteral("scale"), static_cast<float>(scale));
            m_xml.Attribute(QStringLiteral("isReflectX"),
                            kf.reflectX ? QStringLiteral("true") : QStringLiteral("false"));
            m_xml.Attribute(QStringLiteral("isReflectY"),
                            kf.reflectY ? QStringLiteral("true") : QStringLiteral("false"));
            m_xml.BeginObject(QStringLiteral("ACDeformerForm"), QStringLiteral("super"));
            m_xml.BeginObject(QStringLiteral("ACForm"), QStringLiteral("super"));
            m_xml.Ref(QStringLiteral("CFormGuid"), QStringLiteral("guid"), formGuidIds[k]);
            m_xml.B(QStringLiteral("isAnimatedForm"), false);
            m_xml.B(QStringLiteral("isLocalAnimatedForm"), false);
            m_xml.Ref(QStringLiteral("CRotationDeformerSource"), QStringLiteral("_source"),
                      m_deformerSourceIds[d]);
            m_xml.Null(QStringLiteral("name"));
            m_xml.S(QStringLiteral("notes"), QString());
            m_xml.EndObject();
            m_xml.F(QStringLiteral("opacity"), kf.opacity);
            m_xml.Ref(QStringLiteral("CoordType"), QStringLiteral("coordType"), coordTypeId);
            m_xml.EndObject(); // ACDeformerForm
            m_xml.EndObject(); // CRotationDeformerForm
        }
        m_xml.EndList();
        m_xml.F(QStringLiteral("handleLengthOnCanvas"), 512.0f);
        m_xml.F(QStringLiteral("circleRadiusOnCanvas"), 140.0f);
        m_xml.F(QStringLiteral("baseAngle"), rot->base_angles);
        m_xml.EndObject(); // CRotationDeformerSource

        const int parentPart = objectPartIndex(deformer->parent_part_indices);
        if (parentPart >= 0 && parentPart < m_partIds.size()) {
            m_partIds[parentPart].childGuidRefs.append(m_deformerIds[d].guid);
            m_partIds[parentPart].childGuidTags.append(QStringLiteral("CDeformerGuid"));
        }
    }
}

void Generator::writeGlueEntities()
{
    m_glueSourceIds.resize(m_table.glue.size());
    for (int g = 0; g < m_table.glue.size(); ++g) {
        const CGlue* glue = m_table.glue[g];
        const int keyformCount = glue->keyform_sources_counts;
        if (keyformCount <= 0 ||
            !validSlice(glue->keyform_sources_begin_indices, keyformCount, m_table.glue_keyforms.size()) ||
            !validSlice(glue->glue_info_sources_begin_indices, glue->glue_info_sources_counts,
                        m_table.glue_info.size())) continue;

        QVector<QString> formGuidIds;
        for (int k = 0; k < keyformCount; ++k) {
            const QString id = m_xml.AllocId();
            m_xml.GuidObject(QStringLiteral("CFormGuid"), NewGuid(), id);
            formGuidIds.append(id);
        }
        const SBindingChain chain = chainFor(glue->keyform_binding_sources_indices);
        const QString gridId = writeKeyformGrid(chain, formGuidIds);
        const QString affecterGuidId = m_xml.AllocId();
        m_xml.GuidObject(QStringLiteral("CAffecterGuid"), NewGuid(), affecterGuidId, glue->ids);

        m_glueSourceIds[g] = m_xml.AllocId();
        m_xml.BeginObject(QStringLiteral("CGlueSource"), QString(), m_glueSourceIds[g]);
        m_xml.BeginObject(QStringLiteral("ACAffecterSource"), QStringLiteral("super"));
        writeControllableHeader(QStringLiteral("Glue"), true, m_rootPartGuidId, gridId, 0);
        m_xml.EndList();
        m_xml.EndObject();
        m_xml.Ref(QStringLiteral("CAffecterGuid"), QStringLiteral("guid"), affecterGuidId);
        m_xml.BeginObject(QStringLiteral("CAffecterId"), QStringLiteral("id"));
        m_xml.Attribute(QStringLiteral("idstr"), glue->ids);
        m_xml.EndObject();
        m_xml.Ref(QStringLiteral("CDeformerGuid"), QStringLiteral("targetDeformerGuid"),
                  m_deformerRootGuidId);
        m_xml.I(QStringLiteral("editVersion"), 0);
        m_xml.EndObject(); // ACAffecterSource

        // weights：glue_info 表切片
        QVector<float> weights;
        for (int i = 0; i < glue->glue_info_sources_counts; ++i) {
            weights.append(m_table.glue_info[glue->glue_info_sources_begin_indices + i]->weights);
        }
        m_xml.FloatArray(QStringLiteral("weights"), weights);

        m_xml.BeginCArrayList(QStringLiteral("keyforms"), keyformCount);
        for (int k = 0; k < keyformCount; ++k) {
            const CGlue_keyforms* kf = m_table.glue_keyforms[glue->keyform_sources_begin_indices + k];
            m_xml.BeginObject(QStringLiteral("CGlueForm"));
            m_xml.BeginObject(QStringLiteral("ACAffecterForm"), QStringLiteral("super"));
            m_xml.BeginObject(QStringLiteral("ACForm"), QStringLiteral("super"));
            m_xml.Ref(QStringLiteral("CFormGuid"), QStringLiteral("guid"), formGuidIds[k]);
            m_xml.B(QStringLiteral("isAnimatedForm"), false);
            m_xml.B(QStringLiteral("isLocalAnimatedForm"), false);
            m_xml.Ref(QStringLiteral("CGlueSource"), QStringLiteral("_source"), m_glueSourceIds[g]);
            m_xml.Null(QStringLiteral("name"));
            m_xml.S(QStringLiteral("notes"), QString());
            m_xml.EndObject();
            m_xml.EndObject(); // ACAffecterForm
            m_xml.F(QStringLiteral("intensity"), kf->intensities);
            m_xml.EndObject(); // CGlueForm
        }
        m_xml.EndList();

        m_xml.Ref(QStringLiteral("CDrawableGuid"), QStringLiteral("targetArtMeshA_guid"),
                  m_meshGuidIds.value(glue->art_mesh_indices_a));
        m_xml.Ref(QStringLiteral("CDrawableGuid"), QStringLiteral("targetArtMeshB_guid"),
                  m_meshGuidIds.value(glue->art_mesh_indices_b));

        // bindVertexUids：GEditableMesh2 未生成时 Editor 按顶点下标解释 uid
        {
            QVector<qint64> uids;
            for (int i = 0; i < glue->glue_info_sources_counts; ++i) {
                uids.append(m_table.glue_info[glue->glue_info_sources_begin_indices + i]->position_indices);
            }
            m_xml.LongArray(QStringLiteral("bindVertexUids"), uids);
        }

        m_xml.BeginObject(QStringLiteral("GVector2"), QStringLiteral("tabPosOnCanvas"));
        m_xml.F(QStringLiteral("x"), 0.0f);
        m_xml.F(QStringLiteral("y"), 0.0f);
        m_xml.EndObject();
        m_xml.EndObject(); // CGlueSource
    }
}

void Generator::writeMain()
{
    m_xml.BeginMain();
    m_xml.BeginObject(QStringLiteral("CModelSource"));
    m_xml.Attribute(QStringLiteral("isDefaultKeyformLocked"), QStringLiteral("false"));

    m_xml.BeginObject(QStringLiteral("CModelGuid"), QStringLiteral("guid"));
    m_xml.Attribute(QStringLiteral("uuid"), NewGuid());
    m_xml.Attribute(QStringLiteral("note"), kNoDebug);
    m_xml.EndObject();
    m_xml.S(QStringLiteral("name"), m_opt.modelName);
    m_xml.BeginObject(QStringLiteral("EditorEdition"), QStringLiteral("editorEdition"));
    m_xml.I(QStringLiteral("edition"), 15);
    m_xml.EndObject();
    m_xml.BeginObject(QStringLiteral("CImageCanvas"), QStringLiteral("canvas"));
    m_xml.I(QStringLiteral("pixelWidth"), m_canvasW);
    m_xml.I(QStringLiteral("pixelHeight"), m_canvasH);
    m_xml.BeginObject(QStringLiteral("CColor"), QStringLiteral("background"));
    m_xml.EndObject();
    m_xml.EndObject();

    // 参数集
    m_xml.BeginObject(QStringLiteral("CParameterSourceSet"), QStringLiteral("parameterSourceSet"));
    m_xml.BeginCArrayList(QStringLiteral("_sources"), m_table.parameters.size());
    for (int i = 0; i < m_table.parameters.size(); ++i) {
        const CParameters* param = m_table.parameters[i];
        const auto metadata = parameterMetadata(param);
        m_xml.BeginObject(QStringLiteral("CParameterSource"));
        m_xml.I(QStringLiteral("decimalPlaces"), metadata.decimalPlaces);
        m_xml.Ref(QStringLiteral("CParameterGuid"), QStringLiteral("guid"), m_paramIds[i].guid);
        m_xml.F(QStringLiteral("snapEpsilon"), 0.001f);
        m_xml.F(QStringLiteral("minValue"), metadata.minimum);
        m_xml.F(QStringLiteral("maxValue"), metadata.maximum);
        const float defaultValue = m_opt.parameterDefaultMode == CCmo3Builder::ParameterDefaultMode::CurrentValues
            ? m_opt.parameterValues.value(param->ids, metadata.defaultValue) : metadata.defaultValue;
        m_xml.F(QStringLiteral("defaultValue"), defaultValue);
        m_xml.B(QStringLiteral("isRepeat"), metadata.repeat);
        m_xml.BeginObject(QStringLiteral("CParameterId"), QStringLiteral("id"));
        m_xml.Attribute(QStringLiteral("idstr"), param->ids);
        m_xml.EndObject();
        m_xml.Enum(QStringLiteral("Type"), QStringLiteral("paramType"),
                   (param->parameter_types == 1) ? QStringLiteral("BLEND_SHAPE")
                                                 : QStringLiteral("NORMAL"));
        m_xml.S(QStringLiteral("name"), parameterDisplayName(param->ids));
        m_xml.S(QStringLiteral("description"), QString());
        m_xml.B(QStringLiteral("combined"), false);
        m_xml.Null(QStringLiteral("parentGroupGuid"));
        m_xml.EndObject();
    }
    m_xml.EndList();
    m_xml.EndObject();

    // 纹理管理器（MODEL_IMAGE 模式）
    m_entities.WriteTextureManagerMain(m_layeredImage, m_modelImageGroupId);

    // drawable 集
    m_xml.BeginObject(QStringLiteral("CDrawableSourceSet"), QStringLiteral("drawableSourceSet"));
    m_xml.BeginCArrayList(QStringLiteral("_sources"), m_meshSourceIds.size());
    for (const QString& id : m_meshSourceIds) {
        m_xml.Ref(QStringLiteral("CArtMeshSource"), QString(), id);
    }
    m_xml.EndList();
    m_xml.EndObject();

    // 变形器集（moc3 deformers 表原序）
    {
        int total = 0;
        for (const QString& id : m_deformerSourceIds) if (!id.isEmpty()) ++total;
        m_xml.BeginObject(QStringLiteral("CDeformerSourceSet"), QStringLiteral("deformerSourceSet"));
        m_xml.BeginCArrayList(QStringLiteral("_sources"), total);
        for (int d = 0; d < m_table.deformers.size(); ++d) {
            const QString id = m_deformerSourceIds.value(d);
            if (id.isEmpty()) continue;
            const QString tag = m_table.deformers[d]->types == 0
                ? QStringLiteral("CWarpDeformerSource")
                : QStringLiteral("CRotationDeformerSource");
            m_xml.Ref(tag, QString(), id);
        }
        m_xml.EndList();
        m_xml.EndObject();
    }

    // affecter 集（glue）
    m_xml.BeginObject(QStringLiteral("CAffecterSourceSet"), QStringLiteral("affecterSourceSet"));
    m_xml.BeginCArrayList(QStringLiteral("_sources"), m_glueSourceIds.size());
    for (const QString& id : m_glueSourceIds) {
        if (!id.isEmpty()) m_xml.Ref(QStringLiteral("CGlueSource"), QString(), id);
    }
    m_xml.EndList();
    m_xml.EndObject();

    // part 集
    m_xml.BeginObject(QStringLiteral("CPartSourceSet"), QStringLiteral("partSourceSet"));
    m_xml.BeginCArrayList(QStringLiteral("_sources"), m_partIds.size() + 1);
    m_xml.Ref(QStringLiteral("CPartSource"), QString(), m_rootPartSourceId);
    for (const SPartIds& part : m_partIds) {
        m_xml.Ref(QStringLiteral("CPartSource"), QString(), part.source);
    }
    m_xml.EndList();
    m_xml.EndObject();

    // 物理设置（physics3.json 映射）
    writePhysicsMain();

    m_xml.Ref(QStringLiteral("CPartSource"), QStringLiteral("rootPart"), m_rootPartSourceId);

    // 模型信息
    m_xml.BeginObject(QStringLiteral("CModelInfo"), QStringLiteral("modelInfo"));
    m_xml.F(QStringLiteral("pixelsPerUnit"), m_table.canvas_info->pixels_per_unit);
    m_xml.BeginObject(QStringLiteral("CPoint"), QStringLiteral("originInPixels"));
    m_xml.I(QStringLiteral("x"), 0);
    m_xml.I(QStringLiteral("y"), 0);
    m_xml.EndObject();
    m_xml.BeginObject(QStringLiteral("CEffectParameterGroups"), QStringLiteral("_effectParameterGroups"));
    m_xml.BeginObject(QStringLiteral("hash_map"), QStringLiteral("_parameterGroups"));
    m_xml.Attribute(QStringLiteral("count"), QStringLiteral("0"));
    m_xml.EndObject();
    m_xml.EndObject();
    m_xml.EndObject();

    m_xml.I(QStringLiteral("targetVersionNo"), 3000);

    m_xml.EndObject(); // CModelSource
    m_xml.EndMain();
}

bool Generator::Run(QString* outError)
{
    const QSize canvasSize = CCanvasGeometry::PixelSize(m_table.canvas_info->width, m_table.canvas_info->height);
    if (canvasSize.isEmpty()) {
        if (outError) *outError = QStringLiteral("cmo3 画布像素尺寸无效");
        return false;
    }
    m_canvasW = canvasSize.width();
    m_canvasH = canvasSize.height();
    loadDisplayNames();

    if (!prepareDeformerUnits(outError) || !prepareMaterials(outError)) return false;

    // ---- 文档骨架 ----
    const QStringList versionPIs = {
        QStringLiteral("CArtMeshSource:4"),
        QStringLiteral("KeyformGridSource:1"),
        QStringLiteral("SerializeFormatVersion:2"),
        QStringLiteral("CModelSource:4"),
        QStringLiteral("CRotationDeformerForm:1"),
        QStringLiteral("CModelImage:1"),
    };
    const QStringList imports = {
        QStringLiteral("com.live2d.cubism.doc.gameData.physics.CPhysicsController$CPhysicsSourceType"),
        QStringLiteral("com.live2d.cubism.doc.gameData.physics.CPhysicsInput"),
        QStringLiteral("com.live2d.cubism.doc.gameData.physics.CPhysicsOutput"),
        QStringLiteral("com.live2d.cubism.doc.gameData.physics.CPhysicsSettingsSource"),
        QStringLiteral("com.live2d.cubism.doc.gameData.physics.CPhysicsSettingsSourceSet"),
        QStringLiteral("com.live2d.cubism.doc.gameData.physics.CPhysicsVertex"),
        QStringLiteral("com.live2d.cubism.doc.model.id.CPhysicsSettingId"),
        QStringLiteral("com.live2d.cubism.doc.model.ACForm"),
        QStringLiteral("com.live2d.cubism.doc.model.ACParameterControllableSource"),
        QStringLiteral("com.live2d.cubism.doc.model.CEffectParameterGroups"),
        QStringLiteral("com.live2d.cubism.doc.model.CModelInfo"),
        QStringLiteral("com.live2d.cubism.doc.model.CModelSource"),
        QStringLiteral("com.live2d.cubism.doc.model.EffectParameterType"),
        QStringLiteral("com.live2d.cubism.doc.model.affecter.ACAffecterForm"),
        QStringLiteral("com.live2d.cubism.doc.model.affecter.ACAffecterSource"),
        QStringLiteral("com.live2d.cubism.doc.model.affecter.CAffecterSourceSet"),
        QStringLiteral("com.live2d.cubism.doc.model.affecter.glue.CGlueForm"),
        QStringLiteral("com.live2d.cubism.doc.model.affecter.glue.CGlueSource"),
        QStringLiteral("com.live2d.cubism.doc.model.deformer.ACDeformerForm"),
        QStringLiteral("com.live2d.cubism.doc.model.deformer.ACDeformerSource"),
        QStringLiteral("com.live2d.cubism.doc.model.deformer.CDeformerSourceSet"),
        QStringLiteral("com.live2d.cubism.doc.model.deformer.rotation.CRotationDeformerForm"),
        QStringLiteral("com.live2d.cubism.doc.model.deformer.rotation.CRotationDeformerSource"),
        QStringLiteral("com.live2d.cubism.doc.model.deformer.warp.CWarpDeformerForm"),
        QStringLiteral("com.live2d.cubism.doc.model.deformer.warp.CWarpDeformerSource"),
        QStringLiteral("com.live2d.cubism.doc.model.drawable.ACDrawableForm"),
        QStringLiteral("com.live2d.cubism.doc.model.drawable.ACDrawableSource"),
        QStringLiteral("com.live2d.cubism.doc.model.drawable.CDrawableSourceSet"),
        QStringLiteral("com.live2d.cubism.doc.model.drawable.ColorComposition"),
        QStringLiteral("com.live2d.cubism.doc.model.drawable.TextureState"),
        QStringLiteral("com.live2d.cubism.doc.model.drawable.artMesh.CArtMeshForm"),
        QStringLiteral("com.live2d.cubism.doc.model.drawable.artMesh.CArtMeshSource"),
        QStringLiteral("com.live2d.cubism.doc.model.extension.ACExtension"),
        QStringLiteral("com.live2d.cubism.doc.model.extension.textureInput.ACTextureInput"),
        QStringLiteral("com.live2d.cubism.doc.model.extension.textureInput.CTextureInputExtension"),
        QStringLiteral("com.live2d.cubism.doc.model.extension.textureInput.CTextureInput_ModelImage"),
        QStringLiteral("com.live2d.cubism.doc.model.extension.textureInput.inputFilter.CLayerInputData"),
        QStringLiteral("com.live2d.cubism.doc.model.extension.textureInput.inputFilter.CLayerSelectorMap"),
        QStringLiteral("com.live2d.cubism.doc.model.extension.textureInput.inputFilter.ModelImageFilterEnv"),
        QStringLiteral("com.live2d.cubism.doc.model.extension.textureInput.inputFilter.ModelImageFilterSet"),
        QStringLiteral("com.live2d.cubism.doc.model.id.CAffecterId"),
        QStringLiteral("com.live2d.cubism.doc.model.id.CDeformerId"),
        QStringLiteral("com.live2d.cubism.doc.model.id.CDrawableId"),
        QStringLiteral("com.live2d.cubism.doc.model.id.CParameterId"),
        QStringLiteral("com.live2d.cubism.doc.model.id.CPartId"),
        QStringLiteral("com.live2d.cubism.doc.model.interpolator.InterpolationType"),
        QStringLiteral("com.live2d.cubism.doc.model.interpolator.KeyOnParameter"),
        QStringLiteral("com.live2d.cubism.doc.model.interpolator.KeyformBindingSource"),
        QStringLiteral("com.live2d.cubism.doc.model.interpolator.KeyformGridAccessKey"),
        QStringLiteral("com.live2d.cubism.doc.model.interpolator.KeyformGridSource"),
        QStringLiteral("com.live2d.cubism.doc.model.interpolator.KeyformOnGrid"),
        QStringLiteral("com.live2d.cubism.doc.model.param.CParameterSource"),
        QStringLiteral("com.live2d.cubism.doc.model.param.CParameterSource$Type"),
        QStringLiteral("com.live2d.cubism.doc.model.param.CParameterSourceSet"),
        QStringLiteral("com.live2d.cubism.doc.model.parts.CPartForm"),
        QStringLiteral("com.live2d.cubism.doc.model.parts.CPartSource"),
        QStringLiteral("com.live2d.cubism.doc.model.parts.CPartSourceSet"),
        QStringLiteral("com.live2d.cubism.doc.model.texture.CTextureManager"),
        QStringLiteral("com.live2d.cubism.doc.model.texture.LayeredImageWrapper"),
        QStringLiteral("com.live2d.cubism.doc.model.texture.TextureImageGroup"),
        QStringLiteral("com.live2d.graphics.cachedImage.CCachedImageManager"),
        QStringLiteral("com.live2d.graphics.cachedImage.CachedImageType"),
    };
    m_xml.BeginDocument(versionPIs, imports, QStringLiteral("402030000"));
    m_xml.BeginShared();

    // ---- shared 池 ----
    m_deformerRootGuidId = m_entities.WriteDeformerRootGuid();
    m_coordBasic = m_entities.WriteCoordType(QStringLiteral("Basic Coord"));
    m_coordDeformerLocal = m_entities.WriteCoordType(QStringLiteral("DeformerLocal"));
    m_coordCanvas = m_entities.WriteCoordType(QStringLiteral("Canvas"));
    m_filterGraph = m_entities.WriteFilterGraph();
    m_layeredImage = m_entities.WriteLayeredImage(m_opt.modelName + QStringLiteral(".psd"),
                                                  m_sourceCanvas.width(), m_sourceCanvas.height(), m_layers, outError);
    if (m_layeredImage.layeredImage.isEmpty()) return false;
    m_modelImageGroupId = m_entities.WriteModelImageGroup(m_filterGraph, m_layeredImage, m_layers);

    writeParameterGuids();
    writeDeformerGuids();
    writePartEntities();
    writeArtMeshEntities();
    if (!m_geometryError.isEmpty()) { if (outError) *outError = m_geometryError; return false; }
    writeWarpDeformerEntities();
    if (!m_geometryError.isEmpty()) { if (outError) *outError = m_geometryError; return false; }
    writeRotationDeformerEntities();
    if (!m_geometryError.isEmpty()) { if (outError) *outError = m_geometryError; return false; }
    writeGlueEntities();

    // ---- Part 源（childGuids 现已齐全） ----
    // 根 part
    {
        // 未挂任何 part 的顶层对象也归根
        QVector<QString> rootChildRefs;
        QVector<QString> rootChildTags;
        for (int i = 0; i < m_table.parts.size(); ++i) {
            if (partParentIndex(i) < 0) {
                rootChildRefs.append(m_partIds[i].guid);
                rootChildTags.append(QStringLiteral("CPartGuid"));
            }
        }
        for (int i = 0; i < m_table.art_meshes.size(); ++i) {
            if (objectPartIndex(m_table.art_meshes[i]->parent_part_indices) < 0) {
                rootChildRefs.append(m_meshGuidIds[i]);
                rootChildTags.append(QStringLiteral("CDrawableGuid"));
            }
        }
        for (int d = 0; d < m_table.deformers.size(); ++d) {
            if (objectPartIndex(m_table.deformers[d]->parent_part_indices) < 0) {
                rootChildRefs.append(m_deformerIds[d].guid);
                rootChildTags.append(QStringLiteral("CDeformerGuid"));
            }
        }

        m_xml.BeginObject(QStringLiteral("CPartSource"), QString(), m_rootPartSourceId);
        m_xml.BeginObject(QStringLiteral("ACParameterControllableSource"), QStringLiteral("super"));
        m_xml.S(QStringLiteral("localName"), QStringLiteral("Root Part"));
        m_xml.B(QStringLiteral("isVisible"), true);
        m_xml.B(QStringLiteral("isLocked"), true);
        m_xml.Null(QStringLiteral("parentGuid"));
        m_xml.BeginObject(QStringLiteral("KeyformGridSource"), QStringLiteral("keyformGridSource"));
        m_xml.BeginArrayList(QStringLiteral("keyformsOnGrid"), 1);
        m_xml.BeginObject(QStringLiteral("KeyformOnGrid"));
        m_xml.BeginObject(QStringLiteral("KeyformGridAccessKey"), QStringLiteral("accessKey"));
        m_xml.BeginArrayList(QStringLiteral("_keyOnParameterList"), 0);
        m_xml.EndList();
        m_xml.EndObject();
        m_xml.Ref(QStringLiteral("CFormGuid"), QStringLiteral("keyformGuid"), m_rootPartFormGuidId);
        m_xml.EndObject();
        m_xml.EndList();
        m_xml.BeginArrayList(QStringLiteral("keyformBindings"), 0);
        m_xml.EndList();
        m_xml.EndObject(); // KeyformGridSource
        m_xml.BeginCArrayList(QStringLiteral("_extensions"), 0);
        m_xml.EndList();
        m_xml.EndObject(); // ACParameterControllableSource
        m_xml.Ref(QStringLiteral("CPartGuid"), QStringLiteral("guid"), m_rootPartGuidId);
        m_xml.BeginObject(QStringLiteral("CPartId"), QStringLiteral("id"));
        m_xml.Attribute(QStringLiteral("idstr"), QStringLiteral("__RootPart__"));
        m_xml.EndObject();
        m_xml.B(QStringLiteral("enableDrawOrderGroup"), false);
        m_xml.I(QStringLiteral("defaultOrder_forEditor"), 500);
        m_xml.B(QStringLiteral("isSketch"), false);
        m_xml.BeginObject(QStringLiteral("CColor"), QStringLiteral("partsEditColor"));
        m_xml.EndObject();
        m_xml.BeginCArrayList(QStringLiteral("_childGuids"), rootChildRefs.size());
        for (int c = 0; c < rootChildRefs.size(); ++c) {
            m_xml.Ref(rootChildTags[c], QString(), rootChildRefs[c]);
        }
        m_xml.EndList();
        m_xml.Ref(QStringLiteral("CDeformerGuid"), QStringLiteral("targetDeformerGuid"),
                  m_deformerRootGuidId);
        m_xml.BeginCArrayList(QStringLiteral("keyforms"), 1);
        m_xml.BeginObject(QStringLiteral("CPartForm"));
        m_xml.BeginObject(QStringLiteral("ACForm"), QStringLiteral("super"));
        m_xml.Ref(QStringLiteral("CFormGuid"), QStringLiteral("guid"), m_rootPartFormGuidId);
        m_xml.B(QStringLiteral("isAnimatedForm"), false);
        m_xml.B(QStringLiteral("isLocalAnimatedForm"), false);
        m_xml.Ref(QStringLiteral("CPartSource"), QStringLiteral("_source"), m_rootPartSourceId);
        m_xml.Null(QStringLiteral("name"));
        m_xml.S(QStringLiteral("notes"), QString());
        m_xml.EndObject();
        m_xml.I(QStringLiteral("drawOrder"), 500);
        m_xml.EndObject();
        m_xml.EndList();
        m_xml.EndObject(); // CPartSource
    }

    // 普通 part
    for (int i = 0; i < m_table.parts.size(); ++i) {
        const CParts* part = m_table.parts[i];
        const bool hasPartKeyforms = part->keyform_sources_counts > 0 &&
            validSlice(part->keyform_sources_begin_indices, part->keyform_sources_counts,
                       m_table.part_keyforms.size());
        const int keyformCount = hasPartKeyforms ? part->keyform_sources_counts : 1;
        QVector<QString> formGuidIds;
        for (int k = 0; k < keyformCount; ++k) {
            const QString id = m_xml.AllocId();
            m_xml.GuidObject(QStringLiteral("CFormGuid"), NewGuid(), id);
            formGuidIds.append(id);
        }
        const SBindingChain chain = chainFor(part->keyform_binding_sources_indices);
        const QString gridId = writeKeyformGrid(chain, formGuidIds);

        m_xml.BeginObject(QStringLiteral("CPartSource"), QString(), m_partIds[i].source);
        writeControllableHeader(displayName(part->ids), objectVisible(QStringLiteral("part"), part->ids, part->is_visible),
                                partParentIndex(i) >= 0
                                    ? m_partIds[partParentIndex(i)].guid : m_rootPartGuidId,
                                gridId, 0);
        m_xml.EndList();
        m_xml.EndObject(); // ACParameterControllableSource
        m_xml.Ref(QStringLiteral("CPartGuid"), QStringLiteral("guid"), m_partIds[i].guid);
        m_xml.BeginObject(QStringLiteral("CPartId"), QStringLiteral("id"));
        m_xml.Attribute(QStringLiteral("idstr"), part->ids);
        m_xml.EndObject();
        m_xml.B(QStringLiteral("enableDrawOrderGroup"), false);
        m_xml.I(QStringLiteral("defaultOrder_forEditor"), 500);
        m_xml.B(QStringLiteral("isSketch"), false);
        m_xml.BeginObject(QStringLiteral("CColor"), QStringLiteral("partsEditColor"));
        m_xml.EndObject();
        m_xml.BeginCArrayList(QStringLiteral("_childGuids"), m_partIds[i].childGuidRefs.size());
        for (int c = 0; c < m_partIds[i].childGuidRefs.size(); ++c) {
            m_xml.Ref(m_partIds[i].childGuidTags[c], QString(), m_partIds[i].childGuidRefs[c]);
        }
        m_xml.EndList();
        m_xml.Ref(QStringLiteral("CDeformerGuid"), QStringLiteral("targetDeformerGuid"),
                  m_deformerRootGuidId);
        m_xml.BeginCArrayList(QStringLiteral("keyforms"), keyformCount);
        for (int k = 0; k < keyformCount; ++k) {
            int drawOrder = 500;
            if (hasPartKeyforms) {
                drawOrder = qRound(m_table.part_keyforms[
                    part->keyform_sources_begin_indices + k]->draw_orders);
            }
            m_xml.BeginObject(QStringLiteral("CPartForm"));
            m_xml.BeginObject(QStringLiteral("ACForm"), QStringLiteral("super"));
            m_xml.Ref(QStringLiteral("CFormGuid"), QStringLiteral("guid"), formGuidIds[k]);
            m_xml.B(QStringLiteral("isAnimatedForm"), false);
            m_xml.B(QStringLiteral("isLocalAnimatedForm"), false);
            m_xml.Ref(QStringLiteral("CPartSource"), QStringLiteral("_source"), m_partIds[i].source);
            m_xml.Null(QStringLiteral("name"));
            m_xml.S(QStringLiteral("notes"), QString());
            m_xml.EndObject();
            m_xml.I(QStringLiteral("drawOrder"), drawOrder);
            m_xml.EndObject();
        }
        m_xml.EndList();
        m_xml.EndObject(); // CPartSource
    }

    m_xml.EndShared();

    // ---- main ----
    writeMain();
    m_xml.EndDocument();

    if (m_xml.HasError()) {
        if (outError) *outError = m_xml.NumberError().isEmpty()
            ? QStringLiteral("cmo3 XML 写出失败，请检查名称、标签等字段中的非法字符")
            : QStringLiteral("cmo3 数值写出失败：%1").arg(m_xml.NumberError());
        return false;
    }

    // ---- CAFF 打包 ----
    m_caff.AddEntry(QStringLiteral("main.xml"), m_xml.Result(),
                    QStringLiteral("main_xml"), CCaffArchive::CompressFast);
    return m_caff.Save(m_opt.outPath, outError);
}

} // namespace

bool CCmo3Builder::PreparePhysicsExport(const Options& options, PhysicsExport& result, QString* error)
{
    if (error) error->clear();
    PhysicsExport candidate;
    candidate.present = options.physicsBytes.has_value() || !options.physicsPath.isEmpty();
    if (options.physicsBytes) {
        if (!validatePhysicsBytes(*options.physicsBytes, candidate.document, error)) return false;
    } else if (!options.physicsPath.isEmpty() && !validatePhysicsFile(options.physicsPath, candidate.document, error)) return false;
    const auto meta = candidate.document.value(QStringLiteral("Meta")).toObject();
    const auto fps = meta.value(QStringLiteral("Fps"));
    const float runtimeFps = static_cast<float>(fps.toDouble());
    if (fps.isDouble() && std::floor(runtimeFps) != runtimeFps) {
        const int converted = static_cast<int>(runtimeFps);
        auto message = QStringLiteral("计算 FPS %1 将按 Cubism 导入规则转换为整数 %2。")
            .arg(QString::number(fps.toDouble(), 'g', 12)).arg(converted);
        if (!converted) message += QStringLiteral("0 表示使用实际帧间隔。");
        candidate.conversions.append(message);
    }
    const auto forces = meta.value(QStringLiteral("EffectiveForces")).toObject();
    const auto gravity = forces.value(QStringLiteral("Gravity")).toObject();
    const auto wind = forces.value(QStringLiteral("Wind")).toObject();
    if (static_cast<float>(gravity.value(QStringLiteral("X")).toDouble()) != 0 ||
        static_cast<float>(gravity.value(QStringLiteral("Y")).toDouble(-1)) != -1 ||
        static_cast<float>(wind.value(QStringLiteral("X")).toDouble()) != 0 ||
        static_cast<float>(wind.value(QStringLiteral("Y")).toDouble()) != 0)
        candidate.conversions.append(QStringLiteral("Cubism 工程不保存当前重力和风力；重新打开后会使用编辑器默认力场。"));
    qsizetype convertedOutputs = 0;
    for (const auto& group : candidate.document.value(QStringLiteral("PhysicsSettings")).toArray())
        for (const auto& output : group.toObject().value(QStringLiteral("Output")).toArray())
            if (output.toObject().value(QStringLiteral("Type")).toString() != QLatin1String("Angle")) ++convertedOutputs;
    if (convertedOutputs)
        candidate.conversions.append(QStringLiteral("%1 项 X/Y 输出将转换为 Angle 并保留倍率，运动结果可能不同。")
            .arg(convertedOutputs));
    result = std::move(candidate);
    return true;
}

bool CCmo3Builder::Build(const Options& options, QString* outError)
{
    if (outError) outError->clear();
    if (options.parameterDefaultMode != ParameterDefaultMode::ModelDefinition &&
        options.parameterDefaultMode != ParameterDefaultMode::CurrentValues) {
        if (outError) *outError = QStringLiteral("cmo3 参数默认值导出模式无效");
        return false;
    }
    Options opt = options;
    if (opt.modelName.isEmpty()) {
        opt.modelName = QFileInfo(opt.mocPath).completeBaseName();
    }

    CReader reader = opt.mocBytes.isEmpty() ? CReader(opt.mocPath) : CReader::FromBytes(opt.mocBytes);
    CSectionTable table(reader);
    if (!table.IsValid() || !table.count_info || !table.canvas_info || !table.uvs ||
        !table.position_indices || !table.keyform_positions || !table.keys) {
        if (outError) *outError = QStringLiteral("moc3 解析失败: %1").arg(reader.Error().isEmpty() ? opt.mocPath : reader.Error());
        return false;
    }
    QString canvasError;
    if (!CCanvasGeometry::Validate(table.canvas_info, &canvasError)) {
        if (outError) *outError = QStringLiteral("cmo3 画布无效：%1").arg(canvasError);
        return false;
    }

    if (!validateDeformerSources(table, outError) || !validateParameterOptions(table, opt, outError)) return false;

    PhysicsExport physics;
    if (!PreparePhysicsExport(opt, physics, outError)) return false;
    QSet<QString> parameterIds;
    for (const auto* parameter : table.parameters) parameterIds.insert(parameter->ids);
    if (!validatePhysicsParameters(physics.document, parameterIds, outError)) return false;
    if (!physics.conversions.isEmpty() && !opt.allowPhysicsConversion) {
        if (outError) *outError = QStringLiteral("cmo3 物理导出需要确认以下转换：\n%1").arg(physics.conversions.join(QLatin1Char('\n')));
        return false;
    }

    QHash<QString, QString> userData;
    if (!opt.userDataPath.isEmpty()) {
        CModelUserData document;
        QString userDataError;
        if (!document.Load(opt.userDataPath, &userDataError)) {
            if (outError) *outError = QStringLiteral("UserData 读取失败：%1").arg(userDataError);
            return false;
        }
        QSet<QString> meshIds;
        for (const auto* mesh : table.art_meshes) meshIds.insert(mesh->ids);
        for (const auto& entry : document.Entries()) {
            if (!meshIds.contains(entry.id)) {
                if (outError) *outError = QStringLiteral("UserData 引用了不存在的 ArtMesh：%1").arg(entry.id);
                return false;
            }
            userData.insert(entry.id, entry.value);
        }
    }

    // 纹理图集定位：moc3 同目录 model3.json 或 textures/ 推断
    QVector<QString> atlasPaths;
    {
        const QDir dir = QFileInfo(opt.mocPath).dir();
        const bool hasModelConfig = !dir.entryList({QStringLiteral("*.model3.json")}, QDir::Files).isEmpty();
        const bool loadedModelConfig = loadModelTextures(dir, opt.mocPath, &atlasPaths);
        if (atlasPaths.isEmpty() && !hasModelConfig) {
            QDir texDir(dir.filePath(QStringLiteral("textures")));
            if (!texDir.exists()) texDir = dir;
            const QStringList pngs = texDir.entryList({ QStringLiteral("*.png") }, QDir::Files, QDir::Name);
            for (const QString& png : pngs) atlasPaths.append(texDir.filePath(png));
        }
        if (hasModelConfig && !loadedModelConfig) atlasPaths.clear();
    }
    if (atlasPaths.isEmpty()) {
        if (outError) *outError = QStringLiteral("找不到纹理图集 PNG");
        return false;
    }

    CTextureExtractor extractor;
    if (!extractor.LoadAtlases(atlasPaths)) {
        if (outError) *outError = QStringLiteral("纹理图集加载失败");
        return false;
    }
    QVector<ExtractedMaterial> extracted;
    if (!extractor.ExtractMaterials(&table, extracted, outError)) return false;

    Generator generator(opt, table, extracted, userData, physics.document);
    return generator.Run(outError);
}

} // namespace cmo3
