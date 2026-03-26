#include "cmotionpanel.h"
#include "live2d/LAppLive2DManager.hpp"
#include "live2d/LAppModel.hpp"
#include <QDebug>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QSplitter>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

using namespace Live2D::Cubism::Framework;

CMotionPanel::CMotionPanel(QWidget *parent)
    : QDockWidget("动作/表情", parent)
{
    QWidget* wrapper = new QWidget(this);
    QVBoxLayout* mainLayout = new QVBoxLayout(wrapper);

    QSplitter* splitter = new QSplitter(Qt::Vertical, wrapper);

    // 动作部分
    QWidget* motionWidget = new QWidget();
    QVBoxLayout* motionLayout = new QVBoxLayout(motionWidget);
    motionLayout->addWidget(new QLabel("动作列表"));
    m_motionList = new QListWidget();
    motionLayout->addWidget(m_motionList);
    QPushButton* playBtn = new QPushButton("▶ 播放");
    playBtn->setObjectName("primaryBtn");
    motionLayout->addWidget(playBtn);
    splitter->addWidget(motionWidget);

    connect(playBtn, &QPushButton::clicked, this, [this](){
        int row = m_motionList->currentRow();
        if(row >= 0 && row < m_motionEntries.size()){
            emit PlayMotion(m_motionEntries[row].group, m_motionEntries[row].no);
        }
    });

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
    m_exprList = new QListWidget();
    exprLayout->addWidget(m_exprList);
    QPushButton* applyBtn = new QPushButton("✦ 应用");
    applyBtn->setObjectName("primaryBtn");
    exprLayout->addWidget(applyBtn);
    splitter->addWidget(exprWidget);

    // Pose 区域
    m_poseWidget = new QWidget();
    QVBoxLayout* poseLayout = new QVBoxLayout(m_poseWidget);
    poseLayout->addWidget(new QLabel("Pose 切换"));
    m_poseWidget->setVisible(false); // 没有 pose 数据时隐藏
    splitter->addWidget(m_poseWidget);

    connect(applyBtn, &QPushButton::clicked, this, [this](){
        int row = m_exprList->currentRow();
        if(row >= 0 && row < m_exprNames.size()){
            emit ApplyExpression(m_exprNames[row]);
        }
    });

    connect(m_exprList, &QListWidget::itemDoubleClicked, this, [this](){
        int row = m_exprList->currentRow();
        if(row >= 0 && row < m_exprNames.size()){
            emit ApplyExpression(m_exprNames[row]);
        }
    });

    mainLayout->addWidget(splitter);

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
    optLayout->addWidget(m_eyeBlinkCb);
    optLayout->addWidget(m_breathCb);
    optLayout->addWidget(m_mouseFollowCb);
    optLayout->addWidget(m_physicsCb);
    mainLayout->addLayout(optLayout);

    connect(m_eyeBlinkCb, &QCheckBox::toggled, this, &CMotionPanel::AutoEyeBlinkChanged);
    connect(m_breathCb, &QCheckBox::toggled, this, &CMotionPanel::AutoBreathChanged);
    connect(m_mouseFollowCb, &QCheckBox::toggled, this, &CMotionPanel::MouseFollowChanged);
    connect(m_physicsCb, &QCheckBox::toggled, this, &CMotionPanel::PhysicsChanged);

    setWidget(wrapper);
}

void CMotionPanel::BuildLists()
{
    m_motionList->clear();
    m_exprList->clear();
    m_motionEntries.clear();
    m_exprNames.clear();

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
        m_exprList->addItem(name);
        m_exprNames.append(name);
    }

    if(m_motionEntries.isEmpty()) m_motionList->addItem("(此模型无动作定义)");
    if(m_exprNames.isEmpty()) m_exprList->addItem("(此模型无表情定义)");
}

void CMotionPanel::BuildPoseGroups(const QString& poseJsonPath)
{
    m_poseGroups.clear();
    // 清除 poseWidget 中旧的 combo
    QLayout* layout = m_poseWidget->layout();
    while(layout->count() > 1){ // 保留标签
        QLayoutItem* item = layout->takeAt(1);
        if(item->widget()) delete item->widget();
        delete item;
    }

    if(poseJsonPath.isEmpty()){
        m_poseWidget->setVisible(false);
        return;
    }

    QFile file(poseJsonPath);
    if(!file.open(QIODevice::ReadOnly)){
        m_poseWidget->setVisible(false);
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    QJsonArray groups = doc.object()["Groups"].toArray();
    if(groups.isEmpty()){
        m_poseWidget->setVisible(false);
        return;
    }

    for(int g = 0; g < groups.size(); g++){
        QJsonArray group = groups[g].toArray();
        if(group.size() < 2) continue; // 只有一个选项没意义

        PoseGroup pg;
        QComboBox* combo = new QComboBox();
        for(int i = 0; i < group.size(); i++){
            QString partId = group[i].toObject()["Id"].toString();
            pg.partIds << partId;
            combo->addItem(QString("组%1: %2").arg(g + 1).arg(partId), partId);
        }
        pg.combo = combo;
        m_poseGroups.append(pg);
        layout->addWidget(combo);

        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, g](int index){
            if(g >= m_poseGroups.size()) return;
            const PoseGroup& pg = m_poseGroups[g];
            QStringList hideIds;
            QString showId;
            for(int i = 0; i < pg.partIds.size(); i++){
                if(i == index) showId = pg.partIds[i];
                else hideIds << pg.partIds[i];
            }
            emit PosePartChanged(showId, hideIds);
        });
    }

    m_poseWidget->setVisible(!m_poseGroups.isEmpty());
}
