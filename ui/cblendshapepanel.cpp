#include "cblendshapepanel.h"
#include "ui/para/ccidjson.h"
#include "live2dstruct/util/cfilereader.h"
#include "live2dstruct/csectiontable.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QSlider>
#include <QLabel>
#include <QDebug>
#include <QHash>
#include <QSignalBlocker>
#include <cmath>
#include <limits>

using namespace live2dstruct;

namespace {
int sliderPosition(float value, float minimum, float maximum)
{
    if (minimum == maximum) return 0;
    const double ratio = (double(value) - minimum) / (double(maximum) - minimum);
    return qRound(qBound(0.0, ratio, 1.0) * 1000.0);
}

QString parameterText(float value)
{
    return QString::number(value, 'g', std::numeric_limits<float>::max_digits10);
}
}

CBlendShapePanel::CBlendShapePanel(QWidget *parent)
    : QDockWidget("融合变形", parent)
{
    setObjectName(QStringLiteral("blendShapePanel"));
    QWidget* wrapper = new QWidget(this);
    QVBoxLayout* mainLayout = new QVBoxLayout(wrapper);

    m_tree = new QTreeWidget(wrapper);
    m_tree->setHeaderLabels({"名称", "详情"});
    m_tree->setColumnCount(2);
    m_tree->setIndentation(16);
    m_tree->setRootIsDecorated(true);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    mainLayout->addWidget(m_tree);

    setWidget(wrapper);
}

