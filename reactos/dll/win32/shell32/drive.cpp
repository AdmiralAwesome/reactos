/*
 *                 Shell Library Functions
 *
 * Copyright 2005 Johannes Anderwald
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <precomp.h>
using namespace std;

#define MAX_PROPERTY_SHEET_PAGE 32

WINE_DEFAULT_DEBUG_CHANNEL(shell);

typedef enum
{
    HWPD_STANDARDLIST = 0,
    HWPD_LARGELIST,
    HWPD_MAX = HWPD_LARGELIST
} HWPAGE_DISPLAYMODE, *PHWPAGE_DISPLAYMODE;

typedef
BOOLEAN
(NTAPI *INITIALIZE_FMIFS)(
    IN PVOID hinstDll,
    IN DWORD dwReason,
    IN PVOID reserved
);
typedef
BOOLEAN
(NTAPI *QUERY_AVAILABLEFSFORMAT)(
    IN DWORD Index,
    IN OUT PWCHAR FileSystem,
    OUT UCHAR* Major,
    OUT UCHAR* Minor,
    OUT BOOLEAN* LastestVersion
);
typedef
BOOLEAN
(NTAPI *ENABLEVOLUMECOMPRESSION)(
    IN PWCHAR DriveRoot,
    IN USHORT Compression
);

typedef
VOID
(NTAPI *FORMAT_EX)(
    IN PWCHAR DriveRoot,
    IN FMIFS_MEDIA_FLAG MediaFlag,
    IN PWCHAR Format,
    IN PWCHAR Label,
    IN BOOLEAN QuickFormat,
    IN ULONG ClusterSize,
    IN PFMIFSCALLBACK Callback
);

typedef
VOID
(NTAPI *CHKDSK)(
    IN PWCHAR DriveRoot,
    IN PWCHAR Format,
    IN BOOLEAN CorrectErrors,
    IN BOOLEAN Verbose,
    IN BOOLEAN CheckOnlyIfDirty,
    IN BOOLEAN ScanDrive,
    IN PVOID Unused2,
    IN PVOID Unused3,
    IN PFMIFSCALLBACK Callback
);


typedef struct
{
    WCHAR   Drive;
    UINT    Options;
    HMODULE hLibrary;
    QUERY_AVAILABLEFSFORMAT QueryAvailableFileSystemFormat;
    FORMAT_EX FormatEx;
    ENABLEVOLUMECOMPRESSION EnableVolumeCompression;
    CHKDSK Chkdsk;
    UINT Result;
} FORMAT_DRIVE_CONTEXT, *PFORMAT_DRIVE_CONTEXT;

EXTERN_C HPSXA WINAPI SHCreatePropSheetExtArrayEx(HKEY hKey, LPCWSTR pszSubKey, UINT max_iface, IDataObject *pDataObj);
EXTERN_C HWND WINAPI
DeviceCreateHardwarePageEx(HWND hWndParent,
                           LPGUID lpGuids,
                           UINT uNumberOfGuids,
                           HWPAGE_DISPLAYMODE DisplayMode);

HPROPSHEETPAGE SH_CreatePropertySheetPage(LPCSTR resname, DLGPROC dlgproc, LPARAM lParam, LPWSTR szTitle);

static const GUID GUID_DEVCLASS_DISKDRIVE = {0x4d36e967L, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};


static VOID
GetDriveNameWithLetter(LPWSTR szText, UINT cchTextMax, WCHAR wchDrive)
{
    WCHAR szDrive[] = L"C:\\";
    DWORD dwMaxComp, dwFileSys, cchText = 0;

    szDrive[0] = wchDrive;
    if (GetVolumeInformationW(szDrive, szText, cchTextMax, NULL, &dwMaxComp, &dwFileSys, NULL, 0))
    {
        cchText = wcslen(szText);
        if (cchText == 0)
        {
            /* load default volume label */
            cchText = LoadStringW(shell32_hInstance, IDS_DRIVE_FIXED, szText, cchTextMax);
        }
    }

    StringCchPrintfW(szText + cchText, cchTextMax - cchText, L" (%c)", wchDrive);
}

static VOID
InitializeChkDskDialog(HWND hwndDlg, PFORMAT_DRIVE_CONTEXT pContext)
{
    WCHAR szText[100];
    UINT Length;
    SetWindowLongPtr(hwndDlg, DWLP_USER, (INT_PTR)pContext);

    Length = GetWindowTextW(hwndDlg, szText, sizeof(szText) / sizeof(WCHAR));
    szText[Length] = L' ';
    GetDriveNameWithLetter(&szText[Length + 1], (sizeof(szText) / sizeof(WCHAR)) - Length - 1, pContext->Drive);
    SetWindowText(hwndDlg, szText);
}

static HWND ChkdskDrvDialog = NULL;
static BOOLEAN bChkdskSuccess = FALSE;

static BOOLEAN NTAPI
ChkdskCallback(
    IN CALLBACKCOMMAND Command,
    IN ULONG SubAction,
    IN PVOID ActionInfo)
{
    PDWORD Progress;
    PBOOLEAN pSuccess;
    switch(Command)
    {
        case PROGRESS:
            Progress = (PDWORD)ActionInfo;
            SendDlgItemMessageW(ChkdskDrvDialog, 14002, PBM_SETPOS, (WPARAM)*Progress, 0);
            break;
        case DONE:
            pSuccess = (PBOOLEAN)ActionInfo;
            bChkdskSuccess = (*pSuccess);
            break;

        case VOLUMEINUSE:
        case INSUFFICIENTRIGHTS:
        case FSNOTSUPPORTED:
        case CLUSTERSIZETOOSMALL:
            bChkdskSuccess = FALSE;
            FIXME("\n");
            break;

        default:
            break;
    }

    return TRUE;
}

