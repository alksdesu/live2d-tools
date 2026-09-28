// Copyright 2011-2020, Molecular Matters GmbH <office@molecular-matters.com>
// See LICENSE.txt for licensing details (2-clause BSD License: https://opensource.org/licenses/BSD-2-Clause)

#include "psd/psdSamples/PsdPch.h"
#include "psd/psdSamples/Exporter/PsdExport.h"

#include "psd/psdSamples/Util/PsdMemoryUtil.h"
#include "psd/psdSamples/Types/PsdImageResourceType.h"
#include "psd/psdSamples/Exporter/PsdExportDocument.h"
#include "psd/psdSamples/ImageUtil/PsdDecompressRle.h"
#include "psd/psdSamples/Util/PsdSyncFileWriter.h"
#include "psd/psdSamples/Util/PsdSyncFileUtil.h"
#include "psd/psdSamples/Util/PsdKey.h"
#include "psd/psdSamples/PsdChannelType.h"
#include "psd/psdSamples/Util/PsdBitUtil.h"
#include "psd/psdSamples/PsdThumbnail.h"
#include "psd/psdSamples/Zip/Psdminiz.h"
#include <string.h>
#include <limits>
#include <algorithm>
#include <memory>
#include <new>
#include <stdexcept>
#include <array>


PSD_NAMESPACE_BEGIN

namespace
{
	static const char XMP_HEADER[]= "<x:xmpmeta xmlns:x = \"adobe:ns:meta/\">\n"
		"<rdf:RDF xmlns:rdf = \"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
		"<rdf:Description rdf:about=\"\"\n"
		"xmlns:xmp = \"http://ns.adobe.com/xap/1.0/\"\n"
		"xmlns:dc = \"http://purl.org/dc/elements/1.1/\"\n"
		"xmlns:photoshop = \"http://ns.adobe.com/photoshop/1.0/\"\n"
		"xmlns:xmpMM = \"http://ns.adobe.com/xap/1.0/mm/\"\n"
		"xmlns:stEvt = \"http://ns.adobe.com/xap/1.0/sType/ResourceEvent#\">\n";

	static const char XMP_FOOTER[] =
		"</rdf:Description>\n"
		"</rdf:RDF>\n"
		"</x:xmpmeta>\n";


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static char* CreateString(Allocator* allocator, const char* str)
	{
		const size_t length = strlen(str);
		const size_t paddedLength = bitUtil::RoundUpToMultiple(length + 1u, static_cast<size_t>(4u));
		char* newString = memoryUtil::AllocateArray<char>(allocator, paddedLength);
		if (!newString) throw std::bad_alloc();

		// clear and copy null terminator as well
		memset(newString, 0, paddedLength);
		memcpy(newString, str, length + 1u);

		return newString;
	}

	
	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static void DestroyString(Allocator* allocator, char*& str)
	{
		memoryUtil::FreeArray(allocator, str);
		str = nullptr;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	// Decodes a UTF-8 string into UTF-16 code units (with surrogate pairs) for the 'luni' layer name block.
	// Invalid byte sequences are replaced with U+FFFD. Returns nullptr for empty input.
	static uint16_t* CreateUTF16String(Allocator* allocator, const char* utf8, uint32_t* outLength)
	{
		const size_t byteCount = strlen(utf8);
		*outLength = 0u;
		if (byteCount == 0u)
		{
			return nullptr;
		}

		// UTF-16 code unit count never exceeds UTF-8 byte count
		uint16_t* buffer = memoryUtil::AllocateArray<uint16_t>(allocator, byteCount);
		if (!buffer) throw std::bad_alloc();
		uint32_t length = 0u;

		size_t i = 0u;
		while (i < byteCount)
		{
			const uint8_t b0 = static_cast<uint8_t>(utf8[i]);
			uint32_t codePoint = 0xFFFDu;
			size_t sequenceLength = 1u;

			if (b0 < 0x80u)
			{
				codePoint = b0;
			}
			else if ((b0 & 0xE0u) == 0xC0u && i + 1u < byteCount)
			{
				const uint8_t b1 = static_cast<uint8_t>(utf8[i + 1u]);
				if ((b1 & 0xC0u) == 0x80u)
				{
					codePoint = ((b0 & 0x1Fu) << 6u) | (b1 & 0x3Fu);
					sequenceLength = 2u;
				}
			}
			else if ((b0 & 0xF0u) == 0xE0u && i + 2u < byteCount)
			{
				const uint8_t b1 = static_cast<uint8_t>(utf8[i + 1u]);
				const uint8_t b2 = static_cast<uint8_t>(utf8[i + 2u]);
				if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u)
				{
					codePoint = ((b0 & 0x0Fu) << 12u) | ((b1 & 0x3Fu) << 6u) | (b2 & 0x3Fu);
					sequenceLength = 3u;
				}
			}
			else if ((b0 & 0xF8u) == 0xF0u && i + 3u < byteCount)
			{
				const uint8_t b1 = static_cast<uint8_t>(utf8[i + 1u]);
				const uint8_t b2 = static_cast<uint8_t>(utf8[i + 2u]);
				const uint8_t b3 = static_cast<uint8_t>(utf8[i + 3u]);
				if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u && (b3 & 0xC0u) == 0x80u)
				{
					codePoint = ((b0 & 0x07u) << 18u) | ((b1 & 0x3Fu) << 12u) | ((b2 & 0x3Fu) << 6u) | (b3 & 0x3Fu);
					sequenceLength = 4u;
				}
			}

			if (codePoint >= 0x10000u)
			{
				const uint32_t v = codePoint - 0x10000u;
				buffer[length++] = static_cast<uint16_t>(0xD800u + (v >> 10u));
				buffer[length++] = static_cast<uint16_t>(0xDC00u + (v & 0x3FFu));
			}
			else
			{
				buffer[length++] = static_cast<uint16_t>(codePoint);
			}

			i += sequenceLength;
		}

