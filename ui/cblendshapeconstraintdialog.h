#ifndef CBLENDSHAPECONSTRAINTDIALOG_H
#define CBLENDSHAPECONSTRAINTDIALOG_H

#include "live2dstruct/cmodelblendshapeconstraintedit.h"
#include <QDialog>
#include <functional>

class CBlendShapeConstraintTableModel;
class QLabel;
class QPushButton;
class QTableView;

class CBlendShapeConstraintDialog : public QDialog
{
    Q_OBJECT
public:
    using Submit = std::function<bool(const QVector<live2dstruct::CMocBlendShapeConstraintBuilder::Value>&, QString*)>;
    explicit CBlendShapeConstraintDialog(const live2dstruct::CModelBlendShapeConstraintEntry& before,
                                         QWidget* parent = nullptr);
    void SetSubmissionHandler(Submit handler);
    const QVector<live2dstruct::CMocBlendShapeConstraintBuilder::Value>& Values() const;
    QString Error() const;

public slots:
    void done(int result) override;

private:
    void Refresh();
    void Reset();
    CBlendShapeConstraintTableModel* m_model = nullptr;
    QTableView* m_table = nullptr;
    QLabel* m_error = nullptr;
    QPushButton* m_save = nullptr;
    QPushButton* m_reset = nullptr;
    Submit m_submit;
    bool m_submitting = false;
    bool m_accepted = false;
};

#endif
