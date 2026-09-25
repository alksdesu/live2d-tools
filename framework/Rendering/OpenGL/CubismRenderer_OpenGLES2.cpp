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
#include <stdexcept>
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

bool PreparationFailed(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}
}

struct CubismRenderer_OpenGLES2::PreparedMaskResources
{
    std::unique_ptr<CubismClippingManager_OpenGLES2, decltype(&DeleteClippingManager)>
        clipping{nullptr, &DeleteClippingManager};
    std::vector<std::unique_ptr<CubismOffscreenSurface_OpenGLES2>> surfaces;
};

/*********************************************************************************************************************
*                                      CubismClippingManager_OpenGLES2
********************************************************************************************************************/
bool CubismClippingManager_OpenGLES2::SetupClippingContext(CubismModel& model, CubismRenderer_OpenGLES2* renderer, const GLint lastViewport[4])
{
    if (!renderer) return false;
    const auto* context = owningContext();
    if (!context) context = LAppDelegate::GetInstance()->getGlOwningContext();
    if (!setGlOwningContext(context)) return false;
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

        if (renderer->_forceMaskRedraw && !_clearedMaskBufferFlags[clipContext->_bufferIndex]) {
            if (!_currentMaskBuffer->Clear(1.0f, 1.0f, 1.0f, 1.0f)) return false;
            _clearedMaskBufferFlags[clipContext->_bufferIndex] = true;
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
            if (!model.GetDrawableDynamicFlagVertexPositionsDidChange(clipDrawIndex))
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
    const auto* context = owningContext();
    if (!context) context = LAppDelegate::GetInstance()->getGlOwningContext();
    if (!setGlOwningContext(context)) return false;
    return InitializeResources(model, maskBufferCount, CubismVector2(256, 256), false, nullptr);
}

bool CubismRenderer_OpenGLES2::PrepareMaskResources(CubismModel& model, csmInt32 count,
                                                   const CubismVector2& size,
                                                   PreparedMaskResources& resources, QString* error)
{
    const int drawables = model.GetDrawableCount();
    if (count < 1 || count > CGlMeshBuffer::MaxVertices || drawables < 0 || drawables > CGlMeshBuffer::MaxVertices)
        return PreparationFailed(error, QStringLiteral("渲染器网格或蒙版缓冲数量无效"));
    if (!model.HasValidClippingDefinitions(error)) return false;
    const int* maskCounts = model.GetDrawableMaskCounts();
    bool masking = false;
    for (int index = 0; index < drawables; ++index) {
        masking = masking || maskCounts[index] != 0;
    }
    if (!masking) return true;
    int width = 0, height = 0;
    if (!MaskDimensions(size, width, height))
        return PreparationFailed(error, QStringLiteral("蒙版缓冲尺寸无效"));
    resources.clipping.reset(CSM_NEW CubismClippingManager_OpenGLES2());
    if (!resources.clipping || !resources.clipping->setGlOwningContext(owningContext()))
        return PreparationFailed(error, QStringLiteral("无法创建蒙版管理器"));
    resources.clipping->SetClippingMaskBufferSize(size.X, size.Y);
    resources.clipping->Initialize(model, count);
    resources.surfaces.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        auto surface = std::make_unique<CubismOffscreenSurface_OpenGLES2>();
        if (!surface->setGlOwningContext(owningContext()) || !surface->CreateOffscreenSurface(width, height))
            return PreparationFailed(error, QStringLiteral("无法分配蒙版缓冲 %1").arg(index));
        resources.surfaces.push_back(std::move(surface));
    }
    return glGetError() == GL_NO_ERROR || PreparationFailed(error, QStringLiteral("蒙版 GPU 资源准备失败"));
}

bool CubismRenderer_OpenGLES2::UploadDrawableGeometry(const CubismModel& model, csmInt32 index,
                                                     const float* positions,
                                                     std::unique_ptr<CGlMeshBuffer>& mesh, QString* error)
{
    const int vertices = model.GetDrawableVertexCount(index);
    const int count = model.GetDrawableVertexIndexCount(index);
    if (vertices < 0 || vertices > CGlMeshBuffer::MaxVertices || count < 0 ||
        count > CGlMeshBuffer::MaxIndices || count % 3 != 0 || (!vertices && count))
        return PreparationFailed(error, QStringLiteral("网格 %1 的几何数量无效").arg(index));
    if (!vertices) return true;
    const auto* uvs = reinterpret_cast<const float*>(model.GetDrawableVertexUvs(index));
    if (!positions || !uvs)
        return PreparationFailed(error, QStringLiteral("网格 %1 缺少顶点或 UV 数组").arg(index));
    if (!count) {
        for (int value = 0; value < vertices * 2; ++value)
            if (!std::isfinite(positions[value]) || !std::isfinite(uvs[value]))
                return PreparationFailed(error, QStringLiteral("网格 %1 包含非有限坐标").arg(index));
        return true;
    }
    if (!mesh) mesh = std::make_unique<CGlMeshBuffer>();
    if (!mesh->SetContext(owningContext()) ||
        !mesh->Upload(positions, vertices, uvs, model.GetDrawableVertexIndices(index), count))
        return PreparationFailed(error, QStringLiteral("网格 %1 的几何数据或 GPU 上传无效").arg(index));
    return true;
}

