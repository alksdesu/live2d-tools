#include "ctexturepanel.h"
#include "live2dstruct/util/cfilereader.h"
#include "live2dstruct/csectiontable.h"
#include "opengl/cuvgeometry.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGraphicsPixmapItem>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QImageReader>
#include <QSignalBlocker>
#include <QDebug>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <exception>

using namespace live2dstruct;

namespace {
const double kZoomStep = 1.15;
const double kZoomMin = 0.05;
const double kZoomMax = 32.0;
} // namespace

// ============================================================
// CUvOverlayItem：UV 线框叠加层
// ============================================================
class CUvOverlayItem : public QGraphicsItem
{
public:
    explicit CUvOverlayItem(const QSizeF& atlasSize)
        : m_bounds(QPointF(0, 0), atlasSize) {}

    void SetMeshes(QVector<const CAtlasMeshUv*> meshes)
    {
        m_meshes = std::move(meshes);
        update();
    }

    void SetSelected(const QString& name)
    {
        m_selected = name;
        update();
    }

    QRectF boundingRect() const override { return m_bounds; }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override
    {
        painter->save();
        painter->setClipRect(m_bounds, Qt::IntersectClip);
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setBrush(Qt::NoBrush);

        QPen normalPen(QColor(64, 156, 255, 140));
        normalPen.setCosmetic(true); // 缩放时线宽不变
        QPen selectedPen(QColor(255, 120, 40, 230));
        selectedPen.setCosmetic(true);
        selectedPen.setWidthF(1.6);

        const CAtlasMeshUv* selected = nullptr;
        painter->setPen(normalPen);
        for (const CAtlasMeshUv* mesh : m_meshes) {
            if (mesh->name == m_selected) {
                selected = mesh; // 选中项最后画，盖在上层
                continue;
            }
            painter->drawPath(mesh->outline);
        }
        if (selected) {
            painter->setBrush(QColor(255, 120, 40, 40));
            painter->setPen(Qt::NoPen);
            painter->drawPath(selected->fillPath);
            painter->setBrush(Qt::NoBrush);
            painter->setPen(selectedPen);
            painter->drawPath(selected->outline);
        }
        painter->restore();
    }

private:
    QRectF m_bounds;
    QVector<const CAtlasMeshUv*> m_meshes;
    QString m_selected;
};

// ============================================================
// CAtlasView：缩放/平移/点击
// ============================================================
CAtlasView::CAtlasView(QWidget* parent)
    : QGraphicsView(parent)
{
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setRenderHints(QPainter::SmoothPixmapTransform);
    setFrameShape(QFrame::NoFrame);
    setBackgroundBrush(QColor(38, 40, 48));
}

void CAtlasView::FitInViewAll()
{
    if (!scene() || scene()->sceneRect().isEmpty()) return;
    fitInView(scene()->sceneRect(), Qt::KeepAspectRatio);
    m_zoom = transform().m11();
    emit ZoomChanged(m_zoom);
}

void CAtlasView::ZoomReset()
{
    resetTransform();
    m_zoom = 1.0;
    emit ZoomChanged(m_zoom);
}

void CAtlasView::wheelEvent(QWheelEvent* event)
{
    const double factor = (event->angleDelta().y() > 0) ? kZoomStep : 1.0 / kZoomStep;
    const double next = m_zoom * factor;
    if (next < kZoomMin || next > kZoomMax) {
        event->accept();
        return;
    }
    m_zoom = next;
    scale(factor, factor);
    emit ZoomChanged(m_zoom);
    event->accept();
}

void CAtlasView::mousePressEvent(QMouseEvent* event)
{
    m_pressPos = event->pos();
    QGraphicsView::mousePressEvent(event);
}

void CAtlasView::mouseReleaseEvent(QMouseEvent* event)
{
    // 区分点击与拖拽平移：位移小于阈值视为点击
    if (event->button() == Qt::LeftButton &&
        (event->pos() - m_pressPos).manhattanLength() < 4) {
        emit ScenePosClicked(mapToScene(event->pos()));
    }
    QGraphicsView::mouseReleaseEvent(event);
}

