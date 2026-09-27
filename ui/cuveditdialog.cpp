#include "cuveditdialog.h"
#include "cuvcanvasview.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScopeGuard>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QVBoxLayout>
#include <cmath>
#include <exception>

using live2dstruct::CUvEditDraft;
namespace {
bool sameState(const CUvEditDialog::State& a, const CUvEditDialog::State& b)
{
    if (a.size() != b.size()) return false;
    for (int index = 0; index < a.size(); ++index)
        if (a[index] != b[index]) return false;
    return true;
}
}

CUvEditDialog::CUvEditDialog(const Snapshot& snapshot, const QMap<int, QImage>& images,
                            const QString& selectedMesh, QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("uvEditDialog")); setWindowTitle(QStringLiteral("编辑 UV")); setModal(true);
    auto* layout = new QVBoxLayout(this);
    const int gap = std::max(0, style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing));
    layout->setSpacing(gap); layout->setContentsMargins(gap, gap, gap, gap);
    auto* hint = new QLabel(QStringLiteral("调整网格在图集上的采样位置。纹理像素保持不变；修改会影响模型外观。"), this);
    hint->setTextFormat(Qt::PlainText); hint->setWordWrap(true); layout->addWidget(hint);
    auto* toolbar = new QHBoxLayout;
    m_mesh = new QComboBox(this); m_mesh->setObjectName(QStringLiteral("uvMesh")); m_mesh->setAccessibleName(QStringLiteral("编辑对象"));
    m_mesh->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); m_mesh->setMinimumContentsLength(1);
    toolbar->addWidget(m_mesh, 1);
    const auto button = [&](const QString& text, const char* name, QHBoxLayout* row) {
        auto* control = new QPushButton(text, this); control->setObjectName(QString::fromLatin1(name));
        control->setAutoDefault(false); row->addWidget(control); return control;
    };
    m_undo = button(QStringLiteral("撤销"), "uvUndo", toolbar); m_redo = button(QStringLiteral("重做"), "uvRedo", toolbar);
    layout->addLayout(toolbar);
    m_splitter = new QSplitter(Qt::Horizontal, this); m_splitter->setObjectName(QStringLiteral("uvEditorSplitter")); m_splitter->setChildrenCollapsible(false);
    m_canvas = new CUvCanvasView(m_splitter);
    auto* scroll = new QScrollArea(m_splitter); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setObjectName(QStringLiteral("uvControlsScroll")); scroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    m_editor = new QWidget(scroll); auto* controls = new QVBoxLayout(m_editor); controls->setSpacing(gap);
    auto* selectionTools = new QHBoxLayout;
    auto* all = button(QStringLiteral("全选点"), "uvSelectAll", selectionTools);
    auto* fit = button(QStringLiteral("适应图集"), "uvFitAtlas", selectionTools);
    auto* focus = button(QStringLiteral("适应选区"), "uvFitSelection", selectionTools);
    controls->addLayout(selectionTools);
    m_status = new QLabel(m_editor); m_status->setObjectName(QStringLiteral("uvSelectionStatus")); m_status->setTextFormat(Qt::PlainText); m_status->setWordWrap(true);
    controls->addWidget(m_status);
    auto* form = new QFormLayout; form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    const auto spin = [&](const QString& text, const char* name, double value, const QString& suffix = {}) {
        auto* input = new QDoubleSpinBox(m_editor); input->setObjectName(QString::fromLatin1(name)); input->setAccessibleName(text);
        input->setRange(-1e50, 1e50); input->setDecimals(6); input->setValue(value); input->setSuffix(suffix); input->setKeyboardTracking(false);
        input->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); form->addRow(text, input); return input;
    };
    m_x = spin(QStringLiteral("顶点 X"), "uvPointX", 0, QStringLiteral(" px"));
    m_y = spin(QStringLiteral("顶点 Y"), "uvPointY", 0, QStringLiteral(" px"));
    m_pointApply = new QPushButton(QStringLiteral("设置顶点坐标"), m_editor); m_pointApply->setObjectName(QStringLiteral("uvSetPoint")); m_pointApply->setAutoDefault(false); form->addRow(m_pointApply);
    m_dx = spin(QStringLiteral("平移 X"), "uvTranslateX", 0, QStringLiteral(" px"));
    m_dy = spin(QStringLiteral("平移 Y"), "uvTranslateY", 0, QStringLiteral(" px"));
    m_angle = spin(QStringLiteral("顺时针旋转"), "uvRotation", 0, QStringLiteral("°"));
    m_sx = spin(QStringLiteral("水平缩放"), "uvScaleX", 100, QStringLiteral("%"));
    m_sy = spin(QStringLiteral("垂直缩放"), "uvScaleY", 100, QStringLiteral("%"));
    m_uniform = new QCheckBox(QStringLiteral("等比缩放"), m_editor); m_uniform->setObjectName(QStringLiteral("uvUniformScale")); m_uniform->setChecked(true); form->addRow(m_uniform);
    m_pivotMode = new QComboBox(m_editor); m_pivotMode->setObjectName(QStringLiteral("uvPivotMode"));
    m_pivotMode->addItems({QStringLiteral("选区中心"), QStringLiteral("自定义像素坐标")}); form->addRow(QStringLiteral("变换中心"), m_pivotMode);
    m_pivotX = spin(QStringLiteral("中心 X"), "uvPivotX", 0, QStringLiteral(" px"));
    m_pivotY = spin(QStringLiteral("中心 Y"), "uvPivotY", 0, QStringLiteral(" px"));
    m_snap = new QCheckBox(QStringLiteral("吸附到整数像素"), m_editor); m_snap->setObjectName(QStringLiteral("uvPixelSnap")); form->addRow(m_snap);
    auto* transform = new QPushButton(QStringLiteral("变换选点"), m_editor); transform->setObjectName(QStringLiteral("uvApplyTransform")); transform->setAutoDefault(false); form->addRow(transform);
    m_slot = new QComboBox(m_editor); m_slot->setObjectName(QStringLiteral("uvDestinationTexture"));
    for (auto atlas = snapshot.atlasSizes.cbegin(); atlas != snapshot.atlasSizes.cend(); ++atlas)
        m_slot->addItem(QStringLiteral("texture_%1 · %2×%3").arg(atlas.key()).arg(atlas->width()).arg(atlas->height()), atlas.key());
    form->addRow(QStringLiteral("对象图集"), m_slot);
    m_slotCoordinates = new QComboBox(m_editor); m_slotCoordinates->setObjectName(QStringLiteral("uvTextureCoordinates"));
    m_slotCoordinates->addItems({QStringLiteral("保持归一化 UV"), QStringLiteral("保持像素坐标")}); form->addRow(QStringLiteral("切换时"), m_slotCoordinates);
    auto* assign = new QPushButton(QStringLiteral("分配到图集"), m_editor); assign->setObjectName(QStringLiteral("uvAssignTexture")); assign->setAutoDefault(false); form->addRow(assign);
    controls->addLayout(form);
    m_discardInput = new QPushButton(QStringLiteral("放弃未应用的数值输入"), m_editor);
    m_discardInput->setObjectName(QStringLiteral("uvDiscardInput")); m_discardInput->setAutoDefault(false); controls->addWidget(m_discardInput);
    auto* resetSelected = new QPushButton(QStringLiteral("恢复选点的初始 UV"), m_editor); resetSelected->setObjectName(QStringLiteral("uvResetSelection")); resetSelected->setAutoDefault(false); controls->addWidget(resetSelected);
    auto* resetAll = new QPushButton(QStringLiteral("恢复全部进入时状态"), m_editor); resetAll->setObjectName(QStringLiteral("uvResetAll")); resetAll->setAutoDefault(false); controls->addWidget(resetAll);
    auto* help = new QLabel(QStringLiteral("单击选点，拖动空白框选；Shift 增选，Alt 减选。双击选择连通岛。\n拖动时按 X / Y 约束轴向，Shift 锁定主轴。方向键移动 1 px，Shift 移动 10 px。\n空格或中键拖动画布，滚轮缩放。Esc 取消当前拖动。"), m_editor);
    help->setTextFormat(Qt::PlainText); help->setWordWrap(true); controls->addWidget(help); controls->addStretch();
    scroll->setWidget(m_editor); m_splitter->addWidget(m_canvas); m_splitter->addWidget(scroll); m_splitter->setStretchFactor(0, 1); m_splitter->setStretchFactor(1, 0);
    layout->addWidget(m_splitter, 1);
    m_coordinates = new QLabel(this); m_coordinates->setObjectName(QStringLiteral("uvPointerCoordinates")); m_coordinates->setTextFormat(Qt::PlainText); layout->addWidget(m_coordinates);
    m_error = new QLabel(this); m_error->setObjectName(QStringLiteral("uvEditError")); m_error->setTextFormat(Qt::PlainText); m_error->setWordWrap(true); m_error->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(m_error);
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存 UV"));
    m_buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消")); layout->addWidget(m_buttons);
    QString error; m_valid = m_draft.Initialize(snapshot, &error);
    if (m_valid) for (auto atlas = snapshot.atlasSizes.cbegin(); atlas != snapshot.atlasSizes.cend(); ++atlas)
        if (!images.contains(atlas.key()) || images.value(atlas.key()).size() != atlas.value()) {
            m_valid = false; error = QStringLiteral("图集图像与尺寸不一致：texture_%1").arg(atlas.key()); break;
        }
    m_state = m_draft.InitialState(); m_history.append(m_state);
    for (const auto& mesh : snapshot.meshes) m_mesh->addItem(mesh.material.id, mesh.material.id);
    const int selected = m_mesh->findData(selectedMesh); if (selected >= 0) m_mesh->setCurrentIndex(selected);
    m_canvas->SetSnapshot(snapshot, images); m_canvas->SetMesh(m_mesh->currentData().toString());
    m_lastSelection = Selection();
    connect(m_mesh, &QComboBox::currentIndexChanged, this, &CUvEditDialog::SelectMesh);
    connect(m_canvas, &CUvCanvasView::SelectionChanged, this, &CUvEditDialog::Refresh);
    connect(m_canvas, &CUvCanvasView::GestureStarted, this, &CUvEditDialog::BeginGesture);
    connect(m_canvas, &CUvCanvasView::TranslationPreview, this, &CUvEditDialog::PreviewGesture);
    connect(m_canvas, &CUvCanvasView::GestureFinished, this, &CUvEditDialog::EndGesture);
    connect(m_canvas, &CUvCanvasView::PointerMoved, this, [this](QPointF point) { m_coordinates->setText(QStringLiteral("X %1 px    Y %2 px").arg(point.x(), 0, 'f', 3).arg(point.y(), 0, 'f', 3)); });
    connect(m_undo, &QPushButton::clicked, this, &CUvEditDialog::Undo); connect(m_redo, &QPushButton::clicked, this, &CUvEditDialog::Redo);
    connect(all, &QPushButton::clicked, m_canvas, &CUvCanvasView::SelectAll); connect(fit, &QPushButton::clicked, m_canvas, &CUvCanvasView::FitAtlas); connect(focus, &QPushButton::clicked, m_canvas, &CUvCanvasView::FitSelection);
    connect(m_pointApply, &QPushButton::clicked, this, &CUvEditDialog::SetPoint); connect(transform, &QPushButton::clicked, this, &CUvEditDialog::ApplyTransform);
    connect(assign, &QPushButton::clicked, this, &CUvEditDialog::AssignTexture);
    connect(resetSelected, &QPushButton::clicked, this, [this] { ResetSelection(false); }); connect(resetAll, &QPushButton::clicked, this, [this] { ResetSelection(true); });
    connect(m_snap, &QCheckBox::toggled, m_canvas, &CUvCanvasView::SetPixelSnap);
    connect(m_uniform, &QCheckBox::toggled, this, [this](bool checked) { m_sy->setEnabled(!checked); if (!m_refreshing) { m_transformDirty = true; m_discardInput->setEnabled(true); } });
    connect(m_pivotMode, &QComboBox::currentIndexChanged, this, [this] { if (!m_refreshing) m_transformDirty = true; Refresh(); });
    connect(m_x, &QDoubleSpinBox::valueChanged, this, [this] { if (!m_refreshing) { m_pointDirty = m_pointXDirty = true; m_discardInput->setEnabled(true); } });
    connect(m_y, &QDoubleSpinBox::valueChanged, this, [this] { if (!m_refreshing) { m_pointDirty = m_pointYDirty = true; m_discardInput->setEnabled(true); } });
    for (auto* input : {m_dx, m_dy, m_angle, m_sx, m_sy, m_pivotX, m_pivotY})
        connect(input, &QDoubleSpinBox::valueChanged, this, [this] { if (!m_refreshing) { m_transformDirty = true; m_discardInput->setEnabled(true); } });
    connect(m_slot, &QComboBox::currentIndexChanged, this, [this] { if (!m_refreshing) { m_slotDirty = true; m_discardInput->setEnabled(true); } });
    connect(m_discardInput, &QPushButton::clicked, this, &CUvEditDialog::DiscardPendingInput);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &CUvEditDialog::accept); connect(m_buttons, &QDialogButtonBox::rejected, this, &CUvEditDialog::reject);
    auto* undo = new QShortcut(QKeySequence::Undo, this); connect(undo, &QShortcut::activated, this, &CUvEditDialog::Undo);
    auto* redo = new QShortcut(QKeySequence::Redo, this); connect(redo, &QShortcut::activated, this, &CUvEditDialog::Redo);
    resize(fontMetrics().averageCharWidth() * 145, fontMetrics().height() * 46);
    m_splitter->setSizes({fontMetrics().averageCharWidth() * 100, fontMetrics().averageCharWidth() * 45});
    Refresh(); ShowError(error);
}

