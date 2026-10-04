#include "ccmo3project.h"
#include "ccmo3artworkreader.h"
#include "ccmo3xml_p.h"
#include "ccaffarchive.h"
#include "std/cpngimage.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QImage>
#include <QSet>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

namespace cmo3 {
struct CCmo3Project::Data {
    QByteArray bytes, digest;
    QString path, version, artworkError, namesError, materialsError;
    std::optional<CPsdSourceCatalog> artwork;
    std::optional<CCmo3NameReader::Catalog> names;
    std::optional<CCmo3MaterialGraph> materials;
};
namespace {
using detail::Xml;
using detail::ReadError;
using detail::require;
using detail::checkpoint;
using Graph = CCmo3MaterialGraph;
constexpr int MaximumRecords = 100000;
constexpr qint64 MaximumNumbers = 16000000;
constexpr qint64 MaximumImagePixels = 256LL * 1024 * 1024;
QString owned(const QString& value) { return QString(value.constData(), value.size()); }
int modelRoot(const Xml& xml)
{
    int main = -1;
    for (int child : xml.nodes[0].children) if (xml.nodes[child].type == QStringLiteral("main")) {
        require(main < 0, QStringLiteral("CMO 包含多个 main 节点"));
        main = child;
    }
    require(main >= 0 && xml.nodes[main].children.size() == 1, QStringLiteral("CMO main 节点无效"));
    return xml.typed(xml.nodes[main].children.first(), QStringLiteral("CModelSource"));
}
class MaterialReader {
public:
    MaterialReader(const CCaffArchive& source, const Xml& parsed, const CPsdSourceCatalog* artwork, CExportControl* progress)
        : archive(source), xml(parsed), control(progress)
    {
        if (artwork) for (const auto& entry : artwork->documents) if (const auto* document = entry.Reconstructed())
            for (const auto& layer : document->layers) {
                checkpoint(control);
                if (!layer.png.isEmpty()) verifiedPngs.insert(layer.png, layer.bounds.size());
            }
    }
    Graph read()
    {
        const int model = modelRoot(xml);
        const int manager = xml.typed(xml.field(model, "textureManager"), "CTextureManager");
        readLayers(manager);
        const auto groups = xml.array(xml.field(manager, "_modelImageGroups"));
        QSet<int> groupNodes, imageNodes;
        for (int group : groups) {
            checkpoint(control);
            group = xml.typed(group, "CModelImageGroup");
            require(!groupNodes.contains(group), QStringLiteral("CMO 模型图像组重复"));
            groupNodes.insert(group);
            for (int image : xml.array(xml.field(group, "_modelImages"))) {
                require(result.images.size() < MaximumRecords, QStringLiteral("CMO 模型图像数量超出预算"));
                image = xml.typed(image, "CModelImage");
                require(!imageNodes.contains(image), QStringLiteral("CMO 模型图像在活动组中重复"));
                imageNodes.insert(image);
                auto item = readImage(image, group);
                require(!modelImages.contains(item.guid), QStringLiteral("CMO 模型图像 GUID 重复"));
                modelImages.insert(item.guid);
                result.images.append(std::move(item));
            }
        }
        const int drawables = xml.typed(xml.field(model, "drawableSourceSet"), "CDrawableSourceSet");
        QSet<QString> meshIds, meshGuids;
        for (int source : xml.array(xml.field(drawables, "_sources"))) {
            checkpoint(control);
            source = xml.resolve(source);
            if (xml.nodes[source].type != QStringLiteral("CArtMeshSource")) continue;
            require(result.meshes.size() < MaximumRecords, QStringLiteral("CMO 素材网格数量超出预算"));
            auto item = readMesh(source);
            require(!meshIds.contains(item.id) && !meshGuids.contains(item.guid), QStringLiteral("CMO 素材网格身份重复"));
            meshIds.insert(item.id); meshGuids.insert(item.guid);
            result.meshes.append(std::move(item));
        }
        return std::move(result);
    }
private:
    struct Layer {
        QString documentGuid, guid, name;
        int node = -1, index = -1, image = -1;
        QRect bounds;
        bool group = false;
    };
    struct Image { QString path; QSize size; QByteArray digest; };
    const CCaffArchive& archive;
    const Xml& xml;
    CExportControl* control;
    Graph result;
    QHash<int, Layer> layers;
    QHash<QString, int> documents;
    QHash<int, Image> images;
    QHash<QByteArray, QSize> verifiedPngs;
    QSet<QString> modelImages;
    qint64 numbers = 0, imagePixels = 0, edges = 0;

