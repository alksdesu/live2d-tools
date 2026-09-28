#include "cpsdrecoverydialog.h"
#include <QAbstractItemModel>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSet>
#include <QSplitter>
#include <QTreeView>
#include <QVBoxLayout>
#include <exception>

namespace {
class SourceTreeView final : public QTreeView
{
public:
    using QTreeView::QTreeView;
protected:
    void drawBranches(QPainter* painter, const QRect& rect, const QModelIndex& index) const override {
        if (!model()->hasChildren(index)) return;
        const qreal span = qMin(rect.height(), indentation()) / 4.0;
        const QPointF center(layoutDirection() == Qt::LeftToRight ? rect.right() - indentation() / 2.0 :
            rect.left() + indentation() / 2.0, rect.center().y());
        const qreal direction = layoutDirection() == Qt::LeftToRight ? 1 : -1;
        const QPolygonF points = isExpanded(index)
            ? QPolygonF{center + QPointF(-span, -span / 2), center + QPointF(0, span / 2), center + QPointF(span, -span / 2)}
            : QPolygonF{center + QPointF(-direction * span / 2, -span), center + QPointF(direction * span / 2, 0), center + QPointF(-direction * span / 2, span)};
        painter->save(); painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(palette().color(QPalette::Text), qMax(1.0, 2.0 / devicePixelRatioF()), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->drawPolyline(points); painter->restore();
    }
};

struct LayerSummary
{
    QString name;
    int parentIndex = -1;
    QRect bounds;
    QByteArray blendMode;
    quint8 opacity = 255;
    bool group = false, empty = false, original = false, visible = true, clipping = false;
};

QString documentName(const CPsdSourceCatalog::Document& document)
{
    if (const auto* rebuilt = document.Reconstructed()) return rebuilt->name;
    return document.origin.fileName.isEmpty() ? document.origin.familyName : document.origin.fileName;
}

QString originPath(const CPsdSourceCatalog& catalog, const CPsdSourceCatalog::Document& document)
{
    return document.origin.sourcePath.isEmpty() && document.Reconstructed() ? catalog.sourcePath : document.origin.sourcePath;
}

QString versionDescription(const CPsdSourceCatalog::Origin& origin)
{
    QStringList status;
    status.append(origin.currentRevision ? QStringLiteral("目录当前版本") : QStringLiteral("已保存版本"));
    if (origin.linkedMeshes) status.append(QStringLiteral("%1 个对象正在关联").arg(origin.linkedMeshes));
    if (origin.detachedMeshes) status.append(QStringLiteral("%1 个对象已解除关联").arg(origin.detachedMeshes));
    if (!origin.linkedMeshes && !origin.detachedMeshes) status.append(QStringLiteral("当前未被对象使用"));
    return status.join(QStringLiteral(" · "));
}

class SourceLayers final : public QAbstractItemModel
{
public:
    explicit SourceLayers(QObject* parent) : QAbstractItemModel(parent) {}
    bool SetDocument(const CPsdSourceCatalog::Document* document) {
        QVector<LayerSummary> layers;
        bool valid = document != nullptr;
        if (document && document->Reconstructed()) {
            for (const auto& layer : document->Reconstructed()->layers)
                layers.append({layer.name, layer.parentIndex, layer.bounds, layer.blendMode, layer.opacity,
                    layer.kind == CPsdSourceLayer::Kind::Group, layer.png.isEmpty(), false, layer.visible, layer.clipping});
        } else if (document && document->Original() && document->Original()->Directory()) {
            const auto& directory = *document->Original()->Directory();
            QVector<int> remap(directory.layers.size(), -1);
            for (int source : directory.displayOrder) {
                if (source < 0 || source >= directory.layers.size() || remap[source] >= 0) { valid = false; break; }
                const auto& layer = directory.layers[source];
                const int parent = layer.parentIndex < 0 ? -1 : remap.value(layer.parentIndex, -1);
                if ((layer.parentIndex >= 0 && parent < 0) || layer.kind == CPsdLayer::Kind::GroupEnd) { valid = false; break; }
                remap[source] = layers.size();
                layers.append({layer.name, parent, layer.bounds, layer.blendMode, layer.opacity,
                    layer.kind == CPsdLayer::Kind::Group, false, true, layer.visible, layer.clipping});
            }
        } else valid = false;
        QVector<QVector<int>> children(layers.size() + 1); QVector<int> rows(layers.size());
        if (valid) for (int index = 0; index < layers.size(); ++index) {
            const int parent = layers[index].parentIndex;
            if (parent < -1 || parent >= index || (parent >= 0 && !layers[parent].group)) { valid = false; break; }
            rows[index] = children[parent + 1].size(); children[parent + 1].append(index);
        }
        beginResetModel();
        m_layers = valid ? std::move(layers) : QVector<LayerSummary>();
        m_children = valid ? std::move(children) : QVector<QVector<int>>(); m_rows = valid ? std::move(rows) : QVector<int>();
        endResetModel(); return valid;
    }
    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override {
        if (row < 0 || column < 0 || column >= columnCount() || (parent.isValid() && parent.column() != 0)) return {};
        const int offset = parent.isValid() ? int(parent.internalId()) : 0;
        if (offset < 0 || offset >= m_children.size() || row >= m_children[offset].size()) return {};
        return createIndex(row, column, quintptr(m_children[offset][row] + 1));
    }
    QModelIndex parent(const QModelIndex& child) const override {
        const auto* layer = Layer(child); if (!layer || layer->parentIndex < 0) return {};
        return createIndex(m_rows[layer->parentIndex], 0, quintptr(layer->parentIndex + 1));
    }
    int rowCount(const QModelIndex& parent = {}) const override {
        if (parent.isValid() && parent.column() != 0) return 0;
        const int offset = parent.isValid() ? int(parent.internalId()) : 0;
        return offset >= 0 && offset < m_children.size() ? m_children[offset].size() : 0;
    }
    int columnCount(const QModelIndex& = {}) const override { return 2; }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        return section == 0 ? QStringLiteral("来源图层") : QStringLiteral("状态");
    }
    QVariant data(const QModelIndex& index, int role) const override {
        const auto* layer = Layer(index); if (!layer) return {};
        if (role == Qt::UserRole) return int(index.internalId()) - 1;
        if (role != Qt::DisplayRole && role != Qt::AccessibleTextRole) return {};
        if (index.column() == 0) return layer->name.isEmpty() ? QStringLiteral("未命名图层") : layer->name;
        QStringList state;
        if (layer->group) state.append(QStringLiteral("文件夹"));
        else if (layer->original) state.append(QStringLiteral("原文件图层"));
        else if (layer->empty) state.append(QStringLiteral("空图层"));
        if (!layer->visible) state.append(QStringLiteral("隐藏"));
        if (layer->clipping) state.append(QStringLiteral("剪切"));
        if (layer->opacity != 255) state.append(QStringLiteral("透明度 %1%").arg(layer->opacity * 100.0 / 255, 0, 'f', 1));
        return state.isEmpty() ? QStringLiteral("图像") : state.join(QStringLiteral(" · "));
    }
    const LayerSummary* Layer(const QModelIndex& index) const {
        if (!index.isValid() || index.model() != this || !index.internalId() || index.internalId() > quintptr(m_layers.size())) return nullptr;
        return &m_layers[int(index.internalId()) - 1];
    }
    const QVector<LayerSummary>& Layers() const { return m_layers; }
private:
    QVector<LayerSummary> m_layers;
    QVector<QVector<int>> m_children;
    QVector<int> m_rows;
};

QString notes(const QVector<CPsdSourceDiagnostic>& diagnostics)
{
    QStringList result;
    for (const auto& item : diagnostics) {
        const auto label = item.severity == CPsdSourceDiagnostic::Severity::Error ? QStringLiteral("无法恢复") :
            item.severity == CPsdSourceDiagnostic::Severity::Warning ? QStringLiteral("注意") : QStringLiteral("说明");
        result.append(QStringLiteral("%1：%2%3").arg(label, item.message,
            item.layerId.isEmpty() ? QString() : QStringLiteral(" [%1]").arg(item.layerId)));
    }
    return result.join(QLatin1Char('\n'));
}

QString canonicalDestination(const QString& path)
{
    const QFileInfo file(path);
    if (file.exists() || file.isSymLink()) return file.canonicalFilePath();
    const QString directory = QFileInfo(file.absolutePath()).canonicalFilePath();
    return directory.isEmpty() ? QString() : QDir(directory).filePath(file.fileName());
}

QString blendName(const QByteArray& key)
{
    if (key == "norm") return QStringLiteral("正常");
    if (key == "mul ") return QStringLiteral("正片叠底");
    if (key == "lddg") return QStringLiteral("线性减淡（添加）");
    if (key == "pass") return QStringLiteral("穿透");
    return QStringLiteral("来源模式 %1").arg(QString::fromLatin1(key));
}
}