QString CUvEditDialog::Error() const { return m_error->text(); }
void CUvEditDialog::SetPreviewHandler(Handler handler) { if (!m_busy) m_preview = std::move(handler); }
void CUvEditDialog::SetSubmissionHandler(Handler handler) { if (!m_busy) m_submit = std::move(handler); }
void CUvEditDialog::SetCancellationHandler(CancellationHandler handler) { if (!m_busy) m_cancel = std::move(handler); }
void CUvEditDialog::ShowError(const QString& error) { m_error->setText(error); m_error->setVisible(!error.isEmpty()); }
CUvEditDraft::Selection CUvEditDialog::Selection() const { return {m_canvas->Mesh(), m_canvas->SelectedVertices()}; }
bool CUvEditDialog::HasPendingInput() const { return m_pointDirty || m_transformDirty || m_slotDirty; }

void CUvEditDialog::DiscardPendingInput()
{
    if (m_busy || m_gesture) return;
    m_refreshing = true;
    for (auto* input : {m_dx, m_dy, m_angle}) input->setValue(0);
    m_sx->setValue(100); m_sy->setValue(100);
    m_pointDirty = m_pointXDirty = m_pointYDirty = m_transformDirty = m_slotDirty = false; m_refreshing = false;
    ShowError({}); Refresh();
}

