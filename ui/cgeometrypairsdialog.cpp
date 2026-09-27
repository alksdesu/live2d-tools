#include "cgeometrypairsdialog.h"
#include "ctableeditdelegate.h"
#include "theme.h"
#include "para/ccidjson.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/ccanvasgeometry.h"
#include "live2dstruct/csectiontable.h"
#include <QAbstractTableModel>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStyle>
#include <QTableView>
#include <QTabWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

namespace {
using Pairs = live2dstruct::CGeometryObjectPairs;
class WrappingLayout final : public QLayout
{
public:
    ~WrappingLayout() override { while (auto* item = takeAt(0)) delete item; }
    void addItem(QLayoutItem* item) override { m_items.append(item); invalidate(); }
    int count() const override { return m_items.size(); }
    QLayoutItem* itemAt(int index) const override { return index >= 0 && index < m_items.size() ? m_items[index] : nullptr; }
    QLayoutItem* takeAt(int index) override { return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr; }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override {
        QSize result; for (const auto* item : m_items) if (!item->isEmpty()) result = result.expandedTo(item->minimumSize());
        const auto margin = contentsMargins(); return result + QSize(margin.left() + margin.right(), margin.top() + margin.bottom());
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return Arrange(QRect(0, 0, width, 0), false); }
    void setGeometry(const QRect& rect) override { QLayout::setGeometry(rect); Arrange(rect, true); }
    Qt::Orientations expandingDirections() const override { return {}; }
private:
    QVector<QLayoutItem*> m_items;
    int Arrange(const QRect& rect, bool apply) const {
        const auto margins = contentsMargins(); const auto area = rect.marginsRemoved(margins);
        auto* widget = parentWidget();
        int gap = spacing() >= 0 ? spacing() : widget ? widget->style()->pixelMetric(QStyle::PM_LayoutHorizontalSpacing) : 0;
        if (gap < 0) gap = widget ? widget->fontMetrics().horizontalAdvance(QLatin1Char(' ')) : 0;
        int x = area.x(), y = area.y(), line = 0;
        for (auto* item : m_items) {
            if (item->isEmpty()) continue;
            auto size = item->sizeHint(); size.setWidth(qMin(size.width(), qMax(0, area.width())));
            if (x > area.x() && x + size.width() > area.x() + area.width()) { x = area.x(); y += line + gap; line = 0; }
            if (apply) item->setGeometry(QRect(QPoint(x, y), size));
            x += size.width() + gap; line = qMax(line, size.height());
        }
        return y + line - rect.y() + margins.bottom();
    }
};
struct Mapping {
    int count = 0, targetCount = 0;
    QVector<int> values;
    QHash<int, QString> edits;
    QVector<float> sourceValues, targetValues;
};
class MappingModel final : public QAbstractTableModel
{
public:
    using QAbstractTableModel::QAbstractTableModel;
    std::shared_ptr<Mapping> mapping;
    QStringList headers;
    void Set(std::shared_ptr<Mapping> value) { beginResetModel(); mapping = std::move(value); endResetModel(); }
    int rowCount(const QModelIndex& parent = {}) const override { return !parent.isValid() && mapping ? mapping->count : 0; }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 2; }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || !mapping || index.row() >= mapping->count || index.column() > 1) return {};
        if (role != Qt::DisplayRole && role != Qt::EditRole) return {};
        if (index.column() == 0) return QString::number(index.row() + 1) + (index.row() < mapping->sourceValues.size()
            ? QStringLiteral(" · %1").arg(mapping->sourceValues[index.row()], 0, 'g', 9) : QString());
        const auto edited = mapping->edits.constFind(index.row());
        if (edited != mapping->edits.cend()) return *edited;
        if (index.row() >= mapping->values.size() || mapping->values[index.row()] < 0) return QString();
        const int target = mapping->values[index.row()];
        return QString::number(target + 1) + (role == Qt::DisplayRole && target < mapping->targetValues.size()
            ? QStringLiteral(" · %1").arg(mapping->targetValues[target], 0, 'g', 9) : QString());
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        return orientation == Qt::Horizontal && role == Qt::DisplayRole && section < headers.size() ? headers[section] : QVariant();
    }
    Qt::ItemFlags flags(const QModelIndex& index) const override {
        return QAbstractTableModel::flags(index) | (index.column() == 1 ? Qt::ItemIsEditable : Qt::NoItemFlags);
    }
    bool setData(const QModelIndex& index, const QVariant& value, int role) override {
        if (role != Qt::EditRole || !index.isValid() || !mapping || index.column() != 1 || index.row() >= mapping->count) return false;
        mapping->edits.insert(index.row(), value.toString()); emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole}); return true;
    }
};
QVector<int> sequence(int count, bool reverse = false)
{
    QVector<int> result; result.reserve(count);
    for (int index = 0; index < count; ++index) result.append(reverse ? count - index - 1 : index);
    return result;
}
bool readMapping(const Mapping& mapping, QVector<int>& result, QString* error)
{
    QVector<int> candidate; candidate.reserve(mapping.count);
    for (int row = 0; row < mapping.count; ++row) {
        int value = row < mapping.values.size() ? mapping.values[row] : -1;
        if (mapping.edits.contains(row)) {
            bool ok = false; const auto number = mapping.edits.value(row).trimmed().toLongLong(&ok);
            if (!ok || number < 1 || number > mapping.targetCount) {
                if (error) *error = QStringLiteral("第 %1 行需要填写 1 到 %2 之间的目标序号。").arg(row + 1).arg(mapping.targetCount);
                return false;
            }
            value = int(number - 1);
        }
        if (value < 0 || value >= mapping.targetCount) {
            if (error) *error = QStringLiteral("第 %1 行还没有有效对应。请填写目标序号或使用填表按钮。").arg(row + 1);
            return false;
        }
        candidate.append(value);
    }
    result = std::move(candidate); return true;
}
class PairPreview final : public QWidget
{
public:
    using QWidget::QWidget;
    CGeometryPairsDialog::Preview preview;
    QVector<int> mapping;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = palette().color(QPalette::Window).lightness() < palette().color(QPalette::WindowText).lightness();
        const auto token = Theme::tokens(dark);
        painter.fillRect(rect(), QColor(token.value("@surface@")));
        const qreal margin = fontMetrics().lineSpacing();
        QRectF bounds; QPointF minimum, maximum; bool first = true;
        for (const auto* points : {&preview.firstPoints, &preview.secondPoints, &preview.proposedSecondPoints}) for (const auto point : *points) {
            if (first) { minimum = maximum = point; first = false; }
            else { minimum.setX(qMin(minimum.x(), point.x())); minimum.setY(qMin(minimum.y(), point.y()));
                   maximum.setX(qMax(maximum.x(), point.x())); maximum.setY(qMax(maximum.y(), point.y())); }
        }
        bounds = QRectF(minimum, maximum);
        if (first) {
            painter.setPen(QColor(token.value("@textMuted@")));
            painter.drawText(rect().adjusted(int(margin), int(margin), -int(margin), -int(margin)), Qt::AlignCenter | Qt::TextWordWrap,
                             QStringLiteral("确认对象与对应后，查看当前关键形的轮廓和匹配连线。")); return;
        }
        const qreal span = qMax(bounds.width(), bounds.height());
        const qreal width = qMax(bounds.width(), span * .05), height = qMax(bounds.height(), span * .05);
        const qreal scale = qMin((this->width() - margin * 2) / qMax(width, qreal(1)), (this->height() - margin * 2) / qMax(height, qreal(1)));
        const auto project = [&](QPointF point) { return QPointF(this->width() / 2., this->height() / 2.) + (point - bounds.center()) * scale; };
        const auto outline = [&](const QVector<QPointF>& points, const QVector<quint16>& triangles, const QVector<QPair<int, int>>& edges,
                                 const QColor& color, Qt::PenStyle style) {
            QPen pen(color); pen.setWidthF(qMax(qreal(1), fontMetrics().height() / 14.)); pen.setStyle(style); painter.setPen(pen);
            if (!edges.isEmpty()) {
                for (const auto& edge : edges) painter.drawLine(project(points[edge.first]), project(points[edge.second]));
            } else if (triangles.isEmpty()) {
                QPainterPath path;
                for (int index = 0; index < points.size(); ++index) { if (index == 0) path.moveTo(project(points[index])); else path.lineTo(project(points[index])); }
                painter.drawPath(path);
            } else for (int index = 0; index + 2 < triangles.size(); index += 3) {
                QPolygonF polygon;
                for (int corner = 0; corner < 3; ++corner) polygon.append(project(points[triangles[index + corner]]));
                painter.drawPolygon(polygon);
            }
        };
        const auto& target = preview.proposedSecondPoints.isEmpty() ? preview.secondPoints : preview.proposedSecondPoints;
        QPen links(QColor(token.value("@borderStrong@"))); links.setStyle(Qt::DotLine); painter.setPen(links);
        const int stride = qMax(1, mapping.size() / qMax(1, this->width() / fontMetrics().averageCharWidth()));
        for (int index = 0; index < mapping.size() && index < preview.firstPoints.size(); index += stride)
            if (mapping[index] >= 0 && mapping[index] < target.size()) painter.drawLine(project(preview.firstPoints[index]), project(target[mapping[index]]));
        outline(preview.firstPoints, preview.firstTriangles, preview.firstEdges, QColor(token.value("@text@")), Qt::SolidLine);
        outline(preview.secondPoints, preview.secondTriangles, preview.secondEdges, QColor(token.value("@textMuted@")), Qt::DashLine);
        if (!preview.proposedSecondPoints.isEmpty()) outline(preview.proposedSecondPoints, preview.secondTriangles, preview.secondEdges, QColor(token.value("@accentInk@")), Qt::SolidLine);
    }
};
bool validPreview(const CGeometryPairsDialog::Preview& preview)
{
    for (const auto* points : {&preview.firstPoints, &preview.secondPoints, &preview.proposedSecondPoints}) {
        if (points->size() > Pairs::MaximumVertices) return false;
        for (const auto point : *points) if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
            std::abs(point.x()) > std::numeric_limits<float>::max() || std::abs(point.y()) > std::numeric_limits<float>::max()) return false;
    }
    if (!preview.proposedSecondPoints.isEmpty() && preview.proposedSecondPoints.size() != preview.secondPoints.size()) return false;
    for (const auto target : {false, true}) {
        const auto& indices = target ? preview.secondTriangles : preview.firstTriangles;
        const auto& points = target ? preview.secondPoints : preview.firstPoints;
        const auto& edges = target ? preview.secondEdges : preview.firstEdges;
        if (indices.size() % 3 || indices.size() > Pairs::MaximumMappings) return false;
        for (const auto index : indices) if (index >= points.size()) return false;
        if (edges.size() > Pairs::MaximumMappings) return false;
        for (const auto& edge : edges) if (edge.first < 0 || edge.second < 0 || edge.first >= points.size() || edge.second >= points.size()) return false;
    }
    return true;
}
Pairs::Object selected(QComboBox* combo)
{
    return {Pairs::Type(combo->currentData(Qt::UserRole + 1).toInt()), combo->currentData().toString()};
}
void selectObject(QComboBox* combo, const Pairs::Object& object)
{
    for (int row = 0; row < combo->count(); ++row)
        if (combo->itemData(row).toString() == object.id && combo->itemData(row, Qt::UserRole + 1).toInt() == int(object.type)) {
            combo->setCurrentIndex(row); return;
        }
    combo->setCurrentIndex(-1);
}
void wrap(QLabel* label)
{
    label->setTextFormat(Qt::PlainText); label->setWordWrap(true);
    QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Preferred); policy.setHeightForWidth(true); label->setSizePolicy(policy);
}
}

