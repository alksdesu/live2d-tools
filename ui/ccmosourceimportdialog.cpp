#include "ccmosourceimportdialog.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace {
using Artwork = live2dstruct::CModelArtwork;
constexpr int DocumentRole = Qt::UserRole, LayerRole = Qt::UserRole + 1, IncompleteRole = Qt::UserRole + 2;
bool errors(const QVector<CPsdSourceDiagnostic>& diagnostics)
{
    return std::any_of(diagnostics.cbegin(), diagnostics.cend(), [](const auto& value) {
        return value.severity == CPsdSourceDiagnostic::Severity::Error;
    });
}
QString notes(const QVector<CPsdSourceDiagnostic>& diagnostics, const QSet<QString>& layerIds = {})
{
    QStringList values; QHash<QString, int> counts;
    for (const auto& value : diagnostics) {
        if (!layerIds.isEmpty() && !value.layerId.isEmpty() && !layerIds.contains(value.layerId) &&
            value.severity != CPsdSourceDiagnostic::Severity::Error) continue;
        const auto message = (value.severity == CPsdSourceDiagnostic::Severity::Error ? QStringLiteral("无法完整恢复：") :
            value.severity == CPsdSourceDiagnostic::Severity::Warning ? QStringLiteral("注意：") : QString()) + value.message;
        if (!counts.contains(message)) values.append(message);
        ++counts[message];
    }
    for (auto& value : values) if (counts.value(value) > 1) value = QStringLiteral("%1（%2 项）").arg(value).arg(counts.value(value));
    return values.join('\n');
}
QString transform(const QTransform& value)
{
    return QStringLiteral("[%1, %2, %3; %4, %5, %6]")
        .arg(value.m11(), 0, 'g', 9).arg(value.m21(), 0, 'g', 9).arg(value.dx(), 0, 'g', 9)
        .arg(value.m12(), 0, 'g', 9).arg(value.m22(), 0, 'g', 9).arg(value.dy(), 0, 'g', 9);
}
}

