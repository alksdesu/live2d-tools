#ifndef CUBISMID_H
#define CUBISMID_H



#include "framework/Type/csmString.hpp"

namespace Live2D { namespace Cubism { namespace Framework {

class CubismIdManager;

/**
 * @brief パラメータ名・パーツ名・Drawable名を保持
 *
 * パラメータ名・パーツ名・Drawable名を保持するクラス。
 */
struct CubismId
{
    friend class CubismIdManager;
    const csmString& GetString() const;
    CubismId& operator=(const CubismId& c);
    csmBool operator==(const CubismId& c) const;
    csmBool operator!=(const CubismId& c) const;
private:
    CubismId();
    CubismId(const csmChar* id);
    ~CubismId();
    CubismId(const CubismId& c);
    csmString _id;      ///< ID名
};

typedef const CubismId* CubismIdHandle;

}}}

#endif // CUBISMID_H
