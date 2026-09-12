#include "ctextureextractor.h"
#include "live2dstruct/csectiontable.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <QDebug>

using namespace live2dstruct;

CTextureExtractor::CTextureExtractor() {}
CTextureExtractor::~CTextureExtractor() {}

bool CTextureExtractor::LoadAtlases(const QVector<QString>& atlasPaths)
{
    m_atlases.clear();
    m_atlases.reserve(atlasPaths.size());
    for (const QString& path : atlasPaths) {
        QImage img(path);
        if (img.isNull()) {
            qWarning("CTextureExtractor: 无法加载纹理图集: %s", path.toUtf8().constData());
            return false;
        }
        m_atlases.append(img.convertToFormat(QImage::Format_ARGB32));
    }
    return !m_atlases.isEmpty();
}

QSize CTextureExtractor::GetCanvasPixelSize(const CCanvas_info* canvasInfo)
{
    if (!canvasInfo) return {};
    const double w = canvasInfo->width;
    const double h = canvasInfo->height;
    if (!std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0 ||
        w > std::numeric_limits<int>::max() || h > std::numeric_limits<int>::max()) {
        return {};
    }
    return QSize(qRound(w), qRound(h));
}

uint32_t CTextureExtractor::BlendModeToPsd(int drawableFlags)
{
    switch (drawableFlags & 0x03) {
    case 1:  return 0x6C646467; // 'lddg' Linear Dodge (Additive)
    case 2:  return 0x6D756C20; // 'mul ' Multiply
    default: return 0x6E6F726D; // 'norm' Normal
    }
}

QVector<ExtractedLayer> CTextureExtractor::ExtractAll(CSectionTable* table, bool includeHidden)
{
    QVector<ExtractedLayer> results;
    if (!table || !table->IsValid() || !table->count_info || !table->uvs ||
        !table->position_indices || !table->keyform_positions || !table->canvas_info) return results;
    int meshCount = table->count_info->art_meshes;
    meshCount = std::min(meshCount, static_cast<int>(table->art_meshes.size()));
    results.reserve(meshCount);

    for (int i = 0; i < meshCount; ++i) {
        CArt_meshes* mesh = table->art_meshes[i];

        // 跳过隐藏图层（如果不包括的话）
        if (!includeHidden && !mesh->is_visible) {
            continue;
        }

        // 跳过无顶点的 mesh
        if (mesh->vertex_counts <= 0) {
            continue;
        }

        // 检查纹理索引
        int texNo = mesh->texture_nos;
        if (texNo < 0 || texNo >= m_atlases.size()) {
            qWarning("CTextureExtractor: mesh %d (%s) 纹理索引 %d 越界",
                     i, mesh->ids.toUtf8().constData(), texNo);
            continue;
        }

        const QImage& atlas = m_atlases[texNo];
        int atlasW = atlas.width();
        int atlasH = atlas.height();

        // 提取 UV 数据（归一化 [0,1]）
        int uvBegin = mesh->uv_sources_begin_indices;
        int uvCount = mesh->vertex_counts * 2;

        // 提取三角形索引
        int idxBegin = mesh->position_index_sources_begin_indices;
        int idxCount = mesh->position_index_sources_counts;
        const int uvSize = static_cast<int>(table->uvs->uvs.size());
        const int indexSize = static_cast<int>(table->position_indices->indices.size());
        if (uvBegin < 0 || uvCount < 0 || uvBegin > uvSize || uvCount > uvSize - uvBegin ||
            idxBegin < 0 || idxCount < 0 || idxBegin > indexSize || idxCount > indexSize - idxBegin) {
            continue;
        }

        // 准备 UV 数据指针
        const float* uvPtr = &table->uvs->uvs[uvBegin];
        const short* idxPtr = &table->position_indices->indices[idxBegin];

        // 通过 UV 三角形光栅化从 atlas 提取原始纹理
        QRect uvPixelRect;
        QImage meshImage = RasterizeMesh(atlas, uvPtr, mesh->vertex_counts,
                                         idxPtr, idxCount, uvPixelRect);

        if (meshImage.isNull() || meshImage.width() == 0 || meshImage.height() == 0) {
            continue;
        }

        // 计算画布位置：使用默认 keyform 的顶点位置
        QRect canvasRect;
        int keyformBegin = mesh->keyform_sources_begin_indices;
        if (keyformBegin >= 0 && keyformBegin < table->art_mesh_keyforms.size()) {
            int posBegin = table->art_mesh_keyforms[keyformBegin]->keyform_position_sources_begin_indices;
            const int positionSize = static_cast<int>(table->keyform_positions->xys.size());
            if (posBegin >= 0 && posBegin <= positionSize && uvCount <= positionSize - posBegin) {
                const float* posPtr = &table->keyform_positions->xys[posBegin];
                canvasRect = ComputeCanvasRect(posPtr, mesh->vertex_counts, table->canvas_info);
            }
        } else {
            // 回退：使用 UV 像素区域
            canvasRect = uvPixelRect;
        }

        // 读取默认透明度
        float opacity = 1.0f;
        if (keyformBegin >= 0 && keyformBegin < table->art_mesh_keyforms.size()) {
            opacity = table->art_mesh_keyforms[keyformBegin]->opacities;
        }

        // 读取裁剪蒙版信息
        QVector<int> maskIndices;
        int maskBegin = mesh->drawable_mask_sources_begin_indices;
        int maskCount = mesh->drawable_mask_sources_counts;
        const int maskSize = static_cast<int>(table->drawable_masks.size());
        if (maskBegin >= 0 && maskCount >= 0 && maskBegin <= maskSize && maskCount <= maskSize - maskBegin) {
            for (int m = 0; m < maskCount; ++m) {
                int maskIdx = maskBegin + m;
                maskIndices.append(table->drawable_masks[maskIdx]->art_mesh_sources_indices);
            }
        }

        // 构建结果
        ExtractedLayer layer;
        layer.image = meshImage;
        layer.canvasRect = canvasRect;
        layer.name = mesh->ids;
        layer.meshIndex = i;
        layer.textureNo = texNo;
        layer.drawableFlags = mesh->drawable_flags;
        layer.opacity = opacity;
        layer.isVisible = mesh->is_visible;
        layer.isEnabled = mesh->is_enabled;
        layer.parentPartIndex = mesh->parent_part_indices;
        layer.maskIndices = maskIndices;

        results.append(layer);
    }

    return results;
}

