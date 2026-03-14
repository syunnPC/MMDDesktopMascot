#include "WicTexture.hpp"
#include "ExceptionHelper.hpp"
#include "DebugUtil.hpp"
#include <windows.h>
#include <wincodec.h>
#include <winrt/base.h>
#include <limits>
#include <stdexcept>

#pragma comment(lib, "windowscodecs.lib")

using winrt::com_ptr;

namespace WicTexture
{

	WicImage LoadRgba(const std::filesystem::path& path)
	{
		com_ptr<IWICImagingFactory> factory;
		HRESULT hr = CoCreateInstance(
			CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(factory.put())
		);
		DX_CALL(hr);

		com_ptr<IWICBitmapDecoder> decoder;
		hr = factory->CreateDecoderFromFilename(
			path.c_str(), nullptr, GENERIC_READ,
			WICDecodeMetadataCacheOnLoad, decoder.put()
		);
		DX_CALL(hr);

		com_ptr<IWICBitmapFrameDecode> frame;
		hr = decoder->GetFrame(0, frame.put());
		DX_CALL(hr);

		UINT w = 0, h = 0;
		hr = frame->GetSize(&w, &h);
		DX_CALL(hr);
		if (w == 0 || h == 0) throw std::runtime_error("WIC GetSize failed.");
		if (w > (std::numeric_limits<UINT>::max)() / 4u)
		{
			throw std::runtime_error("WIC image width is too large.");
		}

		const uint64_t pixelCount = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
		const uint64_t byteCount64 = pixelCount * 4ull;
		if (byteCount64 > static_cast<uint64_t>((std::numeric_limits<size_t>::max)()) ||
			byteCount64 > static_cast<uint64_t>((std::numeric_limits<UINT>::max)()))
		{
			throw std::runtime_error("WIC image is too large.");
		}

		com_ptr<IWICFormatConverter> converter;
		hr = factory->CreateFormatConverter(converter.put());
		DX_CALL(hr);

		hr = converter->Initialize(
			frame.get(),
			GUID_WICPixelFormat32bppRGBA,
			WICBitmapDitherTypeNone,
			nullptr, 0.0,
			WICBitmapPaletteTypeCustom
		);
		DX_CALL(hr);

		WicImage img;
		img.width = static_cast<uint32_t>(w);
		img.height = static_cast<uint32_t>(h);
		img.rgba.resize(static_cast<size_t>(byteCount64));

		hr = converter->CopyPixels(
			nullptr,
			w * 4u,
			static_cast<UINT>(byteCount64),
			img.rgba.data()
		);
		DX_CALL(hr);

		for (size_t i = 3; i < img.rgba.size(); i += 4)
		{
			const uint8_t alpha = img.rgba[i];
			if (alpha < 255)
			{
				img.hasTransparentPixels = true;
			}
			if (alpha > 0 && alpha < 255)
			{
				img.hasTranslucentPixels = true;
			}
		}

		return img;
	}
}
