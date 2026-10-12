/*
 * kernel_rtl.c - Xbox Runtime Library Functions
 *
 * Implements Rtl* functions: critical sections, string init/conversion,
 * NTSTATUS→Win32 error mapping, time conversion, sprintf variants.
 *
 * Most of these map 1:1 to Win32 CRT functions.
 */

#include "kernel.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ============================================================================
 * String Initialization
 * ============================================================================ */

VOID __stdcall xbox_RtlInitAnsiString(PXBOX_ANSI_STRING DestinationString, const char* SourceString)
{
    if (SourceString) {
        USHORT len = (USHORT)strlen(SourceString);
        DestinationString->Length = len;
        DestinationString->MaximumLength = len + 1;
        DestinationString->Buffer = (PCHAR)SourceString;
    } else {
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
        DestinationString->Buffer = NULL;
    }
}

VOID __stdcall xbox_RtlInitUnicodeString(PXBOX_UNICODE_STRING DestinationString, const WCHAR* SourceString)
{
    if (SourceString) {
        USHORT len = (USHORT)(wcslen(SourceString) * sizeof(WCHAR));
        DestinationString->Length = len;
        DestinationString->MaximumLength = len + sizeof(WCHAR);
        DestinationString->Buffer = (PWCHAR)SourceString;
    } else {
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
        DestinationString->Buffer = NULL;
    }
}

/* ============================================================================
 * String Conversion (ANSI ↔ Unicode)
 * ============================================================================ */

NTSTATUS __stdcall xbox_RtlAnsiStringToUnicodeString(
    PXBOX_UNICODE_STRING DestinationString,
    PXBOX_ANSI_STRING SourceString,
    BOOLEAN AllocateDestinationString)
{
    ULONG unicode_len;

    if (!DestinationString || !SourceString)
        return STATUS_INVALID_PARAMETER;

    unicode_len = (SourceString->Length + 1) * sizeof(WCHAR);

    if (AllocateDestinationString) {
        DestinationString->Buffer = (PWCHAR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, unicode_len);
        if (!DestinationString->Buffer)
            return STATUS_NO_MEMORY;
        DestinationString->MaximumLength = (USHORT)unicode_len;
    } else if (DestinationString->MaximumLength < unicode_len) {
        return STATUS_BUFFER_OVERFLOW;
    }

    int result = MultiByteToWideChar(CP_ACP, 0,
        SourceString->Buffer, SourceString->Length,
        DestinationString->Buffer, DestinationString->MaximumLength / sizeof(WCHAR));

    if (result > 0) {
        DestinationString->Length = (USHORT)(result * sizeof(WCHAR));
        DestinationString->Buffer[result] = L'\0';
        return STATUS_SUCCESS;
    }

    return STATUS_UNSUCCESSFUL;
}

NTSTATUS __stdcall xbox_RtlUnicodeStringToAnsiString(
    PXBOX_ANSI_STRING DestinationString,
    PXBOX_UNICODE_STRING SourceString,
    BOOLEAN AllocateDestinationString)
{
    ULONG ansi_len;

    if (!DestinationString || !SourceString)
        return STATUS_INVALID_PARAMETER;

    ansi_len = SourceString->Length / sizeof(WCHAR) + 1;

    if (AllocateDestinationString) {
        DestinationString->Buffer = (PCHAR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ansi_len);
        if (!DestinationString->Buffer)
            return STATUS_NO_MEMORY;
        DestinationString->MaximumLength = (USHORT)ansi_len;
    } else if (DestinationString->MaximumLength < ansi_len) {
        return STATUS_BUFFER_OVERFLOW;
    }

    int result = WideCharToMultiByte(CP_ACP, 0,
        SourceString->Buffer, SourceString->Length / sizeof(WCHAR),
        DestinationString->Buffer, DestinationString->MaximumLength,
        NULL, NULL);

    if (result >= 0) {
        DestinationString->Length = (USHORT)result;
        if ((USHORT)result < DestinationString->MaximumLength)
            DestinationString->Buffer[result] = '\0';
        return STATUS_SUCCESS;
    }

    return STATUS_UNSUCCESSFUL;
}

/* ============================================================================
 * String Comparison
 * ============================================================================ */