CCmoSourceImportDialog::CCmoSourceImportDialog(const cmo3::CCmo3Project& project, QWidget* parent)
    : QDialog(parent), m_project(project)
{
    setObjectName(QStringLiteral("cmoSourceImportDialog")); setWindowTitle(QStringLiteral("保存 CMO3 原稿素材")); setModal(true); setSizeGripEnabled(true);
    const int gap = std::max(fontMetrics().averageCharWidth(), style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing));
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(gap, gap, gap, gap); layout->setSpacing(gap);
    auto* intro = new QLabel(QStringLiteral("选择要保存的原稿文档。原始工程与已读取素材会随项目保留；PSD 恢复能力以来源说明为准。此次保存不改变当前模型外观。"), this);
    intro->setTextFormat(Qt::PlainText); intro->setWordWrap(true); layout->addWidget(intro);
    auto* tools = new QHBoxLayout;
    auto* all = new QPushButton(QStringLiteral("全选文档"), this); all->setObjectName(QStringLiteral("cmoSourceSelectAll")); all->setAutoDefault(false);
    auto* none = new QPushButton(QStringLiteral("取消全选"), this); none->setObjectName(QStringLiteral("cmoSourceSelectNone")); none->setAutoDefault(false);
    tools->addWidget(all); tools->addWidget(none); tools->addStretch(); layout->addLayout(tools);
    m_splitter = new QSplitter(Qt::Horizontal, this); m_splitter->setObjectName(QStringLiteral("cmoSourceSplitter")); m_splitter->setChildrenCollapsible(false);
    m_tree = new QTreeWidget(m_splitter); m_tree->setObjectName(QStringLiteral("cmoSourceDocuments")); m_tree->setAccessibleName(QStringLiteral("原稿文档与图层树"));
    m_tree->setHeaderLabels({QStringLiteral("原稿与图层"), QStringLiteral("内容")}); m_tree->setUniformRowHeights(true);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows); m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch); m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->setMinimumWidth(fontMetrics().averageCharWidth() * 24); m_tree->setMinimumHeight(fontMetrics().height() * 6);
    m_details = new QPlainTextEdit(m_splitter); m_details->setObjectName(QStringLiteral("cmoSourceDetails")); m_details->setAccessibleName(QStringLiteral("原稿信息与原生素材关系"));
    m_details->setReadOnly(true); m_details->setLineWrapMode(QPlainTextEdit::WidgetWidth); m_details->setMinimumHeight(fontMetrics().height() * 6);
    m_splitter->setStretchFactor(0, 1); m_splitter->setStretchFactor(1, 1); layout->addWidget(m_splitter, 1);
    auto* settings = new QFormLayout; settings->setRowWrapPolicy(QFormLayout::WrapLongRows); settings->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_color = new QComboBox(this); m_color->setObjectName(QStringLiteral("cmoSourceColorPolicy"));
    m_color->addItem(QStringLiteral("保留原始颜色数值"), int(Artwork::ColorPolicy::PreserveValues));
    m_color->addItem(QStringLiteral("转换工作素材到 sRGB"), int(Artwork::ColorPolicy::ConvertToSRgb)); settings->addRow(QStringLiteral("工作素材颜色"), m_color); layout->addLayout(settings);
    auto* colorInfo = new QLabel(QStringLiteral("颜色设置只影响工作素材。恢复原稿 PSD 时使用工程中的原始像素和色彩配置。"), this);
    colorInfo->setTextFormat(Qt::PlainText); colorInfo->setWordWrap(true); layout->addWidget(colorInfo);
    m_status = new QLabel(this); m_status->setObjectName(QStringLiteral("cmoSourceStatus")); m_status->setTextFormat(Qt::PlainText); m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(m_status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_import = buttons->addButton(QStringLiteral("保存所选原稿"), QDialogButtonBox::AcceptRole); m_import->setObjectName(QStringLiteral("cmoSourceSave")); m_import->setDefault(true); layout->addWidget(buttons);
    if (const auto* graph = m_project.Materials()) for (int mesh = 0; mesh < graph->meshes.size(); ++mesh)
        for (int input = 0; input < graph->meshes[mesh].inputs.size(); ++input) {
            const auto& value = graph->meshes[mesh].inputs[input];
            if (!value.modelImageGuid.isEmpty()) m_meshInputs[value.modelImageGuid].append({mesh, input});
        }
    const auto* catalog = m_project.Artwork();
    if (catalog) for (const auto& entry : catalog->documents) {
        const auto* document = entry.Reconstructed(); if (!document) continue;
        int groups = 0, raster = 0, empty = 0, hidden = 0;
        for (const auto& layer : document->layers) {
            if (layer.kind == CPsdSourceLayer::Kind::Group) ++groups; else { ++raster; if (layer.bounds.isEmpty()) ++empty; }
            if (!layer.visible) ++hidden;
        }
        auto* item = new QTreeWidgetItem(m_tree);
        item->setText(0, document->name.isEmpty() ? QStringLiteral("未命名原稿") : document->name);
        item->setText(1, QStringLiteral("%1 图层 / %2 文件夹").arg(raster).arg(groups));
        item->setData(0, DocumentRole, document->id); item->setData(0, LayerRole, -1);
        item->setToolTip(0, QStringLiteral("%1 × %2 像素\n%3 个空图层，%4 项隐藏\n文档 GUID：%5")
            .arg(document->canvasSize.width()).arg(document->canvasSize.height()).arg(empty).arg(hidden).arg(document->id));
        const bool incomplete = errors(catalog->diagnostics) || errors(entry.diagnostics) || errors(document->diagnostics);
        item->setData(0, IncompleteRole, incomplete);
        if (!document->id.trimmed().isEmpty() && document->id.size() <= 1024 && document->id.isValidUtf16() && !document->id.contains(QChar::Null)) {
            m_available.insert(document->id); item->setFlags(item->flags() | Qt::ItemIsUserCheckable); item->setCheckState(0, Qt::Checked);
            if (incomplete) item->setText(1, QStringLiteral("含未解释属性，仍可保留"));
        } else item->setText(1, QStringLiteral("文档标识无效，不能保存"));
        QVector<QTreeWidgetItem*> layers; layers.reserve(document->layers.size());
        for (int index = 0; index < document->layers.size(); ++index) {
            const auto& layer = document->layers[index]; auto* row = new QTreeWidgetItem;
            row->setFlags(row->flags() & ~(Qt::ItemIsUserCheckable | Qt::ItemIsEditable));
            row->setText(0, layer.name.isEmpty() ? QStringLiteral("未命名图层") : layer.name);
            row->setText(1, layer.kind == CPsdSourceLayer::Kind::Group ? QStringLiteral("文件夹") : layer.bounds.isEmpty() ? QStringLiteral("空图层") :
                QStringLiteral("%1 × %2").arg(layer.bounds.width()).arg(layer.bounds.height()));
            row->setData(0, DocumentRole, document->id); row->setData(0, LayerRole, index); row->setToolTip(0, layer.name);
            layers.append(row);
        }
        for (int index = 0; index < layers.size(); ++index) {
            const int parentIndex = document->layers[index].parentIndex;
            if (parentIndex >= 0 && parentIndex < layers.size()) layers[parentIndex]->addChild(layers[index]); else item->addChild(layers[index]);
        }
        item->setExpanded(true);
    }
    connect(all, &QPushButton::clicked, this, [this] { SelectAll(true); }); connect(none, &QPushButton::clicked, this, [this] { SelectAll(false); });
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem*, int) { Refresh(); });
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem*, QTreeWidgetItem*) { ShowSelection(); });
    connect(m_color, &QComboBox::currentIndexChanged, this, [this](int) { Refresh(); });
    connect(buttons, &QDialogButtonBox::accepted, this, &CCmoSourceImportDialog::accept); connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    if (m_tree->topLevelItemCount()) m_tree->setCurrentItem(m_tree->topLevelItem(0)); else ShowSelection();
    const QSize desired(fontMetrics().averageCharWidth() * 110, fontMetrics().height() * 36);
    resize(screen() ? desired.boundedTo(screen()->availableGeometry().size()) : desired); Refresh();
}

