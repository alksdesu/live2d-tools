#ifndef PSDBATCHJOB_H
#define PSDBATCHJOB_H

#include <QObject>
#include <QStringList>
#include <QThread>
#include <QElapsedTimer>
#include <QHash>
#include "psdbatchresult.h"

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
    PsdBatchResults results() const { return m_results; }

public slots:
    void cancel();
    void complete(bool ok, const QString& outPath);
    void completeDetailed(bool ok, const QString& outPath, const QString& error,
                          PsdExportStage stage = PsdExportStage::Exporting, bool outputCommitted = false);
    void completeCanceled(const QString& outPath, PsdExportStage stage = PsdExportStage::Exporting,
                          const QString& reason = {});
    void setStage(const QString& outPath, PsdExportStage stage);
    void shutdown();

signals:
    void progress(int current, int total, const QString& currentName);
    void requestExport(const QString& modelPath, const QString& outPath, bool isAll,
                       bool isFenzu, bool isTexExtract, bool cubismCompat);
    void finished(const QStringList& failedFiles, bool canceled);
    void fileResultChanged(int index, const PsdBatchFileResult& result);
    void resultsReady(const PsdBatchResults& results, bool cancelRequested);
    void cancelCurrentRequested();

private:
    enum class State { Idle, Running, Finished, Shutdown };
    void acknowledge(bool ok, const QString& outPath);
    void skip(const QString& outPath);
    bool publishResult(int index);
    void clearPending();
    void stopThread();

    QThread m_thread;
    PsdBatchWorker* m_worker = nullptr;
    State m_state = State::Idle;
    bool m_canceled = false;
    QString m_pendingOutput;
    int m_pendingIndex = -1;
    bool m_exportDispatched = false;
    PsdBatchResults m_results;
    QHash<QString, int> m_outputIndices;
    QElapsedTimer m_elapsed;
};

#endif // PSDBATCHJOB_H
