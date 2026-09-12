#include "coffscreenmap.h"

COffScreenMap::COffScreenMap() {}

COffScreenMap::~COffScreenMap()
{
    Clear();
}

void COffScreenMap::AddImageAndRect(const QString &name, QImage *image, const QRect &rect)
{
    auto existing = m_map.find(name);
    if (existing != m_map.end() && existing.value().m_image != image) delete existing.value().m_image;
    COffScreenNode node(image,rect);
    m_map.insert(name,node);
}

void COffScreenMap::AddSort(const QString &name)
{
    m_sort.push_back(name);
}

const QImage *COffScreenMap::GetIamge(const QString &name)
{
    const auto it = m_map.constFind(name);
    return it == m_map.cend() ? nullptr : it.value().m_image;
}

const QRect &COffScreenMap::GetRect(const QString &name)
{
    static const QRect emptyRect;
    const auto it = m_map.constFind(name);
    return it == m_map.cend() ? emptyRect : it.value().m_rect;
}

void COffScreenMap::Clear()
{
    for (auto it = m_map.begin(); it != m_map.end(); ++it) {
        delete it.value().m_image;
        it.value().m_image = nullptr;
    }
    m_map.clear();
    m_sort.clear();
}

void COffScreenMap::SetWH(int w, int h)
{
    m_w =w;
    m_h =h;

}
