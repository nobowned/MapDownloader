#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <urlmon.h>
#include "resource.h"

#pragma comment(lib, "urlmon.lib")

#define WM_SYSTEMTRAY WM_APP
#define WM_EXIT WM_APP + 1
#define MAX_QPATH 64
#define DOWNLOAD_SERVER "http://www.x-labs.co.uk/mapdb/rtcw"
#define NOTIFY_ICON_ID 1
#define DEFAULT_DESTROY_WAIT 5000

// TODO(lance): Scrape x-labs.co.uk and s4ndmod.com map databases and place in mine:
 //#define DOWNLOAD_SERVER "http://static.rtcwmp.com/maps"

typedef struct {
	DWORD ProcessId;
	HWND WindowHandle;
	BOOL GetVisible;
	LPSTR Title;
} find_window;

void PrintLastErrorMessage();

HANDLE GetProcess(char *ProcessName, DWORD ProcessId, LPPROCESSENTRY32 ProcessEntry);
HANDLE GetThreadOf(DWORD ProcessId);
void GetModuleEntry(char *ModuleName, HANDLE ProcessHandle, LPPROCESSENTRY32 ProcessEntry, DWORD ProtectionFlags, LPMODULEENTRY32 ModuleRequested);
HWND GetWindowOf(DWORD ProcessId, LPSTR Title, BOOL GetVisible);
void NotificationBalloon(DWORD InfoType, const char *fmt, ...);
void DestroyIn(DWORD Milliseconds);
void Dispose();
void Log(const char *fmt, ...);

LPVOID GetMemory(HANDLE ProcessHandle, LPMODULEENTRY32 ModuleEntry);
int GetOffsetInMemory(char *SearchTerm, LPVOID Memory, SIZE_T MemSize);
int FindBlock(LPVOID Block, SIZE_T BlockSize, LPVOID Memory, SIZE_T MemSize);
void WriteToMemory(HANDLE ProcessHandle, LPMODULEENTRY32 ModuleEntry, SIZE_T Location, LPVOID Block, SIZE_T BlockSize);
void SendMessageToConsole(char *Message);

#ifdef _DEBUG
void DebugOutput(const char *fmt, ...)
{
	char msg[1024];

	wvsprintf(msg, fmt, ((char*)&fmt) + sizeof(void*));
	OutputDebugString(msg);
	// TODO(lance): Un-debug and add logging
}

#define DEBUGOutputString(format, ...) DebugOutput(format, __VA_ARGS__)
#else
#define DEBUGOutputString(format, ...)
#endif

PROCESSENTRY32 ProcessEntry;
MODULEENTRY32 MainModuleEntry;
MODULEENTRY32 UIModuleEntry;
HANDLE ProcessHandle;
HANDLE ThreadHandle;
HWND WindowHandle;
HWND ConsoleWindow;
HWND MainWindow;
NOTIFYICONDATA NotificationData;
HINSTANCE InstanceHandle;
LPVOID Memory;
FILE *LogFile = NULL;
char FileName[MAX_PATH];
char GameDownloadName[MAX_PATH];
char WorkingDirectory[MAX_PATH];
char FileSize[MAX_QPATH];
char WindowClassName[100] = "MapDownloader";
bool KeepRunning;

typedef enum {
	Copied,
	Rate,
	EstimatedMsg,
	RateMsg,
	TimeMinSec,
	TimeHrMin,
	TimeMin,
	TimeSec
} search_term_type;

typedef struct {
	char Format[MAX_QPATH];
	char Out[MAX_QPATH];
	unsigned int Offset;
} search_term;

search_term SearchTerms[] = {
	{ "(%s of %s copied)", "HTTP Download Go!" },
	{ "%s/Sec", "      "},
	{ "Estimated time left:", "                    " },
	{ "Transfer rate:", "              " },
	{ "%d min %d sec", "             " },
	{ "%d hr %d min", "            " },
	{ "%d min", "      " },
	{ "%d sec", "      " }
};

class DownloadFileCallbacks : public IBindStatusCallback
{
protected:
	ULONG ReferenceCount;
public:
	DownloadFileCallbacks()
	{
		ReferenceCount = 0;
	}

	STDMETHODIMP OnDataAvailable(DWORD grfBSCF, DWORD dwSize, FORMATETC *pformatetc, STGMEDIUM *pstgmed)
	{
		return S_OK;
	}

