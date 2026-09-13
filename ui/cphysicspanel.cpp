#include "cphysicspanel.h"
#include "cphysicsgraph.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QFile>
#include <QJsonDocument>
#include <QSplitter>
#include <QDebug>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QInputDialog>
#include <QSaveFile>
#include <QSignalBlocker>
#include <cmath>

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
    auto* saveButton = new QPushButton(QStringLiteral("保存物理配置"), wrapper);
    saveButton->setObjectName(QStringLiteral("savePhysicsButton"));
    mainLayout->addWidget(saveButton);
    connect(saveButton, &QPushButton::clicked, this, [this](){ SavePhysics(); });

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
    m_gravityX->setObjectName(QStringLiteral("physicsGravityX"));
    m_gravityY->setObjectName(QStringLiteral("physicsGravityY"));
    m_windX->setObjectName(QStringLiteral("physicsWindX"));
    m_windY->setObjectName(QStringLiteral("physicsWindY"));
    mainLayout->addWidget(forceBox);

    // 重力/风力信号
    auto connectSlider = [this](QSlider* slider, QLabel* label, bool isGravity, bool isX) {
        connect(slider, &QSlider::valueChanged, this, [this, label, isGravity, isX](int val){
            if (m_updating || m_physicsDocument.isEmpty()) return;
            const double value = val / 100.0;
            label->setText(QString::number(value, 'f', 2));
            updateForceDocument(isGravity, isX, value);
            const QJsonObject force = m_physicsDocument.value(QStringLiteral("Meta")).toObject()
                .value(QStringLiteral("EffectiveForces")).toObject()
                .value(isGravity ? QStringLiteral("Gravity") : QStringLiteral("Wind")).toObject();
            if (isGravity) emit GravityChanged(force.value("X").toDouble(), force.value("Y").toDouble());
            else emit WindChanged(force.value("X").toDouble(), force.value("Y").toDouble());
            m_dirty = true;
            emit PhysicsConfigChanged();
        });
    };
    connectSlider(m_gravityX, m_gravityXLabel, true, true);
    connectSlider(m_gravityY, m_gravityYLabel, true, false);
    connectSlider(m_windX, m_windXLabel, false, true);
    connectSlider(m_windY, m_windYLabel, false, false);

    // 物理组列表
    QGroupBox* groupBox = new QGroupBox("物理组", wrapper);
    QVBoxLayout* groupLayout = new QVBoxLayout(groupBox);

    auto* groupTools = new QHBoxLayout();
    m_addGroupButton = new QPushButton(QStringLiteral("添加组"), groupBox);
    m_removeGroupButton = new QPushButton(QStringLiteral("删除组"), groupBox);
    m_renameGroupButton = new QPushButton(QStringLiteral("重命名"), groupBox);
    m_addGroupButton->setObjectName(QStringLiteral("addPhysicsGroup"));
    m_removeGroupButton->setObjectName(QStringLiteral("removePhysicsGroup"));
    m_renameGroupButton->setObjectName(QStringLiteral("renamePhysicsGroup"));
    m_addGroupButton->setEnabled(false);
    m_removeGroupButton->setEnabled(false);
    m_renameGroupButton->setEnabled(false);
    groupTools->addWidget(m_addGroupButton);
    groupTools->addWidget(m_removeGroupButton);
    groupTools->addWidget(m_renameGroupButton);
    groupTools->addStretch(1);
    groupLayout->addLayout(groupTools);

    m_groupTable = new QTableWidget(0, 3, groupBox);
    m_groupTable->setObjectName(QStringLiteral("physicsGroupTable"));
    m_groupTable->setHorizontalHeaderLabels({"ID", "名称", "输出数"});
    m_groupTable->horizontalHeader()->setStretchLastSection(true);
    m_groupTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_groupTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_groupTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_groupTable->verticalHeader()->setVisible(false);
    groupLayout->addWidget(m_groupTable);

    m_dragModeCb = new QCheckBox("拖拽驱动选中组", groupBox);
    m_dragModeCb->setObjectName(QStringLiteral("physicsDragMode"));
    m_dragModeCb->setToolTip("勾选后，在模型上拖拽鼠标可驱动选中物理组的输入参数");
    groupLayout->addWidget(m_dragModeCb);

    connect(m_groupTable, &QTableWidget::currentCellChanged, this, [this](int row, int, int, int){
        if (!m_updating) showGroupDetail(row);
    });
    connect(m_dragModeCb, &QCheckBox::toggled, this, [this](){
        if (!m_updating) emitDragParams(m_groupTable->currentRow());
    });

    mainLayout->addWidget(groupBox);

    // 详情区域（输入/输出/摆锤）
    QSplitter* detailSplitter = new QSplitter(Qt::Vertical, wrapper);
    m_graph = new CPhysicsGraphWidget(wrapper);
    detailSplitter->addWidget(m_graph);

    // 输入设置
    auto* inputSection = new QWidget(wrapper);
    auto* inputSectionLayout = new QVBoxLayout(inputSection);
    auto* inputTools = new QHBoxLayout();
    m_addInputButton = new QPushButton(QStringLiteral("添加输入"), inputSection);
    m_removeInputButton = new QPushButton(QStringLiteral("删除输入"), inputSection);
    m_addInputButton->setObjectName(QStringLiteral("addPhysicsInput"));
    m_removeInputButton->setObjectName(QStringLiteral("removePhysicsInput"));
    m_addInputButton->setEnabled(false);
    m_removeInputButton->setEnabled(false);
    inputTools->addWidget(m_addInputButton);
    inputTools->addWidget(m_removeInputButton);
    inputTools->addStretch(1);
    inputSectionLayout->addLayout(inputTools);
    m_inputTable = new QTableWidget(0, 4, inputSection);
    m_inputTable->setObjectName(QStringLiteral("physicsInputTable"));
    m_inputTable->setHorizontalHeaderLabels({"参数", "类型", "影响度", "反转"});
    m_inputTable->horizontalHeader()->setStretchLastSection(true);
    m_inputTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                  QAbstractItemView::SelectedClicked);
    m_inputTable->verticalHeader()->setVisible(false);
    inputSectionLayout->addWidget(m_inputTable);
    detailSplitter->addWidget(inputSection);

    // 输出设置
    auto* outputSection = new QWidget(wrapper);
    auto* outputSectionLayout = new QVBoxLayout(outputSection);
    auto* outputTools = new QHBoxLayout();
    m_addOutputButton = new QPushButton(QStringLiteral("添加输出"), outputSection);
    m_removeOutputButton = new QPushButton(QStringLiteral("删除输出"), outputSection);
    m_addOutputButton->setObjectName(QStringLiteral("addPhysicsOutput"));
    m_removeOutputButton->setObjectName(QStringLiteral("removePhysicsOutput"));
    m_addOutputButton->setEnabled(false);
    m_removeOutputButton->setEnabled(false);
    outputTools->addWidget(m_addOutputButton);
    outputTools->addWidget(m_removeOutputButton);
    outputTools->addStretch(1);
    outputSectionLayout->addLayout(outputTools);
    m_outputTable = new QTableWidget(0, 6, outputSection);
    m_outputTable->setObjectName(QStringLiteral("physicsOutputTable"));
    m_outputTable->setHorizontalHeaderLabels({"参数", "类型", "影响度", "倍率", "反转", "摆锤索引"});
    m_outputTable->horizontalHeader()->setStretchLastSection(true);
    m_outputTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                   QAbstractItemView::SelectedClicked);
    m_outputTable->verticalHeader()->setVisible(false);
    outputSectionLayout->addWidget(m_outputTable);
    detailSplitter->addWidget(outputSection);

    // 摆锤参数
    auto* vertexSection = new QWidget(wrapper);
    auto* vertexSectionLayout = new QVBoxLayout(vertexSection);
    auto* vertexTools = new QHBoxLayout();
    m_addVertexButton = new QPushButton(QStringLiteral("添加摆锤"), vertexSection);
    m_removeVertexButton = new QPushButton(QStringLiteral("删除摆锤"), vertexSection);
    m_addVertexButton->setObjectName(QStringLiteral("addPhysicsVertex"));
    m_removeVertexButton->setObjectName(QStringLiteral("removePhysicsVertex"));
    m_addVertexButton->setEnabled(false);
    m_removeVertexButton->setEnabled(false);
    vertexTools->addWidget(m_addVertexButton);
    vertexTools->addWidget(m_removeVertexButton);
    vertexTools->addStretch(1);
    vertexSectionLayout->addLayout(vertexTools);
    m_vertexTable = new QTableWidget(0, 5, vertexSection);
    m_vertexTable->setObjectName(QStringLiteral("physicsVertexTable"));
    m_vertexTable->setHorizontalHeaderLabels({"序号", "长度", "摆幅度", "反应速度", "收敛速度"});
    m_vertexTable->horizontalHeader()->setStretchLastSection(true);
    m_vertexTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                   QAbstractItemView::SelectedClicked);
    m_vertexTable->verticalHeader()->setVisible(false);
    vertexSectionLayout->addWidget(m_vertexTable);
    detailSplitter->addWidget(vertexSection);

    mainLayout->addWidget(detailSplitter);
    setWidget(wrapper);
    connect(m_inputTable, &QTableWidget::cellChanged, this, &CPhysicsPanel::updateInputCell);
    connect(m_outputTable, &QTableWidget::cellChanged, this, &CPhysicsPanel::updateOutputCell);
    connect(m_vertexTable, &QTableWidget::cellChanged, this, &CPhysicsPanel::updateVertexCell);
    connect(m_addInputButton, &QPushButton::clicked, this, &CPhysicsPanel::addPhysicsInput);
    connect(m_removeInputButton, &QPushButton::clicked, this, &CPhysicsPanel::removePhysicsInput);
    connect(m_addOutputButton, &QPushButton::clicked, this, &CPhysicsPanel::addPhysicsOutput);
    connect(m_removeOutputButton, &QPushButton::clicked, this, &CPhysicsPanel::removePhysicsOutput);
    connect(m_addVertexButton, &QPushButton::clicked, this, &CPhysicsPanel::addPhysicsVertex);
    connect(m_removeVertexButton, &QPushButton::clicked, this, &CPhysicsPanel::removePhysicsVertex);
    connect(m_inputTable, &QTableWidget::itemSelectionChanged, this, [this](){
        if (m_removeInputButton) m_removeInputButton->setEnabled(m_inputTable->currentRow() >= 0);
    });
    connect(m_outputTable, &QTableWidget::itemSelectionChanged, this, [this](){
        if (m_removeOutputButton) m_removeOutputButton->setEnabled(m_outputTable->currentRow() >= 0);
    });
    connect(m_vertexTable, &QTableWidget::itemSelectionChanged, this, [this](){
        if (m_removeVertexButton) m_removeVertexButton->setEnabled(m_vertexTable->currentRow() >= 0);
    });
    connect(m_addGroupButton, &QPushButton::clicked, this, &CPhysicsPanel::addPhysicsGroup);
    connect(m_removeGroupButton, &QPushButton::clicked, this, &CPhysicsPanel::removePhysicsGroup);
    connect(m_renameGroupButton, &QPushButton::clicked, this, &CPhysicsPanel::renamePhysicsGroup);
}