BOOLEAN __stdcall xbox_RtlEqualString(
    PXBOX_ANSI_STRING String1,
    PXBOX_ANSI_STRING String2,
    BOOLEAN CaseInSensitive)
{
    if (String1->Length != String2->Length)
        return FALSE;

    if (CaseInSensitive)
        return _strnicmp(String1->Buffer, String2->Buffer, String1->Length) == 0;
    else
        return strncmp(String1->Buffer, String2->Buffer, String1->Length) == 0;
}

/*
 * RtlCompareMemoryUlong - Scans memory for a ULONG pattern.
 * Returns the number of bytes that matched.
 */
ULONG __stdcall xbox_RtlCompareMemoryUlong(PVOID Source, ULONG Length, ULONG Pattern)
{
    PULONG src = (PULONG)Source;
    ULONG count = Length / sizeof(ULONG);

    for (ULONG i = 0; i < count; i++) {
        if (src[i] != Pattern)
            return i * sizeof(ULONG);
    }
    return count * sizeof(ULONG);
}

/* ============================================================================
 * Critical Sections (direct 1:1 mapping)
 * ============================================================================ */

/*
 * Critical section operations are no-ops for now.
 *
 * The Xbox CRITICAL_SECTION is a 20-byte 32-bit structure that's
 * incompatible with the Windows 64-bit CRITICAL_SECTION (40 bytes).
 * Passing Xbox memory pointers to native Windows CS functions would
 * corrupt memory. Since the recompiled game runs single-threaded
 * (all Xbox threads are called synchronously), there's no contention
 * and no-ops are correct.
 *
 * TODO: If multithreading is needed, implement a shadow CS mapping
 * (Xbox VA → native Windows CRITICAL_SECTION).
 */
/*
 * Shadow mapping: each guest critical section address gets a reentrant host
 * lock, created on first use (titles also enter statically zeroed sections
 * they never initialised). Runtimes that give guest threads real host threads
 * (PsCreateSystemThreadEx) need it: with no-ops, Godzilla: Save the Earth's
 * loader thread and its main thread edited the same stream array at once and
 * crashed on half-built entries.
 *
 * Do not use a native CRITICAL_SECTION here. Xbox worker threads can return
 * through incomplete translated unwind paths while holding a guest lock. A
 * Win32 critical section is then abandoned forever, unlike a kernel mutex,
 * and the window-owning thread deadlocks on its next Enter. Keep the owner as
 * a host thread id instead: a waiter can detect that the owner no longer
 * exists, reclaim the guest lock, and let the title continue.
 */
typedef struct {
    void *key;
    volatile LONG owner;
    volatile LONG depth;
} ShadowCS;

#define SHADOW_CS_BUCKETS 8192
static ShadowCS *g_shadow_cs[SHADOW_CS_BUCKETS];
static SRWLOCK g_shadow_cs_lock = SRWLOCK_INIT;

static ShadowCS *shadow_cs(void *key)
{
    uintptr_t h = ((uintptr_t)key >> 3) * 2654435761u;
    unsigned i = (unsigned)(h % SHADOW_CS_BUCKETS);
    ShadowCS *s;
    AcquireSRWLockShared(&g_shadow_cs_lock);
    for (unsigned n = 0; n < SHADOW_CS_BUCKETS; ++n) {
        s = g_shadow_cs[(i + n) % SHADOW_CS_BUCKETS];
        if (!s) break;
        if (s->key == key) { ReleaseSRWLockShared(&g_shadow_cs_lock); return s; }
    }
    ReleaseSRWLockShared(&g_shadow_cs_lock);
    AcquireSRWLockExclusive(&g_shadow_cs_lock);
    for (unsigned n = 0; n < SHADOW_CS_BUCKETS; ++n) {
        ShadowCS **slot = &g_shadow_cs[(i + n) % SHADOW_CS_BUCKETS];
        if (*slot && (*slot)->key == key) { s = *slot; ReleaseSRWLockExclusive(&g_shadow_cs_lock); return s; }
        if (!*slot) {
            s = (ShadowCS *)calloc(1, sizeof(*s));
            if (s) {
                s->key = key;
                *slot = s;
            }
            ReleaseSRWLockExclusive(&g_shadow_cs_lock);
            return s;
        }
    }
    ReleaseSRWLockExclusive(&g_shadow_cs_lock);
    return NULL;   /* table full: degrade to unsynchronised */
}

