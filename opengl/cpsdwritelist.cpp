#include "cpsdwritelist.h"
#include "live2dstruct/cdraworder.h"
#include "std/cexportcontrol.h"

CPsdWriteList::CPsdWriteList() {
    m_list=nullptr;

}

CPsdWriteList::~CPsdWriteList()
{
    if(m_list!=nullptr){
        delete m_list;
    }
}

void CPsdWriteList::SetControl(CExportControl* control)
{
    m_control = control;
    if (m_list) m_list->SetControl(control);
}

uint32_t CPsdWriteList::CubismBlendModeToPsd(Live2D::Cubism::Framework::Rendering::CubismRenderer::CubismBlendMode mode)
{
    switch (mode) {
    case Live2D::Cubism::Framework::Rendering::CubismRenderer::CubismBlendMode_Additive: return 0x6C646467; // lddg
    case Live2D::Cubism::Framework::Rendering::CubismRenderer::CubismBlendMode_Multiplicative: return 0x6D756C20; // mul
    default: return 0x6E6F726D; // norm
    }
}

bool CPsdWriteList::CreatePsd(const QString &name, int w, int h, bool cubismCompat)
{
    if (m_list) {
        delete m_list;
        m_list = nullptr;
    }
    m_cubismCompat = cubismCompat;
    partNameToName.clear();
    m_groupNameStack.clear();
    m_nameUse.clear();
    m_list = new CPsdList();
    m_list->SetControl(m_control);
    if (m_list->SetPsd(name,w,h,cubismCompat)) return true;
    delete m_list;
    m_list = nullptr;
    return false;
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
    if (!m_list) {
        if (m_control) m_control->Fail(QStringLiteral("PSD 文档尚未创建"));
        return false;
    }
    bool ok = m_list->CreatePsd();
    delete m_list;
    m_list= nullptr;
    return ok;
}

bool CPsdWriteList::AddLayer(const QString& layerName, const QRect& rect, const QImage& image)
{
    return AddLayer(layerName, rect, image, 255, 0x6E6F726D, false, false);
}

bool CPsdWriteList::AddLayer(const QString& layerName, const QRect& rect, const QImage& image,
                             uint8_t opacity, uint32_t blendModeKey, bool isClipped, bool isPremultiplied)
{
    if (!m_list) {
        if (m_control) m_control->Fail(QStringLiteral("PSD 文档尚未创建"));
        return false;
    }
    return m_list->AddLayer(UniqueName(layerName), image, rect, opacity, blendModeKey, isClipped, isPremultiplied);
}

bool CPsdWriteList::AddGroupStart(const QString& groupName)
{
    if (!m_list) {
        if (m_control) m_control->Fail(QStringLiteral("PSD 文档尚未创建"));
        return false;
    }
    if (!m_list->AddGroupStart()) return false;
    m_groupNameStack.push_back(groupName);
    return true;
}

bool CPsdWriteList::AddGroupEnd()
{
    if (!m_list) {
        if (m_control) m_control->Fail(QStringLiteral("PSD 文档尚未创建"));
        return false;
    }
    const QString name = m_groupNameStack.isEmpty() ? QStringLiteral("Group") : m_groupNameStack.last();
    const QByteArray utf8 = UniqueName(name).toUtf8();
    if (!m_list->AddGroupEnd(utf8.constData())) return false;
    if (!m_groupNameStack.isEmpty()) m_groupNameStack.removeLast();
    return true;
}

QVector<QPair<int, CPsdWriteList::LayerType>> CPsdWriteList::GetSortDrawableIndexList(
    const QVector<int>& sortedDrawableIndexList, CubismModel* model)
{
    partNameToName.clear();
    if (!model || m_mocName.isEmpty()) return {};
    CModelDocument document;
    if (!document.Load(m_mocName)) return {};
    CCidJson cid;
    cid.Parse(m_CidPath);

    QHash<QString, int> sourceIndices;
    for (int i = 0; i < document.Drawables().size(); ++i) sourceIndices.insert(document.Drawables()[i].id, i);
    QHash<int, int> runtimeIndices;
    QVector<int> orderedSources;
    for (int index : sortedDrawableIndexList) {
        if (index < 0 || index >= model->GetDrawableCount()) continue;
        const auto id = model->GetDrawableId(index);
        if (!id) continue;
        const int source = sourceIndices.value(QString::fromUtf8(id->GetString().GetRawString()), -1);
        if (source < 0 || runtimeIndices.contains(source)) continue;
        runtimeIndices.insert(source, index);
        orderedSources.append(source);
    }

    QVector<QPair<int, LayerType>> result;
    const auto items = BuildGroupedLayerOrder(document.Parts(), document.Drawables(), orderedSources);
    for (const CLayerOrderItem& item : items) {
        QString name = item.id;
        if (item.type == CLayerOrderItem::Layer) {
            result.append({runtimeIndices.value(item.drawableIndex), Layer});
        } else {
            const QString displayName = cid.getPartName(item.id);
            if (!displayName.isEmpty()) name = displayName;
            result.append({0, item.type == CLayerOrderItem::GroupStart ? GroupStart : GroupEnd});
        }
        partNameToName.append(name);
    }
    return result;
}
