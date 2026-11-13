/*
 * TaskManagerPlus QuickActions: the palette window, the candidate model and the actions.
 *
 * One modeless popup (owned by the main window) with an edit box and a custom-painted result list. Candidates are
 * (verb, target) pairs built when the palette opens: every verb that applies to every process, service and startup
 * item. The text typed is fuzzy-matched against "verb aliases target pid kind" and the best 60 are shown, recent
 * actions first. Enter closes the palette and then runs the verb through the same PhUi* functions the menus use,
 * so the usual confirmations appear.
 */

#include "quickactions.h"
#include <commctrl.h>

#define QA_CLASS L"TaskManagerPlusQuickActionsPalette"
#define QA_ID_EDIT 3001
#define QA_MAX_RESULTS 60
#define QA_VISIBLE_ROWS 9
#define QA_MAX_RECENT 24
#define QA_EMPTY_QUERY_PROCESSES 30

typedef enum _QA_KIND
{
    QaKindProcess,
    QaKindService,
    QaKindStartup,
    QaKindCommand // TaskManagerPlus: commands exported by other plugins (reports, profiles, advisor)
} QA_KIND;

typedef enum _QA_VERB
{
    QaVerbGoTo,
    QaVerbEnd,
    QaVerbEndTree,
    QaVerbSuspend,
    QaVerbResume,
    QaVerbEfficiency,
    QaVerbPriorityIdle,
    QaVerbPriorityBelow,
    QaVerbPriorityNormal,
    QaVerbPriorityAbove,
    QaVerbPriorityHigh,
    QaVerbOpenLocation,
    QaVerbProperties,
    QaVerbServiceStart,
    QaVerbServiceStop,
    QaVerbServiceRestart,
    QaVerbStartupDisable,
    QaVerbStartupEnable,
    QaVerbStartupOpenLocation,
    QaVerbCommand,
    QaVerbMaximum
} QA_VERB;

typedef struct _QA_VERB_INFO
{
    PCWSTR Label;
    PCWSTR Alias; // extra search words, not displayed
    BOOLEAN Destructive;
} QA_VERB_INFO;

static CONST QA_VERB_INFO QaVerbInfo[QaVerbMaximum] =
{
    { L"Go to process", L"select show find jump", FALSE },
    { L"End task", L"kill terminate close stop", TRUE },
    { L"End process tree", L"kill terminate tree children", TRUE },
    { L"Suspend", L"pause freeze", TRUE },
    { L"Resume", L"continue unpause thaw", FALSE },
    { L"Efficiency mode", L"eco power throttle battery", FALSE },
    { L"Priority: Idle", L"priority lowest", FALSE },
    { L"Priority: Below normal", L"priority low", FALSE },
    { L"Priority: Normal", L"priority default", FALSE },
    { L"Priority: Above normal", L"priority raise", FALSE },
    { L"Priority: High", L"priority raise", FALSE },
    { L"Open file location", L"explore folder directory show in explorer", FALSE },
    { L"Properties", L"details info", FALSE },
    { L"Start service", L"run", FALSE },
    { L"Stop service", L"end", TRUE },
    { L"Restart service", L"reload", TRUE },
    { L"Disable startup item", L"turn off remove autorun", FALSE },
    { L"Enable startup item", L"turn on autorun", FALSE },
    { L"Open file location", L"explore folder directory show in explorer", FALSE },
    { L"", L"", FALSE }, // QaVerbCommand: the command's own label is the target text
};

static CONST ULONG QaProcessVerbs[] =
{
    QaVerbGoTo, QaVerbEnd, QaVerbEndTree, QaVerbSuspend, QaVerbResume, QaVerbEfficiency,
    QaVerbPriorityIdle, QaVerbPriorityBelow, QaVerbPriorityNormal, QaVerbPriorityAbove, QaVerbPriorityHigh,
    QaVerbOpenLocation, QaVerbProperties
};

typedef struct _QA_ITEM
{
    QA_KIND Kind;
    QA_VERB Verb;
    PPH_STRING Target; // process name, service display name, startup item name
    PPH_STRING Detail; // right-aligned grey text
    PPH_STRING Match; // searched text
    PPH_STRING Key; // identity for the recent list: verb|kind|target
    ULONG ProcessId;
    LONGLONG CreateTime;
    PPH_STRING ServiceName;
    ULONG StartupIndex;
    ULONG CommandId;
    ULONG InterfaceIndex;
    LONG Score;
    ULONG Order;
    BOOLEAN Recent;
} QA_ITEM, *PQA_ITEM;

typedef struct _QA_COLORS
{
    COLORREF Back;
    COLORREF Text;
    COLORREF Dim;
    COLORREF Border;
    COLORREF Select;
    COLORREF Accent;
    COLORREF Danger;
} QA_COLORS;

static HWND QaWindow = NULL;
static HWND QaEdit = NULL;
static HFONT QaFont = NULL;
static HFONT QaEditFont = NULL;
static HFONT QaSmallFont = NULL;
static HBRUSH QaBackBrush = NULL;
static QA_COLORS QaColors;
static PPH_LIST QaItems = NULL; // PQA_ITEM, every candidate
static PPH_LIST QaResults = NULL; // PQA_ITEM, currently shown (not owned)
static PPH_LIST QaRecent = NULL; // PPH_STRING keys, most recent first
static PPH_LIST QaStartupEntries = NULL; // PSTARTUP_ENTRY_INFO
static PSTARTUP_MANAGER_INTERFACE QaStartupInterface = NULL;
#define QA_COMMAND_PROVIDERS 3
static PTMP_COMMANDS_INTERFACE QaCommandInterfaces[QA_COMMAND_PROVIDERS];
static PPH_LIST QaCommandLists[QA_COMMAND_PROVIDERS]; // PTMP_COMMAND_INFO lists, freed with the interface
static ULONG QaSelected = 0;
static ULONG QaTop = 0;
static LONG QaDpi = 96;
static LONG QaRowHeight = 36;
static LONG QaEditHeight = 48;
static LONG QaFooterHeight = 28;
static LONG QaWidth = 640;
static BOOLEAN QaClosing = FALSE;