CPsdRecoveryDialog::CPsdRecoveryDialog(std::shared_ptr<const CPsdSourceCatalog> catalog, QWidget* parent)
    : QDialog(parent), m_catalog(std::move(catalog))
{
    setObjectName(QStringLiteral("psdRecoveryDialog")); setWindowTitle(QStringLiteral("导出来源 PSD"));
    setModal(true); setSizeGripEnabled(true);
    auto* layout = new QVBoxLayout(this);
    auto* description = new QLabel(QStringLiteral("选择一份来源文档或已保存的 PSD 版本。每份文档分别保存，不改变模型中的素材关联。"), this);
    description->setTextFormat(Qt::PlainText); description->setWordWrap(true); layout->addWidget(description);
    m_documents = new QComboBox(this); m_documents->setObjectName(QStringLiteral("psdRecoveryDocuments"));
    m_documents->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); m_documents->setMinimumContentsLength(12);
    if (m_catalog) for (int index = 0; index < m_catalog->documents.size(); ++index) {
        const auto& document = m_catalog->documents[index];
        const auto name = documentName(document);
        const auto family = document.Original() && !document.origin.familyName.isEmpty() ? document.origin.familyName + QStringLiteral(" / ") : QString();
        const auto version = document.Original() ? QStringLiteral(" · ") + versionDescription(document.origin) : QStringLiteral(" · CMO3 图层重建");
        m_documents->addItem(QStringLiteral("%1. %2%3%4").arg(index + 1).arg(family, name.isEmpty() ? QStringLiteral("未命名文档") : name, version), index);
    }
    QStringList sources = m_catalog ? m_catalog->protectedPaths : QStringList();
    if (m_catalog) {
        if (!m_catalog->sourcePath.isEmpty()) sources.append(m_catalog->sourcePath);
        for (const auto& document : m_catalog->documents) sources.append(originPath(*m_catalog, document));
    }
    m_protectedPaths = CaptureProtectedPaths(sources);
    layout->addWidget(m_documents);
    m_summary = new QLabel(this); m_summary->setObjectName(QStringLiteral("psdRecoverySummary"));
    m_summary->setTextFormat(Qt::PlainText); m_summary->setWordWrap(true); m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_summary);
    auto* splitter = new QSplitter(Qt::Vertical, this); splitter->setChildrenCollapsible(false);
    m_layers = new SourceTreeView(splitter); m_layers->setObjectName(QStringLiteral("psdRecoveryLayers"));
    m_layers->setModel(new SourceLayers(m_layers)); m_layers->setUniformRowHeights(true);
    m_layers->setSelectionBehavior(QAbstractItemView::SelectRows); m_layers->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_layers->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_layers->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_layers->setMinimumHeight(fontMetrics().height() * 5);
    m_details = new QPlainTextEdit(splitter); m_details->setObjectName(QStringLiteral("psdRecoveryDetails"));
    m_details->setReadOnly(true); m_details->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_details->setMinimumHeight(fontMetrics().height() * 4);
    splitter->setStretchFactor(0, 3); splitter->setStretchFactor(1, 2); layout->addWidget(splitter, 1);
    m_status = new QLabel(this); m_status->setObjectName(QStringLiteral("psdRecoveryStatus"));
    m_status->setTextFormat(Qt::PlainText); m_status->setWordWrap(true); m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); layout->addWidget(m_status);
    auto* buttons = new QDialogButtonBox(this);
    m_save = buttons->addButton(QStringLiteral("导出所选文档…"), QDialogButtonBox::ActionRole);
    m_save->setObjectName(QStringLiteral("psdRecoveryExport"));
    m_close = buttons->addButton(QStringLiteral("关闭"), QDialogButtonBox::RejectRole); m_close->setObjectName(QStringLiteral("psdRecoveryClose"));
    m_close->setDefault(true); layout->addWidget(buttons);
    connect(m_documents, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_exporting) SelectDocument();
        else { const QSignalBlocker blocker(m_documents); m_documents->setCurrentIndex(m_documents->findData(m_exportIndex)); }
    });
    connect(m_layers->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { SelectLayer(); });
    connect(m_save, &QPushButton::clicked, this, &CPsdRecoveryDialog::ChooseOutput);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    resize(fontMetrics().averageCharWidth() * 90, fontMetrics().height() * 34);
    SelectDocument();
}

