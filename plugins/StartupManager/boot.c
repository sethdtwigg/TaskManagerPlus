/*
 * TaskManagerPlus StartupManager: boot timing from the local Windows event log
 * (Microsoft-Windows-Diagnostics-Performance/Operational). No network; the log is read with the local Event Log API.
 *
 * Degrades gracefully: when the log is missing, empty or cannot be read (usually "access denied" without admin
 * rights) the Startup tab keeps its size-based impact estimate and says why in the Boot timing window.
 */

#include "startupmanager.h"
#include <winevt.h>

#define SM_BOOT_CHANNEL L"Microsoft-Windows-Diagnostics-Performance/Operational"
#define SM_BOOT_QUERY \
    L"*[System[(EventID=100 or EventID=101 or EventID=102 or EventID=103 or EventID=106 or EventID=108)" \
    L" and TimeCreated[timediff(@SystemTime) <= 2592000000]]]" // last 30 days
#define SM_BOOT_MAX_EVENTS 600
#define SM_BOOT_MIN_INTERVAL_MS (60 * 1000)

typedef enum _SM_BOOT_STATUS
{
    SmBootNotLoaded,
    SmBootOk,
    SmBootNoEvents,
    SmBootAccessDenied,
    SmBootUnavailable,
    SmBootSampleFile
} SM_BOOT_STATUS;

static BP_STATE SmBootState;
static BP_EVENT *SmBootScratch = NULL;
static BOOLEAN SmBootInitialized = FALSE;
static ULONG64 SmBootLoadedTick = 0;
static SM_BOOT_STATUS SmBootStatus = SmBootNotLoaded;
static ULONG SmBootError = 0;

static VOID SmBootReset(
    VOID
    )
{
    if (SmBootInitialized)
        BpFree(&SmBootState);

    BpInit(&SmBootState);
    SmBootInitialized = TRUE;

    if (!SmBootScratch)
        SmBootScratch = PhAllocateZero(sizeof(BP_EVENT));
}

static BOOLEAN SmBootLoadSampleFile(
    _In_ PPH_STRING FileName
    )
{
    HANDLE fileHandle;
    LARGE_INTEGER size;
    PVOID buffer;
    PPH_STRING text;
    PWSTR cursor;
    ULONG bytesRead = 0;

    if (!NT_SUCCESS(PhCreateFileWin32(&fileHandle, FileName->Buffer, FILE_GENERIC_READ, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE)))
        return FALSE;

    if (!NT_SUCCESS(PhGetFileSize(fileHandle, &size)) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024)
    {
        NtClose(fileHandle);
        return FALSE;
    }

    buffer = PhAllocate((SIZE_T)size.QuadPart);

    if (!NT_SUCCESS(PhReadFile(fileHandle, buffer, (ULONG)size.QuadPart, NULL, &bytesRead)))
        bytesRead = 0;

    NtClose(fileHandle);
    text = bytesRead ? PhConvertUtf8ToUtf16Ex(buffer, bytesRead) : NULL;
    PhFree(buffer);

    if (!text)
        return FALSE;

    cursor = text->Buffer;

    for (;;)
    {
        PWSTR end = wcsstr(cursor, L"</Event>");
        SIZE_T length;
        PWSTR segment;

        if (!end)
            break;

        length = (SIZE_T)(end - cursor) + 8;
        segment = PhAllocate((length + 1) * sizeof(WCHAR));
        memcpy(segment, cursor, length * sizeof(WCHAR));
        segment[length] = 0;
        BpAddXml(&SmBootState, segment, SmBootScratch);
        PhFree(segment);
        cursor = end + 8;
    }

    PhDereferenceObject(text);
    return TRUE;
}

