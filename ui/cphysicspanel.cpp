#include "cphysicspanel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QFile>
#include <QJsonDocument>
#include <QSplitter>
#include <QDebug>

CPhysicsPanel::CPhysicsPanel(QWidget *parent)
    : QDockWidget("物理设置", parent)
{
    setObjectName(QStringLiteral("physicsPanel"));
    QWidget* wrapper = new QWidget(this);
    QVBoxLayout* mainLayout = new QVBoxLayout(wrapper);

    // FPS
    m_fpsLabel = new QLabel("计算FPS: -", wrapper);
    m_fpsLabel->setObjectName(QStringLiteral("physicsFpsLabel"));
    mainLayout->addWidget(m_fpsLabel);

    // 重力/风力控制
    QGroupBox* forceBox = new QGroupBox("力场控制", wrapper);
    QVBoxLayout* forceLayout = new QVBoxLayout(forceBox);

    auto makeSliderRow = [&](const QString& label, QSlider*& slider, QLabel*& valLabel) {
        QHBoxLayout* row = new QHBoxLayout();
        row->addWidget(new QLabel(label, forceBox));
        slider = new QSlider(Qt::Horizontal, forceBox);
        slider->setRange(-100, 100);
        slider->setValue(0);
        row->addWidget(slider);
        valLabel = new QLabel("0.00", forceBox);
        valLabel->setFixedWidth(50);
        row->addWidget(valLabel);
        forceLayout->addLayout(row);
    };

    makeSliderRow("重力 X", m_gravityX, m_gravityXLabel);
    makeSliderRow("重力 Y", m_gravityY, m_gravityYLabel);
    makeSliderRow("风力 X", m_windX, m_windXLabel);
    makeSliderRow("风力 Y", m_windY, m_windYLabel);
    mainLayout->addWidget(forceBox);

    // 重力/风力信号
    auto connectSlider = [this](QSlider* slider, QLabel* label, bool isGravity, bool isX) {
        connect(slider, &QSlider::valueChanged, this, [this, label, isGravity, isX](int val){
            float fval = val / 100.0f;
            label->setText(QString::number(fval, 'f', 2));
            if(isGravity){
                float gx = m_gravityX->value() / 100.0f;
                float gy = m_gravityY->value() / 100.0f;
                emit GravityChanged(gx, gy);
            } else {
                float wx = m_windX->value() / 100.0f;
                float wy = m_windY->value() / 100.0f;
                emit WindChanged(wx, wy);
            }
        });
    };
    connectSlider(m_gravityX, m_gravityXLabel, true, true);
    connectSlider(m_gravityY, m_gravityYLabel, true, false);
    connectSlider(m_windX, m_windXLabel, false, true);
    connectSlider(m_windY, m_windYLabel, false, false);

    // 物理组列表
    QGroupBox* groupBox = new QGroupBox("物理组", wrapper);
    QVBoxLayout* groupLayout = new QVBoxLayout(groupBox);

    m_groupTable = new QTableWidget(0, 3, groupBox);
    m_groupTable->setHorizontalHeaderLabels({"ID", "名称", "输出数"});
    m_groupTable->horizontalHeader()->setStretchLastSection(true);
    m_groupTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_groupTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_groupTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_groupTable->verticalHeader()->setVisible(false);
    groupLayout->addWidget(m_groupTable);

    m_dragModeCb = new QCheckBox("拖拽驱动选中组", groupBox);
    m_dragModeCb->setToolTip("勾选后，在模型上拖拽鼠标可驱动选中物理组的输入参数");
    groupLayout->addWidget(m_dragModeCb);

    connect(m_groupTable, &QTableWidget::currentCellChanged, this, [this](int row, int, int, int){
        if(row >= 0){
            showGroupDetail(row);
            // 发射选中组的输入参数
            if(m_dragModeCb->isChecked()) emitDragParams(row);
        }
    });
    connect(m_dragModeCb, &QCheckBox::toggled, this, [this](bool checked){
        if(checked){
            int row = m_groupTable->currentRow();
            if(row >= 0) emitDragParams(row);
        } else {
            emit PhysicsDragParamsChanged(QStringList(), QStringList());
        }
    });

    mainLayout->addWidget(groupBox);

    // 详情区域（输入/输出/摆锤）
    QSplitter* detailSplitter = new QSplitter(Qt::Vertical, wrapper);

    // 输入设置
    m_inputTable = new QTableWidget(0, 4, wrapper);
    m_inputTable->setHorizontalHeaderLabels({"参数", "类型", "影响度", "反转"});
    m_inputTable->horizontalHeader()->setStretchLastSection(true);
    m_inputTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_inputTable->verticalHeader()->setVisible(false);
    detailSplitter->addWidget(m_inputTable);

    // 输出设置
    m_outputTable = new QTableWidget(0, 5, wrapper);
    m_outputTable->setHorizontalHeaderLabels({"参数", "类型", "影响度", "倍率", "反转"});
    m_outputTable->horizontalHeader()->setStretchLastSection(true);
    m_outputTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_outputTable->verticalHeader()->setVisible(false);
    detailSplitter->addWidget(m_outputTable);

    // 摆锤参数
    m_vertexTable = new QTableWidget(0, 5, wrapper);
    m_vertexTable->setHorizontalHeaderLabels({"序号", "长度", "摆幅度", "反应速度", "收敛速度"});
    m_vertexTable->horizontalHeader()->setStretchLastSection(true);
    m_vertexTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_vertexTable->verticalHeader()->setVisible(false);
    detailSplitter->addWidget(m_vertexTable);

    mainLayout->addWidget(detailSplitter);
    setWidget(wrapper);
}

