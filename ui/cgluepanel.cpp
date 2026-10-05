#include "cgluepanel.h"
#include "live2dstruct/cmodeldocument.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QPointer>
#include <exception>

using namespace live2dstruct;

namespace {
bool glueEdit(const CModelEdit& edit)
{
    return edit.field == CModelEdit::MeshStructure && edit.meshRequest &&
        edit.meshRequest->meshes.isEmpty() && !edit.meshRequest->glueWeights.isEmpty();
}
}

CGluePanel::CGluePanel(QWidget* parent) : QDockWidget(QStringLiteral("Glue"), parent)
{
    setObjectName(QStringLiteral("gluePanel"));
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    m_emptyLabel = new QLabel(QStringLiteral("当前模型没有 Glue 连接"), content);
    m_emptyLabel->setObjectName(QStringLiteral("glueEmpty"));
    m_emptyLabel->setWordWrap(true);
    layout->addWidget(m_emptyLabel);
    m_glueSelector = new QComboBox(content);
    m_glueSelector->setObjectName(QStringLiteral("glueSelector"));
    m_glueSelector->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_glueSelector->setMinimumContentsLength(12);
    m_glueSelector->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(m_glueSelector);
    m_pairTable = new QTableWidget(content);
    m_pairTable->setObjectName(QStringLiteral("gluePairs"));
    m_pairTable->setColumnCount(5);
    m_pairTable->setHorizontalHeaderLabels({QStringLiteral("配对"), QStringLiteral("A 顶点"), QStringLiteral("A 权重"),
                                           QStringLiteral("B 顶点"), QStringLiteral("B 权重")});
    m_pairTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_pairTable->horizontalHeader()->setStretchLastSection(true);
    m_pairTable->verticalHeader()->hide();
    m_pairTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pairTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pairTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_pairTable, 1);
    auto* form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_weightA = new QDoubleSpinBox(content);
    m_weightB = new QDoubleSpinBox(content);
    m_weightA->setObjectName(QStringLiteral("glueWeightA"));
    m_weightB->setObjectName(QStringLiteral("glueWeightB"));
    for (auto* spin : {m_weightA, m_weightB}) {
        spin->setRange(0, 1); spin->setDecimals(9); spin->setSingleStep(.05);
        spin->setKeyboardTracking(false);
    }
    form->addRow(QStringLiteral("A 端点权重"), m_weightA);
    form->addRow(QStringLiteral("B 端点权重"), m_weightB);
    layout->addLayout(form);
    auto* actions = new QHBoxLayout;
    m_applyButton = new QPushButton(QStringLiteral("应用权重"), content);
    m_applyButton->setObjectName(QStringLiteral("applyGlueWeights"));
    m_undoButton = new QPushButton(QStringLiteral("撤销"), content);
    m_undoButton->setObjectName(QStringLiteral("undoGlueWeights"));
    m_redoButton = new QPushButton(QStringLiteral("重做"), content);
    m_redoButton->setObjectName(QStringLiteral("redoGlueWeights"));
    actions->addWidget(m_applyButton, 1); actions->addWidget(m_undoButton); actions->addWidget(m_redoButton);
    layout->addLayout(actions);
    m_statusLabel = new QLabel(content);
    m_statusLabel->setObjectName(QStringLiteral("glueStatus"));
    m_statusLabel->setTextFormat(Qt::PlainText); m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);
    setWidget(content);
    connect(m_glueSelector, &QComboBox::currentIndexChanged, this, [this] { if (!m_updating) RefreshPairTable(); });
    connect(m_pairTable, &QTableWidget::itemSelectionChanged, this, [this] { if (!m_updating) RefreshWeightControls(); });
    connect(m_applyButton, &QPushButton::clicked, this, &CGluePanel::ApplyWeights);
    connect(m_undoButton, &QPushButton::clicked, this, [this] { QString error; if (!Undo(&error)) RejectEdit(error); });
    connect(m_redoButton, &QPushButton::clicked, this, [this] { QString error; if (!Redo(&error)) RejectEdit(error); });
    RefreshGlueList();
}

void CGluePanel::SetDocument(CModelDocument* document)
{
    m_document = document;
    if (m_editor.IsPublishing()) {
        m_initializedDocument = nullptr;
        m_initializedSourceDigest.clear();
        m_initializedSourcePath.clear();
        m_refreshPending = true;
        return;
    }
    Refresh();
}

