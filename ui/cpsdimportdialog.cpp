#include "cpsdimportdialog.h"
#include "cartworksourceview.h"
#include "theme.h"
#include "live2dstruct/cpsdlayermatcher.h"
#include "std/cpngimage.h"
#include "std/cexportcontrol.h"
#include <QAbstractItemModel>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStyle>
#include <QThread>
#include <QTreeView>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using namespace live2dstruct;
namespace {
using Planner = CArtworkUpdatePlanner;
using Artwork = CModelArtwork;
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
QString actionName(Planner::Action action)
{
    switch (action) {
    case Planner::Action::Keep: return QStringLiteral("保留当前素材");
    case Planner::Action::Associate: return QStringLiteral("关联到所选图层");
    case Planner::Action::Update: return QStringLiteral("更新已有关联");
    case Planner::Action::Detach: return QStringLiteral("解除来源关联");
    }
    return {};
}
bool same(const QTransform& a, const QTransform& b)
{
    const double first[]{a.m11(), a.m12(), a.m13(), a.m21(), a.m22(), a.m23(), a.dx(), a.dy(), a.m33()};
    const double second[]{b.m11(), b.m12(), b.m13(), b.m21(), b.m22(), b.m23(), b.dx(), b.dy(), b.m33()};
    return !std::memcmp(first, second, sizeof(first));
}
QTransform inverted(const QTransform& m)
{
    const double scale = std::max({std::abs(m.m11()), std::abs(m.m12()), std::abs(m.m21()), std::abs(m.m22())});
    const double a = m.m11() / scale, b = m.m12() / scale, c = m.m21() / scale, d = m.m22() / scale, determinant = a * d - b * c;
    const double x = (d / determinant) / scale, y = (-b / determinant) / scale, z = (-c / determinant) / scale, w = (a / determinant) / scale;
    return {x, y, z, w, -x * m.dx() - z * m.dy(), -y * m.dx() - w * m.dy()};
}
QString path(const QVector<Artwork::Layer>& layers, int index)
{
    QStringList names; QSet<int> seen;
    while (index >= 0 && index < layers.size() && !seen.contains(index)) { seen.insert(index); names.prepend(layers[index].name); index = layers[index].parentIndex; }
    return names.join(QStringLiteral(" / "));
}
QString diagnosticText(QString value)
{
    static const QRegularExpression code(QStringLiteral("^[a-z][a-z0-9-]*:\u0020*"));
    return value.remove(code);
}
QStringList diagnosticText(const QStringList& values)
{
    QStringList result; for (const auto& value : values) { const auto text = diagnosticText(value); if (!result.contains(text)) result.append(text); } return result;
}
bool colorDiagnostic(const QString& value)
{
    return value.startsWith(QLatin1String("color-policy:")) || value.startsWith(QLatin1String("missing-icc:")) || value.startsWith(QLatin1String("invalid-icc:"));
}
QString colorPolicyText(const CArtworkSourceImport::SourceCandidate& source)
{
    const bool preserve = source.revision.colorPolicy == Artwork::ColorPolicy::PreserveValues;
    if (source.revision.iccProfile.isEmpty()) return preserve
        ? QStringLiteral("颜色：来源未带 ICC，保留 RGB 数值；预览按 sRGB 显示，不进行色彩转换。")
        : QStringLiteral("颜色：来源未带 ICC，按 sRGB 解释像素，RGB 数值保持不变。");
    const bool invalid = std::any_of(source.diagnostics.cbegin(), source.diagnostics.cend(), [](const QString& value) { return value.startsWith(QLatin1String("invalid-icc:")); });
    if (preserve) return invalid ? QStringLiteral("颜色：保留原始 RGB 数值与 ICC，不进行色彩转换；该 ICC 无法用于转换。")
        : QStringLiteral("颜色：保留原始 RGB 数值与 ICC，不进行色彩转换。");
    return QStringLiteral("颜色：已根据来源 ICC 转换到 sRGB；原始来源和 ICC 保存在工作会话中。");
}
bool supported(const Artwork::Layer& layer) { return layer.kind == Artwork::LayerKind::Raster && layer.pixelEquivalent; }
bool prepareRecipe(const CArtworkSourceImport::SourceCandidate& original, CArtworkSourceImport::SourceCandidate& target, QString* error, CExportControl* control)
{
    const auto policy = target.revision.colorPolicy; const auto mapping = target.revision.documentToFamily;
    if (policy == original.revision.colorPolicy && same(mapping, original.revision.documentToFamily)) { target = original; return true; }
    QByteArray recipe; QDataStream stream(&recipe, QIODevice::WriteOnly); stream.setVersion(QDataStream::Qt_6_0);
    stream << original.revision.id << original.revision.familyId << original.revision.sourceDigest << qint32(policy)
        << mapping.m11() << mapping.m12() << mapping.m21() << mapping.m22() << mapping.dx() << mapping.dy();
    const auto id = QStringLiteral("revision:") + QString::fromLatin1(QCryptographicHash::hash(recipe, QCryptographicHash::Sha256).toHex());
    if (original.kind == Artwork::SourceKind::Psd) {
        CPsdSnapshot snapshot; snapshot.bytes = original.revision.sourceBytes; snapshot.canonicalPath = original.revision.path;
        CArtworkSourceImport::SourceCandidate candidate;
        if (!CArtworkSourceImport::Prepare(snapshot, original.revision.familyId, id, policy, mapping, candidate, error, control)) return false;
        candidate.revision.path = original.revision.path; candidate.revision.fileName = original.revision.fileName;
        target = std::move(candidate);
    } else { target = original; target.revision.id = id; target.revision.documentToFamily = mapping; }
    return true;
}

Planner::Request sourceOnly(Planner::Request request)
{
    for (auto& decision : request.decisions) { decision.action = Planner::Action::Keep; decision.layerId.clear(); decision.confirmed = false; }
    return request;
}
}

class CPsdImportLayerModel : public QAbstractItemModel
{
public:
    explicit CPsdImportLayerModel(QObject* parent) : QAbstractItemModel(parent) {}
    void SetLayers(QVector<Artwork::Layer> layers)
    {
        beginResetModel(); m_layers = std::move(layers); m_children.clear(); m_rows.clear();
        for (int index = 0; index < m_layers.size(); ++index) { auto& siblings = m_children[m_layers[index].parentIndex]; m_rows.append(siblings.size()); siblings.append(index); }
        endResetModel();
    }
    void SetUsed(const QSet<QString>& ids) {
        if (m_used == ids) return; m_used = ids;
        for (auto it = m_children.cbegin(); it != m_children.cend(); ++it) if (!it->isEmpty()) {
            const auto parent = it.key() < 0 ? QModelIndex() : createIndex(m_rows[it.key()], 0, quintptr(it.key() + 1));
            emit dataChanged(index(0, 0, parent), index(it->size() - 1, 1, parent));
        }
    }
    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override
    {
        const int id = parent.isValid() ? int(parent.internalId()) - 1 : -1;
        const auto found = m_children.constFind(id);
        if (column < 0 || column >= 2 || row < 0 || found == m_children.cend() || row >= found->size()) return {};
        return createIndex(row, column, quintptr(found->at(row) + 1));
    }
    QModelIndex parent(const QModelIndex& child) const override
    {
        if (!child.isValid()) return {};
        const int id = int(child.internalId()) - 1, parent = m_layers[id].parentIndex;
        return parent < 0 ? QModelIndex() : createIndex(m_rows[parent], 0, quintptr(parent + 1));
    }
    int rowCount(const QModelIndex& parent = {}) const override
    { return parent.column() > 0 ? 0 : m_children.value(parent.isValid() ? int(parent.internalId()) - 1 : -1).size(); }
    int columnCount(const QModelIndex& = {}) const override { return 2; }
    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid()) return {};
        const int id = int(index.internalId()) - 1; const auto& layer = m_layers[id];
        if (role == Qt::UserRole) return layer.id;
        if (role == Qt::UserRole + 1) return path(m_layers, id);
        if (role == Qt::UserRole + 2) return !m_used.contains(layer.id);
        if (role == Qt::UserRole + 3) return !layer.diagnostics.isEmpty() || !supported(layer);
        if (role == Qt::DisplayRole) return index.column() == 0 ? layer.name : layer.kind == Artwork::LayerKind::Group ? QStringLiteral("文件夹") :
            !supported(layer) ? QStringLiteral("无法更新") : m_used.contains(layer.id) ? QStringLiteral("方案中已使用") : QStringLiteral("未关联");
        if (role == Qt::ToolTipRole) return path(m_layers, id) + QLatin1Char('\n') + diagnosticText(layer.diagnostics).join(QLatin1Char('\n'));
        return {};
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    { return orientation == Qt::Horizontal && role == Qt::DisplayRole ? QVariant(section ? QStringLiteral("状态") : QStringLiteral("PSD 图层")) : QVariant(); }
    QModelIndex Find(const QString& id) const
    { for (int index = 0; index < m_layers.size(); ++index) if (m_layers[index].id == id) return createIndex(m_rows[index], 0, quintptr(index + 1)); return {}; }
