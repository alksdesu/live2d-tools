#ifndef CPHYSICSPANEL_H
#define CPHYSICSPANEL_H

#include <QDockWidget>
#include <QTableWidget>
#include <QSlider>
#include <QLabel>
#include <QCheckBox>
#include <QJsonObject>
#include <QJsonArray>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <array>
#include <memory>
#include <functional>

class CPhysicsGraphWidget;
class QPushButton;
class CTableEditDelegate;

class CPhysicsPanel : public QDockWidget
{
    Q_OBJECT
public:
    struct TableState {
        QPoint current{-1, -1};
        QVector<QRect> selection;
        int horizontalScroll = 0;
        int verticalScroll = 0;
    };
    struct Snapshot {
        QJsonObject document;
        QString path;
        bool dirty = false;
        quint64 revision = 0;
        QString selectedGroupId;
        int selectedGroupRow = -1;
        bool dragEnabled = false;
        std::array<TableState, 3> tables;
        bool SameDocument(const Snapshot& other) const
        { return document == other.document && path == other.path && dirty == other.dirty && revision == other.revision; }
    };
    class PreparedSnapshot {
        friend class CPhysicsPanel;
    public:
        PreparedSnapshot() = default;
        PreparedSnapshot(const PreparedSnapshot&) = delete;
        PreparedSnapshot& operator=(const PreparedSnapshot&) = delete;
        PreparedSnapshot(PreparedSnapshot&&) noexcept = default;
        PreparedSnapshot& operator=(PreparedSnapshot&&) noexcept = default;
        const Snapshot& Result() const { return m_target; }
    private:
        QPointer<CPhysicsPanel> m_owner;
        quint64 m_token = 0;
        Snapshot m_expected, m_target;
        QJsonObject m_document;
        QJsonArray m_settings, m_dictionary;
        QString m_path;
        std::unique_ptr<Snapshot> m_refresh;
    };
    explicit CPhysicsPanel(QWidget *parent = nullptr);
    bool LoadPhysics(const QString& physicsPath, QString* error = nullptr);
    bool SavePhysics(const QString& path = QString(), QString* error = nullptr);
    void SetSessionSaveHandler(std::function<bool(QString*)> handler);
    bool HasUnsavedChanges() const { return m_dirty || HasPendingCellEdits(); }
    const QString& FilePath() const { return m_physicsPath; }
    QJsonObject DocumentSnapshot() const;
    bool CaptureSnapshot(Snapshot& result, QString* error = nullptr) const;
    bool PrepareSnapshot(const Snapshot& expected, const Snapshot& target, PreparedSnapshot& prepared,
                         QString* error = nullptr) const;
    bool PrepareReferenceRemoval(const Snapshot& expected, const QSet<QString>& ids,
                                 PreparedSnapshot& prepared, QString* error = nullptr) const;
    bool ValidatePreparedSnapshot(const PreparedSnapshot& prepared, QString* error = nullptr) const;
    bool CommitPreparedSnapshot(PreparedSnapshot& prepared, QString* error = nullptr);
    void NotifySnapshotCommitted();

signals:
    void GravityChanged(float x, float y);
    void WindChanged(float x, float y);
    void PhysicsConfigChanged();
    void PhysicsSaved(const QString& path);
    // 选择物理组后发射输入参数列表，用于鼠标拖拽驱动
    void PhysicsDragParamsChanged(const QStringList& paramIds, const QStringList& types);

private:
    void showGroupDetail(int row);
    void refreshGroupTable(int selectedRow, bool preserveNoSelection = false);
    void commitGroupChanges(int selectedRow);
    void emitDragParams(int row);
    void updateInputCell(int row, int column);
    void updateOutputCell(int row, int column);
    void updateVertexCell(int row, int column);
    void addPhysicsInput();
    void removePhysicsInput();
    void addPhysicsOutput();
    void removePhysicsOutput();
    void addPhysicsVertex();
    void removePhysicsVertex();
    void addPhysicsGroup();
    void removePhysicsGroup();
    void renamePhysicsGroup();
    void updateForceDocument(bool gravity, bool xAxis, double value);
    bool ValidateDocument(QString* error) const;
    static bool ValidateDocument(const QJsonObject& document, QString* error);
    bool HasPendingCellEdits() const;
    void RefreshSnapshotControls(const Snapshot& snapshot);

    QLabel* m_fpsLabel;
    QTableWidget* m_groupTable;
    QCheckBox* m_dragModeCb;

    // 详情区域
    QTableWidget* m_inputTable;
    QTableWidget* m_outputTable;
    QTableWidget* m_vertexTable;
    CPhysicsGraphWidget* m_graph = nullptr;
    QPushButton* m_addInputButton = nullptr;
    QPushButton* m_saveButton = nullptr;
    QPushButton* m_removeInputButton = nullptr;
    QPushButton* m_addOutputButton = nullptr;
    QPushButton* m_removeOutputButton = nullptr;
    QPushButton* m_addVertexButton = nullptr;
    QPushButton* m_removeVertexButton = nullptr;
    QPushButton* m_addGroupButton = nullptr;
    QPushButton* m_removeGroupButton = nullptr;
    QPushButton* m_renameGroupButton = nullptr;

    // 重力/风力滑条
    QSlider* m_gravityX;
    QSlider* m_gravityY;
    QSlider* m_windX;
    QSlider* m_windY;
    QLabel* m_gravityXLabel;
    QLabel* m_gravityYLabel;
    QLabel* m_windXLabel;
    QLabel* m_windYLabel;

    // 解析后的数据
    QJsonArray m_physicsSettings;
    QJsonArray m_physicsDictionary;
    QJsonObject m_physicsDocument;
    QString m_physicsPath;
    bool m_updating = false;
    bool m_dirty = false;
    quint64 m_documentRevision = 0;
    mutable quint64 m_prepareSerial = 0;
    std::array<CTableEditDelegate*, 3> m_editDelegates{};
    bool m_snapshotNotificationPending = false;
    std::unique_ptr<Snapshot> m_pendingSnapshotRefresh;
    std::function<bool(QString*)> m_sessionSaveHandler;
};

Q_DECLARE_METATYPE(CPhysicsPanel::Snapshot)

#endif // CPHYSICSPANEL_H
