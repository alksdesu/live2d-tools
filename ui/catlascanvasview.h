#ifndef CATLASCANVASVIEW_H
#define CATLASCANVASVIEW_H

#include <QImage>
#include <QMap>
#include <QSet>
#include <QWidget>

class CAtlasCanvasView : public QWidget
{
    Q_OBJECT
public:
    struct Patch {
        QString id;
        int slot = -1;
        QVector<QPointF> points;
        QVector<quint16> triangles;
        bool editable = true;
        QString reason;
    };
    struct Content {
        QMap<int, QImage> images;
        QVector<Patch> patches;
        QSet<QString> selection;
    };
    explicit CAtlasCanvasView(QWidget* parent = nullptr);
    bool PrepareContent(const QMap<int, QImage>& images, const QVector<Patch>& patches, Content& result, QString* error = nullptr) const;
    void AdoptContent(Content&& content);
    bool SetContent(const QMap<int, QImage>& images, const QVector<Patch>& patches, QString* error = nullptr);
    void SetSlot(int slot);
    int Slot() const { return m_slot; }
    void SetSelectedMeshes(const QSet<QString>& ids);
    const QSet<QString>& SelectedMeshes() const { return m_selected; }
    bool SelectionBounds(QRectF& bounds) const;
    void SetEditingEnabled(bool enabled);
    void SetGutter(int pixels);
    void SelectAll();
    void FitAtlas();
    void FitSelection();
    void CancelGesture();
    bool HasGesture() const;
    QPointF PixelToViewport(QPointF point) const;
    QPointF ViewportToPixel(QPointF point) const;
    QSize minimumSizeHint() const override;

signals:
    void SelectionChanged();
    void TranslationRequested(const QStringList& meshes, QPoint delta);
    void PointerMoved(QPointF pixel);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QRectF Bounds(const Patch& patch) const;
    int Hit(QPointF position) const;
    void RequestFit(QRectF bounds);
    void QueueFit();
    void CancelQueuedFit();
    void SetGhost(QPointF delta, Qt::KeyboardModifiers modifiers);
    void FinishTranslation();
    QMap<int, QImage> m_images;
    QVector<Patch> m_patches;
    QSet<QString> m_selected, m_boxBefore;
    QPointF m_offset, m_press, m_last, m_ghost;
    QRectF m_fitBounds;
    QSize m_fitViewport;
    double m_scale = 1;
    int m_slot = -1, m_axis = 0, m_gutter = 1;
    bool m_enabled = true, m_fitPending = false, m_fitQueued = false, m_space = false;
    bool m_pan = false, m_box = false, m_drag = false, m_keyboard = false;
    Qt::KeyboardModifiers m_boxModifiers;
};

#endif
