#ifndef CPSDWRITELIST_H
#define CPSDWRITELIST_H
#include "psd/cpsdlist.h"
#include <QVector>
#include <QList>
#include "live2dstruct/cmodeldocument.h"
#include "ui/para/ccidjson.h"
#include <QPair>
#include "framework/Model/CubismModel.hpp"
#include <QTextCodec>
using namespace Live2D::Cubism::Framework;
using namespace live2dstruct;
class CPsdWriteList
{
public:
    enum LayerType{ Layer,GroupStart,GroupEnd  };
    CPsdWriteList();
    ~CPsdWriteList();

    bool CreatePsd(const QString& name,int w,int h,bool cubismCompat = false);
    bool WritePsd();
    static uint32_t CubismBlendModeToPsd(Live2D::Cubism::Framework::Rendering::CubismRenderer::CubismBlendMode mode);
    void AddLayer(const QString& layerName,const QRect& rect,const QImage& image );
    void AddLayer(const QString& layerName, const QRect& rect, const QImage& image,
                  uint8_t opacity, uint32_t blendModeKey, bool isClipped,
                  bool isPremultiplied = false);
    void AddGroupStart(const QString& groupName);
    void AddGroupEnd();
    void ClosePsd(){
        partNameToName.clear();
        m_groupNameStack.clear();
        m_nameUse.clear();
    }


    //获得绘制的顺序
    QVector<QPair<int,CPsdWriteList::LayerType>> GetSortDrawableIndexList(const QVector<int>& sortedDrawableIndexList,CubismModel* model);
    const QVector<QString>& GetArtToName(){
        return  partNameToName;
    }
    void SetMocName(const QString& name){
        m_mocName = name;
    }
    void SetCidName(const QString& name){
        m_CidPath = name;
    }


    void ClaerArtToName(){
        partNameToName.clear();
    }



private:
    QString UniqueName(const QString& name);


private:
    CPsdList* m_list;
    QHash<QString,int> m_nameUse;
    bool m_cubismCompat = false;
    QVector<QString> partNameToName;
    QVector<QString> m_groupNameStack;

    QString m_mocName;
    QString m_CidPath;
};

#endif // CPSDWRITELIST_H
