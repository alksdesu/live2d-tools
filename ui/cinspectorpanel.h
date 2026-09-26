#ifndef CINSPECTORPANEL_H
#define CINSPECTORPANEL_H

#include <QDockWidget>
#include <QSet>
#include <QVector>
#include <QMap>
#include <functional>
#include <memory>

namespace live2dstruct {
class CModelDocument;
struct CModelEdit;
class CKeyformGridSnapshot;
}

class CKeyformMatrix;

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
    ~CInspectorPanel() override;

    void SetDocument(live2dstruct::CModelDocument* document);
    void SetSelection(const QString& name, const QString& type);
    void SetSelections(const QStringList& names, const QStringList& types);
    void ClearSelection();
    void RefreshSelection();
    bool SyncActiveKeyform(const QString& id, const QString& type, int keyform);
    void SetLockedState(const QSet<QString>& lockedKeys);
    void SetEditHandler(std::function<bool(const live2dstruct::CModelEdit&, QString*)> handler);
    void SetHistoryHandler(std::function<bool(bool, live2dstruct::CModelEdit&, QString*)> handler);
    void SetKeyformHandler(std::function<bool(const QString&, int, QString*)> handler);
    void SetDeformerKeyformHandler(std::function<bool(const QString&, const QString&, int, QString*)> handler);
    QString GetSelectionId() const { return m_selectionName; }
    QString GetSelectionType() const { return m_selectionType; }
    int GetActiveKeyformIndex() const { return m_activeKeyformIndex; }

public slots:
    void Undo();
    void Redo();
    void ClearHistory();
    void SyncHistoryState();

signals:
    void VisibilityChanged(const QString& name, const QString& type, bool visible);
    void VisibilityBatchChanged(const QMap<QString, bool>& values);
    void DrawableOpacityChanged(const QString& name, float opacity);
    void DrawableDrawOrderChanged(const QString& name, int drawOrder);
    void DrawableKeyformPositionsChanged(const QString& name, int keyformIndex, const QVector<float>& positions);
    void DrawableKeyformActivated(const QString& name, int keyformIndex);
    void DrawableKeyformSelected(const QString& name, int keyformIndex);
    void DrawableIndicesChanged(const QString& name, const QVector<quint16>& indices);
    void DrawableMaskIndicesChanged(const QString& name, const QVector<int>& maskIndices);
    void HistoryChanged(bool canUndo, bool canRedo);
    void EditRejected(const QString& error);

private:
    void showModelObject(const QString& name, const QString& type, bool preserveKeyforms = true);
    void EmitEdit(const live2dstruct::CModelEdit& edit, bool forward);
    void RebuildKeyformTable();
    void RefreshKeyformMatrix();
    void RequestMatrixKeyform(int keyform);
    void ApplyKeyformCellEdit(int row, int column);
    void MirrorKeyform(bool horizontal);
    void InterpolatePreviousKeyform();
    void CopyKeyformToNext();
    void ResetCurrentKeyform();
    void OptimizeTopology();
    void AutoTriangulate();
    void ApplyTopology(const QVector<quint16>& before, const QVector<quint16>& after);
    void CopyTopology();
    void PasteTopology();
    void CopyCurrentKeyform();
    void PasteCurrentKeyform();
    void EditMasks();
    void ResetMaskControls();
    void TranslateSelectedVertices();
    void MergeSelectedVertices();
    void TransformSelectedVertices();
    void RebuildRotationKeyformTable();
    void ApplyRotationKeyformCellEdit(int row, int column);
    void RebuildWarpKeyformTable();
    void ApplyWarpKeyformCellEdit(int row, int column);
    bool IsLocked(const QString& type, const QString& name) const;
    bool SubmitEdit(const live2dstruct::CModelEdit& edit);
    bool SubmitHistory(bool undo, live2dstruct::CModelEdit& edit);
    bool RequestKeyform(const QString& name, int index);
    bool RequestDeformerKeyform(int index);
    void RejectEdit(const QString& error);

    live2dstruct::CModelDocument* m_document = nullptr;
    std::function<bool(const live2dstruct::CModelEdit&, QString*)> m_editHandler;
    std::function<bool(bool, live2dstruct::CModelEdit&, QString*)> m_historyHandler;
    std::function<bool(const QString&, int, QString*)> m_keyformHandler;
    bool m_submitting = false;
    bool m_rebuildingTopology = false;
    int m_activeKeyformIndex = -1;
    int m_activeDeformerKeyform = -1;
    std::function<bool(const QString&, const QString&, int, QString*)> m_deformerKeyformHandler;
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
    QComboBox* m_deformerParentCombo = nullptr;
    QTableWidget* m_vertexTable = nullptr;
    CKeyformMatrix* m_keyformMatrix = nullptr;
    std::shared_ptr<const live2dstruct::CKeyformGridSnapshot> m_matrixSnapshot;
    QTableWidget* m_rotationKeyformTable = nullptr;
    QComboBox* m_warpKeyformCombo = nullptr;
    QTableWidget* m_warpKeyformTable = nullptr;
    QPushButton* m_mirrorHorizontal = nullptr;
    QPushButton* m_mirrorVertical = nullptr;
    QPushButton* m_interpolatePrevious = nullptr;
    QPushButton* m_copyToNext = nullptr;
    QPushButton* m_resetKeyform = nullptr;
    QPushButton* m_copyKeyform = nullptr;
    QPushButton* m_pasteKeyform = nullptr;
    QPushButton* m_optimizeTopology = nullptr;
    QPushButton* m_autoTriangulate = nullptr;
    QPushButton* m_copyTopology = nullptr;
    QPushButton* m_pasteTopology = nullptr;
    QPushButton* m_editMasks = nullptr;
    QCheckBox* m_maskInvertedCheck = nullptr;
    QDoubleSpinBox* m_vertexDeltaX = nullptr;
    QDoubleSpinBox* m_vertexDeltaY = nullptr;
    QPushButton* m_moveVertices = nullptr;
    QPushButton* m_mergeVertices = nullptr;
    QDoubleSpinBox* m_vertexScale = nullptr;
    QDoubleSpinBox* m_vertexRotation = nullptr;
    QPushButton* m_transformVertices = nullptr;
    bool m_updatingVisibility = false;
    bool m_updatingKeyform = false;
    bool m_updatingRotationKeyform = false;
    bool m_updatingWarpKeyform = false;
    bool m_updatingDeformerParent = false;
    QSet<QString> m_lockedKeys;
    QLabel* m_geometryLabel = nullptr;
    QLabel* m_textureLabel = nullptr;
    QLabel* m_drawOrderLabel = nullptr;
    QLabel* m_blendLabel = nullptr;
    QLabel* m_maskLabel = nullptr;
    QLabel* m_keyformLabel = nullptr;
};

#endif
