/**
 * Red Dead Revolver (Xbox, NTSC-U) - static recompilation host
 *
 *   Title:          Red Dead Revolver   TT-026 (0x5454001A)  v1
 *   XDK:            5849 (all ten libraries)
 *   Base / entry:   0x00010000 / 0x00011652
 *   Image size:     0x0081C600   (.text 0x3250BC at 0x00011000)
 *   Code sections:  .text D3D XGRPH XACTENG XONLINE XNET BINK* DSOUND WMADEC XPP DOLBY
 *   Kernel imports: 148, all routed; no ordinal remap needed
 *
 * The game directory holds the extracted disc exactly as it came off it
 * (default.xbe, *.dat, bik/, lvls_*). Pick it with, in order:
 *   1. the first command-line argument
 *   2. RDR_GAME_DIR
 *   3. "game" next to the working directory
 *
 * One file for both hosts. Windows uses the template's vectored exception
 * handler; Linux/macOS use SIGSEGV/SIGBUS with the toolkit's POSIX MMIO hook.
 */

#if defined(_WIN32)
#  include <windows.h>
#  include <dbghelp.h>
#  include <commdlg.h>
#  include <shellapi.h>
#else
#  define _GNU_SOURCE
#  include <signal.h>
#  include <ucontext.h>
#  include <unistd.h>
#  include <sys/stat.h>
#endif
#include "ui/rdr_ui.h"
#include "ui/rdr_cheats.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>

#include <xbox/xboxrecomp.h>

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;

#define RDR_ENTRY_POINT 0x00011652u

extern void xbe_entry_point(void);
extern int recomp_dispatch_init(void);

/* MCPX APU model (see the template: gated on RECOMP_AC97_READY). */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;

/* ── crash report (host-neutral part) ─────────────────────────── */

static void print_guest_state(void)
{
    fprintf(stderr, "  Xbox regs: eax=%08X ecx=%08X edx=%08X esp=%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
    fprintf(stderr, "  Xbox regs: ebx=%08X esi=%08X edi=%08X\n",
            g_ebx, g_esi, g_edi);
    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");
    }
    fprintf(stderr, "  recent ICALL targets:");
    for (uint32_t k = 0; k < 16; k++)
        fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + k) & 15]);
    fprintf(stderr, "\n");
    {
        extern void rdr_crash_diagnostics(void);   /* rdr_overrides.c */
        rdr_crash_diagnostics();
    }
    fflush(stderr);
}