void CPsdRecoveryDialog::SetExportHandler(ExportHandler handler) { m_export = std::move(handler); RefreshControls(); }
int CPsdRecoveryDialog::SelectedDocumentIndex() const { return m_documents->currentIndex() < 0 ? -1 : m_documents->currentData().toInt(); }

void CPsdRecoveryDialog::SelectDocument()
{
    m_error.clear(); m_status->clear();
    const int index = SelectedDocumentIndex();
    const auto* document = m_catalog && index >= 0 && index < m_catalog->documents.size() ? &m_catalog->documents[index] : nullptr;
    const QPointer<CPsdRecoveryDialog> alive(this);
    auto* model = static_cast<SourceLayers*>(m_layers->model()); const bool valid = model->SetDocument(document);
    if (!alive) return;
    m_validDocument = valid;
    if (!document) { m_summary->setText(QStringLiteral("没有可保存的来源文档")); m_details->clear(); RefreshControls(); return; }
    int raster = 0, groups = 0, empty = 0, hidden = 0;
    for (const auto& layer : model->Layers()) {
        if (layer.group) ++groups; else { ++raster; if (layer.empty) ++empty; }
        if (!layer.visible) ++hidden;
    }
    for (const auto& value : m_catalog->diagnostics) if (value.severity == CPsdSourceDiagnostic::Severity::Error) m_validDocument = false;
    for (const auto& value : document->diagnostics) if (value.severity == CPsdSourceDiagnostic::Severity::Error) m_validDocument = false;
    const auto* original = document->Original(); const auto* reconstructed = document->Reconstructed();
    if (original) m_validDocument = m_validDocument && original->IsValid();
    if (reconstructed) for (const auto& value : reconstructed->diagnostics)
        if (value.severity == CPsdSourceDiagnostic::Severity::Error) m_validDocument = false;
    const auto size = original && original->Directory() ? original->Directory()->canvasSize : reconstructed ? reconstructed->canvasSize : QSize();
    m_summary->setText(original ? QStringLiteral("%1 × %2 像素 · %3 个来源图层 · %4 个文件夹 · %5 项隐藏\n%6")
        .arg(size.width()).arg(size.height()).arg(raster).arg(groups).arg(hidden).arg(versionDescription(document->origin)) :
        QStringLiteral("%1 × %2 像素 · %3 个图层（含 %4 个空图层）· %5 个文件夹 · %6 项隐藏")
        .arg(size.width()).arg(size.height()).arg(raster).arg(empty).arg(groups).arg(hidden));
    if (original && !original->IsValid()) m_summary->setText(QStringLiteral("这个已保存版本无法验证，具体原因见下方说明。\n%1").arg(versionDescription(document->origin)));
    m_save->setText(original ? QStringLiteral("原样另存 PSD…") : QStringLiteral("重建所选 PSD…"));
    if (!m_validDocument) m_error = QStringLiteral("这份来源文档包含无效目录或无法保存的内容，详情见下方说明");
    m_layers->expandToDepth(0); if (!alive) return;
    SelectLayer(); if (alive) RefreshControls();
}

