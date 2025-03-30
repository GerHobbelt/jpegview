
#include "StdAfx.h"
#include "ImageLoadThread.h"
#include <gdiplus.h>
#include <GdiplusEnums.h>
#include "JPEGImage.h"
#include "MessageDef.h"
#include "Helpers.h"
#include "SettingsProvider.h"
#include "ReaderBMP.h"
#include "ReaderTGA.h"
#include "BasicProcessing.h"
#include "dcraw_mod.h"
#include "TJPEGWrapper.h"
#include "PNGWrapper.h"
#include "JXLWrapper.h"
#include "HEIFWrapper.h"
#include "RAWWrapper.h"
#include "WEBPWrapper.h"
#include "QOIWrapper.h"
#include "PSDWrapper.h"
#include "MaxImageDef.h"
#include "zip/zip.h"
#include "bit7z/include/bitfileextractor.hpp"
#include "bit7z/include/bitarchivereader.hpp"

using namespace bit7z;
using namespace Gdiplus;

#define EXT_MATCHES(extension) (ext.Compare(_T(extension)) == 0)

// static initializers
volatile int CImageLoadThread::m_curHandle = 0;

/*
 * Inrevocably blends alpha data into RGB pixel, with choice of solid colour or checkerboard pattern backgrounds.
 *
 * bUseCheckerboard: whether to use checkerboard pattern instead of solid colour
 * x, y: location of current pixel
*/
static inline uint32 WebpAlphaBlendBackground(uint32 pixel, uint32 backgroundColor, bool bUseCheckerboard = false, int x = -1, int y = -1)
{
	uint32 alpha = pixel & 0xFF000000;

	if (alpha == 0xFF000000)
	{
		return pixel;
	}
	if (bUseCheckerboard)
	{
		//ignore configured backgroundColor, and use white and light gray for checkerboard
		if (((x & 0x10) && ((y & 0x10) == 0))
			|| ((y & 0x10) && ((x & 0x10) == 0)))
		{
			backgroundColor = 0x00ffffff;
		}
		else
		{
			backgroundColor = 0x00c0c0c0;
		}
	}
	if (alpha == 0) {
		return backgroundColor;
	}
	else {
		uint8 r = GetRValue(pixel);
		uint8 g = GetGValue(pixel);
		uint8 b = GetBValue(pixel);
		uint8 bg_r = GetRValue(backgroundColor);
		uint8 bg_g = GetGValue(backgroundColor);
		uint8 bg_b = GetBValue(backgroundColor);
		uint8 a = alpha >> 24;
		uint8 one_minus_a = 255 - a;

		return
			0xFF000000 +
			((uint8)(((r * a + bg_r * one_minus_a) / 255.0) + 0.5)) +
			((uint8)(((g * a + bg_g * one_minus_a) / 255.0) + 0.5) << 8) +
			((uint8)(((b * a + bg_b * one_minus_a) / 255.0) + 0.5) << 16);
	}
}


class ZipEntry
{
public:
	CString ext;
	unsigned int index;
	unsigned long long size;

	ZipEntry(const char *a_pchName, unsigned int a_index, unsigned long long a_size) :
		index(a_index),
		size(a_size)
	{
		//extract extension only, from filename provided by kuba-zip
		CString name(a_pchName);
		int pos = name.ReverseFind('.');
		if (pos >= 0)
		{
			ext = name.Right(name.GetLength() - pos - 1);
			ext.MakeLower();
		}
	}

	ZipEntry(tstring a_ext, unsigned int a_index, unsigned long long a_size) :
		ext(a_ext.c_str()), //extension provided directly by bit7z
		index(a_index),
		size(a_size)
	{
		ext.MakeLower();
	}
};

/////////////////////////////////////////////////////////////////////////////////////////////
// static helpers
/////////////////////////////////////////////////////////////////////////////////////////////

// find image format of this image by reading some header bytes
static EImageFormat GetImageFormat(LPCTSTR sFileName) {
	FILE* fptr;
	if ((fptr = _tfopen(sFileName, _T("rb"))) == NULL) {
		return IF_Unknown;
	}
	unsigned char header[16];
	int nSize = (int)fread((void*)header, 1, 16, fptr);
	fclose(fptr);
	if (nSize < 2) {
		return IF_Unknown;
	}

	if (header[0] == 0x42 && header[1] == 0x4d) {
		return IF_WindowsBMP;
	}
	else if (header[0] == 0xff && header[1] == 0xd8) {
		return IF_JPEG;
	}
	else if (header[0] == 0x89 && header[1] == 'P' && header[2] == 'N' && header[3] == 'G' &&
		header[4] == 0x0d && header[5] == 0x0a && header[6] == 0x1a && header[7] == 0x0a) {
		return IF_PNG;
	}
	else if (header[0] == 'G' && header[1] == 'I' && header[2] == 'F' && header[3] == '8' &&
		(header[4] == '7' || header[4] == '9') && header[5] == 'a') {
		return IF_GIF;
	}
	else if (header[0] == 'R' && header[1] == 'I' && header[2] == 'F' && header[3] == 'F' &&
		header[8] == 'W' && header[9] == 'E' && header[10] == 'B' && header[11] == 'P') {
		return IF_WEBP;
	} else if ((header[0] == 0xff && header[1] == 0x0a) ||
		memcmp(header, "\x00\x00\x00\x0cJXL\x20\x0d\x0a\x87\x0a", 12) == 0) {
		return IF_JXL;

	// Unfortunately, TIFF detection by header bytes is not reliable
	// A few RAW image formats use TIFF as the container
	// ex: CR2 - http://lclevy.free.fr/cr2/#key_info
	// ex: DNG - https://www.adobe.com/creativecloud/file-types/image/raw/dng-file.html#dng
	//
	// JPEGView will fail to open these files if the following code is used
	//
	//} else if ((header[0] == 0x49 && header[1] == 0x49 && header[2] == 0x2a && header[3] == 0x00) ||
	//	(header[0] == 0x4d && header[1] == 0x4d && header[2] == 0x00 && header[3] == 0x2a)) {
	//	return IF_TIFF;
/*
	} else if (header[0] == 0x00 && header[1] == 0x00 && header[2] == 0x00 &&
		memcmp(header+4, "ftyp", 4) == 0 &&
		(
			// https://github.com/strukturag/libheif/issues/83
			memcmp(header+8, "avif", 4) == 0 ||
			memcmp(header+8, "avis", 4) == 0 ||
			memcmp(header+8, "heic", 4) == 0 ||
			memcmp(header+8, "heix", 4) == 0 ||
			memcmp(header+8, "hevc", 4) == 0 ||
			memcmp(header+8, "hevx", 4) == 0 ||
			memcmp(header+8, "heim", 4) == 0 ||
			memcmp(header+8, "heis", 4) == 0 ||
			memcmp(header+8, "hevm", 4) == 0 ||
			memcmp(header+8, "hevs", 4) == 0 ||
			memcmp(header+8, "mif1", 4) == 0 ||
			memcmp(header+8, "msf1", 4) == 0	
		))
	{
*/
	}
	if (header[0] == 0x00 && header[1] == 0x00 && header[2] == 0x00 &&
		memcmp(header + 4, "ftyp", 4) == 0 )
	{
		if (
			// https://github.com/strukturag/libheif/issues/83
			//memcmp(header + 8, "avif", 4) == 0 ||
			memcmp(header + 8, "avis", 4) == 0
			)
		{
			return IF_AVIF;
		}
		if (
			// https://github.com/strukturag/libheif/issues/83
			memcmp(header + 8, "avif", 4) == 0 ||
			memcmp(header + 8, "heic", 4) == 0 ||
			memcmp(header + 8, "heix", 4) == 0 ||
			memcmp(header + 8, "hevc", 4) == 0 ||
			memcmp(header + 8, "hevx", 4) == 0 ||
			memcmp(header + 8, "heim", 4) == 0 ||
			memcmp(header + 8, "heis", 4) == 0 ||
			memcmp(header + 8, "hevm", 4) == 0 ||
			memcmp(header + 8, "hevs", 4) == 0 ||
			memcmp(header + 8, "mif1", 4) == 0 ||
			memcmp(header + 8, "msf1", 4) == 0
			)
		{
			return IF_HEIF;
		}
	} else if (header[0] == '8' && header[1] == 'B' && header[2] == 'P' && header[3] == 'S') {
		return IF_PSD;
	}
	else if (header[0] == 'P' && header[1] == 'K' && header[2] == 0x03 && header[3] == 0x04) {
		return IF_ZIP;
	}
	// default fallback if no matches based on magic bytes
	return Helpers::GetImageFormat(sFileName);
}