void CPhysicsPanel::LoadPhysics(const QString& physicsPath)
{
    m_groupTable->setRowCount(0);
    m_inputTable->setRowCount(0);
    m_outputTable->setRowCount(0);
    m_vertexTable->setRowCount(0);

    QFile file(physicsPath);
    if(!file.open(QIODevice::ReadOnly | QIODevice::Text)){
        qDebug() << "[PhysicsPanel] Failed to open" << physicsPath;
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if(doc.isNull()) return;

    QJsonObject root = doc.object();
    QJsonObject meta = root["Meta"].toObject();

    // FPS
    int fps = meta["Fps"].toInt(30);
    m_fpsLabel->setText(QString("计算FPS: %1").arg(fps));

    // 重力/风力初始值
    QJsonObject forces = meta["EffectiveForces"].toObject();
    QJsonObject gravity = forces["Gravity"].toObject();
    QJsonObject wind = forces["Wind"].toObject();
    m_gravityX->blockSignals(true);
    m_gravityY->blockSignals(true);
    m_windX->blockSignals(true);
    m_windY->blockSignals(true);
    m_gravityX->setValue(static_cast<int>(gravity["X"].toDouble() * 100));
    m_gravityY->setValue(static_cast<int>(gravity["Y"].toDouble() * 100));
    m_windX->setValue(static_cast<int>(wind["X"].toDouble() * 100));
    m_windY->setValue(static_cast<int>(wind["Y"].toDouble() * 100));
    m_gravityXLabel->setText(QString::number(gravity["X"].toDouble(), 'f', 2));
    m_gravityYLabel->setText(QString::number(gravity["Y"].toDouble(), 'f', 2));
    m_windXLabel->setText(QString::number(wind["X"].toDouble(), 'f', 2));
    m_windYLabel->setText(QString::number(wind["Y"].toDouble(), 'f', 2));
    m_gravityX->blockSignals(false);
    m_gravityY->blockSignals(false);
    m_windX->blockSignals(false);
    m_windY->blockSignals(false);

    // 物理字典（名称映射）
    m_physicsDictionary = meta["PhysicsDictionary"].toArray();
    QMap<QString, QString> idToName;
    for(int i = 0; i < m_physicsDictionary.size(); i++){
        QJsonObject entry = m_physicsDictionary[i].toObject();
        idToName[entry["Id"].toString()] = entry["Name"].toString();
    }

    // 物理设置
    m_physicsSettings = root["PhysicsSettings"].toArray();
    m_groupTable->setRowCount(m_physicsSettings.size());

    for(int i = 0; i < m_physicsSettings.size(); i++){
        QJsonObject setting = m_physicsSettings[i].toObject();
        QString id = setting["Id"].toString();
        QString name = idToName.value(id, id);
        int outputCount = setting["Output"].toArray().size();

        m_groupTable->setItem(i, 0, new QTableWidgetItem(id));
        m_groupTable->setItem(i, 1, new QTableWidgetItem(name));
        m_groupTable->setItem(i, 2, new QTableWidgetItem(QString::number(outputCount)));
    }

    m_groupTable->resizeColumnsToContents();

    // 默认选第一组
    if(m_physicsSettings.size() > 0){
        m_groupTable->selectRow(0);
        showGroupDetail(0);
    }
}

void CPhysicsPanel::showGroupDetail(int row)
{
    if(row < 0 || row >= m_physicsSettings.size()) return;

    QJsonObject setting = m_physicsSettings[row].toObject();

    // 输入设置
    QJsonArray inputs = setting["Input"].toArray();
    m_inputTable->setRowCount(inputs.size());
    for(int i = 0; i < inputs.size(); i++){
        QJsonObject inp = inputs[i].toObject();
        QString paramId = inp["Source"].toObject()["Id"].toString();
        m_inputTable->setItem(i, 0, new QTableWidgetItem(paramId));
        m_inputTable->setItem(i, 1, new QTableWidgetItem(inp["Type"].toString()));
        m_inputTable->setItem(i, 2, new QTableWidgetItem(QString::number(inp["Weight"].toDouble())));
        m_inputTable->setItem(i, 3, new QTableWidgetItem(inp["Reflect"].toBool() ? "是" : "否"));
    }
    m_inputTable->resizeColumnsToContents();

    // 输出设置
    QJsonArray outputs = setting["Output"].toArray();
    m_outputTable->setRowCount(outputs.size());
    for(int i = 0; i < outputs.size(); i++){
        QJsonObject out = outputs[i].toObject();
        QString paramId = out["Destination"].toObject()["Id"].toString();
        m_outputTable->setItem(i, 0, new QTableWidgetItem(paramId));
        m_outputTable->setItem(i, 1, new QTableWidgetItem(out["Type"].toString()));
        m_outputTable->setItem(i, 2, new QTableWidgetItem(QString::number(out["Weight"].toDouble())));
        m_outputTable->setItem(i, 3, new QTableWidgetItem(QString::number(out["Scale"].toDouble(), 'f', 2)));
        m_outputTable->setItem(i, 4, new QTableWidgetItem(out["Reflect"].toBool() ? "是" : "否"));
    }
    m_outputTable->resizeColumnsToContents();

    // 摆锤参数 (Vertices，跳过第一个固定点)
    QJsonArray vertices = setting["Vertices"].toArray();
    int pendulumCount = vertices.size() > 1 ? vertices.size() - 1 : 0;
    m_vertexTable->setRowCount(pendulumCount);
    for(int i = 1; i < vertices.size(); i++){
        QJsonObject v = vertices[i].toObject();
        int row = i - 1;
        m_vertexTable->setItem(row, 0, new QTableWidgetItem(QString::number(i)));
        m_vertexTable->setItem(row, 1, new QTableWidgetItem(QString::number(v["Radius"].toDouble(), 'f', 1)));
        m_vertexTable->setItem(row, 2, new QTableWidgetItem(QString::number(v["Mobility"].toDouble(), 'f', 2)));
        m_vertexTable->setItem(row, 3, new QTableWidgetItem(QString::number(v["Delay"].toDouble(), 'f', 2)));
        m_vertexTable->setItem(row, 4, new QTableWidgetItem(QString::number(v["Acceleration"].toDouble(), 'f', 2)));
    }
    m_vertexTable->resizeColumnsToContents();
}

void CPhysicsPanel::emitDragParams(int row)
{
    if(row < 0 || row >= m_physicsSettings.size()) return;
    QJsonObject setting = m_physicsSettings[row].toObject();
    QJsonArray inputs = setting["Input"].toArray();

    QStringList paramIds, types;
    for(int i = 0; i < inputs.size(); i++){
        QJsonObject inp = inputs[i].toObject();
        paramIds << inp["Source"].toObject()["Id"].toString();
        types << inp["Type"].toString();
    }
    emit PhysicsDragParamsChanged(paramIds, types);
}