void CPsdRecoveryDialog::SelectLayer()
{
    const int index = SelectedDocumentIndex();
    if (!m_catalog || index < 0 || index >= m_catalog->documents.size()) return;
    const auto& document = m_catalog->documents[index];
    QStringList detail;
    if (const auto* original = document.Original()) {
        detail.append(original->IsValid() ? QStringLiteral("保存导入时的原始 PSD，包括蒙版、效果、色彩配置与元数据。当前模型的编辑不会写入这份文件。") :
            QStringLiteral("这个版本的 PSD 原文件未通过完整性检查，暂不能另存。其它完好的已保存版本仍可选择。"));
        detail.append(QStringLiteral("%1\n目录当前版本不代表所有对象采用的版本；选择和另存不会替换任何对象的旧版关联。").arg(versionDescription(document.origin)));
        if (const auto* directory = original->Directory()) {
            detail.append(directory->hasMergedPreview ? QStringLiteral("原文件中的合成预览会原样保留。") : QStringLiteral("原文件未提供有效合成预览；另存不会生成新的预览。"));
            for (const auto& note : directory->diagnostics) detail.append(QStringLiteral("原文件说明：%1").arg(note.message));
        }
    } else {
        detail.append(QStringLiteral("根据 CMO3 工程内保存的来源图层重建 PSD。空图层只保留身份，不代表已找回缺失像素。"));
        detail.append(QStringLiteral("PSD 未预先合成预览；请在 Photoshop 中打开并重新保存，由 Photoshop 重建合成预览。"));
        const auto version = document.origin.sourceVersion.isEmpty() ? m_catalog->sourceVersion : document.origin.sourceVersion;
        if (!version.isEmpty()) detail.append(QStringLiteral("工程版本：%1").arg(version));
        if (!document.origin.revisionId.isEmpty()) detail.append(versionDescription(document.origin));
        const auto documentNotes = notes(document.Reconstructed()->diagnostics); if (!documentNotes.isEmpty()) detail.append(documentNotes);
    }
    detail.append(QStringLiteral("来源路径：%1").arg(originPath(*m_catalog, document)));
    detail.append(QStringLiteral("来源文档：%1").arg(documentName(document)));
    const auto* layer = static_cast<SourceLayers*>(m_layers->model())->Layer(m_layers->currentIndex());
    if (layer) {
        detail.append(QStringLiteral("\n所选图层：%1\n范围：%2, %3 · %4 × %5 像素\n混合模式：%6\n透明度：%7%")
            .arg(layer->name).arg(layer->bounds.x()).arg(layer->bounds.y()).arg(layer->bounds.width()).arg(layer->bounds.height())
            .arg(blendName(layer->blendMode)).arg(layer->opacity * 100.0 / 255, 0, 'f', 1));
    }
    const auto catalogNotes = notes(m_catalog->diagnostics); if (!catalogNotes.isEmpty()) detail.append(catalogNotes);
    const auto entryNotes = notes(document.diagnostics); if (!entryNotes.isEmpty()) detail.append(entryNotes);
    if (!m_error.isEmpty()) detail.prepend(QStringLiteral("未能完成操作：%1").arg(m_error));
    m_details->setPlainText(detail.join(QStringLiteral("\n\n")));
}

