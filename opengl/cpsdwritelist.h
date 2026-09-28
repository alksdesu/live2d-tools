#ifndef CPSDWRITELIST_H
#define CPSDWRITELIST_H
#include "psd/cpsdlist.h"
#include <QVector>
#include <optional>
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
    void SetControl(CExportControl* control);
    CExportControl* Control() const { return m_control; }

    bool CreatePsd(const QString& name,int w,int h,bool cubismCompat = false);
    bool WritePsd(CPsdPreviewKind* preview = nullptr);
    static uint32_t CubismBlendModeToPsd(Live2D::Cubism::Framework::Rendering::CubismRenderer::CubismBlendMode mode);
    bool AddLayer(const QString& layerName,const QRect& rect,const QImage& image );
    bool AddLayer(const QString& layerName, const QRect& rect, const QImage& image,
                  uint8_t opacity, uint32_t blendModeKey, bool isClipped,
                  bool isPremultiplied = false, std::optional<quint32> layerId = {});
    bool AddEmptyLayer(const QString& name, quint8 opacity = 255, quint32 blendModeKey = 0x6e6f726d,
                       bool isClipped = false, std::optional<quint32> layerId = {});
    bool AddGroupStart(const QString& groupName);
    bool AddGroupEnd();
    void ClosePsd(){
        if (RejectReentry()) return;
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
        if (RejectReentry()) return;
        m_mocName = name;
    }
    void SetCidName(const QString& name){
        if (RejectReentry()) return;
        m_CidPath = name;
    }


    void ClaerArtToName(){
        if (RejectReentry()) return;
        partNameToName.clear();
    }



private:
    bool Fail(const QString& error);
    bool RejectReentry();
    QString UniqueName(const QString& name);


private:
    CPsdList* m_list;
    CExportControl* m_control = nullptr;
    bool m_busy = false;
    QHash<QString,int> m_nameUse;
    bool m_cubismCompat = false;
    QVector<QString> partNameToName;
    QVector<QString> m_groupNameStack;

    QString m_mocName;
    QString m_CidPath;
};

#endif // CPSDWRITELIST_H
