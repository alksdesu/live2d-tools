#include "cparampanel.h"
#include "live2d/LAppLive2DManager.hpp"
#include "live2d/LAppModel.hpp"
#include "ui/para/ccidjson.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>

using namespace Live2D::Cubism::Framework;

CParamPanel::CParamPanel(QWidget *parent)
    : QDockWidget("参数控制", parent)
{
    QWidget* wrapper = new QWidget(this);
    QVBoxLayout* mainLayout = new QVBoxLayout(wrapper);

    QPushButton* resetBtn = new QPushButton("重置全部", wrapper);
    resetBtn->setObjectName("primaryBtn");
    connect(resetBtn, &QPushButton::clicked, this, &CParamPanel::ResetAll);
    mainLayout->addWidget(resetBtn);

    m_scroll = new QScrollArea(wrapper);
    m_scroll->setWidgetResizable(true);
    m_container = new QWidget();
    m_scroll->setWidget(m_container);
    mainLayout->addWidget(m_scroll);

    setWidget(wrapper);
}

void CParamPanel::BuildParams(CCidJson* cid, const QSet<QString>* blendShapeParams)
{
    m_rows.clear();

    // 删除旧 layout
    if(m_container->layout()){
        QLayout* old = m_container->layout();
        QLayoutItem* item;
        while((item = old->takeAt(0)) != nullptr){
            delete item->widget();
            delete item;
        }
        delete old;
    }

    QVBoxLayout* layout = new QVBoxLayout(m_container);

    LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
    if(!mgr || mgr->GetModelNum() == 0) return;

    LAppModel* model = mgr->GetModel(0);
    if(!model) return;

    CubismModel* cubismModel = model->GetModel();
    if(!cubismModel) return;

    // 收集所有参数信息
    int count = cubismModel->GetParameterCount();
    QMap<QString, int> paramIdToModelIndex;
    for(int i = 0; i < count; i++){
        QString paramId = cubismModel->GetParameterId(i)->GetString().GetRawString();
        paramIdToModelIndex[paramId] = i;
    }

    // 添加一行参数滑条的 lambda
    auto addParamRow = [&](const QString& paramId, int modelIdx){
        float minVal = cubismModel->GetParameterMinimumValue(modelIdx);
        float maxVal = cubismModel->GetParameterMaximumValue(modelIdx);
        float defVal = cubismModel->GetParameterDefaultValue(modelIdx);

        QHBoxLayout* row = new QHBoxLayout();

        // 显示名称：优先用 cid 映射的名称，否则回退到内部 ID
        QString displayName = paramId;
        if(cid){
            QString mapped = cid->getParaName(paramId);
            if(!mapped.isEmpty()) displayName = mapped;
        }

        // 融合变形参数标记
        bool isBlendShape = (blendShapeParams && blendShapeParams->contains(paramId))
                         || (cid && cid->isCombinedParameter(paramId));
        if(isBlendShape){
            displayName = QString("[融合] %1").arg(displayName);
        }

        QLabel* nameLabel = new QLabel(displayName, m_container);
        nameLabel->setFixedWidth(150);
        nameLabel->setToolTip(paramId);
        if(isBlendShape){
            nameLabel->setStyleSheet("background-color: #FFF3CD; padding: 2px; border-radius: 2px;");
        }
        row->addWidget(nameLabel);

        QSlider* slider = new QSlider(Qt::Horizontal, m_container);
        slider->setRange(0, 1000);
        int sliderDefault = 0;
        if(maxVal > minVal){
            sliderDefault = static_cast<int>((defVal - minVal) / (maxVal - minVal) * 1000.0f);
        }
        slider->setValue(sliderDefault);
        row->addWidget(slider);

        QLabel* valLabel = new QLabel(QString::number(defVal, 'f', 2), m_container);
        valLabel->setFixedWidth(60);
        row->addWidget(valLabel);

        ParamRow pr;
        pr.slider = slider;
        pr.valueLabel = valLabel;
        pr.minVal = minVal;
        pr.maxVal = maxVal;
        pr.defaultVal = defVal;
        pr.paramId = paramId;
        pr.modelIndex = modelIdx;
        m_rows.append(pr);

        int idx = m_rows.size() - 1;
        connect(slider, &QSlider::sliderPressed, this, [this, idx](){ m_userDragging.insert(idx); });
        connect(slider, &QSlider::sliderReleased, this, [this, idx](){
            m_userDragging.remove(idx);
            m_userLocked.insert(idx); // 用户改过的参数锁定，不再被 sync 覆盖
        });
        connect(slider, &QSlider::valueChanged, this, [this, idx](int val){
            ParamRow& r = m_rows[idx];
            float realVal = r.minVal + (r.maxVal - r.minVal) * val / 1000.0f;
            r.valueLabel->setText(QString::number(realVal, 'f', 2));
            emit ParamChanged(r.paramId, realVal, false);
        });

        layout->addLayout(row);
    };

    // 有分组信息时按组显示
    if(cid && !cid->getGroups().isEmpty()){
        QSet<QString> addedParams;
        for(const CVetGroup& group : cid->getGroups()){
            // 组标题
            QString groupName;
            const QMap<QString,QString>& groupMap = cid->getGroupIdToName();
            if(groupMap.contains(group.strGroup))
                groupName = groupMap[group.strGroup];
            else
                groupName = group.strGroup;

            QLabel* groupLabel = new QLabel(groupName, m_container);
            QFont boldFont = groupLabel->font();
            boldFont.setBold(true);
            boldFont.setPointSize(boldFont.pointSize() + 1);
            groupLabel->setFont(boldFont);
            groupLabel->setStyleSheet("margin-top: 8px; margin-bottom: 4px; color: #4A90D9;");
            layout->addWidget(groupLabel);

            for(const QString& paramId : group.vetParam){
                if(!paramIdToModelIndex.contains(paramId)) continue;
                addParamRow(paramId, paramIdToModelIndex[paramId]);
                addedParams.insert(paramId);
            }
        }

        // 未分组的参数放到最后
        for(int i = 0; i < count; i++){
            QString paramId = cubismModel->GetParameterId(i)->GetString().GetRawString();
            if(!addedParams.contains(paramId)){
                addParamRow(paramId, i);
            }
        }
    } else {
        // 无分组信息，全部平铺
        for(int i = 0; i < count; i++){
            QString paramId = cubismModel->GetParameterId(i)->GetString().GetRawString();
            addParamRow(paramId, i);
        }
    }

    layout->addStretch();
}

