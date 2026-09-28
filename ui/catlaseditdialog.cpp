#include "catlaseditdialog.h"
#include "live2dstruct/cmodeldisplaynames.h"
#include "catlascanvasview.h"
#include "theme.h"
#include "std/cexportcontrol.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScopeGuard>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>
#include <QTabWidget>
#include <QThread>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>

using live2dstruct::CAtlasRepackPlanner;
namespace {
QVector<CAtlasCanvasView::Patch> patches(const CAtlasEditDialog::Snapshot& snapshot, const CAtlasEditDialog::Plan& plan)
{
    QMap<QString, CAtlasRepackPlanner::Mesh> source;
    for (const auto& mesh : snapshot.meshes) source.insert(mesh.material.id, mesh);
    QVector<CAtlasCanvasView::Patch> result;
    for (const auto& material : plan.state.materials) {
        const auto mesh = source.value(material.id);
        result.append({material.id, material.textureIndex, plan.pixelCoordinates.value(material.id), mesh.triangles, mesh.editable, mesh.reason});
    }
    return result;
}
bool historyFits(const QVector<CAtlasEditDialog::Plan>& history, qint64 maximumPixels, QString* error)
{
    QSet<qint64> images;
    QSet<quintptr> pngs;
    qint64 total = 0;
    const auto add = [&](qint64 count) {
        const qint64 maximum = live2dstruct::CTextureWorkspace::MaximumDecodedBytes;
        if (count < 0 || count > maximum - total) return false;
        total += count; return true;
    };
    if (history.size() > 128) { if (error) *error = QStringLiteral("草稿操作数量达到上限，请先保存图集，再继续编辑。"); return false; }
    qint64 pixels = 0;
    for (const auto& plan : history) {
        for (const auto& image : plan.images) if (!images.contains(image.cacheKey())) {
            images.insert(image.cacheKey());
            const qint64 area = qint64(image.width()) * image.height();
            if (area > maximumPixels - pixels || !add(image.sizeInBytes())) {
                if (error) *error = QStringLiteral("图集草稿历史达到内存预算，请先保存当前修改，再继续编辑。"); return false;
            }
            pixels += area;
        }
        for (const auto& atlas : plan.state.workspace.atlases) {
            const auto address = reinterpret_cast<quintptr>(atlas.png.constData());
            if (!pngs.contains(address)) {
                pngs.insert(address);
                if (!add(atlas.png.size())) { if (error) *error = QStringLiteral("图集草稿历史达到内存预算，请先保存当前修改。"); return false; }
            }
        }
    }
    return true;
}
}

struct CAtlasEditDialog::Job
{
    CExportControl control;
    Plan result;
    QString error;
    bool success = false;
};