void CGluePanel::SetPublisher(Publisher publisher) { m_editor.SetPublisher(std::move(publisher)); }
void CGluePanel::SetHistoryHandler(HistoryHandler handler) { m_historyHandler = std::move(handler); }
const CGlueEditController::Snapshot& CGluePanel::CurrentGlues() const { return m_editor.CurrentGlues(); }

void CGluePanel::Refresh()
{
    if (m_editor.IsPublishing()) { m_refreshPending = true; return; }
    m_refreshPending = false;
    RefreshEditor();
}

void CGluePanel::RefreshEditor()
{
    const QString selection = m_glueSelector->currentData(Qt::UserRole + 1).toString();
    QString error;
    QByteArray bytes;
    if (!m_document || !m_document->IsLoaded()) {
        m_initializedDocument = nullptr;
        m_initializedSourceDigest.clear();
        m_initializedSourcePath.clear();
        m_editor.Reset();
        m_glueSelector->clear(); m_pairTable->setRowCount(0); m_emptyLabel->show();
        m_glueSelector->setEnabled(false); RefreshWeightControls(); return;
    }
    const auto sourceDigest = m_document->SourceDigest();
    const auto sourcePath = m_document->SourcePath();
    if (!m_document->BuildPreviewMoc(bytes, &error) ||
        ((m_document != m_initializedDocument || sourceDigest != m_initializedSourceDigest ||
          sourcePath != m_initializedSourcePath || bytes != m_editor.CurrentBytes()) &&
         !m_editor.Initialize(bytes, &error))) {
        m_initializedDocument = nullptr;
        m_initializedSourceDigest.clear();
        m_initializedSourcePath.clear();
        m_editor.Reset();
        m_glueSelector->clear(); m_pairTable->setRowCount(0); m_emptyLabel->show();
        m_glueSelector->setEnabled(false); m_pairTable->setEnabled(false);
        m_applyButton->setEnabled(false); RejectEdit(error); return;
    }
    m_initializedDocument = m_document;
    m_initializedSourceDigest = sourceDigest;
    m_initializedSourcePath = sourcePath;
    m_statusLabel->clear();
    RefreshGlueList(selection);
}

void CGluePanel::RefreshGlueList(const QString& selectedId)
{
    {
        const QScopedValueRollback<bool> updating(m_updating, true);
        const QSignalBlocker blocker(m_glueSelector);
        m_glueSelector->clear();
        for (const auto& glue : m_editor.CurrentGlues()) {
            m_glueSelector->addItem(QStringLiteral("%1 · %2 ↔ %3").arg(glue.id, glue.artMeshIdA, glue.artMeshIdB), glue.index);
            m_glueSelector->setItemData(m_glueSelector->count() - 1, glue.id, Qt::UserRole + 1);
        }
        const int selected = m_glueSelector->findData(selectedId, Qt::UserRole + 1);
        if (selected >= 0) m_glueSelector->setCurrentIndex(selected);
    }
    m_emptyLabel->setVisible(m_glueSelector->count() == 0);
    m_glueSelector->setEnabled(m_glueSelector->count() > 0);
    RefreshPairTable();
}

int CGluePanel::SelectedGlueIndex() const
{
    return m_glueSelector->currentIndex() < 0 ? -1 : m_glueSelector->currentData().toInt();
}

void CGluePanel::RefreshPairTable()
{
    {
        const QScopedValueRollback<bool> updating(m_updating, true);
        const QSignalBlocker blocker(m_pairTable);
        const int previous = m_pairTable->currentRow();
        m_pairTable->setRowCount(0);
        const int index = SelectedGlueIndex();
        if (index >= 0 && index < m_editor.CurrentGlues().size()) {
            const auto& glue = m_editor.CurrentGlues()[index];
            m_pairTable->setRowCount(glue.pairs.size());
            for (int row = 0; row < glue.pairs.size(); ++row) {
                const auto& pair = glue.pairs[row];
                const QStringList values{QString::number(row), QString::number(pair.a.vertex), QString::number(pair.a.weight, 'g', 9),
                    QString::number(pair.b.vertex), QString::number(pair.b.weight, 'g', 9)};
                for (int column = 0; column < values.size(); ++column) m_pairTable->setItem(row, column, new QTableWidgetItem(values[column]));
            }
            if (!glue.pairs.isEmpty()) m_pairTable->selectRow(qBound(0, previous, int(glue.pairs.size()) - 1));
        }
    }
    m_pairTable->setEnabled(m_pairTable->rowCount() > 0);
    RefreshWeightControls();
}

