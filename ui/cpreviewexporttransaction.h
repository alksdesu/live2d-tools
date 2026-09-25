#ifndef CPREVIEWEXPORTTRANSACTION_H
#define CPREVIEWEXPORTTRANSACTION_H

#include <QString>
#include <functional>

class CPreviewExportTransaction
{
public:
    using Step = std::function<bool(QString*)>;
    struct Steps {
        Step pause;
        Step beginTemporary;
        Step exportFile;
        Step endTemporary;
        Step restorePreview;
        Step resume;
    };

    CPreviewExportTransaction() = default;
    ~CPreviewExportTransaction();
    CPreviewExportTransaction(const CPreviewExportTransaction&) = delete;
    CPreviewExportTransaction& operator=(const CPreviewExportTransaction&) = delete;

    bool Run(Steps steps, QString* error = nullptr);
    bool Recover(QString* error = nullptr);
    bool IsBusy() const;
    bool NeedsRecovery() const;
    bool ResetAfterPreviewClosed();

private:
    enum class RecoveryStage { None, EndTemporary, RestorePreview, Resume };
    static bool Invoke(const Step& step, const QString& stage, QString* error);
    static QString CombinedError(const QString& operation, const QString& recovery);
    bool Restore(QString* error);
    void Clear();

    Steps m_steps;
    RecoveryStage m_recoveryStage = RecoveryStage::None;
    QString m_operationError;
    bool m_executing = false;
    bool m_needsRecovery = false;
};

#endif
