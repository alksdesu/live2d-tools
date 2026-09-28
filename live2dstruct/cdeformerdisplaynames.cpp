#include "cdeformerdisplaynames.h"
#include "cmodeldisplaynames.h"

namespace live2dstruct {
bool CDeformerDisplayNames::ValidateName(const QString& name, QString* error)
{
    return CModelDisplayNames::ValidateName(name, error);
}
bool CDeformerDisplayNames::Validate(const State& names, QString* error)
{
    return CModelDisplayNames::ValidateMap(names, error);
}
bool CDeformerDisplayNames::Normalize(const State& names, State& result, QString* error)
{
    return CModelDisplayNames::NormalizeMap(names, result, error);
}
bool CDeformerDisplayNames::ToJson(const State& names, QJsonObject& result, QString* error)
{
    return CModelDisplayNames::MapToJson(names, result, error);
}
bool CDeformerDisplayNames::FromJson(const QJsonObject& object, State& result, QString* error)
{
    return CModelDisplayNames::MapFromJson(object, result, error);
}
}