bool CUvEditDialog::Publish(const State& state, bool remember)
{
    if (m_busy || !m_valid) return false;
    QString error;
    if (!m_draft.ValidateState(state, &error)) { ShowError(error); return false; }
    if (sameState(state, m_state)) return true;
    if (!m_preview) { ShowError(QStringLiteral("UV 预览尚未连接")); return false; }
    const QPointer<CUvEditDialog> alive(this); bool accepted = false;
    try {
        auto candidate = state;
        auto history = m_history;
        if (remember) { history.resize(m_historyIndex + 1); history.append(candidate); }
        auto handler = m_preview;
        m_busy = true; const auto done = qScopeGuard([alive] { if (alive) alive->m_busy = false; });
        try { accepted = handler(candidate, &error); }
        catch (...) { error = QStringLiteral("UV 预览失败，已接受的修改保持不变"); }
        if (!alive) return accepted;
        if (accepted) {
            m_state.swap(candidate);
            if (remember) { m_history.swap(history); m_historyIndex = m_history.size() - 1; }
        }
    } catch (const std::exception&) { error = QStringLiteral("无法准备 UV 编辑数据，已接受的修改保持不变"); }
    if (!alive) return accepted;
    ShowError(accepted ? QString() : error.isEmpty() ? QStringLiteral("UV 修改未能发布") : error); Refresh();
    return accepted;
}

