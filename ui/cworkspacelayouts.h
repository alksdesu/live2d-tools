#ifndef CWORKSPACELAYOUTS_H
#define CWORKSPACELAYOUTS_H

#include <QByteArray>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <optional>
#include <functional>

class QMainWindow;
class QSettings;

class CWorkspaceLayouts : public QObject
{
public:
    static constexpr int Count = 3;
    static constexpr int LayoutVersion = 1;
    using DefaultLayout = std::function<void(int)>;

    void Load(const QSettings& settings, int legacyMode);
    void Save(QSettings& settings) const;
    void Attach(QMainWindow& window);
    void Capture(QMainWindow& window);
    void Detach(QMainWindow& window);
    bool Apply(QMainWindow& window, int mode, const DefaultLayout& defaults);
    void DeferDefaultSizing(QMainWindow& window, QObject* model, const DefaultLayout& sizing,
                            const std::function<void()>& settled);
    int ActiveMode() const { return m_activeMode; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Entry {
        QByteArray state;
        int qtVersion = LayoutVersion;
    };
    struct PendingSizing {
        QPointer<QMainWindow> window;
        QPointer<QObject> model;
        QByteArray placement;
        int mode = -1;
        quint64 generation = 0;
    };
    void SyncDockSafeArea();
    void CancelDefaultSizing();
    std::optional<PendingSizing> m_pendingSizing;
    quint64 m_generation = 0;
    QMap<int, Entry> m_entries;
    QByteArray m_baseline;
    int m_activeMode = -1;
    bool m_writable = true;
};

#endif