static EImageFormat GetBitmapFormat(Gdiplus::Bitmap* pBitmap) {
	GUID guid{ 0 };
	pBitmap->GetRawFormat(&guid);
	if (guid == Gdiplus::ImageFormatBMP) {
		return IF_WindowsBMP;
	}
	else if (guid == Gdiplus::ImageFormatPNG) {
		return IF_PNG;
	}
	else if (guid == Gdiplus::ImageFormatGIF) {
		return IF_GIF;
	}
	else if (guid == Gdiplus::ImageFormatTIFF) {
		return IF_TIFF;
	}
	else if (guid == Gdiplus::ImageFormatJPEG || guid == Gdiplus::ImageFormatEXIF) {
		return IF_JPEG;
	}
	else {
		return IF_Unknown;
	}
}

static CJPEGImage* ConvertGDIPlusBitmapToJPEGImage(Gdiplus::Bitmap* pBitmap, int nFrameIndex, void* pEXIFData,
	__int64 nJPEGHash, bool& isOutOfMemory, bool& isAnimatedGIF, Helpers::ETransparencyMode nTransparencyMode = Helpers::TP_BLEND) {

	isOutOfMemory = false;
	isAnimatedGIF = false;
	Gdiplus::Status lastStatus = pBitmap->GetLastStatus();
	if (lastStatus != Gdiplus::Ok) {
		isOutOfMemory = lastStatus == Gdiplus::OutOfMemory;
		return NULL;
	}

	if (pBitmap->GetWidth() > MAX_IMAGE_DIMENSION || pBitmap->GetHeight() > MAX_IMAGE_DIMENSION)
	{
		return NULL;
	}
	if ((double)pBitmap->GetWidth() * pBitmap->GetHeight() > MAX_IMAGE_PIXELS)
	{
		isOutOfMemory = true;
		return NULL;
	}

	EImageFormat eImageFormat = GetBitmapFormat(pBitmap);

	// Handle multiframe images.
	// Note that only the first frame dimension is looked at, it is unclear if GDI+ anyway supports image formats with more dimensions
	UINT nDimensions = pBitmap->GetFrameDimensionsCount();
	GUID* pDimensionIDs = new GUID[nDimensions];
	pBitmap->GetFrameDimensionsList(pDimensionIDs, nDimensions);
	int nFrameCount = (nDimensions == 0) ? 1 : pBitmap->GetFrameCount(&pDimensionIDs[0]);
	nFrameIndex = max(0, min(nFrameCount - 1, nFrameIndex));
	int nFrameTimeMs = 100;
	if (nFrameCount > 1) {
		isAnimatedGIF = eImageFormat == IF_GIF;
		int nTagFrameDelaySize = pBitmap->GetPropertyItemSize(PropertyTagFrameDelay);
		if (nTagFrameDelaySize > 0) {
			PropertyItem* pPropertyItem = (PropertyItem*)new char[nTagFrameDelaySize];
			if (pBitmap->GetPropertyItem(PropertyTagFrameDelay, nTagFrameDelaySize, pPropertyItem) == Gdiplus::Ok) {
				nFrameTimeMs = ((long*)pPropertyItem->value)[nFrameIndex] * 10;
			}
			delete[] pPropertyItem;
		}
		GUID pageGuid = (eImageFormat == IF_TIFF) ? FrameDimensionPage : FrameDimensionTime;
		pBitmap->SelectActiveFrame(&pageGuid, nFrameIndex);
	}
	delete[] pDimensionIDs;

	// If there is an alpha channel in the original file we must blit the image onto a background color offscreen
	// bitmap first to achieve proper rendering.
	CJPEGImage* pJPEGImage = NULL;
	Gdiplus::PixelFormat pixelFormat = pBitmap->GetPixelFormat();
	bool bHasAlphaChannel = (pixelFormat & (PixelFormatAlpha | PixelFormatPAlpha));
	Gdiplus::Bitmap* pBmTarget = NULL;
	Gdiplus::Graphics* pBmGraphics = NULL;
	Gdiplus::Bitmap* pBitmapToUse;
	if (bHasAlphaChannel) {
		pBmTarget = new Gdiplus::Bitmap(pBitmap->GetWidth(), pBitmap->GetHeight(), PixelFormat32bppRGB);
		pBmGraphics = new Gdiplus::Graphics(pBmTarget);
		COLORREF bkColor = CSettingsProvider::This().ColorTransparency();
		if (nTransparencyMode == Helpers::TP_BLEND)
		{
			//Somehow unlike WebpAlphaBlendBackground, needs to reverse the colour bytes!
			//Gdiplus::SolidBrush bkBrush(Gdiplus::Color(GetRValue(bkColor), GetGValue(bkColor), GetBValue(bkColor)));
			Gdiplus::SolidBrush bkBrush(Gdiplus::Color(GetBValue(bkColor), GetGValue(bkColor), GetRValue(bkColor)));
			pBmGraphics->FillRectangle(&bkBrush, 0, 0, pBmTarget->GetWidth(), pBmTarget->GetHeight());
		}
		else if (nTransparencyMode == Helpers::TP_BLEND_INVERSE)
		{
			Gdiplus::SolidBrush bkBrush(Gdiplus::Color(~GetBValue(bkColor), ~GetGValue(bkColor), ~GetRValue(bkColor)));
			pBmGraphics->FillRectangle(&bkBrush, 0, 0, pBmTarget->GetWidth(), pBmTarget->GetHeight());
		}
		else //if (nTransparencyMode == Helpers::TP_CHECKERBOARD)
		{
			Gdiplus::HatchBrush bkBrush(HatchStyleLargeCheckerBoard, Gdiplus::Color(0xffc0c0c0), Gdiplus::Color(0xffffffff));
			pBmGraphics->FillRectangle(&bkBrush, 0, 0, pBmTarget->GetWidth(), pBmTarget->GetHeight());
		}
		pBmGraphics->DrawImage(pBitmap, 0, 0, pBmTarget->GetWidth(), pBmTarget->GetHeight());
		pBitmapToUse = pBmTarget;
		if (pBmGraphics->GetLastStatus() == Gdiplus::OutOfMemory) {
			isOutOfMemory = true;
			delete pBmGraphics; delete pBmTarget;
			return NULL;
		}
	}
	else {
		pBitmapToUse = pBitmap;
	}

	Gdiplus::Rect bmRect(0, 0, pBitmap->GetWidth(), pBitmap->GetHeight());
	Gdiplus::BitmapData bmData;
	if (pBitmapToUse->LockBits(&bmRect, Gdiplus::ImageLockModeRead, PixelFormat32bppRGB, &bmData) == Gdiplus::Ok) {
		assert(bmData.PixelFormat == PixelFormat32bppRGB);
		void* pDIB = CBasicProcessing::ConvertGdiplus32bppRGB(bmRect.Width, bmRect.Height, bmData.Stride, bmData.Scan0);
		if (pDIB != NULL) {
			pJPEGImage = new CJPEGImage(bmRect.Width, bmRect.Height, pDIB, pEXIFData, 4, nJPEGHash, eImageFormat,
				eImageFormat == IF_GIF && nFrameCount > 1, nFrameIndex, nFrameCount, nFrameTimeMs);
		}
		pBitmapToUse->UnlockBits(&bmData);
	}

	if (pBmGraphics != NULL && pBmTarget != NULL) {
		delete pBmGraphics;
		delete pBmTarget;
	}

	pBitmap->GetLastStatus(); // reset status

	return pJPEGImage;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Public
/////////////////////////////////////////////////////////////////////////////////////////////

CImageLoadThread::CImageLoadThread(void) : CWorkThread(true),
	m_avifDecoder(0)
{
	m_pLastBitmap = NULL;
}

CImageLoadThread::~CImageLoadThread(void) {
	DeleteCachedGDIBitmap();
	DeleteCachedWebpDecoder();
	DeleteCachedPngDecoder();
	DeleteCachedJxlDecoder();
	DeleteCachedAvifDecoder();
	DeleteCachedZip();
}

int CImageLoadThread::AsyncLoad(LPCTSTR strFileName, int nFrameIndex, const CProcessParams& processParams, HWND targetWnd, HANDLE eventFinished) {
	CRequest* pRequest = new CRequest(strFileName, nFrameIndex, targetWnd, processParams, eventFinished);

	ProcessAsync(pRequest);

	return pRequest->RequestHandle;
}

CImageData CImageLoadThread::GetLoadedImage(int nHandle) {
	Helpers::CAutoCriticalSection criticalSection(m_csList);
	CJPEGImage* imageFound = NULL;
	bool bFailedMemory = false;
	bool bFailedException = false;
	std::list<CRequestBase*>::iterator iter;
	for (iter = m_requestList.begin(); iter != m_requestList.end(); iter++) {
		CRequest* pRequest = (CRequest*)(*iter);
		if (pRequest->Processed && pRequest->Deleted == false && pRequest->RequestHandle == nHandle) {
			imageFound = pRequest->Image;
			bFailedMemory = pRequest->OutOfMemory;
			bFailedException = pRequest->ExceptionError;
			// only mark as deleted
			pRequest->Deleted = true;
			break;
		}
	}
	return CImageData(imageFound, bFailedMemory, bFailedException);
}

void CImageLoadThread::ReleaseFile(LPCTSTR strFileName) {
	CReleaseFileRequest* pRequest = new CReleaseFileRequest(strFileName);
	ProcessAndWait(pRequest);
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Protected
/////////////////////////////////////////////////////////////////////////////////////////////

// Called on the processing thread
void CImageLoadThread::ProcessRequest(CRequestBase& request) {
	if (request.Type == CReleaseFileRequest::ReleaseFileRequest) {
		CReleaseFileRequest& rq = (CReleaseFileRequest&)request;
		if (rq.FileName == m_sLastFileName) {
			DeleteCachedGDIBitmap();
		}
		if (rq.FileName == m_sLastWebpFileName) {
			DeleteCachedWebpDecoder();
		}
		if (rq.FileName == m_sLastPngFileName) {
			DeleteCachedPngDecoder();
		}
		if (rq.FileName == m_sLastAvifFileName) {
			DeleteCachedAvifDecoder();
		}
		if (rq.FileName == m_sLastJxlFileName) {
			DeleteCachedJxlDecoder();
		}
		return;
	}

	CRequest& rq = (CRequest&)request;
	double dStartTime = Helpers::GetExactTickCount();
	// Get image format and read the image
	switch (GetImageFormat(rq.FileName)) {
	case IF_JPEG:
		DeleteCachedGDIBitmap();
		DeleteCachedWebpDecoder();
		DeleteCachedPngDecoder();
		DeleteCachedJxlDecoder();
		DeleteCachedAvifDecoder();
		DeleteCachedZip();
		ProcessReadJPEGRequest(&rq);
		break;
	case IF_WindowsBMP:
		DeleteCachedGDIBitmap();
		DeleteCachedWebpDecoder();
		DeleteCachedPngDecoder();
		DeleteCachedJxlDecoder();
		DeleteCachedAvifDecoder();
		DeleteCachedZip();
		ProcessReadBMPRequest(&rq);
		break;
	case IF_TGA:
		DeleteCachedGDIBitmap();
		DeleteCachedWebpDecoder();
		DeleteCachedPngDecoder();
		DeleteCachedJxlDecoder();
		DeleteCachedAvifDecoder();
		DeleteCachedZip();
		ProcessReadTGARequest(&rq);
		break;
	case IF_WEBP:
		DeleteCachedGDIBitmap();
		DeleteCachedPngDecoder();
		DeleteCachedJxlDecoder();
		DeleteCachedAvifDecoder();
		DeleteCachedZip();
		ProcessReadWEBPRequest(&rq);
		break;
	case IF_JXL:
		DeleteCachedGDIBitmap();
		DeleteCachedWebpDecoder();
		DeleteCachedPngDecoder();
		ProcessReadJXLRequest(&rq);
		DeleteCachedZip();
		DeleteCachedAvifDecoder();
		break;
	case IF_HEIF:
		DeleteCachedGDIBitmap();
		DeleteCachedWebpDecoder();
		DeleteCachedPngDecoder();
		DeleteCachedJxlDecoder();
		DeleteCachedAvifDecoder();
		DeleteCachedZip();
		ProcessReadHEIFRequest(&rq);
		break;
	case IF_QOI:
		DeleteCachedGDIBitmap();
		DeleteCachedWebpDecoder();
		DeleteCachedPngDecoder();
		DeleteCachedJxlDecoder();
		DeleteCachedAvifDecoder();
		DeleteCachedZip();
		ProcessReadQOIRequest(&rq);
		break;
	case IF_CameraRAW:
		DeleteCachedGDIBitmap();
		DeleteCachedWebpDecoder();
		DeleteCachedPngDecoder();
		DeleteCachedJxlDecoder();
		DeleteCachedAvifDecoder();
		DeleteCachedZip();
		ProcessReadRAWRequest(&rq);
		break;
	case IF_WIC:
		DeleteCachedGDIBitmap();
		DeleteCachedWebpDecoder();
		DeleteCachedPngDecoder();
		DeleteCachedJxlDecoder();
		DeleteCachedAvifDecoder();
		DeleteCachedZip();
		ProcessReadWICRequest(&rq);
		break;
	case IF_PNG:
		DeleteCachedGDIBitmap();
		DeleteCachedWebpDecoder();
		DeleteCachedJxlDecoder();
		DeleteCachedAvifDecoder();
		DeleteCachedZip();
		ProcessReadPNGRequest(&rq);
		break;
	case IF_AVIF:
		DeleteCachedGDIBitmap();
		DeleteCachedWebpDecoder();
		DeleteCachedPngDecoder();
		DeleteCachedZip();
		ProcessReadAVIFRequest(&rq);
		break;
	case IF_PSD:
		DeleteCachedGDIBitmap();
		DeleteCachedWebpDecoder();
		DeleteCachedPngDecoder();
		DeleteCachedJxlDecoder();
		DeleteCachedAvifDecoder();
		DeleteCachedZip();
		ProcessReadPSDRequest(&rq);
		break;
	case IF_ZIP:
		DeleteCachedGDIBitmap();
		DeleteCachedWebpDecoder();
		DeleteCachedPngDecoder();
		DeleteCachedJxlDecoder();
		DeleteCachedAvifDecoder();
		ProcessReadZipRequest(&rq);
		break;
	default:
		// try with GDI+
		DeleteCachedWebpDecoder();
		DeleteCachedPngDecoder();
		DeleteCachedJxlDecoder();
		DeleteCachedAvifDecoder();
		DeleteCachedZip();
		ProcessReadGDIPlusRequest(&rq);
		break;
	}
	// then process the image if read was successful
	if (rq.Image != NULL) {
		rq.Image->SetLoadTickCount(Helpers::GetExactTickCount() - dStartTime);
		if (!ProcessImageAfterLoad(&rq)) {
			delete rq.Image;
			rq.Image = NULL;
			rq.OutOfMemory = true;
		}
	}
}

// Called on the processing thread
void CImageLoadThread::AfterFinishProcess(CRequestBase& request) {
	if (request.Type == CReleaseFileRequest::ReleaseFileRequest) {
		return;
	}

	CRequest& rq = (CRequest&)request;
	if (rq.TargetWnd != NULL) {
		// post message to window that request has been processed
		::PostMessage(rq.TargetWnd, WM_IMAGE_LOAD_COMPLETED, 0, rq.RequestHandle);
	}
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Private
/////////////////////////////////////////////////////////////////////////////////////////////

static void LimitOffsets(CPoint& offsets, CSize clippingSize, const CSize& imageSize) {
	int nMaxOffsetX = (imageSize.cx - clippingSize.cx) / 2;
	nMaxOffsetX = max(0, nMaxOffsetX);
	int nMaxOffsetY = (imageSize.cy - clippingSize.cy) / 2;
	nMaxOffsetY = max(0, nMaxOffsetY);
	offsets.x = max(-nMaxOffsetX, min(+nMaxOffsetX, offsets.x));
	offsets.y = max(-nMaxOffsetY, min(+nMaxOffsetY, offsets.y));
}

void CImageLoadThread::DeleteCachedGDIBitmap() {
	if (m_pLastBitmap != NULL) {
		delete m_pLastBitmap;
	}
	m_pLastBitmap = NULL;
	m_sLastFileName.Empty();
}

void CImageLoadThread::DeleteCachedWebpDecoder() {
	WebpReaderWriter::DeleteCache();
	m_sLastWebpFileName.Empty();
}

void CImageLoadThread::DeleteCachedPngDecoder() {
	PngReader::DeleteCache();
	m_sLastPngFileName.Empty();
}

void CImageLoadThread::DeleteCachedJxlDecoder() {
	JxlReader::DeleteCache();
	m_sLastJxlFileName.Empty();
}

void CImageLoadThread::DeleteCachedAvifDecoder() {
	if (m_avifDecoder)
	{
		if (m_avifDecoder->io && m_avifDecoder->io->data)
		{
			delete[] m_avifDecoder->io->data;
			m_avifDecoder->io->data = 0;
		}
		avifDecoderDestroy(m_avifDecoder);
		m_avifDecoder = 0;
	}
	m_sLastAvifFileName.Empty();
}

void CImageLoadThread::DeleteCachedZip()
{
	m_sLastZipFileName.Empty();
	m_nLastZipIndex = -1; m_nZipCount = 0;
	zipEntries.clear();
}

void CImageLoadThread::ProcessReadJPEGRequest(CRequest* request) {
	HANDLE hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return;
	}

	HGLOBAL hFileBuffer = NULL;
	void* pBuffer = NULL;
	try {
		// Don't read too huge files
		unsigned int nFileSize = ::GetFileSize(hFile, NULL);
		if (nFileSize > MAX_JPEG_FILE_SIZE) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		hFileBuffer = ::GlobalAlloc(GMEM_MOVEABLE, nFileSize);
		pBuffer = (hFileBuffer == NULL) ? NULL : ::GlobalLock(hFileBuffer);
		if (pBuffer == NULL) {
			if (hFileBuffer) ::GlobalFree(hFileBuffer);
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		unsigned int nNumBytesRead;
		if (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize) {
			if (CSettingsProvider::This().ForceGDIPlus() || CSettingsProvider::This().UseEmbeddedColorProfiles()) {
				IStream* pStream = NULL;
				if (::CreateStreamOnHGlobal(hFileBuffer, FALSE, &pStream) == S_OK) {
					Gdiplus::Bitmap* pBitmap = Gdiplus::Bitmap::FromStream(pStream, CSettingsProvider::This().UseEmbeddedColorProfiles());
					bool isOutOfMemory, isAnimatedGIF;
					request->Image = ConvertGDIPlusBitmapToJPEGImage(pBitmap, 0, Helpers::FindEXIFBlock(pBuffer, nFileSize),
						Helpers::CalculateJPEGFileHash(pBuffer, nFileSize), isOutOfMemory, isAnimatedGIF, request->ProcessParams.TransparencyMode);
					request->OutOfMemory = request->Image == NULL && isOutOfMemory;
					if (request->Image != NULL) {
						request->Image->SetJPEGComment(Helpers::GetJPEGComment(pBuffer, nFileSize));
					}
					pStream->Release();
					delete pBitmap;
				}
				else {
					request->OutOfMemory = true;
				}
			}
			else {
				int nWidth, nHeight, nBPP;
				TJSAMP eChromoSubSampling;
				bool bOutOfMemory;
				// int nTicks = ::GetTickCount();

				void* pPixelData = TurboJpeg::ReadImage(nWidth, nHeight, nBPP, eChromoSubSampling, bOutOfMemory, pBuffer, nFileSize);

				/*
				TCHAR buffer[20];
				_stprintf_s(buffer, 20, _T("%d"), ::GetTickCount() - nTicks);
				::MessageBox(NULL, CString(_T("Elapsed ticks: ")) + buffer, _T("Time"), MB_OK);
				*/

				// Color and b/w JPEG is supported
				if (pPixelData != NULL && (nBPP == 3 || nBPP == 1)) {
					request->Image = new CJPEGImage(nWidth, nHeight, pPixelData,
						Helpers::FindEXIFBlock(pBuffer, nFileSize), nBPP,
						Helpers::CalculateJPEGFileHash(pBuffer, nFileSize), IF_JPEG, false, 0, 1, 0);
					request->Image->SetJPEGComment(Helpers::GetJPEGComment(pBuffer, nFileSize));
					request->Image->SetJPEGChromoSampling(eChromoSubSampling);
				}
				else if (bOutOfMemory) {
					request->OutOfMemory = true;
				}
				else {
					// failed, try GDI+
					delete[] pPixelData;
					ProcessReadGDIPlusRequest(request);
				}
			}
		}
	}
	catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	::CloseHandle(hFile);
	if (pBuffer) ::GlobalUnlock(hFileBuffer);
	if (hFileBuffer) ::GlobalFree(hFileBuffer);
}

void CImageLoadThread::BlendAlpha(uint32* pImage32, int nWidth, int nHeight, Helpers::ETransparencyMode nTransparencyMode)
{
	COLORREF nTranparency = CSettingsProvider::This().ColorTransparency();
	if (nTransparencyMode != Helpers::TP_CHECKERBOARD)
	{
		if (nTransparencyMode == Helpers::TP_BLEND_INVERSE)
		{
			nTranparency = ~(nTranparency & 0xffffff);
		}
		for (int i = 0; i < nWidth * nHeight; ++i)
			*pImage32++ = WebpAlphaBlendBackground(*pImage32, nTranparency);
	} else {
		for (int y = 0; y < nHeight; ++y)
			for (int x = 0; x < nWidth; ++x)
				*pImage32++ = WebpAlphaBlendBackground(*pImage32, nTranparency, true, x, y);
	}
}

void CImageLoadThread::ProcessReadPNGRequest(CRequest* request) {
	bool bSuccess = false;
	bool bUseCachedDecoder = false;
	const wchar_t* sFileName;
	sFileName = (const wchar_t*)request->FileName;
	if (sFileName != m_sLastPngFileName) {
		DeleteCachedPngDecoder();
	}
	else {
		bUseCachedDecoder = true;
	}

	HANDLE hFile;
	if (!bUseCachedDecoder) {
		hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			return;
		}
	}
	char* pBuffer = NULL;
	try {
		unsigned int nFileSize;
		unsigned int nNumBytesRead;
		if (!bUseCachedDecoder) {
			// Don't read too huge files
			nFileSize = ::GetFileSize(hFile, NULL);
			if (nFileSize > MAX_PNG_FILE_SIZE) {
				::CloseHandle(hFile);
				return ProcessReadGDIPlusRequest(request);
			}

			pBuffer = new(std::nothrow) char[nFileSize];
			if (pBuffer == NULL) {
				::CloseHandle(hFile);
				return ProcessReadGDIPlusRequest(request);
			}
		}
		else {
			nFileSize = 0; // to avoid compiler warnings, not used
		}
		if (bUseCachedDecoder || (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize)) {
			int nWidth, nHeight, nBPP, nFrameCount, nFrameTimeMs;
			bool bHasAnimation;
			uint8* pPixelData = NULL;
			void* pEXIFData = NULL;

			// If UseEmbeddedColorProfiles is true and the image isn't animated, we should use GDI+ for better color management
			if (bUseCachedDecoder || !CSettingsProvider::This().UseEmbeddedColorProfiles() || PngReader::IsAnimated(pBuffer, nFileSize))
				pPixelData = (uint8*)PngReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer, nFileSize);

			if (pPixelData != NULL) {
				if (bHasAnimation)
					m_sLastPngFileName = sFileName;
				// Multiply alpha value into each AABBGGRR pixel
				BlendAlpha((uint32*)pPixelData, nWidth, nHeight, request->ProcessParams.TransparencyMode);
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, 4, 0, IF_PNG, bHasAnimation, request->FrameIndex, nFrameCount, nFrameTimeMs);
				free(pEXIFData);
				bSuccess = true;
			}
			else {
				DeleteCachedPngDecoder();
			}
		}
	}
	catch (...) {
		// delete request->Image;
		// request->Image = NULL;
		request->ExceptionError = true;
	}
	if (!bUseCachedDecoder) {
		::CloseHandle(hFile);
		delete[] pBuffer;
	}
	if (!bSuccess)
	{
		ProcessReadGDIPlusRequest(request);
		if (request->Image)
			request->ExceptionError = false; //GDIPlus succeeded, so clear PNG flagged ExceptionError
	}
}

