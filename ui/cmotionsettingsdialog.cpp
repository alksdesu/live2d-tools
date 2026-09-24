#include "cmotionsettingsdialog.h"
#include "ctableeditdelegate.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>

class COptionalMotionTime : public QWidget
{
public:
    COptionalMotionTime(const QString& name, QWidget* parent) : QWidget(parent)
    {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        m_enabled = new QCheckBox(QStringLiteral("自定义"), this);
        m_enabled->setObjectName(name + QStringLiteral("Enabled"));
        m_seconds = new QDoubleSpinBox(this);
        m_seconds->setObjectName(name);
        m_seconds->setDecimals(6);
        m_seconds->setRange(0, std::numeric_limits<float>::max());
        m_seconds->setSuffix(QStringLiteral(" 秒"));
        m_seconds->setSingleStep(0.1);
        layout->addWidget(m_enabled);
        layout->addWidget(m_seconds, 1);
        connect(m_enabled, &QCheckBox::toggled, this, [this](bool enabled){
            m_changed = true;
            m_seconds->setEnabled(enabled);
        });
        connect(m_seconds, &QDoubleSpinBox::valueChanged, this, [this](){ m_changed = true; });
    }

    void SetValue(std::optional<double> value)
    {
        const QSignalBlocker enabledBlocker(m_enabled);
        const QSignalBlocker secondsBlocker(m_seconds);
        m_original = value;
        const bool explicitTime = value && *value >= 0;
        m_enabled->setChecked(explicitTime);
        m_seconds->setEnabled(explicitTime);
        m_seconds->setValue(explicitTime ? *value : 0.0);
        m_changed = false;
    }

    std::optional<double> Value() const
    {
        if (!m_changed) return m_original;
        return m_enabled->isChecked() ? std::optional<double>(m_seconds->value()) : std::nullopt;
    }

private:
    QCheckBox* m_enabled;
    QDoubleSpinBox* m_seconds;
    std::optional<double> m_original;
    bool m_changed = false;
};

