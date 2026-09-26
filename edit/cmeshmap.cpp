#include "cmeshmap.h"
#include <exception>
#include <utility>

CMeshMap::CMeshMap() {}

void CMeshMap::SetRenderOrder(const QStringList& names)
{
    m_readerSort.clear();
    QSet<QString> seen;
    for (const QString& name : names) {
        if (seen.contains(name) || !m_vetDrawMesh.value(name, nullptr)) continue;
        seen.insert(name);
        m_readerSort.append(name);
    }
}

bool CMeshMap::SetMeshPositions(const QString& name, const QVector<float>& positions)
{
    CDrawMesh* mesh = m_vetDrawMesh.value(name, nullptr);
    return mesh && mesh->SetPositions(positions);
}

bool CMeshMap::SetMeshIndices(const QString& name, const QVector<quint16>& indices)
{
    CDrawMesh* mesh = m_vetDrawMesh.value(name, nullptr);
    return mesh && mesh->SetIndices(indices);
}

bool CMeshMap::SelectNearestPoint(const QString& name, const QVector2D& position, float radius)
{
    CDrawMesh* mesh = m_vetDrawMesh.value(name, nullptr);
    if (!mesh) return false;
    for (auto it = m_vetDrawMesh.begin(); it != m_vetDrawMesh.end(); ++it)
        if (it.value() && it.key() != name) it.value()->CancelSelect();
    return mesh->SelectNearestPoint(position, radius);
}

bool CMeshMap::MoveSelected(const QString& name, const QVector2D& delta)
{
    CDrawMesh* mesh = m_vetDrawMesh.value(name, nullptr);
    if (!mesh) return false;
    mesh->MoveSelected(delta);
    return true;
}

bool CMeshMap::CopyEditorStateFrom(const CMeshMap& source)
{
    CMeshEditorSnapshot state;
    return source.CaptureEditorState(state) && RestoreEditorState(state);
}

bool CMeshMap::CaptureEditorState(CMeshEditorSnapshot& state, QString* error) const
{
    if (error) error->clear();
    try {
        CMeshEditorSnapshot candidate;
        for (auto it = m_vetDrawMesh.cbegin(); it != m_vetDrawMesh.cend(); ++it) {
            CDrawMesh::EditorState value;
            if (!it.value() || !it.value()->CaptureEditorState(value, error)) return false;
            candidate.insert(it.key(), std::move(value));
        }
        state.swap(candidate);
        return true;
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("无法保存模型网格选区");
        return false;
    }
}

bool CMeshMap::RestoreEditorState(const CMeshEditorSnapshot& state, QString* error)
{
    if (error) error->clear();
    try {
        if (state.size() != m_vetDrawMesh.size()) {
            if (error) *error = QStringLiteral("模型网格选区的对象集合不一致");
            return false;
        }
        std::vector<std::pair<CDrawMesh*, CDrawMesh::PreparedEditorState>> prepared;
        prepared.reserve(size_t(state.size()));
        for (auto it = state.cbegin(); it != state.cend(); ++it) {
            auto* mesh = m_vetDrawMesh.value(it.key(), nullptr);
            if (!mesh) {
                if (error) *error = QStringLiteral("模型网格选区引用了未知对象：%1").arg(it.key());
                return false;
            }
            CDrawMesh::PreparedEditorState value;
            if (!mesh->PrepareEditorState(it.value(), value, error)) return false;
            prepared.emplace_back(mesh, std::move(value));
        }
        for (auto& entry : prepared) entry.first->CommitEditorState(std::move(entry.second));
        return true;
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("无法恢复模型网格选区");
        return false;
    }
}

