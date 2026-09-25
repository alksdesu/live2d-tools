/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "CubismMoc.hpp"
#include "CubismModel.hpp"
#include <memory>

namespace Live2D { namespace Cubism { namespace Framework {

CubismMoc* CubismMoc::Create(const csmByte* mocBytes, csmSizeInt size, csmBool shouldCheckMocConsistency)
{
    if (!mocBytes || size == 0) return NULL;
    const auto freeBuffer = [](void* memory) { CSM_FREE_ALLIGNED(memory); };
    std::unique_ptr<void, decltype(freeBuffer)> alignedBuffer(
        CSM_MALLOC_ALLIGNED(size, Core::csmAlignofMoc), freeBuffer);
    if (!alignedBuffer) return NULL;
    memcpy(alignedBuffer.get(), mocBytes, size);

    if (shouldCheckMocConsistency)
    {
        // .moc3の整合性を確認
        csmBool consistency = HasMocConsistency(alignedBuffer.get(), size);
        if (!consistency)
        {
            // 整合性が確認できなければ処理しない
            CubismLogError("Inconsistent MOC3.");
            return NULL;
        }
    }

    Core::csmMoc* moc = Core::csmReviveMocInPlace(alignedBuffer.get(), size);
    if (!moc) return NULL;
    CubismMoc* cubismMoc = CSM_NEW CubismMoc(moc);
    if (!cubismMoc) return NULL;
    cubismMoc->_mocVersion = Core::csmGetMocVersion(alignedBuffer.get(), size);
    alignedBuffer.release();
    return cubismMoc;
}

void CubismMoc::Delete(CubismMoc* moc)
{
    CSM_DELETE_SELF(CubismMoc, moc);
}

CubismMoc::CubismMoc(Core::csmMoc* moc)
                        : _moc(moc)
                        , _modelCount(0)
                        , _mocVersion(0)
{ }

CubismMoc::~CubismMoc()
{
    CSM_ASSERT(_modelCount == 0);

    CSM_FREE_ALLIGNED(_moc);
}

CubismModel* CubismMoc::CreateModel()
{
    const csmUint32  modelSize = Core::csmGetSizeofModel(_moc);
    if (modelSize == 0) return NULL;
    const auto freeBuffer = [](void* memory) { CSM_FREE_ALLIGNED(memory); };
    std::unique_ptr<void, decltype(freeBuffer)> modelMemory(
        CSM_MALLOC_ALLIGNED(modelSize, Core::csmAlignofModel), freeBuffer);
    if (!modelMemory) return NULL;
    Core::csmModel* model = Core::csmInitializeModelInPlace(_moc, modelMemory.get(), modelSize);
    if (!model) return NULL;
    const auto deleteModel = [](CubismModel* value) { CSM_DELETE_SELF(CubismModel, value); };
    std::unique_ptr<CubismModel, decltype(deleteModel)> cubismModel(CSM_NEW CubismModel(model), deleteModel);
    if (!cubismModel) return NULL;
    modelMemory.release();
    cubismModel->Initialize();
    ++_modelCount;
    return cubismModel.release();
}

void CubismMoc::DeleteModel(CubismModel* model)
{
    if (!model) return;
    CSM_DELETE_SELF(CubismModel, model);
    --_modelCount;
}

Core::csmMocVersion CubismMoc::GetLatestMocVersion()
{
    return Core::csmGetLatestMocVersion();
}

Core::csmMocVersion CubismMoc::GetMocVersion()
{
    return _mocVersion;
}

csmBool CubismMoc::HasMocConsistency(void* address, const csmUint32 size)
{
    if (!address || size == 0) return false;
    csmInt32 isConsistent = Core::csmHasMocConsistency(address, size);
    return isConsistent != 0 ? true : false;
}

csmBool CubismMoc::HasMocConsistencyFromUnrevivedMoc(const csmByte* mocBytes, csmSizeInt size)
{
    if (!mocBytes || size == 0) return false;
    const auto freeBuffer = [](void* memory) { CSM_FREE_ALLIGNED(memory); };
    std::unique_ptr<void, decltype(freeBuffer)> alignedBuffer(
        CSM_MALLOC_ALLIGNED(size, Core::csmAlignofMoc), freeBuffer);
    if (!alignedBuffer) return false;
    memcpy(alignedBuffer.get(), mocBytes, size);
    return CubismMoc::HasMocConsistency(alignedBuffer.get(), size);
}

}}}
