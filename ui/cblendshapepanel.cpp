#include "cblendshapepanel.h"
#include "ui/para/ccidjson.h"
#include "live2dstruct/cblendshaperelations.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/csectiontable.h"
#include <QCryptographicHash>
#include <QFile>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QKeyEvent>
#include <QLineEdit>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QTreeWidget>
#include <QTimer>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

using namespace live2dstruct;
using Relations = CBlendShapeRelations;

namespace {
constexpr int KindRole = Qt::UserRole + 1;
constexpr int TypeRole = Qt::UserRole + 2;
constexpr int IndexRole = Qt::UserRole + 3;
constexpr int LoadedRole = Qt::UserRole + 4;
constexpr int SelectableRole = Qt::UserRole + 5;

class CBlendRelationTree : public QTreeWidget
{
public:
    explicit CBlendRelationTree(QWidget* parent) : QTreeWidget(parent)
    {
        setEditTriggers(QAbstractItemView::NoEditTriggers);
    }

    int ContentHeight(int ceiling)
    {
        doItemsLayout();
        int height = (header()->isHidden() ? 0 : header()->height()) + frameWidth() * 2;
        if (horizontalScrollBar()->isVisible()) height += horizontalScrollBar()->height();
        QModelIndex first;
        for (int row = 0; row < model()->rowCount(rootIndex()); ++row) {
            if (isRowHidden(row, rootIndex())) continue;
            first = model()->index(row, 0, rootIndex());
            break;
        }
        for (auto index = first; index.isValid() && height < ceiling; index = indexBelow(index)) {
            const int row = std::max(1, rowHeight(index));
            height += std::min(row, ceiling - height);
        }
        return std::min(height, ceiling);
    }

protected:
    void drawBranches(QPainter* painter, const QRect& rect, const QModelIndex& index) const override
    {
        if (!index.isValid() || !model()->hasChildren(index) || index.flags().testFlag(Qt::ItemNeverHasChildren) ||
            (!rootIsDecorated() && index.parent() == rootIndex())) return;
        QStyleOption option;
        option.initFrom(this);
        option.state.setFlag(QStyle::State_Enabled, isEnabled() && index.flags().testFlag(Qt::ItemIsEnabled));
        option.rect = QRect(isRightToLeft() ? rect.left() : rect.right() - indentation() + 1,
                            rect.top(), indentation(), rect.height());
        const auto disclosure = style()->subElementRect(QStyle::SE_TreeViewDisclosureItem, &option, this);
        const int extent = std::min({disclosure.width(), disclosure.height(),
            style()->pixelMetric(QStyle::PM_SmallIconSize, &option, this)});
        if (extent <= 0) return;
        option.rect = QStyle::alignedRect(layoutDirection(), Qt::AlignCenter, QSize(extent, extent), disclosure);
        for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
            option.palette.setColor(group, QPalette::ButtonText, option.palette.color(group, QPalette::Text));
        const auto arrow = isExpanded(index) ? QStyle::PE_IndicatorArrowDown :
            (isRightToLeft() ? QStyle::PE_IndicatorArrowLeft : QStyle::PE_IndicatorArrowRight);
        painter->save();
        painter->setClipRect(rect, Qt::IntersectClip);
        style()->drawPrimitive(arrow, &option, painter, this);
        painter->restore();
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        const auto modified = event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier | Qt::ShiftModifier);
        // macOS 默认用回车编辑项目，此面板用回车定位。
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && !modified) {
            if (auto* item = currentItem(); item && (item->flags() & Qt::ItemIsEnabled))
                emit itemActivated(item, currentColumn());
            event->accept();
            return;
        }
        QTreeWidget::keyPressEvent(event);
    }
};

int sliderPosition(float value, float minimum, float maximum)
{
    if (minimum == maximum) return 0;
    const double ratio = (double(value) - minimum) / (double(maximum) - minimum);
    return qRound(qBound(0.0, ratio, 1.0) * 1000.0);
}

QString parameterText(float value)
{
    return QString::number(value, 'g', std::numeric_limits<float>::max_digits10);
}

