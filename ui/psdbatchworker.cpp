#include "psdbatchworker.h"
#include <QFileInfo>
#include <QDir>
#include <QHash>
#include <QSet>

namespace {
QString nameKey(const QString& name)
{
    return name.normalized(QString::NormalizationForm_C).toCaseFolded().normalized(QString::NormalizationForm_C);
}
}

PsdBatchWorker::PsdBatchWorker(QObject *parent)
    : QObject(parent)
{}

void PsdBatchWorker::setTask(const QStringList &models, const QString &outDir, bool all, bool fenzu, bool texExtract, bool cubismCompat)
{
    if (m_started || m_finished) return;
    m_models = models;
    m_outDir = outDir;
    m_all = all;
    m_fenzu = fenzu;
    m_texExtract = texExtract;
    m_cubismCompat = cubismCompat;
    m_index = 0;
    m_cancelled = false;
    m_failedFiles.clear();
    m_plan.clear();
    QHash<QString, int> nameCounts;
    QSet<QString> reservedNamesFolded;
    const QDir outputDirectory(m_outDir);
    const QStringList existingFiles = outputDirectory.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QString& file : existingFiles) reservedNamesFolded.insert(nameKey(file));
    for (const QString& model : m_models) {
        QString baseName = QFileInfo(model).completeBaseName();
        if (baseName.endsWith(QStringLiteral(".model3"), Qt::CaseInsensitive))
            baseName.chop(QStringLiteral(".model3").size());
        if (baseName.isEmpty()) baseName = QStringLiteral("model");
        const QString key = nameKey(baseName);
        int count = nameCounts.value(key, 0);
        QString candidate;
        do {
            ++count;
            candidate = count == 1 ? baseName + QStringLiteral(".psd")
                                   : QStringLiteral("%1 #%2.psd").arg(baseName).arg(count);
        } while (reservedNamesFolded.contains(nameKey(candidate)));
        nameCounts.insert(key, count);
        reservedNamesFolded.insert(nameKey(candidate));
        PsdBatchFileResult result;
        result.index = m_plan.size();
        result.sourcePath = model;
        result.outputPath = outputDirectory.filePath(candidate);
        m_plan.append(result);
    }
}

void PsdBatchWorker::start()
{
    if (m_started || m_finished) return;
    m_started = true;
    requestNext();
}

void PsdBatchWorker::requestNext()
{
    if (m_cancelled || m_index >= m_models.size()) {
        finish();
        return;
    }
    const QString& modelPath = m_models[m_index];
    QFileInfo info(modelPath);
    const QString outPath = m_plan.at(m_index).outputPath;
    m_pendingOutput = outPath;
    emit progress(m_index + 1, m_models.size(), info.fileName());
    emit requestExport(modelPath, outPath, m_all, m_fenzu, m_texExtract, m_cubismCompat);
}

void PsdBatchWorker::cancel()
{
    if (m_finished) return;
    m_cancelled = true;
    if (m_pendingOutput.isEmpty()) finish();
}

void PsdBatchWorker::onOneExportFinished(bool ok, const QString& outPath)
{
    if (m_finished || m_pendingOutput.isEmpty() || outPath != m_pendingOutput) return;
    m_pendingOutput.clear();
    if (!ok) {
        m_failedFiles.append(outPath);
    }
    ++m_index;
    requestNext();
}

void PsdBatchWorker::skipCurrent(const QString& outPath)
{
    if (m_finished || m_pendingOutput.isEmpty() || outPath != m_pendingOutput) return;
    m_pendingOutput.clear();
    ++m_index;
    requestNext();
}

void PsdBatchWorker::finish()
{
    if (m_finished) return;
    m_finished = true;
    emit finished(m_failedFiles);
}