    void edge()
    {
        require(++edges <= MaximumRecords * 8LL, QStringLiteral("CMO 素材关系数量超出预算"));
        if ((edges & 255) == 0) checkpoint(control);
    }
    QString identity(int node) const
    {
        return xml.nodes[node].attributes.value(QStringLiteral("xs.id"), QStringLiteral("node:%1").arg(node));
    }
    bool knownFields(int node, const QSet<QString>& known, QStringList& diagnostics) const
    {
        bool complete = true;
        for (int child : xml.nodes[node].children) {
            const QString name = xml.nodes[child].attributes.value(QStringLiteral("xs.n"));
            if (known.contains(name) || xml.empty(child)) continue;
            require(diagnostics.size() < 4096, QStringLiteral("CMO 素材诊断数量超出预算"));
            diagnostics.append(QStringLiteral("保留未解释字段 %1.%2，不能自动执行此来源关系").arg(xml.nodes[node].type, name));
            complete = false;
        }
        return complete;
    }
    QString guid(int node, const QString& type) const
    {
        node = xml.typed(node, type);
        const QString value = xml.nodes[node].attributes.value(QStringLiteral("uuid"));
        require(!value.isEmpty() && value.size() <= 1024 && !value.contains(QChar::Null), QStringLiteral("CMO 素材 GUID 无效"));
        return value;
    }
    QString id(int node, const QString& type) const
    {
        node = xml.typed(node, type);
        const QString value = xml.nodes[node].attributes.value(QStringLiteral("idstr"));
        require(!value.isEmpty() && value.size() <= 1024 && !value.contains(QChar::Null), QStringLiteral("CMO 素材标识无效"));
        return value;
    }
    QTransform transform(int node) const
    {
        node = xml.typed(node, "CAffine");
        double values[6];
        const char* fields[] = {"m00", "m01", "m02", "m10", "m11", "m12"};
        for (int i = 0; i < 6; ++i) {
            bool valid = false;
            values[i] = xml.nodes[node].attributes.value(QLatin1String(fields[i])).toDouble(&valid);
            require(valid && std::isfinite(values[i]), QStringLiteral("CMO 素材仿射矩阵无效"));
        }
        return QTransform(values[0], values[3], values[1], values[4], values[2], values[5]);
    }
    QTransform optionalTransform(int node) const
    {
        node = xml.resolve(node);
        if (xml.nodes[node].type == QStringLiteral("null")) {
            require(xml.empty(node),
                    QStringLiteral("CMO 可选素材变换为空节点但包含内容"));
            return QTransform();
        }
        return transform(node);
    }
    QVector<double> values(int node)
    {
        node = xml.resolve(node);
        const auto& item = xml.nodes[node];
        const qint64 count = xml.number(item.attributes.value(QStringLiteral("count")));
        require(item.children.isEmpty() && count >= 0 && count <= MaximumNumbers - numbers,
                QStringLiteral("CMO 素材几何数量超出预算"));
        numbers += count;
        QVector<double> result;
        result.reserve(count);
        const QStringView text(item.value);
        qsizetype at = 0;
        while (at < text.size()) {
            while (at < text.size() && text[at].isSpace()) ++at;
            if (at == text.size()) break;
            const qsizetype start = at;
            while (at < text.size() && !text[at].isSpace()) ++at;
            require(at - start <= 128 && result.size() < count, QStringLiteral("CMO 素材数值数组无效"));
            bool valid = false;
            const double value = text.mid(start, at - start).toDouble(&valid);
            require(valid && std::isfinite(value), QStringLiteral("CMO 素材坐标不是有限数值"));
            result.append(value);
            if ((result.size() & 1023) == 0) checkpoint(control);
        }
        require(result.size() == count, QStringLiteral("CMO 素材数值数组数量不符"));
        return result;
    }
    QMap<QString, int> map(int node, const QString& keyType)
    {
        QMap<QString, int> result;
        for (int entry : xml.array(node)) {
            edge();
            entry = xml.typed(entry, "entry");
            const int key = xml.field(entry, "key");
            const QString value = keyType.endsWith(QStringLiteral("Guid")) ? guid(key, keyType) : id(key, keyType);
            require(!result.contains(value), QStringLiteral("CMO 素材映射键重复"));
            result.insert(value, xml.field(entry, "value"));
        }
        return result;
    }
    Image image(int node)
    {
        node = xml.typed(node, "CImageResource");
        if (images.contains(node)) return images.value(node);
        const auto& attributes = xml.nodes[node].attributes;
        const qint64 width = xml.number(attributes.value(QStringLiteral("width")));
        const qint64 height = xml.number(attributes.value(QStringLiteral("height")));
        require(width > 0 && height > 0 && width <= 30000 && height <= 30000 &&
                width * height <= 100000000 && width * height <= MaximumImagePixels - imagePixels,
                QStringLiteral("CMO 模型图像尺寸超出预算"));
        imagePixels += width * height;
        const int file = xml.typed(xml.field(node, "imageFileBuf"), "file");
        Image result;
        result.path = xml.nodes[file].attributes.value(QStringLiteral("path"));
        const auto* entry = archive.FindByPath(result.path);
        require(entry && entry->content.size() == xml.number(attributes.value(QStringLiteral("imageFileBuf_size"))),
                QStringLiteral("CMO 模型图像资源缺失或长度不符"));
        const auto verified = verifiedPngs.constFind(entry->content);
        if (verified != verifiedPngs.cend()) result.size = verified.value();
        else {
            QImage decoded;
            QString error;
            require(CPngImage::Decode(entry->content, decoded, &error), QStringLiteral("CMO 模型图像无法解码：%1").arg(error));
            result.size = decoded.size();
            verifiedPngs.insert(entry->content, result.size);
        }
        require(result.size == QSize(int(width), int(height)), QStringLiteral("CMO 模型图像尺寸不符"));
        checkpoint(control);
        result.digest = QCryptographicHash::hash(entry->content, QCryptographicHash::Sha256);
        images.insert(node, result);
        return result;
    }
    void readLayers(int manager)
    {
        for (int wrapper : xml.array(xml.field(manager, "_rawImages"))) {
            checkpoint(control);
            wrapper = xml.typed(wrapper, "LayeredImageWrapper");
            const int document = xml.typed(xml.field(wrapper, "image"), "CLayeredImage");
            const QString documentGuid = guid(xml.field(document, "guid"), "CLayeredImageGuid");
            require(!documents.contains(documentGuid) && documents.size() < 64, QStringLiteral("CMO 素材文档重复或数量超出预算"));
            documents.insert(documentGuid, document);
            const int inventory = xml.typed(xml.field(document, "layerSet"), "LayerSet");
            require(xml.field(inventory, "_layeredImage") == document, QStringLiteral("CMO 素材图层目录文档不一致"));
            QSet<QString> ids;
            int index = 0;
            for (int layer : xml.array(xml.field(inventory, "_layerEntryList"))) {
                edge();
                layer = xml.resolve(layer);
                require(!layers.contains(layer) && layers.size() < MaximumRecords, QStringLiteral("CMO 素材图层重复或数量超出预算"));
                Layer item;
                item.documentGuid = documentGuid; item.node = layer; item.index = index++;
                item.group = xml.nodes[layer].type == QStringLiteral("CLayerGroup");
                require(item.group || xml.nodes[layer].type == QStringLiteral("CLayer"), QStringLiteral("CMO 素材图层类型不支持"));
                const int super = xml.typed(xml.field(layer, "super"), item.group ? "ACLayerGroup" : "ACImageLayer");
                const int common = xml.typed(xml.field(super, "super"), "ACLayerEntry");
                require(xml.field(common, "_layeredImage") == document, QStringLiteral("CMO 素材图层文档不一致"));
                item.guid = guid(xml.field(common, "guid"), "CLayerGuid");
                require(!ids.contains(item.guid), QStringLiteral("CMO 素材图层 GUID 重复"));
                ids.insert(item.guid);
                item.name = xml.text(xml.field(common, "name"));
                if (!item.group) {
                    const int bounds = xml.typed(xml.field(layer, "boundsOnImageDoc"), "CRect");
                    const qint64 x = xml.integer(bounds, "x"), y = xml.integer(bounds, "y");
                    const qint64 width = xml.integer(bounds, "width"), height = xml.integer(bounds, "height");
                    require(x >= -1000000000 && x <= 1000000000 && y >= -1000000000 && y <= 1000000000 &&
                            width >= 0 && height >= 0 && width <= 30000 && height <= 30000,
                            QStringLiteral("CMO 素材图层边界无效"));
                    item.bounds = QRect(int(x), int(y), int(width), int(height));
                    item.image = xml.field(layer, "imageResource");
                }
                layers.insert(layer, std::move(item));
            }
        }
    }
    bool standardFilter(int filter, Graph::ModelImage& image)
    {
        filter = xml.typed(filter, "ModelImageFilterSet");
        image.filterId = identity(filter);
        const int base = xml.typed(xml.field(filter, "super"), "FilterSet");
        const auto filters = map(xml.field(base, "filterMap"), "FilterInstanceId");
        int selector = -1, layerFilter = -1;
        for (auto it = filters.cbegin(); it != filters.cend(); ++it) {
            const int instance = xml.typed(it.value(), "FilterInstance");
            require(xml.field(instance, "ownerFilterSet") == filter && id(xml.field(instance, "filterId"), "FilterInstanceId") == it.key(),
                    QStringLiteral("CMO 滤镜归属或标识不一致"));
            image.filterTypes.append(xml.nodes[instance].attributes.value(QStringLiteral("filterName")));
            const int definition = xml.field(instance, "filterDefGuid");
            const QString type = xml.nodes[definition].type;
            const QString definitionGuid = type.endsWith(QStringLiteral("Guid")) ? xml.nodes[definition].attributes.value(QStringLiteral("uuid")) : QString();
            image.filterGuids.append(definitionGuid);
            if (definitionGuid == QStringLiteral("5e9fe1ea-0ec3-4d68-a5fa-018fc7abe301") && selector < 0) selector = instance;
            if (definitionGuid == QStringLiteral("4083cd1f-40ba-4eda-8400-379019d55ed8") && layerFilter < 0) layerFilter = instance;
        }
        if (filters.size() != 2 || selector < 0 || layerFilter < 0) return false;
        for (int instance : {selector, layerFilter}) if (!xml.empty(xml.field(instance, "filterDef"))) return false;
        const auto inputs = map(xml.field(selector, "inputConnectors"), "FilterValueId");
        if (inputs.size() != 2) return false;
        const auto environment = [&](const QString& key, const QString& value) {
            const int node = inputs.value(key, -1);
            return node >= 0 && xml.nodes[node].type == QStringLiteral("EnvValueConnector") &&
                   id(xml.field(node, "envValueId"), "FilterValueId") == value;
        };
        if (!environment("ilf_inputLayerData", "mi_input_layerInputData") || !environment("ilf_currentImageGuid", "mi_currentImageGuid")) return false;
        const auto outputs = map(xml.field(selector, "outputConnectors"), "FilterValueId");
        const int connector = outputs.value("ilf_outputLayerData", -1);
        if (outputs.size() != 1 || connector < 0 || xml.nodes[connector].type != QStringLiteral("FilterOutputValueConnector")) return false;
        if (xml.field(connector, "instance") != selector || id(xml.field(connector, "id"), "FilterValueId") != QStringLiteral("ilf_outputLayerData")) return false;
        const int definition = xml.typed(xml.field(connector, "valueDef"), "FilterValue");
        if (id(xml.field(definition, "id"), "FilterValueId") != QStringLiteral("ilf_outputLayerData")) return false;
        const auto layerInputs = map(xml.field(layerFilter, "inputConnectors"), "FilterValueId");
        if (layerInputs.size() != 1 || layerInputs.value("ilf_inputLayer", -1) != connector ||
            !map(xml.field(layerFilter, "outputConnectors"), "FilterValueId").isEmpty()) return false;
        const auto external = [&](const QString& field, const QMap<QString, QPair<int, QString>>& expected) {
            const auto links = map(xml.field(base, field), "FilterValueId");
            if (links.size() != expected.size()) return false;
            for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
                const int link = links.value(it.key(), -1);
                if (link < 0 || xml.nodes[link].type != QStringLiteral("EnvConnection") || xml.field(link, "filter") != it.value().first) return false;
                const int environment = xml.typed(xml.field(link, "_envValueDef"), "FilterValue");
                const int port = xml.typed(xml.field(link, "filterValueDef"), "FilterValue");
                if (id(xml.field(environment, "id"), "FilterValueId") != it.key() ||
                    id(xml.field(port, "id"), "FilterValueId") != it.value().second) return false;
            }
            return true;
        };
        return external("_externalInputs", {{"mi_input_layerInputData", {selector, "ilf_inputLayerData"}},
                                           {"mi_currentImageGuid", {selector, "ilf_currentImageGuid"}}}) &&
               external("_externalOutputs", {{"mi_output_image", {layerFilter, "ilf_outputImageRes"}},
                                            {"mi_output_transform", {layerFilter, "ilf_outputTransform"}}});
    }
    Graph::ModelImage readImage(int node, int group)
    {
        checkpoint(control);
        Graph::ModelImage result;
        result.guid = guid(xml.field(node, "guid"), "CModelImageGuid");
        result.name = xml.text(xml.field(node, "name")); result.groupId = identity(group);
        const bool known = knownFields(node, {"guid", "name", "inputFilter", "inputFilterEnv", "_filteredImage", "icon16", "icon64",
            "_materialLocalToCanvasTransform", "_group", "cachedImageManager", "requiredMipmapLevel", "linkedRawImageGuids"}, result.diagnostics);
        require(xml.field(node, "_group") == group, QStringLiteral("CMO 模型图像所属组不一致"));
        const int materialTransform = xml.field(node, "_materialLocalToCanvasTransform");
        if (xml.nodes[materialTransform].type == QStringLiteral("null")) {
            for (auto it = xml.nodes[materialTransform].attributes.cbegin(); it != xml.nodes[materialTransform].attributes.cend(); ++it) {
                if (it.key() == QStringLiteral("xs.n") || it.key() == QStringLiteral("xs.id")) continue;
                if (it.key() == QStringLiteral("xs.idx")) {
                    bool valid = false; const auto index = it.value().toLongLong(&valid);
                    require(valid && index >= 0, QStringLiteral("CMO 空素材变换索引无效")); continue;
                }
                require(false, QStringLiteral("CMO 空素材变换包含未解释属性"));
            }
            require(xml.nodes[materialTransform].children.isEmpty() && xml.nodes[materialTransform].value.trimmed().isEmpty(),
                    QStringLiteral("CMO 空素材变换包含未解释内容"));
            result.materialLocalToCanvas = QTransform();
            result.diagnostics.append(QStringLiteral("素材到画布变换未保存，按单位变换读取；不能单凭此证明原画关联"));
        } else {
            result.materialLocalToCanvas = transform(materialTransform);
            result.materialTransformPresent = true;
        }
        const int filtered = xml.field(node, "_filteredImage");
        if (xml.nodes[filtered].type != QStringLiteral("null")) {
            const auto value = image(filtered);
            result.imageResourcePath = value.path; result.imageSize = value.size; result.imageDigest = value.digest;
        } else result.diagnostics.append(QStringLiteral("模型图像未保存过滤后的图片"));
        const bool filter = standardFilter(xml.field(node, "inputFilter"), result);
        if (!filter) result.diagnostics.append(QStringLiteral("滤镜关系不属于已验证的直接图层输入，保留关系但不自动执行"));
        const int environment = xml.typed(xml.field(node, "inputFilterEnv"), "ModelImageFilterEnv");
        const int base = xml.typed(xml.field(environment, "super"), "FilterEnv");
        const bool inherited = !xml.empty(xml.field(base, "parentEnv"));
        const auto values = map(xml.field(base, "envValues"), "FilterValueId");
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            xml.typed(it.value(), "EnvValueSet");
            require(id(xml.field(it.value(), "id"), "FilterValueId") == it.key(), QStringLiteral("CMO 素材环境键与值标识不一致"));
        }
        const int current = values.value("mi_currentImageGuid", -1);
        const int input = values.value("mi_input_layerInputData", -1);
        if (inherited || current < 0 || input < 0)
            result.diagnostics.append(QStringLiteral("模型图像使用继承或其他环境输入，不能确定直接来源"));
        if (current >= 0) {
            result.currentDocumentGuid = guid(xml.field(current, "value"), "CLayeredImageGuid");
            require(documents.contains(result.currentDocumentGuid), QStringLiteral("CMO 当前源文档不在活动目录中"));
        }
        if (input < 0) return result;
        const int selector = xml.typed(xml.field(input, "value"), "CLayerSelectorMap");
        const auto candidates = map(xml.field(selector, "_imageToLayerInput"), "CLayeredImageGuid");
        bool currentFound = false;
        for (auto it = candidates.cbegin(); it != candidates.cend(); ++it) {
            require(documents.contains(it.key()), QStringLiteral("CMO 图层输入源文档不在活动目录中"));
            Graph::Candidate candidate; candidate.documentGuid = it.key(); candidate.current = it.key() == result.currentDocumentGuid;
            currentFound |= candidate.current;
            for (int input : xml.array(it.value())) {
                edge(); input = xml.typed(input, "CLayerInputData");
                const int layer = xml.field(input, "layer");
                Graph::LayerInput entry;
                knownFields(input, {"layer", "affine", "clippingOnTexturePx"}, entry.diagnostics);
                entry.affine = transform(xml.field(input, "affine"));
                const int clipping = xml.field(input, "clippingOnTexturePx", false);
                entry.clippingType = clipping < 0 || xml.nodes[clipping].type == QStringLiteral("null")
                    ? QString() : xml.nodes[clipping].type;
                const auto sourceIt = layers.constFind(layer);
                if (sourceIt == layers.cend()) {
                    const int resolvedLayer = xml.resolve(layer);
                    const bool group = xml.nodes[resolvedLayer].type == QStringLiteral("CLayerGroup");
                    require(group || xml.nodes[resolvedLayer].type == QStringLiteral("CLayer"),
                            QStringLiteral("CMO 图层输入引用未知图层类型"));
                    const int super = xml.typed(xml.field(resolvedLayer, "super"),
                                                group ? "ACLayerGroup" : "ACImageLayer");
                    const int common = xml.typed(xml.field(super, "super"), "ACLayerEntry");
                    entry.documentGuid = it.key();
                    entry.layerGuid = guid(xml.field(common, "guid"), "CLayerGuid");
                    entry.layerName = xml.text(xml.field(common, "name"));
                    entry.sourceIndex = -1;
                    entry.group = group;
                    entry.diagnostics.append(QStringLiteral("来源图层不在活动文档目录中，保留候选关系但不自动采样"));
                } else {
                    require(sourceIt->documentGuid == it.key(), QStringLiteral("CMO 图层输入引用其他文档或非活动图层"));
                    const auto& source = sourceIt.value();
                    entry.documentGuid = source.documentGuid; entry.layerGuid = source.guid; entry.layerName = source.name;
                    entry.sourceIndex = source.index; entry.group = source.group;
                }
                if (entry.group) entry.diagnostics.append(QStringLiteral("来源为图层组，需要独立处理合成语义"));
                if (!entry.affine.isIdentity()) entry.diagnostics.append(QStringLiteral("保留非恒等图层输入变换，尚未验证其组合顺序"));
                if (clipping >= 0 && !xml.empty(clipping)) entry.diagnostics.append(QStringLiteral("图层输入包含局部裁剪，尚未执行"));
                const bool directCandidate = !entry.group && !result.imageDigest.isEmpty();
                const Layer* source = sourceIt == layers.cend() ? nullptr : &sourceIt.value();
                candidate.layers.append(std::move(entry));
                if (candidate.current && candidate.layers.size() == 1 && directCandidate && source &&
                    source->image >= 0 && xml.nodes[source->image].type == QStringLiteral("CImageResource") &&
                    result.imageSize == source->bounds.size() && result.materialLocalToCanvas == QTransform::fromTranslate(source->bounds.x(), source->bounds.y())) {
                    const auto original = image(source->image);
                    result.directRaster = result.materialTransformPresent && filter && known && !inherited &&
                        candidate.layers.first().diagnostics.isEmpty() && original.digest == result.imageDigest;
                }
            }
            if (candidate.current && candidate.layers.size() != 1) result.directRaster = false;
            result.candidates.append(std::move(candidate));
        }
        require(current < 0 || currentFound, QStringLiteral("CMO 当前源文档缺少图层输入候选"));
        if (!result.directRaster) result.diagnostics.append(QStringLiteral("来源关系已保留，但未证明可以直接按单层原画采样"));
        return result;
    }
    Graph::Mesh readMesh(int node)
    {
        Graph::Mesh result;
        const int body = xml.typed(xml.field(node, "super"), "ACDrawableSource");
        const int common = xml.typed(xml.field(body, "super"), "ACParameterControllableSource");
        result.id = id(xml.field(body, "id"), "CDrawableId");
        result.guid = guid(xml.field(body, "guid"), "CDrawableGuid");
        result.name = xml.text(xml.field(common, "localName"));
        const int state = xml.field(node, "textureState", false);
        if (state >= 0) result.textureState = xml.nodes[state].attributes.value(QStringLiteral("v"));
        else result.diagnostics.append(QStringLiteral("旧工程未保存 textureState，保留实际纹理输入及原始 UV"));
        const auto positions = values(xml.field(node, "positions")), uvs = values(xml.field(node, "uvs"));
        require(positions.size() % 2 == 0 && positions.size() == uvs.size(), QStringLiteral("CMO 素材坐标与 UV 数量不符"));
        for (int index = 0; index < positions.size(); index += 2) {
            result.materialPositions.append(QPointF(positions[index], positions[index + 1]));
            result.materialUvs.append(QPointF(uvs[index], uvs[index + 1]));
        }
        const auto indices = values(xml.field(node, "indices"));
        require(indices.size() % 3 == 0, QStringLiteral("CMO 素材三角形索引数量无效"));
        for (double index : indices) {
            require(index >= 0 && index <= std::numeric_limits<int>::max() && index < result.materialPositions.size() && index == std::floor(index),
                    QStringLiteral("CMO 素材三角形索引越界"));
            result.indices.append(int(index));
        }
        int extensions = 0;
        for (int extension : xml.array(xml.field(common, "_extensions"))) {
            edge(); extension = xml.resolve(extension);
            if (xml.nodes[extension].type != QStringLiteral("CTextureInputExtension")) continue;
            require(++extensions == 1, QStringLiteral("CMO 素材纹理扩展重复"));
            const int base = xml.typed(xml.field(extension, "super"), "ACExtension");
            require(xml.field(base, "_owner") == node, QStringLiteral("CMO 素材纹理扩展所属网格不一致"));
            const int current = xml.field(extension, "currentTextureInputData");
            QSet<int> inputs;
            for (int input : xml.array(xml.field(extension, "_textureInputs"))) {
                edge(); input = xml.resolve(input);
                require(!inputs.contains(input), QStringLiteral("CMO 素材纹理输入重复"));
                inputs.insert(input);
                Graph::TextureInput entry; entry.identity = identity(input); entry.type = xml.nodes[input].type;
                entry.current = input == current;
                const int base = xml.typed(xml.field(input, "super"), "ACTextureInput");
                require(xml.field(base, "_owner") == extension, QStringLiteral("CMO 素材纹理输入所属扩展不一致"));
                const int optionalTransformNode = xml.resolve(xml.field(base, "optionalTransformOnCanvas"));
                entry.optionalTransformOnCanvas = optionalTransform(optionalTransformNode);
                if (xml.nodes[optionalTransformNode].type == QStringLiteral("null"))
                    entry.diagnostics.append(QStringLiteral("画布变换未保存，按单位变换读取"));
                if (!entry.optionalTransformOnCanvas.isIdentity()) entry.diagnostics.append(QStringLiteral("保留非恒等画布变换，尚未验证其组合顺序"));
                if (entry.type == QStringLiteral("CTextureInput_ModelImage")) {
                    knownFields(input, {"super", "_modelImageGuid", "cached_modelImageVersion"}, entry.diagnostics);
                    entry.modelImageGuid = guid(xml.field(input, "_modelImageGuid"), "CModelImageGuid");
                    require(modelImages.contains(entry.modelImageGuid), QStringLiteral("CMO 纹理输入模型图像不在活动集合中"));
                } else entry.diagnostics.append(QStringLiteral("保留纹理输入类型 %1，尚未解释其像素来源").arg(entry.type));
                result.inputs.append(std::move(entry));
            }
            require(xml.nodes[current].type == QStringLiteral("null") || inputs.contains(current), QStringLiteral("CMO 当前纹理输入不是列表中的同一对象"));
        }
        if (!extensions) result.diagnostics.append(QStringLiteral("此网格没有纹理输入扩展"));
        return result;
    }
};
}