BOOL StringEndsWithIgnoreCase(CString& str, CString& end)
{
	int lenFind = end.GetLength(),
		len = str.GetLength();
	if (len < lenFind) return false;
	CString find = str.Right(lenFind);
	return find.CompareNoCase(end) == 0;
}

void CImageLoadThread::ProcessReadZipRequest(CRequest* request) {
	bool bSuccess = false;
	bool bUseCachedDecoder = false,
		bMultipleFiles = false;
	const wchar_t* sFileName;
	int nFrameIndex = request->FrameIndex;
	sFileName = (const wchar_t*)request->FileName;
	if (sFileName == m_sLastZipFileName) {
		bUseCachedDecoder = true;
		if (nFrameIndex >= m_nZipCount)
			nFrameIndex = m_nZipCount - 1;
		if (nFrameIndex < 0)
			nFrameIndex = 0;
		bMultipleFiles = m_nZipCount > 1;
	}
	else {
		DeleteCachedZip();
	}

	bool use7z = !(StringEndsWithIgnoreCase(CString(sFileName), CString(".cbz"))); //true;

	int bufFilenameLen = request->FileName.GetLength() + 1;
	char* bufFilename = new char[bufFilenameLen];
	wcstombs(bufFilename, request->FileName.GetString(), bufFilenameLen);
	struct zip_t* zip = NULL;
	BitArchiveReader* zip7 = NULL;
	if (!bUseCachedDecoder) {
		if (use7z)
		{
			try
			{
				Bit7zLibrary lib{ "7z.dll" };
				zip7 = new BitArchiveReader(lib, bufFilename, BitFormat::Auto);

				for (const auto& item : *zip7) {
					if (!item.isDir())
					{
						zipEntries.push_back(ZipEntry(item.extension(), item.index(), item.size()));
					}
				}
				delete zip7; zip7 = 0; //somehow later extract will fail, unless reopened, so delete 1st
				m_nZipCount = zipEntries.size();
			}
			catch (const BitException& ex) {
				//::OutputDebugString(_T("bit7z ERR: ")); ::OutputDebugString(CString(ex.what()));
				delete zip7; zip7 = 0;
				delete [] bufFilename;
				return;
			}
		}
		else
		{
			zip = zip_open(bufFilename, 0, 'r');
			int n = zip_entries_total(zip);
			for (int i = 0; i < n; ++i) {
				zip_entry_openbyindex(zip, i);
				{
					if (!zip_entry_isdir(zip)) //skip folders
					{
						zipEntries.push_back(ZipEntry(zip_entry_name(zip), i, zip_entry_size(zip)));
					}
				}
				zip_entry_close(zip);
			}
			m_nZipCount = zipEntries.size(); //adjust count to valid one only
		}
		if (m_nZipCount < 0) {
			if (zip) zip_close(zip);
			else if (zip7) delete zip7;
			delete[] bufFilename;
			return;
		}
		if (m_nZipCount > 1) {
			bMultipleFiles = true;
			bUseCachedDecoder = true;
			m_sLastZipFileName = sFileName;
		}
		m_nLastZipIndex = 0;
		if (nFrameIndex >= m_nZipCount)
			nFrameIndex = m_nZipCount - 1;
		if (nFrameIndex < 0)
			nFrameIndex = 0;
	}
	char* pBuffer = NULL;
	try {
		ZipEntry z = zipEntries.at(nFrameIndex);
		CString &ext = z.ext;
		size_t nFileSize = z.size;
		if (!use7z)
		{
			if (!zip)
				zip = zip_open(bufFilename, 0, 'r');
			zip_entry_openbyindex(zip, z.index);
			{
				pBuffer = new(std::nothrow) char[nFileSize];
				zip_entry_noallocread(zip, (void*)pBuffer, nFileSize);
			}
			zip_entry_close(zip);
			zip_close(zip);
			zip = NULL;
		}
		else //if (use7z)
		{
			try
			{
				//const tstring name = ConvertLPCWSTRToString(entryName); //not needed, as can access by index
				pBuffer = new(std::nothrow) char[nFileSize];
				//somehow, has to reopen, else extract fails!
				Bit7zLibrary lib{ "7z.dll" };
				zip7 = new BitArchiveReader(lib, bufFilename, BitFormat::Auto);
				zip7->extractTo((byte_t*)pBuffer, nFileSize, z.index);
				delete zip7; zip7 = 0;
			}
			catch (const BitException& ex) {
				::OutputDebugString(_T("bit7z Extract ERR: ")); ::OutputDebugString(CString(ex.what()));
				delete zip7; zip7 = 0;
				delete[] bufFilename;
				return;
			}
		}

		bool bHasAnimation = false;
		if (EXT_MATCHES("jxl")
			|| ((nFileSize > 2) && (pBuffer[0] == 0xff && pBuffer[1] == 0x0a))
			|| ((nFileSize > 12) && (memcmp(pBuffer, "\x00\x00\x00\x0cJXL\x20\x0d\x0a\x87\x0a", 12) == 0)))
		{
			int nWidth, nHeight, nBPP, nFrameCount, nFrameTimeMs;
			void* pEXIFData;
			uint8* pPixelData = (uint8*)JxlReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer, nFileSize);
			if (pPixelData != NULL) {
				// Multiply alpha value into each AABBGGRR pixel
				BlendAlpha((uint32*)pPixelData, nWidth, nHeight, request->ProcessParams.TransparencyMode);
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, 4, 0, IF_ZIP, IF_JXL, bMultipleFiles, request->FrameIndex, m_nZipCount, 360000); //1hr
				free(pEXIFData);
				if (request->Image) bSuccess = true;
			}
		}
		else if (EXT_MATCHES("jpeg") || EXT_MATCHES("jpg")
			|| ((nFileSize > 2) && (pBuffer[0] == 0xff && pBuffer[1] == 0xd8)))
		{
			int nWidth, nHeight, nBPP;
			TJSAMP eChromoSubSampling;
			bool bOutOfMemory;

			void* pPixelData = TurboJpeg::ReadImage(nWidth, nHeight, nBPP, eChromoSubSampling, bOutOfMemory, pBuffer, nFileSize);

			// Color and b/w JPEG is supported
			if (pPixelData != NULL && (nBPP == 3 || nBPP == 1)) {
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData,
					Helpers::FindEXIFBlock(pBuffer, nFileSize), nBPP,
					Helpers::CalculateJPEGFileHash(pBuffer, nFileSize), IF_ZIP, IF_JPEG, bMultipleFiles, request->FrameIndex, m_nZipCount, 360000);
				request->Image->SetJPEGComment(Helpers::GetJPEGComment(pBuffer, nFileSize));
				request->Image->SetJPEGChromoSampling(eChromoSubSampling);
				if (request->Image) bSuccess = true;
			}
			else if (bOutOfMemory) {
				request->OutOfMemory = true;
			}
		}
		else if (EXT_MATCHES("png")
			|| ((nFileSize > 8) && (pBuffer[0] == 0x89 && pBuffer[1] == 'P' && pBuffer[2] == 'N' && pBuffer[3] == 'G' &&
				pBuffer[4] == 0x0d && pBuffer[5] == 0x0a && pBuffer[6] == 0x1a && pBuffer[7] == 0x0a)))
		{
			int nWidth, nHeight, nBPP, nFrameCount, nFrameTimeMs;
			bool bHasAnimation;
			void* pEXIFData = NULL;
			uint8* pPixelData = (uint8*)PngReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer, nFileSize);
			if (pPixelData != NULL) {
				// Multiply alpha value into each AABBGGRR pixel
				BlendAlpha((uint32*)pPixelData, nWidth, nHeight, request->ProcessParams.TransparencyMode);
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, 4, 0, IF_ZIP, IF_PNG, bMultipleFiles, request->FrameIndex, m_nZipCount, 360000);
				free(pEXIFData);
				if (request->Image) bSuccess = true;
			}
		}
		else if (EXT_MATCHES("webp")
			|| ((nFileSize > 12) && pBuffer[0] == 'R' && pBuffer[1] == 'I' && pBuffer[2] == 'F' && pBuffer[3] == 'F' &&
				pBuffer[8] == 'W' && pBuffer[9] == 'E' && pBuffer[10] == 'B' && pBuffer[11] == 'P'))
		{
			int nWidth, nHeight;
			bool bHasAnimation = bUseCachedDecoder;
			int nFrameCount = 1;
			int nFrameTimeMs = 0;
			int nBPP;
			void* pEXIFData;
			uint8* pPixelData = (uint8*)WebpReaderWriter::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer, nFileSize);
			if (pPixelData && nBPP == 4) {
				// Multiply alpha value into each AABBGGRR pixel
				BlendAlpha((uint32*)pPixelData, nWidth, nHeight, request->ProcessParams.TransparencyMode);
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, nBPP, 0, IF_ZIP, IF_WEBP, bMultipleFiles, request->FrameIndex, m_nZipCount, 360000);
				free(pEXIFData);
				if (request->Image) bSuccess = true;
			}
			else {
				delete[] pPixelData;
			}
		}
		else if (EXT_MATCHES("heif") || EXT_MATCHES("heic") || EXT_MATCHES("avif")
			|| ((nFileSize > 12)
				&& (pBuffer[0] == 0x00 && pBuffer[1] == 0x00 && pBuffer[2] == 0x00 &&
					memcmp(pBuffer + 4, "ftyp", 4) == 0)
				&& (
					memcmp(pBuffer + 8, "avis", 4) == 0 ||
					// https://github.com/strukturag/libheif/issues/83
					memcmp(pBuffer + 8, "avif", 4) == 0 ||
					memcmp(pBuffer + 8, "heic", 4) == 0 ||
					memcmp(pBuffer + 8, "heix", 4) == 0 ||
					memcmp(pBuffer + 8, "hevc", 4) == 0 ||
					memcmp(pBuffer + 8, "hevx", 4) == 0 ||
					memcmp(pBuffer + 8, "heim", 4) == 0 ||
					memcmp(pBuffer + 8, "heis", 4) == 0 ||
					memcmp(pBuffer + 8, "hevm", 4) == 0 ||
					memcmp(pBuffer + 8, "hevs", 4) == 0 ||
					memcmp(pBuffer + 8, "mif1", 4) == 0 ||
					memcmp(pBuffer + 8, "msf1", 4) == 0
					) ))
		{
			try
			{
				int nWidth, nHeight, nBPP, nFrameCount, nFrameTimeMs;
				nFrameCount = 1;
				nFrameTimeMs = 0;
				void* pEXIFData;
				uint8* pPixelData = (uint8*)HeifReader::ReadImage(nWidth, nHeight, nBPP, nFrameCount, pEXIFData, request->OutOfMemory, request->FrameIndex, pBuffer, nFileSize);
				if (pPixelData != NULL) {
					// Multiply alpha value into each AABBGGRR pixel
					BlendAlpha((uint32*)pPixelData, nWidth, nHeight, request->ProcessParams.TransparencyMode);

					request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, nBPP, 0, IF_ZIP, IF_HEIF, bMultipleFiles, request->FrameIndex, m_nZipCount, 360000);
					free(pEXIFData);
					if (request->Image) bSuccess = true;
				}
			}
			catch (heif::Error he) {
				// invalid image
				delete request->Image;
				request->Image = NULL;
			}
		}
		else if (EXT_MATCHES("qoi"))
		{
			int nWidth, nHeight, nBPP;
			void* pPixelData = QoiReaderWriter::ReadImage(nWidth, nHeight, nBPP, request->OutOfMemory, pBuffer, nFileSize);
			if (pPixelData != NULL) {
				if (nBPP == 4) {
					// Multiply alpha value into each AABBGGRR pixel
					BlendAlpha((uint32*)pPixelData, nWidth, nHeight, request->ProcessParams.TransparencyMode);
				}
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, NULL, nBPP, 0, IF_ZIP, IF_QOI, bMultipleFiles, request->FrameIndex, m_nZipCount, 360000);
				if (request->Image) bSuccess = true;
			}
		}
		else if (EXT_MATCHES("bmp"))
		{
			bool bOutOfMemory;
			request->Image = CReaderBMP::ReadBmpImage(pBuffer, nFileSize, bOutOfMemory, bMultipleFiles, request->FrameIndex, m_nZipCount, 360000);
			if (request->Image) bSuccess = true;
			if (bOutOfMemory) {
				request->OutOfMemory = true;
			}
		}
		//else //unable to do as API reads from file and not buffer. GDI+ (GIF, BMP), WIC, PSD, RAW
	}
	catch (const std::exception& e) {
		//std::exception_ptr p = std::current_exception();
		//::OutputDebugString(_T("bit7z ERR?: ")); ::OutputDebugString(p._Current_exception);
		::OutputDebugString(_T("bit7z ERR?: ")); ::OutputDebugString(CString(e.what()));
	}
	catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}

