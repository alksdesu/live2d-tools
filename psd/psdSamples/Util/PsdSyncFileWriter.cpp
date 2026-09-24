// Copyright 2011-2020, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)

#include "psd/psdSamples/PsdPch.h"
#include "psd/psdSamples/Util/PsdSyncFileWriter.h"

#include "psd/psdSamples/Interfaces/PsdFile.h"
#include <limits>


PSD_NAMESPACE_BEGIN

// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
SyncFileWriter::SyncFileWriter(File* file)
	: m_file(file)
	, m_position(0ull)
	, m_failed(file == nullptr)
{
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
void SyncFileWriter::Write(const void* buffer, uint32_t count)
{
	if (m_failed || count == 0) return;
	if (!buffer || m_position > std::numeric_limits<uint64_t>::max() - count)
	{
		m_failed = true;
		return;
	}
	File::WriteOperation op = m_file->Write(buffer, count, m_position);
	if (!op || !m_file->WaitForWrite(op))
	{
		m_failed = true;
		return;
	}
	m_position += count;
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
uint64_t SyncFileWriter::GetPosition(void) const
{
	return m_position;
}

PSD_NAMESPACE_END
