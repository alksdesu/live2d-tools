#ifndef CPARAMPANEL_H
#define CPARAMPANEL_H

#include <QDockWidget>
#include <QScrollArea>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QLabel>
#include <QMap>
#include <QSet>
#include <QMap>
#include <functional>
#include <memory>
#include "live2dstruct/cmocparameterstructurebuilder.h"

class CCidJson;
class QToolButton;
class QComboBox;
class QGroupBox;
class CXYParameterPad;
namespace live2dstruct { class CModelDocument; struct CModelEdit; }

class CParamPanel : public QDockWidget
{
    Q_OBJECT
public:
    using PreEditGuard = std::function<bool(QString*)>;
    using MetadataEditHandler = std::function<bool(const live2dstruct::CModelEdit&, QString*)>;
    using HistoryHandler = std::function<bool(bool, live2dstruct::CModelEdit&, QString*)>;
    using StructureEditHandler = std::function<bool(const live2dstruct::CModelEdit&, QString*)>;
    using ParameterReferenceProvider = std::function<QStringList(const QString&)>;
    struct DeleteReferenceResult { QStringList removals; QStringList blockers; };
    using DeleteReferenceProvider = std::function<bool(const QString&, DeleteReferenceResult&, QString*)>;
    using ParameterOwner = live2dstruct::CMocParameterStructureBuilder::Owner;
    explicit CParamPanel(QWidget *parent = nullptr);
    ~CParamPanel() override;
    void SetPreEditGuard(PreEditGuard guard);
    void SetMetadataEditHandler(MetadataEditHandler handler);
    void SetHistoryHandler(HistoryHandler handler);
    void SetStructureEditHandler(StructureEditHandler handler);
    void SetParameterReferenceProvider(ParameterReferenceProvider provider);
    void SetDeleteReferenceProvider(DeleteReferenceProvider provider);
    void SetBindingTargets(const QVector<ParameterOwner>& targets);
    void SetDocument(live2dstruct::CModelDocument* document);
    void ApplyExternalValue(const QString& paramId, float value, bool record = false, bool userEdit = true, bool writeDocument = true);
    void SetPreviewValues(const QMap<QString, float>& values, const QMap<QString, float>& baselineValues);
    void ClearPreviewValues(const QMap<QString, float>& restoredValues = {}, const QString& preservedParameterId = {});
    bool HasPreviewValues() const { return m_previewActive; }
    void BuildParams(CCidJson* cid = nullptr, const QSet<QString>* blendShapeParams = nullptr);
    void ReconcileParameters(CCidJson* cid = nullptr, const QSet<QString>* blendShapeParams = nullptr);
    void RefreshParameterMetadata();
    bool FocusParameter(const QString& id);
    QMap<QString, float> CurrentValues() const;
    bool CurrentValue(const QString& id, float& value) const;

signals:
    void ParamChanged(const QString& name, float value, bool isInit);
    void HistoryChanged(bool canUndo, bool canRedo);
    void EditRejected(const QString& error);
    void ParameterMetadataChanged();
    void StructureChanged();

public slots:
    void ResetAll();
    void SyncFromModel();
    void Undo();
    void Redo();
    void ClearHistory();
    void SyncHistoryState();
    void ApplyValues(const QMap<QString, float>& values);
    void CopyValues();
    void PasteValues();
    void SavePreset();
    void LoadPreset();
    void EditParameterMetadata();
    void CreateParameter();
    void BindParameter();
    void DeleteParameter();

private:
    friend class ParamPanelTests;
    struct ParamRow {
        QWidget* widget = nullptr;
        QSlider* slider;
        QDoubleSpinBox* spinBox;
        QToolButton* resetButton;
        QLabel* nameLabel = nullptr;
        QLabel* valueLabel;
        float minVal;
        float maxVal;
        float defaultVal;
        QString paramId;
        QString originalName;
        int modelIndex = -1;
        bool blendShape = false;
    };

    QScrollArea* m_scroll;
    QWidget* m_container;
    QLineEdit* m_search;
    QComboBox* m_xyXSelector = nullptr;
    QComboBox* m_xyYSelector = nullptr;
    CXYParameterPad* m_xyPad = nullptr;
    QGroupBox* m_xyBox = nullptr;
    bool m_updatingXY = false;
    bool m_checkingEditGuard = false;
    bool m_editGuardApproved = false;
    PreEditGuard m_preEditGuard;
    MetadataEditHandler m_metadataEditHandler;
    HistoryHandler m_historyHandler;
    StructureEditHandler m_structureEditHandler;
    ParameterReferenceProvider m_parameterReferenceProvider;
    DeleteReferenceProvider m_deleteReferenceProvider;
    QVector<ParameterOwner> m_bindingTargets;
    quint64 m_bindingSelectionRevision = 0;
    std::shared_ptr<CCidJson> m_cachedCid;
    QSet<QString> m_cachedBlendIds;
    QToolButton* m_createParameter = nullptr;
    QToolButton* m_bindParameter = nullptr;
    QToolButton* m_deleteParameter = nullptr;
    bool m_metadataDialogOpen = false;
    bool m_submittingEdit = false;
    QSet<int> m_rejectedSpinEdits;
    QVector<ParamRow> m_rows;
    QHash<QWidget*, QToolButton*> m_groupButtons;
    QSet<int> m_userDragging;
    QSet<int> m_changedDrags;
    QSet<int> m_userLocked; // 用户手动修改过的参数，不再被 sync 覆盖
    QHash<QString, float> m_lastValues;
    QMap<QString, float> m_previewBaseline;
    bool m_previewActive = false;
    QHash<int, float> m_dragStarts;
    live2dstruct::CModelDocument* m_document = nullptr;

    bool ApplyValue(int rowIndex, float value, bool record, bool preserveSpinText = false);
    bool AllowUserEdit();
    void RestoreControls();
    void BeginParameterDrag(int rowIndex);
    void EndParameterDrag(int rowIndex);
    float ValueBeforeEdit(const QString& paramId, float fallback) const;
    void ApplyValuesInternal(const QMap<QString, float>& values, bool record);
    bool PushEdit(const QString& paramId, float before, float after, bool notify = true);
    void EmitHistoryState();
    void ApplyHistory(bool undo);
    void RefreshAcceptedMetadata(const live2dstruct::CModelEdit& edit, bool forward);
    void RefreshParameterRow(int rowIndex);
    void RefreshXYControls();
    void ApplyXYPad(float xValue, float yValue);
    void BuildParameterRows(CCidJson* cid, const QSet<QString>* blendShapeParams, bool resetHistory);
    void FilterParameters();
    enum class StructureAction { Create, Bind, Delete };
    void EditParameterStructure(StructureAction action);
    void UpdateStructureActions();
};

#endif // CPARAMPANEL_H
