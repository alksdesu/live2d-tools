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
class CPhysicsEditDelegate;
class QLineEdit;
struct CPhysicsHistoryCheckpoint;

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
        std::shared_ptr<const CPhysicsHistoryCheckpoint> history;
        QString selectedGroupId;
        int selectedGroupRow = -1;
        bool dragEnabled = false;
        std::array<TableState, 3> tables;
        bool SameDocument(const Snapshot& other) const
        { return document == other.document && path == other.path && revision == other.revision; }
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
        QJsonObject m_document, m_normalizedDocument;
        QJsonArray m_settings, m_dictionary;
        QString m_path;
        std::unique_ptr<Snapshot> m_refresh;
    };
    using EditPublisher = std::function<bool(const Snapshot&, const Snapshot&, QString*)>;
    explicit CPhysicsPanel(QWidget *parent = nullptr);
    ~CPhysicsPanel() override;
    void SetEditPublisher(EditPublisher publisher);
    bool CanUndo() const;
    bool CanRedo() const;
    bool Undo(QString* error = nullptr);
    bool Redo(QString* error = nullptr);
    bool NewPhysics(QString* error = nullptr);
    void MarkSessionSaved();
    bool LoadPhysics(const QString& physicsPath, QString* error = nullptr);
    bool SavePhysics(const QString& path = QString(), QString* error = nullptr);
    bool ExportPhysics(const QString& path, QString* error = nullptr) const;
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
    void HistoryChanged(bool canUndo, bool canRedo);
    void EditRejected(const QString& reason);
    // 选择物理组后发射输入参数列表，用于鼠标拖拽驱动
    void PhysicsDragParamsChanged(const QStringList& paramIds, const QStringList& types);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void showGroupDetail(int row);
    void refreshGroupTable(int selectedRow, bool preserveNoSelection = false);
    bool CommitDocument(const Snapshot& before, QJsonObject document, int selectedRow, QString* error = nullptr,
                        const QString& mergeKey = QString());
    bool PublishPrepared(PreparedSnapshot& prepared, QString* error);
    void InstallPrepared(PreparedSnapshot& prepared) noexcept;
    bool MoveHistory(bool undo, QString* error);
    void RejectEdit(const QString& error);
    void RefreshHistoryControls();
    bool CaptureAcceptedSnapshot(Snapshot& result, QString* error = nullptr) const;
    bool SubmitCell(int table, int row, int column, const QString& text, quint64 revision, QString* error);
    void RefreshAfterEditor();
    void ApplyNormalization();
    void copyPhysicsGroup();
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
    QLineEdit* m_fpsEdit = nullptr;
    QLabel* m_errorLabel = nullptr;
    QPushButton* m_undoButton = nullptr;
    QPushButton* m_redoButton = nullptr;
    QPushButton* m_copyGroupButton = nullptr;
    std::array<QLineEdit*, 6> m_normalizationEdits{};
    std::array<QLineEdit*, 4> m_forceEdits{};
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
    int m_detailGroupRow = -1;
    bool m_updating = false;
    bool m_dirty = false;
    quint64 m_documentRevision = 0;
    mutable quint64 m_prepareSerial = 0;
    std::array<CPhysicsEditDelegate*, 3> m_editDelegates{};
    std::shared_ptr<const CPhysicsHistoryCheckpoint> m_history;
    QJsonObject m_savedDocument;
    QString m_savedPath;
    EditPublisher m_editPublisher;
    bool m_publishing = false;
    bool m_committingCell = false;
    QSet<const QLineEdit*> m_committingControls;
    quint64 m_forceGesture = 0;
    QString m_forceMergeKey;
    bool m_snapshotNotificationPending = false;
    std::unique_ptr<Snapshot> m_pendingSnapshotRefresh;
    std::function<bool(QString*)> m_sessionSaveHandler;
};

Q_DECLARE_METATYPE(CPhysicsPanel::Snapshot)

#endif // CPHYSICSPANEL_H
