#include "ccmo3artworkreader.h"
#include "ccmo3project.h"
#include "ccmo3xml_p.h"
#include "ccaffarchive.h"
#include "std/cexportcontrol.h"
#include "std/cpngimage.h"
#include "psd/psdSamples/Zip/Psdminiz.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QSet>
#include <QXmlStreamReader>
#include <QtEndian>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace cmo3 {
namespace {
constexpr qint64 MaximumMetadataBytes = 128LL * 1024 * 1024;
constexpr qint64 MaximumTotalPixels = 256LL * 1024 * 1024;
constexpr qint64 MaximumLayerPixels = 100000000;
constexpr int MaximumDimension = 30000;
constexpr int MaximumLayers = 32767;
constexpr int MaximumDepth = 128;
using detail::ReadError;
using detail::require;
using detail::checkpoint;
using detail::Xml;

const QMap<QString, QByteArray>& blendModes()
{
    static const QMap<QString, QByteArray> result {
        {"CBlend_Normal", "norm"}, {"CBlend_Normal_Original", "norm"}, {"CBlend_Dissolve", "diss"}, {"CBlend_Darken", "dark"},
        {"CBlend_Multiply", "mul "}, {"CBlend_ColorBurn", "idiv"}, {"CBlend_LinearBurn", "lbrn"},
        {"CBlend_DarkerColor", "dkCl"}, {"CBlend_Lighten", "lite"}, {"CBlend_Screen", "scrn"},
        {"CBlend_ColorDodge", "div "}, {"CBlend_LinearDodge", "lddg"}, {"CBlend_LighterColor", "lgCl"},
        {"CBlend_Overray", "over"}, {"CBlend_SoftLight", "sLit"}, {"CBlend_HardLight", "hLit"},
        {"CBlend_VividLight", "vLit"}, {"CBlend_LinearLight", "lLit"}, {"CBlend_PinLight", "pLit"},
        {"CBlend_HardMix", "hMix"}, {"CBlend_Difference", "diff"}, {"CBlend_Exclusion", "smud"},
        {"CBlend_Subtract", "fsub"}, {"CBlend_Divide", "fdiv"}, {"CBlend_Hue", "hue "},
        {"CBlend_Saturation", "sat "}, {"CBlend_Color", "colr"}, {"CBlend_Luminosity", "lum "},
        {"CBlend_PassThrough", "pass"}
    };
    return result;
}
void diagnostic(CPsdSourceDocument& document, CPsdSourceDiagnostic::Severity severity,
                const QString& code, const QString& message, const QString& layer = {})
{
    require(document.diagnostics.size() < 4096, QStringLiteral("CMO 来源诊断数量超出预算"));
    document.diagnostics.append({severity, code, message, layer});
}
struct PngInfo { QSize size; QByteArray icc; };
PngInfo validatePng(const QByteArray& png, qint64& profileWork)
{
    require(png.size() >= 33 && png.left(8) == QByteArray::fromHex("89504e470d0a1a0a"),
            QStringLiteral("CMO 源图像不是 PNG"));
    const auto* bytes = reinterpret_cast<const uchar*>(png.constData());
    PngInfo result;
    bool header = false, end = false, data = false;
    qsizetype at = 8;
    while (at < png.size()) {
        require(png.size() - at >= 12, QStringLiteral("CMO PNG 块截断"));
        const quint32 length = qFromBigEndian<quint32>(bytes + at);
        require(length <= quint64(png.size() - at - 12), QStringLiteral("CMO PNG 块越界"));
        const QByteArray type(png.constData() + at + 4, 4);
        const auto* payload = bytes + at + 8;
        require(quint32(mz_crc32(MZ_CRC32_INIT, bytes + at + 4, size_t(length) + 4)) ==
                qFromBigEndian<quint32>(payload + length), QStringLiteral("CMO PNG 校验和不匹配"));
        if (type == "IHDR") {
            require(!header && at == 8 && length == 13, QStringLiteral("CMO PNG 图像头无效"));
            const quint32 width = qFromBigEndian<quint32>(payload), height = qFromBigEndian<quint32>(payload + 4);
            require(width > 0 && height > 0 && width <= MaximumDimension && height <= MaximumDimension &&
                    quint64(width) * height <= MaximumLayerPixels, QStringLiteral("CMO PNG 尺寸超出预算"));
            require(payload[8] == 8 && (payload[9] == 2 || payload[9] == 6) &&
                    payload[10] == 0 && payload[11] == 0 && payload[12] <= 1,
                    QStringLiteral("CMO 源图像仅支持 8 位 RGB/RGBA PNG"));
            result.size = QSize(int(width), int(height));
            header = true;
        } else if (type == "IDAT") {
            require(header && !end, QStringLiteral("CMO PNG 像素块顺序无效"));
            data = true;
        } else if (type == "iCCP") {
            require(header && !data && result.icc.isEmpty(), QStringLiteral("CMO PNG ICC 块重复或顺序无效"));
            require(length <= 32 * 1024 * 1024, QStringLiteral("CMO PNG ICC 编码超出预算"));
            const QByteArray chunk = QByteArray::fromRawData(reinterpret_cast<const char*>(payload), length);
            const int nameEnd = chunk.indexOf('\0');
            require(nameEnd >= 1 && nameEnd <= 79 && nameEnd + 2 < chunk.size() && chunk[nameEnd + 1] == 0,
                    QStringLiteral("CMO PNG ICC 编码无效"));
            const qint64 profileLimit = std::min(qint64(16 * 1024 * 1024), MaximumMetadataBytes - profileWork);
            require(profileLimit > 0, QStringLiteral("CMO ICC 解码总量超出预算"));
            QByteArray profile(profileLimit, Qt::Uninitialized);
            mz_ulong size = profile.size();
            require(mz_uncompress(reinterpret_cast<unsigned char*>(profile.data()), &size,
                    payload + nameEnd + 2, length - nameEnd - 2) == MZ_OK && size > 0,
                    QStringLiteral("CMO PNG ICC 无效或超出预算"));
            profileWork += qint64(size);
            profile.resize(qsizetype(size));
            profile.squeeze();
            result.icc = std::move(profile);
        } else if (type == "IEND") {
            require(header && data && length == 0 && at + 12 == png.size(), QStringLiteral("CMO PNG 结束块无效"));
            end = true;
        } else {
            require((type[0] & 0x20) || type == "PLTE", QStringLiteral("CMO PNG 包含未知关键块"));
        }
        at += qsizetype(length) + 12;
    }
    require(end, QStringLiteral("CMO PNG 缺少结束块"));
    QImage decoded;
    QString error;
    require(CPngImage::Decode(png, decoded, &error) && decoded.size() == result.size,
            QStringLiteral("CMO PNG 无法完整解码：%1").arg(error));
    return result;
}
class Reader
{
public:
    const CCaffArchive& archive;
    CExportControl* control;
    const Xml& xml;
    QHash<QString, const CCaffArchive::Entry*> resources;
    QHash<QString, PngInfo> pngs;
    QSet<QString> unprofiledDocuments;
    QHash<QByteArray, QByteArray> iccProfiles;
    qint64 profileBytes = 0, profileWork = 0;
    qint64 pixels = 0;
    int layers = 0;