private:
    QVector<Artwork::Layer> m_layers;
    QMap<int, QVector<int>> m_children;
    QVector<int> m_rows;
    QSet<QString> m_used;
};
class CPsdImportLayerFilter : public QSortFilterProxyModel
{
public:
    explicit CPsdImportLayerFilter(QObject* parent) : QSortFilterProxyModel(parent) { setRecursiveFilteringEnabled(true); setFilterCaseSensitivity(Qt::CaseInsensitive); setFilterRole(Qt::UserRole + 1); }
    void SetMode(int mode) { m_mode = mode; invalidateFilter(); }
protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override
    {
        const auto index = sourceModel()->index(row, 0, parent);
        return QSortFilterProxyModel::filterAcceptsRow(row, parent) && (!m_mode || index.data(Qt::UserRole + (m_mode == 1 ? 2 : 3)).toBool());
    }
private:
    int m_mode = 0;
};
class CPsdImportMeshModel : public QAbstractListModel
{
public:
    explicit CPsdImportMeshModel(QObject* parent) : QAbstractListModel(parent) {}
    void SetMeshes(const QVector<CAtlasRepackPlanner::Mesh>& meshes) { beginResetModel(); m_meshes = meshes; endResetModel(); }
    void SetDecisions(const Planner::Request& request)
    { m_decisions.clear(); for (const auto& decision : request.decisions) m_decisions.insert(decision.meshId, decision); if (!m_meshes.isEmpty()) emit dataChanged(index(0), index(m_meshes.size() - 1)); }
    int rowCount(const QModelIndex& = {}) const override { return m_meshes.size(); }
    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() >= m_meshes.size()) return {};
        const auto& mesh = m_meshes[index.row()];
        if (role == Qt::UserRole) return mesh.material.id;
        if (role == Qt::DisplayRole) return mesh.material.id + QStringLiteral(" · ") + actionName(m_decisions.value(mesh.material.id).action) + (mesh.editable ? QString() : QStringLiteral(" · 锁定"));
        if (role == Qt::ToolTipRole) return mesh.material.id + (mesh.reason.isEmpty() ? QString() : QLatin1Char('\n') + mesh.reason);
        return {};
    }
    QModelIndex Find(const QString& id) const { for (int row = 0; row < m_meshes.size(); ++row) if (m_meshes[row].material.id == id) return index(row); return {}; }
private:
    QVector<CAtlasRepackPlanner::Mesh> m_meshes;
    QHash<QString, Planner::Decision> m_decisions;
};

struct CPsdImportDialog::ViewData
{
    QString meshId, layerId;
    QImage source, reference;
    QRect sourceBounds, atlasBounds;
    QVector<QPointF> points;
    QVector<quint16> triangles;
};
struct CPsdImportDialog::Job
{
    CExportControl control;
    Plan plan;
    Request request;
    ViewData view;
    QHash<QString, QString> suggested, reasons;
    QSet<QString> known;
    QString error;
    bool success = false, submit = false, alignmentConfirmed = true;
};