bool CPhysicsPanel::LoadPhysics(const QString& physicsPath, QString* error)
{
    QFile file(physicsPath);
    if(!file.open(QIODevice::ReadOnly | QIODevice::Text)){
        qDebug() << "[PhysicsPanel] Failed to open" << physicsPath;
        if (error) *error = QStringLiteral("无法打开物理配置：%1").arg(file.errorString());
        return false;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if(parseError.error != QJsonParseError::NoError || doc.isNull() || !doc.isObject()) {
        if (error) *error = QStringLiteral("物理配置 JSON 无效：%1").arg(parseError.errorString());
        return false;
    }

    QJsonObject root = doc.object();
    if (!root.value(QStringLiteral("PhysicsSettings")).isArray()) {
        if (error) *error = QStringLiteral("物理配置缺少 PhysicsSettings 数组");
        return false;
    }
    const QJsonObject previousDocument = m_physicsDocument;
    m_physicsDocument = root;
    QString validationError;
    if (!ValidateDocument(&validationError)) {
        m_physicsDocument = previousDocument;
        if (error) *error = validationError;
        return false;
    }
    m_physicsPath = physicsPath;
    m_dirty = false;
    m_updating = true;
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
    m_physicsSettings = root["PhysicsSettings"].toArray();
    if (m_addGroupButton) m_addGroupButton->setEnabled(true);
    m_updating = false;
    refreshGroupTable(0);
    return true;
}

void CPhysicsPanel::updateForceDocument(bool gravity, bool xAxis, double value)
{
    if (m_physicsDocument.isEmpty()) return;
    QJsonObject meta = m_physicsDocument.value(QStringLiteral("Meta")).toObject();
    QJsonObject forces = meta.value(QStringLiteral("EffectiveForces")).toObject();
    const QString forceName = gravity ? QStringLiteral("Gravity") : QStringLiteral("Wind");
    QJsonObject force = forces.value(forceName).toObject();
    force.insert(xAxis ? QStringLiteral("X") : QStringLiteral("Y"), value);
    forces.insert(forceName, force);
    meta.insert(QStringLiteral("EffectiveForces"), forces);
    m_physicsDocument.insert(QStringLiteral("Meta"), meta);
}

bool CPhysicsPanel::SavePhysics(const QString& path)
{
    QString target = path.isEmpty() ? m_physicsPath : path;
    if (target.isEmpty()) {
        target = QFileDialog::getSaveFileName(this, QStringLiteral("保存物理配置"), QString(),
                                              QStringLiteral("Physics (*.physics3.json);;JSON (*.json)"));
    }
    if (target.isEmpty() || m_physicsDocument.isEmpty()) return false;
    QString validationError;
    if (!ValidateDocument(&validationError)) {
        QMessageBox::warning(this, QStringLiteral("保存物理配置失败"), validationError);
        return false;
    }
    QJsonObject meta = m_physicsDocument.value(QStringLiteral("Meta")).toObject();
    int inputCount = 0;
    int outputCount = 0;
    int vertexCount = 0;
    for (const QJsonValue& settingValue : m_physicsSettings) {
        const QJsonObject setting = settingValue.toObject();
        inputCount += setting.value(QStringLiteral("Input")).toArray().size();
        outputCount += setting.value(QStringLiteral("Output")).toArray().size();
        vertexCount += setting.value(QStringLiteral("Vertices")).toArray().size();
    }
    meta.insert(QStringLiteral("PhysicsSettingCount"), m_physicsSettings.size());
    meta.insert(QStringLiteral("TotalInputCount"), inputCount);
    meta.insert(QStringLiteral("TotalOutputCount"), outputCount);
    meta.insert(QStringLiteral("VertexCount"), vertexCount);
    meta.insert(QStringLiteral("PhysicsDictionary"), m_physicsDictionary);
    QJsonObject updated = m_physicsDocument;
    updated.remove(QStringLiteral("PhysicsDictionary"));
    updated.insert(QStringLiteral("Meta"), meta);
    updated.insert(QStringLiteral("PhysicsSettings"), m_physicsSettings);
    QSaveFile file(target);
    const QByteArray data = QJsonDocument(updated).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("保存物理配置失败"), target);
        return false;
    }
    m_physicsDocument = updated;
    m_physicsPath = target;
    m_dirty = false;
    emit PhysicsSaved(target);
    return true;
}