struct CGeometryPairsDialog::State
{
    explicit State(const live2dstruct::CModelDocument& source) : document(source), result(source.GeometryObjectPairs()) {}
    live2dstruct::CModelDocument document;
    std::unique_ptr<live2dstruct::CCanvas_info> canvas;
    Pairs::State result;
    Pairs::Pair draft;
    QVector<Pairs::Object> selection;
    int row = -1, axis = -1;
    int loadedLine = -1;
    double loadedPosition = 0;
    bool loading = false, dirty = false, submitting = false, rotationConfirmed = false;
    SubmissionHandler submission;
    PairInitializer initializer;
    PreviewProvider provider;
    std::shared_ptr<Mapping> vertices;
    QVector<std::shared_ptr<Mapping>> keys;
    QVector<int> targetAxes;
    QWidget* content = nullptr;
    QComboBox *pairs = nullptr, *first = nullptr, *second = nullptr, *line = nullptr, *sourceAxis = nullptr, *targetAxis = nullptr;
    QCheckBox* enabled = nullptr;
    QTabWidget* tabs = nullptr;
    QDoubleSpinBox* position = nullptr;
    QSpinBox* keyform = nullptr;
    QLabel *status = nullptr, *error = nullptr, *corners = nullptr;
    QLabel* noAxes = nullptr;
    QPushButton* discard = nullptr;
    QPushButton* readObjects = nullptr;
    QPushButton* matchRotation = nullptr;
    QVector<QPushButton*> warpActions;
    MappingModel *vertexModel = nullptr, *keyModel = nullptr;
    CTableEditDelegate *vertexDelegate = nullptr, *keyDelegate = nullptr;
    PairPreview* preview = nullptr;
    QDialogButtonBox* buttons = nullptr;
};