CAtlasEditDialog::CAtlasEditDialog(const Snapshot& snapshot, const QStringList& selectedMeshes, QWidget* parent)
    : QDialog(parent), m_snapshot(snapshot), m_selection(selectedMeshes.cbegin(), selectedMeshes.cend())
{
    setObjectName(QStringLiteral("atlasEditDialog")); setWindowTitle(QStringLiteral("编辑纹理图集")); setModal(true);
    const int gap = std::max(0, style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing));
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(gap, gap, gap, gap); layout->setSpacing(gap);
    auto* description = new QLabel(QStringLiteral("移动素材时同步更新网格采样位置。原素材保持不变，保存后可一次撤销。"), this);
    description->setWordWrap(true); description->setTextFormat(Qt::PlainText); layout->addWidget(description);
    const auto button = [&](const QString& text, const char* name, QBoxLayout* row) {
        auto* control = new QPushButton(text, this); control->setObjectName(QString::fromLatin1(name)); control->setAutoDefault(false);
        row->addWidget(control); return control;
    };
    auto* toolbar = new QHBoxLayout;
    m_slot = new QComboBox(this); m_slot->setObjectName(QStringLiteral("atlasCurrentSlot")); m_slot->setAccessibleName(QStringLiteral("当前图集"));
    m_slot->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); m_slot->setMinimumContentsLength(1); toolbar->addWidget(m_slot, 1);
    m_undo = button(QStringLiteral("撤销"), "atlasUndo", toolbar); m_redo = button(QStringLiteral("重做"), "atlasRedo", toolbar);
    auto* fit = button(QStringLiteral("适应图集"), "atlasFit", toolbar); layout->addLayout(toolbar);
    m_splitter = new QSplitter(Qt::Horizontal, this); m_splitter->setObjectName(QStringLiteral("atlasEditorSplitter")); m_splitter->setChildrenCollapsible(false);
    m_workspace = new QSplitter(Qt::Horizontal, m_splitter); m_workspace->setObjectName(QStringLiteral("atlasWorkspaceSplitter")); m_workspace->setChildrenCollapsible(false);
    m_sidebar = new QWidget(m_workspace); auto* objects = new QVBoxLayout(m_sidebar); objects->setContentsMargins(0, 0, 0, 0);
    m_search = new QLineEdit(m_sidebar); m_search->setObjectName(QStringLiteral("atlasObjectSearch")); m_search->setPlaceholderText(QStringLiteral("搜索对象")); m_search->setAccessibleName(QStringLiteral("搜索图集对象")); objects->addWidget(m_search);
    m_meshes = new QListWidget(m_sidebar); m_meshes->setObjectName(QStringLiteral("atlasObjects")); m_meshes->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_meshes->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel); objects->addWidget(m_meshes, 1);
    auto* select = new QHBoxLayout; auto* selectAll = button(QStringLiteral("全选"), "atlasSelectAll", select); auto* focus = button(QStringLiteral("定位选区"), "atlasFitSelection", select); objects->addLayout(select);
    m_canvas = new CAtlasCanvasView(m_workspace); m_workspace->addWidget(m_sidebar); m_workspace->addWidget(m_canvas);
    m_workspace->setStretchFactor(0, 0); m_workspace->setStretchFactor(1, 1);
    m_controls = new QTabWidget(m_splitter); m_controls->setObjectName(QStringLiteral("atlasControls")); Theme::setDocumentMode(*m_controls);
    const auto tab = [&](const QString& title, const char* name) {
        auto* scroll = new QScrollArea(m_controls); scroll->setObjectName(QString::fromLatin1(name)); scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame); scroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
        auto* page = new QWidget(scroll); auto* content = new QVBoxLayout(page); content->setSpacing(gap); scroll->setWidget(page);
        m_controls->addTab(scroll, title); return content;
    };
    const auto form = [](QVBoxLayout* content) {
        auto* fields = new QFormLayout; fields->setRowWrapPolicy(QFormLayout::WrapLongRows); fields->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        content->addLayout(fields); return fields;
    };
    const auto spin = [&](QFormLayout* fields, const QString& label, const char* name, int minimum, int maximum, int value) {
        auto* control = new QSpinBox(this); control->setObjectName(QString::fromLatin1(name)); control->setAccessibleName(label);
        control->setRange(minimum, maximum); control->setValue(value); control->setSuffix(QStringLiteral(" px")); control->setKeyboardTracking(false);
        control->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); fields->addRow(label, control); return control;
    };
    auto* material = tab(QStringLiteral("素材"), "atlasMaterialScroll");
    m_selectionInfo = new QLabel(this); m_selectionInfo->setObjectName(QStringLiteral("atlasSelectionInfo")); m_selectionInfo->setWordWrap(true); m_selectionInfo->setTextFormat(Qt::PlainText); material->addWidget(m_selectionInfo);
    auto* placement = form(material);
    m_destination = new QComboBox(this); m_destination->setObjectName(QStringLiteral("atlasDestinationSlot")); placement->addRow(QStringLiteral("目标图集"), m_destination);
    m_x = spin(placement, QStringLiteral("区域左上 X"), "atlasPositionX", 0, std::numeric_limits<int>::max(), 0);
    m_y = spin(placement, QStringLiteral("区域左上 Y"), "atlasPositionY", 0, std::numeric_limits<int>::max(), 0);
    m_gutter = spin(placement, QStringLiteral("采样边距"), "atlasGutter", 1, 64, 1);
    auto* coordinateHelp = new QLabel(QStringLiteral("坐标包含采样边距。多对象一起移动时保留彼此位置；跨多个来源图集的选区请使用自动排布。"), this);
    coordinateHelp->setWordWrap(true); coordinateHelp->setTextFormat(Qt::PlainText); material->addWidget(coordinateHelp);
    auto* move = button(QStringLiteral("移动到此位置"), "atlasMove", material);
    auto* rotate = button(QStringLiteral("顺时针旋转 90°"), "atlasRotate90", material);
    auto* help = new QLabel(QStringLiteral("拖动素材，松开后更新模型预览。Shift 增选，Alt 减选，空白处框选。\nX / Y 约束拖动；方向键移动 1 px，Shift 移动 10 px。空格或中键平移，滚轮缩放。"), this);
    help->setTextFormat(Qt::PlainText); help->setWordWrap(true); material->addWidget(help); material->addStretch();
    auto* atlas = tab(QStringLiteral("图集"), "atlasSettingsScroll"); auto* dimensions = form(atlas);
    m_name = new QLineEdit(this); m_name->setObjectName(QStringLiteral("atlasName")); dimensions->addRow(QStringLiteral("名称"), m_name);
    auto* rename = button(QStringLiteral("更改名称"), "atlasRename", atlas);
    m_width = spin(dimensions, QStringLiteral("宽度"), "atlasWidth", 1, std::max(1, snapshot.limits.maximumDimension), 1024);
    m_height = spin(dimensions, QStringLiteral("高度"), "atlasHeight", 1, std::max(1, snapshot.limits.maximumDimension), 1024);
    m_resizeImpact = new QLabel(this); m_resizeImpact->setObjectName(QStringLiteral("atlasResizeImpact")); m_resizeImpact->setWordWrap(true); m_resizeImpact->setTextFormat(Qt::PlainText); atlas->addWidget(m_resizeImpact);
    auto* resizeAtlas = button(QStringLiteral("调整画布尺寸"), "atlasResize", atlas);
    auto* remove = button(QStringLiteral("删除空的新增图集"), "atlasRemove", atlas);
    auto* newFields = form(atlas); m_newName = new QLineEdit(this); m_newName->setObjectName(QStringLiteral("atlasNewName")); m_newName->setPlaceholderText(QStringLiteral("新图集")); newFields->addRow(QStringLiteral("新图集名称"), m_newName);
    m_newWidth = spin(newFields, QStringLiteral("新图集宽度"), "atlasNewWidth", 1, std::max(1, snapshot.limits.maximumDimension), 1024);
    m_newHeight = spin(newFields, QStringLiteral("新图集高度"), "atlasNewHeight", 1, std::max(1, snapshot.limits.maximumDimension), 1024);
    auto* add = button(QStringLiteral("新建图集"), "atlasAdd", atlas); atlas->addStretch();
    auto* packing = tab(QStringLiteral("排布"), "atlasPackingScroll");
    auto* packingHelp = new QLabel(QStringLiteral("将选中的素材重新排入勾选图集。目标图集的现有引用对象必须全部选中。重叠区域会成组保留，不改变素材分辨率。"), this);
    packingHelp->setWordWrap(true); packingHelp->setTextFormat(Qt::PlainText); packing->addWidget(packingHelp);
    m_packTargets = new QListWidget(this); m_packTargets->setObjectName(QStringLiteral("atlasPackTargets")); m_packTargets->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents); packing->addWidget(m_packTargets, 1);
    m_rotate = new QCheckBox(QStringLiteral("允许旋转 90°以节省空间"), this); m_rotate->setObjectName(QStringLiteral("atlasPackRotation")); m_rotate->setChecked(true); packing->addWidget(m_rotate);
    auto* pack = button(QStringLiteral("自动排布选中素材"), "atlasPack", packing);
    m_splitter->addWidget(m_workspace); m_splitter->addWidget(m_controls); m_splitter->setStretchFactor(0, 1); m_splitter->setStretchFactor(1, 0);
    layout->addWidget(m_splitter, 1);
    m_status = new QLabel(this); m_status->setObjectName(QStringLiteral("atlasStatus")); m_status->setTextFormat(Qt::PlainText); m_status->setWordWrap(true); layout->addWidget(m_status);
    m_error = new QLabel(this); m_error->setObjectName(QStringLiteral("atlasError")); m_error->setTextFormat(Qt::PlainText); m_error->setWordWrap(true); m_error->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(m_error);
    auto* recovery = new QHBoxLayout; m_discard = button(QStringLiteral("放弃未应用的数值输入"), "atlasDiscardInput", recovery);
    auto* reset = button(QStringLiteral("恢复进入时图集"), "atlasReset", recovery); m_stop = button(QStringLiteral("停止计算"), "atlasStop", recovery); layout->addLayout(recovery);
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    auto* save = m_buttons->button(QDialogButtonBox::Save); save->setText(QStringLiteral("保存图集")); save->setObjectName(QStringLiteral("primaryBtn")); save->style()->unpolish(save); save->style()->polish(save);
    m_buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消")); layout->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &CAtlasEditDialog::accept); connect(m_buttons, &QDialogButtonBox::rejected, this, &CAtlasEditDialog::reject);
    connect(fit, &QPushButton::clicked, m_canvas, &CAtlasCanvasView::FitAtlas); connect(focus, &QPushButton::clicked, m_canvas, &CAtlasCanvasView::FitSelection);
    connect(selectAll, &QPushButton::clicked, this, [this] {
        if (HasPendingInput()) { ShowError(QStringLiteral("请先应用或放弃数值输入，再改变选区。")); return; }
        QSet<QString> ids; for (const auto& mesh : m_snapshot.meshes) ids.insert(mesh.material.id); m_canvas->SetSelectedMeshes(ids);
    });
    connect(m_canvas, &CAtlasCanvasView::SelectionChanged, this, &CAtlasEditDialog::RefreshSelection);
    connect(m_canvas, &CAtlasCanvasView::TranslationRequested, this, &CAtlasEditDialog::MoveBy);
    connect(m_meshes, &QListWidget::itemSelectionChanged, this, [this] {
        if (m_refreshing) return;
        if (HasPendingInput()) { ShowError(QStringLiteral("请先应用或放弃数值输入，再改变选区。")); Refresh(); return; }
        QSet<QString> ids; for (const auto* item : m_meshes->selectedItems()) ids.insert(item->data(Qt::UserRole).toString());
        m_canvas->SetSelectedMeshes(ids);
    });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& text) {
        for (int index = 0; index < m_meshes->count(); ++index) m_meshes->item(index)->setHidden(!m_meshes->item(index)->data(Qt::UserRole + 1).toString().contains(text, Qt::CaseInsensitive));
    });
    connect(m_slot, &QComboBox::currentIndexChanged, this, &CAtlasEditDialog::ChangeSlot);
    const auto dirty = [this](bool& value) { if (!m_refreshing) { value = true; m_discard->setEnabled(true); } };
    for (auto* input : {m_x, m_y}) connect(input, &QSpinBox::valueChanged, this, [this, dirty] { dirty(m_moveDirty); });
    connect(m_destination, &QComboBox::currentIndexChanged, this, [this, dirty] { dirty(m_moveDirty); });
    for (auto* input : {m_width, m_height}) connect(input, &QSpinBox::valueChanged, this, [this, dirty] { dirty(m_sizeDirty); });
    connect(m_name, &QLineEdit::textChanged, this, [this, dirty] { dirty(m_nameDirty); });
    connect(m_newName, &QLineEdit::textChanged, this, [this, dirty] { dirty(m_newDirty); });
    for (auto* input : {m_newWidth, m_newHeight}) connect(input, &QSpinBox::valueChanged, this, [this, dirty] { dirty(m_newDirty); });
    connect(m_gutter, &QSpinBox::valueChanged, this, [this](int value) { m_canvas->SetGutter(value); Refresh(); });
    connect(move, &QPushButton::clicked, this, [this] { MoveSelection(); }); connect(rotate, &QPushButton::clicked, this, [this] { MoveSelection(1); });
    connect(add, &QPushButton::clicked, this, &CAtlasEditDialog::AddAtlas); connect(resizeAtlas, &QPushButton::clicked, this, &CAtlasEditDialog::ResizeAtlas);
    connect(rename, &QPushButton::clicked, this, &CAtlasEditDialog::RenameAtlas); connect(remove, &QPushButton::clicked, this, &CAtlasEditDialog::RemoveAtlas);
    connect(pack, &QPushButton::clicked, this, &CAtlasEditDialog::Pack); connect(m_undo, &QPushButton::clicked, this, &CAtlasEditDialog::Undo); connect(m_redo, &QPushButton::clicked, this, &CAtlasEditDialog::Redo);
    connect(reset, &QPushButton::clicked, this, &CAtlasEditDialog::Reset); connect(m_discard, &QPushButton::clicked, this, &CAtlasEditDialog::DiscardInputs); connect(m_stop, &QPushButton::clicked, this, &CAtlasEditDialog::CancelCalculation);
    auto* undo = new QShortcut(QKeySequence::Undo, this); connect(undo, &QShortcut::activated, this, &CAtlasEditDialog::Undo);
    auto* redo = new QShortcut(QKeySequence::Redo, this); connect(redo, &QShortcut::activated, this, &CAtlasEditDialog::Redo);
    resize(fontMetrics().averageCharWidth() * 150, fontMetrics().height() * 42);
    m_workspace->setSizes({fontMetrics().averageCharWidth() * 24, fontMetrics().averageCharWidth() * 80});
    m_splitter->setSizes({fontMetrics().averageCharWidth() * 100, fontMetrics().averageCharWidth() * 42});
    Run([](const auto& source, const auto&, auto& result, auto* error, auto* control) {
        return CAtlasRepackPlanner::Initialize(source, result, error, control);
    }, true);
}

