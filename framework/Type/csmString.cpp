/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "csmString.hpp"
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include "framework/CubismFramework.hpp"
#include "framework/Utils/CubismDebug.hpp"

namespace Live2D { namespace Cubism { namespace Framework {
csmInt32 csmString::s_totalInstanceNo = 0;

namespace {
const csmChar* s_emptyString = "";
const csmInt32 MaximumLength = std::numeric_limits<csmInt32>::max() - 1;

csmChar* GetEmptyString()
{
    return const_cast<csmChar*>(s_emptyString);
}

void ValidateLength(csmInt32 length)
{
    if (length < 0 || length > MaximumLength)
    {
        throw std::length_error("csmString length is out of range");
    }
}

csmInt32 CheckedLength(const csmChar* value)
{
    if (!value)
    {
        throw std::invalid_argument("csmString requires a string");
    }
    const csmSizeType length = std::strlen(value);
    if (length > static_cast<csmSizeType>(MaximumLength))
    {
        throw std::length_error("csmString length is out of range");
    }
    return static_cast<csmInt32>(length);
}

csmInt32 CombinedLength(csmInt32 first, csmInt32 second)
{
    ValidateLength(first);
    ValidateLength(second);
    if (second > MaximumLength - first)
    {
        throw std::length_error("csmString length is out of range");
    }
    return first + second;
}
}

csmString::csmString()
    : _ptr(NULL)
    , _length(0)
    , _hashcode(-1)
    , _instanceNo(s_totalInstanceNo++)
    , _small()
{
    _hashcode = CalcHashcode(WritePointer(), _length);
}

csmString::csmString(const csmChar* c) : csmString()
{
    Copy(c, CheckedLength(c));
}

csmString::csmString(const csmString& s) : csmString()
{
    Copy(s.GetRawString(), s._length);
}

csmString::csmString(const csmChar* c, csmInt32 length) : csmString()
{
    Copy(c, length);
}

csmString::csmString(const csmChar* c, csmInt32 length, csmBool usePtr) : csmString()
{
    Initialize(c, length, usePtr);
}

void csmString::Initialize(const csmChar* c, csmInt32 length, csmBool usePtr)
{
    ValidateLength(length);
    if (length > 0 && !c)
    {
        throw std::invalid_argument("csmString requires a string");
    }
    if (!usePtr)
    {
        Copy(c, length);
        return;
    }
    if (!c)
    {
        Clear();
        return;
    }
    if (c == GetRawString())
    {
        if (length > _length)
        {
            throw std::length_error("csmString cannot extend an owned buffer");
        }
        _length = length;
        WritePointer()[length] = '\0';
        _hashcode = CalcHashcode(GetRawString(), length);
        return;
    }
    const std::less<const csmChar*> less;
    if ((!less(c, _small) && less(c, _small + SmallLength))
        || (_ptr && less(_ptr, c) && !less(_ptr + _length, c)))
    {
        throw std::invalid_argument("csmString cannot adopt an interior pointer");
    }
    csmString replacement;
    replacement._ptr = const_cast<csmChar*>(c);
    replacement._length = length;
    replacement._ptr[length] = '\0';
    replacement._hashcode = replacement.CalcHashcode(replacement.GetRawString(), length);
    SwapValue(replacement);
}

csmString::~csmString()
{
    Clear();
}

void csmString::Clear()
{
    if (_ptr)
    {
        CSM_FREE(_ptr);
    }
    SetEmpty();
}

void csmString::SwapValue(csmString& other)
{
    std::swap(_ptr, other._ptr);
    std::swap(_length, other._length);
    std::swap(_hashcode, other._hashcode);
    std::swap(_small, other._small);
}

void csmString::InitializeBuffer(csmInt32 length)
{
    ValidateLength(length);
    csmChar* buffer = NULL;
    if (length >= SmallLength - 1)
    {
        buffer = static_cast<csmChar*>(CSM_MALLOC(static_cast<csmSizeType>(length) + 1));
        if (!buffer)
        {
            throw std::bad_alloc();
        }
    }
    Clear();
    _ptr = buffer;
    _length = length;
    WritePointer()[length] = '\0';
    _hashcode = -1;
}

void csmString::Copy(const csmChar* c, csmInt32 length)
{
    ValidateLength(length);
    if (length > 0 && !c)
    {
        throw std::invalid_argument("csmString requires a string");
    }
    if (length == 0)
    {
        Clear();
        return;
    }
    csmString replacement;
    replacement.InitializeBuffer(length);
    std::memcpy(replacement.WritePointer(), c, static_cast<csmSizeType>(length));
    replacement._hashcode = replacement.CalcHashcode(replacement.GetRawString(), length);
    SwapValue(replacement);
}

csmString& csmString::operator=(const csmChar* c)
{
    Copy(c, CheckedLength(c));
    return *this;
}

csmString& csmString::operator=(const csmString& s)
{
    if (this != &s)
    {
        Copy(s.GetRawString(), s._length);
    }
    return *this;
}

csmString csmString::Concat(const csmString& left, const csmChar* right, csmInt32 length)
{
    const csmInt32 total = CombinedLength(left._length, length);
    if (length > 0 && !right)
    {
        throw std::invalid_argument("csmString requires a string");
    }
    csmString result;
    result.InitializeBuffer(total);
    if (left._length > 0)
    {
        std::memcpy(result.WritePointer(), left.GetRawString(), static_cast<csmSizeType>(left._length));
    }
    if (length > 0)
    {
        std::memcpy(result.WritePointer() + left._length, right, static_cast<csmSizeType>(length));
    }
    result._hashcode = result.CalcHashcode(result.GetRawString(), total);
    return result;
}

csmString csmString::operator+(const csmString& s) const
{
    return Concat(*this, s.GetRawString(), s._length);
}

csmString csmString::operator+(const csmChar* c) const
{
    return Concat(*this, c, CheckedLength(c));
}

csmString& csmString::operator+=(const csmString& s)
{
    return Append(s.GetRawString(), s._length);
}

csmString& csmString::operator+=(const csmChar* c)
{
    return Append(c, CheckedLength(c));
}

csmString& csmString::Append(const csmChar* c, csmInt32 length)
{
    CombinedLength(_length, length);
    if (length == 0)
    {
        return *this;
    }
    csmString replacement = Concat(*this, c, length);
    SwapValue(replacement);
    return *this;
}

csmString& csmString::Append(csmInt32 length, const csmChar value)
{
    const csmInt32 total = CombinedLength(_length, length);
    if (length == 0)
    {
        return *this;
    }
    csmString replacement;
    replacement.InitializeBuffer(total);
    if (_length > 0)
    {
        std::memcpy(replacement.WritePointer(), GetRawString(), static_cast<csmSizeType>(_length));
    }
    std::memset(replacement.WritePointer() + _length, static_cast<unsigned char>(value), static_cast<csmSizeType>(length));
    replacement._hashcode = replacement.CalcHashcode(replacement.GetRawString(), total);
    SwapValue(replacement);
    return *this;
}

csmBool csmString::operator==(const csmString& s) const
{
    if (s._length != _length || s._hashcode != _hashcode)
    {
        return false;
    }
    const csmChar* first = GetRawString();
    const csmChar* second = s.GetRawString();
    for (csmInt32 i = _length - 1; i >= 0; --i)
    {
        if (first[i] != second[i]) return false;
    }
    return true;
}

csmBool csmString::operator==(const csmChar* other) const
{
    if (CheckedLength(other) != _length)
    {
        return false;
    }
    const csmChar* value = GetRawString();
    for (csmInt32 i = _length - 1; i >= 0; --i)
    {
        if (value[i] != other[i]) return false;
    }
    return true;
}

csmBool csmString::operator<(const csmString& s) const
{
    return std::strcmp(GetRawString(), s.GetRawString()) < 0;
}

csmBool csmString::operator<(const csmChar* c) const
{
    return std::strcmp(GetRawString(), c) < 0;
}

csmBool csmString::operator>(const csmString& s) const
{
    return std::strcmp(GetRawString(), s.GetRawString()) > 0;
}

csmBool csmString::operator>(const csmChar* c) const
{
    return std::strcmp(GetRawString(), c) > 0;
}

csmInt32 csmString::CalcHashcode(const csmChar* c, csmInt32 length)
{
    csmUint32 hash = 0;
    for (csmInt32 i = length; i >= 0; --i)
    {
        hash = hash * 31u + static_cast<csmUint32>(static_cast<csmInt32>(c[i]));
    }
    csmInt32 result;
    std::memcpy(&result, &hash, sizeof(result));
    if ((result == -1) || (c == GetEmptyString()))
    {
        result = -2; //-1だけ特別な意味をもたせる
    }
    return result;
}

const csmChar* csmString::GetRawString() const
{
    return _ptr ? _ptr : _small;
}

csmInt32 csmString::GetHashcode()
{
    if (_hashcode == -1) _hashcode = CalcHashcode(WritePointer(), _length);
    return _hashcode;
}

csmBool csmString::IsEmpty() const
{
    return _length == 0;
}

void csmString::SetEmpty()
{
    _ptr = NULL;
    _small[0] = '\0';
    _length = 0;
    _hashcode = CalcHashcode(WritePointer(), _length);
}

csmChar* csmString::WritePointer()
{
    return _ptr ? _ptr : _small;
}

}}}