cleanup:
	if (zip != NULL)
		zip_close(zip);
	if (pBuffer) delete[] pBuffer;
	delete[] bufFilename;
}

void CImageLoadThread::ProcessReadAVIFRequest(CRequest* request) {
	bool bSuccess = false;
	bool bUseCachedDecoder = false,
		bHasAnimation = false;
	const wchar_t* sFileName;
	int nFrameIndex = request->FrameIndex;
	sFileName = (const wchar_t*)request->FileName;
	if ((sFileName == m_sLastAvifFileName) && m_avifDecoder) {
		bUseCachedDecoder = true;
		if (nFrameIndex >= m_avifDecoder->imageCount)
			nFrameIndex = m_avifDecoder->imageCount - 1;
		if (nFrameIndex < 0)
			nFrameIndex = 0;
		bHasAnimation = m_avifDecoder->imageCount > 1;
	}
	else {
		DeleteCachedAvifDecoder();
	}

	HANDLE hFile = 0;
	if (!bUseCachedDecoder) {
		hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			return;
		}
	}
	char* pBuffer = NULL;
	try {
		unsigned int nFileSize = 0;
		if (!bUseCachedDecoder) {
			// Don't read too huge files
			nFileSize = ::GetFileSize(hFile, NULL);
			/*
			* Use PNG limit for now; as it's amongst highest.
			* AVIF supports 3 bit depths: 8, 10 and 12 bits,
			* and the maximum dimensions of a coded image is 65536x65536,
			* when seq_level_idx is set to 31 (maximum parameters level).
			*/
			if (nFileSize > MAX_PNG_FILE_SIZE) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}

			pBuffer = new(std::nothrow) char[nFileSize];
			if (pBuffer == NULL) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}
		}

		avifRGBImage rgb;
		memset(&rgb, 0, sizeof(rgb));
		// Override decoder defaults here (codecChoice, requestedSource, ignoreExif, ignoreXMP, etc)

		unsigned int nNumBytesRead;
		if (bUseCachedDecoder || (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && (nNumBytesRead == nFileSize)))
		{
			if (!bUseCachedDecoder)
			{
				m_avifDecoder = avifDecoderCreate();

				avifResult result = avifDecoderSetIOMemory(m_avifDecoder, (const uint8_t*)pBuffer, nFileSize);
				if (result != AVIF_RESULT_OK) {
					goto cleanup; //won't happen
				}
				m_avifDecoder->io->data = pBuffer; //keep a copy of pointer for our cleanup

				result = avifDecoderParse(m_avifDecoder);
				if (result != AVIF_RESULT_OK) {
					goto cleanup; //Failed to decode image
				}
				bHasAnimation = m_avifDecoder->imageCount > 1;
			}

			// Now available:
			// * All decoder->image information other than pixel data:
			//   * width, height, depth
			//   * transformations (pasp, clap, irot, imir)
			//   * color profile (icc, CICP)
			//   * metadata (Exif, XMP)
			// * decoder->alphaPresent
			// * number of total images in the AVIF (decoder->imageCount)
			// * overall image sequence timing (including per-frame timing with avifDecoderNthImageTiming())

			avifResult r = avifDecoderNthImage(m_avifDecoder, nFrameIndex);
			if (r == AVIF_RESULT_OK)
			{
				// Now available (for this frame):
				// * All decoder->image YUV pixel data (yuvFormat, yuvPlanes, yuvRange, yuvChromaSamplePosition, yuvRowBytes)
				// * decoder->image alpha data (alphaPlane, alphaRowBytes)
				// * this frame's sequence timing

				avifRGBImageSetDefaults(&rgb, m_avifDecoder->image); //internally chooses RGBA
				// Override YUV(A)->RGB(A) defaults here:
				//   depth, format, chromaUpsampling, avoidLibYUV, ignoreAlpha, alphaPremultiplied, etc.
				rgb.format = AVIF_RGB_FORMAT_BGRA; //CJPEGImage desires BGRA, so change it
				if (rgb.depth > 8)
				{
					//CJPEGImage only supports 8bpp? so force downgrade to 8pp if original of higher bpp
					rgb.depth = 8;
				}

				// Alternative: set rgb.pixels and rgb.rowBytes yourself, which should match your chosen rgb.format
				// Be sure to use uint16_t* instead of uint8_t* for rgb.pixels/rgb.rowBytes if (rgb.depth > 8)
				// Use new(std::nothrow) unsigned char[] as per JPEGView internals (so it can cleanup itself), instead of avifRGBImageAllocatePixels(&rgb)'s c-based malloc.
				if (rgb.pixels) {
					delete[] rgb.pixels;
					rgb.pixels = 0;
				}
				rgb.rowBytes = rgb.width * avifRGBImagePixelSize(&rgb);
				rgb.pixels = new(std::nothrow) unsigned char[(size_t)rgb.rowBytes * rgb.height];
				if (rgb.pixels)
				{
					if (avifImageYUVToRGB(m_avifDecoder->image, &rgb) != AVIF_RESULT_OK) {
						delete[] rgb.pixels;
						rgb.pixels = 0;
						goto cleanup; //Conversion from YUV failed
					}

					// Now available:
					// * RGB(A) pixel data (rgb.pixels, rgb.rowBytes)

					int nFrameTimeMs = (int)(m_avifDecoder->imageTiming.duration * 1000);
					if (bHasAnimation) {
						m_sLastAvifFileName = sFileName;
					}
					BlendAlpha((uint32*)(rgb.pixels), m_avifDecoder->image->width, m_avifDecoder->image->height, request->ProcessParams.TransparencyMode);
					request->Image = new CJPEGImage(m_avifDecoder->image->width, m_avifDecoder->image->height, rgb.pixels, 0, 4, 0, IF_AVIF, bHasAnimation, request->FrameIndex, m_avifDecoder->imageCount, nFrameTimeMs);
					if (!bHasAnimation) DeleteCachedAvifDecoder();
					bSuccess = true;
				}
				else
				{
					request->OutOfMemory = true;
				}
			}
		}
	}
	catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}

