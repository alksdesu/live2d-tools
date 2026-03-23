#ifndef CUBISMMODELUSERDATA_H
#define CUBISMMODELUSERDATA_H


#include "CubismModel.hpp"

namespace Live2D {  namespace Cubism {  namespace Framework {

typedef CubismIdHandle  ModelUserDataType;

class CubismModelUserData
{
public:

    struct CubismModelUserDataNode
    {
        ModelUserDataType   TargetType;     ///< ユーザデータターゲットタイプ
        CubismIdHandle      TargetId;       ///< ユーザデータターゲットのID
        csmString           Value;          ///< ユーザデータ
    };
    static CubismModelUserData* Create(const csmByte* buffer, csmSizeInt size);

    static void Delete(CubismModelUserData* modelUserData);

    virtual ~CubismModelUserData();
    const csmVector<const CubismModelUserDataNode*>& GetArtMeshUserDatas() const;

private:
    void ParseUserData(const csmByte* buffer, csmSizeInt size);
    csmVector<const CubismModelUserDataNode*>    _userDataNodes;        ///< ユーザデータ構造体配列
    csmVector<const CubismModelUserDataNode*>    _artMeshUserDataNodes; ///< 閲覧リスト保持
};
}}} //--------- LIVE2D NAMESPACE ------------

#endif // CUBISMMODELUSERDATA_H
