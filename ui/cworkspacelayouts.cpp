#include "cworkspacelayouts.h"
#include <QMainWindow>
#include <QApplication>
#include <QDataStream>
#include <QDockWidget>
#include <QEvent>
#include <QIODevice>
#include <QTimer>
#include <QSettings>
#include <exception>

namespace {
constexpr int MaximumStateSize = 16 * 1024 * 1024;

bool usable(const QByteArray& state)
{
    return !state.isEmpty() && state.size() <= MaximumStateSize;
}

QByteArray placement(const QMainWindow& window)
{
    QMap<QString, QDockWidget*> docks;
    for (auto* dock : window.findChildren<QDockWidget*>(QString(), Qt::FindDirectChildrenOnly))
        docks.insert(dock->objectName(), dock);
    QByteArray result;
    QDataStream stream(&result, QIODevice::WriteOnly);
    for (auto item = docks.cbegin(); item != docks.cend(); ++item) {
        QStringList group;
        for (auto* member : window.tabifiedDockWidgets(item.value())) group.append(member->objectName());
        group.sort();
        stream << item.key() << qint32(window.dockWidgetArea(item.value())) << item.value()->isHidden()
               << item.value()->isFloating() << group;
    }
    return result;
}

QString modeKey(int mode)
{
    return QStringLiteral("workspaceLayouts/modes/%1/").arg(mode);
}
}

void CWorkspaceLayouts::Load(const QSettings& settings, int legacyMode)
{
    CancelDefaultSizing();
    m_entries.clear();
    m_baseline.clear();
    m_activeMode = -1;
    m_writable = true;
    if (settings.contains(QStringLiteral("workspaceLayouts/version"))) {
        if (settings.value(QStringLiteral("workspaceLayouts/version")).toInt() != 1) {
            m_writable = false;
            return;
        }
        for (int mode = 0; mode < Count; ++mode) {
            const QString key = modeKey(mode);
            const QByteArray state = settings.value(key + QStringLiteral("state")).toByteArray();
            const int version = settings.value(key + QStringLiteral("qtVersion"), LayoutVersion).toInt();
            if (usable(state) && (version == 0 || version == LayoutVersion)) m_entries.insert(mode, {state, version});
        }
    } else {
        const QByteArray legacy = settings.value(QStringLiteral("windowState")).toByteArray();
        if (usable(legacy)) m_entries.insert(qBound(0, legacyMode, Count - 1), {legacy, 0});
    }
}

void CWorkspaceLayouts::Save(QSettings& settings) const
{
    if (!m_writable) return;
    settings.beginGroup(QStringLiteral("workspaceLayouts"));
    settings.remove(QString());
    settings.endGroup();
    settings.setValue(QStringLiteral("workspaceLayouts/version"), 1);
    for (auto entry = m_entries.cbegin(); entry != m_entries.cend(); ++entry) {
        const QString key = modeKey(entry.key());
        settings.setValue(key + QStringLiteral("state"), entry->state);
        settings.setValue(key + QStringLiteral("qtVersion"), entry->qtVersion);
    }
}

void CWorkspaceLayouts::Attach(QMainWindow& window)
{
    CancelDefaultSizing();
    m_baseline = window.saveState(LayoutVersion);
    m_activeMode = -1;
}

void CWorkspaceLayouts::Capture(QMainWindow& window)
{
    CancelDefaultSizing();
    if (m_activeMode < 0) return;
    const auto state = window.saveState(LayoutVersion);
    if (usable(state)) m_entries.insert(m_activeMode, {state, LayoutVersion});
}

void CWorkspaceLayouts::Detach(QMainWindow& window)
{
    Capture(window);
    m_activeMode = -1;
    m_baseline.clear();
}

bool CWorkspaceLayouts::Apply(QMainWindow& window, int mode, const DefaultLayout& defaults)
{
    mode = qBound(0, mode, Count - 1);
    if (!usable(m_baseline)) return false;
    if (mode == m_activeMode) return true;
    CancelDefaultSizing();
    const QByteArray before = window.saveState(LayoutVersion);
    auto next = m_entries;
    if (m_activeMode >= 0) next.insert(m_activeMode, {before, LayoutVersion});
    const auto selected = next.constFind(mode);
    const bool restored = selected != next.cend() && window.restoreState(selected->state, selected->qtVersion);
    if (!restored) {
        if (!window.restoreState(m_baseline, LayoutVersion)) {
            window.restoreState(before, LayoutVersion);
            return false;
        }
        try {
            if (defaults) defaults(mode);
        } catch (const std::exception&) {
            window.restoreState(before, LayoutVersion);
            return false;
        }
    }
    next.insert(mode, {window.saveState(LayoutVersion), LayoutVersion});
    m_entries.swap(next);
    m_activeMode = mode;
    return true;
}

void CWorkspaceLayouts::CancelDefaultSizing()
{
    ++m_generation;
    m_pendingSizing.reset();
    if (qApp) qApp->removeEventFilter(this);
}

void CWorkspaceLayouts::DeferDefaultSizing(QMainWindow& window, QObject* model, const DefaultLayout& sizing,
                                          const std::function<void()>& settled)
{
    CancelDefaultSizing();
    if (m_activeMode < 0 || !model || !sizing) return;
    m_pendingSizing = PendingSizing{&window, model, placement(window), m_activeMode, m_generation};
    qApp->installEventFilter(this);
    const quint64 generation = m_generation;
    QTimer::singleShot(0, this, [this, generation, sizing, settled] {
        if (!m_pendingSizing || m_pendingSizing->generation != generation) return;
        const PendingSizing pending = *m_pendingSizing;
        CancelDefaultSizing();
        if (!pending.window || !pending.model || pending.mode != m_activeMode) return;
        const QPointer<CWorkspaceLayouts> alive(this);
        const QByteArray before = pending.window->saveState(LayoutVersion);
        try {
            if (placement(*pending.window) == pending.placement) sizing(pending.mode);
        } catch (const std::exception&) {
            if (pending.window) pending.window->restoreState(before, LayoutVersion);
        }
        if (!alive || !pending.window) return;
        Capture(*pending.window);
        if (settled) settled();
    });
}

bool CWorkspaceLayouts::eventFilter(QObject* watched, QEvent* event)
{
    if (m_pendingSizing && m_pendingSizing->window) {
        const auto type = event->type();
        const bool input = type == QEvent::MouseButtonPress || type == QEvent::NonClientAreaMouseButtonPress ||
                           type == QEvent::KeyPress || type == QEvent::TouchBegin || type == QEvent::Wheel;
        auto* widget = qobject_cast<QWidget*>(watched);
        if (input && widget && (widget == m_pendingSizing->window || m_pendingSizing->window->isAncestorOf(widget)))
            CancelDefaultSizing();
    }
    return QObject::eventFilter(watched, event);
}