bool CPhysicsPanel::ValidateDocument(QString* error) const
{
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const auto numberInRange = [](const QJsonObject& object, const QString& key,
                                  double minimum, double maximum) {
        const QJsonValue value = object.value(key);
        if (value.isUndefined()) return true;
        if (!value.isDouble()) return false;
        const double number = value.toDouble();
        return std::isfinite(number) && number >= minimum && number <= maximum;
    };
    const QJsonObject meta = m_physicsDocument.value(QStringLiteral("Meta")).toObject();
    const QJsonObject forces = meta.value(QStringLiteral("EffectiveForces")).toObject();
    for (const QString& forceName : {QStringLiteral("Gravity"), QStringLiteral("Wind")}) {
        const QJsonObject force = forces.value(forceName).toObject();
        if (!numberInRange(force, QStringLiteral("X"), -100000.0, 100000.0) ||
            !numberInRange(force, QStringLiteral("Y"), -100000.0, 100000.0))
            return fail(QStringLiteral("力场数值无效：%1").arg(forceName));
    }
    const QJsonValue settingsValue = m_physicsDocument.value(QStringLiteral("PhysicsSettings"));
    if (!settingsValue.isArray()) return fail(QStringLiteral("PhysicsSettings 必须是数组"));
    const QJsonArray settings = settingsValue.toArray();
    for (int groupIndex = 0; groupIndex < settings.size(); ++groupIndex) {
        const QJsonValue groupValue = settings.at(groupIndex);
        if (!groupValue.isObject()) return fail(QStringLiteral("物理组 %1 不是对象").arg(groupIndex));
        const QJsonObject group = groupValue.toObject();
        const QJsonValue inputsValue = group.value(QStringLiteral("Input"));
        if (!inputsValue.isUndefined() && !inputsValue.isArray()) return fail(QStringLiteral("物理组 %1 的 Input 无效").arg(groupIndex));
        for (const QJsonValue& inputValue : inputsValue.toArray()) {
            if (!inputValue.isObject()) return fail(QStringLiteral("物理组 %1 的输入无效").arg(groupIndex));
            const QJsonObject input = inputValue.toObject();
            const QString sourceId = input.value(QStringLiteral("Source")).toObject().value(QStringLiteral("Id")).toString();
            const QString inputType = input.value(QStringLiteral("Type")).toString();
            if (sourceId.isEmpty() || (inputType != QStringLiteral("X") && inputType != QStringLiteral("Y") &&
                                       inputType != QStringLiteral("Angle")) ||
                !numberInRange(input, QStringLiteral("Weight"), 0.0, 100.0))
                return fail(QStringLiteral("物理组 %1 的输入权重无效").arg(groupIndex));
        }
        const QJsonValue outputsValue = group.value(QStringLiteral("Output"));
        if (!outputsValue.isUndefined() && !outputsValue.isArray()) return fail(QStringLiteral("物理组 %1 的 Output 无效").arg(groupIndex));
        for (const QJsonValue& outputValue : outputsValue.toArray()) {
            if (!outputValue.isObject()) return fail(QStringLiteral("物理组 %1 的输出无效").arg(groupIndex));
            const QJsonObject output = outputValue.toObject();
            const QString destinationId = output.value(QStringLiteral("Destination")).toObject().value(QStringLiteral("Id")).toString();
            const QString outputType = output.value(QStringLiteral("Type")).toString();
            const QJsonValue vertexIndexValue = output.value(QStringLiteral("VertexIndex"));
            const int vertexCount = group.value(QStringLiteral("Vertices")).toArray().size();
            if (destinationId.isEmpty() || (outputType != QStringLiteral("X") && outputType != QStringLiteral("Y") &&
                                            outputType != QStringLiteral("Angle")) ||
                !vertexIndexValue.isDouble() || vertexIndexValue.toDouble() < 0.0 ||
                vertexIndexValue.toDouble() != std::floor(vertexIndexValue.toDouble()) ||
                vertexIndexValue.toDouble() >= vertexCount ||
                !numberInRange(output, QStringLiteral("Weight"), 0.0, 100.0) ||
                !numberInRange(output, QStringLiteral("Scale"), -100000.0, 100000.0))
                return fail(QStringLiteral("物理组 %1 的输出数值无效").arg(groupIndex));
        }
        const QJsonValue verticesValue = group.value(QStringLiteral("Vertices"));
        if (!verticesValue.isUndefined() && !verticesValue.isArray()) return fail(QStringLiteral("物理组 %1 的 Vertices 无效").arg(groupIndex));
        const QJsonArray vertices = verticesValue.toArray();
        for (const QJsonValue& vertexValue : vertices) {
            if (!vertexValue.isObject()) return fail(QStringLiteral("物理组 %1 的摆锤无效").arg(groupIndex));
            const QJsonObject vertex = vertexValue.toObject();
            if (!numberInRange(vertex, QStringLiteral("Radius"), 0.0, 100000.0) ||
                !numberInRange(vertex, QStringLiteral("Mobility"), 0.0, 1.0) ||
                !numberInRange(vertex, QStringLiteral("Delay"), 0.0, 100000.0) ||
                !numberInRange(vertex, QStringLiteral("Acceleration"), 0.0, 100000.0))
                return fail(QStringLiteral("物理组 %1 的摆锤数值无效").arg(groupIndex));
        }
    }
    return true;
}

