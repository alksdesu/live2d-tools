#ifndef CUVCANVASVIEW_H
#define CUVCANVASVIEW_H

#include "live2dstruct/cuveditdraft.h"
#include <QImage>
#include <QWidget>

class CUvCanvasView : public QWidget
{
    Q_OBJECT
public:
    explicit CUvCanvasView(QWidget* parent = nullptr);
    void SetSnapshot(const live2dstruct::CUvEditDraft::Snapshot& snapshot, const QMap<int, QImage>& images);
    void SetState(const live2dstruct::CModelMaterialState& state);
    void SetMesh(const QString& mesh);
    QString Mesh() const { return m_mesh; }
    const QSet<int>& SelectedVertices() const { return m_selected; }
    void SetSelectedVertices(const QSet<int>& vertices);
    void SelectAll();
    void FitAtlas();
    void FitSelection();
    void SetPixelSnap(bool enabled) { m_snap = enabled; }
    bool HasGesture() const { return m_dragging || m_keyboardGesture; }
    void CancelGesture();
    QPointF PixelToViewport(QPointF point) const;
    QPointF ViewportToPixel(QPointF point) const;
    QSize minimumSizeHint() const override;

signals:
    void SelectionChanged();
    void GestureStarted();
    void TranslationPreview(QPointF delta, int axis, bool snap);
    void GestureFinished(bool canceled);
    void PointerMoved(QPointF pixel);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    int MeshIndex() const;
    int HitVertex(QPointF position) const;
    int HitTriangle(QPointF position) const;
    QPointF Point(int mesh, int vertex) const;
    void Fit(QRectF bounds);
    void RequestFit(QRectF bounds);
    void QueueFit();
    void CancelQueuedFit();
    void SelectIsland(int vertex, Qt::KeyboardModifiers modifiers);
    void EmitTranslation(QPointF delta, Qt::KeyboardModifiers modifiers);
    live2dstruct::CUvEditDraft m_draft;
    live2dstruct::CModelMaterialState m_state;
    QMap<int, QImage> m_images;
    QString m_mesh;
    QSet<int> m_selected, m_boxBefore;
    QPointF m_offset, m_press, m_last, m_keyboardDelta;
    QRectF m_fitBounds;
    QSize m_fitViewport;
    double m_scale = 1;
    int m_axis = 0;
    bool m_fitPending = false, m_fitQueued = false, m_space = false, m_snap = false;
    bool m_pan = false, m_box = false, m_dragReady = false, m_dragging = false, m_keyboardGesture = false;
    Qt::KeyboardModifiers m_boxModifiers;
};

#endif