QTreeWidgetItem* message(QTreeWidget* tree, const QString& text, const QString& detail = {})
{
    auto* item = new QTreeWidgetItem(tree, {text, detail});
    item->setData(0, KindRole, QStringLiteral("message"));
    return item;
}

QString targetType(Relations::TargetType type)
{
    return type == Relations::ArtMesh ? QStringLiteral("artmesh") : QStringLiteral("warp");
}
}

struct CBlendShapePanel::State
{
    struct Control { QTreeWidgetItem* item; QSlider* slider; QLabel* label; };
    struct Range { float minimum, maximum, defaultValue; };
    Relations relations;
    QByteArray digest;
    QSet<QString> blendIds;
    QHash<QString, QVector<Control>> controls;
    QHash<QString, Range> ranges;
    QHash<QString, float> recentValues;
    QHash<QString, QString> parameterNames, targetNames;
    QStringList extraDiagnostics;
    QString selectedParameter;
    QSplitter* splitter = nullptr;
    CBlendRelationTree* parameters = nullptr;
    QTreeWidget* details = nullptr;
    QLineEdit* search = nullptr;
    QLabel* summary = nullptr;
    std::unique_ptr<QWidget> wrapper;
};

CBlendShapePanel::CBlendShapePanel(QWidget* parent) : QDockWidget(QStringLiteral("融合变形"), parent)
{
    setObjectName(QStringLiteral("blendShapePanel"));
    m_state = CreateState();
    setWidget(m_state->wrapper.get());
    message(m_state->parameters, QStringLiteral("载入模型后查看融合参数"));
    ConnectState();
}

CBlendShapePanel::~CBlendShapePanel()
{
    m_rebuilding = true;
    m_state.reset();
}

std::unique_ptr<CBlendShapePanel::State> CBlendShapePanel::CreateState()
{
    auto result = std::make_unique<State>();
    result->wrapper = std::make_unique<QWidget>();
    auto* layout = new QVBoxLayout(result->wrapper.get());
    result->search = new QLineEdit(result->wrapper.get());
    result->search->setObjectName(QStringLiteral("blendParameterSearch"));
    result->search->setPlaceholderText(QStringLiteral("搜索融合或组合参数"));
    result->search->setClearButtonEnabled(true);
    result->summary = new QLabel(result->wrapper.get());
    result->summary->setObjectName(QStringLiteral("blendSummary"));
    result->summary->setWordWrap(true);
    auto* splitter = result->splitter = new QSplitter(Qt::Vertical, result->wrapper.get());
    splitter->setObjectName(QStringLiteral("blendRelationSplitter"));
    splitter->setChildrenCollapsible(false);
    result->parameters = new CBlendRelationTree(splitter);
    result->details = new CBlendRelationTree(splitter);
    result->parameters->setObjectName(QStringLiteral("blendParameterTree"));
    result->details->setObjectName(QStringLiteral("blendRelationTree"));
    result->parameters->setHeaderLabels({QStringLiteral("参数"), QStringLiteral("数值")});
    result->details->setHeaderLabels({QStringLiteral("关系"), QStringLiteral("说明")});
    for (auto* tree : {static_cast<QTreeWidget*>(result->parameters), result->details}) {
        tree->setColumnCount(2);
        tree->setRootIsDecorated(true);
        tree->header()->setStretchLastSection(true);
        tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    }
    layout->addWidget(result->search);
    layout->addWidget(result->summary);
    layout->addWidget(splitter, 1);
    return result;
}