CAtlasEditDialog::~CAtlasEditDialog() { ++m_generation; if (m_job) m_job->control.Cancel(); }
QString CAtlasEditDialog::Error() const { return m_error->text(); }
void CAtlasEditDialog::SetPreviewHandler(Handler handler) { if (!m_busy) m_preview = std::move(handler); }
void CAtlasEditDialog::SetSubmissionHandler(Handler handler) { if (!m_busy) m_submit = std::move(handler); }
void CAtlasEditDialog::SetCancellationHandler(CancellationHandler handler) { if (!m_busy) m_cancel = std::move(handler); }
bool CAtlasEditDialog::SetMeshDisplayNames(const QMap<QString, QString>& names, QString* error)
{
    QMap<QString, QString> owned;
    if (!live2dstruct::CModelDisplayNames::NormalizeMap(names, owned, error)) return false;
    m_meshDisplayNames = std::move(owned);
    if (m_ready) Refresh();
    return true;
}

void CAtlasEditDialog::ShowError(const QString& error) { m_error->setText(error); m_error->setVisible(!error.isEmpty()); }
bool CAtlasEditDialog::HasPendingInput() const { return m_moveDirty || m_sizeDirty || m_nameDirty || m_newDirty; }
QStringList CAtlasEditDialog::Selection() const { auto ids = m_selection.values(); std::sort(ids.begin(), ids.end()); return ids; }

