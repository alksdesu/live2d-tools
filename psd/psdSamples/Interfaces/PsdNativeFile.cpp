// Copyright 2011-2020, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)

#include "psd/psdSamples/PsdPch.h"
#include "psd/psdSamples/Interfaces/PsdNativeFile.h"

#include "psd/psdSamples/Interfaces/PsdAllocator.h"
#include "psd/psdSamples/Platform/PsdPlatform.h"
#include "psd/psdSamples/Util/PsdMemoryUtil.h"
#include "psd/psdSamples/Platform/PsdLog.h"
#include "psd/psdSamples/Psdinttypes.h"
#include <cstdio>
#include <limits>
#include <string>
#include <type_traits>
#if !PSD_USE_MSVC
#include <sys/types.h>
#endif


PSD_NAMESPACE_BEGIN

NativeFile::~NativeFile(void)
{
    DoClose();
}

#if !PSD_USE_MSVC

namespace {
struct PortableOperation {
    bool success;
};

bool ToUtf8Path(const wchar_t* source, std::string& target)
{
    if (!source) return false;
    target.clear();
    using WideCodeUnit = std::make_unsigned<wchar_t>::type;
    while (*source) {
        uint32_t codepoint = static_cast<WideCodeUnit>(*source++);
        if (sizeof(wchar_t) == 2 && codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            const uint32_t low = static_cast<WideCodeUnit>(*source++);
            if (low < 0xDC00 || low > 0xDFFF) return false;
            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + low - 0xDC00;
        }
        if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return false;
        if (codepoint <= 0x7F) {
            target.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FF) {
            target.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            target.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF) {
            target.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            target.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            target.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            target.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            target.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            target.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            target.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }
    return true;
}
}

NativeFile::NativeFile(Allocator* allocator)
    : File(allocator)
    , m_file(nullptr)
{
}

bool NativeFile::DoOpenRead(const wchar_t* filename)
{
    if (m_file) return false;
    std::string path;
    if (!ToUtf8Path(filename, path)) return false;
    m_file = std::fopen(path.c_str(), "rb");
    return m_file != nullptr;
}

bool NativeFile::DoOpenWrite(const wchar_t* filename)
{
    if (m_file) return false;
    std::string path;
    if (!ToUtf8Path(filename, path)) return false;
    m_file = std::fopen(path.c_str(), "wb");
    return m_file != nullptr;
}

bool NativeFile::DoClose(void)
{
    FILE* file = static_cast<FILE*>(m_file);
    m_file = nullptr;
    return !file || std::fclose(file) == 0;
}

File::ReadOperation NativeFile::DoRead(void* buffer, uint32_t count, uint64_t position)
{
    FILE* file = static_cast<FILE*>(m_file);
    bool success = file && position <= static_cast<uint64_t>(std::numeric_limits<off_t>::max()) &&
        ::fseeko(file, static_cast<off_t>(position), SEEK_SET) == 0 && std::fread(buffer, 1, count, file) == count;
    return new PortableOperation{success};
}

bool NativeFile::DoWaitForRead(File::ReadOperation& operation)
{
    auto* result = static_cast<PortableOperation*>(operation);
    if (!result) return false;
    const bool success = result->success;
    delete result;
    operation = nullptr;
    return success;
}

File::WriteOperation NativeFile::DoWrite(const void* buffer, uint32_t count, uint64_t position)
{
    FILE* file = static_cast<FILE*>(m_file);
    bool success = file && position <= static_cast<uint64_t>(std::numeric_limits<off_t>::max()) &&
        ::fseeko(file, static_cast<off_t>(position), SEEK_SET) == 0 && std::fwrite(buffer, 1, count, file) == count;
    return new PortableOperation{success};
}

bool NativeFile::DoWaitForWrite(File::WriteOperation& operation)
{
    auto* result = static_cast<PortableOperation*>(operation);
    if (!result) return false;
    const bool success = result->success;
    delete result;
    operation = nullptr;
    return success;
}

uint64_t NativeFile::DoGetSize(void) const
{
    FILE* file = static_cast<FILE*>(m_file);
    if (!file) return 0;
    const off_t current = ::ftello(file);
    if (current < 0 || ::fseeko(file, 0, SEEK_END) != 0) return 0;
    const off_t end = ::ftello(file);
    const bool restored = ::fseeko(file, current, SEEK_SET) == 0;
    return end < 0 || !restored ? 0 : static_cast<uint64_t>(end);
}

#else

namespace {
struct NativeOperation {
    OVERLAPPED overlapped;
    uint32_t requestedCount;
};
}

// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
NativeFile::NativeFile(Allocator* allocator)
	: File(allocator)
	, m_file(INVALID_HANDLE_VALUE)
{
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
bool NativeFile::DoOpenRead(const wchar_t* filename)
{
	if (m_file != INVALID_HANDLE_VALUE || !filename) return false;
	m_file = ::CreateFileW(filename, FILE_READ_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED, nullptr);
	if (m_file == INVALID_HANDLE_VALUE)
	{
		PSD_ERROR("NativeFile", "Cannot obtain handle for file \"%ls\".", filename);
		return false;
	}

	return true;
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
bool NativeFile::DoOpenWrite(const wchar_t* filename)
{
	if (m_file != INVALID_HANDLE_VALUE || !filename) return false;
	m_file = ::CreateFileW(filename, FILE_WRITE_DATA, FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED, nullptr);
	if (m_file == INVALID_HANDLE_VALUE)
	{
		PSD_ERROR("NativeFile", "Cannot obtain handle for file \"%ls\".", filename);
		return false;
	}

	return true;
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
bool NativeFile::DoClose(void)
{
	if (m_file == INVALID_HANDLE_VALUE) return true;
	const HANDLE file = m_file;
	m_file = INVALID_HANDLE_VALUE;
	const BOOL success = ::CloseHandle(file);
	if  (success == 0)
	{
		PSD_ERROR("NativeFile", "Cannot close handle.");
		return false;
	}

	return true;
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
File::ReadOperation NativeFile::DoRead(void* buffer, uint32_t count, uint64_t position)
{
	NativeOperation* operation = memoryUtil::Allocate<NativeOperation>(m_allocator);
	if (!operation) return nullptr;
	operation->overlapped = {};
	operation->requestedCount = count;
	operation->overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFull);
	operation->overlapped.OffsetHigh = static_cast<DWORD>((position >> 32u) & 0xFFFFFFFFull);

	DWORD bytesRead = 0u;
	const BOOL success = ::ReadFile(m_file, buffer, count, &bytesRead, &operation->overlapped);
	if ((success == 0) && (::GetLastError() != ERROR_IO_PENDING))
	{
		PSD_ERROR("NativeFile", "Cannot read %u bytes from file position %" PRIu64 " asynchronously.", count, position);

		// the read operation failed, so don't return a useful object here
		memoryUtil::Free(m_allocator, operation);

		return nullptr;
	}

	return static_cast<File::ReadOperation>(operation);
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
bool NativeFile::DoWaitForRead(File::ReadOperation& operation)
{
	NativeOperation* pending = static_cast<NativeOperation*>(operation);
	if (!pending)
		return false;

	DWORD bytesTransferred = 0u;
	const BOOL finished = ::GetOverlappedResult(m_file, &pending->overlapped, &bytesTransferred, true);
	const bool complete = finished != 0 && bytesTransferred == pending->requestedCount;

	// free object associated with read operation in any case, whether the operation has failed or not
	memoryUtil::Free(m_allocator, pending);
	operation = nullptr;

	if (!complete)
	{
		PSD_ERROR("NativeFile", "Failed to wait for previous asynchronous read operation.");
		return false;
	}

	return true;
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
File::WriteOperation NativeFile::DoWrite(const void* buffer, uint32_t count, uint64_t position)
{
	NativeOperation* operation = memoryUtil::Allocate<NativeOperation>(m_allocator);
	if (!operation) return nullptr;
	operation->overlapped = {};
	operation->requestedCount = count;
	operation->overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFull);
	operation->overlapped.OffsetHigh = static_cast<DWORD>((position >> 32u) & 0xFFFFFFFFull);

	DWORD bytesWritten = 0u;
	const BOOL success = ::WriteFile(m_file, buffer, count, &bytesWritten, &operation->overlapped);
	if ((success == 0) && (::GetLastError() != ERROR_IO_PENDING))
	{
		PSD_ERROR("NativeFile", "Cannot write %u bytes at file position %" PRIu64 " asynchronously.", count, position);

		// the write operation failed, so don't return a useful object here
		memoryUtil::Free(m_allocator, operation);

		return nullptr;
	}

	return static_cast<File::WriteOperation>(operation);
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
bool NativeFile::DoWaitForWrite(File::WriteOperation& operation)
{
	NativeOperation* pending = static_cast<NativeOperation*>(operation);
	if (!pending)
		return false;

	DWORD bytesTransferred = 0u;
	const BOOL finished = ::GetOverlappedResult(m_file, &pending->overlapped, &bytesTransferred, true);
	const bool complete = finished != 0 && bytesTransferred == pending->requestedCount;

	// free object associated with write operation in any case, whether the operation has failed or not
	memoryUtil::Free(m_allocator, pending);
	operation = nullptr;

	if (!complete)
	{
		PSD_ERROR("NativeFile", "Failed to wait for previous asynchronous write operation.");
		return false;
	}

	return true;
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
uint64_t NativeFile::DoGetSize(void) const
{
	LARGE_INTEGER size = {};
	const BOOL success = ::GetFileSizeEx(m_file, &size);
	if (success == 0)
	{
		PSD_ERROR("NativeFile", "Failed to retrieve file size.");
		return 0ull;
	}

	return static_cast<uint64_t>(size.QuadPart);
}

#endif

PSD_NAMESPACE_END
