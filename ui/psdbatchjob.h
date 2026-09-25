#ifndef PSDBATCHJOB_H
#define PSDBATCHJOB_H

#include <QObject>
#include <QStringList>
#include <QThread>

class PsdBatchWorker;

class PsdBatchJob : public QObject
{
    Q_OBJECT
public:
    explicit PsdBatchJob(QObject* parent = nullptr);
    ~PsdBatchJob() override;

    bool start(const QStringList& models, const QString& outDir, bool all, bool fenzu,
               bool texExtract = false, bool cubismCompat = false);
    bool isRunning() const;

public slots:
    void cancel();
    void complete(bool ok, const QString& outPath);
    void shutdown();

signals:
    void progress(int current, int total, const QString& currentName);
    void requestExport(const QString& modelPath, const QString& outPath, bool isAll,
                       bool isFenzu, bool isTexExtract, bool cubismCompat);
    void finished(const QStringList& failedFiles, bool canceled);

private:
    enum class State { Idle, Running, Finished, Shutdown };
    void acknowledge(bool ok, const QString& outPath);
    void stopThread();

    QThread m_thread;
    PsdBatchWorker* m_worker = nullptr;
    State m_state = State::Idle;
    bool m_canceled = false;
    QString m_pendingOutput;
};

#endif // PSDBATCHJOB_H