QImage CTextureExtractor::RasterizeMesh(const QImage& atlas,
                                         const float* uvData, int vertexCount,
                                         const short* indexData, int indexCount,
                                         QRect& outUvRect)
{
    int atlasW = atlas.width();
    int atlasH = atlas.height();
    if (atlas.isNull() || !uvData || !indexData || vertexCount <= 0 || indexCount < 3) {
        outUvRect = QRect();
        return QImage();
    }

    // 计算 UV 像素空间包围盒
    float minPx = 1e9f, minPy = 1e9f, maxPx = -1e9f, maxPy = -1e9f;
    for (int v = 0; v < vertexCount; ++v) {
        float u = uvData[v * 2];
        float vCoord = uvData[v * 2 + 1];
        if (!std::isfinite(u) || !std::isfinite(vCoord)) {
            outUvRect = QRect();
            return QImage();
        }
        float px = u * atlasW;
        float py = vCoord * atlasH;
        minPx = std::min(minPx, px);
        minPy = std::min(minPy, py);
        maxPx = std::max(maxPx, px);
        maxPy = std::max(maxPy, py);
    }

    // 扩展 1 像素边距，clamp 到图集边界
    int left = std::max(0, static_cast<int>(std::floor(minPx)) - 1);
    int top = std::max(0, static_cast<int>(std::floor(minPy)) - 1);
    int right = std::min(atlasW, static_cast<int>(std::ceil(maxPx)) + 1);
    int bottom = std::min(atlasH, static_cast<int>(std::ceil(maxPy)) + 1);

    int outW = right - left;
    int outH = bottom - top;
    if (outW <= 0 || outH <= 0) {
        outUvRect = QRect();
        return QImage();
    }

    outUvRect = QRect(left, top, outW, outH);

    // 创建输出图像（透明背景）
    QImage output(outW, outH, QImage::Format_ARGB32);
    output.fill(Qt::transparent);

    // 遍历每个三角形进行光栅化
    int triCount = indexCount / 3;
    for (int t = 0; t < triCount; ++t) {
        int i0 = indexData[t * 3];
        int i1 = indexData[t * 3 + 1];
        int i2 = indexData[t * 3 + 2];
        if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;

        // atlas 中的像素坐标
        float ax0 = uvData[i0 * 2] * atlasW;
        float ay0 = uvData[i0 * 2 + 1] * atlasH;
        float ax1 = uvData[i1 * 2] * atlasW;
        float ay1 = uvData[i1 * 2 + 1] * atlasH;
        float ax2 = uvData[i2 * 2] * atlasW;
        float ay2 = uvData[i2 * 2 + 1] * atlasH;

        // 输出图像中的坐标（相对于包围盒左上角偏移）
        float ox0 = ax0 - left, oy0 = ay0 - top;
        float ox1 = ax1 - left, oy1 = ay1 - top;
        float ox2 = ax2 - left, oy2 = ay2 - top;

        RasterizeTriangle(output, atlas,
                          ox0, oy0, ox1, oy1, ox2, oy2,
                          ax0, ay0, ax1, ay1, ax2, ay2);
    }

    return output;
}

