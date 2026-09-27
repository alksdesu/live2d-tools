#ifndef CDRAFTDIALOGLIFECYCLE_H
#define CDRAFTDIALOGLIFECYCLE_H

#include <QDialog>
#include <QPointer>
#include <QScopeGuard>
#include <QTimer>
#include <functional>
#include <memory>

class CDraftDialogLifecycle
{
public:
    using Recovery = std::function<bool(QString*)>;
    using Completion = std::function<void()>;
    using Reporter = std::function<void(const QString&)>;

    static Completion Attach(QDialog* dialog, QObject* owner, Recovery recover, Completion release, Reporter report)
    {
        struct State : std::enable_shared_from_this<State> {
            QPointer<QDialog> dialog;
            QPointer<QObject> owner;
            Recovery recover;
            Completion release;
            Reporter report;
            bool resolving = false, released = false, showQueued = false;

            void Resolve()
            {
                if (!owner || resolving || released) return;
                resolving = true;
                const auto reset = qScopeGuard([this] { resolving = false; });
                QString error; bool restored = false;
                try { restored = !recover || recover(&error); }
                catch (...) { error = QStringLiteral("恢复编辑状态失败，请重试取消"); }
                if (!owner) return;
                if (!restored) {
                    try { if (report) report(error.isEmpty() ? QStringLiteral("未能恢复编辑状态，请重试取消") : error); }
                    catch (...) {}
                    if (!owner || !dialog || showQueued) return;
                    showQueued = true;
                    const auto held = this->shared_from_this();
                    QTimer::singleShot(0, dialog, [held] {
                        held->showQueued = false;
                        if (held->released || !held->owner || !held->dialog) return;
                        const QPointer<QDialog> retained = held->dialog;
                        retained->setWindowModality(Qt::WindowModal);
                        if (retained) retained->show();
                    });
                    return;
                }
                released = true;
                const QPointer<QDialog> closing = dialog;
                try { if (release) release(); }
                catch (...) {}
                if (!owner) return;
                if (closing) closing->deleteLater();
            }
        };
        auto state = std::make_shared<State>(); state->dialog = dialog; state->owner = owner;
        state->recover = std::move(recover); state->release = std::move(release); state->report = std::move(report);
        const Completion settle = [state] { const auto held = state; held->Resolve(); };
        QObject::connect(dialog, &QDialog::finished, owner, [settle](int) { settle(); });
        QObject::connect(dialog, &QObject::destroyed, owner, [owner = QPointer<QObject>(owner), settle] {
            if (owner) QTimer::singleShot(0, owner, settle);
        });
        return settle;
    }
};

#endif
