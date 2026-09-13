#include "cphysicspanel.h"
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
#include <QSaveFile>
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
            if (!m_updating) {
                updateForceDocument();
                m_dirty = true;
                emit PhysicsConfigChanged();
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
    m_groupTable->setObjectName(QStringLiteral("physicsGroupTable"));
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
    m_inputTable->setObjectName(QStringLiteral("physicsInputTable"));
    m_inputTable->setHorizontalHeaderLabels({"参数", "类型", "影响度", "反转"});
    m_inputTable->horizontalHeader()->setStretchLastSection(true);
    m_inputTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                  QAbstractItemView::SelectedClicked);
    m_inputTable->verticalHeader()->setVisible(false);
    detailSplitter->addWidget(m_inputTable);

    // 输出设置
    m_outputTable = new QTableWidget(0, 5, wrapper);
    m_outputTable->setObjectName(QStringLiteral("physicsOutputTable"));
    m_outputTable->setHorizontalHeaderLabels({"参数", "类型", "影响度", "倍率", "反转"});
    m_outputTable->horizontalHeader()->setStretchLastSection(true);
    m_outputTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                   QAbstractItemView::SelectedClicked);
    m_outputTable->verticalHeader()->setVisible(false);
    detailSplitter->addWidget(m_outputTable);

    // 摆锤参数
    m_vertexTable = new QTableWidget(0, 5, wrapper);
    m_vertexTable->setObjectName(QStringLiteral("physicsVertexTable"));
    m_vertexTable->setHorizontalHeaderLabels({"序号", "长度", "摆幅度", "反应速度", "收敛速度"});
    m_vertexTable->horizontalHeader()->setStretchLastSection(true);
    m_vertexTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                   QAbstractItemView::SelectedClicked);
    m_vertexTable->verticalHeader()->setVisible(false);
    detailSplitter->addWidget(m_vertexTable);

    mainLayout->addWidget(detailSplitter);
    setWidget(wrapper);
    connect(m_inputTable, &QTableWidget::cellChanged, this, &CPhysicsPanel::updateInputCell);
    connect(m_outputTable, &QTableWidget::cellChanged, this, &CPhysicsPanel::updateOutputCell);
    connect(m_vertexTable, &QTableWidget::cellChanged, this, &CPhysicsPanel::updateVertexCell);
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
    m_physicsPath = physicsPath;
    m_physicsDocument = root;
    m_dirty = false;
    m_updating = true;
    m_groupTable->setRowCount(0);
    m_inputTable->setRowCount(0);
    m_outputTable->setRowCount(0);
    m_vertexTable->setRowCount(0);
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
    m_updating = false;
    return true;
}

