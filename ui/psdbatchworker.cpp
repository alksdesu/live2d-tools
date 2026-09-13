#include "psdbatchworker.h"
#include <QFileInfo>
#include <QDir>
#include <QHash>
#include <QSet>

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
    m_outputNames.clear();
    QHash<QString, int> nameCounts;
    QSet<QString> reservedNamesFolded;
    const QDir outputDirectory(m_outDir);
    const QStringList existingFiles = outputDirectory.entryList(QDir::Files);
    for (const QString& file : existingFiles) reservedNamesFolded.insert(file.toCaseFolded());
    for (const QString& model : m_models) {
        QString baseName = QFileInfo(model).completeBaseName();
        if (baseName.endsWith(QStringLiteral(".model3"), Qt::CaseInsensitive))
            baseName.chop(QStringLiteral(".model3").size());
        if (baseName.isEmpty()) baseName = QStringLiteral("model");
        int count = nameCounts.value(baseName, 0);
        QString candidate;
        do {
            ++count;
            candidate = count == 1 ? baseName + QStringLiteral(".psd")
                                   : QStringLiteral("%1 #%2.psd").arg(baseName).arg(count);
        } while (reservedNamesFolded.contains(candidate.toCaseFolded()));
        nameCounts.insert(baseName, count);
        reservedNamesFolded.insert(candidate.toCaseFolded());
        m_outputNames.append(candidate);
    }
}

void PsdBatchWorker::start()
{
    if (m_cancelled || m_index >= m_models.size()) {
        emit finished(m_failedFiles);
        return;
    }
    const QString& modelPath = m_models[m_index];
    QFileInfo info(modelPath);
    const QString psdName = m_outputNames.value(m_index, info.completeBaseName() + QStringLiteral(".psd"));
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