void CBlendShapePanel::ConnectState()
{
    m_state->splitter->installEventFilter(this);
    m_state->parameters->installEventFilter(this);
    connect(m_state->splitter, &QSplitter::splitterMoved, this, [this](int, int) {
        if (m_rebuilding || m_applyingLayout) return;
        const auto sizes = m_state->splitter->sizes();
        const qint64 total = qint64(sizes.value(0)) + sizes.value(1);
        if (sizes.size() != 2 || sizes[0] <= 0 || sizes[1] <= 0 || total <= 0) return;
        m_manualSplit = true;
        m_splitRatio = double(sizes[0]) / double(total);
    });
    connect(m_state->parameters, &QTreeWidget::itemExpanded, this, [this] { ScheduleLayout(); });
    connect(m_state->parameters, &QTreeWidget::itemCollapsed, this, [this] { ScheduleLayout(); });
    connect(m_state->search, &QLineEdit::textChanged, this, [this] { FilterParameters(); });
    connect(m_state->parameters, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (m_rebuilding || !item || item->data(0, KindRole) != QStringLiteral("parameter")) return;
        ShowParameterDetails(item->data(0, Qt::UserRole).toString());
    });
    for (auto* tree : {static_cast<QTreeWidget*>(m_state->parameters), m_state->details})
        connect(tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) { ActivateItem(item); });
    connect(m_state->details, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) { ExpandDetails(item); });
    for (auto controls = m_state->controls.cbegin(); controls != m_state->controls.cend(); ++controls) {
        const QString id = controls.key();
        for (const auto& control : controls.value()) {
            connect(control.slider, &QSlider::sliderPressed, this, [this, item = control.item] {
                if (!m_rebuilding) m_state->parameters->setCurrentItem(item);
            });
            connect(control.slider, &QSlider::valueChanged, this, [this, id](int position) {
                if (m_rebuilding || !m_state->ranges.contains(id)) return;
                const auto range = m_state->ranges.value(id);
                const double ratio = qBound(0, position, 1000) / 1000.0;
                const float value = float(double(range.minimum) + (double(range.maximum) - range.minimum) * ratio);
                if (!std::isfinite(value)) return;
                SyncParameterValue(id, value);
                emit ParamChanged(id, value, false);
            });
        }
    }
}

void CBlendShapePanel::BuildBlendShapes(const QString& mocPath, CCidJson* cid)
{
    QFile file(mocPath);
    if (file.open(QIODevice::ReadOnly)) BuildBlendShapesBytes(file.readAll(), cid);
}