static VOID QaUpdateResults(VOID);
static VOID QaRunSelected(VOID);

// ---------------------------------------------------------------- colours

static VOID QaLoadColors(
    VOID
    )
{
    BOOLEAN dark;

    if (PhGetIntegerSetting(L"EnableThemeSupport"))
    {
        const PH_WINDOW_THEME_PALETTE *palette = PhGetWindowThemePalette();

        QaColors.Back = palette->BackgroundColor;
        QaColors.Text = palette->TextColor;
        QaColors.Dim = palette->DisabledTextColor;
        QaColors.Border = palette->BorderColor;
    }
    else
    {
        QaColors.Back = GetSysColor(COLOR_WINDOW);
        QaColors.Text = GetSysColor(COLOR_WINDOWTEXT);
        QaColors.Dim = GetSysColor(COLOR_GRAYTEXT);
        QaColors.Border = GetSysColor(COLOR_3DSHADOW);
    }

    dark = (GetRValue(QaColors.Back) * 299 + GetGValue(QaColors.Back) * 587 + GetBValue(QaColors.Back) * 114) / 1000 < 110;
    QaColors.Accent = dark ? RGB(96, 165, 250) : RGB(37, 99, 235);
    QaColors.Danger = dark ? RGB(248, 113, 113) : RGB(200, 30, 30);
    QaColors.Select = RGB(
        (GetRValue(QaColors.Back) * 80 + GetRValue(QaColors.Accent) * 20) / 100,
        (GetGValue(QaColors.Back) * 80 + GetGValue(QaColors.Accent) * 20) / 100,
        (GetBValue(QaColors.Back) * 80 + GetBValue(QaColors.Accent) * 20) / 100
        );
}

// ---------------------------------------------------------------- model

static VOID QaFreeItem(
    _In_ PQA_ITEM Item
    )
{
    PhClearReference(&Item->Target);
    PhClearReference(&Item->Detail);
    PhClearReference(&Item->Match);
    PhClearReference(&Item->Key);
    PhClearReference(&Item->ServiceName);
    PhFree(Item);
}

static VOID QaClearItems(
    VOID
    )
{
    ULONG i;

    if (QaItems)
    {
        for (i = 0; i < QaItems->Count; i++)
            QaFreeItem(QaItems->Items[i]);

        PhDereferenceObject(QaItems);
        QaItems = NULL;
    }

    if (QaResults)
    {
        PhDereferenceObject(QaResults);
        QaResults = NULL;
    }

    if (QaStartupEntries && QaStartupInterface)
        QaStartupInterface->FreeEntries(QaStartupEntries);

    QaStartupEntries = NULL;
    QaStartupInterface = NULL;

    for (i = 0; i < QA_COMMAND_PROVIDERS; i++)
    {
        if (QaCommandLists[i] && QaCommandInterfaces[i])
            QaCommandInterfaces[i]->FreeCommands(QaCommandLists[i]);

        QaCommandLists[i] = NULL;
        QaCommandInterfaces[i] = NULL;
    }
}

static PQA_ITEM QaAddItem(
    _In_ QA_KIND Kind,
    _In_ QA_VERB Verb,
    _In_ PPH_STRING Target,
    _In_ PPH_STRING Detail, // consumed
    _In_ PCWSTR KindWord
    )
{
    PQA_ITEM item = PhAllocateZero(sizeof(QA_ITEM));

    item->Kind = Kind;
    item->Verb = Verb;
    PhSetReference(&item->Target, Target);
    item->Detail = Detail;
    item->Match = PhFormatString(L"%s %s %s %s", QaVerbInfo[Verb].Label, QaVerbInfo[Verb].Alias, Target->Buffer, KindWord);
    item->Key = PhFormatString(L"%lu|%lu|%s", (ULONG)Verb, (ULONG)Kind, Target->Buffer);
    item->Order = QaItems->Count;
    PhAddItemList(QaItems, item);
    return item;
}

static VOID QaLoadRecent(
    VOID
    )
{
    PPH_STRING setting = PhGetStringSetting(SETTING_NAME_RECENT);
    PH_STRINGREF remaining = setting->sr;
    PH_STRINGREF part;

    if (!QaRecent)
        QaRecent = PhCreateList(QA_MAX_RECENT);

    while (QaRecent->Count)
    {
        PhDereferenceObject(QaRecent->Items[QaRecent->Count - 1]);
        PhRemoveItemList(QaRecent, QaRecent->Count - 1);
    }

    while (remaining.Length && QaRecent->Count < QA_MAX_RECENT)
    {
        if (!PhSplitStringRefAtChar(&remaining, L'\n', &part, &remaining))
            part = remaining, remaining.Length = 0;

        if (part.Length)
            PhAddItemList(QaRecent, PhCreateString2(&part));
    }

    PhDereferenceObject(setting);
}

static VOID QaRememberRecent(
    _In_ PPH_STRING Key
    )
{
    PH_STRING_BUILDER sb;
    PPH_STRING text;
    ULONG i;

    if (!QaRecent)
        QaRecent = PhCreateList(QA_MAX_RECENT);

    for (i = 0; i < QaRecent->Count; i++)
    {
        if (PhEqualString(QaRecent->Items[i], Key, FALSE))
        {
            PhDereferenceObject(QaRecent->Items[i]);
            PhRemoveItemList(QaRecent, i);
            break;
        }
    }

    PhInsertItemList(QaRecent, 0, PhReferenceObject(Key));

    while (QaRecent->Count > QA_MAX_RECENT)
    {
        PhDereferenceObject(QaRecent->Items[QaRecent->Count - 1]);
        PhRemoveItemList(QaRecent, QaRecent->Count - 1);
    }

    PhInitializeStringBuilder(&sb, 256);

    for (i = 0; i < QaRecent->Count; i++)
    {
        if (i)
            PhAppendCharStringBuilder(&sb, L'\n');

        PhAppendStringBuilder(&sb, &((PPH_STRING)QaRecent->Items[i])->sr);
    }

    text = PhFinalStringBuilderString(&sb);
    PhSetStringSetting2(SETTING_NAME_RECENT, &text->sr);
    PhDereferenceObject(text);
}