const QByteArray& CCmo3Project::Bytes() const { static const QByteArray empty; return m_data ? m_data->bytes : empty; }
QString CCmo3Project::SourcePath() const { return m_data ? m_data->path : QString(); }
QByteArray CCmo3Project::SourceDigest() const { return m_data ? m_data->digest : QByteArray(); }
QString CCmo3Project::SourceVersion() const { return m_data ? m_data->version : QString(); }
const CPsdSourceCatalog* CCmo3Project::Artwork() const { return m_data && m_data->artwork ? &*m_data->artwork : nullptr; }
const CCmo3NameReader::Catalog* CCmo3Project::Names() const { return m_data && m_data->names ? &*m_data->names : nullptr; }
const CCmo3MaterialGraph* CCmo3Project::Materials() const { return m_data && m_data->materials ? &*m_data->materials : nullptr; }
QString CCmo3Project::ArtworkError() const { return m_data ? m_data->artworkError : QStringLiteral("CMO 工程尚未读取"); }
QString CCmo3Project::NamesError() const { return m_data ? m_data->namesError : QStringLiteral("CMO 工程尚未读取"); }
QString CCmo3Project::MaterialsError() const { return m_data ? m_data->materialsError : QStringLiteral("CMO 工程尚未读取"); }

bool CCmo3Project::OpenOwned(QByteArray bytes, QString path, CCmo3Project& result,
                            QString* error, CExportControl* control)
{
    try {
        auto data = std::make_shared<Data>();
        data->bytes = std::move(bytes); data->path = std::move(path);
        data->digest = QCryptographicHash::hash(data->bytes, QCryptographicHash::Sha256);
        CCaffArchive archive;
        const bool loaded = archive.LoadOwned(data->bytes, error, control, {});
        require(loaded, error ? *error : QStringLiteral("CMO 归档读取失败"));
        const CCaffArchive::Entry* main = nullptr;
        for (const auto& entry : archive.Entries()) if (entry.tag == QStringLiteral("main_xml")) {
            require(!main, QStringLiteral("CMO 包含多个主 XML"));
            main = &entry;
        }
        require(main, QStringLiteral("CMO 缺少主 XML"));
        Xml xml;
        xml.parse(main->content, control);
        data->version = xml.sourceVersion();
        modelRoot(xml);
        try { data->artwork = CCmo3ArtworkReader::Extract(archive, xml, data->path, data->digest, control); }
        catch (const ReadError& failure) { data->artworkError = failure.message; }
        checkpoint(control);
        try { data->names = CCmo3NameReader::Extract(xml, data->path, data->digest, control); }
        catch (const ReadError& failure) { data->namesError = failure.message; }
        checkpoint(control);
        try { data->materials = MaterialReader(archive, xml, data->artwork ? &*data->artwork : nullptr, control).read(); }
        catch (const ReadError& failure) { data->materialsError = failure.message; }
        checkpoint(control);
        CCmo3Project candidate; candidate.m_data = std::move(data);
        result = std::move(candidate);
        return true;
    } catch (const detail::ReadAborted& failure) { if (error) *error = failure.message; }
    catch (const ReadError& failure) { if (error) *error = failure.message; }
    catch (const std::bad_alloc&) { if (error) *error = QStringLiteral("CMO 工程读取内存不足"); }
    catch (const std::length_error&) { if (error) *error = QStringLiteral("CMO 工程长度超出可表示范围"); }
    return false;
}

