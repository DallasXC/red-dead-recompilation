/*
 * rdr_net.h - online play: the wire, the host's side, the guest's viewer.
 *
 * "Option 2", host streaming: the host runs the game; each guest is a pad
 * on the host (a virtual pad on a free port: rdr_net_pad_*) and a window
 * that shows the host's picture and plays its sound (rdr_viewer.c). The
 * guest needs no game files. UDP, one port (default 27031).
 *
 * Messages (little endian, each a datagram, first 4 bytes "RDRN", then
 * a type byte):
 *   guest -> host  HELLO    name[32]                   (until WELCOME)
 *                  PAD      seq u32, XINPUT_STATE[22]   (60 a second)
 *                  BYE
 *   host -> guest  WELCOME  slot u8 (the pad port), w u16, h u16
 *                  FULL     (no room)
 *                  FRAME    id u32, part u16, parts u16, data... (a JPEG
 *                           picture in parts of up to 1100 bytes)
 *                  AUDIO    seq u32, rate u32, frames u16, s16 stereo...
 *                  BYE
 */
#ifndef RDR_NET_H
#define RDR_NET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDR_NET_PORT        27031
#define RDR_NET_MAGIC       0x4E524452u   /* "RDRN" */
#define RDR_NET_MAX_GUESTS  3
#define RDR_NET_PART        1100          /* picture bytes per datagram */

enum {
    RDRN_HELLO = 1, RDRN_PAD = 2, RDRN_BYE = 3,
    RDRN_WELCOME = 10, RDRN_FULL = 11, RDRN_FRAME = 12, RDRN_AUDIO = 13
};

/* sockets (rdr_net.c) */
typedef struct { uint8_t ip[4]; uint16_t port; } RdrAddr;
int rdr_sock_open(uint16_t port);                     /* bound UDP socket, non-blocking; <0 on failure */
int rdr_sock_send(int s, const RdrAddr *to, const void *p, int n);
int rdr_sock_recv(int s, RdrAddr *from, void *p, int max);   /* <=0: nothing */
void rdr_sock_close(int s);
int rdr_addr_parse(const char *text, RdrAddr *out);   /* "host[:port]" */
int rdr_addr_same(const RdrAddr *a, const RdrAddr *b);
void rdr_net_sleep_ms(int ms);
uint64_t rdr_net_now_ms(void);
int rdr_net_local_ips(uint8_t out[][4], int max);
int rdr_net_stun(int sock, const char *server, RdrAddr *out, int timeout_ms);

/* the online co-op lobby (rdr_lobby.c): the desk's Online Co-op page. The
 * players meet here; when the host starts, every machine starts the game
 * again in lockstep (rdr_lockstep.c) with the host's save and options. */
#define RDR_LOBBY_PORT 27032
typedef struct {
    int state;                  /* 0 off, 1 hosting, 2 joining, 3 joined, 4 starting, 5 failed */
    int players;                /* the game's players (host: chosen) */
    int me;                     /* my player number - 1 */
    char status[160];
    char names[4][33];
    int present[4], ready[4];
    int ncodes;
    char codes[6][40];          /* host: the join code ("ABCDE-FGHJK-MN"), one ... */
    char where[6][48];          /* ... its label */
    char reach[128];            /* where the code works */
} RdrLobbyView;
void rdr_lobby_host(int players, const char *name);
void rdr_lobby_join(const char *code_or_address, const char *name);
void rdr_lobby_set_players(int players);
void rdr_lobby_set_start(int mode);    /* the host's "Start in": 0 menus, 1 continue, 2 new game */
int rdr_lobby_get_start(void);
int  rdr_lobby_can_start(void);
void rdr_lobby_start(void);
void rdr_lobby_leave(void);
void rdr_lobby_view(RdrLobbyView *v);
int  rdr_online_session(void);          /* 1 in a game started from the lobby */
void rdr_online_leave_session(void);    /* back to this machine's own game */
void rdr_online_apply_settings(void *settings);  /* main.c: the host's options over ours */

/* the host (rdr_net_host.c) */
void rdr_net_host_tick(void);           /* each game frame: guests in, pads */
int rdr_net_host_active(void);
/* a guest's pad as the XDK calls see it (rdr_hooks.c, XInput wrappers):
 * the port it has, its state, and whether a pad came or went */
int rdr_net_pad_port_taken(int port);
int rdr_net_pad_state(int port, uint8_t st[22]);
void rdr_net_pad_changes(uint32_t *ins, uint32_t *rem);
/* the picture, from the renderer's present (GL context current) */
void rdr_net_host_present(int w, int h);
/* the sound, from the mixer: s16 stereo */
void rdr_net_host_audio(const int16_t *pcm, int frames, int rate);

/* the guest (rdr_viewer.c): runs instead of the game, returns the exit code */
int rdr_viewer_main(const char *host, const char *name);

#ifdef __cplusplus
}
#endif
#endif
