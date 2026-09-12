#include "ccmo3builder.h"
#include "ccaffarchive.h"
#include "ccmo3entities.h"
#include "ccmo3ids.h"
#include "cxsxmlwriter.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/util/cfilereader.h"
#include "opengl/ctextureextractor.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <cmath>

using namespace live2dstruct;

namespace cmo3 {

namespace {

const QString kNoDebug = QStringLiteral("(no debug info)");

bool validSlice(int begin, int count, int size)
{
    return begin >= 0 && count >= 0 && begin <= size && count <= size - begin;
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
              QVector<ExtractedLayer>& extracted)
        : m_opt(options), m_table(table), m_extracted(extracted),
          m_entities(m_xml, m_caff)
    {
    }

    bool Run(QString* outError);

private:
    // ---- 数据准备 ----
    SBindingChain chainFor(int keyformBindingSourceIndex) const;
    void loadDisplayNames();

    // ---- shared 池写出 ----
    void writeParameterGuids();
    void writeDeformerGuids();
    void writePartEntities();
    void writeArtMeshEntities();
    void writeWarpDeformerEntities();
    void writeRotationDeformerEntities();
    void writeGlueEntities();

    // ---- 复用块 ----
    // KeyformGridSource + KeyformBindingSource 族；返回 gridSource 的 xs.id
    QString writeKeyformGrid(const SBindingChain& chain, const QVector<QString>& formGuidIds);
    void writeControllableHeader(const QString& localName, bool isVisible,
                                 const QString& parentPartGuidId, const QString& gridId,
                                 int extensionCount);
    QString deformerTargetId(int parentDeformerIndex) const;

    // ---- main 段 ----
    void writeMain();
    void writePhysicsMain();

    QString displayName(const QString& id) const
    {
        return m_displayNames.value(id, id);
    }

    const CCmo3Builder::Options& m_opt;
    CSectionTable& m_table;
    QVector<ExtractedLayer>& m_extracted;

    CXsXmlWriter m_xml;
    CCaffArchive m_caff;
    CCmo3Entities m_entities;

    // xs.id 索引（与 moc3 各表下标对齐）
    QVector<SParamIds> m_paramIds;
    QVector<SPartIds> m_partIds;
    QVector<SDeformerIds> m_deformerIds;        // 与 deformers 表对齐
    QVector<QString> m_meshGuidIds;             // 与 art_meshes 表对齐
    QVector<QString> m_meshSourceIds;           // CArtMeshSource xs.id
    QVector<QString> m_warpSourceIds;           // 与 warp_deformers 表对齐（经 deformers 反查）
    QVector<QString> m_rotationSourceIds;
    QVector<QString> m_glueSourceIds;
    QVector<int> m_deformerIndexOfWarp;         // warp 表下标 → deformers 表下标
    QVector<int> m_deformerIndexOfRotation;
    QVector<SLayerData> m_layers;               // 与可见提取层对齐
    QVector<int> m_layerOfMesh;                 // art_meshes 下标 → m_layers 下标（-1 无贴图）

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
    QHash<QString, int> m_paramIndexById;       // 参数 idstr → parameters 表下标
    int m_canvasW = 0;
    int m_canvasH = 0;
};

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
            if (!id.isEmpty() && !name.isEmpty()) m_displayNames.insert(id, name);
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
    QJsonArray settings;
    if (!m_opt.physicsPath.isEmpty()) {
        QFile file(m_opt.physicsPath);
        if (file.open(QIODevice::ReadOnly)) {
            settings = QJsonDocument::fromJson(file.readAll()).object()
                           .value(QStringLiteral("PhysicsSettings")).toArray();
            file.close();
        }
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
        m_xml.BeginObject(QStringLiteral("CPhysicsSettingsSource"));
        m_xml.S(QStringLiteral("name"), setting.value(QStringLiteral("Id")).toString());
        m_xml.BeginObject(QStringLiteral("CPhysicsSettingsGuid"), QStringLiteral("guid"));
        m_xml.Attribute(QStringLiteral("uuid"), NewGuid());
        m_xml.Attribute(QStringLiteral("note"), kNoDebug);
        m_xml.EndObject();
        m_xml.BeginObject(QStringLiteral("CPhysicsSettingId"), QStringLiteral("id"));
        m_xml.Attribute(QStringLiteral("idstr"), setting.value(QStringLiteral("Id")).toString());
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
            m_xml.Enum(QStringLiteral("CPhysicsSourceType"), QStringLiteral("type"),
                       srcType(output.value(QStringLiteral("Type")).toString()));
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
        m_xml.EndObject(); // CPhysicsSettingsSource
    }
    m_xml.EndList();
    m_xml.EndObject(); // CPhysicsSettingsSourceSet
}