bool CCmo3Project::OpenBytes(const QByteArray& input, const QString& sourcePath, CCmo3Project& result,
                            QString* error, CExportControl* control)
{
    if (error) error->clear();
    try {
        require(input.size() <= CCaffReadLimits{}.maximumFileBytes && sourcePath.size() <= 32768 &&
                !sourcePath.contains(QChar::Null) && sourcePath.isValidUtf16(), QStringLiteral("CMO 输入或来源路径无效"));
        const QByteArray bytes(input.constData(), input.size());
        return OpenOwned(bytes, owned(sourcePath), result, error, control);
    } catch (const ReadError& failure) { if (error) *error = failure.message; }
    catch (const std::bad_alloc&) { if (error) *error = QStringLiteral("CMO 工程输入内存不足"); }
    catch (const std::length_error&) { if (error) *error = QStringLiteral("CMO 工程输入长度超出可表示范围"); }
    return false;
}

bool CCmo3Project::OpenFile(const QString& sourcePath, CCmo3Project& result,
                           QString* error, CExportControl* control)
{
    if (error) error->clear();
    try {
        require(!sourcePath.isEmpty() && sourcePath.size() <= 32768 && !sourcePath.contains(QChar::Null) &&
                sourcePath.isValidUtf16(), QStringLiteral("CMO 来源路径无效"));
        const QString path = QFileInfo(owned(sourcePath)).canonicalFilePath();
        require(!path.isEmpty(), QStringLiteral("CMO 来源文件不存在"));
        QByteArray bytes;
        if (!CCaffArchive::ReadFileBytes(path, bytes, error, control)) return false;
        return OpenOwned(std::move(bytes), path, result, error, control);
    } catch (const ReadError& failure) { if (error) *error = failure.message; }
    catch (const std::bad_alloc&) { if (error) *error = QStringLiteral("CMO 工程文件内存不足"); }
    catch (const std::length_error&) { if (error) *error = QStringLiteral("CMO 工程文件长度超出可表示范围"); }
    return false;
}
}
