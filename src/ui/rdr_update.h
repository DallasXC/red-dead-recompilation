/* rdr_update.h - the version, and the launcher's update check (rdr_update.c). */
#ifndef RDR_UPDATE_H
#define RDR_UPDATE_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RDR_UPDATE_IDLE = 0,        /* not asked */
    RDR_UPDATE_CHECKING,
    RDR_UPDATE_CURRENT,         /* this is the latest */
    RDR_UPDATE_NEWER,           /* a newer release: tag and url */
    RDR_UPDATE_UNKNOWN          /* no answer (no network, no releases) */
};

/* This build's version ("1.2.0"). */
const char *rdr_version(void);
/* Ask, in the background (once). */
void rdr_update_start(void);
/* The state; *tag and *url of the latest release once known. */
int  rdr_update_state(const char **tag, const char **url);
/* The release's page in the browser. */
void rdr_update_open_page(void);
/* A newer release, and the project folder the game was built in (so it can
 * be updated in place): "Update now" can be offered. */
int  rdr_update_can_install(void);
/* Start the update (tools/update.ps1, its own window); 1 when started: the
 * caller then closes the game. */
int  rdr_update_install(void);

#ifdef __cplusplus
}
#endif
#endif
