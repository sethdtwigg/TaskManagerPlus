/*
 * TaskManagerPlus Profiles: tmp-profiles.json, export and import.
 *
 * File format (version 1), all strings, human editable:
 *   { "version": 1, "active": "Work",
 *     "profiles": [ { "name": "Work", "theme": "dark",
 *                     "rules": [ { "program": "chrome.exe", "enabled": "on", "priority": "belownormal" } ] } ],
 *     "previous": { "theme": "dark", "rules": [ ... ] } }
 * "enabled": on | off | keep (default keep). "priority": none | idle | belownormal | normal | abovenormal | high; absent = keep.
 * "theme": automatic | light | dark | oled; absent = keep. An exported file has "version" and "profiles" only.
 * Malformed entries are skipped, never fatal: a profile with an unknown theme name is rejected, a rule entry with an unknown
 * "enabled" or "priority" value is skipped.
 */

#include "profiles.h"

PF_STORE PrStore;

static PPH_STRING PrGetFileName(
    VOID
    )
{
    return SystemInformer_GetPluginDataFileName(PROFILES_FILE_NAME);
}

static VOID PrAddUtf16(
    _In_ PVOID Object,
    _In_ PCSTR Key,
    _In_ PCWSTR Value
    )
{
    PPH_BYTES utf8 = PhConvertUtf16ToUtf8((PWSTR)Value);

    PhAddJsonObjectUtf8(Object, Key, utf8);
    PhDereferenceObject(utf8);
}

static PVOID PrProfileToJson(
    _In_ const PF_PROFILE *Profile,
    _In_ BOOLEAN WithName
    )
{
    PVOID object = PhCreateJsonObject();
    PVOID rules = PhCreateJsonArray();
    ULONG i;

    if (WithName)
        PrAddUtf16(object, "name", Profile->Name);

    if (Profile->HasTheme && PfThemeName(Profile->Theme))
        PhAddJsonObject(object, "theme", PfThemeName(Profile->Theme));

    for (i = 0; i < Profile->RuleCount; i++)
    {
        const PF_RULE_SETTING *s = &Profile->Rules[i];
        PVOID rule = PhCreateJsonObject();

        PrAddUtf16(rule, "program", s->Program);
        PhAddJsonObject(rule, "enabled", PfEnabledModeName(s->EnabledMode));

        if (s->HasPriority && PfPriorityName(s->Priority))
            PhAddJsonObject(rule, "priority", PfPriorityName(s->Priority));

        PhAddJsonArrayObject(rules, rule);
    }

    PhAddJsonObjectValue(object, "rules", rules);
    return object;
}

