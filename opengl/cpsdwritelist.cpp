#include "cpsdwritelist.h"

CPsdWriteList::CPsdWriteList() {
    m_list=nullptr;

}

CPsdWriteList::~CPsdWriteList()
{
    if(m_list!=nullptr){
        delete m_list;
    }
}

uint32_t CPsdWriteList::CubismBlendModeToPsd(CubismRenderer::CubismBlendMode mode)
{
    switch (mode) {
    case CubismRenderer::CubismBlendMode_Additive: return 0x6C646467; // lddg
    case CubismRenderer::CubismBlendMode_Multiplicative: return 0x6D756C20; // mul
    default: return 0x6E6F726D; // norm
    }
}

void CPsdWriteList::CreatePsd(const QString &name, int w, int h, bool cubismCompat)
{
    if (m_list) {
        delete m_list;
        m_list = nullptr;
    }
    m_cubismCompat = cubismCompat;
    m_nameUse.clear();
    m_list = new CPsdList();
    m_list->SetPsd(name,w,h,cubismCompat);
}

// Cubism 导入时同名图层会关联错乱，兼容模式下对重名追加序号后缀
QString CPsdWriteList::UniqueName(const QString &name)
{
    if (!m_cubismCompat) {
        return name;
    }
    int& count = m_nameUse[name];
    ++count;
    if (count == 1) {
        return name;
    }
    return name + QString(" #%1").arg(count);
}

bool CPsdWriteList::WritePsd()
{
    bool ok = m_list->CreatePsd();
    delete m_list;
    m_list= nullptr;
    return ok;
}

void CPsdWriteList::AddLayer(const QString &layerName, const QRect &rect, const QImage &image)
{
    if(rect.width() > 0 && rect.height() > 0 && !image.isNull()){
        m_list->AddLayer(UniqueName(layerName),image,rect);
    }else{
        qDebug("CPsdWriteList::AddLayer skip empty: %s", layerName.toUtf8().constData());
    }
}

void CPsdWriteList::AddLayer(const QString &layerName, const QRect &rect, const QImage &image,
                             uint8_t opacity, uint32_t blendModeKey, bool isClipped,
                             bool isPremultiplied)
{
    if(rect.width() > 0 && rect.height() > 0 && !image.isNull()){
        m_list->AddLayer(UniqueName(layerName), image, rect, opacity, blendModeKey, isClipped, isPremultiplied);
    }else{
        qDebug("CPsdWriteList::AddLayer skip empty: %s", layerName.toUtf8().constData());
    }
}

void CPsdWriteList::AddGroupStart(const QString &groupName)
{
    // PSD 规范：GroupStart 必须命名为 "</Layer group>"，真实名称写在 GroupEnd。
    // 为保持名称一致，这里将名称暂存到栈中，GroupEnd 时再取出写入。
    m_groupNameStack.push_back(groupName);
    m_list->AddGroupStart();
}

void CPsdWriteList::AddGroupEnd()
{
    QString name = m_groupNameStack.isEmpty() ? QString("Group") : m_groupNameStack.takeLast();
    QByteArray utf8Text = UniqueName(name).toUtf8();
    m_list->AddGroupEnd(utf8Text.constData());
}



