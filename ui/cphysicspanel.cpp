#include "cphysicspanel.h"
#include "live2dstruct/cphysicsdocument.h"
#include "cphysicsgraph.h"
#include <QStyledItemDelegate>
#include <QKeyEvent>
#include <QTimer>
#include <QLineEdit>
#include <QToolTip>
#include <QApplication>
#include <QScrollArea>
#include <QGridLayout>
#include <QLocale>
#include "live2dstruct/cparameterreferenceeditor.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QFile>
#include <QJsonDocument>
#include <QSplitter>
#include <QDebug>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QInputDialog>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QItemSelectionModel>
#include <QComboBox>
#include <limits>
#include <exception>
#include <cmath>
#include <algorithm>

struct CPhysicsHistoryCheckpoint
{
    QPointer<CPhysicsPanel> owner;
    QVector<CPhysicsPanel::Snapshot> states;
    int cursor = 0;
    QString mergeKey;
};

class CPhysicsEditDelegate : public QStyledItemDelegate
{
public:
    using Submit = std::function<bool(const QModelIndex&, const QString&, quint64, QString*)>;
    CPhysicsEditDelegate(QObject* parent, Submit submit, std::function<quint64()> revision,
                         std::function<void()> finished)
        : QStyledItemDelegate(parent), m_submit(std::move(submit)), m_revision(std::move(revision)),
          m_finished(std::move(finished)) {}
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const override
    {
        auto* editor = new QLineEdit(parent);
        m_editor = editor;
        m_index = index;
        m_original = index.data(Qt::EditRole).toString();
        m_startRevision = m_revision();
        return editor;
    }
    void setEditorData(QWidget* editor, const QModelIndex& index) const override
    { static_cast<QLineEdit*>(editor)->setText(index.data(Qt::EditRole).toString()); }
    void setModelData(QWidget*, QAbstractItemModel*, const QModelIndex&) const override {}
    void destroyEditor(QWidget* editor, const QModelIndex& index) const override
    {
        if (m_editor == editor) { m_editor.clear(); m_index = QPersistentModelIndex(); }
        QStyledItemDelegate::destroyEditor(editor, index);
    }
    bool HasPendingChanges() const { return !m_finishing && m_editor && m_index.isValid() && m_editor->text() != m_original; }
    void Detach() { m_submit = {}; m_revision = {}; m_finished = {}; }
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (object != m_editor || !m_submit) return QStyledItemDelegate::eventFilter(object, event);
        if (event->type() == QEvent::KeyPress) {
            const auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape) {
                emit closeEditor(m_editor, QAbstractItemDelegate::RevertModelCache);
                return true;
            }
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter ||
                key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) return Finish();
        }
        if (event->type() == QEvent::FocusOut) { Finish(); return true; }
        return QStyledItemDelegate::eventFilter(object, event);
    }
private:
    bool Finish()
    {
        if (m_finishing || !m_editor) return true;
        const QScopedValueRollback<bool> guard(m_finishing, true);
        const QPointer<QLineEdit> editor = m_editor;
        QString error;
        if (m_index.isValid() && editor->text() != m_original && !m_submit(m_index, editor->text(), m_startRevision, &error)) {
            editor->setToolTip(error);
            QToolTip::showText(editor->mapToGlobal(QPoint(0, editor->height())), QStringLiteral("<qt>%1</qt>").arg(error.toHtmlEscaped()), editor);
            QTimer::singleShot(0, editor, [editor] { if (editor) editor->setFocus(Qt::OtherFocusReason); });
            return true;
        }
        QToolTip::hideText();
        emit closeEditor(editor, QAbstractItemDelegate::NoHint);
        if (m_finished) m_finished();
        return true;
    }
    Submit m_submit;
    std::function<quint64()> m_revision;
    std::function<void()> m_finished;
    mutable QPointer<QLineEdit> m_editor;
    mutable QPersistentModelIndex m_index;
    mutable QString m_original;
    mutable quint64 m_startRevision = 0;
    bool m_finishing = false;
};

namespace {
void fitPhysicsRows(QTableWidget* table)
{
    const int rowHeight = qMax(table->verticalHeader()->defaultSectionSize(), table->rowHeight(0));
    const int visibleRows = qBound(1, table->rowCount(), 4);
    table->setMinimumHeight(table->horizontalHeader()->height() + visibleRows * rowHeight +
        table->horizontalScrollBar()->sizeHint().height() + 2 * table->frameWidth());
}
QString physicsNumber(double value)
{
    return QLocale::c().toString(value, 'g', QLocale::FloatingPointShortest);
}
double physicsForceValue(const QJsonObject& document, bool gravity, bool xAxis)
{
    return document.value("Meta").toObject().value("EffectiveForces").toObject()
        .value(gravity ? "Gravity" : "Wind").toObject().value(xAxis ? "X" : "Y")
        .toDouble(gravity && !xAxis ? -1.0 : 0.0);
}
QJsonArray physicsDictionary(const QJsonObject& document)
{
    const auto meta = document.value("Meta").toObject();
    return meta.contains("PhysicsDictionary") ? meta.value("PhysicsDictionary").toArray()
                                               : document.value("PhysicsDictionary").toArray();
}
QJsonObject normalizedPhysics(const QJsonObject& document)
{
    QJsonObject normalized;
    if (!document.isEmpty() && live2dstruct::CPhysicsDocument::Normalize(document, normalized)) return normalized;
    return document;
}
}

CPhysicsPanel::~CPhysicsPanel()
{
    m_updating = true;
    for (auto* child : findChildren<QObject*>()) {
        QObject::disconnect(child, nullptr, this, nullptr);
        child->removeEventFilter(this);
    }
    for (auto* delegate : m_editDelegates) if (delegate) delegate->Detach();
}

void CPhysicsPanel::SetEditPublisher(EditPublisher publisher) { m_editPublisher = std::move(publisher); }
bool CPhysicsPanel::CanUndo() const { return m_history && m_history->cursor > 0; }
bool CPhysicsPanel::CanRedo() const { return m_history && m_history->cursor + 1 < m_history->states.size(); }
bool CPhysicsPanel::Undo(QString* error) { return MoveHistory(true, error); }
bool CPhysicsPanel::Redo(QString* error) { return MoveHistory(false, error); }

void CPhysicsPanel::RejectEdit(const QString& error)
{
    m_errorLabel->setText(error);
    m_errorLabel->setVisible(!error.isEmpty());
    if (!error.isEmpty()) {
        auto* focused = QApplication::focusWidget();
        if (focused && isAncestorOf(focused))
            QToolTip::showText(focused->mapToGlobal(QPoint(0, focused->height())), QStringLiteral("<qt>%1</qt>").arg(error.toHtmlEscaped()), focused);
        emit EditRejected(error);
    }
}

void CPhysicsPanel::RefreshHistoryControls()
{
    m_undoButton->setEnabled(CanUndo());
    m_redoButton->setEnabled(CanRedo());
    emit HistoryChanged(CanUndo(), CanRedo());
}

void CPhysicsPanel::MarkSessionSaved()
{
    if (m_publishing || HasPendingCellEdits()) return;
    QJsonObject saved = normalizedPhysics(m_physicsDocument);
    QString path = m_physicsPath;
    m_savedDocument.swap(saved);
    m_savedPath.swap(path);
    m_dirty = false;
}

bool CPhysicsPanel::PublishPrepared(PreparedSnapshot& prepared, QString* error)
{
    if (m_publishing || !ValidatePreparedSnapshot(prepared, error)) return false;
    const QScopedValueRollback<bool> guard(m_publishing, true);
    try {
        const auto publisher = m_editPublisher;
        if (publisher && !publisher(prepared.m_expected, prepared.m_target, error)) {
            if (error && error->isEmpty()) *error = QStringLiteral("运行时未接受物理修改");
            return false;
        }
        InstallPrepared(prepared);
        return true;
    } catch (...) {
        if (error) *error = QStringLiteral("无法发布物理修改，原配置已保留");
        return false;
    }
}

bool CPhysicsPanel::CommitDocument(const Snapshot& before, QJsonObject document, int selectedRow,
                                  QString* error, const QString& mergeKey)
{
    QString detail;
    if (!error) error = &detail;
    error->clear();
    try {
        QJsonObject normalized;
        if (m_publishing) { *error = QStringLiteral("物理修改正在提交"); return false; }
        if (!live2dstruct::CPhysicsDocument::Normalize(document, normalized, error)) { RejectEdit(*error); return false; }
        Snapshot current;
        if (!(m_committingCell ? CaptureAcceptedSnapshot(current, error) : CaptureSnapshot(current, error)) || !current.SameDocument(before)) {
            if (error->isEmpty()) *error = QStringLiteral("物理草稿已改变，请重新编辑"); RejectEdit(*error); return false;
        }
        if (normalizedPhysics(before.document) == normalized) {
            for (const auto* control : m_committingControls) {
                auto* edit = const_cast<QLineEdit*>(control);
                edit->setText(edit->property("physicsAcceptedText").toString());
            }
            m_errorLabel->clear(); m_errorLabel->hide(); QToolTip::hideText(); return true;
        }
        Snapshot target = before;
        target.document = std::move(normalized);
        target.selectedGroupRow = selectedRow;
        const auto groups = target.document.value("PhysicsSettings").toArray();
        target.selectedGroupRow = groups.isEmpty() ? -1 : qBound(0, selectedRow, int(groups.size()) - 1);
        target.selectedGroupId = target.selectedGroupRow < 0 ? QString() : groups[target.selectedGroupRow].toObject().value("Id").toString();
        if (target.selectedGroupId != before.selectedGroupId) target.tables = {};
        else {
            const auto selected = target.selectedGroupRow >= 0 ? groups[target.selectedGroupRow].toObject() : QJsonObject();
            const std::array<int, 3> rows{int(selected.value("Input").toArray().size()), int(selected.value("Output").toArray().size()),
                qMax(0, int(selected.value("Vertices").toArray().size()) - 1)};
            for (int i = 0; i < 3; ++i) {
                if (target.tables[i].current.y() >= rows[i]) target.tables[i].current = QPoint(-1, -1);
                auto& selection = target.tables[i].selection;
                selection.erase(std::remove_if(selection.begin(), selection.end(), [&](const QRect& range) {
                    return range.bottom() >= rows[i];
                }), selection.end());
            }
        }
        target.dirty = normalizedPhysics(target.document) != m_savedDocument || target.path != m_savedPath;
        auto history = m_history ? std::make_shared<CPhysicsHistoryCheckpoint>(*m_history)
                                 : std::make_shared<CPhysicsHistoryCheckpoint>();
        history->owner = this;
        if (history->states.isEmpty()) { Snapshot initial = before; initial.history.reset(); history->states.append(initial); }
        history->states.resize(history->cursor + 1);
        Snapshot stored = target; stored.history.reset();
        if (!mergeKey.isEmpty() && history->mergeKey == mergeKey && history->cursor > 0)
            history->states[history->cursor] = stored;
        else { history->states.append(stored); ++history->cursor; }
        history->mergeKey = mergeKey;
        target.history = history;
        PreparedSnapshot prepared;
        if (!PrepareSnapshot(before, target, prepared, error) || !PublishPrepared(prepared, error)) {
            RejectEdit(*error); return false;
        }
        m_errorLabel->clear(); m_errorLabel->hide(); QToolTip::hideText();
        if (!m_committingCell) { try { NotifySnapshotCommitted(); } catch (...) {} }
        return true;
    } catch (...) { *error = QStringLiteral("无法准备物理修改"); RejectEdit(*error); return false; }
}

bool CPhysicsPanel::MoveHistory(bool undo, QString* error)
{
    QString detail;
    if (!error) error = &detail;
    error->clear();
    try {
        if (m_publishing || (undo ? !CanUndo() : !CanRedo())) { *error = QStringLiteral("没有可执行的物理历史"); return false; }
        Snapshot before;
        if (!CaptureSnapshot(before, error)) return false;
        auto history = std::make_shared<CPhysicsHistoryCheckpoint>(*m_history);
        history->cursor += undo ? -1 : 1;
        history->mergeKey.clear();
        Snapshot target = history->states[history->cursor];
        target.path = m_physicsPath;
        target.dirty = normalizedPhysics(target.document) != m_savedDocument || target.path != m_savedPath;
        target.history = history;
        PreparedSnapshot prepared;
        if (!PrepareSnapshot(before, target, prepared, error) || !PublishPrepared(prepared, error)) { RejectEdit(*error); return false; }
        m_errorLabel->clear(); m_errorLabel->hide(); QToolTip::hideText();
        try { NotifySnapshotCommitted(); } catch (...) {}
        return true;
    } catch (...) { *error = QStringLiteral("无法准备物理历史"); RejectEdit(*error); return false; }
}

