/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppPal.hpp"
#include <cstdio>
#include <stdarg.h>
#include <sys/stat.h>
#include <iostream>
#include <fstream>
#include <limits>
#include <new>
#include <framework/Model/CubismMoc.hpp>
#include "LAppDefine.hpp"
#include <QDebug>
using std::endl;
using namespace Csm;
using namespace std;
using namespace LAppDefine;

double LAppPal::s_currentFrame = 0.0;
double LAppPal::s_lastFrame = 0.0;
double LAppPal::s_deltaTime = 0.0;

csmByte* LAppPal::LoadFileAsBytes(const string filePath, csmSizeInt* outSize)
{
    if (!outSize) return nullptr;
    *outSize = 0;

    // 创建QFile对象
    QFile file(filePath.c_str());

    // 以二进制只读方式打开文件
    if (!file.open(QIODevice::ReadOnly )) {
        qDebug() << "Failed to open the file.";
        return nullptr;
    }

    // 获取文件大小
    qint64 size = file.size();
    if (size <= 0 || size > std::numeric_limits<int>::max() || size > std::numeric_limits<csmSizeInt>::max()) {
        qDebug() << "File is empty or an error occurred.";
        file.close();
        return nullptr;
    }

    // 动态分配内存
    char* buf = new (std::nothrow) char[size];
    if (!buf) return nullptr;

    // 读取文件内容到buf
    qint64 bytesRead = file.read(buf, size);
    if (bytesRead != size) {
        qDebug() << "Error reading file. Expected:" << size << "bytes, but read:" << bytesRead << "bytes.";
        delete[] buf; // 释放内存
        file.close();
        return nullptr;
    }

    // 关闭文件
    file.close();
    *outSize = size;
    return reinterpret_cast<csmByte*>(buf);
}

void LAppPal::ReleaseBytes(csmByte* byteData)
{
    delete[] byteData;
}

csmFloat32  LAppPal::GetDeltaTime()
{
    return static_cast<csmFloat32>(s_deltaTime);
}

void LAppPal::UpdateTime()
{
    s_currentFrame = QDateTime::currentMSecsSinceEpoch() / 1000.0;;
    s_deltaTime = s_currentFrame - s_lastFrame;
    s_lastFrame = s_currentFrame;
}

void LAppPal::ResetTime()
{
    s_currentFrame = QDateTime::currentMSecsSinceEpoch() / 1000.0;
    s_lastFrame = s_currentFrame;
    s_deltaTime = 0.0;
}

void LAppPal::PrintLog(const csmChar* format, ...)
{
    if (!format) return;
    va_list args;
    csmChar buf[256];
    va_start(args, format);
#if defined(_MSC_VER)
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, format, args);
#else
    std::vsnprintf(buf, sizeof(buf), format, args);
#endif
#ifdef CSM_DEBUG_MEMORY_LEAKING
// メモリリークチェック時は大量の標準出力がはしり重いのでprintfを利用する
    std::printf("%s", buf);
#else
    std::cout << buf;
#endif
    va_end(args);
}

void LAppPal::PrintLogLn(const Csm::csmChar* format, ...)
{
    if (!format) return;
    va_list args;
    csmChar buf[256];
    va_start(args, format);
#if defined(_MSC_VER)
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, format, args);
#else
    std::vsnprintf(buf, sizeof(buf), format, args);
#endif
#ifdef CSM_DEBUG_MEMORY_LEAKING
    // メモリリークチェック時は大量の標準出力がはしり重いのでprintfを利用する
    std::printf("%s\n", buf);
#else
    std::cout << buf << std::endl;
#endif
    va_end(args);
}

void LAppPal::PrintMessage(const csmChar* message)
{
    PrintLog("%s", message);
}

void LAppPal::PrintMessageLn(const csmChar* message)
{
    PrintLogLn("%s", message);
}
