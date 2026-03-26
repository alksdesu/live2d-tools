#include "cmeshmap.h"

CMeshMap::CMeshMap() {}

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
        if(!isFind && m_vetDrawMesh[m_readerSort[i]]->ClickMesh(point)){
            re =m_readerSort[i];
            if(IsOver){
                m_vetDrawMesh[m_readerSort[i]]->SetIsOver(true);
            }else{
                m_vetDrawMesh[m_readerSort[i]]->SetShow(true);
            }
            isFind =true;
        }
        else{
            if(IsOver){
                m_vetDrawMesh[m_readerSort[i]]->SetIsOver(false);
            }else{
                m_vetDrawMesh[m_readerSort[i]]->SetShow(false);
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