static LONG QaRecentRank(
    _In_ PPH_STRING Key
    )
{
    ULONG i;

    if (!QaRecent)
        return -1;

    for (i = 0; i < QaRecent->Count; i++)
    {
        if (PhEqualString(QaRecent->Items[i], Key, FALSE))
            return (LONG)i;
    }

    return -1;
}

static int __cdecl QaProcessOrderCompare(
    _In_ const void *A,
    _In_ const void *B
    )
{
    PPH_PROCESS_ITEM a = *(PPH_PROCESS_ITEM *)A;
    PPH_PROCESS_ITEM b = *(PPH_PROCESS_ITEM *)B;

    if (a->CpuUsage != b->CpuUsage)
        return a->CpuUsage < b->CpuUsage ? 1 : -1;

    return PhCompareStringWithNull(a->ProcessName, b->ProcessName, TRUE);
}

static VOID QaBuildProcesses(
    VOID
    )
{
    PPH_PROCESS_ITEM *processes;
    ULONG count;
    ULONG i;
    ULONG j;

    PhEnumProcessItems(&processes, &count);
    qsort(processes, count, sizeof(PVOID), QaProcessOrderCompare);

    for (i = 0; i < count; i++)
    {
        PPH_PROCESS_ITEM process = processes[i];
        PPH_STRING path = NULL;
        BOOLEAN canAct;
        PPH_STRING name;

        if (PH_IS_FAKE_PROCESS_ID(process->ProcessId) || process->ProcessId == SYSTEM_IDLE_PROCESS_ID)
            continue;

        name = process->ProcessName;

        if (!name)
            continue;

        // Never act on System, protected or secure processes, or on System Informer itself.
        canAct = process->ProcessId != SYSTEM_PROCESS_ID && !process->IsProtectedProcess && !process->IsSecureProcess &&
            process->ProcessId != NtCurrentProcessId();

        if (process->FileName)
            path = PhGetFileName(process->FileName);

        for (j = 0; j < RTL_NUMBER_OF(QaProcessVerbs); j++)
        {
            QA_VERB verb = QaProcessVerbs[j];
            PQA_ITEM item;

            if (!canAct && verb != QaVerbGoTo && verb != QaVerbOpenLocation && verb != QaVerbProperties)
                continue;

            if (verb == QaVerbOpenLocation && !path)
                continue;

            if (verb == QaVerbResume && !process->IsSuspended && canAct)
                continue; // nothing to resume

            item = QaAddItem(
                QaKindProcess,
                verb,
                name,
                PhFormatString(L"PID %lu%s%s", HandleToUlong(process->ProcessId), path ? L"   " : L"", path ? path->Buffer : L""),
                L"process"
                );

            // pid is searchable too
            PhMoveReference(&item->Match, PhFormatString(L"%s %lu", item->Match->Buffer, HandleToUlong(process->ProcessId)));
            item->ProcessId = HandleToUlong(process->ProcessId);
            item->CreateTime = process->CreateTime.QuadPart;
        }

        PhClearReference(&path);
    }

    PhDereferenceObjects(processes, count);
    PhFree(processes);
}

static VOID QaBuildServices(
    VOID
    )
{
    LPENUM_SERVICE_STATUS_PROCESS services;
    ULONG count;
    ULONG i;

    if (!NT_SUCCESS(PhEnumServices(&services, &count)))
        return;

    for (i = 0; i < count; i++)
    {
        LPENUM_SERVICE_STATUS_PROCESS service = &services[i];
        BOOLEAN running = service->ServiceStatusProcess.dwCurrentState == SERVICE_RUNNING;
        BOOLEAN stopped = service->ServiceStatusProcess.dwCurrentState == SERVICE_STOPPED;
        PPH_STRING target = PhCreateString(service->lpDisplayName && service->lpDisplayName[0] ? service->lpDisplayName : service->lpServiceName);
        QA_VERB verbs[3];
        ULONG verbCount = 0;
        ULONG j;

        if (stopped)
        {
            verbs[verbCount++] = QaVerbServiceStart;
        }
        else if (running)
        {
            verbs[verbCount++] = QaVerbServiceStop;
            verbs[verbCount++] = QaVerbServiceRestart;
        }

        for (j = 0; j < verbCount; j++)
        {
            PQA_ITEM item = QaAddItem(
                QaKindService,
                verbs[j],
                target,
                PhFormatString(L"%s   %s", service->lpServiceName, running ? L"Running" : L"Stopped"),
                L"service"
                );

            PhMoveReference(&item->Match, PhFormatString(L"%s %s", item->Match->Buffer, service->lpServiceName));
            item->ServiceName = PhCreateString(service->lpServiceName);
        }

        PhDereferenceObject(target);
    }

    PhFree(services);
}

static VOID QaBuildStartup(
    VOID
    )
{
    ULONG i;

    QaStartupInterface = PhGetPluginInterfaceZ(STARTUP_MANAGER_PLUGIN_NAME, STARTUP_MANAGER_INTERFACE_VERSION);

    if (!QaStartupInterface)
        return;

    QaStartupEntries = QaStartupInterface->Enumerate();

    for (i = 0; i < QaStartupEntries->Count; i++)
    {
        PSTARTUP_ENTRY_INFO entry = QaStartupEntries->Items[i];
        PPH_STRING detail = PhFormatString(L"%s   %s", PhGetStringOrEmpty(entry->Type), entry->Enabled ? L"Enabled" : L"Disabled");
        PQA_ITEM item;

        if (!entry->Name)
        {
            PhDereferenceObject(detail);
            continue;
        }

        if (entry->CanToggle)
        {
            item = QaAddItem(QaKindStartup, entry->Enabled ? QaVerbStartupDisable : QaVerbStartupEnable, entry->Name, PhReferenceObject(detail), L"startup item autorun");
            item->StartupIndex = i;
        }

        if (entry->FileName)
        {
            item = QaAddItem(QaKindStartup, QaVerbStartupOpenLocation, entry->Name, PhReferenceObject(detail), L"startup item autorun");
            item->StartupIndex = i;
        }

        PhDereferenceObject(detail);
    }
}

