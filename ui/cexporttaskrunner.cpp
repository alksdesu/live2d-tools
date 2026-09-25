#include "cexporttaskrunner.h"
#include "std/cexportcontrol.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QScopedValueRollback>
#include <QThread>
#include <atomic>
#include <exception>
#include <memory>

namespace {
thread_local bool running = false;

bool fail(QString* error, const QString& reason)
{
    if (error) *error = reason;
    return false;
}

bool invoke(const std::function<bool(QString*)>& operation, QString* error)
{
    try {
        return operation(error);
    } catch (const std::exception& exception) {
        return fail(error, QString::fromUtf8(exception.what()));
    } catch (...) {
        return fail(error, QStringLiteral("导出任务发生未知异常"));
    }
}

struct DispatchState {
    QObject* dispatcher = nullptr;
    QThread* worker = nullptr;
    std::atomic<bool> accepting{false};
};
}

bool CExportTaskRunner::Run(const Task& task, CExportControl* control, QString* error)
{
    if (error) error->clear();
    if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread())
        return fail(error, QStringLiteral("导出任务必须从主线程启动"));
    if (running) return fail(error, QStringLiteral("导出任务正在执行，不能重复启动"));
    if (!task) return fail(error, QStringLiteral("导出任务为空"));
    const QScopedValueRollback<bool> executing(running, true);
    if (control && !control->Checkpoint()) return fail(error, control->Error());
    QObject dispatcher;
    QEventLoop loop;
    auto state = std::make_shared<DispatchState>();
    state->dispatcher = &dispatcher;
    const GuiCall guiCall = [state](const std::function<bool(QString*)>& operation, QString* detail) {
        if (detail) detail->clear();
        if (!state->accepting.load(std::memory_order_acquire) || QThread::currentThread() != state->worker)
            return fail(detail, QStringLiteral("导出主线程调用已失效"));
        if (!operation) return fail(detail, QStringLiteral("导出主线程操作为空"));
        bool accepted = false;
        QString reason;
        const bool dispatched = QMetaObject::invokeMethod(state->dispatcher, [&] {
            accepted = invoke(operation, &reason);
        }, Qt::BlockingQueuedConnection);
        if (!dispatched) return fail(detail, QStringLiteral("无法调度导出主线程操作"));
        if (!accepted && reason.isEmpty()) reason = QStringLiteral("导出主线程操作失败");
        if (detail) *detail = reason;
        return accepted;
    };
    std::atomic<bool> completed{false};
    bool accepted = false;
    QString reason;
    std::unique_ptr<QThread> worker(QThread::create([&] {
        accepted = invoke([&](QString* detail) { return task(guiCall, detail); }, &reason);
        state->accepting.store(false, std::memory_order_release);
        completed.store(true, std::memory_order_release);
    }));
    state->worker = worker.get();
    state->accepting.store(true, std::memory_order_release);
    QObject::connect(worker.get(), &QThread::finished, &loop, &QEventLoop::quit, Qt::QueuedConnection);
    worker->start();
    while (!completed.load(std::memory_order_acquire)) {
        loop.exec();
        if (!completed.load(std::memory_order_acquire)) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
    }
    worker->wait();
    if (accepted && control && !control->Checkpoint()) {
        accepted = false;
        reason = control->Error();
    }
    if (!accepted) {
        if (reason.isEmpty() && control) reason = control->Error();
        if (reason.isEmpty()) reason = QStringLiteral("导出任务失败，未提供详细原因");
        if (control) {
            if (reason != control->Error()) control->Fail(reason);
            if (!control->Error().isEmpty()) reason = control->Error();
        }
        return fail(error, reason);
    }
    return true;
}