bool CPhysicsPanel::NewPhysics(QString* error)
{
    Snapshot before;
    if (!CaptureSnapshot(before, error)) return false;
    if (!before.document.isEmpty()) { if (error) *error = QStringLiteral("已有物理草稿，请先保存并关闭当前模型"); return false; }
    QJsonObject forces{{"Gravity", QJsonObject{{"X", 0.0}, {"Y", -1.0}}}, {"Wind", QJsonObject{{"X", 0.0}, {"Y", 0.0}}}};
    QJsonObject document{{"Version", 3}, {"Meta", QJsonObject{{"Fps", 30.0}, {"EffectiveForces", forces}, {"PhysicsDictionary", QJsonArray{}}}},
                         {"PhysicsSettings", QJsonArray{}}};
    return CommitDocument(before, document, -1, error);
}

void CPhysicsPanel::RefreshAfterEditor()
{
    QTimer::singleShot(0, this, [this] { if (!HasPendingCellEdits()) NotifySnapshotCommitted(); });
}

CPhysicsPanel::CPhysicsPanel(QWidget *parent)
    : QDockWidget("物理设置", parent)
{
    setObjectName(QStringLiteral("physicsPanel"));
    QWidget* wrapper = new QWidget(this);
    QVBoxLayout* mainLayout = new QVBoxLayout(wrapper);

    auto* toolbar = new QGridLayout;
    auto* newButton = new QPushButton(QStringLiteral("新建"), wrapper);
    newButton->setObjectName(QStringLiteral("newPhysicsButton"));
    m_undoButton = new QPushButton(QStringLiteral("撤销"), wrapper);
    m_redoButton = new QPushButton(QStringLiteral("重做"), wrapper);
    m_undoButton->setObjectName(QStringLiteral("undoPhysicsButton"));
    m_redoButton->setObjectName(QStringLiteral("redoPhysicsButton"));
    m_saveButton = new QPushButton(QStringLiteral("保存物理配置"), wrapper);
    m_saveButton->setObjectName(QStringLiteral("savePhysicsButton"));
    auto* exportButton = new QPushButton(QStringLiteral("导出配置"), wrapper);
    exportButton->setObjectName(QStringLiteral("exportPhysicsButton"));
    toolbar->addWidget(newButton, 0, 0); toolbar->addWidget(m_undoButton, 0, 1); toolbar->addWidget(m_redoButton, 0, 2);
    toolbar->addWidget(m_saveButton, 1, 0, 1, 2); toolbar->addWidget(exportButton, 1, 2);
    connect(exportButton, &QPushButton::clicked, this, [this] {
        const auto target = QFileDialog::getSaveFileName(this, QStringLiteral("导出物理配置"), {}, QStringLiteral("Physics (*.physics3.json)"));
        if (target.isEmpty()) return;
        QString error;
        if (!ExportPhysics(target, &error)) RejectEdit(error);
        else { m_errorLabel->setText(QStringLiteral("已导出配置")); m_errorLabel->show(); }
    });
    mainLayout->addLayout(toolbar);
    connect(newButton, &QPushButton::clicked, this, [this] { QString error; if (!NewPhysics(&error)) RejectEdit(error); });
    connect(m_undoButton, &QPushButton::clicked, this, [this] { QString error; if (!Undo(&error)) RejectEdit(error); });
    connect(m_redoButton, &QPushButton::clicked, this, [this] { QString error; if (!Redo(&error)) RejectEdit(error); });
    connect(m_saveButton, &QPushButton::clicked, this, [this] { QString error; if (!SavePhysics({}, &error)) RejectEdit(error); });
    auto* monitorTitle = new QLabel(QStringLiteral("物理监视"), wrapper);
    mainLayout->addWidget(monitorTitle);
    auto* observationGroupRow = new QHBoxLayout;
    auto* observationGroupLabel = new QLabel(QStringLiteral("当前物理组"), wrapper);
    m_observationGroup = new QComboBox(wrapper);
    m_observationGroup->setObjectName(QStringLiteral("physicsObservationGroup"));
    m_observationGroup->setAccessibleName(QStringLiteral("当前物理组"));
    m_observationGroup->setPlaceholderText(QStringLiteral("请选择物理组"));
    m_observationGroup->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_observationGroup->setMinimumContentsLength(8);
    m_observationGroup->setEnabled(false);
    observationGroupLabel->setBuddy(m_observationGroup);
    observationGroupRow->addWidget(observationGroupLabel);
    observationGroupRow->addWidget(m_observationGroup, 1);
    mainLayout->addLayout(observationGroupRow);
    connect(m_observationGroup, &QComboBox::currentIndexChanged, this, [this](int row) {
        if (!m_updating) m_groupTable->setCurrentCell(row, row >= 0 ? 0 : -1);
    });
    m_observationStatus = new QLabel(QStringLiteral("尚未实时：选择物理组并开始预览"), wrapper);
    m_observationStatus->setObjectName(QStringLiteral("physicsObservationStatus"));
    m_observationStatus->setTextFormat(Qt::PlainText);
    m_observationStatus->setWordWrap(true);
    mainLayout->addWidget(m_observationStatus);
    m_graph = new CPhysicsGraphWidget(wrapper);
    mainLayout->addWidget(m_graph);
    m_observationChannel = new QComboBox(wrapper);
    m_observationChannel->setObjectName(QStringLiteral("physicsObservationChannel"));
    m_observationChannel->setAccessibleName(QStringLiteral("趋势通道"));
    m_observationChannel->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_observationChannel->setMinimumContentsLength(8);
    auto* channelLabel = new QLabel(QStringLiteral("趋势通道"), wrapper);
    channelLabel->setBuddy(m_observationChannel);
    auto* channelRow = new QHBoxLayout;
    channelRow->addWidget(channelLabel); channelRow->addWidget(m_observationChannel, 1);
    mainLayout->addLayout(channelRow);
    connect(m_observationChannel, &QComboBox::currentIndexChanged, this, [this] { RefreshObservationChannel(); });
    m_observationTable = new QTableWidget(0, 6, wrapper);
    m_observationTable->setObjectName(QStringLiteral("physicsObservationTable"));
    m_observationTable->setAccessibleName(QStringLiteral("物理输入输出实时数值"));
    m_observationTable->setHorizontalHeaderLabels({QStringLiteral("通道"), QStringLiteral("参数"), QStringLiteral("实值"),
        QStringLiteral("范围"), QStringLiteral("原始 → 裁剪"), QStringLiteral("状态")});
    m_observationTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_observationTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_observationTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_observationTable->verticalHeader()->hide();
    m_observationTable->horizontalHeader()->setStretchLastSection(true);
    fitPhysicsRows(m_observationTable);
    mainLayout->addWidget(m_observationTable);
    m_bakeButton = new QPushButton(QStringLiteral("烘焙物理到时间轴…"), wrapper);
    m_bakeButton->setObjectName(QStringLiteral("bakePhysicsButton"));
    m_bakeButton->setEnabled(false);
    connect(m_bakeButton, &QPushButton::clicked, this, &CPhysicsPanel::BakeRequested);
    mainLayout->addWidget(m_bakeButton);
    auto* fpsRow = new QHBoxLayout;
    m_fpsLabel = new QLabel(QStringLiteral("计算 FPS"), wrapper);
    m_fpsLabel->setObjectName(QStringLiteral("physicsFpsLabel"));
    m_fpsEdit = new QLineEdit(wrapper);
    m_fpsEdit->setObjectName(QStringLiteral("physicsFps"));
    m_fpsEdit->setToolTip(QStringLiteral("0 表示跟随运行时；有效范围 0–1000"));
    fpsRow->addWidget(m_fpsLabel); fpsRow->addWidget(m_fpsEdit);
    mainLayout->addLayout(fpsRow);
    connect(m_fpsEdit, &QLineEdit::editingFinished, this, [this] {
        const QScopedValueRollback<QSet<const QLineEdit*>> committing(m_committingControls, {m_fpsEdit});
        if (m_updating || m_physicsDocument.isEmpty()) return;
        bool valid = false; const double value = m_fpsEdit->text().toDouble(&valid);
        if (!valid || !std::isfinite(value) || value < 0 || value > 1000) { RejectEdit(QStringLiteral("计算 FPS 必须为 0–1000 的有限数值")); return; }
        Snapshot before; QString error;
        if (!CaptureSnapshot(before, &error)) { RejectEdit(error); return; }
        auto document = before.document; auto meta = document.value("Meta").toObject();
        if (meta.value("Fps").toDouble(0) == value) {
            m_fpsEdit->setText(m_fpsEdit->property("physicsAcceptedText").toString());
            m_errorLabel->clear(); m_errorLabel->hide(); QToolTip::hideText(); return;
        }
        meta.insert("Fps", value); document.insert("Meta", meta);
        CommitDocument(before, document, before.selectedGroupRow);
    });
    m_errorLabel = new QLabel(wrapper);
    m_errorLabel->setObjectName(QStringLiteral("physicsEditError"));
    m_errorLabel->setTextFormat(Qt::PlainText); m_errorLabel->setWordWrap(true);
    m_errorLabel->setTextInteractionFlags(Qt::TextSelectableByMouse); m_errorLabel->hide();
    mainLayout->addWidget(m_errorLabel);

    // 重力/风力控制
    QGroupBox* forceBox = new QGroupBox("力场控制", wrapper);
    QVBoxLayout* forceLayout = new QVBoxLayout(forceBox);

    int forceRow = 0;
    auto makeSliderRow = [&](const QString& label, QSlider*& slider, QLabel*& valLabel) {
        QHBoxLayout* row = new QHBoxLayout();
        row->addWidget(new QLabel(label, forceBox));
        slider = new QSlider(Qt::Horizontal, forceBox);
        slider->setRange(-100, 100);
        slider->setValue(0);
        row->addWidget(slider);
        valLabel = new QLabel("0.00", forceBox);
        valLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        valLabel->hide();
        auto* edit = new QLineEdit(forceBox);
        m_forceEdits[forceRow] = edit;
        edit->setObjectName(QStringLiteral("physicsForceValue%1").arg(forceRow++));
        edit->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        row->addWidget(edit);
        forceLayout->addLayout(row);
    };

    makeSliderRow("重力 X", m_gravityX, m_gravityXLabel);
    makeSliderRow("重力 Y", m_gravityY, m_gravityYLabel);
    makeSliderRow("风力 X", m_windX, m_windXLabel);
    makeSliderRow("风力 Y", m_windY, m_windYLabel);
    m_gravityX->setObjectName(QStringLiteral("physicsGravityX"));
    m_gravityY->setObjectName(QStringLiteral("physicsGravityY"));
    m_windX->setObjectName(QStringLiteral("physicsWindX"));
    m_windY->setObjectName(QStringLiteral("physicsWindY"));
    mainLayout->addWidget(forceBox);

    auto connectSlider = [this](QSlider* slider, bool isGravity, bool isX) {
        slider->installEventFilter(this);
        connect(slider, &QSlider::sliderPressed, this, [this, slider] {
            m_forceMergeKey = slider->objectName() + QString::number(++m_forceGesture);
        });
        connect(slider, &QSlider::sliderReleased, this, [this] { m_forceMergeKey.clear(); });
        connect(slider, &QSlider::valueChanged, this, [this, isGravity, isX](int val) {
            if (m_updating || m_physicsDocument.isEmpty()) return;
            updateForceDocument(isGravity, isX, val / 100.0);
        });
    };
    connectSlider(m_gravityX, true, true); connectSlider(m_gravityY, true, false);
    connectSlider(m_windX, false, true); connectSlider(m_windY, false, false);

    for (int i = 0; i < 4; ++i) {
        auto* edit = m_forceEdits[i];
        connect(edit, &QLineEdit::editingFinished, this, [this, edit, i] {
            const QScopedValueRollback<QSet<const QLineEdit*>> committing(m_committingControls, {edit});
            if (m_updating || m_physicsDocument.isEmpty()) return;
            bool valid = false; const double value = edit->text().toDouble(&valid);
            if (!valid || !std::isfinite(value) || std::abs(value) > 100000) {
                RejectEdit(QStringLiteral("力场必须为 -100000 至 100000 的有限数值")); return;
            }
            updateForceDocument(i < 2, i % 2 == 0, value);
        });
    }


    // 物理组列表
    QGroupBox* groupBox = new QGroupBox("物理组", wrapper);
    QVBoxLayout* groupLayout = new QVBoxLayout(groupBox);

    auto* groupTools = new QGridLayout();
    m_addGroupButton = new QPushButton(QStringLiteral("添加组"), groupBox);
    m_removeGroupButton = new QPushButton(QStringLiteral("删除组"), groupBox);
    m_renameGroupButton = new QPushButton(QStringLiteral("重命名"), groupBox);
    m_addGroupButton->setObjectName(QStringLiteral("addPhysicsGroup"));
    m_removeGroupButton->setObjectName(QStringLiteral("removePhysicsGroup"));
    m_renameGroupButton->setObjectName(QStringLiteral("renamePhysicsGroup"));
    m_addGroupButton->setEnabled(false);
    m_removeGroupButton->setEnabled(false);
    m_renameGroupButton->setEnabled(false);
    groupTools->addWidget(m_addGroupButton, 0, 0);
    groupTools->addWidget(m_removeGroupButton, 0, 1);
    groupTools->addWidget(m_renameGroupButton, 1, 0);
    m_copyGroupButton = new QPushButton(QStringLiteral("复制组"), groupBox);
    m_copyGroupButton->setObjectName(QStringLiteral("copyPhysicsGroup"));
    groupTools->addWidget(m_copyGroupButton, 1, 1);
    connect(m_copyGroupButton, &QPushButton::clicked, this, &CPhysicsPanel::copyPhysicsGroup);

    groupLayout->addLayout(groupTools);

    m_groupTable = new QTableWidget(0, 3, groupBox);
    m_groupTable->setObjectName(QStringLiteral("physicsGroupTable"));
    m_groupTable->setHorizontalHeaderLabels({"ID", "名称", "输出数"});
    m_groupTable->horizontalHeader()->setStretchLastSection(true);
    m_groupTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_groupTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_groupTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_groupTable->verticalHeader()->setVisible(false);
    groupLayout->addWidget(m_groupTable);

    m_dragModeCb = new QCheckBox("拖拽驱动选中组", groupBox);
    m_dragModeCb->setObjectName(QStringLiteral("physicsDragMode"));
    m_dragModeCb->setToolTip("勾选后，在模型上拖拽鼠标可驱动选中物理组的输入参数");
    groupLayout->addWidget(m_dragModeCb);

    connect(m_groupTable, &QTableWidget::currentCellChanged, this, [this](int row, int, int, int){
        if (!m_updating) {
            if (m_publishing || HasPendingCellEdits()) {
                {
                    const QSignalBlocker table(m_groupTable), picker(m_observationGroup);
                    m_groupTable->setCurrentCell(m_detailGroupRow, m_detailGroupRow >= 0 ? 0 : -1);
                    m_observationGroup->setCurrentIndex(m_detailGroupRow);
                }
                RejectEdit(QStringLiteral("请先完成或取消正在编辑的物理内容，再切换物理组"));
                return;
            }
            showGroupDetail(row);
        }
    });
    connect(m_dragModeCb, &QCheckBox::toggled, this, [this](){
        if (!m_updating) emitDragParams(m_groupTable->currentRow());
    });

    mainLayout->addWidget(groupBox);

    auto* normalizationBox = new QGroupBox(QStringLiteral("输入归一化"), wrapper);
    auto* normalizationLayout = new QGridLayout(normalizationBox);
    const QStringList limits{QStringLiteral("最小值"), QStringLiteral("默认值"), QStringLiteral("最大值")};
    for (int column = 0; column < 3; ++column) normalizationLayout->addWidget(new QLabel(limits[column], normalizationBox), 0, column + 1);
    for (int row = 0; row < 2; ++row) {
        normalizationLayout->addWidget(new QLabel(row == 0 ? QStringLiteral("位置") : QStringLiteral("角度"), normalizationBox), row + 1, 0);
        for (int column = 0; column < 3; ++column) {
            auto* edit = new QLineEdit(normalizationBox); m_normalizationEdits[row * 3 + column] = edit;
            edit->setObjectName(QStringLiteral("physicsNormalization%1").arg(row * 3 + column));
            edit->setMinimumWidth(edit->fontMetrics().horizontalAdvance(QStringLiteral("-100")));
            edit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            normalizationLayout->addWidget(edit, row + 1, column + 1);
        }
    }
    auto* applyNormalization = new QPushButton(QStringLiteral("应用归一化"), normalizationBox);
    applyNormalization->setObjectName(QStringLiteral("applyPhysicsNormalization"));
    normalizationLayout->addWidget(applyNormalization, 3, 1, 1, 3);
    connect(applyNormalization, &QPushButton::clicked, this, &CPhysicsPanel::ApplyNormalization);
    mainLayout->addWidget(normalizationBox);

    // 详情区域（输入/输出/摆锤）
    QSplitter* detailSplitter = new QSplitter(Qt::Vertical, wrapper);
    detailSplitter->setChildrenCollapsible(false);

    // 输入设置
    auto* inputSection = new QWidget(wrapper);
    auto* inputSectionLayout = new QVBoxLayout(inputSection);
    auto* inputTools = new QHBoxLayout();
    m_addInputButton = new QPushButton(QStringLiteral("添加输入"), inputSection);
    m_removeInputButton = new QPushButton(QStringLiteral("删除输入"), inputSection);
    m_addInputButton->setObjectName(QStringLiteral("addPhysicsInput"));
    m_removeInputButton->setObjectName(QStringLiteral("removePhysicsInput"));
    m_addInputButton->setEnabled(false);
    m_removeInputButton->setEnabled(false);
    inputTools->addWidget(m_addInputButton);
    inputTools->addWidget(m_removeInputButton);
    inputTools->addStretch(1);
    inputSectionLayout->addLayout(inputTools);
    m_inputTable = new QTableWidget(0, 4, inputSection);
    m_inputTable->setObjectName(QStringLiteral("physicsInputTable"));
    m_inputTable->setHorizontalHeaderLabels({"参数", "类型", "影响度", "反转"});
    m_inputTable->horizontalHeader()->setStretchLastSection(true);
    m_inputTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                  QAbstractItemView::SelectedClicked);
    m_inputTable->verticalHeader()->setVisible(false);
    inputSectionLayout->addWidget(m_inputTable);
    detailSplitter->addWidget(inputSection);

    // 输出设置
    auto* outputSection = new QWidget(wrapper);
    auto* outputSectionLayout = new QVBoxLayout(outputSection);
    auto* outputTools = new QHBoxLayout();
    m_addOutputButton = new QPushButton(QStringLiteral("添加输出"), outputSection);
    m_removeOutputButton = new QPushButton(QStringLiteral("删除输出"), outputSection);
    m_addOutputButton->setObjectName(QStringLiteral("addPhysicsOutput"));
    m_removeOutputButton->setObjectName(QStringLiteral("removePhysicsOutput"));
    m_addOutputButton->setEnabled(false);
    m_removeOutputButton->setEnabled(false);
    outputTools->addWidget(m_addOutputButton);
    outputTools->addWidget(m_removeOutputButton);
    outputTools->addStretch(1);
    outputSectionLayout->addLayout(outputTools);
    m_outputTable = new QTableWidget(0, 6, outputSection);
    m_outputTable->setObjectName(QStringLiteral("physicsOutputTable"));
    m_outputTable->setHorizontalHeaderLabels({"参数", "类型", "影响度", "倍率", "反转", "摆锤索引"});
    m_outputTable->horizontalHeader()->setStretchLastSection(true);
    m_outputTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                   QAbstractItemView::SelectedClicked);
    m_outputTable->verticalHeader()->setVisible(false);
    outputSectionLayout->addWidget(m_outputTable);
    detailSplitter->addWidget(outputSection);

    // 摆锤参数
    auto* vertexSection = new QWidget(wrapper);
    auto* vertexSectionLayout = new QVBoxLayout(vertexSection);
    auto* vertexTools = new QHBoxLayout();
    m_addVertexButton = new QPushButton(QStringLiteral("添加摆锤"), vertexSection);
    m_removeVertexButton = new QPushButton(QStringLiteral("删除摆锤"), vertexSection);
    m_addVertexButton->setObjectName(QStringLiteral("addPhysicsVertex"));
    m_removeVertexButton->setObjectName(QStringLiteral("removePhysicsVertex"));
    m_addVertexButton->setEnabled(false);
    m_removeVertexButton->setEnabled(false);
    vertexTools->addWidget(m_addVertexButton);
    vertexTools->addWidget(m_removeVertexButton);
    vertexTools->addStretch(1);
    vertexSectionLayout->addLayout(vertexTools);
    auto* presetRow = new QHBoxLayout;
    m_particlePreset = new QComboBox(vertexSection);
    m_particlePreset->setObjectName(QStringLiteral("physicsParticlePreset"));
    m_particlePreset->setAccessibleName(QStringLiteral("摆锤预设"));
    m_particlePreset->addItems({QStringLiteral("轻"), QStringLiteral("标准"), QStringLiteral("重")});
    m_particlePreset->setCurrentIndex(1);
    m_particlePreset->setToolTip(QStringLiteral("仅设置可动节点的摆幅度、反应速度和收敛速度；不改变长度、位置或输入输出"));
    m_applyPreset = new QPushButton(QStringLiteral("应用预设"), vertexSection);
    m_applyPreset->setObjectName(QStringLiteral("applyPhysicsPreset"));
    m_applyPreset->setEnabled(false);
    presetRow->addWidget(m_particlePreset, 1); presetRow->addWidget(m_applyPreset);
    vertexSectionLayout->addLayout(presetRow);
    connect(m_applyPreset, &QPushButton::clicked, this, &CPhysicsPanel::ApplyParticlePreset);
    m_vertexTable = new QTableWidget(0, 5, vertexSection);
    m_vertexTable->setObjectName(QStringLiteral("physicsVertexTable"));
    m_vertexTable->setHorizontalHeaderLabels({"序号", "长度", "摆幅度", "反应速度", "收敛速度"});
    m_vertexTable->horizontalHeader()->setStretchLastSection(true);
    m_vertexTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                   QAbstractItemView::SelectedClicked);
    m_vertexTable->verticalHeader()->setVisible(false);
    vertexSectionLayout->addWidget(m_vertexTable);
    detailSplitter->addWidget(vertexSection);

    mainLayout->addWidget(detailSplitter);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("physicsWorkspaceScroll"));
    scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(wrapper); setWidget(scroll);
    for (auto* table : {m_groupTable, m_inputTable, m_outputTable, m_vertexTable}) table->installEventFilter(this);
    const std::array<QTableWidget*, 3> editTables{m_inputTable, m_outputTable, m_vertexTable};
    for (int index = 0; index < 3; ++index) {
        m_editDelegates[index] = new CPhysicsEditDelegate(editTables[index],
            [this, index](const QModelIndex& cell, const QString& text, quint64 revision, QString* error) {
                const QScopedValueRollback<bool> committing(m_committingCell, true);
                return SubmitCell(index, cell.row(), cell.column(), text, revision, error);
            }, [this] { return m_documentRevision; }, [this] { RefreshAfterEditor(); });
        editTables[index]->setItemDelegate(m_editDelegates[index]);
    }
    connect(m_inputTable, &QTableWidget::cellChanged, this, &CPhysicsPanel::updateInputCell);
    connect(m_outputTable, &QTableWidget::cellChanged, this, &CPhysicsPanel::updateOutputCell);
    connect(m_vertexTable, &QTableWidget::cellChanged, this, &CPhysicsPanel::updateVertexCell);
    connect(m_addInputButton, &QPushButton::clicked, this, &CPhysicsPanel::addPhysicsInput);
    connect(m_removeInputButton, &QPushButton::clicked, this, &CPhysicsPanel::removePhysicsInput);
    connect(m_addOutputButton, &QPushButton::clicked, this, &CPhysicsPanel::addPhysicsOutput);
    connect(m_removeOutputButton, &QPushButton::clicked, this, &CPhysicsPanel::removePhysicsOutput);
    connect(m_addVertexButton, &QPushButton::clicked, this, &CPhysicsPanel::addPhysicsVertex);
    connect(m_removeVertexButton, &QPushButton::clicked, this, &CPhysicsPanel::removePhysicsVertex);
    connect(m_inputTable, &QTableWidget::itemSelectionChanged, this, [this](){
        if (m_removeInputButton) m_removeInputButton->setEnabled(m_inputTable->currentRow() >= 0);
    });
    connect(m_outputTable, &QTableWidget::itemSelectionChanged, this, [this](){
        if (m_removeOutputButton) m_removeOutputButton->setEnabled(m_outputTable->currentRow() >= 0);
    });
    connect(m_vertexTable, &QTableWidget::itemSelectionChanged, this, [this](){
        if (m_removeVertexButton) m_removeVertexButton->setEnabled(m_vertexTable->currentRow() >= 0);
    });
    connect(m_addGroupButton, &QPushButton::clicked, this, &CPhysicsPanel::addPhysicsGroup);
    connect(m_removeGroupButton, &QPushButton::clicked, this, &CPhysicsPanel::removePhysicsGroup);
    connect(m_renameGroupButton, &QPushButton::clicked, this, &CPhysicsPanel::renamePhysicsGroup);
    RefreshHistoryControls();
}