#if defined(_WIN32)
/* ── Windows: vectored exception handler ──────────────────────── */

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    uintptr_t fault = ep->ExceptionRecord->ExceptionInformation[1];
    if (fault >= 0xFD000000 && fault < 0xFE000000)
        return EXCEPTION_CONTINUE_SEARCH;
    if (xbox_VehMmioFault(ep))
        return EXCEPTION_CONTINUE_EXECUTION;

    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;

    fprintf(stderr, "[CRASH] access violation RIP=0x%llX addr=0x%llX (%s) guest VA=0x%08X\n",
            (unsigned long long)ep->ContextRecord->Rip, (unsigned long long)fault,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
            (uint32_t)(fault - (uintptr_t)g_xbox_mem_offset));
    if (SymFromAddr(GetCurrentProcess(), ep->ContextRecord->Rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n", sym->Name, (unsigned long long)disp);
    print_guest_state();
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Anything the vectored handler let through and nothing handled: a stack
 * overflow, an illegal instruction, a divide by zero, a fast-fail. These
 * used to end the process with no line in the log at all. Say what and
 * where, show the guest state, and leave logs\crash.dmp (a minidump) for
 * a debugger. */
static void crash_tell(void);
static LONG WINAPI last_chance(PEXCEPTION_POINTERS ep)
{
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    HANDLE f;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    fprintf(stderr, "[CRASH] unhandled exception 0x%08lX at RIP=0x%llX\n",
            (unsigned long)ep->ExceptionRecord->ExceptionCode,
            (unsigned long long)ep->ContextRecord->Rip);
    if (SymFromAddr(GetCurrentProcess(), ep->ContextRecord->Rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n", sym->Name, (unsigned long long)disp);
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
        print_guest_state();
    f = CreateFileA(GetFileAttributesA("logs") != INVALID_FILE_ATTRIBUTES
                        ? "logs\\crash.dmp" : "crash.dmp",
                    GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                              MiniDumpWithIndirectlyReferencedMemory, &mei, NULL, NULL))
            fprintf(stderr, "  minidump written: logs\\crash.dmp\n");
        CloseHandle(f);
    }
    fflush(stderr);
    if (!getenv("RDR_NO_CRASH_BOX"))
        crash_tell();
    return EXCEPTION_CONTINUE_SEARCH;
}

/* After a crash: a box that says so, with the log one click away (the last
 * part of Logs\last_run.log to the clipboard, ready to paste into a
 * report, or the Logs folder opened). */
static char s_exe_dir[1024];            /* (below: the exe's folder) */
extern void *xbox_FramebufferWindowHandle(void);
static char s_crash_log[1200];
static void crash_tell(void)
{
    static volatile LONG once;
    HWND game;
    int r;
    if (InterlockedExchange(&once, 1))
        return;
    if (s_exe_dir[0])
        snprintf(s_crash_log, sizeof s_crash_log, "%sLogs\\last_run.log", s_exe_dir);
    game = (HWND)xbox_FramebufferWindowHandle();
    if (game)
        ShowWindow(game, SW_MINIMIZE);      /* (a fullscreen game would hide the box) */
    r = MessageBoxA(NULL,
                    "Red Dead Recompilation stopped: it hit an error it could not get past.\n\n"
                    "What happened is in Logs\\last_run.log (and Logs\\crash.dmp). Sending the log with "
                    "a report helps get it fixed.\n\n"
                    "Yes: copy the log to the clipboard (to paste into a report)\n"
                    "No: open the Logs folder\n"
                    "Cancel: close",
                    "Red Dead Recompilation", MB_YESNOCANCEL | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    if (r == IDYES && s_crash_log[0]) {
        FILE *f = fopen(s_crash_log, "rb");
        if (f) {
            /* the start (the version, the settings) and the last 96 KB (the crash) */
            static char head[8192], tail[98304];
            size_t nh = fread(head, 1, sizeof head - 1, f), nt = 0, total;
            long size;
            head[nh] = 0;
            fseek(f, 0, SEEK_END);
            size = ftell(f);
            if (size > (long)(sizeof head + sizeof tail)) {
                fseek(f, size - (long)(sizeof tail - 1), SEEK_SET);
                nt = fread(tail, 1, sizeof tail - 1, f);
            }
            tail[nt] = 0;
            fclose(f);
            total = nh + nt + 64;
            if (OpenClipboard(NULL)) {
                HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, total);
                char *t = g ? (char *)GlobalLock(g) : NULL;
                if (t) {
                    snprintf(t, total, "%s%s%s", head, nt ? "\r\n[...]\r\n" : "", tail);
                    GlobalUnlock(g);
                    EmptyClipboard();
                    SetClipboardData(CF_TEXT, g);
                }
                CloseClipboard();
            }
            MessageBoxA(NULL, "The log is on the clipboard: paste it into your report (Ctrl+V).",
                        "Red Dead Recompilation", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        }
    } else if (r == IDNO && s_crash_log[0]) {
        char dir[1200], *e;
        snprintf(dir, sizeof dir, "%s", s_crash_log);
        e = strrchr(dir, '\\');
        if (e)
            *e = 0;
        ShellExecuteA(NULL, "open", dir, NULL, NULL, SW_SHOWNORMAL);
    }
}

static void install_crash_handler(void)
{
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);
    SetUnhandledExceptionFilter(last_chance);
}

#else
/* ── POSIX: SIGSEGV / SIGBUS / SIGFPE / SIGILL ─────────────────── */

#include <execinfo.h>

static void posix_fault(int sig, siginfo_t *si, void *ucv)
{
    uintptr_t fault = (uintptr_t)si->si_addr;
    if (xbox_PosixMmioFault(ucv, fault))
        return;                       /* trapped register access, resumed */

    fprintf(stderr, "[CRASH] signal %d addr=%p guest VA=0x%08X\n", sig,
            si->si_addr, (uint32_t)(fault - (uintptr_t)g_xbox_mem_offset));
    {
        void *bt[24];
        int n = backtrace(bt, 24);
        fprintf(stderr, "  host backtrace (innermost first):\n");
        backtrace_symbols_fd(bt, n, 2);
    }
    print_guest_state();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handler(void)
{
    struct sigaction sa;
    static char altstack[1 << 16];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack };
    sigaltstack(&ss, NULL);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = posix_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    /* a guest integer division by zero traps on the host too (the Xbox
     * would have stopped there as well): report it like the others
     * instead of dying without a word */
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}
#endif

/* ── boot ─────────────────────────────────────────────────────── */

static void *load_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    void *data;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = n > 0 ? malloc((size_t)n) : NULL;
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = NULL; }
    fclose(f);
    if (data) *size = (size_t)n;
    return data;
}