    Reader(const CCaffArchive& input, const Xml& parsed, CExportControl* progress)
        : archive(input), control(progress), xml(parsed) {}
    QString guid(int owner, const QString& type)
    {
        const int value = xml.typed(xml.field(owner, QStringLiteral("guid")), type);
        const QString result = xml.nodes[value].attributes.value(QStringLiteral("uuid"));
        require(!result.isEmpty() && result.size() <= 32768, QStringLiteral("CMO 来源 GUID 缺失或过长"));
        return result;
    }
    void unknownFields(int node, const QSet<QString>& known, CPsdSourceDocument& document, const QString& id)
    {
        for (int child : xml.nodes[node].children) {
            const QString name = xml.nodes[child].attributes.value(QStringLiteral("xs.n"));
            if (!known.contains(name) && !xml.empty(child))
                diagnostic(document, CPsdSourceDiagnostic::Severity::Error, QStringLiteral("unsupported-source-field"),
                           QStringLiteral("来源字段 %1.%2 尚未解释，不能保证源图层效果完整").arg(xml.nodes[node].type, name), id);
        }
    }
    std::optional<quint32> externalId(int node)
    {
        const int identifier = xml.field(node, QStringLiteral("layerIdentifier"), false);
        if (identifier < 0 || xml.nodes[identifier].type == QStringLiteral("null")) return {};
        xml.typed(identifier, QStringLiteral("CLayerIdentifier"));
        const int raw = xml.field(identifier, QStringLiteral("layerId"));
        const int numeric = xml.field(identifier, QStringLiteral("layerIdValue_testImpl"), false);
        if (xml.nodes[raw].type == QStringLiteral("null")) {
            require(numeric < 0 || xml.number(xml.text(numeric)) == -1, QStringLiteral("CMO 空图层标识不一致"));
            return {};
        }
        const QString value = xml.text(raw);
        require(value.size() == 11 && value[2] == '-' && value[5] == '-' && value[8] == '-',
                QStringLiteral("CMO 源图层标识格式无效"));
        QString digits = value;
        digits.remove('-');
        bool valid = false;
        const quint32 id = digits.toUInt(&valid, 16);
        require(valid, QStringLiteral("CMO 源图层标识不是十六进制"));
        if (numeric >= 0) {
            const qint64 number = xml.number(xml.text(numeric));
            require(number >= std::numeric_limits<qint32>::min() && number <= std::numeric_limits<quint32>::max() &&
                    quint32(number) == id, QStringLiteral("CMO 源图层标识值不一致"));
        }
        return id;
    }
    CPsdSourceLayer common(int node, int documentNode, int parentNode, CPsdSourceDocument& document)
    {
        CPsdSourceLayer layer;
        const bool group = xml.nodes[node].type == QStringLiteral("CLayerGroup");
        require(group || xml.nodes[node].type == QStringLiteral("CLayer"), QStringLiteral("不支持此来源图层类型"));
        layer.kind = group ? CPsdSourceLayer::Kind::Group : CPsdSourceLayer::Kind::Raster;
        const int super = xml.typed(xml.field(node, QStringLiteral("super")),
                                    group ? QStringLiteral("ACLayerGroup") : QStringLiteral("ACImageLayer"));
        const int entry = xml.typed(xml.field(super, QStringLiteral("super")), QStringLiteral("ACLayerEntry"));
        layer.id = guid(entry, QStringLiteral("CLayerGuid"));
        layer.name = xml.text(xml.field(entry, QStringLiteral("name")));
        layer.visible = xml.boolean(entry, QStringLiteral("isVisible"));
        layer.clipping = xml.boolean(entry, QStringLiteral("isClipping"));
        const qint64 opacity = xml.integer(entry, QStringLiteral("opacity255"));
        require(opacity >= 0 && opacity <= 255, QStringLiteral("CMO 源图层透明度越界"));
        layer.opacity = quint8(opacity);
        const int owner = xml.field(entry, QStringLiteral("_layeredImage"));
        const int parent = xml.field(entry, QStringLiteral("group"));
        require(owner == documentNode && (parentNode < 0 ? xml.nodes[parent].type == QStringLiteral("null") : parent == parentNode),
                QStringLiteral("CMO 来源图层父级或文档引用不一致"));
        const int blend = xml.field(entry, QStringLiteral("blend"));
        const auto& type = xml.nodes[blend].type;
        layer.blendMode = blendModes().value(type);
        if (type == QStringLiteral("CBlend_Normal_Original"))
            diagnostic(document, CPsdSourceDiagnostic::Severity::Information, QStringLiteral("legacy-normal-blend"),
                       QStringLiteral("保留旧混合类型 CBlend_Normal_Original，按普通图层写入；不保证合成算法逐像素一致"), layer.id);
        if (type == QStringLiteral("CBlend_NotSupported"))
            layer.blendMode = xml.text(xml.field(blend, QStringLiteral("blendKey"))).toLatin1();
        if (!blendModes().values().contains(layer.blendMode) || (!group && layer.blendMode == "pass"))
            diagnostic(document, CPsdSourceDiagnostic::Severity::Error, QStringLiteral("unsupported-blend"),
                       QStringLiteral("来源混合模式 %1 尚不支持").arg(type), layer.id);
        unknownFields(blend, type == QStringLiteral("CBlend_NotSupported") ? QSet<QString>{"super", "blendKey"} :
                      QSet<QString>{"super"}, document, layer.id);
        const int blendBase = xml.field(blend, QStringLiteral("super"), false);
        if (blendBase >= 0) unknownFields(blendBase, {}, document, layer.id);
        layer.externalId = externalId(node);
        unknownFields(entry, {"name", "memo", "isVisible", "isClipping", "blend", "guid", "group", "opacity255", "_layeredImage"}, document, layer.id);
        unknownFields(super, group ? QSet<QString>{"super", "_children"} : QSet<QString>{"super"}, document, layer.id);
        unknownFields(node, group ? QSet<QString>{"super", "layerIdentifier"} :
                      QSet<QString>{"super", "imageResource", "boundsOnImageDoc", "layerIdentifier", "icon16", "icon64"}, document, layer.id);
        return layer;
    }
    void raster(int node, CPsdSourceLayer& layer, CPsdSourceDocument& document)
    {
        const int bounds = xml.typed(xml.field(node, QStringLiteral("boundsOnImageDoc")), QStringLiteral("CRect"));
        const qint64 x = xml.integer(bounds, QStringLiteral("x")), y = xml.integer(bounds, QStringLiteral("y"));
        const qint64 width = xml.integer(bounds, QStringLiteral("width")), height = xml.integer(bounds, QStringLiteral("height"));
        require((width == 0) == (height == 0), QStringLiteral("CMO 空源图层必须是 0×0"));
        require(width >= 0 && height >= 0 && width <= MaximumDimension && height <= MaximumDimension &&
                width * height <= MaximumLayerPixels && x >= std::numeric_limits<int>::min() &&
                x <= std::numeric_limits<int>::max() && y >= std::numeric_limits<int>::min() &&
                y <= std::numeric_limits<int>::max() && x + width <= std::numeric_limits<int>::max() &&
                y + height <= std::numeric_limits<int>::max() &&
                (width != 0 || x != std::numeric_limits<int>::min()) &&
                (height != 0 || y != std::numeric_limits<int>::min()), QStringLiteral("CMO 源图层边界越界或超出预算"));
        layer.bounds = QRect(int(x), int(y), int(width), int(height));
        require(width * height <= MaximumTotalPixels - pixels, QStringLiteral("CMO 源图层总像素数超出预算"));
        pixels += width * height;
        const int resource = xml.field(node, QStringLiteral("imageResource"));
        if (xml.nodes[resource].type == QStringLiteral("null")) {
            require(layer.bounds.isEmpty(), QStringLiteral("CMO 非空图层缺少源图像"));
            return;
        }
        xml.typed(resource, QStringLiteral("CImageResource"));
        const auto& attributes = xml.nodes[resource].attributes;
        require(attributes.value(QStringLiteral("type")) == QStringLiteral("INT_ARGB") ||
                attributes.value(QStringLiteral("type")) == QStringLiteral("INT_RGB"), QStringLiteral("不支持此 CMO 源图像像素格式"));
        require(xml.number(attributes.value(QStringLiteral("width"))) == width &&
                xml.number(attributes.value(QStringLiteral("height"))) == height, QStringLiteral("CMO 源图像声明与图层边界不一致"));
        const int file = xml.field(resource, QStringLiteral("imageFileBuf"));
        if (xml.nodes[file].type == QStringLiteral("null")) {
            require(layer.bounds.isEmpty(), QStringLiteral("CMO 非空图层缺少源 PNG"));
            return;
        }
        xml.typed(file, QStringLiteral("file"));
        const QString path = xml.nodes[file].attributes.value(QStringLiteral("path"));
        const auto found = resources.constFind(path);
        require(found != resources.cend(), QStringLiteral("CMO 内未找到源 PNG：%1").arg(path));
        const auto& content = found.value()->content;
        require(xml.number(attributes.value(QStringLiteral("imageFileBuf_size"))) == content.size(),
                QStringLiteral("CMO 源 PNG 声明长度不一致"));
        checkpoint(control);
        if (!pngs.contains(path)) {
            auto information = validatePng(content, profileWork);
            if (!information.icc.isEmpty()) {
                const auto digest = QCryptographicHash::hash(information.icc, QCryptographicHash::Sha256);
                const auto retained = iccProfiles.constFind(digest);
                if (retained != iccProfiles.cend()) {
                    require(retained.value() == information.icc, QStringLiteral("CMO ICC 摘要冲突"));
                    information.icc = retained.value();
                } else {
                    require(information.icc.size() <= MaximumMetadataBytes - xml.metadataBytes - profileBytes,
                            QStringLiteral("CMO ICC 与来源元数据总量超出预算"));
                    profileBytes += information.icc.size();
                    iccProfiles.insert(digest, information.icc);
                }
            }
            pngs.insert(path, std::move(information));
        }
        const auto& info = pngs[path];
        require(info.size == layer.bounds.size(), QStringLiteral("CMO 源 PNG 真实尺寸与边界不一致"));
        if (info.icc.isEmpty()) unprofiledDocuments.insert(document.id);
        if (!info.icc.isEmpty()) {
            if (document.iccProfile.isEmpty()) document.iccProfile = info.icc;
            else if (document.iccProfile != info.icc)
                diagnostic(document, CPsdSourceDiagnostic::Severity::Error, QStringLiteral("conflicting-icc"),
                           QStringLiteral("同一来源文档的图层 ICC 不一致，无法写入统一配置"), layer.id);
        }
        layer.png = content;
    }
    void visit(int node, int documentNode, int parentNode, int parentIndex, int depth, bool root,
               CPsdSourceDocument& document, QSet<int>& visited, QSet<QString>& ids, QSet<quint32>& externalIds)
    {
        checkpoint(control);
        node = xml.resolve(node);
        require(depth <= MaximumDepth && !visited.contains(node) && layers < MaximumLayers,
                QStringLiteral("CMO 来源图层循环、重复或数量超出预算"));
        visited.insert(node);
        ++layers;
        auto layer = common(node, documentNode, parentNode, document);
        require(!ids.contains(layer.id), QStringLiteral("CMO 来源图层 GUID 重复"));
        ids.insert(layer.id);
        if (layer.externalId) {
            if (externalIds.contains(*layer.externalId))
                diagnostic(document, CPsdSourceDiagnostic::Severity::Warning, QStringLiteral("duplicate-layer-id"),
                           QStringLiteral("来源 PSD 图层标识重复，保留原值"), layer.id);
            externalIds.insert(*layer.externalId);
        }
        const bool group = layer.kind == CPsdSourceLayer::Kind::Group;
        require(!root || group, QStringLiteral("CMO 来源根节点不是分组"));
        const bool omit = root && layer.name == QStringLiteral("root") && layer.visible && !layer.clipping &&
                          layer.opacity == 255 && layer.blendMode == "norm" && !layer.externalId;
        int nextParent = parentIndex;
        if (!omit) {
            layer.parentIndex = parentIndex;
            if (!group) raster(node, layer, document);
            nextParent = document.layers.size();
            document.layers.append(std::move(layer));
        }
        if (group) {
            const int groupNode = xml.field(node, QStringLiteral("super"));
            for (int child : xml.array(xml.field(groupNode, QStringLiteral("_children"))))
                visit(child, documentNode, node, nextParent, depth + 1, false, document, visited, ids, externalIds);
        }
    }
    CPsdSourceDocument document(int index)
    {
        index = xml.typed(index, QStringLiteral("CLayeredImage"));
        CPsdSourceDocument result;
        result.id = guid(index, QStringLiteral("CLayeredImageGuid"));
        result.name = xml.text(xml.field(index, QStringLiteral("name")));
        const qint64 width = xml.integer(index, QStringLiteral("width")), height = xml.integer(index, QStringLiteral("height"));
        require(width > 0 && height > 0 && width <= MaximumDimension && height <= MaximumDimension &&
                width * height <= MaximumLayerPixels, QStringLiteral("CMO 来源画布尺寸超出 PSD 预算"));
        result.canvasSize = QSize(int(width), int(height));
        unknownFields(index, {"name", "memo", "width", "height", "psdFile", "description", "guid", "psdFileLastModified",
                             "_rootLayer", "layerSet", "icon16", "icon64"}, result, {});
        QSet<int> visited;
        QSet<QString> ids;
        QSet<quint32> externalIds;
        visit(xml.field(index, QStringLiteral("_rootLayer")), index, -1, -1, 0, true, result, visited, ids, externalIds);
        const int inventory = xml.typed(xml.field(index, QStringLiteral("layerSet")), QStringLiteral("LayerSet"));
        require(xml.field(inventory, QStringLiteral("_layeredImage")) == index,
                QStringLiteral("CMO 来源图层目录指向其他文档"));
        QSet<int> listed;
        for (int item : xml.array(xml.field(inventory, QStringLiteral("_layerEntryList")))) {
            item = xml.resolve(item);
            require(!listed.contains(item), QStringLiteral("CMO 来源图层目录重复"));
            listed.insert(item);
        }
        require(listed == visited, QStringLiteral("CMO 来源图层树与目录不一致"));
        if (!result.iccProfile.isEmpty() && unprofiledDocuments.contains(result.id))
            diagnostic(result, CPsdSourceDiagnostic::Severity::Error, QStringLiteral("partial-icc"),
                       QStringLiteral("仅部分源图层保存 ICC，无法确认文档统一颜色配置"));
        if (result.iccProfile.isEmpty())
            diagnostic(result, CPsdSourceDiagnostic::Severity::Warning, QStringLiteral("missing-icc"),
                       QStringLiteral("工程未保存可恢复的 ICC，输出不指定颜色配置"));
        return result;
    }
    void read(CPsdSourceCatalog& result)
    {
        for (const auto& entry : archive.Entries()) resources.insert(entry.path, &entry);
        result.sourceVersion = xml.sourceVersion();
        int mainNode = -1;
        for (int child : xml.nodes[0].children) if (xml.nodes[child].type == QStringLiteral("main")) {
            require(mainNode < 0, QStringLiteral("CMO 包含多个 main 节点"));
            mainNode = child;
        }
        require(mainNode >= 0 && xml.nodes[mainNode].children.size() == 1, QStringLiteral("CMO main 节点无效"));
        const int model = xml.typed(xml.nodes[mainNode].children.first(), QStringLiteral("CModelSource"));
        const int manager = xml.typed(xml.field(model, QStringLiteral("textureManager")), QStringLiteral("CTextureManager"));
        const auto documents = xml.array(xml.field(manager, QStringLiteral("_rawImages")));
        require(documents.size() <= 64, QStringLiteral("CMO 来源文档数量超出预算"));
        QSet<QString> ids;
        for (int wrapper : documents) {
            wrapper = xml.typed(wrapper, QStringLiteral("LayeredImageWrapper"));
            auto candidate = document(xml.field(wrapper, QStringLiteral("image")));
            require(!ids.contains(candidate.id), QStringLiteral("CMO 来源文档 GUID 重复"));
            ids.insert(candidate.id);
            result.documents.append(std::move(candidate));
        }
        if (xml.unrelatedInvalidReferences) result.diagnostics.append({CPsdSourceDiagnostic::Severity::Warning,
            QStringLiteral("unrelated-invalid-references"),
            QStringLiteral("工程中有 %1 个非源素材引用无效；已独立验证全部导出的来源树与图像").arg(xml.unrelatedInvalidReferences), {}});
        if (documents.isEmpty()) result.diagnostics.append({CPsdSourceDiagnostic::Severity::Warning,
            QStringLiteral("no-source-documents"), QStringLiteral("此工程没有保留源素材文档"), {}});
    }
};
}