	STDMETHODIMP OnObjectAvailable(REFIID riid, IUnknown *punk)
	{
		return S_OK;
	}

	STDMETHODIMP OnProgress(ULONG ulProgress, ULONG ulProgressMax, ULONG ulStatusCode, LPCWSTR szStatusText)
	{
		static const char *plpszStatus[] =
		{
			("BINDSTATUS_FINDINGRESOURCE"),  // 1
			("BINDSTATUS_CONNECTING"),
			("BINDSTATUS_REDIRECTING"),
			("BINDSTATUS_BEGINDOWNLOADDATA"),
			("BINDSTATUS_DOWNLOADINGDATA"),
			("BINDSTATUS_ENDDOWNLOADDATA"),
			("BINDSTATUS_BEGINDOWNLOADCOMPONENTS"),
			("BINDSTATUS_INSTALLINGCOMPONENTS"),
			("BINDSTATUS_ENDDOWNLOADCOMPONENTS"),
			("BINDSTATUS_USINGCACHEDCOPY"),
			("BINDSTATUS_SENDINGREQUEST"),
			("BINDSTATUS_CLASSIDAVAILABLE"),
			("BINDSTATUS_MIMETYPEAVAILABLE"),
			("BINDSTATUS_CACHEFILENAMEAVAILABLE"),
			("BINDSTATUS_BEGINSYNCOPERATION"),
			("BINDSTATUS_ENDSYNCOPERATION"),
			("BINDSTATUS_BEGINUPLOADDATA"),
			("BINDSTATUS_UPLOADINGDATA"),
			("BINDSTATUS_ENDUPLOADINGDATA"),
			("BINDSTATUS_PROTOCOLCLASSID"),
			("BINDSTATUS_ENCODING"),
			("BINDSTATUS_VERFIEDMIMETYPEAVAILABLE"),
			("BINDSTATUS_CLASSINSTALLLOCATION"),
			("BINDSTATUS_DECODING"),
			("BINDSTATUS_LOADINGMIMEHANDLER"),
			("BINDSTATUS_CONTENTDISPOSITIONATTACH"),
			("BINDSTATUS_FILTERREPORTMIMETYPE"),
			("BINDSTATUS_CLSIDCANINSTANTIATE"),
			("BINDSTATUS_IUNKNOWNAVAILABLE"),
			("BINDSTATUS_DIRECTBIND"),
			("BINDSTATUS_RAWMIMETYPE"),
			("BINDSTATUS_PROXYDETECTING"),
			("BINDSTATUS_ACCEPTRANGES"),
			("???")  // unknown
		};

		if (ulStatusCode < BINDSTATUS_FINDINGRESOURCE ||
			ulStatusCode > BINDSTATUS_ACCEPTRANGES)
		{
			ulStatusCode = BINDSTATUS_ACCEPTRANGES + 1;
		}

		Log("(%s): %ls", plpszStatus[ulStatusCode - BINDSTATUS_FINDINGRESOURCE], szStatusText);

		if (ulStatusCode == BINDSTATUS_BEGINDOWNLOADDATA)
		{
			SendMessageToConsole("stopdl");

			for (int i = 0; i < ARRAYSIZE(SearchTerms); ++i)
			{
				search_term *SearchTerm = SearchTerms + i;

				if (SearchTerm->Offset)
				{
					WriteToMemory(ProcessHandle, &UIModuleEntry, SearchTerm->Offset, (LPVOID)SearchTerm->Out, strlen(SearchTerm->Out));
				}
			}
		}
		else if (ulStatusCode == BINDSTATUS_DOWNLOADINGDATA)
		{
			search_term *SearchTerm = SearchTerms + Copied;
			char *ByteType;
			float Progress;
			float FileLength = 0.0f;

			if (*FileSize)
			{
				char FileSizeWithoutType[MAX_QPATH];
				strncpy(FileSizeWithoutType, FileSize, sizeof(FileSizeWithoutType));
				*strrchr(FileSizeWithoutType, ' ') = 0;

				FileLength = (float)atof(FileSizeWithoutType);
			}

			if (strstr(FileSize, "MB"))
			{
				ByteType = "MB";
				Progress = (float)ulProgress / (1000.0f * 1000.0f);
			}
			else if (strstr(FileSize, "KB"))
			{
				ByteType = "KB";
				Progress = (float)ulProgress / 1000.0f;
			}
			else
			{
				ByteType = "B";
				Progress = (float)ulProgress;
			}

			if (FileLength && Progress > FileLength)
			{
				Progress = FileLength;
			}

			sprintf(SearchTerm->Out, "      (%.2f%s of %s copied)", Progress, ByteType, *FileSize ? FileSize : "Unknown");
			WriteToMemory(ProcessHandle, &UIModuleEntry, SearchTerm->Offset, (LPVOID)SearchTerm->Out, strlen(SearchTerm->Out));
		}
		else if (ulStatusCode == BINDSTATUS_ENDDOWNLOADDATA)
		{
			SendMessageToConsole("disconnect");

			char MainDirectory[MAX_PATH];
			sprintf(MainDirectory, "%s\\main", WorkingDirectory);

			WIN32_FIND_DATA FileData;
			HANDLE NextFileHandle;
			char FullPathWildcard[MAX_PATH];
			char FullPath[MAX_PATH];

			sprintf(FullPathWildcard, "%s\\%s", MainDirectory, "*.pk3.tmp");

			if ((NextFileHandle = FindFirstFile(FullPathWildcard, &FileData)) != INVALID_HANDLE_VALUE)
			{
				do
				{
					int Attempts = 0;
					sprintf(FullPath, "%s\\%s", MainDirectory, FileData.cFileName);
					while (DeleteFile(FullPath) == 0 && Attempts++ < 5)
					{
						Sleep(500);
					}
				} while (FindNextFile(NextFileHandle, &FileData));
				FindClose(NextFileHandle);
			}

			SendMessageToConsole("vid_restart");
			SendMessageToConsole("reconnect");
		}

		return S_OK;
	}

