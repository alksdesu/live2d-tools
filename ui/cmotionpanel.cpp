#include "cmotionpanel.h"
#include "cmotioncurveview.h"
#include "live2d/LAppLive2DManager.hpp"
#include "live2d/LAppModel.hpp"
#include <QDebug>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QSlider>
#include <QSplitter>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QInputDialog>
#include <QMessageBox>
#include <QSaveFile>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QGroupBox>
#include <QFileDialog>
#include <QTimer>
#include <QSignalBlocker>
#include <algorithm>

using namespace Live2D::Cubism::Framework;

namespace {
void clearLayoutItems(QLayout* layout)
{
    if (!layout) return;
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            delete widget;
        } else if (QLayout* child = item->layout()) {
            clearLayoutItems(child);
            delete child;
        }
        delete item;
    }
}
}

CMotionPanel::CMotionPanel(QWidget *parent)
    : QDockWidget("动作/表情", parent)
{
    setObjectName(QStringLiteral("motionPanel"));
    QWidget* wrapper = new QWidget(this);
    QVBoxLayout* mainLayout = new QVBoxLayout(wrapper);

    QSplitter* splitter = new QSplitter(Qt::Vertical, wrapper);

    // 动作部分
    QWidget* motionWidget = new QWidget();
    QVBoxLayout* motionLayout = new QVBoxLayout(motionWidget);
    motionLayout->addWidget(new QLabel("动作列表"));
    m_motionSearch = new QLineEdit(motionWidget);
    m_motionSearch->setPlaceholderText(QStringLiteral("搜索动作或分组"));
    m_motionSearch->setClearButtonEnabled(true);
    motionLayout->addWidget(m_motionSearch);
    m_motionList = new QListWidget();
    motionLayout->addWidget(m_motionList);
    QPushButton* playBtn = new QPushButton("▶ 播放");
    playBtn->setObjectName("primaryBtn");
    QPushButton* stopBtn = new QPushButton("■ 停止");
    motionLayout->addWidget(playBtn);
    motionLayout->addWidget(stopBtn);
    splitter->addWidget(motionWidget);

    connect(playBtn, &QPushButton::clicked, this, [this](){
        int row = m_motionList->currentRow();
        if(row >= 0 && row < m_motionEntries.size()){
            emit PlayMotion(m_motionEntries[row].group, m_motionEntries[row].no);
        }
    });
    connect(stopBtn, &QPushButton::clicked, this, &CMotionPanel::StopMotions);

    connect(m_motionList, &QListWidget::itemDoubleClicked, this, [this](){
        int row = m_motionList->currentRow();
        if(row >= 0 && row < m_motionEntries.size()){
            emit PlayMotion(m_motionEntries[row].group, m_motionEntries[row].no);
        }
    });

    // 表情部分
    QWidget* exprWidget = new QWidget();
    QVBoxLayout* exprLayout = new QVBoxLayout(exprWidget);
    exprLayout->addWidget(new QLabel("表情列表"));
    m_exprSearch = new QLineEdit(exprWidget);
    m_exprSearch->setPlaceholderText(QStringLiteral("搜索表情"));
    m_exprSearch->setClearButtonEnabled(true);
    exprLayout->addWidget(m_exprSearch);
    m_exprList = new QListWidget();
    exprLayout->addWidget(m_exprList);
    QPushButton* applyBtn = new QPushButton("✦ 应用");
    applyBtn->setObjectName("primaryBtn");
    m_editExpressionButton = new QPushButton(QStringLiteral("编辑表情"), exprWidget);
    m_editExpressionButton->setEnabled(false);
    exprLayout->addWidget(applyBtn);
    exprLayout->addWidget(m_editExpressionButton);
    splitter->addWidget(exprWidget);

    // Pose 区域
    m_poseWidget = new QWidget();
    QVBoxLayout* poseLayout = new QVBoxLayout(m_poseWidget);
    poseLayout->addWidget(new QLabel("Pose 切换"));
    auto* poseTools = new QHBoxLayout();
    m_addPosePartButton = new QPushButton(QStringLiteral("添加 Part"), m_poseWidget);
    m_removePosePartButton = new QPushButton(QStringLiteral("删除 Part"), m_poseWidget);
    m_editPoseLinksButton = new QPushButton(QStringLiteral("编辑 Link"), m_poseWidget);
    m_savePoseButton = new QPushButton(QStringLiteral("保存 Pose"), m_poseWidget);
    m_addPosePartButton->setEnabled(false);
    m_removePosePartButton->setEnabled(false);
    m_editPoseLinksButton->setEnabled(false);
    m_savePoseButton->setEnabled(false);
    poseTools->addWidget(m_addPosePartButton);
    poseTools->addWidget(m_removePosePartButton);
    poseTools->addWidget(m_editPoseLinksButton);
    poseTools->addWidget(m_savePoseButton, 1);
    poseLayout->addLayout(poseTools);
    connect(m_addPosePartButton, &QPushButton::clicked, this, &CMotionPanel::AddPosePart);
    connect(m_removePosePartButton, &QPushButton::clicked, this, &CMotionPanel::RemovePosePart);
    connect(m_editPoseLinksButton, &QPushButton::clicked, this, &CMotionPanel::EditPoseLinks);
    connect(m_savePoseButton, &QPushButton::clicked, this, [this](){
        QString error;
        if (!SavePose(&error)) QMessageBox::warning(this, QStringLiteral("保存 Pose 失败"), error);
    });
    m_poseWidget->setVisible(false); // 没有 pose 数据时隐藏
    splitter->addWidget(m_poseWidget);

    connect(applyBtn, &QPushButton::clicked, this, [this](){
        int row = m_exprList->currentRow();
        if(row >= 0 && row < m_exprEntries.size()){
            emit ApplyExpression(m_exprEntries[row].name);
        }
    });

    connect(m_exprList, &QListWidget::itemDoubleClicked, this, [this](){
        int row = m_exprList->currentRow();
        if(row >= 0 && row < m_exprEntries.size()){
            emit ApplyExpression(m_exprEntries[row].name);
        }
    });

    connect(m_editExpressionButton, &QPushButton::clicked, this, &CMotionPanel::EditSelectedExpression);
    connect(m_exprList, &QListWidget::currentRowChanged, this, [this](int row){
        m_editExpressionButton->setEnabled(row >= 0 && row < m_exprEntries.size());
    });

    connect(m_motionSearch, &QLineEdit::textChanged, this, [this](const QString& text){
        const QString query = text.trimmed().toLower();
        for (int i = 0; i < m_motionList->count(); ++i)
            m_motionList->item(i)->setHidden(!query.isEmpty() && !m_motionList->item(i)->text().toLower().contains(query));
    });
    connect(m_exprSearch, &QLineEdit::textChanged, this, [this](const QString& text){
        const QString query = text.trimmed().toLower();
        for (int i = 0; i < m_exprList->count(); ++i)
            m_exprList->item(i)->setHidden(!query.isEmpty() && !m_exprList->item(i)->text().toLower().contains(query));
    });

    mainLayout->addWidget(splitter);

    auto* timelineBox = new QGroupBox(QStringLiteral("参数录制时间轴"), wrapper);
    auto* timelineLayout = new QVBoxLayout(timelineBox);
    auto* timelineTools = new QHBoxLayout();
    m_recordTimelineCb = new QCheckBox(QStringLiteral("录制"), timelineBox);
    m_recordTimelineCb->setObjectName(QStringLiteral("recordTimeline"));
    auto* clearTimelineButton = new QPushButton(QStringLiteral("清空"), timelineBox);
    auto* removeTimelineButton = new QPushButton(QStringLiteral("删除关键帧"), timelineBox);
    auto* exportTimelineButton = new QPushButton(QStringLiteral("导出 motion3"), timelineBox);
    auto* importTimelineButton = new QPushButton(QStringLiteral("导入 motion3"), timelineBox);
    m_playTimelineButton = new QPushButton(QStringLiteral("预览"), timelineBox);
    m_playTimelineButton->setObjectName(QStringLiteral("previewTimeline"));
    m_playTimelineButton->setEnabled(false);
    timelineTools->addWidget(m_playTimelineButton);
    timelineTools->addWidget(m_recordTimelineCb);
    timelineTools->addWidget(clearTimelineButton);
    timelineTools->addWidget(removeTimelineButton);
    timelineTools->addWidget(exportTimelineButton);
    timelineTools->addWidget(importTimelineButton);
    timelineTools->addStretch(1);
    m_timelineStatus = new QLabel(QStringLiteral("暂无关键帧"), timelineBox);
    timelineTools->addWidget(m_timelineStatus);
    timelineLayout->addLayout(timelineTools);
    m_curveView = new CMotionCurveView(timelineBox);
    timelineLayout->addWidget(m_curveView);
    m_timelineTable = new QTableWidget(0, 4, timelineBox);
    m_timelineTable->setObjectName(QStringLiteral("motionTimelineTable"));
    m_timelineTable->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("参数"), QStringLiteral("值"), QStringLiteral("插值")});
    m_timelineTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_timelineTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_timelineTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_timelineTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_timelineTable->verticalHeader()->setVisible(false);
    m_timelineTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_timelineTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                     QAbstractItemView::SelectedClicked);
    m_timelineTable->setMaximumHeight(170);
    timelineLayout->addWidget(m_timelineTable);
    m_timelinePlayhead = new QSlider(Qt::Horizontal, timelineBox);
    m_timelinePlayhead->setObjectName(QStringLiteral("motionTimelinePlayhead"));
    m_timelinePlayhead->setRange(0, 1000);
    m_timelinePlayhead->setEnabled(false);
    m_timelineTimeLabel = new QLabel(QStringLiteral("0.000s"), timelineBox);
    auto* playheadRow = new QHBoxLayout();
    playheadRow->addWidget(new QLabel(QStringLiteral("播放头"), timelineBox));
    playheadRow->addWidget(m_timelinePlayhead, 1);
    playheadRow->addWidget(m_timelineTimeLabel);
    timelineLayout->addLayout(playheadRow);
    mainLayout->addWidget(timelineBox);
    connect(m_recordTimelineCb, &QCheckBox::toggled, this, [this](bool recording){
        if (recording) {
            m_timeline.StartRecording();
            m_timelineStatus->setText(QStringLiteral("正在录制"));
        } else {
            m_timeline.StopRecording();
            m_timelineStatus->setText(m_timeline.PointCount() > 0
                                      ? QStringLiteral("已录制 %1 个关键帧").arg(m_timeline.PointCount())
                                      : QStringLiteral("暂无关键帧"));
        }
    });
    connect(clearTimelineButton, &QPushButton::clicked, this, &CMotionPanel::ClearTimeline);
    connect(removeTimelineButton, &QPushButton::clicked, this, &CMotionPanel::RemoveTimelineKeyframe);
    connect(exportTimelineButton, &QPushButton::clicked, this, &CMotionPanel::ExportTimeline);
    connect(importTimelineButton, &QPushButton::clicked, this, &CMotionPanel::ImportTimeline);
    connect(m_timelineTable, &QTableWidget::cellChanged, this, &CMotionPanel::ApplyTimelineCellEdit);
    connect(m_timelineTable, &QTableWidget::currentCellChanged, this, [this](int row, int, int, int){
        if (!m_curveView || row < 0 || row >= m_timelineRows.size()) return;
        const TimelineRow& selected = m_timelineRows.at(row);
        m_curveView->SetSelectedPoint(selected.parameterId, selected.pointIndex);
    });
    connect(m_curveView, &CMotionCurveView::PointSelected, this, [this](const QString& parameterId, int pointIndex){
        if (!m_timelineTable) return;
        for (int row = 0; row < m_timelineRows.size(); ++row) {
            const TimelineRow& candidate = m_timelineRows.at(row);
            if (candidate.parameterId == parameterId && candidate.pointIndex == pointIndex) {
                m_timelineTable->selectRow(row);
                return;
            }
        }
    });
    connect(m_curveView, &CMotionCurveView::PointEdited, this, &CMotionPanel::ApplyTimelineCurveEdit);
    connect(m_timelinePlayhead, &QSlider::valueChanged, this, &CMotionPanel::SetTimelineTimeFromSlider);
    m_timelineTimer = new QTimer(this);
    m_timelineTimer->setInterval(33);
    connect(m_playTimelineButton, &QPushButton::clicked, this, &CMotionPanel::ToggleTimelinePlayback);
    connect(m_timelineTimer, &QTimer::timeout, this, &CMotionPanel::TickTimelinePlayback);
    connect(this, &CMotionPanel::MotionTimelineChanged, this, [this](){
        if (m_recordTimelineCb && m_recordTimelineCb->isChecked()) return;
        emit MotionTimelinePreviewChanged(m_timeline.EvaluateAll(m_timelineTime));
    });

    // 选项
    QHBoxLayout* optLayout = new QHBoxLayout();
    m_eyeBlinkCb = new QCheckBox("自动眨眼");
    m_eyeBlinkCb->setChecked(true);
    m_breathCb = new QCheckBox("自动呼吸");
    m_breathCb->setChecked(true);
    m_mouseFollowCb = new QCheckBox("鼠标追踪");
    m_mouseFollowCb->setChecked(false);
    m_physicsCb = new QCheckBox("物理模拟");
    m_physicsCb->setChecked(true);
    m_loopCb = new QCheckBox(QStringLiteral("动作循环"));
    m_speedSlider = new QSlider(Qt::Horizontal, wrapper);
    m_speedSlider->setRange(25, 200);
    m_speedSlider->setValue(100);
    m_speedSlider->setToolTip(QStringLiteral("动作播放速度：25%–200%"));
    m_speedLabel = new QLabel(QStringLiteral("100%"), wrapper);
    m_speedLabel->setMinimumWidth(42);
    optLayout->addWidget(m_eyeBlinkCb);
    optLayout->addWidget(m_breathCb);
    optLayout->addWidget(m_mouseFollowCb);
    optLayout->addWidget(m_physicsCb);
    optLayout->addWidget(m_loopCb);
    optLayout->addWidget(new QLabel(QStringLiteral("速度"), wrapper));
    optLayout->addWidget(m_speedSlider, 1);
    optLayout->addWidget(m_speedLabel);
    mainLayout->addLayout(optLayout);

    connect(m_eyeBlinkCb, &QCheckBox::toggled, this, &CMotionPanel::AutoEyeBlinkChanged);
    connect(m_breathCb, &QCheckBox::toggled, this, &CMotionPanel::AutoBreathChanged);
    connect(m_mouseFollowCb, &QCheckBox::toggled, this, &CMotionPanel::MouseFollowChanged);
    connect(m_physicsCb, &QCheckBox::toggled, this, &CMotionPanel::PhysicsChanged);
    connect(m_loopCb, &QCheckBox::toggled, this, [this](bool loop){
        emit MotionPlaybackChanged(m_speedSlider->value() / 100.0f, loop);
    });
    connect(m_speedSlider, &QSlider::valueChanged, this, [this](int value){
        m_speedLabel->setText(QStringLiteral("%1%").arg(value));
        emit MotionPlaybackChanged(value / 100.0f, m_loopCb->isChecked());
    });

    setWidget(wrapper);
}

