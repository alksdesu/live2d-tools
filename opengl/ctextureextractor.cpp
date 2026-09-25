#include "ctextureextractor.h"
#include "cuvgeometry.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/cmeshtopology.h"
#include "live2dstruct/cdrawableflags.h"
#include "live2dstruct/ccanvasgeometry.h"
#include "std/cexportcontrol.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <exception>
#include <utility>
#include <QDebug>

using namespace live2dstruct;

CTextureExtractor::CTextureExtractor() {}
CTextureExtractor::~CTextureExtractor() {}

bool CTextureExtractor::LoadAtlases(const QVector<QString>& atlasPaths, CExportControl* control)
{
    try {
        if (control && !control->Checkpoint()) return false;
        if (atlasPaths.isEmpty()) {
            if (control) control->Fail(QStringLiteral("没有可加载的纹理图集"));
            return false;
        }
        QVector<QImage> candidate;
        candidate.reserve(atlasPaths.size());
        for (const QString& path : atlasPaths) {
            if (control && !control->Report(CExportControl::Phase::Preparing, candidate.size(), atlasPaths.size(), path)) return false;
            QImage img(path);
            if (control && !control->Checkpoint()) return false;
            if (img.isNull()) {
                qWarning("CTextureExtractor: 无法加载纹理图集: %s", path.toUtf8().constData());
                if (control) control->Fail(QStringLiteral("无法加载纹理图集：%1").arg(path));
                return false;
            }
            QImage converted = img.convertToFormat(QImage::Format_ARGB32);
            if (control && !control->Checkpoint()) return false;
            if (converted.isNull()) {
                if (control) control->Fail(QStringLiteral("无法转换纹理图集像素格式：%1").arg(path));
                return false;
            }
            candidate.append(std::move(converted));
        }
        if (control && !control->Report(CExportControl::Phase::Preparing, candidate.size(), atlasPaths.size())) return false;
        m_atlases.swap(candidate);
        return true;
    } catch (const std::exception& exception) {
        if (control) control->Fail(QStringLiteral("加载纹理图集失败：%1").arg(QString::fromUtf8(exception.what())));
        return false;
    }
}

QSize CTextureExtractor::GetCanvasPixelSize(const CCanvas_info* canvasInfo)
{
    return canvasInfo ? CCanvasGeometry::PixelSize(canvasInfo->width, canvasInfo->height) : QSize();
}

uint32_t CTextureExtractor::BlendModeToPsd(int drawableFlags)
{
    switch (CDrawableFlags::BlendMode(drawableFlags)) {
    case 1:  return 0x6C646467; // 'lddg' Linear Dodge (Additive)
    case 2:  return 0x6D756C20; // 'mul ' Multiply
    default: return 0x6E6F726D; // 'norm' Normal
    }
}

