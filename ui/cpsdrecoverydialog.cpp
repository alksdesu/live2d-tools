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

class SourceLayers final : public QAbstractItemModel
{
public:
    explicit SourceLayers(QObject* parent) : QAbstractItemModel(parent) {}
    bool SetDocument(const CPsdSourceDocument* document) {
        beginResetModel(); m_document = document; m_children.clear(); m_rows.clear();
        bool valid = document != nullptr;
        if (document) {
            m_children.resize(document->layers.size() + 1); m_rows.resize(document->layers.size());
            for (int index = 0; index < document->layers.size(); ++index) {
                const int parent = document->layers[index].parentIndex;
                if (parent < -1 || parent >= index ||
                    (parent >= 0 && document->layers[parent].kind != CPsdSourceLayer::Kind::Group)) { valid = false; break; }
                m_rows[index] = m_children[parent + 1].size(); m_children[parent + 1].append(index);
            }
        }
        if (!valid) { m_document = nullptr; m_children.clear(); m_rows.clear(); }
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
        if (layer->kind == CPsdSourceLayer::Kind::Group) state.append(QStringLiteral("文件夹"));
        else if (layer->png.isEmpty()) state.append(QStringLiteral("空图层"));
        if (!layer->visible) state.append(QStringLiteral("隐藏"));
        if (layer->clipping) state.append(QStringLiteral("剪切"));
        if (layer->opacity != 255) state.append(QStringLiteral("透明度 %1%").arg(layer->opacity * 100.0 / 255, 0, 'f', 1));
        return state.isEmpty() ? QStringLiteral("图像") : state.join(QStringLiteral(" · "));
    }
    const CPsdSourceLayer* Layer(const QModelIndex& index) const {
        if (!m_document || !index.isValid() || index.model() != this || !index.internalId() ||
            index.internalId() > quintptr(m_document->layers.size())) return nullptr;
        return &m_document->layers[int(index.internalId()) - 1];
    }
private:
    const CPsdSourceDocument* m_document = nullptr;
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
    setObjectName(QStringLiteral("psdRecoveryDialog")); setWindowTitle(QStringLiteral("从工程恢复 PSD"));
    setModal(true); setSizeGripEnabled(true);
    auto* layout = new QVBoxLayout(this);
    auto* description = new QLabel(QStringLiteral("选择工程中的一份来源文档。保留其图层树、隐藏与空图层；不同画布分别导出。"), this);
    description->setTextFormat(Qt::PlainText); description->setWordWrap(true); layout->addWidget(description);
    m_documents = new QComboBox(this); m_documents->setObjectName(QStringLiteral("psdRecoveryDocuments"));
    m_documents->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); m_documents->setMinimumContentsLength(12);
    if (m_catalog) for (int index = 0; index < m_catalog->documents.size(); ++index) {
        const auto& document = m_catalog->documents[index];
        m_documents->addItem(QStringLiteral("%1. %2").arg(index + 1).arg(document.name.isEmpty() ? QStringLiteral("未命名文档") : document.name), index);
    }
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
    const bool valid = static_cast<SourceLayers*>(m_layers->model())->SetDocument(document);
    if (!alive) return;
    m_validDocument = valid;
    if (!document) { m_summary->setText(QStringLiteral("工程中没有可恢复的来源文档")); m_details->clear(); RefreshControls(); return; }
    int raster = 0, groups = 0, empty = 0, hidden = 0;
    for (const auto& layer : document->layers) {
        if (layer.kind == CPsdSourceLayer::Kind::Group) ++groups;
        else { ++raster; if (layer.png.isEmpty()) ++empty; }
        if (!layer.visible) ++hidden;
    }
    for (const auto* diagnostics : {&m_catalog->diagnostics, &document->diagnostics})
        for (const auto& value : *diagnostics) if (value.severity == CPsdSourceDiagnostic::Severity::Error) m_validDocument = false;
    m_summary->setText(QStringLiteral("%1 × %2 像素 · %3 个图层（含 %4 个空图层）· %5 个文件夹 · %6 项隐藏")
        .arg(document->canvasSize.width()).arg(document->canvasSize.height()).arg(raster).arg(empty).arg(groups).arg(hidden));
    if (!m_validDocument) m_error = QStringLiteral("这份来源文档包含无效目录或无法恢复的内容，详情见下方说明");
    m_layers->expandToDepth(0); if (!alive) return;
    SelectLayer(); if (alive) RefreshControls();
}

