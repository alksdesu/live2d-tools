/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#pragma once

#include <framework/CubismFramework.hpp>
#include <framework/Math/CubismMatrix44.hpp>
#include <framework/Type/csmVector.hpp>
#include <QString>
#include <QFileInfo>
#include <QDirIterator>
#include <QMatrix4x4>
#include <memory>
#include <vector>
class LAppModel;

/**
* @brief サンプルアプリケーションにおいてCubismModelを管理するクラス<br>
*         モデル生成と破棄、タップイベントの処理、モデル切り替えを行う。
*
*/
class LAppLive2DManager
{

public:

    static LAppLive2DManager* GetInstance();
    static void ReleaseInstance();
    void SetUpModel();
    Csm::csmVector<Csm::csmString> GetModelDir() const;
    Csm::csmInt32 GetModelDirSize() const;
    LAppModel* GetModel(Csm::csmUint32 no) const;
    void ReleaseAllModel();
    void OnDrag(Csm::csmFloat32 x, Csm::csmFloat32 y) const;
    void OnTap(Csm::csmFloat32 x, Csm::csmFloat32 y);
    void OnUpdate(Csm::CubismMatrix44& projection) const;
    void NextScene();
    bool ChangeScene(Csm::csmInt32 index);
    bool ChangeSceneFileName(const QString& name);
    bool BeginTemporaryScene(const QString& name);
    bool EndTemporaryScene();
    Csm::csmUint32 GetModelNum() const;
    void SetViewMatrix(Live2D::Cubism::Framework::CubismMatrix44* m);



private:

    LAppLive2DManager();
    virtual ~LAppLive2DManager();
    void CommitModelTextures();

    Csm::CubismMatrix44* _viewMatrix; ///< モデル描画に用いるView行列
    std::vector<std::unique_ptr<LAppModel>> _models;
    std::vector<std::unique_ptr<LAppModel>> m_suspendedModels;
    Csm::csmInt32 m_suspendedSceneIndex = 0;
    bool m_temporaryScene = false;
    Csm::csmInt32 _sceneIndex; ///< 表示するシーンのインデックス値
    Csm::csmVector<Csm::csmString> _modelDir; ///< モデルディレクトリ名のコンテナ
};