void CMotionPanel::BuildLists()
{
    if (m_timelineTimer) m_timelineTimer->stop();
    if (m_playTimelineButton) m_playTimelineButton->setText(QStringLiteral("预览"));
    if (m_timelinePlayhead) m_timelinePlayhead->setValue(0);
    if (m_recordTimelineCb) m_recordTimelineCb->setChecked(false);
    m_timeline.Clear();
    RefreshTimelineTable();
    m_motionList->clear();
    m_exprList->clear();
    m_motionEntries.clear();
    m_exprEntries.clear();
    m_editExpressionButton->setEnabled(false);

    LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
    if(!mgr || mgr->GetModelNum() == 0) return;

    LAppModel* model = mgr->GetModel(0);
    if(!model) return;

    ICubismModelSetting* setting = model->GetModelSetting();
    if(!setting) return;

    // 动作
    int groupCount = setting->GetMotionGroupCount();
    qDebug() << "[MotionPanel] motion groups:" << groupCount
             << "expressions:" << setting->GetExpressionCount();
    for(int g = 0; g < groupCount; g++){
        const char* groupName = setting->GetMotionGroupName(g);
        if (!groupName || !*groupName) continue;
        int motionCount = setting->GetMotionCount(groupName);
        for(int m = 0; m < motionCount; m++){
            QString label = QString("%1 [%2]").arg(groupName).arg(m);
            m_motionList->addItem(label);
            MotionEntry entry;
            entry.group = groupName;
            entry.no = m;
            m_motionEntries.append(entry);
        }
    }

    // 表情
    int exprCount = setting->GetExpressionCount();
    for(int i = 0; i < exprCount; i++){
        const char* name = setting->GetExpressionName(i);
        if (!name || !*name) continue;
        m_exprList->addItem(name);
        ExpressionEntry entry;
        entry.name = QString::fromUtf8(name);
        const char* fileName = setting->GetExpressionFileName(i);
        if (fileName && *fileName) {
            const QString home = QString::fromUtf8(model->GetModelHomeDir().GetRawString());
            entry.path = QDir(home).filePath(QString::fromUtf8(fileName));
        }
        m_exprEntries.append(entry);
    }

    if(m_motionEntries.isEmpty()) m_motionList->addItem("(此模型无动作定义)");
    if(m_exprEntries.isEmpty()) m_exprList->addItem("(此模型无表情定义)");
}

