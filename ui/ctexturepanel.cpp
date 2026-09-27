#include "ctexturepanel.h"
#include "live2dstruct/util/cfilereader.h"
#include "live2dstruct/csectiontable.h"
#include "opengl/cuvgeometry.h"
#include "std/cpngimage.h"
#include <QAction>
#include <QCryptographicHash>
#include <QDir>
#include <QEvent>
#include <QFocusEvent>
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
#include <QPointer>
#include <QSaveFile>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolBar>
#include <QTimer>
#include <QShowEvent>
#include <QResizeEvent>
#include <QDebug>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <exception>
#include <filesystem>

using namespace live2dstruct;

namespace {
const double kZoomStep = 1.15;
const double kZoomMin = 0.05;
const double kZoomMax = 32.0;

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool readPng(const QString& path, QByteArray& result, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, QStringLiteral("无法读取 PNG：%1").arg(file.errorString()));
    const auto size = file.size();
    if (size <= 0 || size > CTextureAssets::MaximumBytes)
        return fail(error, QStringLiteral("PNG 文件为空或大小超出支持范围"));
    auto bytes = file.read(size);
    if (file.error() != QFileDevice::NoError || bytes.size() != size || file.size() != size || !file.atEnd())
        return fail(error, QStringLiteral("PNG 读取未完成或读取期间发生变化"));
    result.swap(bytes);
    return true;
}

bool sourceTarget(const QString& target, const CTextureAssets::Catalog& sources)
{
    const auto filesystemPath = [](const QString& path) {
#ifdef Q_OS_WIN
        return std::filesystem::path(path.toStdWString());
#else
        return std::filesystem::path(QFile::encodeName(path).toStdString());
#endif
    };
    const QFileInfo destination(target);
    const QString absolute = QDir::cleanPath(destination.absoluteFilePath());
    for (const auto& source : sources) {
        const QFileInfo original(source.path);
        if (absolute == QDir::cleanPath(original.absoluteFilePath())) return true;
        const auto canonical = original.canonicalFilePath();
        if (!canonical.isEmpty() && canonical == destination.canonicalFilePath()) return true;
        std::error_code error;
        if (std::filesystem::equivalent(filesystemPath(absolute), filesystemPath(source.path), error) && !error) return true;
    }
    return false;
}
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

QSize CAtlasView::minimumSizeHint() const
{
    auto result = QGraphicsView::minimumSizeHint();
    const int height = fontMetrics().height() * 9 + 2 * frameWidth() + style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    result.setHeight(std::max(result.height(), height));
    return result;
}

void CAtlasView::FitInViewAll()
{
    m_fitPending = scene() && !scene()->sceneRect().isEmpty();
    m_fitViewport = {};
    QueueFit();
}

void CAtlasView::QueueFit()
{
    if (!m_fitPending || m_fitQueued || !isVisible() || !viewport()->isVisible()) return;
    m_fitQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_fitQueued = false;
        if (!m_fitPending || !isVisible() || !viewport()->isVisible()) return;
        const auto size = viewport()->size();
        if (size.isEmpty()) return;
        if (size != m_fitViewport) { m_fitViewport = size; QueueFit(); return; }
        m_fitPending = false;
        if (!scene() || scene()->sceneRect().isEmpty()) return;
        fitInView(scene()->sceneRect(), Qt::KeepAspectRatio);
        m_zoom = transform().m11();
        emit ZoomChanged(m_zoom);
    });
}

void CAtlasView::ZoomReset()
{
    m_fitPending = false;
    resetTransform();
    m_zoom = 1.0;
    emit ZoomChanged(m_zoom);
}