CPsdImportDialog::CPsdImportDialog(const Context& context, QWidget* parent) : QDialog(parent), m_context(context)
{
    setObjectName(QStringLiteral("psdImportDialog")); setWindowTitle(QStringLiteral("PSD 素材与模型关联")); setModal(true);
    m_request.incoming = context.incoming; m_request.familyName = context.familyName;
    if (m_request.familyName.trimmed().isEmpty()) m_request.familyName = context.incoming.revision.fileName;
    for (const auto& mesh : context.snapshot.meshes) m_request.decisions.append({mesh.material.id});
    for (const auto& id : context.selectedMeshIds) if (std::any_of(context.snapshot.meshes.cbegin(), context.snapshot.meshes.cend(), [&](const auto& mesh) { return mesh.material.id == id; })) m_selectedMeshes.insert(id);
    if (m_selectedMeshes.isEmpty() && !context.snapshot.meshes.isEmpty()) m_selectedMeshes.insert(context.snapshot.meshes.first().material.id);
    const auto& artwork = context.snapshot.baseline.artwork;
    m_reusesRevision = artwork.revisions.contains(context.incoming.revision.id);
    const auto family = artwork.families.value(context.incoming.revision.familyId);
    if (artwork.revisions.contains(family.currentRevision)) m_documentAlignmentConfirmed = artwork.revisions[family.currentRevision].canvasSize == context.incoming.revision.canvasSize;
    if (m_reusesRevision) m_documentAlignmentConfirmed = true;
    const int gap = std::max(fontMetrics().averageCharWidth(), style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing));
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(gap, gap, gap, gap); layout->setSpacing(gap);
    auto* heading = new QHBoxLayout; m_familyName = new QLineEdit(m_request.familyName, this); m_familyName->setObjectName(QStringLiteral("psdFamilyName")); m_familyName->setAccessibleName(QStringLiteral("素材来源名称"));
    heading->addWidget(new QLabel(QStringLiteral("素材来源"), this)); heading->addWidget(m_familyName, 1);
    const auto button = [&](const QString& text, const char* name, QBoxLayout* row) {
        auto* result = new QPushButton(text, this); result->setObjectName(QString::fromLatin1(name)); result->setAutoDefault(false); row->addWidget(result); return result;
    };
    m_undo = button(QStringLiteral("撤销"), "psdUndo", heading); m_redo = button(QStringLiteral("重做"), "psdRedo", heading); layout->addLayout(heading);
    m_info = new QLabel(this); m_info->setObjectName(QStringLiteral("psdSourceInfo")); m_info->setWordWrap(true); m_info->setTextFormat(Qt::PlainText);
    m_info->setText(QStringLiteral("%1 · %2 × %3 px · %4\n同名与同尺寸只作为建议。确认图层及来源像素对齐后，才会建立关联。")
        .arg(context.incoming.revision.fileName).arg(context.incoming.revision.canvasSize.width()).arg(context.incoming.revision.canvasSize.height())
        .arg(m_reusesRevision ? QStringLiteral("已保存来源，可继续管理关联") : family.id.isEmpty() ? QStringLiteral("首次导入") : QStringLiteral("来源新版本")));
    layout->addWidget(m_info);
    m_splitter = new QSplitter(Qt::Horizontal, this); m_splitter->setObjectName(QStringLiteral("psdImportSplitter")); m_splitter->setChildrenCollapsible(false);
    m_left = new QWidget(m_splitter); auto* left = new QVBoxLayout(m_left); left->setContentsMargins(0, 0, 0, 0);
    m_layerSearch = new QLineEdit(m_left); m_layerSearch->setObjectName(QStringLiteral("psdLayerSearch")); m_layerSearch->setPlaceholderText(QStringLiteral("搜索图层或层级路径")); left->addWidget(m_layerSearch);
    m_layerFilter = new QComboBox(m_left); m_layerFilter->setObjectName(QStringLiteral("psdLayerFilter")); m_layerFilter->addItems({QStringLiteral("全部图层"), QStringLiteral("方案中未关联"), QStringLiteral("包含诊断")}); left->addWidget(m_layerFilter);
    m_layers = new QTreeView(m_left); m_layers->setObjectName(QStringLiteral("psdLayers")); m_layers->setUniformRowHeights(true); m_layers->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_layerModel = new CPsdImportLayerModel(this); m_layerProxy = new CPsdImportLayerFilter(this); m_layerProxy->setSourceModel(m_layerModel); m_layers->setModel(m_layerProxy);
    m_layers->header()->setSectionResizeMode(0, QHeaderView::Stretch); m_layers->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents); left->addWidget(m_layers, 1);
    auto* center = m_center = new QWidget(m_splitter); auto* middle = new QVBoxLayout(center); middle->setContentsMargins(0, 0, 0, 0);
    auto* viewTools = new QHBoxLayout; auto* fitAll = button(QStringLiteral("适应文档"), "psdFitAll", viewTools); auto* fitSource = button(QStringLiteral("适应图层"), "psdFitLayer", viewTools); auto* fitReference = button(QStringLiteral("适应参照"), "psdFitReference", viewTools); middle->addLayout(viewTools);
    m_source = new CArtworkSourceView(center); m_source->setMinimumHeight(fontMetrics().height() * 8); middle->addWidget(m_source, 1);
    m_coordinates = new QLabel(QStringLiteral("PSD 文档像素；参照来自当前图集，不使用模型姿态包围盒。"), center); m_coordinates->setObjectName(QStringLiteral("psdCoordinates")); m_coordinates->setTextFormat(Qt::PlainText); m_coordinates->setWordWrap(true); middle->addWidget(m_coordinates);
    auto* opacity = new QHBoxLayout;
    const auto slider = [&](const QString& label, const char* name, int value) { auto* s = new QSlider(Qt::Horizontal, center); s->setObjectName(QString::fromLatin1(name)); s->setAccessibleName(label); s->setRange(0, 100); s->setValue(value); opacity->addWidget(new QLabel(label, center)); opacity->addWidget(s, 1); return s; };
    m_sourceOpacity = slider(QStringLiteral("源图层预览"), "psdSourceOpacity", 100); m_referenceOpacity = slider(QStringLiteral("图集参照预览"), "psdReferenceOpacity", 50); middle->addLayout(opacity);
    auto* scroll = m_inspector = new QScrollArea(m_splitter); scroll->setObjectName(QStringLiteral("psdControlsScroll")); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame); scroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    m_right = new QWidget(scroll); auto* right = new QVBoxLayout(m_right); right->setSpacing(gap);
    m_meshSearch = new QLineEdit(m_right); m_meshSearch->setObjectName(QStringLiteral("psdMeshSearch")); m_meshSearch->setPlaceholderText(QStringLiteral("搜索模型对象 ID")); right->addWidget(m_meshSearch);
    m_meshes = new QListView(m_right); m_meshes->setObjectName(QStringLiteral("psdMeshes")); m_meshes->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_meshModel = new CPsdImportMeshModel(this); m_meshProxy = new QSortFilterProxyModel(this); m_meshProxy->setSourceModel(m_meshModel); m_meshProxy->setFilterRole(Qt::UserRole); m_meshProxy->setFilterCaseSensitivity(Qt::CaseInsensitive); m_meshes->setModel(m_meshProxy);
    m_meshes->setMinimumHeight(fontMetrics().height() * 6); right->addWidget(m_meshes);
    m_selectionInfo = new QLabel(m_right); m_selectionInfo->setObjectName(QStringLiteral("psdSelectionInfo")); m_selectionInfo->setWordWrap(true); m_selectionInfo->setTextFormat(Qt::PlainText); right->addWidget(m_selectionInfo);
    auto* decisions = new QFormLayout; decisions->setRowWrapPolicy(QFormLayout::WrapLongRows); decisions->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_action = new QComboBox(m_right); m_action->setObjectName(QStringLiteral("psdAction")); for (const auto action : {Planner::Action::Keep, Planner::Action::Associate, Planner::Action::Update, Planner::Action::Detach}) m_action->addItem(actionName(action), int(action)); decisions->addRow(QStringLiteral("当前选区"), m_action);
    m_layerOpacity = new QCheckBox(QStringLiteral("将 PSD 图层透明度应用到素材像素"), m_right); m_layerOpacity->setObjectName(QStringLiteral("psdApplyLayerOpacity")); m_layerOpacity->setChecked(true); m_layerOpacity->setToolTip(QStringLiteral("将 PSD 图层透明度乘入素材 PNG 像素；不替代模型现有透明度。")); decisions->addRow(m_layerOpacity);
    m_confirm = new QCheckBox(QStringLiteral("确认所选图层与像素对齐"), m_right); m_confirm->setObjectName(QStringLiteral("psdConfirmAssociation")); decisions->addRow(m_confirm); right->addLayout(decisions);
    m_alignmentInfo = new QLabel(m_right); m_alignmentInfo->setObjectName(QStringLiteral("psdAlignmentInfo")); m_alignmentInfo->setWordWrap(true); m_alignmentInfo->setTextFormat(Qt::PlainText); right->addWidget(m_alignmentInfo);
    auto* transforms = new QFormLayout; transforms->setRowWrapPolicy(QFormLayout::WrapLongRows); transforms->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto spin = [&](QFormLayout* form, const QString& label, const char* name, double value, const QString& suffix = {}) {
        auto* input = new QDoubleSpinBox(m_right); input->setObjectName(QString::fromLatin1(name)); input->setAccessibleName(label); input->setRange(-1e12, 1e12); input->setDecimals(6); input->setKeyboardTracking(false); input->setValue(value); input->setSuffix(suffix); input->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); form->addRow(label, input); return input;
    };
    m_x = spin(transforms, QStringLiteral("调整 X"), "psdTranslateX", 0, QStringLiteral(" px")); m_y = spin(transforms, QStringLiteral("调整 Y"), "psdTranslateY", 0, QStringLiteral(" px"));
    m_angle = spin(transforms, QStringLiteral("顺时针旋转"), "psdRotation", 0, QStringLiteral("°")); m_sx = spin(transforms, QStringLiteral("水平缩放"), "psdScaleX", 100, QStringLiteral("%")); m_sy = spin(transforms, QStringLiteral("垂直缩放"), "psdScaleY", 100, QStringLiteral("%")); right->addLayout(transforms);
    auto* transform = button(QStringLiteral("调整参照映射"), "psdApplyTransform", right); auto* propose = button(QStringLiteral("按图层区域提出对齐"), "psdProposeAlignment", right);
    m_applySelection = button(QStringLiteral("应用选区决定并预览"), "psdApplySelection", right); m_confirmKnown = button(QStringLiteral("预览已确认 ID 的更新建议"), "psdConfirmKnown", right);
    auto* registration = new QFormLayout; registration->setRowWrapPolicy(QFormLayout::WrapLongRows); registration->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_documentInfo = new QLabel(m_right); m_documentInfo->setObjectName(QStringLiteral("psdDocumentAlignmentInfo")); m_documentInfo->setWordWrap(true); m_documentInfo->setTextFormat(Qt::PlainText); right->addWidget(m_documentInfo);
    m_documentAlignment = new QComboBox(m_right); m_documentAlignment->setObjectName(QStringLiteral("psdDocumentAlignment")); m_documentAlignment->addItems({QStringLiteral("保持文档左上角"), QStringLiteral("对齐旧画布中心"), QStringLiteral("按旧画布尺寸缩放"), QStringLiteral("在当前文档对齐上平移")}); m_documentAlignment->setCurrentIndex(3); registration->addRow(QStringLiteral("文档对齐"), m_documentAlignment);
    m_documentX = spin(registration, QStringLiteral("文档偏移 X"), "psdDocumentX", 0, QStringLiteral(" px")); m_documentY = spin(registration, QStringLiteral("文档偏移 Y"), "psdDocumentY", 0, QStringLiteral(" px")); right->addLayout(registration);
    m_colorPolicy = new QComboBox(m_right); m_colorPolicy->setObjectName(QStringLiteral("psdColorPolicy"));
    m_colorPolicy->addItem(QStringLiteral("转换到 sRGB"), int(Artwork::ColorPolicy::ConvertToSRgb));
    m_colorPolicy->addItem(QStringLiteral("保留原始颜色数值"), int(Artwork::ColorPolicy::PreserveValues));
    m_colorPolicy->setCurrentIndex(m_colorPolicy->findData(int(context.incoming.revision.colorPolicy)));
    m_colorPolicy->setEnabled(context.incoming.kind == Artwork::SourceKind::Psd); registration->addRow(QStringLiteral("PSD 颜色"), m_colorPolicy);
    auto* alignDocument = button(QStringLiteral("应用来源设置并预览"), "psdApplyDocumentAlignment", right);
    m_impacts = new QPlainTextEdit(m_right); m_impacts->setObjectName(QStringLiteral("psdImpacts")); m_impacts->setReadOnly(true); m_impacts->setLineWrapMode(QPlainTextEdit::WidgetWidth); m_impacts->setFocusPolicy(Qt::StrongFocus); m_impacts->setMinimumHeight(fontMetrics().height() * 7); right->addWidget(m_impacts); right->addStretch(); scroll->setWidget(m_right);
    m_splitter->addWidget(m_left); m_splitter->addWidget(center); m_splitter->addWidget(scroll); m_splitter->setStretchFactor(0, 0); m_splitter->setStretchFactor(1, 1); m_splitter->setStretchFactor(2, 0); layout->addWidget(m_splitter, 1);
    m_compact = new QTabWidget(this); m_compact->setObjectName(QStringLiteral("psdCompactWorkspace")); m_compact->hide(); layout->addWidget(m_compact, 1);
    m_status = new QLabel(this); m_status->setObjectName(QStringLiteral("psdImportStatus")); m_status->setTextFormat(Qt::PlainText); m_status->setWordWrap(true); layout->addWidget(m_status);
    m_error = new QLabel(this); m_error->setObjectName(QStringLiteral("psdImportError")); m_error->setWordWrap(true); m_error->setTextFormat(Qt::PlainText); m_error->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(m_error);
    auto* tools = new QHBoxLayout; m_previewButton = button(QStringLiteral("预览当前方案"), "psdPreview", tools); m_discard = button(QStringLiteral("放弃未应用输入"), "psdDiscardInput", tools); m_stop = button(QStringLiteral("停止计算"), "psdStop", tools); layout->addLayout(tools);
    auto* footer = new QHBoxLayout; m_sourceOnly = button(QStringLiteral("仅保存来源素材"), "psdSaveSourceOnly", footer); m_sourceOnly->setToolTip(QStringLiteral("保留模型现有外观，只保存来源与未关联图层，不创建新 ArtMesh"));
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this); m_buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存素材更新")); m_buttons->button(QDialogButtonBox::Save)->setObjectName(QStringLiteral("primaryBtn")); m_buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消")); footer->addWidget(m_buttons, 1); layout->addLayout(footer);
    connect(m_layers->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { SelectLayer(); }); connect(m_meshes->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] { SelectMeshes(); });
    connect(m_layerSearch, &QLineEdit::textChanged, m_layerProxy, &QSortFilterProxyModel::setFilterFixedString); connect(m_meshSearch, &QLineEdit::textChanged, m_meshProxy, &QSortFilterProxyModel::setFilterFixedString);
    connect(m_layerFilter, &QComboBox::currentIndexChanged, this, [this](int index) { static_cast<CPsdImportLayerFilter*>(m_layerProxy)->SetMode(index); });
    connect(m_familyName, &QLineEdit::textChanged, this, [this](const QString& name) { if (!m_refreshing) { m_request.familyName = name; m_requestDirty = true; Refresh(); } });
    const auto dirty = [this] { if (!m_refreshing) { m_inputDirty = true; Refresh(); } };
    connect(m_action, &QComboBox::currentIndexChanged, this, dirty); connect(m_confirm, &QCheckBox::toggled, this, dirty);
    connect(m_layerOpacity, &QCheckBox::toggled, this, [this, dirty] { dirty(); RefreshViewOpacity(); });
    for (auto* input : {m_x, m_y, m_angle, m_sx, m_sy}) connect(input, &QDoubleSpinBox::valueChanged, this, [this] { if (!m_refreshing) { m_numericDirty = true; Refresh(); } });
    const auto recipeDirty = [this] { if (!m_refreshing) { m_recipeDirty = true; Refresh(); } };
    connect(m_documentAlignment, &QComboBox::currentIndexChanged, this, recipeDirty);
    connect(m_colorPolicy, &QComboBox::currentIndexChanged, this, recipeDirty);
    connect(m_documentX, &QDoubleSpinBox::valueChanged, this, recipeDirty); connect(m_documentY, &QDoubleSpinBox::valueChanged, this, recipeDirty);
    connect(m_sourceOpacity, &QSlider::valueChanged, this, [this] { RefreshViewOpacity(); }); connect(m_referenceOpacity, &QSlider::valueChanged, this, [this] { RefreshViewOpacity(); });
    connect(fitAll, &QPushButton::clicked, m_source, &CArtworkSourceView::FitAll); connect(fitSource, &QPushButton::clicked, m_source, &CArtworkSourceView::FitSource); connect(fitReference, &QPushButton::clicked, m_source, &CArtworkSourceView::FitReference);
    connect(transform, &QPushButton::clicked, this, &CPsdImportDialog::ApplyAlignment); connect(propose, &QPushButton::clicked, this, &CPsdImportDialog::ProposeAlignment);
    connect(m_applySelection, &QPushButton::clicked, this, &CPsdImportDialog::ApplySelection); connect(m_confirmKnown, &QPushButton::clicked, this, &CPsdImportDialog::ConfirmKnownMatches); connect(alignDocument, &QPushButton::clicked, this, &CPsdImportDialog::ApplyDocumentAlignment);
    connect(m_previewButton, &QPushButton::clicked, this, &CPsdImportDialog::PreviewRequest); connect(m_sourceOnly, &QPushButton::clicked, this, &CPsdImportDialog::SaveSourceOnly);
    connect(m_discard, &QPushButton::clicked, this, &CPsdImportDialog::DiscardInput); connect(m_undo, &QPushButton::clicked, this, &CPsdImportDialog::Undo); connect(m_redo, &QPushButton::clicked, this, &CPsdImportDialog::Redo); connect(m_stop, &QPushButton::clicked, this, &CPsdImportDialog::StopCalculation);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &CPsdImportDialog::accept); connect(m_buttons, &QDialogButtonBox::rejected, this, &CPsdImportDialog::reject);
    m_source->SetPreviewHandler([](const auto&, QString*) { return true; });
    m_source->SetCommitHandler([this](const auto& mapping, QString*) { m_alignment = mapping; MarkMappingEdited(); return true; });
    m_source->SetCancellationHandler([](QString*) { return true; });
    connect(m_source, &CArtworkSourceView::GestureFinished, this, [this] { Refresh(); }); connect(m_source, &CArtworkSourceView::GestureStarted, this, [this] { Refresh(); });
    connect(m_source, &CArtworkSourceView::EditRejected, this, &CPsdImportDialog::ShowError);
    connect(m_source, &CArtworkSourceView::PointerMoved, this, [this](QPointF document, QPointF local, QPointF atlas, bool) {
        m_coordinates->setText(QStringLiteral("PSD 文档：%1, %2 px    图层局部：%3, %4 px    图集：%5, %6 px")
            .arg(document.x(), 0, 'f', 2).arg(document.y(), 0, 'f', 2).arg(local.x(), 0, 'f', 2).arg(local.y(), 0, 'f', 2).arg(atlas.x(), 0, 'f', 2).arg(atlas.y(), 0, 'f', 2));
    });
    auto* undo = new QShortcut(QKeySequence::Undo, this); connect(undo, &QShortcut::activated, this, &CPsdImportDialog::Undo); auto* redo = new QShortcut(QKeySequence::Redo, this); connect(redo, &QShortcut::activated, this, &CPsdImportDialog::Redo);
    resize(fontMetrics().averageCharWidth() * 180, fontMetrics().height() * 46);
    m_splitter->setSizes({fontMetrics().averageCharWidth() * 35, fontMetrics().averageCharWidth() * 90, fontMetrics().averageCharWidth() * 52});
    Initialize();
}