bool CPhysicsPanel::LoadPhysics(const QString& physicsPath, QString* error)
{
    if (error) error->clear();
    if (m_publishing || HasPendingCellEdits()) {
        if (error) *error = QStringLiteral("请先完成或取消正在编辑的物理单元格");
        return false;
    }
    QFile file(physicsPath);
    if(!file.open(QIODevice::ReadOnly | QIODevice::Text)){
        qDebug() << "[PhysicsPanel] Failed to open" << physicsPath;
        if (error) *error = QStringLiteral("无法打开物理配置：%1").arg(file.errorString());
        return false;
    }

    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        if (error) *error = QStringLiteral("无法读取物理配置：%1").arg(file.errorString());
        return false;
    }
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
    file.close();
    if(parseError.error != QJsonParseError::NoError || doc.isNull() || !doc.isObject()) {
        if (error) *error = QStringLiteral("物理配置 JSON 无效：%1").arg(parseError.errorString());
        return false;
    }

    QJsonObject root = doc.object();
    if (!root.value(QStringLiteral("PhysicsSettings")).isArray()) {
        if (error) *error = QStringLiteral("物理配置缺少 PhysicsSettings 数组");
        return false;
    }
    QString validationError;
    if (!ValidateDocument(root, &validationError)) {
        if (error) *error = validationError;
        return false;
    }
    if (m_publishing) { if (error) *error = QStringLiteral("物理修改正在提交"); return false; }
    m_physicsDocument = root;
    m_physicsSettings = root.value("PhysicsSettings").toArray();
    m_physicsDictionary = physicsDictionary(root);
    m_physicsPath = physicsPath;
    m_savedDocument = normalizedPhysics(root); m_savedPath = physicsPath;
    m_history.reset(); m_dirty = false; ++m_documentRevision;
    auto snapshot = std::make_unique<Snapshot>();
    snapshot->document = root; snapshot->path = physicsPath; snapshot->revision = m_documentRevision;
    snapshot->selectedGroupRow = m_physicsSettings.isEmpty() ? -1 : 0;
    m_pendingSnapshotRefresh = std::move(snapshot); m_snapshotNotificationPending = true;
    { const QSignalBlocker blocker(this); NotifySnapshotCommitted(); }
    RefreshHistoryControls();
    emit ObservationGroupChanged(SelectedGroupIndex());
    return true;
}