CMotionSettingsDialog::CMotionSettingsDialog(const CMotionTimeline& timeline, double playhead, QWidget* parent)
    : QDialog(parent), m_timeline(timeline), m_playhead(playhead)
{
    setObjectName(QStringLiteral("motionSettingsDialog"));
    setWindowTitle(QStringLiteral("动作设置"));
    resize(640, 480);
    auto* layout = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("motionSettingsTabs"));
    layout->addWidget(tabs, 1);

    auto* general = new QWidget(tabs);
    auto* generalLayout = new QFormLayout(general);
    m_fps = new QDoubleSpinBox(general);
    m_fps->setObjectName(QStringLiteral("motionFps"));
    m_fps->setDecimals(6);
    m_fps->setRange(0.000001, std::numeric_limits<float>::max());
    m_fps->setValue(timeline.Timing().fps);
    m_duration = new QDoubleSpinBox(general);
    m_duration->setObjectName(QStringLiteral("motionDuration"));
    m_duration->setDecimals(6);
    m_duration->setRange(0, std::numeric_limits<float>::max());
    m_duration->setSuffix(QStringLiteral(" 秒"));
    m_duration->setValue(timeline.Timing().duration);
    generalLayout->addRow(QStringLiteral("帧率"), m_fps);
    generalLayout->addRow(QStringLiteral("动作时长"), m_duration);
    m_fadeIn = new COptionalMotionTime(QStringLiteral("motionFadeIn"), general);
    m_fadeOut = new COptionalMotionTime(QStringLiteral("motionFadeOut"), general);
    const auto fades = timeline.FadeTimes();
    m_fadeIn->SetValue(fades.fadeIn);
    m_fadeOut->SetValue(fades.fadeOut);
    generalLayout->addRow(QStringLiteral("淡入"), m_fadeIn);
    generalLayout->addRow(QStringLiteral("淡出"), m_fadeOut);
    auto* generalHint = new QLabel(QStringLiteral("未自定义时使用动作默认淡入淡出。时长须覆盖所有关键帧与事件；新增较晚的事件会自动延长动作。"), general);
    generalHint->setWordWrap(true);
    generalLayout->addRow(generalHint);
    tabs->addTab(general, QStringLiteral("动作"));
    connect(m_fps, &QDoubleSpinBox::valueChanged, this, [this](){ m_fpsChanged = true; });
    connect(m_duration, &QDoubleSpinBox::valueChanged, this, [this](){ m_durationChanged = true; });

    auto* curves = new QWidget(tabs);
    auto* curveLayout = new QFormLayout(curves);
    m_curve = new QComboBox(curves);
    m_curve->setObjectName(QStringLiteral("motionFadeTrack"));
    m_curve->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_curve->setMinimumContentsLength(16);
    m_curveFadeIn = new COptionalMotionTime(QStringLiteral("curveFadeIn"), curves);
    m_curveFadeOut = new COptionalMotionTime(QStringLiteral("curveFadeOut"), curves);
    curveLayout->addRow(QStringLiteral("曲线"), m_curve);
    curveLayout->addRow(QStringLiteral("淡入"), m_curveFadeIn);
    curveLayout->addRow(QStringLiteral("淡出"), m_curveFadeOut);
    auto* curveHint = new QLabel(timeline.HasCurves()
        ? QStringLiteral("未自定义的曲线沿用动作设置。可在时间轴勾选“混合”查看参数与基础姿态的混合效果。")
        : QStringLiteral("先在时间轴添加关键帧，即可为曲线设置淡入淡出。"), curves);
    curveHint->setWordWrap(true);
    curveLayout->addRow(curveHint);
    m_curveFades = timeline.TrackFadeTimes();
    for (const auto& track : timeline.Tracks().keys()) {
        m_curve->addItem(track.TargetName() + QStringLiteral(" · ") + track.id, QVariant::fromValue(track));
    }
    const auto selectCurve = [this, curveHint](int index){
        StoreCurveFade();
        m_currentTrack.reset();
        m_curveFadeIn->setEnabled(index >= 0);
        m_curveFadeOut->setEnabled(index >= 0);
        if (index < 0) return;
        m_currentTrack = m_curve->itemData(index).value<CMotionTrack>();
        curveHint->setText(m_currentTrack->target == CMotionTarget::Parameter
            ? QStringLiteral("未自定义时沿用动作设置。可在时间轴勾选“混合”查看参数与基础姿态的混合效果。")
            : QStringLiteral("此类曲线的设置可保存，但当前 Cubism SDK 不使用它的单独淡入淡出时间。"));
        const auto fades = m_curveFades.value(*m_currentTrack);
        m_curveFadeIn->SetValue(fades.fadeIn);
        m_curveFadeOut->SetValue(fades.fadeOut);
    };
    connect(m_curve, &QComboBox::currentIndexChanged, this, selectCurve);
    selectCurve(m_curve->currentIndex());
    tabs->addTab(curves, QStringLiteral("曲线"));

    auto* events = new QWidget(tabs);
    auto* eventsLayout = new QVBoxLayout(events);
    auto* eventTools = new QHBoxLayout();
    auto* addEvent = new QPushButton(QStringLiteral("添加事件"), events);
    addEvent->setObjectName(QStringLiteral("addMotionEvent"));
    auto* removeEvent = new QPushButton(QStringLiteral("删除事件"), events);
    removeEvent->setObjectName(QStringLiteral("removeMotionEvent"));
    eventTools->addWidget(addEvent);
    eventTools->addWidget(removeEvent);
    eventTools->addStretch();
    eventsLayout->addLayout(eventTools);
    m_events = new QTableWidget(0, 2, events);
    m_events->setObjectName(QStringLiteral("motionEventTable"));
    m_events->setAccessibleName(QStringLiteral("动作事件时间和内容"));
    m_events->setHorizontalHeaderLabels({QStringLiteral("时间（秒）"), QStringLiteral("事件内容")});
    m_events->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_events->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_events->verticalHeader()->setVisible(false);
    m_events->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_eventDelegate = new CTableEditDelegate(m_events);
    m_events->setItemDelegate(m_eventDelegate);
    eventsLayout->addWidget(m_events, 1);
    auto* eventHint = new QLabel(QStringLiteral("播放到指定时间时提示事件内容。多个事件可使用相同时间。"), events);
    eventHint->setWordWrap(true);
    eventsLayout->addWidget(eventHint);
    for (const auto& event : timeline.Events()) AddEvent(event.toObject());
    removeEvent->setEnabled(false);
    connect(m_events, &QTableWidget::itemSelectionChanged, this, [this, removeEvent](){
        removeEvent->setEnabled(!m_events->selectionModel()->selectedRows().isEmpty());
    });
    connect(addEvent, &QPushButton::clicked, this, [this](){
        m_eventDelegate->CommitEditor();
        AddEvent({{QStringLiteral("Time"), m_playhead}, {QStringLiteral("Value"), QString()}});
        const int row = m_events->rowCount() - 1;
        m_events->setCurrentCell(row, 1);
        m_events->editItem(m_events->item(row, 1));
    });
    connect(removeEvent, &QPushButton::clicked, this, [this](){
        m_eventDelegate->CommitEditor();
        auto rows = m_events->selectionModel()->selectedRows();
        std::sort(rows.begin(), rows.end(), [](const QModelIndex& left, const QModelIndex& right){
            return left.row() > right.row();
        });
        for (const auto& row : rows) {
            m_events->removeRow(row.row());
            m_eventSources.removeAt(row.row());
        }
    });
    tabs->addTab(events, QStringLiteral("事件"));

    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("motionSettingsError"));
    m_error->setTextFormat(Qt::PlainText);
    m_error->setWordWrap(true);
    m_error->hide();
    layout->addWidget(m_error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("应用"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &CMotionSettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &CMotionSettingsDialog::reject);
    layout->addWidget(buttons);
}