static BOOL
GetDefaultClusterSize(LPWSTR szFs, PDWORD pClusterSize, PULARGE_INTEGER TotalNumberOfBytes)
{
    DWORD ClusterSize;

    if (!wcsicmp(szFs, L"FAT16") ||
        !wcsicmp(szFs, L"FAT")) //REACTOS HACK
    {
        if (TotalNumberOfBytes->QuadPart <= (16 * 1024 * 1024))
            ClusterSize = 2048;
        else if (TotalNumberOfBytes->QuadPart <= (32 * 1024 * 1024))
            ClusterSize = 512;
        else if (TotalNumberOfBytes->QuadPart <= (64 * 1024 * 1024))
            ClusterSize = 1024;
        else if (TotalNumberOfBytes->QuadPart <= (128 * 1024 * 1024))
            ClusterSize = 2048;
        else if (TotalNumberOfBytes->QuadPart <= (256 * 1024 * 1024))
            ClusterSize = 4096;
        else if (TotalNumberOfBytes->QuadPart <= (512 * 1024 * 1024))
            ClusterSize = 8192;
        else if (TotalNumberOfBytes->QuadPart <= (1024 * 1024 * 1024))
            ClusterSize = 16384;
        else if (TotalNumberOfBytes->QuadPart <= (2048LL * 1024LL * 1024LL))
            ClusterSize = 32768;
        else if (TotalNumberOfBytes->QuadPart <= (4096LL * 1024LL * 1024LL))
            ClusterSize = 8192;
        else
            return FALSE;
    }
    else if (!wcsicmp(szFs, L"FAT32"))
    {
        if (TotalNumberOfBytes->QuadPart <= (64 * 1024 * 1024))
            ClusterSize = 512;
        else if (TotalNumberOfBytes->QuadPart <= (128   * 1024 * 1024))
            ClusterSize = 1024;
        else if (TotalNumberOfBytes->QuadPart <= (256   * 1024 * 1024))
            ClusterSize = 2048;
        else if (TotalNumberOfBytes->QuadPart <= (8192LL  * 1024LL * 1024LL))
            ClusterSize = 2048;
        else if (TotalNumberOfBytes->QuadPart <= (16384LL * 1024LL * 1024LL))
            ClusterSize = 8192;
        else if (TotalNumberOfBytes->QuadPart <= (32768LL * 1024LL * 1024LL))
            ClusterSize = 16384;
        else
            return FALSE;
    }
    else if (!wcsicmp(szFs, L"NTFS"))
    {
        if (TotalNumberOfBytes->QuadPart <= (512 * 1024 * 1024))
            ClusterSize = 512;
        else if (TotalNumberOfBytes->QuadPart <= (1024 * 1024 * 1024))
            ClusterSize = 1024;
        else if (TotalNumberOfBytes->QuadPart <= (2048LL * 1024LL * 1024LL))
            ClusterSize = 2048;
        else
            ClusterSize = 2048;
    }
    else
        return FALSE;

    *pClusterSize = ClusterSize;
    return TRUE;
}

