/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Windows Client
 *
 * Copyright 2009-2011 Jay Sorg
 * Copyright 2010-2011 Vic Lee
 * Copyright 2010-2011 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 *
 * Myrtille: A native HTML4/5 Remote Desktop Protocol client
 *
 * Copyright(c) 2014-2020 Cedric Coste
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma region Myrtille

#include <string>
#include <sstream>
#include <vector>
#include <map>
#include <atomic>

#include <objidl.h>

#include <GdiPlus.h>
#pragma comment(lib, "gdiplus.lib")
using namespace Gdiplus;

#include "encode.h"
#pragma comment(lib, "libwebp.lib")

#include <freerdp/client/cmdline.h>
#include "wf_client.h"
#include "wf_myrtille.h"

int getEncoderClsid(const WCHAR* format, CLSID* pClsid);
std::string getCurrentTime();
std::string createLogDirectory();
std::wstring s2ws(const std::string& s);
DWORD connectRemoteSessionPipes(wfContext* wfc);
HANDLE connectRemoteSessionPipe(wfContext* wfc, std::string pipeName, DWORD accessMode, DWORD shareMode, DWORD flags);
std::string createRemoteSessionDirectory(wfContext* wfc);
void processResizeDisplay(wfContext* wfc, bool keepAspectRatio, std::string resolution);
void processMouseInput(wfContext* wfc, std::string input, UINT16 flags);
void sendMessage(wfContext* wfc, std::wstring msg);
void processImage(wfContext* wfc, Gdiplus::Bitmap* bmp, int left, int top, int right, int bottom, bool fullscreen, bool adaptive);
void saveImage(wfContext* wfc, Gdiplus::Bitmap* bmp, int idx, int format, int quality, bool fullscreen);
void sendImage(wfContext* wfc, Gdiplus::Bitmap* bmp, int idx, int posX, int posY, int width, int height, int format, int quality, IStream* stream, int size, bool fullscreen);
void sendAudio(wfContext* wfc, const BYTE* data, size_t size);
void takeScreenshot(wfContext* wfc, Gdiplus::Bitmap* bmp);
void int32ToBytes(int value, int offset, byte* bytes);
int bytesToInt32(byte* bytes);

void webPEncoder(wfContext* wfc, Gdiplus::Bitmap* bmp, int idx, IStream* stream, float quality, bool fullscreen);
static int webPWriter(const uint8_t* data, size_t data_size, const WebPPicture* const pic);

DWORD WINAPI processInputsPipe(LPVOID lpParameter);

#define TAG CLIENT_TAG("myrtille")

#define IMAGE_COUNT_SAMPLING_RATE 100		// ips sampling (%) less images = lower cpu and bandwidth usage / faster; more = smoother display (skipping images may result in some display inconsistencies). tweaked dynamically to fit the available bandwidth; possible values: 5, 10, 20, 25, 50, 100 (lower = higher drop rate)

#define CLIPBOARD_MAX_LENGTH_LOG 100		// max number of characters to log for the client clipboard
#define CLIPBOARD_MAX_LENGTH_SEND 1048576	// max number of characters to send for the server clipboard; 1MB is usually enough for most copy/paste actions

// command
enum class COMMAND
{
	// connection
	SEND_SERVER_ADDRESS = 0,
	SEND_VM_GUID = 1,
	SEND_USER_DOMAIN = 2,
	SEND_USER_NAME = 3,
	SEND_USER_PASSWORD = 4,
	SEND_START_PROGRAM = 5,
	CONNECT_CLIENT = 6,

	// browser
	SEND_BROWSER_RESIZE = 7,
	SEND_BROWSER_PULSE = 8,

	// keyboard
	SEND_KEY_UNICODE = 9,
	SEND_KEY_SCANCODE = 10,

	// mouse
	SEND_MOUSE_MOVE = 11,
	SEND_MOUSE_LEFT_BUTTON = 12,
	SEND_MOUSE_MIDDLE_BUTTON = 13,
	SEND_MOUSE_RIGHT_BUTTON = 14,
	SEND_MOUSE_WHEEL_UP = 15,
	SEND_MOUSE_WHEEL_DOWN = 16,

	// control
	SET_SCALE_DISPLAY = 17,
	SET_RECONNECT_SESSION = 18,
	SET_IMAGE_ENCODING = 19,
	SET_IMAGE_QUALITY = 20,
	SET_IMAGE_QUANTITY = 21,
	SET_AUDIO_FORMAT = 22,
	SET_AUDIO_BITRATE = 23,
	SET_SCREENSHOT_CONFIG = 24,
	START_TAKING_SCREENSHOTS = 25,
	STOP_TAKING_SCREENSHOTS = 26,
	TAKE_SCREENSHOT = 27,
	REQUEST_FULLSCREEN_UPDATE = 28,
	SEND_LOCAL_CLIPBOARD = 29,
	CLOSE_CLIENT = 30
};

// command mapping
std::map<std::string, COMMAND> commandMap;

// image encoding
enum class IMAGE_ENCODING
{
	AUTO = 0,
	PNG = 1,								// default
	JPEG = 2,
	WEBP = 3
};

// image format
enum class IMAGE_FORMAT
{
	CUR = 0,
	PNG = 1,
	JPEG = 2,
	WEBP = 3
};

// image quality (%)
// fact is, it may vary depending on the image format...
// to keep things easy, and because there are only 2 quality based (lossy) formats managed by this program (JPEG and WEBP... PNG is lossless), we use the same * base * values for all of them...
enum class IMAGE_QUALITY
{
	LOW = 10,
	MEDIUM = 25,
	HIGH = 50,								// not applicable for PNG (lossless); may be tweaked dynamically depending on image encoding and client bandwidth
	HIGHER = 75,							// not applicable for PNG (lossless); used for fullscreen updates in adaptive mode
	HIGHEST = 100							// default
};

// audio format
enum class AUDIO_FORMAT
{
	NONE = 0,								// audio disabled
	WAV = 1,								// uncompressed PCM 44100 Hz, 16 bits stereo
	MP3 = 2									// compressed MPEG 3 (default)
};

struct wf_myrtille
{
	// pipes
	HANDLE inputsPipe;
	HANDLE updatesPipe;
	HANDLE audioPipe;

	// inputs
	bool processInputs;

	// updates
	int imageEncoding;						// provided by the client
	int imageQuality;						// provided by the client
	int imageQuantity;						// provided by the client
	std::atomic<int> imageCount;			// protect from concurrent accesses
	std::atomic<int> imageIdx;				// protect from concurrent accesses

	// updates buffer
	// in case of bandwidth issue, the browser/gateway roundtrip duration will increase dramatically (increasing accumulated delay -> lag)
	// some updates must be consolidated into a single one to reduce both the cpu and bandwidth usage
	RECT imageBuffer;						// consolidated region

	// display
	bool scaleDisplay;						// overrides the FreeRDP "SmartSizing" setting; the objective is not to interfere with the FreeRDP window, if shown
	int clientWidth;						// overrides wf_context::client_width
	int clientHeight;						// overrides wf_context::client_height
	float aspectRatio;						// original aspect ratio of the display

	// audio
	int audioFormat;						// if needed (handled by the gateway)
	int audioBitrate;						// if needed (handled by the gateway)

	// screenshot
	int screenshotIntervalSecs;				// if needed (handled by the gateway)
	int screenshotFormat;					// PNG or JPEG
	std::string screenshotPath;				// output location
	bool screenshotEnabled;					// take screenshot on the next fullscreen update

	// clipboard
	std::wstring clipboardText;				// unicode text

	// GDI+
	ULONG_PTR gdiplusToken;
	CLSID pngClsid;
	CLSID jpgClsid;
	EncoderParameters encoderParameters;

	// WebP
	WebPConfig webpConfig;
};
typedef struct wf_myrtille wfMyrtille;

