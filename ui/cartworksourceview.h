#ifndef CARTWORKSOURCEVIEW_H
#define CARTWORKSOURCEVIEW_H

#include <QImage>
#include <QTransform>
#include <QWidget>
#include <functional>

class CArtworkSourceView : public QWidget
{
    Q_OBJECT
public:
    struct Coordinates {
        QPointF document;
        QPointF layerLocal;
        QPointF atlas;
        bool onLayer = false;
        bool onReference = false;
        bool hasAtlas = false;
    };
    struct Content {
        QImage source, reference;
        QRect sourceBounds, atlasBounds;
        QSize documentCanvas;
        QTransform atlasToDocument;
        QVector<QPointF> atlasPixels;
        QVector<quint16> triangles;
    };
    using MappingHandler = std::function<bool(const QTransform&, QString*)>;
    using CancellationHandler = std::function<bool(QString*)>;

    explicit CArtworkSourceView(QWidget* parent = nullptr);
    bool SetContent(const Content& content, QString* error = nullptr);
    bool SetSource(const QImage& raster, QRect documentBounds, QSize documentCanvas, QString* error = nullptr);
    bool SetReference(const QImage& crop, QRect atlasBounds, const QTransform& atlasToDocument, QString* error = nullptr);
    bool SetMeshOverlay(const QVector<QPointF>& atlasPixels, const QVector<quint16>& triangles, QString* error = nullptr);
    bool SetMapping(const QTransform& atlasToDocument, QString* error = nullptr);
    const QTransform& Mapping() const { return m_mapping; }
    bool SetSourceOpacity(double opacity);
    bool SetReferenceOpacity(double opacity);
    double SourceOpacity() const { return m_sourceOpacity; }
    double ReferenceOpacity() const { return m_referenceOpacity; }
    void SetPreviewHandler(MappingHandler handler);
    void SetCommitHandler(MappingHandler handler);
    void SetCancellationHandler(CancellationHandler handler);
    bool SetEditingEnabled(bool enabled, QString* error = nullptr);
    bool HasGesture() const { return m_gesture; }
    bool FinishGesture(QString* error = nullptr);
    bool CancelGesture(QString* error = nullptr);
    QString LastError() const { return m_error; }
    void FitAll();
    void FitSource();
    void FitReference();
    QPointF DocumentToViewport(QPointF point) const;
    QPointF ViewportToDocument(QPointF point) const;
    Coordinates CoordinatesAt(QPointF viewportPoint) const;
    QSize minimumSizeHint() const override;

signals:
    void GestureStarted();
    void GestureFinished(bool canceled);
    void MappingPreviewed(const QTransform& mapping);
    void MappingCommitted(const QTransform& mapping);
    void MappingCanceled(const QTransform& mapping);
    void EditRejected(const QString& error);
    void PointerMoved(QPointF document, QPointF layerLocal, QPointF atlas, bool onReference);
    void ViewChanged();

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
    bool ReadyForData(QString* error) const;
    bool PrepareMapping(const QTransform& mapping, QTransform& inverse, QString* error) const;
    bool PreviewMapping(const QTransform& mapping, QString* error);
    bool Translate(QPointF delta, Qt::KeyboardModifiers modifiers, QString* error);
    void BeginGesture();
    void RejectEdit(const QString& error);
    void ResetGesture();
    QRectF ReferenceBounds() const;
    void RequestFit(QRectF bounds);
    void QueueFit();
    void CancelQueuedFit();
    QImage m_source, m_reference;
    QRect m_sourceBounds, m_atlasBounds;
    QSize m_documentCanvas;
    QVector<QPointF> m_vertices;
    QVector<quint16> m_triangles;
    QTransform m_mapping, m_inverse, m_gestureBefore;
    MappingHandler m_preview, m_commit;
    CancellationHandler m_cancel;
    QString m_error;
    QPointF m_offset, m_press, m_pressDocument, m_last, m_keyboardDelta;
    QRectF m_fitBounds;
    QSize m_fitViewport;
    double m_scale = 1, m_sourceOpacity = 1, m_referenceOpacity = .5;
    bool m_editing = true, m_busy = false, m_gesture = false, m_dragReady = false, m_dragging = false, m_keyboard = false;
    bool m_pan = false, m_space = false, m_fitPending = false, m_fitQueued = false;
};

#endif