void CAtlasEditDialog::Run(Operation operation, bool initialize)
{
    if (IsBusy() || (!initialize && !m_ready)) return;
    auto job = std::make_shared<Job>(); m_job = job; const auto generation = ++m_generation;
    const auto snapshot = m_snapshot; const auto before = m_plan;
    auto* worker = QThread::create([job, snapshot, before, operation] {
        try { job->success = operation(snapshot, before, job->result, &job->error, &job->control); }
        catch (...) { job->error = QStringLiteral("图集计算失败，已接受的修改保持不变"); }
        if (job->control.IsCancellationRequested()) { job->success = false; job->error = QStringLiteral("已停止计算，图集保持不变。"); }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, job, generation, initialize] {
        if (generation != m_generation || m_job != job) return;
        m_job.reset();
        if (m_closeWhenIdle) { m_closeWhenIdle = false; reject(); return; }
        if (job->control.IsCancellationRequested()) { job->success = false; job->error = QStringLiteral("已停止计算，图集保持不变。"); }
        if (!job->success) {
            if (initialize) m_valid = false;
            ShowError(job->error.isEmpty() ? QStringLiteral("图集计算未能完成") : job->error); Refresh(); return;
        }
        const QPointer<CAtlasEditDialog> alive(this);
        if (initialize) {
            QString error;
            if (!m_canvas->SetContent(job->result.images, patches(m_snapshot, job->result), &error)) { m_valid = false; ShowError(error); Refresh(); return; }
            m_plan = job->result; m_initial = m_plan; m_history = {m_plan}; m_ready = true;
            m_canvas->SetSelectedMeshes(m_selection); if (!alive) return;
            if (!m_selection.isEmpty()) for (const auto& value : m_plan.state.materials) if (m_selection.contains(value.id)) { m_canvas->SetSlot(value.textureIndex); break; }
            Refresh(); emit Ready();
        } else if (Publish(job->result, true) && alive) {
            m_moveDirty = m_sizeDirty = m_nameDirty = m_newDirty = false; Refresh();
        }
    }, Qt::QueuedConnection);
    ShowError({}); Refresh(); worker->start();
}