cleanup:
	if (!bUseCachedDecoder)
	{
		::CloseHandle(hFile);
	}
	if (!bSuccess)
	{
		DeleteCachedAvifDecoder();
		if (!bHasAnimation)
		{
			//failed, so try HEIF/dav1d decoder instead
			try {
				ProcessReadHEIFRequest(request);
			}
			catch (...) {
				request->Image = 0;
			}
			if (!request->Image)
			{
				try {
					ProcessReadGDIPlusRequest(request);
				}
				catch (...) {
					request->Image = 0;
				}
			}
			if (request->Image)
				request->ExceptionError = false; //either HEIF or GDIPlus succeeded, so clear ExceptionError
		}
	}
}

void CImageLoadThread::ProcessReadBMPRequest(CRequest* request) {
	bool bOutOfMemory;
	request->Image = CReaderBMP::ReadBmpImage(request->FileName, bOutOfMemory);
	if (bOutOfMemory) {
		request->OutOfMemory = true;
	} else if (request->Image == NULL) {
		// probably one of the bitmap formats that can not be read directly, try with GDI+
		ProcessReadGDIPlusRequest(request);
	}
}

void CImageLoadThread::ProcessReadTGARequest(CRequest* request) {
	bool bOutOfMemory;
	request->Image = CReaderTGA::ReadTgaImage(request->FileName, CSettingsProvider::This().ColorTransparency(), bOutOfMemory, request->ProcessParams.TransparencyMode);
	if (bOutOfMemory) {
		request->OutOfMemory = true;
	}
}