void CAtlasView::wheelEvent(QWheelEvent* event)
{
    m_fitPending = false;
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

void CAtlasView::showEvent(QShowEvent* event)
{
    QGraphicsView::showEvent(event);
    QueueFit();
}

void CAtlasView::resizeEvent(QResizeEvent* event)
{
    QGraphicsView::resizeEvent(event);
    if (m_fitPending) { m_fitViewport = {}; QueueFit(); }
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
    const int gap = std::max(0, style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing));
    mainLayout->setContentsMargins(gap, gap, gap, gap);
    mainLayout->setSpacing(gap);

    // ---- 顶栏 ----
    QHBoxLayout* topBar = new QHBoxLayout();
    topBar->setSpacing(gap);

    m_selector = new QComboBox(wrapper);
    m_selector->setObjectName(QStringLiteral("textureSlot"));
    m_selector->setAccessibleName(QStringLiteral("纹理图集"));
    m_selector->setMinimumContentsLength(1);
    m_selector->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_selector->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    topBar->addWidget(m_selector, 1);

    m_checkWireframe = new QCheckBox("UV 线框", wrapper);
    m_checkWireframe->setEnabled(false);
    topBar->addWidget(m_checkWireframe);

    mainLayout->addLayout(topBar);
    auto* tools = new QToolBar(wrapper);
    tools->setObjectName(QStringLiteral("textureActions"));
    tools->setMovable(false);
    tools->setFocusPolicy(Qt::StrongFocus);
    const auto button = [&](const QString& text, const QString& tooltip, const char* name) {
        auto* action = tools->addAction(text);
        action->setToolTip(tooltip);
        auto* result = qobject_cast<QToolButton*>(tools->widgetForAction(action));
        result->setObjectName(QString::fromLatin1(name));
        result->setFocusPolicy(Qt::StrongFocus);
        return result;
    };
    m_btnReplace = button(QStringLiteral("替换 PNG…"), QStringLiteral("使用同尺寸 PNG 替换当前图集，原文件保持不变"), "textureReplace");
    m_btnRestore = button(QStringLiteral("恢复原图"), QStringLiteral("恢复模型首次加载时的素材，可撤销"), "textureRestore");
    tools->addSeparator();
    m_btnFit = button(QStringLiteral("适应窗口"), QStringLiteral("缩放图集以适应窗口"), "textureFit");
    m_btnOriginal = button(QStringLiteral("1:1"), QStringLiteral("按原始像素显示"), "textureOriginalSize");
    m_btnExport = button(QStringLiteral("导出 PNG"), QStringLiteral("导出当前已接受的图集 PNG，保留原始像素和文件信息"), "textureExport");
    mainLayout->addWidget(tools);
    for (auto* extension : tools->findChildren<QToolButton*>(QStringLiteral("qt_toolbar_ext_button")))
        extension->setFocusPolicy(Qt::StrongFocus);

    // ---- 视图 ----
    m_scene = new QGraphicsScene(this);
    m_view = new CAtlasView(wrapper);
    m_view->setScene(m_scene);
    mainLayout->addWidget(m_view, 1);

    // ---- 底部状态栏 ----
    m_infoLabel = new QLabel("未加载纹理", wrapper);
    m_infoLabel->setObjectName("texInfoLabel");
    m_infoLabel->setTextFormat(Qt::PlainText);
    m_infoLabel->setWordWrap(true);
    m_infoLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    mainLayout->addWidget(m_infoLabel);
    m_errorLabel = new QLabel(wrapper);
    m_errorLabel->setObjectName(QStringLiteral("textureError"));
    m_errorLabel->setTextFormat(Qt::PlainText);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_errorLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_errorLabel->hide();
    mainLayout->addWidget(m_errorLabel);

    setWidget(wrapper);

    connect(m_selector, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CTexturePanel::showTexture);
    connect(m_view, &CAtlasView::ScenePosClicked, this, &CTexturePanel::onSceneClicked);
    connect(m_btnFit->defaultAction(), &QAction::triggered, m_view, &CAtlasView::FitInViewAll);
    connect(m_btnOriginal->defaultAction(), &QAction::triggered, m_view, &CAtlasView::ZoomReset);
    connect(m_btnExport->defaultAction(), &QAction::triggered, this, &CTexturePanel::onExportPng);
    connect(m_btnReplace->defaultAction(), &QAction::triggered, this, &CTexturePanel::onReplacePng);
    connect(m_btnRestore->defaultAction(), &QAction::triggered, this, &CTexturePanel::onRestoreOriginal);
    connect(m_selector, &QComboBox::activated, this, [this] { if (!m_loading) emit HistorySelected(); });
    connect(m_checkWireframe, &QCheckBox::toggled, this, [this](bool on){
        if (m_overlay) m_overlay->setVisible(on);
    });
    connect(m_view, &CAtlasView::ZoomChanged, this, &CTexturePanel::RefreshInformation);
    for (auto* child : findChildren<QWidget*>()) child->installEventFilter(this);
    RefreshActions();
}

CTexturePanel::~CTexturePanel()
{
    m_scene->clear();
    m_overlay = nullptr;
}