//moc的位置
QVector<QPair<int,CPsdWriteList::LayerType>> CPsdWriteList::GetSortDrawableIndexList( const QVector<int> &sortedDrawableIndexList,CubismModel* model)
{

    partNameToName.clear();
    m_sort.clear();
    QMap<QString,int> qMapNameToIndex;
    CCidJson* cid = new CCidJson();
    cid->Parse(m_CidPath);
    CReader* reader = new  CReader(m_mocName);
    CSectionTable* sectionTable = new CSectionTable(*reader);
    if (!sectionTable->IsValid()) {
        delete reader;
        delete sectionTable;
        delete cid;
        return {};
    }
    C2DTree* tree = new C2DTree(*sectionTable);
    int size = sortedDrawableIndexList.size();


    for(int i = 0 ;i < size ;i++){
        QString drName(model->GetDrawableId(sortedDrawableIndexList[i])->GetString().GetRawString()) ;
        m_sort.insert(drName,i);//权重
        qMapNameToIndex.insert(drName,sortedDrawableIndexList[i]);//顺序
    }

    //名称， 顺序
    //排序part
    Sort(tree->getPartRoot());

    m_logsort.clear();
    SetLogSort(tree->getPartRoot());



    QVector<QPair<int,CPsdWriteList::LayerType>> out;    //输出的indedx顺序

    // 将分组名称在开始与结束位置都映射为显示名（如中文），避免只在GroupEnd时替换导致Start使用原名
    for(int i = 0 ; i < m_logsort.size(); ++i){
        if(m_logsort[i] == m_nameGroupStart || m_logsort[i] == m_nameGroupEnd){
            QString part = partNameToName[i];
            QString disp = cid->getPartName(part);
            if(disp != ""){
                partNameToName[i] = disp;
            }
        }
    }


    for(int i = 0 ;i < m_logsort.size();i++){
        QString name = m_logsort[i];
        if(name == m_nameGroupStart){
            out.push_back(QPair<int,LayerType>(0,LayerType::GroupStart ) );
        }
        else if(name == m_nameGroupEnd){
            out.push_back(QPair<int,LayerType>(0,LayerType::GroupEnd ) );
        }
        else{
            out.push_back(QPair<int,LayerType>(qMapNameToIndex[name],LayerType::Layer) );
        }

        //这里换成中文名称输出


    }



    m_logsort.clear();
    m_sort.clear();
    delete cid;
    delete reader;
    delete sectionTable;
    delete tree;
    return out;
}






int CPsdWriteList::Sort(QList<C2DTreeNode *> &part)
{
    QVector<int> vetSerial;
    QVector<int> sizeNum;

    for(int i = 0 ;i < part.size() ;i++){
        if(dynamic_cast<CArtMeshNode*>(part[i])){//判断是是层
            vetSerial.push_back(i);
            if(m_sort.contains(part[i]->getName())){
                sizeNum.push_back(m_sort[part[i]->getName()]);//这里只有art的大小
            }else{
                sizeNum.push_back(INT_MAX);
            }
        }
        else if(dynamic_cast<CPartsNode*>(part[i])){//判断是群组
            vetSerial.push_back(i);
            sizeNum.push_back(Sort(part[i]->getChilds()));
        }
    }

    //已经知道排序和大小
    int conut = vetSerial.size();
    for(int i = 0 ;i < conut - 1 ;i++){
        for(int j = 0 ;j < conut-i-1 ; j++ ){
            if(sizeNum[j] > sizeNum[j+1]){
                C2DTreeNode* temp = part[vetSerial[j]];
                part[vetSerial[j]] = part[vetSerial[j+1]];
                part[vetSerial[j+1]] = temp;
                int ti = sizeNum[j];
                sizeNum[j] = sizeNum[j+1];
                sizeNum[j+1] = ti;
            }

        }
    }

    return sizeNum.size()==0 ? INT_MAX:sizeNum[0];
}

void CPsdWriteList::SetLogSort(QList<C2DTreeNode *> &part)
{

    for(int i = 0 ;i < part.size() ;i++){
        if(dynamic_cast<CArtMeshNode*>(part[i])){//判断是是层
            m_logsort.push_back(part[i]->getName());
            partNameToName.push_back(part[i]->getName());
        }
        else if(dynamic_cast<CPartsNode*>(part[i])){//判断是群组
            m_logsort.push_back(m_nameGroupStart);
            partNameToName.push_back(part[i]->getName());

            SetLogSort(part[i]->getChilds());

            m_logsort.push_back(m_nameGroupEnd);
            partNameToName.push_back(part[i]->getName());
        }
    }
}
