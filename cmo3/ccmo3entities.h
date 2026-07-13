#ifndef CCMO3ENTITIES_H
#define CCMO3ENTITIES_H

#include "cxsxmlwriter.h"
#include <QImage>
#include <QRect>
#include <QVector>

namespace cmo3 {

class CCaffArchive;

// 共享 filter graph（CLayerSelector + CLayerFilter）的 xs.id 集合，
// 全模型只写一份，所有 CModelImage 复用
struct SFilterGraphIds {
    QString filterSet;              // ModelImageFilterSet
    QString fvidMiInputLayerData;   // mi_input_layerInputData
    QString fvidMiCurrentImageGuid; // mi_currentImageGuid
};

// 单个图层（对应一个 ArtMesh 的光栅化贴图）
struct SLayerData {
    QString name;                   // 显示名（ArtMesh 名）
    QImage  image;                  // 裁剪后的贴图
    QRect   bounds;                 // 画布像素坐标上的位置
    bool    visible = true;
    // 写出阶段填充：
    QString layerId;                // CLayer xs.id
    QString imageResId;             // CImageResource xs.id
    QString modelImageGuidId;       // CModelImageGuid xs.id
    QString pngPath;                // CAFF 内 PNG 路径
};

// 图层容器（单 CLayeredImage 模式）
struct SLayeredImageIds {
    QString layeredImage;           // CLayeredImage xs.id
    QString layeredImageGuid;       // CLayeredImageGuid xs.id
    QString rootGroup;              // CLayerGroup xs.id
    QString blendNormal;            // CBlend_Normal xs.id
};

// 实体写出器：持有 XML writer + CAFF 容器，逐实体写 shared 池
// 固定骨架（filter graph、图层树、纹理链）与逐 mesh 结构在此实现，
// 数据装配由 CCmo3Builder 编排
class CCmo3Entities
{
public:
    CCmo3Entities(CXsXmlWriter& xml, CCaffArchive& caff);

    // ---- 固定骨架（全模型一份） ----
    QString WriteBlendNormal();
    QString WriteCoordType(const QString& coordName);   // "Basic Coord" / "DeformerLocal"
    SFilterGraphIds WriteFilterGraph();
    QString WriteDeformerRootGuid();                    // well-known ROOT

    // ---- 图层与纹理链 ----
    // 写出 CImageResource 并把 PNG 存入 CAFF；返回 xs.id
    QString WriteImageResource(const QImage& image, QString* outPngPath);
    // 写出 CLayeredImage + root CLayerGroup + 全部 CLayer + LayerSet（就地修改 layers 填充 id）
    SLayeredImageIds WriteLayeredImage(const QString& docName, int canvasW, int canvasH,
                                       QVector<SLayerData>& layers);
    // 写出 CModelImageGroup（内联全部 CModelImage，填充各层 modelImageGuidId）；返回 group 的 xs.id
    QString WriteModelImageGroup(const SFilterGraphIds& fg, const SLayeredImageIds& li,
                                 QVector<SLayerData>& layers);

    // ---- main 段辅助 ----
    void WriteTextureManagerMain(const SLayeredImageIds& li, const QString& modelImageGroupId);
    void WriteAffecterSourceSetMain();

    CXsXmlWriter& Xml() { return m_xml; }

private:
    QString nextPngName();

    CXsXmlWriter& m_xml;
    CCaffArchive& m_caff;
    int m_pngCounter = 0;
};

} // namespace cmo3

#endif // CCMO3ENTITIES_H