void CMotionPanel::RecordParameterValue(const QString& parameterId, float value)
{
    if (!m_timeline.IsRecording() || (m_timelineTimer && m_timelineTimer->isActive())) return;
    const int before = m_timeline.PointCount();
    m_timeline.Record(parameterId, value);
    if (m_timeline.PointCount() != before) {
        RefreshTimelineTable();
        m_timelineStatus->setText(QStringLiteral("正在录制 · %1 个关键帧").arg(m_timeline.PointCount()));
        emit MotionTimelineChanged();
    }
}

void CMotionPanel::RefreshTimelineTable()
{
    if (!m_timelineTable) return;
    QVector<TimelineRow> points;
    for (auto it = m_timeline.Curves().cbegin(); it != m_timeline.Curves().cend(); ++it)
        for (int index = 0; index < it.value().size(); ++index)
            points.append({it.key(), index, it.value().at(index), m_timeline.SegmentType(it.key(), index)});
    std::sort(points.begin(), points.end(), [](const auto& left, const auto& right){
        if (!qFuzzyCompare(left.point.x() + 1.0, right.point.x() + 1.0)) return left.point.x() < right.point.x();
        return left.parameterId < right.parameterId;
    });
    m_timelineRows = points;
    if (m_curveView) {
        m_curveView->SetTimeline(&m_timeline);
        m_curveView->SetTime(m_timelineTime);
    }
    if (m_playTimelineButton) m_playTimelineButton->setEnabled(!points.isEmpty());
    if (m_timelinePlayhead) m_timelinePlayhead->setEnabled(!points.isEmpty());
    if (m_timelineTimeLabel) m_timelineTimeLabel->setText(QStringLiteral("%1s").arg(m_timelineTime, 0, 'f', 3));
    m_updatingTimeline = true;
    m_timelineTable->setRowCount(points.size());
    for (int row = 0; row < points.size(); ++row) {
        m_timelineTable->setItem(row, 0, new QTableWidgetItem(QString::number(points[row].point.x(), 'f', 3)));
        auto* parameter = new QTableWidgetItem(points[row].parameterId);
        parameter->setFlags(parameter->flags() & ~Qt::ItemIsEditable);
        m_timelineTable->setItem(row, 1, parameter);
        m_timelineTable->setItem(row, 2, new QTableWidgetItem(QString::number(points[row].point.y(), 'g', 6)));
        auto* interpolation = new QTableWidgetItem(points[row].pointIndex == 0
            ? QStringLiteral("起点") : points[row].segmentType == 1 ? QStringLiteral("Bézier")
            : points[row].segmentType == 2 ? QStringLiteral("阶梯")
            : points[row].segmentType == 3 ? QStringLiteral("反阶梯") : QStringLiteral("线性"));
        if (points[row].pointIndex == 0) interpolation->setFlags(interpolation->flags() & ~Qt::ItemIsEditable);
        interpolation->setToolTip(QStringLiteral("0 线性，1 Bézier，2 阶梯，3 反阶梯"));
        m_timelineTable->setItem(row, 3, interpolation);
    }
    m_updatingTimeline = false;
}

