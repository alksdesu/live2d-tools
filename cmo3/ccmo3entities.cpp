#include "ccmo3entities.h"
#include "ccaffarchive.h"
#include "ccmo3ids.h"
#include <QBuffer>

namespace cmo3 {

namespace {
const QString kNoDebug = QStringLiteral("(no debug info)");
} // namespace

CCmo3Entities::CCmo3Entities(CXsXmlWriter& xml, CCaffArchive& caff)
    : m_xml(xml), m_caff(caff)
{
}

QString CCmo3Entities::nextPngName()
{
    // 首个条目沿用官方 "imageFileBuf.png" 无编号命名
    if (m_pngCounter == 0) {
        ++m_pngCounter;
        return QStringLiteral("imageFileBuf.png");
    }
    return QString("imageFileBuf_%1.png").arg(m_pngCounter++ - 1);
}

QString CCmo3Entities::WriteBlendNormal()
{
    const QString id = m_xml.AllocId();
    m_xml.BeginObject(QStringLiteral("CBlend_Normal"), QString(), id);
    m_xml.BeginObject(QStringLiteral("ACBlend"), QStringLiteral("super"));
    m_xml.EndObject();
    m_xml.EndObject();
    return id;
}

QString CCmo3Entities::WriteCoordType(const QString& coordName)
{
    const QString id = m_xml.AllocId();
    m_xml.BeginObject(QStringLiteral("CoordType"), QString(), id);
    m_xml.S(QStringLiteral("coordName"), coordName);
    m_xml.EndObject();
    return id;
}

QString CCmo3Entities::WriteDeformerRootGuid()
{
    const QString id = m_xml.AllocId();
    m_xml.GuidObject(QStringLiteral("CDeformerGuid"), WellKnown::kDeformerRoot, id);
    return id;
}

SFilterGraphIds CCmo3Entities::WriteFilterGraph()
{
    // 双滤镜固定图：CLayerSelector(filter0) → CLayerFilter(filter1)。
    // 结构照抄官方样本 #192~#226 区域，xs.id 动态分配。
    SFilterGraphIds out;

    const QString instId0   = m_xml.AllocId();  // FilterInstanceId "filter0"
    const QString inst0     = m_xml.AllocId();  // FilterInstance CLayerSelector
    const QString fvidIlfOutputLayerData = m_xml.AllocId();
    const QString filterSet = m_xml.AllocId();  // ModelImageFilterSet
    const QString instId1   = m_xml.AllocId();  // FilterInstanceId "filter1"
    const QString conn0     = m_xml.AllocId();  // FilterOutputValueConnector
    const QString fvidMiInputLayerData = m_xml.AllocId();
    const QString fvidIlfInputLayerData = m_xml.AllocId();
    const QString fvidMiCurrentImageGuid = m_xml.AllocId();
    const QString fvidIlfCurrentImageGuid = m_xml.AllocId();
    const QString fvidMiOutputImage = m_xml.AllocId();
    const QString inst1     = m_xml.AllocId();  // FilterInstance CLayerFilter
    const QString fvidMiOutputTransform = m_xml.AllocId();
    const QString defGuid0  = m_xml.AllocId();  // StaticFilterDefGuid CLayerSelector
    const QString defGuid1  = m_xml.AllocId();  // StaticFilterDefGuid CLayerFilter
    const QString fvidIlfInputLayer = m_xml.AllocId();
    const QString fvOutputLayerData = m_xml.AllocId();  // FilterValue 选择レイヤー
    const QString fvMiInputLayerData = m_xml.AllocId();
    const QString fvIlfInputLayerData = m_xml.AllocId();
    const QString fvMiCurrentImageGuid = m_xml.AllocId();
    const QString fvIlfCurrentImageGuid = m_xml.AllocId();
    const QString fvMiOutputImage = m_xml.AllocId();
    const QString fvIlfOutputImageRes = m_xml.AllocId();
    const QString fvMiOutputTransform = m_xml.AllocId();
    const QString fvIlfOutputTransform = m_xml.AllocId();

    // FilterInstanceId filter0
    m_xml.BeginObject(QStringLiteral("FilterInstanceId"), QString(), instId0);
    m_xml.Attribute(QStringLiteral("idstr"), QStringLiteral("filter0"));
    m_xml.EndObject();

    // FilterInstance CLayerSelector
    m_xml.BeginObject(QStringLiteral("FilterInstance"), QString(), inst0);
    m_xml.Attribute(QStringLiteral("filterName"), QStringLiteral("CLayerSelector"));
    m_xml.Ref(QStringLiteral("StaticFilterDefGuid"), QStringLiteral("filterDefGuid"), defGuid0);
    m_xml.Null(QStringLiteral("filterDef"));
    m_xml.Ref(QStringLiteral("FilterInstanceId"), QStringLiteral("filterId"), instId0);
    m_xml.BeginObject(QStringLiteral("hash_map"), QStringLiteral("inputConnectors"));
    m_xml.Attribute(QStringLiteral("count"), QStringLiteral("2"));
    m_xml.BeginObject(QStringLiteral("entry"));
    m_xml.Ref(QStringLiteral("FilterValueId"), QStringLiteral("key"), fvidIlfInputLayerData);
    m_xml.BeginObject(QStringLiteral("EnvValueConnector"), QStringLiteral("value"));
    m_xml.BeginObject(QStringLiteral("AValueConnector"), QStringLiteral("super"));
    m_xml.EndObject();
    m_xml.Ref(QStringLiteral("FilterValueId"), QStringLiteral("envValueId"), fvidMiInputLayerData);
    m_xml.EndObject();
    m_xml.EndObject();
    m_xml.BeginObject(QStringLiteral("entry"));
    m_xml.Ref(QStringLiteral("FilterValueId"), QStringLiteral("key"), fvidIlfCurrentImageGuid);
    m_xml.BeginObject(QStringLiteral("EnvValueConnector"), QStringLiteral("value"));
    m_xml.BeginObject(QStringLiteral("AValueConnector"), QStringLiteral("super"));
    m_xml.EndObject();
    m_xml.Ref(QStringLiteral("FilterValueId"), QStringLiteral("envValueId"), fvidMiCurrentImageGuid);
    m_xml.EndObject();
    m_xml.EndObject();
    m_xml.EndObject(); // hash_map inputConnectors
    m_xml.BeginObject(QStringLiteral("hash_map"), QStringLiteral("outputConnectors"));
    m_xml.Attribute(QStringLiteral("count"), QStringLiteral("1"));
    m_xml.BeginObject(QStringLiteral("entry"));
    m_xml.Ref(QStringLiteral("FilterValueId"), QStringLiteral("key"), fvidIlfOutputLayerData);
    m_xml.Ref(QStringLiteral("FilterOutputValueConnector"), QStringLiteral("value"), conn0);
    m_xml.EndObject();
    m_xml.EndObject();
    m_xml.Ref(QStringLiteral("ModelImageFilterSet"), QStringLiteral("ownerFilterSet"), filterSet);
    m_xml.EndObject(); // FilterInstance

    // FilterValueId ilf_outputLayerData
    m_xml.BeginObject(QStringLiteral("FilterValueId"), QString(), fvidIlfOutputLayerData);
    m_xml.Attribute(QStringLiteral("idstr"), QStringLiteral("ilf_outputLayerData"));
    m_xml.EndObject();

    // ModelImageFilterSet
    m_xml.BeginObject(QStringLiteral("ModelImageFilterSet"), QString(), filterSet);
    m_xml.BeginObject(QStringLiteral("FilterSet"), QStringLiteral("super"));
    m_xml.BeginObject(QStringLiteral("linked_map"), QStringLiteral("filterMap"));
    m_xml.Attribute(QStringLiteral("count"), QStringLiteral("2"));
    m_xml.BeginObject(QStringLiteral("entry"));
    m_xml.Ref(QStringLiteral("FilterInstanceId"), QStringLiteral("key"), instId0);
    m_xml.Ref(QStringLiteral("FilterInstance"), QStringLiteral("value"), inst0);
    m_xml.EndObject();
    m_xml.BeginObject(QStringLiteral("entry"));
    m_xml.Ref(QStringLiteral("FilterInstanceId"), QStringLiteral("key"), instId1);
    m_xml.Ref(QStringLiteral("FilterInstance"), QStringLiteral("value"), inst1);
    m_xml.EndObject();
    m_xml.EndObject(); // filterMap

    m_xml.BeginObject(QStringLiteral("linked_map"), QStringLiteral("_externalInputs"));
    m_xml.Attribute(QStringLiteral("count"), QStringLiteral("2"));
    m_xml.BeginObject(QStringLiteral("entry"));
    m_xml.Ref(QStringLiteral("FilterValueId"), QStringLiteral("key"), fvidMiInputLayerData);
    m_xml.BeginObject(QStringLiteral("EnvConnection"), QStringLiteral("value"));
    m_xml.Ref(QStringLiteral("FilterValue"), QStringLiteral("_envValueDef"), fvMiInputLayerData);
    m_xml.Ref(QStringLiteral("FilterInstance"), QStringLiteral("filter"), inst0);
    m_xml.Ref(QStringLiteral("FilterValue"), QStringLiteral("filterValueDef"), fvIlfInputLayerData);
    m_xml.EndObject();
    m_xml.EndObject();
    m_xml.BeginObject(QStringLiteral("entry"));
    m_xml.Ref(QStringLiteral("FilterValueId"), QStringLiteral("key"), fvidMiCurrentImageGuid);
    m_xml.BeginObject(QStringLiteral("EnvConnection"), QStringLiteral("value"));
    m_xml.Ref(QStringLiteral("FilterValue"), QStringLiteral("_envValueDef"), fvMiCurrentImageGuid);
    m_xml.Ref(QStringLiteral("FilterInstance"), QStringLiteral("filter"), inst0);
    m_xml.Ref(QStringLiteral("FilterValue"), QStringLiteral("filterValueDef"), fvIlfCurrentImageGuid);
    m_xml.EndObject();
    m_xml.EndObject();
    m_xml.EndObject(); // _externalInputs

    m_xml.BeginObject(QStringLiteral("linked_map"), QStringLiteral("_externalOutputs"));
    m_xml.Attribute(QStringLiteral("count"), QStringLiteral("2"));
    m_xml.BeginObject(QStringLiteral("entry"));
    m_xml.Ref(QStringLiteral("FilterValueId"), QStringLiteral("key"), fvidMiOutputImage);
    m_xml.BeginObject(QStringLiteral("EnvConnection"), QStringLiteral("value"));
    m_xml.Ref(QStringLiteral("FilterValue"), QStringLiteral("_envValueDef"), fvMiOutputImage);
    m_xml.Ref(QStringLiteral("FilterInstance"), QStringLiteral("filter"), inst1);
    m_xml.Ref(QStringLiteral("FilterValue"), QStringLiteral("filterValueDef"), fvIlfOutputImageRes);
    m_xml.EndObject();
    m_xml.EndObject();
    m_xml.BeginObject(QStringLiteral("entry"));
    m_xml.Ref(QStringLiteral("FilterValueId"), QStringLiteral("key"), fvidMiOutputTransform);
    m_xml.BeginObject(QStringLiteral("EnvConnection"), QStringLiteral("value"));
    m_xml.Ref(QStringLiteral("FilterValue"), QStringLiteral("_envValueDef"), fvMiOutputTransform);
    m_xml.Ref(QStringLiteral("FilterInstance"), QStringLiteral("filter"), inst1);
    m_xml.Ref(QStringLiteral("FilterValue"), QStringLiteral("filterValueDef"), fvIlfOutputTransform);
    m_xml.EndObject();
    m_xml.EndObject();
    m_xml.EndObject(); // _externalOutputs
    m_xml.EndObject(); // FilterSet
    m_xml.EndObject(); // ModelImageFilterSet

    // FilterInstanceId filter1
    m_xml.BeginObject(QStringLiteral("FilterInstanceId"), QString(), instId1);
    m_xml.Attribute(QStringLiteral("idstr"), QStringLiteral("filter1"));
    m_xml.EndObject();

    // FilterOutputValueConnector（filter0 输出 → filter1 输入）
    m_xml.BeginObject(QStringLiteral("FilterOutputValueConnector"), QString(), conn0);
    m_xml.BeginObject(QStringLiteral("AValueConnector"), QStringLiteral("super"));
    m_xml.EndObject();
    m_xml.Ref(QStringLiteral("FilterInstance"), QStringLiteral("instance"), inst0);
    m_xml.Ref(QStringLiteral("FilterValueId"), QStringLiteral("id"), fvidIlfOutputLayerData);
    m_xml.Ref(QStringLiteral("FilterValue"), QStringLiteral("valueDef"), fvOutputLayerData);
    m_xml.EndObject();

    // FilterValueId 常量族
    struct { const QString& id; const char* idstr; } fvids[] = {
        { fvidMiInputLayerData,   "mi_input_layerInputData" },
        { fvidIlfInputLayerData,  "ilf_inputLayerData" },
        { fvidMiCurrentImageGuid, "mi_currentImageGuid" },
        { fvidIlfCurrentImageGuid,"ilf_currentImageGuid" },
        { fvidMiOutputImage,      "mi_output_image" },
    };
    for (const auto& fv : fvids) {
        m_xml.BeginObject(QStringLiteral("FilterValueId"), QString(), fv.id);
        m_xml.Attribute(QStringLiteral("idstr"), QLatin1String(fv.idstr));
        m_xml.EndObject();
    }

    // FilterInstance CLayerFilter
    m_xml.BeginObject(QStringLiteral("FilterInstance"), QString(), inst1);
    m_xml.Attribute(QStringLiteral("filterName"), QStringLiteral("CLayerFilter"));
    m_xml.Ref(QStringLiteral("StaticFilterDefGuid"), QStringLiteral("filterDefGuid"), defGuid1);
    m_xml.Null(QStringLiteral("filterDef"));
    m_xml.Ref(QStringLiteral("FilterInstanceId"), QStringLiteral("filterId"), instId1);
    m_xml.BeginObject(QStringLiteral("hash_map"), QStringLiteral("inputConnectors"));
    m_xml.Attribute(QStringLiteral("count"), QStringLiteral("1"));
    m_xml.BeginObject(QStringLiteral("entry"));
    m_xml.Ref(QStringLiteral("FilterValueId"), QStringLiteral("key"), fvidIlfInputLayer);
    m_xml.Ref(QStringLiteral("FilterOutputValueConnector"), QStringLiteral("value"), conn0);
    m_xml.EndObject();
    m_xml.EndObject();
    m_xml.BeginObject(QStringLiteral("hash_map"), QStringLiteral("outputConnectors"));
    m_xml.Attribute(QStringLiteral("count"), QStringLiteral("0"));
    m_xml.Attribute(QStringLiteral("keyType"), QStringLiteral("string"));
    m_xml.EndObject();
    m_xml.Ref(QStringLiteral("ModelImageFilterSet"), QStringLiteral("ownerFilterSet"), filterSet);
    m_xml.EndObject();

    // FilterValueId mi_output_transform
    m_xml.BeginObject(QStringLiteral("FilterValueId"), QString(), fvidMiOutputTransform);
    m_xml.Attribute(QStringLiteral("idstr"), QStringLiteral("mi_output_transform"));
    m_xml.EndObject();

    // StaticFilterDefGuid 常量对
    m_xml.GuidObject(QStringLiteral("StaticFilterDefGuid"),
                     QStringLiteral("5e9fe1ea-0ec3-4d68-a5fa-018fc7abe301"), defGuid0);
    m_xml.GuidObject(QStringLiteral("StaticFilterDefGuid"),
                     QStringLiteral("4083cd1f-40ba-4eda-8400-379019d55ed8"), defGuid1);

    // FilterValueId ilf_inputLayer
    m_xml.BeginObject(QStringLiteral("FilterValueId"), QString(), fvidIlfInputLayer);
    m_xml.Attribute(QStringLiteral("idstr"), QStringLiteral("ilf_inputLayer"));
    m_xml.EndObject();

    // FilterValue 定义族（name 可为任意描述文本；id 引用或内联 idstr）
    struct FvDef { const QString& id; const char* name; const QString* refId; const char* inlineIdstr; };
    const FvDef fvDefs[] = {
        { fvOutputLayerData,     "output layer",           &fvidIlfOutputLayerData,  nullptr },
        { fvMiInputLayerData,    "input layer data",       &fvidMiInputLayerData,    nullptr },
        { fvIlfInputLayerData,   "input layer data (ilf)", &fvidIlfInputLayerData,   nullptr },
        { fvMiCurrentImageGuid,  "image guid",             &fvidMiCurrentImageGuid,  nullptr },
        { fvIlfCurrentImageGuid, "image guid (ilf)",       &fvidIlfCurrentImageGuid, nullptr },
        { fvMiOutputImage,       "output image",           &fvidMiOutputImage,       nullptr },
        { fvIlfOutputImageRes,   "output image resource",  nullptr, "ilf_outputImageRes" },
        { fvMiOutputTransform,   "layer to canvas",        &fvidMiOutputTransform,   nullptr },
        { fvIlfOutputTransform,  "layer to canvas (ilf)",  nullptr, "ilf_outputTransform" },
    };
    for (const FvDef& d : fvDefs) {
        m_xml.BeginObject(QStringLiteral("FilterValue"), QString(), d.id);
        m_xml.S(QStringLiteral("name"), QLatin1String(d.name));
        if (d.refId) {
            m_xml.Ref(QStringLiteral("FilterValueId"), QStringLiteral("id"), *d.refId);
        } else {
            m_xml.BeginObject(QStringLiteral("FilterValueId"), QStringLiteral("id"));
            m_xml.Attribute(QStringLiteral("idstr"), QLatin1String(d.inlineIdstr));
            m_xml.EndObject();
        }
        m_xml.Null(QStringLiteral("defaultValueInitializer"));
        m_xml.EndObject();
    }

    out.filterSet = filterSet;
    out.fvidMiInputLayerData = fvidMiInputLayerData;
    out.fvidMiCurrentImageGuid = fvidMiCurrentImageGuid;
    return out;
}

QString CCmo3Entities::WriteImageResource(const QImage& image, QString* outPngPath)
{
    QByteArray png;
    {
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
    }
    const QString pngPath = nextPngName();
    m_caff.AddEntry(pngPath, png);
    if (outPngPath) *outPngPath = pngPath;

    const QString id = m_xml.AllocId();
    m_xml.BeginObject(QStringLiteral("CImageResource"), QString(), id);
    m_xml.Attribute(QStringLiteral("width"), QString::number(image.width()));
    m_xml.Attribute(QStringLiteral("height"), QString::number(image.height()));
    m_xml.Attribute(QStringLiteral("type"), QStringLiteral("INT_ARGB"));
    m_xml.Attribute(QStringLiteral("imageFileBuf_size"), QString::number(png.size()));
    m_xml.Attribute(QStringLiteral("previewFileBuf_size"), QStringLiteral("0"));
    m_xml.File(QStringLiteral("imageFileBuf"), pngPath);
    m_xml.EndObject();
    return id;
}

SLayeredImageIds CCmo3Entities::WriteLayeredImage(const QString& docName, int canvasW, int canvasH,
                                                  QVector<SLayerData>& layers)
{
    SLayeredImageIds out;
    out.blendNormal = WriteBlendNormal();
    out.layeredImage = m_xml.AllocId();
    out.layeredImageGuid = m_xml.AllocId();
    out.rootGroup = m_xml.AllocId();

    // 先写每层的 CImageResource（PNG 入 CAFF）
    for (SLayerData& layer : layers) {
        layer.imageResId = WriteImageResource(layer.image, &layer.pngPath);
        layer.layerId = m_xml.AllocId();
    }

    // CLayeredImageGuid
    m_xml.GuidObject(QStringLiteral("CLayeredImageGuid"), NewGuid(), out.layeredImageGuid);

    // root CLayerGroup
    m_xml.BeginObject(QStringLiteral("CLayerGroup"), QString(), out.rootGroup);
    m_xml.BeginObject(QStringLiteral("ACLayerGroup"), QStringLiteral("super"));
    m_xml.BeginObject(QStringLiteral("ACLayerEntry"), QStringLiteral("super"));
    m_xml.S(QStringLiteral("name"), QStringLiteral("root"));
    m_xml.S(QStringLiteral("memo"), QString());
    m_xml.B(QStringLiteral("isVisible"), true);
    m_xml.B(QStringLiteral("isClipping"), false);
    m_xml.Ref(QStringLiteral("CBlend_Normal"), QStringLiteral("blend"), out.blendNormal);
    m_xml.BeginObject(QStringLiteral("CLayerGuid"), QStringLiteral("guid"));
    m_xml.Attribute(QStringLiteral("uuid"), NewGuid());
    m_xml.Attribute(QStringLiteral("note"), kNoDebug);
    m_xml.EndObject();
    m_xml.Null(QStringLiteral("group"));
    m_xml.I(QStringLiteral("opacity255"), 255);
    m_xml.BeginObject(QStringLiteral("hash_map"), QStringLiteral("_optionOfIOption"));
    m_xml.Attribute(QStringLiteral("count"), QStringLiteral("0"));
    m_xml.Attribute(QStringLiteral("keyType"), QStringLiteral("string"));
    m_xml.EndObject();
    m_xml.Ref(QStringLiteral("CLayeredImage"), QStringLiteral("_layeredImage"), out.layeredImage);
    m_xml.EndObject(); // ACLayerEntry
    m_xml.BeginCArrayList(QStringLiteral("_children"), layers.size());
    for (const SLayerData& layer : layers) {
        m_xml.Ref(QStringLiteral("CLayer"), QString(), layer.layerId);
    }
    m_xml.EndList();
    m_xml.EndObject(); // ACLayerGroup
    m_xml.Null(QStringLiteral("layerIdentifier"));
    m_xml.EndObject(); // CLayerGroup

    // 各 CLayer
    int layerSeq = 0;
    for (const SLayerData& layer : layers) {
        m_xml.BeginObject(QStringLiteral("CLayer"), QString(), layer.layerId);
        m_xml.BeginObject(QStringLiteral("ACImageLayer"), QStringLiteral("super"));
        m_xml.BeginObject(QStringLiteral("ACLayerEntry"), QStringLiteral("super"));
        m_xml.S(QStringLiteral("name"), layer.name);
        m_xml.S(QStringLiteral("memo"), QString());
        m_xml.B(QStringLiteral("isVisible"), layer.visible);
        m_xml.B(QStringLiteral("isClipping"), false);
        m_xml.Ref(QStringLiteral("CBlend_Normal"), QStringLiteral("blend"), out.blendNormal);
        m_xml.BeginObject(QStringLiteral("CLayerGuid"), QStringLiteral("guid"));
        m_xml.Attribute(QStringLiteral("uuid"), NewGuid());
        m_xml.Attribute(QStringLiteral("note"), kNoDebug);
        m_xml.EndObject();
        m_xml.Ref(QStringLiteral("CLayerGroup"), QStringLiteral("group"), out.rootGroup);
        m_xml.I(QStringLiteral("opacity255"), 255);
        m_xml.BeginObject(QStringLiteral("hash_map"), QStringLiteral("_optionOfIOption"));
        m_xml.Attribute(QStringLiteral("count"), QStringLiteral("0"));
        m_xml.Attribute(QStringLiteral("keyType"), QStringLiteral("string"));
        m_xml.EndObject();
        m_xml.Ref(QStringLiteral("CLayeredImage"), QStringLiteral("_layeredImage"), out.layeredImage);
        m_xml.EndObject(); // ACLayerEntry
        m_xml.EndObject(); // ACImageLayer
        m_xml.Ref(QStringLiteral("CImageResource"), QStringLiteral("imageResource"), layer.imageResId);
        m_xml.BeginObject(QStringLiteral("CRect"), QStringLiteral("boundsOnImageDoc"));
        m_xml.I(QStringLiteral("x"), layer.bounds.x());
        m_xml.I(QStringLiteral("y"), layer.bounds.y());
        m_xml.I(QStringLiteral("width"), layer.bounds.width());
        m_xml.I(QStringLiteral("height"), layer.bounds.height());
        m_xml.EndObject();
        m_xml.BeginObject(QStringLiteral("CLayerIdentifier"), QStringLiteral("layerIdentifier"));
        m_xml.S(QStringLiteral("layerName"), layer.name);
        m_xml.S(QStringLiteral("layerId"),
                QString("00-00-%1-%2")
                    .arg((layerSeq >> 8) & 0xFF, 2, 16, QLatin1Char('0'))
                    .arg(layerSeq & 0xFF, 2, 16, QLatin1Char('0')).toUpper());
        m_xml.I(QStringLiteral("layerIdValue_testImpl"), layerSeq);
        m_xml.EndObject();
        m_xml.BeginObject(QStringLiteral("linked_map"), QStringLiteral("layerInfo"));
        m_xml.Attribute(QStringLiteral("count"), QStringLiteral("0"));
        m_xml.Attribute(QStringLiteral("keyType"), QStringLiteral("string"));
        m_xml.EndObject();
        m_xml.BeginObject(QStringLiteral("hash_map"), QStringLiteral("_optionOfIOption"));
        m_xml.Attribute(QStringLiteral("count"), QStringLiteral("0"));
        m_xml.Attribute(QStringLiteral("keyType"), QStringLiteral("string"));
        m_xml.EndObject();
        m_xml.EndObject(); // CLayer
        ++layerSeq;
    }

    // CLayeredImage 本体
    m_xml.BeginObject(QStringLiteral("CLayeredImage"), QString(), out.layeredImage);
    m_xml.S(QStringLiteral("name"), docName);
    m_xml.S(QStringLiteral("memo"), QString());
    m_xml.I(QStringLiteral("width"), canvasW);
    m_xml.I(QStringLiteral("height"), canvasH);
    m_xml.File(QStringLiteral("psdFile"), docName);
    m_xml.S(QStringLiteral("description"), QString());
    m_xml.Ref(QStringLiteral("CLayeredImageGuid"), QStringLiteral("guid"), out.layeredImageGuid);
    m_xml.Null(QStringLiteral("psdBytes"));
    m_xml.L(QStringLiteral("psdFileLastModified"), 0);
    m_xml.Ref(QStringLiteral("CLayerGroup"), QStringLiteral("_rootLayer"), out.rootGroup);
    m_xml.BeginObject(QStringLiteral("LayerSet"), QStringLiteral("layerSet"));
    m_xml.Ref(QStringLiteral("CLayeredImage"), QStringLiteral("_layeredImage"), out.layeredImage);
    m_xml.BeginCArrayList(QStringLiteral("_layerEntryList"), layers.size() + 1);
    m_xml.Ref(QStringLiteral("CLayerGroup"), QString(), out.rootGroup);
    for (const SLayerData& layer : layers) {
        m_xml.Ref(QStringLiteral("CLayer"), QString(), layer.layerId);
    }
    m_xml.EndList();
    m_xml.EndObject(); // LayerSet
    m_xml.EndObject(); // CLayeredImage

    return out;
}

QString CCmo3Entities::WriteModelImageGroup(const SFilterGraphIds& fg, const SLayeredImageIds& li,
                                            QVector<SLayerData>& layers)
{
    // 每层先分配 CModelImageGuid
    for (SLayerData& layer : layers) {
        layer.modelImageGuidId = m_xml.AllocId();
        m_xml.GuidObject(QStringLiteral("CModelImageGuid"), NewGuid(), layer.modelImageGuidId);
    }

    const QString groupId = m_xml.AllocId();
    m_xml.BeginObject(QStringLiteral("CModelImageGroup"), QString(), groupId);
    m_xml.S(QStringLiteral("memo"), QString());
    m_xml.S(QStringLiteral("groupName"), QStringLiteral("Recovered"));
    m_xml.BeginCArrayList(QStringLiteral("_linkedRawImageGuids"), 1);
    m_xml.Ref(QStringLiteral("CLayeredImageGuid"), QString(), li.layeredImageGuid);
    m_xml.EndList();
    m_xml.BeginCArrayList(QStringLiteral("_modelImages"), layers.size());
    for (const SLayerData& layer : layers) {
        m_xml.BeginObject(QStringLiteral("CModelImage"));
        m_xml.Attribute(QStringLiteral("modelImageVersion"), QStringLiteral("1"));
        m_xml.Ref(QStringLiteral("CModelImageGuid"), QStringLiteral("guid"), layer.modelImageGuidId);
        m_xml.S(QStringLiteral("name"), layer.name);
        m_xml.Ref(QStringLiteral("ModelImageFilterSet"), QStringLiteral("inputFilter"), fg.filterSet);

        m_xml.BeginObject(QStringLiteral("ModelImageFilterEnv"), QStringLiteral("inputFilterEnv"));
        m_xml.BeginObject(QStringLiteral("FilterEnv"), QStringLiteral("super"));
        m_xml.Null(QStringLiteral("parentEnv"));
        m_xml.BeginObject(QStringLiteral("hash_map"), QStringLiteral("envValues"));
        m_xml.Attribute(QStringLiteral("count"), QStringLiteral("2"));
        // entry 1: 当前 PSD 文档 GUID
        m_xml.BeginObject(QStringLiteral("entry"));
        m_xml.Ref(QStringLiteral("FilterValueId"), QStringLiteral("key"), fg.fvidMiCurrentImageGuid);
        m_xml.BeginObject(QStringLiteral("EnvValueSet"), QStringLiteral("value"));
        m_xml.Ref(QStringLiteral("FilterValueId"), QStringLiteral("id"), fg.fvidMiCurrentImageGuid);
        m_xml.Ref(QStringLiteral("CLayeredImageGuid"), QStringLiteral("value"), li.layeredImageGuid);
        m_xml.L(QStringLiteral("updateTimeMs"), 0);
        m_xml.EndObject();
        m_xml.EndObject();
        // entry 2: 层选择映射
        m_xml.BeginObject(QStringLiteral("entry"));
        m_xml.Ref(QStringLiteral("FilterValueId"), QStringLiteral("key"), fg.fvidMiInputLayerData);
        m_xml.BeginObject(QStringLiteral("EnvValueSet"), QStringLiteral("value"));
        m_xml.Ref(QStringLiteral("FilterValueId"), QStringLiteral("id"), fg.fvidMiInputLayerData);
        m_xml.BeginObject(QStringLiteral("CLayerSelectorMap"), QStringLiteral("value"));
        m_xml.BeginObject(QStringLiteral("linked_map"), QStringLiteral("_imageToLayerInput"));
        m_xml.Attribute(QStringLiteral("count"), QStringLiteral("1"));
        m_xml.BeginObject(QStringLiteral("entry"));
        m_xml.Ref(QStringLiteral("CLayeredImageGuid"), QStringLiteral("key"), li.layeredImageGuid);
        m_xml.BeginArrayList(QStringLiteral("value"), 1);
        m_xml.BeginObject(QStringLiteral("CLayerInputData"));
        m_xml.Ref(QStringLiteral("CLayer"), QStringLiteral("layer"), layer.layerId);
        m_xml.BeginObject(QStringLiteral("CAffine"), QStringLiteral("affine"));
        m_xml.Attribute(QStringLiteral("m00"), QStringLiteral("1.0"));
        m_xml.Attribute(QStringLiteral("m01"), QStringLiteral("0.0"));
        m_xml.Attribute(QStringLiteral("m02"), QStringLiteral("0.0"));
        m_xml.Attribute(QStringLiteral("m10"), QStringLiteral("0.0"));
        m_xml.Attribute(QStringLiteral("m11"), QStringLiteral("1.0"));
        m_xml.Attribute(QStringLiteral("m12"), QStringLiteral("0.0"));
        m_xml.EndObject();
        m_xml.EndObject(); // CLayerInputData
        m_xml.EndList();
        m_xml.EndObject(); // entry
        m_xml.EndObject(); // linked_map
        m_xml.EndObject(); // CLayerSelectorMap
        m_xml.EndObject(); // EnvValueSet
        m_xml.EndObject(); // entry
        m_xml.EndObject(); // hash_map
        m_xml.EndObject(); // FilterEnv
        m_xml.EndObject(); // ModelImageFilterEnv

        m_xml.Ref(QStringLiteral("CImageResource"), QStringLiteral("_filteredImage"), layer.imageResId);
        m_xml.BeginObject(QStringLiteral("CAffine"), QStringLiteral("_materialLocalToCanvasTransform"));
        m_xml.Attribute(QStringLiteral("m00"), QStringLiteral("1.0"));
        m_xml.Attribute(QStringLiteral("m01"), QStringLiteral("0.0"));
        m_xml.Attribute(QStringLiteral("m02"), CXsXmlWriter::FormatFloat(static_cast<float>(layer.bounds.x())));
        m_xml.Attribute(QStringLiteral("m10"), QStringLiteral("0.0"));
        m_xml.Attribute(QStringLiteral("m11"), QStringLiteral("1.0"));
        m_xml.Attribute(QStringLiteral("m12"), CXsXmlWriter::FormatFloat(static_cast<float>(layer.bounds.y())));
        m_xml.EndObject();
        m_xml.Ref(QStringLiteral("CModelImageGroup"), QStringLiteral("_group"), groupId);
        m_xml.BeginObject(QStringLiteral("CCachedImageManager"), QStringLiteral("cachedImageManager"));
        m_xml.Ref(QStringLiteral("CImageResource"), QStringLiteral("rawImage"), layer.imageResId);
        m_xml.EndObject();
        m_xml.EndObject(); // CModelImage
    }
    m_xml.EndList();
    m_xml.EndObject(); // CModelImageGroup
    return groupId;
}

void CCmo3Entities::WriteTextureManagerMain(const SLayeredImageIds& li, const QString& modelImageGroupId)
{
    m_xml.BeginObject(QStringLiteral("CTextureManager"), QStringLiteral("textureManager"));
    m_xml.BeginObject(QStringLiteral("TextureImageGroup"), QStringLiteral("textureList"));
    m_xml.BeginCArrayList(QStringLiteral("children"), 0);
    m_xml.EndList();
    m_xml.EndObject();
    m_xml.BeginCArrayList(QStringLiteral("_rawImages"), 1);
    m_xml.BeginObject(QStringLiteral("LayeredImageWrapper"));
    m_xml.Ref(QStringLiteral("CLayeredImage"), QStringLiteral("image"), li.layeredImage);
    m_xml.L(QStringLiteral("importedTimeMSec"), 0);
    m_xml.L(QStringLiteral("lastModifiedTimeMSec"), 0);
    m_xml.B(QStringLiteral("isReplaced"), false);
    m_xml.EndObject();
    m_xml.EndList();
    m_xml.BeginCArrayList(QStringLiteral("_modelImageGroups"), 1);
    m_xml.Ref(QStringLiteral("CModelImageGroup"), QString(), modelImageGroupId);
    m_xml.EndList();
    m_xml.BeginCArrayList(QStringLiteral("_textureAtlases"), 0);
    m_xml.EndList();
    m_xml.B(QStringLiteral("isTextureInputModelImageMode"), true);
    m_xml.EndObject();
}

void CCmo3Entities::WriteAffecterSourceSetMain()
{
    m_xml.BeginObject(QStringLiteral("CAffecterSourceSet"), QStringLiteral("affecterSourceSet"));
    m_xml.BeginCArrayList(QStringLiteral("_sources"), 0);
    m_xml.EndList();
    m_xml.EndObject();
}

} // namespace cmo3