void CTexturePanel::LoadTextures(const QMap<int, QString>& texMap, const QString& mocPath)
{
    try {
        CTextureAssets::Catalog sources;
        QString error;
        for (auto texture = texMap.cbegin(); texture != texMap.cend(); ++texture) {
            const QString path = QFileInfo(texture.value()).absoluteFilePath();
            QByteArray png;
            CTextureAssets::Source source;
            if (!readPng(path, png, &error) || !CTextureAssets::MakeSource(path, png, source, &error)) {
                RejectEdit(error); return;
            }
            sources.insert(texture.key(), std::move(source));
        }
        QByteArray moc;
        if (!sources.isEmpty() && !mocPath.isEmpty()) {
            QFile file(mocPath);
            if (!file.open(QIODevice::ReadOnly)) { RejectEdit(QStringLiteral("无法读取 UV 模型：%1").arg(file.errorString())); return; }
            moc = file.readAll();
            if (file.error() != QFileDevice::NoError || moc.isEmpty()) { RejectEdit(QStringLiteral("UV 模型读取失败")); return; }
        }
        if (!LoadSnapshot(sources, {}, mocPath.isEmpty() ? nullptr : &moc, &error)) RejectEdit(error);
    } catch (const std::exception&) { RejectEdit(QStringLiteral("无法加载纹理，原图集保持不变")); }
}

bool CTexturePanel::LoadTextureSnapshot(const CTextureAssets::Catalog& sources,
                                       const CTextureAssets::State& state, QString* error)
{
    return LoadSnapshot(sources, state, nullptr, error);
}

bool CTexturePanel::PrepareScene(int slot, const QMap<int, QImage>& images,
                                const QVector<CAtlasMeshUv>& meshes, const QString& selected,
                                std::unique_ptr<QGraphicsScene>& scene, CUvOverlayItem*& overlay,
                                QString* error) const
{
    auto candidate = std::make_unique<QGraphicsScene>();
    CUvOverlayItem* nextOverlay = nullptr;
    if (slot >= 0) {
        const auto image = images.constFind(slot);
        if (image == images.cend() || image->isNull()) return fail(error, QStringLiteral("所选纹理尚未加载"));
        const auto pixmap = QPixmap::fromImage(*image);
        if (pixmap.isNull()) return fail(error, QStringLiteral("无法准备图集显示，原图集保持不变"));
        auto* item = candidate->addPixmap(pixmap);
        item->setTransformationMode(Qt::SmoothTransformation);
        candidate->setSceneRect(pixmap.rect());
        QVector<const CAtlasMeshUv*> visible;
        for (const auto& mesh : meshes) if (mesh.textureNo == slot) visible.append(&mesh);
        if (!visible.isEmpty()) {
            nextOverlay = new CUvOverlayItem(pixmap.size());
            candidate->addItem(nextOverlay);
            nextOverlay->SetMeshes(std::move(visible));
            nextOverlay->SetSelected(selected);
            nextOverlay->setVisible(m_checkWireframe->isChecked());
        }
    }
    scene.swap(candidate);
    overlay = nextOverlay;
    return true;
}

