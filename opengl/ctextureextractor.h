#ifndef CTEXTUREEXTRACTOR_H
#define CTEXTUREEXTRACTOR_H

#include <QImage>
#include <QByteArray>
#include <QRect>
#include <QString>
#include <QVector>
#include <QHash>
#include <QSet>
#include "std/cpsdexportcoverage.h"

class CExportControl;

namespace live2dstruct {
class CSectionTable;
class CCanvas_info;
}

struct ExtractedLayer {
    CPsdExportCoverage::Reason emptyReason = CPsdExportCoverage::Reason::None;
    QImage image;               // RGBA 像素（直通 alpha，非预乘）
    QRect canvasRect;           // 在画布上的像素位置
    QString name;               // art mesh ID (例如 "ArtMesh17")
    int meshIndex;              // 在 art_meshes 数组中的索引
    int textureNo;              // 纹理图集索引
    int drawableFlags;          // 混合模式 (bits 0-1: 0=Normal, 1=Additive, 2=Multiplicative)
    float opacity;              // 默认透明度 [0,1]
    bool isVisible;
    bool isEnabled;
    int parentPartIndex;        // 父 Part 索引 (-1=无)
    QVector<int> maskIndices;   // 裁剪蒙版引用的 art mesh 索引
};

struct ExtractedMaterial {
    QString name;
    int meshIndex = -1;
    int textureNo = -1;
    QImage image;
    QRect atlasRect;
    QSize atlasSize;
    bool visible = true;
    QVector<float> authorUvs;
};

class CTextureExtractor
{
public:
    static constexpr qint64 MaximumMaterialPixels = 128LL * 1024 * 1024;
    static constexpr qint64 MaximumTotalMaterialPixels = 256LL * 1024 * 1024;
    static constexpr qint64 MaximumMaterialReferences = 16LL * 1024 * 1024;
    static constexpr qint64 MaximumRasterSamples = 512LL * 1024 * 1024;
    CTextureExtractor();
    ~CTextureExtractor();

    /// 从文件路径列表加载纹理图集 PNG
    bool LoadAtlases(const QVector<QString>& atlasPaths, CExportControl* control = nullptr);
    bool LoadAtlasBytes(const QVector<QByteArray>& atlasPngs, CExportControl* control = nullptr);

    bool ExtractMaterials(live2dstruct::CSectionTable* table, QVector<ExtractedMaterial>& result,
                          QString* error = nullptr) const;

    // 提供模型坐标时采样到画布；否则返回 UV 空间裁片。
    QVector<ExtractedLayer> ExtractAll(live2dstruct::CSectionTable* table, bool includeHidden,
                                       const QHash<QString, QVector<quint16>>* indexOverrides = nullptr,
                                       CExportControl* control = nullptr,
                                       const QHash<QString, QVector<float>>* modelPositions = nullptr,
                                       const QSet<QString>* excludedIds = nullptr);

    /// 获取画布像素尺寸
    static QSize GetCanvasPixelSize(const live2dstruct::CCanvas_info* canvasInfo);

    /// Live2D drawable_flags 转 PSD blend mode key
    static uint32_t BlendModeToPsd(int drawableFlags);

private:
    QImage RasterizeCanvasMesh(const QImage& atlas, const float* uvs, const float* positions, int vertexCount,
                               const quint16* indices, int indexCount, const live2dstruct::CCanvas_info* canvas,
                               QRect& bounds, qint64& pixels, qint64& samples, CExportControl* control,
                               CPsdExportCoverage::Reason& emptyReason);
    /// 从 atlas 中通过 UV 三角形网格光栅化提取 mesh 图像
    /// @param atlas 纹理图集图像
    /// @param uvData UV 坐标数组 (vertex_count * 2 floats)
    /// @param vertexCount 顶点数
    /// @param indexData 三角形索引数组
    /// @param indexCount 索引总数 (三角形数 * 3)
    /// @param outUvRect [out] UV 空间包围盒（像素坐标）
    /// @return 裁剪后的 RGBA 图像
    QImage RasterizeMesh(const QImage& atlas,
                         const float* uvData, int vertexCount,
                         const quint16* indexData, int indexCount,
                         QRect& outUvRect, qint64& pixels, qint64& samples, CExportControl* control);

    /// 计算模型空间顶点在画布上的像素包围盒
    QRect ComputeCanvasRect(const float* positions, int vertexCount,
                            const live2dstruct::CCanvas_info* canvasInfo, CExportControl* control,
                            CPsdExportCoverage::Reason* emptyReason = nullptr);

    /// 光栅化单个三角形：从 atlas 采样写入输出图像
    /// 三角形顶点使用原始 atlas 像素坐标
    bool RasterizeTriangle(QImage& output,
                           const QImage& atlas,
                           double x0, double y0, double x1, double y1,
                           double x2, double y2, int atlasLeft, int atlasTop, CExportControl* control);

    QVector<QImage> m_atlases;
};

#endif // CTEXTUREEXTRACTOR_H