CPsdSourceCatalog CCmo3ArtworkReader::Extract(const CCaffArchive& archive, const detail::Xml& xml,
                                             const QString& path, const QByteArray& digest, CExportControl* control)
{
    CPsdSourceCatalog candidate;
    candidate.sourcePath = path;
    candidate.sourceDigest = digest;
    Reader reader(archive, xml, control);
    reader.read(candidate);
    if (!path.isEmpty()) candidate.protectedPaths.append(path);
    for (auto& document : candidate.documents) {
        document.origin.sourcePath = path;
        document.origin.sourceDigest = digest;
        document.origin.fileName = QFileInfo(path).fileName();
        document.origin.sourceVersion = candidate.sourceVersion;
    }
    checkpoint(control);
    return candidate;
}

bool CCmo3ArtworkReader::ReadBytes(const QByteArray& bytes, const QString& sourcePath,
                                  CPsdSourceCatalog& result, QString* error, CExportControl* control)
{
    CCmo3Project project;
    if (!CCmo3Project::OpenBytes(bytes, sourcePath, project, error, control)) return false;
    const auto* artwork = project.Artwork();
    if (!artwork) { if (error) *error = project.ArtworkError(); return false; }
    result = *artwork;
    return true;
}

bool CCmo3ArtworkReader::ReadFile(const QString& path, CPsdSourceCatalog& result,
                                 QString* error, CExportControl* control)
{
    CCmo3Project project;
    if (!CCmo3Project::OpenFile(path, project, error, control)) return false;
    const auto* artwork = project.Artwork();
    if (!artwork) { if (error) *error = project.ArtworkError(); return false; }
    result = *artwork;
    return true;
}
}