// ============================================================
// CTexturePanel
// ============================================================
CTexturePanel::CTexturePanel(QWidget *parent)
    : QDockWidget("纹理", parent)
{
    setObjectName(QStringLiteral("texturePanel"));
    QWidget* wrapper = new QWidget(this);
    wrapper->setObjectName(QStringLiteral("texturePanelRoot"));
    QVBoxLayout* mainLayout = new QVBoxLayout(wrapper);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    // ---- 顶栏 ----
    QHBoxLayout* topBar = new QHBoxLayout();
    topBar->setSpacing(6);

    m_selector = new QComboBox(wrapper);
    m_selector->setMinimumWidth(180);
    topBar->addWidget(m_selector, 1);

    m_checkWireframe = new QCheckBox("UV 线框", wrapper);
    m_checkWireframe->setEnabled(false);
    topBar->addWidget(m_checkWireframe);

    m_btnFit = new QToolButton(wrapper);
    m_btnFit->setText("适应窗口");
    m_btnFit->setToolTip("缩放图集以适应窗口");
    topBar->addWidget(m_btnFit);

    m_btnOriginal = new QToolButton(wrapper);
    m_btnOriginal->setText("1:1");
    m_btnOriginal->setToolTip("按原始像素显示");
    topBar->addWidget(m_btnOriginal);

    m_btnExport = new QToolButton(wrapper);
    m_btnExport->setText("导出 PNG");
    m_btnExport->setEnabled(false);
    m_btnExport->setToolTip("导出当前图集为 PNG（直通 alpha）");
    topBar->addWidget(m_btnExport);

    mainLayout->addLayout(topBar);

    // ---- 视图 ----
    m_scene = new QGraphicsScene(this);
    m_view = new CAtlasView(wrapper);
    m_view->setScene(m_scene);
    mainLayout->addWidget(m_view, 1);

    // ---- 底部状态栏 ----
    m_infoLabel = new QLabel("未加载纹理", wrapper);
    m_infoLabel->setObjectName("texInfoLabel");
    mainLayout->addWidget(m_infoLabel);

    setWidget(wrapper);

    connect(m_selector, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CTexturePanel::showTexture);
    connect(m_view, &CAtlasView::ScenePosClicked, this, &CTexturePanel::onSceneClicked);
    connect(m_btnFit, &QToolButton::clicked, m_view, &CAtlasView::FitInViewAll);
    connect(m_btnOriginal, &QToolButton::clicked, m_view, &CAtlasView::ZoomReset);
    connect(m_btnExport, &QToolButton::clicked, this, &CTexturePanel::onExportPng);
    connect(m_checkWireframe, &QCheckBox::toggled, this, [this](bool on){
        if (m_overlay) m_overlay->setVisible(on);
    });
    connect(m_view, &CAtlasView::ZoomChanged, this, [this](double factor){
        if (m_currentTexKey < 0) return;
        QFileInfo fi(m_textures.value(m_currentTexKey));
        m_infoLabel->setText(QString("%1 | 缩放 %2%").arg(fi.fileName()).arg(qRound(factor * 100)));
    });
}

CTexturePanel::~CTexturePanel()
{
    m_scene->clear();
    m_overlay = nullptr;
}

void CTexturePanel::LoadTextures(const QMap<int, QString>& texMap, const QString& mocPath)
{
    const QSignalBlocker selectorSignals(m_selector);
    m_scene->clear();
    m_overlay = nullptr;
    m_scene->setSceneRect(QRectF());
    m_textures = texMap;
    m_atlasSizes.clear();
    for (auto it = texMap.cbegin(); it != texMap.cend(); ++it) {
        QImageReader image(it.value());
        m_atlasSizes.insert(it.key(), image.size());
    }
    m_meshes.clear();
    m_runtimeRenderOrder.clear();
    m_selectedMesh.clear();
    m_currentTexKey = -1;
    m_selector->clear();
    m_checkWireframe->setEnabled(false);
    m_btnExport->setEnabled(false);
    m_infoLabel->setText("未加载纹理");

    if (texMap.isEmpty()) return;

    if (!mocPath.isEmpty()) {
        QFile source(mocPath);
        QString error;
        if (!source.open(QIODevice::ReadOnly) || !UpdateUvMeshes(source.readAll(), &error))
            qWarning() << "CTexturePanel: UV 网格不可用:" << error;
    }
    m_checkWireframe->setEnabled(!m_meshes.isEmpty());

    for (auto it = texMap.constBegin(); it != texMap.constEnd(); ++it) {
        QFileInfo fi(it.value());
        m_selector->addItem(QString("texture_%1 (%2)").arg(it.key()).arg(fi.fileName()), it.key());
    }
    if (m_selector->count() > 0) {
        showTexture(0);
    }
}