void Generator::writeDeformerGuids()
{
    m_deformerIds.resize(m_table.deformers.size());
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
    // 先分配全部 id（父子引用需要）
    m_partIds.resize(m_table.parts.size());
    for (int i = 0; i < m_table.parts.size(); ++i) {
        m_partIds[i].source = m_xml.AllocId();
        m_partIds[i].guid = m_xml.AllocId();
        m_xml.GuidObject(QStringLiteral("CPartGuid"), NewGuid(), m_partIds[i].guid,
                         m_table.parts[i]->ids);
    }

    // 根 Part（moc3 没有显式根，综合所有 parent==-1 的部件挂到虚拟根）
    m_rootPartSourceId = m_xml.AllocId();
    m_rootPartGuidId = m_xml.AllocId();
    m_rootPartFormGuidId = m_xml.AllocId();
    m_xml.GuidObject(QStringLiteral("CPartGuid"), NewGuid(), m_rootPartGuidId,
                     QStringLiteral("__RootPart__"));
    m_xml.GuidObject(QStringLiteral("CFormGuid"), NewGuid(), m_rootPartFormGuidId);

    // 子 guid 归属登记（drawable/deformer 写出时再补，先记 part 树结构）
    for (int i = 0; i < m_table.parts.size(); ++i) {
        const int parent = m_table.parts[i]->parent_part_indices;
        if (parent >= 0 && parent < m_partIds.size()) {
            m_partIds[parent].childGuidRefs.append(m_partIds[i].guid);
            m_partIds[parent].childGuidTags.append(QStringLiteral("CPartGuid"));
        }
    }
}