static BOOLEAN PrProfileFromJson(
    _In_ PVOID Object,
    _In_opt_ PCWSTR FixedName,
    _Out_ PF_PROFILE *Profile
    )
{
    PPH_STRING name;
    PPH_STRING text;
    PVOID rules;
    ULONG count;
    ULONG i;
    PPH_BYTES utf8;

    memset(Profile, 0, sizeof(*Profile));

    if (FixedName)
    {
        wcsncpy_s(Profile->Name, PF_NAME_CHARS, FixedName, _TRUNCATE);
    }
    else
    {
        name = PhGetJsonValueAsString(Object, "name");

        if (!name)
            return FALSE;

        wcsncpy_s(Profile->Name, PF_NAME_CHARS, name->Buffer, _TRUNCATE);
        PhDereferenceObject(name);
    }

    if (!PfValidName(Profile->Name))
        return FALSE;

    if ((text = PhGetJsonValueAsString(Object, "theme")))
    {
        utf8 = PhConvertUtf16ToUtf8(text->Buffer);
        Profile->HasTheme = PfThemeFromName(utf8->Buffer, &Profile->Theme);
        PhDereferenceObject(utf8);
        PhDereferenceObject(text);

        if (!Profile->HasTheme)
            return FALSE; // an unknown theme name (typo, "custom") rejects the profile instead of silently ignoring it
    }

    if (!(rules = PhGetJsonObject(Object, "rules")))
        return TRUE;

    count = PhGetJsonArrayLength(rules);

    for (i = 0; i < count && Profile->RuleCount < PF_MAX_RULES; i++)
    {
        PVOID entry = PhGetJsonArrayIndexObject(rules, i);
        PF_RULE_SETTING *s = &Profile->Rules[Profile->RuleCount];
        PPH_STRING program;
        int valid;

        if (!entry || !(program = PhGetJsonValueAsString(entry, "program")))
            continue;

        if (!program->Length)
        {
            PhDereferenceObject(program);
            continue;
        }

        wcsncpy_s(s->Program, PF_PROGRAM_CHARS, program->Buffer, _TRUNCATE);
        PhDereferenceObject(program);
        s->EnabledMode = PF_KEEP;

        if ((text = PhGetJsonValueAsString(entry, "enabled")))
        {
            utf8 = PhConvertUtf16ToUtf8(text->Buffer);
            valid = PfEnabledModeFromName(utf8->Buffer, &s->EnabledMode);
            PhDereferenceObject(utf8);
            PhDereferenceObject(text);

            if (!valid)
                continue; // skip the rule entry
        }

        if ((text = PhGetJsonValueAsString(entry, "priority")))
        {
            utf8 = PhConvertUtf16ToUtf8(text->Buffer);
            s->HasPriority = PfPriorityFromName(utf8->Buffer, &s->Priority);
            PhDereferenceObject(utf8);
            PhDereferenceObject(text);

            if (!s->HasPriority)
                continue;
        }

        if (s->EnabledMode == PF_KEEP && !s->HasPriority)
            continue;

        Profile->RuleCount++;
    }

    return TRUE;
}

static NTSTATUS PrSaveJson(
    _In_ PPH_STRING FileName,
    _In_ PVOID Root
    )
{
    PH_STRINGREF directory;
    PH_STRINGREF baseName;
    PPH_STRING ntFileName;
    NTSTATUS status;

    if (PhSplitStringRefAtLastChar(&FileName->sr, OBJ_NAME_PATH_SEPARATOR, &directory, &baseName))
        PhCreateDirectoryFullPathWin32(&directory);

    // phlib's JSON file functions take NT paths
    if (ntFileName = PhDosPathNameToNtPathName(&FileName->sr))
    {
        status = PhSaveJsonObjectToFile(&ntFileName->sr, Root, PH_JSON_TO_STRING_PRETTY);
        PhDereferenceObject(ntFileName);
    }
    else
    {
        status = STATUS_OBJECT_PATH_SYNTAX_BAD;
    }

    return status;
}

static NTSTATUS PrLoadJson(
    _In_ PPH_STRING FileName,
    _Out_ PVOID *Root
    )
{
    PPH_STRING ntFileName;
    NTSTATUS status;

    *Root = NULL;

    if (ntFileName = PhDosPathNameToNtPathName(&FileName->sr))
    {
        status = PhLoadJsonObjectFromFile(Root, &ntFileName->sr);
        PhDereferenceObject(ntFileName);
    }
    else
    {
        status = STATUS_OBJECT_PATH_SYNTAX_BAD;
    }

    return status;
}

VOID PrSave(
    VOID
    )
{
    PPH_STRING fileName = PrGetFileName();
    PVOID root;
    PVOID profiles;
    ULONG i;

    if (!fileName)
        return; // -nosettings: stays in memory

    root = PhCreateJsonObject();
    PhAddJsonObjectUInt64(root, "version", PROFILES_FILE_VERSION);
    PrAddUtf16(root, "active", PrStore.Active);
    profiles = PhCreateJsonArray();

    for (i = 0; i < PrStore.Count; i++)
        PhAddJsonArrayObject(profiles, PrProfileToJson(PrStore.Profiles[i], TRUE));

    PhAddJsonObjectValue(root, "profiles", profiles);

    if (PrStore.Previous)
        PhAddJsonObjectValue(root, "previous", PrProfileToJson(PrStore.Previous, FALSE));

    PrSaveJson(fileName, root);
    PhFreeJsonObject(root);
    PhDereferenceObject(fileName);
}