void CPsdRecoveryDialog::RefreshControls()
{
    const QPointer<CPsdRecoveryDialog> alive(this);
    m_save->setEnabled(!m_exporting && m_validDocument && bool(m_export)); if (!alive) return;
    m_documents->setEnabled(!m_exporting); if (!alive) return;
    m_close->setText(m_exporting ? QStringLiteral("取消导出并关闭") : QStringLiteral("关闭"));
}

void CPsdRecoveryDialog::ShowError(const QString& error)
{
    m_error = error;
    m_status->setText(error.isEmpty() ? QString() : QStringLiteral("未能完成操作，详细原因见上方说明。"));
    SelectLayer();
}

QSet<QString> CPsdRecoveryDialog::CaptureProtectedPaths(const QStringList& sources)
{
    QSet<QString> paths;
    for (const auto& source : sources) {
        if (source.isEmpty()) continue;
        paths.insert(QDir::cleanPath(QFileInfo(source).absoluteFilePath()));
        const auto canonical = canonicalDestination(source); if (!canonical.isEmpty()) paths.insert(canonical);
    }
    return paths;
}

bool CPsdRecoveryDialog::ValidateProtectedOutputPath(const QSet<QString>& protectedPaths, const QString& output, QString* error)
{
    const auto fail = [&](const QString& reason) { if (error) *error = reason; return false; };
    if (error) error->clear();
    if (output.isEmpty() || output.contains(QChar::Null)) return fail(QStringLiteral("请选择有效的 PSD 保存路径"));
    if (QFileInfo(output).isSymLink()) return fail(QStringLiteral("保存目标是符号链接，请选择普通文件路径，以免覆盖来源文件"));
    if (QFileInfo(output).isDir()) return fail(QStringLiteral("PSD 保存路径不能是文件夹"));
    const auto destination = canonicalDestination(output);
    if (destination.isEmpty()) return fail(QStringLiteral("PSD 保存目录不存在或无法解析"));
    const auto absolute = QDir::cleanPath(QFileInfo(output).absoluteFilePath());
    if (protectedPaths.contains(absolute) || protectedPaths.contains(destination))
        return fail(QStringLiteral("不能覆盖来源、模型、纹理或工作会话文件，请另选 PSD 保存路径"));
    return true;
}

