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

void CParamPanel::BuildParams()
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

    int count = cubismModel->GetParameterCount();
    for(int i = 0; i < count; i++){
        QString paramId = cubismModel->GetParameterId(i)->GetString().GetRawString();
        float minVal = cubismModel->GetParameterMinimumValue(i);
        float maxVal = cubismModel->GetParameterMaximumValue(i);
        float defVal = cubismModel->GetParameterDefaultValue(i);

        QHBoxLayout* row = new QHBoxLayout();

        QLabel* nameLabel = new QLabel(paramId, m_container);
        nameLabel->setFixedWidth(150);
        nameLabel->setToolTip(paramId);
        row->addWidget(nameLabel);

        QSlider* slider = new QSlider(Qt::Horizontal, m_container);
        slider->setRange(0, 1000);
        // 将默认值映射到 slider 范围
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
        m_rows.append(pr);

        int idx = m_rows.size() - 1;
        connect(slider, &QSlider::valueChanged, this, [this, idx](int val){
            ParamRow& r = m_rows[idx];
            float realVal = r.minVal + (r.maxVal - r.minVal) * val / 1000.0f;
            r.valueLabel->setText(QString::number(realVal, 'f', 2));
            emit ParamChanged(r.paramId, realVal, false);
        });

        layout->addLayout(row);
    }

    layout->addStretch();
}

void CParamPanel::ResetAll()
{
    for(auto& r : m_rows){
        int sliderDefault = 0;
        if(r.maxVal > r.minVal){
            sliderDefault = static_cast<int>((r.defaultVal - r.minVal) / (r.maxVal - r.minVal) * 1000.0f);
        }
        r.slider->setValue(sliderDefault);
    }
}