void Generator::writeArtMeshEntities()
{
    const CCanvas_info* canvas = m_table.canvas_info;
    const float ppu = canvas->pixels_per_unit;
    const float ox = canvas->origin_x;
    const float oy = canvas->origin_y;

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
        if (vertexCount <= 0 || vertexCount > 1000000 || keyformCount <= 0 ||
            !validSlice(mesh->keyform_sources_begin_indices, keyformCount, m_table.art_mesh_keyforms.size()) ||
            !validSlice(mesh->uv_sources_begin_indices, vertexCount * 2, static_cast<int>(m_table.uvs->uvs.size())) ||
            !validSlice(mesh->position_index_sources_begin_indices, mesh->position_index_sources_counts,
                        static_cast<int>(m_table.position_indices->indices.size()))) {
            continue;
        }

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
        if (layerIdx >= 0) {
            m_xml.BeginObject(QStringLiteral("CTextureInput_ModelImage"));
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
        m_xml.EndList();
        if (layerIdx >= 0) {
            // currentTextureInputData 需要指向同一 CTextureInput_ModelImage；
            // xs 序列化不支持列表元素引用，内联重复一份等价对象
            m_xml.BeginObject(QStringLiteral("CTextureInput_ModelImage"),
                              QStringLiteral("currentTextureInputData"));
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
            m_xml.EndObject();
            m_xml.Ref(QStringLiteral("CModelImageGuid"), QStringLiteral("_modelImageGuid"),
                      m_layers[layerIdx].modelImageGuidId);
            m_xml.EndObject();
        } else {
            m_xml.Null(QStringLiteral("currentTextureInputData"));
        }
        m_xml.EndObject(); // CTextureInputExtension

        // CArtMeshSource
        m_xml.BeginObject(QStringLiteral("CArtMeshSource"), QString(), m_meshSourceIds[i]);
        m_xml.BeginObject(QStringLiteral("ACDrawableSource"), QStringLiteral("super"));
        writeControllableHeader(displayName(mesh->ids), mesh->is_visible && !m_opt.hiddenIds.contains(mesh->ids),
                                (mesh->parent_part_indices >= 0)
                                    ? m_partIds[mesh->parent_part_indices].guid : m_rootPartGuidId,
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
        if (validSlice(mesh->drawable_mask_sources_begin_indices, mesh->drawable_mask_sources_counts,
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
        m_xml.B(QStringLiteral("invertClippingMask"), false);
        m_xml.EndObject(); // ACDrawableSource

        // indices
        QVector<int> indices;
        indices.reserve(mesh->position_index_sources_counts);
        for (int t = 0; t < mesh->position_index_sources_counts; ++t) {
            indices.append(m_table.position_indices->indices[
                mesh->position_index_sources_begin_indices + t]);
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
            m_xml.I(QStringLiteral("drawOrder"), qRound(kf->draw_orders));
            m_xml.F(QStringLiteral("opacity"), kf->opacities);
            m_xml.Ref(QStringLiteral("CoordType"), QStringLiteral("coordType"), coordTypeId);
            m_xml.EndObject(); // ACDrawableForm
            // keyform positions：原始局部值（父变形器坐标系 / 挂根时为像素画布系換算）
            QVector<float> positions;
            positions.reserve(vertexCount * 2);
            const int posBegin = kf->keyform_position_sources_begin_indices;
            const bool onCanvas = (mesh->parent_deformer_indices < 0);
            for (int v = 0; v < vertexCount; ++v) {
                const float mx = m_table.keyform_positions->GetData(posBegin + v * 2);
                const float my = m_table.keyform_positions->GetData(posBegin + v * 2 + 1);
                if (onCanvas) {
                    positions.append((mx + ox) * ppu);
                    positions.append((oy - my) * ppu);
                } else {
                    positions.append(mx);
                    positions.append(my);
                }
            }
            m_xml.FloatArray(QStringLiteral("positions"), positions);
            m_xml.EndObject(); // CArtMeshForm
        }
        m_xml.EndList();

        // 顶层 positions（默认 keyform，画布像素坐标）
        {
            const CArt_mesh_keyforms* kf0 = m_table.art_mesh_keyforms[
                mesh->keyform_sources_begin_indices];
            QVector<float> pixelPositions;
            pixelPositions.reserve(vertexCount * 2);
            const int posBegin = kf0->keyform_position_sources_begin_indices;
            for (int v = 0; v < vertexCount; ++v) {
                const float mx = m_table.keyform_positions->GetData(posBegin + v * 2);
                const float my = m_table.keyform_positions->GetData(posBegin + v * 2 + 1);
                pixelPositions.append((mx + ox) * ppu);
                pixelPositions.append((oy - my) * ppu);
            }
            m_xml.FloatArray(QStringLiteral("positions"), pixelPositions);
        }

        // uvs：原始 moc3 v 值（Editor 用同一约定，不做 1-v 翻转）
        {
            QVector<float> uvs;
            uvs.reserve(vertexCount * 2);
            const float* uvPtr = &m_table.uvs->uvs[mesh->uv_sources_begin_indices];
            for (int v = 0; v < vertexCount * 2; ++v) uvs.append(uvPtr[v]);
            m_xml.FloatArray(QStringLiteral("uvs"), uvs);
        }

        m_xml.Null(QStringLiteral("texture"));
        // 混合模式：drawable_flags bits 0-1
        const int blendBits = mesh->drawable_flags & 0x3;
        const QString blend = (blendBits == 1) ? QStringLiteral("ADD")
                            : (blendBits == 2) ? QStringLiteral("MULTIPLY")
                                               : QStringLiteral("NORMAL");
        m_xml.Enum(QStringLiteral("ColorComposition"), QStringLiteral("colorComposition"), blend);
        m_xml.B(QStringLiteral("culling"), !(mesh->drawable_flags & 0x4));
        m_xml.Enum(QStringLiteral("TextureState"), QStringLiteral("textureState"),
                   QStringLiteral("MODEL_IMAGE"));
        m_xml.S(QStringLiteral("userData"), QString());
        m_xml.EndObject(); // CArtMeshSource

        // 挂到父 part 的 childGuids
        const int parentPart = mesh->parent_part_indices;
        if (parentPart >= 0 && parentPart < m_partIds.size()) {
            m_partIds[parentPart].childGuidRefs.append(m_meshGuidIds[i]);
            m_partIds[parentPart].childGuidTags.append(QStringLiteral("CDrawableGuid"));
        }
    }
}

void Generator::writeWarpDeformerEntities()
{
    // deformers 表 → warp 表映射
    m_deformerIndexOfWarp.clear();
    for (int d = 0; d < m_table.deformers.size(); ++d) {
        if (m_table.deformers[d]->types == 0) m_deformerIndexOfWarp.append(d);
    }
    m_warpSourceIds.resize(m_table.warp_deformers.size());

    for (int w = 0; w < m_table.warp_deformers.size(); ++w) {
        const CWarp_deformers* warp = m_table.warp_deformers[w];
        const int d = m_deformerIndexOfWarp.value(w, -1);
        if (d < 0) continue;
        const CDeformers* deformer = m_table.deformers[d];

        const int keyformCount = warp->keyform_sources_counts;
        if (warp->rows < 0 || warp->columns < 0 || warp->rows > 1000 || warp->columns > 1000) continue;
        const int gridVerts = (warp->columns + 1) * (warp->rows + 1);
        if (gridVerts <= 0 || gridVerts > 1000000 ||
            !validSlice(warp->keyform_sources_begin_indices, keyformCount,
                        m_table.warp_deformer_keyforms.size())) continue;
        QVector<QString> formGuidIds;
        for (int k = 0; k < keyformCount; ++k) {
            const QString id = m_xml.AllocId();
            m_xml.GuidObject(QStringLiteral("CFormGuid"), NewGuid(), id);
            formGuidIds.append(id);
        }
        const SBindingChain chain = chainFor(deformer->keyform_binding_sources_indices);
        const QString gridId = writeKeyformGrid(chain, formGuidIds);
        const QString coordTypeId = (deformer->parent_deformer_indices >= 0)
                                        ? m_coordDeformerLocal : m_coordCanvas;

        m_warpSourceIds[w] = m_xml.AllocId();
        m_xml.BeginObject(QStringLiteral("CWarpDeformerSource"), QString(), m_warpSourceIds[w]);
        m_xml.BeginObject(QStringLiteral("ACDeformerSource"), QStringLiteral("super"));
        writeControllableHeader(displayName(deformer->ids), deformer->is_visible && !m_opt.hiddenIds.contains(deformer->ids),
                                (deformer->parent_part_indices >= 0)
                                    ? m_partIds[deformer->parent_part_indices].guid : m_rootPartGuidId,
                                gridId, 0);
        m_xml.EndList();   // _extensions
        m_xml.EndObject(); // ACParameterControllableSource
        m_xml.Ref(QStringLiteral("CDeformerGuid"), QStringLiteral("guid"), m_deformerIds[d].guid);
        m_xml.BeginObject(QStringLiteral("CDeformerId"), QStringLiteral("id"));
        m_xml.Attribute(QStringLiteral("idstr"), deformer->ids);
        m_xml.EndObject();
        m_xml.Ref(QStringLiteral("CDeformerGuid"), QStringLiteral("targetDeformerGuid"),
                  deformerTargetId(deformer->parent_deformer_indices));
        m_xml.EndObject(); // ACDeformerSource

        m_xml.I(QStringLiteral("col"), warp->columns);
        m_xml.I(QStringLiteral("row"), warp->rows);
        m_xml.B(QStringLiteral("isQuadTransform"), warp->is_quad_source);

        m_xml.BeginCArrayList(QStringLiteral("keyforms"), keyformCount);
        for (int k = 0; k < keyformCount; ++k) {
            const CWarp_deformer_keyforms* kf = m_table.warp_deformer_keyforms[
                warp->keyform_sources_begin_indices + k];
            m_xml.BeginObject(QStringLiteral("CWarpDeformerForm"));
            m_xml.BeginObject(QStringLiteral("ACDeformerForm"), QStringLiteral("super"));
            m_xml.BeginObject(QStringLiteral("ACForm"), QStringLiteral("super"));
            m_xml.Ref(QStringLiteral("CFormGuid"), QStringLiteral("guid"), formGuidIds[k]);
            m_xml.B(QStringLiteral("isAnimatedForm"), false);
            m_xml.B(QStringLiteral("isLocalAnimatedForm"), false);
            m_xml.Ref(QStringLiteral("CWarpDeformerSource"), QStringLiteral("_source"), m_warpSourceIds[w]);
            m_xml.Null(QStringLiteral("name"));
            m_xml.S(QStringLiteral("notes"), QString());
            m_xml.EndObject(); // ACForm
            m_xml.F(QStringLiteral("opacity"), kf->opacities);
            m_xml.Ref(QStringLiteral("CoordType"), QStringLiteral("coordType"), coordTypeId);
            m_xml.EndObject(); // ACDeformerForm

            QVector<float> positions;
            positions.reserve(gridVerts * 2);
            const int posBegin = kf->keyform_position_sources_begin_indices;
            const bool onCanvas = (deformer->parent_deformer_indices < 0);
            const CCanvas_info* canvas = m_table.canvas_info;
            for (int v = 0; v < gridVerts; ++v) {
                const float mx = m_table.keyform_positions->GetData(posBegin + v * 2);
                const float my = m_table.keyform_positions->GetData(posBegin + v * 2 + 1);
                if (onCanvas) {
                    positions.append((mx + canvas->origin_x) * canvas->pixels_per_unit);
                    positions.append((canvas->origin_y - my) * canvas->pixels_per_unit);
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

        const int parentPart = deformer->parent_part_indices;
        if (parentPart >= 0 && parentPart < m_partIds.size()) {
            m_partIds[parentPart].childGuidRefs.append(m_deformerIds[d].guid);
            m_partIds[parentPart].childGuidTags.append(QStringLiteral("CDeformerGuid"));
        }
    }
}

void Generator::writeRotationDeformerEntities()
{
    m_deformerIndexOfRotation.clear();
    for (int d = 0; d < m_table.deformers.size(); ++d) {
        if (m_table.deformers[d]->types == 1) m_deformerIndexOfRotation.append(d);
    }
    m_rotationSourceIds.resize(m_table.rotation_deformers.size());

    const CCanvas_info* canvas = m_table.canvas_info;

    for (int r = 0; r < m_table.rotation_deformers.size(); ++r) {
        const CRotation_deformers* rot = m_table.rotation_deformers[r];
        const int d = m_deformerIndexOfRotation.value(r, -1);
        if (d < 0) continue;
        const CDeformers* deformer = m_table.deformers[d];

        const int keyformCount = rot->keyform_sources_counts;
        if (keyformCount <= 0 || !validSlice(rot->keyform_sources_begin_indices, keyformCount,
                                             m_table.rotation_deformer_keyforms.size())) continue;
        QVector<QString> formGuidIds;
        for (int k = 0; k < keyformCount; ++k) {
            const QString id = m_xml.AllocId();
            m_xml.GuidObject(QStringLiteral("CFormGuid"), NewGuid(), id);
            formGuidIds.append(id);
        }
        const SBindingChain chain = chainFor(deformer->keyform_binding_sources_indices);
        const QString gridId = writeKeyformGrid(chain, formGuidIds);
        const QString coordTypeId = (deformer->parent_deformer_indices >= 0)
                                        ? m_coordDeformerLocal : m_coordCanvas;

        m_rotationSourceIds[r] = m_xml.AllocId();
        m_xml.BeginObject(QStringLiteral("CRotationDeformerSource"), QString(), m_rotationSourceIds[r]);
        m_xml.BeginObject(QStringLiteral("ACDeformerSource"), QStringLiteral("super"));
        writeControllableHeader(displayName(deformer->ids), deformer->is_visible && !m_opt.hiddenIds.contains(deformer->ids),
                                (deformer->parent_part_indices >= 0)
                                    ? m_partIds[deformer->parent_part_indices].guid : m_rootPartGuidId,
                                gridId, 0);
        m_xml.EndList();
        m_xml.EndObject();
        m_xml.Ref(QStringLiteral("CDeformerGuid"), QStringLiteral("guid"), m_deformerIds[d].guid);
        m_xml.BeginObject(QStringLiteral("CDeformerId"), QStringLiteral("id"));
        m_xml.Attribute(QStringLiteral("idstr"), deformer->ids);
        m_xml.EndObject();
        m_xml.Ref(QStringLiteral("CDeformerGuid"), QStringLiteral("targetDeformerGuid"),
                  deformerTargetId(deformer->parent_deformer_indices));
        m_xml.EndObject(); // ACDeformerSource

        m_xml.B(QStringLiteral("useBoneUi_testImpl"), true);
        m_xml.BeginCArrayList(QStringLiteral("keyforms"), keyformCount);
        for (int k = 0; k < keyformCount; ++k) {
            const CRotation_deformer_keyforms* kf = m_table.rotation_deformer_keyforms[
                rot->keyform_sources_begin_indices + k];
            const bool onCanvas = (deformer->parent_deformer_indices < 0);
            float px = kf->origin_x;
            float py = kf->origin_y;
            if (onCanvas) {
                px = (kf->origin_x + canvas->origin_x) * canvas->pixels_per_unit;
                py = (canvas->origin_y - kf->origin_y) * canvas->pixels_per_unit;
            }
            m_xml.BeginObject(QStringLiteral("CRotationDeformerForm"));
            m_xml.Attribute(QStringLiteral("angle"), CXsXmlWriter::FormatFloat(kf->angles));
            m_xml.Attribute(QStringLiteral("originX"), CXsXmlWriter::FormatFloat(px));
            m_xml.Attribute(QStringLiteral("originY"), CXsXmlWriter::FormatFloat(py));
            m_xml.Attribute(QStringLiteral("scale"), CXsXmlWriter::FormatFloat(kf->scales));
            m_xml.Attribute(QStringLiteral("isReflectX"),
                            kf->is_reflect_x ? QStringLiteral("true") : QStringLiteral("false"));
            m_xml.Attribute(QStringLiteral("isReflectY"),
                            kf->is_reflect_y ? QStringLiteral("true") : QStringLiteral("false"));
            m_xml.BeginObject(QStringLiteral("ACDeformerForm"), QStringLiteral("super"));
            m_xml.BeginObject(QStringLiteral("ACForm"), QStringLiteral("super"));
            m_xml.Ref(QStringLiteral("CFormGuid"), QStringLiteral("guid"), formGuidIds[k]);
            m_xml.B(QStringLiteral("isAnimatedForm"), false);
            m_xml.B(QStringLiteral("isLocalAnimatedForm"), false);
            m_xml.Ref(QStringLiteral("CRotationDeformerSource"), QStringLiteral("_source"),
                      m_rotationSourceIds[r]);
            m_xml.Null(QStringLiteral("name"));
            m_xml.S(QStringLiteral("notes"), QString());
            m_xml.EndObject();
            m_xml.F(QStringLiteral("opacity"), kf->opacities);
            m_xml.Ref(QStringLiteral("CoordType"), QStringLiteral("coordType"), coordTypeId);
            m_xml.EndObject(); // ACDeformerForm
            m_xml.EndObject(); // CRotationDeformerForm
        }
        m_xml.EndList();
        m_xml.F(QStringLiteral("handleLengthOnCanvas"), 512.0f);
        m_xml.F(QStringLiteral("circleRadiusOnCanvas"), 140.0f);
        m_xml.F(QStringLiteral("baseAngle"), rot->base_angles);
        m_xml.EndObject(); // CRotationDeformerSource

        const int parentPart = deformer->parent_part_indices;
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
        m_xml.BeginObject(QStringLiteral("CParameterSource"));
        m_xml.I(QStringLiteral("decimalPlaces"), param->decimal_places);
        m_xml.Ref(QStringLiteral("CParameterGuid"), QStringLiteral("guid"), m_paramIds[i].guid);
        m_xml.F(QStringLiteral("snapEpsilon"), 0.001f);
        m_xml.F(QStringLiteral("minValue"), param->min_values);
        m_xml.F(QStringLiteral("maxValue"), param->max_values);
        m_xml.F(QStringLiteral("defaultValue"), param->default_values);
        m_xml.B(QStringLiteral("isRepeat"), param->is_repeat);
        m_xml.BeginObject(QStringLiteral("CParameterId"), QStringLiteral("id"));
        m_xml.Attribute(QStringLiteral("idstr"), param->ids);
        m_xml.EndObject();
        m_xml.Enum(QStringLiteral("Type"), QStringLiteral("paramType"),
                   (param->parameter_types == 1) ? QStringLiteral("BLEND_SHAPE")
                                                 : QStringLiteral("NORMAL"));
        m_xml.S(QStringLiteral("name"), displayName(param->ids));
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
        for (const QString& id : m_warpSourceIds) if (!id.isEmpty()) ++total;
        for (const QString& id : m_rotationSourceIds) if (!id.isEmpty()) ++total;
        m_xml.BeginObject(QStringLiteral("CDeformerSourceSet"), QStringLiteral("deformerSourceSet"));
        m_xml.BeginCArrayList(QStringLiteral("_sources"), total);
        int warpCursor = 0;
        int rotationCursor = 0;
        for (int d = 0; d < m_table.deformers.size(); ++d) {
            if (m_table.deformers[d]->types == 0) {
                const QString id = m_warpSourceIds.value(warpCursor++);
                if (!id.isEmpty()) m_xml.Ref(QStringLiteral("CWarpDeformerSource"), QString(), id);
            } else {
                const QString id = m_rotationSourceIds.value(rotationCursor++);
                if (!id.isEmpty()) m_xml.Ref(QStringLiteral("CRotationDeformerSource"), QString(), id);
            }
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
    const QSize canvasSize = CTextureExtractor::GetCanvasPixelSize(m_table.canvas_info);
    m_canvasW = canvasSize.width();
    m_canvasH = canvasSize.height();
    loadDisplayNames();

    // 提取层 → SLayerData；按 mesh 下标建索引
    m_layerOfMesh.fill(-1, m_table.art_meshes.size());
    for (const ExtractedLayer& ex : m_extracted) {
        if (ex.image.isNull()) continue;
        SLayerData layer;
        layer.name = displayName(ex.name);
        layer.image = ex.image;
        layer.bounds = ex.canvasRect;
        layer.visible = ex.isVisible;
        m_layerOfMesh[ex.meshIndex] = m_layers.size();
        m_layers.append(layer);
    }
    if (m_layers.isEmpty()) {
        if (outError) *outError = QStringLiteral("没有任何可提取的 ArtMesh 贴图");
        return false;
    }

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
                                                  m_canvasW, m_canvasH, m_layers);
    m_modelImageGroupId = m_entities.WriteModelImageGroup(m_filterGraph, m_layeredImage, m_layers);

    writeParameterGuids();
    writeDeformerGuids();
    writePartEntities();
    writeArtMeshEntities();
    writeWarpDeformerEntities();
    writeRotationDeformerEntities();
    writeGlueEntities();

    // ---- Part 源（childGuids 现已齐全） ----
    // 根 part
    {
        // 未挂任何 part 的顶层对象也归根
        QVector<QString> rootChildRefs;
        QVector<QString> rootChildTags;
        for (int i = 0; i < m_table.parts.size(); ++i) {
            if (m_table.parts[i]->parent_part_indices < 0) {
                rootChildRefs.append(m_partIds[i].guid);
                rootChildTags.append(QStringLiteral("CPartGuid"));
            }
        }
        for (int i = 0; i < m_table.art_meshes.size(); ++i) {
            if (m_table.art_meshes[i]->parent_part_indices < 0) {
                rootChildRefs.append(m_meshGuidIds[i]);
                rootChildTags.append(QStringLiteral("CDrawableGuid"));
            }
        }
        for (int d = 0; d < m_table.deformers.size(); ++d) {
            if (m_table.deformers[d]->parent_part_indices < 0) {
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
        writeControllableHeader(displayName(part->ids), part->is_visible && !m_opt.hiddenIds.contains(part->ids),
                                (part->parent_part_indices >= 0)
                                    ? m_partIds[part->parent_part_indices].guid : m_rootPartGuidId,
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

    // ---- CAFF 打包 ----
    m_caff.AddEntry(QStringLiteral("main.xml"), m_xml.Result(),
                    QStringLiteral("main_xml"), CCaffArchive::CompressFast);
    return m_caff.Save(m_opt.outPath, outError);
}

} // namespace

bool CCmo3Builder::Build(const Options& options, QString* outError)
{
    Options opt = options;
    if (opt.modelName.isEmpty()) {
        opt.modelName = QFileInfo(opt.mocPath).completeBaseName();
    }

    CReader reader(opt.mocPath);
    CSectionTable table(reader);
    if (!table.IsValid() || !table.count_info || !table.canvas_info || !table.uvs ||
        !table.position_indices || !table.keyform_positions || !table.keys ||
        !std::isfinite(table.canvas_info->pixels_per_unit) || table.canvas_info->pixels_per_unit <= 0.0f) {
        if (outError) *outError = QStringLiteral("moc3 解析失败: %1").arg(opt.mocPath);
        return false;
    }

    // 纹理图集定位：moc3 同目录 model3.json 或 textures/ 推断
    QVector<QString> atlasPaths;
    {
        const QDir dir = QFileInfo(opt.mocPath).dir();
        // model3.json 引用优先
        const QStringList jsons = dir.entryList({ QStringLiteral("*.model3.json") }, QDir::Files);
        if (!jsons.isEmpty()) {
            QFile file(dir.filePath(jsons.first()));
            if (file.open(QIODevice::ReadOnly)) {
                const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
                file.close();
                const QJsonArray textures = root.value(QStringLiteral("FileReferences"))
                                                .toObject().value(QStringLiteral("Textures")).toArray();
                for (const QJsonValue& v : textures) {
                    atlasPaths.append(dir.filePath(v.toString()));
                }
            }
        }
        if (atlasPaths.isEmpty()) {
            QDir texDir(dir.filePath(QStringLiteral("textures")));
            if (!texDir.exists()) texDir = dir;
            const QStringList pngs = texDir.entryList({ QStringLiteral("*.png") }, QDir::Files, QDir::Name);
            for (const QString& png : pngs) atlasPaths.append(texDir.filePath(png));
        }
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
    QVector<ExtractedLayer> extracted = extractor.ExtractAll(&table, true);

    Generator generator(opt, table, extracted);
    return generator.Run(outError);
}

} // namespace cmo3
