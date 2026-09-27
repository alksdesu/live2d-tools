#include "mainwindow.h"
#include "psdbatchjob.h"
#include "cdeformercreatedialog.h"
#include "cmodeltreepanel.h"
#include "live2d/LAppLive2DManager.hpp"
#include "live2d/LAppModel.hpp"
#include "live2dstruct/cdeformerstructurereplay.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/tree/cartmeshnode.h"
#include "live2dstruct/tree/crotationnode.h"
#include <QCryptographicHash>
#include <QMessageBox>
#include <QStatusBar>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
using Builder = live2dstruct::CMocDeformerStructureBuilder;
using Document = live2dstruct::CModelDocument;
using Deformer = live2dstruct::CModelDeformer;

QMap<QString, QVector<float>> rotationChildPoints(LAppModel& model, const QStringList& ids, const QStringList& types)
{
    QMap<QString, QVector<float>> result;
    const auto& snapshot = live2dstruct::CTreeManage::CurrentSnapshot();
    if (!snapshot.HasValidEvaluation() || snapshot.MocDigest() != model.GetPreviewMocDigest() ||
        snapshot.Pose() != model.GetParameterValues()) return result;
    auto* core = model.GetModel();
    QHash<QString, int> meshes;
    for (int index = 0; index < core->GetDrawableCount(); ++index)
        meshes.insert(QString::fromUtf8(core->GetDrawableId(index)->GetString().GetRawString()), index);
    for (int selected = 0; selected < ids.size() && selected < types.size(); ++selected) {
        if (types[selected] != QLatin1String("rotation")) continue;
        const auto found = std::find_if(snapshot.Deformers().cbegin(), snapshot.Deformers().cend(), [&](auto* node) {
            return node->getName() == ids[selected] && dynamic_cast<live2dstruct::CRotationNode*>(node);
        });
        if (found == snapshot.Deformers().cend()) continue;
        auto* target = *found;
        QList<live2dstruct::C2DTreeNode*> pending = target->getChilds();
        QSet<live2dstruct::C2DTreeNode*> visited;
        QVector<float> points;
        bool complete = true;
        while (!pending.isEmpty() && complete) {
            auto* node = pending.takeLast();
            if (!node || visited.contains(node)) continue;
            visited.insert(node);
            if (dynamic_cast<live2dstruct::CArtMeshNode*>(node)) {
                const auto index = meshes.constFind(node->getName());
                if (index == meshes.cend()) { complete = false; break; }
                const int count = core->GetDrawableVertexCount(*index);
                const auto* vertices = core->GetDrawableVertices(*index);
                std::vector<float> canvas, local;
                canvas.reserve(count * 2);
                for (int vertex = 0; vertex < count; ++vertex) {
                    canvas.push_back(vertices[vertex * 2]);
                    canvas.push_back(-vertices[vertex * 2 + 1]);
                }
                if (!target->MapFromCanvas(canvas, local)) { complete = false; break; }
                for (float value : local) points.append(value);
            } else pending.append(node->getChilds());
        }
        if (complete && !points.isEmpty()) result.insert(ids[selected], std::move(points));
    }
    return result;
}