void CGluePanel::RefreshWeightControls()
{
    const int glueIndex = SelectedGlueIndex(), row = m_pairTable->currentRow();
    const bool selected = m_document && glueIndex >= 0 && glueIndex < m_editor.CurrentGlues().size() && row >= 0 &&
        row < m_editor.CurrentGlues()[glueIndex].pairs.size();
    bool editable = selected;
    if (selected) {
        const auto& glue = m_editor.CurrentGlues()[glueIndex];
        m_weightA->setValue(glue.pairs[row].a.weight); m_weightB->setValue(glue.pairs[row].b.weight);
        editable = !m_document->IsLocked(QStringLiteral("glue"), glue.id) &&
            !m_document->IsLocked(QStringLiteral("artmesh"), glue.artMeshIdA) && !m_document->IsLocked(QStringLiteral("artmesh"), glue.artMeshIdB);
    }
    m_weightA->setEnabled(editable); m_weightB->setEnabled(editable); m_applyButton->setEnabled(editable);
    m_undoButton->setEnabled(CanUndo()); m_redoButton->setEnabled(CanRedo());
}

void CGluePanel::ApplyWeights()
{
    if (!m_document || m_editor.IsPublishing()) return;
    const int index = SelectedGlueIndex(), row = m_pairTable->currentRow();
    if (index < 0 || index >= m_editor.CurrentGlues().size() || row < 0 || row >= m_editor.CurrentGlues()[index].pairs.size()) return;
    const auto& pair = m_editor.CurrentGlues()[index].pairs[row];
    QVector<CGlueEditController::WeightEdit> edits;
    if (float(m_weightA->value()) != pair.a.weight) edits.append({index, row, CGlueEditController::Side::A, float(m_weightA->value())});
    if (float(m_weightB->value()) != pair.b.weight) edits.append({index, row, CGlueEditController::Side::B, float(m_weightB->value())});
    if (edits.isEmpty()) return;
    const QPointer<CGluePanel> alive(this);
    QString error;
    const bool accepted = m_editor.Apply(edits, &error);
    if (!alive) return;
    if (!accepted) {
        if (m_refreshPending) Refresh();
        RejectEdit(error);
        return;
    }
    Refresh();
    emit HistoryChanged(CanUndo(), CanRedo());
}

bool CGluePanel::CanUndo() const
{
    CModelEdit edit;
    return m_document && !m_editor.IsPublishing() && m_document->PeekUndoEdit(&edit) && glueEdit(edit);
}

bool CGluePanel::CanRedo() const
{
    CModelEdit edit;
    return m_document && !m_editor.IsPublishing() && m_document->PeekRedoEdit(&edit) && glueEdit(edit);
}

bool CGluePanel::MoveHistory(bool undo, QString* error)
{
    if (!(undo ? CanUndo() : CanRedo())) { if (error) *error = QStringLiteral("当前历史不是 Glue 权重修改"); return false; }
    const QPointer<CGluePanel> alive(this);
    CModelEdit edit;
    try {
        const auto handler = m_historyHandler;
        const bool accepted = handler ? handler(undo, edit, error) : undo ? m_document->UndoEdit(&edit) : m_document->RedoEdit(&edit);
        if (!accepted) {
            if (m_refreshPending) Refresh();
            return false;
        }
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("Glue 历史操作失败，原状态已保留"); return false;
    }
    if (!alive) return false;
    Refresh();
    emit HistoryChanged(CanUndo(), CanRedo());
    return true;
}

bool CGluePanel::Undo(QString* error) { return MoveHistory(true, error); }
bool CGluePanel::Redo(QString* error) { return MoveHistory(false, error); }
void CGluePanel::MarkSaved() { m_editor.MarkSaved(); }
void CGluePanel::RejectEdit(const QString& error)
{
    m_statusLabel->setText(error.isEmpty() ? QStringLiteral("Glue 修改未提交，原状态已保留") : error);
    emit EditRejected(m_statusLabel->text());
}
