/*
 * rdr_update.c - is there a newer release? (the launcher's update check)
 *
 * Once a start, when the launcher opens and the player has not switched it
 * off (About: "Check for updates"), a background thread asks GitHub for the
 * project's latest release (api.github.com, .../releases/latest: its
 * "tag_name" and "html_url") and compares the tag with this build's version
 * (RDR_VERSION, from the VERSION file). Nothing is downloaded or installed:
 * a newer one is only shown, with a button that opens its page in the
 * browser. With no network, or no releases yet, it says nothing.
 */
#include "rdr_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef RDR_VERSION
#define RDR_VERSION "0.0.0"
#endif

#define RDR_RELEASES_API "/repos/DallasXC/red-dead-recompilation/releases/latest"
#define RDR_RELEASES_PAGE "https://github.com/DallasXC/red-dead-recompilation/releases"

static volatile int s_state;            /* RDR_UPDATE_* */
static char s_tag[64], s_url[512];

const char *rdr_version(void) { return RDR_VERSION; }

/* "v1.2.3", "1.2", "0.88.0-beta": the numbers in order; -1, 0, 1. */
static int version_cmp(const char *a, const char *b)
{
    int i;
    for (i = 0; i < 4; i++) {
        long x, y;
        while (*a && (*a < '0' || *a > '9')) a++;
        while (*b && (*b < '0' || *b > '9')) b++;
        x = *a ? strtol(a, (char **)&a, 10) : 0;
        y = *b ? strtol(b, (char **)&b, 10) : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

/* "key": "value" out of a JSON text (the first one; no escapes needed). */
static int json_str(const char *j, const char *key, char *out, size_t n)
{
    char k[64];
    const char *p, *e;
    snprintf(k, sizeof k, "\"%s\"", key);
    p = strstr(j, k);
    if (!p)
        return 0;
    p = strchr(p + strlen(k), ':');
    if (!p)
        return 0;
    p = strchr(p, '"');
    if (!p)
        return 0;
    p++;
    e = strchr(p, '"');
    if (!e || (size_t)(e - p) >= n)
        return 0;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = 0;
    return 1;
}

static void decide(const char *body)
{
    char tag[64] = "", url[512] = "";
    if (!body || !json_str(body, "tag_name", tag, sizeof tag)) {
        s_state = RDR_UPDATE_UNKNOWN;
        fprintf(stderr, "[UPDATE] no release to compare with\n");
        return;
    }
    if (!json_str(body, "html_url", url, sizeof url) || strncmp(url, "https://github.com/", 19))
        snprintf(url, sizeof url, "%s", RDR_RELEASES_PAGE);
    snprintf(s_tag, sizeof s_tag, "%s", tag);
    snprintf(s_url, sizeof s_url, "%s", url);
    s_state = version_cmp(tag, RDR_VERSION) > 0 ? RDR_UPDATE_NEWER : RDR_UPDATE_CURRENT;
    fprintf(stderr, "[UPDATE] latest release %s, this build %s: %s\n", tag, RDR_VERSION,
            s_state == RDR_UPDATE_NEWER ? "a newer one is out" : "up to date");
    fflush(stderr);
}

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

static DWORD WINAPI check_thread(LPVOID unused)
{
    HINTERNET ses = NULL, con = NULL, req = NULL;
    char *body = NULL;
    size_t len = 0;
    DWORD status = 0, sz = sizeof status;
    (void)unused;
    ses = WinHttpOpen(L"RedDeadRecompilation-update-check", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses)
        ses = WinHttpOpen(L"RedDeadRecompilation-update-check", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses)
        goto out;
    WinHttpSetTimeouts(ses, 5000, 5000, 5000, 8000);
    con = WinHttpConnect(ses, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!con)
        goto out;
    req = WinHttpOpenRequest(con, L"GET", L"" RDR_RELEASES_API, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req)
        goto out;
    WinHttpAddRequestHeaders(req, L"Accept: application/vnd.github+json", (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL))
        goto out;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &sz, WINHTTP_NO_HEADER_INDEX);
    for (;;) {
        DWORD avail = 0, got = 0;
        char *nb;
        if (!WinHttpQueryDataAvailable(req, &avail) || !avail || len + avail > (1u << 20))
            break;
        nb = (char *)realloc(body, len + avail + 1);
        if (!nb)
            break;
        body = nb;
        if (!WinHttpReadData(req, body + len, avail, &got) || !got)
            break;
        len += got;
        body[len] = 0;
    }
out:
    if (status == 200 && body)
        decide(body);
    else {
        s_state = RDR_UPDATE_UNKNOWN;
        fprintf(stderr, "[UPDATE] could not ask for the latest release (HTTP %lu)\n", (unsigned long)status);
        fflush(stderr);
    }
    free(body);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return 0;
}

void rdr_update_start(void)
{
    HANDLE t;
    if (s_state != RDR_UPDATE_IDLE)
        return;
    if (getenv("RDR_UPDATE_TEST")) {
        /* (tests: a release given as JSON in a file) */
        FILE *f = fopen(getenv("RDR_UPDATE_TEST"), "rb");
        char buf[4096];
        size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        if (f) fclose(f);
        buf[n] = 0;
        decide(n ? buf : NULL);
        return;
    }
    s_state = RDR_UPDATE_CHECKING;
    t = CreateThread(NULL, 0, check_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        s_state = RDR_UPDATE_UNKNOWN;
}

void rdr_update_open_page(void)
{
    if (s_url[0])
        ShellExecuteA(NULL, "open", s_url, NULL, NULL, SW_SHOWNORMAL);
}
#else
void rdr_update_start(void)
{
    if (s_state != RDR_UPDATE_IDLE)
        return;
    if (getenv("RDR_UPDATE_TEST")) {
        FILE *f = fopen(getenv("RDR_UPDATE_TEST"), "rb");
        char buf[4096];
        size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        if (f) fclose(f);
        buf[n] = 0;
        decide(n ? buf : NULL);
        return;
    }
    s_state = RDR_UPDATE_UNKNOWN;         /* (Windows only, for now) */
}

void rdr_update_open_page(void) {}
#endif

int rdr_update_state(const char **tag, const char **url)
{
    if (tag) *tag = s_tag;
    if (url) *url = s_url;
    return s_state;
}
