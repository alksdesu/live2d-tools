#ifndef COFFSCREENMAP_H
#define COFFSCREENMAP_H

#include <QImage>
#include <QRect>
#include <QVector>
#include <QMap>
#include <QPainter>
class COffScreenNode{
public:
    COffScreenNode(QImage* image,const QRect& rect,unsigned char*  imagebyte){
        m_image = image;
        m_rect = rect;
        m_imagebyte = imagebyte;
    }
    COffScreenNode(){
        m_image = nullptr;
        m_imagebyte =nullptr;
    }
    ~COffScreenNode(){
        // Note: ownership of m_image and m_imagebyte is managed by the
        // consumer (via explicit Clear() calls), not by the destructor,
        // because COffScreenNode is stored by value in QMap and copied.
    }

    void Clear(){
        if(m_imagebyte!=nullptr){
             delete m_imagebyte;
         }
        if(m_image != nullptr){
          delete m_image;
        }
        m_image = nullptr;
        m_imagebyte=nullptr;
    }
    QImage* m_image;//因为图像占用的内存过大，所有用指针保存
    QRect m_rect;
    unsigned char* m_imagebyte;

    //因为生成的图像数据会特别的大，所以

};

class COffScreenMap
{
public:


    COffScreenMap();
    void AddImageAndRect(const QString& name,QImage* image,const QRect& rect,unsigned char*  imagebyte);
    void AddSort(const QString& name);
    const QImage* GetIamge(const QString& name);
    const QRect& GetRect(const QString& name);
    void Clear();
    void SetWH(int w,int h);    //最好的办法是用一个删除一个
private:
    int m_w;
    int m_h;
    QMap<QString,COffScreenNode> m_map;
    QVector<QString> m_sort;
};

#endif // COFFSCREENMAP_H