CGeometryPairsDialog::CGeometryPairsDialog(const live2dstruct::CModelDocument& document, const CCidJson*,
                                           const QVector<Pairs::Object>& selection, QWidget* parent)
    : QDialog(parent), m_state(std::make_shared<State>(document))
{
    auto& s = *m_state; s.selection = selection;
    auto reader = live2dstruct::CReader::FromBytes(document.WorkingBaseBytes()); live2dstruct::CSectionTable table(reader);
    if (table.IsValid() && live2dstruct::CCanvasGeometry::Validate(table.canvas_info)) s.canvas = std::make_unique<live2dstruct::CCanvas_info>(*table.canvas_info);
    setObjectName("geometryPairsDialog"); setWindowTitle(QStringLiteral("对象配对与对称"));
    const auto metrics = fontMetrics(); resize(metrics.horizontalAdvance(QString(78, QLatin1Char('0'))), metrics.lineSpacing() * 35);
    auto* layout = new QVBoxLayout(this);
    auto* scroll = new QScrollArea(this); scroll->setObjectName("geometryPairsScroll"); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    s.content = new QWidget(scroll); auto* content = new QVBoxLayout(s.content); content->setContentsMargins(0, 0, 0, 0);
    scroll->setWidget(s.content); layout->addWidget(scroll, 1);
    const auto combo = [&](const QString& name) {
        auto* result = new QComboBox(s.content); result->setObjectName(name); result->setMinimumContentsLength(1);
        result->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); result->setMinimumWidth(0);
        result->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* view = new QListView(result); view->setTextElideMode(Qt::ElideNone); view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded); result->setView(view);
        connect(result, &QComboBox::currentTextChanged, result, [result](const QString& text) { result->setToolTip(QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped())); }); return result;
    };
    const auto button = [&](const QString& label, const QString& name, QLayout* row, auto action) {
        auto* result = new QPushButton(label, s.content); result->setObjectName(name); result->setAutoDefault(false);
        row->addWidget(result); connect(result, &QPushButton::clicked, this, action); return result;
    };
    auto* pairRow = new QHBoxLayout; s.pairs = combo("geometryPairList"); pairRow->addWidget(s.pairs, 1);
    button(QStringLiteral("新建"), "geometryPairNew", pairRow, [this] { NewPair(); });
    button(QStringLiteral("移除"), "geometryPairRemove", pairRow, [this] { RemovePair(); }); content->addLayout(pairRow);
    auto* form = new QFormLayout; form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    s.first = combo("geometryPairFirst"); s.second = combo("geometryPairSecond");
    const auto addObject = [&](Pairs::Type type, const QString& id) {
        QString name = type != Pairs::Type::ArtMesh ? document.DeformerDisplayName(id) : QString();
        const auto typeName = type == Pairs::Type::ArtMesh ? QStringLiteral("ArtMesh") : type == Pairs::Type::Warp ? QStringLiteral("Warp") : QStringLiteral("Rotation");
        const auto label = (name.isEmpty() || name == id ? id : name + QStringLiteral(" · ") + id) + QStringLiteral(" (%1)").arg(typeName);
        for (auto* target : {s.first, s.second}) { target->addItem(label, id); target->setItemData(target->count() - 1, int(type), Qt::UserRole + 1); }
    };
    for (const auto& mesh : document.Drawables()) addObject(Pairs::Type::ArtMesh, mesh.id);
    for (const auto& node : document.Deformers()) addObject(node.type == live2dstruct::CModelDeformer::Warp ? Pairs::Type::Warp : Pairs::Type::Rotation, node.id);
    form->addRow(QStringLiteral("源对象"), s.first); form->addRow(QStringLiteral("目标对象"), s.second);
    s.enabled = new QCheckBox(QStringLiteral("同步编辑这组配对"), s.content); s.enabled->setObjectName("geometryPairEnabled"); form->addRow(s.enabled);
    s.line = combo("geometryPairLine"); s.line->addItems({QStringLiteral("垂直对称轴 · X"), QStringLiteral("水平对称轴 · Y"), QStringLiteral("保留原有方向")});
    s.position = new QDoubleSpinBox(s.content); s.position->setObjectName("geometryPairPosition"); s.position->setDecimals(4);
    s.position->setRange(-std::numeric_limits<float>::max(), std::numeric_limits<float>::max()); s.position->setSuffix(QStringLiteral(" 像素"));
    s.position->setMinimumWidth(0); s.position->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    form->addRow(QStringLiteral("对称轴"), s.line); form->addRow(QStringLiteral("画布位置"), s.position);
    s.keyform = new QSpinBox(s.content); s.keyform->setObjectName("geometryPairKeyform"); s.keyform->setMinimum(1);
    form->addRow(QStringLiteral("源参考关键形"), s.keyform); content->addLayout(form);
    auto* objectActions = new WrappingLayout;
    s.readObjects = button(QStringLiteral("读取对象结构"), "geometryPairReadObjects", objectActions, [this] { ReadObjects(); });
    button(QStringLiteral("确认此配对"), "geometryPairConfirm", objectActions, [this] { ConfirmPair(); });
    s.discard = button(QStringLiteral("放弃当前修改"), "geometryPairDiscard", objectActions, [this] { LoadPair(m_state->row); }); content->addLayout(objectActions);
    auto* rotationActions = new QHBoxLayout;
    s.matchRotation = button(QStringLiteral("匹配当前朝向"), "geometryPairMatchRotation", rotationActions, [this] { MatchRotation(); });
    rotationActions->addStretch(); content->addLayout(rotationActions);
    s.status = new QLabel(s.content); s.status->setObjectName("geometryPairStatus"); wrap(s.status); content->addWidget(s.status);
    auto* meaning = new QLabel(QStringLiteral("保存只记录对应。使用“镜像当前形”复制形状；勾选同步后，编辑一侧会同时修改对侧。"), s.content);
    wrap(meaning); content->addWidget(meaning);
    auto* tabs = new QTabWidget(s.content); s.tabs = tabs; tabs->setObjectName("geometryPairTabs"); Theme::setDocumentMode(*tabs); content->addWidget(tabs);
    const auto makeTable = [&](QWidget* parent, const QString& name, MappingModel*& model, CTableEditDelegate*& delegate) {
        auto* result = new QTableView(parent); result->setObjectName(name); result->setMinimumWidth(0);
        result->setMinimumHeight(metrics.lineSpacing() * 7); result->setAlternatingRowColors(true); result->setShowGrid(false);
        model = new MappingModel(result); model->headers = {QStringLiteral("源序号"), QStringLiteral("目标序号")}; result->setModel(model);
        delegate = new CTableEditDelegate(result); result->setItemDelegate(delegate); result->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        result->verticalHeader()->hide(); connect(model, &QAbstractItemModel::dataChanged, this, [this] { MarkChanged(); }); return result;
    };
    auto* vertices = new QWidget(tabs); auto* vertexLayout = new QVBoxLayout(vertices);
    auto* vertexActions = new WrappingLayout;
    button(QStringLiteral("按序号"), "geometryVerticesIdentity", vertexActions, [this] { FillVertices(0); });
    button(QStringLiteral("序号反序"), "geometryVerticesReverse", vertexActions, [this] { FillVertices(1); });
    s.warpActions.append(button(QStringLiteral("左右翻列"), "geometryVerticesColumns", vertexActions, [this] { FillVertices(2); }));
    s.warpActions.append(button(QStringLiteral("上下翻行"), "geometryVerticesRows", vertexActions, [this] { FillVertices(3); }));
    s.warpActions.append(button(QStringLiteral("行列互换"), "geometryVerticesTranspose", vertexActions, [this] { FillVertices(4); })); vertexLayout->addLayout(vertexActions);
    vertexLayout->addWidget(makeTable(vertices, "geometryVertexMap", s.vertexModel, s.vertexDelegate));
    auto* vertexHint = new QLabel(QStringLiteral("序号从 1 开始。填表只建立草稿；确认时会检查全部顶点、网格连接和绕序。"), vertices);
    wrap(vertexHint); vertexLayout->addWidget(vertexHint);
    tabs->addTab(vertices, QStringLiteral("顶点对应"));
    auto* axes = new QWidget(tabs); auto* axisLayout = new QVBoxLayout(axes); auto* allAxes = new QHBoxLayout;
    button(QStringLiteral("所有参数轴按顺序对应"), "geometryAxesIdentity", allAxes, [this] { FillAxes(); }); axisLayout->addLayout(allAxes);
    auto* axisForm = new QFormLayout; axisForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); axisForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    s.sourceAxis = combo("geometrySourceAxis"); s.targetAxis = combo("geometryTargetAxis");
    axisForm->addRow(QStringLiteral("源参数轴"), s.sourceAxis); axisForm->addRow(QStringLiteral("目标参数轴"), s.targetAxis); axisLayout->addLayout(axisForm);
    auto* keyActions = new QHBoxLayout;
    button(QStringLiteral("关键值同序"), "geometryKeysIdentity", keyActions, [this] { FillKeys(false); });
    button(QStringLiteral("关键值反序"), "geometryKeysReverse", keyActions, [this] { FillKeys(true); }); axisLayout->addLayout(keyActions);
    axisLayout->addWidget(makeTable(axes, "geometryKeyMap", s.keyModel, s.keyDelegate));
    s.keyModel->headers = {QStringLiteral("源序号 / 关键值"), QStringLiteral("目标序号 / 关键值")}; tabs->addTab(axes, QStringLiteral("参数与关键值"));
    s.noAxes = new QLabel(s.content); s.noAxes->setObjectName("geometryPairNoAxes"); wrap(s.noAxes); content->addWidget(s.noAxes);
    auto* previewTitle = new QLabel(QStringLiteral("镜像结果预览"), s.content); wrap(previewTitle);
    auto titleFont = previewTitle->font(); titleFont.setWeight(QFont::DemiBold); previewTitle->setFont(titleFont); content->addWidget(previewTitle);
    auto* previewHint = new QLabel(QStringLiteral("使用打开设置时的参数姿态。保存配对不会改动模型形状。"), s.content); wrap(previewHint); content->addWidget(previewHint);
    s.preview = new PairPreview(s.content); s.preview->setObjectName("geometryPairPreview"); s.preview->setMinimumHeight(metrics.lineSpacing() * 10);
    s.preview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred); content->addWidget(s.preview);
    s.corners = new QLabel(s.content); s.corners->setObjectName("geometryPairCorners"); wrap(s.corners); content->addWidget(s.corners);
    auto* previewActions = new QHBoxLayout; button(QStringLiteral("更新轮廓预览"), "geometryPairRefreshPreview", previewActions, [this] { RefreshPreview(); }); content->addLayout(previewActions);
    s.error = new QLabel(this); s.error->setObjectName("geometryPairError"); wrap(s.error); layout->addWidget(s.error);
    s.buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this); layout->addWidget(s.buttons);
    auto* save = s.buttons->button(QDialogButtonBox::Save); save->setText(QStringLiteral("保存配对"));
    save->setObjectName(QStringLiteral("primaryBtn")); save->setDefault(true);
    save->style()->unpolish(save); save->style()->polish(save);
    s.buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(s.buttons, &QDialogButtonBox::accepted, this, &CGeometryPairsDialog::accept);
    connect(s.buttons, &QDialogButtonBox::rejected, this, &CGeometryPairsDialog::reject);
    connect(s.pairs, &QComboBox::currentIndexChanged, this, [this](int row) {
        auto state = m_state; if (state->loading || state->submitting) return;
        QString error; if (!CommitPair(&error)) { const QSignalBlocker blocked(state->pairs); state->pairs->setCurrentIndex(state->row); ShowError(error); return; }
        LoadPair(row);
    });
    for (auto* target : {s.first, s.second, s.line}) connect(target, &QComboBox::currentIndexChanged, this, [this] { MarkChanged(); });
    connect(s.first, &QComboBox::currentIndexChanged, this, [this] {
        auto& s = *m_state; auto* model = qobject_cast<QStandardItemModel*>(s.second->model()); if (!model) return;
        const auto first = selected(s.first);
        s.matchRotation->setVisible(first.type == Pairs::Type::Rotation);
        for (int row = 0; row < s.second->count(); ++row) model->item(row)->setEnabled(
            s.second->itemData(row, Qt::UserRole + 1).toInt() == int(first.type) && s.second->itemData(row).toString() != first.id);
    });
    connect(s.line, &QComboBox::currentIndexChanged, this, [this](int index) { m_state->position->setEnabled(index != 2); });
    connect(s.enabled, &QCheckBox::toggled, this, [this] { MarkChanged(); });
    connect(s.position, &QDoubleSpinBox::valueChanged, this, [this] { MarkChanged(); });
    connect(s.keyform, &QSpinBox::valueChanged, this, [this] { if (!m_state->loading) { m_state->preview->preview = {}; m_state->preview->update(); } });
    connect(s.sourceAxis, &QComboBox::currentIndexChanged, this, [this](int index) { if (!m_state->loading) SelectAxis(index); });
    connect(s.targetAxis, &QComboBox::currentIndexChanged, this, [this](int index) {
        auto& s = *m_state; if (s.loading || s.axis < 0 || s.axis >= s.keys.size()) return;
        s.targetAxes[s.axis] = index; s.keys[s.axis]->targetCount = index < 0 ? 0 : s.draft.second.axes[index].keys.size();
        s.keys[s.axis]->targetValues = index < 0 ? QVector<float>{} : s.draft.second.axes[index].keys;
        s.keyModel->Set(s.keys[s.axis]); MarkChanged();
    });
    int initial = s.result.pairs.isEmpty() ? -1 : 0;
    if (!selection.isEmpty() && selection.size() <= 2) {
        initial = -1;
        for (int row = 0; row < s.result.pairs.size(); ++row) {
            const auto& pair = s.result.pairs[row]; bool matches = true;
            for (const auto& object : selection) matches = matches && (object == pair.first.object || object == pair.second.object);
            if (matches) { initial = row; break; }
        }
    }
    RefreshList(); LoadPair(initial);
}