void CPhysicsPanel::updateInputCell(int row, int column)
{
    const int groupRow = m_groupTable->currentRow();
    if (m_updating || groupRow < 0 || groupRow >= m_physicsSettings.size() || column < 0 || column > 3) return;
    QJsonObject setting = m_physicsSettings[groupRow].toObject();
    QJsonArray inputs = setting.value(QStringLiteral("Input")).toArray();
    if (row >= inputs.size()) return;
    if (row < 0 || row >= inputs.size() || !m_inputTable->item(row, column)) return;
    QJsonObject input = inputs[row].toObject();
    if (column == 0) {
        const QString id = m_inputTable->item(row, column)->text().trimmed();
        if (id.isEmpty()) { m_updating = true; showGroupDetail(groupRow); m_updating = false; return; }
        QJsonObject source = input.value(QStringLiteral("Source")).toObject();
        source.insert(QStringLiteral("Id"), id);
        input.insert(QStringLiteral("Source"), source);
    } else if (column == 1) {
        const QString type = m_inputTable->item(row, column)->text().trimmed();
        if (type != QStringLiteral("X") && type != QStringLiteral("Y") && type != QStringLiteral("Angle")) {
            m_updating = true; showGroupDetail(groupRow); m_updating = false; return;
        }
        input.insert(QStringLiteral("Type"), type);
    } else if (column == 2) {
        bool ok = false;
        const double value = m_inputTable->item(row, column)->text().toDouble(&ok);
        if (!ok || !std::isfinite(value) || value < 0.0 || value > 100.0) {
            m_updating = true; showGroupDetail(groupRow); m_updating = false; return;
        }
        input.insert(QStringLiteral("Weight"), value);
    } else {
        const QString text = m_inputTable->item(row, column)->text().trimmed().toLower();
        if (text == QStringLiteral("是") || text == QStringLiteral("true") || text == QStringLiteral("1")) input.insert(QStringLiteral("Reflect"), true);
        else if (text == QStringLiteral("否") || text == QStringLiteral("false") || text == QStringLiteral("0")) input.insert(QStringLiteral("Reflect"), false);
        else { m_updating = true; showGroupDetail(groupRow); m_updating = false; return; }
    }
    inputs[row] = input;
    setting.insert(QStringLiteral("Input"), inputs);
    m_physicsSettings[groupRow] = setting;
    commitGroupChanges(groupRow);
}

