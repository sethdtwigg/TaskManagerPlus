/*
 * TaskManagerPlus: one formatter for the metric columns (memory, disk, network).
 *
 * Fixed units so a column never changes unit from row to row: memory in MB (GB from 10 GB), disk rates in
 * MB/s, network rates in Mbps, one decimal, thousands separators ("1,773.5 MB", "0.1 MB/s", "0.2 Mbps").
 * Each helper fills two PH_FORMAT entries (number, unit). A copy of this header lives in
 * plugins/ExtendedTools; keep both identical.
 */

#ifndef PH_METRICFMT_H
#define PH_METRICFMT_H

FORCEINLINE VOID PhMetricInitBytes(
    _Out_writes_(2) PPH_FORMAT Format,
    _In_ ULONG64 Bytes
    )
{
    DOUBLE mb = (DOUBLE)Bytes / 1048576.0;
    PCWSTR unit = L" MB";

    if (mb >= 10240.0)
    {
        mb /= 1024.0;
        unit = L" GB";
    }

    PhInitFormatF(&Format[0], (FLOAT)mb, 1);
    Format[0].Type |= FormatGroupDigits;
    PhInitFormatS(&Format[1], unit);
}

FORCEINLINE VOID PhMetricInitByteRate(
    _Out_writes_(2) PPH_FORMAT Format,
    _In_ ULONG64 BytesPerSecond
    )
{
    DOUBLE mb = (DOUBLE)BytesPerSecond / 1048576.0;

    PhInitFormatF(&Format[0], (FLOAT)mb, mb < 0.05 ? 0 : 1);
    Format[0].Type |= FormatGroupDigits;
    PhInitFormatS(&Format[1], L" MB/s");
}

FORCEINLINE VOID PhMetricInitBitRate(
    _Out_writes_(2) PPH_FORMAT Format,
    _In_ ULONG64 BytesPerSecond
    )
{
    DOUBLE mbps = (DOUBLE)BytesPerSecond * 8.0 / 1000000.0;

    PhInitFormatF(&Format[0], (FLOAT)mbps, mbps < 0.05 ? 0 : 1);
    Format[0].Type |= FormatGroupDigits;
    PhInitFormatS(&Format[1], L" Mbps");
}

#endif
