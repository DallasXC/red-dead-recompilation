/*
 * rdr_preview_setup.c - the launcher's "Set up the preview characters":
 * the 2002 preview's disc image picked in a window, then
 * tools/mods/setup_preview.py run with Python in the background, its
 * "PROGRESS <percent> <what>" and "ERROR <what>" lines shown on the page.
 *
 * The scripts: Tools\mods beside the exe (the play folder, tools/package.ps1
 * copies them), else the project's tools/mods (a build folder's exe). The
 * output: rdr_preview_dir() (Mods/Preview). Python: "py -3", then
 * "python" (Windows), "python3" elsewhere -- the build installed it.
 */
#include "rdr_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

const char *rdr_ui_game_dir(void);

static volatile int s_state;            /* 0 idle, 1 running, 2 done, 3 failed */
static volatile int s_pct;
static char s_text[256];
static char s_log[2048];                /* the last lines, for a failure */
static char s_cmd[4096];

#if defined(_WIN32)
static CRITICAL_SECTION s_lk;
static int s_lk_init;
static void lk(void) { if (!s_lk_init) { InitializeCriticalSection(&s_lk); s_lk_init = 1; } EnterCriticalSection(&s_lk); }
static void ulk(void) { LeaveCriticalSection(&s_lk); }
#else
static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static void lk(void) { pthread_mutex_lock(&s_mx); }
static void ulk(void) { pthread_mutex_unlock(&s_mx); }
#endif

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void exe_dir(char *out, size_t n)
{
    char p[1024] = "";
    char *s1, *s2, *sl;
#if defined(_WIN32)
    GetModuleFileNameA(NULL, p, sizeof p);
#else
    ssize_t k = readlink("/proc/self/exe", p, sizeof p - 1);
    p[k > 0 ? k : 0] = 0;
#endif
    s1 = strrchr(p, '\\');
    s2 = strrchr(p, '/');
    sl = s1 > s2 ? s1 : s2;
    if (sl) sl[1] = 0;
    snprintf(out, n, "%s", p);
}

/* setup_preview.py: beside the exe, or the project's (build\Release\). */
static int find_script(char *out, size_t n)
{
    static const char *rel[] = { "Tools/mods/", "tools/mods/", "../tools/mods/", "../../tools/mods/",
                                 "../../../tools/mods/" };
    char dir[1024], p[1200];
    unsigned i;
    exe_dir(dir, sizeof dir);
    for (i = 0; i < sizeof rel / sizeof rel[0]; i++) {
        snprintf(p, sizeof p, "%s%ssetup_preview.py", dir, rel[i]);
        if (file_exists(p)) {
            snprintf(out, n, "%s", p);
            return 1;
        }
    }
    return 0;
}

static void line_in(const char *line)
{
    size_t l;
    lk();
    if (!strncmp(line, "PROGRESS ", 9)) {
        const char *t = line + 9;
        s_pct = atoi(t);
        t = strchr(t, ' ');
        snprintf(s_text, sizeof s_text, "%s", t ? t + 1 : "");
    } else if (!strncmp(line, "ERROR ", 6)) {
        snprintf(s_text, sizeof s_text, "%s", line + 6);
        s_state = 3;
    }
    l = strlen(s_log);
    if (l + strlen(line) + 2 >= sizeof s_log) {
        memmove(s_log, s_log + sizeof s_log / 2, sizeof s_log / 2);
        s_log[sizeof s_log / 2] = 0;
        l = strlen(s_log);
    }
    snprintf(s_log + l, sizeof s_log - l, "%s\n", line);
    ulk();
    fprintf(stderr, "[PREVIEW] %s\n", line);
    fflush(stderr);
}

static void finished(int code)
{
    lk();
    if (code == 0 && s_state != 3) {
        s_state = 2;
        s_pct = 100;
    } else if (s_state != 3) {
        s_state = 3;
        if (code == 9009 || code == 127)
            snprintf(s_text, sizeof s_text, "Python 3 was not found. It is installed by build.bat, or from python.org.");
        else
            snprintf(s_text, sizeof s_text, "The setup stopped (code %d). Logs\\last_run.log has the details.", code);
    }
    ulk();
}

