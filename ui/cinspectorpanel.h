#ifndef CINSPECTORPANEL_H
#define CINSPECTORPANEL_H

#include <QDockWidget>
#include <QSet>
#include <QVector>

namespace live2dstruct {
class CModelDocument;
struct CModelEdit;
}

class QLabel;
class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;
class QComboBox;
class QTableWidget;
class QPushButton;

class CInspectorPanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CInspectorPanel(QWidget* parent = nullptr);

    void SetDocument(live2dstruct::CModelDocument* document);
    void SetSelection(const QString& name, const QString& type);
    void SetSelections(const QStringList& names, const QStringList& types);
    void ClearSelection();
    void RefreshSelection();
    void SetLockedState(const QSet<QString>& lockedKeys);

public slots:
    void Undo();
    void Redo();
    void ClearHistory();
    void SyncHistoryState();

signals:
    void VisibilityChanged(const QString& name, const QString& type, bool visible);
    void DrawableOpacityChanged(const QString& name, float opacity);
    void DrawableDrawOrderChanged(const QString& name, int drawOrder);
    void DrawableKeyformPositionsChanged(const QString& name, int keyformIndex, const QVector<float>& positions);
    void DrawableIndicesChanged(const QString& name, const QVector<quint16>& indices);
    void HistoryChanged(bool canUndo, bool canRedo);

private:
    void showModelObject(const QString& name, const QString& type);
    void EmitEdit(const live2dstruct::CModelEdit& edit, bool forward);
    void RebuildKeyformTable();
    void ApplyKeyformCellEdit(int row, int column);
    void MirrorKeyform(bool horizontal);
    void InterpolatePreviousKeyform();
    void CopyKeyformToNext();
    void ResetCurrentKeyform();
    void AutoTriangulate();
    void CopyTopology();
    void PasteTopology();
    void CopyCurrentKeyform();
    void PasteCurrentKeyform();
    bool IsLocked(const QString& type, const QString& name) const;

    live2dstruct::CModelDocument* m_document = nullptr;
    QString m_selectionName;
    QString m_selectionType;
    QStringList m_selectionNames;
    QStringList m_selectionTypes;
    QLabel* m_emptyLabel = nullptr;
    QLabel* m_typeLabel = nullptr;
    QLabel* m_idLabel = nullptr;
    QLabel* m_parentLabel = nullptr;
    QLabel* m_deformerLabel = nullptr;
    QLabel* m_visibilityLabel = nullptr;
    QCheckBox* m_visibilityCheck = nullptr;
    QDoubleSpinBox* m_opacitySpin = nullptr;
    QSpinBox* m_drawOrderSpin = nullptr;
    QComboBox* m_keyformCombo = nullptr;
    QTableWidget* m_vertexTable = nullptr;
    QPushButton* m_mirrorHorizontal = nullptr;
    QPushButton* m_mirrorVertical = nullptr;
    QPushButton* m_interpolatePrevious = nullptr;
    QPushButton* m_copyToNext = nullptr;
    QPushButton* m_resetKeyform = nullptr;
    QPushButton* m_copyKeyform = nullptr;
    QPushButton* m_pasteKeyform = nullptr;
    QPushButton* m_autoTriangulate = nullptr;
    QPushButton* m_copyTopology = nullptr;
    QPushButton* m_pasteTopology = nullptr;
    bool m_updatingVisibility = false;
    bool m_updatingKeyform = false;
    QSet<QString> m_lockedKeys;
    QLabel* m_geometryLabel = nullptr;
    QLabel* m_textureLabel = nullptr;
    QLabel* m_drawOrderLabel = nullptr;
    QLabel* m_blendLabel = nullptr;
    QLabel* m_maskLabel = nullptr;
    QLabel* m_keyformLabel = nullptr;
};

#endif