void CParamPanel::ResetAll()
{
    m_userLocked.clear();
    m_userDragging.clear();
    for(auto& r : m_rows){
        int sliderDefault = 0;
        if(r.maxVal > r.minVal){
            sliderDefault = static_cast<int>((r.defaultVal - r.minVal) / (r.maxVal - r.minVal) * 1000.0f);
        }
        r.slider->setValue(sliderDefault);
    }
}

void CParamPanel::SyncFromModel()
{
    LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
    if(!mgr || mgr->GetModelNum() == 0) return;

    LAppModel* model = mgr->GetModel(0);
    if(!model) return;

    CubismModel* cubismModel = model->GetModel();
    if(!cubismModel) return;

    for(int i = 0; i < m_rows.size(); i++){
        auto& r = m_rows[i];
        if(r.modelIndex < 0) continue;
        if(m_userDragging.contains(i) || m_userLocked.contains(i)) continue;
        float curVal = cubismModel->GetParameterValue(r.modelIndex);
        int sliderVal = 0;
        if(r.maxVal > r.minVal){
            sliderVal = static_cast<int>((curVal - r.minVal) / (r.maxVal - r.minVal) * 1000.0f);
            sliderVal = qBound(0, sliderVal, 1000);
        }
        r.slider->blockSignals(true);
        r.slider->setValue(sliderVal);
        r.slider->blockSignals(false);
        r.valueLabel->setText(QString::number(curVal, 'f', 2));
    }
}