static VOID SmBootLoadEventLog(
    VOID
    )
{
    EVT_HANDLE query;
    ULONG total = 0;

    query = EvtQuery(NULL, SM_BOOT_CHANNEL, SM_BOOT_QUERY, EvtQueryChannelPath | EvtQueryReverseDirection);

    if (!query)
    {
        SmBootError = GetLastError();
        SmBootStatus = SmBootError == ERROR_ACCESS_DENIED ? SmBootAccessDenied : SmBootUnavailable;
        return;
    }

    while (total < SM_BOOT_MAX_EVENTS)
    {
        EVT_HANDLE events[32];
        ULONG returned = 0;
        ULONG i;

        if (!EvtNext(query, RTL_NUMBER_OF(events), events, 1000, 0, &returned))
        {
            ULONG error = GetLastError();

            if (error != ERROR_NO_MORE_ITEMS && !total)
            {
                SmBootError = error;
                SmBootStatus = error == ERROR_ACCESS_DENIED ? SmBootAccessDenied : SmBootUnavailable;
            }

            break;
        }

        for (i = 0; i < returned; i++)
        {
            ULONG used = 0;
            ULONG props = 0;
            PWSTR xml = NULL;

            EvtRender(NULL, events[i], EvtRenderEventXml, 0, NULL, &used, &props);

            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && used && used < 256 * 1024)
            {
                xml = PhAllocate(used + sizeof(WCHAR));

                if (EvtRender(NULL, events[i], EvtRenderEventXml, used, xml, &used, &props))
                    BpAddXml(&SmBootState, xml, SmBootScratch);

                PhFree(xml);
            }

            EvtClose(events[i]);
            total++;
        }
    }

    EvtClose(query);

    if (SmBootStatus == SmBootNotLoaded)
        SmBootStatus = (SmBootState.EventsSeen || total) ? SmBootOk : SmBootNoEvents;
}

VOID SmBootRefresh(
    _In_ BOOLEAN Force
    )
{
    ULONG64 now = NtGetTickCount64();
    PPH_STRING sample;

    if (!Force && SmBootStatus != SmBootNotLoaded && now - SmBootLoadedTick < SM_BOOT_MIN_INTERVAL_MS)
        return;

    SmBootLoadedTick = now;
    SmBootStatus = SmBootNotLoaded;
    SmBootError = 0;
    SmBootReset();

    sample = PhGetStringSetting(SETTING_NAME_BOOT_SAMPLE_FILE);

    if (sample->Length)
    {
        if (SmBootLoadSampleFile(sample))
            SmBootStatus = SmBootState.EventsSeen ? SmBootSampleFile : SmBootNoEvents;
        else
            SmBootStatus = SmBootUnavailable;
    }
    else
    {
        SmBootLoadEventLog();
    }

    PhDereferenceObject(sample);
}

static PPH_STRING SmBootFormatMs(
    _In_ ULONG Milliseconds
    )
{
    if (Milliseconds < 1000)
        return PhFormatString(L"%lu ms", Milliseconds);

    return PhFormatString(L"%.1f s", (double)Milliseconds / 1000.0);
}

VOID SmBootApplyToItem(
    _Inout_ PSM_STARTUP_ITEM Item
    )
{
    const BP_ITEM *found;
    PPH_STRING time;

    PhClearReference(&Item->ImpactText);
    PhClearReference(&Item->BootDelayText);
    Item->BootDelayMs = 0;
    Item->BootDegradationMs = 0;
    Item->ImpactMeasured = FALSE;

    if (!SmBootInitialized || !SmBootState.ItemCount)
        return;

    found = BpFindItem(
        &SmBootState,
        Item->Name ? Item->Name->Buffer : NULL,
        Item->FileName ? Item->FileName->Buffer : NULL
        );

    if (!found || !found->TotalMs)
        return;

    Item->BootDelayMs = found->TotalMs;
    Item->BootDegradationMs = found->DegradationMs;
    Item->ImpactMeasured = TRUE;
    Item->Impact = (SM_IMPACT)BpRateImpact(found->TotalMs); // SmImpactLow = 1 ... SmImpactHigh = 3
    time = SmBootFormatMs(found->TotalMs);
    Item->BootDelayText = time;
    Item->ImpactText = PhFormatString(L"%s (measured)", SmGetImpactString(Item->Impact)->Buffer);
}