QRect CTextureExtractor::ComputeCanvasRect(const float* positions, int vertexCount,
                                            const CCanvas_info* canvasInfo)
{
    if (!positions || !canvasInfo || vertexCount <= 0 || !std::isfinite(canvasInfo->pixels_per_unit) ||
        canvasInfo->pixels_per_unit <= 0.0f ||
        !std::isfinite(canvasInfo->origin_x) || !std::isfinite(canvasInfo->origin_y)) return QRect();
    float ppu = canvasInfo->pixels_per_unit;
    float originX = canvasInfo->origin_x;
    float originY = canvasInfo->origin_y;

    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (int v = 0; v < vertexCount; ++v) {
        float mx = positions[v * 2];
        float my = positions[v * 2 + 1];
        if (!std::isfinite(mx) || !std::isfinite(my)) return QRect();
        // 模型空间 → 像素空间
        float px = (mx + originX) * ppu;
        float py = (originY - my) * ppu;
        minX = std::min(minX, px);
        minY = std::min(minY, py);
        maxX = std::max(maxX, px);
        maxY = std::max(maxY, py);
    }

    int left = static_cast<int>(std::floor(minX));
    int top = static_cast<int>(std::floor(minY));
    int right = static_cast<int>(std::ceil(maxX));
    int bottom = static_cast<int>(std::ceil(maxY));

    return QRect(left, top, right - left, bottom - top);
}

void CTextureExtractor::RasterizeTriangle(QImage& output, const QImage& atlas,
                                           float ox0, float oy0,
                                           float ox1, float oy1,
                                           float ox2, float oy2,
                                           float ax0, float ay0,
                                           float ax1, float ay1,
                                           float ax2, float ay2)
{
    // 三角形包围盒 (输出空间)
    int minX = std::max(0, static_cast<int>(std::floor(std::min({ox0, ox1, ox2}))));
    int minY = std::max(0, static_cast<int>(std::floor(std::min({oy0, oy1, oy2}))));
    int maxX = std::min(output.width() - 1, static_cast<int>(std::ceil(std::max({ox0, ox1, ox2}))));
    int maxY = std::min(output.height() - 1, static_cast<int>(std::ceil(std::max({oy0, oy1, oy2}))));

    // 重心坐标计算的预计算分母
    float denom = (oy1 - oy2) * (ox0 - ox2) + (ox2 - ox1) * (oy0 - oy2);
    if (std::abs(denom) < 1e-6f) return; // 退化三角形

    float invDenom = 1.0f / denom;

    int atlasW = atlas.width();
    int atlasH = atlas.height();

    for (int py = minY; py <= maxY; ++py) {
        QRgb* outScanLine = reinterpret_cast<QRgb*>(output.scanLine(py));
        for (int px = minX; px <= maxX; ++px) {
            float x = px + 0.5f; // 像素中心
            float y = py + 0.5f;

            // 重心坐标
            float w0 = ((oy1 - oy2) * (x - ox2) + (ox2 - ox1) * (y - oy2)) * invDenom;
            float w1 = ((oy2 - oy0) * (x - ox2) + (ox0 - ox2) * (y - oy2)) * invDenom;
            float w2 = 1.0f - w0 - w1;

            // 允许微小负值容差（边缘抗锯齿）
            const float eps = -0.001f;
            if (w0 < eps || w1 < eps || w2 < eps) continue;

            // 插值 atlas 坐标
            float sampleX = w0 * ax0 + w1 * ax1 + w2 * ax2;
            float sampleY = w0 * ay0 + w1 * ay1 + w2 * ay2;

            // 最近邻采样（clamp）
            int sx = std::clamp(static_cast<int>(sampleX), 0, atlasW - 1);
            int sy = std::clamp(static_cast<int>(sampleY), 0, atlasH - 1);

            QRgb pixel = reinterpret_cast<const QRgb*>(atlas.constScanLine(sy))[sx];

            // 只写入有内容的像素（避免覆盖已有三角形的结果）
            if (qAlpha(pixel) > 0 || qAlpha(outScanLine[px]) == 0) {
                outScanLine[px] = pixel;
            }
        }
    }
}