void CImageLoadThread::ProcessReadWEBPRequest(CRequest* request) {
	bool bUseCachedDecoder = false;
	const wchar_t* sFileName;
	sFileName = (const wchar_t*)request->FileName;
	if (sFileName != m_sLastWebpFileName) {
		DeleteCachedWebpDecoder();
	}
	else {
		bUseCachedDecoder = true;
	}

	HANDLE hFile;
	if (!bUseCachedDecoder) {
		hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			return;
		}
	}
	char* pBuffer = NULL;
	try {
		unsigned int nFileSize = 0;
		unsigned int nNumBytesRead;
		if (!bUseCachedDecoder) {
			// Don't read too huge files
			nFileSize = ::GetFileSize(hFile, NULL);
			if (nFileSize > MAX_WEBP_FILE_SIZE) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}

			pBuffer = new(std::nothrow) char[nFileSize];
			if (pBuffer == NULL) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}
		}
		if (bUseCachedDecoder || (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize)) {
			int nWidth, nHeight;
			bool bHasAnimation = bUseCachedDecoder;
			int nFrameCount = 1;
			int nFrameTimeMs = 0;
			int nBPP;
			void* pEXIFData;
			uint8* pPixelData = (uint8*)WebpReaderWriter::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer, nFileSize);
			if (pPixelData && nBPP == 4) {
				// Multiply alpha value into each AABBGGRR pixel
				BlendAlpha((uint32*)pPixelData, nWidth, nHeight, request->ProcessParams.TransparencyMode);

				if (bHasAnimation) {
					m_sLastWebpFileName = sFileName;
				}
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, nBPP, 0, IF_WEBP, bHasAnimation, request->FrameIndex, nFrameCount, nFrameTimeMs);
				free(pEXIFData);
			}
			else {
				delete[] pPixelData;
				DeleteCachedWebpDecoder();
			}
		}
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
	}
	if (!bUseCachedDecoder) {
		::CloseHandle(hFile);
		delete[] pBuffer;
	}
}