void CTexturePanel::HighlightMesh(const QString& meshName)
{
    m_selectedMesh = meshName;
    for (const CAtlasMeshUv& mesh : m_meshes) {
        if (mesh.name != meshName) continue;
        // 切到该 mesh 所属图集
        if (mesh.textureNo != m_currentTexKey) {
            int selIdx = m_selector->findData(mesh.textureNo);
            if (selIdx >= 0) {
                m_selector->setCurrentIndex(selIdx); // 触发 rebuildScene
                return;
            }
        }
        break;
    }
    if (m_overlay) m_overlay->SetSelected(meshName);
}

void CTexturePanel::SetRenderOrder(const QStringList& orderedNames)
{
    QMap<QString, int> next;
    for (int i = 0; i < orderedNames.size(); ++i) {
        if (!next.contains(orderedNames[i])) next.insert(orderedNames[i], i);
    }
    if (next == m_runtimeRenderOrder) return;
    m_runtimeRenderOrder = next;
}

bool CTexturePanel::PrepareUvMeshes(const QByteArray& mocBytes, QVector<CAtlasMeshUv>& result, QString* error) const
{
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (mocBytes.isEmpty()) return fail(QStringLiteral("UV 网格缺少模型数据"));
    auto reader = CReader::FromBytes(mocBytes);
    CSectionTable table(reader);
    if (!table.IsValid() || !table.count_info || !table.uvs || !table.position_indices) {
        return fail(QStringLiteral("无法解析 UV 网格：%1").arg(reader.Error()));
    }

    const int meshCount = table.count_info->art_meshes;
    if (meshCount < 0 || meshCount != table.art_meshes.size()) return fail(QStringLiteral("UV 网格数量与模型不一致"));
    QVector<CAtlasMeshUv> candidate;
    candidate.reserve(meshCount);
    QSet<QString> ids;

    for (int i = 0; i < meshCount; ++i) {
        CArt_meshes* mesh = table.art_meshes[i];
        if (!mesh || mesh->ids.isEmpty() || mesh->ids.contains(QChar::Null) || ids.contains(mesh->ids))
            return fail(QStringLiteral("UV 网格身份无效或重复"));
        ids.insert(mesh->ids);
        if (mesh->vertex_counts < 0 || mesh->vertex_counts > 1000000 ||
            mesh->vertex_counts > std::numeric_limits<int>::max() / 2)
            return fail(QStringLiteral("UV 顶点数量无效：%1").arg(mesh->ids));
        const int uvSize = static_cast<int>(table.uvs->uvs.size());
        const int uvCount = mesh->vertex_counts * 2;
        if (mesh->uv_sources_begin_indices < 0 || mesh->uv_sources_begin_indices > uvSize ||
            uvCount > uvSize - mesh->uv_sources_begin_indices)
            return fail(QStringLiteral("UV 顶点切片越界：%1").arg(mesh->ids));
        const int indexSize = static_cast<int>(table.position_indices->indices.size());
        if (mesh->position_index_sources_begin_indices < 0 || mesh->position_index_sources_counts < 0 || mesh->position_index_sources_counts > 3000000 ||
            mesh->position_index_sources_counts % 3 != 0 ||
            mesh->position_index_sources_begin_indices > indexSize ||
            mesh->position_index_sources_counts > indexSize - mesh->position_index_sources_begin_indices)
            return fail(QStringLiteral("UV 三角形切片无效：%1").arg(mesh->ids));
        if (!mesh->vertex_counts && mesh->position_index_sources_counts)
            return fail(QStringLiteral("空网格包含三角形：%1").arg(mesh->ids));

        CAtlasMeshUv uv;
        uv.name = mesh->ids;
        uv.textureNo = mesh->texture_nos;
        if (mesh->keyform_sources_begin_indices >= 0 &&
            mesh->keyform_sources_begin_indices < table.art_mesh_keyforms.size()) {
            const float drawOrder = table.art_mesh_keyforms[mesh->keyform_sources_begin_indices]->draw_orders;
            if (!std::isfinite(drawOrder)) return fail(QStringLiteral("UV 网格绘制顺序无效：%1").arg(mesh->ids));
            uv.drawOrder = drawOrder;
        }

        const QSize atlasSize = m_atlasSizes.value(uv.textureNo);
        if (atlasSize.isEmpty()) return fail(QStringLiteral("UV 网格引用的图集未加载：%1").arg(mesh->ids));
        const double w = atlasSize.width();
        const double h = atlasSize.height();
        const QRectF atlasRect(0, 0, w, h);

        const float* uvPtr = uvCount ? &table.uvs->uvs[mesh->uv_sources_begin_indices] : nullptr;
        uv.points.reserve(mesh->vertex_counts);
        bool uvValid = true;
        for (int v = 0; v < mesh->vertex_counts; ++v) {
            const float u = uvPtr[v * 2];
            const float vCoord = uvPtr[v * 2 + 1];
            if (!std::isfinite(u) || !std::isfinite(vCoord)) {
                uvValid = false;
                break;
            }
            uv.points.append(QPointF(u * w, vCoord * h));
        }
        if (!uvValid) return fail(QStringLiteral("UV 网格含非有限坐标：%1").arg(mesh->ids));

        const int idxCount = mesh->position_index_sources_counts;
        const short* idxPtr = idxCount ? &table.position_indices->indices[mesh->position_index_sources_begin_indices] : nullptr;
        uv.triangles.reserve(idxCount);
        for (int t = 0; t < idxCount; ++t) {
            const int index = idxPtr[t];
            if (index < 0 || index >= uv.points.size()) {
                return fail(QStringLiteral("UV 三角形索引越界：%1").arg(mesh->ids));
            }
            uv.triangles.append(index);
        }
        if (uv.triangles.isEmpty()) continue;

        uv.fillPath.setFillRule(Qt::WindingFill);
        for (int t = 0; t + 2 < uv.triangles.size(); t += 3) {
            const QPointF& p0 = uv.points[uv.triangles[t]];
            const QPointF& p1 = uv.points[uv.triangles[t + 1]];
            const QPointF& p2 = uv.points[uv.triangles[t + 2]];
            const QPointF vertices[]{p0, p1, p2};
            for (int edge = 0; edge < 3; ++edge) {
                QPointF start = vertices[edge], end = vertices[(edge + 1) % 3];
                if (!CUvGeometry::ClipSegment(start, end, atlasRect)) continue;
                uv.outline.moveTo(start);
                uv.outline.lineTo(end);
            }
            auto polygon = CUvGeometry::ClipTriangle(p0, p1, p2, atlasRect);
            if (polygon.size() < 3) continue;
            if (CUvGeometry::Orientation(p0, p1, p2) < 0) std::reverse(polygon.begin(), polygon.end());
            uv.fillPath.moveTo(polygon.front());
            for (std::size_t point = 1; point < polygon.size(); ++point) uv.fillPath.lineTo(polygon[point]);
            uv.fillPath.closeSubpath();
        }

        candidate.append(std::move(uv));
    }
    std::stable_sort(candidate.begin(), candidate.end(), [](const CAtlasMeshUv& left, const CAtlasMeshUv& right) {
        return left.drawOrder < right.drawOrder;
    });
    result.swap(candidate);
    return true;
}

