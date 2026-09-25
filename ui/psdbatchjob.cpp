#include "psdbatchjob.h"
#include "psdbatchworker.h"

PsdBatchJob::PsdBatchJob(QObject* parent)
    : QObject(parent)
{}

PsdBatchJob::~PsdBatchJob()
{
    shutdown();
}

bool PsdBatchJob::start(const QStringList& models, const QString& outDir, bool all, bool fenzu,
                        bool texExtract, bool cubismCompat)
{
    if (m_state != State::Idle) return false;
    m_state = State::Running;
    m_worker = new PsdBatchWorker();
    m_worker->setTask(models, outDir, all, fenzu, texExtract, cubismCompat);
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
        if (m_canceled) {
            acknowledge(true, output);
            return;
        }
        if (!m_pendingOutput.isEmpty()) return;
        m_pendingOutput = output;
        emit requestExport(model, output, all, fenzu, texExtract, cubismCompat);
    }, Qt::QueuedConnection);
    connect(m_worker, &PsdBatchWorker::finished, this, [this](const QStringList& failedFiles) {
        if (m_state != State::Running) return;
        m_state = State::Finished;
        m_pendingOutput.clear();
        stopThread();
        emit finished(failedFiles, m_canceled);
    }, Qt::QueuedConnection);
    m_thread.start();
    return true;
}

bool PsdBatchJob::isRunning() const
{
    return m_thread.isRunning();
}

void PsdBatchJob::cancel()
{
    if (m_state != State::Running || m_canceled) return;
    m_canceled = true;
    QMetaObject::invokeMethod(m_worker, &PsdBatchWorker::cancel, Qt::QueuedConnection);
}

void PsdBatchJob::complete(bool ok, const QString& outPath)
{
    if (m_state != State::Running || m_pendingOutput.isEmpty() || outPath != m_pendingOutput) return;
    m_pendingOutput.clear();
    acknowledge(ok, outPath);
}

void PsdBatchJob::acknowledge(bool ok, const QString& outPath)
{
    auto* worker = m_worker;
    QMetaObject::invokeMethod(worker, [worker, ok, outPath] {
        worker->onOneExportFinished(ok, outPath);
    }, Qt::QueuedConnection);
}

void PsdBatchJob::shutdown()
{
    m_state = State::Shutdown;
    m_pendingOutput.clear();
    stopThread();
}

void PsdBatchJob::stopThread()
{
    if (m_worker) disconnect(m_worker, nullptr, this, nullptr);
    m_thread.quit();
    m_thread.wait();
    m_worker = nullptr;
}