bool CBlendShapePanel::BuildBlendShapesBytes(const QByteArray& bytes, CCidJson* cid, QString* error)
{
    std::unique_ptr<State> candidate;
    try {
        auto reader = CReader::FromBytes(bytes);
        CSectionTable table(reader);
        if (!table.IsValid()) { if (error) *error = reader.Error(); return false; }
        candidate = CreateState();
        if (!candidate->relations.Build(table, error)) return false;
        candidate->digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        if (candidate->digest == m_state->digest) candidate->recentValues = m_state->recentValues;
        for (const auto& parameter : candidate->relations.Parameters()) {
            if (cid) {
                const auto name = cid->getParaName(parameter.id);
                if (!name.isEmpty()) candidate->parameterNames.insert(parameter.id, name);
            }
            if (parameter.type == 1 && !parameter.id.isEmpty()) candidate->blendIds.insert(parameter.id);
        }
        if (cid) for (const auto& target : candidate->relations.Targets()) if (target.type == Relations::Warp) {
            const auto name = cid->getDeformerName(target.id);
            if (!name.isEmpty()) candidate->targetNames.insert(target.id, name);
        }
        const auto appendParameter = [&](QTreeWidgetItem* group, int index) {
            const auto& parameter = candidate->relations.Parameters()[index];
            const QString name = parameter.id.isEmpty() ? QStringLiteral("参数[%1]").arg(index)
                : candidate->parameterNames.value(parameter.id, parameter.id);
            auto* item = new QTreeWidgetItem(group, {name, {}});
            item->setData(0, Qt::UserRole, parameter.id);
            item->setData(0, KindRole, QStringLiteral("parameter"));
            item->setData(0, IndexRole, index);
            item->setData(0, SelectableRole, parameter.selectable);
            item->setToolTip(0, parameter.id);
            if (!parameter.selectable) {
                item->setText(1, QStringLiteral("参数无效，查看诊断"));
                return;
            }
            auto* widget = new QWidget();
            auto* layout = new QHBoxLayout(widget);
            layout->setContentsMargins(0, 0, 0, 0);
            auto* slider = new QSlider(Qt::Horizontal, widget);
            slider->setRange(0, 1000);
            slider->setAccessibleName(name);
            auto* label = new QLabel(widget);
            label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            layout->addWidget(slider, 1);
            layout->addWidget(label);
            candidate->parameters->setItemWidget(item, 1, widget);
            candidate->controls[parameter.id].append({item, slider, label});
        };
        const auto& drivers = candidate->relations.BlendShapeParameterIndices();
        if (!drivers.isEmpty()) {
            auto* group = new QTreeWidgetItem(candidate->parameters, {QStringLiteral("融合参数 (%1)").arg(drivers.size())});
            group->setData(0, KindRole, QStringLiteral("group"));
            for (int parameter : drivers) appendParameter(group, parameter);
            group->setExpanded(true);
        }
        QVector<int> combined;
        QSet<QString> added;
        if (cid) for (const auto& group : cid->getCombinedParameters()) for (const auto& id : group) {
            if (added.contains(id)) continue;
            added.insert(id);
            const int index = candidate->relations.ParameterIndex(id);
            if (index >= 0) combined.append(index);
            else candidate->extraDiagnostics.append(QStringLiteral("组合参数引用不存在或无效：%1").arg(id));
        }
        if (!combined.isEmpty()) {
            auto* group = new QTreeWidgetItem(candidate->parameters, {QStringLiteral("组合参数 (%1)").arg(combined.size())});
            group->setData(0, KindRole, QStringLiteral("group"));
            for (int parameter : combined) appendParameter(group, parameter);
            group->setExpanded(true);
        }
        if (drivers.isEmpty() && combined.isEmpty()) message(candidate->parameters, QStringLiteral("此模型没有融合或组合参数"));
        candidate->summary->setText(QStringLiteral("%1 个融合参数 · %2 个组合参数 · %3 条诊断")
            .arg(drivers.size()).arg(combined.size()).arg(candidate->relations.Diagnostics().size() + candidate->extraDiagnostics.size()));
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("无法准备融合参数面板");
        return false;
    }
    const QString selected = m_state->selectedParameter;
    const QString search = m_state->search->text();
    const QScopedValueRollback<bool> rebuilding(m_rebuilding, true);
    auto previous = std::move(m_state);
    m_state = std::move(candidate);
    setWidget(m_state->wrapper.get());
    previous.reset();
    ConnectState();
    RefreshParameterMetadata();
    m_state->search->setText(search);
    FilterParameters();
    QTreeWidgetItem* selection = nullptr;
    for (QTreeWidgetItemIterator item(m_state->parameters); *item; ++item) {
        if ((*item)->data(0, KindRole) != QStringLiteral("parameter")) continue;
        if (!selection) selection = *item;
        if ((*item)->data(0, Qt::UserRole).toString() == selected) { selection = *item; break; }
    }
    if (selection) m_state->parameters->setCurrentItem(selection);
    ShowParameterDetails(selection ? selection->data(0, Qt::UserRole).toString() : QString());
    ScheduleLayout();
    if (error) error->clear();
    return true;
}

QByteArray CBlendShapePanel::SaveLayout() const
{
    QJsonObject state{{QStringLiteral("version"), 1},
                      {QStringLiteral("mode"), m_manualSplit ? QStringLiteral("manual") : QStringLiteral("auto")}};
    if (m_manualSplit) state.insert(QStringLiteral("ratio"), m_splitRatio);
    return QJsonDocument(state).toJson(QJsonDocument::Compact);
}

bool CBlendShapePanel::RestoreLayout(const QByteArray& state)
{
    const auto document = QJsonDocument::fromJson(state);
    if (!document.isObject()) return false;
    const auto object = document.object();
    const auto version = object.value(QStringLiteral("version"));
    const auto mode = object.value(QStringLiteral("mode"));
    if (!version.isDouble() || version.toDouble() != 1.0 || !mode.isString()) return false;
    const bool manual = mode.toString() == QLatin1String("manual");
    if ((!manual && mode.toString() != QLatin1String("auto")) || object.size() != (manual ? 3 : 2)) return false;
    double ratio = 0.5;
    if (manual) {
        const auto value = object.value(QStringLiteral("ratio"));
        if (!value.isDouble()) return false;
        ratio = value.toDouble();
        if (!std::isfinite(ratio) || ratio <= 0.0 || ratio >= 1.0) return false;
    }
    m_manualSplit = manual;
    m_splitRatio = ratio;
    ScheduleLayout();
    return true;
}