	STDMETHODIMP OnStartBinding(DWORD dwReserved, IBinding *pib)
	{
		return S_OK;
	}

	STDMETHODIMP OnStopBinding(HRESULT hresult, LPCWSTR szError)
	{
		return S_OK;
	}

	STDMETHODIMP OnLowResource(DWORD reserved)
	{
		return S_OK;
	}

	STDMETHODIMP QueryInterface(REFIID riid, void **ppvObject)
	{
		*ppvObject = NULL;
		if (IsEqualIID(riid, __uuidof(IUnknown)))
		{
			*ppvObject = this;
		}
		else if (IsEqualIID(riid, __uuidof(IBindStatusCallback)))
		{
			*ppvObject = (IBindStatusCallback *)(this);
		}

		if (*ppvObject)
		{
			(*(LPUNKNOWN *)ppvObject)->AddRef();
			return S_OK;
		}

		return E_NOINTERFACE;
	}

	STDMETHODIMP_(ULONG) AddRef()
	{
		return ++ReferenceCount;
	}

	STDMETHODIMP_(ULONG) Release()
	{
		return --ReferenceCount;
	}

	STDMETHODIMP GetPriority(LONG *pnPriority)
	{
		return E_NOTIMPL;
	}

	STDMETHODIMP GetBindInfo(DWORD *grfBINDF, BINDINFO *pbindinfo)
	{
		return S_OK;
	}
};

CONDITION_VARIABLE ProcessExists;
CRITICAL_SECTION   ProcessLock;

