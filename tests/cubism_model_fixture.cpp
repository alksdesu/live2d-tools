#include "cubism_model_fixture.h"
#include <new>

namespace CubismModelFixture {
Core::csmModel* AttachModel(void* address, GetterState* state)
{
    if (!address || !state) return nullptr;
    return reinterpret_cast<Core::csmModel*>(new (address) ModelHandle{state});
}

GetterState& State(const Core::csmModel* model)
{
    const auto* handle = reinterpret_cast<const ModelHandle*>(model);
    if (!handle || !handle->state) qFatal("Core getter double received an uninitialized model handle");
    return *handle->state;
}
}

namespace Live2D { namespace Cubism { namespace Core {
#define CORE_ARRAY_GETTER(type, name, field) \
    type csmCallingConvention name(const csmModel* model) \
    { auto& data = CubismModelFixture::State(model); return data.nullGetters.contains(QStringLiteral(#name)) ? nullptr : data.field.data(); }
CORE_ARRAY_GETTER(const char**, csmGetParameterIds, parameterIds)
CORE_ARRAY_GETTER(const char**, csmGetPartIds, partIds)
CORE_ARRAY_GETTER(const char**, csmGetDrawableIds, drawableIds)
CORE_ARRAY_GETTER(const csmParameterType*, csmGetParameterTypes, types)
CORE_ARRAY_GETTER(const float*, csmGetParameterMinimumValues, minimum)
CORE_ARRAY_GETTER(const float*, csmGetParameterMaximumValues, maximum)
CORE_ARRAY_GETTER(const float*, csmGetParameterDefaultValues, defaults)
CORE_ARRAY_GETTER(const int*, csmGetParameterKeyCounts, keyCounts)
CORE_ARRAY_GETTER(const float**, csmGetParameterKeyValues, keyPointers)
CORE_ARRAY_GETTER(const int*, csmGetPartParentPartIndices, partParents)
CORE_ARRAY_GETTER(const int*, csmGetDrawableParentPartIndices, drawableParents)
CORE_ARRAY_GETTER(const csmFlags*, csmGetDrawableConstantFlags, flags)
CORE_ARRAY_GETTER(const csmFlags*, csmGetDrawableDynamicFlags, dynamicFlags)
CORE_ARRAY_GETTER(const int*, csmGetDrawableTextureIndices, textures)
CORE_ARRAY_GETTER(const int*, csmGetDrawableDrawOrders, drawOrders)
CORE_ARRAY_GETTER(const int*, csmGetDrawableRenderOrders, renderOrders)
#if LIVE2D_CORE_API_VERSION >= 6
CORE_ARRAY_GETTER(const int*, csmGetDrawableBlendModes, blendModes)
CORE_ARRAY_GETTER(const int*, csmGetPartOffscreenIndices, partOffscreens)
CORE_ARRAY_GETTER(const int*, csmGetOffscreenOwnerIndices, offscreenOwners)
CORE_ARRAY_GETTER(const int*, csmGetOffscreenBlendModes, offscreenBlendModes)
CORE_ARRAY_GETTER(const float*, csmGetOffscreenOpacities, offscreenOpacities)
CORE_ARRAY_GETTER(const csmFlags*, csmGetOffscreenConstantFlags, offscreenFlags)
CORE_ARRAY_GETTER(const int*, csmGetOffscreenMaskCounts, offscreenMaskCounts)
CORE_ARRAY_GETTER(const int**, csmGetOffscreenMasks, offscreenMaskPointers)
CORE_ARRAY_GETTER(const csmVector4*, csmGetOffscreenMultiplyColors, offscreenMultiply)
CORE_ARRAY_GETTER(const csmVector4*, csmGetOffscreenScreenColors, offscreenScreen)
#endif
CORE_ARRAY_GETTER(const float*, csmGetDrawableOpacities, drawableOpacities)
CORE_ARRAY_GETTER(const int*, csmGetDrawableMaskCounts, maskCounts)
CORE_ARRAY_GETTER(const int**, csmGetDrawableMasks, maskPointers)
CORE_ARRAY_GETTER(const int*, csmGetDrawableVertexCounts, vertexCounts)
CORE_ARRAY_GETTER(const int*, csmGetDrawableIndexCounts, indexCounts)
CORE_ARRAY_GETTER(const unsigned short**, csmGetDrawableIndices, indexPointers)
CORE_ARRAY_GETTER(const csmVector2**, csmGetDrawableVertexPositions, positionPointers)
CORE_ARRAY_GETTER(const csmVector2**, csmGetDrawableVertexUvs, uvPointers)
CORE_ARRAY_GETTER(const csmVector4*, csmGetDrawableMultiplyColors, multiply)
CORE_ARRAY_GETTER(const csmVector4*, csmGetDrawableScreenColors, screen)
#undef CORE_ARRAY_GETTER
int csmCallingConvention csmGetParameterCount(const csmModel* model) { return CubismModelFixture::State(model).parameterCount; }
int csmCallingConvention csmGetPartCount(const csmModel* model) { return CubismModelFixture::State(model).partCount; }
int csmCallingConvention csmGetDrawableCount(const csmModel* model) { return CubismModelFixture::State(model).drawableCount; }
#if LIVE2D_CORE_API_VERSION >= 6
int csmCallingConvention csmGetOffscreenCount(const csmModel* model) { return CubismModelFixture::State(model).offscreenCount; }
#endif
float* csmCallingConvention csmGetParameterValues(csmModel* model) { return CubismModelFixture::State(model).values.data(); }
float* csmCallingConvention csmGetPartOpacities(csmModel* model) { return CubismModelFixture::State(model).partOpacities.data(); }
void csmCallingConvention csmReadCanvasInfo(const csmModel* model, csmVector2* size, csmVector2* origin, float* ppu)
{
    const auto& data = CubismModelFixture::State(model);
    *size = data.size;
    *origin = data.origin;
    *ppu = data.ppu;
}
void csmCallingConvention csmUpdateModel(csmModel* model) { ++CubismModelFixture::State(model).updates; }
void csmCallingConvention csmResetDrawableDynamicFlags(csmModel* model) { CubismModelFixture::State(model).dynamicFlags.fill(0); }
}}}
