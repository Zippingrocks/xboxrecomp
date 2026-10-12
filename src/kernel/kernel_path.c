/*
 * kernel_path.c - Xbox device-path translation
 *
 * Translates Xbox device-style paths to host filesystem paths:
 *   \Device\CdRom0\  -> <game_dir>/
 *   D:\               -> <game_dir>/
 *   T:\               -> <save_dir>/TitleData/
 *   U:\               -> <save_dir>/UserData/
 *   Z:\               -> <save_dir>/Cache/
 *
 * The Win32 build emits UTF-16 paths (for CreateFileW); the Linux build
 * emits UTF-8 paths with '/' separators (for open()).
 */

#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/*
 * Helper: check if an ANSI string starts with a prefix (case-insensitive).
 * Returns the number of chars consumed from the prefix, or 0 if no match.
 * Platform-independent.
 */
static int match_prefix(const char* path, const char* prefix)
{
    int i = 0;
    while (prefix[i]) {
        if (tolower((unsigned char)path[i]) != tolower((unsigned char)prefix[i]))
            return 0;
        i++;
    }
    return i;
}

static int path_equals(const char* path, const char* expected)
{
    int length = match_prefix(path, expected);
    return length != 0 && path[length] == '\0';
}

/* Guest ANSI strings must never be handed to host CRT/Win32 routines without
 * a bound.  Xbox RAM is mapped as one large host region, so a bogus pointer
 * can remain readable for hundreds of megabytes; an unterminated path then
 * looks like a permanent CPU spin instead of producing a useful fault. */
static BOOL path_is_bounded(const char* path)
{
    if (memchr(path, '\0', MAX_PATH) != NULL)
        return TRUE;

    fprintf(stderr,
            "  [PATH-FAIL] unterminated guest path at %p (no NUL in %u bytes)\n",
            (const void*)path, (unsigned)MAX_PATH);
    fflush(stderr);
    return FALSE;
}

/* A device-path translation rule, shared by both backends. */
typedef struct {
    const char* prefix;     /* Xbox path prefix (backslash form)         */
    int         to_save;    /* 1 = under save_dir, 0 = under game_dir    */
    const char* sub_win;    /* sub-directory, Win32 backslash form       */
    const char* sub_posix;  /* sub-directory, POSIX slash form           */
} path_rule;

static const path_rule s_rules[] = {
    { "\\Device\\CdRom0\\",                   0, NULL,         NULL          },
    { "\\Device\\Harddisk0\\Partition1\\",    0, NULL,         NULL          },
    { "D:\\",                                 0, NULL,         NULL          },
    { "d:\\",                                 0, NULL,         NULL          },
    /* Y: is the Xbox dashboard partition; the dashboard opens its assets
     * (e.g. "Y:\default.xip") from there. Map it to the game dir. */
    { "Y:\\",                                 0, NULL,         NULL          },
    { "y:\\",                                 0, NULL,         NULL          },
    /* Some XDK libraries open the drive object itself before accessing any
     * child path.  Keep these root-form aliases ahead of their slash-form
     * rules so bare T: resolves to the same title-data directory. */
    { "T:",                                   1, "\\TitleData","/TitleData"  },
    { "T:\\",                                 1, "\\TitleData","/TitleData"  },
    { "U:\\",                                 1, "\\UserData", "/UserData"   },
    { "Z:\\",                                 1, "\\Cache",    "/Cache"      },
    { "\\??\\D:\\",                           0, NULL,         NULL          },
    { "\\??\\Y:\\",                           0, NULL,         NULL          },
    { "\\??\\y:\\",                           0, NULL,         NULL          },
    { "\\??\\T:",                            1, "\\TitleData","/TitleData"  },
    { "\\??\\T:\\",                           1, "\\TitleData","/TitleData"  },
};
#define PATH_RULE_COUNT ((int)(sizeof(s_rules) / sizeof(s_rules[0])))

/* ======================================================================== */
#if defined(_WIN32)
/* ======================================================================== */

#include <shlobj.h>