void CBlendShapePanel::ScheduleLayout()
{
    if (m_layoutQueued) return;
    m_layoutQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_layoutQueued = false;
        if (!m_rebuilding) UpdateSplitterLayout();
    });
}

void CBlendShapePanel::UpdateSplitterLayout()
{
    if (!m_state || !m_state->splitter->isVisible() || m_applyingLayout) return;
    const auto sizes = m_state->splitter->sizes();
    const qint64 sum = qint64(sizes.value(0)) + sizes.value(1);
    if (sizes.size() != 2 || sum <= 1 || sum > std::numeric_limits<int>::max()) return;
    const int available = int(sum);
    const QScopedValueRollback<bool> applying(m_applyingLayout, true);
    const int requested = m_manualSplit ? qRound(available * m_splitRatio)
        : m_state->parameters->ContentHeight(available / 2);
    int minimum = 0, maximum = available;
    m_state->splitter->getRange(1, &minimum, &maximum);
    const int origin = m_state->splitter->contentsRect().top();
    minimum = qBound(1, minimum - origin, available - 1);
    maximum = qBound(minimum, maximum - origin, available - 1);
    const int parameters = qBound(minimum, requested, maximum);
    if (sizes[0] != parameters) m_state->splitter->setSizes({parameters, available - parameters});
}

bool CBlendShapePanel::eventFilter(QObject* watched, QEvent* event)
{
    if (m_state && !m_applyingLayout &&
        ((watched == m_state->splitter && (event->type() == QEvent::Resize || event->type() == QEvent::Show ||
                                         event->type() == QEvent::LayoutRequest)) ||
         (watched == m_state->parameters && (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange))))
        ScheduleLayout();
    return QDockWidget::eventFilter(watched, event);
}

void CBlendShapePanel::SetDocument(const CModelDocument* document)
{
    m_document = document;
    RefreshParameterMetadata();
}

const QSet<QString>& CBlendShapePanel::getBlendShapeParamIds() const { return m_state->blendIds; }

QString CBlendShapePanel::ParameterName(const QString& id) const
{
    if (m_document && m_document->SourceDigest() == m_state->digest) {
        const auto& metadata = m_document->ParameterMetadataOverrides();
        const auto value = metadata.constFind(id);
        if (value != metadata.cend()) return value->displayName.isEmpty() ? id : value->displayName;
    }
    return m_state->parameterNames.value(id, id);
}

void CBlendShapePanel::RefreshParameterMetadata()
{
    if (!m_state) return;
    const bool matching = m_document && m_document->SourceDigest() == m_state->digest;
    for (auto controls = m_state->controls.begin(); controls != m_state->controls.end(); ++controls) {
        const int index = m_state->relations.ParameterIndex(controls.key());
        if (index < 0) continue;
        const auto& parameter = m_state->relations.Parameters()[index];
        State::Range range{parameter.minimum, parameter.maximum, parameter.defaultValue};
        const QString name = ParameterName(parameter.id);
        if (matching) {
            const auto& metadata = m_document->ParameterMetadataOverrides();
            const auto value = metadata.constFind(parameter.id);
            if (value != metadata.cend()) {
                range = {value->minimum, value->maximum, value->defaultValue};
            }
        }
        if (!std::isfinite(range.minimum) || !std::isfinite(range.maximum) || !std::isfinite(range.defaultValue) ||
            range.minimum > range.defaultValue || range.defaultValue > range.maximum) continue;
        m_state->ranges.insert(parameter.id, range);
        const float value = qBound(range.minimum, m_state->recentValues.value(parameter.id, range.defaultValue), range.maximum);
        for (const auto& control : controls.value()) {
            const QSignalBlocker blocker(control.slider);
            control.item->setText(0, name);
            control.slider->setAccessibleName(name);
            control.slider->setValue(sliderPosition(value, range.minimum, range.maximum));
            control.slider->setEnabled(range.minimum != range.maximum);
            control.label->setText(parameterText(value));
        }
    }
    for (QTreeWidgetItemIterator item(m_state->details); *item; ++item) {
        const auto kind = (*item)->data(0, KindRole).toString();
        const auto id = (*item)->data(0, Qt::UserRole).toString();
        if (kind == QLatin1String("binding"))
            (*item)->setText(0, QStringLiteral("绑定 %1 · %2").arg((*item)->data(0, IndexRole).toInt()).arg(ParameterName(id)));
        else if (kind == QLatin1String("limiter")) (*item)->setText(0, QStringLiteral("限制参数 · %1").arg(ParameterName(id)));
    }
    FilterParameters();
}

