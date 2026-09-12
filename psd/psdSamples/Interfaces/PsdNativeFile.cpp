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
#include <cwchar>
#include <limits>
#if !PSD_USE_MSVC
#include <sys/types.h>
#endif


PSD_NAMESPACE_BEGIN

#if !PSD_USE_MSVC

namespace {
struct PortableOperation {
    bool success;
};

bool ToUtf8Path(const wchar_t* source, char* target, size_t targetSize)
{
    if (!source || !target || targetSize == 0) return false;
    const wchar_t* cursor = source;
    mbstate_t state{};
    const size_t converted = ::wcsrtombs(target, &cursor, targetSize - 1, &state);
    if (converted == static_cast<size_t>(-1)) return false;
    target[converted] = '\0';
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
    char path[4096];
    if (!ToUtf8Path(filename, path, sizeof(path))) return false;
    m_file = std::fopen(path, "rb");
    return m_file != nullptr;
}

bool NativeFile::DoOpenWrite(const wchar_t* filename)
{
    char path[4096];
    if (!ToUtf8Path(filename, path, sizeof(path))) return false;
    m_file = std::fopen(path, "wb");
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
    ::fseeko(file, current, SEEK_SET);
    return end < 0 ? 0 : static_cast<uint64_t>(end);
}

#else

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
	const BOOL success = ::CloseHandle(m_file);
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
	OVERLAPPED* operation = memoryUtil::Allocate<OVERLAPPED>(m_allocator);
	operation->hEvent = nullptr;
	operation->Offset = static_cast<DWORD>(position & 0xFFFFFFFFull);
	operation->OffsetHigh = static_cast<DWORD>((position >> 32u) & 0xFFFFFFFFull);

	DWORD bytesRead = 0u;
	const BOOL success = ::ReadFile(m_file, buffer, count, &bytesRead, operation);
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
	OVERLAPPED* overlapped = static_cast<OVERLAPPED*>(operation);
	if (!overlapped)
		return false;

	DWORD bytesTransferred = 0u;
	const BOOL finished = ::GetOverlappedResult(m_file, overlapped, &bytesTransferred, true);

	// free object associated with read operation in any case, whether the operation has failed or not
	memoryUtil::Free(m_allocator, overlapped);

	if (finished == 0)
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
	OVERLAPPED* operation = memoryUtil::Allocate<OVERLAPPED>(m_allocator);
	operation->hEvent = nullptr;
	operation->Offset = static_cast<DWORD>(position & 0xFFFFFFFFull);
	operation->OffsetHigh = static_cast<DWORD>((position >> 32u) & 0xFFFFFFFFull);

	DWORD bytesWritten = 0u;
	const BOOL success = ::WriteFile(m_file, buffer, count, &bytesWritten, operation);
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
	OVERLAPPED* overlapped = static_cast<OVERLAPPED*>(operation);
	if (!overlapped)
		return false;

	DWORD bytesTransferred = 0u;
	const BOOL finished = ::GetOverlappedResult(m_file, overlapped, &bytesTransferred, true);

	// free object associated with write operation in any case, whether the operation has failed or not
	memoryUtil::Free(m_allocator, overlapped);

	if (finished == 0)
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