void CMotionPanel::ToggleTimelinePlayback()
{
    if (!m_timelineTimer || m_timeline.Curves().isEmpty()) return;
    if (m_timelineTimer->isActive()) {
        m_timelineTimer->stop();
        m_playTimelineButton->setText(QStringLiteral("预览"));
        return;
    }
    m_timelineTime = 0.0f;
    m_timelineTimer->start();
    m_playTimelineButton->setText(QStringLiteral("停止"));
    if (m_timelinePlayhead) {
        QSignalBlocker blocker(m_timelinePlayhead);
        m_timelinePlayhead->setValue(0);
    }
    if (m_curveView) m_curveView->SetTime(m_timelineTime);
    emit MotionTimelinePreviewChanged(m_timeline.EvaluateAll(m_timelineTime));
}

void CMotionPanel::TickTimelinePlayback()
{
    const float duration = m_timeline.Duration();
    if (duration <= 0.0f) {
        m_timelineTimer->stop();
        m_playTimelineButton->setText(QStringLiteral("预览"));
        return;
    }
    m_timelineTime += 0.033f;
    if (m_timelineTime > duration) {
        if (m_loopCb && m_loopCb->isChecked()) m_timelineTime = 0.0f;
        else {
            m_timelineTime = duration;
            m_timelineTimer->stop();
            m_playTimelineButton->setText(QStringLiteral("预览"));
        }
    }
    if (m_timelinePlayhead) {
        QSignalBlocker blocker(m_timelinePlayhead);
        m_timelinePlayhead->setValue(duration > 0.0f
            ? qBound(0, qRound(m_timelineTime * 1000.0f / duration), 1000) : 0);
    }
    if (m_timelineTimeLabel) m_timelineTimeLabel->setText(QStringLiteral("%1s").arg(m_timelineTime, 0, 'f', 3));
    if (m_curveView) m_curveView->SetTime(m_timelineTime);
    emit MotionTimelinePreviewChanged(m_timeline.EvaluateAll(m_timelineTime));
}