QVector<ExtractedLayer> CTextureExtractor::ExtractAll(CSectionTable* table, bool includeHidden,
                                                      const QHash<QString, QVector<quint16>>* indexOverrides,
                                                      CExportControl* control)
{
    try {
        QVector<ExtractedLayer> results;
        if (control && !control->Checkpoint()) return {};
        if (!table || !table->IsValid() || !table->count_info || !table->uvs ||
            !table->position_indices || !table->keyform_positions || !table->canvas_info) {
            if (control) control->Fail(QStringLiteral("无法提取纹理：模型区段数据无效"));
            return results;
        }
        int meshCount = table->count_info->art_meshes;
        meshCount = std::min(meshCount, static_cast<int>(table->art_meshes.size()));
        results.reserve(meshCount);

        for (int i = 0; i < meshCount; ++i) {
            CArt_meshes* mesh = table->art_meshes[i];
            if (control && !control->Report(CExportControl::Phase::Extracting, i, meshCount, mesh->ids)) return {};

            // 跳过隐藏图层（如果不包括的话）
            if (!includeHidden && !mesh->is_visible) {
                continue;
            }

            // 跳过无顶点的 mesh
            if (mesh->vertex_counts <= 0 || mesh->vertex_counts > 1000000 ||
                mesh->vertex_counts > std::numeric_limits<int>::max() / 2) {
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

            // 提取 UV 数据（归一化 [0,1]）
            int uvBegin = mesh->uv_sources_begin_indices;
            int uvCount = mesh->vertex_counts * 2;

            // 提取三角形索引
            int idxBegin = mesh->position_index_sources_begin_indices;
            int idxCount = mesh->position_index_sources_counts;
            const int uvSize = static_cast<int>(table->uvs->uvs.size());
            const int indexSize = static_cast<int>(table->position_indices->indices.size());
            if (uvBegin < 0 || uvCount < 0 || uvBegin > uvSize || uvCount > uvSize - uvBegin ||
                idxBegin < 0 || idxCount < 3 || idxCount % 3 != 0 || idxBegin > indexSize || idxCount > indexSize - idxBegin) {
                continue;
            }

            // 准备 UV 数据指针
            const float* uvPtr = &table->uvs->uvs[uvBegin];
            QVector<short> overrideIndices;
            const short* idxPtr = &table->position_indices->indices[idxBegin];
            if (indexOverrides) {
                const auto overrideIt = indexOverrides->constFind(mesh->ids);
                if (control && !control->Checkpoint()) return {};
                if (overrideIt != indexOverrides->cend() &&
                    CMeshTopology::IsValid(overrideIt.value(), mesh->vertex_counts)) {
                    if (control && !control->Checkpoint()) return {};
                    overrideIndices.reserve(overrideIt.value().size());
                    for (qsizetype index = 0; index < overrideIt.value().size(); ++index) {
                        if (control && index % 1024 == 0 && !control->Checkpoint()) return {};
                        overrideIndices.append(static_cast<short>(overrideIt.value()[index]));
                    }
                    idxPtr = overrideIndices.constData();
                    idxCount = overrideIndices.size();
                }
            }
            bool indicesValid = true;
            for (int t = 0; t < idxCount; ++t) {
                if (control && t % 1024 == 0 && !control->Checkpoint()) return {};
                if (idxPtr[t] < 0 || idxPtr[t] >= mesh->vertex_counts) {
                    indicesValid = false;
                    break;
                }
            }
            if (!indicesValid) continue;

            // 通过 UV 三角形光栅化从 atlas 提取原始纹理
            QRect uvPixelRect;
            QImage meshImage = RasterizeMesh(atlas, uvPtr, mesh->vertex_counts,
                                             idxPtr, idxCount, uvPixelRect, control);
            if (control && !control->Checkpoint()) return {};

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
                    canvasRect = ComputeCanvasRect(posPtr, mesh->vertex_counts, table->canvas_info, control);
                    if (control && !control->Checkpoint()) return {};
                }
            } else {
                // 回退：使用 UV 像素区域
                canvasRect = uvPixelRect;
            }
            if (canvasRect.isEmpty()) canvasRect = uvPixelRect;

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
                    if (control && m % 1024 == 0 && !control->Checkpoint()) return {};
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

        if (control && !control->Report(CExportControl::Phase::Extracting, meshCount, meshCount)) return {};
        return results;
    } catch (const std::exception& exception) {
        if (control) control->Fail(QStringLiteral("提取纹理失败：%1").arg(QString::fromUtf8(exception.what())));
        return {};
    }
}

QImage CTextureExtractor::RasterizeMesh(const QImage& atlas,
                                         const float* uvData, int vertexCount,
                                         const short* indexData, int indexCount,
                                         QRect& outUvRect, CExportControl* control)
{
    outUvRect = QRect();
    const int atlasW = atlas.width(), atlasH = atlas.height();
    if (atlas.isNull() || !uvData || !indexData || vertexCount <= 0 || vertexCount > 1000000 ||
        indexCount < 3 || indexCount > 3000000 || indexCount % 3 != 0) return {};
    double minPx = std::numeric_limits<double>::max(), minPy = minPx;
    double maxPx = -minPx, maxPy = -minPx;
    for (int vertex = 0; vertex < vertexCount; ++vertex) {
        if (control && vertex % 1024 == 0 && !control->Checkpoint()) return {};
        const double u = uvData[vertex * 2], v = uvData[vertex * 2 + 1];
        if (!std::isfinite(u) || !std::isfinite(v)) return {};
        const double x = u * atlasW, y = v * atlasH;
        minPx = std::min(minPx, x);
        minPy = std::min(minPy, y);
        maxPx = std::max(maxPx, x);
        maxPy = std::max(maxPy, y);
    }
    if (maxPx < 0.0 || maxPy < 0.0 || minPx > atlasW || minPy > atlasH) return {};
    const int left = static_cast<int>(std::max(0.0, std::floor(std::clamp(minPx, 0.0, double(atlasW))) - 1.0));
    const int top = static_cast<int>(std::max(0.0, std::floor(std::clamp(minPy, 0.0, double(atlasH))) - 1.0));
    const int right = static_cast<int>(std::min(double(atlasW), std::ceil(std::clamp(maxPx, 0.0, double(atlasW))) + 1.0));
    const int bottom = static_cast<int>(std::min(double(atlasH), std::ceil(std::clamp(maxPy, 0.0, double(atlasH))) + 1.0));
    if (right <= left || bottom <= top) return {};
    QImage output(right - left, bottom - top, QImage::Format_ARGB32);
    if (output.isNull()) {
        if (control) control->Fail(QStringLiteral("无法分配纹理裁片图像"));
        return {};
    }
    output.fill(Qt::transparent);
    outUvRect = QRect(left, top, right - left, bottom - top);
    for (int triangle = 0; triangle < indexCount; triangle += 3) {
        if (control && !control->Checkpoint()) return {};
        const int i0 = indexData[triangle], i1 = indexData[triangle + 1], i2 = indexData[triangle + 2];
        if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;
        const double x0 = double(uvData[i0 * 2]) * atlasW;
        const double y0 = double(uvData[i0 * 2 + 1]) * atlasH;
        const double x1 = double(uvData[i1 * 2]) * atlasW;
        const double y1 = double(uvData[i1 * 2 + 1]) * atlasH;
        const double x2 = double(uvData[i2 * 2]) * atlasW;
        const double y2 = double(uvData[i2 * 2 + 1]) * atlasH;
        if (!RasterizeTriangle(output, atlas, x0, y0, x1, y1, x2, y2, left, top, control)) return {};
    }
    return output;
}

