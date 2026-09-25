#include "psdbatchjob.h"
#include "psdbatchworker.h"
#include <QPointer>
#include <exception>
#include <limits>
#include <memory>

PsdBatchJob::PsdBatchJob(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<PsdBatchFileResult>();
    qRegisterMetaType<PsdBatchResults>();
}

PsdBatchJob::~PsdBatchJob() { shutdown(); }

bool PsdBatchJob::start(const QStringList& models, const QString& outDir, bool all, bool fenzu,
                        bool texExtract, bool cubismCompat)
{
    if (m_state != State::Idle || models.size() > std::numeric_limits<int>::max()) return false;
    std::unique_ptr<PsdBatchWorker> candidate;
    PsdBatchResults plan;
    QHash<QString, int> outputs;
    try {
        candidate = std::make_unique<PsdBatchWorker>();
        candidate->setTask(models, outDir, all, fenzu, texExtract, cubismCompat);
        plan = candidate->plannedResults();
        for (const auto& item : plan) outputs.insert(item.outputPath, item.index);
    } catch (const std::exception&) {
        return false;
    }
    m_worker = candidate.release();
    m_results = std::move(plan);
    m_outputIndices = std::move(outputs);
    m_state = State::Running;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::started, m_worker, &PsdBatchWorker::start);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &PsdBatchWorker::progress, this,
            [this](int current, int total, const QString& name) {
        if (m_state == State::Running && !m_canceled) emit progress(current, total, name);
    }, Qt::QueuedConnection);
    connect(m_worker, &PsdBatchWorker::requestExport, this,
            [this](const QString& model, const QString& output, bool all, bool fenzu,
                   bool texExtract, bool cubismCompat) {
        if (m_state != State::Running) return;
        if (m_canceled) { skip(output); return; }
        const int index = m_outputIndices.value(output, -1);
        if (m_pendingIndex >= 0 || index < 0 || m_results[index].state != PsdBatchFileState::Pending ||
            m_results[index].sourcePath != model) return;
        m_pendingOutput = output;
        m_pendingIndex = index;
        m_results[index].state = PsdBatchFileState::Running;
        m_results[index].stage = PsdExportStage::Preflight;
        if (!publishResult(index)) return;
        if (m_canceled) { clearPending(); skip(output); return; }
        m_exportDispatched = true;
        m_elapsed.start();
        emit requestExport(model, output, all, fenzu, texExtract, cubismCompat);
    }, Qt::QueuedConnection);
    connect(m_worker, &PsdBatchWorker::finished, this, [this](const QStringList&) {
        if (m_state != State::Running) return;
        m_state = State::Finished;
        clearPending();
        stopThread();
        const auto snapshot = m_results;
        QStringList failures;
        for (const auto& item : snapshot)
            if (item.state == PsdBatchFileState::Failed) failures.append(item.outputPath);
        const QPointer<PsdBatchJob> guard(this);
        emit resultsReady(snapshot, m_canceled);
        if (guard && m_state == State::Finished) emit finished(failures, m_canceled);
    }, Qt::QueuedConnection);
    m_thread.start();
    return true;
}

bool PsdBatchJob::isRunning() const { return m_thread.isRunning(); }

bool PsdBatchJob::publishResult(int index)
{
    const auto snapshot = m_results.at(index);
    const QPointer<PsdBatchJob> guard(this);
    emit fileResultChanged(index, snapshot);
    return guard && m_state == State::Running;
}

void PsdBatchJob::cancel()
{
    if (m_state != State::Running || m_canceled) return;
    m_canceled = true;
    QMetaObject::invokeMethod(m_worker, &PsdBatchWorker::cancel, Qt::QueuedConnection);
    QVector<int> changed;
    for (int index = 0; index < m_results.size(); ++index) {
        auto& result = m_results[index];
        if (result.state != PsdBatchFileState::Pending &&
            !(index == m_pendingIndex && result.state == PsdBatchFileState::Running && !m_exportDispatched)) continue;
        result.state = PsdBatchFileState::NotRun;
        result.stage = PsdExportStage::Queued;
        result.error = QStringLiteral("已取消，未开始导出");
        result.elapsedMs = 0;
        changed.append(index);
    }
    for (int index : changed) if (!publishResult(index)) return;
}