bool createContext(const Document& document, const QStringList& ids, const QStringList& types,
                   const QByteArray& bytes, const QMap<QString, QVector<float>>& descendants,
                   CDeformerCreateDialog::Context& context, QString* error)
{
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (ids.size() != types.size()) return fail(QStringLiteral("选区信息不完整，请重新选择对象"));
    context.mocVersion = bytes.size() > 4 ? static_cast<quint8>(bytes.at(4)) : 0;
    for (const auto& node : document.Deformers()) context.existingIds.insert(node.id);
    for (int suffix = 1; ; ++suffix) {
        context.suggestedId = QStringLiteral("Deformer%1").arg(suffix);
        if (!context.existingIds.contains(context.suggestedId)) break;
    }
    int parent = -1, part = -1;
    bool first = true, hasPoint = false;
    double left = 0, top = 0, right = 0, bottom = 0;
    const auto point = [&](double x, double y) {
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        if (!hasPoint) { left = right = x; top = bottom = y; hasPoint = true; }
        else { left = std::min(left, x); right = std::max(right, x); top = std::min(top, y); bottom = std::max(bottom, y); }
    };
    const auto positions = [&](const QVector<float>& values) {
        for (int i = 0; i + 1 < values.size(); i += 2) point(values[i], values[i + 1]);
    };
    if (ids.size() == 1 && types.first() == QLatin1String("part")) {
        const auto found = std::find_if(document.Parts().cbegin(), document.Parts().cend(), [&](const auto& item) { return item.id == ids.first(); });
        if (found == document.Parts().cend()) return fail(QStringLiteral("选中的部件已不存在"));
        part = static_cast<int>(found - document.Parts().cbegin());
    } else for (int i = 0; i < ids.size(); ++i) {
        int childParent = -1, childPart = -1;
        if (types[i] == QLatin1String("artmesh")) {
            const auto found = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(), [&](const auto& mesh) { return mesh.id == ids[i]; });
            if (found == document.Drawables().cend()) return fail(QStringLiteral("选中的网格已不存在"));
            childParent = found->parentDeformerIndex;
            childPart = found->parentPartIndex;
            context.children.append({Builder::Owner::ArtMesh, ids[i]});
            for (int key = 0; key < found->keyforms.size(); ++key) positions(document.DrawableKeyformPositions(ids[i], key));
        } else if (types[i] == QLatin1String("warp") || types[i] == QLatin1String("rotation")) {
            const auto found = std::find_if(document.Deformers().cbegin(), document.Deformers().cend(), [&](const auto& node) {
                return node.id == ids[i] && (node.type == Deformer::Warp) == (types[i] == QLatin1String("warp"));
            });
            if (found == document.Deformers().cend()) return fail(QStringLiteral("选中的变形器已不存在"));
            childParent = document.DeformerParentIndex(ids[i]);
            childPart = found->parentPartIndex;
            context.children.append({found->type == Deformer::Warp ? Builder::Owner::Warp : Builder::Owner::Rotation, ids[i]});
            if (found->type == Deformer::Warp) for (const auto& key : document.DeformerWarpKeyforms(ids[i])) positions(key.positions);
            else {
                for (const auto& key : document.DeformerRotationKeyforms(ids[i])) point(key.originX, key.originY);
                positions(descendants.value(ids[i]));
                if (!descendants.contains(ids[i]))
                    context.warpGridHint = QStringLiteral("当前无法获取完整子对象外框，建议范围围绕旋转中心建立");
            }
        } else return fail(QStringLiteral("请选择网格或变形器；也可以单选部件创建空变形器"));
        if (!first && (childParent != parent || childPart != part))
            return fail(QStringLiteral("请选中同一部件、同一直接父级下的对象"));
        parent = childParent; part = childPart; first = false;
    }
    context.partIndex = part;
    context.partLabel = part >= 0 && part < document.Parts().size() ? document.Parts()[part].id : QStringLiteral("模型根级");
    context.parentLabel = QStringLiteral("无父级变形器");
    context.selectionSummary = context.children.isEmpty() ? QStringLiteral("创建空变形器")
        : QStringLiteral("将 %1 个选中对象移入新变形器").arg(context.children.size());
    if (parent >= 0) {
        if (parent >= document.Deformers().size()) return fail(QStringLiteral("选中对象的父级无效"));
        const auto& node = document.Deformers()[parent];
        context.parentId = node.id;
        context.parentLabel = node.id;
        if (node.type == Deformer::Warp) {
            context.warp.rows = node.rows;
            context.warp.columns = node.columns;
            context.warp.quad = node.quadTransform;
            context.warpBounds = QRectF(0, 0, 1, 1);
            context.lockWarpGrid = true;
            context.warpGridHint = QStringLiteral("沿用父级网格的范围与行列，保留现有变形和动画");
        }
    }
    double fallback = 1.0;
    if (parent < 0) {
        auto reader = live2dstruct::CReader::FromBytes(bytes);
        live2dstruct::CSectionTable table(reader);
        if (!table.IsValid() || !table.canvas_info || !std::isfinite(table.canvas_info->pixels_per_unit) ||
            table.canvas_info->pixels_per_unit <= 0) return fail(QStringLiteral("模型画布比例无效"));
        fallback = std::max(table.canvas_info->width, table.canvas_info->height) / table.canvas_info->pixels_per_unit * .1;
        if (!std::isfinite(fallback) || fallback <= 0) return fail(QStringLiteral("模型画布范围无效"));
    }
    if (!hasPoint) { left = top = -fallback * .5; right = bottom = fallback * .5; }
    const double width = std::max(right - left, fallback * .1), height = std::max(bottom - top, fallback * .1);
    const double centerX = left + (right - left) * .5, centerY = top + (bottom - top) * .5;
    if (!context.warpBounds) context.warpBounds = QRectF(centerX - width * .55, centerY - height * .55, width * 1.1, height * 1.1);
    context.rotation.originX = static_cast<float>(centerX);
    context.rotation.originY = static_cast<float>(centerY);
    context.startWithRotation = types.size() == 1 && types.first() == QLatin1String("rotation");
    return true;
}
}