static WCHAR s_game_dir[MAX_PATH];
static WCHAR s_save_dir[MAX_PATH];
static BOOL  s_initialized = FALSE;

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    WCHAR save_base[MAX_PATH];

    /* The fallbacks used to name Burnout 3 specifically, so any other title
     * that passed NULL silently pointed its game dir and its saves at another
     * game's folders. Generic now -- a caller that wants a title-specific
     * location should pass one. */
    if (game_dir) {
        MultiByteToWideChar(CP_UTF8, 0, game_dir, -1, s_game_dir, MAX_PATH);
    } else {
        GetCurrentDirectoryW(MAX_PATH, s_game_dir);
    }

    if (save_dir) {
        MultiByteToWideChar(CP_UTF8, 0, save_dir, -1, s_save_dir, MAX_PATH);
    } else {
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, save_base))) {
            swprintf_s(s_save_dir, MAX_PATH, L"%s\\xboxrecomp", save_base);
        } else {
            GetCurrentDirectoryW(MAX_PATH, s_save_dir);
            wcscat_s(s_save_dir, MAX_PATH, L"\\SaveData");
        }
    }

    size_t len = wcslen(s_game_dir);
    if (len > 0 && s_game_dir[len - 1] == L'\\')
        s_game_dir[len - 1] = L'\0';

    len = wcslen(s_save_dir);
    if (len > 0 && s_save_dir[len - 1] == L'\\')
        s_save_dir[len - 1] = L'\0';

    /* Create the save-side directories. T:/U:/Z: map into subdirectories of
     * save_dir, and a title that opens a file there with a create disposition
     * fails if the parent does not exist -- which reads as "cannot create save
     * file" and sends the title down its init-failure path. Halo asserts
     * exactly that at saved games/game_state_xbox.c:97 and then unwinds,
     * clearing global_d3d_device on the way out, so a missing directory
     * surfaces as a graphics failure.
     *
     * Cheap and idempotent: SHCreateDirectoryExW builds intermediates and is
     * happy if they already exist. */
    {
        static const WCHAR *subs[] = { L"TitleData", L"UserData", L"Cache" };
        WCHAR dir[MAX_PATH];
        SHCreateDirectoryExW(NULL, s_save_dir, NULL);
        for (int i = 0; i < 3; i++) {
            swprintf_s(dir, MAX_PATH, L"%s\\%s", s_save_dir, subs[i]);
            SHCreateDirectoryExW(NULL, dir, NULL);
        }

        /* XNet stores its small machine configuration in sectors on the raw
         * Xbox HDD object.  Give that object a private, persistent backing
         * file.  NtOpenFile uses FILE_OPEN here, so the file must exist before
         * the first XNet startup attempt. */
        swprintf_s(dir, MAX_PATH, L"%s\\Partition0.bin", s_save_dir);
        HANDLE partition = CreateFileW(dir, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (partition != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size;
            if (GetFileSizeEx(partition, &size) && size.QuadPart < 0x2000) {
                LARGE_INTEGER wanted;
                wanted.QuadPart = 0x2000;
                SetFilePointerEx(partition, wanted, NULL, FILE_BEGIN);
                SetEndOfFile(partition);
            }
            CloseHandle(partition);
        }
    }

    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%S, save=%S", s_game_dir, s_save_dir);
}

BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char*  remainder = NULL;
    const WCHAR* base_dir  = NULL;
    const char*  sub_dir   = NULL;
    int          skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;
    if (!path_is_bounded(xbox_path))
        return FALSE;

    {
        static unsigned call_count;
        ++call_count;
        if (getenv("XBOXRECOMP_TRACE_PATH") &&
            (call_count <= 16u || (call_count & (call_count - 1u)) == 0u)) {
            fprintf(stderr, "  [PATH-CALL] #%u ptr=%p path='%s'\n",
                    call_count, (const void*)xbox_path, xbox_path);
            fflush(stderr);
        }
    }

    if (!s_initialized)
        xbox_path_init(NULL, NULL);

    if (path_equals(xbox_path, "\\Device\\Harddisk0\\partition0")) {
        if (wcslen(s_save_dir) + wcslen(L"\\Partition0.bin") + 1 > buf_size)
            return FALSE;
        swprintf_s(host_path_buf, buf_size, L"%s\\Partition0.bin", s_save_dir);
        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %S", xbox_path, host_path_buf);
        return TRUE;
    }

    for (int i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_prefix(xbox_path, s_rules[i].prefix);
        if (skip) {
            remainder = xbox_path + skip;
            base_dir  = s_rules[i].to_save ? s_save_dir : s_game_dir;
            sub_dir   = s_rules[i].sub_win;
            goto translate;
        }
    }

    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "Unrecognized Xbox path: %s", xbox_path);
    return MultiByteToWideChar(CP_ACP, 0, xbox_path, -1,
                               host_path_buf, buf_size) > 0;

translate:
    if (getenv("XBOXRECOMP_TRACE_PATH")) {
        fprintf(stderr, "  [PATH] %s\n", xbox_path);
        fflush(stderr);
    }
    {
        WCHAR remainder_wide[MAX_PATH];
        if (MultiByteToWideChar(CP_ACP, 0, remainder, -1,
                                remainder_wide, MAX_PATH) <= 0)
            return FALSE;

        for (WCHAR* p = remainder_wide; *p; p++) {
            if (*p == L'/') *p = L'\\';
        }

        if (sub_dir) {
            WCHAR sub_wide[MAX_PATH];
            if (MultiByteToWideChar(CP_ACP, 0, sub_dir, -1,
                                    sub_wide, MAX_PATH) <= 0)
                return FALSE;
            if (wcslen(base_dir) + wcslen(sub_wide) + 1 +
                    wcslen(remainder_wide) + 1 > buf_size)
                return FALSE;
            swprintf_s(host_path_buf, buf_size, L"%s%s\\%s", base_dir, sub_wide, remainder_wide);

            WCHAR dir_path[MAX_PATH];
            if (wcslen(base_dir) + wcslen(sub_wide) + 1 > MAX_PATH)
                return FALSE;
            swprintf_s(dir_path, MAX_PATH, L"%s%s", base_dir, sub_wide);
            CreateDirectoryW(s_save_dir, NULL);
            CreateDirectoryW(dir_path, NULL);
        } else {
            if (wcslen(base_dir) + 1 + wcslen(remainder_wide) + 1 > buf_size)
                return FALSE;
            swprintf_s(host_path_buf, buf_size, L"%s\\%s", base_dir, remainder_wide);
        }

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %S", xbox_path, host_path_buf);
        return TRUE;
    }
}

