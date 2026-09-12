#include "cmotionpanel.h"
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

using namespace Live2D::Cubism::Framework;

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
    exprLayout->addWidget(applyBtn);
    splitter->addWidget(exprWidget);

    // Pose 区域
    m_poseWidget = new QWidget();
    QVBoxLayout* poseLayout = new QVBoxLayout(m_poseWidget);
    poseLayout->addWidget(new QLabel("Pose 切换"));
    auto* poseTools = new QHBoxLayout();
    m_addPosePartButton = new QPushButton(QStringLiteral("添加 Part"), m_poseWidget);
    m_removePosePartButton = new QPushButton(QStringLiteral("删除 Part"), m_poseWidget);
    m_savePoseButton = new QPushButton(QStringLiteral("保存 Pose"), m_poseWidget);
    m_addPosePartButton->setEnabled(false);
    m_removePosePartButton->setEnabled(false);
    m_savePoseButton->setEnabled(false);
    poseTools->addWidget(m_addPosePartButton);
    poseTools->addWidget(m_removePosePartButton);
    poseTools->addWidget(m_savePoseButton, 1);
    poseLayout->addLayout(poseTools);
    connect(m_addPosePartButton, &QPushButton::clicked, this, &CMotionPanel::AddPosePart);
    connect(m_removePosePartButton, &QPushButton::clicked, this, &CMotionPanel::RemovePosePart);
    connect(m_savePoseButton, &QPushButton::clicked, this, [this](){
        QString error;
        if (!SavePose(&error)) QMessageBox::warning(this, QStringLiteral("保存 Pose 失败"), error);
    });
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
        m_exprNames.append(name);
    }

    if(m_motionEntries.isEmpty()) m_motionList->addItem("(此模型无动作定义)");
    if(m_exprNames.isEmpty()) m_exprList->addItem("(此模型无表情定义)");
}

bool CMotionPanel::BuildPoseGroups(const QString& poseJsonPath, QString* error)
{
    m_poseJsonPath = poseJsonPath;
    m_poseDocument = {};
    m_poseGroups.clear();
    m_activePoseGroup = -1;
    m_savePoseButton->setText(QStringLiteral("保存 Pose"));
    m_addPosePartButton->setEnabled(false);
    m_removePosePartButton->setEnabled(false);
    m_savePoseButton->setEnabled(false);
    // 清除 poseWidget 中旧的 combo
    QLayout* layout = m_poseWidget->layout();
    while(layout->count() > 1){ // 保留标签
        QLayoutItem* item = layout->takeAt(1);
        if(item->widget()) delete item->widget();
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
            const PoseGroup& pg = m_poseGroups[poseIndex];
            QStringList hideIds;
            QString showId;
            QStringList showLinks;
            for(int i = 0; i < pg.partIds.size(); i++){
                const QStringList links = i < pg.links.size() ? pg.links[i] : QStringList();
                if(i == index) {
                    showId = pg.partIds[i];
                    showLinks = links;
                } else {
                    hideIds << pg.partIds[i];
                    hideIds.append(links);
                }
            }
            hideIds.removeDuplicates();
            showLinks.removeDuplicates();
            hideIds.removeAll(showId);
            for (const QString& link : showLinks) hideIds.removeAll(link);
            QStringList showIds;
            showIds << showId;
            showIds.append(showLinks);
            showIds.removeDuplicates();
            emit PosePartChanged(showIds, hideIds);
        });
    }

    m_poseWidget->setVisible(!m_poseGroups.isEmpty());
    m_savePoseButton->setEnabled(!m_poseGroups.isEmpty());
    if (!m_poseGroups.isEmpty()) {
        m_activePoseGroup = 0;
        m_addPosePartButton->setEnabled(true);
        m_removePosePartButton->setEnabled(m_poseGroups[0].partIds.size() > 1);
    }
    return true;
}

bool CMotionPanel::SavePose(QString* error)
{
    if (m_poseJsonPath.isEmpty() || m_poseDocument.isEmpty()) {
        if (error) *error = QStringLiteral("没有可保存的 Pose 配置");
        return false;
    }
    QJsonArray groups;
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
}
