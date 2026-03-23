/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#pragma once

#include <framework/CubismFramework.hpp>
#include <string>
#include <QTime>
#include <QFileInfo>
#include <QIODevice>
class LAppPal
{
public:
    static Csm::csmByte* LoadFileAsBytes(const std::string filePath, Csm::csmSizeInt* outSize);
    static void ReleaseBytes(Csm::csmByte* byteData);
    static Csm::csmFloat32 GetDeltaTime();
    static void UpdateTime();
    static void PrintLog(const Csm::csmChar* format, ...);
    static void PrintLogLn(const Csm::csmChar* format, ...);
    static void PrintMessage(const Csm::csmChar* message);
    static void PrintMessageLn(const Csm::csmChar* message);
private:
    static double s_currentFrame;
    static double s_lastFrame;
    static double s_deltaTime;


};

