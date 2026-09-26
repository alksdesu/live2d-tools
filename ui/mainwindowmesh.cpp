#include "mainwindow.h"
#include "psdbatchjob.h"
#include "opengl/opengglform.h"
#include "edit/cmeshmap.h"

bool MainWindow::PrepareMeshMigration(const live2dstruct::CModelDocument& document,
                                      const live2dstruct::CModelEdit* command, bool forward,
                                      CMeshGeometryMigration& migration, QVariant* leaving, QString* error) const
{
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (!m_openglwindows || !m_openglwindows->IsModelReady()) return fail(QStringLiteral("网格预览尚未就绪"));
    CMeshGeometryMigration next;
    next.sourcePreviewDigest = m_openglwindows->CurrentPreviewDigest();
    CMeshEditorSnapshot current;
    if (!m_openglwindows->CaptureMeshEditorState(current, error)) return false;
    if (command) {
        if (command->field != live2dstruct::CModelEdit::MeshStructure || !command->runtime)
            return fail(QStringLiteral("网格历史缺少运行时状态"));
        for (const auto& remap : command->meshRemaps) {
            if (next.remaps.contains(remap.artMeshId)) return fail(QStringLiteral("网格顶点映射重复"));
            next.remaps.insert(remap.artMeshId, {forward ? remap.oldVertexCount : remap.newVertexCount,
                forward ? remap.newVertexCount : remap.oldVertexCount, forward ? remap.oldToNew : remap.newToOld});
        }
        const auto& archived = forward ? command->runtime->afterEditor : command->runtime->beforeEditor;
        if (archived.isValid()) {
            if (archived.metaType() != QMetaType::fromType<CMeshEditorSnapshot>())
                return fail(QStringLiteral("网格历史选区格式无效"));
            next.targetState = archived.value<CMeshEditorSnapshot>();
        }
    } else {
        CMeshEditorSnapshot target;
        for (const auto& mesh : document.Drawables()) {
            const auto old = current.constFind(mesh.id);
            if (old == current.cend() || target.contains(mesh.id)) return fail(QStringLiteral("会话网格目录与预览不匹配"));
            auto state = *old;
            state.vertexCount = mesh.vertexCount;
            state.selectedVertices.clear();
            target.insert(mesh.id, state);
            next.remaps.insert(mesh.id, {old->vertexCount, mesh.vertexCount, QVector<int>(old->vertexCount, -1)});
        }
        if (target.size() != current.size()) return fail(QStringLiteral("会话网格目录与预览数量不匹配"));
        next.targetState = std::move(target);
    }
    if (next.sourcePreviewDigest != m_openglwindows->CurrentPreviewDigest())
        return fail(QStringLiteral("捕获选区期间网格预览已改变"));
    QVariant snapshot;
    if (leaving) snapshot = QVariant::fromValue(current);
    migration = std::move(next);
    if (leaving) leaving->swap(snapshot);
    return true;
}

bool MainWindow::PublishMeshStructure(const live2dstruct::CModelDocument& document,
                                     const live2dstruct::CModelEdit& command, bool forward, QString* error)
{
    if (m_closingModel || m_batchExporting || m_psdBatch || m_previewExport.IsBusy() || !command.runtime) {
        if (error) *error = QStringLiteral("模型正在关闭或导出，请稍后编辑网格");
        return false;
    }
    if (!RuntimeMatchesDocument(error)) return false;
    QByteArray bytes;
    if (!document.BuildRuntimeMoc(bytes, error)) return false;
    if (!m_openglwindows || !m_openglwindows->IsModelReady()) return true;
    CMeshGeometryMigration migration;
    QVariant leaving;
    if (!PrepareMeshMigration(document, &command, forward, migration, &leaving, error) ||
        !m_openglwindows->ApplyMeshGeometryPreview(bytes, document.SourceDigest(), migration, error)) return false;
    (forward ? command.runtime->beforeEditor : command.runtime->afterEditor).swap(leaving);
    return true;
}