bool MainWindow::PublishDeformerStructure(const live2dstruct::CModelDocument& document,
                                          const live2dstruct::CModelEdit& command, bool forward, QString* error)
{
    live2dstruct::CKeyformEditController::EditorState editor;
    if (!live2dstruct::CKeyformEditController::ReadDeformerEditorState(command, forward, editor, error)) return false;
    auto* manager = m_openglwindows && m_openglwindows->IsModelReady() ? LAppLive2DManager::GetInstance() : nullptr;
    auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
    if (!model) { if (error) *error = QStringLiteral("请等待模型预览就绪再编辑变形器"); return false; }
    auto pose = editor.pose;
    return PublishParameterStructure(document, command, forward, false, {}, editor.keyform >= 0, pose, error);
}

void MainWindow::RefreshDeformerActions()
{
    if (!m_treePanel) return;
    const bool ready = m_workspaceReady && !m_closingModel && !m_batchExporting && !m_psdBatch &&
        !m_previewExport.IsBusy() && m_openglwindows && m_openglwindows->IsModelReady();
    const bool single = m_selectedObjectIds.size() == 1 && m_selectedObjectTypes.size() == 1;
    const bool deformer = single && (m_selectedObjectTypes.first() == QLatin1String("warp") ||
                                     m_selectedObjectTypes.first() == QLatin1String("rotation"));
    m_treePanel->SetDeformerActionsEnabled(ready, ready && deformer &&
        !m_document.IsLocked(m_selectedObjectTypes.first(), m_selectedObjectIds.first()));
    RefreshGeometryPairActions();
}

void MainWindow::CreateDeformer()
{
    if (!m_workspaceReady || m_closingModel || m_batchExporting || m_psdBatch || m_previewExport.IsBusy() ||
        !m_openglwindows || !m_openglwindows->IsModelReady()) return;
    QString error;
    QByteArray bytes;
    if (!m_openglwindows->FinishMeshDrag(false, &error) || !m_document.BuildPreviewMoc(bytes, &error)) {
        statusBar()->showMessage(error, 8000); return;
    }
    CDeformerCreateDialog::Context context;
    auto* model = LAppLive2DManager::GetInstance()->GetModel(0);
    const auto descendants = model ? rotationChildPoints(*model, m_selectedObjectIds, m_selectedObjectTypes)
                                   : QMap<QString, QVector<float>>();
    if (!createContext(m_document, m_selectedObjectIds, m_selectedObjectTypes, bytes, descendants, context, &error)) {
        QMessageBox::information(this, QStringLiteral("新建变形器"), error); return;
    }
    const auto revision = m_document.Revision();
    const auto source = m_document.SourceDigest();
    const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    const auto selectedIds = m_selectedObjectIds, selectedTypes = m_selectedObjectTypes;
    const auto prepare = [this, revision, source, digest, selectedIds, selectedTypes](const Builder::InsertRequest& operation,
                            live2dstruct::CModelEdit& command, QString* detail) {
        if (m_document.Revision() != revision || m_document.SourceDigest() != source ||
            m_selectedObjectIds != selectedIds || m_selectedObjectTypes != selectedTypes ||
            !m_workspaceReady || m_closingModel || m_batchExporting || m_psdBatch || m_previewExport.IsBusy()) {
            if (detail) *detail = QStringLiteral("模型或选区已改变，请关闭对话框后重新创建");
            return false;
        }
        return m_document.MakeDeformerStructureEdit({operation, source, digest}, command, detail);
    };
    CDeformerCreateDialog dialog(std::move(context),
        [prepare, bytes](const auto& operation, QString* summary, QString* detail) {
            live2dstruct::CModelEdit command;
            if (!prepare(operation, command, detail)) return false;
            *summary = operation.children.isEmpty() ? QStringLiteral("可创建空变形器。支持撤销与会话保存。")
                : QStringLiteral("已通过保形检查，%1 个对象的全部关键形会一并转换。支持撤销。").arg(operation.children.size());
            live2dstruct::CDeformerStructureReplay::Impact impact;
            if (!live2dstruct::CDeformerStructureReplay::Analyze(bytes, *command.deformerRequest, impact, detail)) return false;
            if (!impact.numericalLimitations.isEmpty()) *summary += QStringLiteral("\n保形转换可能产生极小的坐标舍入误差。");
            return true;
        }, [this, prepare](const auto& operation, QString* detail) {
            live2dstruct::CModelEdit command;
            if (!prepare(operation, command, detail) || !SubmitInspectorEdit(command, detail)) return false;
            const QString type = std::holds_alternative<Builder::Warp>(operation.shape) ? QStringLiteral("warp") : QStringLiteral("rotation");
            SelectModelObjects({operation.id}, {type});
            if (m_treePanel) m_treePanel->HighlightObjects(m_selectedObjectIds, m_selectedObjectTypes);
            if (!m_keyformEditor.IsActive()) m_openglwindows->do_ShowDeformers({operation.id});
            statusBar()->showMessage(QStringLiteral("已创建变形器 %1，可撤销").arg(operation.id), 5000);
            return true;
        }, this);
    dialog.exec();
}