void CPhysicsPanel::updateOutputCell(int row, int column)
{
    const int groupRow = m_groupTable->currentRow();
    if (m_updating || groupRow < 0 || groupRow >= m_physicsSettings.size() || column < 0 || column > 5) return;
    const int detailRow = row;
    QJsonObject setting = m_physicsSettings[groupRow].toObject();
    QJsonArray outputs = setting.value(QStringLiteral("Output")).toArray();
    if (detailRow < 0 || detailRow >= outputs.size() || !m_outputTable->item(detailRow, column)) return;
    QJsonObject output = outputs[detailRow].toObject();
    if (column == 0) {
        const QString id = m_outputTable->item(detailRow, column)->text().trimmed();
        if (id.isEmpty()) { m_updating = true; showGroupDetail(groupRow); m_updating = false; return; }
        QJsonObject destination = output.value(QStringLiteral("Destination")).toObject();
        destination.insert(QStringLiteral("Id"), id);
        output.insert(QStringLiteral("Destination"), destination);
    } else if (column == 1) {
        const QString type = m_outputTable->item(detailRow, column)->text().trimmed();
        if (type != QStringLiteral("X") && type != QStringLiteral("Y") && type != QStringLiteral("Angle")) {
            m_updating = true; showGroupDetail(groupRow); m_updating = false; return;
        }
        output.insert(QStringLiteral("Type"), type);
    } else if (column == 2 || column == 3) {
        bool ok = false;
        const double value = m_outputTable->item(detailRow, column)->text().toDouble(&ok);
        if (!ok || !std::isfinite(value) || (column == 2 && (value < 0.0 || value > 100.0))) {
            m_updating = true; showGroupDetail(groupRow); m_updating = false; return;
        }
        output.insert(column == 2 ? QStringLiteral("Weight") : QStringLiteral("Scale"), value);
    } else if (column == 4) {
        const QString text = m_outputTable->item(detailRow, column)->text().trimmed().toLower();
        if (text == QStringLiteral("是") || text == QStringLiteral("true") || text == QStringLiteral("1")) output.insert(QStringLiteral("Reflect"), true);
        else if (text == QStringLiteral("否") || text == QStringLiteral("false") || text == QStringLiteral("0")) output.insert(QStringLiteral("Reflect"), false);
        else { m_updating = true; showGroupDetail(groupRow); m_updating = false; return; }
    } else {
        bool ok = false;
        const double value = m_outputTable->item(detailRow, column)->text().toDouble(&ok);
        const int vertexCount = setting.value(QStringLiteral("Vertices")).toArray().size();
        if (!ok || !std::isfinite(value) || std::floor(value) != value || value < 0.0 || value >= vertexCount) {
            m_updating = true; showGroupDetail(groupRow); m_updating = false; return;
        }
        output.insert(QStringLiteral("VertexIndex"), static_cast<int>(value));
    }
    outputs[detailRow] = output;
    setting.insert(QStringLiteral("Output"), outputs);
    m_physicsSettings[groupRow] = setting;
    commitGroupChanges(groupRow);
}

void CPhysicsPanel::updateVertexCell(int row, int column)
{
    if (m_updating || row < 0 || (column < 1 || column > 4)) return;
    const int groupRow = m_groupTable->currentRow();
    if (groupRow < 0 || groupRow >= m_physicsSettings.size() || !m_vertexTable->item(row, column)) return;
    QJsonObject setting = m_physicsSettings[groupRow].toObject();
    QJsonArray vertices = setting.value(QStringLiteral("Vertices")).toArray();
    const int vertexIndex = row + 1;
    if (vertexIndex >= vertices.size()) return;
    bool ok = false;
    const double value = m_vertexTable->item(row, column)->text().toDouble(&ok);
    if (!ok || !std::isfinite(value) || value < 0.0 || (column == 2 && value > 1.0)) {
        m_updating = true; showGroupDetail(groupRow); m_updating = false; return;
    }
    QJsonObject vertex = vertices[vertexIndex].toObject();
    const QString key = column == 1 ? QStringLiteral("Radius") : column == 2 ? QStringLiteral("Mobility")
                         : column == 3 ? QStringLiteral("Delay") : QStringLiteral("Acceleration");
    vertex.insert(key, value);
    vertices[vertexIndex] = vertex;
    setting.insert(QStringLiteral("Vertices"), vertices);
    m_physicsSettings[groupRow] = setting;
    commitGroupChanges(groupRow);
}

