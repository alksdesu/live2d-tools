// Copyright 2011-2020, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)

#include "psd/psdSamples/PsdPch.h"
#include "psd/psdSamples/Parser/PsdParseColorModeDataSection.h"

#include "psd/psdSamples/Interfaces/PsdAllocator.h"
#include "psd/psdSamples/Platform/PsdAssert.h"
#include "psd/psdSamples/Sections/PsdColorModeDataSection.h"
#include "psd/psdSamples/Types/PsdDocument.h"
#include "psd/psdSamples/Util/PsdMemoryUtil.h"
#include "psd/psdSamples/Util/PsdSyncFileReader.h"
#include "psd/psdSamples/Util/PsdSyncFileUtil.h"


PSD_NAMESPACE_BEGIN

// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
ColorModeDataSection* ParseColorModeDataSection(const Document* document, File* file, Allocator* allocator)
{
	PSD_ASSERT_NOT_NULL(document);
	PSD_ASSERT_NOT_NULL(file);
	PSD_ASSERT_NOT_NULL(allocator);

	const Section& section = document->colorModeDataSection;
	if (section.length == 0u)
	{
		return nullptr;
	}

	ColorModeDataSection* colorModeData = memoryUtil::Allocate<ColorModeDataSection>(allocator);
	*colorModeData = ColorModeDataSection();

	SyncFileReader reader(file);
	reader.SetPosition(document->colorModeDataSection.offset);

	colorModeData->colorData = memoryUtil::AllocateArray<uint8_t>(allocator, section.length);
	colorModeData->sizeOfColorData = section.length;
	reader.Read(colorModeData->colorData, section.length);

	return colorModeData;
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
void DestroyColorModeDataSection(ColorModeDataSection*& section, Allocator* allocator)
{
	PSD_ASSERT_NOT_NULL(section);
	PSD_ASSERT_NOT_NULL(allocator);

	if (section->colorData)
	{
		memoryUtil::FreeArray(allocator, section->colorData);
	}
	memoryUtil::Free(allocator, section);
}

PSD_NAMESPACE_END