#if defined(_WIN32)
/* Runs one command line, its output read line by line; its exit code, or
 * -1 when it could not start. */
static int run_one(const char *cmd)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char buf[1024], line[1024];
    int ln = 0;
    DWORD got, code = 1;
    char *c = _strdup(cmd);
    if (!c || !CreatePipe(&rd, &wr, &sa, 0)) {
        free(c);
        return -1;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    if (!CreateProcessA(NULL, c, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        free(c);
        return -1;
    }
    free(c);
    CloseHandle(wr);
    while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got) {
        DWORD i;
        for (i = 0; i < got; i++) {
            char ch = buf[i];
            if (ch == '\r')
                continue;
            if (ch == '\n' || ln >= (int)sizeof line - 1) {
                line[ln] = 0;
                if (ln) line_in(line);
                ln = 0;
                if (ch == '\n')
                    continue;
            }
            line[ln++] = ch;
        }
    }
    if (ln) {
        line[ln] = 0;
        line_in(line);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

static DWORD WINAPI worker(LPVOID arg)
{
    /* "py -3" first (the launcher Python installs), then "python" */
    static const char *pys[] = { "py -3 -u", "python -u" };
    unsigned i;
    int code = -1;
    (void)arg;
    for (i = 0; i < 2 && code == -1; i++) {
        char full[4200];
        snprintf(full, sizeof full, "%s %s", pys[i], s_cmd);
        code = run_one(full);
        if (code == 9009 || code == 103)      /* not found: the next one */
            code = -1;
    }
    finished(code == -1 ? 9009 : code);
    return 0;
}
#else
static void *worker(void *arg)
{
    char full[4200], line[1024];
    FILE *p;
    int code;
    (void)arg;
    snprintf(full, sizeof full, "python3 -u %s 2>&1", s_cmd);
    p = popen(full, "r");
    if (!p) {
        finished(127);
        return NULL;
    }
    while (fgets(line, sizeof line, p)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = 0;
        if (l) line_in(line);
    }
    code = pclose(p);
    finished(WIFEXITED(code) ? WEXITSTATUS(code) : 1);
    return NULL;
}
#endif

int rdr_preview_setup_start(const char *image)
{
    char script[1200];
    if (s_state == 1)
        return 0;
    if (!find_script(script, sizeof script)) {
        lk();
        s_state = 3;
        snprintf(s_text, sizeof s_text, "The setup's scripts (Tools\\mods) are not beside the game. Build it again with build.bat.");
        ulk();
        return 0;
    }
    if (image)
        snprintf(s_cmd, sizeof s_cmd, "\"%s\" \"%s\" \"%s\" \"%s\"", script, image, rdr_ui_game_dir(),
                 rdr_preview_dir());
    else                                /* the story's people alone: no disc */
        snprintf(s_cmd, sizeof s_cmd, "\"%s\" --people \"%s\" \"%s\"", script, rdr_ui_game_dir(),
                 rdr_people_dir());
    lk();
    s_state = 1;
    s_pct = 0;
    s_log[0] = 0;
    snprintf(s_text, sizeof s_text, "Starting");
    ulk();
    fprintf(stderr, "[PREVIEW] setup: %s\n", s_cmd);
    fflush(stderr);
#if defined(_WIN32)
    {
        HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (!h) {
            finished(1);
            return 0;
        }
        CloseHandle(h);
    }
#else
    {
        pthread_t t;
        if (pthread_create(&t, NULL, worker, NULL) != 0) {
            finished(1);
            return 0;
        }
        pthread_detach(t);
    }
#endif
    return 1;
}

int rdr_preview_setup_state(int *pct, char *text, int n)
{
    int st;
    lk();
    st = s_state;
    if (pct) *pct = s_pct;
    if (text && n > 0) snprintf(text, (size_t)n, "%s", s_text);
    ulk();
    return st;
}

/* A file picked in a window: 1 when one was. filter: Windows' pairs
 * ("text\0pattern\0...\0"). Elsewhere zenity, or the env variable named. */
int rdr_pick_file(const char *title, const char *filter, const char *env, char *out, int n)
{
#if defined(_WIN32)
    OPENFILENAMEA ofn;
    char file[1024] = "";
    (void)env;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof file;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn))
        return 0;
    snprintf(out, (size_t)n, "%s", file);
    return 1;
#else
    const char *e = env ? getenv(env) : NULL;
    char cmd[400];
    FILE *p;
    (void)filter;
    if (e && *e) {
        snprintf(out, (size_t)n, "%s", e);
        return 1;
    }
    snprintf(cmd, sizeof cmd, "zenity --file-selection --title=\"%s\" 2>/dev/null", title);
    p = popen(cmd, "r");
    if (!p)
        return 0;
    if (!fgets(out, n, p)) {
        pclose(p);
        return 0;
    }
    pclose(p);
    out[strcspn(out, "\r\n")] = 0;
    return out[0] != 0;
#endif
}