bool CAtlasEditDialog::Publish(const Plan& plan, bool remember)
{
    if (IsBusy() || !m_ready || !m_valid) return false;
    if (plan.state == m_plan.state) return true;
    if (!m_preview) { ShowError(QStringLiteral("图集预览尚未连接")); return false; }
    const QPointer<CAtlasEditDialog> alive(this); QString error; bool accepted = false;
    try {
        auto candidate = plan; auto history = m_history;
        if (remember) { history.resize(m_historyIndex + 1); history.append(candidate); }
        if (!historyFits(history, m_snapshot.limits.maximumPixels, &error)) { ShowError(error); return false; }
        CAtlasCanvasView::Content canvasContent;
        if (!m_canvas->PrepareContent(candidate.images, patches(m_snapshot, candidate), canvasContent, &error)) { ShowError(error); return false; }
        int selectedSlot = -1;
        for (const auto& material : candidate.state.materials) if (m_selection.contains(material.id)) {
            if (selectedSlot == -1) selectedSlot = material.textureIndex;
            else if (selectedSlot != material.textureIndex) { selectedSlot = -2; break; }
        }
        for (auto atlas = candidate.state.workspace.atlases.cbegin(); atlas != candidate.state.workspace.atlases.cend(); ++atlas)
            if (!m_plan.state.workspace.atlases.contains(atlas.key())) selectedSlot = atlas.key();
        auto handler = m_preview; m_busy = true; const auto done = qScopeGuard([alive] { if (alive) alive->m_busy = false; });
        try { accepted = handler(candidate, &error); }
        catch (...) { error = QStringLiteral("图集预览失败，已接受的修改保持不变"); }
        if (!alive) return accepted;
        if (accepted) {
            m_plan = std::move(candidate);
            if (remember) { m_history.swap(history); m_historyIndex = m_history.size() - 1; }
            m_canvas->AdoptContent(std::move(canvasContent));
            if (selectedSlot >= 0) m_canvas->SetSlot(selectedSlot);
        }
    } catch (...) { error = QStringLiteral("无法准备图集草稿，已接受的修改保持不变"); }
    if (!alive) return accepted;
    ShowError(accepted ? QString() : error.isEmpty() ? QStringLiteral("图集预览未能发布") : error); Refresh(); return accepted;
}

QString CAtlasEditDialog::ResizeImpact(int slot) const
{
    QStringList affected, locked;
    for (const auto& material : m_plan.state.materials) if (material.textureIndex == slot && !material.uvs.isEmpty()) {
        affected.append(material.id);
        for (const auto& mesh : m_snapshot.meshes) if (mesh.material.id == material.id && !mesh.editable) locked.append(material.id + QStringLiteral("：") + mesh.reason);
    }
    QString text = QStringLiteral("保持素材像素密度，更新此图集全部 %1 个网格的 UV。\n%2").arg(affected.size()).arg(affected.join(QStringLiteral("、")));
    if (!locked.isEmpty()) text += QStringLiteral("\n尺寸修改受锁定对象阻止：\n") + locked.join(QLatin1Char('\n'));
    return text;
}