		*outLength = length;
		return buffer;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	// 'luni' block: signature(4) + key(4) + length(4) + [charCount(4) + UTF-16BE data], data padded to 4 bytes
	static uint32_t GetLuniBlockLength(ExportLayer* layer)
	{
		const uint32_t dataLength = 4u + layer->utf16NameLength * 2u;
		const uint32_t paddedDataLength = bitUtil::RoundUpToMultiple(dataLength, 4u);
		return 4u + 4u + 4u + paddedDataLength;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static uint16_t GetChannelCount(ExportLayer* layer)
	{
		uint16_t count = 0u;
		for (unsigned int i = 0u; i < ExportLayer::MAX_CHANNEL_COUNT; ++i)
		{
			if (layer->channelData[i])
			{
				++count;
			}
		}

		return count;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static uint32_t GetChannelDataSize(ExportLayer* layer)
	{
		uint32_t size = 0u;
		for (unsigned int i = 0u; i < ExportLayer::MAX_CHANNEL_COUNT; ++i)
		{
			if (layer->channelData[i])
			{
				size += layer->channelSize[i];				
			}
		}

		return size;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static unsigned int GetChannelIndex(exportChannel::Enum channel)
	{
		switch (channel)
		{
			case exportChannel::GRAY:
				return 0u;

			case exportChannel::RED:
				return 0u;

			case exportChannel::GREEN:
				return 1u;

			case exportChannel::BLUE:
				return 2u;

			case exportChannel::ALPHA:
				return 3u;

			default:
				return 0u;
		}
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static int16_t GetChannelId(unsigned int channelIndex)
	{
		switch (channelIndex)
		{
			case 0u:
				return channelType::R;

			case 1u:
				return channelType::G;

			case 2u:
				return channelType::B;

			case 3u:
				return channelType::TRANSPARENCY_MASK;

			default:
				return 0u;
		}
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static uint32_t GetExtraDataLength(ExportLayer* layer)
	{
		const uint8_t nameLength = static_cast<uint8_t>(strlen(layer->name));
		const uint32_t paddedNameLength = bitUtil::RoundUpToMultiple(nameLength + 1u, 4u);

		// includes the lengths of the layer mask data and layer blending ranges data
		//4 + 4 + 4 + 4 ·Ö±ðÎªÇ©Ãû£¬ key, length ,data
		uint32_t length = (4u + 4u + paddedNameLength) + 4 + 8 +4;

		if (layer->utf16Name)
		{
			length += GetLuniBlockLength(layer);
		}

		if (layer->hasLayerId) length += 16u;
		if (layer->hasSectionBlendMode) length += 8u;
		return length;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static uint32_t GetLayerInfoSectionLength(ExportDocument* document)
	{
		// the layer info section includes the following data:
		// - layer count (2)
		//   per layer:
		//   - top, left, bottom, right (16)
		//   - channel count (2)
		//     per channel
		//     - channel ID and size (6)
		//   - blend mode signature (4)
		//   - blend mode key (4)
		//   - opacity, clipping, flags, filler (4)
		//   - extra data (variable)
		//     - length (4)
		//     - layer mask data length (4)
		//     - layer blending ranges length (4)
		//     - padded name (variable)
		// - all channel data (variable)
		//   - compression (2)
		//   - channel data (variable)

		uint32_t size = 2u;
		for (unsigned int i = 0u; i < document->layerCount; ++i)
		{
			ExportLayer* layer = document->layers + i;
			size += 16u + 2u + GetChannelCount(layer) * 6u + 4u + 4u + 4u + GetExtraDataLength(layer) + 4u;
			size += GetChannelDataSize(layer) + GetChannelCount(layer) * 2u;
		}

		return size;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static uint32_t GetImageResourceSize(void)
	{
		uint32_t size = 0u;
		size += sizeof(uint32_t);			// signature
		size += sizeof(uint16_t);			// resource ID		
		size += 2u;							// padded name, 2 zero bytes
		size += sizeof(uint32_t);			// resource size

		return size;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static uint32_t GetMetaDataResourceSize(ExportDocument* document)
	{
		size_t metaDataSize = sizeof(XMP_HEADER)-1u;
		for (unsigned int i = 0u; i < document->attributeCount; ++i)
		{
			metaDataSize += strlen("<xmp:>");
			metaDataSize += strlen(document->attributes[i].name)*2u;
			metaDataSize += strlen(document->attributes[i].value);
			metaDataSize += strlen("</xmp:>\n");
		}
		metaDataSize += sizeof(XMP_FOOTER)-1u;

		return static_cast<uint32_t>(metaDataSize);
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static uint32_t GetIccProfileResourceSize(ExportDocument* document)
	{
		return document->sizeOfICCProfile;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static uint32_t GetExifDataResourceSize(ExportDocument* document)
	{
		return document->sizeOfExifData;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static uint32_t GetThumbnailResourceSize(ExportDocument* document)
	{
		return document->thumbnail->binaryJpegSize + 28u;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static uint32_t GetDisplayInfoResourceSize(ExportDocument* document)
	{
		// display info consists of 4-byte version, followed by 13 bytes per channel
		return sizeof(uint32_t) + 13u * document->alphaChannelCount;
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static uint32_t GetChannelNamesResourceSize(ExportDocument* document)
	{
		size_t size = 0u;
		for (unsigned int i = 0u; i < document->alphaChannelCount; ++i)
		{
			size += document->alphaChannels[i].asciiName.GetLength() + 1u;
		}

		return static_cast<uint32_t>(size);
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static uint32_t GetUnicodeChannelNamesResourceSize(ExportDocument* document)
	{
		size_t size = 0u;
		for (unsigned int i = 0u; i < document->alphaChannelCount; ++i)
		{
			// unicode strings are null terminated
			size += (document->alphaChannels[i].asciiName.GetLength() + 1u)*2u + 4u;
		}

		return static_cast<uint32_t>(size);
	}


	// ---------------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------------
	static void WriteImageResource(SyncFileWriter& writer, uint16_t id, uint32_t resourceSize)
	{
		const uint32_t signature = util::Key<'8', 'B', 'I', 'M'>::VALUE;
		fileUtil::WriteToFileBE(writer, signature);
		fileUtil::WriteToFileBE(writer, id);

		// padded name, unused
		fileUtil::WriteToFileBE(writer, uint8_t(0u));
		fileUtil::WriteToFileBE(writer, uint8_t(0u));

		fileUtil::WriteToFileBE(writer, resourceSize);
	}
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
ExportDocument* CreateExportDocument(Allocator* allocator, unsigned int canvasWidth, unsigned int canvasHeight, unsigned int bitsPerChannel, exportColorMode::Enum colorMode)
{
	ExportDocument* document = memoryUtil::Allocate<ExportDocument>(allocator);
	if (!document) return nullptr;
	memset(document, 0, sizeof(ExportDocument));

	document->width = canvasWidth;
	document->height = canvasHeight;
	document->bitsPerChannel = static_cast<uint16_t>(bitsPerChannel);
	document->colorMode = colorMode;

	document->attributeCount = 0u;
	document->layerCount = 0u;

	document->mergedImageData[0] = nullptr;
	document->mergedImageData[1] = nullptr;
	document->mergedImageData[2] = nullptr;

	document->alphaChannelCount = 0u;

	document->iccProfile = nullptr;
	document->sizeOfICCProfile = 0u;

	document->exifData = nullptr;
	document->sizeOfExifData = 0u;

	document->thumbnail = nullptr;

	return document;
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
void DestroyExportDocument(ExportDocument*& document, Allocator* allocator)
{
	PSD_ASSERT_NOT_NULL(document);
	PSD_ASSERT_NOT_NULL(allocator);

	if (document->thumbnail)
	{
		memoryUtil::FreeArray(allocator, document->thumbnail->binaryJpeg);
	}
	memoryUtil::Free(allocator, document->thumbnail);

	memoryUtil::FreeArray(allocator, document->exifData);
	memoryUtil::FreeArray(allocator, document->iccProfile);

	memoryUtil::FreeArray(allocator, document->mergedImageData[0]);
	memoryUtil::FreeArray(allocator, document->mergedImageData[1]);
	memoryUtil::FreeArray(allocator, document->mergedImageData[2]);

	for (unsigned int i = 0u; i < document->alphaChannelCount; ++i)
	{
		memoryUtil::FreeArray(allocator, document->alphaChannelData[i]);
	}

	for (unsigned int i = 0u; i < document->attributeCount; ++i)
	{
		DestroyString(allocator, document->attributes[i].name);
		DestroyString(allocator, document->attributes[i].value);
	}

	for (unsigned int i = 0u; i < document->layerCount; ++i)
	{
		DestroyString(allocator, document->layers[i].name);
		memoryUtil::FreeArray(allocator, document->layers[i].utf16Name);
		document->layers[i].utf16Name = nullptr;

		for (unsigned int j = 0u; j < ExportLayer::MAX_CHANNEL_COUNT; ++j)
		{
			const uint16_t compression = document->layers[i].channelCompression[j];
			void*& data = document->layers[i].channelData[j];
			if ((compression == compressionType::ZIP) ||
				(compression == compressionType::ZIP_WITH_PREDICTION))
			{
				// data was allocated by miniz
				free(data);
			}
			else
			{
				memoryUtil::FreeArray(allocator, data);
			}
			data = nullptr;
		}
	}

	memoryUtil::Free(allocator, document);
	document = nullptr;
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
unsigned int AddMetaData(ExportDocument* document, Allocator* allocator, const char* name, const char* value)
{
	const unsigned int index = document->attributeCount;
	++document->attributeCount;

	UpdateMetaData(document, allocator, index, name, value);

	return index;
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
void UpdateMetaData(ExportDocument* document, Allocator* allocator, unsigned int index, const char* name, const char* value)
{
	ExportMetaDataAttribute* attribute = document->attributes + index;
	DestroyString(allocator, attribute->name);
	DestroyString(allocator, attribute->value);
	attribute->name = CreateString(allocator, name);
	attribute->value = CreateString(allocator, value);
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
void SetICCProfile(ExportDocument* document, Allocator* allocator, void* rawProfileData, uint32_t size)
{
	memoryUtil::FreeArray(allocator, document->iccProfile);
	document->iccProfile = memoryUtil::AllocateArray<uint8_t>(allocator, size);
	if (!document->iccProfile && size) throw std::bad_alloc();
	document->sizeOfICCProfile = size;

	memcpy(document->iccProfile, rawProfileData, size);
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
void SetEXIFData(ExportDocument* document, Allocator* allocator, void* rawExifData, uint32_t size)
{
	memoryUtil::FreeArray(allocator, document->exifData);
	document->exifData = memoryUtil::AllocateArray<uint8_t>(allocator, size);
	document->sizeOfExifData = size;
	memcpy(document->exifData, rawExifData, size);
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
void SetJpegThumbnail(ExportDocument* document, Allocator* allocator, uint32_t width, uint32_t height, void* rawJpegData, uint32_t size)
{
	if (document->thumbnail)
	{
		memoryUtil::FreeArray(allocator, document->thumbnail->binaryJpeg);
	}
	memoryUtil::Free(allocator, document->thumbnail);

	Thumbnail* thumbnail = memoryUtil::Allocate<Thumbnail>(allocator);
	thumbnail->width = width;
	thumbnail->height = height;
	thumbnail->binaryJpeg = memoryUtil::AllocateArray<uint8_t>(allocator, size);
	thumbnail->binaryJpegSize = size;

	memcpy(thumbnail->binaryJpeg, rawJpegData, size);

	document->thumbnail = thumbnail;
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
unsigned int AddLayer(ExportDocument* document, Allocator* allocator, const char* name)
{
	const unsigned int index = document->layerCount;
	++document->layerCount;

	ExportLayer* layer = document->layers + index;
	layer->name = CreateString(allocator, name);
	layer->utf16Name = CreateUTF16String(allocator, name, &layer->utf16NameLength);
	layer->type = layerType::ANY;
	layer->IsEmptyChannel = false;
	layer->opacity = 255u;
	layer->clipping = 0u;
	layer->blendModeKey = util::Key<'n', 'o', 'r', 'm'>::VALUE;
	return index;
}

unsigned int AddLayerGroupStart(ExportDocument* document, Allocator* allocator, const char* name)
{
	const unsigned int index = document->layerCount;
	++document->layerCount;

	ExportLayer* layer = document->layers + index;
	layer->name = CreateString(allocator, name);
	layer->utf16Name = CreateUTF16String(allocator, name, &layer->utf16NameLength);
	layer->type = layerType::SECTION_DIVIDER;
	layer->IsEmptyChannel = true;
	layer->opacity = 255u;
	layer->clipping = 0u;
	layer->blendModeKey = util::Key<'n', 'o', 'r', 'm'>::VALUE;
	return index;
}


unsigned int AddLayerGroupEnd(ExportDocument* document, Allocator* allocator, const char* name)
{
	const unsigned int index = document->layerCount;
	++document->layerCount;

	ExportLayer* layer = document->layers + index;
	layer->name = CreateString(allocator, name);
	layer->utf16Name = CreateUTF16String(allocator, name, &layer->utf16NameLength);
	layer->type = layerType::OPEN_FOLDER;
	layer->IsEmptyChannel = true;
	layer->opacity = 255u;
	layer->clipping = 0u;
	layer->blendModeKey = util::Key<'n', 'o', 'r', 'm'>::VALUE;
	return index;
}



// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
namespace {
struct AllocatedDeleter
{
    Allocator* allocator;
    void operator()(void* data) const { if (data) allocator->Free(data); }
};

template <typename T>
using AllocatedBuffer = std::unique_ptr<T, AllocatedDeleter>;

template <typename T>
AllocatedBuffer<T> MakeBuffer(Allocator* allocator, size_t count)
{
    if (count > std::numeric_limits<size_t>::max() / sizeof(T)) throw std::bad_alloc();
    T* data = memoryUtil::AllocateArray<T>(allocator, count);
    if (!data) throw std::bad_alloc();
    return AllocatedBuffer<T>(data, AllocatedDeleter{allocator});
}

bool CheckExport(const ExportCheckpoint& checkpoint, uint32_t current, uint32_t total)
{
    return !checkpoint || checkpoint(current, total);
}

template <typename T>
bool CreateDataRaw(Allocator* allocator, ExportLayer& layer, unsigned int channelIndex,
                   const T* planarData, uint32_t width, uint32_t height, const ExportCheckpoint& checkpoint)
{
    const size_t size = size_t(width) * height;
    auto data = MakeBuffer<T>(allocator, size);
    for (uint32_t y = 0; y < height; ++y) {
        if (!CheckExport(checkpoint, y, height)) return false;
        for (uint32_t x = 0; x < width; ++x) {
            const size_t index = size_t(y) * width + x;
            data.get()[index] = endianUtil::NativeToBigEndian(planarData[index]);
        }
    }
    if (!CheckExport(checkpoint, height, height)) return false;
    layer.channelData[channelIndex] = data.release();
    layer.channelSize[channelIndex] = static_cast<uint32_t>(size * sizeof(T));
    return true;
}

template <typename T>
bool CreateDataRLE(Allocator* allocator, ExportLayer& layer, unsigned int channelIndex,
                   const T* planarData, uint32_t width, uint32_t height, const ExportCheckpoint& checkpoint)
{
    const size_t capacity = size_t(height) * sizeof(uint16_t) + size_t(width) * height * sizeof(T) * 2u;
    if (capacity > std::numeric_limits<uint32_t>::max()) return false;
    auto data = MakeBuffer<uint8_t>(allocator, capacity);
    auto row = MakeBuffer<uint8_t>(allocator, size_t(width) * sizeof(T) * 2u);
    auto bigEndian = MakeBuffer<T>(allocator, width);
    uint32_t offset = 0;
    for (uint32_t y = 0; y < height; ++y) {
        if (!CheckExport(checkpoint, y, height)) return false;
        for (uint32_t x = 0; x < width; ++x)
            bigEndian.get()[x] = endianUtil::NativeToBigEndian(planarData[size_t(y) * width + x]);
        const unsigned int compressedSize = imageUtil::CompressRle(
            reinterpret_cast<const uint8_t*>(bigEndian.get()), row.get(), width * sizeof(T));
        if (compressedSize > width * sizeof(T) * 2u || compressedSize > std::numeric_limits<uint16_t>::max()) return false;
        const uint16_t rowSize = endianUtil::NativeToBigEndian(static_cast<uint16_t>(compressedSize));
        memcpy(data.get() + size_t(y) * sizeof(uint16_t), &rowSize, sizeof(rowSize));
        memcpy(data.get() + size_t(height) * sizeof(uint16_t) + offset, row.get(), compressedSize);
        offset += compressedSize;
    }
    if (!CheckExport(checkpoint, height, height)) return false;
    layer.channelData[channelIndex] = data.release();
    layer.channelSize[channelIndex] = offset + height * sizeof(uint16_t);
    return true;
}

template <typename T>
bool UpdateLayerImpl(ExportDocument* document, Allocator* allocator, unsigned int layerIndex,
                     exportChannel::Enum channel, int left, int top, int right, int bottom,
                     const T* planarData, compressionType::Enum compression, const ExportCheckpoint& checkpoint)
{
    if (!document || !allocator || layerIndex >= document->layerCount ||
        (channel != exportChannel::GRAY && channel != exportChannel::RED && channel != exportChannel::GREEN &&
         channel != exportChannel::BLUE && channel != exportChannel::ALPHA) ||
        (document->colorMode == exportColorMode::GRAYSCALE && channel != exportChannel::GRAY && channel != exportChannel::ALPHA) ||
        (document->colorMode == exportColorMode::RGB && channel == exportChannel::GRAY) ||
        (compression != compressionType::RAW && compression != compressionType::RLE)) return false;
    ExportLayer& layer = document->layers[layerIndex];
    const int64_t width = int64_t(right) - left, height = int64_t(bottom) - top;
    if (!layer.IsEmptyChannel && (!planarData || width <= 0 || height <= 0 ||
        width > std::numeric_limits<uint32_t>::max() / sizeof(T) ||
        height > std::numeric_limits<uint32_t>::max() / sizeof(T) / width ||
        document->bitsPerChannel != sizeof(T) * 8u)) return false;
    try {
        if (!CheckExport(checkpoint, 0, static_cast<uint32_t>(height > 0 ? height : 0))) return false;
        const unsigned int channelIndex = GetChannelIndex(channel);
        ExportLayer candidate = layer;
        candidate.channelData[channelIndex] = nullptr;
        candidate.channelSize[channelIndex] = 0;
        if (layer.IsEmptyChannel) {
            auto empty = MakeBuffer<T>(allocator, 1);
            *empty = T{};
            candidate.channelData[channelIndex] = empty.release();
        } else if (compression == compressionType::RAW) {
            if (!CreateDataRaw(allocator, candidate, channelIndex, planarData, width, height, checkpoint)) return false;
        } else {
            if (!CreateDataRLE(allocator, candidate, channelIndex, planarData, width, height, checkpoint)) return false;
        }
        AllocatedBuffer<void> replacement(candidate.channelData[channelIndex], AllocatedDeleter{allocator});
        if (document->compactChannelStorage && compression == compressionType::RLE && candidate.channelSize[channelIndex]) {
            auto compact = MakeBuffer<uint8_t>(allocator, candidate.channelSize[channelIndex]);
            memcpy(compact.get(), replacement.get(), candidate.channelSize[channelIndex]);
            replacement.reset(compact.release());
        }
        void* old = layer.channelData[channelIndex];
        if (old) {
            const uint16_t previous = layer.channelCompression[channelIndex];
            if (previous == compressionType::ZIP || previous == compressionType::ZIP_WITH_PREDICTION) free(old);
            else allocator->Free(old);
        }
        layer.top = top; layer.left = left; layer.bottom = bottom; layer.right = right;
        layer.channelCompression[channelIndex] = static_cast<uint16_t>(compression);
        layer.channelData[channelIndex] = replacement.release();
        layer.channelSize[channelIndex] = candidate.channelSize[channelIndex];
        return true;
    } catch (...) {
        return false;
    }
}
}

void UpdateLayer(ExportDocument* document, Allocator* allocator, unsigned int layerIndex, exportChannel::Enum channel, int left, int top, int right, int bottom, const uint8_t* planarData, compressionType::Enum compression)
{
    UpdateLayerImpl(document, allocator, layerIndex, channel, left, top, right, bottom, planarData, compression, {});
}

void UpdateLayer(ExportDocument* document, Allocator* allocator, unsigned int layerIndex, exportChannel::Enum channel, int left, int top, int right, int bottom, const uint16_t* planarData, compressionType::Enum compression)
{
    UpdateLayerImpl(document, allocator, layerIndex, channel, left, top, right, bottom, planarData, compression, {});
}

void UpdateLayer(ExportDocument* document, Allocator* allocator, unsigned int layerIndex, exportChannel::Enum channel, int left, int top, int right, int bottom, const float32_t* planarData, compressionType::Enum compression)
{
    UpdateLayerImpl(document, allocator, layerIndex, channel, left, top, right, bottom, planarData, compression, {});
}

bool UpdateLayer(ExportDocument* document, Allocator* allocator, unsigned int layerIndex, exportChannel::Enum channel, int left, int top, int right, int bottom, const uint8_t* planarData, compressionType::Enum compression, const ExportCheckpoint& checkpoint)
{
    return UpdateLayerImpl(document, allocator, layerIndex, channel, left, top, right, bottom, planarData, compression, checkpoint);
}

bool UpdateLayer(ExportDocument* document, Allocator* allocator, unsigned int layerIndex, exportChannel::Enum channel, int left, int top, int right, int bottom, const uint16_t* planarData, compressionType::Enum compression, const ExportCheckpoint& checkpoint)
{
    return UpdateLayerImpl(document, allocator, layerIndex, channel, left, top, right, bottom, planarData, compression, checkpoint);
}

bool UpdateLayer(ExportDocument* document, Allocator* allocator, unsigned int layerIndex, exportChannel::Enum channel, int left, int top, int right, int bottom, const float32_t* planarData, compressionType::Enum compression, const ExportCheckpoint& checkpoint)
{
    return UpdateLayerImpl(document, allocator, layerIndex, channel, left, top, right, bottom, planarData, compression, checkpoint);
}

void UpdateLayerGroup(ExportDocument* document, Allocator* allocator, unsigned int layerIndex, exportChannel::Enum channel, int left, int top, int right, int bottom, const uint8_t* planarData, compressionType::Enum compression)
{
    UpdateLayerImpl(document, allocator, layerIndex, channel, left, top, right, bottom, planarData, compression, {});
}

bool UpdateLayerGroup(ExportDocument* document, Allocator* allocator, unsigned int layerIndex, exportChannel::Enum channel, int left, int top, int right, int bottom, const uint8_t* planarData, compressionType::Enum compression, const ExportCheckpoint& checkpoint)
{
    return UpdateLayerImpl(document, allocator, layerIndex, channel, left, top, right, bottom, planarData, compression, checkpoint);
}

// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
unsigned int AddAlphaChannel(ExportDocument* document, Allocator* allocator, const char* name, uint16_t r, uint16_t g, uint16_t b, uint16_t a, uint16_t opacity, AlphaChannel::Mode::Enum mode)
{
	PSD_UNUSED(allocator);

	const unsigned int index = document->alphaChannelCount;
	++document->alphaChannelCount;

	AlphaChannel* channel = document->alphaChannels + index;
	channel->asciiName.Assign(name);
	channel->colorSpace = 0u;
	channel->color[0] = r;
	channel->color[1] = g;
	channel->color[2] = b;
	channel->color[3] = a;
	channel->opacity = opacity;
	channel->mode = static_cast<uint8_t>(mode);

	return index;
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
template <typename T>
void UpdateChannelImpl(ExportDocument* document, Allocator* allocator, unsigned int channelIndex, const T* data)
{
    if (!document || !allocator || !data || channelIndex >= document->alphaChannelCount ||
        channelIndex >= ExportDocument::MAX_ALPHA_CHANNEL_COUNT || document->bitsPerChannel != sizeof(T) * 8u ||
        !document->width || !document->height ||
        document->height > std::numeric_limits<uint32_t>::max() / sizeof(T) / document->width)
        throw std::invalid_argument("Invalid alpha channel");
    const uint32_t size = document->width * document->height;
    auto channelData = MakeBuffer<T>(allocator, size);
    for (uint32_t i = 0u; i < size; ++i) channelData.get()[i] = endianUtil::NativeToBigEndian(data[i]);
    memoryUtil::FreeArray(allocator, document->alphaChannelData[channelIndex]);
    document->alphaChannelData[channelIndex] = channelData.release();
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
void UpdateChannel(ExportDocument* document, Allocator* allocator, unsigned int channelIndex, const uint8_t* data)
{
	UpdateChannelImpl(document, allocator, channelIndex, data);
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
void UpdateChannel(ExportDocument* document, Allocator* allocator, unsigned int channelIndex, const uint16_t* data)
{
	UpdateChannelImpl(document, allocator, channelIndex, data);
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
void UpdateChannel(ExportDocument* document, Allocator* allocator, unsigned int channelIndex, const float32_t* data)
{
	UpdateChannelImpl(document, allocator, channelIndex, data);
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
template <typename T>
void UpdateMergedImageImpl(ExportDocument* document, Allocator* allocator, const T* planarDataR, const T* planarDataG, const T* planarDataB)
{
    if (!document || !allocator || !planarDataR || !planarDataG || !planarDataB ||
        document->bitsPerChannel != sizeof(T) * 8u || !document->width || !document->height ||
        document->height > std::numeric_limits<uint32_t>::max() / sizeof(T) / document->width)
        throw std::invalid_argument("Invalid merged image");
    const uint32_t size = document->width * document->height;
    auto memoryR = MakeBuffer<T>(allocator, size);
    auto memoryG = MakeBuffer<T>(allocator, size);
    auto memoryB = MakeBuffer<T>(allocator, size);
    for (uint32_t i = 0u; i < size; ++i) {
        memoryR.get()[i] = endianUtil::NativeToBigEndian(planarDataR[i]);
        memoryG.get()[i] = endianUtil::NativeToBigEndian(planarDataG[i]);
        memoryB.get()[i] = endianUtil::NativeToBigEndian(planarDataB[i]);
    }
    for (void* plane : document->mergedImageData) memoryUtil::FreeArray(allocator, plane);
    document->mergedImageData[0] = memoryR.release();
    document->mergedImageData[1] = memoryG.release();
    document->mergedImageData[2] = memoryB.release();
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
void UpdateMergedImage(ExportDocument* document, Allocator* allocator, const uint8_t* planarDataR, const uint8_t* planarDataG, const uint8_t* planarDataB)
{
	UpdateMergedImageImpl(document, allocator, planarDataR, planarDataG, planarDataB);
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
void UpdateMergedImage(ExportDocument* document, Allocator* allocator, const uint16_t* planarDataR, const uint16_t* planarDataG, const uint16_t* planarDataB)
{
	UpdateMergedImageImpl(document, allocator, planarDataR, planarDataG, planarDataB);
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
void UpdateMergedImage(ExportDocument* document, Allocator* allocator, const float32_t* planarDataR, const float32_t* planarDataG, const float32_t* planarDataB)
{
	UpdateMergedImageImpl(document, allocator, planarDataR, planarDataG, planarDataB);
}


// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
bool WriteDocument(ExportDocument* document, Allocator* allocator, File* file)
{
    return WriteDocument(document, allocator, file, {});
}

bool WriteDocument(ExportDocument* document, Allocator* allocator, File* file, const ExportCheckpoint& checkpoint)
{
    return WriteDocument(document, allocator, file, checkpoint, {});
}

bool WriteDocument(ExportDocument* document, Allocator* allocator, File* file,
                   const ExportCheckpoint& checkpoint, const MergedImageProvider& mergedImage)
{
    const MergedImageProvider provider = mergedImage;
    const ExportCheckpoint check = checkpoint;
    if (!document || !allocator || !file || document->layerCount > ExportDocument::MAX_LAYER_COUNT ||
        document->alphaChannelCount > ExportDocument::MAX_ALPHA_CHANNEL_COUNT ||
        (document->colorMode != exportColorMode::RGB && document->colorMode != exportColorMode::GRAYSCALE) ||
        (document->bitsPerChannel != 8u && document->bitsPerChannel != 16u && document->bitsPerChannel != 32u) ||
        !document->width || !document->height ||
        document->height > std::numeric_limits<uint32_t>::max() / (document->bitsPerChannel / 8u) / document->width ||
        (provider && (document->bitsPerChannel != 8u || document->colorMode != exportColorMode::RGB || document->omitMergedPreview)) ||
        !CheckExport(check, 0, document->layerCount)) return false;
    const bool transparency = bool(provider);
    const bool hasMergedData = !document->omitMergedPreview && (transparency ||
        (document->mergedImageData[0] && (document->colorMode == exportColorMode::GRAYSCALE ||
         (document->mergedImageData[1] && document->mergedImageData[2]))));
    const uint16_t documentChannelCount = static_cast<uint16_t>(document->colorMode + document->alphaChannelCount + unsigned(transparency));
    if (documentChannelCount > 56u) return false;
	SyncFileWriter writer(file);

	// signature
	fileUtil::WriteToFileBE(writer, util::Key<'8', 'B', 'P', 'S'>::VALUE);

	// version
	fileUtil::WriteToFileBE(writer, static_cast<uint16_t>(1u));

	// reserved bytes
	const uint8_t zeroes[6] = { 0u, 0u, 0u, 0u, 0u, 0u };
	fileUtil::WriteToFile(writer, zeroes);

	// channel count
	fileUtil::WriteToFileBE(writer, documentChannelCount);

	// header
	const uint16_t mode = static_cast<uint16_t>(document->colorMode);
	fileUtil::WriteToFileBE(writer, document->height);
	fileUtil::WriteToFileBE(writer, document->width);
	fileUtil::WriteToFileBE(writer, document->bitsPerChannel);
	fileUtil::WriteToFileBE(writer, mode);

	if (document->bitsPerChannel == 32u)
	{
		// in 32-bit mode, Photoshop insists on having a color mode data section with magic info.
		// this whole section is undocumented. there's no information to be found on the web.
		// we write Photoshop's default values.
		const uint32_t colorModeSectionLength = 112u;
		fileUtil::WriteToFileBE(writer, colorModeSectionLength);
		{
			// tests suggest that this is some kind of HDR toning information
			const uint32_t key = util::Key<'h', 'd', 'r', 't'>::VALUE;
			fileUtil::WriteToFileBE(writer, key);

			fileUtil::WriteToFileBE(writer, static_cast<uint32_t>(3u));			// ?
			fileUtil::WriteToFileBE(writer, static_cast<float32_t>(0.23f));		// ?
			fileUtil::WriteToFileBE(writer, static_cast<uint32_t>(2u));			// ?

			fileUtil::WriteToFileBE(writer, static_cast<uint32_t>(8u));			// length of the following Unicode string
			fileUtil::WriteToFileBE(writer, static_cast<uint16_t>('D'));
			fileUtil::WriteToFileBE(writer, static_cast<uint16_t>('e'));
			fileUtil::WriteToFileBE(writer, static_cast<uint16_t>('f'));
			fileUtil::WriteToFileBE(writer, static_cast<uint16_t>('a'));
			fileUtil::WriteToFileBE(writer, static_cast<uint16_t>('u'));
			fileUtil::WriteToFileBE(writer, static_cast<uint16_t>('l'));
			fileUtil::WriteToFileBE(writer, static_cast<uint16_t>('t'));
			fileUtil::WriteToFileBE(writer, static_cast<uint16_t>('\0'));

			fileUtil::WriteToFileBE(writer, static_cast<uint16_t>(2u));			// ?
			fileUtil::WriteToFileBE(writer, static_cast<uint16_t>(2u));			// ?
			fileUtil::WriteToFileBE(writer, static_cast<uint16_t>(0u));			// ?
			fileUtil::WriteToFileBE(writer, static_cast<uint16_t>(0u));			// ?
			fileUtil::WriteToFileBE(writer, static_cast<uint16_t>(255u));		// ?
			fileUtil::WriteToFileBE(writer, static_cast<uint16_t>(255u));		// ?

			fileUtil::WriteToFileBE(writer, static_cast<uint8_t>(1u));			// ?
			fileUtil::WriteToFileBE(writer, static_cast<uint8_t>(1u));			// ?
			fileUtil::WriteToFileBE(writer, static_cast<uint32_t>(0u));			// ?
			fileUtil::WriteToFileBE(writer, static_cast<uint32_t>(0u));			// ?

			fileUtil::WriteToFileBE(writer, static_cast<float32_t>(16.0f));		// ?
			fileUtil::WriteToFileBE(writer, static_cast<uint32_t>(1u));			// ?
			fileUtil::WriteToFileBE(writer, static_cast<uint32_t>(1u));			// ?
			fileUtil::WriteToFileBE(writer, static_cast<float32_t>(1.0f));		// ?
		}
		{
			// HDR alpha information?
			const uint32_t key = util::Key<'h', 'd', 'r', 'a'>::VALUE;
			fileUtil::WriteToFileBE(writer, key);

			fileUtil::WriteToFileBE(writer, static_cast<uint32_t>(6u));			// number of following values
			fileUtil::WriteToFileBE(writer, static_cast<float32_t>(0.0f));		// ?
			fileUtil::WriteToFileBE(writer, static_cast<float32_t>(20.0f));		// ?
			fileUtil::WriteToFileBE(writer, static_cast<float32_t>(30.0f));		// ?
			fileUtil::WriteToFileBE(writer, static_cast<float32_t>(0.0f));		// ?
			fileUtil::WriteToFileBE(writer, static_cast<float32_t>(0.0f));		// ?
			fileUtil::WriteToFileBE(writer, static_cast<float32_t>(1.0f));		// ?

			fileUtil::WriteToFileBE(writer, static_cast<uint32_t>(0u));			// ?
			fileUtil::WriteToFileBE(writer, static_cast<uint16_t>(0u));			// ?
		}
	}
	else
	{
		// empty color mode data section
		fileUtil::WriteToFileBE(writer, static_cast<uint32_t>(0u));
	}

	// image resources
	{
		const bool hasMetaData = (document->attributeCount != 0u);
		const bool hasIccProfile = (document->iccProfile != nullptr);
		const bool hasExifData = (document->exifData != nullptr);
		const bool hasThumbnail = (document->thumbnail != nullptr);
		const bool hasAlphaChannels = (document->alphaChannelCount != 0u);

		// write image resources section with optional XMP meta data, ICC profile, EXIF data, thumbnail, alpha channels
		{
			const uint32_t metaDataSize = hasMetaData ? GetMetaDataResourceSize(document) : 0u;
			const uint32_t iccProfileSize = hasIccProfile ? GetIccProfileResourceSize(document) : 0u;
			const uint32_t exifDataSize = hasExifData ? GetExifDataResourceSize(document) : 0u;
			const uint32_t thumbnailSize = hasThumbnail ? GetThumbnailResourceSize(document) : 0u;
			const uint32_t displayInfoSize = hasAlphaChannels ? GetDisplayInfoResourceSize(document) : 0u;
			const uint32_t channelNamesSize = hasAlphaChannels ? GetChannelNamesResourceSize(document) + (transparency ? 13u : 0u) : 0u;
			const uint32_t unicodeChannelNamesSize = hasAlphaChannels ? GetUnicodeChannelNamesResourceSize(document) + (transparency ? 30u : 0u) : 0u;

			uint32_t sectionLength = GetImageResourceSize() + 18u;
            if (transparency) sectionLength += GetImageResourceSize() + 4u * (document->alphaChannelCount + 1u);
			sectionLength += hasMetaData ? bitUtil::RoundUpToMultiple(GetImageResourceSize() + metaDataSize, 2u) : 0u;
			sectionLength += hasIccProfile ? bitUtil::RoundUpToMultiple(GetImageResourceSize() + iccProfileSize, 2u) : 0u;
			sectionLength += hasExifData ? bitUtil::RoundUpToMultiple(GetImageResourceSize() + exifDataSize, 2u) : 0u;
			sectionLength += hasThumbnail ? bitUtil::RoundUpToMultiple(GetImageResourceSize() + thumbnailSize, 2u) : 0u;
			sectionLength += hasAlphaChannels ? bitUtil::RoundUpToMultiple(GetImageResourceSize() + displayInfoSize, 2u) : 0u;
			sectionLength += hasAlphaChannels ? bitUtil::RoundUpToMultiple(GetImageResourceSize() + channelNamesSize, 2u) : 0u;
			sectionLength += hasAlphaChannels ? bitUtil::RoundUpToMultiple(GetImageResourceSize() + unicodeChannelNamesSize, 2u) : 0u;

			// image resource section starts with length of the whole section
			fileUtil::WriteToFileBE(writer, sectionLength);
            {
                WriteImageResource(writer, 1057u, 17u);
                fileUtil::WriteToFileBE(writer, uint32_t(1u));
                fileUtil::WriteToFileBE(writer, uint8_t(hasMergedData));
                fileUtil::WriteToFileBE(writer, uint32_t(0u));
                fileUtil::WriteToFileBE(writer, uint32_t(0u));
                fileUtil::WriteToFileBE(writer, uint32_t(1u));
                fileUtil::WriteToFileBE(writer, uint8_t(0u));
            }
            if (transparency) {
                WriteImageResource(writer, 1053u, 4u * (document->alphaChannelCount + 1u));
                for (uint32_t channel = 0; channel <= document->alphaChannelCount; ++channel)
                    fileUtil::WriteToFileBE(writer, channel);
            }

			if (hasMetaData)
			{
				WriteImageResource(writer, imageResource::XMP_METADATA, metaDataSize);

				const uint64_t start = writer.GetPosition();
				{
					writer.Write(XMP_HEADER, sizeof(XMP_HEADER)-1u);
					for (unsigned int i = 0u; i < document->attributeCount; ++i)
					{
						writer.Write("<xmp:", 5u);
						writer.Write(document->attributes[i].name, static_cast<uint32_t>(strlen(document->attributes[i].name)));
						writer.Write(">", 1u);
						writer.Write(document->attributes[i].value, static_cast<uint32_t>(strlen(document->attributes[i].value)));
						writer.Write("</xmp:", 6u);
						writer.Write(document->attributes[i].name, static_cast<uint32_t>(strlen(document->attributes[i].name)));
						writer.Write(">\n", 2u);
					}
					writer.Write(XMP_FOOTER, sizeof(XMP_FOOTER)-1u);
				}
				const uint64_t bytesWritten = writer.GetPosition() - start;
				if (bytesWritten & 1ull)
				{
					// write padding byte
					fileUtil::WriteToFileBE(writer, static_cast<uint8_t>(0u));
				}
			}

			if (hasIccProfile)
			{
				WriteImageResource(writer, imageResource::ICC_PROFILE, iccProfileSize);

				const uint64_t start = writer.GetPosition();
				{
					writer.Write(document->iccProfile, document->sizeOfICCProfile);
				}
				const uint64_t bytesWritten = writer.GetPosition() - start;
				if (bytesWritten & 1ull)
				{
					// write padding byte
					fileUtil::WriteToFileBE(writer, static_cast<uint8_t>(0u));
				}
			}

			if (hasExifData)
			{
				WriteImageResource(writer, imageResource::EXIF_DATA, exifDataSize);

				const uint64_t start = writer.GetPosition();
				{
					writer.Write(document->exifData, document->sizeOfExifData);
				}
				const uint64_t bytesWritten = writer.GetPosition() - start;
				if (bytesWritten & 1ull)
				{
					// write padding byte
					fileUtil::WriteToFileBE(writer, static_cast<uint8_t>(0u));
				}
			}

			if (hasThumbnail)
			{
				WriteImageResource(writer, imageResource::THUMBNAIL_RESOURCE, thumbnailSize);

				const uint64_t start = writer.GetPosition();
				{
					const uint32_t format = 1u;				// format = kJpegRGB
					const uint16_t bitsPerPixel = 24u;
					const uint16_t planeCount = 1u;
					const uint32_t widthInBytes = (document->thumbnail->width * bitsPerPixel + 31u) / 32u * 4u;
					const uint32_t totalSize = widthInBytes * document->thumbnail->height * planeCount;

					fileUtil::WriteToFileBE(writer, format);
					fileUtil::WriteToFileBE(writer, document->thumbnail->width);
					fileUtil::WriteToFileBE(writer, document->thumbnail->height);
					fileUtil::WriteToFileBE(writer, widthInBytes);
					fileUtil::WriteToFileBE(writer, totalSize);
					fileUtil::WriteToFileBE(writer, document->thumbnail->binaryJpegSize);
					fileUtil::WriteToFileBE(writer, bitsPerPixel);
					fileUtil::WriteToFileBE(writer, planeCount);

					writer.Write(document->thumbnail->binaryJpeg, document->thumbnail->binaryJpegSize);
				}
				const uint64_t bytesWritten = writer.GetPosition() - start;
				if (bytesWritten & 1ull)
				{
					// write padding byte
					fileUtil::WriteToFileBE(writer, static_cast<uint8_t>(0u));
				}
			}

			if (hasAlphaChannels)
			{
				// write display info
				{
					WriteImageResource(writer, imageResource::DISPLAY_INFO, displayInfoSize);

					const uint64_t start = writer.GetPosition();

					// version
					fileUtil::WriteToFileBE(writer, static_cast<uint32_t>(1u));

					// per channel data
					for (unsigned int i=0u; i < document->alphaChannelCount; ++i)
					{
						AlphaChannel* channel = document->alphaChannels + i;
						fileUtil::WriteToFileBE(writer, channel->colorSpace);
						fileUtil::WriteToFileBE(writer, channel->color[0]);
						fileUtil::WriteToFileBE(writer, channel->color[1]);
						fileUtil::WriteToFileBE(writer, channel->color[2]);
						fileUtil::WriteToFileBE(writer, channel->color[3]);
						fileUtil::WriteToFileBE(writer, channel->opacity);
						fileUtil::WriteToFileBE(writer, channel->mode);
					}

					const uint64_t bytesWritten = writer.GetPosition() - start;
					if (bytesWritten & 1ull)
					{
						// write padding byte
						fileUtil::WriteToFileBE(writer, static_cast<uint8_t>(0u));
					}
				}

				// write channel names
				{
					WriteImageResource(writer, imageResource::ALPHA_CHANNEL_ASCII_NAMES, channelNamesSize);

					const uint64_t start = writer.GetPosition();
                    if (transparency) {
                        fileUtil::WriteToFileBE(writer, uint8_t(12u));
                        writer.Write("Transparency", 12u);
                    }

					for (unsigned int i = 0u; i < document->alphaChannelCount; ++i)
					{
						fileUtil::WriteToFileBE(writer, static_cast<uint8_t>(document->alphaChannels[i].asciiName.GetLength()));
						writer.Write(document->alphaChannels[i].asciiName.c_str(), static_cast<uint32_t>(document->alphaChannels[i].asciiName.GetLength()));
					}

					const uint64_t bytesWritten = writer.GetPosition() - start;
					if (bytesWritten & 1ull)
					{
						// write padding byte
						fileUtil::WriteToFileBE(writer, static_cast<uint8_t>(0u));
					}
				}

				// write unicode channel names
				{
					WriteImageResource(writer, imageResource::ALPHA_CHANNEL_UNICODE_NAMES, unicodeChannelNamesSize);

					const uint64_t start = writer.GetPosition();
                    if (transparency) {
                        fileUtil::WriteToFileBE(writer, uint32_t(13u));
                        for (char character : "Transparency") fileUtil::WriteToFileBE(writer, uint16_t(character));
                    }

					for (unsigned int i = 0u; i < document->alphaChannelCount; ++i)
					{
						// PSD expects UTF-16 strings, followed by a null terminator
						const size_t length = document->alphaChannels[i].asciiName.GetLength();
						fileUtil::WriteToFileBE(writer, static_cast<uint32_t>(length + 1u));

						const char* asciiStr = document->alphaChannels[i].asciiName.c_str();
						for (size_t j = 0u; j < length; ++j)
						{
							const uint16_t unicodeGlyph = asciiStr[j];
							fileUtil::WriteToFileBE(writer, unicodeGlyph);
						}

						fileUtil::WriteToFileBE(writer, uint16_t(0u));
					}

					const uint64_t bytesWritten = writer.GetPosition() - start;
					if (bytesWritten & 1ull)
					{
						// write padding byte
						fileUtil::WriteToFileBE(writer, static_cast<uint8_t>(0u));
					}
				}
			}
		}
	}

	// layer mask section
	uint32_t layerInfoSectionLength = GetLayerInfoSectionLength(document);

	// layer info section must be padded to a multiple of 4
	const unsigned int paddingNeeded = bitUtil::RoundUpToMultiple(layerInfoSectionLength, 4u) - layerInfoSectionLength;
	layerInfoSectionLength += paddingNeeded;

	const bool is8BitData = (document->bitsPerChannel == 8u);
	if (is8BitData)
	{
		// 8-bit data
		// layer mask section length also includes global layer mask info marker. layer info follows directly after that
		const uint32_t layerMaskSectionLength = layerInfoSectionLength + 8u + (transparency ? 12u : 0u);
		fileUtil::WriteToFileBE(writer, layerMaskSectionLength);
	}
	else
	{
		// 16-bit and 32-bit layer data is stored in Additional Layer Information, so we leave the following layer info section empty
		const uint32_t layerMaskSectionLength = layerInfoSectionLength + 4u * 5u;
		fileUtil::WriteToFileBE(writer, layerMaskSectionLength);

		// empty layer info section
		fileUtil::WriteToFileBE(writer, static_cast<uint32_t>(0u));

		// empty global layer mask info
		fileUtil::WriteToFileBE(writer, static_cast<uint32_t>(0u));

		// additional layer information
		const uint32_t signature = util::Key<'8', 'B', 'I', 'M'>::VALUE;
		fileUtil::WriteToFileBE(writer, signature);

		if (document->bitsPerChannel == 16u)
		{
			const uint32_t key = util::Key<'L', 'r', '1', '6'>::VALUE;
			fileUtil::WriteToFileBE(writer, key);
		}
		else if (document->bitsPerChannel == 32u)
		{
			const uint32_t key = util::Key<'L', 'r', '3', '2'>::VALUE;
			fileUtil::WriteToFileBE(writer, key);
		}
	}

	fileUtil::WriteToFileBE(writer, layerInfoSectionLength);

	// layer count
	fileUtil::WriteToFileBE(writer, static_cast<int16_t>(transparency ? -int(document->layerCount) : int(document->layerCount)));

	// per-layer info
	for (unsigned int i = 0u; i < document->layerCount; ++i)
	{
        if (!CheckExport(check, i, document->layerCount) || writer.HasError()) return false;
		ExportLayer* layer = document->layers + i;
		
		fileUtil::WriteToFileBE(writer, layer->top);
		fileUtil::WriteToFileBE(writer, layer->left);
		fileUtil::WriteToFileBE(writer, layer->bottom);
		fileUtil::WriteToFileBE(writer, layer->right);

		const uint16_t channelCount = GetChannelCount(layer);
		fileUtil::WriteToFileBE(writer, channelCount);

		// per-channel info
		for (unsigned int j = 0u; j < ExportLayer::MAX_CHANNEL_COUNT; ++j)
		{
			if (layer->channelData[j])
			{
				const int16_t channelId = GetChannelId(j);
				fileUtil::WriteToFileBE(writer, channelId);

				// channel data always has a 2-byte compression type in front of the data
				const uint32_t channelDataSize = layer->channelSize[j] + 2u;
				fileUtil::WriteToFileBE(writer, channelDataSize);
			}
		}

		// blend mode signature
		fileUtil::WriteToFileBE(writer, util::Key<'8', 'B', 'I', 'M'>::VALUE);

		// blend mode data (per-layer values)
		const uint8_t flags = layer->flags;
		const uint8_t filler = 0u;
		fileUtil::WriteToFileBE(writer, layer->blendModeKey);
		fileUtil::WriteToFileBE(writer, layer->opacity);
		fileUtil::WriteToFileBE(writer, layer->clipping);
		fileUtil::WriteToFileBE(writer, flags);
		fileUtil::WriteToFileBE(writer, filler);

		// extra data, including layer name
		const uint32_t extraDataLength = GetExtraDataLength(layer);
		fileUtil::WriteToFileBE(writer, extraDataLength);

		const uint32_t layerMaskDataLength = 0u;
		fileUtil::WriteToFileBE(writer, layerMaskDataLength);

		const uint32_t layerBlendingRangesDataLength = 0u;
		fileUtil::WriteToFileBE(writer, layerBlendingRangesDataLength);

		// the layer name is stored as pascal string, padded to a multiple of 4
		const uint8_t nameLength = static_cast<uint8_t>(strlen(layer->name));
		const uint32_t paddedNameLength = bitUtil::RoundUpToMultiple(nameLength + 1u, 4u);
		fileUtil::WriteToFileBE(writer, nameLength);
		writer.Write(layer->name, paddedNameLength - 1u);

		fileUtil::WriteToFileBE(writer, util::Key<'8', 'B', 'I', 'M'>::VALUE);
		fileUtil::WriteToFileBE(writer, util::Key<'l', 's', 'c', 't'>::VALUE);
		const uint32_t lengthExt = layer->hasSectionBlendMode ? 12u : 4u;
		fileUtil::WriteToFileBE(writer, lengthExt);
		fileUtil::WriteToFileBE(writer, layer->type);
        if (layer->hasSectionBlendMode) {
            fileUtil::WriteToFileBE(writer, util::Key<'8', 'B', 'I', 'M'>::VALUE);
            fileUtil::WriteToFileBE(writer, layer->blendModeKey);
        }
        if (layer->hasLayerId) {
            fileUtil::WriteToFileBE(writer, util::Key<'8', 'B', 'I', 'M'>::VALUE);
            fileUtil::WriteToFileBE(writer, util::Key<'l', 'y', 'i', 'd'>::VALUE);
            fileUtil::WriteToFileBE(writer, uint32_t(4u));
            fileUtil::WriteToFileBE(writer, layer->layerId);
        }

		// 'luni' Unicode layer name: readers prefer it over the pascal name, fixing non-ASCII names
		if (layer->utf16Name)
		{
			fileUtil::WriteToFileBE(writer, util::Key<'8', 'B', 'I', 'M'>::VALUE);
			fileUtil::WriteToFileBE(writer, util::Key<'l', 'u', 'n', 'i'>::VALUE);

			const uint32_t dataLength = 4u + layer->utf16NameLength * 2u;
			const uint32_t paddedDataLength = bitUtil::RoundUpToMultiple(dataLength, 4u);
			fileUtil::WriteToFileBE(writer, paddedDataLength);

			fileUtil::WriteToFileBE(writer, layer->utf16NameLength);
			for (uint32_t c = 0u; c < layer->utf16NameLength; ++c)
			{
				fileUtil::WriteToFileBE(writer, layer->utf16Name[c]);
			}
			for (uint32_t p = dataLength; p < paddedDataLength; ++p)
			{
				const uint8_t zero = 0u;
				fileUtil::WriteToFileBE(writer, zero);
			}
		}
	}

	// per-layer data
	for (unsigned int i = 0u; i < document->layerCount; ++i)
	{
        if (!CheckExport(check, i, document->layerCount) || writer.HasError()) return false;
		ExportLayer* layer = document->layers + i;

		// per-channel data
		for (unsigned int j = 0u; j < ExportLayer::MAX_CHANNEL_COUNT; ++j)
		{
			if (layer->channelData[j])
			{
				fileUtil::WriteToFileBE(writer, layer->channelCompression[j]);
				writer.Write(layer->channelData[j], layer->channelSize[j]);
			}
		}
	}

	// add padding to align layer info section to multiple of 4
	if (paddingNeeded != 0u)
	{
		writer.Write(zeroes, paddingNeeded);
	}

	// global layer mask info
	if (is8BitData)
	{
		const uint32_t globalLayerMaskInfoLength = 0u;
		fileUtil::WriteToFileBE(writer, globalLayerMaskInfoLength);
	}

    if (transparency) {
        fileUtil::WriteToFileBE(writer, util::Key<'8', 'B', 'I', 'M'>::VALUE);
        fileUtil::WriteToFileBE(writer, util::Key<'M', 't', 'r', 'n'>::VALUE);
        fileUtil::WriteToFileBE(writer, uint32_t(0u));
    }
	if (writer.HasError()) return false;

    const uint32_t planeBytes = document->width * document->height * (document->bitsPerChannel / 8u);
    fileUtil::WriteToFileBE(writer, static_cast<uint16_t>(compressionType::RAW));
    if (writer.HasError()) return false;
    if (provider) {
        const uint32_t width = document->width, height = document->height;
        const uint64_t begin = writer.GetPosition();
        if (begin > std::numeric_limits<uint64_t>::max() - uint64_t(planeBytes) * 4u) return false;
        struct StreamState {
            SyncFileWriter* writer = nullptr;
            uint32_t nextRow = 0;
            bool failed = false;
            bool writing = false;
        };
        auto state = std::make_shared<StreamState>();
        state->writer = &writer;
        const MergedImageSink sink = [state, width, height, begin, planeBytes, check](uint32_t firstRow,
            uint32_t rowCount, const uint8_t* red, const uint8_t* green, const uint8_t* blue, const uint8_t* alpha) {
            if (!state->writer || state->failed) return false;
            if (state->writing || firstRow != state->nextRow || !rowCount || rowCount > height - state->nextRow ||
                !red || !green || !blue || !alpha) {
                state->failed = true;
                return false;
            }
            state->writing = true;
            const uint32_t bytes = width * rowCount;
            const uint8_t* planes[] = {red, green, blue, alpha};
            for (uint32_t plane = 0; plane < 4 && !state->failed; ++plane) {
                for (uint32_t offset = 0; offset < bytes;) {
                    if (!CheckExport(check, firstRow, height) || state->failed) {
                        state->failed = true;
                        break;
                    }
                    const uint32_t count = std::min(uint32_t(1024 * 1024), bytes - offset);
                    state->writer->WriteAt(planes[plane] + offset, count,
                        begin + uint64_t(planeBytes) * plane + uint64_t(firstRow) * width + offset);
                    if (state->writer->HasError()) { state->failed = true; break; }
                    offset += count;
                }
            }
            if (!state->failed) state->nextRow += rowCount;
            state->writing = false;
            return !state->failed;
        };
        bool complete = false;
        try { complete = provider(sink); }
        catch (...) { state->writer = nullptr; throw; }
        state->writer = nullptr;
        if (!complete || state->failed || state->nextRow != height ||
            writer.GetPosition() != begin + uint64_t(planeBytes) * 4u) return false;
    } else {
        std::array<uint8_t, 65536> empty{};
        const unsigned count = document->colorMode == exportColorMode::GRAYSCALE ? 1u : 3u;
        for (unsigned plane = 0; plane < count; ++plane) {
            const auto* data = static_cast<const uint8_t*>(document->mergedImageData[plane]);
            for (uint32_t offset = 0; offset < planeBytes;) {
                if (!CheckExport(check, offset, planeBytes)) return false;
                const uint32_t bytes = std::min(uint32_t(empty.size()), planeBytes - offset);
                writer.Write(data ? data + offset : empty.data(), bytes);
                if (writer.HasError()) return false;
                offset += bytes;
            }
        }
    }
    for (unsigned plane = 0; plane < document->alphaChannelCount; ++plane) {
        const auto* data = static_cast<const uint8_t*>(document->alphaChannelData[plane]);
        if (!data) return false;
        for (uint32_t offset = 0; offset < planeBytes;) {
            if (!CheckExport(check, offset, planeBytes)) return false;
            const uint32_t bytes = std::min(uint32_t(1024 * 1024), planeBytes - offset);
            writer.Write(data + offset, bytes);
            if (writer.HasError()) return false;
            offset += bytes;
        }
    }

	return !writer.HasError() && CheckExport(check, document->layerCount, document->layerCount);
}

// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
void SetLayerBlendMode(ExportDocument* document, unsigned int layerIndex, uint32_t blendModeKey)
{
	document->layers[layerIndex].blendModeKey = blendModeKey;
}

void SetLayerOpacity(ExportDocument* document, unsigned int layerIndex, uint8_t opacity)
{
	document->layers[layerIndex].opacity = opacity;
}

void SetLayerClipping(ExportDocument* document, unsigned int layerIndex, uint8_t clipping)
{
	document->layers[layerIndex].clipping = clipping;
}

bool SetLayerAttributes(ExportDocument* document, unsigned int layerIndex, const ExportLayerAttributes& attributes)
{
    if (!document || layerIndex >= document->layerCount || attributes.clipping > 1u) return false;
    ExportLayer& layer = document->layers[layerIndex];
    layer.opacity = attributes.opacity;
    layer.clipping = attributes.clipping;
    layer.flags = attributes.visible ? 0u : 2u;
    layer.hasLayerId = attributes.hasLayerId;
    layer.layerId = attributes.layerId;
    layer.blendModeKey = attributes.blendModeKey;
    layer.hasSectionBlendMode = layer.type == layerType::OPEN_FOLDER || layer.type == layerType::CLOSED_FOLDER;
    return true;
}

bool SetEmptyLayerBounds(ExportDocument* document, unsigned int layerIndex, int left, int top, int right, int bottom)
{
    if (!document || layerIndex >= document->layerCount || right < left || bottom < top) return false;
    ExportLayer& layer = document->layers[layerIndex];
    if (GetChannelCount(&layer) != 0u) return false;
    if (layer.type == layerType::ANY && (right != left || bottom != top)) return false;
    layer.left = left;
    layer.top = top;
    layer.right = right;
    layer.bottom = bottom;
    layer.IsEmptyChannel = true;
    return true;
}

void SetSourceDocumentOptions(ExportDocument* document)
{
    if (!document) return;
    document->compactChannelStorage = true;
    document->omitMergedPreview = true;
}

PSD_NAMESPACE_END