QStringList CCmoSourceImportDialog::SelectedDocumentIds() const
{
    QStringList result;
    for (int index = 0; index < m_tree->topLevelItemCount(); ++index) {
        const auto* item = m_tree->topLevelItem(index); const auto id = item->data(0, DocumentRole).toString();
        if (m_available.contains(id) && item->checkState(0) == Qt::Checked) result.append(id);
    }
    return result;
}
Artwork::ColorPolicy CCmoSourceImportDialog::ColorPolicy() const { return Artwork::ColorPolicy(m_color->currentData().toInt()); }
void CCmoSourceImportDialog::SelectAll(bool selected)
{
    const QSignalBlocker blocker(m_tree);
    for (int index = 0; index < m_tree->topLevelItemCount(); ++index) {
        auto* item = m_tree->topLevelItem(index);
        if (m_available.contains(item->data(0, DocumentRole).toString())) item->setCheckState(0, selected ? Qt::Checked : Qt::Unchecked);
    }
    Refresh();
}
void CCmoSourceImportDialog::Refresh()
{
    const auto selected = SelectedDocumentIds(); int incomplete = 0;
    for (int index = 0; index < m_tree->topLevelItemCount(); ++index) {
        const auto* item = m_tree->topLevelItem(index);
        if (selected.contains(item->data(0, DocumentRole).toString()) && item->data(0, IncompleteRole).toBool()) ++incomplete;
    }
    const bool conversionBlocked = incomplete && ColorPolicy() == Artwork::ColorPolicy::ConvertToSRgb;
    m_import->setEnabled(!selected.isEmpty() && !conversionBlocked); m_color->setEnabled(!m_available.isEmpty());
    if (selected.isEmpty()) m_status->setText(m_available.isEmpty() ? QStringLiteral("没有可保存的原稿文档，请查看来源说明。") : QStringLiteral("勾选至少一份原稿文档。"));
    else if (conversionBlocked) m_status->setText(QStringLiteral("所选 %1 份原稿含未解释属性，不能完整转换颜色。请改为保留原始颜色数值，或取消勾选这些文档。").arg(incomplete));
    else {
        QString status = QStringLiteral("已选择 %1 份原稿；将一起保存，可一起撤销。").arg(selected.size());
        if (incomplete) status += QStringLiteral("\n其中 %1 份仅保留工程与已读取素材，不能保证完整 PSD 恢复或自动关联。").arg(incomplete);
        m_status->setText(status);
    }
}
void CCmoSourceImportDialog::accept() { Refresh(); if (m_import->isEnabled()) QDialog::accept(); }
void CCmoSourceImportDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    if (m_splitter) m_splitter->setOrientation(width() < fontMetrics().averageCharWidth() * 95 ? Qt::Vertical : Qt::Horizontal);
}