void MainWindow::RemoveDeformer()
{
    if (!m_workspaceReady || m_closingModel || m_batchExporting || m_psdBatch || m_previewExport.IsBusy() ||
        !m_openglwindows || !m_openglwindows->IsModelReady() || m_selectedObjectIds.size() != 1 ||
        m_selectedObjectTypes.size() != 1 || (m_selectedObjectTypes.first() != QLatin1String("warp") &&
        m_selectedObjectTypes.first() != QLatin1String("rotation"))) return;
    QString error;
    QByteArray bytes;
    live2dstruct::CModelEdit command;
    const auto id = m_selectedObjectIds.first();
    if (!m_openglwindows->FinishMeshDrag(false, &error) || !m_document.BuildPreviewMoc(bytes, &error)) {
        statusBar()->showMessage(error, 8000); return;
    }
    const auto revision = m_document.Revision();
    const live2dstruct::CDeformerPreserveRequest request{Builder::RemoveRequest{id}, m_document.SourceDigest(),
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)};
    live2dstruct::CDeformerStructureReplay::Impact impact;
    if (!live2dstruct::CDeformerStructureReplay::Analyze(bytes, request, impact, &error) ||
        !m_document.MakeDeformerStructureEdit(request, command, &error)) {
        QMessageBox::information(this, QStringLiteral("无法删除变形器"), error); return;
    }
    QMessageBox confirm(QMessageBox::Question, QStringLiteral("删除变形器"),
        QStringLiteral("删除 %1？\n%2 个直接子对象将移至原父级，全部关键形会保留。此操作可以撤销。")
            .arg(id).arg(impact.structure.reparented.size()), QMessageBox::Cancel, this);
    auto* remove = confirm.addButton(QStringLiteral("删除并保留形状"), QMessageBox::AcceptRole);
    if (!impact.numericalLimitations.isEmpty()) confirm.setInformativeText(QStringLiteral("保形转换可能产生极小的坐标舍入误差。"));
    confirm.setDefaultButton(QMessageBox::Cancel);
    confirm.exec();
    if (confirm.clickedButton() != remove) return;
    if (m_document.Revision() != revision) error = QStringLiteral("模型已改变，请重新执行删除");
    else if (SubmitInspectorEdit(command, &error)) {
        statusBar()->showMessage(QStringLiteral("已删除变形器 %1，可撤销").arg(id), 5000); return;
    }
    QMessageBox::warning(this, QStringLiteral("删除未完成"), error);
}