static VOID QaBuildCommands(
    VOID
    )
{
    static PCWSTR names[QA_COMMAND_PROVIDERS] =
    {
        TMP_COMMANDS_REPORTS_PLUGIN_NAME, TMP_COMMANDS_PROFILES_PLUGIN_NAME, TMP_COMMANDS_ADVISOR_PLUGIN_NAME
    };
    ULONG p;
    ULONG i;

    for (p = 0; p < QA_COMMAND_PROVIDERS; p++)
    {
        QaCommandInterfaces[p] = PhGetPluginInterfaceZ(names[p], TMP_COMMANDS_INTERFACE_VERSION);

        if (!QaCommandInterfaces[p])
            continue;

        QaCommandLists[p] = QaCommandInterfaces[p]->Enumerate();

        for (i = 0; i < QaCommandLists[p]->Count; i++)
        {
            PTMP_COMMAND_INFO command = QaCommandLists[p]->Items[i];
            PQA_ITEM item;

            if (!command->Label)
                continue;

            item = QaAddItem(QaKindCommand, QaVerbCommand, command->Label, command->Detail ? PhReferenceObject(command->Detail) : PhReferenceEmptyString(), L"command");
            item->CommandId = command->Id;
            item->InterfaceIndex = p;

            if (command->Alias)
            {
                PhMoveReference(&item->Match, PhFormatString(L"%s %s command", command->Label->Buffer, command->Alias));
            }
        }
    }
}

static VOID QaBuildCandidates(
    VOID
    )
{
    QaClearItems();
    QaItems = PhCreateList(4096);
    QaResults = PhCreateList(QA_MAX_RESULTS + 8);
    QaLoadRecent();
    QaBuildProcesses();
    QaBuildServices();
    QaBuildStartup();
    QaBuildCommands();
}

static int __cdecl QaResultCompare(
    _In_ const void *A,
    _In_ const void *B
    )
{
    PQA_ITEM a = *(PQA_ITEM *)A;
    PQA_ITEM b = *(PQA_ITEM *)B;

    if (a->Score != b->Score)
        return a->Score < b->Score ? 1 : -1;

    if (a->Order != b->Order)
        return a->Order < b->Order ? -1 : 1;

    return 0;
}

static VOID QaUpdateResults(
    VOID
    )
{
    PPH_STRING query;
    ULONG i;
    PPH_LIST matches;
    ULONG goToShown = 0;

    if (!QaItems)
        return;

    query = PhGetWindowText(QaEdit);
    PhClearList(QaResults);
    matches = PhCreateList(QaItems->Count + 16);

    if (query && query->Length == 0)
    {
        // Empty query: the recent actions that still apply, then "Go to process" for the busiest processes.
        for (i = 0; i < QaItems->Count; i++)
        {
            PQA_ITEM item = QaItems->Items[i];
            LONG rank = QaRecentRank(item->Key);

            item->Recent = rank >= 0;

            if (rank >= 0)
            {
                item->Score = 100000 - rank;
                PhAddItemList(matches, item);
            }
            else if (item->Kind == QaKindProcess && item->Verb == QaVerbGoTo && goToShown < QA_EMPTY_QUERY_PROCESSES)
            {
                item->Score = 0;
                goToShown++;
                PhAddItemList(matches, item);
            }
        }
    }
    else
    {
        for (i = 0; i < QaItems->Count; i++)
        {
            PQA_ITEM item = QaItems->Items[i];
            LONG score = QaFuzzyScore(PhGetStringOrEmpty(query), item->Match->Buffer);
            LONG rank;

            item->Recent = FALSE;

            if (score == QA_NO_MATCH)
                continue;

            rank = QaRecentRank(item->Key);

            if (rank >= 0)
            {
                score += 400 - rank * 10;
                item->Recent = TRUE;
            }

            if (item->Verb == QaVerbGoTo)
                score += 5;

            item->Score = score;
            PhAddItemList(matches, item);
        }
    }

    if (matches->Count > 1)
        qsort(matches->Items, matches->Count, sizeof(PVOID), QaResultCompare);

    for (i = 0; i < matches->Count && i < QA_MAX_RESULTS; i++)
        PhAddItemList(QaResults, matches->Items[i]);

    PhDereferenceObject(matches);
    PhClearReference(&query);

    QaSelected = 0;
    QaTop = 0;
}

// ---------------------------------------------------------------- geometry and painting

static VOID QaApplySize(
    VOID
    )
{
    ULONG rows = QaResults ? QaResults->Count : 0;
    RECT ownerRect;
    RECT windowRect;
    LONG height;
    LONG x;
    LONG y;
    HRGN region;

    rows = max(1, min(rows, QA_VISIBLE_ROWS));
    height = QaEditHeight + (LONG)rows * QaRowHeight + QaFooterHeight;

    GetWindowRect(SystemInformer_GetWindowHandle(), &ownerRect);
    GetWindowRect(QaWindow, &windowRect);

    if (windowRect.right - windowRect.left == QaWidth && windowRect.bottom - windowRect.top == height)
        return;

    x = ownerRect.left + ((ownerRect.right - ownerRect.left) - QaWidth) / 2;
    y = ownerRect.top + (ownerRect.bottom - ownerRect.top) / 7;

    SetWindowPos(QaWindow, NULL, x, y, QaWidth, height, SWP_NOZORDER | SWP_NOACTIVATE);
    region = CreateRoundRectRgn(0, 0, QaWidth + 1, height + 1, PhScaleToDisplay(14, QaDpi), PhScaleToDisplay(14, QaDpi));
    SetWindowRgn(QaWindow, region, TRUE);
}

static VOID QaEnsureVisible(
    VOID
    )
{
    if (QaSelected < QaTop)
        QaTop = QaSelected;
    else if (QaSelected >= QaTop + QA_VISIBLE_ROWS)
        QaTop = QaSelected - QA_VISIBLE_ROWS + 1;
}

