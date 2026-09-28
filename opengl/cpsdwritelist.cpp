#include "cpsdwritelist.h"
#include "live2dstruct/cdraworder.h"
#include "std/cexportcontrol.h"
#include <QScopedValueRollback>

CPsdWriteList::CPsdWriteList() {
    m_list=nullptr;

}

CPsdWriteList::~CPsdWriteList()
{
    if(m_list!=nullptr){
        delete m_list;
    }
}

bool CPsdWriteList::Fail(const QString& error)
{
    if (m_control && !m_control->WasCanceled() && !m_control->IsCancellationRequested())
        m_control->Fail(error);
    return false;
}

bool CPsdWriteList::RejectReentry()
{
    if (!m_busy) return false;
    Fail(QStringLiteral("PSD 导出操作不能在回调中重入"));
    return true;
}

void CPsdWriteList::SetControl(CExportControl* control)
{
    if (RejectReentry()) return;
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
    if (RejectReentry()) return false;
    const QScopedValueRollback<bool> busy(m_busy, true);
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
    const QString frozen(name.constData(), name.size());
    if (!m_cubismCompat) {
        return frozen;
    }
    int& count = m_nameUse[frozen];
    ++count;
    if (count == 1) {
        return frozen;
    }
    return frozen + QString(" #%1").arg(count);
}

bool CPsdWriteList::WritePsd(CPsdPreviewKind* preview)
{
    if (RejectReentry()) return false;
    const QScopedValueRollback<bool> busy(m_busy, true);
    if (preview) *preview = CPsdPreviewKind::Unknown;
    if (!m_list) return Fail(QStringLiteral("PSD 文档尚未创建"));
    bool ok = m_list->CreatePsd();
    if (ok && preview) *preview = m_list->Preview();
    delete m_list;
    m_list= nullptr;
    return ok;
}

bool CPsdWriteList::AddLayer(const QString& layerName, const QRect& rect, const QImage& image)
{
    return AddLayer(layerName, rect, image, 255, 0x6E6F726D, false, false);
}

bool CPsdWriteList::AddLayer(const QString& layerName, const QRect& rect, const QImage& image,
                             uint8_t opacity, uint32_t blendModeKey, bool isClipped, bool isPremultiplied,
                             std::optional<quint32> layerId)
{
    if (RejectReentry()) return false;
    const QScopedValueRollback<bool> busy(m_busy, true);
    if (!m_list) return Fail(QStringLiteral("PSD 文档尚未创建"));
    return m_list->AddLayer(UniqueName(layerName), image, rect, opacity, blendModeKey, isClipped, isPremultiplied, layerId);
}

bool CPsdWriteList::AddEmptyLayer(const QString& name, quint8 opacity, quint32 blendModeKey,
                                 bool isClipped, std::optional<quint32> layerId)
{
    if (RejectReentry()) return false;
    const QScopedValueRollback<bool> busy(m_busy, true);
    if (!m_list) return Fail(QStringLiteral("PSD 文档尚未创建"));
    return m_list->AddEmptyLayer(UniqueName(name), opacity, blendModeKey, isClipped, layerId);
}

bool CPsdWriteList::AddGroupStart(const QString& groupName)
{
    if (RejectReentry()) return false;
    const QScopedValueRollback<bool> busy(m_busy, true);
    const QString frozenName(groupName.constData(), groupName.size());
    if (!m_list) return Fail(QStringLiteral("PSD 文档尚未创建"));
    if (!m_list->AddGroupStart()) return false;
    m_groupNameStack.push_back(frozenName);
    return true;
}

bool CPsdWriteList::AddGroupEnd()
{
    if (RejectReentry()) return false;
    const QScopedValueRollback<bool> busy(m_busy, true);
    if (!m_list) return Fail(QStringLiteral("PSD 文档尚未创建"));
    const QString name = m_groupNameStack.isEmpty() ? QStringLiteral("Group") : m_groupNameStack.last();
    const QByteArray utf8 = UniqueName(name).toUtf8();
    if (!m_list->AddGroupEnd(utf8.constData())) return false;
    if (!m_groupNameStack.isEmpty()) m_groupNameStack.removeLast();
    return true;
}

QVector<QPair<int, CPsdWriteList::LayerType>> CPsdWriteList::GetSortDrawableIndexList(
    const QVector<int>& sortedDrawableIndexList, CubismModel* model)
{
    if (RejectReentry()) return {};
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