QRect CTextureExtractor::ComputeCanvasRect(const float* positions, int vertexCount,
                                            const CCanvas_info* canvasInfo, CExportControl* control)
{
    if (!positions || !canvasInfo || vertexCount <= 0 || vertexCount > 1000000) return {};
    double minX = std::numeric_limits<double>::max(), minY = minX;
    double maxX = -minX, maxY = -minX;
    for (int vertex = 0; vertex < vertexCount; ++vertex) {
        if (control && vertex % 1024 == 0 && !control->Checkpoint()) return {};
        QPointF pixels;
        if (!CCanvasGeometry::ModelToPixels(canvasInfo, QPointF(positions[vertex * 2], positions[vertex * 2 + 1]), pixels)) return {};
        minX = std::min(minX, pixels.x());
        minY = std::min(minY, pixels.y());
        maxX = std::max(maxX, pixels.x());
        maxY = std::max(maxY, pixels.y());
    }
    const double left = std::floor(minX), top = std::floor(minY);
    const double right = std::ceil(maxX), bottom = std::ceil(maxY);
    const double minimum = std::numeric_limits<int>::min(), maximum = std::numeric_limits<int>::max();
    if (left < minimum || top < minimum || right > maximum || bottom > maximum ||
        right <= left || bottom <= top || right - left > maximum || bottom - top > maximum) return {};
    return QRect(static_cast<int>(left), static_cast<int>(top),
                 static_cast<int>(right - left), static_cast<int>(bottom - top));
}

bool CTextureExtractor::RasterizeTriangle(QImage& output, const QImage& atlas,
                                           double x0, double y0, double x1, double y1,
                                           double x2, double y2, int atlasLeft, int atlasTop, CExportControl* control)
{
    const double left = std::max(0.0, std::floor(std::min({x0, x1, x2}) - atlasLeft));
    const double top = std::max(0.0, std::floor(std::min({y0, y1, y2}) - atlasTop));
    const double right = std::min(double(output.width() - 1), std::ceil(std::max({x0, x1, x2}) - atlasLeft));
    const double bottom = std::min(double(output.height() - 1), std::ceil(std::max({y0, y1, y2}) - atlasTop));
    if (right < left || bottom < top) return true;
    const QPointF a(x0, y0), b(x1, y1), c(x2, y2);
    if (CUvGeometry::Orientation(a, b, c) == 0) return true;
    const int minX = static_cast<int>(left), minY = static_cast<int>(top);
    const int maxX = static_cast<int>(right), maxY = static_cast<int>(bottom);
    for (int py = minY; py <= maxY; ++py) {
        if (control && !control->Report(CExportControl::Phase::Extracting, py - minY, maxY - minY + 1,
                                        QStringLiteral("光栅化纹理三角形"))) return false;
        QRgb* outputRow = reinterpret_cast<QRgb*>(output.scanLine(py));
        for (int px = minX; px <= maxX; ++px) {
            if (control && (px - minX) % 4096 == 0 && !control->Checkpoint()) return false;
            const QPointF point(double(px) + atlasLeft + 0.5, double(py) + atlasTop + 0.5);
            if (!CUvGeometry::ContainsTriangle(point, a, b, c)) continue;
            // UV 裁片只改变原点，直接采样可避免大坐标重心插值抵消。
            const qint64 sampleX = qint64(px) + atlasLeft, sampleY = qint64(py) + atlasTop;
            if (sampleX < 0 || sampleY < 0 || sampleX >= atlas.width() || sampleY >= atlas.height()) continue;
            const QRgb pixel = reinterpret_cast<const QRgb*>(atlas.constScanLine(static_cast<int>(sampleY)))[sampleX];
            if (qAlpha(pixel) > 0 || qAlpha(outputRow[px]) == 0) outputRow[px] = pixel;
        }
    }
    return !control || control->Checkpoint();
}
