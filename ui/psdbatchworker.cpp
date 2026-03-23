#include "psdbatchworker.h"
#include <QFileInfo>
#include <QDir>

PsdBatchWorker::PsdBatchWorker(QObject *parent)
    : QObject(parent)
{}

void PsdBatchWorker::setTask(const QStringList &models, const QString &outDir, bool all, bool fenzu, bool texExtract)
{
    m_models = models;
    m_outDir = outDir;
    m_all = all;
    m_fenzu = fenzu;
    m_texExtract = texExtract;
    m_index = 0;
    m_cancelled = false;
}

void PsdBatchWorker::start()
{
    if (m_cancelled || m_index >= m_models.size()) {
        emit finished();
        return;
    }
    const QString& modelPath = m_models[m_index];
    QFileInfo info(modelPath);
    const QString psdName = info.completeBaseName() + ".psd";
    const QString outPath = QDir(m_outDir).filePath(psdName);
    emit progress(m_index + 1, m_models.size(), info.fileName());
    emit requestExport(modelPath, outPath, m_all, m_fenzu, m_texExtract);
}

void PsdBatchWorker::cancel()
{
    m_cancelled = true;
}

void PsdBatchWorker::onOneExportFinished()
{
    if (m_cancelled) {
        emit finished();
        return;
    }
    ++m_index;
    start();
}