void CUvEditDialog::Refresh()
{
    if (m_refreshing) return;
    m_refreshing = true; const auto restore = qScopeGuard([this] { m_refreshing = false; });
    auto currentSelection = Selection();
    if (HasPendingInput() && currentSelection.meshId == m_lastSelection.meshId && currentSelection.vertices != m_lastSelection.vertices) {
        m_canvas->SetSelectedVertices(m_lastSelection.vertices);
        ShowError(QStringLiteral("请先应用或放弃当前数值输入，再改变选点。"));
    } else m_lastSelection = currentSelection;
    const int index = m_draft.MeshIndex(m_canvas->Mesh()); const bool editable = m_valid && index >= 0 && m_draft.Source().meshes[index].editable;
    m_editor->setEnabled(editable && !m_busy && !m_gesture);
    m_mesh->setEnabled(m_valid && !m_busy && !m_gesture);
    m_undo->setEnabled(m_valid && !m_busy && !m_gesture && m_historyIndex > 0);
    m_redo->setEnabled(m_valid && !m_busy && !m_gesture && m_historyIndex + 1 < m_history.size());
    m_buttons->button(QDialogButtonBox::Save)->setEnabled(m_valid && !m_busy);
    m_discardInput->setEnabled(HasPendingInput());
    m_canvas->SetState(m_state);
    const auto selected = Selection(); QRectF bounds;
    const bool hasBounds = m_draft.SelectionBounds(m_state, selected, bounds);
    m_pointApply->setEnabled(editable && selected.vertices.size() == 1);
    m_x->setEnabled(selected.vertices.size() == 1); m_y->setEnabled(selected.vertices.size() == 1);
    if (hasBounds) {
        if (selected.vertices.size() == 1 && !m_pointDirty) {
            const QSignalBlocker x(m_x), y(m_y); m_x->setValue(bounds.left()); m_y->setValue(bounds.top());
        }
        if (!m_pivotMode->currentIndex()) {
            const QSignalBlocker x(m_pivotX), y(m_pivotY); m_pivotX->setValue(bounds.center().x()); m_pivotY->setValue(bounds.center().y());
        }
    }
    m_pivotX->setEnabled(m_pivotMode->currentIndex() == 1); m_pivotY->setEnabled(m_pivotMode->currentIndex() == 1); m_sy->setEnabled(!m_uniform->isChecked());
    if (index >= 0) {
        if (!m_slotDirty) { const QSignalBlocker block(m_slot); m_slot->setCurrentIndex(m_slot->findData(m_state[index].textureIndex)); }
        const auto diagnostics = m_draft.Diagnostics(m_state, m_state[index].id);
        m_status->setText(QStringLiteral("已选 %1 / %2 个顶点%3").arg(selected.vertices.size()).arg(m_state[index].uvs.size() / 2)
            .arg(diagnostics.isEmpty() ? QString() : QLatin1Char('\n') + diagnostics.join(QLatin1Char('\n'))));
    } else m_status->setText(QStringLiteral("没有可编辑的 ArtMesh"));
}