void CAtlasEditDialog::Refresh()
{
    if (m_refreshing) return;
    m_refreshing = true; const auto restore = qScopeGuard([this] { m_refreshing = false; });
    const bool editable = m_ready && m_valid && !IsBusy();
    m_sidebar->setEnabled(editable); m_controls->setEnabled(editable); m_slot->setEnabled(editable);
    m_canvas->SetEditingEnabled(editable && !HasPendingInput());
    m_undo->setEnabled(editable && m_historyIndex > 0); m_redo->setEnabled(editable && m_historyIndex + 1 < m_history.size());
    m_stop->setEnabled(bool(m_job)); m_discard->setEnabled(editable && HasPendingInput());
    m_buttons->button(QDialogButtonBox::Save)->setEnabled(editable); m_buttons->button(QDialogButtonBox::Cancel)->setEnabled(!m_busy);
    if (!m_ready) { m_status->setText(m_job ? QStringLiteral("正在读取图集…") : QStringLiteral("图集未就绪")); return; }
    const int slot = m_canvas->Slot(), destination = m_destination->currentData().toInt();
    QSet<int> checkedTargets;
    for (int index = 0; index < m_packTargets->count(); ++index) if (m_packTargets->item(index)->checkState() == Qt::Checked) checkedTargets.insert(m_packTargets->item(index)->data(Qt::UserRole).toInt());
    const bool initialTargets = m_packTargets->count() == 0;
    const QSignalBlocker slotSignals(m_slot), destinations(m_destination), names(m_name), widths(m_width), heights(m_height), objects(m_meshes);
    m_slot->clear(); m_destination->clear(); m_packTargets->clear();
    for (auto atlas = m_plan.state.workspace.atlases.cbegin(); atlas != m_plan.state.workspace.atlases.cend(); ++atlas) {
        const auto text = QStringLiteral("%1  %2 × %3").arg(atlas->name).arg(atlas->size.width()).arg(atlas->size.height());
        m_slot->addItem(text, atlas.key()); m_destination->addItem(text, atlas.key());
        auto* item = new QListWidgetItem(text, m_packTargets); item->setData(Qt::UserRole, atlas.key()); item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(checkedTargets.contains(atlas.key()) || (initialTargets && atlas.key() == slot) ? Qt::Checked : Qt::Unchecked);
    }
    m_slot->setCurrentIndex(m_slot->findData(slot));
    m_destination->setCurrentIndex(m_destination->findData(m_moveDirty ? destination : slot));
    m_meshes->clear();
    for (const auto& mesh : m_snapshot.meshes) {
        const auto name = m_meshDisplayNames.value(mesh.material.id, mesh.material.id);
        const auto label = name == mesh.material.id ? mesh.material.id : name + QStringLiteral(" · ") + mesh.material.id;
        auto* item = new QListWidgetItem(label + (mesh.editable ? QString() : QStringLiteral("  · 已锁定")), m_meshes);
        item->setData(Qt::UserRole, mesh.material.id); item->setData(Qt::UserRole + 1, label);
        item->setToolTip(QStringLiteral("<qt>%1</qt>").arg((mesh.editable ? label : mesh.reason).toHtmlEscaped())); item->setSelected(m_selection.contains(mesh.material.id));
        item->setHidden(!label.contains(m_search->text(), Qt::CaseInsensitive));
    }
    const auto atlas = m_plan.state.workspace.atlases.constFind(slot);
    if (atlas != m_plan.state.workspace.atlases.cend()) {
        if (!m_nameDirty) m_name->setText(atlas->name);
        if (!m_sizeDirty) { m_width->setValue(atlas->size.width()); m_height->setValue(atlas->size.height()); }
    }
    QRect bounds; int sourceSlot = -1; QString selectionError;
    if (CAtlasRepackPlanner::SelectionBounds(m_snapshot, m_plan, Selection(), m_gutter->value(), bounds, sourceSlot, &selectionError)) {
        if (!m_moveDirty) { const QSignalBlocker x(m_x), y(m_y); m_x->setValue(std::max(0, bounds.left())); m_y->setValue(std::max(0, bounds.top())); }
        m_selectionInfo->setText(QStringLiteral("已选 %1 个对象\n素材区域 %2 × %3 px（含边距）").arg(m_selection.size()).arg(bounds.width()).arg(bounds.height()));
    } else m_selectionInfo->setText(m_selection.isEmpty() ? QStringLiteral("选择画布素材或列表对象以移动、旋转和排布。") : QStringLiteral("已选 %1 个对象\n%2").arg(m_selection.size()).arg(selectionError));
    m_resizeImpact->setText(ResizeImpact(slot));
    m_status->setText(m_job ? QStringLiteral("正在计算图集，可停止或取消…") :
        QStringLiteral("影响 %1 个网格、%2 张图集%3").arg(m_plan.changedMeshes.size()).arg(m_plan.changedSlots.size())
        .arg(m_plan.warnings.isEmpty() ? QString() : QLatin1Char('\n') + m_plan.warnings.join(QLatin1Char('\n'))));
}