void CMotionPanel::SetTimelineTimeFromSlider(int value)
{
    if (!m_timelinePlayhead || m_timeline.Curves().isEmpty()) return;
    if (m_timelineTimer && m_timelineTimer->isActive()) {
        m_timelineTimer->stop();
        if (m_playTimelineButton) m_playTimelineButton->setText(QStringLiteral("预览"));
    }
    const float duration = m_timeline.Duration();
    m_timelineTime = duration * qBound(0, value, 1000) / 1000.0f;
    if (m_timelineTimeLabel) m_timelineTimeLabel->setText(QStringLiteral("%1s").arg(m_timelineTime, 0, 'f', 3));
    if (m_curveView) m_curveView->SetTime(m_timelineTime);
    emit MotionTimelinePreviewChanged(m_timeline.EvaluateAll(m_timelineTime));
}

void CMotionPanel::ApplyTimelineCurveEdit(const QString& parameterId, int pointIndex, float time, float value)
{
    if (m_updatingTimeline || !m_timeline.UpdatePoint(parameterId, pointIndex, time, value)) return;
    RefreshTimelineTable();
    emit MotionTimelineChanged();
}

void CMotionPanel::ApplyTimelineCellEdit(int row, int column)
{
    if (m_updatingTimeline || row < 0 || row >= m_timelineRows.size() ||
        (column != 0 && column != 2 && column != 3)) return;
    const TimelineRow reference = m_timelineRows.at(row);
    if (column == 3) {
        const QString text = m_timelineTable->item(row, column)
            ? m_timelineTable->item(row, column)->text().trimmed().toLower() : QString();
        int segmentType = -1;
        if (text == QStringLiteral("0") || text == QStringLiteral("线性") || text == QStringLiteral("linear")) segmentType = 0;
        else if (text == QStringLiteral("1") || text == QStringLiteral("bezier") || text == QStringLiteral("bézier")) segmentType = 1;
        else if (text == QStringLiteral("2") || text == QStringLiteral("阶梯") || text == QStringLiteral("stepped")) segmentType = 2;
        else if (text == QStringLiteral("3") || text == QStringLiteral("反阶梯") || text == QStringLiteral("inverse stepped")) segmentType = 3;
        if (segmentType < 0 || !m_timeline.SetSegmentType(reference.parameterId, reference.pointIndex, segmentType)) {
            RefreshTimelineTable();
            return;
        }
        RefreshTimelineTable();
        emit MotionTimelineChanged();
        return;
    }
    const float oldValue = column == 0 ? static_cast<float>(reference.point.x())
                                       : static_cast<float>(reference.point.y());
    bool ok = false;
    const float value = m_timelineTable->item(row, column)
        ? static_cast<float>(m_timelineTable->item(row, column)->text().toDouble(&ok)) : oldValue;
    if (!ok || !std::isfinite(value) ||
        !m_timeline.UpdatePoint(reference.parameterId, reference.pointIndex,
                                column == 0 ? value : static_cast<float>(reference.point.x()),
                                column == 2 ? value : static_cast<float>(reference.point.y()))) {
        RefreshTimelineTable();
        return;
    }
    RefreshTimelineTable();
    emit MotionTimelineChanged();
}

void CMotionPanel::RemoveTimelineKeyframe()
{
    if (!m_timelineTable || m_timelineTable->currentRow() < 0 ||
        m_timelineTable->currentRow() >= m_timelineRows.size()) return;
    const TimelineRow reference = m_timelineRows.at(m_timelineTable->currentRow());
    if (!m_timeline.RemovePoint(reference.parameterId, reference.pointIndex)) return;
    RefreshTimelineTable();
    m_timelineStatus->setText(m_timeline.PointCount() > 0
                              ? QStringLiteral("已录制 %1 个关键帧").arg(m_timeline.PointCount())
                              : QStringLiteral("暂无关键帧"));
    emit MotionTimelineChanged();
}

void CMotionPanel::ClearTimeline()
{
    if (m_timelineTimer) m_timelineTimer->stop();
    if (m_playTimelineButton) m_playTimelineButton->setText(QStringLiteral("预览"));
    if (m_recordTimelineCb) m_recordTimelineCb->setChecked(false);
    m_timeline.Clear();
    RefreshTimelineTable();
    if (m_timelineStatus) m_timelineStatus->setText(QStringLiteral("暂无关键帧"));
    emit MotionTimelineChanged();
}

void CMotionPanel::ExportTimeline()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出 motion3"),
                                                       QString(), QStringLiteral("Motion (*.motion3.json)"));
    if (path.isEmpty()) return;
    QString error;
    if (!m_timeline.Save(path, 30.0f, m_loopCb && m_loopCb->isChecked(), &error)) {
        QMessageBox::warning(this, QStringLiteral("导出 motion3 失败"), error);
        return;
    }
    m_timelineStatus->setText(QStringLiteral("已导出 motion3"));
}