void CPhysicsPanel::updateForceDocument(bool gravity, bool xAxis, double value)
{
    const auto restoreSliders = [this] {
        const std::array<QSlider*, 4> sliders{m_gravityX, m_gravityY, m_windX, m_windY};
        for (int i = 0; i < 4; ++i) {
            const QSignalBlocker blocker(sliders[i]);
            sliders[i]->setValue(qRound(physicsForceValue(m_physicsDocument, i < 2, i % 2 == 0) * 100));
        }
    };
    Snapshot before; QString error;
    if (m_physicsDocument.isEmpty() || !CaptureSnapshot(before, &error)) {
        restoreSliders(); if (!error.isEmpty()) RejectEdit(error); return;
    }
    if (physicsForceValue(before.document, gravity, xAxis) == value) {
        for (const auto* control : m_committingControls) {
            auto* edit = const_cast<QLineEdit*>(control);
            edit->setText(edit->property("physicsAcceptedText").toString());
        }
        m_errorLabel->clear(); m_errorLabel->hide(); QToolTip::hideText(); return;
    }
    QJsonObject document = before.document;
    auto meta = document.value("Meta").toObject(); auto forces = meta.value("EffectiveForces").toObject();
    const QString name = gravity ? QStringLiteral("Gravity") : QStringLiteral("Wind");
    auto force = forces.value(name).toObject(); force.insert(xAxis ? "X" : "Y", value);
    forces.insert(name, force); meta.insert("EffectiveForces", forces); document.insert("Meta", meta);
    if (!CommitDocument(before, document, before.selectedGroupRow, &error, m_forceMergeKey)) {
        restoreSliders();
    } else if (!m_editPublisher && before.document != m_physicsDocument) {
        if (gravity) emit GravityChanged(physicsForceValue(m_physicsDocument, true, true), physicsForceValue(m_physicsDocument, true, false));
        else emit WindChanged(physicsForceValue(m_physicsDocument, false, true), physicsForceValue(m_physicsDocument, false, false));
    }
}

QJsonObject CPhysicsPanel::DocumentSnapshot() const
{
    QJsonObject result = m_physicsDocument;
    if (!result.isEmpty()) result.insert(QStringLiteral("PhysicsSettings"), m_physicsSettings);
    return result;
}

bool CPhysicsPanel::SavePhysics(const QString& path, QString* error)
{
    if (error) error->clear();
    if (m_publishing || HasPendingCellEdits()) {
        if (error) *error = QStringLiteral("请先完成或取消正在编辑的物理单元格");
        return false;
    }
    if (path.isEmpty() && m_sessionSaveHandler) {
        try {
            const auto handler = m_sessionSaveHandler;
            QString detail;
            const bool accepted = handler(&detail);
            if (!accepted && error) *error = detail.isEmpty() ? QStringLiteral("物理草稿尚未保存到会话") : detail;
            return accepted;
        } catch (...) {
            if (error) *error = QStringLiteral("无法将物理草稿保存到会话");
            return false;
        }
    }
    QString target = path.isEmpty() ? m_physicsPath : path;
    if (target.isEmpty()) {
        target = QFileDialog::getSaveFileName(this, QStringLiteral("保存物理配置"), QString(),
                                              QStringLiteral("Physics (*.physics3.json);;JSON (*.json)"));
    }
    if (target.isEmpty() || m_physicsDocument.isEmpty()) return false;
    QString validationError;
    QJsonObject updated;
    if (!live2dstruct::CPhysicsDocument::Normalize(DocumentSnapshot(), updated, &validationError)) {
        if (error) *error = validationError;
        QMessageBox::warning(this, QStringLiteral("保存物理配置失败"), validationError);
        return false;
    }
    QJsonObject marker = updated;
    QString savedPath = target;
    QSaveFile file(target);
    const QByteArray data = QJsonDocument(updated).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error) *error = QStringLiteral("无法保存物理配置：%1").arg(file.errorString());
        QMessageBox::warning(this, QStringLiteral("保存物理配置失败"), target);
        return false;
    }
    m_physicsDocument = updated;
    m_snapshotNotificationPending = false;
    m_pendingSnapshotRefresh.reset();
    ++m_documentRevision;
    m_physicsPath = target;
    m_savedDocument.swap(marker); m_savedPath.swap(savedPath);
    m_dirty = false;
    emit PhysicsSaved(target);
    return true;
}

bool CPhysicsPanel::ExportPhysics(const QString& path, QString* error) const
{
    if (error) error->clear();
    Snapshot current;
    if (!CaptureSnapshot(current, error)) return false;
    if (path.isEmpty() || path.contains(QChar::Null) || current.document.isEmpty()) {
        if (error) *error = QStringLiteral("没有可导出的物理草稿或路径无效"); return false;
    }
    QJsonObject document;
    if (!live2dstruct::CPhysicsDocument::Normalize(current.document, document, error)) return false;
    const auto bytes = QJsonDocument(document).toJson(QJsonDocument::Indented);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = QStringLiteral("无法导出物理配置：%1").arg(file.errorString()); return false;
    }
    return true;
}

bool CPhysicsPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (auto* slider = qobject_cast<QSlider*>(watched)) {
        if (event->type() == QEvent::KeyPress) {
            const auto* key = static_cast<QKeyEvent*>(event);
            if (!key->isAutoRepeat() && (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right ||
                key->key() == Qt::Key_Up || key->key() == Qt::Key_Down || key->key() == Qt::Key_PageUp ||
                key->key() == Qt::Key_PageDown || key->key() == Qt::Key_Home || key->key() == Qt::Key_End))
                m_forceMergeKey = slider->objectName() + QString::number(++m_forceGesture);
        } else if (event->type() == QEvent::FocusOut || event->type() == QEvent::Hide ||
                   (event->type() == QEvent::KeyRelease && !static_cast<QKeyEvent*>(event)->isAutoRepeat()))
            m_forceMergeKey.clear();
    }
    if (auto* table = qobject_cast<QTableWidget*>(watched)) {
        if (event->type() == QEvent::Show || event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
            QTimer::singleShot(0, table, [table] { fitPhysicsRows(table); });
    }
    return QDockWidget::eventFilter(watched, event);
}

void CPhysicsPanel::SetSessionSaveHandler(std::function<bool(QString*)> handler)
{
    m_sessionSaveHandler = std::move(handler);
    m_saveButton->setText(m_sessionSaveHandler ? QStringLiteral("保存到会话") : QStringLiteral("保存物理配置"));
}

bool CPhysicsPanel::ValidateDocument(QString* error) const
{
    return ValidateDocument(DocumentSnapshot(), error);
}

bool CPhysicsPanel::ValidateDocument(const QJsonObject& document, QString* error)
{
    return live2dstruct::CPhysicsDocument::Validate(document, error);
}

bool CPhysicsPanel::SubmitCell(int table, int row, int column, const QString& text, quint64 revision, QString* error)
{
    QString detail;
    if (!error) error = &detail;
    const auto fail = [&](const QString& message) { *error = message; RejectEdit(message); return false; };
    if (m_publishing || revision != m_documentRevision) return fail(QStringLiteral("物理草稿已改变，请重新编辑"));
    Snapshot before;
    if (!CaptureAcceptedSnapshot(before, error)) return fail(*error);
    const int groupRow = before.selectedGroupRow;
    if (groupRow < 0 || groupRow >= m_physicsSettings.size()) return fail(QStringLiteral("物理组已不存在"));
    auto settings = m_physicsSettings;
    auto group = settings[groupRow].toObject();
    const QString field = table == 0 ? QStringLiteral("Input") : table == 1 ? QStringLiteral("Output") : QStringLiteral("Vertices");
    auto entries = group.value(field).toArray();
    const int index = row + (table == 2 ? 1 : 0);
    if (index < 0 || index >= entries.size()) return fail(QStringLiteral("物理行已不存在"));
    auto entry = entries[index].toObject();
    if (table < 2 && column == 0) {
        const QString id = text.trimmed();
        if (id.isEmpty() || id.contains(QChar::Null)) return fail(QStringLiteral("参数 ID 不能为空或包含空字符"));
        const QString key = table == 0 ? QStringLiteral("Source") : QStringLiteral("Destination");
        auto target = entry.value(key).toObject(); target.insert("Id", id); entry.insert(key, target);
    } else if (table < 2 && column == 1) {
        const QString type = text.trimmed();
        if (type != "X" && type != "Y" && type != "Angle") return fail(QStringLiteral("类型必须为 X、Y 或 Angle"));
        entry.insert("Type", type);
    } else if ((table == 0 && column == 3) || (table == 1 && column == 4)) {
        const QString value = text.trimmed().toLower();
        if (value == "是" || value == "true" || value == "1") entry.insert("Reflect", true);
        else if (value == "否" || value == "false" || value == "0") entry.insert("Reflect", false);
        else return fail(QStringLiteral("反转必须为 是 或 否"));
    } else {
        bool valid = false; const double value = text.toDouble(&valid);
        if (!valid || !std::isfinite(value)) return fail(QStringLiteral("请输入有限数值，支持科学计数法"));
        QString key;
        if (table == 0 && column == 2) key = QStringLiteral("Weight");
        else if (table == 1) {
            if (column == 2) key = QStringLiteral("Weight");
            else if (column == 3) key = QStringLiteral("Scale");
            else if (column == 5) key = QStringLiteral("VertexIndex");
        } else if (table == 2 && column >= 1 && column <= 4)
            key = QStringList{"Radius", "Mobility", "Delay", "Acceleration"}[column - 1];
        if (key.isEmpty()) return fail(QStringLiteral("该单元格不可编辑"));
        entry.insert(key, value);
    }
    entries[index] = entry; group.insert(field, entries); settings[groupRow] = group;
    auto document = before.document; document.insert("PhysicsSettings", settings);
    return CommitDocument(before, document, groupRow, error);
}

void CPhysicsPanel::updateInputCell(int row, int column)
{
    if (m_updating || !m_inputTable->item(row, column)) return;
    QString error;
    if (!SubmitCell(0, row, column, m_inputTable->item(row, column)->text(), m_documentRevision, &error)) {
        const QScopedValueRollback<bool> updating(m_updating, true); showGroupDetail(m_detailGroupRow);
    }
}

