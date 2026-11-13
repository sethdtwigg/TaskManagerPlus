/*
 * TaskManagerPlus ProcessRules: share codes. A share code is a short text a user can paste into a chat or a forum:
 *
 *     TMPR1:<base64 of the rules JSON (UTF-8)>.<8 hex digits CRC-32 of that JSON>
 *
 * The decoder ignores whitespace and line breaks (a code copied from a wrapped message), requires the prefix, the base64
 * alphabet and a matching checksum (so a truncated or edited code is rejected instead of half-imported), and refuses
 * payloads over ScMaxPayload bytes.
 *
 * Pure C (no System Informer headers) so tools\tests\unit can compile it on its own.
 */

#ifndef _SC_SHARECODE_H_
#define _SC_SHARECODE_H_

#include <stdint.h>
#include <stddef.h>

#define SC_PREFIX "TMPR1:"
#define SC_MAX_PAYLOAD (256 * 1024)

#ifdef __cplusplus
extern "C" {
#endif

uint32_t ScCrc32(const uint8_t *Data, size_t Length);

/** Bytes needed (including the terminating NUL) for a code of Length payload bytes. */
size_t ScEncodedSize(size_t Length);

/** Writes the code into Out (capacity from ScEncodedSize). Returns 1, or 0 when Out is too small or Length is over the limit. */
int ScEncode(const uint8_t *Data, size_t Length, char *Out, size_t Capacity);

/**
 * Decodes a code into a malloc'ed buffer (free with ScFree), NUL terminated for convenience (the NUL is not counted in
 * *Length). Returns 1 on success; 0 on any problem, with *Reason (static text) saying which.
 */
int ScDecode(const char *Code, size_t CodeLength, uint8_t **Data, size_t *Length, const char **Reason);

void ScFree(uint8_t *Data);

#ifdef __cplusplus
}
#endif

#endif
