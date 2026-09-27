#ifndef CPARAMETERORGANIZATIONDIALOG_H
#define CPARAMETERORGANIZATIONDIALOG_H

#include <QDialog>
#include <QHash>
#include <functional>
#include "live2dstruct/cparameterorganization.h"

class CCidJson;
class QTreeWidget;
class QTreeWidgetItem;
class QTableWidget;
class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QDialogButtonBox;
class CTableEditDelegate;
namespace live2dstruct { class CModelDocument; }

class CParameterOrganizationDialog : public QDialog
{
    Q_OBJECT
public:
    using SubmissionHandler = std::function<bool(const live2dstruct::CParameterOrganization::State&, QString*)>;
    explicit CParameterOrganizationDialog(const live2dstruct::CModelDocument& document,
                                           const CCidJson* displayInfo, QWidget* parent = nullptr);
    static live2dstruct::CParameterOrganization::State InitialState(
        const live2dstruct::CModelDocument& document, const CCidJson* displayInfo);
    const live2dstruct::CParameterOrganization::State& Result() const { return m_result; }
    void SetSubmissionHandler(SubmissionHandler handler);
    void accept() override;
    void reject() override;

private:
    using Organization = live2dstruct::CParameterOrganization;
    QTreeWidget* m_tree = nullptr;
    QTableWidget* m_pairs = nullptr;
    QComboBox* m_destination = nullptr;
    QComboBox* m_first = nullptr;
    QComboBox* m_second = nullptr;
    QCheckBox* m_reversed = nullptr;
    QCheckBox* m_enabled = nullptr;
    QLabel* m_error = nullptr;
    QLabel* m_pairStatus = nullptr;
    QPushButton* m_discardPair = nullptr;
    QPushButton* m_newPair = nullptr;
    QPushButton* m_storePair = nullptr;
    QPushButton* m_removePair = nullptr;
    QWidget* m_content = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    CTableEditDelegate* m_treeDelegate = nullptr;
    SubmissionHandler m_submission;
    int m_pairRow = -1;
    bool m_loadingPair = false;
    bool m_submitting = false;
    QString m_pairFirstText, m_pairSecondText;
    bool m_pairReversed = false, m_pairEnabled = true;
    QSet<QString> m_parameterIds;
    QHash<QString, QString> m_names;
    Organization::State m_result;
    void RefreshFolders();
    void AddFolder();
    void DissolveFolders();
    void MoveSelected(int direction);
    void MoveParameters();
    void SetLabelColor(bool reset);
    void StorePair();
    void SelectPair();
    bool PairDraftChanged() const;
    bool CommitPairDraft(QString* error, bool selectStored);
    void LoadPair(int row);
    void StartNewPair();
    void RemovePair();
    void RefreshPairDraft();
    bool ReadState(Organization::State& state, QString* error) const;
    void SetNodeDecoration(QTreeWidgetItem* item);
};

#endif
