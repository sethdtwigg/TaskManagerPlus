/*
 * TaskManagerPlus SessionReports: writing a table to a file in CSV, JSON or HTML.
 */

#include "sessionreports.h"

static PPH_STRING SrGeneratedText(
    VOID
    )
{
    SYSTEMTIME st;

    GetLocalTime(&st);
    return PhFormatString(L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

static PPH_STRING SrStampText(
    VOID
    )
{
    SYSTEMTIME st;

    GetLocalTime(&st);
    return PhFormatString(L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

NTSTATUS SrWriteReport(
    _In_ SR_KIND Kind,
    _In_ SR_FORMAT Format,
    _In_ PPH_LIST Cells,
    _In_ ULONG Columns,
    _In_ PPH_STRING FileName
    )
{
    RF_TABLE table;
    RF_BUFFER buffer;
    PPH_STRING generated;
    PCWSTR *pointers;
    HANDLE fileHandle;
    NTSTATUS status;
    ULONG i;
    ULONG written = 0;
    BOOLEAN ok;

    if (!Columns || Cells->Count < Columns)
        return STATUS_INVALID_PARAMETER;

    pointers = PhAllocateZero(sizeof(PCWSTR) * (Cells->Count + 1));

    for (i = 0; i < Cells->Count; i++)
        pointers[i] = ((PPH_STRING)Cells->Items[i])->Buffer;

    memset(&table, 0, sizeof(table));
    table.Title = SrGetKindTitle(Kind);
    table.ColumnCount = Columns;
    table.Headers = pointers;
    table.RowCount = (Cells->Count - Columns) / Columns;
    table.Cells = pointers + Columns;

    memset(&buffer, 0, sizeof(buffer));
    generated = SrGeneratedText();

    switch (Format)
    {
    case SrFormatJson:
        ok = RfWriteJson(&table, generated->Buffer, &buffer);
        break;
    case SrFormatHtml:
        ok = RfWriteHtml(&table, generated->Buffer, &buffer);
        break;
    default:
        ok = RfWriteCsv(&table, &buffer);
        break;
    }

    PhDereferenceObject(generated);
    PhFree(pointers);

    if (!ok)
    {
        RfFree(&buffer);
        return STATUS_NO_MEMORY;
    }

    status = PhCreateFileWin32(
        &fileHandle,
        FileName->Buffer,
        FILE_GENERIC_WRITE,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ,
        FILE_OVERWRITE_IF,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE
        );

    if (NT_SUCCESS(status))
    {
        status = PhWriteFile(fileHandle, buffer.Data, (ULONG)buffer.Length, NULL, &written);
        NtClose(fileHandle);
    }

    RfFree(&buffer);
    return status;
}

static SR_FORMAT SrFormatFromFileName(
    _In_ PPH_STRING FileName,
    _In_ SR_FORMAT Default
    )
{
    if (PhEndsWithString2(FileName, L".json", TRUE))
        return SrFormatJson;
    if (PhEndsWithString2(FileName, L".html", TRUE) || PhEndsWithString2(FileName, L".htm", TRUE))
        return SrFormatHtml;
    if (PhEndsWithString2(FileName, L".csv", TRUE))
        return SrFormatCsv;

    return Default;
}

VOID SrExportInteractive(
    _In_ HWND OwnerWindow,
    _In_ SR_KIND Kind,
    _In_ SR_FORMAT DefaultFormat
    )
{
    static CONST PH_FILETYPE_FILTER filters[] =
    {
        { L"CSV (*.csv)", L"*.csv" },
        { L"JSON (*.json)", L"*.json" },
        { L"HTML page (*.html)", L"*.html" }
    };
    PPH_LIST cells;
    ULONG columns = 0;
    PVOID fileDialog;
    PPH_STRING stamp;
    PPH_STRING suggestion;
    PPH_STRING fileName;
    SR_FORMAT format;
    NTSTATUS status;

    cells = SrCollect(Kind, (ULONG)PhGetIntegerSetting(SETTING_NAME_TREND_HOURS), &columns);

    if (!cells)
    {
        PhShowInformation2(OwnerWindow, L"Export report", L"%s", L"That data source is not available (its plugin is not loaded).");
        return;
    }

    stamp = SrStampText();
    suggestion = PhFormatString(L"taskmanagerplus-%s-%s.%s", SrGetKindName(Kind), stamp->Buffer, SrGetFormatExtension(DefaultFormat));
    PhDereferenceObject(stamp);

    fileDialog = PhCreateSaveFileDialog();
    PhSetFileDialogFilter(fileDialog, (PPH_FILETYPE_FILTER)filters, RTL_NUMBER_OF(filters));
    PhSetFileDialogFileName(fileDialog, suggestion->Buffer);
    PhDereferenceObject(suggestion);

    if (PhShowFileDialog(OwnerWindow, fileDialog))
    {
        fileName = PhGetFileDialogFileName(fileDialog);
        format = SrFormatFromFileName(fileName, SrFormatCsv);
        status = SrWriteReport(Kind, format, cells, columns, fileName);

        if (NT_SUCCESS(status))
        {
            PhLogMessageEntry(PH_LOG_ENTRY_MESSAGE, PhFormatString(L"Exported %s report to %s", SrGetKindName(Kind), fileName->Buffer));
            PhShowInformation2(OwnerWindow, L"Export report", L"Saved %s to:\n%s", SrGetKindTitle(Kind), fileName->Buffer);
        }
        else
        {
            PhShowStatus(OwnerWindow, L"Unable to save the report", status, 0);
        }

        PhDereferenceObject(fileName);
    }

    PhFreeFileDialog(fileDialog);
    SrFreeTable(cells);
}

NTSTATUS SrExportToDirectory(
    _In_ SR_KIND Kind,
    _In_ SR_FORMAT Format,
    _In_ PPH_STRING Directory
    )
{
    PPH_LIST cells;
    ULONG columns = 0;
    PPH_STRING fileName;
    NTSTATUS status;

    cells = SrCollect(Kind, (ULONG)PhGetIntegerSetting(SETTING_NAME_TREND_HOURS), &columns);

    if (!cells)
        return STATUS_NOT_FOUND;

    fileName = PhFormatString(L"%s\\report-%s.%s", Directory->Buffer, SrGetKindName(Kind), SrGetFormatExtension(Format));
    status = SrWriteReport(Kind, Format, cells, columns, fileName);
    PhDereferenceObject(fileName);
    SrFreeTable(cells);
    return status;
}