CGeometryPairsDialog::~CGeometryPairsDialog() = default;
const Pairs::State& CGeometryPairsDialog::DraftResult() const { return m_state->result; }
void CGeometryPairsDialog::SetSubmissionHandler(SubmissionHandler handler) { if (!m_state->submitting) m_state->submission = std::move(handler); }
void CGeometryPairsDialog::SetPairInitializer(PairInitializer initializer) { if (!m_state->submitting) m_state->initializer = std::move(initializer); }
void CGeometryPairsDialog::SetPreviewProvider(PreviewProvider provider) { if (!m_state->submitting) m_state->provider = std::move(provider); }
void CGeometryPairsDialog::ShowError(const QString& error) { m_state->error->setText(error); }
void CGeometryPairsDialog::RefreshList()
{
    auto& s = *m_state; const QSignalBlocker blocked(s.pairs); s.pairs->clear();
    for (const auto& pair : s.result.pairs) s.pairs->addItem(QStringLiteral("%1 ↔ %2%3").arg(pair.first.object.id, pair.second.object.id,
        pair.requiresReview ? QStringLiteral(" · 待确认") : !pair.enabled ? QStringLiteral(" · 同步已关闭") : QString()), pair.id);
    s.pairs->setCurrentIndex(s.row);
}
void CGeometryPairsDialog::MarkChanged()
{
    auto& s = *m_state; if (s.loading || s.submitting) return;
    s.dirty = true; s.discard->setEnabled(true); s.preview->preview = {}; s.preview->update(); s.corners->clear();
    RefreshMappingLayout();
    s.status->setText(QStringLiteral("当前配对有未保存的修改。保存或切换配对时会检查；无效输入会保留在表格中。"));
}
void CGeometryPairsDialog::LoadPair(int row)
{
    auto& s = *m_state; if (s.submitting) return; s.loading = true; s.row = row >= 0 && row < s.result.pairs.size() ? row : -1;
    s.draft = s.row < 0 ? Pairs::Pair{} : s.result.pairs[s.row];
    s.readObjects->setText(s.row < 0 ? QStringLiteral("读取对象结构") : QStringLiteral("重新读取并清空对应"));
    if (s.row < 0) s.draft.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (s.row >= 0) { selectObject(s.first, s.draft.first.object); selectObject(s.second, s.draft.second.object); }
    else { if (!s.selection.isEmpty()) selectObject(s.first, s.selection[0]); if (s.selection.size() > 1) selectObject(s.second, s.selection[1]); else s.second->setCurrentIndex(-1); }
    s.enabled->setChecked(s.draft.enabled); s.rotationConfirmed = s.row >= 0 && !s.draft.requiresReview;
    s.matchRotation->setVisible(selected(s.first).type == Pairs::Type::Rotation);
    for (auto* action : s.warpActions) action->setVisible(s.draft.first.object.type == Pairs::Type::Warp);
    s.line->setCurrentIndex(s.draft.lineDirection.x() == 0 ? 0 : s.draft.lineDirection.y() == 0 ? 1 : 2);
    if (auto* lineModel = qobject_cast<QStandardItemModel*>(s.line->model())) lineModel->item(2)->setEnabled(s.line->currentIndex() == 2);
    QPointF pixel; if (s.row >= 0 && s.canvas && live2dstruct::CCanvasGeometry::ModelToPixels(s.canvas.get(), s.draft.linePoint, pixel))
        s.position->setValue(s.line->currentIndex() == 1 ? pixel.y() : pixel.x());
    else s.position->setValue(s.canvas ? s.canvas->width / 2. : 0);
    s.loadedLine = s.line->currentIndex(); s.loadedPosition = s.position->value();
    s.keyform->setRange(1, qMax(1, s.draft.first.keyformCount)); s.keyform->setValue(1);
    s.vertices = std::make_shared<Mapping>(Mapping{s.draft.first.vertexCount, s.draft.second.vertexCount, s.draft.firstVertexToSecondVertex, {}});
    s.vertexModel->Set(s.vertices); s.keys.clear(); s.targetAxes.clear(); s.sourceAxis->clear(); s.targetAxis->clear();
    for (const auto& axis : s.draft.second.axes) s.targetAxis->addItem(QStringLiteral("%1 · %2").arg(axis.index + 1).arg(axis.parameterId));
    for (const auto& axis : s.draft.first.axes) {
        s.sourceAxis->addItem(QStringLiteral("%1 · %2").arg(axis.index + 1).arg(axis.parameterId));
        auto map = std::make_shared<Mapping>(); map->count = axis.keys.size(); map->sourceValues = axis.keys; int target = -1;
        for (const auto& stored : s.draft.axes) if (stored.firstAxis == axis.index) { target = stored.secondAxis; map->values = stored.firstKeyToSecondKey; }
        map->targetCount = target >= 0 && target < s.draft.second.axes.size() ? s.draft.second.axes[target].keys.size() : 0;
        if (target >= 0 && target < s.draft.second.axes.size()) map->targetValues = s.draft.second.axes[target].keys;
        s.targetAxes.append(target); s.keys.append(std::move(map));
    }
    s.loading = false; SelectAxis(s.keys.isEmpty() ? -1 : 0); s.dirty = false; s.discard->setEnabled(false);
    RefreshMappingLayout();
    s.status->setText(s.row < 0 ? QStringLiteral("选择两个同类对象并读取结构，再显式填写对应。名称只用于查找。") :
        s.draft.requiresReview ? QStringLiteral("结构已变化。请重新读取对象、修正对应，并确认此配对后继续使用。") :
        s.draft.first.object.type == Pairs::Type::Rotation ? QStringLiteral("已保存旋转朝向对应。参考关键形用于检查当前镜像结果。") : QStringLiteral("已保存完整对应；顶点与关键值序号均从 1 开始。"));
    { const QSignalBlocker blocked(s.pairs); s.pairs->setCurrentIndex(s.row); }
    s.preview->preview = {}; s.preview->update(); s.corners->clear(); ShowError({});
}
void CGeometryPairsDialog::RefreshMappingLayout()
{
    auto& s = *m_state;
    const bool ready = s.draft.first.keyformCount > 0 && selected(s.first) == s.draft.first.object && selected(s.second) == s.draft.second.object;
    const bool rotation = s.draft.first.object.type == Pairs::Type::Rotation;
    const bool vertices = ready && !rotation;
    const bool axes = ready && (!s.draft.first.axes.isEmpty() || !s.draft.second.axes.isEmpty());
    s.tabs->setTabVisible(0, vertices); s.tabs->setTabVisible(1, axes); s.tabs->setVisible(vertices || axes);
    if (rotation && axes) s.tabs->setCurrentIndex(1);
    else if (vertices && !s.tabs->isTabVisible(s.tabs->currentIndex())) s.tabs->setCurrentIndex(0);
    s.noAxes->setVisible(ready && !axes);
    s.noAxes->setText(rotation ? QStringLiteral("此配对没有参数轴，只需确认旋转朝向和对称轴。")
                               : QStringLiteral("此配对没有参数轴，只需设置顶点对应。"));
}
void CGeometryPairsDialog::SelectAxis(int index)
{
    auto& s = *m_state; s.keyDelegate->CommitEditor(); s.axis = index >= 0 && index < s.keys.size() ? index : -1;
    const QSignalBlocker blocked(s.targetAxis); s.targetAxis->setCurrentIndex(s.axis < 0 ? -1 : s.targetAxes[s.axis]);
    s.keyModel->Set(s.axis < 0 ? std::shared_ptr<Mapping>{} : s.keys[s.axis]);
}
bool CGeometryPairsDialog::ReadPair(Pairs::Pair& result, QString* error) const
{
    auto& s = *m_state;
    if (s.draft.first.keyformCount < 1 || selected(s.first) != s.draft.first.object || selected(s.second) != s.draft.second.object) {
        if (error) *error = QStringLiteral("对象选择尚未确认，或已经改变。请点击“读取对象结构”，再填写对应。"); return false;
    }
    auto pair = s.draft; pair.enabled = s.enabled->isChecked();
    if (!s.canvas) { if (error) *error = QStringLiteral("画布信息无效，无法设置像素对称轴。"); return false; }
    if (s.line->currentIndex() == 2 && (pair.lineDirection.x() == 0 || pair.lineDirection.y() == 0)) {
        if (error) *error = QStringLiteral("请选择水平或垂直对称轴。"); return false;
    }
    if (s.line->currentIndex() != 2 && (s.line->currentIndex() != s.loadedLine || s.position->value() != s.loadedPosition)) {
        const bool horizontal = s.line->currentIndex() == 1; QPointF origin, direction;
        const QPointF pixel = horizontal ? QPointF(0, s.position->value()) : QPointF(s.position->value(), 0);
        if (!live2dstruct::CCanvasGeometry::PixelsToModel(s.canvas.get(), pixel, origin) ||
            !live2dstruct::CCanvasGeometry::PixelsToModel(s.canvas.get(), pixel + (horizontal ? QPointF(1, 0) : QPointF(0, 1)), direction)) {
            if (error) *error = QStringLiteral("对称轴位置无法转换到模型坐标。"); return false;
        }
        pair.linePoint = origin; pair.lineDirection = direction - origin;
    }
    if (!readMapping(*s.vertices, pair.firstVertexToSecondVertex, error)) return false;
    pair.axes.clear();
    for (int axis = 0; axis < s.keys.size(); ++axis) {
        if (s.targetAxes[axis] < 0 || s.targetAxes[axis] >= pair.second.axes.size()) {
            if (error) *error = QStringLiteral("源参数轴 %1 尚未选择目标参数轴。").arg(axis + 1); return false;
        }
        Pairs::AxisMapping map{axis, s.targetAxes[axis], {}};
        if (!readMapping(*s.keys[axis], map.firstKeyToSecondKey, error)) return false;
        pair.axes.append(std::move(map));
    }
    Pairs::State normalized;
    if (!Pairs::Normalize({s.document.SourceDigest(), {pair}}, normalized, error)) {
        if (pair.first.object.type == Pairs::Type::Rotation) return false;
        pair.reversedWinding = !pair.reversedWinding;
        if (!Pairs::Normalize({s.document.SourceDigest(), {pair}}, normalized, error)) return false;
    }
    result = normalized.pairs.first(); return true;
}
bool CGeometryPairsDialog::CommitPair(QString* error)
{
    auto& s = *m_state; s.vertexDelegate->CommitEditor(); s.keyDelegate->CommitEditor();
    if (!s.dirty) return true;
    Pairs::Pair pair; if (!ReadPair(pair, error)) return false;
    if (pair.first.object.type == Pairs::Type::Rotation && !pair.requiresReview && !s.rotationConfirmed) {
        if (error) *error = QStringLiteral("请点击“匹配当前朝向”建立旋转朝向对应。"); return false;
    }
    auto candidate = s.result; candidate.sourceDigest = s.document.SourceDigest();
    if (s.row < 0) candidate.pairs.append(pair); else candidate.pairs[s.row] = pair;
    Pairs::State normalized;
    if (!Pairs::Normalize(candidate, normalized, error) || !Pairs::Validate(normalized, s.document, error)) return false;
    s.result = std::move(normalized); s.row = s.row < 0 ? s.result.pairs.size() - 1 : s.row; s.draft = s.result.pairs[s.row];
    s.dirty = false; s.discard->setEnabled(false); RefreshList(); return true;
}
void CGeometryPairsDialog::NewPair()
{
    if (m_state->submitting) return; QString error;
    if (!CommitPair(&error)) { ShowError(error); return; } LoadPair(-1);
}
void CGeometryPairsDialog::RemovePair()
{
    auto& s = *m_state; if (s.submitting) return;
    if (s.row >= 0) s.result.pairs.removeAt(s.row);
    const int next = qMin(s.row, int(s.result.pairs.size()) - 1); s.dirty = false; RefreshList(); LoadPair(next);
}
void CGeometryPairsDialog::ReadObjects()
{
    auto& s = *m_state; if (s.submitting) return; const auto first = selected(s.first), second = selected(s.second);
    if (first.id.isEmpty() || second.id.isEmpty() || first == second || first.type != second.type) {
        ShowError(QStringLiteral("请选择两个不同的同类对象。")); return;
    }
    auto pair = s.draft; QString error;
    if (!Pairs::DescribeEndpoint(s.document, first, pair.first, &error) || !Pairs::DescribeEndpoint(s.document, second, pair.second, &error)) { ShowError(error); return; }
    if (s.line->currentIndex() != 2 && s.canvas && (s.row < 0 || s.line->currentIndex() != s.loadedLine || s.position->value() != s.loadedPosition)) {
        const bool horizontal = s.line->currentIndex() == 1; const QPointF pixel = horizontal ? QPointF(0, s.position->value()) : QPointF(s.position->value(), 0);
        QPointF origin, direction;
        if (!live2dstruct::CCanvasGeometry::PixelsToModel(s.canvas.get(), pixel, origin) ||
            !live2dstruct::CCanvasGeometry::PixelsToModel(s.canvas.get(), pixel + (horizontal ? QPointF(1, 0) : QPointF(0, 1)), direction)) {
            ShowError(QStringLiteral("对称轴位置无法转换到模型坐标。")); return;
        }
        pair.linePoint = origin; pair.lineDirection = direction - origin;
    }
    pair.firstVertexToSecondVertex.clear(); pair.axes.clear(); pair.rotationLocalX = QPointF(1, 0); pair.rotationLocalY = QPointF(0, 1);
    const auto row = s.row; const auto previous = s.result; const int line = s.line->currentIndex(); const double position = s.position->value();
    const bool enabled = s.enabled->isChecked();
    s.result.pairs.append(pair); LoadPair(s.result.pairs.size() - 1); s.row = row; s.result = previous;
    s.loading = true; s.line->setCurrentIndex(line); s.position->setValue(position); s.enabled->setChecked(enabled); s.loading = false;
    s.rotationConfirmed = false; s.draft.requiresReview = row >= 0 && previous.pairs[row].requiresReview;
    RefreshList(); MarkChanged(); ShowError({});
}
void CGeometryPairsDialog::FillVertices(int mode)
{
    auto& s = *m_state; if (s.submitting || s.draft.first.keyformCount < 1) return; const auto& first = s.draft.first; const auto& second = s.draft.second;
    if (first.vertexCount != second.vertexCount) { ShowError(QStringLiteral("两侧顶点数量不同，无法建立完整的一一对应。")); return; }
    QVector<int> mapping;
    if (mode < 2) mapping = sequence(first.vertexCount, mode == 1);
    else {
        if (first.object.type != Pairs::Type::Warp || (mode != 4 && (first.rows != second.rows || first.columns != second.columns)) ||
            (mode == 4 && (first.rows != second.columns || first.columns != second.rows))) {
            ShowError(QStringLiteral("此操作需要行列尺寸对应的 Warp 网格。")); return;
        }
        for (int row = 0; row <= first.rows; ++row) for (int column = 0; column <= first.columns; ++column)
            mapping.append(mode == 2 ? row * (second.columns + 1) + first.columns - column :
                           mode == 3 ? (first.rows - row) * (second.columns + 1) + column : column * (second.columns + 1) + row);
    }
    s.vertexDelegate->CommitEditor(); s.vertices->values = std::move(mapping); s.vertices->edits.clear(); s.vertexModel->Set(s.vertices); MarkChanged(); ShowError({});
}
void CGeometryPairsDialog::FillAxes()
{
    auto& s = *m_state; if (s.submitting) return;
    if (s.draft.first.axes.size() != s.draft.second.axes.size()) { ShowError(QStringLiteral("两侧参数轴数量不同，无法按顺序一一对应。")); return; }
    s.keyDelegate->CommitEditor();
    for (int axis = 0; axis < s.keys.size(); ++axis) {
        s.targetAxes[axis] = axis; s.keys[axis]->targetCount = s.draft.second.axes[axis].keys.size();
        s.keys[axis]->targetValues = s.draft.second.axes[axis].keys;
        s.keys[axis]->values = sequence(s.keys[axis]->count); s.keys[axis]->edits.clear();
    }
    SelectAxis(s.axis); MarkChanged(); ShowError({});
}
void CGeometryPairsDialog::FillKeys(bool reversed)
{
    auto& s = *m_state; if (s.submitting || s.axis < 0) return;
    s.keyDelegate->CommitEditor(); auto& mapping = *s.keys[s.axis];
    if (mapping.targetCount != mapping.count) { ShowError(QStringLiteral("两侧关键值数量不同，请重新选择参数轴。")); return; }
    mapping.values = sequence(mapping.count, reversed); mapping.edits.clear(); s.keyModel->Set(s.keys[s.axis]); MarkChanged(); ShowError({});
}
void CGeometryPairsDialog::ConfirmPair()
{
    const auto state = m_state; if (state->submitting) return; state->vertexDelegate->CommitEditor(); state->keyDelegate->CommitEditor();
    Pairs::Pair pair; QString error;
    if (!ReadPair(pair, &error)) { ShowError(error); return; }
    if (pair.first.object.type == Pairs::Type::Rotation && !state->rotationConfirmed) {
        ShowError(QStringLiteral("请先点击“匹配当前朝向”建立旋转朝向对应。")); return;
    }
    pair.requiresReview = false; Pairs::State checked;
    if (!Pairs::Normalize({state->document.SourceDigest(), {pair}}, checked, &error) || !Pairs::Validate(checked, state->document, &error)) { ShowError(error); return; }
    state->draft = checked.pairs[0]; state->dirty = true;
    if (!CommitPair(&error)) { ShowError(error); return; }
    state->status->setText(QStringLiteral("配对已确认，尚未改动模型形状。点击“保存配对”后保留设置。")); ShowError({}); RefreshPreview();
}
void CGeometryPairsDialog::MatchRotation()
{
    const auto state = m_state; if (state->submitting) return; state->vertexDelegate->CommitEditor(); state->keyDelegate->CommitEditor();
    Pairs::Pair pair; QString error;
    if (!ReadPair(pair, &error)) { ShowError(error); return; }
    if (pair.first.object.type != Pairs::Type::Rotation) return;
    const QPointer<CGeometryPairsDialog> alive(this);
    {
        if (!state->initializer) { ShowError(QStringLiteral("旋转朝向检查尚未接入，暂时无法确认这组 Rotation 配对。")); return; }
        const auto before = pair; bool ok = false;
        state->submitting = true; state->content->setEnabled(false); state->buttons->setEnabled(false);
        const auto restore = qScopeGuard([state, alive] { state->submitting = false; if (alive) { state->content->setEnabled(true); state->buttons->setEnabled(true); } });
        try { ok = state->initializer(pair, state->keyform->value() - 1, &error); }
        catch (const std::exception&) { error = QStringLiteral("旋转朝向检查失败，当前草稿已保留。"); }
        if (!alive) return;
        auto allowed = before; allowed.rotationLocalX = pair.rotationLocalX; allowed.rotationLocalY = pair.rotationLocalY;
        if (!ok || pair != allowed) { ShowError(error.isEmpty() ? QStringLiteral("旋转朝向检查未通过，当前草稿已保留。") : error); return; }
    }
    Pairs::State checked;
    if (!Pairs::Normalize({state->document.SourceDigest(), {pair}}, checked, &error)) { ShowError(error); return; }
    state->draft = checked.pairs[0]; state->rotationConfirmed = true; MarkChanged();
    state->status->setText(QStringLiteral("已匹配当前参考形的旋转朝向。后续修改对称轴和预览会保留此对应。")); ShowError({});
}
void CGeometryPairsDialog::RefreshPreview()
{
    const auto state = m_state; if (state->submitting) return; state->vertexDelegate->CommitEditor(); state->keyDelegate->CommitEditor();
    Pairs::Pair pair; QString error;
    if (!ReadPair(pair, &error) || pair.requiresReview) { ShowError(error.isEmpty() ? QStringLiteral("请先重新确认这组配对。") : error); return; }
    if (!state->provider) { ShowError(QStringLiteral("轮廓预览尚未接入；对应关系仍可独立保存。")); return; }
    const QPointer<CGeometryPairsDialog> alive(this); Preview preview; bool ok = false;
    state->submitting = true; state->content->setEnabled(false); state->buttons->setEnabled(false);
    const auto restore = qScopeGuard([state, alive] { state->submitting = false; if (alive) { state->content->setEnabled(true); state->buttons->setEnabled(true); } });
    try { ok = state->provider(pair, state->keyform->value() - 1, preview, &error); }
    catch (const std::exception&) { error = QStringLiteral("轮廓预览失败，当前草稿已保留。"); }
    if (!alive) return;
    if (!ok || !validPreview(preview)) { ShowError(error.isEmpty() ? QStringLiteral("预览坐标或网格无效。") : error); return; }
    state->preview->preview = std::move(preview); state->preview->mapping = pair.firstVertexToSecondVertex; state->preview->update();
    state->corners->setText(QStringLiteral("源：%1\n目标：%2\n实线为源轮廓，虚线为目标原轮廓，强调色为镜像后的目标。").arg(state->preview->preview.firstCorner, state->preview->preview.secondCorner));
    ShowError({});
}
void CGeometryPairsDialog::accept()
{
    const auto state = m_state; if (state->submitting) return; QString error;
    if (!CommitPair(&error)) { ShowError(error); return; }
    Pairs::State candidate;
    if (!Pairs::Normalize(state->result, candidate, &error) || !Pairs::Validate(candidate, state->document, &error)) { ShowError(error); return; }
    const QPointer<CGeometryPairsDialog> alive(this); state->submitting = true;
    state->content->setEnabled(false); state->buttons->setEnabled(false);
    const auto restore = qScopeGuard([state, alive] { state->submitting = false; if (alive) { state->content->setEnabled(true); state->buttons->setEnabled(true); } });
    bool accepted = true;
    try { if (state->submission) accepted = state->submission(candidate, &error); }
    catch (const std::exception&) { accepted = false; error = QStringLiteral("保存配对失败，当前草稿已保留。"); }
    if (!alive) return;
    if (!accepted) { ShowError(error.isEmpty() ? QStringLiteral("配对未能保存，请检查模型是否已改变。") : error); return; }
    state->result = std::move(candidate); QDialog::accept();
}
void CGeometryPairsDialog::reject() { if (!m_state->submitting) QDialog::reject(); }