PPH_STRING SmBootFormatSummary(
    VOID
    )
{
    PH_STRING_BUILDER sb;
    const BP_BOOT *b = &SmBootState.Boot;
    ULONG i;
    ULONG shown = 0;
    PPH_STRING text;

    SmBootRefresh(FALSE);
    PhInitializeStringBuilder(&sb, 1024);

    switch (SmBootStatus)
    {
    case SmBootAccessDenied:
        PhAppendStringBuilder2(&sb, L"The Windows event log \"Diagnostics-Performance\" cannot be read without administrator rights (access denied).\r\n\r\n");
        PhAppendStringBuilder2(&sb, L"Run TaskManagerPlus as administrator to see the boot breakdown and measured per-item delays. Until then the Startup tab keeps its file-size based impact estimate.");
        goto done;
    case SmBootUnavailable:
        text = PhFormatString(L"The Diagnostics-Performance event log could not be opened (error %lu).\r\n\r\nThe Startup tab keeps its file-size based impact estimate.", SmBootError);
        PhAppendStringBuilder(&sb, &text->sr);
        PhDereferenceObject(text);
        goto done;
    case SmBootNoEvents:
    case SmBootNotLoaded:
        PhAppendStringBuilder2(&sb, L"The event log has no boot performance events from the last 30 days (the log may have been cleared or the service that writes it is off).\r\n\r\nThe Startup tab keeps its file-size based impact estimate.");
        goto done;
    default:
        break;
    }

    if (SmBootStatus == SmBootSampleFile)
        PhAppendStringBuilder2(&sb, L"(Test data from a sample file, not the event log.)\r\n\r\n");

    if (SmBootState.HasBoot)
    {
        SYSTEMTIME st;
        LARGE_INTEGER when;
        PPH_STRING s;

        when.QuadPart = b->Time;
        PhLargeIntegerToLocalSystemTime(&st, &when);
        text = PhFormatString(L"Last boot measured %04u-%02u-%02u %02u:%02u   (%lu boot record(s) in the last 30 days)\r\n\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, SmBootState.BootCount);
        PhAppendStringBuilder(&sb, &text->sr);
        PhDereferenceObject(text);

#define SM_LINE(Label, Value) \
        if (Value) \
        { \
            s = SmBootFormatMs(Value); \
            text = PhFormatString(L"  %s: %s\r\n", Label, s->Buffer); \
            PhAppendStringBuilder(&sb, &text->sr); \
            PhDereferenceObject(text); \
            PhDereferenceObject(s); \
        }

        SM_LINE(L"Boot time (total)", b->BootTimeMs)
        SM_LINE(L"Power on to desktop (main path)", b->MainPathBootTimeMs)
        SM_LINE(L"Desktop to idle (post boot)", b->PostBootTimeMs)
        PhAppendStringBuilder2(&sb, L"\r\n  Phases\r\n");
        SM_LINE(L"OS loader", b->OsLoaderMs)
        SM_LINE(L"Kernel initialisation", b->KernelInitMs)
        SM_LINE(L"Drivers", b->DriverInitMs)
        SM_LINE(L"Devices (PnP)", b->DevicesInitMs)
        SM_LINE(L"Session manager (smss)", b->SmssInitMs)
        SM_LINE(L"Critical services", b->CriticalServicesInitMs)
        SM_LINE(L"User profile", b->UserProfileMs)
        SM_LINE(L"Explorer / shell", b->ExplorerInitMs)
        SM_LINE(L"Waiting for user logon", b->UserLogonWaitMs)

        if (b->NumStartupApps)
        {
            text = PhFormatString(L"\r\n  Startup apps: %lu%s\r\n", b->NumStartupApps, b->IsDegradation ? L"   (this boot was slower than usual)" : L"");
            PhAppendStringBuilder(&sb, &text->sr);
            PhDereferenceObject(text);
        }
    }
    else
    {
        PhAppendStringBuilder2(&sb, L"No full boot record (event 100) was found, only per-item events.\r\n");
    }

    PhAppendStringBuilder2(&sb, L"\r\nSlowest items (time they took to start, and how much slower than usual):\r\n");

    // selection sort of the top 15 by TotalMs
    {
        BOOLEAN used[512];
        ULONG count = SmBootState.ItemCount < RTL_NUMBER_OF(used) ? SmBootState.ItemCount : RTL_NUMBER_OF(used);

        memset(used, 0, sizeof(used));

        while (shown < 15)
        {
            ULONG best = ULONG_MAX;

            for (i = 0; i < count; i++)
            {
                if (!used[i] && (best == ULONG_MAX || SmBootState.Items[i].TotalMs > SmBootState.Items[best].TotalMs))
                    best = i;
            }

            if (best == ULONG_MAX)
                break;

            used[best] = TRUE;
            {
                const BP_ITEM *it = &SmBootState.Items[best];
                PPH_STRING total = SmBootFormatMs(it->TotalMs);
                PPH_STRING slower = SmBootFormatMs(it->DegradationMs);
                PCWSTR kind = it->EventId == 101 ? L"app" : it->EventId == 103 ? L"service" : it->EventId == 102 ? L"driver" : it->EventId == 106 ? L"device" : L"other";

                text = PhFormatString(L"  %s: %s  (+%s slower than usual, %s)\r\n", it->Name, total->Buffer, slower->Buffer, kind);
                PhAppendStringBuilder(&sb, &text->sr);
                PhDereferenceObject(text);
                PhDereferenceObject(total);
                PhDereferenceObject(slower);
            }

            shown++;
        }

        if (!shown)
            PhAppendStringBuilder2(&sb, L"  (none)\r\n");
    }

done:
    return PhFinalStringBuilderString(&sb);
}

PPH_STRING SmBootGetStatusText(
    VOID
    )
{
    switch (SmBootStatus)
    {
    case SmBootOk:
        return PhFormatString(L"%lu boot events", SmBootState.EventsSeen);
    case SmBootSampleFile:
        return PhCreateString(L"sample data");
    case SmBootAccessDenied:
        return PhCreateString(L"event log needs admin");
    case SmBootNoEvents:
        return PhCreateString(L"no boot events");
    case SmBootUnavailable:
        return PhCreateString(L"event log unavailable");
    }

    return PhCreateString(L"not loaded");
}

// ---- Boot timing window

INT_PTR CALLBACK SmBootTimingDlgProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    switch (WindowMessage)
    {
    case WM_INITDIALOG:
        {
            PPH_STRING summary;

            PhCenterWindow(WindowHandle, GetParent(WindowHandle));
            PhInitializeWindowTheme(WindowHandle, !!PhGetIntegerSetting(L"EnableThemeSupport"));
            SmBootRefresh(TRUE);
            summary = SmBootFormatSummary();
            PhSetDialogItemText(WindowHandle, IDC_BOOT_TEXT, summary->Buffer);
            PhDereferenceObject(summary);
            SetFocus(GetDlgItem(WindowHandle, IDOK));
            SendMessage(GetDlgItem(WindowHandle, IDC_BOOT_TEXT), EM_SETSEL, 0, 0);
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
            EndDialog(WindowHandle, IDOK);
        break;
    case WM_CTLCOLORBTN:
        return HANDLE_WM_CTLCOLORBTN(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLORDLG:
        return HANDLE_WM_CTLCOLORDLG(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLORSTATIC:
        return HANDLE_WM_CTLCOLORSTATIC(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLOREDIT:
        return HANDLE_WM_CTLCOLOREDIT(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    }

    return FALSE;
}

VOID SmShowBootTiming(
    _In_ HWND ParentWindowHandle
    )
{
    PhDialogBox(PluginInstance->DllBase, MAKEINTRESOURCE(IDD_BOOT_TIMING), ParentWindowHandle, SmBootTimingDlgProc, NULL);
}

VOID SmBootShutdown(
    VOID
    )
{
    if (SmBootInitialized)
    {
        BpFree(&SmBootState);
        SmBootInitialized = FALSE;
    }

    if (SmBootScratch)
    {
        PhFree(SmBootScratch);
        SmBootScratch = NULL;
    }
}