void CImageLoadThread::ProcessReadJXLRequest(CRequest* request) {
	bool bUseCachedDecoder = false;
	const wchar_t* sFileName;
	sFileName = (const wchar_t*)request->FileName;
	if (sFileName != m_sLastJxlFileName) {
		DeleteCachedJxlDecoder();
	} else {
		bUseCachedDecoder = true;
	}

	HANDLE hFile;
	if (!bUseCachedDecoder) {
		hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			return;
		}
	}
	char* pBuffer = NULL;
	UINT nPrevErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS);
	try {
		unsigned int nFileSize = 0;
		unsigned int nNumBytesRead;
		if (!bUseCachedDecoder) {
			// Don't read too huge files
			nFileSize = ::GetFileSize(hFile, NULL);
			if (nFileSize > MAX_JXL_FILE_SIZE) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}

			pBuffer = new(std::nothrow) char[nFileSize];
			if (pBuffer == NULL) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}
		}
		if (bUseCachedDecoder || (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize)) {
			int nWidth, nHeight, nBPP, nFrameCount, nFrameTimeMs;
			bool bHasAnimation;
			void* pEXIFData;
			uint8* pPixelData = (uint8*)JxlReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer, nFileSize);
			if (pPixelData != NULL) {
				if (bHasAnimation)
					m_sLastJxlFileName = sFileName;
				// Multiply alpha value into each AABBGGRR pixel
				BlendAlpha((uint32*)pPixelData, nWidth, nHeight, request->ProcessParams.TransparencyMode);
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, 4, 0, IF_JXL, bHasAnimation, request->FrameIndex, nFrameCount, nFrameTimeMs);
				free(pEXIFData);
			} else {
				DeleteCachedJxlDecoder();
			}
		}
	}
	catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	SetErrorMode(nPrevErrorMode);
	if (!bUseCachedDecoder) {
		::CloseHandle(hFile);
		delete[] pBuffer;
	}
}

