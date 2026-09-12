#ifndef CINSPECTORPANEL_H
#define CINSPECTORPANEL_H

#include <QDockWidget>

namespace live2dstruct {
class CModelDocument;
}

class QLabel;
class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;

class CInspectorPanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CInspectorPanel(QWidget* parent = nullptr);

    void SetDocument(live2dstruct::CModelDocument* document);
    void SetSelection(const QString& name, const QString& type);
    void SetSelections(const QStringList& names, const QStringList& types);
    void ClearSelection();

signals:
    void VisibilityChanged(const QString& name, const QString& type, bool visible);
    void DrawableOpacityChanged(const QString& name, float opacity);
    void DrawableDrawOrderChanged(const QString& name, int drawOrder);

private:
    void showModelObject(const QString& name, const QString& type);

    live2dstruct::CModelDocument* m_document = nullptr;
    QString m_selectionName;
    QString m_selectionType;
    QLabel* m_emptyLabel = nullptr;
    QLabel* m_typeLabel = nullptr;
    QLabel* m_idLabel = nullptr;
    QLabel* m_parentLabel = nullptr;
    QLabel* m_deformerLabel = nullptr;
    QLabel* m_visibilityLabel = nullptr;
    QCheckBox* m_visibilityCheck = nullptr;
    QDoubleSpinBox* m_opacitySpin = nullptr;
    QSpinBox* m_drawOrderSpin = nullptr;
    bool m_updatingVisibility = false;
    QLabel* m_geometryLabel = nullptr;
    QLabel* m_textureLabel = nullptr;
    QLabel* m_drawOrderLabel = nullptr;
    QLabel* m_blendLabel = nullptr;
    QLabel* m_maskLabel = nullptr;
    QLabel* m_keyformLabel = nullptr;
};

#endif