void CPhysicsPanel::refreshGroupTable(int selectedRow)
{
    if (!m_groupTable) return;
    const QSignalBlocker blocker(m_groupTable);
    const bool wasUpdating = m_updating;
    m_updating = true;
    QMap<QString, QString> idToName;
    for (const QJsonValue& value : m_physicsDictionary) {
        const QJsonObject entry = value.toObject();
        idToName.insert(entry.value(QStringLiteral("Id")).toString(), entry.value(QStringLiteral("Name")).toString());
    }
    m_groupTable->setRowCount(m_physicsSettings.size());
    for (int i = 0; i < m_physicsSettings.size(); ++i) {
        const QJsonObject setting = m_physicsSettings.at(i).toObject();
        const QString id = setting.value(QStringLiteral("Id")).toString();
        m_groupTable->setItem(i, 0, new QTableWidgetItem(id));
        m_groupTable->setItem(i, 1, new QTableWidgetItem(idToName.value(id, id)));
        m_groupTable->setItem(i, 2, new QTableWidgetItem(QString::number(setting.value(QStringLiteral("Output")).toArray().size())));
    }
    m_groupTable->resizeColumnsToContents();
    selectedRow = m_physicsSettings.isEmpty() ? -1 : qBound(0, selectedRow, static_cast<int>(m_physicsSettings.size()) - 1);
    m_groupTable->setCurrentCell(selectedRow, selectedRow >= 0 ? 0 : -1);
    if (selectedRow >= 0) m_groupTable->selectRow(selectedRow);
    m_updating = wasUpdating;
    showGroupDetail(selectedRow);
}

void CPhysicsPanel::commitGroupChanges(int selectedRow)
{
    QJsonObject meta = m_physicsDocument.value(QStringLiteral("Meta")).toObject();
    meta.insert(QStringLiteral("PhysicsDictionary"), m_physicsDictionary);
    m_physicsDocument.insert(QStringLiteral("Meta"), meta);
    m_physicsDocument.insert(QStringLiteral("PhysicsSettings"), m_physicsSettings);
    m_dirty = true;
    refreshGroupTable(selectedRow);
    emit PhysicsConfigChanged();
}

void CPhysicsPanel::addPhysicsGroup()
{
    if (m_physicsDocument.isEmpty() || m_physicsSettings.size() >= 10000) return;
    bool accepted = false;
    const QString id = QInputDialog::getText(this, QStringLiteral("添加物理组"),
                                              QStringLiteral("组 ID"), QLineEdit::Normal,
                                              QString(), &accepted).trimmed();
    if (!accepted || id.isEmpty()) return;
    for (const QJsonValue& value : m_physicsSettings) {
        if (value.toObject().value(QStringLiteral("Id")).toString() == id) {
            QMessageBox::warning(this, QStringLiteral("添加物理组"), QStringLiteral("该组 ID 已存在"));
            return;
        }
    }
    const QString name = QInputDialog::getText(this, QStringLiteral("添加物理组"),
                                                QStringLiteral("显示名称"), QLineEdit::Normal,
                                                id, &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;
    QJsonObject position;
    position.insert(QStringLiteral("Minimum"), -10.0);
    position.insert(QStringLiteral("Default"), 0.0);
    position.insert(QStringLiteral("Maximum"), 10.0);
    QJsonObject normalization;
    normalization.insert(QStringLiteral("Position"), position);
    normalization.insert(QStringLiteral("Angle"), position);
    QJsonObject rootPosition;
    rootPosition.insert(QStringLiteral("X"), 0.0);
    rootPosition.insert(QStringLiteral("Y"), 0.0);
    QJsonObject rootVertex;
    rootVertex.insert(QStringLiteral("Position"), rootPosition);
    rootVertex.insert(QStringLiteral("Mobility"), 1.0);
    rootVertex.insert(QStringLiteral("Delay"), 1.0);
    rootVertex.insert(QStringLiteral("Acceleration"), 1.0);
    rootVertex.insert(QStringLiteral("Radius"), 0.0);
    QJsonArray vertices;
    vertices.append(rootVertex);
    QJsonObject setting;
    setting.insert(QStringLiteral("Id"), id);
    setting.insert(QStringLiteral("Input"), QJsonArray());
    setting.insert(QStringLiteral("Output"), QJsonArray());
    setting.insert(QStringLiteral("Vertices"), vertices);
    setting.insert(QStringLiteral("Normalization"), normalization);
    m_physicsSettings.append(setting);
    QJsonObject dictionaryEntry;
    dictionaryEntry.insert(QStringLiteral("Id"), id);
    dictionaryEntry.insert(QStringLiteral("Name"), name);
    m_physicsDictionary.append(dictionaryEntry);
    commitGroupChanges(m_physicsSettings.size() - 1);
}

void CPhysicsPanel::removePhysicsGroup()
{
    const int row = m_groupTable ? m_groupTable->currentRow() : -1;
    if (row < 0 || row >= m_physicsSettings.size()) return;
    const QString id = m_physicsSettings.at(row).toObject().value(QStringLiteral("Id")).toString();
    m_physicsSettings.removeAt(row);
    for (int i = m_physicsDictionary.size() - 1; i >= 0; --i) {
        if (m_physicsDictionary.at(i).toObject().value(QStringLiteral("Id")).toString() == id)
            m_physicsDictionary.removeAt(i);
    }
    commitGroupChanges(row);
}

void CPhysicsPanel::renamePhysicsGroup()
{
    const int row = m_groupTable ? m_groupTable->currentRow() : -1;
    if (row < 0 || row >= m_physicsSettings.size()) return;
    const QString id = m_physicsSettings.at(row).toObject().value(QStringLiteral("Id")).toString();
    QString currentName = id;
    for (const QJsonValue& value : m_physicsDictionary) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("Id")).toString() == id) { currentName = entry.value(QStringLiteral("Name")).toString(); break; }
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("重命名物理组"),
                                                QStringLiteral("显示名称"), QLineEdit::Normal,
                                                currentName, &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;
    bool dictionaryFound = false;
    for (int i = 0; i < m_physicsDictionary.size(); ++i) {
        QJsonObject entry = m_physicsDictionary.at(i).toObject();
        if (entry.value(QStringLiteral("Id")).toString() != id) continue;
        entry.insert(QStringLiteral("Name"), name);
        m_physicsDictionary[i] = entry;
        dictionaryFound = true;
        break;
    }
    if (!dictionaryFound) {
        QJsonObject entry;
        entry.insert(QStringLiteral("Id"), id);
        entry.insert(QStringLiteral("Name"), name);
        m_physicsDictionary.append(entry);
    }
    commitGroupChanges(row);
}