CPsdImportDialog::~CPsdImportDialog() { ++m_generation; if (m_job) m_job->control.Cancel(); }
QString CPsdImportDialog::Error() const { return m_error->text(); }
void CPsdImportDialog::SetPreviewHandler(Handler handler) { if (!m_busy) m_preview = std::move(handler); }
void CPsdImportDialog::SetSubmissionHandler(Handler handler) { if (!m_busy) m_submit = std::move(handler); }
void CPsdImportDialog::SetCancellationHandler(CancellationHandler handler) { if (!m_busy) m_cancel = std::move(handler); }
void CPsdImportDialog::ShowError(const QString& error) { m_error->setText(error); m_error->setVisible(!error.isEmpty()); }
bool CPsdImportDialog::HasPendingInput() const { return m_inputDirty || m_numericDirty || m_recipeDirty || m_source->HasGesture(); }
QStringList CPsdImportDialog::SelectedMeshes() const { auto ids = m_selectedMeshes.values(); std::sort(ids.begin(), ids.end()); return ids; }
int CPsdImportDialog::LayerIndex(const QString& id) const { const auto& layers = m_request.incoming.revision.layers; for (int index = 0; index < layers.size(); ++index) if (layers[index].id == id) return index; return -1; }
Planner::Decision CPsdImportDialog::DecisionFor(const QString& id) const { for (const auto& decision : m_request.decisions) if (decision.meshId == id) return decision; return {id}; }
QTransform CPsdImportDialog::InitialMapping(const QString& id) const
{
    const auto decision = DecisionFor(id); if (decision.action == Planner::Action::Associate) return decision.atlasToDocument;
    const auto& artwork = m_context.snapshot.baseline.artwork; const auto binding = artwork.bindings.constFind(id);
    if (binding != artwork.bindings.cend()) {
        const auto placement = artwork.placements.constFind(binding->placementId);
        if (placement != artwork.placements.cend()) return inverted(placement->familyToAtlas) * inverted(m_request.incoming.revision.documentToFamily);
    }
    return {};
}