void CImageLoadThread::ProcessReadHEIFRequest(CRequest* request) {
	HANDLE hFile;
	hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return;
	}
	char* pBuffer = NULL;
	UINT nPrevErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS);
	try {
		unsigned int nFileSize = 0;
		unsigned int nNumBytesRead;
		// Don't read too huge files
		nFileSize = ::GetFileSize(hFile, NULL);
		if (nFileSize > MAX_HEIF_FILE_SIZE) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}

		pBuffer = new(std::nothrow) char[nFileSize];
		if (pBuffer == NULL) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		if (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize) {
			int nWidth, nHeight, nBPP, nFrameCount, nFrameTimeMs;
			nFrameCount = 1;
			nFrameTimeMs = 0;
			void* pEXIFData;
			uint8* pPixelData = (uint8*)HeifReader::ReadImage(nWidth, nHeight, nBPP, nFrameCount, pEXIFData, request->OutOfMemory, request->FrameIndex, pBuffer, nFileSize);
			if (pPixelData != NULL) {
				// Multiply alpha value into each AABBGGRR pixel
				BlendAlpha((uint32*)pPixelData, nWidth, nHeight, request->ProcessParams.TransparencyMode);

				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, nBPP, 0, IF_HEIF, false, request->FrameIndex, nFrameCount, nFrameTimeMs);
				free(pEXIFData);
			}
		}
	} catch(heif::Error he) {
		// invalid image
		delete request->Image;
		request->Image = NULL;
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	SetErrorMode(nPrevErrorMode);
	::CloseHandle(hFile);
	delete[] pBuffer;
}

void CImageLoadThread::ProcessReadPSDRequest(CRequest* request) {
	request->Image = PsdReader::ReadImage(request->FileName, request->OutOfMemory);
	if (request->Image == NULL && !request->OutOfMemory) {
		request->Image = PsdReader::ReadThumb(request->FileName, request->OutOfMemory);
	}
}

void CImageLoadThread::ProcessReadQOIRequest(CRequest* request) {
	HANDLE hFile;
	hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return;
	}
	char* pBuffer = NULL;
	try {
		unsigned int nFileSize = 0;
		unsigned int nNumBytesRead;
		// Don't read too huge files
		nFileSize = ::GetFileSize(hFile, NULL);
		if (nFileSize > MAX_PNG_FILE_SIZE) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}

		pBuffer = new(std::nothrow) char[nFileSize];
		if (pBuffer == NULL) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		if (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize) {
			int nWidth, nHeight, nBPP;
			void* pPixelData = QoiReaderWriter::ReadImage(nWidth, nHeight, nBPP, request->OutOfMemory, pBuffer, nFileSize);
			if (pPixelData != NULL) {
				if (nBPP == 4) {
					// Multiply alpha value into each AABBGGRR pixel
					BlendAlpha((uint32*)pPixelData, nWidth, nHeight, request->ProcessParams.TransparencyMode);
				}
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, NULL, nBPP, 0, IF_QOI, false, 0, 1, 0);
			}
		}
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	::CloseHandle(hFile);
	delete[] pBuffer;
}

void CImageLoadThread::ProcessReadRAWRequest(CRequest * request) {
	bool bOutOfMemory = false;
	try {
		int fullsize = CSettingsProvider::This().DisplayFullSizeRAW();

#ifndef WINXP
		// Try with libraw
		UINT nPrevErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS);
		try {
			if (fullsize == 2 || fullsize == 3) {
				request->Image = RawReader::ReadImage(request->FileName, bOutOfMemory, fullsize == 2);
			}
			if (request->Image == NULL) {
				request->Image = RawReader::ReadImage(request->FileName, bOutOfMemory, fullsize == 0 || fullsize == 3);
			}
		} catch (...) {
			// libraw.dll not found or VC++ Runtime not installed
		}
		SetErrorMode(nPrevErrorMode);
#endif

		// Try with dcraw_mod
		if (request->Image == NULL && fullsize != 1) {
			request->Image = CReaderRAW::ReadRawImage(request->FileName, bOutOfMemory);
		}
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
	}
	if (bOutOfMemory) {
		request->OutOfMemory = true;
	}
}

void CImageLoadThread::ProcessReadGDIPlusRequest(CRequest* request) {
	const wchar_t* sFileName;
	sFileName = (const wchar_t*)request->FileName;

	Gdiplus::Bitmap* pBitmap = NULL;
	if (sFileName == m_sLastFileName) {
		pBitmap = m_pLastBitmap;
	}
	else {
		DeleteCachedGDIBitmap();
		m_pLastBitmap = pBitmap = new Gdiplus::Bitmap(sFileName, CSettingsProvider::This().UseEmbeddedColorProfiles());
		m_sLastFileName = sFileName;
	}
	bool isOutOfMemory, isAnimatedGIF;
	request->Image = ConvertGDIPlusBitmapToJPEGImage(pBitmap, request->FrameIndex, NULL, 0, isOutOfMemory, isAnimatedGIF, request->ProcessParams.TransparencyMode);
	request->OutOfMemory = request->Image == NULL && isOutOfMemory;
	if (!isAnimatedGIF) {
		DeleteCachedGDIBitmap();
	}
}

static unsigned char* alloc(int sizeInBytes) {
	return new(std::nothrow) unsigned char[sizeInBytes];
}

static void dealloc(unsigned char* buffer) {
	delete[] buffer;
}

typedef unsigned char* Allocator(int sizeInBytes);
typedef void Deallocator(unsigned char* buffer);

__declspec(dllimport) unsigned char* __stdcall LoadImageWithWIC(LPCWSTR fileName, Allocator* allocator, Deallocator* deallocator,
	unsigned int* width, unsigned int* height);

void CImageLoadThread::ProcessReadWICRequest(CRequest* request) {
	const wchar_t* sFileName;
	sFileName = (const wchar_t*)request->FileName;

	try {
		uint32 nWidth, nHeight;
		unsigned char* pDIB = LoadImageWithWIC(sFileName, &alloc, &dealloc, &nWidth, &nHeight);
		if (pDIB != NULL) {
			BlendAlpha((uint32*)(pDIB), nWidth, nHeight, request->ProcessParams.TransparencyMode);
			request->Image = new CJPEGImage(nWidth, nHeight, pDIB, NULL, 4, 0, IF_WIC, false, 0, 1, 0);
		}
	}
	catch (...) {
		// fatal error in WIC
		request->ExceptionError = true;
	}
}

bool CImageLoadThread::ProcessImageAfterLoad(CRequest* request) {
	// set process parameters depending on filename
	request->Image->SetFileDependentProcessParams(request->FileName, &(request->ProcessParams));

	// First do rotation, this maybe modifies the width and height
	if (!request->Image->VerifyRotation(CRotationParams(request->ProcessParams.RotationParams, request->ProcessParams.RotationParams.Rotation + request->ProcessParams.UserRotation))) {
		return false;
	}

	// Do nothing (except rotation) if processing after load turned off
	if (GetProcessingFlag(request->ProcessParams.ProcFlags, PFLAG_NoProcessingAfterLoad)) {
		return true;
	}

	int nWidth = request->Image->OrigWidth();
	int nHeight = request->Image->OrigHeight();

	double dZoom = request->ProcessParams.Zoom;
	CSize newSize;
	if (dZoom < 0.0) {
		newSize = Helpers::GetImageRect(nWidth, nHeight,
			request->ProcessParams.TargetWidth, request->ProcessParams.TargetHeight, request->ProcessParams.AutoZoomMode, dZoom);
	}
	else {
		newSize = CSize((int)(nWidth * dZoom + 0.5), (int)(nHeight * dZoom + 0.5));
	}

	newSize.cx = max(1, min(65535, newSize.cx));
	newSize.cy = max(1, min(65535, newSize.cy)); // max size must not be bigger than this after zoom

	// clip to target rectangle
	CSize clippedSize(min(request->ProcessParams.TargetWidth, newSize.cx),
		min(request->ProcessParams.TargetHeight, newSize.cy));

	LimitOffsets(request->ProcessParams.Offsets, CSize(request->ProcessParams.TargetWidth, request->ProcessParams.TargetHeight), newSize);

	// this will process the image and cache the processed DIB in the CJPEGImage instance
	CPoint offsetInImage = request->Image->ConvertOffset(newSize, clippedSize, request->ProcessParams.Offsets);
	return NULL != request->Image->GetDIB(newSize, clippedSize, offsetInImage,
		request->ProcessParams.ImageProcParams, request->ProcessParams.ProcFlags);
}