DWORD WINAPI ThreadProc(LPVOID lpParameter)
{
	bool GameDownload;
	InitializeCriticalSection(&ProcessLock);

Initialize:
	GameDownload = false;
	KeepRunning = true;
	Dispose();
	ZeroMemory(FileName, sizeof(FileName));
	ZeroMemory(WorkingDirectory, sizeof(WorkingDirectory));
	ZeroMemory(FileSize, sizeof(FileSize));
	ZeroMemory(&ProcessEntry, sizeof(PROCESSENTRY32));
	ZeroMemory(&MainModuleEntry, sizeof(MODULEENTRY32));
	ZeroMemory(&UIModuleEntry, sizeof(MODULEENTRY32));
	ProcessEntry.dwSize = sizeof(PROCESSENTRY32);
	MainModuleEntry.dwSize = sizeof(MODULEENTRY32);
	UIModuleEntry.dwSize = sizeof(MODULEENTRY32);

	HWND WindowHandle = GetWindowOf(0, "Wolfenstein", TRUE);
	DWORD ProcessId = 0;
	if (WindowHandle)
	{
		GetWindowThreadProcessId(WindowHandle, &ProcessId);
	}

	// NOTE(lance): This is needed in-case of the below goto ocurring before the other Leave is called
	// That way ownership is properly released and the RecursionCount won't get out of control (possible memory leak?)
	LeaveCriticalSection(&ProcessLock);
	EnterCriticalSection(&ProcessLock);

	if ((ProcessHandle = GetProcess("WolfMP.exe", ProcessId, &ProcessEntry)) == NULL)
	{
		Log("Unable to acquire a Process Handle to Wolfenstein. Retrying in 3 seconds");

		SleepConditionVariableCS(&ProcessExists, &ProcessLock, 3000);

		if (!KeepRunning)
		{
			return EXIT_SUCCESS;
		}

		goto Initialize;
	}

	LeaveCriticalSection(&ProcessLock);

	ThreadHandle = GetThreadOf(ProcessEntry.th32ProcessID);
	if (WindowHandle == NULL)
	{
		WindowHandle = GetWindowOf(ProcessEntry.th32ProcessID, NULL, TRUE);
	}
	ConsoleWindow = GetWindowOf(0, "Wolf Console", FALSE);
	if (!ConsoleWindow)
	{
		Log("Your Wolf Console appears to be missing. Will re-attempt shortly.");
		goto Initialize;
	}
	GetModuleEntry("ui_mp_x86.dll", ProcessHandle, &ProcessEntry, PAGE_EXECUTE_READWRITE, &UIModuleEntry);
	if (!UIModuleEntry.modBaseAddr)
	{
		Log("Your ui_mp_x86.dll appears to be missing. Will re-attempt shortly.");
		goto Initialize;
	}

	GetModuleEntry(ProcessEntry.szExeFile, ProcessHandle, &ProcessEntry, PAGE_EXECUTE_READWRITE, &MainModuleEntry);
	if (!MainModuleEntry.modBaseAddr)
	{
		Log("Your WolfMP.exe appears to be missing. Will re-attempt shortly.");
		goto Initialize;
	}
	strncpy(WorkingDirectory, MainModuleEntry.szExePath, sizeof(WorkingDirectory));
	*strrchr(WorkingDirectory, '\\') = 0;

	while (KeepRunning)
	{
		if (Memory)
		{
			VirtualFree(Memory, 0, MEM_RELEASE);
			Memory = NULL;
		}

		Sleep(1000);

		DWORD ExitCode;
		if (GetExitCodeProcess(ProcessHandle, &ExitCode))
		{
			if (ExitCode != STILL_ACTIVE)
			{
				goto Initialize;
			}
		}
		else
		{
			goto Initialize;
		}

		Memory = GetMemory(ProcessHandle, &UIModuleEntry);
		if (Memory == NULL)
		{
			// NOTE(lance): Unfortunately this seems to happen when Wolf is launching..
			// Should atleast log it though, but continue on.
			Log("Failed to acquire Memory from Wolfenstein. Will re-attempt shortly");
			goto Initialize;
		}

		int ExtensionOffset = GetOffsetInMemory(".pk3 (", Memory, UIModuleEntry.modBaseSize);
		if (ExtensionOffset == 0)
		{
			GameDownload = false;
			continue;
		}

		char *Ptr = (char *)Memory + ExtensionOffset;

		char *FileCopies = Ptr + 10;
		if (*FileCopies)
		{
			// TODO(lance): Make this less shitty/error-prone
			strncpy(FileSize, Ptr + 10, sizeof(FileSize));
			char *GettingThere = strchr(FileSize, ' ') + 1;
			GettingThere = strchr(GettingThere, ' ') + 1;
			GettingThere = strchr(GettingThere, ' ') + 1;
			*strrchr(FileSize, ' ') = 0;
			strncpy(FileSize, GettingThere, sizeof(FileSize));
		}
		else
		{
			ZeroMemory(FileSize, sizeof(FileSize));
		}

		while (*(Ptr - 1)) Ptr--;
		char FilePath[MAX_QPATH];
		strncpy(FilePath, Ptr, sizeof(FilePath));

		char *EndOfPath = strrchr(FilePath, ' ');
		if (!EndOfPath)
		{
			Log("EndOfPath: Failed to acquire a File Name from Wolfenstein. It's likely your version of Wolf is incompatible.");
			goto Initialize;
		}
		*EndOfPath = 0;

		char *BeginFileName = strchr(FilePath, '/');
		if (!BeginFileName)
		{
			Log("BeginFileName: Failed to acquire a File Name from Wolfenstein. It's likely your version of Wolf is incompatible.");
			goto Initialize;
		}
		BeginFileName++;
		memcpy(FileName, BeginFileName, sizeof(FileName));

		if (GameDownload)
		{
			if (strcmp(GameDownloadName, FileName))
			{
				GameDownload = false;
			}
			else
			{
				continue;
			}
		}

		for (int i = 0; i < ARRAYSIZE(SearchTerms); ++i)
		{
			search_term *SearchTerm = SearchTerms + i;
			SearchTerm->Offset = GetOffsetInMemory(SearchTerm->Format, Memory, UIModuleEntry.modBaseSize);
		}

		char DownloadUrl[MAX_PATH];
		sprintf(DownloadUrl, "%s/%s", DOWNLOAD_SERVER, FileName);
		sprintf(FilePath, "%s\\main\\%s", WorkingDirectory, FileName);
		DownloadFileCallbacks Dfc;

		Log("Download of map %s will begin shortly\n"
			"---------------------------", FileName);

		if (URLDownloadToFile(NULL, DownloadUrl, FilePath, 0, &Dfc) != S_OK)
		{
			GameDownload = true;
			strcpy(GameDownloadName, FileName);
			NotificationBalloon(NIIF_WARNING, "Failed to initiate http download of map %s. Chances are it hasn't been added to the mapdb yet.", FileName);
			continue;
		}
		else
		{
			Log("Download has finished successfuly\n"
				"---------------------------");
		}
	}

	Dispose();
	return EXIT_SUCCESS;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	int wmId, wmEvent;

	switch (message)
	{
	case WM_COMMAND:
		wmId = LOWORD(wParam);
		wmEvent = HIWORD(wParam);

		switch (wmId)
		{
		case WM_EXIT:
			DestroyWindow(hWnd);
			break;
		}
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	case WM_SYSTEMTRAY:
		switch (lParam)
		{
		case WM_LBUTTONDOWN:
		case WM_RBUTTONDOWN:
		case WM_CONTEXTMENU:
			POINT pt;
			GetCursorPos(&pt);
			HMENU hMenu = CreatePopupMenu();
			if (hMenu)
			{
				InsertMenu(hMenu, -1, MF_BYPOSITION, WM_EXIT, "Exit");
				SetForegroundWindow(hWnd);
				TrackPopupMenu(hMenu, TPM_BOTTOMALIGN, pt.x, pt.y, 0, hWnd, NULL);
				DestroyMenu(hMenu);
			}
			break;
		}
		break;
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nCmdShow)
{
	MSG msg;
	WNDCLASS wc;

	memset(&wc, 0, sizeof(wc));

	LogFile = fopen("debug.log", "w+");

	wc.hInstance = hInstance;
	wc.lpfnWndProc = WndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.lpszMenuName = 0;
	wc.style = 0;
	wc.hIcon = LoadIcon(hInstance, IDI_WINLOGO);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)COLOR_WINDOW;
	wc.lpszClassName = WindowClassName;

	if (!RegisterClass(&wc))
	{
		Log("Failed to register the WNDCLASS.");
		return EXIT_FAILURE;
	}

	MainWindow = CreateWindow(WindowClassName, WindowClassName, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, NULL, NULL, hInstance, NULL);
	InstanceHandle = hInstance;

	NotificationData.cbSize = sizeof(NOTIFYICONDATA);
	NotificationData.hWnd = MainWindow;
	NotificationData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
	NotificationData.uID = NOTIFY_ICON_ID;
	NotificationData.uCallbackMessage = WM_SYSTEMTRAY;
	NotificationData.hIcon = (HICON)LoadImage(InstanceHandle, MAKEINTRESOURCE(IDI_ICON1),
		IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
		LR_DEFAULTCOLOR);
	strncpy(NotificationData.szTip, "Map Downloader", ARRAYSIZE(NotificationData.szTip));

	Shell_NotifyIcon(NIM_ADD, &NotificationData);

	HANDLE ThreadHandle = CreateThread(NULL, 0, &ThreadProc, NULL, 0, NULL);
	if (!ThreadHandle)
	{
		Log("Failed to create the thread.");
		return EXIT_FAILURE;
	}

	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	KeepRunning = false;
	WakeConditionVariable(&ProcessExists);
	WaitForSingleObject(ThreadHandle, INFINITE);

	if (LogFile)
	{
		fclose(LogFile);
	}

	Shell_NotifyIcon(NIM_DELETE, &NotificationData);
	return EXIT_SUCCESS;
}