void CMotionPanel::ImportTimeline()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入 motion3"),
                                                       QString(), QStringLiteral("Motion (*.motion3.json)"));
    if (path.isEmpty()) return;
    QString error;
    if (!m_timeline.Load(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("导入 motion3 失败"), error);
        return;
    }
    if (m_timelineTimer) m_timelineTimer->stop();
    if (m_playTimelineButton) m_playTimelineButton->setText(QStringLiteral("预览"));
    m_timelineTime = 0.0f;
    RefreshTimelineTable();
    m_timelineStatus->setText(QStringLiteral("已导入 %1 个关键帧").arg(m_timeline.PointCount()));
    emit MotionTimelineChanged();
}

bool CMotionPanel::BuildPoseGroups(const QString& poseJsonPath, QString* error)
{
    m_poseJsonPath = poseJsonPath;
    m_poseDocument = {};
    m_poseGroups.clear();
    m_activePoseGroup = -1;
    m_poseDirty = false;
    m_savePoseButton->setText(QStringLiteral("保存 Pose"));
    m_addPosePartButton->setEnabled(false);
    m_removePosePartButton->setEnabled(false);
    m_editPoseLinksButton->setEnabled(false);
    m_savePoseButton->setEnabled(false);
    // 清除 poseWidget 中旧的 combo
    QLayout* layout = m_poseWidget->layout();
    while(layout->count() > 1){ // 保留标签
        QLayoutItem* item = layout->takeAt(1);
        if (QWidget* widget = item->widget()) {
            delete widget;
        } else if (QLayout* child = item->layout()) {
            clearLayoutItems(child);
            delete child;
        }
        delete item;
    }

    if(poseJsonPath.isEmpty()){
        m_poseWidget->setVisible(false);
        return true;
    }

    QFile file(poseJsonPath);
    if(!file.open(QIODevice::ReadOnly)){
        m_poseWidget->setVisible(false);
        if (error) *error = QStringLiteral("无法打开 Pose 配置：%1").arg(file.errorString());
        return false;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError || doc.isNull() || !doc.isObject()) {
        m_poseWidget->setVisible(false);
        if (error) *error = QStringLiteral("Pose 配置 JSON 无效：%1").arg(parseError.errorString());
        return false;
    }
    m_poseDocument = doc.object();
    QJsonArray groups = doc.object()["Groups"].toArray();
    if(groups.isEmpty()){
        m_poseWidget->setVisible(false);
        return true;
    }

    for(int g = 0; g < groups.size(); g++){
        QJsonArray group = groups[g].toArray();
        if(group.size() < 2) continue; // 只有一个选项没意义

        PoseGroup pg;
        pg.sourceIndex = g;
        QComboBox* combo = new QComboBox();
        for(int i = 0; i < group.size(); i++){
            const QJsonObject part = group[i].toObject();
            QString partId = part["Id"].toString();
            if(partId.isEmpty()) continue;
            pg.partIds << partId;
            QStringList links;
            for (const QJsonValue& link : part["Link"].toArray()) {
                const QString linkId = link.toString();
                if (!linkId.isEmpty() && linkId != partId && !links.contains(linkId)) links.append(linkId);
            }
            pg.links.append(links);
            combo->addItem(QString("组%1: %2").arg(g + 1).arg(partId), partId);
        }
        if (pg.partIds.size() < 2) {
            delete combo;
            continue;
        }
        pg.combo = combo;
        m_poseGroups.append(pg);
        const int poseIndex = m_poseGroups.size() - 1;
        layout->addWidget(combo);

        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, poseIndex](int index){
            if(poseIndex >= m_poseGroups.size()) return;
            m_activePoseGroup = poseIndex;
            m_addPosePartButton->setEnabled(true);
            m_removePosePartButton->setEnabled(m_poseGroups[poseIndex].partIds.size() > 1);
            m_editPoseLinksButton->setEnabled(true);
            EmitPoseSelection(poseIndex, index);
        });
    }

    m_poseWidget->setVisible(!m_poseGroups.isEmpty());
    m_savePoseButton->setEnabled(!m_poseGroups.isEmpty());
    if (!m_poseGroups.isEmpty()) {
        m_activePoseGroup = 0;
        m_addPosePartButton->setEnabled(true);
        m_removePosePartButton->setEnabled(m_poseGroups[0].partIds.size() > 1);
        m_editPoseLinksButton->setEnabled(true);
    }
    return true;
}

void CMotionPanel::EmitPoseSelection(int poseIndex, int optionIndex)
{
    if (poseIndex < 0 || poseIndex >= m_poseGroups.size()) return;
    const PoseGroup& group = m_poseGroups.at(poseIndex);
    if (optionIndex < 0 || optionIndex >= group.partIds.size()) return;
    QStringList hideIds;
    const QString showId = group.partIds.at(optionIndex);
    QStringList showLinks = optionIndex < group.links.size() ? group.links.at(optionIndex) : QStringList();
    for (int i = 0; i < group.partIds.size(); ++i) {
        if (i == optionIndex) continue;
        hideIds << group.partIds.at(i);
        if (i < group.links.size()) hideIds.append(group.links.at(i));
    }
    hideIds.removeDuplicates();
    showLinks.removeDuplicates();
    hideIds.removeAll(showId);
    for (const QString& link : showLinks) hideIds.removeAll(link);
    QStringList showIds{showId};
    showIds.append(showLinks);
    showIds.removeDuplicates();
    emit PosePartChanged(showIds, hideIds);
}