int rdr_preview_pick_image(char *out, int n)
{
    return rdr_pick_file("Choose the January 2002 preview's disc image",
                         "The 2002 preview's disc image (*.iso, SWAT.ZIP)\0*.iso;*.zip\0All files (*.*)\0*.*\0",
                         "RDR_PREVIEW_IMAGE", out, n);
}

/* ── The game itself from its disc image (the launcher's install page,
 * when the play folder has no game: iso_install.c in the background) ── */
extern int rdr_install_iso(const char *image, const char *dest);
extern void rdr_install_progress(int *pct, unsigned *files);
extern const char *rdr_play_game_dir(void);
static volatile int s_inst_state;       /* 0 idle, 1 running, 2 done, 3 failed */
static char s_inst_image[1024];
static char s_inst_text[200];

#if defined(_WIN32)
static DWORD WINAPI install_worker(LPVOID arg)
#else
static void *install_worker(void *arg)
#endif
{
    int r;
    (void)arg;
    r = rdr_install_iso(s_inst_image, rdr_play_game_dir());
    lk();
    if (r == 1) {
        s_inst_state = 2;
        snprintf(s_inst_text, sizeof s_inst_text, "Installed.");
    } else {
        s_inst_state = 3;
        snprintf(s_inst_text, sizeof s_inst_text, r == -2
                 ? "That disc image is another game. Choose the Red Dead Revolver (USA) Xbox disc image."
                 : "That file could not be installed: it has to be an Xbox disc image (.iso) of Red "
                   "Dead Revolver (USA), and the drive needs about 2 GB free.");
    }
    ulk();
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

int rdr_game_install_pick(char *out, int n)
{
    return rdr_pick_file("Your Red Dead Revolver (USA) Xbox disc image",
                         "Xbox disc images (*.iso)\0*.iso\0All files (*.*)\0*.*\0",
                         "RDR_GAME_IMAGE", out, n);
}

int rdr_game_install_start(const char *image)
{
    if (s_inst_state == 1)
        return 0;
    snprintf(s_inst_image, sizeof s_inst_image, "%s", image);
    lk();
    s_inst_state = 1;
    s_inst_text[0] = 0;
    ulk();
#if defined(_WIN32)
    {
        HANDLE h = CreateThread(NULL, 0, install_worker, NULL, 0, NULL);
        if (!h) { s_inst_state = 3; return 0; }
        CloseHandle(h);
    }
#else
    {
        pthread_t t;
        if (pthread_create(&t, NULL, install_worker, NULL) != 0) { s_inst_state = 3; return 0; }
        pthread_detach(t);
    }
#endif
    return 1;
}

int rdr_game_pick_xbe(char *out, int n)
{
    return rdr_pick_file("The extracted game's default.xbe",
                         "The game's default.xbe\0default.xbe\0Xbox programs (*.xbe)\0*.xbe\0All files (*.*)\0*.*\0",
                         "RDR_GAME_XBE", out, n);
}

/* default.xbe's title ID: its certificate's +8 (Red Dead Revolver (USA):
 * 0x5454001A). */
static unsigned xbe_title(const char *path)
{
    unsigned char h[0x200], c[12];
    unsigned base, cert;
    FILE *f = fopen(path, "rb");
    unsigned id = 0;
    if (!f)
        return 0;
    if (fread(h, 1, sizeof h, f) == sizeof h && !memcmp(h, "XBEH", 4)) {
        base = h[0x104] | h[0x105] << 8 | h[0x106] << 16 | (unsigned)h[0x107] << 24;
        cert = h[0x118] | h[0x119] << 8 | h[0x11A] << 16 | (unsigned)h[0x11B] << 24;
        if (cert > base && fseek(f, (long)(cert - base), SEEK_SET) == 0 && fread(c, 1, 12, f) == 12)
            id = c[8] | c[9] << 8 | c[10] << 16 | (unsigned)c[11] << 24;
    }
    fclose(f);
    return id;
}

int rdr_game_use_folder(const char *xbe_path)
{
    char dir[1024], sw[1100];
    char *s1, *s2, *sl;
    FILE *f;
    if (xbe_title(xbe_path) != 0x5454001Au)
        return xbe_title(xbe_path) ? -2 : 0;
    snprintf(dir, sizeof dir, "%s", xbe_path);
    s1 = strrchr(dir, '\\');
    s2 = strrchr(dir, '/');
    sl = s1 > s2 ? s1 : s2;
    if (!sl)
        return 0;
    *sl = 0;
    snprintf(sw, sizeof sw, "%s/swat.dat", dir);
    f = fopen(sw, "rb");
    if (!f)
        return 0;                       /* default.xbe alone: not the whole disc */
    fclose(f);
    snprintf(g_rdr_settings.game_folder, sizeof g_rdr_settings.game_folder, "%s", dir);
    rdr_settings_save(&g_rdr_settings);
    {
        extern void rdr_ui_set_game_dir(const char *d);
        rdr_ui_set_game_dir(dir);       /* the launcher's art from there */
    }
    fprintf(stderr, "[INSTALL] the game in %s\n", dir);
    lk();
    s_inst_state = 2;
    snprintf(s_inst_text, sizeof s_inst_text, "Using the game in %s.", dir);
    ulk();
    return 1;
}

int rdr_game_install_state(int *pct, char *text, int n)
{
    int st;
    unsigned files = 0;
    lk();
    st = s_inst_state;
    if (text && n > 0) snprintf(text, (size_t)n, "%s", s_inst_text);
    ulk();
    if (pct) {
        rdr_install_progress(pct, &files);
        if (st == 2) *pct = 100;
    }
    return st;
}

#if !defined(_WIN32)
static int remove_tree(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int ok = 1;
    if (!d)
        return remove(path) == 0;
    while ((e = readdir(d)) != NULL) {
        char p[1200];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode))
            ok &= remove_tree(p);
        else
            ok &= remove(p) == 0;
    }
    closedir(d);
    return ok && rmdir(path) == 0;
}
#endif

/* The preview's folder deleted (the setting goes off). */
int rdr_preview_remove(void)
{
    const char *dir = rdr_preview_dir();
    if (s_state == 1 || !dir || strlen(dir) < 8 || !strstr(dir, "Preview"))
        return 0;                       /* (only ever that folder) */
#if defined(_WIN32)
    {
        char from[1200];
        SHFILEOPSTRUCTA op;
        size_t l;
        memset(from, 0, sizeof from);
        snprintf(from, sizeof from - 2, "%s", dir);
        for (l = 0; from[l]; l++)
            if (from[l] == '/') from[l] = '\\';
        memset(&op, 0, sizeof op);
        op.wFunc = FO_DELETE;
        op.pFrom = from;                /* double-0 ended */
        /* to the Recycle Bin (Windows deletes it outright only when it is
         * bigger than the bin is set to hold) */
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        if (SHFileOperationA(&op) != 0)
            return 0;
    }
#else
    if (!remove_tree(dir))
        return 0;
#endif
    lk();
    s_state = 0;
    s_text[0] = 0;
    ulk();
    return 1;
}