VOID __stdcall xbox_RtlEnterCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)
{
    ShadowCS *s = shadow_cs((void *)CriticalSection);
    const LONG thread_id = (LONG)GetCurrentThreadId();
    if (!s) return;
    for (;;) {
        LONG owner = InterlockedCompareExchange(&s->owner, 0, 0);
        if (owner == thread_id) {
            InterlockedIncrement(&s->depth);
            return;
        }
        if (owner == 0 &&
            InterlockedCompareExchange(&s->owner, thread_id, 0) == 0) {
            InterlockedExchange(&s->depth, 1);
            return;
        }
        if (owner != 0) {
            HANDLE owner_thread = OpenThread(SYNCHRONIZE, FALSE, (DWORD)owner);
            BOOL abandoned = owner_thread == NULL &&
                             GetLastError() == ERROR_INVALID_PARAMETER;
            if (owner_thread) {
                abandoned = WaitForSingleObject(owner_thread, 0) == WAIT_OBJECT_0;
                CloseHandle(owner_thread);
            }
            if (abandoned &&
                InterlockedCompareExchange(&s->owner, 0, owner) == owner) {
                InterlockedExchange(&s->depth, 0);
                fprintf(stderr,
                        "[RTL-CS] recovered abandoned guest lock=%p owner=%lu\n",
                        CriticalSection, (unsigned long)(DWORD)owner);
                fflush(stderr);
                continue;
            }
        }
        if (!SwitchToThread()) Sleep(1);
    }
}

VOID __stdcall xbox_RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)
{
    ShadowCS *s = shadow_cs((void *)CriticalSection);
    LONG depth;
    if (!s || s->owner != (LONG)GetCurrentThreadId() || s->depth <= 0) return;
    depth = InterlockedDecrement(&s->depth);
    if (depth <= 0) {
        InterlockedExchange(&s->depth, 0);
        InterlockedExchange(&s->owner, 0);
    }
}

VOID __stdcall xbox_RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)
{
    /* the shadow lock is created on first use; re-initialising a section
     * that is in use must not reset the host lock under its owner */
    (void)shadow_cs((void *)CriticalSection);
}

/* ============================================================================
 * NTSTATUS → Win32 Error Code Mapping
 * ============================================================================ */

ULONG __stdcall xbox_RtlNtStatusToDosError(NTSTATUS Status)
{
    switch (Status) {
        case STATUS_SUCCESS:                    return ERROR_SUCCESS;
        case STATUS_INVALID_PARAMETER:          return ERROR_INVALID_PARAMETER;
        case STATUS_NO_MEMORY:                  return ERROR_NOT_ENOUGH_MEMORY;
        case STATUS_INSUFFICIENT_RESOURCES:     return ERROR_NO_SYSTEM_RESOURCES;
        case STATUS_ACCESS_DENIED:              return ERROR_ACCESS_DENIED;
        case STATUS_OBJECT_NAME_NOT_FOUND:      return ERROR_FILE_NOT_FOUND;
        case STATUS_OBJECT_PATH_NOT_FOUND:      return ERROR_PATH_NOT_FOUND;
        case STATUS_OBJECT_NAME_COLLISION:      return ERROR_ALREADY_EXISTS;
        case STATUS_NO_SUCH_FILE:               return ERROR_FILE_NOT_FOUND;
        case STATUS_END_OF_FILE:                return ERROR_HANDLE_EOF;
        case STATUS_INVALID_HANDLE:             return ERROR_INVALID_HANDLE;
        case STATUS_NOT_IMPLEMENTED:            return ERROR_CALL_NOT_IMPLEMENTED;
        case STATUS_UNSUCCESSFUL:               return ERROR_GEN_FAILURE;
        case STATUS_PENDING:                    return ERROR_IO_PENDING;
        case STATUS_BUFFER_OVERFLOW:            return ERROR_MORE_DATA;
        case STATUS_NO_MORE_FILES:              return ERROR_NO_MORE_FILES;
        case STATUS_NOT_SUPPORTED:              return ERROR_NOT_SUPPORTED;
        case STATUS_CANCELLED:                  return ERROR_CANCELLED;
        case STATUS_ALREADY_COMMITTED:          return ERROR_COMMITMENT_LIMIT;
        default:
            /* Fall back to RtlNtStatusToDosError from ntdll if available */
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_RTL,
                "RtlNtStatusToDosError: unmapped status 0x%08X", Status);
            return ERROR_MR_MID_NOT_FOUND;
    }
}