bool CTexturePanel::LoadSnapshot(const CTextureAssets::Catalog& sources, const CTextureAssets::State& state,
                                 const QByteArray* mocBytes, QString* error)
{
    if (error) error->clear();
    if (m_loading) return fail(error, QStringLiteral("图集快照正在更新"));
    try {
        if (!CTextureAssets::ValidateCatalog(sources, error) || !CTextureAssets::ValidateState(sources, state, error)) return false;
        const bool preserve = sources == m_sources;
        if (preserve && state == m_state && !mocBytes) return true;
        if (m_snapshotRevision == std::numeric_limits<quint64>::max()) return fail(error, QStringLiteral("图集版本已超出范围，请重新打开模型"));
        auto catalog = sources;
        auto overrides = state;
        auto pngs = CTextureAssets::Effective(sources, state);
        QMap<int, QString> paths;
        QMap<int, QSize> sizes;
        QMap<int, QImage> images;
        QHash<QByteArray, QImage> decoded;
        QHash<QByteArray, int> previous;
        for (auto image = m_pngs.cbegin(); image != m_pngs.cend(); ++image)
            previous.insert(QCryptographicHash::hash(*image, QCryptographicHash::Sha256), image.key());
        qint64 decodedBytes = 0;
        for (auto png = pngs.cbegin(); png != pngs.cend(); ++png) {
            const auto& source = sources[png.key()];
            const auto digest = QCryptographicHash::hash(*png, QCryptographicHash::Sha256);
            if (!decoded.contains(digest)) {
                const qint64 bytes = qint64(source.size.width()) * source.size.height() * 4;
                if (bytes > CTextureAssets::MaximumBytes - decodedBytes)
                    return fail(error, QStringLiteral("图集解码后的总像素大小超出支持范围"));
                decodedBytes += bytes;
                QImage image;
                const auto cached = previous.constFind(digest);
                if (cached != previous.cend() && m_pngs.value(*cached) == *png) image = m_images.value(*cached);
                if (image.isNull() && !CPngImage::Decode(*png, image, error)) return false;
                decoded.insert(digest, std::move(image));
            }
            images.insert(png.key(), decoded.value(digest));
            paths.insert(png.key(), source.path);
            sizes.insert(png.key(), source.size);
        }
        QVector<CAtlasMeshUv> meshes = preserve ? m_meshes : QVector<CAtlasMeshUv>{};
        if (mocBytes && !sources.isEmpty() && !PrepareUvMeshes(*mocBytes, sizes, meshes, error)) return false;
        const auto selected = preserve ? m_selectedMesh : QString();
        const int slot = preserve && images.contains(m_currentTexKey) ? m_currentTexKey : images.isEmpty() ? -1 : images.firstKey();
        std::unique_ptr<QGraphicsScene> scene;
        CUvOverlayItem* overlay = nullptr;
        if (!PrepareScene(slot, images, meshes, selected, scene, overlay, error)) return false;
        const auto transform = m_view->transform();
        const int horizontal = m_view->horizontalScrollBar()->value(), vertical = m_view->verticalScrollBar()->value();
        const QPointer<CTexturePanel> alive(this);
        m_loading = true;
        const auto loading = qScopeGuard([alive] { if (alive) alive->m_loading = false; });
        m_sources.swap(catalog); m_state.swap(overrides); m_pngs.swap(pngs); m_images.swap(images);
        m_textures.swap(paths); m_atlasSizes.swap(sizes); m_meshes.swap(meshes);
        if (!preserve) m_runtimeRenderOrder.clear();
        m_selectedMesh = selected;
        m_currentTexKey = slot;
        ++m_snapshotRevision;
        {
            const QSignalBlocker selectorSignals(m_selector);
            m_selector->clear();
            for (auto texture = m_textures.cbegin(); texture != m_textures.cend(); ++texture) {
                m_selector->addItem(QStringLiteral("texture_%1 (%2)%3").arg(texture.key())
                    .arg(QFileInfo(texture.value()).fileName(), m_state.contains(texture.key()) ? QStringLiteral(" · 已替换") : QString()), texture.key());
                m_selector->setItemData(m_selector->count() - 1, QStringLiteral("<qt>%1</qt>").arg(texture.value().toHtmlEscaped()), Qt::ToolTipRole);
            }
            m_selector->setCurrentIndex(m_selector->findData(slot));
        }
        auto* oldScene = m_scene;
        m_scene = scene.release(); m_scene->setParent(this); m_overlay = overlay;
        m_view->setScene(m_scene); delete oldScene;
        if (preserve && slot >= 0) {
            m_view->setTransform(transform);
            m_view->horizontalScrollBar()->setValue(horizontal); m_view->verticalScrollBar()->setValue(vertical);
        } else {
            const QSignalBlocker viewSignals(m_view);
            m_view->FitInViewAll();
        }
        m_errorLabel->clear(); m_errorLabel->hide();
        RefreshActions(); RefreshInformation();
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法准备图集快照，原图集保持不变")); }
}

void CTexturePanel::SetEditHandler(EditHandler handler)
{
    if (m_publishing) return;
    if (m_snapshotRevision == std::numeric_limits<quint64>::max()) {
        m_edit = {};
        RefreshActions();
        return;
    }
    m_edit = std::move(handler);
    ++m_snapshotRevision;
    RefreshActions();
}

void CTexturePanel::SetEditingEnabled(bool enabled, const QString& reason)
{
    m_editingEnabled = enabled;
    m_editingReason = reason;
    RefreshActions();
}

bool CTexturePanel::CanEdit() const
{
    return m_editingEnabled && m_edit && !m_busy && m_sources.contains(m_currentTexKey);
}

void CTexturePanel::RefreshActions()
{
    const bool loaded = m_images.contains(m_currentTexKey);
    m_btnReplace->defaultAction()->setEnabled(CanEdit());
    m_btnRestore->defaultAction()->setEnabled(CanEdit() && m_state.contains(m_currentTexKey));
    m_btnExport->defaultAction()->setEnabled(loaded && !m_busy);
    m_btnFit->defaultAction()->setEnabled(loaded);
    m_btnOriginal->defaultAction()->setEnabled(loaded);
    m_selector->setEnabled(!m_busy && !m_images.isEmpty());
    m_checkWireframe->setEnabled(m_overlay != nullptr);
    const QString reason = !m_editingEnabled && !m_editingReason.isEmpty()
        ? QStringLiteral("<qt>%1</qt>").arg(m_editingReason.toHtmlEscaped()) : QString();
    m_btnReplace->defaultAction()->setToolTip(reason.isEmpty() ? QStringLiteral("使用同尺寸 PNG 替换当前图集，原文件保持不变") : reason);
    m_btnRestore->defaultAction()->setToolTip(reason.isEmpty() ? QStringLiteral("恢复模型首次加载时的素材，可撤销") : reason);
}

void CTexturePanel::RefreshInformation()
{
    if (!m_images.contains(m_currentTexKey)) { m_infoLabel->setText(QStringLiteral("未加载纹理")); return; }
    const auto size = m_atlasSizes.value(m_currentTexKey);
    int meshes = 0;
    for (const auto& mesh : m_meshes) meshes += mesh.textureNo == m_currentTexKey;
    m_infoLabel->setText(QStringLiteral("%1 | %2×%3 | 网格 %4 个 | %5 | 缩放 %6%")
        .arg(QFileInfo(m_textures.value(m_currentTexKey)).fileName()).arg(size.width()).arg(size.height()).arg(meshes)
        .arg(m_state.contains(m_currentTexKey) ? QStringLiteral("已替换") : QStringLiteral("原素材"))
        .arg(qRound(m_view->ZoomFactor() * 100)));
}

void CTexturePanel::RejectEdit(const QString& error)
{
    m_errorLabel->setText(error); m_errorLabel->setVisible(!error.isEmpty());
    emit EditRejected(error);
}

bool CTexturePanel::eventFilter(QObject* watched, QEvent* event)
{
    const auto reason = event->type() == QEvent::FocusIn ? static_cast<QFocusEvent*>(event)->reason() : Qt::OtherFocusReason;
    if (event->type() == QEvent::FocusIn && !m_loading && !m_busy &&
        (reason == Qt::MouseFocusReason || reason == Qt::TabFocusReason ||
         reason == Qt::BacktabFocusReason || reason == Qt::ShortcutFocusReason)) {
        const QPointer<CTexturePanel> alive(this);
        emit HistorySelected();
        if (!alive) return true;
    }
    return QDockWidget::eventFilter(watched, event);
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

bool CTexturePanel::PrepareUvMeshes(const QByteArray& mocBytes, const QMap<int, QSize>& atlasSizes,
                                    QVector<CAtlasMeshUv>& result, QString* error) const
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

        const QSize atlasSize = atlasSizes.value(uv.textureNo);
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
        if (!PrepareUvMeshes(mocBytes, m_atlasSizes, candidate, error)) return false;
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
    try {
        std::unique_ptr<QGraphicsScene> scene;
        CUvOverlayItem* overlay = nullptr;
        QString error;
        if (!PrepareScene(texKey, m_images, m_meshes, m_selectedMesh, scene, overlay, &error)) { RejectEdit(error); return; }
        auto* previous = m_scene;
        m_scene = scene.release(); m_scene->setParent(this); m_overlay = overlay;
        m_currentTexKey = texKey;
        m_view->setScene(m_scene); delete previous;
        RefreshActions(); RefreshInformation();
        m_view->FitInViewAll();
    } catch (const std::exception&) { RejectEdit(QStringLiteral("无法显示图集，原图集保持不变")); }
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

bool CTexturePanel::SubmitTexture(int slot, const QByteArray& png, QString* error)
{
    if (!m_editingEnabled || !m_edit || !m_sources.contains(slot))
        return fail(error, m_editingReason.isEmpty() ? QStringLiteral("当前不能修改纹理") : m_editingReason);
    CTextureAssets::State candidate;
    if (!CTextureAssets::Replace(m_sources, m_state, slot, png, candidate, error)) return false;
    if (candidate == m_state) return true;
    const auto handler = m_edit;
    const QPointer<CTexturePanel> alive(this);
    bool accepted = false;
    {
        m_publishing = true;
        const auto publishing = qScopeGuard([alive] { if (alive) alive->m_publishing = false; });
        try { accepted = handler(slot, png, error); }
        catch (...) { if (error) *error = QStringLiteral("纹理提交失败，原素材保持不变"); }
    }
    if (!alive) return accepted;
    if (!accepted) {
        if (error && error->isEmpty()) *error = QStringLiteral("纹理修改未能提交，原素材保持不变");
        return false;
    }
    m_errorLabel->clear(); m_errorLabel->hide();
    emit HistorySelected();
    return true;
}

void CTexturePanel::onReplacePng()
{
    if (!CanEdit()) return;
    const int slot = m_currentTexKey;
    const auto revision = m_snapshotRevision;
    const QPointer<CTexturePanel> alive(this);
    m_busy = true; RefreshActions();
    const auto restore = qScopeGuard([alive] { if (alive) { alive->m_busy = false; alive->RefreshActions(); } });
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("替换图集 PNG"),
        QFileInfo(m_textures.value(slot)).absolutePath(), QStringLiteral("PNG 图像 (*.png)"));
    if (!alive || path.isEmpty()) return;
    if (revision != m_snapshotRevision || slot != m_currentTexKey) {
        RejectEdit(QStringLiteral("图集已改变，请重新选择要替换的素材")); return;
    }
    QByteArray png;
    QString error;
    if (!readPng(path, png, &error) || !SubmitTexture(slot, png, &error)) {
        if (alive) RejectEdit(error);
    }
}

void CTexturePanel::onRestoreOriginal()
{
    if (!CanEdit() || !m_state.contains(m_currentTexKey)) return;
    const int slot = m_currentTexKey;
    const QByteArray original = m_sources.value(slot).png;
    const QPointer<CTexturePanel> alive(this);
    m_busy = true; RefreshActions();
    const auto restore = qScopeGuard([alive] { if (alive) { alive->m_busy = false; alive->RefreshActions(); } });
    QString error;
    if (!SubmitTexture(slot, original, &error) && alive) RejectEdit(error);
}

void CTexturePanel::onExportPng()
{
    if (m_busy || !m_pngs.contains(m_currentTexKey)) return;
    const QByteArray png = m_pngs.value(m_currentTexKey);
    const auto revision = m_snapshotRevision;
    const auto sources = m_sources;
    const QString suggested = QFileInfo(m_textures.value(m_currentTexKey)).fileName();
    const QPointer<CTexturePanel> alive(this);
    m_busy = true; RefreshActions();
    const auto restore = qScopeGuard([alive] { if (alive) { alive->m_busy = false; alive->RefreshActions(); } });
    const QString target = QFileDialog::getSaveFileName(this, QStringLiteral("导出图集 PNG"), suggested,
                                                       QStringLiteral("PNG 图像 (*.png)"));
    if (!alive || target.isEmpty()) return;
    if (revision != m_snapshotRevision) { RejectEdit(QStringLiteral("图集已改变，请重新导出")); return; }
    try {
        if (target.contains(QChar::Null) || sourceTarget(target, sources)) {
            RejectEdit(QStringLiteral("导出不能覆盖模型的原始素材，请选择其他文件")); return;
        }
        QSaveFile output(target);
        if (!output.open(QIODevice::WriteOnly) || output.write(png) != png.size()) {
            RejectEdit(QStringLiteral("无法保存 PNG：%1").arg(output.errorString())); return;
        }
        if (sourceTarget(target, sources)) { RejectEdit(QStringLiteral("导出目标与原始素材相同，已取消保存")); return; }
        if (!output.commit()) { RejectEdit(QStringLiteral("无法完成 PNG 保存：%1").arg(output.errorString())); return; }
        m_errorLabel->clear(); m_errorLabel->hide();
        m_infoLabel->setText(QStringLiteral("已导出：%1").arg(target));
    } catch (const std::exception&) { RejectEdit(QStringLiteral("无法保存 PNG，原文件保持不变")); }
}