void CPhysicsPanel::updateOutputCell(int row, int column)
{
    if (m_updating || !m_outputTable->item(row, column)) return;
    QString error;
    if (!SubmitCell(1, row, column, m_outputTable->item(row, column)->text(), m_documentRevision, &error)) {
        const QScopedValueRollback<bool> updating(m_updating, true); showGroupDetail(m_detailGroupRow);
    }
}

void CPhysicsPanel::updateVertexCell(int row, int column)
{
    if (m_updating || !m_vertexTable->item(row, column)) return;
    QString error;
    if (!SubmitCell(2, row, column, m_vertexTable->item(row, column)->text(), m_documentRevision, &error)) {
        const QScopedValueRollback<bool> updating(m_updating, true); showGroupDetail(m_detailGroupRow);
    }
}

void CPhysicsPanel::ApplyNormalization()
{
    QSet<const QLineEdit*> controls;
    for (auto* edit : m_normalizationEdits) controls.insert(edit);
    const QScopedValueRollback<QSet<const QLineEdit*>> committing(m_committingControls, controls);
    Snapshot before; QString error;
    if (!CaptureSnapshot(before, &error)) { RejectEdit(error); return; }
    if (before.selectedGroupRow < 0) return;
    auto settings = m_physicsSettings; auto group = settings[before.selectedGroupRow].toObject();
    auto normalization = group.value("Normalization").toObject();
    const QStringList keys{"Minimum", "Default", "Maximum"};
    for (int row = 0; row < 2; ++row) {
        const QString kind = row == 0 ? QStringLiteral("Position") : QStringLiteral("Angle");
        auto values = normalization.value(kind).toObject();
        for (int column = 0; column < 3; ++column) {
            bool valid = false; const double value = m_normalizationEdits[row * 3 + column]->text().toDouble(&valid);
            if (!valid || !std::isfinite(value)) { RejectEdit(QStringLiteral("归一化范围必须为有限数值")); return; }
            values.insert(keys[column], value);
        }
        normalization.insert(kind, values);
    }
    group.insert("Normalization", normalization); settings[before.selectedGroupRow] = group;
    auto document = before.document; document.insert("PhysicsSettings", settings);
    CommitDocument(before, document, before.selectedGroupRow);
}

void CPhysicsPanel::copyPhysicsGroup()
{
    Snapshot before; QString error;
    if (!CaptureSnapshot(before, &error)) { RejectEdit(error); return; }
    const int row = before.selectedGroupRow;
    if (row < 0 || row >= m_physicsSettings.size() || m_physicsSettings.size() >= 10000) return;
    const QPointer<CPhysicsPanel> alive(this);
    auto group = m_physicsSettings[row].toObject();
    bool accepted = false;
    const QString id = QInputDialog::getText(this, QStringLiteral("复制物理组"), QStringLiteral("新组 ID"),
        QLineEdit::Normal, group.value("Id").toString() + QStringLiteral("Copy"), &accepted).trimmed();
    if (!alive || !accepted) return;
    if (id.isEmpty() || id.contains(QChar::Null)) { RejectEdit(QStringLiteral("组 ID 无效")); return; }
    auto settings = before.document.value("PhysicsSettings").toArray();
    for (const auto& value : settings) if (value.toObject().value("Id").toString() == id) {
        RejectEdit(QStringLiteral("该组 ID 已存在")); return;
    }
    group.insert("Id", id); settings.append(group);
    auto document = before.document; auto meta = document.value("Meta").toObject();
    auto dictionary = meta.value("PhysicsDictionary").toArray();
    dictionary.append(QJsonObject{{"Id", id}, {"Name", id}}); meta.insert("PhysicsDictionary", dictionary);
    document.insert("Meta", meta); document.insert("PhysicsSettings", settings);
    CommitDocument(before, document, settings.size() - 1);
}

void CPhysicsPanel::refreshGroupTable(int selectedRow, bool preserveNoSelection)
{
    if (!m_groupTable) return;
    const QSignalBlocker blocker(m_groupTable), picker(m_observationGroup);
    const bool wasUpdating = m_updating;
    m_updating = true;
    QMap<QString, QString> idToName;
    for (const QJsonValue& value : m_physicsDictionary) {
        const QJsonObject entry = value.toObject();
        idToName.insert(entry.value(QStringLiteral("Id")).toString(), entry.value(QStringLiteral("Name")).toString());
    }
    m_groupTable->setRowCount(m_physicsSettings.size());
    m_observationGroup->clear();
    m_observationGroup->setEnabled(!m_physicsSettings.isEmpty());
    for (int i = 0; i < m_physicsSettings.size(); ++i) {
        const QJsonObject setting = m_physicsSettings.at(i).toObject();
        const QString id = setting.value(QStringLiteral("Id")).toString();
        const QString name = idToName.value(id, id);
        const QString label = name.isEmpty() || name == id ? id : QStringLiteral("%1（%2）").arg(name, id);
        m_observationGroup->addItem(label.isEmpty() ? QStringLiteral("组 %1").arg(i + 1) : label, id);
        m_groupTable->setItem(i, 0, new QTableWidgetItem(id));
        m_groupTable->setItem(i, 1, new QTableWidgetItem(idToName.value(id, id)));
        m_groupTable->setItem(i, 2, new QTableWidgetItem(QString::number(setting.value(QStringLiteral("Output")).toArray().size())));
    }
    m_groupTable->resizeColumnsToContents();
    fitPhysicsRows(m_groupTable);
    selectedRow = m_physicsSettings.isEmpty() || (preserveNoSelection && selectedRow < 0) ? -1 : qBound(0, selectedRow, static_cast<int>(m_physicsSettings.size()) - 1);
    m_groupTable->setCurrentCell(selectedRow, selectedRow >= 0 ? 0 : -1);
    if (selectedRow >= 0) m_groupTable->selectRow(selectedRow);
    m_updating = wasUpdating;
    showGroupDetail(selectedRow);
}

void CPhysicsPanel::addPhysicsGroup()
{
    Snapshot before; QString error;
    if (!CaptureSnapshot(before, &error)) { RejectEdit(error); return; }
    const QPointer<CPhysicsPanel> alive(this);
    QJsonArray settings = m_physicsSettings, dictionary = m_physicsDictionary;
    if (m_physicsDocument.isEmpty() || settings.size() >= 10000) return;
    bool accepted = false;
    const QString id = QInputDialog::getText(this, QStringLiteral("添加物理组"),
                                              QStringLiteral("组 ID"), QLineEdit::Normal,
                                              QString(), &accepted).trimmed();
    if (!alive || !accepted || id.isEmpty()) return;
    for (const QJsonValue& value : settings) {
        if (value.toObject().value(QStringLiteral("Id")).toString() == id) {
            QMessageBox::warning(this, QStringLiteral("添加物理组"), QStringLiteral("该组 ID 已存在"));
            return;
        }
    }
    const QString name = QInputDialog::getText(this, QStringLiteral("添加物理组"),
                                                QStringLiteral("显示名称"), QLineEdit::Normal,
                                                id, &accepted).trimmed();
    if (!alive || !accepted || name.isEmpty()) return;
    QJsonObject position;
    position.insert(QStringLiteral("Minimum"), -10.0);
    position.insert(QStringLiteral("Default"), 0.0);
    position.insert(QStringLiteral("Maximum"), 10.0);
    QJsonObject normalization;
    normalization.insert(QStringLiteral("Position"), position);
    normalization.insert(QStringLiteral("Angle"), position);
    QJsonObject rootPosition;
    rootPosition.insert(QStringLiteral("X"), 0.0);
    rootPosition.insert(QStringLiteral("Y"), 0.0);
    QJsonObject rootVertex;
    rootVertex.insert(QStringLiteral("Position"), rootPosition);
    rootVertex.insert(QStringLiteral("Mobility"), 1.0);
    rootVertex.insert(QStringLiteral("Delay"), 1.0);
    rootVertex.insert(QStringLiteral("Acceleration"), 1.0);
    rootVertex.insert(QStringLiteral("Radius"), 0.0);
    QJsonArray vertices;
    vertices.append(rootVertex);
    QJsonObject setting;
    setting.insert(QStringLiteral("Id"), id);
    setting.insert(QStringLiteral("Input"), QJsonArray());
    setting.insert(QStringLiteral("Output"), QJsonArray());
    setting.insert(QStringLiteral("Vertices"), vertices);
    setting.insert(QStringLiteral("Normalization"), normalization);
    settings.append(setting);
    QJsonObject dictionaryEntry;
    dictionaryEntry.insert(QStringLiteral("Id"), id);
    dictionaryEntry.insert(QStringLiteral("Name"), name);
    dictionary.append(dictionaryEntry);
    auto document = before.document;
    auto meta = document.value("Meta").toObject(); meta.insert("PhysicsDictionary", dictionary);
    document.insert("Meta", meta); document.insert("PhysicsSettings", settings);
    CommitDocument(before, document, settings.size() - 1);
}

void CPhysicsPanel::removePhysicsGroup()
{
    Snapshot before; QString error;
    if (!CaptureSnapshot(before, &error)) { RejectEdit(error); return; }
    const QPointer<CPhysicsPanel> alive(this);
    QJsonArray settings = m_physicsSettings, dictionary = m_physicsDictionary;
    const int row = m_groupTable ? m_groupTable->currentRow() : -1;
    if (row < 0 || row >= settings.size()) return;
    const QString id = settings.at(row).toObject().value(QStringLiteral("Id")).toString();
    settings.removeAt(row);
    for (int i = dictionary.size() - 1; i >= 0; --i) {
        if (dictionary.at(i).toObject().value(QStringLiteral("Id")).toString() == id)
            dictionary.removeAt(i);
    }
    auto document = before.document;
    auto meta = document.value("Meta").toObject(); meta.insert("PhysicsDictionary", dictionary);
    document.insert("Meta", meta); document.insert("PhysicsSettings", settings);
    CommitDocument(before, document, row);
}

void CPhysicsPanel::renamePhysicsGroup()
{
    Snapshot before; QString error;
    if (!CaptureSnapshot(before, &error)) { RejectEdit(error); return; }
    const QPointer<CPhysicsPanel> alive(this);
    QJsonArray settings = m_physicsSettings, dictionary = m_physicsDictionary;
    const int row = m_groupTable ? m_groupTable->currentRow() : -1;
    if (row < 0 || row >= settings.size()) return;
    const QString id = settings.at(row).toObject().value(QStringLiteral("Id")).toString();
    QString currentName = id;
    for (const QJsonValue& value : dictionary) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("Id")).toString() == id) { currentName = entry.value(QStringLiteral("Name")).toString(); break; }
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("重命名物理组"),
                                                QStringLiteral("显示名称"), QLineEdit::Normal,
                                                currentName, &accepted).trimmed();
    if (!alive || !accepted || name.isEmpty()) return;
    bool dictionaryFound = false;
    for (int i = 0; i < dictionary.size(); ++i) {
        QJsonObject entry = dictionary.at(i).toObject();
        if (entry.value(QStringLiteral("Id")).toString() != id) continue;
        entry.insert(QStringLiteral("Name"), name);
        dictionary[i] = entry;
        dictionaryFound = true;
        break;
    }
    if (!dictionaryFound) {
        QJsonObject entry;
        entry.insert(QStringLiteral("Id"), id);
        entry.insert(QStringLiteral("Name"), name);
        dictionary.append(entry);
    }
    auto document = before.document;
    auto meta = document.value("Meta").toObject(); meta.insert("PhysicsDictionary", dictionary);
    document.insert("Meta", meta); document.insert("PhysicsSettings", settings);
    CommitDocument(before, document, row);
}