void CPhysicsPanel::updateForceDocument()
{
    if (m_physicsDocument.isEmpty()) return;
    QJsonObject meta = m_physicsDocument.value(QStringLiteral("Meta")).toObject();
    QJsonObject forces = meta.value(QStringLiteral("EffectiveForces")).toObject();
    QJsonObject gravity;
    gravity.insert(QStringLiteral("X"), m_gravityX->value() / 100.0);
    gravity.insert(QStringLiteral("Y"), m_gravityY->value() / 100.0);
    QJsonObject wind;
    wind.insert(QStringLiteral("X"), m_windX->value() / 100.0);
    wind.insert(QStringLiteral("Y"), m_windY->value() / 100.0);
    forces.insert(QStringLiteral("Gravity"), gravity);
    forces.insert(QStringLiteral("Wind"), wind);
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
    updateForceDocument();
    QString validationError;
    if (!ValidateDocument(&validationError)) {
        QMessageBox::warning(this, QStringLiteral("保存物理配置失败"), validationError);
        return false;
    }
    m_physicsDocument.insert(QStringLiteral("PhysicsSettings"), m_physicsSettings);
    QSaveFile file(target);
    const QByteArray data = QJsonDocument(m_physicsDocument).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("保存物理配置失败"), target);
        return false;
    }
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
            if (!inputValue.isObject() || !numberInRange(inputValue.toObject(), QStringLiteral("Weight"), 0.0, 1.0))
                return fail(QStringLiteral("物理组 %1 的输入权重无效").arg(groupIndex));
        }
        const QJsonValue outputsValue = group.value(QStringLiteral("Output"));
        if (!outputsValue.isUndefined() && !outputsValue.isArray()) return fail(QStringLiteral("物理组 %1 的 Output 无效").arg(groupIndex));
        for (const QJsonValue& outputValue : outputsValue.toArray()) {
            if (!outputValue.isObject()) return fail(QStringLiteral("物理组 %1 的输出无效").arg(groupIndex));
            const QJsonObject output = outputValue.toObject();
            if (!numberInRange(output, QStringLiteral("Weight"), 0.0, 1.0) ||
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
    if (m_updating || groupRow < 0 || groupRow >= m_physicsSettings.size() || (column != 2 && column != 3)) return;
    QJsonObject setting = m_physicsSettings[groupRow].toObject();
    QJsonArray inputs = setting.value(QStringLiteral("Input")).toArray();
    if (row >= inputs.size()) return;
    if (row < 0 || row >= inputs.size() || !m_inputTable->item(row, column)) return;
    QJsonObject input = inputs[row].toObject();
    if (column == 2) {
        bool ok = false;
        const double value = m_inputTable->item(row, column)->text().toDouble(&ok);
        if (!ok || !std::isfinite(value) || value < 0.0 || value > 1.0) {
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
    m_physicsDocument.insert(QStringLiteral("PhysicsSettings"), m_physicsSettings);
    m_dirty = true;
    emit PhysicsConfigChanged();
}

void CPhysicsPanel::updateOutputCell(int row, int column)
{
    const int groupRow = m_groupTable->currentRow();
    if (m_updating || groupRow < 0 || groupRow >= m_physicsSettings.size() || (column != 2 && column != 3 && column != 4)) return;
    const int detailRow = row;
    QJsonObject setting = m_physicsSettings[groupRow].toObject();
    QJsonArray outputs = setting.value(QStringLiteral("Output")).toArray();
    if (detailRow < 0 || detailRow >= outputs.size() || !m_outputTable->item(detailRow, column)) return;
    QJsonObject output = outputs[detailRow].toObject();
    if (column == 2 || column == 3) {
        bool ok = false;
        const double value = m_outputTable->item(detailRow, column)->text().toDouble(&ok);
        if (!ok || !std::isfinite(value) || (column == 2 && (value < 0.0 || value > 1.0))) {
            m_updating = true; showGroupDetail(groupRow); m_updating = false; return;
        }
        output.insert(column == 2 ? QStringLiteral("Weight") : QStringLiteral("Scale"), value);
    } else {
        const QString text = m_outputTable->item(detailRow, column)->text().trimmed().toLower();
        if (text == QStringLiteral("是") || text == QStringLiteral("true") || text == QStringLiteral("1")) output.insert(QStringLiteral("Reflect"), true);
        else if (text == QStringLiteral("否") || text == QStringLiteral("false") || text == QStringLiteral("0")) output.insert(QStringLiteral("Reflect"), false);
        else { m_updating = true; showGroupDetail(groupRow); m_updating = false; return; }
    }
    outputs[detailRow] = output;
    setting.insert(QStringLiteral("Output"), outputs);
    m_physicsSettings[groupRow] = setting;
    m_physicsDocument.insert(QStringLiteral("PhysicsSettings"), m_physicsSettings);
    m_dirty = true;
    emit PhysicsConfigChanged();
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
    m_physicsDocument.insert(QStringLiteral("PhysicsSettings"), m_physicsSettings);
    m_dirty = true;
    emit PhysicsConfigChanged();
}

void CPhysicsPanel::showGroupDetail(int row)
{
    if(row < 0 || row >= m_physicsSettings.size()) return;
    const bool wasUpdating = m_updating;
    m_updating = true;

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
    m_updating = wasUpdating;
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
