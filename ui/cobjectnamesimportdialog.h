#ifndef COBJECTNAMESIMPORTDIALOG_H
#define COBJECTNAMESIMPORTDIALOG_H

#include <QDialog>
#include <QByteArray>
#include <QStringList>
#include <QVector>
#include <functional>

class QAbstractItemModel;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSortFilterProxyModel;
class QTableView;

class CObjectNamesImportDialog : public QDialog
{
    Q_OBJECT
public:
    struct Row {
        QString type, id, currentName, sourceName, status, reason;
        QStringList evidence;
        bool eligible = false;
    };
    struct Context {
        QString sourcePath;
        QByteArray sourceDigest;
        QVector<Row> rows;
        QStringList diagnostics;
    };
    struct Selection { QString type, id, name; };
    using SubmissionHandler = std::function<bool(const QVector<Selection>&, QString*)>;
    explicit CObjectNamesImportDialog(const Context& context, QWidget* parent = nullptr);
    void SetSubmissionHandler(SubmissionHandler handler);
    bool IsSubmitting() const { return m_submitting; }
    QString Error() const { return m_error; }

public slots:
    bool Submit();
    void done(int result) override;

private:
    void RefreshControls();
    void RefreshDetails();
    void SetError(const QString& error);
    Context m_context;
    SubmissionHandler m_handler;
    QAbstractItemModel* m_model = nullptr;
    QSortFilterProxyModel* m_filter = nullptr;
    QTableView* m_table = nullptr;
    QLineEdit* m_search = nullptr;
    QPlainTextEdit* m_details = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_apply = nullptr;
    QPushButton* m_cancel = nullptr;
    QPushButton* m_fill = nullptr;
    QPushButton* m_keep = nullptr;
    QString m_error;
    bool m_submitting = false;
    bool m_valid = false;
    bool m_completed = false;
};

#endif