void CPhysicsPanel::addPhysicsInput()
{
    const int groupRow = m_groupTable ? m_groupTable->currentRow() : -1;
    if (groupRow < 0 || groupRow >= m_physicsSettings.size()) return;
    bool accepted = false;
    const QString id = QInputDialog::getText(this, QStringLiteral("添加物理输入"),
                                              QStringLiteral("参数 ID"), QLineEdit::Normal,
                                              QString(), &accepted).trimmed();
    if (!accepted || id.isEmpty()) return;
    const QStringList types = {QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Angle")};
    const QString type = QInputDialog::getItem(this, QStringLiteral("添加物理输入"),
                                                QStringLiteral("输入类型"), types, 0, false, &accepted);
    if (!accepted) return;
    const double weight = QInputDialog::getDouble(this, QStringLiteral("添加物理输入"),
                                                   QStringLiteral("影响度（0-100）"), 100.0, 0.0, 100.0, 2, &accepted);
    if (!accepted) return;
    QJsonObject source;
    source.insert(QStringLiteral("Target"), QStringLiteral("Parameter"));
    source.insert(QStringLiteral("Id"), id);
    QJsonObject input;
    input.insert(QStringLiteral("Source"), source);
    input.insert(QStringLiteral("Weight"), weight);
    input.insert(QStringLiteral("Type"), type);
    input.insert(QStringLiteral("Reflect"), false);
    QJsonObject setting = m_physicsSettings.at(groupRow).toObject();
    QJsonArray inputs = setting.value(QStringLiteral("Input")).toArray();
    inputs.append(input);
    setting.insert(QStringLiteral("Input"), inputs);
    m_physicsSettings[groupRow] = setting;
    commitGroupChanges(groupRow);
}

void CPhysicsPanel::removePhysicsInput()
{
    const int groupRow = m_groupTable ? m_groupTable->currentRow() : -1;
    const int row = m_inputTable ? m_inputTable->currentRow() : -1;
    if (groupRow < 0 || groupRow >= m_physicsSettings.size() || row < 0) return;
    QJsonObject setting = m_physicsSettings.at(groupRow).toObject();
    QJsonArray inputs = setting.value(QStringLiteral("Input")).toArray();
    if (row >= inputs.size()) return;
    inputs.removeAt(row);
    setting.insert(QStringLiteral("Input"), inputs);
    m_physicsSettings[groupRow] = setting;
    commitGroupChanges(groupRow);
}

void CPhysicsPanel::addPhysicsOutput()
{
    const int groupRow = m_groupTable ? m_groupTable->currentRow() : -1;
    if (groupRow < 0 || groupRow >= m_physicsSettings.size()) return;
    const QJsonObject current = m_physicsSettings.at(groupRow).toObject();
    const int vertexCount = current.value(QStringLiteral("Vertices")).toArray().size();
    if (vertexCount <= 0) {
        QMessageBox::warning(this, QStringLiteral("添加物理输出"), QStringLiteral("当前物理组没有可用摆锤"));
        return;
    }
    bool accepted = false;
    const QString id = QInputDialog::getText(this, QStringLiteral("添加物理输出"),
                                              QStringLiteral("参数 ID"), QLineEdit::Normal,
                                              QString(), &accepted).trimmed();
    if (!accepted || id.isEmpty()) return;
    const QStringList types = {QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Angle")};
    const QString type = QInputDialog::getItem(this, QStringLiteral("添加物理输出"),
                                                QStringLiteral("输出类型"), types, 0, false, &accepted);
    if (!accepted) return;
    const int vertexIndex = QInputDialog::getInt(this, QStringLiteral("添加物理输出"),
                                                 QStringLiteral("摆锤索引"), 0, 0, vertexCount - 1, 1, &accepted);
    if (!accepted) return;
    const double scale = QInputDialog::getDouble(this, QStringLiteral("添加物理输出"),
                                                 QStringLiteral("倍率"), 1.0, -100000.0, 100000.0, 3, &accepted);
    if (!accepted) return;
    QJsonObject destination;
    destination.insert(QStringLiteral("Target"), QStringLiteral("Parameter"));
    destination.insert(QStringLiteral("Id"), id);
    QJsonObject output;
    output.insert(QStringLiteral("Destination"), destination);
    output.insert(QStringLiteral("VertexIndex"), vertexIndex);
    output.insert(QStringLiteral("Scale"), scale);
    output.insert(QStringLiteral("Weight"), 100.0);
    output.insert(QStringLiteral("Type"), type);
    output.insert(QStringLiteral("Reflect"), false);
    QJsonObject setting = current;
    QJsonArray outputs = setting.value(QStringLiteral("Output")).toArray();
    outputs.append(output);
    setting.insert(QStringLiteral("Output"), outputs);
    m_physicsSettings[groupRow] = setting;
    commitGroupChanges(groupRow);
}

void CPhysicsPanel::removePhysicsOutput()
{
    const int groupRow = m_groupTable ? m_groupTable->currentRow() : -1;
    const int row = m_outputTable ? m_outputTable->currentRow() : -1;
    if (groupRow < 0 || groupRow >= m_physicsSettings.size() || row < 0) return;
    QJsonObject setting = m_physicsSettings.at(groupRow).toObject();
    QJsonArray outputs = setting.value(QStringLiteral("Output")).toArray();
    if (row >= outputs.size()) return;
    outputs.removeAt(row);
    setting.insert(QStringLiteral("Output"), outputs);
    m_physicsSettings[groupRow] = setting;
    commitGroupChanges(groupRow);
}

void CPhysicsPanel::addPhysicsVertex()
{
    const int groupRow = m_groupTable ? m_groupTable->currentRow() : -1;
    if (groupRow < 0 || groupRow >= m_physicsSettings.size()) return;
    QJsonObject setting = m_physicsSettings.at(groupRow).toObject();
    QJsonArray vertices = setting.value(QStringLiteral("Vertices")).toArray();
    if (vertices.size() >= 10000) return;
    QJsonObject position;
    position.insert(QStringLiteral("X"), 0.0);
    position.insert(QStringLiteral("Y"), 0.0);
    QJsonObject vertex;
    vertex.insert(QStringLiteral("Position"), position);
    vertex.insert(QStringLiteral("Mobility"), 0.5);
    vertex.insert(QStringLiteral("Delay"), 0.2);
    vertex.insert(QStringLiteral("Acceleration"), 0.3);
    vertex.insert(QStringLiteral("Radius"), 1.0);
    vertices.append(vertex);
    setting.insert(QStringLiteral("Vertices"), vertices);
    m_physicsSettings[groupRow] = setting;
    commitGroupChanges(groupRow);
}

void CPhysicsPanel::removePhysicsVertex()
{
    const int groupRow = m_groupTable ? m_groupTable->currentRow() : -1;
    const int row = m_vertexTable ? m_vertexTable->currentRow() : -1;
    if (groupRow < 0 || groupRow >= m_physicsSettings.size() || row < 0) return;
    const int vertexIndex = row + 1;
    QJsonObject setting = m_physicsSettings.at(groupRow).toObject();
    QJsonArray vertices = setting.value(QStringLiteral("Vertices")).toArray();
    if (vertexIndex <= 0 || vertexIndex >= vertices.size()) return;
    QJsonArray outputs = setting.value(QStringLiteral("Output")).toArray();
    for (const QJsonValue& outputValue : outputs) {
        const int outputIndex = outputValue.toObject().value(QStringLiteral("VertexIndex")).toInt(-1);
        if (outputIndex == vertexIndex) {
            QMessageBox::warning(this, QStringLiteral("删除摆锤"), QStringLiteral("该摆锤仍被输出引用，不能删除"));
            return;
        }
    }
    for (int i = 0; i < outputs.size(); ++i) {
        QJsonObject output = outputs.at(i).toObject();
        const int outputIndex = output.value(QStringLiteral("VertexIndex")).toInt(-1);
        if (outputIndex > vertexIndex) output.insert(QStringLiteral("VertexIndex"), outputIndex - 1);
        outputs[i] = output;
    }
    vertices.removeAt(vertexIndex);
    setting.insert(QStringLiteral("Vertices"), vertices);
    setting.insert(QStringLiteral("Output"), outputs);
    m_physicsSettings[groupRow] = setting;
    commitGroupChanges(groupRow);
}

void CPhysicsPanel::showGroupDetail(int row)
{
    const bool validGroup = row >= 0 && row < m_physicsSettings.size();
    const bool wasUpdating = m_updating;
    m_updating = true;

    const QJsonObject setting = validGroup ? m_physicsSettings.at(row).toObject() : QJsonObject();
    m_addInputButton->setEnabled(validGroup);
    m_addOutputButton->setEnabled(validGroup && !setting.value("Vertices").toArray().isEmpty());
    m_addVertexButton->setEnabled(validGroup);
    m_removeGroupButton->setEnabled(validGroup);
    m_renameGroupButton->setEnabled(validGroup);
    for (auto* table : {m_inputTable, m_outputTable, m_vertexTable}) {
        table->clearSelection();
        table->setCurrentCell(-1, -1);
    }
    if (m_graph) m_graph->SetGroup(setting);

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
        m_outputTable->setItem(i, 5, new QTableWidgetItem(QString::number(out["VertexIndex"].toInt())));
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
    if (m_removeInputButton) m_removeInputButton->setEnabled(false);
    if (m_removeOutputButton) m_removeOutputButton->setEnabled(false);
    if (m_removeVertexButton) m_removeVertexButton->setEnabled(false);
    m_updating = wasUpdating;
    if (!m_updating) emitDragParams(row);
}

void CPhysicsPanel::emitDragParams(int row)
{
    QStringList paramIds, types;
    if (m_dragModeCb->isChecked() && row >= 0 && row < m_physicsSettings.size()) {
        const QJsonArray inputs = m_physicsSettings.at(row).toObject().value("Input").toArray();
        for (const auto& value : inputs) {
            const QJsonObject input = value.toObject();
            paramIds << input.value("Source").toObject().value("Id").toString();
            types << input.value("Type").toString();
        }
    }
    emit PhysicsDragParamsChanged(paramIds, types);
}