void CBlendShapePanel::BuildBlendShapes(const QString& mocPath, CCidJson* cid)
{
    m_tree->clear();
    m_blendShapeParamIds.clear();
    m_paramSliders.clear();
    m_paramValueLabels.clear();
    m_paramRanges.clear();

    CReader reader(mocPath);
    CSectionTable sec(reader);
    if (!sec.IsValid()) return;

    int bsArtMeshCount = sec.blend_shapes_art_meshes.size();
    int bsWarpCount = sec.blend_shapes_warp_deformers.size();
    int constraintCount = sec.blend_shape_constraints.size();
    int paramCount = sec.parameters.size();
    QHash<QString, QPair<float, float>> parameterRanges;
    QHash<QString, float> parameterDefaults;
    for (const auto* parameter : sec.parameters) {
        float minimum = parameter->min_values;
        float maximum = parameter->max_values;
        if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum) {
            minimum = 0.0f;
            maximum = 1.0f;
        }
        parameterRanges.insert(parameter->ids, qMakePair(minimum, maximum));
        parameterDefaults.insert(parameter->ids, std::isfinite(parameter->default_values)
                                             ? qBound(minimum, parameter->default_values, maximum) : minimum);
    }

    // 从 blend_shape_constraints 直接获取 blend shape 关联的参数
    for(int i = 0; i < constraintCount; i++){
        int paramIdx = sec.blend_shape_constraints[i]->parameter_indices;
        if(paramIdx >= 0 && paramIdx < paramCount){
            m_blendShapeParamIds.insert(sec.parameters[paramIdx]->ids);
        }
    }

    // 也从 cdi3.json 的 CombinedParameters 获取
    if(cid){
        for(const auto& group : cid->getCombinedParameters()){
            for(const QString& id : group){
                m_blendShapeParamIds.insert(id);
            }
        }
    }

    // blend_shape_keyform_bindings → parameter_bindings 链较复杂
    // constraints 的 parameter_indices 已直接给出关联参数，足够 UI 使用

    if(m_blendShapeParamIds.isEmpty() && bsArtMeshCount == 0 && bsWarpCount == 0){
        QTreeWidgetItem* item = new QTreeWidgetItem(m_tree);
        item->setText(0, "此模型无融合变形");
        return;
    }

    // 显示受 blend shape 影响的 ArtMesh
    if(bsArtMeshCount > 0){
        QTreeWidgetItem* artMeshGroup = new QTreeWidgetItem(m_tree);
        artMeshGroup->setText(0, QString("ArtMesh (%1)").arg(bsArtMeshCount));
        artMeshGroup->setExpanded(true);

        for(int i = 0; i < bsArtMeshCount; i++){
            int targetIdx = sec.blend_shapes_art_meshes[i]->target_indices;
            QString name = (targetIdx >= 0 && targetIdx < sec.art_meshes.size())
                ? sec.art_meshes[targetIdx]->ids : QString("ArtMesh[%1]").arg(targetIdx);

            int bindingCount = sec.blend_shapes_art_meshes[i]->blend_shape_keyform_binding_sources_counts;

            QTreeWidgetItem* item = new QTreeWidgetItem(artMeshGroup);
            item->setText(0, name);
            item->setText(1, QString("%1 个绑定").arg(bindingCount));
        }
    }

    // 显示受 blend shape 影响的 WarpDeformer
    if(bsWarpCount > 0){
        QTreeWidgetItem* warpGroup = new QTreeWidgetItem(m_tree);
        warpGroup->setText(0, QString("WarpDeformer (%1)").arg(bsWarpCount));
        warpGroup->setExpanded(true);

        for(int i = 0; i < bsWarpCount; i++){
            int targetIdx = sec.blend_shapes_warp_deformers[i]->target_indices;
            // 变形器在 deformers 表中，但 blend_shapes_warp_deformers 的 target 指向 warp_deformers 表
            QString name = (targetIdx >= 0 && targetIdx < sec.warp_deformers.size())
                ? QString("Warp[%1]").arg(targetIdx) : QString("Warp[%1]").arg(targetIdx);
            // 尝试从 deformers 表中按 specific_sources_indices 反查
            for(int d = 0; d < sec.deformers.size(); d++){
                if(sec.deformers[d]->types == 0 && sec.deformers[d]->specific_sources_indices == targetIdx){
                    name = sec.deformers[d]->ids;
                    break;
                }
            }

            int bindingCount = sec.blend_shapes_warp_deformers[i]->blend_shape_keyform_binding_sources_counts;

            QTreeWidgetItem* item = new QTreeWidgetItem(warpGroup);
            item->setText(0, name);
            item->setText(1, QString("%1 个绑定").arg(bindingCount));
        }
    }

    // 显示 blend shape 关联的参数（带滑条）
    if(!m_blendShapeParamIds.isEmpty()){
        QTreeWidgetItem* paramGroup = new QTreeWidgetItem(m_tree);
        paramGroup->setText(0, QString("融合参数 (%1)").arg(m_blendShapeParamIds.size()));
        paramGroup->setExpanded(true);

        for(const QString& paramId : m_blendShapeParamIds){
            QString displayName = paramId;
            if(cid){
                QString mapped = cid->getParaName(paramId);
                if(!mapped.isEmpty()) displayName = mapped;
            }

            QTreeWidgetItem* item = new QTreeWidgetItem(paramGroup);
            item->setText(0, displayName);
            item->setData(0, Qt::UserRole, paramId);
            item->setToolTip(0, paramId);

            // 嵌入滑条
            QWidget* sliderWidget = new QWidget();
            QHBoxLayout* sl = new QHBoxLayout(sliderWidget);
            sl->setContentsMargins(0,0,0,0);
            QSlider* slider = new QSlider(Qt::Horizontal);
            slider->setRange(0, 1000);
            const auto range = parameterRanges.value(paramId, qMakePair(0.0f, 1.0f));
            const float minValue = range.first;
            const float maxValue = range.second;
            const float defaultValue = parameterDefaults.value(paramId, minValue);
            const int defaultSliderValue = sliderPosition(defaultValue, minValue, maxValue);
            slider->setValue(defaultSliderValue);
            slider->setEnabled(minValue != maxValue);
            QLabel* valLabel = new QLabel(parameterText(defaultValue));
            valLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            sl->addWidget(slider, 1);
            sl->addWidget(valLabel);

            m_tree->setItemWidget(item, 1, sliderWidget);
            m_paramSliders.insert(paramId, slider);
            m_paramValueLabels.insert(paramId, valLabel);
            m_paramRanges.insert(paramId, qMakePair(minValue, maxValue));

            QString pid = paramId;
            connect(slider, &QSlider::valueChanged, this, [this, pid, valLabel, minValue, maxValue](int val){
                const double ratio = qBound(0, val, 1000) / 1000.0;
                const float realVal = float(double(minValue) + (double(maxValue) - minValue) * ratio);
                valLabel->setText(parameterText(realVal));
                emit ParamChanged(pid, realVal, false);
            });
        }
    }

    // 约束信息
    if(constraintCount > 0){
        QTreeWidgetItem* constraintGroup = new QTreeWidgetItem(m_tree);
        constraintGroup->setText(0, QString("权重约束 (%1)").arg(constraintCount));

        for(int i = 0; i < constraintCount; i++){
            int paramIdx = sec.blend_shape_constraints[i]->parameter_indices;
            QString paramName = (paramIdx >= 0 && paramIdx < paramCount)
                ? sec.parameters[paramIdx]->ids : QString("[%1]").arg(paramIdx);
            int valueCount = sec.blend_shape_constraints[i]->blend_shape_constraint_value_sources_counts;

            QTreeWidgetItem* item = new QTreeWidgetItem(constraintGroup);
            item->setText(0, paramName);
            item->setText(1, QString("%1 个约束值").arg(valueCount));
        }
    }
}

void CBlendShapePanel::SyncParameterValue(const QString& paramId, float value)
{
    if (!std::isfinite(value) || !m_paramSliders.contains(paramId)) return;
    QSlider* slider = m_paramSliders.value(paramId);
    const auto range = m_paramRanges.value(paramId, qMakePair(0.0f, 1.0f));
    const float minValue = range.first;
    const float maxValue = range.second;
    const float bounded = qBound(minValue, value, maxValue);
    const int sliderValue = sliderPosition(bounded, minValue, maxValue);
    QSignalBlocker blocker(slider);
    slider->setValue(sliderValue);
    if (m_paramValueLabels.contains(paramId)) m_paramValueLabels.value(paramId)->setText(parameterText(bounded));
}
