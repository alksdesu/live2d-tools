#ifndef CMODELDIAGNOSTICSPANEL_H
#define CMODELDIAGNOSTICSPANEL_H

#include <QDockWidget>

namespace live2dstruct {
class CModelDocument;
}

class QLabel;
class QListWidget;

class CModelDiagnosticsPanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CModelDiagnosticsPanel(QWidget* parent = nullptr);
    void SetDocument(const live2dstruct::CModelDocument* document);

public slots:
    void Refresh();

private:
    const live2dstruct::CModelDocument* m_document = nullptr;
    QLabel* m_summary = nullptr;
    QListWidget* m_list = nullptr;
};

#endif