void CMotionPanel::EditPoseLinks()
{
    if (m_activePoseGroup < 0 || m_activePoseGroup >= m_poseGroups.size()) return;
    PoseGroup& group = m_poseGroups[m_activePoseGroup];
    const int optionIndex = group.combo ? group.combo->currentIndex() : -1;
    if (optionIndex < 0 || optionIndex >= group.partIds.size()) return;
    const QString currentId = group.partIds.at(optionIndex);

    QStringList candidates;
    QSet<QString> candidateSet;
    LAppLive2DManager* manager = LAppLive2DManager::GetInstance();
    LAppModel* model = manager && manager->GetModelNum() > 0 ? manager->GetModel(0) : nullptr;
    if (model && model->GetModel()) {
        for (int i = 0; i < model->GetModel()->GetPartCount(); ++i) {
            const CubismId* partId = model->GetModel()->GetPartId(i);
            if (!partId) continue;
            const QString id = QString::fromUtf8(partId->GetString().GetRawString());
            if (!id.isEmpty() && !candidateSet.contains(id)) {
                candidateSet.insert(id);
                candidates.append(id);
            }
        }
    }
    for (const QString& id : group.partIds) if (!candidateSet.contains(id)) { candidateSet.insert(id); candidates.append(id); }
    for (const QStringList& links : group.links)
        for (const QString& id : links) if (!candidateSet.contains(id)) { candidateSet.insert(id); candidates.append(id); }
    candidates.sort(Qt::CaseInsensitive);

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("编辑 Link：%1").arg(currentId));
    dialog.resize(360, 420);
    auto* layout = new QVBoxLayout(&dialog);
    auto* list = new QListWidget(&dialog);
    const QStringList currentLinks = optionIndex < group.links.size() ? group.links.at(optionIndex) : QStringList();
    for (const QString& id : candidates) {
        if (id == currentId) continue;
        auto* item = new QListWidgetItem(id, list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(currentLinks.contains(id) ? Qt::Checked : Qt::Unchecked);
    }
    layout->addWidget(list);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    QStringList links;
    for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem* item = list->item(row);
        if (item && item->checkState() == Qt::Checked) links.append(item->text());
    }
    links.removeDuplicates();
    if (group.links.size() < group.partIds.size()) group.links.resize(group.partIds.size());
    if (group.links.at(optionIndex) == links) return;
    group.links[optionIndex] = links;
    m_savePoseButton->setText(QStringLiteral("保存 Pose"));
    m_poseDirty = true;
    emit PoseConfigChanged();
    EmitPoseSelection(m_activePoseGroup, optionIndex);
}

bool CMotionPanel::SavePose(QString* error)
{
    if (m_poseJsonPath.isEmpty() || m_poseDocument.isEmpty()) {
        if (error) *error = QStringLiteral("没有可保存的 Pose 配置");
        return false;
    }
    QJsonArray groups = m_poseDocument.value(QStringLiteral("Groups")).toArray();
    for (const PoseGroup& group : m_poseGroups) {
        QJsonArray entries;
        for (int i = 0; i < group.partIds.size(); ++i) {
            QJsonObject entry;
            entry.insert(QStringLiteral("Id"), group.partIds[i]);
            if (i < group.links.size() && !group.links[i].isEmpty()) {
                QJsonArray links;
                for (const QString& link : group.links[i]) links.append(link);
                entry.insert(QStringLiteral("Link"), links);
            }
            entries.append(entry);
        }
        if (group.sourceIndex >= 0 && group.sourceIndex < groups.size())
            groups[group.sourceIndex] = entries;
        else
            groups.append(entries);
    }
    m_poseDocument.insert(QStringLiteral("Groups"), groups);
    const QByteArray data = QJsonDocument(m_poseDocument).toJson(QJsonDocument::Indented);
    QSaveFile file(m_poseJsonPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error) *error = QStringLiteral("无法写入文件：%1").arg(file.errorString());
        return false;
    }
    m_savePoseButton->setText(QStringLiteral("已保存 Pose"));
    m_poseDirty = false;
    emit PoseSaved();
    return true;
}

void CMotionPanel::AddPosePart()
{
    if (m_activePoseGroup < 0 || m_activePoseGroup >= m_poseGroups.size()) return;
    bool accepted = false;
    const QString partId = QInputDialog::getText(this, QStringLiteral("添加 Pose Part"),
                                                  QStringLiteral("Part ID"), QLineEdit::Normal,
                                                  QString(), &accepted).trimmed();
    if (!accepted || partId.isEmpty()) return;
    PoseGroup& group = m_poseGroups[m_activePoseGroup];
    if (group.partIds.contains(partId)) {
        QMessageBox::information(this, QStringLiteral("添加 Pose Part"), QStringLiteral("该 Part 已在当前组中"));
        return;
    }
    group.partIds.append(partId);
    group.links.append(QStringList());
    group.combo->addItem(partId, partId);
    group.combo->setCurrentIndex(group.combo->count() - 1);
    m_removePosePartButton->setEnabled(group.partIds.size() > 1);
    m_savePoseButton->setText(QStringLiteral("保存 Pose"));
    m_poseDirty = true;
    emit PoseConfigChanged();
}