#define MegaBytes(count) count * 1000 * 1000

void Log(const char *fmt, ...)
{
	char Message[1024];

	if (!LogFile)
	{
		return;
	}

	wvsprintf(Message, fmt, ((char*)&fmt) + sizeof(void*));
	strcat(Message, "\n");

	fputs(Message, LogFile);
}

void NotificationBalloon(DWORD InfoType, const char *fmt, ...)
{
	char Message[1024];
	char Date[64];
	char *Title = "";

	wvsprintf(Message, fmt, ((char*)&fmt) + sizeof(void*));

	switch (InfoType)
	{
	case NIIF_ERROR:
		Title = "Error";
		break;
	case NIIF_WARNING:
		Title = "Warning";
		break;
	case NIIF_INFO:
		Title = "Info";
		break;
	}

	SYSTEMTIME TimeData;
	GetSystemTime(&TimeData);
	sprintf(Date, "%d/%d/%d %d:%d", TimeData.wMonth, TimeData.wDay, TimeData.wYear, TimeData.wHour, TimeData.wSecond);

	Log("%s - %s\n\n"
		"%s\n"
		"-------------------------------", Title, Date, Message);

	strncpy(NotificationData.szInfoTitle, Title, sizeof(NotificationData.szInfoTitle));
	strncpy(NotificationData.szInfo, Message, sizeof(NotificationData.szInfo));
	NotificationData.dwInfoFlags = InfoType;

	Shell_NotifyIcon(NIM_MODIFY, &NotificationData);
}

