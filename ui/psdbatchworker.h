#ifndef PSDBATCHWORKER_H
#define PSDBATCHWORKER_H

#include <QObject>
#include <QStringList>

class PsdBatchWorker : public QObject
{
    Q_OBJECT
public:
    explicit PsdBatchWorker(QObject* parent = nullptr);
    void setTask(const QStringList& models, const QString& outDir, bool all, bool fenzu, bool texExtract = false);

public slots:
    void start();
    void cancel();
    void onOneExportFinished();

signals:
    void requestExport(const QString& modelPath, const QString& outPath, bool isAll, bool isFenzu, bool isTexExtract);
    void progress(int current, int total, const QString& currentName);
    void finished();

private:
    QStringList m_models;
    QString m_outDir;
    bool m_all = false;
    bool m_fenzu = false;
    bool m_texExtract = false;
    int m_index = 0;
    bool m_cancelled = false;
};

#endif // PSDBATCHWORKER_H