void wf_myrtille_start(wfContext* wfc)
{
	if (wfc->context.settings->MyrtilleSessionId == NULL)
		return;

	wfc->myrtille = (wfMyrtille*)calloc(1, sizeof(wfMyrtille));
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	#if !defined(WITH_DEBUG) && !defined(_DEBUG)
	// by default, redirect stdout and stderr to nothing (same as linux "/dev/null")
	freopen("nul", "w", stdout);
	freopen("nul", "w", stderr);
	#endif

	// debug
	if (wfc->context.settings->MyrtilleDebugLog)
	{
		std::string logDirectoryPath = createLogDirectory();
		if (logDirectoryPath != "")
		{
			std::stringstream ss;
			ss << logDirectoryPath << "\\wfreerdp." << GetCurrentProcessId() << ".log";
			std::string s = ss.str();
			const char* logFilename = s.c_str();
			freopen(logFilename, "w", stdout);
			freopen(logFilename, "w", stderr);
		}
	}

	/*
	prefixes (3 chars) are used to serialize commands with strings instead of numbers
	they make it easier to read log traces to find out which commands are issued
	they must match the prefixes used client side
	commands can also be reordered without any issue
	*/
	commandMap["SRV"] = COMMAND::SEND_SERVER_ADDRESS;
	commandMap["VMG"] = COMMAND::SEND_VM_GUID;
	commandMap["DOM"] = COMMAND::SEND_USER_DOMAIN;
	commandMap["USR"] = COMMAND::SEND_USER_NAME;
	commandMap["PWD"] = COMMAND::SEND_USER_PASSWORD;
	commandMap["PRG"] = COMMAND::SEND_START_PROGRAM;
	commandMap["CON"] = COMMAND::CONNECT_CLIENT;
	commandMap["RSZ"] = COMMAND::SEND_BROWSER_RESIZE;
	commandMap["PLS"] = COMMAND::SEND_BROWSER_PULSE;
	commandMap["KUC"] = COMMAND::SEND_KEY_UNICODE;
	commandMap["KSC"] = COMMAND::SEND_KEY_SCANCODE;
	commandMap["MMO"] = COMMAND::SEND_MOUSE_MOVE;
	commandMap["MLB"] = COMMAND::SEND_MOUSE_LEFT_BUTTON;
	commandMap["MMB"] = COMMAND::SEND_MOUSE_MIDDLE_BUTTON;
	commandMap["MRB"] = COMMAND::SEND_MOUSE_RIGHT_BUTTON;
	commandMap["MWU"] = COMMAND::SEND_MOUSE_WHEEL_UP;
	commandMap["MWD"] = COMMAND::SEND_MOUSE_WHEEL_DOWN;
	commandMap["SCA"] = COMMAND::SET_SCALE_DISPLAY;
	commandMap["RCN"] = COMMAND::SET_RECONNECT_SESSION;
	commandMap["ECD"] = COMMAND::SET_IMAGE_ENCODING;
	commandMap["QLT"] = COMMAND::SET_IMAGE_QUALITY;
	commandMap["QNT"] = COMMAND::SET_IMAGE_QUANTITY;
	commandMap["AUD"] = COMMAND::SET_AUDIO_FORMAT;
	commandMap["BIT"] = COMMAND::SET_AUDIO_BITRATE;
	commandMap["SSC"] = COMMAND::SET_SCREENSHOT_CONFIG;
	commandMap["SS1"] = COMMAND::START_TAKING_SCREENSHOTS;
	commandMap["SS0"] = COMMAND::STOP_TAKING_SCREENSHOTS;
	commandMap["SCN"] = COMMAND::TAKE_SCREENSHOT;
	commandMap["FSU"] = COMMAND::REQUEST_FULLSCREEN_UPDATE;
	commandMap["CLP"] = COMMAND::SEND_LOCAL_CLIPBOARD;
	commandMap["CLO"] = COMMAND::CLOSE_CLIENT;

	// inputs
	myrtille->processInputs = true;

	// updates
	myrtille->imageEncoding = (int)IMAGE_ENCODING::AUTO;
	myrtille->imageQuality = (int)IMAGE_QUALITY::HIGH;
	myrtille->imageQuantity = IMAGE_COUNT_SAMPLING_RATE;
	myrtille->imageCount = 0;
	myrtille->imageIdx = 0;

	// updates buffer
	myrtille->imageBuffer.top = -1;
	myrtille->imageBuffer.left = -1;
	myrtille->imageBuffer.bottom = -1;
	myrtille->imageBuffer.right = -1;

	// display
	myrtille->scaleDisplay = false;
	myrtille->clientWidth = wfc->context.settings->DesktopWidth;
	myrtille->clientHeight = wfc->context.settings->DesktopHeight;
	myrtille->aspectRatio = (float)myrtille->clientWidth / (float)myrtille->clientHeight;

	// audio
	myrtille->audioFormat = (int)AUDIO_FORMAT::MP3;
	myrtille->audioBitrate = 128;

	// screenshot
	myrtille->screenshotIntervalSecs = 60;
	myrtille->screenshotFormat = (int)IMAGE_FORMAT::PNG;
	myrtille->screenshotPath = "";
	myrtille->screenshotEnabled = false;

	// clipboard
	myrtille->clipboardText = L"";

	// if the local (gateway) clipboard is not set, the rdp server won't enable the paste action
	// this is a problem because, even if the client (browser) clipboard is received and its value stored,
	// it won't be possible to paste it, thus retrieve it and render it! :/
	
	// a workaround is to set an empty value into the clipboard in order to enable the paste action
	// pasting an empty value just does nothing and it's quite reasonable to have the paste action enabled for clipboard synchronization
	// once the client clipboard is received, the paste action will trigger its retrieval and rendering!

	// TODO: find a better way to handle that...

	if (OpenClipboard(0))
	{
		HANDLE hData = GetClipboardData(CF_UNICODETEXT);
		if (!hData)
		{
			size_t bytesPerChar = sizeof(wchar_t);
			size_t size = (wcslen(myrtille->clipboardText.c_str()) + 1) * bytesPerChar;
			HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
			memcpy(GlobalLock(hMem), myrtille->clipboardText.c_str(), size);
			GlobalUnlock(hMem);
			SetClipboardData(CF_UNICODETEXT, hMem);
		}
		CloseClipboard();
	}

	// GDI+
	GdiplusStartupInput gdiplusStartupInput;
	Gdiplus::GdiplusStartup(&myrtille->gdiplusToken, &gdiplusStartupInput, NULL);

	getEncoderClsid(L"image/png", &myrtille->pngClsid);
	getEncoderClsid(L"image/jpeg", &myrtille->jpgClsid);

	int quality = (int)IMAGE_QUALITY::HIGH;
	EncoderParameters encoderParameters;
	encoderParameters.Count = 1;
	encoderParameters.Parameter[0].Guid = EncoderQuality;
	encoderParameters.Parameter[0].Type = EncoderParameterValueTypeLong;
	encoderParameters.Parameter[0].NumberOfValues = 1;
	encoderParameters.Parameter[0].Value = &quality;

	myrtille->encoderParameters = encoderParameters;

	// WebP
	float webpQuality = (int)IMAGE_QUALITY::HIGH;
	WebPConfig webpConfig;
	WebPConfigPreset(&webpConfig, WEBP_PRESET_PICTURE, webpQuality);

	// override preset settings below, if needed

	//webpConfig.quality = webpQuality;
	//webpConfig.target_size = 0;
	//webpConfig.target_PSNR = 0.;
	//webpConfig.method = 3;
	//webpConfig.sns_strength = 30;
	//webpConfig.filter_strength = 20;
	//webpConfig.filter_sharpness = 3;
	//webpConfig.filter_type = 0;
	//webpConfig.partitions = 0;
	//webpConfig.segments = 2;
	//webpConfig.pass = 1;
	//webpConfig.show_compressed = 0;
	//webpConfig.preprocessing = 0;
	//webpConfig.autofilter = 0;
	//webpConfig.alpha_compression = 0;
	//webpConfig.partition_limit = 0;

	myrtille->webpConfig = webpConfig;
}

void wf_myrtille_stop(wfContext* wfc)
{
	if (wfc->context.settings->MyrtilleSessionId == NULL)
		return;

	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	// setting the exit condition for the process inputs loop is not enough
	// "ReadFile" is synchronous; it waits for something to read on the file (or pipe) then return it
	// problem is, it can wait for a long time if there is nothing to read!
	// possibly, it will timeout or the pipe will be closed so it will return; but this is not something reliable...
	// a better option would be to use an asynchronous call with an overlapped structure,
	// but this is a more complex scenario and must be synchronized with the gateway (acting as pipes server),
	// while we want simple FIFO queues to process the user inputs, display updates and other notifications in order

	//myrtille->processInputs = false;

	// also, closing the pipes at this step may result in errors if there are read/write operations going on in their own threads
	// this will result in setting the exit condition for the process inputs loop, with the same comments as above
	// additionally, the cleanup sequence may run twice, which could raise even more errors
	// and finally have an unknown exit code for wfreerdp when it could be a simple disconnect from the start!
	// the pipes will be anyway closed and released by the gateway (acting as pipes server), so there is nothing to worry from this side

	//CloseHandle(myrtille->inputsPipe);
	//CloseHandle(myrtille->updatesPipe);
	//CloseHandle(myrtille->audioPipe);

	Gdiplus::GdiplusShutdown(myrtille->gdiplusToken);
	fclose(stdout);
	fclose(stderr);
	UINT32 exitCode = freerdp_get_last_error((rdpContext*)wfc);
	exit(exitCode);
}

HANDLE wf_myrtille_connect(wfContext* wfc)
{
	if (wfc->context.settings->MyrtilleSessionId == NULL)
		return NULL;

	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	// connect pipes
	DWORD result = connectRemoteSessionPipes(wfc);
	if (result != 0)
	{
		WLog_ERR(TAG, "wf_myrtille_connect: failed to connect session %s with error %d", wfc->context.settings->MyrtilleSessionId, result);
		return NULL;
	}

	WLog_INFO(TAG, "wf_myrtille_connect: connected session %s", wfc->context.settings->MyrtilleSessionId);

	// process inputs
	HANDLE thread;
	if ((thread = CreateThread(NULL, 0, processInputsPipe, (void*)wfc, 0, &wfc->mainThreadId)) == NULL)
	{
		WLog_ERR(TAG, "wf_myrtille_connect: CreateThread failed for processInputsPipe with error %d", GetLastError());
		return NULL;
	}

	return thread;
}

