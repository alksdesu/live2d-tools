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
#include "live2dstruct/cparametervalueeditcontroller.h"

class CCidJson;
class QToolButton;
class QComboBox;
class QGroupBox;
class CXYParameterPad;
class QCheckBox;
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
    using ValueController = live2dstruct::CParameterValueEditController;
    void SetValuePublisher(ValueController::Publisher publisher);
    void SetValueExpander(ValueController::Expander expander);
    void SetValueBaselineProvider(ValueController::BaselineProvider provider);
    void SetValueContextDigestProvider(ValueController::ContextDigestProvider provider);
    bool ApplyUserValues(const QMap<QString, float>& values, QString* error = nullptr);
    bool BeginUserValueGesture(const QMap<QString, float>& drivers, QString* error = nullptr);
    bool PreviewUserValueGesture(const QMap<QString, float>& drivers, QString* error = nullptr);
    bool FinishUserValueGesture(QString* error = nullptr);
    bool CancelUserValueGesture(QString* error = nullptr);
    bool HasUserValueGesture() const;
    void SetPreEditGuard(PreEditGuard guard);
    void SetMetadataEditHandler(MetadataEditHandler handler);
    void SetHistoryHandler(HistoryHandler handler);
    void SetStructureEditHandler(StructureEditHandler handler);
    void SetParameterReferenceProvider(ParameterReferenceProvider provider);
    void SetDeleteReferenceProvider(DeleteReferenceProvider provider);
    void SetBindingTargets(const QVector<ParameterOwner>& targets);
    void SetDocument(live2dstruct::CModelDocument* document);
    void SetOrganizationEditHandler(MetadataEditHandler handler);
    void SetRelatedParameterIds(const QSet<QString>& ids, const QString& detail = {});
    void RefreshOrganization();
    void EditOrganization();
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
    void UserValuesApplied(const QMap<QString, float>& values);
    void UserValuesEdited(const QMap<QString, float>& values);
    void HistoryChanged(bool canUndo, bool canRedo);
    void EditRejected(const QString& error);
    void ParameterMetadataChanged();
    void StructureChanged();
    void OrganizationChanged();

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

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

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
    QCheckBox* m_onlyRelated = nullptr;
    QLabel* m_relatedStatus = nullptr;
    QSet<QString> m_relatedParameterIds;
    QString m_relatedDetail;
    MetadataEditHandler m_organizationEditHandler;
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

    ValueController m_valueController;
    struct ValueUiSnapshot {
        QMap<QString, float> values;
        QMap<QString, float> previewBaseline;
        QSet<int> userLocked;
        bool previewActive = false;
    };
    std::optional<ValueUiSnapshot> m_valueUiSnapshot;
    quint64 m_valueGestureSerial = 0;
    bool m_hasValuePublisher = false;
    bool m_applyingValueRequest = false;
    bool ApplyValueRequest(const ValueController::Request& request, QString* error);
    void SyncAcceptedValues(const QMap<QString, float>& values, bool notify);
    void InstallValueEventFilters();
    bool ApplyValue(int rowIndex, float value, bool record, bool preserveSpinText = false);
    bool AllowUserEdit(QString* error = nullptr);
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