void CPsdImportDialog::Initialize()
{
    auto job = std::make_shared<Job>(); m_job = job; const auto generation = ++m_generation; const auto context = m_context; job->request = m_request;
    auto* worker = QThread::create([job, context] {
        try {
            if (!Planner::Build(context.snapshot, sourceOnly(job->request), job->plan, &job->error, &job->control)) return;
            const auto baselineSlots = context.snapshot.baseline.workspace.atlases.keys();
            if (!context.atlasImages.isEmpty() && context.atlasImages.keys() != baselineSlots) { job->error = QStringLiteral("冻结图集目录与文档不一致"); return; }
            for (const auto slot : baselineSlots) {
                if (!job->control.Checkpoint()) return;
                const auto image = job->plan.images.constFind(slot);
                if (image == job->plan.images.cend() || image->isNull()) { job->error = QStringLiteral("规划缺少当前图集参照"); return; }
                if (!context.atlasImages.isEmpty() && context.atlasImages.value(slot).convertToFormat(QImage::Format_RGBA8888) != image->convertToFormat(QImage::Format_RGBA8888)) {
                    job->error = QStringLiteral("冻结图集像素与文档不一致"); return;
                }
            }
            const auto& artwork = context.snapshot.baseline.artwork; QMap<QString, CPsdLayerMatcher::Result> matches;
            for (const auto& decision : job->request.decisions) {
                if (!job->control.Checkpoint()) { job->error = job->control.Error(); return; }
                const auto binding = artwork.bindings.constFind(decision.meshId);
                if (binding != artwork.bindings.cend()) {
                    const auto placement = artwork.placements.constFind(binding->placementId);
                    if (placement != artwork.placements.cend()) {
                        const auto material = artwork.materials.constFind(placement->materialId);
                        if (material != artwork.materials.cend() && material->familyId == context.incoming.revision.familyId) {
                            if (!matches.contains(material->revisionId)) {
                                CPsdLayerMatcher::Result result;
                                if (!CPsdLayerMatcher::Compare(artwork.revisions.value(material->revisionId), context.incoming.revision, result, &job->error, &job->control)) return;
                                matches.insert(material->revisionId, result);
                            }
                            for (const auto& match : matches[material->revisionId].matches) if (match.oldLayerId == material->layerId) {
                                job->reasons[decision.meshId] = diagnosticText(match.reasons).join(QLatin1Char('\n'));
                                if (binding->status == Artwork::BindingStatus::Detached) job->reasons[decision.meshId] += QStringLiteral("\n当前素材已与来源分离，请人工重新关联。");
                                else if (match.status == CPsdLayerMatcher::Status::Matched) { job->known.insert(decision.meshId); job->suggested[decision.meshId] = match.matchedLayerId; }
                                else if (match.status == CPsdLayerMatcher::Status::Missing) job->reasons[decision.meshId] += QStringLiteral("\n新版本缺少该层，保留旧素材。");
                                else if (match.candidates.size() == 1) job->suggested[decision.meshId] = match.candidates.first().layerId;
                            }
                        }
                    }
                }
                if (!job->suggested.contains(decision.meshId)) {
                    QStringList names; for (const auto& layer : context.incoming.revision.layers) if (supported(layer) && layer.name == decision.meshId) names.append(layer.id);
                    if (names.size() == 1) { job->suggested[decision.meshId] = names.first(); job->reasons[decision.meshId] += QStringLiteral("\n名称相同，仅为候选，尚未确认关联或空间映射。"); }
                    else if (names.size() > 1) job->reasons[decision.meshId] += QStringLiteral("\n存在多个同名图层，请按完整路径选择。");
                }
            }
            job->success = job->control.Checkpoint();
        } catch (...) { job->error = QStringLiteral("无法准备 PSD 关联，当前模型保持不变"); }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, job, generation] {
        if (m_generation != generation || m_job != job) return; m_job.reset();
        if (m_closeWhenIdle) { m_closeWhenIdle = false; reject(); return; }
        if (!job->success || job->control.IsCancellationRequested()) { m_valid = false; ShowError(job->error.isEmpty() ? QStringLiteral("读取已停止") : job->error); Refresh(); return; }
        m_context.atlasImages.clear();
        for (const auto slot : m_context.snapshot.baseline.workspace.atlases.keys()) m_context.atlasImages.insert(slot, job->plan.images.value(slot));
        m_plan = job->plan; m_acceptedRequest = sourceOnly(m_request); m_history = {{m_acceptedRequest, m_plan, m_documentAlignmentConfirmed}};
        m_suggestedLayers = job->suggested; m_matchReasons = job->reasons; m_knownMatches = job->known;
        m_layerModel->SetLayers(m_request.incoming.revision.layers); m_meshModel->SetMeshes(m_context.snapshot.meshes); m_meshModel->SetDecisions(m_request);
        m_ready = true;
        const QSignalBlocker rows(m_meshes->selectionModel()); for (const auto& id : m_selectedMeshes) m_meshes->selectionModel()->select(m_meshProxy->mapFromSource(m_meshModel->Find(id)), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        if (!m_selectedMeshes.isEmpty()) m_layerId = m_suggestedLayers.value(SelectedMeshes().first());
        if (m_layerId.isEmpty()) for (const auto& layer : m_request.incoming.revision.layers) if (supported(layer)) { m_layerId = layer.id; break; }
        { const QSignalBlocker selected(m_layers->selectionModel()); const auto index = m_layerProxy->mapFromSource(m_layerModel->Find(m_layerId)); m_layers->setCurrentIndex(index); m_layers->scrollTo(index); }
        RefreshDecision(); Refresh(); const QPointer<CPsdImportDialog> alive(this); LoadView(); if (alive) emit Ready();
    }, Qt::QueuedConnection);
    Refresh(); worker->start();
}

void CPsdImportDialog::Build(Request request, bool submit)
{
    if (IsBusy() || !m_ready || !m_valid) return;
    auto job = std::make_shared<Job>(); job->request = std::move(request); job->submit = submit; job->alignmentConfirmed = m_documentAlignmentConfirmed; m_job = job;
    const auto generation = ++m_generation; const auto snapshot = m_context.snapshot; const auto original = m_context.incoming;
    auto* worker = QThread::create([job, snapshot, original] { try {
        if (!prepareRecipe(original, job->request.incoming, &job->error, &job->control)) return;
        job->success = Planner::Build(snapshot, job->request, job->plan, &job->error, &job->control); } catch (...) { job->error = QStringLiteral("素材规划失败，已接受预览保持不变"); } });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, job, generation] {
        if (m_generation != generation || m_job != job) return; m_job.reset();
        if (m_closeWhenIdle) { m_closeWhenIdle = false; reject(); return; }
        if (!job->success || job->control.IsCancellationRequested()) { ShowError(job->control.IsCancellationRequested() ? QStringLiteral("计算已停止，预览保持不变") : job->error); Refresh(); return; }
        const QPointer<CPsdImportDialog> alive(this);
        if (job->submit) { m_documentAlignmentConfirmed = job->alignmentConfirmed; Submit(job->plan); return; }
        if (Publish(job->plan, job->request, true) && alive) { m_inputDirty = m_numericDirty = m_requestDirty = m_recipeDirty = false; RestoreInputs(); RefreshDecision(); Refresh(); LoadView(); }
    }, Qt::QueuedConnection);
    ShowError({}); Refresh(); worker->start();
}

void CPsdImportDialog::LoadView()
{
    if (IsBusy() || !m_ready) return;
    m_viewData.reset();
    auto job = std::make_shared<Job>(); m_job = job; const auto generation = ++m_generation;
    const auto context = m_context; const auto incoming = m_request.incoming; const auto layerId = m_layerId;
    const auto meshIds = SelectedMeshes(); const auto meshId = meshIds.isEmpty() ? QString() : meshIds.first();
    auto* worker = QThread::create([job, context, incoming, layerId, meshId] {
        try {
            job->view.meshId = meshId; job->view.layerId = layerId;
            for (const auto& layer : incoming.revision.layers) if (layer.id == layerId && supported(layer)) {
                job->view.sourceBounds = layer.bounds; if (layer.imageDigest.isEmpty()) break;
                const auto image = incoming.images.constFind(layer.imageDigest);
                if (image == incoming.images.cend() || !CPngImage::Decode(image->png, job->view.source, &job->error)) return;
                job->view.sourceBounds = layer.bounds;
            }
            for (const auto& mesh : context.snapshot.meshes) if (mesh.material.id == meshId && !mesh.material.uvs.isEmpty()) {
                const auto atlas = context.atlasImages.constFind(mesh.material.textureIndex);
                if (atlas == context.atlasImages.cend() || atlas->isNull() || atlas->size() != context.snapshot.baseline.workspace.atlases.value(mesh.material.textureIndex).size) { job->error = QStringLiteral("当前图集参照缺失或尺寸不一致"); return; }
                double left = std::numeric_limits<double>::infinity(), top = left, right = -left, bottom = -left;
                for (int index = 0; index < mesh.material.uvs.size(); index += 2) {
                    const QPointF point(double(mesh.material.uvs[index]) * atlas->width(), double(mesh.material.uvs[index + 1]) * atlas->height());
                    if (!std::isfinite(point.x()) || !std::isfinite(point.y())) { job->error = QStringLiteral("图集参照 UV 无效"); return; }
                    left = std::min(left, point.x()); top = std::min(top, point.y()); right = std::max(right, point.x()); bottom = std::max(bottom, point.y()); job->view.points.append(point);
                }
                left = std::floor(left) - 1; top = std::floor(top) - 1; right = std::ceil(right) + 1; bottom = std::ceil(bottom) + 1;
                const double limit = std::numeric_limits<int>::max();
                if (left < std::numeric_limits<int>::min() || top < std::numeric_limits<int>::min() || right > limit || bottom > limit || right - left > limit || bottom - top > limit || right <= left || bottom <= top ||
                    (right - left) * (bottom - top) > context.snapshot.limits.maximumPixels || (right - left) * (bottom - top) > context.snapshot.limits.maximumOperations ||
                    (right - left) * (bottom - top) * 4 > CTextureWorkspace::MaximumDecodedBytes - job->view.source.sizeInBytes()) { job->error = QStringLiteral("图集参照区域超出像素或计算预算，请先调整 UV"); return; }
                job->view.atlasBounds = QRect(int(left), int(top), int(right - left), int(bottom - top));
                qint64 retainedBytes = job->view.source.sizeInBytes(), retainedPixels = qint64(job->view.source.width()) * job->view.source.height();
                QSet<qint64> images;
                for (const auto& frozen : context.atlasImages) if (!images.contains(frozen.cacheKey())) {
                    images.insert(frozen.cacheKey()); retainedBytes += frozen.sizeInBytes(); retainedPixels += qint64(frozen.width()) * frozen.height();
                }
                const qint64 cropPixels = qint64(job->view.atlasBounds.width()) * job->view.atlasBounds.height();
                if (retainedBytes > CTextureWorkspace::MaximumDecodedBytes - cropPixels * 4 || retainedPixels > context.snapshot.limits.maximumPixels - cropPixels) {
                    job->error = QStringLiteral("来源画布与图集参照总像素超出内存预算"); return;
                }
                QImage source = atlas->convertToFormat(QImage::Format_RGBA8888); QImage crop(job->view.atlasBounds.size(), QImage::Format_RGBA8888);
                if (crop.isNull()) { job->error = QStringLiteral("无法分配图集参照"); return; }
                const auto repeat = [](qint64 value, int size) { const auto remainder = value % size; return int(remainder < 0 ? remainder + size : remainder); };
                for (int y = 0; y < crop.height(); ++y) {
                    if (!job->control.Checkpoint()) { job->error = job->control.Error(); return; }
                    const auto* row = source.constScanLine(repeat(qint64(job->view.atlasBounds.y()) + y, source.height())); auto* target = crop.scanLine(y);
                    for (int x = 0; x < crop.width(); ++x) std::memcpy(target + x * 4, row + repeat(qint64(job->view.atlasBounds.x()) + x, source.width()) * 4, 4);
                }
                job->view.reference = std::move(crop); job->view.triangles = mesh.triangles;
            }
            job->success = job->control.Checkpoint();
        } catch (...) { job->error = QStringLiteral("无法准备来源画布，旧预览保持不变"); }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, job, generation] {
        if (generation != m_generation || m_job != job) return; m_job.reset();
        if (m_closeWhenIdle) { m_closeWhenIdle = false; reject(); return; }
        if (!job->success || job->control.IsCancellationRequested()) { ShowError(job->error.isEmpty() ? QStringLiteral("参照读取已停止") : job->error); Refresh(); return; }
        QString error; auto view = std::make_unique<ViewData>(job->view);
        const CArtworkSourceView::Content content{view->source, view->reference, view->sourceBounds, view->atlasBounds, m_request.incoming.revision.canvasSize, m_alignment, view->points, view->triangles};
        if (!m_source->SetContent(content, &error)) { ShowError(error); Refresh(); return; }
        m_viewData = std::move(view); RefreshViewOpacity(); Refresh();
    }, Qt::QueuedConnection);
    Refresh(); worker->start();
}