void wf_myrtille_send_screen(wfContext* wfc, bool adaptive)
{
	if (wfc->context.settings->MyrtilleSessionId == NULL)
		return;

	if (!wfc->primary || !wfc->primary->hdc)
		return;

	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	// --------------------------- retrieve the fullscreen bitmap ---------------------------------

	int cw, ch, dw, dh;
	cw = myrtille->clientWidth;
	ch = myrtille->clientHeight;
	dw = wfc->context.settings->DesktopWidth;
	dh = wfc->context.settings->DesktopHeight;

	HDC hdc = CreateCompatibleDC(wfc->primary->hdc);
	HBITMAP hbmp = CreateCompatibleBitmap(wfc->primary->hdc, myrtille->scaleDisplay ? cw : dw, myrtille->scaleDisplay ? ch : dh);
	SelectObject(hdc, hbmp);

	if (!myrtille->scaleDisplay || (cw == dw && ch == dh))
	{
		BitBlt(hdc, 0, 0, dw, dh, wfc->primary->hdc, 0, 0, SRCCOPY);
	}
	else
	{
		SetStretchBltMode(hdc, HALFTONE);
		SetBrushOrgEx(hdc, 0, 0, NULL);
		StretchBlt(hdc, 0, 0, cw, ch, wfc->primary->hdc, 0, 0, dw, dh, SRCCOPY);
	}

	// debug, if needed
	//WLog_INFO(TAG, "wf_myrtille_send_screen");

	Gdiplus::Bitmap *bmpScreen = Gdiplus::Bitmap::FromHBITMAP(hbmp, (HPALETTE)0);

	// ---------------------------  process it ----------------------------------------------------

	processImage(wfc, bmpScreen, 0, 0, myrtille->scaleDisplay ? cw : dw, myrtille->scaleDisplay ? ch : dh, true, adaptive);

	// ---------------------------  cleanup -------------------------------------------------------

	delete bmpScreen;
	bmpScreen = NULL;

	DeleteObject(hbmp);
	hbmp = NULL;

	DeleteDC(hdc);
	hdc = NULL;
}

void wf_myrtille_send_region(wfContext* wfc, RECT region)
{
	if (wfc->context.settings->MyrtilleSessionId == NULL)
		return;

	if (!wfc->primary || !wfc->primary->hdc)
		return;

	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	// --------------------------- consistency check ----------------------------------------------

	if (region.left < 0 || region.left > wfc->context.settings->DesktopWidth || region.top < 0 || region.top > wfc->context.settings->DesktopHeight ||
		region.right < 0 || region.right > wfc->context.settings->DesktopWidth || region.bottom < 0 || region.bottom > wfc->context.settings->DesktopHeight ||
		region.left > region.right || region.top > region.bottom)
		return;

	// --------------------------- ips regulator --------------------------------------------------

	// only applies to region updates (not to fullscreen or cursor)

	if (myrtille->imageCount == INT_MAX)
	{
		myrtille->imageCount = 0;
	}

	myrtille->imageCount++;

	if (myrtille->imageQuantity == 5 ||
		myrtille->imageQuantity == 10 ||
		myrtille->imageQuantity == 20 ||
		myrtille->imageQuantity == 25 ||
		myrtille->imageQuantity == 50)
	{
		if (myrtille->imageBuffer.top == -1 || region.top < myrtille->imageBuffer.top)
			myrtille->imageBuffer.top = region.top;

		if (myrtille->imageBuffer.left == -1 || region.left < myrtille->imageBuffer.left)
			myrtille->imageBuffer.left = region.left;

		if (myrtille->imageBuffer.bottom == -1 || region.bottom > myrtille->imageBuffer.bottom)
			myrtille->imageBuffer.bottom = region.bottom;

		if (myrtille->imageBuffer.right == -1 || region.right > myrtille->imageBuffer.right)
			myrtille->imageBuffer.right = region.right;

		if (myrtille->imageCount % (100 / myrtille->imageQuantity) != 0)
			return;

		if (myrtille->imageBuffer.top != -1 &&
			myrtille->imageBuffer.left != -1 &&
			myrtille->imageBuffer.bottom != -1 &&
			myrtille->imageBuffer.right != -1)
		{
			region.top = myrtille->imageBuffer.top;
			region.left = myrtille->imageBuffer.left;
			region.bottom = myrtille->imageBuffer.bottom;
			region.right = myrtille->imageBuffer.right;
		}

		myrtille->imageBuffer.top = -1;
		myrtille->imageBuffer.left = -1;
		myrtille->imageBuffer.bottom = -1;
		myrtille->imageBuffer.right = -1;
	}

	// --------------------------- extract the consolidated region --------------------------------

	int cw, ch, dw, dh;
	cw = myrtille->clientWidth;
	ch = myrtille->clientHeight;
	dw = wfc->context.settings->DesktopWidth;
	dh = wfc->context.settings->DesktopHeight;

	HDC hdc = CreateCompatibleDC(wfc->primary->hdc);
	HBITMAP	hbmp;
		
	if (!myrtille->scaleDisplay || (cw == dw && ch == dh))
	{
		hbmp = CreateCompatibleBitmap(wfc->primary->hdc, region.right - region.left, region.bottom - region.top);
		SelectObject(hdc, hbmp);

		BitBlt(
			hdc,
			0,
			0,
			region.right - region.left,
			region.bottom - region.top,
			wfc->primary->hdc,
			region.left,
			region.top,
			SRCCOPY);
	}
	else
	{
		hbmp = CreateCompatibleBitmap(wfc->primary->hdc, (region.right - region.left) * cw / dw, (region.bottom - region.top) * ch / dh);
		SelectObject(hdc, hbmp);

		SetStretchBltMode(hdc, HALFTONE);
		SetBrushOrgEx(hdc, 0, 0, NULL);
		StretchBlt(
			hdc,
			0,
			0,
			(region.right - region.left) * cw / dw,
			(region.bottom - region.top) * ch / dh,
			wfc->primary->hdc,
			region.left,
			region.top,
			region.right - region.left,
			region.bottom - region.top,
			SRCCOPY);

		// scale region
		region.left = region.left * cw / dw;
		region.top = region.top * ch / dh;
		region.right = region.right * cw / dw;
		region.bottom = region.bottom * ch / dh;
	}

	// debug, if needed
	//WLog_INFO(TAG, "wf_myrtille_send_region: left:%i, top:%i, right:%i, bottom:%i", region.left, region.top, region.right, region.bottom);

	Gdiplus::Bitmap *bmpRegion = Gdiplus::Bitmap::FromHBITMAP(hbmp, (HPALETTE)0);

	// ---------------------------  process it ----------------------------------------------------

	processImage(wfc, bmpRegion, region.left, region.top, region.right, region.bottom, false, false);

	// ---------------------------  cleanup -------------------------------------------------------

	delete bmpRegion;
	bmpRegion = NULL;

	DeleteObject(hbmp);
	hbmp = NULL;

	DeleteDC(hdc);
	hdc = NULL;
}

