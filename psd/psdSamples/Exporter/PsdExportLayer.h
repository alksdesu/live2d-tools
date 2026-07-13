// Copyright 2011-2020, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)

#pragma once


#include "psd/psdSamples/Platform/PsdNamespace.h"
#include "psd/psdSamples/Platform/PsdTypes.h"


PSD_NAMESPACE_BEGIN


struct ExportLayer
{
	// the SDK currently supports R, G, B, A
	static const unsigned int MAX_CHANNEL_COUNT = 4u;
	int32_t top;
	int32_t left;
	int32_t bottom;
	int32_t right;
	char* name;
	uint16_t* utf16Name;        // Unicode layer name for the 'luni' block (UTF-16 code units, no terminator)
	uint32_t utf16NameLength;   // number of UTF-16 code units in utf16Name
	void* channelData[MAX_CHANNEL_COUNT];
	uint32_t channelSize[MAX_CHANNEL_COUNT];
	uint16_t channelCompression[MAX_CHANNEL_COUNT];
    uint32_t type;
	bool IsEmptyChannel;

	uint8_t opacity;        // 0-255, default 255
	uint8_t clipping;       // 0=base, 1=non-base (clipped to layer below)
	uint32_t blendModeKey;  // 4-char key: 'norm'=0x6E6F726D, 'lddg'=0x6C646467, 'mul '=0x6D756C20
};

PSD_NAMESPACE_END
