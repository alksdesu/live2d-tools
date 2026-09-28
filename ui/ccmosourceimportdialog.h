#ifndef CCMOSOURCEIMPORTDIALOG_H
#define CCMOSOURCEIMPORTDIALOG_H

#include "cmo3/ccmo3project.h"
#include "live2dstruct/cmodelartwork.h"
#include <QDialog>
#include <QHash>
#include <QSet>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSplitter;
class QTreeWidget;
class QTreeWidgetItem;

class CCmoSourceImportDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CCmoSourceImportDialog(const cmo3::CCmo3Project& project, QWidget* parent = nullptr);
    QStringList SelectedDocumentIds() const;
    live2dstruct::CModelArtwork::ColorPolicy ColorPolicy() const;
    void accept() override;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void SelectAll(bool selected);
    void ShowSelection();
    void Refresh();
    QString Relations(const QString& documentId, const QString& layerId) const;
    cmo3::CCmo3Project m_project;
    QHash<QString, QVector<QPair<int, int>>> m_meshInputs;
    QSet<QString> m_available;
    QTreeWidget* m_tree = nullptr;
    QPlainTextEdit* m_details = nullptr;
    QComboBox* m_color = nullptr;
    QLabel* m_status = nullptr;
    QSplitter* m_splitter = nullptr;
    QPushButton* m_import = nullptr;
};

#endif