void CPsdRecoveryDialog::SelectLayer()
{
    const int index = SelectedDocumentIndex();
    if (!m_catalog || index < 0 || index >= m_catalog->documents.size()) return;
    const auto& document = m_catalog->documents[index];
    QStringList detail{QStringLiteral("来源工程：%1").arg(m_catalog->sourcePath), QStringLiteral("来源文档：%1").arg(document.name),
        QStringLiteral("这是工程内保存的来源素材，不是当前模型姿态截图。空图层只保留身份，不代表已找回缺失像素。"),
        QStringLiteral("PSD 未预先合成预览；请在 Photoshop 中打开并重新保存，由 Photoshop 重建合成预览。")};
    if (!m_catalog->sourceVersion.isEmpty()) detail.append(QStringLiteral("工程版本：%1").arg(m_catalog->sourceVersion));
    const auto* layer = static_cast<SourceLayers*>(m_layers->model())->Layer(m_layers->currentIndex());
    if (layer) {
        detail.append(QStringLiteral("\n所选图层：%1\n范围：%2, %3 · %4 × %5 像素\n混合模式：%6\n透明度：%7%")
            .arg(layer->name).arg(layer->bounds.x()).arg(layer->bounds.y()).arg(layer->bounds.width()).arg(layer->bounds.height())
            .arg(blendName(layer->blendMode)).arg(layer->opacity * 100.0 / 255, 0, 'f', 1));
    }
    const auto catalogNotes = notes(m_catalog->diagnostics), documentNotes = notes(document.diagnostics);
    if (!catalogNotes.isEmpty()) detail.append(catalogNotes);
    if (!documentNotes.isEmpty()) detail.append(documentNotes);
    if (!m_error.isEmpty()) detail.append(QStringLiteral("\n未能完成操作：%1").arg(m_error));
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

bool CPsdRecoveryDialog::ValidateOutputPath(const QString& source, const QString& output, QString* error)
{
    const auto fail = [&](const QString& reason) { if (error) *error = reason; return false; };
    if (error) error->clear();
    if (output.isEmpty() || output.contains(QChar::Null)) return fail(QStringLiteral("请选择有效的 PSD 保存路径"));
    if (QFileInfo(output).isSymLink()) return fail(QStringLiteral("保存目标是符号链接，请选择普通文件路径，以免覆盖来源工程"));
    if (QFileInfo(output).isDir()) return fail(QStringLiteral("PSD 保存路径不能是文件夹"));
    const auto input = canonicalDestination(source), destination = canonicalDestination(output);
    if (destination.isEmpty()) return fail(QStringLiteral("PSD 保存目录不存在或无法解析"));
    if (QDir::cleanPath(QFileInfo(source).absoluteFilePath()) == QDir::cleanPath(QFileInfo(output).absoluteFilePath()) ||
        (!input.isEmpty() && input == destination)) return fail(QStringLiteral("不能覆盖来源工程，请另选 PSD 保存路径"));
    return true;
}

void CPsdRecoveryDialog::ChooseOutput()
{
    if (m_exporting || !m_validDocument || !m_catalog) return;
    const int index = SelectedDocumentIndex();
    QString name = m_catalog->documents[index].name;
    name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    if (name.isEmpty()) name = QStringLiteral("恢复素材");
    if (!name.endsWith(QStringLiteral(".psd"), Qt::CaseInsensitive)) name += QStringLiteral(".psd");
    const QPointer<CPsdRecoveryDialog> alive(this);
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存恢复的 PSD"),
        QDir(QFileInfo(m_catalog->sourcePath).absolutePath()).filePath(name), QStringLiteral("Photoshop 文档 (*.psd)"));
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
    if (!ValidateOutputPath(m_catalog->sourcePath, frozenPath, &error)) { ShowError(error); return false; }
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
