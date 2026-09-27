#ifndef CMODELDIAGNOSTICSPANEL_H
#define CMODELDIAGNOSTICSPANEL_H

#include "live2dstruct/cmodeldiagnostic.h"
#include <QDockWidget>
#include <QJsonObject>
#include <QVector>

namespace live2dstruct {
class CModelDocument;
}

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

class CModelDiagnosticsPanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CModelDiagnosticsPanel(QWidget* parent = nullptr);
    void SetDocument(const live2dstruct::CModelDocument* document);
    QJsonObject ReportJson() const;

public slots:
    void Refresh();
    void ExportReport();

signals:
    void ObjectRequested(const QString& id, const QString& type);

protected:
    void changeEvent(QEvent* event) override;

private:
    void RefreshSelection();
    void RefreshAppearance();
    bool CanLocate(int row) const;
    void LocateSelection();
    const live2dstruct::CModelDocument* m_document = nullptr;
    QVector<live2dstruct::CModelDiagnostic> m_diagnostics;
    QByteArray m_source;
    QString m_sourcePath;
    quint64 m_revision = 0;
    QLabel* m_summary = nullptr;
    QListWidget* m_list = nullptr;
    QPlainTextEdit* m_details = nullptr;
    QPushButton* m_locate = nullptr;
};

#endif