bool CPsdRecoveryDialog::ValidateOutputPath(const QString& source, const QString& output, QString* error)
{
    return ValidateProtectedOutputPath(CaptureProtectedPaths({source}), output, error);
}

void CPsdRecoveryDialog::ChooseOutput()
{
    if (m_exporting || !m_validDocument || !m_catalog) return;
    const int index = SelectedDocumentIndex();
    QString name = documentName(m_catalog->documents[index]);
    name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    if (name.isEmpty()) name = QStringLiteral("恢复素材");
    if (!name.endsWith(QStringLiteral(".psd"), Qt::CaseInsensitive)) name += QStringLiteral(".psd");
    const QPointer<CPsdRecoveryDialog> alive(this);
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存恢复的 PSD"),
        QDir(QFileInfo(originPath(*m_catalog, m_catalog->documents[index])).absolutePath()).filePath(name), QStringLiteral("Photoshop 文档 (*.psd)"));
    if (alive && !path.isEmpty()) ExportTo(path);
}

bool CPsdRecoveryDialog::ExportTo(const QString& path)
{
    if (m_exporting || !m_validDocument || !m_export || !m_catalog) return false;
    if (path.isEmpty() || path.size() > 32768 || path.contains(QChar::Null)) {
        ShowError(QStringLiteral("PSD 保存路径为空、过长或含无效字符")); return false;
    }
    const QString frozenPath(path.constData(), path.size());
    QString error;
    if (!ValidateProtectedOutputPath(m_protectedPaths, frozenPath, &error)) { ShowError(error); return false; }
    const QPointer<CPsdRecoveryDialog> alive(this);
    const int index = SelectedDocumentIndex(); const auto handler = m_export;
    m_exporting = true; m_exportIndex = index; m_closePending = false;
    ShowError({}); if (!alive) return false;
    RefreshControls(); if (!alive) return false;
    bool success = false;
    try { success = handler(index, frozenPath, &error); }
    catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
    catch (...) { error = QStringLiteral("导出发生未知异常"); }
    if (!alive) return false;
    m_exporting = false;
    if (success) {
        m_error.clear(); m_status->setText(QStringLiteral("所选文档已保存，可选择另一份来源文档继续导出。"));
        SelectLayer(); if (!alive) return success;
        m_details->appendPlainText(QStringLiteral("\n已保存：%1%2").arg(frozenPath, error.isEmpty() ? QString() : QLatin1Char('\n') + error));
    }
    else ShowError(error.isEmpty() ? QStringLiteral("导出未完成，可调整保存路径后重试") : error);
    if (!alive) return false;
    RefreshControls(); if (!alive) return false;
    if (success) emit Exported(index, frozenPath);
    if (!alive) return success;
    if (m_closePending) QDialog::done(QDialog::Rejected);
    return success;
}

void CPsdRecoveryDialog::done(int result)
{
    if (m_exporting) { m_closePending = true; emit CancelRequested(); return; }
    QDialog::done(result);
}
