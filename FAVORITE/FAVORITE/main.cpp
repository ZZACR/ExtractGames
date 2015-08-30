#include <Windows.h>
#include <strsafe.h>
#include "Favorite.h"
#include "resource.h"

#define MAXPATH 350

#define THREADNUM 4

HWND hEdit;
CRITICAL_SECTION cs;

extern _UNCOM gfnUncompress;

struct thread_param
{
	enum {QUEUE_SIZE = 1500};
	wchar_t **queue;
	DWORD front, tail;
	HANDLE hEvent;
	HANDLE hThread;
	bool thread_exit;
};

DWORD WINAPI Thread(PVOID pv);
BOOL CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPreInstance,
					PSTR pCmdLine, int iCmdShow)
{
	DialogBoxParam(hInstance, MAKEINTRESOURCE(IDD_MAIN), 0, DlgProc, 0);
	return 0;
}

void AppendMsg(const wchar_t *szBuffer)
{
	static DWORD dwPos;
	if (0 == szBuffer)
	{
		dwPos = 0;
		SendMessage(hEdit, EM_SETSEL, 0, -1);
		SendMessage(hEdit, EM_REPLACESEL, FALSE, 0);
	} else {
		SendMessage(hEdit, EM_SETSEL, (WPARAM)&dwPos, (LPARAM)&dwPos);
		SendMessage(hEdit, EM_REPLACESEL, 0, (LPARAM)szBuffer);
		SendMessage(hEdit, EM_GETSEL, 0, (LPARAM)&dwPos);
	}
	return;
}

int mycmp(wchar_t* src, wchar_t* dst)
{
	int i = 0;
	while (src[i]) tolower(src[i++]);
	return wcscmp(src, dst);
}
//////////////////////////////////////////////////////////////////////////////////
// 用于展开子目录
// lpszPath		 - 待展开目录
// callback		 - 回调函数
// pcb			 - 回调函数参数
//////////////////////////////////////////////////////////////////////////////////
typedef int (*CallBack)(struct CB* pcb, wchar_t* path);

struct CB
{
	int cnt;
	thread_param* ptp;
	wchar_t* filter;
};

int callback(struct CB* pcb, wchar_t* path)
{
	int len = lstrlen(path);
	while(len>=0 && path[len-1] != '.') --len;
	if (!pcb->filter || !wcscmp(&path[len], pcb->filter))
	{
		while (pcb->ptp[pcb->cnt].front == pcb->ptp[pcb->cnt].tail+1)		// 队列满，转下一个
			pcb->cnt = (pcb->cnt+1) % THREADNUM;

		EnterCriticalSection(&cs);
		{
			StringCchCopy(*pcb->ptp[pcb->cnt].queue + pcb->ptp[pcb->cnt].tail*MAXPATH, MAXPATH, path);
		
			if (pcb->ptp[pcb->cnt].tail == pcb->ptp[pcb->cnt].front)		// 原先队列为空，置位
				SetEvent(pcb->ptp[pcb->cnt].hEvent);

			pcb->ptp[pcb->cnt].tail = (pcb->ptp[pcb->cnt].tail + 1) % thread_param::QUEUE_SIZE;// 更新队列
		}
		LeaveCriticalSection(&cs);

		pcb->cnt = (pcb->cnt+1) % THREADNUM;	// 转下一个线程
	}
	return 0;
}

int ExpandDirectory(PTSTR lpszPath, CallBack callback, struct CB* pcb)
{
	static const DWORD MemAllocStep = 1024*MAX_PATH;
	wchar_t			lpFind[MAXPATH], lpSearch[MAXPATH], lpPath[MAXPATH];
	HANDLE			hFindFile;
	WIN32_FIND_DATA FindData;
	int				cnt = 0;

	// Path\*.*
	StringCchCopy(lpPath, MAXPATH, lpszPath);
	StringCchCat(lpPath, MAXPATH, L"\\");
	StringCchCopy(lpSearch, MAXPATH, lpPath);
	StringCchCat(lpSearch, MAXPATH, L"*.*");

	if (INVALID_HANDLE_VALUE != (hFindFile = FindFirstFile(lpSearch, &FindData)))
	{
		do
		{
			// 完整文件名
			StringCchCopy(lpFind, MAXPATH, lpPath);
			StringCchCat(lpFind, MAXPATH, FindData.cFileName);

			if (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				if (FindData.cFileName[0] != '.')
					ExpandDirectory(lpFind, callback, pcb);
			}
			else
			{
				callback(pcb, lpFind);
			}
		}while(FindNextFile(hFindFile, &FindData));
		FindClose(hFindFile);
		return 0;
	}
	return -2;
}

