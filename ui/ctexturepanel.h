#ifndef CTEXTUREPANEL_H
#define CTEXTUREPANEL_H

#include <QDockWidget>
#include <QByteArray>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QToolButton>
#include <QPainterPath>
#include <QVector>
#include <QMap>
#include <QImage>
#include <functional>
#include <memory>
#include "live2dstruct/ctextureassets.h"
#include "live2dstruct/ctextureworkspace.h"

// 单个 ArtMesh 在图集 UV 像素空间的网格数据
struct CAtlasMeshUv {
    QString name;               // art mesh ID
    int textureNo;              // 所属图集索引
    float drawOrder = 0.0f;     // 默认 keyform 绘制顺序
    QVector<QPointF> points;    // 顶点（图集像素坐标）
    QVector<int> triangles;     // 三角形顶点索引（3 个一组）
    QPainterPath outline;       // 三角形边缘合并路径（绘制用）
    QPainterPath fillPath;
};

// UV 线框叠加层：按 mesh 绘制三角网格，支持高亮选中项
class CUvOverlayItem;

// 图集交互视图：滚轮缩放（锚定鼠标）、拖拽平移、点击命中
class CAtlasView : public QGraphicsView
{
    Q_OBJECT
public:
    explicit CAtlasView(QWidget* parent = nullptr);
    void FitInViewAll();
    void ZoomReset();
    double ZoomFactor() const { return m_zoom; }
    QSize minimumSizeHint() const override;

signals:
    void ScenePosClicked(const QPointF& scenePos);
    void ZoomChanged(double factor);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void QueueFit();
    double m_zoom = 1.0;
    QPoint m_pressPos;
    QSize m_fitViewport;
    bool m_fitPending = false;
    bool m_fitQueued = false;
};

class CTexturePanel : public QDockWidget
{
    Q_OBJECT
public:
    using EditHandler = std::function<bool(int, const QByteArray&, QString*)>;
    explicit CTexturePanel(QWidget *parent = nullptr);
    ~CTexturePanel();

    // texMap: 图集索引 → PNG 路径；mocPath: 用于解析 UV 网格（可为空，仅禁用线框/反查）
    void LoadTextures(const QMap<int, QString>& texMap, const QString& mocPath = QString());
    bool LoadTextureSnapshot(const live2dstruct::CTextureAssets::Catalog& sources,
                             const live2dstruct::CTextureAssets::State& state, QString* error = nullptr);
    bool LoadWorkspaceSnapshot(const live2dstruct::CTextureAssets::Catalog& sources,
                               const live2dstruct::CTextureWorkspace::State& workspace,
                               const QByteArray& mocBytes = {}, QString* error = nullptr);
    void SetEditHandler(EditHandler handler);
    void SetEditingEnabled(bool enabled, const QString& reason = {});
    void SetPreviewActive(bool active);
    void SetSnapshotAvailable(bool available, const QString& reason = {});
    int CurrentSlot() const { return m_currentTexKey; }
    bool UpdateUvMeshes(const QByteArray& mocBytes, QString* error = nullptr);

    // 外部选中联动：高亮该 mesh 并切换到其所属图集
    void HighlightMesh(const QString& meshName);
    void SetRenderOrder(const QStringList& orderedNames);

signals:
    void HistorySelected();
    void EditUvRequested(const QString& meshName);
    void EditAtlasRequested(const QString& selectedMesh);
    void EditRejected(const QString& error);
    // 点击图集命中 ArtMesh（用于联动树面板与主视图高亮）
    void AtlasMeshPicked(const QString& meshName);

private slots:
    void showTexture(int selectorIndex);
    void onSceneClicked(const QPointF& scenePos);
    void onExportPng();
    void onReplacePng();
    void onRestoreOriginal();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool PrepareUvMeshes(const QByteArray& mocBytes, const QMap<int, QSize>& atlasSizes,
                         QVector<CAtlasMeshUv>& result, QString* error) const;
    bool UpdateUvSnapshot(const QByteArray& mocBytes, QString* error);
    bool LoadSnapshot(const live2dstruct::CTextureAssets::Catalog& sources,
                      const live2dstruct::CTextureWorkspace::State& workspace, const QByteArray* mocBytes, QString* error);
    bool PrepareScene(int slot, const QMap<int, QImage>& images, const QVector<CAtlasMeshUv>& meshes,
                      const QString& selected, std::unique_ptr<QGraphicsScene>& scene,
                      CUvOverlayItem*& overlay, QString* error) const;
    bool CanEdit() const;
    QString RestoreReason() const;
    QString AtlasName(int slot) const;
    void RefreshActions();
    void RefreshInformation();
    void RejectEdit(const QString& error);
    bool SubmitTexture(int slot, const QByteArray& png, QString* error);
    void rebuildScene(int texKey);
    QString hitTest(const QPointF& scenePos) const;

    // 顶栏控件
    QComboBox* m_selector;
    QCheckBox* m_checkWireframe;
    QToolButton* m_btnFit;
    QToolButton* m_btnOriginal;
    QToolButton* m_btnExport;
    QToolButton* m_btnReplace;
    QToolButton* m_btnRestore;
    QToolButton* m_btnEditUv;
    QToolButton* m_btnEditAtlas;
    QLabel* m_infoLabel;
    QLabel* m_errorLabel;

    // 视图
    CAtlasView* m_view;
    QGraphicsScene* m_scene;
    CUvOverlayItem* m_overlay = nullptr;

    QMap<int, QString> m_textures;
    live2dstruct::CTextureAssets::Catalog m_sources;
    live2dstruct::CTextureWorkspace::State m_workspace;
    QByteArray m_mocBytes;
    live2dstruct::CTextureAssets::State m_state, m_pngs;
    QMap<int, QImage> m_images;
    EditHandler m_edit;
    QString m_editingReason;
    QString m_snapshotReason;
    bool m_editingEnabled = false;
    bool m_previewActive = false;
    bool m_snapshotAvailable = true;
    bool m_busy = false;
    bool m_publishing = false;
    bool m_loading = false;
    quint64 m_snapshotRevision = 0;
    QMap<int, QSize> m_atlasSizes;
    QVector<CAtlasMeshUv> m_meshes;     // 全部 mesh 的 UV 网格
    int m_currentTexKey = -1;
    QString m_selectedMesh;
    QMap<QString, int> m_runtimeRenderOrder;
};

#endif // CTEXTUREPANEL_H