bool CPsdImportDialog::Publish(Plan plan, Request request, bool remember)
{
    if (IsBusy() || !m_ready) return false;
    QString error; if (!Planner::Validate(m_context.snapshot, plan, &error)) { ShowError(error); return false; }
    if (!m_preview) { ShowError(QStringLiteral("素材预览尚未连接")); return false; }
    const QPointer<CPsdImportDialog> alive(this);
    try {
        auto history = m_history;
        if (remember) { history.resize(m_historyIndex + 1); history.append({request, plan, m_documentAlignmentConfirmed}); }
        if (history.size() > 128) { ShowError(QStringLiteral("草稿历史达到上限，请先保存当前素材更新")); return false; }
        qint64 bytes = 0, pixels = 0; QSet<quintptr> arrays; QSet<qint64> images;
        const auto add = [&](const QByteArray& value) {
            const auto address = reinterpret_cast<quintptr>(value.constData()); if (arrays.contains(address)) return true; arrays.insert(address);
            if (value.size() > CTextureWorkspace::MaximumDecodedBytes - bytes) return false; bytes += value.size(); return true;
        };
        for (const auto& entry : history) {
            bool fits = true;
            for (const auto& image : entry.plan.images) if (!images.contains(image.cacheKey())) {
                images.insert(image.cacheKey()); const qint64 area = qint64(image.width()) * image.height();
                if (area > m_context.snapshot.limits.maximumPixels - pixels || image.sizeInBytes() > CTextureWorkspace::MaximumDecodedBytes - bytes) fits = false;
                else { pixels += area; bytes += image.sizeInBytes(); }
            }
            for (const auto& atlas : entry.plan.state.workspace.atlases) fits = add(atlas.png) && fits;
            for (const auto& image : entry.plan.state.artwork.images) fits = add(image.png) && fits;
            for (const auto& revision : entry.plan.state.artwork.revisions) fits = add(revision.sourceBytes) && add(revision.iccProfile) && fits;
            if (!fits) { ShowError(QStringLiteral("素材草稿历史达到内存预算，请先保存当前修改")); return false; }
        }
        const auto handler = m_preview; m_busy = true; bool accepted = false;
        try { accepted = handler(plan, &error); } catch (...) { error = QStringLiteral("素材预览失败，原有预览保持不变"); }
        if (!alive) return false; m_busy = false;
        if (!accepted) { ShowError(error.isEmpty() ? QStringLiteral("素材预览未能发布") : error); Refresh(); return false; }
        m_plan = std::move(plan); m_request = m_acceptedRequest = std::move(request); m_published = true;
        if (remember) { m_history.swap(history); m_historyIndex = m_history.size() - 1; }
        m_requestDirty = false; ShowError({}); Refresh(); return true;
    } catch (...) { if (alive) { m_busy = false; ShowError(QStringLiteral("无法保留素材草稿历史")); } return false; }
}

void CPsdImportDialog::Refresh()
{
    if (m_refreshing) return; m_refreshing = true; const QPointer<CPsdImportDialog> alive(this); const auto reset = qScopeGuard([alive] { if (alive) alive->m_refreshing = false; });
    const bool available = m_ready && m_valid && !IsBusy(); const bool gesture = m_source->HasGesture();
    m_left->setEnabled(available && !gesture); m_right->setEnabled(available && !gesture); m_familyName->setEnabled(available && !gesture);
    QString ignored; m_source->SetEditingEnabled(available && bool(m_viewData) && !m_numericDirty && !m_mixedMapping && !m_recipeDirty, &ignored); if (!alive) return;
    m_undo->setEnabled(available && !HasPendingInput() && m_historyIndex > 0); m_redo->setEnabled(available && !HasPendingInput() && m_historyIndex + 1 < m_history.size());
    m_stop->setEnabled(bool(m_job)); m_discard->setEnabled(available && (HasPendingInput() || m_requestDirty));
    m_previewButton->setEnabled(available && !HasPendingInput()); m_sourceOnly->setEnabled(available && !HasPendingInput());
    m_buttons->button(QDialogButtonBox::Save)->setEnabled(available && !HasPendingInput() && !m_requestDirty);
    m_buttons->button(QDialogButtonBox::Cancel)->setEnabled(!m_busy);
    m_applySelection->setEnabled(available && !m_selectedMeshes.isEmpty() && !m_numericDirty && !m_recipeDirty);
    m_confirmKnown->setEnabled(available && !HasPendingInput() && !m_knownMatches.isEmpty());
    m_documentInfo->setText(m_context.snapshot.baseline.artwork.revisions.contains(m_request.incoming.revision.id) ? QStringLiteral("管理已保存来源；修改配准或颜色将创建新修订，旧素材只有选择更新才改变。") : m_documentAlignmentConfirmed
        ? QStringLiteral("来源修订已准备；文档对齐只定义来源坐标，保留原样的对象仍使用旧版素材。") : QStringLiteral("PSD 画布尺寸发生变化。请先确认文档对齐，不能按旧模型包围盒推断。"));
    if (!m_ready) { m_status->setText(m_job ? QStringLiteral("正在准备 PSD 来源与关联建议…") : QStringLiteral("来源尚未就绪")); return; }
    m_meshModel->SetDecisions(m_request);
    QSet<QString> used; for (const auto& decision : m_request.decisions)
        if (decision.action == Planner::Action::Associate || decision.action == Planner::Action::Update) used.insert(decision.layerId);
    { const QSignalBlocker selection(m_layers->selectionModel()); m_layerModel->SetUsed(used); }
    m_alignmentInfo->setText(m_mixedMapping ? QStringLiteral("选区包含不同的映射；当前显示第一个对象。关联或配准请逐对象处理，保持每个对象的独立映射。") :
        QStringLiteral("当前图集像素 → PSD 文档像素\n[%1  %2  %3; %4  %5  %6]\n数值调整作用于当前映射，可用负缩放反射。")
            .arg(m_alignment.m11(), 0, 'g', 6).arg(m_alignment.m21(), 0, 'g', 6).arg(m_alignment.dx(), 0, 'g', 6).arg(m_alignment.m12(), 0, 'g', 6).arg(m_alignment.m22(), 0, 'g', 6).arg(m_alignment.dy(), 0, 'g', 6));
    if (m_displayedImpactToken != m_plan.token) {
        QStringList impacts{colorPolicyText(m_request.incoming)};
        for (const auto& message : m_request.incoming.diagnostics) if (!colorDiagnostic(message)) impacts.append(diagnosticText(message));
        for (const auto& impact : m_plan.impacts) impacts.append(impact.meshId + QStringLiteral("：") + actionName(impact.action) + (impact.notes.isEmpty() ? QString() : QLatin1Char('\n') + impact.notes.join(QLatin1Char('\n'))));
        for (const auto& message : m_plan.warnings) if (!colorDiagnostic(message)) impacts.append(diagnosticText(message));
        impacts.removeDuplicates(); m_impacts->setPlainText(impacts.join(QStringLiteral("\n\n"))); m_displayedImpactToken = m_plan.token;
    }
    m_status->setText(m_job ? QStringLiteral("正在计算，可停止；当前模型保留最近一次成功预览。") : QStringLiteral("%1 个图层 · %2 个待确认对象%3\n未关联图层作为来源保存，不创建新 ArtMesh。")
        .arg(m_request.incoming.revision.layers.size()).arg(m_plan.unconfirmed.size()).arg(m_requestDirty || HasPendingInput() ? QStringLiteral(" · 有未应用修改") : QString()));
}

