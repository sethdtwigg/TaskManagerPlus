/*
 * TaskManagerPlus SessionReports: headless `--report <kind> <path>`.
 *
 * The core sets PhStartupParameters.ReportMode for --report (no window, no tray icon, no first-run prompts, never forwards
 * to a running instance). This file re-reads the command line, waits for the second process refresh (CPU % needs two),
 * writes the file with the same code the menu uses, and ends the process with a status code (cliparse.h). It exits with
 * ExitProcess so the settings file is never rewritten by a report run.
 */

#include "sessionreports.h"
#include "cliparse.h"

#define SR_CLI_TIMEOUT_MS 90000
#define SR_CLI_REFRESHES 2

typedef struct _SR_CLI_ARGS
{
    BOOLEAN Seen;
    PPH_STRING Kind;
    PPH_STRING Path;
} SR_CLI_ARGS;

static SR_CLI_ARGS SrCliArgs;
static ULONG SrCliRefreshes = 0;
static SR_KIND SrCliKind;
static SR_FORMAT SrCliFormat;
static PPH_STRING SrCliFullPath = NULL;
static PH_CALLBACK_REGISTRATION SrCliUpdatedRegistration;

static BOOLEAN NTAPI SrCliOption(
    _In_opt_ PCPH_COMMAND_LINE_OPTION Option,
    _In_opt_ PPH_STRING Value,
    _In_opt_ PVOID Context
    )
{
    if (Option)
    {
        SrCliArgs.Seen = TRUE;

        if (Value)
            PhMoveReference(&SrCliArgs.Kind, PhReferenceObject(Value));
    }
    else if (Value && SrCliArgs.Seen && !SrCliArgs.Path)
    {
        SrCliArgs.Path = PhReferenceObject(Value); // the first plain argument after --report <kind>
    }

    return TRUE;
}

static DECLSPEC_NORETURN VOID SrCliExit(
    _In_ ULONG Code
    )
{
    ExitProcess(Code);
}

static ULONG SrCliWrite(
    VOID
    )
{
    PPH_LIST cells;
    ULONG columns = 0;
    NTSTATUS status;

    cells = SrCollect(SrCliKind, (ULONG)PhGetIntegerSetting(SETTING_NAME_TREND_HOURS), &columns);

    if (!cells)
        return SR_CLI_EXIT_NO_SOURCE;

    status = SrWriteReport(SrCliKind, SrCliFormat, cells, columns, SrCliFullPath);
    SrFreeTable(cells);
    return NT_SUCCESS(status) ? SR_CLI_EXIT_OK : SR_CLI_EXIT_WRITE_FAILED;
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI SrCliProcessesUpdated(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    if (++SrCliRefreshes >= SR_CLI_REFRESHES)
        SrCliExit(SrCliWrite());
}

static DWORD WINAPI SrCliWatchdog(
    _In_ PVOID Parameter
    )
{
    Sleep(SR_CLI_TIMEOUT_MS);
    SrCliExit(SR_CLI_EXIT_TIMEOUT);
}

/** Called from the plugin load callback. Does nothing unless --report is on the command line. */
VOID SrCliInitialize(
    VOID
    )
{
    static CONST PH_COMMAND_LINE_OPTION options[] =
    {
        { 1, L"report", OptionalArgumentType },
        { 1, L"-report", OptionalArgumentType },
    };
    PH_STRINGREF commandLine;
    int kind;
    int format;
    HANDLE thread;

    PhInitializeStringRef(&commandLine, GetCommandLineW());

    PhParseCommandLine(
        &commandLine,
        (PCPH_COMMAND_LINE_OPTION)options,
        RTL_NUMBER_OF(options),
        PH_COMMAND_LINE_IGNORE_UNKNOWN_OPTIONS | PH_COMMAND_LINE_IGNORE_FIRST_PART,
        SrCliOption,
        NULL
        );

    if (!SrCliArgs.Seen)
        return;

    kind = SrCliArgs.Kind ? SrCliParseKind(SrCliArgs.Kind->Buffer) : -1;
    format = SrCliArgs.Path ? SrCliFormatFromPath(SrCliArgs.Path->Buffer) : -1;

    if (kind < 0 || format < 0 || !NT_SUCCESS(PhGetFullPath(SrCliArgs.Path->Buffer, &SrCliFullPath, NULL)))
        SrCliExit(SR_CLI_EXIT_BAD_ARGS);

    SrCliKind = (SR_KIND)kind;
    SrCliFormat = (SR_FORMAT)format;

    PhRegisterCallback(
        PhGetGeneralCallback(GeneralCallbackProcessesUpdated),
        SrCliProcessesUpdated,
        NULL,
        &SrCliUpdatedRegistration
        );

    if (thread = CreateThread(NULL, 0, SrCliWatchdog, NULL, 0, NULL))
        CloseHandle(thread);
}
