/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "CubismRenderer_OpenGLES2.hpp"
#include "opengl/creadopengl.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <QScopeGuard>
// #include "framework/Math/CubismMatrix44.hpp"
// #include "framework/Type/csmVector.hpp"
// #include "framework/Model/CubismModel.hpp"
// #include <float.h>

// #ifdef CSM_TARGET_WIN_GL
// #include <Windows.h>
// #endif



#define CSM_FRAGMENT_SHADER_FP_PRECISION_HIGH "highp"
#define CSM_FRAGMENT_SHADER_FP_PRECISION_MID "mediump"
#define CSM_FRAGMENT_SHADER_FP_PRECISION_LOW "lowp"

#define CSM_FRAGMENT_SHADER_FP_PRECISION CSM_FRAGMENT_SHADER_FP_PRECISION_HIGH

//------------ LIVE2D NAMESPACE ------------
namespace Live2D { namespace Cubism { namespace Framework { namespace Rendering {

namespace {
bool MaskDimensions(const CubismVector2& size, int& width, int& height)
{
    const double maximum = std::numeric_limits<int>::max();
    if (!std::isfinite(size.X) || !std::isfinite(size.Y) ||
        size.X < 1.0f || size.Y < 1.0f || size.X > maximum || size.Y > maximum) return false;
    width = static_cast<int>(size.X);
    height = static_cast<int>(size.Y);
    return true;
}

void DeleteClippingManager(CubismClippingManager_OpenGLES2* manager)
{
    CSM_DELETE_SELF(CubismClippingManager_OpenGLES2, manager);
}
}

/*********************************************************************************************************************
*                                      CubismClippingManager_OpenGLES2
********************************************************************************************************************/
bool CubismClippingManager_OpenGLES2::SetupClippingContext(CubismModel& model, CubismRenderer_OpenGLES2* renderer, const GLint lastViewport[4])
{
    if (!renderer || !setGlOwningContext(LAppDelegate::GetInstance()->getGlOwningContext())) return false;
    int maskWidth = 0, maskHeight = 0;
    if (!MaskDimensions(_clippingMaskBufferSize, maskWidth, maskHeight)) return false;

    csmInt32 usingClipCount = 0;
    for (csmUint32 clipIndex = 0; clipIndex < _clippingContextListForMask.GetSize(); clipIndex++)
    {
        // １つのクリッピングマスクに関して
        CubismClippingContext_OpenGLES2* cc = _clippingContextListForMask[clipIndex];

        // このクリップを利用する描画オブジェクト群全体を囲む矩形を計算
        CalcClippedDrawTotalBounds(model, cc);

        if (cc->_isUsing)
        {
            usingClipCount++; //使用中としてカウント
        }
    }

    if (usingClipCount <= 0)
    {
        return true;
    }
    glViewport(0, 0, maskWidth, maskHeight);
    const auto restore = qScopeGuard([&] {
        if (_currentMaskBuffer) _currentMaskBuffer->EndDraw();
        _currentMaskBuffer = nullptr;
        renderer->SetClippingContextBufferForMask(nullptr);
        glViewport(lastViewport[0], lastViewport[1], lastViewport[2], lastViewport[3]);
    });

    // 後の計算のためにインデックスの最初をセット
    _currentMaskBuffer = renderer->GetMaskBuffer(0);
    // ----- マスク描画処理 -----
    if (!_currentMaskBuffer || !_currentMaskBuffer->BeginDraw()) return false;

    renderer->PreDraw(); // バッファをクリアする

    // 各マスクのレイアウトを決定していく
    SetupLayoutBounds(usingClipCount);

    // サイズがレンダーテクスチャの枚数と合わない場合は合わせる
    if (_clearedMaskBufferFlags.GetSize() != _renderTextureCount)
    {
        _clearedMaskBufferFlags.Clear();

        for (csmInt32 i = 0; i < _renderTextureCount; ++i)
        {
            _clearedMaskBufferFlags.PushBack(false);
        }
    }
    else
    {
        // マスクのクリアフラグを毎フレーム開始時に初期化
        for (csmInt32 i = 0; i < _renderTextureCount; ++i)
        {
            _clearedMaskBufferFlags[i] = false;
        }
    }

    // 実際にマスクを生成する
    // 全てのマスクをどの様にレイアウトして描くかを決定し、ClipContext , ClippedDrawContext に記憶する
    for (csmUint32 clipIndex = 0; clipIndex < _clippingContextListForMask.GetSize(); clipIndex++)
    {
        // --- 実際に１つのマスクを描く ---
        CubismClippingContext_OpenGLES2* clipContext = _clippingContextListForMask[clipIndex];
        if (!clipContext->_isUsing) continue;
        csmRectF* allClippedDrawRect = clipContext->_allClippedDrawRect; //このマスクを使う、全ての描画オブジェクトの論理座標上の囲み矩形
        csmRectF* layoutBoundsOnTex01 = clipContext->_layoutBounds; //この中にマスクを収める
        const csmFloat32 MARGIN = 0.05f;

        // clipContextに設定したオフスクリーンサーフェイスをインデックスで取得
        CubismOffscreenSurface_OpenGLES2* clipContextOffscreenSurface = renderer->GetMaskBuffer(clipContext->_bufferIndex);

        // 現在のオフスクリーンサーフェイスがclipContextのものと異なる場合
        if (_currentMaskBuffer != clipContextOffscreenSurface)
        {
            if (!_currentMaskBuffer->EndDraw()) return false;
            _currentMaskBuffer = clipContextOffscreenSurface;
            // マスク用RenderTextureをactiveにセット
            if (!_currentMaskBuffer || !_currentMaskBuffer->BeginDraw()) return false;

            // バッファをクリアする。
            renderer->PreDraw();
        }

        // モデル座標上の矩形を、適宜マージンを付けて使う
        _tmpBoundsOnModel.SetRect(allClippedDrawRect);
        _tmpBoundsOnModel.Expand(allClippedDrawRect->Width * MARGIN, allClippedDrawRect->Height * MARGIN);
        // movePeriod' = movePeriod * scaleX + offX     [[ movePeriod' = (movePeriod - tmpBoundsOnModel.movePeriod)*scale + layoutBoundsOnTex01.movePeriod ]]
        csmFloat32 scaleX = layoutBoundsOnTex01->Width / _tmpBoundsOnModel.Width;
        csmFloat32 scaleY = layoutBoundsOnTex01->Height / _tmpBoundsOnModel.Height;
        createMatrixForMask(false, layoutBoundsOnTex01, scaleX, scaleY);
        clipContext->_matrixForMask.SetMatrix(_tmpMatrixForMask.GetArray());
        clipContext->_matrixForDraw.SetMatrix(_tmpMatrixForDraw.GetArray());
        const csmInt32 clipDrawCount = clipContext->_clippingIdCount;
        for (csmInt32 i = 0; i < clipDrawCount; i++)
        {
            const csmInt32 clipDrawIndex = clipContext->_clippingIdList[i];
            if (!renderer->_forceMaskRedraw && !model.GetDrawableDynamicFlagVertexPositionsDidChange(clipDrawIndex))
            {
                continue;
            }
            renderer->IsCulling(model.GetDrawableCulling(clipDrawIndex) != 0);
            if (!_clearedMaskBufferFlags[clipContext->_bufferIndex])
            {
                if (!_currentMaskBuffer->Clear(1.0f, 1.0f, 1.0f, 1.0f)) return false;
                _clearedMaskBufferFlags[clipContext->_bufferIndex] = true;
            }
            renderer->SetClippingContextBufferForMask(clipContext);
            if (!renderer->DrawMeshOpenGL(model, clipDrawIndex,LAppDelegate::GetInstance()->m_IsPrintALl)) return false;
        }
    }


    const bool ended = _currentMaskBuffer->EndDraw();
    _currentMaskBuffer = nullptr;
    return ended;
}

/*********************************************************************************************************************
*                                      CubismClippingContext_OpenGLES2
********************************************************************************************************************/
CubismClippingContext_OpenGLES2::CubismClippingContext_OpenGLES2(CubismClippingManager<CubismClippingContext_OpenGLES2, CubismOffscreenSurface_OpenGLES2>* manager, CubismModel& model, const csmInt32* clippingDrawableIndices, csmInt32 clipCount)
    : CubismClippingContext(clippingDrawableIndices, clipCount)
{
    _owner = manager;
}

CubismClippingContext_OpenGLES2::~CubismClippingContext_OpenGLES2()
{
}

CubismClippingManager<CubismClippingContext_OpenGLES2, CubismOffscreenSurface_OpenGLES2>* CubismClippingContext_OpenGLES2::GetClippingManager()
{
    return _owner;
}

/*********************************************************************************************************************
 *                                      CubismRenderer_OpenGLES2
 ********************************************************************************************************************/

#ifdef CSM_TARGET_ANDROID_ES2
void CubismRenderer_OpenGLES2::SetExtShaderMode(csmBool extMode, csmBool extPAMode)
{
    CubismShader_OpenGLES2::SetExtShaderMode(extMode, extPAMode);
    CubismShader_OpenGLES2::DeleteInstance();
}

void CubismRenderer_OpenGLES2::ReloadShader()
{
    CubismShader_OpenGLES2::DeleteInstance();
}
#endif



CubismRenderer* CubismRenderer::Create()
{
    return CSM_NEW CubismRenderer_OpenGLES2();
}

void CubismRenderer::StaticRelease()
{
    CubismRenderer_OpenGLES2::DoStaticRelease();
}

CubismRenderer_OpenGLES2::CubismRenderer_OpenGLES2() : _clippingManager(NULL)
                                                     , _clippingContextBufferForMask(NULL)
                                                     , _clippingContextBufferForDraw(NULL)
{
    // テクスチャ対応マップの容量を確保しておく.
    _textures.PrepareCapacity(32, true);
}

CubismRenderer_OpenGLES2::~CubismRenderer_OpenGLES2()
{
    CSM_DELETE_SELF(CubismClippingManager_OpenGLES2, _clippingManager);
}

void CubismRenderer_OpenGLES2::DoStaticRelease()
{

    CubismShader_OpenGLES2::DeleteInstance();
}

csmBool CubismRenderer_OpenGLES2::Initialize(CubismModel* model)
{
    return Initialize(model, 1);
}

csmBool CubismRenderer_OpenGLES2::Initialize(CubismModel* model, csmInt32 maskBufferCount)
{
    if (!model || maskBufferCount < 1 ||
        !setGlOwningContext(LAppDelegate::GetInstance()->getGlOwningContext()) ||
        !CubismShader_OpenGLES2::GetInstance()->Initialize(owningContext())) return false;
    std::unique_ptr<CubismClippingManager_OpenGLES2, decltype(&DeleteClippingManager)>
        clipping(nullptr, &DeleteClippingManager);
    std::vector<std::unique_ptr<CubismOffscreenSurface_OpenGLES2>> surfaces;
    if (model->IsUsingMasking())
    {
        clipping.reset(CSM_NEW CubismClippingManager_OpenGLES2());
        if (!clipping) return false;
        clipping->Initialize(*model, maskBufferCount);
        int maskWidth = 0, maskHeight = 0;
        if (!MaskDimensions(clipping->GetClippingMaskBufferSize(), maskWidth, maskHeight)) return false;
        surfaces.reserve(maskBufferCount);
        for (csmInt32 i = 0; i < maskBufferCount; ++i)
        {
            auto surface = std::make_unique<CubismOffscreenSurface_OpenGLES2>();
            if (!surface->setGlOwningContext(owningContext()) ||
                !surface->CreateOffscreenSurface(maskWidth, maskHeight)) return false;
            surfaces.push_back(std::move(surface));
        }
    }
    if (!CubismRenderer::Initialize(model, maskBufferCount)) return false;
    CSM_DELETE_SELF(CubismClippingManager_OpenGLES2, _clippingManager);
    _clippingManager = clipping.release();
    _offscreenSurfaces.swap(surfaces);
    _meshBuffers.clear();
    _meshBuffers.resize(static_cast<size_t>(model->GetDrawableCount()));
    _sortedDrawableIndexList.Resize(model->GetDrawableCount(), 0);
    _clippingContextBufferForMask = nullptr;
    _clippingContextBufferForDraw = nullptr;
    _maskIndicesRevision = 0;
    _forceMaskRedraw = false;
    _maskTopologyUpdated = false;
    _maskBuffersNeedRedraw = _clippingManager != nullptr;
    return true;
}

void CubismRenderer_OpenGLES2::PreDraw()
{
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);

