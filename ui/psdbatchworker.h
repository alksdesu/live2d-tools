#ifndef PSDBATCHWORKER_H
#define PSDBATCHWORKER_H

#include <QObject>
#include <QStringList>

class PsdBatchWorker : public QObject
{
    Q_OBJECT
public:
    explicit PsdBatchWorker(QObject* parent = nullptr);
    void setTask(const QStringList& models, const QString& outDir, bool all, bool fenzu, bool texExtract = false, bool cubismCompat = false);

public slots:
    void start();
    void cancel();
    void onOneExportFinished(bool ok, const QString& outPath);

signals:
    void requestExport(const QString& modelPath, const QString& outPath, bool isAll, bool isFenzu, bool isTexExtract, bool cubismCompat);
    void progress(int current, int total, const QString& currentName);
    void finished(const QStringList& failedFiles);

private:
    QStringList m_models;
    QString m_outDir;
    bool m_all = false;
    bool m_fenzu = false;
    bool m_texExtract = false;
    bool m_cubismCompat = false;
    int m_index = 0;
    bool m_cancelled = false;
    QStringList m_failedFiles;
};

#endif // PSDBATCHWORKER_H