//////////////////////////////////////////////////////////////////////////////////
// 添加pInBuf(当它是文件)/pInBuf下的所有子文件(当它是目录)到队列
// pInBuf		 - 待添加文件(展开目录)
// callback		 - 回调函数
// pcb			 - 回调函数参数
//////////////////////////////////////////////////////////////////////////////////
DWORD AppendFileToQueue(PTSTR pInBuf, CallBack callback, struct CB *pcb)
{	
	if (FILE_ATTRIBUTE_DIRECTORY == GetFileAttributes(pInBuf))
	{
		ExpandDirectory(pInBuf, callback, pcb);
	}
	else
	{
		callback(pcb, pInBuf);
	}
	return 0;
}

void OnDropFiles(HDROP hDrop, HWND hwnd, thread_param* ptp)
{
	struct CB cb;
	TCHAR FileName[MAXPATH];
	DWORD i;
	DWORD FileNum;

	cb.cnt = 0;
	cb.filter = 0;
	cb.ptp = ptp;

	FileNum  = DragQueryFile(hDrop, -1, NULL, 0);

	for (i=0; i<FileNum; ++i)
	{
		DragQueryFile(hDrop, i, (LPTSTR)FileName, MAXPATH);
		AppendFileToQueue(FileName, callback, &cb);
	}
	DragFinish(hDrop);

	return;
}

BOOL CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	static thread_param tp[4];

	switch (msg)
	{
	case WM_INITDIALOG:
		SendMessage(hDlg, WM_SETICON, ICON_BIG,
					(LPARAM)LoadIcon((HINSTANCE)GetWindowLong(hDlg, GWL_HINSTANCE), MAKEINTRESOURCE(IDI_ICON1)));

		if (!(gfnUncompress = (_UNCOM)GetProcAddress(LoadLibrary(L"zlib.dll"), "uncompress")))
			MessageBox(hDlg, L"找不到zlib.dll，将无法处理图片资源", L"警告", MB_ICONWARNING);

		hEdit = GetDlgItem(hDlg, IDC_EDIT);
		SendMessage(hEdit, EM_LIMITTEXT, -1, 0);
		AppendMsg(TEXT("拖放封包文件至此处...\r\n【注意】文件路径须小于260个字符\r\n"));

		for (int i=0; i<THREADNUM; ++i)
		{
			if (!(tp[i].hEvent = CreateEvent(NULL, TRUE, FALSE, NULL)))
			{
				AppendMsg(TEXT("事件初始化错误！"));
				EndDialog(hDlg, 0);
			}
			if (!(tp[i].queue = (wchar_t**)VirtualAlloc(NULL, sizeof(wchar_t**), MEM_COMMIT, PAGE_READWRITE)))
			{
				AppendMsg(TEXT("内存分配错误！"));
				EndDialog(hDlg, 0);
			}
			if (!(*(tp[i].queue) = (wchar_t*)VirtualAlloc(NULL, thread_param::QUEUE_SIZE * MAXPATH * sizeof(wchar_t),
																MEM_COMMIT, PAGE_READWRITE)))
			{
				AppendMsg(TEXT("内存分配错误！"));
				EndDialog(hDlg, 0);
			}
			if (!(tp[i].hThread = CreateThread(NULL, 0, Thread, &tp[i], 0, NULL)))
			{
				AppendMsg(TEXT("线程创建失败！"));
				EndDialog(hDlg, 0);
			}
			tp[i].front = tp[i].tail = 0;
			tp[i].thread_exit = false;
		}
		InitializeCriticalSection(&cs);
		return TRUE;

	case WM_DROPFILES:
		OnDropFiles((HDROP)wParam, hDlg, tp);
		return TRUE;

	case WM_CLOSE:
		for (int i=0; i<THREADNUM; ++i)
		{
			tp[i].thread_exit = true;
			SetEvent(tp[i].hEvent);
		}
		FreeLibrary(GetModuleHandle(L"zlib.dll"));
		EndDialog(hDlg, 0);
		return TRUE;
	}
	return FALSE;
}

DWORD WINAPI Thread(PVOID pv)
{
	wchar_t cur_dir[MAXPATH];
	LPTSTR CurrentFile;
	DWORD dwNowProcess = 0;
	thread_param * ptp = (thread_param*) pv;
	

	while (1)
	{
		WaitForSingleObject(ptp->hEvent, INFINITE);

		if (ptp->thread_exit) break;

		CurrentFile = (wchar_t*)(*ptp->queue + ptp->front*MAXPATH);

		StringCchCopy(cur_dir, MAXPATH, CurrentFile);

		int l = lstrlen(cur_dir);
		while(l && cur_dir[l-1] != '\\') --l;
		cur_dir[l] = '\0';

		StringCchCat(cur_dir, MAXPATH, TEXT("[extract] "));
		StringCchCat(cur_dir, MAXPATH, &CurrentFile[l]);
		CreateDirectory(cur_dir, 0);
		
		Entrance(CurrentFile, cur_dir);
		
		EnterCriticalSection(&cs);
		{
			ptp->front = (ptp->front + 1) % ptp->QUEUE_SIZE;
		
			if (ptp->front == ptp->tail)
				ResetEvent(ptp->hEvent);
		}
		LeaveCriticalSection(&cs);
	}
	return 0;
}