void PsdBatchJob::setStage(const QString& outPath, PsdExportStage stage)
{
    if (m_state != State::Running || !m_exportDispatched || m_pendingIndex < 0 || outPath != m_pendingOutput ||
        stage < PsdExportStage::Preflight || stage > PsdExportStage::Restoring) return;
    auto& result = m_results[m_pendingIndex];
    if (stage < result.stage) return;
    result.stage = stage;
    result.elapsedMs = m_elapsed.isValid() ? qMax(qint64(0), m_elapsed.elapsed()) : 0;
    publishResult(m_pendingIndex);
}

void PsdBatchJob::complete(bool ok, const QString& outPath)
{
    completeDetailed(ok, outPath, {}, PsdExportStage::Exporting, ok);
}

void PsdBatchJob::completeDetailed(bool ok, const QString& outPath, const QString& error,
                                   PsdExportStage stage, bool outputCommitted)
{
    if (m_state != State::Running || !m_exportDispatched || m_pendingIndex < 0 || outPath != m_pendingOutput) return;
    const int index = m_pendingIndex;
    auto& result = m_results[index];
    result.state = ok ? PsdBatchFileState::Succeeded : PsdBatchFileState::Failed;
    result.stage = ok ? PsdExportStage::Done :
        stage >= PsdExportStage::Preflight && stage <= PsdExportStage::Restoring ? stage : result.stage;
    result.error = ok ? QString() : error.isEmpty() ? QStringLiteral("导出失败，未提供详细原因") : error;
    result.outputCommitted = ok || outputCommitted;
    result.elapsedMs = m_elapsed.isValid() ? qMax(qint64(0), m_elapsed.elapsed()) : 0;
    clearPending();
    if (publishResult(index)) acknowledge(ok, outPath);
}

void PsdBatchJob::acknowledge(bool ok, const QString& outPath)
{
    auto* worker = m_worker;
    if (!worker || m_state != State::Running) return;
    QMetaObject::invokeMethod(worker, [worker, ok, outPath] { worker->onOneExportFinished(ok, outPath); }, Qt::QueuedConnection);
}

void PsdBatchJob::skip(const QString& outPath)
{
    auto* worker = m_worker;
    if (!worker || m_state != State::Running) return;
    QMetaObject::invokeMethod(worker, [worker, outPath] { worker->skipCurrent(outPath); }, Qt::QueuedConnection);
}

void PsdBatchJob::clearPending()
{
    m_pendingOutput.clear();
    m_pendingIndex = -1;
    m_exportDispatched = false;
    m_elapsed.invalidate();
}

void PsdBatchJob::shutdown()
{
    if (m_state == State::Shutdown) return;
    m_state = State::Shutdown;
    for (auto& result : m_results) {
        if (result.state == PsdBatchFileState::Pending ||
            (result.state == PsdBatchFileState::Running && !m_exportDispatched)) {
            result.state = PsdBatchFileState::NotRun;
            result.stage = PsdExportStage::Queued;
            result.error = QStringLiteral("任务已关闭，未开始导出");
            result.elapsedMs = 0;
        } else if (result.state == PsdBatchFileState::Running) {
            result.state = PsdBatchFileState::Failed;
            result.error = QStringLiteral("任务已关闭，未收到导出结果");
            result.elapsedMs = m_elapsed.isValid() ? qMax(qint64(0), m_elapsed.elapsed()) : 0;
        }
    }
    clearPending();
    stopThread();
}

void PsdBatchJob::stopThread()
{
    if (m_worker) disconnect(m_worker, nullptr, this, nullptr);
    m_thread.quit();
    m_thread.wait();
    m_worker = nullptr;
}
