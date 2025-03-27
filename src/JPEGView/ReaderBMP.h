#pragma once

class CJPEGImage;

// Simple reader for windows bitmap files (.bmp)
class CReaderBMP
{
public:
	// Returns NULL in case of errors
	static CJPEGImage* ReadBmpImage(LPCTSTR strFileName, bool& bOutOfMemory);
	static CJPEGImage* ReadBmpImage(void * pBuffer, int bufsize, bool& bOutOfMemory, bool bIsAnimation = false, int nFrameIndex = 0, int nNumberOfFrames = 1, int nFrameTimeMs = 0);
private:
	CReaderBMP(void);
};