void CUvEditDialog::SelectMesh()
{
    if (m_busy || m_refreshing || m_gesture) {
        const QSignalBlocker block(m_mesh); m_mesh->setCurrentIndex(m_mesh->findData(m_canvas->Mesh())); return;
    }
    if (HasPendingInput()) {
        const QSignalBlocker block(m_mesh); m_mesh->setCurrentIndex(m_mesh->findData(m_canvas->Mesh()));
        ShowError(QStringLiteral("请先应用或放弃当前数值输入，再切换对象。")); return;
    }
    const QPointer<CUvEditDialog> alive(this); m_canvas->SetMesh(m_mesh->currentData().toString()); if (alive) Refresh();
}

void CUvEditDialog::ApplyTransform()
{
    if (m_busy || m_gesture) return;
    CUvEditDraft::Transform transform;
    transform.translation = {m_dx->value(), m_dy->value()}; transform.angleDegrees = m_angle->value();
    transform.scale = {m_sx->value() / 100.0, (m_uniform->isChecked() ? m_sx->value() : m_sy->value()) / 100.0};
    transform.pivot = {m_pivotX->value(), m_pivotY->value()}; transform.snapToPixels = m_snap->isChecked();
    State candidate; QString error; QRectF bounds;
    if (!m_pivotMode->currentIndex() && m_draft.SelectionBounds(m_state, Selection(), bounds)) transform.pivot = bounds.center();
    if (!m_draft.TransformSelection(m_state, Selection(), transform, candidate, &error)) ShowError(error);
    else {
        const QPointer<CUvEditDialog> alive(this);
        if (Publish(candidate, true) && alive) { m_transformDirty = false; Refresh(); }
    }
}

