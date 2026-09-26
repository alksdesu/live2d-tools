#ifndef CDOCKTOGGLE_H
#define CDOCKTOGGLE_H

#include <QAbstractButton>
#include <QAction>
#include <QDockWidget>
#include <QPointer>
#include <QSignalBlocker>

inline void ConnectDockToggle(QAbstractButton* button, QDockWidget* dock)
{
    if (!button || !dock) return;
    auto* action = dock->toggleViewAction();
    {
        const QSignalBlocker blocker(button);
        button->setChecked(action->isChecked());
    }
    QObject::connect(action, &QAction::toggled, button, [button](bool visible) {
        const QSignalBlocker blocker(button);
        button->setChecked(visible);
    });
    QObject::connect(button, &QAbstractButton::clicked, dock, [dock](bool visible) {
        const QPointer<QDockWidget> target(dock);
        target->setVisible(visible);
        if (target && visible) target->raise();
    });
}

#endif