bool CMeshMap::CopyEditorStateFrom(const CMeshMap& source, const CMeshGeometryMigration& migration, QString* error)
{
    if (error) error->clear();
    const auto fail = [&](const QString& reason) { if (error) *error = reason; return false; };
    try {
        CMeshEditorSnapshot state, target;
        if (!source.CaptureEditorState(state, error) || !CaptureEditorState(target, error)) return false;
        if (state.keys() != target.keys()) return fail(QStringLiteral("候选网格对象集合发生变化"));
        for (auto it = migration.remaps.cbegin(); it != migration.remaps.cend(); ++it) {
            const auto old = state.constFind(it.key());
            const auto next = target.constFind(it.key());
            const auto& remap = it.value();
            if (old == state.cend() || next == target.cend() || old->vertexCount != remap.oldVertexCount ||
                next->vertexCount != remap.newVertexCount || remap.oldToNew.size() != remap.oldVertexCount)
                return fail(QStringLiteral("顶点映射与模型不一致：%1").arg(it.key()));
            QVector<unsigned char> seen(remap.newVertexCount, 0);
            for (int index : remap.oldToNew) {
                if (index < -1 || index >= remap.newVertexCount || (index >= 0 && seen[index]))
                    return fail(QStringLiteral("顶点映射越界或重复：%1").arg(it.key()));
                if (index >= 0) seen[index] = 1;
            }
        }
        for (auto it = state.begin(); it != state.end(); ++it) {
            const auto remap = migration.remaps.constFind(it.key());
            const int count = target.value(it.key()).vertexCount;
            if (remap == migration.remaps.cend()) {
                if (count != it->vertexCount) return fail(QStringLiteral("缺少顶点身份映射：%1").arg(it.key()));
                continue;
            }
            QVector<int> selected;
            selected.reserve(it->selectedVertices.size());
            for (int index : it->selectedVertices)
                if (remap->oldToNew[index] >= 0) selected.append(remap->oldToNew[index]);
            it->selectedVertices.swap(selected);
            it->vertexCount = count;
        }
        return RestoreEditorState(migration.targetState ? *migration.targetState : state, error);
    } catch (const std::exception&) {
        return fail(QStringLiteral("无法准备顶点选区迁移"));
    }
}

QVector<float> CMeshMap::MeshPositions(const QString& name) const
{
    const CDrawMesh* mesh = m_vetDrawMesh.value(name, nullptr);
    return mesh ? mesh->Positions() : QVector<float>();
}

void CMeshMap::clear()
{
    for (QMap<QString, CDrawMesh*>::iterator i = m_vetDrawMesh.begin(); i != m_vetDrawMesh.end(); ++i) {
        delete i.value();
    }
    m_vetDrawMesh.clear();
    m_vetImage.clear();
    m_readerSort.clear();
}

void CMeshMap::DrawMesh(const QString &drawKey, const QMatrix4x4 &mvp)
{
    if(m_vetDrawMesh.contains(drawKey)){
        m_vetDrawMesh[drawKey]->Draw(mvp);
    }
}

void CMeshMap::SetShow(const QString &artName)
{
    for (QString key : m_vetDrawMesh.keys()) {
        if(key == artName ){
            CDrawMesh* mesh = m_vetDrawMesh[artName];
            mesh->SetShow(true);
        }else{
            m_vetDrawMesh[key]->SetShow(false);
        }
    }
}
void CMeshMap::SetShowMultiple(const QStringList& artNames)
{
    QSet<QString> nameSet(artNames.begin(), artNames.end());
    for(auto it = m_vetDrawMesh.begin(); it != m_vetDrawMesh.end(); ++it){
        it.value()->SetShow(nameSet.contains(it.key()));
    }
}

void CMeshMap::HideAll()
{
    for(auto it = m_vetDrawMesh.begin(); it != m_vetDrawMesh.end(); ++it){
        it.value()->SetShow(false);
    }
}

QString CMeshMap::SetOver(const QPointF &point, bool IsOver)
{
    QString re="";
    bool isFind = false;
    for(int i = m_readerSort.size()-1 ;i >=0 ;i--){
        auto meshIt = m_vetDrawMesh.find(m_readerSort[i]);
        if (meshIt == m_vetDrawMesh.end() || !meshIt.value()) continue;
        if(!isFind && meshIt.value()->ClickMesh(point)){
            re =m_readerSort[i];
            if(IsOver){
                meshIt.value()->SetIsOver(true);
            }else{
                meshIt.value()->SetShow(true);
            }
            isFind =true;
        }
        else{
            if(IsOver){
                meshIt.value()->SetIsOver(false);
            }else{
                meshIt.value()->SetShow(false);
            }

        }
    }
    // for (auto it = m_vetDrawMesh.begin(); it != m_vetDrawMesh.end(); ++it) {
    //     if(!isFind && it.value()->ClickMesh(point)){
    //         re =it.key();
    //         if(IsOver){
    //             it.value()->SetIsOver(true);
    //         }else{
    //             it.value()->SetShow(true);
    //         }
    //         isFind =true;
    //     }
    //     else{
    //         if(IsOver){
    //             it.value()->SetIsOver(false);
    //         }else{
    //             it.value()->SetShow(false);
    //         }

    //     }

    // }
    return re;
}