void CUvEditDialog::SetPoint()
{
    if (m_busy || m_gesture) return;
    State candidate; QString error; QRectF bounds;
    if (!m_draft.SelectionBounds(m_state, Selection(), bounds, &error)) { ShowError(error); return; }
    const QPointF point(m_pointXDirty ? m_x->value() : bounds.left(), m_pointYDirty ? m_y->value() : bounds.top());
    if (!m_draft.SetPointPixels(m_state, Selection(), point, candidate, &error)) ShowError(error);
    else {
        const QPointer<CUvEditDialog> alive(this);
        if (Publish(candidate, true) && alive) { m_pointDirty = m_pointXDirty = m_pointYDirty = false; Refresh(); }
    }
}

void CUvEditDialog::AssignTexture()
{
    if (m_busy || m_gesture) return;
    State candidate; QString error;
    if (!m_draft.AssignTexture(m_state, m_canvas->Mesh(), m_slot->currentData().toInt(),
        m_slotCoordinates->currentIndex() ? CUvEditDraft::SlotCoordinates::Pixels : CUvEditDraft::SlotCoordinates::Normalized, candidate, &error)) ShowError(error);
    else {
        const QPointer<CUvEditDialog> alive(this);
        if (Publish(candidate, true) && alive) { m_slotDirty = false; Refresh(); }
    }
}

void CUvEditDialog::ResetSelection(bool all)
{
    if (m_busy || m_gesture) return;
    if (HasPendingInput()) { ShowError(QStringLiteral("请先应用或放弃当前数值输入，再恢复 UV。")); return; }
    State candidate; QString error;
    if (!m_draft.Reset(m_state, Selection(), all, candidate, &error)) ShowError(error);
    else Publish(candidate, true);
}

void CUvEditDialog::Undo()
{
    if (m_busy || m_gesture || m_historyIndex <= 0) return;
    if (HasPendingInput()) { ShowError(QStringLiteral("请先应用或放弃当前数值输入，再撤销。")); return; }
    const QPointer<CUvEditDialog> alive(this); const int next = m_historyIndex - 1;
    if (Publish(m_history[next], false) && alive) { m_historyIndex = next; Refresh(); }
}

void CUvEditDialog::Redo()
{
    if (m_busy || m_gesture || m_historyIndex + 1 >= m_history.size()) return;
    if (HasPendingInput()) { ShowError(QStringLiteral("请先应用或放弃当前数值输入，再重做。")); return; }
    const QPointer<CUvEditDialog> alive(this); const int next = m_historyIndex + 1;
    if (Publish(m_history[next], false) && alive) { m_historyIndex = next; Refresh(); }
}

void CUvEditDialog::BeginGesture()
{
    if (m_busy || m_gesture || !m_valid) return;
    if (HasPendingInput()) { ShowError(QStringLiteral("请先应用或放弃当前数值输入，再拖动 UV。")); return; }
    m_gestureBefore = m_state; m_gesture = true; Refresh();
}

