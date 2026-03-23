#ifndef CUBISMMATRIX44_H
#define CUBISMMATRIX44_H


#include "framework/Type/CubismBasicType.hpp"
#include <QDebug>
#include <QMatrix4x4>
namespace Live2D { namespace Cubism { namespace Framework {

/**
 * @brief 4x4の行列
 *
 * 4x4行列の便利クラス。
 */
class CubismMatrix44
{
public:
    CubismMatrix44();
    virtual ~CubismMatrix44();
    static void Multiply(csmFloat32* a, csmFloat32* b, csmFloat32* dst);
    void            LoadIdentity();
    csmFloat32*     GetArray();
    void            SetMatrix(csmFloat32* tr);
    csmFloat32      GetScaleX() const;
    csmFloat32      GetScaleY() const;
    csmFloat32      GetTranslateX() const;
    csmFloat32      GetTranslateY() const;
    csmFloat32      TransformX(csmFloat32 src);
    csmFloat32      TransformY(csmFloat32 src);
    csmFloat32      InvertTransformX(csmFloat32 src);
    csmFloat32      InvertTransformY(csmFloat32 src);
    void            TranslateRelative(csmFloat32 x, csmFloat32 y);
    void            Translate(csmFloat32 x, csmFloat32 y);
    void            Translation(csmFloat32 x, csmFloat32 y);
    void            TranslateX(csmFloat32 x);
    void            TranslateY(csmFloat32 y);
    void            ScaleRelative(csmFloat32 x, csmFloat32 y);
    void            Scale(csmFloat32 x, csmFloat32 y);
    void            MultiplyByMatrix(CubismMatrix44* m);
    void            DumpMatrix();
    QMatrix4x4      GetQMatrix44();


protected:
    csmFloat32  _tr[16];     ///< 4x4行列データ
};

}}}

#endif // CUBISMMATRIX44_H