void wf_myrtille_send_cursor(wfContext* wfc)
{
	if (wfc->context.settings->MyrtilleSessionId == NULL)
		return;

	if (!wfc->primary || !wfc->primary->hdc)
		return;

	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	// --------------------------- set cursor -----------------------------------------------------

	// for the record, I tried several ways to handle the alpha channel (transparency) and ended with the following solutions:
	// - looping all cursor pixels, making the mask transparent: GetPixel/SetPixel: GDI+, working, but a bit slow and unsafe as the bitmap data is not locked
	// - looping all cursor pixels, making the mask transparent: LockBits/UnlockBits: GDI+, working, fast and safe as the bitmap data is locked (current solution)
	// - cursor to bitmap: GDI+, working, but loose the alpha channel when using Gdiplus::Bitmap::FromHBITMAP. See https://stackoverflow.com/questions/4451839/how-to-render-a-transparent-cursor-to-bitmap-preserving-alpha-channel
	// - hbitmap to bitmap: memcpy instead of Gdiplus::Bitmap::FromHBITMAP: GDI+, working partially, cursors are bottom/up and some are blackened. See https://stackoverflow.com/questions/335273/how-to-create-a-gdiplusbitmap-from-an-hbitmap-retaining-the-alpha-channel-inf
	// - TransparentBlt: GDI, working, but also loose the alpha channel when passed to a GDI+ bitmap using Gdiplus::Bitmap::FromHBITMAP
	// - AlphaBlend: same as for TransparentBlt

	// set a display context and a bitmap to store the mouse cursor
	HDC hdc = CreateCompatibleDC(wfc->primary->hdc);
	HBITMAP hbmp = CreateCompatibleBitmap(wfc->primary->hdc, GetSystemMetrics(SM_CXCURSOR), GetSystemMetrics(SM_CYCURSOR));
	SelectObject(hdc, hbmp);

	// set a colored mask, so that it will be possible to identify parts of the cursor that should be made transparent
	HBRUSH hbrush = CreateSolidBrush(RGB(0, 0, 255));

	// draw the cursor on the display context
	DrawIconEx(hdc, 0, 0, (HICON)wfc->cursor, 0, 0, 0, hbrush, DI_MASK);

	// cursor bitmap
	Gdiplus::Bitmap *bmpCursor = Gdiplus::Bitmap::FromHBITMAP(hbmp, (HPALETTE)0);

	// extract the relevant cursor image. also, transparency requires ARGB format
	Gdiplus::Bitmap *bmpTransparentCursor = new Gdiplus::Bitmap(bmpCursor->GetWidth(), bmpCursor->GetHeight(), PixelFormat32bppARGB);
	Gdiplus::Graphics gfxTransparentCursor(bmpTransparentCursor);
	gfxTransparentCursor.DrawImage(bmpCursor, 0, 0, bmpCursor->GetWidth(), bmpCursor->GetHeight());

	// lock the cursor while manipulating it
	Gdiplus::BitmapData* bmpData = new Gdiplus::BitmapData();
	Gdiplus::Rect* rect = new Gdiplus::Rect(0, 0, bmpTransparentCursor->GetWidth(), bmpTransparentCursor->GetHeight());
	bmpTransparentCursor->LockBits(rect, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppARGB, bmpData);

	UINT* bmpBits = (UINT*)bmpData->Scan0;
	int stride = bmpData->Stride;

	bool bmpBitsTransparent = false;
	bool bmpBitsColor = false;

	// make the cursor transparent
	for (int x = 0; x < bmpTransparentCursor->GetWidth(); x++)
	{
		for (int y = 0; y < bmpTransparentCursor->GetHeight(); y++)
		{
			UINT color = bmpBits[y * stride / 4 + x];

			int b = color & 0xff;
			int g = (color & 0xff00) >> 8;
			int r = (color & 0xff0000) >> 16;
			int a = (color & 0xff000000) >> 24;

			// replace the blue (mask) color by transparent one
			if (r == 0 && g == 0 && b == 255)
			{
				bmpBits[y * stride / 4 + x] = 0x00ffffff;
				bmpBitsTransparent = true;
			}
			else
			{
				// for some reason, some cursors (like the text one) are yellow instead of black ?! switching color...
				if (r == 255 && g == 255 && b == 0)
				{
					bmpBits[y * stride / 4 + x] = 0xff000000;
				}
				bmpBitsColor = true;
			}
		}
	}

	// unlock the cursor
	bmpTransparentCursor->UnlockBits(bmpData);

	// send the cursor only if it has a transparent mask and isn't empty
	if (bmpBitsTransparent && bmpBitsColor)
	{
		// convert into PNG
		IStream* pngStream;
		CreateStreamOnHGlobal(NULL, TRUE, &pngStream);
		bmpTransparentCursor->Save(pngStream, &myrtille->pngClsid);

		STATSTG statstg;
		pngStream->Stat(&statstg, STATFLAG_DEFAULT);
		ULONG pngSize = (ULONG)statstg.cbSize.LowPart;

		// retrieve cursor info
		ICONINFO cursorInfo;
		GetIconInfo((HICON)wfc->cursor, &cursorInfo);

		if (myrtille->imageIdx == INT_MAX)
		{
			myrtille->imageIdx = 0;
		}

		// send
		if (pngStream != NULL && pngSize > 0)
		{
			sendImage(
				wfc,
				bmpTransparentCursor,
				++myrtille->imageIdx,
				cursorInfo.xHotspot,
				cursorInfo.yHotspot,
				bmpTransparentCursor->GetWidth(),
				bmpTransparentCursor->GetHeight(),
				(int)IMAGE_FORMAT::CUR,
				(int)IMAGE_QUALITY::HIGHEST,
				pngStream,
				pngSize,
				false);
		}

		// cleanup
		DeleteObject(cursorInfo.hbmMask);
		DeleteObject(cursorInfo.hbmColor);

		if (pngStream != NULL)
		{
			pngStream->Release();
			pngStream = NULL;
		}
	}

	delete rect;
	rect = NULL;

	delete bmpData;
	bmpData = NULL;

	delete bmpTransparentCursor;
	bmpTransparentCursor = NULL;

	delete bmpCursor;
	bmpCursor = NULL;

	DeleteObject(hbrush);
	hbrush = NULL;

	DeleteObject(hbmp);
	hbmp = NULL;

	DeleteDC(hdc);
	hdc = NULL;
}

void wf_myrtille_read_client_clipboard(wfContext* wfc, const wchar_t** text, size_t* size)
{
	if (wfc->context.settings->MyrtilleSessionId == NULL)
		return;

	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	*text = myrtille->clipboardText.c_str();

	// unicode is 2 bytes (16 bits) per character (UTF-16LE)
	size_t bytesPerChar = sizeof(wchar_t);

	// clipboard length + null terminator size in bytes
	*size = (myrtille->clipboardText.length() + 1) * bytesPerChar;
}

void wf_myrtille_read_server_clipboard(wfContext* wfc)
{
	if (wfc->context.settings->MyrtilleSessionId == NULL)
		return;

	CLIPRDR_FORMAT_DATA_REQUEST formatDataRequest;
	formatDataRequest.requestedFormatId = CF_UNICODETEXT;
	wfc->cliprdr->ClientFormatDataRequest(wfc->cliprdr, &formatDataRequest);
}

void wf_myrtille_send_server_clipboard(wfContext* wfc, BYTE* data, size_t size)
{
	if (wfc->context.settings->MyrtilleSessionId == NULL)
		return;

	// unicode is 2 bytes (16 bits) per character (UTF-16LE)
	size_t bytesPerChar = sizeof(wchar_t);
	
	// number of characters into the clipboard
	// subtract the null terminator
	size_t clipboardLength = (size / bytesPerChar) - 1;

	// if the clipboard is larger than allowed, truncate it
	std::wstring clipboardText(reinterpret_cast<wchar_t*>(data), clipboardLength <= CLIPBOARD_MAX_LENGTH_SEND ? clipboardLength : CLIPBOARD_MAX_LENGTH_SEND);

	std::wstringstream wss;
	wss << L"clipboard|" << clipboardText.c_str();

	if (clipboardLength > CLIPBOARD_MAX_LENGTH_SEND)
	{
		wss << L"--- TRUNCATED ---";
	}

	sendMessage(wfc, wss.str());
}

void wf_myrtille_send_printjob(wfContext* wfc, wchar_t* printJobName)
{
	if (wfc->context.settings->MyrtilleSessionId == NULL)
		return;

	std::wstringstream wss;
	wss << L"printjob|" << printJobName << L".pdf";

	sendMessage(wfc, wss.str());
}

void wf_myrtille_send_audio(wfContext* wfc, const BYTE* data, size_t size)
{
	if (wfc->context.settings->MyrtilleSessionId == NULL)
		return;

	sendAudio(wfc, data, size);
}

int getEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
	UINT  num = 0;          // number of image encoders
	UINT  size = 0;         // size of the image encoder array in bytes

	ImageCodecInfo* pImageCodecInfo = NULL;

	GetImageEncodersSize(&num, &size);
	if (size == 0)
		return -1;  // Failure

	pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
	if (pImageCodecInfo == NULL)
		return -1;  // Failure

	GetImageEncoders(num, size, pImageCodecInfo);

	for (UINT j = 0; j < num; ++j)
	{
		if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0)
		{
			*pClsid = pImageCodecInfo[j].Clsid;
			free(pImageCodecInfo);
			return j;  // Success
		}
	}

	free(pImageCodecInfo);
	return -1;  // Failure
}

std::string getCurrentTime()
{
	SYSTEMTIME time;
	GetLocalTime(&time);

	WORD year = time.wYear;
	WORD month = time.wMonth;
	WORD day = time.wDay;
	WORD hour = time.wHour;
	WORD minute = time.wMinute;
	WORD second = time.wSecond;
	WORD millisecond = time.wMilliseconds;

	// YYYY-MM-DD hh:mm:ss,fff
	std::stringstream ss;
	ss << year << "-" <<
		(month < 10 ? "0" : "") << month << "-" <<
		(day < 10 ? "0" : "") << day << " " <<
		(hour < 10 ? "0" : "") << hour << ":" <<
		(minute < 10 ? "0" : "") << minute << ":" <<
		(second < 10 ? "0" : "") << second << "," <<
		(millisecond < 100 ? (millisecond < 10 ? "00" : "0") : "") << millisecond;

	return ss.str();
}

std::string createLogDirectory()
{
	std::string path = "";

	// retrieve the module file name
	wchar_t* buffer = new wchar_t[MAX_PATH];
	if (GetModuleFileName(NULL, buffer, MAX_PATH))
	{
		// extract the parent folder
		char moduleFilename[MAX_PATH];
		wcstombs(moduleFilename, buffer, MAX_PATH);
		std::string::size_type pos = std::string(moduleFilename).find_last_of("\\/");
		std::string currentdir = std::string(moduleFilename).substr(0, pos);
		pos = currentdir.find_last_of("\\/");
		std::string parentdir = currentdir.substr(0, pos);

		// log folder
		std::stringstream ss;
		ss << parentdir << "\\log";
		path = ss.str();
		std::wstring ws = s2ws(path);
		LPCWSTR logDir = ws.c_str();

		// create the log folder if not already exists
		if (!CreateDirectory(logDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
		{
			WLog_ERR(TAG, "createLogDirectory: create directory failed with error %d", GetLastError());
			path = "";
		}
	}
	else
	{
		WLog_ERR(TAG, "createLogDirectory: can't retrieve the module filename %d", GetLastError());
	}

	// cleanup
	delete[] buffer;

	return path;
}

std::wstring s2ws(const std::string& s)
{
	int len;
	int slength = (int)s.length() + 1;
	len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, 0, 0);
	wchar_t* buf = new wchar_t[len];
	MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, buf, len);
	std::wstring r(buf);
	delete[] buf;
	return r;
}