void CAtlasEditDialog::RefreshSelection()
{
    if (m_refreshing) return;
    if (HasPendingInput()) { const QSignalBlocker blocker(m_canvas); m_canvas->SetSelectedMeshes(m_selection); ShowError(QStringLiteral("请先应用或放弃数值输入，再改变选区。")); return; }
    m_selection = m_canvas->SelectedMeshes();
    for (const auto& material : m_plan.state.materials) if (m_selection.contains(material.id)) { m_canvas->SetSlot(material.textureIndex); break; }
    Refresh();
}
void CAtlasEditDialog::ChangeSlot()
{
    if (m_refreshing || IsBusy()) return;
    if (HasPendingInput()) { ShowError(QStringLiteral("请先应用或放弃数值输入，再切换图集。")); Refresh(); return; }
    m_canvas->SetSlot(m_slot->currentData().toInt()); Refresh();
}
bool CAtlasEditDialog::CheckInputs()
{
    if (IsBusy() || !m_ready || !m_valid) return false;
    for (auto* input : {m_x, m_y, m_width, m_height, m_newWidth, m_newHeight, m_gutter}) {
        if (!input->hasAcceptableInput()) { ShowError(QStringLiteral("请先修正无效的数值输入。")); input->setFocus(); return false; }
        input->interpretText();
    }
    return true;
}
void CAtlasEditDialog::MoveSelection(int quarterTurns)
{
    if (!CheckInputs()) return;
    if (m_sizeDirty || m_nameDirty || m_newDirty) { ShowError(QStringLiteral("请先应用或放弃图集设置，再移动素材。")); return; }
    const auto ids = Selection(); const int slot = m_destination->currentData().toInt(), gutter = m_gutter->value(); const QPoint position(m_x->value(), m_y->value());
    Run([ids, slot, gutter, position, quarterTurns](const auto& source, const auto& plan, auto& result, auto* error, auto* control) {
        return CAtlasRepackPlanner::Move(source, plan, ids, slot, position, quarterTurns, gutter, result, error, control);
    });
}
void CAtlasEditDialog::MoveBy(const QStringList& ids, QPoint delta)
{
    if (IsBusy() || !m_ready) return;
    if (HasPendingInput()) { ShowError(QStringLiteral("请先应用或放弃数值输入，再拖动素材。")); return; }
    QRect bounds; int slot = -1; QString error; const int gutter = m_gutter->value();
    if (!CAtlasRepackPlanner::SelectionBounds(m_snapshot, m_plan, ids, gutter, bounds, slot, &error)) { ShowError(error); return; }
    const qint64 x = qint64(bounds.x()) + delta.x(), y = qint64(bounds.y()) + delta.y();
    if (x < 0 || y < 0 || x > std::numeric_limits<int>::max() || y > std::numeric_limits<int>::max()) { ShowError(QStringLiteral("素材区域超出目标图集边界。")); return; }
    const QPoint position{int(x), int(y)};
    Run([ids, slot, gutter, position](const auto& source, const auto& plan, auto& result, auto* detail, auto* control) {
        return CAtlasRepackPlanner::Move(source, plan, ids, slot, position, 0, gutter, result, detail, control);
    });
}
void CAtlasEditDialog::AddAtlas()
{
    if (!CheckInputs()) return;
    if (m_moveDirty || m_sizeDirty || m_nameDirty) { ShowError(QStringLiteral("请先应用或放弃当前修改，再新建图集。")); return; }
    const auto name = m_newName->text().trimmed().isEmpty() ? QStringLiteral("新图集") : m_newName->text().trimmed(); const QSize size(m_newWidth->value(), m_newHeight->value());
    Run([name, size](const auto& source, const auto& plan, auto& result, auto* error, auto* control) { return CAtlasRepackPlanner::AddAtlas(source, plan, name, size, result, error, control); });
}
void CAtlasEditDialog::ResizeAtlas()
{
    if (!CheckInputs()) return;
    if (m_moveDirty || m_nameDirty || m_newDirty) { ShowError(QStringLiteral("请先应用或放弃其它输入，再调整画布尺寸。")); return; }
    const int slot = m_canvas->Slot(); const QSize size(m_width->value(), m_height->value());
    Run([slot, size](const auto& source, const auto& plan, auto& result, auto* error, auto* control) { return CAtlasRepackPlanner::ResizeAtlas(source, plan, slot, size, result, error, control); });
}
void CAtlasEditDialog::RenameAtlas()
{
    if (!CheckInputs()) return;
    if (m_moveDirty || m_sizeDirty || m_newDirty) { ShowError(QStringLiteral("请先应用或放弃其它输入，再更改名称。")); return; }
    const int slot = m_canvas->Slot(); const auto name = m_name->text();
    Run([slot, name](const auto& source, const auto& plan, auto& result, auto* error, auto*) { return CAtlasRepackPlanner::RenameAtlas(source, plan, slot, name, result, error); });
}
void CAtlasEditDialog::RemoveAtlas()
{
    if (!CheckInputs()) return;
    if (HasPendingInput()) { ShowError(QStringLiteral("请先应用或放弃数值输入，再删除图集。")); return; }
    const int slot = m_canvas->Slot();
    Run([slot](const auto& source, const auto& plan, auto& result, auto* error, auto* control) { return CAtlasRepackPlanner::RemoveAtlas(source, plan, slot, result, error, control); });
}
void CAtlasEditDialog::Pack()
{
    if (!CheckInputs()) return;
    if (HasPendingInput()) { ShowError(QStringLiteral("请先应用或放弃数值输入，再自动排布。")); return; }
    const auto ids = Selection(); QVector<int> targetSlots;
    for (int index = 0; index < m_packTargets->count(); ++index) if (m_packTargets->item(index)->checkState() == Qt::Checked) targetSlots.append(m_packTargets->item(index)->data(Qt::UserRole).toInt());
    const int gutter = m_gutter->value(); const bool rotate = m_rotate->isChecked();
    Run([ids, targetSlots, gutter, rotate](const auto& source, const auto& plan, auto& result, auto* error, auto* control) { return CAtlasRepackPlanner::Pack(source, plan, ids, targetSlots, gutter, rotate, result, error, control); });
}
void CAtlasEditDialog::Undo()
{
    if (!CheckInputs() || m_historyIndex <= 0) return;
    if (HasPendingInput()) { ShowError(QStringLiteral("请先应用或放弃数值输入，再撤销。")); return; }
    const QPointer<CAtlasEditDialog> alive(this); const int next = m_historyIndex - 1;
    if (Publish(m_history[next], false) && alive) { m_historyIndex = next; Refresh(); }
}
void CAtlasEditDialog::Redo()
{
    if (!CheckInputs() || m_historyIndex + 1 >= m_history.size()) return;
    if (HasPendingInput()) { ShowError(QStringLiteral("请先应用或放弃数值输入，再重做。")); return; }
    const QPointer<CAtlasEditDialog> alive(this); const int next = m_historyIndex + 1;
    if (Publish(m_history[next], false) && alive) { m_historyIndex = next; Refresh(); }
}
void CAtlasEditDialog::Reset()
{
    if (!CheckInputs()) return;
    if (HasPendingInput()) { ShowError(QStringLiteral("请先应用或放弃数值输入，再恢复图集。")); return; }
    Publish(m_initial, true);
}
void CAtlasEditDialog::DiscardInputs()
{
    if (IsBusy()) return;
    m_refreshing = true;
    m_moveDirty = m_sizeDirty = m_nameDirty = m_newDirty = false;
    m_newName->clear(); m_newWidth->setValue(std::min(1024, m_snapshot.limits.maximumDimension)); m_newHeight->setValue(std::min(1024, m_snapshot.limits.maximumDimension));
    m_refreshing = false; ShowError({}); Refresh();
}
void CAtlasEditDialog::CancelCalculation() { if (m_job) { m_job->control.Cancel(); m_status->setText(QStringLiteral("正在停止计算…")); } }