/* ============================================================================
 * Time Conversion
 * ============================================================================ */

BOOLEAN __stdcall xbox_RtlTimeFieldsToTime(PXBOX_TIME_FIELDS TimeFields, PLARGE_INTEGER Time)
{
    SYSTEMTIME st;
    FILETIME ft;

    st.wYear         = (WORD)TimeFields->Year;
    st.wMonth        = (WORD)TimeFields->Month;
    st.wDayOfWeek    = (WORD)TimeFields->Weekday;
    st.wDay          = (WORD)TimeFields->Day;
    st.wHour         = (WORD)TimeFields->Hour;
    st.wMinute       = (WORD)TimeFields->Minute;
    st.wSecond       = (WORD)TimeFields->Second;
    st.wMilliseconds = (WORD)TimeFields->Milliseconds;

    if (!SystemTimeToFileTime(&st, &ft))
        return FALSE;

    Time->LowPart  = ft.dwLowDateTime;
    Time->HighPart = ft.dwHighDateTime;
    return TRUE;
}

VOID __stdcall xbox_RtlTimeToTimeFields(PLARGE_INTEGER Time, PXBOX_TIME_FIELDS TimeFields)
{
    FILETIME ft;
    SYSTEMTIME st;

    ft.dwLowDateTime  = Time->LowPart;
    ft.dwHighDateTime = Time->HighPart;

    if (FileTimeToSystemTime(&ft, &st)) {
        TimeFields->Year         = (SHORT)st.wYear;
        TimeFields->Month        = (SHORT)st.wMonth;
        TimeFields->Day          = (SHORT)st.wDay;
        TimeFields->Hour         = (SHORT)st.wHour;
        TimeFields->Minute       = (SHORT)st.wMinute;
        TimeFields->Second       = (SHORT)st.wSecond;
        TimeFields->Milliseconds = (SHORT)st.wMilliseconds;
        TimeFields->Weekday      = (SHORT)st.wDayOfWeek;
    } else {
        memset(TimeFields, 0, sizeof(XBOX_TIME_FIELDS));
    }
}

/* ============================================================================
 * Exception Handling
 * ============================================================================ */

VOID __stdcall xbox_RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PVOID ExceptionRecord, PVOID ReturnValue)
{
    /* Delegate to Win32 RtlUnwind */
    RtlUnwind(TargetFrame, TargetIp, (PEXCEPTION_RECORD)ExceptionRecord, ReturnValue);
}

VOID __stdcall xbox_RtlRaiseException(PVOID ExceptionRecord)
{
    RaiseException(
        ((PEXCEPTION_RECORD)ExceptionRecord)->ExceptionCode,
        ((PEXCEPTION_RECORD)ExceptionRecord)->ExceptionFlags,
        ((PEXCEPTION_RECORD)ExceptionRecord)->NumberParameters,
        ((PEXCEPTION_RECORD)ExceptionRecord)->ExceptionInformation);
}

VOID __stdcall xbox_RtlRip(PCHAR ApiName, PCHAR Expression, PCHAR Message)
{
    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_RTL, "RtlRip: %s - %s: %s",
        ApiName ? ApiName : "?",
        Expression ? Expression : "?",
        Message ? Message : "?");

#ifdef _DEBUG
    DebugBreak();
#endif
}

/* ============================================================================
 * String Formatting (Rtl sprintf variants → CRT)
 * ============================================================================ */

int __cdecl xbox_RtlSnprintf(char* buffer, size_t count, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsnprintf(buffer, count, format, args);
    va_end(args);
    return result;
}

int __cdecl xbox_RtlSprintf(char* buffer, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsprintf(buffer, format, args);
    va_end(args);
    return result;
}

int __cdecl xbox_RtlVsnprintf(char* buffer, size_t count, const char* format, va_list argptr)
{
    return vsnprintf(buffer, count, format, argptr);
}

int __cdecl xbox_RtlVsprintf(char* buffer, const char* format, va_list argptr)
{
    return vsprintf(buffer, format, argptr);
}