/* Runtime switches this title needs, applied unless already set in the
 * environment (set one to 0 to override). See docs/dev/BRINGUP.md for why each
 * is on. They must be in place before xbox_MemoryLayoutInit, which reads
 * several of them while it maps the device apertures. */
static void rdr_default_env(void)
{
    static const char *const defaults[][2] = {
        { "RECOMP_AC97_READY",   "1"     },  /* DirectSound init waits on the codec */
        { "RECOMP_PB_EXEC",      "1"     },  /* execute the pushbuffer (GPU)        */
        { "RECOMP_PB_WALK",      "dma"   },  /* walk it like the DMA engine         */
        { "RECOMP_VBLANK",       "paced" },  /* 60 Hz vblank for D3D's flip ISR     */
        { "RECOMP_TITLE_KEVENTS","1"     },  /* D3D's static KEVENT in .data        */
        { "RECOMP_GUEST_LOCK",   "1"     },  /* one guest CPU: Bink's reader race   */
        { "RECOMP_USB",          "1"     },  /* controllers via emulated OHCI       */
        { "RECOMP_KEYBOARD",     "1"     },  /* keyboard as pad 1                   */
        { "RECOMP_FB_WINDOW",    "1"     },  /* show the game: the window is opt-in */
        { "RECOMP_CONTIG_AVOID_IMAGE", "1" },  /* USB descriptors vs. the 8 MB image  */
    };
    size_t i;
    for (i = 0; i < sizeof defaults / sizeof defaults[0]; i++) {
        if (getenv(defaults[i][0]))
            continue;
#if defined(_WIN32)
        _putenv_s(defaults[i][0], defaults[i][1]);
#else
        setenv(defaults[i][0], defaults[i][1], 0);
#endif
    }
}

/* ── The player folder ──────────────────────────────────────────────
 *
 * build.bat assembles a ready-to-play folder (tools/package.ps1):
 *
 *   Red Dead Revolver.exe
 *   Game\                       the game's files
 *   Saves\  Mods\  Mods\textures\  Logs\  settings.ini
 *
 * The exe sees that layout beside itself (or knows it is that exe by its
 * name) and needs nothing else: no .bat, no paths. A disc image given to it
 * (dragged onto it), or picked in the window it opens when Game\ has no
 * game, is unpacked into Game\ first (iso_install.c). Without the layout -- run from the
 * project's play.bat, or with a game folder named -- nothing here applies. */
extern int rdr_install_iso(const char *image, const char *dest);

static char s_exe_dir[1024];            /* with its trailing separator */
static char s_play_game[1100];          /* the play folder's Game\ */