bool CubismRenderer_OpenGLES2::InitializeResources(CubismModel* model, csmInt32 maskBufferCount,
                                                 const CubismVector2& maskSize, bool preuploadGeometry,
                                                 QString* error)
{
    if (!model || GetModel() || maskBufferCount < 1 || !setGlOwningContext(owningContext()))
        return PreparationFailed(error, QStringLiteral("候选渲染器需要新实例、有效模型和当前 OpenGL 上下文"));
    try {
        CGlRenderState caller;
        if (!caller.Save(owningContext())) return PreparationFailed(error, QStringLiteral("无法保存 OpenGL 状态"));
        const auto restore = qScopeGuard([&] { caller.Restore(); });
        if (!CubismShader_OpenGLES2::GetInstance()->Initialize(owningContext()))
            return PreparationFailed(error, QStringLiteral("无法准备模型着色器"));
        PreparedMaskResources masks;
        if (!PrepareMaskResources(*model, maskBufferCount, maskSize, masks, error)) return false;
        const int count = model->GetDrawableCount();
        std::vector<std::unique_ptr<CGlMeshBuffer>> meshes(static_cast<size_t>(count));
        if (preuploadGeometry) {
            if (count && (!Core::csmGetDrawableVertexCounts(model->GetModel()) ||
                !Core::csmGetDrawableIndexCounts(model->GetModel()) ||
                !Core::csmGetDrawableVertexPositions(model->GetModel()) ||
                !Core::csmGetDrawableVertexUvs(model->GetModel()) || !Core::csmGetDrawableIndices(model->GetModel())))
                return PreparationFailed(error, QStringLiteral("渲染器缺少几何数组"));
            for (int index = 0; index < count; ++index)
                if (!UploadDrawableGeometry(*model, index, model->GetDrawableVertices(index), meshes[index], error)) return false;
        }
        _sortedDrawableIndexList.Resize(count, 0);
        if (!CubismRenderer::Initialize(model, maskBufferCount)) return false;
        _clippingManager = masks.clipping.release();
        _offscreenSurfaces.swap(masks.surfaces);
        _meshBuffers.swap(meshes);
        _clippingContextBufferForMask = nullptr;
        _clippingContextBufferForDraw = nullptr;
        _maskIndicesRevision = 0;
        _forceMaskRedraw = false;
        _maskTopologyUpdated = false;
        _maskBuffersNeedRedraw = _clippingManager != nullptr;
        _requestedMaskBufferCount = maskBufferCount;
        _requestedMaskBufferSize = maskSize;
        return true;
    } catch (const std::bad_alloc&) {
        return PreparationFailed(error, QStringLiteral("候选渲染器内存不足"));
    } catch (const std::length_error&) {
        return PreparationFailed(error, QStringLiteral("候选渲染器资源长度超出范围"));
    }
}

