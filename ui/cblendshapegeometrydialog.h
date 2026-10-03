#ifndef CBLENDSHAPEGEOMETRYDIALOG_H
#define CBLENDSHAPEGEOMETRYDIALOG_H

#include "live2dstruct/cmodelgeometryedit.h"
#include <QDialog>
#include <functional>

class CBlendShapeGeometryTableModel;
class QLabel;
class QPushButton;
class QTableView;

class CBlendShapeGeometryDialog : public QDialog
{
    Q_OBJECT
public:
    using Submit = std::function<bool(const QVector<float>&, QString*)>;
    explicit CBlendShapeGeometryDialog(const live2dstruct::CModelBlendShapeGeometryEntry& before,
                                      QWidget* parent = nullptr);
    void SetSubmissionHandler(Submit handler);
    const QVector<float>& Deltas() const;
    QString Error() const;
    bool IsSubmitting() const { return m_submitting; }

public slots:
    void done(int result) override;

private:
    void Refresh();
    void Reset();
    CBlendShapeGeometryTableModel* m_model = nullptr;
    QTableView* m_table = nullptr;
    QLabel* m_error = nullptr;
    QPushButton* m_save = nullptr;
    QPushButton* m_reset = nullptr;
    Submit m_submit;
    bool m_submitting = false;
    bool m_accepted = false;
};

#endif