/* The play folder has no game yet (the launcher's install page). */
static int has_xbe(const char *dir)
{
    char p[1200];
    FILE *f;
    if (!dir || !dir[0])
        return 0;
    snprintf(p, sizeof p, "%s/default.xbe", dir);
    f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* The play folder's game: Game\, else an extracted copy elsewhere chosen
 * on the launcher's Install page (settings game_folder). */
const char *rdr_play_game_effective(void)
{
    if (!s_play_game[0])
        return NULL;
    if (has_xbe(s_play_game))
        return s_play_game;
    if (has_xbe(g_rdr_settings.game_folder))
        return g_rdr_settings.game_folder;
    return NULL;
}

int rdr_game_missing(void)
{
    return s_play_game[0] && !rdr_play_game_effective();
}

/* A play folder whose game is an extracted copy elsewhere: that folder. */
static void use_game_copy(const char **game_dir)
{
    static char eff[1100];
    const char *e = rdr_play_game_effective();
    extern void rdr_ui_set_game_dir(const char *dir);
    if (!e || !strcmp(e, s_play_game) || (*game_dir && !strcmp(*game_dir, e)))
        return;
    snprintf(eff, sizeof eff, "%s", e);
    *game_dir = eff;
    rdr_ui_set_game_dir(eff);
    printf("Game: %s (an extracted copy)\n", eff);
}

const char *rdr_play_game_dir(void)
{
    return s_play_game;
}

static void exe_dir_init(void)
{
    char *slash;
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, s_exe_dir, (DWORD)sizeof s_exe_dir);
    if (n == 0 || n >= sizeof s_exe_dir)
        s_exe_dir[0] = 0;
    slash = strrchr(s_exe_dir, '\\');
#else
    ssize_t n = readlink("/proc/self/exe", s_exe_dir, sizeof s_exe_dir - 1);
    s_exe_dir[n > 0 ? n : 0] = 0;
    slash = strrchr(s_exe_dir, '/');
#endif
    if (slash)
        slash[1] = 0;
    else
        s_exe_dir[0] = 0;
}

static int path_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f) {
        fclose(f);
        return 1;
    }
#if defined(_WIN32)
    {
        DWORD a = GetFileAttributesA(p);
        return a != INVALID_FILE_ATTRIBUTES;
    }
#else
    return 0;
#endif
}

static void make_dir(const char *p)
{
#if defined(_WIN32)
    CreateDirectoryA(p, NULL);
#else
    mkdir(p, 0755);
#endif
}

static void set_default_env(const char *name, const char *value)
{
    if (getenv(name))
        return;
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
}

/* The build the hooks and overrides were written for: Red Dead Revolver
 * (USA) v1, default.xbe of 4,403,200 bytes, SHA-256 AC00C86B...E3D698E0
 * (build.ps1 checks the hash): here its size, the header's time stamp
 * (+0x114) and the certificate's title ID, region and version. Another
 * revision or region with the same title ID would get every patch at the
 * wrong address. RDR_ANY_XBE=1 lets one through (development). */
static int xbe_is_known(const unsigned char *x, size_t n)
{
    uint32_t base, cert, ts;
    if (n != 4403200u || memcmp(x, "XBEH", 4))
        return 0;
    memcpy(&base, x + 0x104, 4);
    memcpy(&ts, x + 0x114, 4);
    memcpy(&cert, x + 0x118, 4);
    cert -= base;
    if (cert > n - 0xB0u)
        return 0;
    return ts == 0x4080A025u && !memcmp(x + cert + 8, "\x1A\x00\x54\x54", 4)
           && !memcmp(x + cert + 0xA0, "\x01\x00\x00\x00", 4) && !memcmp(x + cert + 0xAC, "\x01\x00\x00\x00", 4);
}

#if defined(_WIN32)
/* The log into path; when that cannot be written (a read-only play folder,
 * a full disk), into Windows' temporary folder instead, and said so, so a
 * bug report still has one. */
static void log_to(const char *path)
{
    char tmp[MAX_PATH], alt[MAX_PATH + 64];
    DWORD n;
    if (freopen(path, "w", stdout) && freopen(path, "a", stderr))
        return;
    n = GetTempPathA(sizeof tmp, tmp);
    if (!n || n >= sizeof tmp)
        return;
    snprintf(alt, sizeof alt, "%sRed Dead Recompilation last_run.log", tmp);
    if (freopen(alt, "w", stdout) && freopen(alt, "a", stderr))
        fprintf(stderr, "[RDR] %s could not be written: the log is here instead\n", path);
}
#endif