void DestroyIn(DWORD Milliseconds)
{
	Sleep(Milliseconds);
	SendMessage(MainWindow, WM_DESTROY, NULL, NULL);
}

void Dispose()
{
	CloseHandle(ThreadHandle);
	CloseHandle(ProcessHandle);
	ProcessHandle = NULL;
	ThreadHandle = NULL;
	WindowHandle = NULL;
	ConsoleWindow = NULL;
	if (Memory)
	{
		VirtualFree(Memory, 0, MEM_RELEASE);
		Memory = NULL;
	}
}

void SendMessageToConsole(char *Message)
{
	HWND EditField = FindWindowEx(ConsoleWindow, NULL, "edit", NULL);

	while (*Message)
	{
		SendMessage(EditField, WM_CHAR, *Message++, NULL);
	}
	SendMessage(EditField, WM_CHAR, 13, NULL);
}

void WriteToMemory(HANDLE ProcessHandle, LPMODULEENTRY32 ModuleEntry, SIZE_T Location, LPVOID Block, SIZE_T BlockSize)
{
	if (WriteProcessMemory(ProcessHandle, ModuleEntry->modBaseAddr + Location, Block, BlockSize, NULL) == 0)
	{
		PrintLastErrorMessage();
	}
}

LPVOID GetMemory(HANDLE ProcessHandle, LPMODULEENTRY32 ModuleEntry)
{
	DWORD BytesRead = 0;

	LPVOID Memory = VirtualAlloc(NULL, ModuleEntry->modBaseSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (Memory)
	{
		ReadProcessMemory(ProcessHandle, ModuleEntry->modBaseAddr, Memory, ModuleEntry->modBaseSize, &BytesRead);

		if (BytesRead <= 0)
		{
			VirtualFree(Memory, 0, MEM_RELEASE);
			return NULL;
		}
	}

	return Memory;
}

int GetOffsetInMemory(char *SearchTerm, LPVOID Memory, SIZE_T MemSize)
{
	int Address = 0;

	char *Ptr = (char *)Memory;
	char *Found;

	while (Ptr && Ptr < ((char *)Memory) + MemSize)
	{
		if (*Ptr && (Found = strstr(Ptr, SearchTerm)))
		{
			Ptr = Found + strlen(SearchTerm);
			Address = Found - ((char *)Memory);
			break;
		}
		else
		{
			Ptr++;
		}
	}

	return Address;
}

int FindBlock(LPVOID Block, SIZE_T BlockSize, LPVOID Memory, SIZE_T MemSize)
{
	int BlockPosition = 0;
	LPBYTE Ptr = (LPBYTE)Memory;
	LPBYTE End = (LPBYTE)Memory + MemSize;

	while (Ptr < End)
	{
		if (*Ptr++ != ((LPBYTE)Block)[BlockPosition++])
		{
			BlockPosition = 0;
			continue;
		}
		else if (BlockPosition == BlockSize)
		{
			return (Ptr - BlockSize) - (LPBYTE)Memory;
		}
	}

	return 0;
}

BOOL CALLBACK EnumerateWindowsCallback(HWND WindowHandle, LPARAM lParam)
{
	find_window *WindowData = (find_window *)lParam;
	DWORD WindowProcessId;
	CHAR WindowTitle[64];

	GetWindowThreadProcessId(WindowHandle, &WindowProcessId);
	GetWindowText(WindowHandle, WindowTitle, sizeof(WindowTitle));
	if ((WindowData->Title != NULL && WindowData->Title[0] && strcmp(WindowData->Title, WindowTitle)) ||
		(WindowData->ProcessId > 0 && WindowData->ProcessId != WindowProcessId) ||
		(!WindowData->GetVisible && IsWindowVisible(WindowHandle)))
	{
		return TRUE;
	}

	WindowData->WindowHandle = WindowHandle;
	return FALSE;
}

HWND GetWindowOf(DWORD ProcessId, LPSTR Title, BOOL GetVisible)
{
	find_window WindowData;
	WindowData.ProcessId = ProcessId;
	WindowData.GetVisible = GetVisible;
	WindowData.Title = Title;
	WindowData.WindowHandle = NULL;

	EnumWindows(EnumerateWindowsCallback, (LPARAM)&WindowData);

	return WindowData.WindowHandle;
}

HANDLE GetThreadOf(DWORD ProcessId)
{
	HANDLE hThread = NULL;
	HANDLE hThreadSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);

	THREADENTRY32 threadEntry;
	threadEntry.dwSize = sizeof(THREADENTRY32);

	Thread32First(hThreadSnapshot, &threadEntry);

	do
	{
		if (threadEntry.th32OwnerProcessID == ProcessId)
		{
			hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, threadEntry.th32ThreadID);
			break;
		}
	} while (Thread32Next(hThreadSnapshot, &threadEntry));

	CloseHandle(hThreadSnapshot);
	return hThread;
}