void CMotionPanel::RemovePosePart()
{
    if (m_activePoseGroup < 0 || m_activePoseGroup >= m_poseGroups.size()) return;
    PoseGroup& group = m_poseGroups[m_activePoseGroup];
    const int index = group.combo->currentIndex();
    if (index < 0 || index >= group.partIds.size() || group.partIds.size() <= 1) return;
    group.partIds.removeAt(index);
    if (index < group.links.size()) group.links.removeAt(index);
    group.combo->removeItem(index);
    m_removePosePartButton->setEnabled(group.partIds.size() > 1);
    m_savePoseButton->setText(QStringLiteral("保存 Pose"));
    m_poseDirty = true;
    emit PoseConfigChanged();
}

void CMotionPanel::EditSelectedExpression()
{
    const int expressionIndex = m_exprList->currentRow();
    if (expressionIndex < 0 || expressionIndex >= m_exprEntries.size()) return;
    const ExpressionEntry entry = m_exprEntries.at(expressionIndex);
    if (entry.path.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("编辑表情失败"), QStringLiteral("当前表情没有有效文件路径"));
        return;
    }

    QFile file(entry.path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("编辑表情失败"), file.errorString());
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument source = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError || !source.isObject()) {
        QMessageBox::warning(this, QStringLiteral("编辑表情失败"),
                             QStringLiteral("Expression JSON 无效：%1").arg(parseError.errorString()));
        return;
    }
    const QJsonObject root = source.object();
    const QJsonValue parametersValue = root.value(QStringLiteral("Parameters"));
    if (!parametersValue.isArray()) {
        QMessageBox::warning(this, QStringLiteral("编辑表情失败"), QStringLiteral("Expression 缺少 Parameters 数组"));
        return;
    }
    const QJsonArray sourceParameters = parametersValue.toArray();

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("编辑表情：%1").arg(entry.name));
    dialog.resize(560, 420);
    auto* layout = new QVBoxLayout(&dialog);
    auto* table = new QTableWidget(&dialog);
    table->setColumnCount(3);
    table->setHorizontalHeaderLabels({QStringLiteral("参数 ID"), QStringLiteral("值"), QStringLiteral("Blend")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->verticalHeader()->setVisible(false);
    table->setRowCount(sourceParameters.size());
    const QStringList blendTypes = {QStringLiteral("Add"), QStringLiteral("Multiply"), QStringLiteral("Overwrite")};
    for (int row = 0; row < sourceParameters.size(); ++row) {
        const QJsonValue parameterValue = sourceParameters.at(row);
        if (!parameterValue.isObject()) {
            QMessageBox::warning(this, QStringLiteral("编辑表情失败"),
                                 QStringLiteral("Expression 参数 %1 不是对象").arg(row));
            return;
        }
        const QJsonObject parameter = parameterValue.toObject();
        const QString id = parameter.value(QStringLiteral("Id")).toString();
        const QJsonValue value = parameter.value(QStringLiteral("Value"));
        if (id.isEmpty() || !value.isDouble() || !std::isfinite(value.toDouble())) {
            QMessageBox::warning(this, QStringLiteral("编辑表情失败"),
                                 QStringLiteral("Expression 参数 %1 无效").arg(row));
            return;
        }
        auto* idItem = new QTableWidgetItem(id);
        idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, 0, idItem);
        table->setItem(row, 1, new QTableWidgetItem(QString::number(value.toDouble(), 'g', 9)));
        auto* blend = new QComboBox(table);
        const QString blendType = parameter.value(QStringLiteral("Blend")).toString(QStringLiteral("Add"));
        if (!blendTypes.contains(blendType)) {
            QMessageBox::warning(this, QStringLiteral("编辑表情失败"),
                                 QStringLiteral("Expression 参数 %1 的 Blend 无效").arg(row));
            return;
        }
        blend->addItems(blendTypes);
        blend->setCurrentText(blendType);
        table->setCellWidget(row, 2, blend);
    }
    layout->addWidget(table);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    QJsonArray parameters = sourceParameters;
    QSet<QString> ids;
    for (int row = 0; row < table->rowCount(); ++row) {
        const QString id = table->item(row, 0) ? table->item(row, 0)->text().trimmed() : QString();
        bool ok = false;
        const double value = table->item(row, 1) ? table->item(row, 1)->text().toDouble(&ok) : 0.0;
        auto* blend = qobject_cast<QComboBox*>(table->cellWidget(row, 2));
        if (id.isEmpty() || ids.contains(id) || !ok || !std::isfinite(value) || !blend) {
            QMessageBox::warning(this, QStringLiteral("保存表情失败"),
                                 QStringLiteral("第 %1 行的参数值或 ID 无效").arg(row + 1));
            return;
        }
        ids.insert(id);
        QJsonObject parameter = parameters.at(row).toObject();
        parameter.insert(QStringLiteral("Id"), id);
        parameter.insert(QStringLiteral("Value"), value);
        parameter.insert(QStringLiteral("Blend"), blend->currentText());
        parameters[row] = parameter;
    }

    QJsonObject updated = root;
    updated.insert(QStringLiteral("Parameters"), parameters);
    const QByteArray data = QJsonDocument(updated).toJson(QJsonDocument::Indented);
    QSaveFile output(entry.path);
    if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size() || !output.commit()) {
        QMessageBox::warning(this, QStringLiteral("保存表情失败"), output.errorString());
        return;
    }

    LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
    LAppModel* model = mgr && mgr->GetModelNum() > 0 ? mgr->GetModel(0) : nullptr;
    if (!model || !model->ReloadExpressionFromFile(entry.name, entry.path)) {
        QMessageBox::warning(this, QStringLiteral("表情已保存但运行时刷新失败"),
                             QStringLiteral("下次重新打开模型后会读取新配置"));
    }
}
