/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "CubismModelMatrix.hpp"
#include "framework/Type/csmString.hpp"
#include <cmath>
#include <limits>

namespace Live2D { namespace Cubism { namespace Framework {
namespace {
csmBool ToFloat(double value, csmFloat32& result)
{
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<csmFloat32>::max())
    {
        return false;
    }
    result = static_cast<csmFloat32>(value);
    return std::isfinite(result);
}
}

CubismModelMatrix::CubismModelMatrix()
                                : _width(0.0f)
                                , _height(0.0f)
{ }

CubismModelMatrix::CubismModelMatrix(csmFloat32 w, csmFloat32 h)
                                : _width(0.0f)
                                , _height(0.0f)
{
    csmFloat32 width, height;
    if (!TryComputeModelSize(w, h, 1.0f, width, height))
    {
        return;
    }
    _width = width;
    _height = height;
    SetHeight(2.0f);
}

CubismModelMatrix::~CubismModelMatrix()
{ }

csmBool CubismModelMatrix::TryComputeModelSize(csmFloat32 pixelWidth, csmFloat32 pixelHeight,
                                               csmFloat32 pixelsPerUnit, csmFloat32& unitWidth,
                                               csmFloat32& unitHeight)
{
    if (!std::isfinite(pixelWidth) || !std::isfinite(pixelHeight) || !std::isfinite(pixelsPerUnit) ||
        pixelWidth <= 0.0f || pixelHeight <= 0.0f || pixelsPerUnit <= 0.0f)
    {
        return false;
    }
    csmFloat32 width, height, scale;
    if (!ToFloat(static_cast<double>(pixelWidth) / pixelsPerUnit, width) || width <= 0.0f ||
        !ToFloat(static_cast<double>(pixelHeight) / pixelsPerUnit, height) || height <= 0.0f ||
        !ToFloat(2.0 / height, scale) || scale <= 0.0f)
    {
        return false;
    }
    unitWidth = width;
    unitHeight = height;
    return true;
}

csmBool CubismModelMatrix::IsValid() const
{
    if (!std::isfinite(_width) || !std::isfinite(_height) || _width <= 0.0f || _height <= 0.0f ||
        GetScaleX() == 0.0f || GetScaleY() == 0.0f)
    {
        return false;
    }
    for (csmFloat32 value : _tr)
    {
        if (!std::isfinite(value))
        {
            return false;
        }
    }
    return true;
}

csmBool CubismModelMatrix::TryScale(double value)
{
    csmFloat32 scale;
    if (!IsValid() || !ToFloat(value, scale) || scale == 0.0f)
    {
        return false;
    }
    Scale(scale, scale);
    return true;
}

csmBool CubismModelMatrix::TryPosition(double x, double y)
{
    csmFloat32 positionX, positionY;
    if (!IsValid() || !ToFloat(x, positionX) || !ToFloat(y, positionY))
    {
        return false;
    }
    Translate(positionX, positionY);
    return true;
}

void CubismModelMatrix::SetCenterPosition(csmFloat32 x, csmFloat32 y)
{
    TryPosition(static_cast<double>(x) - static_cast<double>(_width) * GetScaleX() / 2.0,
                static_cast<double>(y) - static_cast<double>(_height) * GetScaleY() / 2.0);
}

void CubismModelMatrix::Bottom(csmFloat32 y)
{
    TryPosition(GetTranslateX(), static_cast<double>(y) - static_cast<double>(_height) * GetScaleY());
}

void CubismModelMatrix::CenterY(csmFloat32 y)
{
    TryPosition(GetTranslateX(), static_cast<double>(y) - static_cast<double>(_height) * GetScaleY() / 2.0);
}

void CubismModelMatrix::Right(csmFloat32 x)
{
    TryPosition(static_cast<double>(x) - static_cast<double>(_width) * GetScaleX(), GetTranslateY());
}

void CubismModelMatrix::CenterX(csmFloat32 x)
{
    TryPosition(static_cast<double>(x) - static_cast<double>(_width) * GetScaleX() / 2.0, GetTranslateY());
}

void CubismModelMatrix::SetWidth(csmFloat32 w)
{
    if (IsValid())
    {
        TryScale(static_cast<double>(w) / _width);
    }
}

void CubismModelMatrix::SetHeight(csmFloat32 h)
{
    if (IsValid())
    {
        TryScale(static_cast<double>(h) / _height);
    }
}

csmBool CubismModelMatrix::TrySetupFromLayout(const csmMap<csmString, csmFloat32>& layout)
{
    if (!IsValid())
    {
        return false;
    }
    CubismModelMatrix candidate = *this;
    for (csmMap<csmString, csmFloat32>::const_iterator ite = layout.Begin(); ite != layout.End(); ++ite)
    {
        const csmString& key = ite->First;
        const csmFloat32 value = ite->Second;
        if (key == "width")
        {
            if (!candidate.TryScale(static_cast<double>(value) / _width)) return false;
        }
        else if (key == "height")
        {
            if (!candidate.TryScale(static_cast<double>(value) / _height)) return false;
        }
    }
    for (csmMap<csmString, csmFloat32>::const_iterator ite = layout.Begin(); ite != layout.End(); ++ite)
    {
        const csmString& key = ite->First;
        const csmFloat32 value = ite->Second;
        double x = candidate.GetTranslateX();
        double y = candidate.GetTranslateY();
        if (key == "x" || key == "left")
        {
            x = value;
        }
        else if (key == "y" || key == "top")
        {
            y = value;
        }
        else if (key == "center_x")
        {
            x = static_cast<double>(value) - static_cast<double>(_width) * candidate.GetScaleX() / 2.0;
        }
        else if (key == "center_y")
        {
            y = static_cast<double>(value) - static_cast<double>(_height) * candidate.GetScaleY() / 2.0;
        }
        else if (key == "bottom")
        {
            y = static_cast<double>(value) - static_cast<double>(_height) * candidate.GetScaleY();
        }
        else if (key == "right")
        {
            x = static_cast<double>(value) - static_cast<double>(_width) * candidate.GetScaleX();
        }
        else
        {
            continue;
        }
        if (!candidate.TryPosition(x, y)) return false;
    }
    *this = candidate;
    return true;
}

void CubismModelMatrix::SetupFromLayout(csmMap<csmString, csmFloat32>& layout)
{
    TrySetupFromLayout(layout);
}

void CubismModelMatrix::SetPosition(csmFloat32 x, csmFloat32 y)
{
    TryPosition(x, y);
}

void CubismModelMatrix::Top(csmFloat32 y)
{
    SetY(y);
}

void CubismModelMatrix::Left(csmFloat32 x)
{
    SetX(x);
}

void CubismModelMatrix::SetX(csmFloat32 x)
{
    TryPosition(x, GetTranslateY());
}

void CubismModelMatrix::SetY(csmFloat32 y)
{
    TryPosition(GetTranslateX(), y);
}

}}}
