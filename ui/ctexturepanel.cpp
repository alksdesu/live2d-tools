#include "ctexturepanel.h"
#include "live2dstruct/util/cfilereader.h"
#include "live2dstruct/csectiontable.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGraphicsPixmapItem>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QDebug>

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
            painter->setPen(selectedPen);
            painter->drawPath(selected->outline);
        }
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
    QWidget* wrapper = new QWidget(this);
    wrapper->setObjectName("texPanelRoot");
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

    // 深色工作区风格，选择器均限定在本面板内避免污染宿主
    wrapper->setStyleSheet(
        "#texPanelRoot { background: #2c2e36; }"
        "#texPanelRoot QComboBox, #texPanelRoot QToolButton {"
        "  background: #3a3d47; color: #e8eaf0; border: 1px solid #4a4e5a;"
        "  border-radius: 4px; padding: 3px 10px; }"
        "#texPanelRoot QToolButton:hover, #texPanelRoot QComboBox:hover {"
        "  background: #464a56; }"
        "#texPanelRoot QToolButton:pressed { background: #2f6fd0; }"
        "#texPanelRoot QCheckBox { color: #e8eaf0; }"
        "#texInfoLabel { color: #9aa0ae; padding: 2px 4px; }"
    );

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

CTexturePanel::~CTexturePanel() = default;

void CTexturePanel::LoadTextures(const QMap<int, QString>& texMap, const QString& mocPath)
{
    m_textures = texMap;
    m_meshes.clear();
    m_selectedMesh.clear();
    m_currentTexKey = -1;
    m_selector->clear();
    m_scene->clear();
    m_overlay = nullptr;
    m_infoLabel->setText("未加载纹理");

    if (texMap.isEmpty()) return;

    if (!mocPath.isEmpty()) {
        buildUvMeshes(mocPath);
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

void CTexturePanel::buildUvMeshes(const QString& mocPath)
{
    CReader reader(mocPath);
    CSectionTable table(reader);
    if (!table.IsValid() || !table.count_info || !table.uvs || !table.position_indices) {
        qWarning("CTexturePanel: moc3 解析失败，UV 线框不可用");
        return;
    }

    const int meshCount = table.count_info->art_meshes;
    m_meshes.reserve(meshCount);

    for (int i = 0; i < meshCount; ++i) {
        CArt_meshes* mesh = table.art_meshes[i];
        if (mesh->vertex_counts <= 0) continue;

        CAtlasMeshUv uv;
        uv.name = mesh->ids;
        uv.textureNo = mesh->texture_nos;

        const QString& atlasPath = m_textures.value(uv.textureNo);
        QImage probe(atlasPath);
        if (probe.isNull()) continue;
        const double w = probe.width();
        const double h = probe.height();

        const float* uvPtr = &table.uvs->uvs[mesh->uv_sources_begin_indices];
        uv.points.reserve(mesh->vertex_counts);
        for (int v = 0; v < mesh->vertex_counts; ++v) {
            uv.points.append(QPointF(uvPtr[v * 2] * w, uvPtr[v * 2 + 1] * h));
        }

        const short* idxPtr = &table.position_indices->indices[mesh->position_index_sources_begin_indices];
        const int idxCount = mesh->position_index_sources_counts;
        uv.triangles.reserve(idxCount);
        for (int t = 0; t < idxCount; ++t) {
            uv.triangles.append(idxPtr[t]);
        }

        // 预生成绘制路径
        for (int t = 0; t + 2 < uv.triangles.size(); t += 3) {
            const QPointF& p0 = uv.points[uv.triangles[t]];
            const QPointF& p1 = uv.points[uv.triangles[t + 1]];
            const QPointF& p2 = uv.points[uv.triangles[t + 2]];
            uv.outline.moveTo(p0);
            uv.outline.lineTo(p1);
            uv.outline.lineTo(p2);
            uv.outline.closeSubpath();
        }

        m_meshes.append(uv);
    }
}

void CTexturePanel::rebuildScene(int texKey)
{
    m_scene->clear();
    m_overlay = nullptr;

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
    // 重心坐标点内测试；后添加的 mesh 渲染顺序靠上，倒序遍历优先命中上层
    for (int m = m_meshes.size() - 1; m >= 0; --m) {
        const CAtlasMeshUv& mesh = m_meshes[m];
        if (mesh.textureNo != m_currentTexKey) continue;

        for (int t = 0; t + 2 < mesh.triangles.size(); t += 3) {
            const QPointF& a = mesh.points[mesh.triangles[t]];
            const QPointF& b = mesh.points[mesh.triangles[t + 1]];
            const QPointF& c = mesh.points[mesh.triangles[t + 2]];

            const double d = (b.y() - c.y()) * (a.x() - c.x()) + (c.x() - b.x()) * (a.y() - c.y());
            if (qFuzzyIsNull(d)) continue;
            const double w0 = ((b.y() - c.y()) * (scenePos.x() - c.x()) + (c.x() - b.x()) * (scenePos.y() - c.y())) / d;
            const double w1 = ((c.y() - a.y()) * (scenePos.x() - c.x()) + (a.x() - c.x()) * (scenePos.y() - c.y())) / d;
            const double w2 = 1.0 - w0 - w1;
            if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                return mesh.name;
            }
        }
    }
    return QString();
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
