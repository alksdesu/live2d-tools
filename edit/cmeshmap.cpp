#include "cmeshmap.h"

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