static VOID
ChkDskNow(HWND hwndDlg, PFORMAT_DRIVE_CONTEXT pContext)
{
    DWORD ClusterSize = 0;
    WCHAR szFs[30];
    WCHAR szDrive[] = L"C:\\";
    ULARGE_INTEGER TotalNumberOfFreeBytes, FreeBytesAvailableUser;
    BOOLEAN bCorrectErrors = FALSE, bScanDrive = FALSE;

    szDrive[0] = pContext->Drive;
    if(!GetVolumeInformationW(szDrive, NULL, 0, NULL, NULL, NULL, szFs, sizeof(szFs) / sizeof(WCHAR)))
    {
        FIXME("failed to get drive fs type\n");
        return;
    }

    if (!GetDiskFreeSpaceExW(szDrive, &FreeBytesAvailableUser, &TotalNumberOfFreeBytes, NULL))
    {
        FIXME("failed to get drive space type\n");
        return;
    }

    if (!GetDefaultClusterSize(szFs, &ClusterSize, &TotalNumberOfFreeBytes))
    {
        FIXME("invalid cluster size\n");
        return;
    }

    if (SendDlgItemMessageW(hwndDlg, 14000, BM_GETCHECK, 0, 0) == BST_CHECKED)
        bCorrectErrors = TRUE;

    if (SendDlgItemMessageW(hwndDlg, 14001, BM_GETCHECK, 0, 0) == BST_CHECKED)
        bScanDrive = TRUE;

    ChkdskDrvDialog = hwndDlg;
    bChkdskSuccess = FALSE;
    SendDlgItemMessageW(hwndDlg, 14002, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    pContext->Chkdsk(szDrive, szFs, bCorrectErrors, TRUE, FALSE, bScanDrive, NULL, NULL, ChkdskCallback);

    ChkdskDrvDialog = NULL;
    pContext->Result = bChkdskSuccess;
    bChkdskSuccess = FALSE;
}

static INT_PTR CALLBACK
ChkDskDlg(
    HWND hwndDlg,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    PFORMAT_DRIVE_CONTEXT pContext;
    switch(uMsg)
    {
        case WM_INITDIALOG:
            SetWindowLongPtr(hwndDlg, DWLP_USER, (LONG_PTR)lParam);
            InitializeChkDskDialog(hwndDlg, (PFORMAT_DRIVE_CONTEXT)lParam);
            return TRUE;
        case WM_COMMAND:
            switch(LOWORD(wParam))
            {
                case IDCANCEL:
                    EndDialog(hwndDlg, 0);
                    break;
                case IDOK:
                    pContext = (PFORMAT_DRIVE_CONTEXT) GetWindowLongPtr(hwndDlg, DWLP_USER);
                    ChkDskNow(hwndDlg, pContext);
                    break;
            }
            break;
    }

    return FALSE;
}

static
ULONG
GetFreeBytesShare(ULONGLONG TotalNumberOfFreeBytes, ULONGLONG TotalNumberOfBytes)
{
    ULONGLONG Temp;

    if (TotalNumberOfFreeBytes == 0LL)
    {
        return 0;
    }

    Temp = TotalNumberOfBytes / 100;
    if (Temp >= TotalNumberOfFreeBytes)
    {
        return 1;
    }
    else
    {
        return (ULONG)(TotalNumberOfFreeBytes / Temp);
    }
}

static
VOID
PaintStaticControls(HWND hwndDlg, LPDRAWITEMSTRUCT drawItem)
{
    HBRUSH hBrush;

    if (drawItem->CtlID == 14013)
    {
        hBrush = CreateSolidBrush(RGB(0, 0, 255));
        if (hBrush)
        {
            FillRect(drawItem->hDC, &drawItem->rcItem, hBrush);
            DeleteObject((HGDIOBJ)hBrush);
        }
    }
    else if (drawItem->CtlID == 14014)
    {
        hBrush = CreateSolidBrush(RGB(255, 0, 255));
        if (hBrush)
        {
            FillRect(drawItem->hDC, &drawItem->rcItem, hBrush);
            DeleteObject((HGDIOBJ)hBrush);
        }
    }
    else if (drawItem->CtlID == 14015)
    {
        HBRUSH hBlueBrush;
        HBRUSH hMagBrush;
        RECT rect;
        LONG horzsize;
        LONGLONG Result;
        WCHAR szBuffer[20];

        hBlueBrush = CreateSolidBrush(RGB(0, 0, 255));
        hMagBrush = CreateSolidBrush(RGB(255, 0, 255));

        GetDlgItemTextW(hwndDlg, 14006, szBuffer, 20);
        Result = _wtoi(szBuffer);

        CopyRect(&rect, &drawItem->rcItem);
        horzsize = rect.right - rect.left;
        Result = (Result * horzsize) / 100;

        rect.right = drawItem->rcItem.right - Result;
        FillRect(drawItem->hDC, &rect, hBlueBrush);
        rect.left = rect.right;
        rect.right = drawItem->rcItem.right;
        FillRect(drawItem->hDC, &rect, hMagBrush);
        DeleteObject(hBlueBrush);
        DeleteObject(hMagBrush);
    }
}

static
VOID
InitializeGeneralDriveDialog(HWND hwndDlg, WCHAR *szDrive)
{
    WCHAR wszVolumeName[MAX_PATH+1] = {0};
    WCHAR wszFileSystem[MAX_PATH+1] = {0};
    WCHAR wszFormat[50];
    WCHAR wszBuf[128];
    BOOL bRet;
    UINT DriveType;
    ULARGE_INTEGER FreeBytesAvailable;
    ULARGE_INTEGER TotalNumberOfFreeBytes;
    ULARGE_INTEGER TotalNumberOfBytes;

    bRet = GetVolumeInformationW(szDrive, wszVolumeName, _countof(wszVolumeName), NULL, NULL, NULL, wszFileSystem, _countof(wszFileSystem));
    if (bRet)
    {
        /* set volume label */
        SetDlgItemTextW(hwndDlg, 14000, wszVolumeName);

        /* set filesystem type */
        SetDlgItemTextW(hwndDlg, 14002, wszFileSystem);
    }

    DriveType = GetDriveTypeW(szDrive);
    if (DriveType == DRIVE_FIXED || DriveType == DRIVE_CDROM)
    {
        if(GetDiskFreeSpaceExW(szDrive, &FreeBytesAvailable, &TotalNumberOfBytes, &TotalNumberOfFreeBytes))
        {
            ULONG SpacePercent;
            HANDLE hVolume;
            DWORD BytesReturned = 0;

            swprintf(wszBuf, L"\\\\.\\%c:", towupper(szDrive[0]));
            hVolume = CreateFileW(wszBuf, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (hVolume != INVALID_HANDLE_VALUE)
            {
                bRet = DeviceIoControl(hVolume, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, (LPVOID)&TotalNumberOfBytes, sizeof(ULARGE_INTEGER), &BytesReturned, NULL);
                if (bRet && StrFormatByteSizeW(TotalNumberOfBytes.QuadPart, wszBuf, sizeof(wszBuf) / sizeof(WCHAR)))
                    SetDlgItemTextW(hwndDlg, 14007, wszBuf);

                CloseHandle(hVolume);
            }

            TRACE("wszBuf %s hVolume %p bRet %d LengthInformation %ul BytesReturned %d\n", debugstr_w(wszBuf), hVolume, bRet, TotalNumberOfBytes.QuadPart, BytesReturned);

            if (StrFormatByteSizeW(TotalNumberOfBytes.QuadPart - FreeBytesAvailable.QuadPart, wszBuf, sizeof(wszBuf) / sizeof(WCHAR)))
                SetDlgItemTextW(hwndDlg, 14003, wszBuf);

            if (StrFormatByteSizeW(FreeBytesAvailable.QuadPart, wszBuf, sizeof(wszBuf) / sizeof(WCHAR)))
                SetDlgItemTextW(hwndDlg, 14005, wszBuf);

            SpacePercent = GetFreeBytesShare(TotalNumberOfFreeBytes.QuadPart, TotalNumberOfBytes.QuadPart);
            /* set free bytes percentage */
            swprintf(wszBuf, L"%u%%", SpacePercent);
            SetDlgItemTextW(hwndDlg, 14006, wszBuf);
            /* store used share amount */
            SpacePercent = 100 - SpacePercent;
            swprintf(wszBuf, L"%u%%", SpacePercent);
            SetDlgItemTextW(hwndDlg, 14004, wszBuf);
            if (DriveType == DRIVE_FIXED)
            {
                if (LoadStringW(shell32_hInstance, IDS_DRIVE_FIXED, wszBuf, sizeof(wszBuf) / sizeof(WCHAR)))
                    SetDlgItemTextW(hwndDlg, 14001, wszBuf);
            }
            else /* DriveType == DRIVE_CDROM) */
            {
                if (LoadStringW(shell32_hInstance, IDS_DRIVE_CDROM, wszBuf, sizeof(wszBuf) / sizeof(WCHAR)))
                    SetDlgItemTextW(hwndDlg, 14001, wszBuf);
            }
        }
    }
    /* set drive description */
    GetDlgItemTextW(hwndDlg, 14009, wszFormat, _countof(wszFormat));
    swprintf(wszBuf, wszFormat, szDrive);
    SetDlgItemTextW(hwndDlg, 14009, wszBuf);
}

static INT_PTR CALLBACK
DriveGeneralDlg(
    HWND hwndDlg,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    switch(uMsg)
    {
        case WM_INITDIALOG:
        {
            LPPROPSHEETPAGEW ppsp = (LPPROPSHEETPAGEW)lParam;
            if (ppsp == NULL)
                break;

            SetWindowLongPtr(hwndDlg, DWLP_USER, (LONG_PTR)ppsp->lParam);
            InitializeGeneralDriveDialog(hwndDlg, (LPWSTR)ppsp->lParam);
            return TRUE;
        }
        case WM_DRAWITEM:
        {
            LPDRAWITEMSTRUCT drawItem = (LPDRAWITEMSTRUCT)lParam;

            if (drawItem->CtlID >= 14013 && drawItem->CtlID <= 14015)
            {
                PaintStaticControls(hwndDlg, drawItem);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == 14010) /* Disk Cleanup */
            {
                UINT cchSysPath;
                STARTUPINFOW si;
                PROCESS_INFORMATION pi;
                WCHAR wszPath[MAX_PATH];
                LPWSTR pwszDrive = (WCHAR*)GetWindowLongPtr(hwndDlg, DWLP_USER);

                ZeroMemory(&si, sizeof(si));
                si.cb = sizeof(si);
                ZeroMemory(&pi, sizeof(pi));
                cchSysPath = GetSystemDirectoryW(wszPath, MAX_PATH);
                if (!cchSysPath)
                    break;
                StringCchPrintfW(wszPath + cchSysPath, _countof(wszPath) - cchSysPath, L"\\cleanmgr.exe /D %s", pwszDrive);
                if (CreateProcessW(NULL, wszPath, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
                {
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                }
            }
            break;
        case WM_NOTIFY:
            if (LOWORD(wParam) == 14000) // Label
            {
                if (HIWORD(wParam) == EN_CHANGE)
                {
                    PropSheet_Changed(GetParent(hwndDlg), hwndDlg);
                }
                break;
            }
            else if (((LPNMHDR)lParam)->hwndFrom == GetParent(hwndDlg))
            {
                /* Property Sheet */
                LPPSHNOTIFY lppsn = (LPPSHNOTIFY)lParam;
                
                if (lppsn->hdr.code == PSN_APPLY)
                {
                    LPWSTR pwszDrive = (LPWSTR)GetWindowLongPtr(hwndDlg, DWLP_USER);
                    WCHAR wszBuf[256];

                    if (pwszDrive && GetDlgItemTextW(hwndDlg, 14000, wszBuf, sizeof(wszBuf) / sizeof(WCHAR)))
                        SetVolumeLabelW(pwszDrive, wszBuf);
                    SetWindowLongPtr(hwndDlg, DWL_MSGRESULT, PSNRET_NOERROR);
                    return TRUE;
                }
            }
            break;

        default:
            break;
    }

    return FALSE;
}

static BOOL
InitializeFmifsLibrary(PFORMAT_DRIVE_CONTEXT pContext)
{
    INITIALIZE_FMIFS InitFmifs;
    BOOLEAN ret;
    HMODULE hLibrary;

    hLibrary = pContext->hLibrary = LoadLibraryW(L"fmifs.dll");
    if(!hLibrary)
    {
        ERR("failed to load fmifs.dll\n");
        return FALSE;
    }

    InitFmifs = (INITIALIZE_FMIFS)GetProcAddress(hLibrary, "InitializeFmIfs");
    if (!InitFmifs)
    {
        ERR("InitializeFmIfs export is missing\n");
        FreeLibrary(hLibrary);
        return FALSE;
    }

    ret = (*InitFmifs)(NULL, DLL_PROCESS_ATTACH, NULL);
    if (!ret)
    {
        ERR("fmifs failed to initialize\n");
        FreeLibrary(hLibrary);
        return FALSE;
    }

    pContext->QueryAvailableFileSystemFormat = (QUERY_AVAILABLEFSFORMAT)GetProcAddress(hLibrary, "QueryAvailableFileSystemFormat");
    if (!pContext->QueryAvailableFileSystemFormat)
    {
        ERR("QueryAvailableFileSystemFormat export is missing\n");
        FreeLibrary(hLibrary);
        return FALSE;
    }

    pContext->FormatEx = (FORMAT_EX) GetProcAddress(hLibrary, "FormatEx");
    if (!pContext->FormatEx)
    {
        ERR("FormatEx export is missing\n");
        FreeLibrary(hLibrary);
        return FALSE;
    }

    pContext->EnableVolumeCompression = (ENABLEVOLUMECOMPRESSION) GetProcAddress(hLibrary, "EnableVolumeCompression");
    if (!pContext->FormatEx)
    {
        ERR("EnableVolumeCompression export is missing\n");
        FreeLibrary(hLibrary);
        return FALSE;
    }

    pContext->Chkdsk = (CHKDSK) GetProcAddress(hLibrary, "Chkdsk");
    if (!pContext->Chkdsk)
    {
        ERR("Chkdsk export is missing\n");
        FreeLibrary(hLibrary);
        return FALSE;
    }

    return TRUE;
}

static INT_PTR CALLBACK
DriveExtraDlg(
    HWND hwndDlg,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            LPPROPSHEETPAGEW ppsp = (LPPROPSHEETPAGEW)lParam;
            SetWindowLongPtr(hwndDlg, DWLP_USER, (LONG_PTR)ppsp->lParam);
            return TRUE;
        }
        case WM_COMMAND:
        {
            STARTUPINFOW si;
            PROCESS_INFORMATION pi;
            WCHAR szPath[MAX_PATH + 10];
            WCHAR szArg[MAX_PATH];
            WCHAR *szDrive;
            DWORD dwSize;
            FORMAT_DRIVE_CONTEXT Context;

            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            ZeroMemory(&pi, sizeof(pi));

            szDrive = (WCHAR*)GetWindowLongPtr(hwndDlg, DWLP_USER);
            switch(LOWORD(wParam))
            {
                case 14000:
                    if (InitializeFmifsLibrary(&Context))
                    {
                        Context.Drive = szDrive[0];
                        DialogBoxParamW(shell32_hInstance, L"CHKDSK_DLG", hwndDlg, ChkDskDlg, (LPARAM)&Context);
                        FreeLibrary(Context.hLibrary);
                    }
                    break;
                case 14001:
                    dwSize = sizeof(szPath);
                    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                                     L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MyComputer\\DefragPath",
                                     NULL,
                                     RRF_RT_REG_EXPAND_SZ,
                                     NULL,
                                     (PVOID)szPath,
                                     &dwSize) == ERROR_SUCCESS)
                    {
                        swprintf(szArg, szPath, szDrive[0]);
                        if (!GetSystemDirectoryW(szPath, MAX_PATH))
                            break;
                        szDrive = PathAddBackslashW(szPath);
                        if (!szDrive)
                            break;

                        wcscat(szDrive, L"mmc.exe");
                        if (CreateProcessW(szPath, szArg, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
                        {
                            CloseHandle(pi.hProcess);
                            CloseHandle(pi.hThread);
                        }
                    }
                    break;
                case 14002:
                    dwSize = sizeof(szPath);
                    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                                     L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MyComputer\\BackupPath",
                                     NULL,
                                     RRF_RT_REG_EXPAND_SZ,
                                     NULL,
                                     (PVOID)szPath,
                                     &dwSize) == ERROR_SUCCESS)
                    {
                        if (CreateProcessW(szPath, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
                        {
                            CloseHandle(pi.hProcess);
                            CloseHandle(pi.hThread);
                        }
                    }
            }
            break;
        }
    }
    return FALSE;
}

static INT_PTR CALLBACK
DriveHardwareDlg(
    HWND hwndDlg,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    UNREFERENCED_PARAMETER(wParam);

    switch(uMsg)
    {
        case WM_INITDIALOG:
        {
            GUID Guid = GUID_DEVCLASS_DISKDRIVE;

            /* create the hardware page */
            DeviceCreateHardwarePageEx(hwndDlg, &Guid, 1, HWPD_STANDARDLIST);
            break;
        }
    }

    return FALSE;
}

static HRESULT CALLBACK
AddPropSheetPageProc(HPROPSHEETPAGE hpage, LPARAM lParam)
{
    PROPSHEETHEADER *ppsh = (PROPSHEETHEADER *)lParam;
    if (ppsh != NULL && ppsh->nPages < MAX_PROPERTY_SHEET_PAGE)
    {
        ppsh->phpage[ppsh->nPages++] = hpage;
        return TRUE;
    }
    return FALSE;
}

typedef struct _DRIVE_PROP_PAGE
{
    LPCSTR resname;
    DLGPROC dlgproc;
    UINT DriveType;
} DRIVE_PROP_PAGE;

BOOL
SH_ShowDriveProperties(WCHAR *pwszDrive, LPCITEMIDLIST pidlFolder, LPCITEMIDLIST *apidl)
{
    HPSXA hpsx = NULL;
    HPROPSHEETPAGE hpsp[MAX_PROPERTY_SHEET_PAGE];
    PROPSHEETHEADERW psh;
    HWND hwnd;
    UINT i, DriveType;
    WCHAR wszName[256];
    CComPtr<IDataObject> pDataObj;

    static const DRIVE_PROP_PAGE PropPages[] =
    {
        { "DRIVE_GENERAL_DLG", DriveGeneralDlg, -1},
        { "DRIVE_EXTRA_DLG", DriveExtraDlg, DRIVE_FIXED},
        { "DRIVE_HARDWARE_DLG", DriveHardwareDlg, -1}
    };

    ZeroMemory(&psh, sizeof(PROPSHEETHEADERW));
    psh.dwSize = sizeof(PROPSHEETHEADERW);
    psh.dwFlags = 0; // FIXME: make it modeless
    psh.hwndParent = NULL;
    psh.nStartPage = 0;
    psh.phpage = hpsp;

    if (GetVolumeInformationW(pwszDrive, wszName, sizeof(wszName) / sizeof(WCHAR), NULL, NULL, NULL, NULL, 0))
    {
        psh.pszCaption = wszName;
        psh.dwFlags |= PSH_PROPTITLE;
        if (wszName[0] == UNICODE_NULL)
        {
            /* FIXME: check if disk is a really a local hdd */
            i = LoadStringW(shell32_hInstance, IDS_DRIVE_FIXED, wszName, sizeof(wszName) / sizeof(WCHAR) - 6);
            StringCchPrintf(wszName + i, sizeof(wszName) / sizeof(WCHAR) - i, L" (%s)", pwszDrive);
        }
    }

    DriveType = GetDriveTypeW(pwszDrive);
    for (i = 0; i < _countof(PropPages); i++)
    {
        if (PropPages[i].DriveType == (UINT)-1 || PropPages[i].DriveType == DriveType)
        {
            HPROPSHEETPAGE hprop = SH_CreatePropertySheetPage(PropPages[i].resname, PropPages[i].dlgproc, (LPARAM)pwszDrive, NULL);
            if (hprop)
            {
                hpsp[psh.nPages] = hprop;
                psh.nPages++;
            }
        }
    }

    if (SHCreateDataObject(pidlFolder, 1, apidl, NULL, IID_IDataObject, (void **)&pDataObj) == S_OK)
    {
        hpsx = SHCreatePropSheetExtArrayEx(HKEY_CLASSES_ROOT, L"Drive", MAX_PROPERTY_SHEET_PAGE - _countof(PropPages), pDataObj);
        if (hpsx)
            SHAddFromPropSheetExtArray(hpsx, (LPFNADDPROPSHEETPAGE)AddPropSheetPageProc, (LPARAM)&psh);
    }

    hwnd = (HWND)PropertySheetW(&psh);

    if (hpsx)
        SHDestroyPropSheetExtArray(hpsx);

    if (!hwnd)
        return FALSE;
    return TRUE;
}

static VOID
InsertDefaultClusterSizeForFs(HWND hwndDlg, PFORMAT_DRIVE_CONTEXT pContext)
{
    WCHAR wszBuf[100] = {0};
    WCHAR szDrive[] = L"C:\\";
    INT iSelIndex;
    ULARGE_INTEGER FreeBytesAvailableUser, TotalNumberOfBytes;
    DWORD ClusterSize;
    LRESULT lIndex;
    HWND hDlgCtrl;

    hDlgCtrl = GetDlgItem(hwndDlg, 28677);
    iSelIndex = SendMessage(hDlgCtrl, CB_GETCURSEL, 0, 0);
    if (iSelIndex == CB_ERR)
        return;

    if (SendMessageW(hDlgCtrl, CB_GETLBTEXT, iSelIndex, (LPARAM)wszBuf) == CB_ERR)
        return;

    szDrive[0] = pContext->Drive + 'A';

    if (!GetDiskFreeSpaceExW(szDrive, &FreeBytesAvailableUser, &TotalNumberOfBytes, NULL))
        return;

    if (!wcsicmp(wszBuf, L"FAT16") ||
        !wcsicmp(wszBuf, L"FAT")) //REACTOS HACK
    {
        if (!GetDefaultClusterSize(wszBuf, &ClusterSize, &TotalNumberOfBytes))
        {
            TRACE("FAT16 is not supported on hdd larger than 4G current %lu\n", TotalNumberOfBytes.QuadPart);
            SendMessageW(hDlgCtrl, CB_DELETESTRING, iSelIndex, 0);
            return;
        }

        if (LoadStringW(shell32_hInstance, IDS_DEFAULT_CLUSTER_SIZE, wszBuf, sizeof(wszBuf) / sizeof(WCHAR)))
        {
            hDlgCtrl = GetDlgItem(hwndDlg, 28680);
            SendMessageW(hDlgCtrl, CB_RESETCONTENT, 0, 0);
            lIndex = SendMessageW(hDlgCtrl, CB_ADDSTRING, 0, (LPARAM)wszBuf);
            if (lIndex != CB_ERR)
                SendMessageW(hDlgCtrl, CB_SETITEMDATA, lIndex, (LPARAM)ClusterSize);
            SendMessageW(hDlgCtrl, CB_SETCURSEL, 0, 0);
        }
    }
    else if (!wcsicmp(wszBuf, L"FAT32"))
    {
        if (!GetDefaultClusterSize(wszBuf, &ClusterSize, &TotalNumberOfBytes))
        {
            TRACE("FAT32 is not supported on hdd larger than 32G current %lu\n", TotalNumberOfBytes.QuadPart);
            SendMessageW(hDlgCtrl, CB_DELETESTRING, iSelIndex, 0);
            return;
        }

        if (LoadStringW(shell32_hInstance, IDS_DEFAULT_CLUSTER_SIZE, wszBuf, sizeof(wszBuf) / sizeof(WCHAR)))
        {
            hDlgCtrl = GetDlgItem(hwndDlg, 28680);
            SendMessageW(hDlgCtrl, CB_RESETCONTENT, 0, 0);
            lIndex = SendMessageW(hDlgCtrl, CB_ADDSTRING, 0, (LPARAM)wszBuf);
            if (lIndex != CB_ERR)
                SendMessageW(hDlgCtrl, CB_SETITEMDATA, lIndex, (LPARAM)ClusterSize);
            SendMessageW(hDlgCtrl, CB_SETCURSEL, 0, 0);
        }
    }
    else if (!wcsicmp(wszBuf, L"NTFS"))
    {
        if (!GetDefaultClusterSize(wszBuf, &ClusterSize, &TotalNumberOfBytes))
        {
            TRACE("NTFS is not supported on hdd larger than 2TB current %lu\n", TotalNumberOfBytes.QuadPart);
            SendMessageW(hDlgCtrl, CB_DELETESTRING, iSelIndex, 0);
            return;
        }

        hDlgCtrl = GetDlgItem(hwndDlg, 28680);
        if (LoadStringW(shell32_hInstance, IDS_DEFAULT_CLUSTER_SIZE, wszBuf, sizeof(wszBuf) / sizeof(WCHAR)))
        {
            SendMessageW(hDlgCtrl, CB_RESETCONTENT, 0, 0);
            lIndex = SendMessageW(hDlgCtrl, CB_ADDSTRING, 0, (LPARAM)wszBuf);
            if (lIndex != CB_ERR)
                SendMessageW(hDlgCtrl, CB_SETITEMDATA, lIndex, (LPARAM)ClusterSize);
            SendMessageW(hDlgCtrl, CB_SETCURSEL, 0, 0);
        }
        ClusterSize = 512;
        for (lIndex = 0; lIndex < 4; lIndex++)
        {
            TotalNumberOfBytes.QuadPart = ClusterSize;
            if (StrFormatByteSizeW(TotalNumberOfBytes.QuadPart, wszBuf, sizeof(wszBuf) / sizeof(WCHAR)))
            {
                lIndex = SendMessageW(hDlgCtrl, CB_ADDSTRING, 0, (LPARAM)wszBuf);
                if (lIndex != CB_ERR)
                    SendMessageW(hDlgCtrl, CB_SETITEMDATA, lIndex, (LPARAM)ClusterSize);
            }
            ClusterSize *= 2;
        }
    }
    else
    {
        FIXME("unknown fs\n");
        SendDlgItemMessageW(hwndDlg, 28680, CB_RESETCONTENT, iSelIndex, 0);
        return;
    }
}

static VOID
InitializeFormatDriveDlg(HWND hwndDlg, PFORMAT_DRIVE_CONTEXT pContext)
{
    WCHAR szText[120];
    WCHAR szDrive[] = L"C:\\";
    WCHAR szFs[30] = L"";
    INT cchText;
    ULARGE_INTEGER FreeBytesAvailableUser, TotalNumberOfBytes;
    DWORD dwIndex, dwDefault;
    UCHAR uMinor, uMajor;
    BOOLEAN Latest;
    HWND hwndFileSystems;

    cchText = GetWindowTextW(hwndDlg, szText, sizeof(szText) / sizeof(WCHAR) - 1);
    if (cchText < 0)
        cchText = 0;
    szText[cchText++] = L' ';
    szDrive[0] = pContext->Drive + L'A';
    if (GetVolumeInformationW(szDrive, &szText[cchText], (sizeof(szText) / sizeof(WCHAR)) - cchText, NULL, NULL, NULL, szFs, sizeof(szFs) / sizeof(WCHAR)))
    {
        if (szText[cchText] == UNICODE_NULL)
        {
            /* load default volume label */
            cchText += LoadStringW(shell32_hInstance, IDS_DRIVE_FIXED, &szText[cchText], (sizeof(szText) / sizeof(WCHAR)) - cchText);
        }
        else
        {
            /* set volume label */
            SetDlgItemTextW(hwndDlg, 28679, &szText[cchText]);
            cchText += wcslen(&szText[cchText]);
        }
    }

    StringCchPrintfW(szText + cchText, _countof(szText) - cchText, L" (%c)", szDrive[0]);

    /* set window text */
    SetWindowTextW(hwndDlg, szText);

    if (GetDiskFreeSpaceExW(szDrive, &FreeBytesAvailableUser, &TotalNumberOfBytes, NULL))
    {
        if (StrFormatByteSizeW(TotalNumberOfBytes.QuadPart, szText, sizeof(szText) / sizeof(WCHAR)))
        {
            /* add drive capacity */
            SendDlgItemMessageW(hwndDlg, 28673, CB_ADDSTRING, 0, (LPARAM)szText);
            SendDlgItemMessageW(hwndDlg, 28673, CB_SETCURSEL, 0, (LPARAM)0);
        }
    }

    if (pContext->Options & SHFMT_OPT_FULL)
    {
        /* check quick format button */
        SendDlgItemMessageW(hwndDlg, 28674, BM_SETCHECK, BST_CHECKED, 0);
    }

    /* enumerate all available filesystems */
    dwIndex = 0;
    dwDefault = 0;
    hwndFileSystems = GetDlgItem(hwndDlg, 28677);

    while(pContext->QueryAvailableFileSystemFormat(dwIndex, szText, &uMajor, &uMinor, &Latest))
    {
        if (!wcsicmp(szText, szFs))
            dwDefault = dwIndex;

        SendMessageW(hwndFileSystems, CB_ADDSTRING, 0, (LPARAM)szText);
        dwIndex++;
    }

    if (!dwIndex)
    {
        ERR("no filesystem providers\n");
        return;
    }

    /* select default filesys */
    SendMessageW(hwndFileSystems, CB_SETCURSEL, dwDefault, 0);
    /* setup cluster combo */
    InsertDefaultClusterSizeForFs(hwndDlg, pContext);
    /* hide progress control */
    ShowWindow(GetDlgItem(hwndDlg, 28678), SW_HIDE);
}

static HWND FormatDrvDialog = NULL;
static BOOLEAN bSuccess = FALSE;

static BOOLEAN NTAPI
FormatExCB(
    IN CALLBACKCOMMAND Command,
    IN ULONG SubAction,
    IN PVOID ActionInfo)
{
    PDWORD Progress;
    PBOOLEAN pSuccess;
    switch(Command)
    {
        case PROGRESS:
            Progress = (PDWORD)ActionInfo;
            SendDlgItemMessageW(FormatDrvDialog, 28678, PBM_SETPOS, (WPARAM)*Progress, 0);
            break;
        case DONE:
            pSuccess = (PBOOLEAN)ActionInfo;
            bSuccess = (*pSuccess);
            break;

        case VOLUMEINUSE:
        case INSUFFICIENTRIGHTS:
        case FSNOTSUPPORTED:
        case CLUSTERSIZETOOSMALL:
            bSuccess = FALSE;
            FIXME("\n");
            break;

        default:
            break;
    }

    return TRUE;
}

VOID
FormatDrive(HWND hwndDlg, PFORMAT_DRIVE_CONTEXT pContext)
{
    WCHAR szDrive[4] = { L'C', ':', '\\', 0 };
    WCHAR szFileSys[40] = {0};
    WCHAR szLabel[40] = {0};
    INT iSelIndex;
    UINT Length;
    HWND hDlgCtrl;
    BOOL QuickFormat;
    DWORD ClusterSize;

    /* set volume path */
    szDrive[0] = pContext->Drive;

    /* get filesystem */
    hDlgCtrl = GetDlgItem(hwndDlg, 28677);
    iSelIndex = SendMessageW(hDlgCtrl, CB_GETCURSEL, 0, 0);
    if (iSelIndex == CB_ERR)
    {
        FIXME("\n");
        return;
    }
    Length = SendMessageW(hDlgCtrl, CB_GETLBTEXTLEN, iSelIndex, 0);
    if ((int)Length == CB_ERR || Length + 1 > sizeof(szFileSys) / sizeof(WCHAR))
    {
        FIXME("\n");
        return;
    }

    /* retrieve the file system */
    SendMessageW(hDlgCtrl, CB_GETLBTEXT, iSelIndex, (LPARAM)szFileSys);
    szFileSys[(sizeof(szFileSys)/sizeof(WCHAR))-1] = L'\0';

    /* retrieve the volume label */
    hDlgCtrl = GetWindow(hwndDlg, 28679);
    Length = SendMessageW(hDlgCtrl, WM_GETTEXTLENGTH, 0, 0);
    if (Length + 1 > sizeof(szLabel) / sizeof(WCHAR))
    {
        FIXME("\n");
        return;
    }
    SendMessageW(hDlgCtrl, WM_GETTEXT, sizeof(szLabel) / sizeof(WCHAR), (LPARAM)szLabel);
    szLabel[(sizeof(szLabel)/sizeof(WCHAR))-1] = L'\0';

    /* check for quickformat */
    if (SendDlgItemMessageW(hwndDlg, 28674, BM_GETCHECK, 0, 0) == BST_CHECKED)
        QuickFormat = TRUE;
    else
        QuickFormat = FALSE;

    /* get the cluster size */
    hDlgCtrl = GetDlgItem(hwndDlg, 28680);
    iSelIndex = SendMessageW(hDlgCtrl, CB_GETCURSEL, 0, 0);
    if (iSelIndex == CB_ERR)
    {
        FIXME("\n");
        return;
    }
    ClusterSize = SendMessageW(hDlgCtrl, CB_GETITEMDATA, iSelIndex, 0);
    if ((int)ClusterSize == CB_ERR)
    {
        FIXME("\n");
        return;
    }

    hDlgCtrl = GetDlgItem(hwndDlg, 28680);
    ShowWindow(hDlgCtrl, SW_SHOW);
    SendMessageW(hDlgCtrl, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    bSuccess = FALSE;

    /* FIXME
     * will cause display problems
     * when performing more than one format
     */
    FormatDrvDialog = hwndDlg;

    pContext->FormatEx(szDrive,
                       FMIFS_HARDDISK, /* FIXME */
                       szFileSys,
                       szLabel,
                       QuickFormat,
                       ClusterSize,
                       FormatExCB);

    ShowWindow(hDlgCtrl, SW_HIDE);
    FormatDrvDialog = NULL;
    if (!bSuccess)
    {
        pContext->Result = SHFMT_ERROR;
    }
    else if (QuickFormat)
    {
        pContext->Result = SHFMT_OPT_FULL;
    }
    else
    {
        pContext->Result =  FALSE;
    }
}

static INT_PTR CALLBACK
FormatDriveDlg(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    PFORMAT_DRIVE_CONTEXT pContext;

    switch(uMsg)
    {
        case WM_INITDIALOG:
            InitializeFormatDriveDlg(hwndDlg, (PFORMAT_DRIVE_CONTEXT)lParam);
            SetWindowLongPtr(hwndDlg, DWLP_USER, (LONG_PTR)lParam);
            return TRUE;
        case WM_COMMAND:
            switch(LOWORD(wParam))
            {
                case IDOK:
                    pContext = (PFORMAT_DRIVE_CONTEXT)GetWindowLongPtr(hwndDlg, DWLP_USER);
                    FormatDrive(hwndDlg, pContext);
                    break;
                case IDCANCEL:
                    pContext = (PFORMAT_DRIVE_CONTEXT)GetWindowLongPtr(hwndDlg, DWLP_USER);
                    EndDialog(hwndDlg, pContext->Result);
                    break;
                case 28677: // filesystem combo
                    if (HIWORD(wParam) == CBN_SELENDOK)
                    {
                        pContext = (PFORMAT_DRIVE_CONTEXT)GetWindowLongPtr(hwndDlg, DWLP_USER);
                        InsertDefaultClusterSizeForFs(hwndDlg, pContext);
                    }
                    break;
            }
    }
    return FALSE;
}

/*************************************************************************
 *              SHFormatDrive (SHELL32.@)
 */

DWORD
WINAPI
SHFormatDrive(HWND hwnd, UINT drive, UINT fmtID, UINT options)
{
    FORMAT_DRIVE_CONTEXT Context;
    int result;

    TRACE("%p, 0x%08x, 0x%08x, 0x%08x - stub\n", hwnd, drive, fmtID, options);

    if (!InitializeFmifsLibrary(&Context))
    {
        ERR("failed to initialize fmifs\n");
        return SHFMT_NOFORMAT;
    }

    Context.Drive = drive;
    Context.Options = options;

    result = DialogBoxParamW(shell32_hInstance, L"FORMAT_DLG", hwnd, FormatDriveDlg, (LPARAM)&Context);

    FreeLibrary(Context.hLibrary);
    return result;
}