static PCWSTR QaKindText(
    _In_ PQA_ITEM Item
    )
{
    if (Item->Recent)
        return L"Recent";

    switch (Item->Kind)
    {
    case QaKindProcess:
        return L"Process";
    case QaKindService:
        return L"Service";
    case QaKindCommand:
        return L"Command";
    default:
        return L"Startup";
    }
}

static VOID QaPaint(
    _In_ HDC Dc,
    _In_ PRECT Client
    )
{
    HBRUSH brush;
    HPEN pen;
    HPEN oldPen;
    HFONT oldFont;
    ULONG i;
    LONG pad = PhScaleToDisplay(14, QaDpi);
    LONG badgeWidth = PhScaleToDisplay(70, QaDpi);
    RECT rect;
    ULONG rows = QaResults ? QaResults->Count : 0;

    brush = CreateSolidBrush(QaColors.Back);
    FillRect(Dc, Client, brush);
    DeleteObject(brush);

    SetBkMode(Dc, TRANSPARENT);
    oldFont = SelectObject(Dc, QaFont);

    // separator under the edit box
    pen = CreatePen(PS_SOLID, 1, QaColors.Border);
    oldPen = SelectObject(Dc, pen);
    MoveToEx(Dc, 0, QaEditHeight - 1, NULL);
    LineTo(Dc, Client->right, QaEditHeight - 1);

    if (rows == 0)
    {
        rect.left = pad;
        rect.right = Client->right - pad;
        rect.top = QaEditHeight;
        rect.bottom = QaEditHeight + QaRowHeight;
        SetTextColor(Dc, QaColors.Dim);
        DrawText(Dc, L"No matches", -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    for (i = 0; i < QA_VISIBLE_ROWS && QaTop + i < rows; i++)
    {
        PQA_ITEM item = QaResults->Items[QaTop + i];
        BOOLEAN selected = QaTop + i == QaSelected;
        RECT row;
        RECT text;
        SIZE verbSize;
        PCWSTR verb = QaVerbInfo[item->Verb].Label;

        row.left = PhScaleToDisplay(6, QaDpi);
        row.right = Client->right - PhScaleToDisplay(6, QaDpi);
        row.top = QaEditHeight + (LONG)i * QaRowHeight + PhScaleToDisplay(2, QaDpi);
        row.bottom = row.top + QaRowHeight - PhScaleToDisplay(4, QaDpi);

        if (selected)
        {
            HBRUSH selectBrush = CreateSolidBrush(QaColors.Select);
            HBRUSH accentBrush = CreateSolidBrush(QaColors.Accent);
            HRGN rowRegion = CreateRoundRectRgn(row.left, row.top, row.right + 1, row.bottom + 1, PhScaleToDisplay(8, QaDpi), PhScaleToDisplay(8, QaDpi));
            RECT bar = row;

            FillRgn(Dc, rowRegion, selectBrush);
            DeleteObject(rowRegion);
            bar.left = row.left + PhScaleToDisplay(2, QaDpi);
            bar.right = bar.left + PhScaleToDisplay(3, QaDpi);
            bar.top = row.top + PhScaleToDisplay(7, QaDpi);
            bar.bottom = row.bottom - PhScaleToDisplay(7, QaDpi);
            FillRect(Dc, &bar, accentBrush);
            DeleteObject(selectBrush);
            DeleteObject(accentBrush);
        }

        // kind badge
        text = row;
        text.left = row.left + pad;
        text.right = text.left + badgeWidth;
        SelectObject(Dc, QaSmallFont);
        SetTextColor(Dc, item->Recent ? QaColors.Accent : QaColors.Dim);
        DrawText(Dc, QaKindText(item), -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        // "Verb  target"
        SelectObject(Dc, QaFont);
        text.left = row.left + pad + badgeWidth;
        text.right = row.right - pad;
        GetTextExtentPoint32(Dc, verb, (int)wcslen(verb), &verbSize);
        SetTextColor(Dc, QaVerbInfo[item->Verb].Destructive ? QaColors.Danger : QaColors.Text);
        DrawText(Dc, verb, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        text.left += verbSize.cx + PhScaleToDisplay(10, QaDpi);
        SetTextColor(Dc, QaColors.Text);

        // right-aligned detail, clipped so it never covers the target
        {
            RECT detail = text;
            LONG available = text.right - text.left;

            detail.left = text.left + available * 55 / 100;
            SelectObject(Dc, QaSmallFont);
            SetTextColor(Dc, QaColors.Dim);
            DrawText(Dc, PhGetStringOrEmpty(item->Detail), -1, &detail, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(Dc, QaFont);
            SetTextColor(Dc, QaColors.Text);
            text.right = detail.left - PhScaleToDisplay(8, QaDpi);
        }

        DrawText(Dc, PhGetStringOrEmpty(item->Target), -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    // footer
    rect.left = pad;
    rect.right = Client->right - pad;
    rect.top = Client->bottom - QaFooterHeight;
    rect.bottom = Client->bottom;
    MoveToEx(Dc, 0, rect.top, NULL);
    LineTo(Dc, Client->right, rect.top);
    SelectObject(Dc, QaSmallFont);
    SetTextColor(Dc, QaColors.Dim);
    DrawText(Dc, L"Enter  run     \x2191\x2193  select     Esc  close", -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    {
        PPH_STRING count = PhFormatString(L"%lu result%s", rows, rows == 1 ? L"" : L"s");

        DrawText(Dc, count->Buffer, -1, &rect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        PhDereferenceObject(count);
    }

    SelectObject(Dc, oldPen);
    DeleteObject(pen);
    SelectObject(Dc, oldFont);
}

static LONG QaRowFromPoint(
    _In_ LONG X,
    _In_ LONG Y
    )
{
    LONG row;

    if (Y < QaEditHeight)
        return -1;

    row = (Y - QaEditHeight) / QaRowHeight;

    if (row < 0 || row >= QA_VISIBLE_ROWS || !QaResults || QaTop + (ULONG)row >= QaResults->Count)
        return -1;

    return (LONG)(QaTop + (ULONG)row);
}

static VOID QaMoveSelection(
    _In_ LONG Delta,
    _In_ BOOLEAN Wrap
    )
{
    LONG count = QaResults ? (LONG)QaResults->Count : 0;
    LONG selected = (LONG)QaSelected + Delta;

    if (!count)
        return;

    if (Wrap)
        selected = (selected % count + count) % count;
    else
        selected = max(0, min(selected, count - 1));

    QaSelected = (ULONG)selected;
    QaEnsureVisible();
    InvalidateRect(QaWindow, NULL, FALSE);
}

static VOID QaRefresh(
    VOID
    )
{
    QaUpdateResults();
    QaApplySize();
    InvalidateRect(QaWindow, NULL, FALSE);
}

// ---------------------------------------------------------------- actions

typedef struct _QA_ACTION
{
    QA_KIND Kind;
    QA_VERB Verb;
    ULONG ProcessId;
    LONGLONG CreateTime;
    PPH_STRING Target;
    PPH_STRING ServiceName;
    STARTUP_ENTRY_INFO Startup;
    PSTARTUP_MANAGER_INTERFACE StartupInterface;
    PTMP_COMMANDS_INTERFACE CommandInterface;
    ULONG CommandId;
} QA_ACTION, *PQA_ACTION;

static PPH_PROCESS_ITEM QaReferenceProcess(
    _In_ PQA_ACTION Action
    )
{
    PPH_PROCESS_ITEM process = PhReferenceProcessItem(UlongToHandle(Action->ProcessId));

    if (process && process->CreateTime.QuadPart != Action->CreateTime)
    {
        PhDereferenceObject(process);
        process = NULL;
    }

    if (!process)
    {
        PhShowInformation2(SystemInformer_GetWindowHandle(), L"Quick Actions", L"%s (PID %lu) is no longer running.", PhGetStringOrDefault(Action->Target, L"The process"), Action->ProcessId);
    }

    return process;
}

static VOID QaRunProcessAction(
    _In_ PQA_ACTION Action
    )
{
    PPH_PROCESS_ITEM process = QaReferenceProcess(Action);
    ULONG priority = 0;

    if (!process)
        return;

    switch (Action->Verb)
    {
    case QaVerbGoTo:
        {
            PPH_PROCESS_NODE node = PhFindProcessNode(process->ProcessId);

            if (node)
            {
                SystemInformer_SelectTabPage(0);
                SystemInformer_SelectProcessNode(node);
            }
        }
        break;
    case QaVerbEnd:
        PhUiTerminateProcesses(SystemInformer_GetWindowHandle(), &process, 1);
        break;
    case QaVerbEndTree:
        PhUiTerminateTreeProcess(SystemInformer_GetWindowHandle(), process);
        break;
    case QaVerbSuspend:
        PhUiSuspendProcesses(SystemInformer_GetWindowHandle(), &process, 1);
        break;
    case QaVerbResume:
        PhUiResumeProcesses(SystemInformer_GetWindowHandle(), &process, 1);
        break;
    case QaVerbEfficiency:
        PhUiSetEcoModeProcess(SystemInformer_GetWindowHandle(), process);
        break;
    case QaVerbPriorityIdle:
        priority = PROCESS_PRIORITY_CLASS_IDLE;
        break;
    case QaVerbPriorityBelow:
        priority = PROCESS_PRIORITY_CLASS_BELOW_NORMAL;
        break;
    case QaVerbPriorityNormal:
        priority = PROCESS_PRIORITY_CLASS_NORMAL;
        break;
    case QaVerbPriorityAbove:
        priority = PROCESS_PRIORITY_CLASS_ABOVE_NORMAL;
        break;
    case QaVerbPriorityHigh:
        priority = PROCESS_PRIORITY_CLASS_HIGH;
        break;
    case QaVerbOpenLocation:
        {
            PPH_STRING fileName = process->FileName ? PhGetFileName(process->FileName) : NULL;

            if (fileName)
                PhShellExploreFile(SystemInformer_GetWindowHandle(), fileName->Buffer);

            PhClearReference(&fileName);
        }
        break;
    case QaVerbProperties:
        {
            PPH_PROCESS_PROPCONTEXT context = PhCreateProcessPropContext(SystemInformer_GetWindowHandle(), process);

            if (context)
            {
                PhShowProcessProperties(context);
                PhDereferenceObject(context);
            }
        }
        break;
    }

    if (priority)
        PhUiSetPriorityClassProcesses(SystemInformer_GetWindowHandle(), &process, 1, priority);

    PhDereferenceObject(process);
}

static VOID QaRunServiceAction(
    _In_ PQA_ACTION Action
    )
{
    PPH_SERVICE_ITEM service = PhReferenceServiceItem(&Action->ServiceName->sr);

    if (!service)
    {
        PhShowInformation2(SystemInformer_GetWindowHandle(), L"Quick Actions", L"The service %s no longer exists.", PhGetStringOrEmpty(Action->ServiceName));
        return;
    }

    switch (Action->Verb)
    {
    case QaVerbServiceStart:
        PhUiStartService(SystemInformer_GetWindowHandle(), service);
        break;
    case QaVerbServiceStop:
        PhUiStopService(SystemInformer_GetWindowHandle(), service);
        break;
    case QaVerbServiceRestart:
        PhUiRestartServices(SystemInformer_GetWindowHandle(), &service, 1);
        break;
    }

    PhDereferenceObject(service);
}

static VOID QaRunStartupAction(
    _In_ PQA_ACTION Action
    )
{
    switch (Action->Verb)
    {
    case QaVerbStartupDisable:
    case QaVerbStartupEnable:
        {
            NTSTATUS status = Action->StartupInterface->SetEnabled(&Action->Startup, Action->Verb == QaVerbStartupEnable);

            if (!NT_SUCCESS(status))
                PhShowStatus(SystemInformer_GetWindowHandle(), L"Unable to change the startup item.", status, 0);
        }
        break;
    case QaVerbStartupOpenLocation:
        if (Action->Startup.FileName)
            PhShellExploreFile(SystemInformer_GetWindowHandle(), Action->Startup.FileName->Buffer);
        break;
    }
}

static VOID QaRunAction(
    _In_ PQA_ACTION Action
    )
{
    switch (Action->Kind)
    {
    case QaKindProcess:
        QaRunProcessAction(Action);
        break;
    case QaKindService:
        QaRunServiceAction(Action);
        break;
    case QaKindStartup:
        QaRunStartupAction(Action);
        break;
    case QaKindCommand:
        if (Action->CommandInterface)
            Action->CommandInterface->Run(Action->CommandId, SystemInformer_GetWindowHandle());
        break;
    }
}

static VOID QaRunSelected(
    VOID
    )
{
    PQA_ITEM item;
    QA_ACTION action;

    if (!QaResults || QaSelected >= QaResults->Count)
        return;

    item = QaResults->Items[QaSelected];
    memset(&action, 0, sizeof(action));
    action.Kind = item->Kind;
    action.Verb = item->Verb;
    action.ProcessId = item->ProcessId;
    action.CreateTime = item->CreateTime;
    PhSetReference(&action.Target, item->Target);
    PhSetReference(&action.ServiceName, item->ServiceName);

    if (item->Kind == QaKindCommand && item->InterfaceIndex < QA_COMMAND_PROVIDERS)
    {
        action.CommandInterface = QaCommandInterfaces[item->InterfaceIndex];
        action.CommandId = item->CommandId;
    }

    if (item->Kind == QaKindStartup && QaStartupEntries && item->StartupIndex < QaStartupEntries->Count)
    {
        PSTARTUP_ENTRY_INFO entry = QaStartupEntries->Items[item->StartupIndex];

        action.StartupInterface = QaStartupInterface;
        action.Startup.Source = entry->Source;
        action.Startup.Enabled = entry->Enabled;
        action.Startup.CanToggle = entry->CanToggle;
        PhSetReference(&action.Startup.Name, entry->Name);
        PhSetReference(&action.Startup.FileName, entry->FileName);
        PhSetReference(&action.Startup.ObjectPath, entry->ObjectPath);
        PhSetReference(&action.Startup.ApprovedValueName, entry->ApprovedValueName);
    }

    QaRememberRecent(item->Key);

    // Close first: the palette would otherwise lose activation to the confirmation dialog and its items
    // (which the action does not need any more) are freed with it.
    QaClosePalette();
    QaRunAction(&action);

    PhClearReference(&action.Target);
    PhClearReference(&action.ServiceName);
    PhClearReference(&action.Startup.Name);
    PhClearReference(&action.Startup.FileName);
    PhClearReference(&action.Startup.ObjectPath);
    PhClearReference(&action.Startup.ApprovedValueName);
}

// ---------------------------------------------------------------- window

static LRESULT CALLBACK QaEditSubclassProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam,
    _In_ UINT_PTR SubclassId,
    _In_ DWORD_PTR RefData
    )
{
    switch (WindowMessage)
    {
    case WM_KEYDOWN:
        switch (wParam)
        {
        case VK_DOWN:
            QaMoveSelection(1, TRUE);
            return 0;
        case VK_UP:
            QaMoveSelection(-1, TRUE);
            return 0;
        case VK_NEXT:
            QaMoveSelection(QA_VISIBLE_ROWS, FALSE);
            return 0;
        case VK_PRIOR:
            QaMoveSelection(-QA_VISIBLE_ROWS, FALSE);
            return 0;
        case VK_TAB:
            QaMoveSelection(GetKeyState(VK_SHIFT) < 0 ? -1 : 1, TRUE);
            return 0;
        case VK_RETURN:
            QaRunSelected();
            return 0;
        case VK_ESCAPE:
            QaClosePalette();
            return 0;
        }
        break;
    case WM_CHAR:
        if (wParam == VK_RETURN || wParam == VK_ESCAPE || wParam == VK_TAB)
            return 0; // no beep
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(WindowHandle, QaEditSubclassProc, SubclassId);
        break;
    }

    return DefSubclassProc(WindowHandle, WindowMessage, wParam, lParam);
}

static HFONT QaCreateFont(
    _In_ LONG Dpi,
    _In_ LONG Percent
    )
{
    HFONT base = PhCreateApplicationFont(Dpi);
    LOGFONT logFont;
    HFONT result;

    if (!base)
        return NULL;

    GetObject(base, sizeof(logFont), &logFont);
    DeleteObject(base);
    logFont.lfHeight = logFont.lfHeight * Percent / 100;
    logFont.lfQuality = CLEARTYPE_QUALITY;
    result = CreateFontIndirect(&logFont);
    return result;
}

static LRESULT CALLBACK QaWndProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    switch (WindowMessage)
    {
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(WindowHandle, &ps);
            RECT client;
            HDC memoryDc;
            HBITMAP bitmap;
            HBITMAP oldBitmap;
            HRGN frame;
            HBRUSH frameBrush;

            GetClientRect(WindowHandle, &client);
            memoryDc = CreateCompatibleDC(dc);
            bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
            oldBitmap = SelectObject(memoryDc, bitmap);
            QaPaint(memoryDc, &client);

            // rounded outline
            frame = CreateRoundRectRgn(0, 0, client.right + 1, client.bottom + 1, PhScaleToDisplay(14, QaDpi), PhScaleToDisplay(14, QaDpi));
            frameBrush = CreateSolidBrush(QaColors.Border);
            FrameRgn(memoryDc, frame, frameBrush, 1, 1);
            DeleteObject(frameBrush);
            DeleteObject(frame);

            BitBlt(dc, 0, 0, client.right, client.bottom, memoryDc, 0, 0, SRCCOPY);
            SelectObject(memoryDc, oldBitmap);
            DeleteObject(bitmap);
            DeleteDC(memoryDc);
            EndPaint(WindowHandle, &ps);
        }
        return 0;
    case WM_CTLCOLOREDIT:
        {
            SetTextColor((HDC)wParam, QaColors.Text);
            SetBkColor((HDC)wParam, QaColors.Back);
            return (LRESULT)QaBackBrush;
        }
    case WM_COMMAND:
        if (GET_WM_COMMAND_ID(wParam, lParam) == QA_ID_EDIT && GET_WM_COMMAND_CMD(wParam, lParam) == EN_CHANGE)
            QaRefresh();
        return 0;
    case WM_MOUSEMOVE:
        {
            LONG row = QaRowFromPoint(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));

            if (row >= 0 && (ULONG)row != QaSelected)
            {
                QaSelected = (ULONG)row;
                InvalidateRect(WindowHandle, NULL, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        {
            LONG row = QaRowFromPoint(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));

            if (row >= 0)
            {
                QaSelected = (ULONG)row;
                QaRunSelected();
            }
        }
        return 0;
    case WM_MOUSEWHEEL:
        QaMoveSelection(GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -3 : 3, FALSE);
        return 0;
    case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && !QaClosing)
            PostMessage(WindowHandle, WM_CLOSE, 0, 0);
        return 0;
    case WM_SETFOCUS:
        SetFocus(QaEdit);
        return 0;
    case WM_CLOSE:
        QaClosePalette();
        return 0;
    case WM_NCDESTROY:
        QaWindow = NULL;
        QaEdit = NULL;
        break;
    }

    return DefWindowProc(WindowHandle, WindowMessage, wParam, lParam);
}

static BOOLEAN QaRegisterClass(
    VOID
    )
{
    static PH_INITONCE initOnce = PH_INITONCE_INIT;
    static BOOLEAN ok = FALSE;

    if (PhBeginInitOnce(&initOnce))
    {
        WNDCLASSEX wcex = { sizeof(WNDCLASSEX) };

        wcex.style = CS_DROPSHADOW;
        wcex.lpfnWndProc = QaWndProc;
        wcex.hInstance = PluginInstance->DllBase;
        wcex.hCursor = PhLoadCursor(NULL, IDC_ARROW);
        wcex.lpszClassName = QA_CLASS;
        ok = RegisterClassEx(&wcex) != 0;
        PhEndInitOnce(&initOnce);
    }

    return ok;
}

BOOLEAN QaIsPaletteWindow(
    _In_ HWND WindowHandle
    )
{
    return QaWindow && WindowHandle == QaWindow;
}

BOOLEAN QaIsPaletteOpen(
    VOID
    )
{
    return QaWindow != NULL;
}

VOID QaOpenPalette(
    VOID
    )
{
    LONG editPad;
    LONG editTop;
    RECT ownerRect;


    if (QaWindow)
    {
        SetForegroundWindow(QaWindow);
        SetFocus(QaEdit);
        return;
    }

    if (!QaRegisterClass())
        return;

    QaClosing = FALSE;
    QaDpi = PhGetWindowDpi(SystemInformer_GetWindowHandle());
    QaRowHeight = PhScaleToDisplay(36, QaDpi);
    QaEditHeight = PhScaleToDisplay(52, QaDpi);
    QaFooterHeight = PhScaleToDisplay(30, QaDpi);
    GetWindowRect(SystemInformer_GetWindowHandle(), &ownerRect);
    QaWidth = min(PhScaleToDisplay(680, QaDpi), max((LONG)(ownerRect.right - ownerRect.left) - PhScaleToDisplay(40, QaDpi), PhScaleToDisplay(360, QaDpi)));

    QaLoadColors();
    QaBackBrush = CreateSolidBrush(QaColors.Back);
    QaFont = QaCreateFont(QaDpi, 110);
    QaEditFont = QaCreateFont(QaDpi, 135);
    QaSmallFont = QaCreateFont(QaDpi, 95);

    QaBuildCandidates();

    QaWindow = CreateWindowEx(
        WS_EX_TOOLWINDOW,
        QA_CLASS,
        L"Quick Actions",
        WS_POPUP | WS_CLIPCHILDREN,
        ownerRect.left, ownerRect.top, QaWidth, QaEditHeight + QaRowHeight + QaFooterHeight,
        SystemInformer_GetWindowHandle(),
        NULL,
        PluginInstance->DllBase,
        NULL
        );

    if (!QaWindow)
    {
        QaClearItems();
        return;
    }

    editPad = PhScaleToDisplay(16, QaDpi);
    editTop = PhScaleToDisplay(10, QaDpi);
    QaEdit = CreateWindowEx(
        0,
        WC_EDIT,
        L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        editPad, editTop, QaWidth - 2 * editPad, QaEditHeight - 2 * editTop,
        QaWindow,
        (HMENU)(ULONG_PTR)QA_ID_EDIT,
        PluginInstance->DllBase,
        NULL
        );

    SendMessage(QaEdit, WM_SETFONT, (WPARAM)QaEditFont, TRUE);
    SendMessage(QaEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search processes, services and startup items, or type an action...");
    SetWindowSubclass(QaEdit, QaEditSubclassProc, 1, 0);

    QaUpdateResults();
    QaApplySize();
    ShowWindow(QaWindow, SW_SHOW);
    SetForegroundWindow(QaWindow);
    SetFocus(QaEdit);
}

VOID QaClosePalette(
    VOID
    )
{
    HWND window = QaWindow;

    if (!window || QaClosing)
        return;

    QaClosing = TRUE;
    DestroyWindow(window);
    QaWindow = NULL;
    QaEdit = NULL;
    QaClearItems();

    if (QaFont)
        DeleteObject(QaFont);

    if (QaEditFont)
        DeleteObject(QaEditFont);

    if (QaSmallFont)
        DeleteObject(QaSmallFont);

    if (QaBackBrush)
        DeleteObject(QaBackBrush);

    QaFont = QaEditFont = QaSmallFont = NULL;
    QaBackBrush = NULL;

    // give focus back to the main window so keyboard use continues there
    if (SystemInformer_GetWindowHandle())
        SetForegroundWindow(SystemInformer_GetWindowHandle());
}