static void tell(const char *text, int error)
{
#if defined(_WIN32)
    MessageBoxA(NULL, text, "Red Dead Recompilation", MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
#else
    fprintf(stderr, "%s\n", text);
    (void)error;
#endif
}

static int ends_with_iso(const char *a)
{
    size_t n = strlen(a);
    return n > 4 && (!strcmp(a + n - 4, ".iso") || !strcmp(a + n - 4, ".ISO") || !strcmp(a + n - 4, ".Iso"));
}

/* The disc image to install, if one was given: --install-iso <file>, or a
 * .iso as an argument (dropped onto the exe). */
static const char *iso_argument(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--install-iso") && i + 1 < argc)
            return argv[i + 1];
        if (argv[i][0] != '-' && ends_with_iso(argv[i]))
            return argv[i];
    }
    return NULL;
}

/* The play folder's exe, by its name ("Red Dead Revolver.exe"; the build's
 * own is rdr_recomp.exe). */
static int is_play_exe(void)
{
#if defined(_WIN32)
    char p[1024];
    const char *b;
    DWORD n = GetModuleFileNameA(NULL, p, (DWORD)sizeof p);
    if (n == 0 || n >= sizeof p)
        return 0;
    b = strrchr(p, '\\');
    return _stricmp(b ? b + 1 : p, "Red Dead Revolver.exe") == 0;
#else
    return 0;
#endif
}

/* Returns the game folder to use (the player folder's Game\), or NULL to
 * carry on as before. *quit: stop here (nothing to play). */
static const char *player_folder(int argc, char **argv, int *quit)
{
    static char game[1100];
    char p[1200];
    const char *iso = iso_argument(argc, argv);
    int i, named = 0;
    *quit = 0;
    exe_dir_init();
    if (!s_exe_dir[0])
        return NULL;
    for (i = 1; i < argc; i++)
        if (argv[i][0] != '-' && !ends_with_iso(argv[i]) && (i == 1 || strcmp(argv[i - 1], "--install-iso")))
            named = 1;
    if (named || getenv("RDR_GAME_DIR"))
        return NULL;                    /* a game folder was named: as before */
    snprintf(game, sizeof game, "%sGame", s_exe_dir);
    snprintf(p, sizeof p, "%sInstall Game from ISO.bat", s_exe_dir);   /* (older folders) */
    if (!iso && !path_exists(game) && !path_exists(p) && !is_play_exe())
        return NULL;                    /* not a player folder */
    /* The log beside the exe, unless whoever started it redirected it. */
    snprintf(p, sizeof p, "%sLogs", s_exe_dir);
    make_dir(p);
#if defined(_WIN32)
    {
        HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
        int redirected = h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
        if (!redirected && !iso) {
            char last[1200], prev[1200];
            snprintf(last, sizeof last, "%sLogs\\last_run.log", s_exe_dir);
            snprintf(prev, sizeof prev, "%sLogs\\previous_run.log", s_exe_dir);
            MoveFileExA(last, prev, MOVEFILE_REPLACE_EXISTING);
            log_to(last);
        }
    }
#endif

    if (iso) {
#if defined(_WIN32)
        /* Progress in a console of its own (the game is a window program). */
        if (AllocConsole()) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
            SetConsoleTitleA("Red Dead Recompilation - installing the game");
        }
#endif
        fprintf(stderr, "Red Dead Recompilation: installing the game from its disc image.\n");
        int r = rdr_install_iso(iso, game);
        if (r != 1) {
            tell(r == -2 ? "That disc image is another game.\n\n"
                           "It has to be an Xbox disc image (.iso) of Red Dead Revolver (USA)."
                         : "The game could not be installed from that file.\n\n"
                           "It has to be an Xbox disc image (.iso) of Red Dead Revolver (USA).", 1);
            *quit = 1;
            return NULL;
        }
        snprintf(p, sizeof p, "%s%cdefault.xbe", game, '/');
        if (!path_exists(p)) {
            tell("That disc image has no default.xbe: it is not an Xbox game disc.", 1);
            *quit = 1;
            return NULL;
        }
        tell("Red Dead Revolver is installed. The game starts now.\n\n"
             "Next time, just start Red Dead Revolver.exe.", 0);
#if defined(_WIN32)
        FreeConsole();
        {
            /* From here on, the log as on any start. */
            char last[1200];
            snprintf(last, sizeof last, "%sLogs\\last_run.log", s_exe_dir);
            log_to(last);
        }
#endif
    }

    /* No game yet: the launcher opens on its install page (the disc image
     * picked there, unpacked with its progress shown), main.c checks again
     * after it. */
    make_dir(game);
    snprintf(s_play_game, sizeof s_play_game, "%s", game);
    /* Saves, mods and texture packs beside the exe. */
    snprintf(p, sizeof p, "%sSaves", s_exe_dir);
    make_dir(p);
    set_default_env("RECOMP_SAVE_DIR", p);
    snprintf(p, sizeof p, "%sMods", s_exe_dir);
    make_dir(p);
    set_default_env("RECOMP_MODS_DIR", p);
    snprintf(p, sizeof p, "%sMods%ctextures", s_exe_dir, s_exe_dir[strlen(s_exe_dir) - 1]);
    make_dir(p);
    set_default_env("RECOMP_TEXTURE_PACK", p);
    return game;
}