bool CTexturePanel::UpdateUvMeshes(const QByteArray& mocBytes, QString* error)
{
    if (error) error->clear();
    QVector<CAtlasMeshUv> candidate;
    std::unique_ptr<CUvOverlayItem> overlay;
    QString information;
    try {
        if (!PrepareUvMeshes(mocBytes, candidate, error)) return false;
        const auto same = [](const CAtlasMeshUv& left, const CAtlasMeshUv& right) {
            return left.name == right.name && left.textureNo == right.textureNo && left.drawOrder == right.drawOrder &&
                left.points == right.points && left.triangles == right.triangles;
        };
        if (candidate.size() == m_meshes.size() && std::equal(candidate.cbegin(), candidate.cend(), m_meshes.cbegin(), same)) return true;
        QVector<const CAtlasMeshUv*> visible;
        for (const auto& mesh : candidate) if (mesh.textureNo == m_currentTexKey) visible.append(&mesh);
        if (m_currentTexKey >= 0 && !m_scene->sceneRect().isEmpty()) {
            if (!visible.isEmpty()) {
                overlay = std::make_unique<CUvOverlayItem>(m_scene->sceneRect().size());
                overlay->SetMeshes(visible);
                overlay->SetSelected(m_selectedMesh);
                overlay->setVisible(m_checkWireframe->isChecked());
                m_scene->addItem(overlay.get());
            }
            const auto size = m_atlasSizes.value(m_currentTexKey);
            information = QStringLiteral("%1 | %2x%3 | 网格 %4 个")
                .arg(QFileInfo(m_textures.value(m_currentTexKey)).fileName()).arg(size.width()).arg(size.height()).arg(visible.size());
        }
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("无法准备 UV 网格，原图集保持不变");
        return false;
    }
    m_meshes.swap(candidate);
    delete m_overlay;
    m_overlay = overlay.release();
    m_checkWireframe->setEnabled(m_currentTexKey < 0 ? !m_meshes.isEmpty() : m_overlay != nullptr);
    if (!information.isEmpty()) m_infoLabel->setText(information);
    return true;
}

