#ifndef CTEXTUREPANEL_H
#define CTEXTUREPANEL_H

#include <QDockWidget>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QToolButton>
#include <QPainterPath>
#include <QVector>
#include <QMap>

// 单个 ArtMesh 在图集 UV 像素空间的网格数据
struct CAtlasMeshUv {
    QString name;               // art mesh ID
    int textureNo;              // 所属图集索引
    float drawOrder = 0.0f;     // 默认 keyform 绘制顺序
    QVector<QPointF> points;    // 顶点（图集像素坐标）
    QVector<int> triangles;     // 三角形顶点索引（3 个一组）
    QPainterPath outline;       // 三角形边缘合并路径（绘制用）
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

signals:
    void ScenePosClicked(const QPointF& scenePos);
    void ZoomChanged(double factor);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    double m_zoom = 1.0;
    QPoint m_pressPos;
};

class CTexturePanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CTexturePanel(QWidget *parent = nullptr);
    ~CTexturePanel();

    // texMap: 图集索引 → PNG 路径；mocPath: 用于解析 UV 网格（可为空，仅禁用线框/反查）
    void LoadTextures(const QMap<int, QString>& texMap, const QString& mocPath = QString());

    // 外部选中联动：高亮该 mesh 并切换到其所属图集
    void HighlightMesh(const QString& meshName);

signals:
    // 点击图集命中 ArtMesh（用于联动树面板与主视图高亮）
    void AtlasMeshPicked(const QString& meshName);

private slots:
    void showTexture(int selectorIndex);
    void onSceneClicked(const QPointF& scenePos);
    void onExportPng();

private:
    void buildUvMeshes(const QString& mocPath);
    void rebuildScene(int texKey);
    QString hitTest(const QPointF& scenePos) const;

    // 顶栏控件
    QComboBox* m_selector;
    QCheckBox* m_checkWireframe;
    QToolButton* m_btnFit;
    QToolButton* m_btnOriginal;
    QToolButton* m_btnExport;
    QLabel* m_infoLabel;

    // 视图
    CAtlasView* m_view;
    QGraphicsScene* m_scene;
    CUvOverlayItem* m_overlay = nullptr;

    QMap<int, QString> m_textures;
    QVector<CAtlasMeshUv> m_meshes;     // 全部 mesh 的 UV 网格
    int m_currentTexKey = -1;
    QString m_selectedMesh;
};

#endif // CTEXTUREPANEL_H