void CBlendShapePanel::SyncParameterValue(const QString& paramId, float value)
{
    if (!m_state || !std::isfinite(value) || m_state->relations.ParameterIndex(paramId) < 0) return;
    m_state->recentValues.insert(paramId, value);
    const auto range = m_state->ranges.constFind(paramId);
    if (range == m_state->ranges.cend()) return;
    const float bounded = qBound(range->minimum, value, range->maximum);
    for (const auto& control : m_state->controls.value(paramId)) {
        const QSignalBlocker blocker(control.slider);
        control.slider->setValue(sliderPosition(bounded, range->minimum, range->maximum));
        control.label->setText(parameterText(bounded));
    }
}

void CBlendShapePanel::FilterParameters()
{
    if (!m_state) return;
    const auto query = m_state->search->text().trimmed();
    for (int group = 0; group < m_state->parameters->topLevelItemCount(); ++group) {
        auto* parent = m_state->parameters->topLevelItem(group);
        bool matches = parent->childCount() == 0;
        for (int row = 0; row < parent->childCount(); ++row) {
            auto* item = parent->child(row);
            const bool match = query.isEmpty() || item->text(0).contains(query, Qt::CaseInsensitive) ||
                               item->data(0, Qt::UserRole).toString().contains(query, Qt::CaseInsensitive);
            item->setHidden(!match);
            matches = matches || match;
        }
        parent->setHidden(!matches);
        if (matches && !query.isEmpty()) parent->setExpanded(true);
    }
    ScheduleLayout();
}

void CBlendShapePanel::ShowParameterDetails(const QString& id)
{
    if (!m_state || (m_state->selectedParameter == id && m_state->details->topLevelItemCount())) return;
    m_state->selectedParameter = id;
    const QSignalBlocker blocker(m_state->details);
    m_state->details->clear();
    const int parameter = m_state->relations.ParameterIndex(id);
    if (parameter >= 0) {
        const auto& source = m_state->relations.Parameters()[parameter];
        message(m_state->details, QStringLiteral("源范围 · %1").arg(id),
                QStringLiteral("[%1, %2] · 默认 %3").arg(parameterText(source.minimum), parameterText(source.maximum), parameterText(source.defaultValue)));
        const auto targets = m_state->relations.TargetsForParameter(parameter);
        for (const auto& target : targets) {
            const QString name = target.type == Relations::Warp ? m_state->targetNames.value(target.id, target.id) : target.id;
            auto* item = new QTreeWidgetItem(m_state->details, {name, target.type == Relations::ArtMesh ? QStringLiteral("ArtMesh") : QStringLiteral("网格变形器")});
            item->setData(0, Qt::UserRole, target.id);
            item->setData(0, KindRole, QStringLiteral("target"));
            item->setData(0, TypeRole, targetType(target.type));
            item->setData(0, IndexRole, int(target.type));
            item->setData(0, SelectableRole, target.selectable);
            item->setToolTip(0, target.id);
            item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        }
        if (targets.isEmpty()) message(m_state->details, QStringLiteral("没有有效的融合目标"),
            source.type == 1 ? QStringLiteral("参数仍可调整和定位") : QStringLiteral("普通组合参数"));
    } else message(m_state->details, QStringLiteral("选择参数查看目标和权重限制"));
    const int diagnostics = m_state->relations.Diagnostics().size() + m_state->extraDiagnostics.size();
    if (diagnostics) {
        auto* item = new QTreeWidgetItem(m_state->details, {QStringLiteral("诊断 (%1)").arg(diagnostics), QStringLiteral("展开查看")});
        item->setData(0, KindRole, QStringLiteral("diagnostics"));
        item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }
}