void CPhysicsPanel::addPhysicsInput()
{
    Snapshot before; QString error;
    if (!CaptureSnapshot(before, &error)) { RejectEdit(error); return; }
    const QPointer<CPhysicsPanel> alive(this);
    QJsonArray settings = m_physicsSettings, dictionary = m_physicsDictionary;
    const int groupRow = m_groupTable ? m_groupTable->currentRow() : -1;
    if (groupRow < 0 || groupRow >= settings.size()) return;
    bool accepted = false;
    const QString id = QInputDialog::getText(this, QStringLiteral("添加物理输入"),
                                              QStringLiteral("参数 ID"), QLineEdit::Normal,
                                              QString(), &accepted).trimmed();
    if (!alive || !accepted || id.isEmpty()) return;
    const QStringList types = {QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Angle")};
    const QString type = QInputDialog::getItem(this, QStringLiteral("添加物理输入"),
                                                QStringLiteral("输入类型"), types, 0, false, &accepted);
    if (!alive || !accepted) return;
    const double weight = QInputDialog::getDouble(this, QStringLiteral("添加物理输入"),
                                                   QStringLiteral("影响度（0-100）"), 100.0, 0.0, 100.0, 2, &accepted);
    if (!alive || !accepted) return;
    QJsonObject source;
    source.insert(QStringLiteral("Target"), QStringLiteral("Parameter"));
    source.insert(QStringLiteral("Id"), id);
    QJsonObject input;
    input.insert(QStringLiteral("Source"), source);
    input.insert(QStringLiteral("Weight"), weight);
    input.insert(QStringLiteral("Type"), type);
    input.insert(QStringLiteral("Reflect"), false);
    QJsonObject setting = settings.at(groupRow).toObject();
    QJsonArray inputs = setting.value(QStringLiteral("Input")).toArray();
    inputs.append(input);
    setting.insert(QStringLiteral("Input"), inputs);
    settings[groupRow] = setting;
    auto document = before.document;
    auto meta = document.value("Meta").toObject(); meta.insert("PhysicsDictionary", dictionary);
    document.insert("Meta", meta); document.insert("PhysicsSettings", settings);
    CommitDocument(before, document, groupRow);
}

void CPhysicsPanel::removePhysicsInput()
{
    Snapshot before; QString error;
    if (!CaptureSnapshot(before, &error)) { RejectEdit(error); return; }
    const QPointer<CPhysicsPanel> alive(this);
    QJsonArray settings = m_physicsSettings, dictionary = m_physicsDictionary;
    const int groupRow = m_groupTable ? m_groupTable->currentRow() : -1;
    const int row = m_inputTable ? m_inputTable->currentRow() : -1;
    if (groupRow < 0 || groupRow >= settings.size() || row < 0) return;
    QJsonObject setting = settings.at(groupRow).toObject();
    QJsonArray inputs = setting.value(QStringLiteral("Input")).toArray();
    if (row >= inputs.size()) return;
    inputs.removeAt(row);
    setting.insert(QStringLiteral("Input"), inputs);
    settings[groupRow] = setting;
    auto document = before.document;
    auto meta = document.value("Meta").toObject(); meta.insert("PhysicsDictionary", dictionary);
    document.insert("Meta", meta); document.insert("PhysicsSettings", settings);
    CommitDocument(before, document, groupRow);
}

void CPhysicsPanel::addPhysicsOutput()
{
    Snapshot before; QString error;
    if (!CaptureSnapshot(before, &error)) { RejectEdit(error); return; }
    const QPointer<CPhysicsPanel> alive(this);
    QJsonArray settings = m_physicsSettings, dictionary = m_physicsDictionary;
    const int groupRow = m_groupTable ? m_groupTable->currentRow() : -1;
    if (groupRow < 0 || groupRow >= settings.size()) return;
    const QJsonObject current = settings.at(groupRow).toObject();
    const int vertexCount = current.value(QStringLiteral("Vertices")).toArray().size();
    if (vertexCount <= 0) {
        QMessageBox::warning(this, QStringLiteral("添加物理输出"), QStringLiteral("当前物理组没有可用摆锤"));
        return;
    }
    bool accepted = false;
    const QString id = QInputDialog::getText(this, QStringLiteral("添加物理输出"),
                                              QStringLiteral("参数 ID"), QLineEdit::Normal,
                                              QString(), &accepted).trimmed();
    if (!alive || !accepted || id.isEmpty()) return;
    const QStringList types = {QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Angle")};
    const QString type = QInputDialog::getItem(this, QStringLiteral("添加物理输出"),
                                                QStringLiteral("输出类型"), types, 0, false, &accepted);
    if (!alive || !accepted) return;
    const int vertexIndex = QInputDialog::getInt(this, QStringLiteral("添加物理输出"),
                                                 QStringLiteral("摆锤索引"), 0, 0, vertexCount - 1, 1, &accepted);
    if (!alive || !accepted) return;
    const double scale = QInputDialog::getDouble(this, QStringLiteral("添加物理输出"),
                                                 QStringLiteral("倍率"), 1.0, -100000.0, 100000.0, 3, &accepted);
    if (!alive || !accepted) return;
    QJsonObject destination;
    destination.insert(QStringLiteral("Target"), QStringLiteral("Parameter"));
    destination.insert(QStringLiteral("Id"), id);
    QJsonObject output;
    output.insert(QStringLiteral("Destination"), destination);
    output.insert(QStringLiteral("VertexIndex"), vertexIndex);
    output.insert(QStringLiteral("Scale"), scale);
    output.insert(QStringLiteral("Weight"), 100.0);
    output.insert(QStringLiteral("Type"), type);
    output.insert(QStringLiteral("Reflect"), false);
    QJsonObject setting = current;
    QJsonArray outputs = setting.value(QStringLiteral("Output")).toArray();
    outputs.append(output);
    setting.insert(QStringLiteral("Output"), outputs);
    settings[groupRow] = setting;
    auto document = before.document;
    auto meta = document.value("Meta").toObject(); meta.insert("PhysicsDictionary", dictionary);
    document.insert("Meta", meta); document.insert("PhysicsSettings", settings);
    CommitDocument(before, document, groupRow);
}

void CPhysicsPanel::removePhysicsOutput()
{
    Snapshot before; QString error;
    if (!CaptureSnapshot(before, &error)) { RejectEdit(error); return; }
    const QPointer<CPhysicsPanel> alive(this);
    QJsonArray settings = m_physicsSettings, dictionary = m_physicsDictionary;
    const int groupRow = m_groupTable ? m_groupTable->currentRow() : -1;
    const int row = m_outputTable ? m_outputTable->currentRow() : -1;
    if (groupRow < 0 || groupRow >= settings.size() || row < 0) return;
    QJsonObject setting = settings.at(groupRow).toObject();
    QJsonArray outputs = setting.value(QStringLiteral("Output")).toArray();
    if (row >= outputs.size()) return;
    outputs.removeAt(row);
    setting.insert(QStringLiteral("Output"), outputs);
    settings[groupRow] = setting;
    auto document = before.document;
    auto meta = document.value("Meta").toObject(); meta.insert("PhysicsDictionary", dictionary);
    document.insert("Meta", meta); document.insert("PhysicsSettings", settings);
    CommitDocument(before, document, groupRow);
}

void CPhysicsPanel::addPhysicsVertex()
{
    Snapshot before; QString error;
    if (!CaptureSnapshot(before, &error)) { RejectEdit(error); return; }
    const QPointer<CPhysicsPanel> alive(this);
    QJsonArray settings = m_physicsSettings, dictionary = m_physicsDictionary;
    const int groupRow = m_groupTable ? m_groupTable->currentRow() : -1;
    if (groupRow < 0 || groupRow >= settings.size()) return;
    QJsonObject setting = settings.at(groupRow).toObject();
    QJsonArray vertices = setting.value(QStringLiteral("Vertices")).toArray();
    if (vertices.size() >= 10000) return;
    QJsonObject position;
    position.insert(QStringLiteral("X"), 0.0);
    position.insert(QStringLiteral("Y"), 0.0);
    QJsonObject vertex;
    vertex.insert(QStringLiteral("Position"), position);
    vertex.insert(QStringLiteral("Mobility"), 0.5);
    vertex.insert(QStringLiteral("Delay"), 0.2);
    vertex.insert(QStringLiteral("Acceleration"), 0.3);
    vertex.insert(QStringLiteral("Radius"), 1.0);
    vertices.append(vertex);
    setting.insert(QStringLiteral("Vertices"), vertices);
    settings[groupRow] = setting;
    auto document = before.document;
    auto meta = document.value("Meta").toObject(); meta.insert("PhysicsDictionary", dictionary);
    document.insert("Meta", meta); document.insert("PhysicsSettings", settings);
    CommitDocument(before, document, groupRow);
}

void CPhysicsPanel::removePhysicsVertex()
{
    Snapshot before; QString error;
    if (!CaptureSnapshot(before, &error)) { RejectEdit(error); return; }
    const QPointer<CPhysicsPanel> alive(this);
    QJsonArray settings = m_physicsSettings, dictionary = m_physicsDictionary;
    const int groupRow = m_groupTable ? m_groupTable->currentRow() : -1;
    const int row = m_vertexTable ? m_vertexTable->currentRow() : -1;
    if (groupRow < 0 || groupRow >= settings.size() || row < 0) return;
    const int vertexIndex = row + 1;
    QJsonObject setting = settings.at(groupRow).toObject();
    QJsonArray vertices = setting.value(QStringLiteral("Vertices")).toArray();
    if (vertexIndex <= 0 || vertexIndex >= vertices.size()) return;
    QJsonArray outputs = setting.value(QStringLiteral("Output")).toArray();
    for (const QJsonValue& outputValue : outputs) {
        const int outputIndex = outputValue.toObject().value(QStringLiteral("VertexIndex")).toInt(-1);
        if (outputIndex == vertexIndex) {
            QMessageBox::warning(this, QStringLiteral("删除摆锤"), QStringLiteral("该摆锤仍被输出引用，不能删除"));
            return;
        }
    }
    for (int i = 0; i < outputs.size(); ++i) {
        QJsonObject output = outputs.at(i).toObject();
        const int outputIndex = output.value(QStringLiteral("VertexIndex")).toInt(-1);
        if (outputIndex > vertexIndex) output.insert(QStringLiteral("VertexIndex"), outputIndex - 1);
        outputs[i] = output;
    }
    vertices.removeAt(vertexIndex);
    setting.insert(QStringLiteral("Vertices"), vertices);
    setting.insert(QStringLiteral("Output"), outputs);
    settings[groupRow] = setting;
    auto document = before.document;
    auto meta = document.value("Meta").toObject(); meta.insert("PhysicsDictionary", dictionary);
    document.insert("Meta", meta); document.insert("PhysicsSettings", settings);
    CommitDocument(before, document, groupRow);
}

void CPhysicsPanel::showGroupDetail(int row)
{
    const int previousRow = m_detailGroupRow;
    m_detailGroupRow = row;
    const bool validGroup = row >= 0 && row < m_physicsSettings.size();
    {
        const QSignalBlocker picker(m_observationGroup);
        m_observationGroup->setCurrentIndex(validGroup ? row : -1);
    }
    const bool wasUpdating = m_updating;
    m_updating = true;

    const QJsonObject setting = validGroup ? m_physicsSettings.at(row).toObject() : QJsonObject();
    m_addInputButton->setEnabled(validGroup);
    m_addOutputButton->setEnabled(validGroup && !setting.value("Vertices").toArray().isEmpty());
    m_addVertexButton->setEnabled(validGroup);
    m_removeGroupButton->setEnabled(validGroup);
    m_renameGroupButton->setEnabled(validGroup);
    m_copyGroupButton->setEnabled(validGroup);
    const auto normalization = setting.value("Normalization").toObject();
    const QStringList keys{"Minimum", "Default", "Maximum"};
    for (int index = 0; index < 6; ++index) {
        auto* edit = m_normalizationEdits[index]; edit->setEnabled(validGroup);
        const auto values = normalization.value(index < 3 ? "Position" : "Angle").toObject();
        edit->setText(validGroup ? physicsNumber(values.value(keys[index % 3]).toDouble()) : QString());
        edit->setProperty("physicsAcceptedText", edit->text());
    }
    for (auto* table : {m_inputTable, m_outputTable, m_vertexTable}) {
        table->clearSelection();
        table->setCurrentCell(-1, -1);
    }
    if (m_graph) m_graph->SetGroup(setting);
    ClearObservation();
    m_applyPreset->setEnabled(validGroup && setting.value("Vertices").toArray().size() > 1);
    m_particlePreset->setEnabled(m_applyPreset->isEnabled());

    // 输入设置
    QJsonArray inputs = setting["Input"].toArray();
    m_inputTable->setRowCount(inputs.size());
    for(int i = 0; i < inputs.size(); i++){
        QJsonObject inp = inputs[i].toObject();
        QString paramId = inp["Source"].toObject()["Id"].toString();
        m_inputTable->setItem(i, 0, new QTableWidgetItem(paramId));
        m_inputTable->setItem(i, 1, new QTableWidgetItem(inp["Type"].toString()));
        m_inputTable->setItem(i, 2, new QTableWidgetItem(physicsNumber(inp["Weight"].toDouble())));
        m_inputTable->setItem(i, 3, new QTableWidgetItem(inp["Reflect"].toBool() ? "是" : "否"));
    }
    m_inputTable->resizeColumnsToContents();
    fitPhysicsRows(m_inputTable);

    // 输出设置
    QJsonArray outputs = setting["Output"].toArray();
    m_outputTable->setRowCount(outputs.size());
    for(int i = 0; i < outputs.size(); i++){
        QJsonObject out = outputs[i].toObject();
        QString paramId = out["Destination"].toObject()["Id"].toString();
        m_outputTable->setItem(i, 0, new QTableWidgetItem(paramId));
        m_outputTable->setItem(i, 1, new QTableWidgetItem(out["Type"].toString()));
        m_outputTable->setItem(i, 2, new QTableWidgetItem(physicsNumber(out["Weight"].toDouble())));
        m_outputTable->setItem(i, 3, new QTableWidgetItem(physicsNumber(out["Scale"].toDouble())));
        m_outputTable->setItem(i, 4, new QTableWidgetItem(out["Reflect"].toBool() ? "是" : "否"));
        m_outputTable->setItem(i, 5, new QTableWidgetItem(QString::number(out["VertexIndex"].toInt())));
    }
    m_outputTable->resizeColumnsToContents();
    fitPhysicsRows(m_outputTable);

    // 摆锤参数 (Vertices，跳过第一个固定点)
    QJsonArray vertices = setting["Vertices"].toArray();
    int pendulumCount = vertices.size() > 1 ? vertices.size() - 1 : 0;
    m_vertexTable->setRowCount(pendulumCount);
    for(int i = 1; i < vertices.size(); i++){
        QJsonObject v = vertices[i].toObject();
        int row = i - 1;
        m_vertexTable->setItem(row, 0, new QTableWidgetItem(QString::number(i)));
        m_vertexTable->item(row, 0)->setFlags(m_vertexTable->item(row, 0)->flags() & ~Qt::ItemIsEditable);
        m_vertexTable->setItem(row, 1, new QTableWidgetItem(physicsNumber(v["Radius"].toDouble())));
        m_vertexTable->setItem(row, 2, new QTableWidgetItem(physicsNumber(v["Mobility"].toDouble())));
        m_vertexTable->setItem(row, 3, new QTableWidgetItem(physicsNumber(v["Delay"].toDouble())));
        m_vertexTable->setItem(row, 4, new QTableWidgetItem(physicsNumber(v["Acceleration"].toDouble())));
    }
    m_vertexTable->resizeColumnsToContents();
    fitPhysicsRows(m_vertexTable);
    if (m_removeInputButton) m_removeInputButton->setEnabled(false);
    if (m_removeOutputButton) m_removeOutputButton->setEnabled(false);
    if (m_removeVertexButton) m_removeVertexButton->setEnabled(false);
    m_updating = wasUpdating;
    const QPointer<CPhysicsPanel> alive(this);
    if (previousRow != row) emit ObservationGroupChanged(SelectedGroupIndex());
    if (alive && !m_updating) emitDragParams(row);
}

void CPhysicsPanel::emitDragParams(int row)
{
    QStringList paramIds, types;
    if (m_dragModeCb->isChecked() && row >= 0 && row < m_physicsSettings.size()) {
        const QJsonArray inputs = m_physicsSettings.at(row).toObject().value("Input").toArray();
        for (const auto& value : inputs) {
            const QJsonObject input = value.toObject();
            paramIds << input.value("Source").toObject().value("Id").toString();
            types << input.value("Type").toString();
        }
    }
    emit PhysicsDragParamsChanged(paramIds, types);
}

bool CPhysicsPanel::HasPendingCellEdits() const
{
    for (const auto* delegate : m_editDelegates)
        if (delegate && delegate->HasPendingChanges()) return true;
    const auto pending = [this](const QLineEdit* edit) {
        return edit && edit->isEnabled() && !m_committingControls.contains(edit) &&
            edit->text() != edit->property("physicsAcceptedText").toString();
    };
    if (pending(m_fpsEdit)) return true;
    for (auto* edit : m_forceEdits) if (pending(edit)) return true;
    for (auto* edit : m_normalizationEdits) if (pending(edit)) return true;
    return false;
}

bool CPhysicsPanel::CaptureSnapshot(Snapshot& result, QString* error) const
{
    if (m_publishing || HasPendingCellEdits()) {
        if (error) *error = QStringLiteral("请先完成或取消正在编辑的物理单元格"); return false;
    }
    return CaptureAcceptedSnapshot(result, error);
}

bool CPhysicsPanel::CaptureAcceptedSnapshot(Snapshot& result, QString* error) const
{
    if (error) error->clear();
    if (m_updating || HasPendingCellEdits()) {
        if (error) *error = QStringLiteral("请先完成或取消正在编辑的物理单元格");
        return false;
    }
    try {
        Snapshot candidate;
        if (m_pendingSnapshotRefresh) candidate = *m_pendingSnapshotRefresh;
        candidate.document = DocumentSnapshot();
        candidate.path = m_physicsPath;
        candidate.dirty = m_dirty;
        candidate.revision = m_documentRevision;
        candidate.history = m_history;
        if (m_pendingSnapshotRefresh) { result = std::move(candidate); return true; }
        candidate.selectedGroupRow = m_groupTable->currentRow();
        if (candidate.selectedGroupRow >= 0 && candidate.selectedGroupRow < m_physicsSettings.size())
            candidate.selectedGroupId = m_physicsSettings[candidate.selectedGroupRow].toObject().value(QStringLiteral("Id")).toString();
        candidate.dragEnabled = m_dragModeCb->isChecked();
        const std::array<QTableWidget*, 3> tables{m_inputTable, m_outputTable, m_vertexTable};
        for (int index = 0; index < 3; ++index) {
            const auto* table = tables[index];
            auto& state = candidate.tables[index];
            state.current = QPoint(table->currentColumn(), table->currentRow());
            for (const auto& range : table->selectedRanges())
                state.selection.append(QRect(range.leftColumn(), range.topRow(), range.columnCount(), range.rowCount()));
            state.horizontalScroll = table->horizontalScrollBar()->value();
            state.verticalScroll = table->verticalScrollBar()->value();
        }
        result = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("无法保存物理草稿快照");
        return false;
    }
}

bool CPhysicsPanel::PrepareSnapshot(const Snapshot& expected, const Snapshot& target, PreparedSnapshot& prepared,
                                    QString* error) const
{
    if (error) error->clear();
    try {
        Snapshot current;
        if (!(m_committingCell ? CaptureAcceptedSnapshot(current, error) : CaptureSnapshot(current, error))) return false;
        if (!current.SameDocument(expected)) {
            if (error) *error = QStringLiteral("物理草稿已独立修改，不能覆盖当前内容");
            return false;
        }
        if (!target.document.isEmpty() && !ValidateDocument(target.document, error)) return false;
        if (target.path.contains(QChar::Null) || (target.document.isEmpty() && !target.path.isEmpty())) {
            if (error) *error = QStringLiteral("物理草稿路径或空快照状态无效");
            return false;
        }
        PreparedSnapshot candidate;
        candidate.m_expected = current;
        candidate.m_target = target;
        const auto settings = target.document.value(QStringLiteral("PhysicsSettings")).toArray();
        int row = target.selectedGroupRow;
        if (!target.selectedGroupId.isEmpty()) {
            if (row < 0 || row >= settings.size() || settings[row].toObject().value(QStringLiteral("Id")).toString() != target.selectedGroupId) {
                row = -1;
                for (int index = 0; index < settings.size(); ++index)
                    if (settings[index].toObject().value(QStringLiteral("Id")).toString() == target.selectedGroupId) { row = index; break; }
            }
            if (row < 0) {
                if (error) *error = QStringLiteral("物理快照的选中组已不存在");
                return false;
            }
        } else if (row < -1 || row >= settings.size()) {
            if (error) *error = QStringLiteral("物理快照的选中组无效");
            return false;
        }
        candidate.m_target.selectedGroupRow = row;
        candidate.m_target.selectedGroupId = row >= 0 ? settings[row].toObject().value(QStringLiteral("Id")).toString() : QString();
        const auto group = row >= 0 ? settings[row].toObject() : QJsonObject();
        const std::array<int, 3> rows{int(group.value("Input").toArray().size()), int(group.value("Output").toArray().size()),
            qMax(0, int(group.value("Vertices").toArray().size()) - 1)};
        const std::array<int, 3> columns{4, 6, 5};
        for (int table = 0; table < 3; ++table) {
            const auto& state = target.tables[table];
            const auto validCell = [&](const QPoint& cell) {
                return cell.x() >= 0 && cell.x() < columns[table] && cell.y() >= 0 && cell.y() < rows[table];
            };
            if ((state.current != QPoint(-1, -1) && !validCell(state.current)) || state.horizontalScroll < 0 || state.verticalScroll < 0) {
                if (error) *error = QStringLiteral("物理快照的单元格或滚动位置无效");
                return false;
            }
            for (const auto& selection : state.selection)
                if (selection.isEmpty() || !validCell(selection.topLeft()) || !validCell(selection.bottomRight())) {
                    if (error) *error = QStringLiteral("物理快照的单元格选区无效");
                    return false;
                }
        }
        if (target.history && (target.history->owner != this || target.history->states.isEmpty() ||
            target.history->cursor < 0 || target.history->cursor >= target.history->states.size() ||
            normalizedPhysics(target.history->states[target.history->cursor].document) != normalizedPhysics(target.document))) {
            if (error) *error = QStringLiteral("物理历史快照无效或属于其他面板"); return false;
        }
        candidate.m_target.dirty = normalizedPhysics(target.document) != m_savedDocument || target.path != m_savedPath;
        const bool changed = current.document != target.document || current.path != target.path || current.history != target.history;
        if ((changed && m_documentRevision == std::numeric_limits<quint64>::max()) || m_prepareSerial == std::numeric_limits<quint64>::max()) {
            if (error) *error = QStringLiteral("物理草稿版本计数已耗尽");
            return false;
        }
        candidate.m_target.revision = current.revision + (changed ? 1 : 0);
        candidate.m_document = candidate.m_target.document;
        candidate.m_normalizedDocument = normalizedPhysics(candidate.m_document);
        candidate.m_settings = candidate.m_target.document.value("PhysicsSettings").toArray();
        candidate.m_dictionary = physicsDictionary(candidate.m_target.document);
        candidate.m_path = candidate.m_target.path;
        candidate.m_refresh = std::make_unique<Snapshot>(candidate.m_target);
        candidate.m_owner = const_cast<CPhysicsPanel*>(this);
        candidate.m_token = ++m_prepareSerial;
        prepared = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("无法准备物理草稿修改");
        return false;
    }
}

bool CPhysicsPanel::PrepareReferenceRemoval(const Snapshot& expected, const QSet<QString>& ids,
                                            PreparedSnapshot& prepared, QString* error) const
{
    if (error) error->clear();
    try {
        for (const auto& id : ids) if (id.isEmpty() || id.contains(QChar::Null)) {
            if (error) *error = QStringLiteral("待移除的物理参数 ID 无效");
            return false;
        }
        Snapshot current;
        if (!(m_committingCell ? CaptureAcceptedSnapshot(current, error) : CaptureSnapshot(current, error))) return false;
        if (!current.SameDocument(expected)) {
            if (error) *error = QStringLiteral("物理草稿已独立修改，不能移除引用");
            return false;
        }
        Snapshot target = current;
        if (!current.document.isEmpty()) {
            live2dstruct::CParameterReferenceEditor::Prepared references;
            if (!live2dstruct::CParameterReferenceEditor::Prepare(live2dstruct::CParameterReferenceEditor::Kind::Physics,
                QJsonDocument(current.document).toJson(QJsonDocument::Compact), ids, references, error)) return false;
            target.document = references.effective;
            target.dirty = current.dirty || target.document != current.document;
            if (current.selectedGroupRow >= 0) {
                const auto group = current.document.value("PhysicsSettings").toArray()[current.selectedGroupRow].toObject();
                for (int table = 0; table < 2; ++table) {
                    const auto source = group.value(table == 0 ? "Input" : "Output").toArray();
                    QVector<int> indices(source.size(), -1);
                    int retained = 0;
                    for (int row = 0; row < source.size(); ++row) {
                        const auto id = source[row].toObject().value(table == 0 ? "Source" : "Destination").toObject().value("Id").toString();
                        if (!ids.contains(id)) indices[row] = retained++;
                    }
                    auto& state = target.tables[table];
                    const int selected = state.current.y();
                    state.current = selected >= 0 && selected < indices.size() && indices[selected] >= 0
                        ? QPoint(state.current.x(), indices[selected]) : QPoint(-1, -1);
                    state.selection.clear();
                    for (const auto& range : current.tables[table].selection)
                        for (int row = range.top(); row <= range.bottom(); ++row)
                            if (row >= 0 && row < indices.size() && indices[row] >= 0)
                                state.selection.append(QRect(range.left(), indices[row], range.width(), 1));
                }
            }
        }
        target.history.reset();
        return PrepareSnapshot(expected, target, prepared, error);
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("无法准备物理参数引用移除");
        return false;
    }
}

bool CPhysicsPanel::ValidatePreparedSnapshot(const PreparedSnapshot& prepared, QString* error) const
{
    if (error) error->clear();
    if (!prepared.m_token || prepared.m_owner != this) {
        if (error) *error = QStringLiteral("物理草稿候选不属于此面板或已提交");
        return false;
    }
    if (m_updating || HasPendingCellEdits()) {
        if (error) *error = QStringLiteral("请先完成或取消正在编辑的物理单元格");
        return false;
    }
    const auto& expected = prepared.m_expected;
    if (m_documentRevision != expected.revision || m_history != expected.history ||
        m_physicsPath != expected.path || m_physicsDocument != expected.document) {
        if (error) *error = QStringLiteral("物理草稿在准备后发生变化");
        return false;
    }
    return true;
}

void CPhysicsPanel::RefreshSnapshotControls(const Snapshot& snapshot)
{
    const auto meta = m_physicsDocument.value("Meta").toObject();
    m_fpsEdit->setEnabled(!m_physicsDocument.isEmpty());
    m_fpsEdit->setText(m_physicsDocument.isEmpty() ? QString() : physicsNumber(meta.value("Fps").toDouble(0)));
    m_fpsEdit->setProperty("physicsAcceptedText", m_fpsEdit->text());
    const std::array<QSlider*, 4> sliders{m_gravityX, m_gravityY, m_windX, m_windY};
    const std::array<QLabel*, 4> labels{m_gravityXLabel, m_gravityYLabel, m_windXLabel, m_windYLabel};
    for (int index = 0; index < 4; ++index) {
        const double value = physicsForceValue(m_physicsDocument, index < 2, index % 2 == 0);
        const int bound = qMax(100, int(std::ceil(std::abs(value) * 100)));
        if (!m_forceMergeKey.startsWith(sliders[index]->objectName())) sliders[index]->setRange(-bound, bound);
        sliders[index]->setValue(qRound(value * 100));
        labels[index]->setText(QString::number(value, 'g', 6));
        m_forceEdits[index]->setText(physicsNumber(value));
        m_forceEdits[index]->setProperty("physicsAcceptedText", m_forceEdits[index]->text());
        m_forceEdits[index]->setEnabled(!m_physicsDocument.isEmpty());
    }
    m_dragModeCb->setChecked(snapshot.dragEnabled);
    m_addGroupButton->setEnabled(!m_physicsDocument.isEmpty());
    refreshGroupTable(snapshot.selectedGroupRow, true);
    const std::array<QTableWidget*, 3> tables{m_inputTable, m_outputTable, m_vertexTable};
    for (int index = 0; index < 3; ++index) {
        auto* table = tables[index];
        const auto& state = snapshot.tables[index];
        table->setCurrentCell(state.current.y(), state.current.x(), QItemSelectionModel::NoUpdate);
        table->clearSelection();
        for (const auto& range : state.selection)
            table->setRangeSelected(QTableWidgetSelectionRange(range.top(), range.left(), range.bottom(), range.right()), true);
        table->horizontalScrollBar()->setValue(state.horizontalScroll);
        table->verticalScrollBar()->setValue(state.verticalScroll);
    }
    m_removeInputButton->setEnabled(m_inputTable->currentRow() >= 0);
    m_removeOutputButton->setEnabled(m_outputTable->currentRow() >= 0);
    m_removeVertexButton->setEnabled(m_vertexTable->currentRow() >= 0);
}

bool CPhysicsPanel::CommitPreparedSnapshot(PreparedSnapshot& prepared, QString* error)
{
    if (m_publishing) { if (error) *error = QStringLiteral("物理修改正在提交"); return false; }
    if (!ValidatePreparedSnapshot(prepared, error)) return false;
    InstallPrepared(prepared);
    return true;
}

void CPhysicsPanel::InstallPrepared(PreparedSnapshot& prepared) noexcept
{
    m_physicsDocument.swap(prepared.m_document);
    m_physicsSettings.swap(prepared.m_settings);
    m_physicsDictionary.swap(prepared.m_dictionary);
    m_physicsPath.swap(prepared.m_path);
    m_dirty = prepared.m_normalizedDocument != m_savedDocument || m_physicsPath != m_savedPath;
    m_history = prepared.m_target.history;
    m_documentRevision = prepared.m_target.revision;
    m_pendingSnapshotRefresh.swap(prepared.m_refresh);
    prepared.m_token = 0;
    prepared.m_owner.clear();
    m_snapshotNotificationPending = true;
}

void CPhysicsPanel::NotifySnapshotCommitted()
{
    if (!m_snapshotNotificationPending || !m_pendingSnapshotRefresh) return;
    {
        const QSignalBlocker panel(this), groups(m_groupTable), inputs(m_inputTable), outputs(m_outputTable), vertices(m_vertexTable);
        const QSignalBlocker drag(m_dragModeCb), gravityX(m_gravityX), gravityY(m_gravityY), windX(m_windX), windY(m_windY);
        const QSignalBlocker groupSelection(m_groupTable->selectionModel()), inputSelection(m_inputTable->selectionModel());
        const QSignalBlocker outputSelection(m_outputTable->selectionModel()), vertexSelection(m_vertexTable->selectionModel());
        const QScopedValueRollback<bool> updating(m_updating, true);
        RefreshSnapshotControls(*m_pendingSnapshotRefresh);
        m_pendingSnapshotRefresh.reset();
        m_snapshotNotificationPending = false;
    }
    const QPointer<CPhysicsPanel> panel(this);
    emit PhysicsConfigChanged();
    if (panel) emit ObservationGroupChanged(SelectedGroupIndex());
    if (panel) { RefreshHistoryControls(); emitDragParams(m_groupTable->currentRow()); }
}

int CPhysicsPanel::SelectedGroupIndex() const
{
    const int row = m_pendingSnapshotRefresh ? m_pendingSnapshotRefresh->selectedGroupRow : m_detailGroupRow;
    return row >= 0 && row < m_physicsSettings.size() ? row : -1;
}

void CPhysicsPanel::SetBakeAvailable(bool available)
{
    m_bakeButton->setEnabled(available);
}

void CPhysicsPanel::ClearObservation(const QString& status)
{
    m_observation = {};
    m_hasObservation = false;
    m_graph->ClearObservation();
    const QSignalBlocker blocker(m_observationChannel);
    m_observationChannel->clear(); m_observationChannel->setEnabled(false);
    m_observationTable->setRowCount(0);
    fitPhysicsRows(m_observationTable);
    m_observationStatus->setText(status.isEmpty() ? QStringLiteral("尚未实时：等待选中组的运行时物理样本") : status);
}

void CPhysicsPanel::SetObservation(const Live2D::Cubism::Framework::PhysicsObservation& sample, const QString& status)
{
    const int group = SelectedGroupIndex();
    if (group < 0 || sample.groupIndex != group || sample.groupId != m_physicsSettings[group].toObject().value("Id").toString()) return;
    const bool same = m_hasObservation && sample.generation == m_observation.generation && sample.groupIndex == m_observation.groupIndex &&
        sample.groupId == m_observation.groupId && sample.sourceDigest == m_observation.sourceDigest;
    if (same && sample.sampled && m_observation.sampled && sample.sequence < m_observation.sequence) return;
    const auto validRow = [](const Live2D::Cubism::Framework::PhysicsInputObservation& row) {
        return (!row.sampled || std::isfinite(row.value)) && (!row.hasRange ||
            (std::isfinite(row.minimum) && std::isfinite(row.maximum) && std::isfinite(row.defaultValue) && row.minimum <= row.maximum));
    };
    bool valid = std::isfinite(sample.time) && sample.time >= 0;
    for (const auto& point : sample.particles) valid = valid && std::isfinite(point.x()) && std::isfinite(point.y());
    for (const auto& row : sample.inputs) valid = valid && validRow(row);
    for (const auto& row : sample.outputs) valid = valid && validRow(row) &&
        (!row.sampled || (std::isfinite(row.rawValue) && std::isfinite(row.clampedValue)));
    if (!valid) { ClearObservation(QStringLiteral("物理样本无效，等待下一帧")); return; }
    m_observationStatus->setText(status.isEmpty() ? (sample.sampled ? QStringLiteral("实时 · %1 · %2 秒").arg(sample.groupId, QString::number(sample.time, 'f', 2))
                                                                      : QStringLiteral("尚未实时：选中组还没有完成物理帧")) : status);
    if (same && sample.sampled && m_observation.sampled && sample.sequence == m_observation.sequence) return;
    const auto channelNames = [](const Live2D::Cubism::Framework::PhysicsObservation& value) {
        QStringList names;
        for (const auto& row : value.outputs) names.append(QStringLiteral("输出 %1 (%2)").arg(row.id, row.type));
        for (const auto& row : value.inputs) names.append(QStringLiteral("输入 %1 (%2)").arg(row.id, row.type));
        return names;
    };
    const auto names = channelNames(sample);
    const bool rebuild = !same || names != channelNames(m_observation);
    m_observation = sample; m_hasObservation = true;
    if (rebuild) {
        const QSignalBlocker blocker(m_observationChannel);
        m_observationChannel->clear();
        for (int i = 0; i < sample.outputs.size(); ++i) m_observationChannel->addItem(names[i], i);
        for (int i = 0; i < sample.inputs.size(); ++i) m_observationChannel->addItem(names[sample.outputs.size() + i], -i - 1);
        m_observationChannel->setEnabled(!names.isEmpty());
        m_graph->ClearObservation();
        RefreshObservationChannel();
    }
    m_graph->SetObservation(sample);
    RefreshObservationRows();
}

void CPhysicsPanel::RefreshObservationChannel()
{
    const int channel = m_observationChannel->currentData().toInt();
    m_graph->SetChannel(channel >= 0, m_observationChannel->currentIndex() < 0 ? -1 : channel >= 0 ? channel : -channel - 1);
}

void CPhysicsPanel::RefreshObservationRows()
{
    using namespace Live2D::Cubism::Framework;
    const int count = m_observation.inputs.size() + m_observation.outputs.size();
    const bool changed = m_observationTable->rowCount() != count;
    m_observationTable->setRowCount(count);
    const auto number = [](float value) { return QString::number(value, 'g', 7); };
    const auto write = [&](int row, const PhysicsInputObservation& value, const PhysicsOutputObservation* output) {
        const bool sampled = m_observation.sampled && value.sampled && value.active;
        const QString range = value.hasRange ? QStringLiteral("%1 … %2").arg(number(value.minimum), number(value.maximum)) : QStringLiteral("—");
        const QString detail = sampled && output ? QStringLiteral("%1 → %2").arg(number(output->rawValue), number(output->clampedValue)) : QStringLiteral("—");
        const QString state = !value.active ? QStringLiteral("已屏蔽") : !sampled ? QStringLiteral("尚未采样") :
            output && output->clipped ? QStringLiteral("超限，已裁剪") : QStringLiteral("有效");
        const QStringList cells{QStringLiteral("%1 %2").arg(output ? QStringLiteral("输出") : QStringLiteral("输入"), value.type), value.id,
            sampled ? number(value.value) : QStringLiteral("—"), range, detail, state};
        for (int column = 0; column < cells.size(); ++column) {
            auto* item = m_observationTable->item(row, column);
            if (!item) { item = new QTableWidgetItem; m_observationTable->setItem(row, column, item); }
            if (item->text() != cells[column]) item->setText(cells[column]);
            item->setToolTip(QStringLiteral("<qt>%1</qt>").arg(cells[column].toHtmlEscaped()));
        }
    };
    int index = 0;
    for (const auto& row : m_observation.inputs) write(index++, row, nullptr);
    for (const auto& row : m_observation.outputs) write(index++, row, &row);
    if (changed) { m_observationTable->resizeColumnsToContents(); fitPhysicsRows(m_observationTable); }
}

void CPhysicsPanel::ApplyParticlePreset()
{
    Snapshot before; QString error;
    if (!CaptureSnapshot(before, &error)) { RejectEdit(error); return; }
    const int row = before.selectedGroupRow;
    const int preset = m_particlePreset->currentIndex();
    if (row < 0 || row >= m_physicsSettings.size() || preset < 0 || preset > 2) return;
    const std::array<std::array<double, 3>, 3> coefficients{{{{.85, .1, .7}}, {{.8, .2, 1.0}}, {{.7, .4, 1.3}}}};
    auto groups = before.document.value("PhysicsSettings").toArray();
    auto group = groups[row].toObject(); auto vertices = group.value("Vertices").toArray();
    for (int i = 1; i < vertices.size(); ++i) {
        auto vertex = vertices[i].toObject();
        vertex.insert("Mobility", coefficients[preset][0]); vertex.insert("Delay", coefficients[preset][1]); vertex.insert("Acceleration", coefficients[preset][2]);
        vertices[i] = vertex;
    }
    group.insert("Vertices", vertices); groups[row] = group;
    auto document = before.document; document.insert("PhysicsSettings", groups);
    CommitDocument(before, document, row, &error);
}