/* ======================================================================== */
#else /* !_WIN32  -- POSIX / Linux */
/* ======================================================================== */

#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>

static char s_game_dir[MAX_PATH];
static char s_save_dir[MAX_PATH];
static BOOL s_initialized = FALSE;

/* Strip a single trailing '/' (but never the root '/'). */
static void strip_trailing_slash(char* s)
{
    size_t len = strlen(s);
    if (len > 1 && s[len - 1] == '/')
        s[len - 1] = '\0';
}

/* Recursively create a directory and all missing parents. */
static void mkdir_p(const char* path)
{
    char tmp[MAX_PATH];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return;
    memcpy(tmp, path, len + 1);

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
}

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    if (game_dir) {
        snprintf(s_game_dir, sizeof(s_game_dir), "%s", game_dir);
    } else {
        char cwd[MAX_PATH];
        if (!getcwd(cwd, sizeof(cwd)))
            snprintf(cwd, sizeof(cwd), ".");
        snprintf(s_game_dir, sizeof(s_game_dir), "%s/Burnout 3 Takedown", cwd);
    }

    if (save_dir) {
        snprintf(s_save_dir, sizeof(s_save_dir), "%s", save_dir);
    } else {
        /* XDG base-directory spec: $XDG_DATA_HOME or ~/.local/share */
        const char* xdg = getenv("XDG_DATA_HOME");
        if (xdg && xdg[0]) {
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/burnout3", xdg);
        } else {
            const char* home = getenv("HOME");
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/.local/share/burnout3",
                     (home && home[0]) ? home : ".");
        }
    }

    strip_trailing_slash(s_game_dir);
    strip_trailing_slash(s_save_dir);

    mkdir_p(s_save_dir);
    {
        char partition_path[MAX_PATH];
        snprintf(partition_path, sizeof(partition_path), "%s/Partition0.bin", s_save_dir);
        int fd = open(partition_path, O_RDWR | O_CREAT, 0600);
        if (fd >= 0) {
            off_t size = lseek(fd, 0, SEEK_END);
            if (size >= 0 && size < 0x2000)
                ftruncate(fd, 0x2000);
            close(fd);
        }
    }

    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%s, save=%s",
             s_game_dir, s_save_dir);
}

BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char* remainder = NULL;
    const char* base_dir  = NULL;
    const char* sub_dir   = NULL;
    int         skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;
    if (!path_is_bounded(xbox_path))
        return FALSE;

    if (!s_initialized)
        xbox_path_init(NULL, NULL);

    if (path_equals(xbox_path, "\\Device\\Harddisk0\\partition0")) {
        snprintf(host_path_buf, buf_size, "%s/Partition0.bin", s_save_dir);
        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %s", xbox_path, host_path_buf);
        return TRUE;
    }

    for (int i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_prefix(xbox_path, s_rules[i].prefix);
        if (skip) {
            remainder = xbox_path + skip;
            base_dir  = s_rules[i].to_save ? s_save_dir : s_game_dir;
            sub_dir   = s_rules[i].sub_posix;
            goto translate;
        }
    }

    /* Unrecognized path: pass through, just normalize separators. */
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "Unrecognized Xbox path: %s", xbox_path);
    snprintf(host_path_buf, buf_size, "%s", xbox_path);
    for (char* p = host_path_buf; *p; p++)
        if (*p == '\\') *p = '/';
    return TRUE;

translate:
    {
        char remainder_posix[MAX_PATH];
        snprintf(remainder_posix, sizeof(remainder_posix), "%s", remainder);

        /* Xbox paths use backslashes -> POSIX slashes. */
        for (char* p = remainder_posix; *p; p++)
            if (*p == '\\') *p = '/';

        if (sub_dir) {
            snprintf(host_path_buf, buf_size, "%s%s/%s",
                     base_dir, sub_dir, remainder_posix);

            /* Ensure the save directory tree exists. */
            char dir_path[MAX_PATH];
            snprintf(dir_path, sizeof(dir_path), "%s%s", base_dir, sub_dir);
            mkdir_p(dir_path);
        } else {
            snprintf(host_path_buf, buf_size, "%s/%s", base_dir, remainder_posix);
        }

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %s", xbox_path, host_path_buf);
        return TRUE;
    }
}

#endif /* _WIN32 */
