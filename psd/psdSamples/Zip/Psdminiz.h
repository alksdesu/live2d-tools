// Copyright 2011-2020, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)

#pragma once
#include "psd/psdSamples/Platform/PsdNamespace.h"
#include "psd/psdSamples/Platform/PsdCompilerMacros.h"


PSD_PUSH_WARNING_LEVEL(0)
	#define MINIZ_HEADER_FILE_ONLY
	#include "psd/psdSamples/Zip/Psdminiz.c"
	#undef MINIZ_HEADER_FILE_ONLY
PSD_POP_WARNING_LEVEL