void CUvEditDialog::PreviewGesture(QPointF delta, int axis, bool snap)
{
    if (!m_gesture || m_busy) return;
    if (axis == 1) delta.setY(0); else if (axis == 2) delta.setX(0);
    if (delta.isNull()) { Publish(m_gestureBefore, false); return; }
    CUvEditDraft::Transform transform; transform.translation = delta; transform.snapToPixels = snap;
    State candidate; QString error;
    if (!m_draft.TransformSelection(m_gestureBefore, Selection(), transform, candidate, &error)) ShowError(error);
    else Publish(candidate, false);
}

void CUvEditDialog::EndGesture(bool canceled)
{
    if (!m_gesture || m_busy) return;
    const QPointer<CUvEditDialog> alive(this);
    if (canceled && !Publish(m_gestureBefore, false)) {
        if (!alive) return;
        ShowError(QStringLiteral("未能恢复本次拖动；保留当前预览，可再次撤销或取消整个 UV 编辑。"));
    }
    if (!alive) return;
    if (!sameState(m_state, m_gestureBefore)) {
        m_history.resize(m_historyIndex + 1); m_history.append(m_state); m_historyIndex = m_history.size() - 1;
    }
    m_gesture = false; m_gestureBefore.clear(); Refresh();
}

void CUvEditDialog::accept()
{
    if (m_busy || !m_valid) return;
    const QPointer<CUvEditDialog> alive(this);
    for (auto* input : {m_x, m_y, m_dx, m_dy, m_angle, m_sx, m_sy, m_pivotX, m_pivotY}) {
        if (!input->hasAcceptableInput()) { ShowError(QStringLiteral("请先修正无效的数值输入，再保存 UV。")); input->setFocus(); return; }
        input->interpretText(); if (!alive) return;
    }
    if (m_gesture) EndGesture(false);
    if (!alive) return;
    if (HasPendingInput()) { ShowError(QStringLiteral("仍有未应用的数值输入。请应用或放弃输入，再保存 UV。")); return; }
    if (!m_submit) { ShowError(QStringLiteral("UV 保存尚未连接")); return; }
    QString error; bool accepted = false; auto handler = m_submit;
    m_busy = true;
    try { accepted = handler(m_state, &error); }
    catch (...) { error = QStringLiteral("UV 保存失败，草稿仍保留在窗口中"); }
    if (!alive) return;
    m_busy = false;
    if (!accepted) { ShowError(error.isEmpty() ? QStringLiteral("UV 保存未能完成") : error); Refresh(); return; }
    m_accepted = true; QDialog::accept();
}

void CUvEditDialog::reject()
{
    if (m_busy) return;
    if (m_accepted) { QDialog::reject(); return; }
    const QPointer<CUvEditDialog> alive(this); QString error; bool canceled = true;
    auto handler = m_cancel; m_busy = true;
    try { if (handler) canceled = handler(&error); else if (!sameState(m_state, m_draft.InitialState())) { canceled = false; error = QStringLiteral("UV 恢复尚未连接"); } }
    catch (...) { canceled = false; error = QStringLiteral("UV 恢复失败，请保留窗口并重试"); }
    if (!alive) return;
    m_busy = false;
    if (!canceled) { ShowError(error.isEmpty() ? QStringLiteral("无法恢复进入时状态，请重试取消") : error); return; }
    m_gesture = false; m_state = m_draft.InitialState(); QDialog::reject();
}

void CUvEditDialog::closeEvent(QCloseEvent* event)
{
    event->ignore(); reject();
}

void CUvEditDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    if (!m_splitter) return;
    const auto orientation = event->size().width() < fontMetrics().averageCharWidth() * 105 ? Qt::Vertical : Qt::Horizontal;
    if (m_splitter->orientation() != orientation) { m_splitter->setOrientation(orientation); m_splitter->setSizes({event->size().height() * 3 / 5, event->size().height() * 2 / 5}); }
}