void CTexturePanel::rebuildScene(int texKey)
{
    m_scene->clear();
    m_overlay = nullptr;
    m_scene->setSceneRect(QRectF());
    m_currentTexKey = -1;
    m_btnExport->setEnabled(false);
    m_checkWireframe->setEnabled(false);

    const QString path = m_textures.value(texKey);
    QImage img(path);
    if (img.isNull()) {
        m_infoLabel->setText("无法加载: " + path);
        return;
    }

    QPixmap pixmap = QPixmap::fromImage(img);
    QGraphicsPixmapItem* pixItem = m_scene->addPixmap(pixmap);
    pixItem->setTransformationMode(Qt::SmoothTransformation);
    m_scene->setSceneRect(pixmap.rect());

    // UV 叠加层
    QVector<const CAtlasMeshUv*> current;
    for (const CAtlasMeshUv& mesh : m_meshes) {
        if (mesh.textureNo == texKey) current.append(&mesh);
    }
    if (!current.isEmpty()) {
        m_overlay = new CUvOverlayItem(pixmap.size());
        m_overlay->SetMeshes(current);
        m_overlay->SetSelected(m_selectedMesh);
        m_overlay->setVisible(m_checkWireframe->isChecked());
        m_scene->addItem(m_overlay);
    }

    m_currentTexKey = texKey;
    m_btnExport->setEnabled(true);
    m_checkWireframe->setEnabled(!current.isEmpty());
    m_view->FitInViewAll();

    QFileInfo fi(path);
    m_infoLabel->setText(QString("%1 | %2x%3 | 网格 %4 个")
        .arg(fi.fileName())
        .arg(img.width()).arg(img.height())
        .arg(current.size()));
}

void CTexturePanel::showTexture(int selectorIndex)
{
    if (selectorIndex < 0 || selectorIndex >= m_selector->count()) return;
    const int texKey = m_selector->itemData(selectorIndex).toInt();
    if (!m_textures.contains(texKey)) return;
    rebuildScene(texKey);
}

QString CTexturePanel::hitTest(const QPointF& scenePos) const
{
    if (m_currentTexKey < 0 || !std::isfinite(scenePos.x()) || !std::isfinite(scenePos.y()) ||
        !m_scene->sceneRect().contains(scenePos)) return {};
    QString hit;
    int hitOrder = -1;
    for (int m = m_meshes.size() - 1; m >= 0; --m) {
        const CAtlasMeshUv& mesh = m_meshes[m];
        if (mesh.textureNo != m_currentTexKey) continue;

        for (int t = 0; t + 2 < mesh.triangles.size(); t += 3) {
            if (mesh.triangles[t] < 0 || mesh.triangles[t] >= mesh.points.size() ||
                mesh.triangles[t + 1] < 0 || mesh.triangles[t + 1] >= mesh.points.size() ||
                mesh.triangles[t + 2] < 0 || mesh.triangles[t + 2] >= mesh.points.size()) continue;
            const QPointF& a = mesh.points[mesh.triangles[t]];
            const QPointF& b = mesh.points[mesh.triangles[t + 1]];
            const QPointF& c = mesh.points[mesh.triangles[t + 2]];

            if (CUvGeometry::ContainsTriangle(scenePos, a, b, c)) {
                const int order = m_runtimeRenderOrder.value(mesh.name, m);
                if (order >= hitOrder) {
                    hit = mesh.name;
                    hitOrder = order;
                }
                break;
            }
        }
    }
    return hit;
}

void CTexturePanel::onSceneClicked(const QPointF& scenePos)
{
    const QString hit = hitTest(scenePos);
    if (hit.isEmpty()) return;

    m_selectedMesh = hit;
    if (m_overlay) m_overlay->SetSelected(hit);
    emit AtlasMeshPicked(hit);
}

void CTexturePanel::onExportPng()
{
    if (m_currentTexKey < 0 || !m_textures.contains(m_currentTexKey)) return;

    const QString srcPath = m_textures[m_currentTexKey];
    QFileInfo fi(srcPath);
    const QString target = QFileDialog::getSaveFileName(
        this, "导出图集 PNG", fi.fileName(), "PNG 图像 (*.png)");
    if (target.isEmpty()) return;

    // 项目纹理为直通 alpha（未预乘），原图直存即可
    QImage img(srcPath);
    if (img.isNull() || !img.save(target, "PNG")) {
        QMessageBox::warning(this, "导出失败", "无法保存: " + target);
        return;
    }
    m_infoLabel->setText("已导出: " + target);
}
