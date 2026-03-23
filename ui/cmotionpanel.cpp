#include "cmotionpanel.h"
#include "live2d/LAppLive2DManager.hpp"
#include "live2d/LAppModel.hpp"
#include <QDebug>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QSplitter>

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
    optLayout->addWidget(m_eyeBlinkCb);
    optLayout->addWidget(m_breathCb);
    mainLayout->addLayout(optLayout);

    connect(m_eyeBlinkCb, &QCheckBox::toggled, this, &CMotionPanel::AutoEyeBlinkChanged);
    connect(m_breathCb, &QCheckBox::toggled, this, &CMotionPanel::AutoBreathChanged);

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
}