void CAtlasEditDialog::accept()
{
    if (!CheckInputs()) return;
    if (m_canvas->HasGesture()) { ShowError(QStringLiteral("请先完成或取消当前素材拖动，再保存图集。")); return; }
    if (HasPendingInput()) { ShowError(QStringLiteral("仍有未应用的输入，请应用或放弃输入，再保存图集。")); return; }
    if (!m_submit) { ShowError(QStringLiteral("图集保存尚未连接")); return; }
    const QPointer<CAtlasEditDialog> alive(this); QString error; bool accepted = false; const auto handler = m_submit;
    m_busy = true;
    try { accepted = handler(m_plan, &error); } catch (...) { error = QStringLiteral("图集保存失败，草稿仍保留在窗口中"); }
    if (!alive) return;
    m_busy = false;
    if (!accepted) { ShowError(error.isEmpty() ? QStringLiteral("图集保存未能完成") : error); Refresh(); return; }
    m_accepted = m_finished = true; QDialog::accept();
}
void CAtlasEditDialog::reject()
{
    if (m_busy) return;
    if (m_accepted) { QDialog::reject(); return; }
    if (m_job) { m_closeWhenIdle = true; CancelCalculation(); return; }
    const QPointer<CAtlasEditDialog> alive(this); QString error; bool canceled = true; const auto handler = m_cancel;
    m_busy = true;
    try {
        if (handler) canceled = handler(&error);
        else if (m_ready && m_plan.state != m_initial.state) { canceled = false; error = QStringLiteral("图集恢复尚未连接"); }
    } catch (...) { canceled = false; error = QStringLiteral("图集恢复失败，请保留窗口并重试"); }
    if (!alive) return;
    m_busy = false;
    if (!canceled) { ShowError(error.isEmpty() ? QStringLiteral("无法恢复进入时图集，请重试取消") : error); Refresh(); return; }
    m_canvas->CancelGesture(); m_plan = m_initial; m_finished = true; QDialog::reject();
}
void CAtlasEditDialog::done(int result)
{
    if (m_finished) QDialog::done(result);
    else if (result == QDialog::Accepted) accept();
    else reject();
}
void CAtlasEditDialog::closeEvent(QCloseEvent* event) { event->ignore(); reject(); }
void CAtlasEditDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event); if (!m_splitter) return;
    const bool narrow = event->size().width() < fontMetrics().averageCharWidth() * 130;
    const auto orientation = narrow ? Qt::Vertical : Qt::Horizontal;
    if (m_splitter->orientation() != orientation) {
        m_splitter->setOrientation(orientation);
        m_splitter->setSizes(narrow ? QList<int>{event->size().height() * 3 / 5, event->size().height() * 2 / 5} : QList<int>{event->size().width() * 2 / 3, event->size().width() / 3});
    }
    if (m_workspace) {
        const auto inner = event->size().width() < fontMetrics().averageCharWidth() * 70 ? Qt::Vertical : Qt::Horizontal;
        if (m_workspace->orientation() != inner) { m_workspace->setOrientation(inner); m_workspace->setSizes({fontMetrics().height() * 6, event->size().height() / 2}); }
    }
}