void CPsdImportDialog::RefreshDecision()
{
    const auto ids = SelectedMeshes(); const auto first = ids.isEmpty() ? Planner::Decision{} : DecisionFor(ids.first());
    m_alignment = ids.isEmpty() ? QTransform() : InitialMapping(ids.first()); m_mixedMapping = false;
    for (const auto& id : ids) if (!same(InitialMapping(id), m_alignment)) m_mixedMapping = true;
    const QSignalBlocker action(m_action), confirm(m_confirm), opacity(m_layerOpacity);
    const bool allConfirmed = !ids.isEmpty() && std::all_of(ids.cbegin(), ids.cend(), [&](const QString& id) {
        const auto value = DecisionFor(id); return value.confirmed && value.action == first.action && value.layerId == m_layerId && value.applyLayerOpacity == first.applyLayerOpacity && same(value.atlasToDocument, first.atlasToDocument);
    });
    m_action->setCurrentIndex(m_action->findData(int(first.action))); m_confirm->setChecked(allConfirmed); m_layerOpacity->setChecked(first.applyLayerOpacity);
    QStringList notes;
    for (const auto& id : ids) { if (!m_matchReasons.value(id).isEmpty()) notes.append(id + QStringLiteral("：") + m_matchReasons.value(id));
        for (const auto& mesh : m_context.snapshot.meshes) if (mesh.material.id == id && !mesh.editable) notes.append(id + QStringLiteral("：") + mesh.reason); }
    const int layer = LayerIndex(m_layerId);
    if (layer >= 0) { const auto& value = m_request.incoming.revision.layers[layer]; notes.prepend(QStringLiteral("所选图层：%1").arg(path(m_request.incoming.revision.layers, layer))); notes += diagnosticText(value.diagnostics); if (!value.visible) notes.append(QStringLiteral("PSD 中该层已隐藏；导入保留模型对象的可见性。")); }
    m_selectionInfo->setText(QStringLiteral("已选 %1 个模型对象\n%2").arg(ids.size()).arg(notes.join(QLatin1Char('\n'))));
}