void CMotionSettingsDialog::StoreCurveFade()
{
    if (m_currentTrack) m_curveFades.insert(*m_currentTrack, {m_curveFadeIn->Value(), m_curveFadeOut->Value()});
}

void CMotionSettingsDialog::AddEvent(const QJsonObject& event)
{
    const int row = m_events->rowCount();
    m_events->insertRow(row);
    m_events->setItem(row, 0, new QTableWidgetItem(QString::number(event.value(QStringLiteral("Time")).toDouble(), 'g', 12)));
    m_events->setItem(row, 1, new QTableWidgetItem(event.value(QStringLiteral("Value")).toString()));
    m_eventSources.append(event);
}

void CMotionSettingsDialog::accept()
{
    m_eventDelegate->CommitEditor();
    StoreCurveFade();
    m_error->clear();
    m_error->hide();
    CMotionTimeline candidate = m_timeline;
    QString error;
    QJsonArray events;
    for (int row = 0; row < m_events->rowCount(); ++row) {
        bool valid = false;
        const QString timeText = m_events->item(row, 0)->text();
        double time = timeText.toDouble(&valid);
        const double originalTime = m_eventSources.at(row).value(QStringLiteral("Time")).toDouble();
        if (timeText == QString::number(originalTime, 'g', 12)) time = originalTime;
        if (!valid || !std::isfinite(time) || time < 0) {
            error = QStringLiteral("第 %1 行事件时间无效，请输入不小于 0 的秒数。").arg(row + 1);
            break;
        }
        auto event = m_eventSources.at(row);
        event.insert(QStringLiteral("Time"), time);
        event.insert(QStringLiteral("Value"), m_events->item(row, 1)->text());
        events.append(event);
    }
    if (error.isEmpty()) candidate.SetEvents(events, &error);
    if (error.isEmpty() && (m_fpsChanged || m_durationChanged)) {
        const auto timing = candidate.Timing();
        candidate.SetTiming(m_fpsChanged ? m_fps->value() : timing.fps,
                            m_durationChanged ? m_duration->value() : timing.duration, &error);
    }
    if (error.isEmpty()) candidate.SetFadeTimes({m_fadeIn->Value(), m_fadeOut->Value()}, &error);
    if (error.isEmpty()) for (auto it = m_curveFades.cbegin(); it != m_curveFades.cend(); ++it) {
        if (!candidate.SetTrackFadeTimes(it.key(), it.value(), &error)) break;
    }
    if (!error.isEmpty()) {
        m_error->setText(error);
        m_error->show();
        return;
    }
    m_timeline = std::move(candidate);
    QDialog::accept();
}