QString CCmoSourceImportDialog::Relations(const QString& documentId, const QString& layerId) const
{
    const auto* graph = m_project.Materials();
    if (!graph) return QStringLiteral("原生素材关系暂不能读取：%1").arg(m_project.MaterialsError());
    QStringList lines; int linked = 0;
    for (const auto& image : graph->images) for (const auto& candidate : image.candidates) {
        if (candidate.documentGuid != documentId) continue;
        QStringList layers;
        for (const auto& layer : candidate.layers) {
            if (!layerId.isEmpty() && layer.layerGuid != layerId) continue;
            layers.append(QStringLiteral("  图层：%1\n  GUID：%2\n  来源变换：%3").arg(layer.layerName, layer.layerGuid, transform(layer.affine)));
            if (!layer.clippingType.isEmpty()) layers.append(QStringLiteral("  输入剪切类型：%1").arg(layer.clippingType));
            for (const auto& note : layer.diagnostics) layers.append(QStringLiteral("  说明：%1").arg(note));
        }
        if (layers.isEmpty()) continue;
        ++linked; lines.append(QStringLiteral("模型参考图：%1\nGUID：%2\n%3").arg(image.name, image.guid,
            candidate.current ? QStringLiteral("该原稿是此参考图的当前来源") : QStringLiteral("该原稿是此参考图保留的候选来源")));
        lines.append(layers); lines.append(QStringLiteral("素材到画布：%1").arg(transform(image.materialLocalToCanvas)));
        if (!image.filterTypes.isEmpty()) lines.append(QStringLiteral("处理节点：%1").arg(image.filterTypes.join(QStringLiteral("、"))));
        int meshes = 0;
        for (const auto& indices : m_meshInputs.value(image.guid)) {
            const auto& mesh = graph->meshes[indices.first]; const auto& input = mesh.inputs[indices.second];
            ++meshes; lines.append(QStringLiteral("  ArtMesh：%1 (%2)\n  输入状态：%3\n  画布变换：%4").arg(mesh.name, mesh.id,
                input.current ? QStringLiteral("当前输入") : QStringLiteral("保留的其他输入"), transform(input.optionalTransformOnCanvas)));
            for (const auto& note : input.diagnostics) lines.append(QStringLiteral("  说明：%1").arg(note));
            for (const auto& note : mesh.diagnostics) lines.append(QStringLiteral("  说明：%1").arg(note));
        }
        if (!meshes) lines.append(QStringLiteral("没有活动 ArtMesh 使用这份参考图；原稿仍完整保留。"));
        lines.append(image.directRaster ? QStringLiteral("已识别单层来源；当前模型的采样映射尚未验证。") : QStringLiteral("包含尚未完整解释的素材处理关系。"));
        lines.append(image.diagnostics); lines.append(QString());
    }
    if (!linked) lines.append(QStringLiteral("所选来源没有已识别的模型参考图关系；保存原稿不要求存在模型关联。"));
    lines.append(graph->diagnostics);
    lines.append(QStringLiteral("这里展示工程内保存的关系，保存原稿不会执行这些关联或替换当前模型素材。"));
    return lines.join('\n');
}

void CCmoSourceImportDialog::ShowSelection()
{
    const auto* catalog = m_project.Artwork(); const auto* selected = m_tree->currentItem();
    if (!catalog || !selected) { m_details->setPlainText(m_project.ArtworkError().isEmpty() ? QStringLiteral("此工程没有可读取的原稿文档。") : m_project.ArtworkError()); return; }
    const auto documentId = selected->data(0, DocumentRole).toString(); const int layerIndex = selected->data(0, LayerRole).toInt();
    for (const auto& entry : catalog->documents) {
        const auto* document = entry.Reconstructed(); if (!document || document->id != documentId) continue;
        QStringList lines{document->name, QStringLiteral("%1 × %2 像素").arg(document->canvasSize.width()).arg(document->canvasSize.height()),
            QStringLiteral("文档 GUID：%1").arg(document->id), QStringLiteral("工程版本：%1").arg(m_project.SourceVersion())};
        if (errors(catalog->diagnostics) || errors(entry.diagnostics) || errors(document->diagnostics))
            lines.append(QStringLiteral("保留工程与已读取素材，不能保证完整 PSD 恢复或自动关联。请选择保留原始颜色数值。"));
        lines.append(document->iccProfile.isEmpty() ? QStringLiteral("原稿未带 ICC，工作素材按 sRGB 解释。") : QStringLiteral("原稿色彩配置已保留。"));
        QString layerId; QSet<QString> lineage;
        if (layerIndex >= 0 && layerIndex < document->layers.size()) {
            const auto& layer = document->layers[layerIndex]; layerId = layer.id;
            for (int index = layerIndex; index >= 0; index = document->layers[index].parentIndex) lineage.insert(document->layers[index].id);
            lines.append(QStringLiteral("\n图层：%1\n图层 GUID：%2\n位置：%3, %4\n透明度：%5 / 255\n混合模式：%6\n%7%8")
                .arg(layer.name, layer.id).arg(layer.bounds.x()).arg(layer.bounds.y()).arg(layer.opacity).arg(QString::fromLatin1(layer.blendMode),
                    layer.visible ? QStringLiteral("可见") : QStringLiteral("隐藏"), layer.clipping ? QStringLiteral("；剪贴图层") : QString()));
        }
        lines.append(notes(catalog->diagnostics)); lines.append(notes(entry.diagnostics)); lines.append(notes(document->diagnostics, lineage));
        lines.append(QStringLiteral("\n原生素材关系")); lines.append(Relations(documentId, layerId)); m_details->setPlainText(lines.join('\n')); return;
    }
    m_details->clear();
}