void CPsdImportDialog::SelectLayer()
{
    if (m_refreshing || !m_ready) return;
    if (IsBusy() || HasPendingInput()) { const QSignalBlocker block(m_layers->selectionModel()); m_layers->setCurrentIndex(m_layerProxy->mapFromSource(m_layerModel->Find(m_layerId))); ShowError(QStringLiteral("请先应用或放弃当前输入，再切换图层")); return; }
    m_layerId = m_layers->currentIndex().data(Qt::UserRole).toString(); RefreshDecision(); LoadView();
}
void CPsdImportDialog::SelectMeshes()
{
    if (m_refreshing || !m_ready) return;
    if (IsBusy() || HasPendingInput()) {
        const QSignalBlocker block(m_meshes->selectionModel()); m_meshes->selectionModel()->clearSelection();
        for (const auto& id : m_selectedMeshes) m_meshes->selectionModel()->select(m_meshProxy->mapFromSource(m_meshModel->Find(id)), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        ShowError(QStringLiteral("请先应用或放弃当前输入，再切换模型对象")); return;
    }
    m_selectedMeshes.clear(); for (const auto& index : m_meshes->selectionModel()->selectedRows()) m_selectedMeshes.insert(index.data(Qt::UserRole).toString());
    RefreshDecision(); LoadView();
}
bool CPsdImportDialog::CheckInput()
{
    if (IsBusy() || !m_ready || !m_valid) return false;
    for (auto* input : {m_x, m_y, m_angle, m_sx, m_sy, m_documentX, m_documentY}) {
        if (!input->hasAcceptableInput()) { ShowError(QStringLiteral("请先修正无效的数值输入")); input->setFocus(); return false; } input->interpretText();
    }
    return true;
}
void CPsdImportDialog::ApplySelection()
{
    if (!CheckInput() || m_numericDirty || m_recipeDirty || m_source->HasGesture()) return;
    const auto action = Planner::Action(m_action->currentData().toInt());
    if (m_mixedMapping && action == Planner::Action::Associate) { ShowError(QStringLiteral("多个对象的映射不同，请逐对象确认关联")); return; }
    const int index = LayerIndex(m_layerId);
    if ((action == Planner::Action::Associate || action == Planner::Action::Update) && !m_viewData) { ShowError(QStringLiteral("来源画布读取失败，请先重新选择图层或模型对象")); return; }
    if ((action == Planner::Action::Associate || action == Planner::Action::Update) && (index < 0 || !supported(m_request.incoming.revision.layers[index]))) { ShowError(QStringLiteral("请选择可读取像素的栅格图层；不支持的图层可以保留在来源素材中")); return; }
    auto request = m_request;
    for (auto& decision : request.decisions) if (m_selectedMeshes.contains(decision.meshId)) {
        decision.action = action; decision.layerId = action == Planner::Action::Associate || action == Planner::Action::Update ? m_layerId : QString();
        if (action == Planner::Action::Associate) decision.atlasToDocument = m_alignment;
        decision.confirmed = m_confirm->isChecked(); decision.applyLayerOpacity = m_layerOpacity->isChecked();
    }
    m_request = request; m_requestDirty = true; Build(std::move(request));
}
void CPsdImportDialog::PreviewRequest()
{
    if (!CheckInput()) return;
    if (HasPendingInput()) { ShowError(QStringLiteral("请先应用选区决定或放弃输入")); return; } Build(m_request);
}
void CPsdImportDialog::ConfirmKnownMatches()
{
    if (!CheckInput() || HasPendingInput()) return;
    auto request = m_request;
    for (auto& decision : request.decisions) if (m_knownMatches.contains(decision.meshId)) { decision.action = Planner::Action::Update; decision.layerId = m_suggestedLayers.value(decision.meshId); decision.confirmed = false; }
    m_request = request; m_requestDirty = true; Build(std::move(request));
}
void CPsdImportDialog::MarkMappingEdited()
{
    const QSignalBlocker action(m_action), confirm(m_confirm);
    m_inputDirty = true; m_action->setCurrentIndex(m_action->findData(int(Planner::Action::Associate))); m_confirm->setChecked(false);
}
void CPsdImportDialog::ApplyAlignment()
{
    if (!CheckInput() || m_source->HasGesture()) return;
    if (m_mixedMapping || m_selectedMeshes.size() != 1 || !m_viewData || m_viewData->reference.isNull()) { ShowError(QStringLiteral("请选择一个有图集参照的对象进行配准")); return; }
    const auto bounds = m_alignment.mapRect(QRectF(m_viewData->atlasBounds)); const auto pivot = bounds.center();
    QTransform adjustment; adjustment.translate(m_x->value() + pivot.x(), m_y->value() + pivot.y()); adjustment.rotate(m_angle->value()); adjustment.scale(m_sx->value() / 100, m_sy->value() / 100); adjustment.translate(-pivot.x(), -pivot.y());
    const auto candidate = m_alignment * adjustment; QString error;
    if (!m_source->SetMapping(candidate, &error)) { ShowError(error); return; }
    m_alignment = candidate; MarkMappingEdited(); m_numericDirty = false;
    const QSignalBlocker x(m_x), y(m_y), a(m_angle), sx(m_sx), sy(m_sy), c(m_confirm);
    m_x->setValue(0); m_y->setValue(0); m_angle->setValue(0); m_sx->setValue(100); m_sy->setValue(100); m_confirm->setChecked(false); Refresh();
}
void CPsdImportDialog::ProposeAlignment()
{
    if (!CheckInput() || m_numericDirty || m_source->HasGesture()) return;
    const int index = LayerIndex(m_layerId);
    if (m_selectedMeshes.size() != 1 || !m_viewData || m_viewData->reference.isNull() || index < 0 || !supported(m_request.incoming.revision.layers[index])) { ShowError(QStringLiteral("请选择一个模型对象与可读取的源图层")); return; }
    const auto target = m_request.incoming.revision.layers[index].bounds, source = m_viewData->atlasBounds;
    const double x = double(target.width()) / source.width(), y = double(target.height()) / source.height();
    const QTransform candidate(x, 0, 0, y, target.x() - source.x() * x, target.y() - source.y() * y); QString error;
    if (!m_source->SetMapping(candidate, &error)) { ShowError(error); return; }
    m_alignment = candidate; MarkMappingEdited(); Refresh();
    ShowError(QStringLiteral("这是按当前图集裁片与 PSD 图层区域生成的提案，请检查后明确确认；未使用模型姿态包围盒。"));
}
void CPsdImportDialog::ApplyDocumentAlignment()
{
    if (!CheckInput() || m_inputDirty || m_numericDirty || m_source->HasGesture()) { ShowError(QStringLiteral("请先应用或放弃选区配准输入")); return; }
    const auto& artwork = m_context.snapshot.baseline.artwork; const auto family = artwork.families.value(m_request.incoming.revision.familyId);
    const auto previous = artwork.revisions.value(family.currentRevision); const auto incoming = m_request.incoming.revision.canvasSize;
    QTransform registration = previous.id.isEmpty() ? QTransform() : previous.documentToFamily; QTransform alignment;
    if (m_documentAlignment->currentIndex() == 1 && !previous.id.isEmpty()) alignment.translate((previous.canvasSize.width() - incoming.width()) / 2.0, (previous.canvasSize.height() - incoming.height()) / 2.0);
    else if (m_documentAlignment->currentIndex() == 2 && !previous.id.isEmpty()) alignment.scale(double(previous.canvasSize.width()) / incoming.width(), double(previous.canvasSize.height()) / incoming.height());
    else if (m_documentAlignment->currentIndex() == 3) registration = m_request.incoming.revision.documentToFamily;
    QTransform offset; offset.translate(m_documentX->value(), m_documentY->value()); const auto candidate = alignment * offset * registration; QString error;
    if (!Artwork::ValidateTransform(candidate, &error)) { ShowError(error); return; }
    m_request.incoming.revision.documentToFamily = candidate;
    m_request.incoming.revision.colorPolicy = Artwork::ColorPolicy(m_colorPolicy->currentData().toInt());
    for (auto& decision : m_request.decisions) decision.confirmed = false;
    m_documentAlignmentConfirmed = true; m_requestDirty = true; Build(m_request);
}
void CPsdImportDialog::RestoreInputs()
{
    const QSignalBlocker family(m_familyName), x(m_x), y(m_y), a(m_angle), sx(m_sx), sy(m_sy), dx(m_documentX), dy(m_documentY), mode(m_documentAlignment), color(m_colorPolicy);
    m_familyName->setText(m_request.familyName); m_x->setValue(0); m_y->setValue(0); m_angle->setValue(0); m_sx->setValue(100); m_sy->setValue(100);
    m_documentX->setValue(0); m_documentY->setValue(0); m_documentAlignment->setCurrentIndex(3);
    m_colorPolicy->setCurrentIndex(m_colorPolicy->findData(int(m_request.incoming.revision.colorPolicy)));
}

void CPsdImportDialog::DiscardInput()
{
    if (IsBusy()) return; QString error; if (!m_source->CancelGesture(&error)) { ShowError(error); return; }
    m_request = m_acceptedRequest; m_inputDirty = m_numericDirty = m_requestDirty = m_recipeDirty = false; m_documentAlignmentConfirmed = m_history[m_historyIndex].documentAlignmentConfirmed;
    RestoreInputs();
    ShowError({}); RefreshDecision(); LoadView();
}
void CPsdImportDialog::Undo()
{
    if (!CheckInput() || m_historyIndex <= 0) return;
    if (HasPendingInput() || m_requestDirty) { ShowError(QStringLiteral("请先应用或放弃未完成输入，再撤销")); return; }
    const auto entry = m_history[m_historyIndex - 1]; const QPointer<CPsdImportDialog> alive(this);
    if (Publish(entry.plan, entry.request, false) && alive) { --m_historyIndex; m_documentAlignmentConfirmed = entry.documentAlignmentConfirmed; RestoreInputs(); RefreshDecision(); LoadView(); }
}
void CPsdImportDialog::Redo()
{
    if (!CheckInput() || m_historyIndex + 1 >= m_history.size()) return;
    if (HasPendingInput() || m_requestDirty) { ShowError(QStringLiteral("请先应用或放弃未完成输入，再重做")); return; }
    const auto entry = m_history[m_historyIndex + 1]; const QPointer<CPsdImportDialog> alive(this);
    if (Publish(entry.plan, entry.request, false) && alive) { ++m_historyIndex; m_documentAlignmentConfirmed = entry.documentAlignmentConfirmed; RestoreInputs(); RefreshDecision(); LoadView(); }
}
void CPsdImportDialog::RefreshViewOpacity()
{
    double opacity = m_sourceOpacity->value() / 100.0; const int index = LayerIndex(m_layerId);
    if (index >= 0 && m_layerOpacity->isChecked()) opacity *= m_request.incoming.revision.layers[index].opacity / 255.0;
    m_source->SetSourceOpacity(opacity); m_source->SetReferenceOpacity(m_referenceOpacity->value() / 100.0);
}
void CPsdImportDialog::StopCalculation() { if (m_job) { m_job->control.Cancel(); m_status->setText(QStringLiteral("正在停止计算…")); } }
void CPsdImportDialog::SaveSourceOnly()
{
    if (!CheckInput()) return;
    if (HasPendingInput()) { ShowError(QStringLiteral("请先应用或放弃配准输入，再仅保存来源")); return; }
    if (!m_documentAlignmentConfirmed) { ShowError(QStringLiteral("请先确认新旧 PSD 文档画布的对齐关系")); return; }
    Build(sourceOnly(m_request), true);
}
void CPsdImportDialog::Submit(const Plan& plan)
{
    if (IsBusy() || !m_ready) return;
    QString error;
    if (!m_documentAlignmentConfirmed || !Planner::Validate(m_context.snapshot, plan, &error, true)) { ShowError(error.isEmpty() ? QStringLiteral("请先确认文档对齐与图层关联") : error); Refresh(); return; }
    if (!m_submit) { ShowError(QStringLiteral("素材保存尚未连接")); return; }
    const QPointer<CPsdImportDialog> alive(this); const auto handler = m_submit; m_busy = true; bool accepted = false;
    try { accepted = handler(plan, &error); } catch (...) { error = QStringLiteral("素材保存失败，草稿保留在窗口中"); }
    if (!alive) return; m_busy = false;
    if (!accepted) { ShowError(error.isEmpty() ? QStringLiteral("素材保存未能完成") : error); Refresh(); return; }
    m_plan = plan; m_finished = true; QDialog::accept();
}
void CPsdImportDialog::accept()
{
    if (!CheckInput()) return;
    if (HasPendingInput() || m_requestDirty) { ShowError(QStringLiteral("仍有未应用修改，请先应用选区决定并预览")); return; }
    Submit(m_plan);
}
void CPsdImportDialog::reject()
{
    if (m_busy) return;
    if (m_finished) { QDialog::reject(); return; }
    if (m_job) { m_closeWhenIdle = true; StopCalculation(); return; }
    const QPointer<CPsdImportDialog> alive(this); const auto handler = m_cancel; QString error; bool canceled = true; m_busy = true;
    try { if (handler) canceled = handler(&error); else if (m_published) { canceled = false; error = QStringLiteral("素材恢复尚未连接"); } }
    catch (...) { canceled = false; error = QStringLiteral("恢复模型预览失败，请保留窗口并重试取消"); }
    if (!alive) return; m_busy = false;
    if (!canceled) { ShowError(error.isEmpty() ? QStringLiteral("未能恢复进入时状态") : error); Refresh(); return; }
    m_finished = true; QDialog::reject();
}
void CPsdImportDialog::done(int result) { if (m_finished) QDialog::done(result); else if (result == QDialog::Accepted) accept(); else reject(); }
void CPsdImportDialog::closeEvent(QCloseEvent* event) { event->ignore(); reject(); }
void CPsdImportDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event); if (!m_splitter) return;
    const bool compact = event->size().width() < fontMetrics().averageCharWidth() * 150;
    if (compact && m_compact->count() == 0) {
        m_compact->addTab(m_left, QStringLiteral("来源图层")); m_compact->addTab(m_center, QStringLiteral("像素配准")); m_compact->addTab(m_inspector, QStringLiteral("模型关联"));
        m_compact->setCurrentIndex(1); m_splitter->hide(); m_compact->show();
    } else if (!compact && m_compact->count() != 0) {
        while (m_compact->count()) m_compact->removeTab(0);
        m_splitter->addWidget(m_left); m_splitter->addWidget(m_center); m_splitter->addWidget(m_inspector);
        m_left->show(); m_center->show(); m_inspector->show(); m_compact->hide(); m_splitter->show();
        m_splitter->setSizes({event->size().width() / 5, event->size().width() * 2 / 5, event->size().width() * 2 / 5});
    }
}
