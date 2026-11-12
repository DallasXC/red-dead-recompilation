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

/* This build's version ("1.1.1"). */
const char *rdr_version(void);
/* Ask, in the background (once). */
void rdr_update_start(void);
/* The state; *tag and *url of the latest release once known. */
int  rdr_update_state(const char **tag, const char **url);
/* The release's page in the browser. */
void rdr_update_open_page(void);

#ifdef __cplusplus
}
#endif
#endif