bool CubismRenderer_OpenGLES2::PrepareModelReplacement(CubismModel* model,
                                                     const CubismRenderer_OpenGLES2& previous, QString* error)
{
    if (error) error->clear();
    if (this == &previous || previous.IsDrawing() || !previous.GetModel() || GetModel() || _textures.GetSize() ||
        !setGlOwningContext(previous.owningContext()))
        return PreparationFailed(error, QStringLiteral("模型替换需要同一 OpenGL 上下文中的全新渲染器"));
    try {
        auto matrix = previous.GetMvpMatrix();
        const auto color = previous.GetModelColor();
        for (int index = 0; index < 16; ++index)
            if (!std::isfinite(matrix.GetArray()[index]))
                return PreparationFailed(error, QStringLiteral("原渲染器矩阵包含非有限值"));
        if (!std::isfinite(color.R) || !std::isfinite(color.G) || !std::isfinite(color.B) ||
            !std::isfinite(color.A) || !std::isfinite(previous.GetAnisotropy()) || previous.GetAnisotropy() < 0.0f)
            return PreparationFailed(error, QStringLiteral("原渲染器颜色或过滤设置无效"));
        SetMvpMatrix(&matrix);
        SetModelColor(color.R, color.G, color.B, color.A);
        IsPremultipliedAlpha(previous.IsPremultipliedAlpha());
        IsCulling(previous.IsCulling());
        SetAnisotropy(previous.GetAnisotropy());
        UseHighPrecisionMask(previous.IsUsingHighPrecisionMask());
        for (auto texture = previous._textures.Begin(); texture != previous._textures.End(); ++texture) {
            if (texture->First < 0 || (texture->Second && !glIsTexture(texture->Second)))
                return PreparationFailed(error, QStringLiteral("原渲染器纹理绑定无效"));
            _textures[texture->First] = texture->Second;
        }
        return InitializeResources(model, previous._requestedMaskBufferCount,
                                   previous._requestedMaskBufferSize, true, error);
    } catch (const std::bad_alloc&) {
        return PreparationFailed(error, QStringLiteral("复制渲染设置时内存不足"));
    } catch (const std::length_error&) {
        return PreparationFailed(error, QStringLiteral("复制渲染设置时长度超出范围"));
    }
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
        for (auto texture = _textures.Begin(); texture != _textures.End(); ++texture)
        {
            if (!texture->Second) continue;
            glBindTexture(GL_TEXTURE_2D, texture->Second);
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
        if (!GetModel()->GetDrawableDynamicFlagVertexPositionsDidChange(clipDrawIndex)) continue;
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
                                            csmFloat32 opacityOverride, bool)
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
        const uint8_t opacity = static_cast<uint8_t>(qBound(0.0f, effectiveOpacity * GetModelColor().A * 255.0f, 255.0f));
        // Core clipping is already baked into the layer alpha.
        writer->AddLayer(drawName, rect, image, opacity,
                        CPsdWriteList::CubismBlendModeToPsd(GetModel()->GetDrawableBlendMode(drawableIndex)),
                        false, true);
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
    if (!UploadDrawableGeometry(model, index, positions, mesh, nullptr)) return false;
    if (!vertexCount || !indexCount) return true;
    if (IsCulling()) glEnable(GL_CULL_FACE);
    else glDisable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    auto* shader = CubismShader_OpenGLES2::GetInstance();
    const bool generatingMask = IsGeneratingMask();
    if (generatingMask) {
        if (!shader->SetupShaderProgramForMask(this, model, index)) return false;
    } else if (!shader->SetupShaderProgramForDraw(this, model, index, IsAll, opacityOverride,
        delegate->IsOffScreen() ? CubismShader_OpenGLES2::DrawMode::IsolatedPsdLayer : CubismShader_OpenGLES2::DrawMode::Preview)) {
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

void CubismRenderer_OpenGLES2::UseHighPrecisionMask(csmBool high)
{
    if (IsUsingHighPrecisionMask() == high) return;
    CubismRenderer::UseHighPrecisionMask(high);
    _maskBuffersNeedRedraw = true;
}

void CubismRenderer_OpenGLES2::SetClippingMaskBufferSize(csmFloat32 width, csmFloat32 height)
{
    int maskWidth = 0, maskHeight = 0;
    if (!GetModel() || !setGlOwningContext(owningContext()) ||
        !MaskDimensions(CubismVector2(width, height), maskWidth, maskHeight)) return;
    if (maskWidth > std::numeric_limits<int>::max() / 4 / maskHeight) return;
    GLint maximum = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
    if (maximum <= 0 || maskWidth > maximum || maskHeight > maximum || glGetError() != GL_NO_ERROR) return;
    if (!_clippingManager) {
        _requestedMaskBufferSize = CubismVector2(width, height);
        return;
    }
    try {
        CGlRenderState caller;
        if (!caller.Save(owningContext())) return;
        const auto restore = qScopeGuard([&] { caller.Restore(); });
        GLint read = 0, draw = 0, active = 0, units = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &units);
        const auto restoreActive = qScopeGuard([&] { glActiveTexture(active); });
        for (const auto& surface : _offscreenSurfaces)
            if (surface && (surface->GetRenderTexture() == GLuint(read) || surface->GetRenderTexture() == GLuint(draw))) return;
        for (int unit = 0; unit < units; ++unit) {
            glActiveTexture(GL_TEXTURE0 + unit);
            GLint texture = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
            for (const auto& surface : _offscreenSurfaces)
                if (surface && surface->GetColorBuffer() == GLuint(texture)) return;
        }
        glActiveTexture(active);
        if (glGetError() != GL_NO_ERROR) return;
        PreparedMaskResources replacement;
        if (!PrepareMaskResources(*GetModel(), _clippingManager->GetRenderTextureCount(),
                                  CubismVector2(width, height), replacement, nullptr)) return;
        CSM_DELETE_SELF(CubismClippingManager_OpenGLES2, _clippingManager);
        _clippingManager = replacement.clipping.release();
        _offscreenSurfaces.swap(replacement.surfaces);
        SetClippingContextBufferForMask(nullptr);
        SetClippingContextBufferForDraw(nullptr);
        _forceMaskRedraw = _clippingManager != nullptr;
        _maskBuffersNeedRedraw = _clippingManager != nullptr;
        _requestedMaskBufferSize = CubismVector2(width, height);
    } catch (const std::bad_alloc&) {
        return;
    } catch (const std::length_error&) {
        return;
    }
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
    for (auto texture = _textures.Begin(); texture != _textures.End(); ++texture)
        if (texture->First == textureId) return texture->Second;
    return 0;
}

}}}}

//------------ LIVE2D NAMESPACE ------------
