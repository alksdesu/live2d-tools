#ifndef CPSDBATCHDIALOG_H
#define CPSDBATCHDIALOG_H

#include <QDialog>
#include <QListWidget>
#include <QCheckBox>
#include <QPushButton>
#include <QStringList>

class CPsdBatchDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CPsdBatchDialog(QWidget* parent = nullptr);
    void setFileList(const QStringList& files);
    QStringList getSelectedFiles() const;

    bool isAllChecked() const;
    bool isFenzuChecked() const;
    bool isTexExtractChecked() const;

private slots:
    void onSelectAll();
    void onDeselectAll();
    void onInvertSelect();
    void onAccept();
    void onReject();

private:
    QListWidget* m_list{nullptr};
    QCheckBox* m_checkAll{nullptr};
    QCheckBox* m_checkFenzu{nullptr};
    QCheckBox* m_checkTexExtract{nullptr};
    QPushButton* m_btnAll{nullptr};
    QPushButton* m_btnNone{nullptr};
    QPushButton* m_btnInvert{nullptr};
};

#endif // CPSDBATCHDIALOG_H

