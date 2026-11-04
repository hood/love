/**
 * Copyright (c) 2006-2026 LOVE Development Team
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 **/

#include "PNGHandler.h"

// LOVE
#include "common/Exception.h"
#include "common/math.h"

// LodePNG
#include "lodepng/lodepng.h"

// zlib
#include <zlib.h>

// C++
#include <algorithm>

// C
#include <cstdlib>
#include <climits>

namespace love
{
namespace image
{
namespace magpie
{

// Custom PNG decompression function for LodePNG, using zlib.
static unsigned zlibDecompress(unsigned char **out, size_t *outsize, const unsigned char *in,
                               size_t insize, const LodePNGDecompressSettings* /*settings*/)
{
	z_stream stream = {};
	if (inflateInit(&stream) != Z_OK)
		return 83; // "Memory allocation failed" error code for LodePNG.

	// LodePNG uses malloc, realloc, and free.
	// Since version 2014-08-23, LodePNG passes in an existing pointer in
	// the 'out' argument that it expects to be realloc'd. Not doing so can
	// result in a memory leak.
	unsigned char *outdata = out != nullptr ? *out : nullptr;

	// Start with a guess and grow the buffer when it fills up. inflate() picks
	// up where it left off, so no data is decompressed more than once.
	size_t capacity = std::max(insize * 4, (size_t) 1024);
	size_t written = 0;
	size_t consumed = 0;
	unsigned error = 0;

	while (true)
	{
		if (written == capacity)
			capacity *= 2;

		unsigned char *newdata = (unsigned char *) realloc(outdata, capacity);
		if (newdata == nullptr)
		{
			error = 83; // "Memory allocation failed" error code for LodePNG.
			break;
		}
		outdata = newdata;

		stream.next_in = (Bytef *) (in + consumed);
		stream.avail_in = (uInt) std::min(insize - consumed, (size_t) UINT_MAX);
		stream.next_out = outdata + written;
		stream.avail_out = (uInt) std::min(capacity - written, (size_t) UINT_MAX);

		int status = inflate(&stream, Z_NO_FLUSH);

		consumed = (size_t) (stream.next_in - in);
		written = (size_t) (stream.next_out - outdata);

		if (status == Z_STREAM_END)
			break;

		// Z_BUF_ERROR only means no progress was possible. That's fine if the
		// output buffer is full (we grow it), but an error if the input ran out.
		bool needsoutput = written == capacity;
		if ((status != Z_OK && status != Z_BUF_ERROR) || (!needsoutput && consumed == insize))
		{
			error = 10000; // "Unknown error code" for LodePNG.
			break;
		}
	}

	inflateEnd(&stream);

	if (error != 0)
	{
		free(outdata);
		outdata = nullptr;
		written = 0;
	}

	if (out != nullptr)
		*out = outdata;
	else
		free(outdata);

	if (outsize != nullptr)
		*outsize = written;

	return error;
}

// Custom PNG compression function for LodePNG, using zlib.
static unsigned zlibCompress(unsigned char **out, size_t *outsize, const unsigned char *in,
                             size_t insize, const LodePNGCompressSettings* /*settings*/)
{
	// Get the maximum compressed size of the data.
	uLongf outdatasize = compressBound(insize);

	// LodePNG uses malloc, realloc, and free.
	unsigned char *outdata = (unsigned char *) malloc(outdatasize);

	if (!outdata)
		return 83; // "Memory allocation failed" error code for LodePNG.

	// Use zlib to compress the PNG data.
	int status = compress(outdata, &outdatasize, in, insize);

	if (status != Z_OK)
	{
		free(outdata);
		return 10000; // "Unknown error code" for LodePNG.
	}

	if (out != nullptr)
		*out = outdata;

	if (outsize != nullptr)
		*outsize = (size_t) outdatasize;

	return 0; // Success.
}

bool PNGHandler::canDecode(Data *data)
{
	unsigned int width = 0, height = 0;
	unsigned char *indata = (unsigned char *) data->getData();
	size_t insize = data->getSize();

	lodepng::State state;
	unsigned status = lodepng_inspect(&width, &height, &state, indata, insize);

	return status == 0 && width > 0 && height > 0;
}

bool PNGHandler::canEncode(PixelFormat rawFormat, EncodedFormat encodedFormat)
{
	return encodedFormat == ENCODED_PNG
		&& (rawFormat == PIXELFORMAT_RGBA8_UNORM || rawFormat == PIXELFORMAT_RGBA16_UNORM);
}

PNGHandler::DecodedImage PNGHandler::decode(Data *fdata)
{
	unsigned int width = 0, height = 0;
	unsigned char *indata = (unsigned char *) fdata->getData();
	size_t insize = fdata->getSize();

	DecodedImage img;

	lodepng::State state;
	unsigned status = lodepng_inspect(&width, &height, &state, indata, insize);

	if (status != 0)
	{
		const char *err = lodepng_error_text(status);
		throw love::Exception("Could not decode PNG image (%s)", err);
	}

	state.decoder.zlibsettings.custom_zlib = zlibDecompress;
	state.info_raw.colortype = LCT_RGBA;

	if (state.info_png.color.bitdepth == 16)
		state.info_raw.bitdepth = 16;
	else
		state.info_raw.bitdepth = 8;

	status = lodepng_decode(&img.data, &width, &height, &state, indata, insize);

	if (status != 0)
	{
		const char *err = lodepng_error_text(status);
		throw love::Exception("Could not decode PNG image (%s)", err);
	}

	img.width  = (int) width;
	img.height = (int) height;
	img.size   = width * height * (state.info_raw.bitdepth * 4 / 8);
	img.format = state.info_raw.bitdepth == 16 ? PIXELFORMAT_RGBA16_UNORM : PIXELFORMAT_RGBA8_UNORM;

	// LodePNG keeps raw 16 bit images stored as big-endian.
#ifndef LOVE_BIG_ENDIAN
	if (state.info_raw.bitdepth == 16)
	{
		uint16 *pixeldata = (uint16 *) img.data;
		size_t numpixelcomponents = img.size / sizeof(uint16);

		for (size_t i = 0; i < numpixelcomponents; i++)
			pixeldata[i] = swapuint16(pixeldata[i]);
	}
#endif

	return img;
}

FormatHandler::EncodedImage PNGHandler::encode(const DecodedImage &img, EncodedFormat encodedFormat)
{
	if (!canEncode(img.format, encodedFormat))
		throw love::Exception("PNG encoder cannot encode to non-PNG format.");

	EncodedImage encimg;

	lodepng::State state;

	state.info_raw.colortype = LCT_RGBA;
	state.info_raw.bitdepth = img.format == PIXELFORMAT_RGBA16_UNORM ? 16 : 8;

	state.info_png.color.colortype = LCT_RGBA;
	state.info_png.color.bitdepth = state.info_raw.bitdepth;

	state.encoder.zlibsettings.custom_zlib = zlibCompress;

	const uint8 *data = img.data;
	uint16 *swappeddata = nullptr;

	// LodePNG expects big-endian raw pixel data when encoding a 16 bit image.
#ifndef LOVE_BIG_ENDIAN
	if (state.info_raw.bitdepth == 16)
	{
		try
		{
			swappeddata = new uint16[img.size / sizeof(uint16)];
		}
		catch (std::exception &)
		{
			throw love::Exception("Out of memory.");
		}

		const uint16 *rawdata = (const uint16 *) img.data;
		size_t numpixelcomponents = img.size / sizeof(uint16);

		for (size_t i = 0; i < numpixelcomponents; i++)
			swappeddata[i] = swapuint16(rawdata[i]);

		data = (const uint8 *) swappeddata;
	}
#endif

	unsigned status = lodepng_encode(&encimg.data, &encimg.size,
	                                 data, img.width, img.height, &state);

	if (swappeddata != nullptr)
		delete[] swappeddata;

	if (status != 0)
	{
		const char *err = lodepng_error_text(status);
		throw love::Exception("Could not encode PNG image (%s)", err);
	}

	return encimg;
}

void PNGHandler::freeRawPixels(unsigned char *mem)
{
	// LodePNG uses malloc, realloc, and free.
	if (mem)
		::free(mem);
}

void PNGHandler::freeEncodedImage(unsigned char *mem)
{
	if (mem)
		::free(mem);
}

} // magpie
} // image
} // love