#include "net/rdr_net.h"

char **g_rdr_argv;                      /* the online lobby starts the game again with them */

int main(int argc, char **argv)
{
    const char *game_dir = getenv("RDR_GAME_DIR");
    char xbe_path[1024];
    size_t xbe_size = 0;
    void *xbe;
    int i;

    g_rdr_argv = argv;

    /* Online play, a guest: no game here, a window onto the host's
     * (src/net/rdr_viewer.c). */
    for (i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--join")) {
            const char *name = getenv("RDR_NET_NAME");
            int k;
            for (k = 1; k + 1 < argc; k++)
                if (!strcmp(argv[k], "--name"))
                    name = argv[k + 1];
            return rdr_viewer_main(argv[i + 1], name);
        }

    /* The ready-to-play folder: Game\\ beside the exe, a disc image dropped
     * onto it installed first. */
    {
        int quit = 0;
        const char *pf = player_folder(argc, argv, &quit);
#ifdef RDR_VERSION
        fprintf(stderr, "[RDR] Red Dead Recompilation %s\n", RDR_VERSION);
        if (getenv("RECOMP_SAVE_DIR"))
            fprintf(stderr, "[RDR] saves: %s (the game's disc and its folder are never written)\n",
                    getenv("RECOMP_SAVE_DIR"));
#endif
        if (quit)
            return 1;
        if (pf)
            game_dir = pf;
        else
            /* The game folder is the first argument that is not an option
             * (--launcher opens the settings window). */
            for (i = 1; i < argc; i++)
                if (argv[i][0] != '-' && !ends_with_iso(argv[i])
                    && (i == 1 || strcmp(argv[i - 1], "--install-iso"))) {
                    game_dir = argv[i];
                    break;
                }
    }
    if (!game_dir) game_dir = "game";
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("=== Red Dead Revolver - static recompilation ===\n");

    /* The player's settings (settings.ini), and the launcher when it is
     * wanted: the first start, Shift held, --launcher. They become RECOMP_*
     * switches before anything reads them. */
#if defined(_WIN32)
    {
        /* Controllers: SDL starts now, on its own thread -- before the
         * launcher, whose controller list names each pad, and while the game
         * boots, not at its first pad read, where seconds of device
         * enumeration held the picture on the Rockstar logo. */
        extern void xbox_InputInit(void);
        xbox_InputInit();
    }
#endif
    {
        /* the menus draw with the game's own art, read from its files */
        extern void rdr_ui_set_game_dir(const char *dir);
        rdr_ui_set_game_dir(game_dir ? game_dir : "game");
    }
    {
        int existed = rdr_settings_load(&g_rdr_settings);
        rdr_online_apply_settings(&g_rdr_settings);   /* a game from the lobby: the host's options */
        use_game_copy(&game_dir);       /* an extracted copy chosen before */
        if (rdr_launcher_wanted(argc, argv, existed) || rdr_game_missing()) {
            if (!rdr_launcher_run()) {
                printf("Closed from the launcher.\n");
                return 0;
            }
        }
        use_game_copy(&game_dir);       /* (chosen on the Install page just now) */
        if (rdr_game_missing()) {
            tell("Red Dead Revolver is not installed yet.\n\n"
                 "Start Red Dead Revolver.exe and choose your Red Dead Revolver (USA) Xbox disc\n"
                 "image (.iso) on the launcher's first page, or copy the disc's files into the\n"
                 "Game folder.", 0);
            return 0;
        }
        rdr_settings_apply_env(&g_rdr_settings);
        printf("Settings: %s\n", rdr_settings_path());
        /* the characters the mods add, for Play As (rdr_modchars.c) */
        rdr_characters_scan_mods(game_dir, getenv("RECOMP_MODS_DIR"));
        rdr_settings_resolve_characters(&g_rdr_settings);
    }
    rdr_default_env();
    {
        /* lockstep (rdr_lockstep.c) decided before the game starts: its clock
         * replaces the kernel's from the first read */
        extern int rdr_lockstep_mode(void);
        rdr_lockstep_mode();
    }

    snprintf(xbe_path, sizeof xbe_path, "%s/default.xbe", game_dir);
    xbe = load_file(xbe_path, &xbe_size);
    if (!xbe) {
        fprintf(stderr, "Cannot open %s\n"
                "Pass the folder with the extracted disc (default.xbe, swat.dat, bik/...)\n"
                "as the first argument, or set RDR_GAME_DIR.\n", xbe_path);
        return 1;
    }
    printf("XBE: %s (%zu bytes)\n", xbe_path, xbe_size);
    if (!xbe_is_known(xbe, xbe_size) && !getenv("RDR_ANY_XBE")) {
        tell("This copy of Red Dead Revolver is not the one this port was made for: the USA "
             "release (version 1). Its fixes and features are tied to that build's code, and "
             "another version would run with them in the wrong places.\n\n"
             "Use the USA disc (the build checks it when you run build.bat).", 1);
        return 1;
    }

    install_crash_handler();

    if (!xbox_MemoryLayoutInit(xbe, xbe_size)) {
        fprintf(stderr, "Xbox memory layout init failed (address range unavailable?)\n");
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();

    /* USB host controllers for the pads. Nothing in the runtime calls this:
     * the host has to, once guest memory is mapped (it traps the OHCI
     * register blocks). No-op unless RECOMP_USB is set. */
    {
        extern void xbox_OhciInit(void);
        xbox_OhciInit();
    }

    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n", g_apu_state ? "up" : "FAILED");
    }

    xbox_kernel_init();
    /* a copy of the saves (Saves/Backup/1-3), not in an online game: its
     * saves are the host's, in Saves/Online, and are not this player's */
    if (!getenv("RDR_ONLINE_HOME_SAVE")) {
        extern int rdr_save_backup(const char *save_dir);
        rdr_save_backup(getenv("RECOMP_SAVE_DIR"));
    }
    xbox_path_init(game_dir, getenv("RECOMP_SAVE_DIR"));
    xbox_kernel_bridge_init();
    g_esp = XBOX_STACK_TOP;

    {
        extern void rdr_runtime_init(void);   /* rdr_overrides.c */
        rdr_runtime_init();
    }
    rdr_overlay_install();                    /* F1 menu, F2 frame counter */
    {
        extern void rdr_sharptext_install(void);
        rdr_sharptext_install();              /* the game's lettering, 4x finer */
    }

    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; using binary search\n");
    xbox_WatchdogStart();

    printf("Entry 0x%08X, esp 0x%08X - starting game\n", RDR_ENTRY_POINT, g_esp);
    xbe_entry_point();

    printf("Game returned.\n");
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe);
    return 0;
}

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR cmd, int show)
{
    (void)h; (void)p; (void)cmd; (void)show;
    return main(__argc, __argv);
}
#endif
