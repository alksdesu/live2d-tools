#ifndef CEXPORTCONTROL_H
#define CEXPORTCONTROL_H

#include <QString>
#include <functional>
#include <memory>
#include <mutex>

class CExportControl
{
public:
    enum class Phase { Preparing, Rendering, Extracting, Encoding, Writing, Committing };
    struct Progress {
        Phase phase = Phase::Preparing;
        qint64 current = 0;
        qint64 total = 0;
        QString detail;
    };
    using Handler = std::function<bool(const Progress&, QString*)>;

    void Cancel() noexcept;
    bool IsCancellationRequested() const;
    bool WasCanceled() const;
    bool IsCommitted() const;
    bool Checkpoint();
    bool Report(Phase phase, qint64 current = 0, qint64 total = 0, const QString& detail = {});
    bool BeginCommit();
    void CommitFinished(bool success, const QString& error = {});
    void Fail(const QString& error);
    QString Error() const;
    Progress Snapshot() const;
    bool SetHandler(Handler handler);

private:
    enum class State { Active, Canceled, Committing, Committed, Failed };
    mutable std::mutex m_mutex;
    bool m_cancelRequested = false;
    bool m_inHandler = false;
    State m_state = State::Active;
    Progress m_progress;
    QString m_error;
    std::shared_ptr<Handler> m_handler;
};

#endif
