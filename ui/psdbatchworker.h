#ifndef PSDBATCHWORKER_H
#define PSDBATCHWORKER_H

#include <QObject>
#include <QStringList>
#include "psdbatchresult.h"

class PsdBatchWorker : public QObject
{
    Q_OBJECT
public:
    explicit PsdBatchWorker(QObject* parent = nullptr);
    void setTask(const QStringList& models, const QString& outDir, bool all, bool fenzu, bool texExtract = false, bool cubismCompat = false);
    PsdBatchResults plannedResults() const { return m_plan; }

public slots:
    void start();
    void cancel();
    void onOneExportFinished(bool ok, const QString& outPath);
    void skipCurrent(const QString& outPath);

signals:
    void requestExport(const QString& modelPath, const QString& outPath, bool isAll, bool isFenzu, bool isTexExtract, bool cubismCompat);
    void progress(int current, int total, const QString& currentName);
    void finished(const QStringList& failedFiles);

private:
    void requestNext();
    void finish();

    QStringList m_models;
    QString m_outDir;
    bool m_all = false;
    bool m_fenzu = false;
    bool m_texExtract = false;
    bool m_cubismCompat = false;
    int m_index = 0;
    bool m_cancelled = false;
    bool m_started = false;
    bool m_finished = false;
    QString m_pendingOutput;
    QStringList m_failedFiles;
    PsdBatchResults m_plan;
};

#endif // PSDBATCHWORKER_H