static VOID PrReadProfiles(
    _In_ PVOID Root,
    _In_ BOOLEAN Replace,
    _Out_opt_ PULONG Imported,
    _Out_opt_ PULONG Skipped
    )
{
    PVOID profiles;
    ULONG count;
    ULONG i;
    PF_PROFILE *profile = PfAllocProfile();
    ULONG imported = 0;
    ULONG skipped = 0;

    if (Replace)
    {
        for (i = 0; i < PrStore.Count; i++)
            PfFreeProfile(PrStore.Profiles[i]);

        PrStore.Count = 0;
    }

    if (profile && (profiles = PhGetJsonObject(Root, "profiles")))
    {
        count = PhGetJsonArrayLength(profiles);

        for (i = 0; i < count; i++)
        {
            PVOID entry = PhGetJsonArrayIndexObject(profiles, i);

            if (entry && PrProfileFromJson(entry, NULL, profile) && PfStoreSet(&PrStore, profile) == 0)
                imported++;
            else
                skipped++;
        }
    }

    PfFreeProfile(profile);

    if (Imported)
        *Imported = imported;

    if (Skipped)
        *Skipped = skipped;
}

VOID PrLoad(
    VOID
    )
{
    PPH_STRING fileName = PrGetFileName();
    PVOID root;
    PPH_STRING active;
    PVOID previous;

    PfStoreInit(&PrStore);

    if (!fileName)
        return;

    if (NT_SUCCESS(PrLoadJson(fileName, &root)))
    {
        PrReadProfiles(root, TRUE, NULL, NULL);

        if ((active = PhGetJsonValueAsString(root, "active")))
        {
            if (PfStoreFind(&PrStore, active->Buffer) >= 0)
                wcsncpy_s(PrStore.Active, PF_NAME_CHARS, active->Buffer, _TRUNCATE);

            PhDereferenceObject(active);
        }

        if ((previous = PhGetJsonObject(root, "previous")))
        {
            PF_PROFILE *snapshot = PfAllocProfile();

            if (snapshot && PrProfileFromJson(previous, L"previous", snapshot))
                PrStore.Previous = snapshot;
            else
                PfFreeProfile(snapshot);
        }

        PhFreeJsonObject(root);
    }

    PhDereferenceObject(fileName);
}

NTSTATUS PrExportToFile(
    _In_ PPH_STRING FileName,
    _In_opt_ PCWSTR OnlyProfile
    )
{
    PVOID root = PhCreateJsonObject();
    PVOID profiles = PhCreateJsonArray();
    ULONG i;
    NTSTATUS status;

    PhAddJsonObjectUInt64(root, "version", PROFILES_FILE_VERSION);

    for (i = 0; i < PrStore.Count; i++)
    {
        if (OnlyProfile && _wcsicmp(PrStore.Profiles[i]->Name, OnlyProfile))
            continue;

        PhAddJsonArrayObject(profiles, PrProfileToJson(PrStore.Profiles[i], TRUE));
    }

    PhAddJsonObjectValue(root, "profiles", profiles);
    status = PrSaveJson(FileName, root);
    PhFreeJsonObject(root);
    return status;
}

NTSTATUS PrImportFromFile(
    _In_ PPH_STRING FileName,
    _Out_ PULONG Imported,
    _Out_ PULONG Skipped
    )
{
    PVOID root;
    NTSTATUS status;

    *Imported = 0;
    *Skipped = 0;
    status = PrLoadJson(FileName, &root);

    if (!NT_SUCCESS(status))
        return status;

    PrReadProfiles(root, FALSE, Imported, Skipped);
    PhFreeJsonObject(root);

    if (*Imported)
        PrSave();

    return STATUS_SUCCESS;
}
