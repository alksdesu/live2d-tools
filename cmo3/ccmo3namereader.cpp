#include "ccmo3namereader.h"
#include "ccmo3project.h"
#include "ccmo3xml_p.h"
#include "ccaffarchive.h"
#include "ccmo3ids.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmodeldisplaynames.h"
#include "live2dstruct/csectiontable.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QSet>
#include <QtEndian>
#include <cmath>
#include <stdexcept>

namespace cmo3 {
struct CCmo3NameReader::Catalog::Data {
    QVector<Object> objects;
    QStringList diagnostics;
    QString sourcePath;
    QByteArray sourceDigest;
    QSize canvas;
};
namespace {
using detail::Xml;
using detail::ReadError;
using detail::require;
using detail::checkpoint;
using Reader = CCmo3NameReader;
using Object = Reader::Object;
using Type = Reader::ObjectType;
constexpr int MaximumObjects = 100000;
constexpr qint64 MaximumNumbers = 16000000;
QString key(Type type, const QString& id) { return Reader::TypeName(type) + QChar(':') + id; }
QString owned(const QString& value) { return QString(value.constData(), value.size()); }
bool validId(const QString& value)
{
    return !value.isEmpty() && value.toUtf8().size() <= 64 && !value.contains(QChar::Null) && value.isValidUtf16();
}
QByteArray topologyDigest(int vertices, const QVector<quint16>& indices)
{
    QCryptographicHash digest(QCryptographicHash::Sha256);
    uchar value[4];
    qToLittleEndian<quint32>(quint32(vertices), value);
    digest.addData(QByteArrayView(reinterpret_cast<const char*>(value), 4));
    for (quint16 index : indices) {
        qToLittleEndian<quint16>(index, value);
        digest.addData(QByteArrayView(reinterpret_cast<const char*>(value), 2));
    }
    return digest.result();
}
QString guid(const Xml& xml, int node, const QString& type)
{
    if (node < 0 || xml.nodes[xml.resolve(node)].type == QStringLiteral("null")) return {};
    node = xml.typed(node, type);
    const QString result = xml.nodes[node].attributes.value(QStringLiteral("uuid"));
    require(!result.isEmpty() && result.size() <= 256 && !result.contains(QChar::Null), QStringLiteral("CMO 对象 GUID 无效"));
    return result;
}
QVector<double> numbers(const Xml& xml, int node, qint64& work, CExportControl* control)
{
    node = xml.resolve(node);
    const auto& item = xml.nodes[node];
    require(item.children.isEmpty(), QStringLiteral("CMO 数值数组混有子节点"));
    const qint64 count = xml.number(item.attributes.value(QStringLiteral("count")));
    require(count >= 0 && count <= MaximumNumbers - work, QStringLiteral("CMO 几何数值超出预算"));
    work += count;
    QVector<double> result;
    result.reserve(count);
    const QStringView text(item.value);
    qsizetype at = 0;
    while (at < text.size()) {
        while (at < text.size() && text[at].isSpace()) ++at;
        if (at == text.size()) break;
        const qsizetype start = at;
        while (at < text.size() && !text[at].isSpace()) ++at;
        require(at - start <= 128 && result.size() < count, QStringLiteral("CMO 数值数组数量或数值长度无效"));
        bool ok = false;
        const double value = text.mid(start, at - start).toDouble(&ok);
        require(ok && std::isfinite(value), QStringLiteral("CMO 几何数值无效"));
        result.append(value);
        if ((result.size() & 1023) == 0) checkpoint(control);
    }
    require(result.size() == count, QStringLiteral("CMO 数值数组数量不符"));
    return result;
}
struct Parsed {
    QVector<Object> objects;
    QStringList diagnostics;
    QSize canvas;
};
Parsed parse(const Xml& xml, CExportControl* control)
{
    xml.sourceVersion();
    int mainNode = -1;
    for (int child : xml.nodes[0].children) if (xml.nodes[child].type == QStringLiteral("main")) {
        require(mainNode < 0, QStringLiteral("CMO 包含多个 main 节点"));
        mainNode = child;
    }
    require(mainNode >= 0 && xml.nodes[mainNode].children.size() == 1, QStringLiteral("CMO main 节点无效"));
    const int model = xml.typed(xml.nodes[mainNode].children.first(), QStringLiteral("CModelSource"));
    const int canvas = xml.typed(xml.field(model, QStringLiteral("canvas")), QStringLiteral("CImageCanvas"));
    const qint64 width = xml.integer(canvas, QStringLiteral("pixelWidth"));
    const qint64 height = xml.integer(canvas, QStringLiteral("pixelHeight"));
    require(width > 0 && height > 0 && width <= 1000000 && height <= 1000000, QStringLiteral("CMO 画布尺寸无效"));
    Parsed result;
    result.canvas = QSize(int(width), int(height));
    const int root = xml.typed(xml.field(model, QStringLiteral("rootPart")), QStringLiteral("CPartSource"));
    const QString rootGuid = guid(xml, xml.field(root, QStringLiteral("guid")), QStringLiteral("CPartGuid"));
    QVector<QString> parentParts, parentDeformers;
    QHash<QString, QVector<int>> guids;
    QHash<QString, QVector<int>> ids;
    qint64 work = 0;
    int rootCount = 0;
    const struct { const char* field; const char* type; } sets[] = {
        {"partSourceSet", "CPartSourceSet"}, {"drawableSourceSet", "CDrawableSourceSet"}, {"deformerSourceSet", "CDeformerSourceSet"}
    };
    for (const auto& set : sets) {
        const int container = xml.typed(xml.field(model, QString::fromLatin1(set.field)), QString::fromLatin1(set.type));
        const auto sources = xml.array(xml.field(container, QStringLiteral("_sources")));
        require(sources.size() <= MaximumObjects - result.objects.size(), QStringLiteral("CMO 活动对象数量超出预算"));
        for (int source : sources) {
            checkpoint(control);
            source = xml.resolve(source);
            const QString sourceType = xml.nodes[source].type;
            Type type;
            if (sourceType == QStringLiteral("CPartSource") && QString::fromLatin1(set.field) == QStringLiteral("partSourceSet")) type = Type::Part;
            else if (sourceType == QStringLiteral("CArtMeshSource") && QString::fromLatin1(set.field) == QStringLiteral("drawableSourceSet")) type = Type::ArtMesh;
            else if (sourceType == QStringLiteral("CWarpDeformerSource") && QString::fromLatin1(set.field) == QStringLiteral("deformerSourceSet")) type = Type::Warp;
            else if (sourceType == QStringLiteral("CRotationDeformerSource") && QString::fromLatin1(set.field) == QStringLiteral("deformerSourceSet")) type = Type::Rotation;
            else {
                require(result.diagnostics.size() < 4096, QStringLiteral("CMO 诊断数量超出预算"));
                result.diagnostics.append(QStringLiteral("未读取的活动对象类型：%1").arg(sourceType));
                continue;
            }
            const int body = type == Type::Part ? source : xml.typed(xml.field(source, QStringLiteral("super")),
                type == Type::ArtMesh ? QStringLiteral("ACDrawableSource") : QStringLiteral("ACDeformerSource"));
            const int common = xml.typed(xml.field(body, QStringLiteral("super")), QStringLiteral("ACParameterControllableSource"));
            Object item;
            item.type = type;
            const QString idType = type == Type::Part ? QStringLiteral("CPartId") : type == Type::ArtMesh ? QStringLiteral("CDrawableId") : QStringLiteral("CDeformerId");
            item.id = xml.nodes[xml.typed(xml.field(body, QStringLiteral("id")), idType)].attributes.value(QStringLiteral("idstr"));
            item.name = xml.text(xml.field(common, QStringLiteral("localName")));
            item.guid = guid(xml, xml.field(body, QStringLiteral("guid")), type == Type::Part ? QStringLiteral("CPartGuid") :
                             type == Type::ArtMesh ? QStringLiteral("CDrawableGuid") : QStringLiteral("CDeformerGuid"));
            if (type == Type::Part && source == root) { ++rootCount; continue; }
            if (!validId(item.id)) item.diagnostics.append(QStringLiteral("来源对象 ID 无效"));
            const QString part = guid(xml, xml.field(common, QStringLiteral("parentGuid")), QStringLiteral("CPartGuid"));
            const QString deformer = type == Type::Part ? QString() : guid(xml, xml.field(body, QStringLiteral("targetDeformerGuid")), QStringLiteral("CDeformerGuid"));
            if (type == Type::ArtMesh) {
                const auto forms = xml.array(xml.field(source, QStringLiteral("keyforms")));
                require(!forms.isEmpty(), QStringLiteral("CMO ArtMesh 缺少关键形"));
                const int form = xml.typed(forms.first(), QStringLiteral("CArtMeshForm"));
                const auto positions = numbers(xml, xml.field(form, QStringLiteral("positions")), work, control);
                require(positions.size() % 2 == 0 && positions.size() <= 2000000, QStringLiteral("CMO ArtMesh 顶点数量无效"));
                item.vertexCount = positions.size() / 2;
                const auto rawIndices = numbers(xml, xml.field(source, QStringLiteral("indices")), work, control);
                require(rawIndices.size() % 3 == 0, QStringLiteral("CMO ArtMesh 三角索引数量无效"));
                QVector<quint16> indices;
                indices.reserve(rawIndices.size());
                for (double value : rawIndices) {
                    require(value >= 0 && value <= 65535 && value < item.vertexCount && value == std::floor(value),
                            QStringLiteral("CMO ArtMesh 三角索引越界"));
                    indices.append(quint16(value));
                }
                item.indexCount = indices.size();
                item.topologyDigest = topologyDigest(item.vertexCount, indices);
            } else if (type == Type::Warp) {
                const qint64 rows = xml.integer(source, QStringLiteral("row")), columns = xml.integer(source, QStringLiteral("col"));
                require(rows > 0 && columns > 0 && rows <= 1000 && columns <= 1000, QStringLiteral("CMO Warp 网格尺寸无效"));
                item.rows = int(rows);
                item.columns = int(columns);
                item.quadTransform = xml.boolean(source, QStringLiteral("isQuadTransform"));
            }
            const int index = result.objects.size();
            guids[item.guid].append(index);
            ids[key(type, item.id)].append(index);
            parentParts.append(part);
            parentDeformers.append(deformer);
            result.objects.append(std::move(item));
        }
    }
    require(rootCount == 1, QStringLiteral("CMO 根部件不在唯一活动集合中"));
    for (int i = 0; i < result.objects.size(); ++i) {
        auto& item = result.objects[i];
        if (guids.value(item.guid).size() != 1 || item.guid == rootGuid) item.diagnostics.append(QStringLiteral("来源 GUID 重复"));
        if (ids.value(key(item.type, item.id)).size() != 1) item.diagnostics.append(QStringLiteral("来源同类型 ID 重复"));
        const auto parent = [&](const QString& value, bool part) {
            if ((part && value == rootGuid) || (!part && value == WellKnown::kDeformerRoot)) return QString();
            const auto found = guids.constFind(value);
            if (value.isEmpty() || found == guids.cend() || found.value().size() != 1) {
                item.diagnostics.append(part ? QStringLiteral("来源父部件引用缺失或不唯一") : QStringLiteral("来源父变形器引用缺失或不唯一"));
                return QString();
            }
            const auto& target = result.objects[found.value().first()];
            if ((part && target.type != Type::Part) || (!part && target.type != Type::Warp && target.type != Type::Rotation)) {
                item.diagnostics.append(QStringLiteral("来源父级引用类型不匹配"));
                return QString();
            }
            if (ids.value(key(target.type, target.id)).size() != 1) item.diagnostics.append(QStringLiteral("来源父级 ID 不唯一"));
            return target.id;
        };
        item.parentPart = parent(parentParts[i], true);
        if (item.type != Type::Part) item.parentDeformer = parent(parentDeformers[i], false);
    }
    QHash<QString, int> partById, deformerById;
    QSet<QString> ambiguousDeformers;
    for (int i = 0; i < result.objects.size(); ++i) {
        const auto& item = result.objects[i];
        if (item.type == Type::Part) partById.insert(item.id, i);
        if (item.type == Type::Warp || item.type == Type::Rotation) {
            if (deformerById.contains(item.id)) ambiguousDeformers.insert(item.id);
            deformerById.insert(item.id, i);
        }
    }
    for (int i = 0; i < result.objects.size(); ++i) {
        auto& item = result.objects[i];
        if (ambiguousDeformers.contains(item.id) && (item.type == Type::Warp || item.type == Type::Rotation))
            item.diagnostics.append(QStringLiteral("来源变形器 ID 跨类型重复"));
        for (bool part : {true, false}) {
            if (!part && item.type == Type::Part) continue;
            QSet<int> visited;
            int at = i;
            while (at >= 0) {
                require(++work <= MaximumNumbers, QStringLiteral("CMO 父级校验超出预算"));
                if ((work & 1023) == 0) checkpoint(control);
                if (visited.contains(at)) { item.diagnostics.append(QStringLiteral("来源父级包含循环")); break; }
                visited.insert(at);
                const auto& current = result.objects[at];
                const QString id = part ? current.parentPart : current.parentDeformer;
                if (id.isEmpty()) break;
                at = (part ? partById : deformerById).value(id, -1);
            }
        }
        if ((i & 255) == 0) checkpoint(control);
    }
    if (xml.unrelatedInvalidReferences) result.diagnostics.append(QStringLiteral("工程有 %1 个未使用的无效引用；匹配仅检查活动对象字段").arg(xml.unrelatedInvalidReferences));
    return result;
}
}

const QVector<CCmo3NameReader::Object>& CCmo3NameReader::Catalog::Objects() const
{
    static const QVector<Object> empty;
    return m_data ? m_data->objects : empty;
}
const QStringList& CCmo3NameReader::Catalog::Diagnostics() const
{
    static const QStringList empty;
    return m_data ? m_data->diagnostics : empty;
}
QString CCmo3NameReader::Catalog::SourcePath() const { return m_data ? m_data->sourcePath : QString(); }
QByteArray CCmo3NameReader::Catalog::SourceDigest() const { return m_data ? m_data->sourceDigest : QByteArray(); }
QSize CCmo3NameReader::Catalog::CanvasSize() const { return m_data ? m_data->canvas : QSize(); }
QString CCmo3NameReader::TypeName(ObjectType type)
{
    switch (type) {
    case ObjectType::Part: return QStringLiteral("part");
    case ObjectType::ArtMesh: return QStringLiteral("artmesh");
    case ObjectType::Warp: return QStringLiteral("warp");
    case ObjectType::Rotation: return QStringLiteral("rotation");
    }
    return {};
}

CCmo3NameReader::Catalog CCmo3NameReader::Extract(const detail::Xml& xml, const QString& path,
                                                  const QByteArray& digest, CExportControl* control)
{
    auto parsed = parse(xml, control);
    auto data = std::make_shared<Catalog::Data>();
    data->objects = std::move(parsed.objects);
    data->diagnostics = std::move(parsed.diagnostics);
    data->canvas = parsed.canvas;
    data->sourcePath = path;
    data->sourceDigest = digest;
    checkpoint(control);
    Catalog candidate;
    candidate.m_data = std::move(data);
    return candidate;
}

bool CCmo3NameReader::ReadBytes(const QByteArray& bytes, const QString& sourcePath, Catalog& result,
                              QString* error, CExportControl* control)
{
    CCmo3Project project;
    if (!CCmo3Project::OpenBytes(bytes, sourcePath, project, error, control)) return false;
    const auto* names = project.Names();
    if (!names) { if (error) *error = project.NamesError(); return false; }
    result = *names;
    return true;
}

bool CCmo3NameReader::ReadFile(const QString& path, Catalog& result, QString* error, CExportControl* control)
{
    CCmo3Project project;
    if (!CCmo3Project::OpenFile(path, project, error, control)) return false;
    const auto* names = project.Names();
    if (!names) { if (error) *error = project.NamesError(); return false; }
    result = *names;
    return true;
}

bool CCmo3NameReader::BuildProposal(const Catalog& input, const QByteArray& sourceMocBytes, Proposal& result,
                                  QString* error, CExportControl* control)
{
    if (error) error->clear();
    try {
        const auto data = input.m_data;
        require(bool(data), QStringLiteral("CMO 名称目录尚未读取"));
        require(sourceMocBytes.size() <= 512LL * 1024 * 1024, QStringLiteral("模型来源超出匹配预算"));
        const QByteArray moc(sourceMocBytes.constData(), sourceMocBytes.size());
        checkpoint(control);
        live2dstruct::CModelDocument document;
        QString message;
        require(document.LoadBytes(moc, {}, &message), message);
        auto reader = live2dstruct::CReader::FromBytes(moc);
        live2dstruct::CSectionTable table(reader);
        require(table.IsValid() && table.canvas_info, QStringLiteral("模型画布结构无效"));
        const bool canvasMatches = table.canvas_info->width == data->canvas.width() && table.canvas_info->height == data->canvas.height();
        QVector<Object> targets;
        const auto partId = [&](int index) {
            require(index == -1 || (index >= 0 && index < document.Parts().size()), QStringLiteral("模型父部件索引越界"));
            return index < 0 ? QString() : document.Parts()[index].id;
        };
        const auto deformerId = [&](int index) {
            require(index == -1 || (index >= 0 && index < document.Deformers().size()), QStringLiteral("模型父变形器索引越界"));
            return index < 0 ? QString() : document.Deformers()[index].id;
        };
        require(document.Parts().size() + document.Drawables().size() + document.Deformers().size() <= MaximumObjects,
                QStringLiteral("模型活动对象数量超出匹配预算"));
        for (const auto& part : document.Parts()) {
            Object item; item.type = Type::Part; item.id = part.id; item.parentPart = partId(part.parentIndex);
            targets.append(std::move(item));
        }
        for (const auto& mesh : document.Drawables()) {
            checkpoint(control);
            Object item; item.type = Type::ArtMesh; item.id = mesh.id;
            item.parentPart = partId(mesh.parentPartIndex); item.parentDeformer = deformerId(mesh.parentDeformerIndex);
            item.vertexCount = mesh.vertexCount; item.indexCount = mesh.indices.size(); item.topologyDigest = topologyDigest(mesh.vertexCount, mesh.indices);
            targets.append(std::move(item));
        }
        for (const auto& deformer : document.Deformers()) {
            Object item; item.type = deformer.type == live2dstruct::CModelDeformer::Warp ? Type::Warp : Type::Rotation;
            item.id = deformer.id; item.parentPart = partId(deformer.parentPartIndex); item.parentDeformer = deformerId(deformer.parentDeformerIndex);
            item.rows = deformer.rows; item.columns = deformer.columns; item.quadTransform = deformer.quadTransform;
            targets.append(std::move(item));
        }
        QHash<QString, QVector<int>> targetIds, sourceIds;
        QHash<QString, int> targetDeformers;
        for (int i = 0; i < targets.size(); ++i) {
            targetIds[key(targets[i].type, targets[i].id)].append(i);
            if (targets[i].type == Type::Warp || targets[i].type == Type::Rotation) ++targetDeformers[targets[i].id];
        }
        for (int i = 0; i < data->objects.size(); ++i) sourceIds[key(data->objects[i].type, data->objects[i].id)].append(i);
        QHash<QString, int> targetParts;
        QHash<QString, int> targetParentDeformers;
        for (int i = 0; i < targets.size(); ++i) {
            if (targets[i].type == Type::Part) targetParts.insert(targets[i].id, i);
            if (targets[i].type == Type::Warp || targets[i].type == Type::Rotation) targetParentDeformers.insert(targets[i].id, i);
        }
        qint64 graphWork = 0;
        for (int i = 0; i < targets.size(); ++i) {
            for (bool part : {true, false}) {
                if (!part && targets[i].type == Type::Part) continue;
                QSet<int> visited;
                int at = i;
                while (at >= 0) {
                    require(++graphWork <= MaximumNumbers, QStringLiteral("模型父级校验超出预算"));
                    if ((graphWork & 1023) == 0) checkpoint(control);
                    if (visited.contains(at)) { targets[i].diagnostics.append(QStringLiteral("模型父级包含循环")); break; }
                    visited.insert(at);
                    const QString id = part ? targets[at].parentPart : targets[at].parentDeformer;
                    if (id.isEmpty()) break;
                    if ((part && targetIds.value(key(Type::Part, id)).size() != 1) || (!part && targetDeformers.value(id) != 1)) {
                        targets[i].diagnostics.append(QStringLiteral("模型父级身份不唯一"));
                        break;
                    }
                    at = (part ? targetParts : targetParentDeformers).value(id, -1);
                }
            }
        }
        Proposal candidate;
        candidate.sourceCmoDigest = data->sourceDigest;
        candidate.sourceMocDigest = QCryptographicHash::hash(moc, QCryptographicHash::Sha256);
        candidate.diagnostics = data->diagnostics;
        candidate.diagnostics.append(QStringLiteral("结构相符只提供名称匹配依据，不能证明两个文件同源；请确认后采用"));
        if (!canvasMatches) candidate.diagnostics.append(QStringLiteral("工程与原始模型画布尺寸不同"));
        for (int i = 0; i < data->objects.size(); ++i) {
            checkpoint(control);
            const auto& source = data->objects[i];
            Match match; match.type = source.type; match.id = source.id; match.name = source.name; match.sourceIndex = i;
            match.diagnostics = source.diagnostics;
            const QString identity = key(source.type, source.id);
            const auto found = targetIds.constFind(identity);
            if (found == targetIds.cend()) {
                match.status = MatchStatus::MissingTarget;
                match.diagnostics.append(QStringLiteral("原始模型没有此类型和 ID"));
            } else if (found.value().size() != 1 || sourceIds.value(identity).size() != 1 ||
                       ((source.type == Type::Warp || source.type == Type::Rotation) && targetDeformers.value(source.id) != 1)) {
                match.diagnostics.append(QStringLiteral("来源或模型对象身份不唯一"));
            } else if (match.diagnostics.isEmpty() && canvasMatches) {
                const auto& target = targets[found.value().first()];
                match.diagnostics.append(target.diagnostics);
                match.evidence.append(QStringLiteral("类型、ID 与画布尺寸一致"));
                if (source.parentPart != target.parentPart || source.parentDeformer != target.parentDeformer)
                    match.diagnostics.append(QStringLiteral("所属部件或父变形器不同"));
                else match.evidence.append(QStringLiteral("父部件与父变形器一致"));
                if (source.type == Type::ArtMesh) {
                    if (source.vertexCount != target.vertexCount || source.topologyDigest != target.topologyDigest)
                        match.diagnostics.append(QStringLiteral("顶点数量或完整三角索引不同"));
                    else match.evidence.append(QStringLiteral("顶点数量与完整三角索引一致"));
                } else if (source.type == Type::Warp) {
                    if (source.rows != target.rows || source.columns != target.columns || source.quadTransform != target.quadTransform)
                        match.diagnostics.append(QStringLiteral("Warp 行列或插值方式不同"));
                    else match.evidence.append(QStringLiteral("Warp 行列与插值方式一致"));
                }
                if (match.diagnostics.isEmpty()) {
                    match.status = MatchStatus::Compatible;
                    if (!live2dstruct::CModelDisplayNames::ValidateName(source.name, &message)) {
                        match.status = MatchStatus::InvalidName;
                        match.diagnostics.append(message);
                    }
                }
            }
            candidate.matches.append(std::move(match));
        }
        for (const auto& target : targets) if (!sourceIds.contains(key(target.type, target.id))) {
            Match match; match.type = target.type; match.id = target.id; match.status = MatchStatus::MissingSource;
            match.diagnostics.append(QStringLiteral("工程活动集合没有此类型和 ID"));
            candidate.matches.append(std::move(match));
        }
        QHash<QString, QVector<int>> dependents;
        QVector<QStringList> dependencies(data->objects.size());
        QHash<QString, QString> sourceDeformerKeys;
        for (const auto& object : data->objects)
            if (object.type == Type::Warp || object.type == Type::Rotation) sourceDeformerKeys.insert(object.id, key(object.type, object.id));
        for (int i = 0; i < data->objects.size(); ++i) {
            const auto& object = data->objects[i];
            if (!object.parentPart.isEmpty()) dependencies[i].append(key(Type::Part, object.parentPart));
            if (!object.parentDeformer.isEmpty()) dependencies[i].append(sourceDeformerKeys.value(object.parentDeformer));
            for (const auto& dependency : dependencies[i]) dependents[dependency].append(i);
        }
        QVector<int> queue;
        QSet<QString> rejected;
        for (int i = 0; i < candidate.matches.size(); ++i) {
            const auto& match = candidate.matches[i];
            if (match.status != MatchStatus::Compatible && match.status != MatchStatus::InvalidName) {
                rejected.insert(key(match.type, match.id));
                queue.append(i);
            }
        }
        for (qsizetype at = 0; at < queue.size(); ++at) {
            checkpoint(control);
            const auto& parent = candidate.matches[queue[at]];
            for (int dependent : dependents.value(key(parent.type, parent.id))) {
                auto& child = candidate.matches[dependent];
                const QString identity = key(child.type, child.id);
                if (rejected.contains(identity)) continue;
                child.status = MatchStatus::Conflict;
                child.diagnostics.append(QStringLiteral("父级结构无法与原始模型核对"));
                rejected.insert(identity);
                queue.append(dependent);
            }
        }
        bool geometryWitness = false;
        for (int i = 0; i < data->objects.size(); ++i) {
            const auto& object = data->objects[i];
            const auto status = candidate.matches[i].status;
            if (object.type == Type::ArtMesh && object.vertexCount >= 3 && object.indexCount >= 3 &&
                (status == MatchStatus::Compatible || status == MatchStatus::InvalidName)) geometryWitness = true;
        }
        if (!geometryWitness) candidate.diagnostics.append(QStringLiteral("没有可核对的共同 ArtMesh 拓扑，不能仅凭 ID 导入名称"));
        for (auto& match : candidate.matches) {
            if (!geometryWitness && match.status == MatchStatus::Compatible) {
                match.status = MatchStatus::Conflict;
                match.diagnostics.append(QStringLiteral("缺少共同网格结构证据"));
            }
            if (match.status == MatchStatus::Compatible) ++candidate.compatibleCount;
            else candidate.partial = true;
        }
        checkpoint(control);
        result = std::move(candidate);
        return true;
    } catch (const detail::ReadAborted& failure) { if (error) *error = failure.message; }
    catch (const ReadError& failure) { if (error) *error = failure.message; }
    catch (const std::exception&) { if (error) *error = QStringLiteral("CMO 名称匹配超出资源限制"); }
    return false;
}
}