DWORD connectRemoteSessionPipes(wfContext* wfc)
{
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	// connect inputs pipe (commands)
	if ((myrtille->inputsPipe = connectRemoteSessionPipe(wfc, "inputs", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH)) == INVALID_HANDLE_VALUE)
	{
		WLog_ERR(TAG, "connectRemoteSessionPipes: connect failed for inputs pipe with error %d", GetLastError());
		return GetLastError();
	}

	// connect updates pipe (region, fullscreen and cursor updates)
	if ((myrtille->updatesPipe = connectRemoteSessionPipe(wfc, "updates", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH)) == INVALID_HANDLE_VALUE)
	{
		WLog_ERR(TAG, "connectRemoteSessionPipes: connect failed for updates pipe with error %d", GetLastError());
		return GetLastError();
	}

	// connect audio pipe (requires audio supported and enabled on the remote server)
	if ((myrtille->audioPipe = connectRemoteSessionPipe(wfc, "audio", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_FLAG_WRITE_THROUGH)) == INVALID_HANDLE_VALUE)
	{
		WLog_ERR(TAG, "connectRemoteSessionPipes: connect failed for audio pipe with error %d", GetLastError());
		return GetLastError();
	}

	return 0;
}

HANDLE connectRemoteSessionPipe(wfContext* wfc, std::string pipeName, DWORD accessMode, DWORD shareMode, DWORD flags)
{
	std::stringstream ss;
	ss << "\\\\.\\pipe\\remotesession_" << wfc->context.settings->MyrtilleSessionId << "_" << pipeName;
	std::string s = ss.str();
	std::wstring ws = s2ws(s);
	LPCWSTR pipeFileName = ws.c_str();

	return CreateFile(pipeFileName, accessMode, shareMode, NULL, OPEN_EXISTING, flags, NULL);
}

