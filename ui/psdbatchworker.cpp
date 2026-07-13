#include "psdbatchworker.h"
#include <QFileInfo>
#include <QDir>

PsdBatchWorker::PsdBatchWorker(QObject *parent)
    : QObject(parent)
{}

void PsdBatchWorker::setTask(const QStringList &models, const QString &outDir, bool all, bool fenzu, bool texExtract, bool cubismCompat)
{
    m_models = models;
    m_outDir = outDir;
    m_all = all;
    m_fenzu = fenzu;
    m_texExtract = texExtract;
    m_cubismCompat = cubismCompat;
    m_index = 0;
    m_cancelled = false;
    m_failedFiles.clear();
}

void PsdBatchWorker::start()
{
    if (m_cancelled || m_index >= m_models.size()) {
        emit finished(m_failedFiles);
        return;
    }
    const QString& modelPath = m_models[m_index];
    QFileInfo info(modelPath);
    const QString psdName = info.completeBaseName() + ".psd";
    const QString outPath = QDir(m_outDir).filePath(psdName);
    emit progress(m_index + 1, m_models.size(), info.fileName());
    emit requestExport(modelPath, outPath, m_all, m_fenzu, m_texExtract, m_cubismCompat);
}

void PsdBatchWorker::cancel()
{
    m_cancelled = true;
}

void PsdBatchWorker::onOneExportFinished(bool ok, const QString& outPath)
{
    if (!ok) {
        m_failedFiles.append(outPath);
    }
    if (m_cancelled) {
        emit finished(m_failedFiles);
        return;
    }
    ++m_index;
    start();
}

