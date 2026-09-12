#ifndef COFFSCREENMAP_H
#define COFFSCREENMAP_H

#include <QImage>
#include <QRect>
#include <QVector>
#include <QMap>
#include <QPainter>
class COffScreenNode{
public:
    COffScreenNode(QImage* image,const QRect& rect){
        m_image = image;
        m_rect = rect;
    }
    COffScreenNode(){
        m_image = nullptr;
    }
    ~COffScreenNode(){
        // 所有权由消费方显式 Clear() 管理，不在析构中释放：
        // COffScreenNode 按值存入 QMap 并被拷贝，析构释放会导致二次释放
    }

    void Clear(){
        delete m_image;
        m_image = nullptr;
    }
    QImage* m_image;//因为图像占用的内存过大，所有用指针保存
    QRect m_rect;
};

class COffScreenMap
{
public:


    COffScreenMap();
    ~COffScreenMap();
    void AddImageAndRect(const QString& name,QImage* image,const QRect& rect);
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