std::string createRemoteSessionDirectory(wfContext* wfc)
{
	std::string path = "";

	std::string logDirectoryPath = createLogDirectory();
	if (logDirectoryPath != "")
	{
		std::stringstream ss;
		ss << logDirectoryPath << "\\remotesession_" << wfc->context.settings->MyrtilleSessionId << "." << GetCurrentProcessId();
		path = ss.str();
		std::wstring ws = s2ws(path);
		LPCWSTR remoteSessionDir = ws.c_str();

		if (!CreateDirectory(remoteSessionDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
		{
			WLog_ERR(TAG, "createRemoteSessionDirectory: CreateDirectory failed with error %d", GetLastError());
			path = "";
		}
	}

	return path;
}

std::vector<std::string> &split(const std::string &s, char delim, std::vector<std::string> &elems)
{
	std::stringstream ss(s);
	std::string item;
	while (std::getline(ss, item, delim)) {
		elems.push_back(item);
	}
	return elems;
}

std::vector<std::string> split(const std::string &s, char delim)
{
	std::vector<std::string> elems;
	return split(s, delim, elems);
}

DWORD WINAPI processInputsPipe(LPVOID lpParameter)
{
	wfContext* wfc = (wfContext*)lpParameter;
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	byte* buffer = NULL;
	DWORD bytesToRead;
	DWORD bytesRead;
	bool sizeHeader = true;

	// main loop
	while (myrtille->processInputs)
	{
		if (sizeHeader)
		{
			bytesToRead = 4;
		}

		buffer = new byte[bytesToRead];

		// wait for inputs pipe event
		if (ReadFile(myrtille->inputsPipe, buffer, bytesToRead, &bytesRead, NULL) == 0)
		{
			switch (GetLastError())
			{
				case ERROR_INVALID_HANDLE:
					WLog_ERR(TAG, "processInputsPipe: ReadFile failed with error ERROR_INVALID_HANDLE");
					break;

				case ERROR_PIPE_NOT_CONNECTED:
					WLog_ERR(TAG, "processInputsPipe: ReadFile failed with error ERROR_PIPE_NOT_CONNECTED");
					break;

				case ERROR_PIPE_BUSY:
					WLog_ERR(TAG, "processInputsPipe: ReadFile failed with error ERROR_PIPE_BUSY");
					break;

				case ERROR_BAD_PIPE:
					WLog_ERR(TAG, "processInputsPipe: ReadFile failed with error ERROR_BAD_PIPE");
					break;

				case ERROR_BROKEN_PIPE:
					WLog_ERR(TAG, "processInputsPipe: ReadFile failed with error ERROR_BROKEN_PIPE");
					break;

				default:
					WLog_ERR(TAG, "processInputsPipe: ReadFile failed with error %d", GetLastError());
					break;
			}

			// pipe problem; exit
			myrtille->processInputs = false;
		}
		else if (bytesRead > 0)
		{
			if (sizeHeader)
			{
				bytesToRead = bytesToInt32(buffer);
			}
			else
			{
				std::string message(reinterpret_cast<char*>(buffer), bytesRead);

				COMMAND command = commandMap[message.substr(0, 3)];
				std::string commandArgs = message.substr(3, message.length() - 3);

				// for safety sake, don't log passwords
				if (command != COMMAND::SEND_USER_PASSWORD)
				{
					if (command != COMMAND::SEND_LOCAL_CLIPBOARD)
					{
						WLog_INFO(TAG, "processInputsPipe: %s", message.c_str());
					}
					else
					{
						std::stringstream ss;

						// only log the first 100 characters (disable as needed, if a security issue)
						// unicode characters are not preserved into the console output (stdout)
						if (commandArgs.length() <= CLIPBOARD_MAX_LENGTH_LOG)
						{
							ss << message.substr(0, 3).c_str() << commandArgs.c_str();
						}
						else
						{
							ss << message.substr(0, 3).c_str() << commandArgs.substr(0, 100).c_str() << "...";
						}

						WLog_INFO(TAG, "processInputsPipe: %s", ss.str().c_str());
					}
				}

				std::vector<std::string> args;
				WCHAR* clipboardText = NULL;

				switch (command)
				{
					// server address
					case COMMAND::SEND_SERVER_ADDRESS:

						const char* p;
						const char* p2;
						int length;

						free(wfc->context.settings->ServerHostname);
						wfc->context.settings->ServerHostname = NULL;
						p = strchr(commandArgs.c_str(), '[');

						/* ipv4 */
						if (!p)
						{
							p = strchr(commandArgs.c_str(), ':');

							if (p)
							{
								length = (int)(p - commandArgs.c_str());
								wfc->context.settings->ServerPort = atoi(&p[1]);

								if (wfc->context.settings->ServerHostname = (char*)calloc(length + 1UL, sizeof(char)))
								{
									strncpy(wfc->context.settings->ServerHostname, commandArgs.c_str(), length);
									wfc->context.settings->ServerHostname[length] = '\0';
								}
							}
							else
							{
								wfc->context.settings->ServerHostname = _strdup(commandArgs.c_str());
							}
						}
						else /* ipv6 */
						{
							p2 = strchr(commandArgs.c_str(), ']');

							/* valid [] ipv6 addr found */
							if (p2)
							{
								length = p2 - p;

								if (wfc->context.settings->ServerHostname = (char*)calloc(length, sizeof(char)))
								{
									strncpy(wfc->context.settings->ServerHostname, p + 1, length - 1);

									if (*(p2 + 1) == ':')
									{
										wfc->context.settings->ServerPort = atoi(&p2[2]);
									}
								}
							}
						}
						break;

					// hyper-v vm guid
					case COMMAND::SEND_VM_GUID:
						wfc->context.settings->VmConnectMode = TRUE;
						wfc->context.settings->ServerPort = 2179;
						wfc->context.settings->NegotiateSecurityLayer = FALSE;
						wfc->context.settings->SendPreconnectionPdu = TRUE;
						free(wfc->context.settings->PreconnectionBlob);
						wfc->context.settings->PreconnectionBlob = _strdup(commandArgs.c_str());
						break;

					// user domain
					case COMMAND::SEND_USER_DOMAIN:
						free(wfc->context.settings->Domain);
						wfc->context.settings->Domain = _strdup(commandArgs.c_str());
						break;

					// user name
					case COMMAND::SEND_USER_NAME:
						char* user;
						user = _strdup(commandArgs.c_str());
						if (user)
						{
							free(wfc->context.settings->Username);
							if (!wfc->context.settings->Domain && user)
							{
								free(wfc->context.settings->Domain);
								freerdp_parse_username(user, &wfc->context.settings->Username, &wfc->context.settings->Domain);
								free(user);
							}
							else
								wfc->context.settings->Username = user;
						}
						break;

					// user password
					case COMMAND::SEND_USER_PASSWORD:
						free(wfc->context.settings->Password);
						wfc->context.settings->Password = _strdup(commandArgs.c_str());
						break;

					// start program
					case COMMAND::SEND_START_PROGRAM:
						free(wfc->context.settings->AlternateShell);
						wfc->context.settings->AlternateShell = _strdup(commandArgs.c_str());
						break;

					// connect rdp
					case COMMAND::CONNECT_CLIENT:
						DWORD threadId;
						if (CreateThread(NULL, 0, wf_client_thread, (void*)wfc->context.instance, 0, &threadId) == NULL)
						{
							WLog_ERR(TAG, "processInputsPipe: CreateThread failed for wf_client_thread with error %d", GetLastError());
						}
						break;

					// browser resize
					case COMMAND::SEND_BROWSER_RESIZE:
						if (myrtille->scaleDisplay)
						{
							args = split(commandArgs, '|');
							if (args.size() == 2)
							{
								processResizeDisplay(wfc, args[0] == "1", args[1]);
							}
							sendMessage(wfc, L"reload");
						}
						break;

					// browser pulse
				    case COMMAND::SEND_BROWSER_PULSE:
					    // this command is handled by the gateway to monitor browser activity
					    break;

					// keystroke
					case COMMAND::SEND_KEY_UNICODE:
					case COMMAND::SEND_KEY_SCANCODE:

						args = split(commandArgs, '-');
						if (args.size() >= 2)
						{
							std::string keyCode = args[0];
							std::string pressed = args[1];

							// character key
							if (command == COMMAND::SEND_KEY_UNICODE)
							{
								if (wfc->context.input->UnicodeKeyboardEvent)
									wfc->context.input->UnicodeKeyboardEvent(wfc->context.input, (pressed == "1" ? KBD_FLAGS_DOWN : KBD_FLAGS_RELEASE), atoi(keyCode.c_str()));
							}
							// non character key
							else if (args.size() == 3)
							{
								std::string extend = args[2];
								if (wfc->context.input->KeyboardEvent)
									wfc->context.input->KeyboardEvent(wfc->context.input, (extend == "1" ? KBD_FLAGS_EXTENDED : 0) | (pressed == "1" ? KBD_FLAGS_DOWN : KBD_FLAGS_RELEASE), atoi(keyCode.c_str()));
							}
						}
						break;

					// mouse move
					case COMMAND::SEND_MOUSE_MOVE:
						processMouseInput(wfc, commandArgs, PTR_FLAGS_MOVE);
						break;

					// mouse left button
					case COMMAND::SEND_MOUSE_LEFT_BUTTON:
						processMouseInput(wfc, commandArgs.substr(1, commandArgs.length() - 1), commandArgs.substr(0, 1) == "0" ? PTR_FLAGS_BUTTON1 : PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON1);
						break;

					// mouse middle button
					case COMMAND::SEND_MOUSE_MIDDLE_BUTTON:
						processMouseInput(wfc, commandArgs.substr(1, commandArgs.length() - 1), commandArgs.substr(0, 1) == "0" ? PTR_FLAGS_BUTTON3 : PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON3);
						break;

					// mouse right button
					case COMMAND::SEND_MOUSE_RIGHT_BUTTON:
						processMouseInput(wfc, commandArgs.substr(1, commandArgs.length() - 1), commandArgs.substr(0, 1) == "0" ? PTR_FLAGS_BUTTON2 : PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON2);
						break;

					// mouse wheel up
					case COMMAND::SEND_MOUSE_WHEEL_UP:
						processMouseInput(wfc, commandArgs, PTR_FLAGS_WHEEL | 0x0078);
						break;

					// mouse wheel down
					case COMMAND::SEND_MOUSE_WHEEL_DOWN:
						processMouseInput(wfc, commandArgs, PTR_FLAGS_WHEEL | PTR_FLAGS_WHEEL_NEGATIVE | 0x0088);
						break;

					// scale display
					case COMMAND::SET_SCALE_DISPLAY:
						myrtille->scaleDisplay = commandArgs != "0";
						if (myrtille->scaleDisplay)
						{
							args = split(commandArgs, '|');
							if (args.size() == 2)
							{
								processResizeDisplay(wfc, args[0] == "1", args[1]);
							}
						}
						sendMessage(wfc, L"reload");
						break;

					// reconnect session
					case COMMAND::SET_RECONNECT_SESSION:
						// there are methods into freerdp to handle session reconnection but there are some issues with them
						// reconnection is delegated to the gateway
						args = split(commandArgs, '|');
						if (args.size() == 2)
						{
							// reloading the page is optional
							if (args[1] == "1")
							{
								sendMessage(wfc, L"reload");
							}
						}
						break;

					// image encoding
					case COMMAND::SET_IMAGE_ENCODING:
						myrtille->imageEncoding = stoi(commandArgs);
						myrtille->imageQuality = (int)IMAGE_QUALITY::HIGH;
						break;

					// image quality is tweaked depending on the available client bandwidth (low available bandwidth = quality tweaked down)
					case COMMAND::SET_IMAGE_QUALITY:
						myrtille->imageQuality = stoi(commandArgs);
						break;

					// like for image quality, it's interesting to tweak down the image quantity if the available bandwidth gets too low
					// but skipping some images as well may also result in display inconsistencies, so be careful not to set it too low either (15 ips is a fair average in most cases)
					// to circumvent such inconsistencies, the combination with adaptive fullscreen update is nice because the whole screen is refreshed after a small user idle time (1,5 sec by default)
					case COMMAND::SET_IMAGE_QUANTITY:
						myrtille->imageQuantity = stoi(commandArgs);
						break;

					// audio encoding is actually done by the gateway (using NAudio/Lame)
					// it's not as critical as images for performance (should be used for notifications only)
					// if needed, have the audio encoding into wfreerdp (Lame support can be enabled from cmake option)
					case COMMAND::SET_AUDIO_FORMAT:
						myrtille->audioFormat = stoi(commandArgs);
						break;

					// audio bitrate
					case COMMAND::SET_AUDIO_BITRATE:
						myrtille->audioBitrate = stoi(commandArgs);
						break;

					// screenshot config
					case COMMAND::SET_SCREENSHOT_CONFIG:
						args = split(commandArgs, '|');
						if (args.size() == 3)
						{
							myrtille->screenshotIntervalSecs = stoi(args[0]);
							myrtille->screenshotFormat = stoi(args[1]);
							myrtille->screenshotPath = args[2];
						}
						break;

					// start/stop taking screenshots
					case COMMAND::START_TAKING_SCREENSHOTS:
					case COMMAND::STOP_TAKING_SCREENSHOTS:
						// these commands are handled by the gateway, by sending a TAKE_SCREENSHOT command periodically
						// that way, each screenshot taken can be traced individually
						break;

					// take screenshot
					case COMMAND::TAKE_SCREENSHOT:
						myrtille->screenshotEnabled = true;
						wf_myrtille_send_screen(wfc, true);
						break;

					// fullscreen update
					case COMMAND::REQUEST_FULLSCREEN_UPDATE:
						wf_myrtille_send_screen(wfc, commandArgs == "adaptive");
						break;

					// client clipboard
					case COMMAND::SEND_LOCAL_CLIPBOARD:
						// convert to unicode and store the value
						ConvertToUnicode(CP_UTF8, 0, commandArgs.c_str(), -1, &clipboardText, 0);
						myrtille->clipboardText = std::wstring(clipboardText);

						// the clipboard virtual channel is sometimes bugged (wfc->cliprdr is null; wfreerdp or rdp server issue?)
						// I wasn't able to replicate the issue (had it once whith wfreerdp running under an account which is not member of the target domain, but then stopped to have it)
						// if that happens, it's from the opening of the session and for its whole duration (disconnecting/reconnecting the session doesn't fix the issue, leaning more toward a server side issue)

						// another issue is, the channel is opened (wfc->cliprdr is not null) but the copy & paste events don't fire (nothing is received on the channel!)

						// in both cases, the only way is to sign out the session and open a new one

						if (wfc->cliprdr != NULL)
						{
							// invalidate the server clipboard so that the next paste action will trigger the retrieval of the stored value
							CLIPRDR_MONITOR_READY monitorReady;
							monitorReady.msgType = CB_MONITOR_READY;
							monitorReady.msgFlags = 0;
							monitorReady.dataLen = 0;
							wfc->cliprdr->MonitorReady(wfc->cliprdr, &monitorReady);
						}
						break;

					// the standard way to close an rdp session is to logoff the user; an alternate way is to simply close the rdp client
					// this disconnect the session, which is then subsequently closed (1 sec later if "MaxDisconnectionTime" = 1000 ms)
					case COMMAND::CLOSE_CLIENT:
						myrtille->processInputs = false;
						break;
				}
			}

			sizeHeader = !sizeHeader;
		}

		// cleanup
		delete[] buffer;
		buffer = NULL;
	}

	CloseHandle(myrtille->inputsPipe);
	CloseHandle(myrtille->updatesPipe);
	CloseHandle(myrtille->audioPipe);
	Gdiplus::GdiplusShutdown(myrtille->gdiplusToken);
	fclose(stdout);
	fclose(stderr);
	UINT32 exitCode = freerdp_get_last_error((rdpContext*)wfc);
	exit(exitCode);
	return 0;
}

void processResizeDisplay(wfContext* wfc, bool keepAspectRatio, std::string resolution)
{
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	int separatorIdx = resolution.find("x");
	if (separatorIdx != std::string::npos)
	{
		int clientWidth = stoi(resolution.substr(0, separatorIdx));
		int clientHeight = stoi(resolution.substr(separatorIdx + 1, resolution.length() - separatorIdx - 1));
		if (keepAspectRatio)
		{
			float aspectRatio = (float)clientWidth / (float)clientHeight;
			if (myrtille->aspectRatio == aspectRatio)
			{
				myrtille->clientWidth = clientWidth;
				myrtille->clientHeight = clientHeight;
			}
			else if (myrtille->aspectRatio < aspectRatio)
			{
				myrtille->clientWidth = clientHeight * myrtille->aspectRatio;
				myrtille->clientHeight = clientHeight;
			}
			else
			{
				myrtille->clientWidth = clientWidth;
				myrtille->clientHeight = clientWidth / myrtille->aspectRatio;
			}
		}
		else
		{
			myrtille->clientWidth = clientWidth;
			myrtille->clientHeight = clientHeight;
		}
	}
}

void processMouseInput(wfContext* wfc, std::string input, UINT16 flags)
{
	if (!wfc->context.input->MouseEvent)
		return;

	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	int separatorIdx = input.find("-");
	if (separatorIdx != std::string::npos)
	{
		std::string mX = input.substr(0, separatorIdx);
		std::string mY = input.substr(separatorIdx + 1, input.length() - separatorIdx - 1);
		if (!mX.empty() && stoi(mX) >= 0 && !mY.empty() && stoi(mY) >= 0)
		{
			int cw, ch, dw, dh;
			cw = myrtille->clientWidth;
			ch = myrtille->clientHeight;
			dw = wfc->context.settings->DesktopWidth;
			dh = wfc->context.settings->DesktopHeight;

			if (!myrtille->scaleDisplay || (cw == dw && ch == dh))
			{
				wfc->context.input->MouseEvent(
					wfc->context.input,
					flags,
					stoi(mX),
					stoi(mY));
			}
			else
			{
				wfc->context.input->MouseEvent(
					wfc->context.input,
					flags,
					stoi(mX) * dw / cw,
					stoi(mY) * dh / ch);
			}
		}
	}
}

void sendMessage(wfContext* wfc, std::wstring msg)
{
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	if (msg.length() == 0)
		return;

	// unicode is 2 bytes (16 bits) per character (UTF-16LE)
	size_t bytesPerChar = sizeof(wchar_t);
	
	// message size in bytes
	size_t size = msg.length() * bytesPerChar;

	// size header (4 bytes)
	byte* header = new byte[4];
	int32ToBytes(size, 0, header);

	// buffer
	byte* buffer = new byte[size + 4];
	memcpy(buffer, header, 4);
	memcpy(&buffer[4], msg.c_str(), size);

	// send
	DWORD bytesToWrite = size + 4;
	DWORD bytesWritten;
	if (WriteFile(myrtille->updatesPipe, buffer, bytesToWrite, &bytesWritten, NULL) == 0)
	{
		switch (GetLastError())
		{
		case ERROR_INVALID_HANDLE:
			WLog_ERR(TAG, "sendMessage: WriteFile failed with error ERROR_INVALID_HANDLE");
			break;

		case ERROR_PIPE_NOT_CONNECTED:
			WLog_ERR(TAG, "sendMessage: WriteFile failed with error ERROR_PIPE_NOT_CONNECTED");
			break;

		case ERROR_PIPE_BUSY:
			WLog_ERR(TAG, "sendMessage: WriteFile failed with error ERROR_PIPE_BUSY");
			break;

		case ERROR_BAD_PIPE:
			WLog_ERR(TAG, "sendMessage: WriteFile failed with error ERROR_BAD_PIPE");
			break;

		case ERROR_BROKEN_PIPE:
			WLog_ERR(TAG, "sendMessage: WriteFile failed with error ERROR_BROKEN_PIPE");
			break;

		default:
			WLog_ERR(TAG, "sendMessage: WriteFile failed with error %d", GetLastError());
			break;
		}

		// pipe problem; exit
		myrtille->processInputs = false;
	}

	// cleanup
	delete[] buffer;
	buffer = NULL;

	delete[] header;
	header = NULL;
}

void processImage(wfContext* wfc, Gdiplus::Bitmap* bmp, int left, int top, int right, int bottom, bool fullscreen, bool adaptive)
{
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	IStream* pngStream = NULL;
	IStream* jpgStream = NULL;
	IStream* webpStream = NULL;

	STATSTG statstg;

	int format;
	// PNG: use highest quality (lossless); AUTO/JPEG/WEBP: use higher quality for fullscreen updates in adaptive mode or current quality otherwise
	int quality = (myrtille->imageEncoding == (int)IMAGE_ENCODING::PNG ? (int)IMAGE_QUALITY::HIGHEST : (fullscreen && adaptive ? (int)IMAGE_QUALITY::HIGHER : myrtille->imageQuality));
	IStream* stream = NULL;
	ULONG size = 0;

	/*
	normally, the PNG format is best suited (lower size and better quality) for office applications (with text) and JPG for graphic ones (with images)
	PNG is lossless as opposite to JPG
	WEBP can either be lossy or lossless
	*/

	if (myrtille->imageEncoding == (int)IMAGE_ENCODING::PNG || myrtille->imageEncoding == (int)IMAGE_ENCODING::JPEG || myrtille->imageEncoding == (int)IMAGE_ENCODING::AUTO)
	{
		ULONG pngSize = 0;
		ULONG jpgSize = 0;

		// --------------------------- convert the bitmap into PNG --------------------------------

		if (myrtille->imageEncoding == (int)IMAGE_ENCODING::PNG || myrtille->imageEncoding == (int)IMAGE_ENCODING::AUTO)
		{
			CreateStreamOnHGlobal(NULL, TRUE, &pngStream);
			bmp->Save(pngStream, &myrtille->pngClsid);

			pngStream->Stat(&statstg, STATFLAG_DEFAULT);
			pngSize = (ULONG)statstg.cbSize.LowPart;
		}

		// --------------------------- convert the bitmap into JPEG -------------------------------

		if (myrtille->imageEncoding == (int)IMAGE_ENCODING::JPEG || myrtille->imageEncoding == (int)IMAGE_ENCODING::AUTO)
		{
			CreateStreamOnHGlobal(NULL, TRUE, &jpgStream);
			myrtille->encoderParameters.Parameter[0].Value = &quality;
			bmp->Save(jpgStream, &myrtille->jpgClsid, &myrtille->encoderParameters);

			jpgStream->Stat(&statstg, STATFLAG_DEFAULT);
			jpgSize = (ULONG)statstg.cbSize.LowPart;
		}

		// ---------------------------  use the lowest sized format -------------------------------

		// text, buttons, menus, etc... (simple image structure and low color palette) are more likely to be lower sized in PNG than JPG
		// on the opposite, a complex image (photo or graphical) is more likely to be lower sized in JPG

		if (myrtille->imageEncoding == (int)IMAGE_ENCODING::PNG || (myrtille->imageEncoding == (int)IMAGE_ENCODING::AUTO && pngSize <= jpgSize))
		{
			stream = pngStream;
			format = (int)IMAGE_FORMAT::PNG;
			quality = (int)IMAGE_QUALITY::HIGHEST;	// lossless
			size = pngSize;
		}
		else
		{
			stream = jpgStream;
			format = (int)IMAGE_FORMAT::JPEG;
			size = jpgSize;
		}
	}
	else if (myrtille->imageEncoding == (int)IMAGE_ENCODING::WEBP)
	{
		// --------------------------- convert the bitmap into WEBP -------------------------------

		CreateStreamOnHGlobal(NULL, TRUE, &webpStream);
		webPEncoder(wfc, bmp, myrtille->imageIdx + 1, webpStream, quality, fullscreen);

		webpStream->Stat(&statstg, STATFLAG_DEFAULT);
		ULONG webpSize = (ULONG)statstg.cbSize.LowPart;

		stream = webpStream;
		format = (int)IMAGE_FORMAT::WEBP;
		size = webpSize;
	}

	// ---------------------------  send the image ------------------------------------------------

	if (myrtille->imageIdx == INT_MAX)
	{
		myrtille->imageIdx = 0;
	}

	// in order to avoid overloading both the bandwidth and the browser, images are limited to 1024 KB each

	if (stream != NULL && size > 0)
	{
		sendImage(
			wfc,
			bmp,
			++myrtille->imageIdx,
			left,
			top,
			right - left,
			bottom - top,
			format,
			quality,
			stream,
			size,
			fullscreen);
	}

	// ---------------------------  cleanup -------------------------------------------------------

	if (pngStream != NULL)
	{
		pngStream->Release();
		pngStream = NULL;
	}

	if (jpgStream != NULL)
	{
		jpgStream->Release();
		jpgStream = NULL;
	}

	if (webpStream != NULL)
	{
		webpStream->Release();
		webpStream = NULL;
	}
}

void saveImage(wfContext* wfc, Gdiplus::Bitmap* bmp, int idx, int format, int quality, bool fullscreen)
{
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	std::string imgDirectoryPath = createRemoteSessionDirectory(wfc);
	if (imgDirectoryPath != "")
	{
		std::stringstream ss;
		ss << imgDirectoryPath;

		switch (format)
		{
			case (int)IMAGE_FORMAT::CUR:
				ss << "\\cursor_" << idx << ".png";
				break;

			case (int)IMAGE_FORMAT::PNG:
				ss << (fullscreen ? "\\screen_" : "\\region_") << idx << ".png";
				break;

			case (int)IMAGE_FORMAT::JPEG:
				ss << (fullscreen ? "\\screen_" : "\\region_") << idx << "_" << quality << ".jpg";
				break;
		}

		std::string s = ss.str();
		std::wstring ws = s2ws(s);
		const wchar_t *filename = ws.c_str();

		switch (format)
		{
			case (int)IMAGE_FORMAT::CUR:
			case (int)IMAGE_FORMAT::PNG:
				bmp->Save(filename, &myrtille->pngClsid);
				break;

			case (int)IMAGE_FORMAT::JPEG:
				myrtille->encoderParameters.Parameter[0].Value = &quality;
				bmp->Save(filename, &myrtille->jpgClsid, &myrtille->encoderParameters);
				break;
		}
	}
}

void sendImage(wfContext* wfc, Gdiplus::Bitmap* bmp, int idx, int posX, int posY, int width, int height, int format, int quality, IStream* stream, int size, bool fullscreen)
{
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	// image structure: tag (4 bytes) + info (32 bytes) + data
	// > tag is used to identify an image (0: image; other: message)
	// > info contains the image metadata (idx, posX, posY, etc.)
	// > data is the image raw data

	// image structure size (4 bytes)
	byte* header = new byte[40];
	int32ToBytes(size + 36, 0, header);

	// tag
	int32ToBytes(0, 4, header);

	// info
	int32ToBytes(idx, 8, header);
	int32ToBytes(posX, 12, header);
	int32ToBytes(posY, 16, header);
	int32ToBytes(width, 20, header);
	int32ToBytes(height, 24, header);
	int32ToBytes(format, 28, header);
	int32ToBytes(quality, 32, header);
	int32ToBytes(fullscreen, 36, header);

	// seek to the beginning of the stream
	LARGE_INTEGER li = { 0 };
	stream->Seek(li, STREAM_SEEK_SET, NULL);

	// data
	ULONG bytesRead;
	byte* data = new byte[size];
	stream->Read(data, size, &bytesRead);

	if (bytesRead != size)
	{
		WLog_WARN(TAG, "sendImage: number of bytes read from image stream (%d) differs from image size (%d)", bytesRead, size);
	}

	// buffer
	byte* buffer = new byte[size + 40];
	memcpy(buffer, header, 40);
	memcpy(&buffer[40], data, size);

	// send
	DWORD bytesToWrite = size + 40;
	DWORD bytesWritten;
	if (WriteFile(myrtille->updatesPipe, buffer, bytesToWrite, &bytesWritten, NULL) == 0)
	{
		switch (GetLastError())
		{
			case ERROR_INVALID_HANDLE:
				WLog_ERR(TAG, "sendImage: WriteFile failed with error ERROR_INVALID_HANDLE");
				break;

			case ERROR_PIPE_NOT_CONNECTED:
				WLog_ERR(TAG, "sendImage: WriteFile failed with error ERROR_PIPE_NOT_CONNECTED");
				break;

			case ERROR_PIPE_BUSY:
				WLog_ERR(TAG, "sendImage: WriteFile failed with error ERROR_PIPE_BUSY");
				break;

			case ERROR_BAD_PIPE:
				WLog_ERR(TAG, "sendImage: WriteFile failed with error ERROR_BAD_PIPE");
				break;

			case ERROR_BROKEN_PIPE:
				WLog_ERR(TAG, "sendImage: WriteFile failed with error ERROR_BROKEN_PIPE");
				break;

			default:
				WLog_ERR(TAG, "sendImage: WriteFile failed with error %d", GetLastError());
				break;
		}

		// pipe problem; exit
		myrtille->processInputs = false;
	}

	//WLog_INFO(TAG, "sendImage: WriteFile succeeded for image: %i (%s)", idx, (fullscreen ? "screen" : "region"));

	// images are saved under parent "log\remotesession_#ID.#PID" folder
	//saveImage(wfc, bmp, idx, format, quality, fullscreen);	// debug. enable with caution as it will flood the disk and hinder performance!!!

	// if taking screenshot and the image is fullscreen, save it
	if (myrtille->screenshotEnabled && fullscreen)
	{
		myrtille->screenshotEnabled = false;
		takeScreenshot(wfc, bmp);
	}

	// cleanup
	delete[] buffer;
	buffer = NULL;

	delete[] data;
	data = NULL;

	delete[] header;
	header = NULL;
}

void sendAudio(wfContext* wfc, const BYTE* data, size_t size)
{
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	DWORD bytesWritten;
	if (WriteFile(myrtille->audioPipe, data, size, &bytesWritten, NULL) == 0)
	{
		switch (GetLastError())
		{
			case ERROR_INVALID_HANDLE:
				WLog_ERR(TAG, "sendAudio: WriteFile failed with error ERROR_INVALID_HANDLE");
				break;

			case ERROR_PIPE_NOT_CONNECTED:
				WLog_ERR(TAG, "sendAudio: WriteFile failed with error ERROR_PIPE_NOT_CONNECTED");
				break;

			case ERROR_PIPE_BUSY:
				WLog_ERR(TAG, "sendAudio: WriteFile failed with error ERROR_PIPE_BUSY");
				break;

			case ERROR_BAD_PIPE:
				WLog_ERR(TAG, "sendAudio: WriteFile failed with error ERROR_BAD_PIPE");
				break;

			case ERROR_BROKEN_PIPE:
				WLog_ERR(TAG, "sendAudio: WriteFile failed with error ERROR_BROKEN_PIPE");
				break;

			default:
				WLog_ERR(TAG, "sendAudio: WriteFile failed with error %d", GetLastError());
				break;
		}

		// pipe problem; exit
		myrtille->processInputs = false;
	}
}

void takeScreenshot(wfContext* wfc, Gdiplus::Bitmap* bmp)
{
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	if (myrtille->screenshotPath != "")
	{
		std::stringstream ss;
		ss << myrtille->screenshotPath;
		if (myrtille->screenshotPath.substr(myrtille->screenshotPath.length() - 1, 1) != "\\")
		{
			ss << "\\";
		}

		ss << wfc->context.settings->MyrtilleSessionId << "_" << GetTickCount64() << (myrtille->screenshotFormat == (int)IMAGE_FORMAT::PNG ? ".png" : ".jpg");

		std::string s = ss.str();
		std::wstring ws = s2ws(s);
		const wchar_t *filename = ws.c_str();

		switch (myrtille->screenshotFormat)
		{
			case (int)IMAGE_FORMAT::PNG:
				bmp->Save(filename, &myrtille->pngClsid);
				break;

			case (int)IMAGE_FORMAT::JPEG:
				int quality = (int)IMAGE_QUALITY::HIGH;
				myrtille->encoderParameters.Parameter[0].Value = &quality;
				bmp->Save(filename, &myrtille->jpgClsid, &myrtille->encoderParameters);
				break;
		}
	}
}

void int32ToBytes(int value, int offset, byte* bytes)
{
	// little endian
	bytes[offset] = value & 0xFF;
	bytes[offset + 1] = (value >> 8) & 0xFF;
	bytes[offset + 2] = (value >> 16) & 0xFF;
	bytes[offset + 3] = (value >> 24) & 0xFF;
}

int bytesToInt32(byte* bytes)
{
	// little endian
	return int(
		(bytes[0]) |
		(bytes[1]) << 8 |
		(bytes[2]) << 16 |
		(bytes[3]) << 24);
}

void webPEncoder(wfContext* wfc, Gdiplus::Bitmap* bmp, int idx, IStream* stream, float quality, bool fullscreen)
{
	wfMyrtille* myrtille = (wfMyrtille*)wfc->myrtille;

	WebPPicture webpPic;

	if (WebPPictureInit(&webpPic))
	{
		webpPic.user_data = NULL;

		// debug

		//std::string imgDirectoryPath = createRemoteSessionDirectory(wfc);
		//if (imgDirectoryPath != "")
		//{
		//	std::stringstream ss;
		//	ss << imgDirectoryPath << (fullscreen ? "\\screen_" : "\\region_") << idx << "_" << quality << ".webp";
		//	webpPic.user_data = new std::string(ss.str());
		//}

		webpPic.custom_ptr = (void*)stream;
		webpPic.writer = webPWriter;

		webpPic.width = bmp->GetWidth();
		webpPic.height = bmp->GetHeight();

		Gdiplus::BitmapData* bmpData = new Gdiplus::BitmapData();
		Gdiplus::Rect* rect = new Gdiplus::Rect(0, 0, bmp->GetWidth(), bmp->GetHeight());
		bmp->LockBits(rect, ImageLockModeRead, PixelFormat32bppARGB, bmpData);

		const uint8_t* bmpBits = (uint8_t*)bmpData->Scan0;

		if (WebPPictureImportBGRA(&webpPic, bmpBits, bmpData->Stride))
		{
			myrtille->webpConfig.quality = quality;

			if (!WebPEncode(&myrtille->webpConfig, &webpPic))
				WLog_ERR(TAG, "webPEncoder: WebP encoding failed");
		}

		bmp->UnlockBits(bmpData);
		
		// cleanup

		delete rect;
		rect = NULL;

		delete bmpData;
		bmpData = NULL;

		if (webpPic.user_data != NULL)
			delete webpPic.user_data;

		WebPPictureFree(&webpPic);
	}
}

static int webPWriter(const uint8_t* data, size_t data_size, const WebPPicture* const pic)
{
	IStream* stream = (IStream*)pic->custom_ptr;

	ULONG bytesWritten;
	stream->Write(data, data_size, &bytesWritten);

	// debug

	//if (pic->user_data != NULL)
	//{
	//	std::string &filename = *(std::string*)pic->user_data;
	//	FILE* file = fopen(filename.c_str(), "ab");
	//	if (file != NULL)
	//	{
	//		fwrite(data, 1, data_size, file);
	//		fclose(file);
	//	}
	//}

	return bytesWritten == data_size ? 1 : 0;
}

#pragma endregion