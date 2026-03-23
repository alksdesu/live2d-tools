#ifndef CPSDPRINTDIALOG_H
#define CPSDPRINTDIALOG_H

#include <QDialog>
#include <QString>
#include <QLabel>
#include <QCheckBox>
#include <QPushButton>

class CPsdPrintDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CPsdPrintDialog(QWidget *parent = nullptr);
    ~CPsdPrintDialog();
    void SetName(const QString& name);
    bool m_IsPrint;
    bool m_IsFenZu;
    bool m_IsAll;
    bool m_IsTexExtract;
    bool m_IsSnapshot;

private slots:
    void on_pushButton_clicked();   // 确定
    void on_pushButton_2_clicked(); // 取消

private:
    QLabel* m_pathLabel{nullptr};
    QCheckBox* m_checkAll{nullptr};
    QCheckBox* m_checkFenzu{nullptr};
    QCheckBox* m_checkTexExtract{nullptr};
    QCheckBox* m_checkSnapshot{nullptr};
    QPushButton* m_okBtn{nullptr};
    QPushButton* m_cancelBtn{nullptr};
};

#endif // CPSDPRINTDIALOG_H
