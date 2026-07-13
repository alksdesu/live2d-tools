#ifndef CCMO3IDS_H
#define CCMO3IDS_H

#include <QString>
#include <QUuid>

namespace cmo3 {

// Cubism Editor 内建 well-known UUID：引用它们即挂到编辑器隐式根对象上
namespace WellKnown {
// 变形器树根（ArtMesh/Deformer 无父变形器时 targetDeformerGuid 指向它）
inline const QString kDeformerRoot = QStringLiteral("71fae776-e218-4aee-873e-78e8ac0cb48a");
// 参数分组树根
inline const QString kParamGroupRoot = QStringLiteral("e9fe6eff-953b-4ce2-be7c-4a7c3913686b");
} // namespace WellKnown

// 小写连字符格式（无花括号）的随机 GUID，匹配样本 uuid 属性风格
inline QString NewGuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

} // namespace cmo3

#endif // CCMO3IDS_H
