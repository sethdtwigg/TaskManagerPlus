/*
 * TaskManagerPlus StartupManager plugin.
 *
 * Enumeration of startup entries (registry Run/RunOnce, startup folders,
 * logon/boot scheduled tasks) and the enable/disable/delete actions on them.
 *
 * Every function in this file must be called on the main thread: the shell
 * link and Task Scheduler COM objects rely on the apartment that System
 * Informer initialises there.
 */

#include "startupmanager.h"
#include <shobjidl.h>
#include <taskschd.h>

DEFINE_GUID(SmFolderIdStartup, 0xB97D20BB, 0xF46A, 0x4C97, 0xBA, 0x10, 0x5E, 0x36, 0x08, 0x43, 0x08, 0x54);
DEFINE_GUID(SmFolderIdCommonStartup, 0x82A5EA35, 0xD9CD, 0x47C5, 0x96, 0x29, 0xE1, 0x5D, 0x2F, 0x71, 0x4E, 0x6E);
DEFINE_GUID(SmClsidShellLink, 0x00021401, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
DEFINE_GUID(SmIidShellLinkW, 0x000214f9, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
DEFINE_GUID(SmIidPersistFile, 0x0000010b, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
DEFINE_GUID(SmClsidTaskScheduler, 0x0f87369f, 0xa4e5, 0x4cfc, 0xbd, 0x3e, 0x73, 0xe6, 0x15, 0x45, 0x72, 0xdd);
DEFINE_GUID(SmIidTaskService, 0x2FABA4C7, 0x4DA9, 0x4013, 0x96, 0x97, 0x20, 0xCC, 0x3F, 0xD4, 0x0F, 0x85);
DEFINE_GUID(SmIidExecAction, 0x4C3D624D, 0xfd6b, 0x49A3, 0xB9, 0xB7, 0x09, 0xCB, 0x3C, 0xD3, 0xF0, 0x47);

static CONST PH_STRINGREF SmStartupApprovedKeyName = PH_STRINGREF_INIT(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\");
static CONST PH_STRINGREF SmApprovedRun = PH_STRINGREF_INIT(L"Run");
static CONST PH_STRINGREF SmApprovedRun32 = PH_STRINGREF_INIT(L"Run32");
static CONST PH_STRINGREF SmApprovedStartupFolder = PH_STRINGREF_INIT(L"StartupFolder");
static CONST PH_STRINGREF SmTaskRootFolder = PH_STRINGREF_INIT(L"\\");
static CONST PH_STRINGREF SmShortcutExtension = PH_STRINGREF_INIT(L".lnk");
static CONST PH_STRINGREF SmDesktopIni = PH_STRINGREF_INIT(L"desktop.ini");

static CONST PH_STRINGREF SmTypeStrings[] =
{
    PH_STRINGREF_INIT(L"Registry Run (HKLM)"),
    PH_STRINGREF_INIT(L"Registry Run (HKLM, 32-bit)"),
    PH_STRINGREF_INIT(L"Registry RunOnce (HKLM)"),
    PH_STRINGREF_INIT(L"Registry RunOnce (HKLM, 32-bit)"),
    PH_STRINGREF_INIT(L"Registry Run (HKCU)"),
    PH_STRINGREF_INIT(L"Registry Run (HKCU, 32-bit)"),
    PH_STRINGREF_INIT(L"Registry RunOnce (HKCU)"),
    PH_STRINGREF_INIT(L"Registry RunOnce (HKCU, 32-bit)"),
    PH_STRINGREF_INIT(L"Startup folder (user)"),
    PH_STRINGREF_INIT(L"Startup folder (common)"),
    PH_STRINGREF_INIT(L"Scheduled task"),
    PH_STRINGREF_INIT(L"Store app startup task"),
};

static CONST PH_STRINGREF SmImpactStrings[] =
{
    PH_STRINGREF_INIT(L"Not measured"),
    PH_STRINGREF_INIT(L"Low"),
    PH_STRINGREF_INIT(L"Medium"),
    PH_STRINGREF_INIT(L"High"),
};

typedef struct _SM_REGISTRY_SOURCE
{
    SM_SOURCE_TYPE Source;
    HANDLE RootKey;
    PCWSTR RootName;
    PCWSTR KeyPath;
    BOOLEAN Wow64;
    BOOLEAN RunOnce;
} SM_REGISTRY_SOURCE, *PSM_REGISTRY_SOURCE;

typedef CONST SM_REGISTRY_SOURCE* PCSM_REGISTRY_SOURCE;

static CONST SM_REGISTRY_SOURCE SmRegistrySources[] =
{
    { SmSourceRegistryHklm, PH_KEY_LOCAL_MACHINE, L"HKLM", L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", FALSE, FALSE },
    { SmSourceRegistryHklm, PH_KEY_LOCAL_MACHINE, L"HKLM", L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", FALSE, TRUE },
    { SmSourceRegistryHklm, PH_KEY_LOCAL_MACHINE, L"HKLM", L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run", TRUE, FALSE },
    { SmSourceRegistryHklm, PH_KEY_LOCAL_MACHINE, L"HKLM", L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnce", TRUE, TRUE },
    { SmSourceRegistryHkcu, PH_KEY_CURRENT_USER, L"HKCU", L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", FALSE, FALSE },
    { SmSourceRegistryHkcu, PH_KEY_CURRENT_USER, L"HKCU", L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", FALSE, TRUE },
    { SmSourceRegistryHkcu, PH_KEY_CURRENT_USER, L"HKCU", L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run", TRUE, FALSE },
    { SmSourceRegistryHkcu, PH_KEY_CURRENT_USER, L"HKCU", L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnce", TRUE, TRUE },
};

typedef struct _SM_REGISTRY_ENUM_CONTEXT
{
    PPH_LIST List;
    PCSM_REGISTRY_SOURCE Source;
} SM_REGISTRY_ENUM_CONTEXT, *PSM_REGISTRY_ENUM_CONTEXT;

typedef struct _SM_FOLDER_ENUM_CONTEXT
{
    PPH_LIST List;
    SM_SOURCE_TYPE Source;
    PPH_STRING FolderPath;
} SM_FOLDER_ENUM_CONTEXT, *PSM_FOLDER_ENUM_CONTEXT;

PCPH_STRINGREF SmGetSourceTypeString(
    _In_ PSM_STARTUP_ITEM Item
    )
{
    ULONG index;

    switch (Item->Source)
    {
    case SmSourceRegistryHklm:
    case SmSourceRegistryHkcu:
        index = (Item->Source == SmSourceRegistryHkcu ? 4 : 0) + (Item->RunOnce ? 2 : 0) + (Item->Wow64 ? 1 : 0);
        break;
    case SmSourceStartupFolderUser:
        index = 8;
        break;
    case SmSourceStartupFolderCommon:
        index = 9;
        break;
    case SmSourcePackagedApp:
        index = 11;
        break;
    default:
        index = 10;
        break;
    }

    return &SmTypeStrings[index];
}

PCPH_STRINGREF SmGetImpactString(
    _In_ SM_IMPACT Impact
    )
{
    if ((ULONG)Impact < RTL_NUMBER_OF(SmImpactStrings))
        return &SmImpactStrings[Impact];

    return &SmImpactStrings[SmImpactUnknown];
}

static PSM_STARTUP_ITEM SmCreateStartupItem(
    _In_ SM_SOURCE_TYPE Source
    )
{
    PSM_STARTUP_ITEM item;

    item = PhAllocateZero(sizeof(SM_STARTUP_ITEM));
    item->Source = Source;
    item->Enabled = TRUE;

    return item;
}

VOID SmFreeStartupItem(
    _In_ PSM_STARTUP_ITEM Item
    )
{
    PhClearReference(&Item->Name);
    PhClearReference(&Item->Command);
    PhClearReference(&Item->Location);
    PhClearReference(&Item->FileName);
    PhClearReference(&Item->Publisher);
    PhClearReference(&Item->ObjectPath);
    PhClearReference(&Item->ApprovedValueName);
    PhFree(Item);
}

/**
 * Strips surrounding quotation marks from a path.
 */
static PPH_STRING SmUnquotePath(
    _In_ PCPH_STRINGREF Path
    )
{
    PH_STRINGREF path;

    path = *Path;

    if (path.Length >= 2 * sizeof(WCHAR) && path.Buffer[0] == L'"' && path.Buffer[path.Length / sizeof(WCHAR) - 1] == L'"')
    {
        path.Buffer++;
        path.Length -= 2 * sizeof(WCHAR);
    }

    return PhCreateString2(&path);
}

/**
 * Fills the publisher, file size and impact fields from the target file.
 */
static VOID SmQueryTargetFileInformation(
    _Inout_ PSM_STARTUP_ITEM Item
    )
{
    FILE_NETWORK_OPEN_INFORMATION fileInformation;
    PH_IMAGE_VERSION_INFO versionInfo;

    if (PhIsNullOrEmptyString(Item->FileName))
        return;

    if (NT_SUCCESS(PhQueryFullAttributesFileWin32(Item->FileName->Buffer, &fileInformation)))
    {
        Item->FileSize = (ULONG64)fileInformation.EndOfFile.QuadPart;

        if (Item->FileSize < 1 * 1024 * 1024)
            Item->Impact = SmImpactLow;
        else if (Item->FileSize < 5 * 1024 * 1024)
            Item->Impact = SmImpactMedium;
        else
            Item->Impact = SmImpactHigh;
    }

    if (NT_SUCCESS(PhInitializeImageVersionInfo(&versionInfo, Item->FileName->Buffer)))
    {
        if (!PhIsNullOrEmptyString(versionInfo.CompanyName))
            PhSetReference(&Item->Publisher, versionInfo.CompanyName);

        PhDeleteImageVersionInfo(&versionInfo);
    }
}

/**
 * Resolves the target executable from a command line (registry value or task action).
 */
static VOID SmResolveCommandTarget(
    _Inout_ PSM_STARTUP_ITEM Item,
    _In_ PCPH_STRINGREF CommandLine
    )
{
    PH_STRINGREF fileNamePart;
    PH_STRINGREF argumentsPart;
    PPH_STRING fullFileName = NULL;

    if (PhParseCommandLineFuzzy(CommandLine, &fileNamePart, &argumentsPart, &fullFileName))
    {
        if (fullFileName)
        {
            PhMoveReference(&Item->FileName, fullFileName);
        }
        else
        {
            PhMoveReference(&Item->FileName, SmUnquotePath(&fileNamePart));
        }
    }
}

/**
 * Reads the StartupApproved state for an entry. A missing value means enabled.
 */
static BOOLEAN SmQueryApprovedState(
    _In_ HANDLE RootKey,
    _In_ PCPH_STRINGREF ApprovedKeyName,
    _In_ PCPH_STRINGREF ValueName
    )
{
    BOOLEAN enabled = TRUE;
    NTSTATUS status;
    HANDLE keyHandle;
    PPH_STRING keyPath;
    PKEY_VALUE_PARTIAL_INFORMATION buffer;

    keyPath = PhConcatStringRef2(&SmStartupApprovedKeyName, ApprovedKeyName);

    status = PhOpenKey(
        &keyHandle,
        KEY_READ,
        RootKey,
        &keyPath->sr,
        0
        );

    if (NT_SUCCESS(status))
    {
        if (NT_SUCCESS(PhQueryValueKey(keyHandle, ValueName, KeyValuePartialInformation, &buffer)))
        {
            if (buffer->Type == REG_BINARY && buffer->DataLength >= sizeof(UCHAR))
            {
                // 0x02 / 0x06 = enabled, 0x03 / 0x07 = disabled.
                enabled = !(buffer->Data[0] & 1);
            }

            PhFree(buffer);
        }

        NtClose(keyHandle);
    }

    PhDereferenceObject(keyPath);

    return enabled;
}

/**
 * Writes the StartupApproved state for an entry.
 */
static NTSTATUS SmSetApprovedState(
    _In_ HANDLE RootKey,
    _In_ PCPH_STRINGREF ApprovedKeyName,
    _In_ PCPH_STRINGREF ValueName,
    _In_ BOOLEAN Enable
    )
{
    NTSTATUS status;
    HANDLE keyHandle;
    PPH_STRING keyPath;
    UCHAR data[12];

    keyPath = PhConcatStringRef2(&SmStartupApprovedKeyName, ApprovedKeyName);

    status = PhCreateKey(
        &keyHandle,
        KEY_WRITE,
        RootKey,
        &keyPath->sr,
        0,
        0,
        NULL
        );

    if (NT_SUCCESS(status))
    {
        memset(data, 0, sizeof(data));

        if (Enable)
        {
            data[0] = 0x02;
        }
        else
        {
            LARGE_INTEGER systemTime;

            data[0] = 0x03;
            PhQuerySystemTime(&systemTime);
            memcpy(&data[4], &systemTime, sizeof(LARGE_INTEGER));
        }

        status = PhSetValueKey(keyHandle, ValueName, REG_BINARY, data, sizeof(data));
        NtClose(keyHandle);
    }

    PhDereferenceObject(keyPath);

    return status;
}

static HANDLE SmGetApprovedRootKey(
    _In_ PSM_STARTUP_ITEM Item
    )
{
    switch (Item->Source)
    {
    case SmSourceRegistryHklm:
    case SmSourceStartupFolderCommon:
        return PH_KEY_LOCAL_MACHINE;
    default:
        return PH_KEY_CURRENT_USER;
    }
}

// Registry Run / RunOnce

_Function_class_(PH_ENUM_KEY_CALLBACK)
static BOOLEAN NTAPI SmEnumRegistryValueCallback(
    _In_ HANDLE RootDirectory,
    _In_ PVOID Information,
    _In_opt_ PVOID Context
    )
{
    PKEY_VALUE_FULL_INFORMATION information = Information;
    PSM_REGISTRY_ENUM_CONTEXT context = Context;
    PSM_STARTUP_ITEM item;
    PH_STRINGREF valueName;
    PH_STRINGREF valueData;
    PPH_STRING command;

    if (!context)
        return FALSE;

    if (information->Type != REG_SZ && information->Type != REG_EXPAND_SZ)
        return TRUE;

    valueName.Buffer = information->Name;
    valueName.Length = information->NameLength;

    // Skip the unnamed default value.
    if (valueName.Length == 0)
        return TRUE;

    valueData.Buffer = PTR_ADD_OFFSET(information, information->DataOffset);
    valueData.Length = information->DataLength;

    // Trim a trailing null terminator from the data.
    while (valueData.Length >= sizeof(WCHAR) && valueData.Buffer[valueData.Length / sizeof(WCHAR) - 1] == UNICODE_NULL)
        valueData.Length -= sizeof(WCHAR);

    if (valueData.Length == 0)
        return TRUE;

    if (information->Type == REG_EXPAND_SZ)
        command = PhExpandEnvironmentStrings(&valueData);
    else
        command = PhCreateString2(&valueData);

    item = SmCreateStartupItem(context->Source->Source);
    item->Wow64 = context->Source->Wow64;
    item->RunOnce = context->Source->RunOnce;
    item->Name = PhCreateString2(&valueName);
    item->Command = command;
    item->Location = PhFormatString(L"%s\\%s", context->Source->RootName, context->Source->KeyPath);
    item->ObjectPath = PhCreateString(context->Source->KeyPath);
    item->ApprovedKeyName = context->Source->Wow64 ? SmApprovedRun32 : SmApprovedRun;
    item->ApprovedValueName = PhCreateString2(&valueName);

    // Explorer only consults StartupApproved for the Run keys; RunOnce entries
    // cannot be disabled without removing them.
    item->CanToggle = !context->Source->RunOnce;

    if (item->CanToggle)
    {
        item->Enabled = SmQueryApprovedState(
            context->Source->RootKey,
            &item->ApprovedKeyName,
            &item->ApprovedValueName->sr
            );
    }

    SmResolveCommandTarget(item, &command->sr);
    SmQueryTargetFileInformation(item);

    PhAddItemList(context->List, item);

    return TRUE;
}

static VOID SmEnumerateRegistrySource(
    _Inout_ PPH_LIST List,
    _In_ PCSM_REGISTRY_SOURCE Source
    )
{
    NTSTATUS status;
    HANDLE keyHandle;
    PH_STRINGREF keyPath;
    SM_REGISTRY_ENUM_CONTEXT context;

    PhInitializeStringRef(&keyPath, Source->KeyPath);

    status = PhOpenKey(
        &keyHandle,
        KEY_READ,
        Source->RootKey,
        &keyPath,
        0
        );

    if (!NT_SUCCESS(status))
        return;

    context.List = List;
    context.Source = Source;

    PhEnumerateValueKey(keyHandle, KeyValueFullInformation, SmEnumRegistryValueCallback, &context);

    NtClose(keyHandle);
}

// Startup folders

/**
 * Resolves a .lnk file to its target path and arguments via IShellLinkW.
 */
static BOOLEAN SmResolveShortcut(
    _In_ PPH_STRING ShortcutFileName,
    _Out_ PPH_STRING* TargetFileName,
    _Out_ PPH_STRING* Arguments
    )
{
    BOOLEAN result = FALSE;
    IShellLinkW* shellLink;
    IPersistFile* persistFile;
    WCHAR path[MAX_PATH];
    WCHAR arguments[MAX_PATH];

    *TargetFileName = NULL;
    *Arguments = NULL;

    if (SUCCEEDED(PhGetClassObject(L"shell32.dll", &SmClsidShellLink, &SmIidShellLinkW, &shellLink)))
    {
        if (SUCCEEDED(IShellLinkW_QueryInterface(shellLink, &SmIidPersistFile, &persistFile)))
        {
            if (SUCCEEDED(IPersistFile_Load(persistFile, ShortcutFileName->Buffer, STGM_READ)))
            {
                if (SUCCEEDED(IShellLinkW_GetPath(shellLink, path, RTL_NUMBER_OF(path), NULL, SLGP_RAWPATH)) && path[0] != UNICODE_NULL)
                {
                    *TargetFileName = PhExpandEnvironmentStringsZ(path);
                    result = TRUE;

                    if (SUCCEEDED(IShellLinkW_GetArguments(shellLink, arguments, RTL_NUMBER_OF(arguments))) && arguments[0] != UNICODE_NULL)
                    {
                        *Arguments = PhCreateString(arguments);
                    }
                }
            }

            IPersistFile_Release(persistFile);
        }

        IShellLinkW_Release(shellLink);
    }

    return result;
}

_Function_class_(PH_ENUM_DIRECTORY_FILE)
static BOOLEAN NTAPI SmEnumStartupFolderCallback(
    _In_ HANDLE RootDirectory,
    _In_ PVOID Information,
    _In_opt_ PVOID Context
    )
{
    PFILE_DIRECTORY_INFORMATION information = Information;
    PSM_FOLDER_ENUM_CONTEXT context = Context;
    PSM_STARTUP_ITEM item;
    PH_STRINGREF fileName;
    PH_STRINGREF baseName;
    PPH_STRING fullPath;
    PPH_STRING targetFileName;
    PPH_STRING arguments;

    if (!context)
        return FALSE;

    if (FlagOn(information->FileAttributes, FILE_ATTRIBUTE_DIRECTORY))
        return TRUE;

    fileName.Buffer = information->FileName;
    fileName.Length = information->FileNameLength;

    if (PhEqualStringRef(&fileName, &SmDesktopIni, TRUE))
        return TRUE;

    fullPath = PhConcatStringRef3(&context->FolderPath->sr, &PhNtPathSeparatorString, &fileName);

    item = SmCreateStartupItem(context->Source);
    item->CanToggle = TRUE;
    item->Location = PhReferenceObject(context->FolderPath);
    item->ObjectPath = fullPath;
    item->ApprovedKeyName = SmApprovedStartupFolder;
    item->ApprovedValueName = PhCreateString2(&fileName);

    if (PhEndsWithStringRef(&fileName, &SmShortcutExtension, TRUE))
    {
        baseName = fileName;
        baseName.Length -= SmShortcutExtension.Length;
        item->Name = PhCreateString2(&baseName);

        if (SmResolveShortcut(fullPath, &targetFileName, &arguments))
        {
            item->FileName = targetFileName;

            if (arguments)
            {
                item->Command = PhFormatString(L"\"%s\" %s", targetFileName->Buffer, arguments->Buffer);
                PhDereferenceObject(arguments);
            }
            else
            {
                item->Command = PhReferenceObject(targetFileName);
            }
        }
        else
        {
            item->Command = PhReferenceObject(fullPath);
        }
    }
    else
    {
        item->Name = PhCreateString2(&fileName);
        item->FileName = PhReferenceObject(fullPath);
        item->Command = PhReferenceObject(fullPath);
    }

    item->Enabled = SmQueryApprovedState(
        SmGetApprovedRootKey(item),
        &item->ApprovedKeyName,
        &item->ApprovedValueName->sr
        );

    SmQueryTargetFileInformation(item);

    PhAddItemList(context->List, item);

    return TRUE;
}

static VOID SmEnumerateStartupFolder(
    _Inout_ PPH_LIST List,
    _In_ SM_SOURCE_TYPE Source,
    _In_ PCGUID FolderId
    )
{
    NTSTATUS status;
    PPH_STRING folderPath;
    HANDLE directoryHandle;
    SM_FOLDER_ENUM_CONTEXT context;

    if (!(folderPath = PhGetKnownFolderPath(FolderId, NULL)))
        return;

    // Drop a trailing separator so paths can be joined uniformly.
    if (PhEndsWithStringRef(&folderPath->sr, &PhNtPathSeparatorString, FALSE))
        PhMoveReference(&folderPath, PhCreateStringEx(folderPath->Buffer, folderPath->Length - PhNtPathSeparatorString.Length));

    status = PhCreateFileWin32(
        &directoryHandle,
        folderPath->Buffer,
        FILE_LIST_DIRECTORY | SYNCHRONIZE,
        FILE_ATTRIBUTE_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN,
        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT
        );

    if (NT_SUCCESS(status))
    {
        context.List = List;
        context.Source = Source;
        context.FolderPath = folderPath;

        PhEnumDirectoryFile(directoryHandle, NULL, SmEnumStartupFolderCallback, &context);

        NtClose(directoryHandle);
    }

    PhDereferenceObject(folderPath);
}

// Scheduled tasks

static PPH_STRING SmCreateStringFromBSTR(
    _In_opt_ BSTR String
    )
{
    if (!String)
        return NULL;

    return PhCreateStringEx(String, SysStringLen(String) * sizeof(WCHAR));
}

static BOOLEAN SmTaskHasLogonTrigger(
    _In_ ITaskDefinition* Definition
    )
{
    BOOLEAN result = FALSE;
    ITriggerCollection* triggers = NULL;
    LONG count = 0;

    if (SUCCEEDED(ITaskDefinition_get_Triggers(Definition, &triggers)) && triggers)
    {
        if (SUCCEEDED(ITriggerCollection_get_Count(triggers, &count)))
        {
            for (LONG i = 1; i <= count && !result; i++)
            {
                ITrigger* trigger = NULL;
                TASK_TRIGGER_TYPE2 type;

                if (SUCCEEDED(ITriggerCollection_get_Item(triggers, i, &trigger)) && trigger)
                {
                    if (SUCCEEDED(ITrigger_get_Type(trigger, &type)))
                    {
                        if (type == TASK_TRIGGER_LOGON || type == TASK_TRIGGER_BOOT)
                            result = TRUE;
                    }

                    ITrigger_Release(trigger);
                }
            }
        }

        ITriggerCollection_Release(triggers);
    }

    return result;
}

static BOOLEAN SmGetTaskExecAction(
    _In_ ITaskDefinition* Definition,
    _Out_ PPH_STRING* Path,
    _Out_ PPH_STRING* Arguments
    )
{
    BOOLEAN result = FALSE;
    IActionCollection* actions = NULL;
    LONG count = 0;

    *Path = NULL;
    *Arguments = NULL;

    if (SUCCEEDED(ITaskDefinition_get_Actions(Definition, &actions)) && actions)
    {
        if (SUCCEEDED(IActionCollection_get_Count(actions, &count)))
        {
            for (LONG i = 1; i <= count && !result; i++)
            {
                IAction* action = NULL;
                IExecAction* execAction = NULL;
                TASK_ACTION_TYPE type;

                if (SUCCEEDED(IActionCollection_get_Item(actions, i, &action)) && action)
                {
                    if (SUCCEEDED(IAction_get_Type(action, &type)) && type == TASK_ACTION_EXEC)
                    {
                        if (SUCCEEDED(IAction_QueryInterface(action, &SmIidExecAction, &execAction)) && execAction)
                        {
                            BSTR path = NULL;
                            BSTR arguments = NULL;

                            if (SUCCEEDED(IExecAction_get_Path(execAction, &path)) && path)
                            {
                                *Path = SmCreateStringFromBSTR(path);
                                SysFreeString(path);
                                result = TRUE;

                                if (SUCCEEDED(IExecAction_get_Arguments(execAction, &arguments)) && arguments)
                                {
                                    *Arguments = SmCreateStringFromBSTR(arguments);
                                    SysFreeString(arguments);
                                }
                            }

                            IExecAction_Release(execAction);
                        }
                    }

                    IAction_Release(action);
                }
            }
        }

        IActionCollection_Release(actions);
    }

    return result;
}

static VOID SmProcessRegisteredTask(
    _Inout_ PPH_LIST List,
    _In_ IRegisteredTask* Task
    )
{
    ITaskDefinition* definition = NULL;
    PSM_STARTUP_ITEM item;
    BSTR name = NULL;
    BSTR path = NULL;
    VARIANT_BOOL enabled = VARIANT_TRUE;
    PPH_STRING execPath;
    PPH_STRING execArguments;
    PPH_STRING taskPath;
    PH_STRINGREF folderPart;
    PH_STRINGREF namePart;

    if (FAILED(IRegisteredTask_get_Definition(Task, &definition)) || !definition)
        return;

    if (!SmTaskHasLogonTrigger(definition))
    {
        ITaskDefinition_Release(definition);
        return;
    }

    if (FAILED(IRegisteredTask_get_Path(Task, &path)) || !path)
    {
        ITaskDefinition_Release(definition);
        return;
    }

    taskPath = SmCreateStringFromBSTR(path);
    SysFreeString(path);

    item = SmCreateStartupItem(SmSourceScheduledTask);
    item->CanToggle = TRUE;
    item->ObjectPath = taskPath;

    if (SUCCEEDED(IRegisteredTask_get_Name(Task, &name)) && name)
    {
        item->Name = SmCreateStringFromBSTR(name);
        SysFreeString(name);
    }
    else
    {
        item->Name = PhReferenceObject(taskPath);
    }

    // Location is the task folder ("\" for the root).
    if (PhSplitStringRefAtLastChar(&taskPath->sr, OBJ_NAME_PATH_SEPARATOR, &folderPart, &namePart) && folderPart.Length != 0)
        item->Location = PhCreateString2(&folderPart);
    else
        item->Location = PhCreateString2(&SmTaskRootFolder);

    if (SUCCEEDED(IRegisteredTask_get_Enabled(Task, &enabled)))
        item->Enabled = (enabled != VARIANT_FALSE);

    if (SmGetTaskExecAction(definition, &execPath, &execArguments))
    {
        PPH_STRING expandedPath;

        expandedPath = PhExpandEnvironmentStrings(&execPath->sr);
        item->FileName = SmUnquotePath(&expandedPath->sr);

        if (execArguments)
        {
            item->Command = PhFormatString(L"%s %s", expandedPath->Buffer, execArguments->Buffer);
            PhDereferenceObject(execArguments);
        }
        else
        {
            item->Command = PhReferenceObject(expandedPath);
        }

        PhDereferenceObject(expandedPath);
        PhDereferenceObject(execPath);

        // If the path was relative (e.g. "cmd.exe"), let the fuzzy parser search for it.
        if (item->FileName && !PhDoesFileExistWin32(item->FileName->Buffer))
        {
            SmResolveCommandTarget(item, &item->Command->sr);
        }
    }
    else
    {
        item->Command = PhReferenceObject(taskPath);
    }

    SmQueryTargetFileInformation(item);

    PhAddItemList(List, item);

    ITaskDefinition_Release(definition);
}

static VOID SmEnumerateTaskFolder(
    _Inout_ PPH_LIST List,
    _In_ ITaskFolder* Folder
    )
{
    IRegisteredTaskCollection* tasks = NULL;
    ITaskFolderCollection* folders = NULL;
    LONG count = 0;
    VARIANT index;

    VariantInit(&index);
    V_VT(&index) = VT_I4;

    if (SUCCEEDED(ITaskFolder_GetTasks(Folder, TASK_ENUM_HIDDEN, &tasks)) && tasks)
    {
        if (SUCCEEDED(IRegisteredTaskCollection_get_Count(tasks, &count)))
        {
            for (LONG i = 1; i <= count; i++)
            {
                IRegisteredTask* task = NULL;

                V_I4(&index) = i;

                if (SUCCEEDED(IRegisteredTaskCollection_get_Item(tasks, index, &task)) && task)
                {
                    SmProcessRegisteredTask(List, task);
                    IRegisteredTask_Release(task);
                }
            }
        }

        IRegisteredTaskCollection_Release(tasks);
    }

    if (SUCCEEDED(ITaskFolder_GetFolders(Folder, 0, &folders)) && folders)
    {
        count = 0;

        if (SUCCEEDED(ITaskFolderCollection_get_Count(folders, &count)))
        {
            for (LONG i = 1; i <= count; i++)
            {
                ITaskFolder* subFolder = NULL;

                V_I4(&index) = i;

                if (SUCCEEDED(ITaskFolderCollection_get_Item(folders, index, &subFolder)) && subFolder)
                {
                    SmEnumerateTaskFolder(List, subFolder);
                    ITaskFolder_Release(subFolder);
                }
            }
        }

        ITaskFolderCollection_Release(folders);
    }
}

/**
 * Connects to the Task Scheduler service and opens the root task folder.
 * The caller releases both interfaces.
 */
static HRESULT SmOpenTaskRootFolder(
    _Out_ ITaskService** Service,
    _Out_ ITaskFolder** RootFolder
    )
{
    HRESULT status;
    ITaskService* taskService = NULL;
    ITaskFolder* rootFolder = NULL;
    BSTR rootFolderName = NULL;
    VARIANT empty = { VT_EMPTY };

    *Service = NULL;
    *RootFolder = NULL;

    status = PhGetClassObject(
        L"taskschd.dll",
        &SmClsidTaskScheduler,
        &SmIidTaskService,
        &taskService
        );

    if (FAILED(status))
        return status;

    status = ITaskService_Connect(taskService, empty, empty, empty, empty);

    if (SUCCEEDED(status))
    {
        rootFolderName = SysAllocStringLen(SmTaskRootFolder.Buffer, (UINT)(SmTaskRootFolder.Length / sizeof(WCHAR)));
        status = ITaskService_GetFolder(taskService, rootFolderName, &rootFolder);
        SysFreeString(rootFolderName);
    }

    if (SUCCEEDED(status))
    {
        *Service = taskService;
        *RootFolder = rootFolder;
    }
    else
    {
        if (rootFolder)
            ITaskFolder_Release(rootFolder);

        ITaskService_Release(taskService);
    }

    return status;
}

static VOID SmEnumerateScheduledTasks(
    _Inout_ PPH_LIST List
    )
{
    ITaskService* taskService;
    ITaskFolder* rootFolder;

    if (FAILED(SmOpenTaskRootFolder(&taskService, &rootFolder)))
        return;

    SmEnumerateTaskFolder(List, rootFolder);

    ITaskFolder_Release(rootFolder);
    ITaskService_Release(taskService);
}

static HRESULT SmSetScheduledTaskEnabled(
    _In_ PSM_STARTUP_ITEM Item,
    _In_ BOOLEAN Enable
    )
{
    HRESULT status;
    ITaskService* taskService;
    ITaskFolder* rootFolder;
    IRegisteredTask* task = NULL;
    BSTR taskPath;

    status = SmOpenTaskRootFolder(&taskService, &rootFolder);

    if (FAILED(status))
        return status;

    taskPath = SysAllocStringLen(Item->ObjectPath->Buffer, (UINT)(Item->ObjectPath->Length / sizeof(WCHAR)));
    status = ITaskFolder_GetTask(rootFolder, taskPath, &task);

    if (SUCCEEDED(status) && task)
    {
        status = IRegisteredTask_put_Enabled(task, Enable ? VARIANT_TRUE : VARIANT_FALSE);
        IRegisteredTask_Release(task);
    }

    SysFreeString(taskPath);
    ITaskFolder_Release(rootFolder);
    ITaskService_Release(taskService);

    return status;
}

static HRESULT SmDeleteScheduledTask(
    _In_ PSM_STARTUP_ITEM Item
    )
{
    HRESULT status;
    ITaskService* taskService;
    ITaskFolder* rootFolder;
    BSTR taskPath;

    status = SmOpenTaskRootFolder(&taskService, &rootFolder);

    if (FAILED(status))
        return status;

    taskPath = SysAllocStringLen(Item->ObjectPath->Buffer, (UINT)(Item->ObjectPath->Length / sizeof(WCHAR)));
    status = ITaskFolder_DeleteTask(rootFolder, taskPath, 0);

    SysFreeString(taskPath);
    ITaskFolder_Release(rootFolder);
    ITaskService_Release(taskService);

    return status;
}

// Public entry points

/**
 * Enumerates every startup entry into the list. Items are appended in a stable
 * order and numbered so the unsorted view is deterministic.
 */
// Packaged (Store/MSIX) app startup tasks
//
// Windows keeps the per-user state of every manifest <uap5:StartupTask> under
// HKCU\Software\Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\
// AppModel\SystemAppData\<PackageFamilyName>\<TaskId>, value "State":
// 0 Disabled, 1 Enabled, 2 DisabledByUser, 3 DisabledByPolicy, 4 EnabledByPolicy.
// The only supported way to change it is StartupTask.RequestEnableAsync from
// inside the owning package, so these rows are read-only here.

static CONST PH_STRINGREF SmPackagedRootKey = PH_STRINGREF_INIT(L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\SystemAppData");
static CONST PH_STRINGREF SmPackagedSchemasKey = PH_STRINGREF_INIT(L"Schemas");

typedef struct _SM_PACKAGED_ENUM_CONTEXT
{
    PPH_LIST List;
    PPH_STRING FamilyName;
    PPH_STRING FullName; // may be NULL
} SM_PACKAGED_ENUM_CONTEXT, *PSM_PACKAGED_ENUM_CONTEXT;

static BOOLEAN SmQueryDwordValue(
    _In_ HANDLE KeyHandle,
    _In_ PCWSTR ValueName,
    _Out_ PULONG Value
    )
{
    PKEY_VALUE_PARTIAL_INFORMATION information;
    BOOLEAN result = FALSE;

    if (NT_SUCCESS(PhQueryValueKeyZ(KeyHandle, ValueName, KeyValuePartialInformation, &information)))
    {
        if (information->Type == REG_DWORD && information->DataLength >= sizeof(ULONG))
        {
            *Value = *(PULONG)information->Data;
            result = TRUE;
        }

        PhFree(information);
    }

    return result;
}

static BOOLEAN NTAPI SmEnumPackagedTaskCallback(
    _In_ HANDLE RootDirectory,
    _In_ PVOID Information,
    _In_opt_ PVOID Context
    )
{
    PKEY_BASIC_INFORMATION information = Information;
    PSM_PACKAGED_ENUM_CONTEXT context = Context;
    PH_STRINGREF taskId;
    HANDLE keyHandle;
    ULONG state;

    if (!context)
        return FALSE;

    taskId.Buffer = information->Name;
    taskId.Length = information->NameLength;

    if (!NT_SUCCESS(PhOpenKey(&keyHandle, KEY_READ, RootDirectory, &taskId, 0)))
        return TRUE;

    // Only subkeys carrying a State value are startup tasks (HAM, PSR, Schemas, ... have none).
    if (SmQueryDwordValue(keyHandle, L"State", &state))
    {
        PSM_STARTUP_ITEM item;
        PH_STRINGREF packageName;
        PH_STRINGREF remainder;

        item = SmCreateStartupItem(SmSourcePackagedApp);
        item->CanToggle = FALSE;
        item->Enabled = state == 1 || state == 4;

        if (!PhSplitStringRefAtChar(&context->FamilyName->sr, L'_', &packageName, &remainder))
            packageName = context->FamilyName->sr;

        {
            PPH_STRING taskText = PhCreateString2(&taskId);
            PPH_STRING packageText = PhCreateString2(&packageName);

            item->Name = PhFormatString(L"%s (%s)", taskText->Buffer, packageText->Buffer);
            PhDereferenceObject(taskText);
            PhDereferenceObject(packageText);
        }

        item->Command = context->FullName ?
            PhFormatString(L"Packaged app: %s", context->FullName->Buffer) :
            PhFormatString(L"Packaged app: %s", context->FamilyName->Buffer);
        item->Location = PhFormatString(L"HKCU\\%s\\%s",
            SmPackagedRootKey.Buffer, context->FamilyName->Buffer);
        item->ObjectPath = PhReferenceObject(item->Location);

        PhAddItemList(context->List, item);
    }

    NtClose(keyHandle);

    return TRUE;
}

static BOOLEAN NTAPI SmEnumPackagedFamilyCallback(
    _In_ HANDLE RootDirectory,
    _In_ PVOID Information,
    _In_opt_ PVOID Context
    )
{
    PKEY_BASIC_INFORMATION information = Information;
    PSM_PACKAGED_ENUM_CONTEXT context = Context;
    PH_STRINGREF familyName;
    HANDLE keyHandle;
    HANDLE schemasHandle;

    familyName.Buffer = information->Name;
    familyName.Length = information->NameLength;

    if (!NT_SUCCESS(PhOpenKey(&keyHandle, KEY_READ, RootDirectory, &familyName, 0)))
        return TRUE;

    context->FamilyName = PhCreateString2(&familyName);
    context->FullName = NULL;

    if (NT_SUCCESS(PhOpenKey(&schemasHandle, KEY_READ, keyHandle, &SmPackagedSchemasKey, 0)))
    {
        PKEY_VALUE_PARTIAL_INFORMATION value;

        if (NT_SUCCESS(PhQueryValueKeyZ(schemasHandle, L"PackageFullName", KeyValuePartialInformation, &value)))
        {
            if (value->Type == REG_SZ && value->DataLength >= sizeof(WCHAR))
            {
                PH_STRINGREF text;

                text.Buffer = (PWSTR)value->Data;
                text.Length = value->DataLength;

                while (text.Length >= sizeof(WCHAR) && text.Buffer[text.Length / sizeof(WCHAR) - 1] == UNICODE_NULL)
                    text.Length -= sizeof(WCHAR);

                context->FullName = PhCreateString2(&text);
            }

            PhFree(value);
        }

        NtClose(schemasHandle);
    }

    PhEnumerateKey(keyHandle, KeyBasicInformation, SmEnumPackagedTaskCallback, context);

    PhClearReference(&context->FamilyName);
    PhClearReference(&context->FullName);
    NtClose(keyHandle);

    return TRUE;
}

static VOID SmEnumeratePackagedTasks(
    _Inout_ PPH_LIST List
    )
{
    HANDLE keyHandle;
    SM_PACKAGED_ENUM_CONTEXT context;

    if (!NT_SUCCESS(PhOpenKey(&keyHandle, KEY_READ, PH_KEY_CURRENT_USER, &SmPackagedRootKey, 0)))
        return;

    memset(&context, 0, sizeof(context));
    context.List = List;

    PhEnumerateKey(keyHandle, KeyBasicInformation, SmEnumPackagedFamilyCallback, &context);

    NtClose(keyHandle);
}

VOID SmEnumerateStartupItems(
    _Inout_ PPH_LIST List
    )
{
    for (ULONG i = 0; i < RTL_NUMBER_OF(SmRegistrySources); i++)
    {
        SmEnumerateRegistrySource(List, &SmRegistrySources[i]);
    }

    SmEnumerateStartupFolder(List, SmSourceStartupFolderUser, &SmFolderIdStartup);
    SmEnumerateStartupFolder(List, SmSourceStartupFolderCommon, &SmFolderIdCommonStartup);

    SmEnumerateScheduledTasks(List);
    SmEnumeratePackagedTasks(List);

    for (ULONG i = 0; i < List->Count; i++)
    {
        PSM_STARTUP_ITEM item = List->Items[i];

        item->Index = i;
    }
}

NTSTATUS SmSetStartupItemEnabled(
    _In_ PSM_STARTUP_ITEM Item,
    _In_ BOOLEAN Enable
    )
{
    if (!Item->CanToggle)
        return STATUS_NOT_SUPPORTED;

    if (Item->Source == SmSourceScheduledTask)
    {
        HRESULT status;

        status = SmSetScheduledTaskEnabled(Item, Enable);

        if (FAILED(status))
            return PhDosErrorToNtStatus(HRESULT_CODE(status));

        return STATUS_SUCCESS;
    }

    return SmSetApprovedState(
        SmGetApprovedRootKey(Item),
        &Item->ApprovedKeyName,
        &Item->ApprovedValueName->sr,
        Enable
        );
}

NTSTATUS SmDeleteStartupItem(
    _In_ PSM_STARTUP_ITEM Item
    )
{
    NTSTATUS status;

    switch (Item->Source)
    {
    case SmSourceRegistryHklm:
    case SmSourceRegistryHkcu:
        {
            HANDLE keyHandle;

            status = PhOpenKey(
                &keyHandle,
                KEY_SET_VALUE,
                Item->Source == SmSourceRegistryHklm ? PH_KEY_LOCAL_MACHINE : PH_KEY_CURRENT_USER,
                &Item->ObjectPath->sr,
                0
                );

            if (NT_SUCCESS(status))
            {
                status = PhDeleteValueKey(keyHandle, &Item->Name->sr);
                NtClose(keyHandle);
            }
        }
        break;
    case SmSourceStartupFolderUser:
    case SmSourceStartupFolderCommon:
        {
            status = PhDeleteFileWin32(Item->ObjectPath->Buffer);
        }
        break;
    case SmSourceScheduledTask:
        {
            HRESULT result;

            result = SmDeleteScheduledTask(Item);

            if (FAILED(result))
                status = PhDosErrorToNtStatus(HRESULT_CODE(result));
            else
                status = STATUS_SUCCESS;
        }
        break;
    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    // Remove any stale StartupApproved value so a re-added entry starts enabled.
    if (NT_SUCCESS(status) && Item->CanToggle && Item->Source != SmSourceScheduledTask)
    {
        HANDLE keyHandle;
        PPH_STRING keyPath;

        keyPath = PhConcatStringRef2(&SmStartupApprovedKeyName, &Item->ApprovedKeyName);

        if (NT_SUCCESS(PhOpenKey(&keyHandle, KEY_SET_VALUE, SmGetApprovedRootKey(Item), &keyPath->sr, 0)))
        {
            PhDeleteValueKey(keyHandle, &Item->ApprovedValueName->sr);
            NtClose(keyHandle);
        }

        PhDereferenceObject(keyPath);
    }

    return status;
}
