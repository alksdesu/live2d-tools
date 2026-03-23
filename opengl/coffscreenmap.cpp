#include "coffscreenmap.h"

COffScreenMap::COffScreenMap() {}

void COffScreenMap::AddImageAndRect(const QString &name, QImage *image, const QRect &rect, unsigned char *imagebyte)
{
    COffScreenNode node(image,rect,imagebyte);
    m_map.insert(name,node);
}

void COffScreenMap::AddSort(const QString &name)
{
    m_sort.push_back(name);
}

const QImage *COffScreenMap::GetIamge(const QString &name)
{
    return m_map[name].m_image;
}

const QRect &COffScreenMap::GetRect(const QString &name)
{
    return m_map[name].m_rect;
}

void COffScreenMap::Clear()
{
    m_map.clear();
    m_sort.clear();
}

void COffScreenMap::SetWH(int w, int h)
{
    m_w =w;
    m_h =h;

}