void CBlendShapePanel::ExpandDetails(QTreeWidgetItem* item)
{
    if (m_rebuilding || !item || item->data(0, LoadedRole).toBool()) return;
    item->setData(0, LoadedRole, true);
    const auto kind = item->data(0, KindRole).toString();
    if (kind == QLatin1String("target")) {
        const auto type = Relations::TargetType(item->data(0, IndexRole).toInt());
        const auto bindings = m_state->relations.BindingsForTarget(type, item->data(0, Qt::UserRole).toString());
        for (int index : bindings) {
            const auto& binding = m_state->relations.Bindings()[index];
            const auto& parameter = m_state->relations.Parameters()[binding.parameterIndex];
            auto* child = new QTreeWidgetItem(item, {QStringLiteral("绑定 %1 · %2").arg(index).arg(ParameterName(parameter.id)),
                QStringLiteral("%1 个关键形").arg(binding.keyforms.count)});
            child->setData(0, Qt::UserRole, parameter.id);
            child->setData(0, KindRole, QStringLiteral("binding"));
            child->setData(0, IndexRole, index);
            child->setData(0, SelectableRole, parameter.selectable);
            child->setToolTip(0, parameter.id);
            const auto& source = m_state->relations.ParameterBindings()[binding.parameterBindingIndex];
            child->setToolTip(1, QStringLiteral("源关键值 %1 项 · 基准索引 %2").arg(source.keys.count).arg(source.baseKeyIndex));
            if (binding.constraints.count) child->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        }
        if (bindings.isEmpty()) new QTreeWidgetItem(item, {QStringLiteral("没有有效绑定")});
    } else if (kind == QLatin1String("binding")) {
        for (const auto& limiter : m_state->relations.LimitersForBinding(item->data(0, IndexRole).toInt())) {
            const auto& parameter = m_state->relations.Parameters()[limiter.parameterIndex];
            auto* child = new QTreeWidgetItem(item, {QStringLiteral("限制参数 · %1").arg(ParameterName(parameter.id)), QStringLiteral("%1 个曲线点").arg(limiter.values.count)});
            child->setData(0, Qt::UserRole, parameter.id);
            child->setData(0, KindRole, QStringLiteral("limiter"));
            child->setData(0, IndexRole, limiter.values.begin);
            child->setData(1, IndexRole, limiter.values.count);
            child->setData(0, SelectableRole, parameter.selectable && limiter.valid);
            child->setToolTip(0, parameter.id);
            if (limiter.values.count) child->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        }
        if (!item->childCount()) new QTreeWidgetItem(item, {QStringLiteral("没有有效权重限制")});
    } else if (kind == QLatin1String("limiter")) {
        const int begin = item->data(0, IndexRole).toInt(), count = item->data(1, IndexRole).toInt();
        for (int index = begin; index < begin + count; ++index) {
            const auto& value = m_state->relations.ConstraintValues()[index];
            new QTreeWidgetItem(item, {QStringLiteral("关键值 %1").arg(parameterText(value.key)), QStringLiteral("权重 %1").arg(parameterText(value.weight))});
        }
    } else if (kind == QLatin1String("diagnostics")) {
        for (const auto& diagnostic : m_state->relations.Diagnostics())
            new QTreeWidgetItem(item, {diagnostic.message, QStringLiteral("%1[%2] · %3").arg(diagnostic.table).arg(diagnostic.index).arg(diagnostic.code)});
        for (const auto& diagnostic : m_state->extraDiagnostics) new QTreeWidgetItem(item, {diagnostic});
    }
}

void CBlendShapePanel::ActivateItem(QTreeWidgetItem* item)
{
    if (m_rebuilding || !item || !item->data(0, SelectableRole).toBool()) return;
    const QString id = item->data(0, Qt::UserRole).toString();
    const auto kind = item->data(0, KindRole).toString();
    if (kind == QLatin1String("target")) emit ObjectSelected(id, item->data(0, TypeRole).toString());
    else if (m_state->relations.ParameterIndex(id) >= 0) emit ParameterSelected(id);
}