HANDLE GetProcess(char *ProcessName, DWORD ProcessId, LPPROCESSENTRY32 ProcessEntry)
{
	HANDLE hProcess = NULL;
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);

	while (Process32Next(hSnap, ProcessEntry) == TRUE)
	{
		if ((ProcessName != NULL && strcmp(ProcessEntry->szExeFile, ProcessName) == 0) || ProcessId == ProcessEntry->th32ProcessID)
		{
			hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, ProcessEntry->th32ProcessID);
			break;
		}
	}

	CloseHandle(hSnap);
	return hProcess;
}

void GetModuleEntry(char *ModuleName, HANDLE ProcessHandle, LPPROCESSENTRY32 ProcessEntry, DWORD ProtectionFlags, LPMODULEENTRY32 ModuleRequested)
{
	bool Found = false;
	DWORD PrevProtectionFlags = 0;

	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ProcessEntry->th32ProcessID);

	while ((Module32Next(hSnap, ModuleRequested)) == TRUE)
	{
		if (strcmp(ModuleRequested->szModule, ModuleName) == 0)
		{
			if (VirtualProtectEx(ProcessHandle, ModuleRequested->modBaseAddr, ModuleRequested->modBaseSize, ProtectionFlags, &PrevProtectionFlags) == 0)
			{
				PrintLastErrorMessage();
			}

			Found = true;
			break;
		}
	}

	if (!Found)
	{
		ZeroMemory(ModuleRequested, sizeof(MODULEENTRY32));
	}

	CloseHandle(hSnap);
}

void PrintLastErrorMessage()
{
	LPTSTR ErrorBuf;

	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ARGUMENT_ARRAY,
		NULL, GetLastError(), LANG_NEUTRAL, (LPTSTR)&ErrorBuf, 0, NULL);
	DEBUGOutputString(ErrorBuf);

	LocalFree(ErrorBuf);
}