    glEnable(GL_BLEND);
    glColorMask(1, 1, 1, 1);

    //異方性フィルタリング。プラットフォームのOpenGLによっては未対応の場合があるので、未設定のときは設定しない
    if (GetAnisotropy() > 0.0f)
    {
        for (csmInt32 i = 0; i < _textures.GetSize(); i++)
        {
            glBindTexture(GL_TEXTURE_2D, _textures[i]);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, GetAnisotropy());
        }
    }
}


bool CubismRenderer_OpenGLES2::DrawHighPrecisionMask(CubismClippingContext_OpenGLES2* clipContext)
{
    if (!clipContext || !clipContext->_isUsing || !IsUsingHighPrecisionMask()) return true;
    auto* surface = GetMaskBuffer(clipContext->_bufferIndex);
    if (!surface || !surface->IsValid() || !surface->BeginDraw()) return false;
    bool drawing = true;
    const auto restore = qScopeGuard([&] {
        if (drawing) surface->EndDraw();
        SetClippingContextBufferForMask(nullptr);
        glViewport(_rendererProfile.Viewport()[0], _rendererProfile.Viewport()[1],
                   _rendererProfile.Viewport()[2], _rendererProfile.Viewport()[3]);
    });
    glViewport(0, 0, surface->GetBufferWidth(), surface->GetBufferHeight());
    PreDraw();
    if (!surface->Clear(1.0f, 1.0f, 1.0f, 1.0f)) return false;
    for (csmInt32 index = 0; index < clipContext->_clippingIdCount; ++index) {
        const csmInt32 clipDrawIndex = clipContext->_clippingIdList[index];
        if (!_forceMaskRedraw && !GetModel()->GetDrawableDynamicFlagVertexPositionsDidChange(clipDrawIndex)) continue;
        IsCulling(GetModel()->GetDrawableCulling(clipDrawIndex) != 0);
        SetClippingContextBufferForMask(clipContext);
        if (!DrawMeshOpenGL(*GetModel(), clipDrawIndex, LAppDelegate::GetInstance()->m_IsPrintALl)) return false;
    }
    if (!surface->EndDraw()) return false;
    drawing = false;
    PreDraw();
    return true;
}

bool CubismRenderer_OpenGLES2::WritePsdLayer(csmInt32 drawableIndex, const QString& drawName,
                                            csmFloat32 opacityOverride, bool hasMask)
{
    auto* delegate = LAppDelegate::GetInstance();
    auto* writer = delegate->GetCPsdWriteList();
    if (!writer) return false;
    QImage image;
    QRect rect;
    if (!delegate->read.GetScreenNode(GetModel()->GetDrawableVertices(drawableIndex),
            GetModel()->GetDrawableVertexCount(drawableIndex), GetMvpMatrix().GetArray(),
            _rendererProfile.Viewport()[2], _rendererProfile.Viewport()[3], image, rect)) return false;
    if (!rect.isEmpty()) {
        if (image.isNull()) return false;
        const csmFloat32 effectiveOpacity = opacityOverride >= 0.0f ? opacityOverride :
            GetModel()->GetDrawableOpacityAll(drawableIndex, delegate->m_IsPrintALl);
        const uint8_t opacity = static_cast<uint8_t>(qBound(0.0f, effectiveOpacity * 255.0f, 255.0f));
        writer->AddLayer(drawName, rect, image, opacity,
                        CPsdWriteList::CubismBlendModeToPsd(GetModel()->GetDrawableBlendMode(drawableIndex)),
                        hasMask, false);
    }
    return delegate->read.UpdateFrame();
}

csmBool CubismRenderer_OpenGLES2::DoDrawModel()
{
    if (!GetModel() || !setGlOwningContext(owningContext())) return false;
    bool complete = false;
    const auto clearClipping = qScopeGuard([&] {
        SetClippingContextBufferForMask(nullptr);
        SetClippingContextBufferForDraw(nullptr);
        if (!complete && _clippingManager) _maskBuffersNeedRedraw = true;
    });
    const quint64 indicesRevision = GetModel()->GetDrawableIndicesRevision();
    _forceMaskRedraw = _maskBuffersNeedRedraw || GetModel()->HasDrawableIndicesOverrides() ||
                       indicesRevision != _maskIndicesRevision;
    _maskTopologyUpdated = false;

    if (_clippingManager) {
        int maskWidth = 0, maskHeight = 0;
        if (!MaskDimensions(_clippingManager->GetClippingMaskBufferSize(), maskWidth, maskHeight) ||
            _clippingManager->GetRenderTextureCount() < 1 ||
            _offscreenSurfaces.size() != static_cast<size_t>(_clippingManager->GetRenderTextureCount())) return false;
        PreDraw();
        for (auto& surface : _offscreenSurfaces) {
            if (!surface || !surface->IsValid() || surface->GetBufferWidth() != maskWidth ||
                surface->GetBufferHeight() != maskHeight) {
                auto replacement = std::make_unique<CubismOffscreenSurface_OpenGLES2>();
                if (!replacement->setGlOwningContext(owningContext()) ||
                    !replacement->CreateOffscreenSurface(maskWidth, maskHeight)) return false;
                surface = std::move(replacement);
                _forceMaskRedraw = true;
                _maskBuffersNeedRedraw = true;
            }
        }
        if (IsUsingHighPrecisionMask()) {
            _clippingManager->SetupMatrixForHighPrecision(*GetModel(), false);
        } else if (!_clippingManager->SetupClippingContext(*GetModel(), this, _rendererProfile.Viewport().data())) {
            return false;
        }
    }

    auto* delegate = LAppDelegate::GetInstance();
    const bool offscreen = delegate->IsOffScreen();
    if (offscreen && (!delegate->GetCPsdWriteList() || !delegate->read.PreDraw())) return false;
    PreDraw();
    const csmInt32 drawableCount = GetModel()->GetDrawableCount();
    const QVector<int> drawableOrder = GetModel()->GetSortedDrawableIndices(
        delegate->m_drawableDrawOrderOverrides, delegate->m_drawableOrderGroupIndices, delegate->m_drawOrderGroups);
    _sortedDrawableIndexList.Resize(drawableCount);
    for (csmInt32 i = 0; i < drawableCount; ++i) _sortedDrawableIndexList[i] = drawableOrder[i];
    QStringList names;
    for (int index : drawableOrder) {
        const auto id = GetModel()->GetDrawableId(index);
        if (id) names.append(QString::fromUtf8(id->GetString().GetRawString()));
    }
    GetModel()->GetMeshMap()->SetRenderOrder(names);
    if (const QWidget* window = delegate->GetWindow()) {
        QMetaObject::invokeMethod(const_cast<QWidget*>(window), "PublishRenderOrder",
                                  Qt::DirectConnection, Q_ARG(QStringList, names));
    }

    const auto drawDrawable = [&](csmInt32 drawableIndex, const QString& drawName) {
        CubismClippingContext_OpenGLES2* clipContext = _clippingManager
            ? (*_clippingManager->GetClippingContextListForDraw())[drawableIndex] : nullptr;
        if (clipContext) {
            auto* surface = GetMaskBuffer(clipContext->_bufferIndex);
            if (!surface || !surface->IsValid()) return false;
        }
        if (!DrawHighPrecisionMask(clipContext)) return false;
        SetClippingContextBufferForDraw(clipContext);
        IsCulling(GetModel()->GetDrawableCulling(drawableIndex) != 0);
        const csmFloat32 opacityOverride = delegate->m_drawableOpacityOverrides.value(drawName, -1.0f);
        if (!DrawMeshOpenGL(*GetModel(), drawableIndex, delegate->m_IsPrintALl, opacityOverride)) return false;
        return !offscreen || WritePsdLayer(drawableIndex, drawName, opacityOverride, clipContext != nullptr);
    };

    if (offscreen && delegate->m_IsPrintFenzu) {
        QVector<int> sorted;
        for (csmInt32 i = 0; i < drawableCount; ++i) sorted.push_back(_sortedDrawableIndexList[i]);
        auto* writer = delegate->GetCPsdWriteList();
        const auto order = writer->GetSortDrawableIndexList(sorted, GetModel());
        const auto& groupNames = writer->GetArtToName();
        for (int i = 0; i < order.size(); ++i) {
            if (order[i].second == CPsdWriteList::GroupStart) {
                writer->AddGroupStart(groupNames[i]);
                continue;
            }
            if (order[i].second == CPsdWriteList::GroupEnd) {
                writer->AddGroupEnd();
                continue;
            }
            const csmInt32 index = order[i].first;
            if (!delegate->m_IsPrintALl && !GetModel()->GetDrawableDynamicFlagIsVisible(index)) continue;
            const QString name(GetModel()->GetDrawableId(index)->GetString().GetRawString());
            if (!drawDrawable(index, name)) return false;
        }
    } else {
        for (csmInt32 i = 0; i < drawableCount; ++i) {
            const csmInt32 index = _sortedDrawableIndexList[i];
            const QString name(GetModel()->GetDrawableId(index)->GetString().GetRawString());
            if (delegate->m_hiddenDrawables.contains(name) || delegate->m_hiddenDeformerDrawables.contains(name)) continue;
            if (!delegate->m_IsPrintALl && !GetModel()->GetDrawableDynamicFlagIsVisible(index)) continue;
            if (!drawDrawable(index, name)) return false;
        }
        if (!offscreen) {
            for (csmInt32 i = 0; i < drawableCount; ++i) {
                const QString name(GetModel()->GetDrawableId(i)->GetString().GetRawString());
                if (delegate->m_hiddenDrawables.contains(name) || delegate->m_hiddenDeformerDrawables.contains(name)) continue;
                GetModel()->DrawMesh(name, GetMvpMatrix().GetQMatrix44());
            }
        }
    }
    if (offscreen) delegate->SetIsOffset(false);
    PostDraw();
    if (_maskTopologyUpdated) {
        _maskIndicesRevision = indicesRevision;
        _maskBuffersNeedRedraw = false;
    }
    complete = true;
    return true;
}

bool CubismRenderer_OpenGLES2::DrawMeshOpenGL(const CubismModel& model, const csmInt32 index,
                                             bool IsAll, csmFloat32 opacityOverride)
{
    const auto clearClipping = qScopeGuard([this] {
        SetClippingContextBufferForDraw(nullptr);
        SetClippingContextBufferForMask(nullptr);
    });
    if (index < 0 || index >= model.GetDrawableCount() ||
        static_cast<size_t>(index) >= _meshBuffers.size()) return false;
    if (!GetBindedTextureId(model.GetDrawableTextureIndex(index))) return true;
    const int vertexCount = model.GetDrawableVertexCount(index);
    const int indexCount = model.GetDrawableVertexIndexCount(index);
    if (vertexCount < 0 || vertexCount > CGlMeshBuffer::MaxVertices || indexCount < 0 ||
        indexCount > CGlMeshBuffer::MaxIndices || indexCount % 3 != 0) return false;
    if (!vertexCount || !indexCount) return true;
    const float* positions = model.GetDrawableVertices(index);
    auto* delegate = LAppDelegate::GetInstance();
    if (delegate->m_isDraggingDeformer && !delegate->IsOffScreen()) {
        const auto saved = delegate->m_savedVertices.constFind(index);
        if (saved != delegate->m_savedVertices.cend()) {
            if (saved.value().size() != static_cast<size_t>(vertexCount) * 2) return false;
            positions = saved.value().data();
        }
    }
    auto& mesh = _meshBuffers[static_cast<size_t>(index)];
    if (!mesh) mesh = std::make_unique<CGlMeshBuffer>();
    if (!mesh->SetContext(owningContext()) || !mesh->Upload(positions, vertexCount,
            reinterpret_cast<const float*>(model.GetDrawableVertexUvs(index)),
            model.GetDrawableVertexIndices(index), indexCount)) return false;
    if (IsCulling()) glEnable(GL_CULL_FACE);
    else glDisable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    auto* shader = CubismShader_OpenGLES2::GetInstance();
    const bool generatingMask = IsGeneratingMask();
    if (generatingMask) {
        if (!shader->SetupShaderProgramForMask(this, model, index)) return false;
    } else if (!shader->SetupShaderProgramForDraw(this, model, index, IsAll, opacityOverride)) {
        return false;
    }
    if (!mesh->Draw(GL_TRIANGLES)) return false;
    if (generatingMask) _maskTopologyUpdated = true;
    return true;
}

csmBool CubismRenderer_OpenGLES2::SaveProfile()
{
    return _rendererProfile.Save(LAppDelegate::GetInstance()->getGlOwningContext());
}

csmBool CubismRenderer_OpenGLES2::RestoreProfile()
{
    return _rendererProfile.Restore();
}

void CubismRenderer_OpenGLES2::BindTexture(csmUint32 modelTextureIndex, GLuint glTextureIndex)
{
    _textures[modelTextureIndex] = glTextureIndex;
}

const csmMap<csmInt32, GLuint>& CubismRenderer_OpenGLES2::GetBindedTextures() const
{
    return _textures;
}

void CubismRenderer_OpenGLES2::SetClippingMaskBufferSize(csmFloat32 width, csmFloat32 height)
{
    int maskWidth = 0, maskHeight = 0;
    if (!_clippingManager || !MaskDimensions(CubismVector2(width, height), maskWidth, maskHeight)) return;
    const csmInt32 renderTextureCount = _clippingManager->GetRenderTextureCount();
    std::unique_ptr<CubismClippingManager_OpenGLES2, decltype(&DeleteClippingManager)>
        replacement(CSM_NEW CubismClippingManager_OpenGLES2(), &DeleteClippingManager);
    if (!replacement) return;
    replacement->SetClippingMaskBufferSize(width, height);
    replacement->Initialize(*GetModel(), renderTextureCount);
    CSM_DELETE_SELF(CubismClippingManager_OpenGLES2, _clippingManager);
    _clippingManager = replacement.release();
    SetClippingContextBufferForMask(nullptr);
    SetClippingContextBufferForDraw(nullptr);
}

csmInt32 CubismRenderer_OpenGLES2::GetRenderTextureCount() const
{
    return _clippingManager ? _clippingManager->GetRenderTextureCount() : 0;
}

CubismVector2 CubismRenderer_OpenGLES2::GetClippingMaskBufferSize() const
{
    return _clippingManager ? _clippingManager->GetClippingMaskBufferSize() : CubismVector2();
}

CubismOffscreenSurface_OpenGLES2* CubismRenderer_OpenGLES2::GetMaskBuffer(csmInt32 index)
{
    return index >= 0 && static_cast<size_t>(index) < _offscreenSurfaces.size()
        ? _offscreenSurfaces[index].get() : nullptr;
}

void CubismRenderer_OpenGLES2::SetClippingContextBufferForMask(CubismClippingContext_OpenGLES2* clip)
{
    _clippingContextBufferForMask = clip;
}

CubismClippingContext_OpenGLES2* CubismRenderer_OpenGLES2::GetClippingContextBufferForMask() const
{
    return _clippingContextBufferForMask;
}

void CubismRenderer_OpenGLES2::SetClippingContextBufferForDraw(CubismClippingContext_OpenGLES2* clip)
{
    _clippingContextBufferForDraw = clip;
}

CubismClippingContext_OpenGLES2* CubismRenderer_OpenGLES2::GetClippingContextBufferForDraw() const
{
    return _clippingContextBufferForDraw;
}

const csmBool inline CubismRenderer_OpenGLES2::IsGeneratingMask() const
{
    return (GetClippingContextBufferForMask() != NULL);
}

GLuint CubismRenderer_OpenGLES2::GetBindedTextureId(csmInt32 textureId)
{
    return (_textures[textureId] != 0) ? _textures[textureId] : -1;
}

}}}}

//------------ LIVE2D NAMESPACE ------------
