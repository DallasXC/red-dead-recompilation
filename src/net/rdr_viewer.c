/*
 * rdr_viewer.c - online play, the guest's side of "option 2" (streaming).
 *
 *   "Red Dead Revolver.exe" --join <host>[:port] [--name <name>]
 *
 * No game runs here: a window shows the host's picture, plays its sound,
 * and sends this machine's controller (or the keyboard) to the host 60
 * times a second, where it is a pad on the port the host gave it.
 *
 * Keyboard (no controller): WASD move, arrows look, Space A, Left Ctrl B,
 * E X, R Y, Q LT (aim), Left mouse or F RT (fire), Enter Start, Tab Back,
 * C Black, V White, Shift left-stick click. Esc closes the window.
 */
#include "rdr_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(RDR_HAVE_SDL2) || !defined(_WIN32)
#define SDL_MAIN_HANDLED
#include <SDL.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#include "../../third_party/stb/stb_image.h"

typedef struct {
    uint32_t id;
    uint16_t parts, got;
    uint8_t have[1024];
    uint8_t *data;
    int len[1024];
} Assembly;

static void put16(uint8_t *p, int v) { int16_t s = (int16_t)v; memcpy(p, &s, 2); }

static int axis(SDL_GameController *c, SDL_GameControllerAxis a)
{
    return c ? SDL_GameControllerGetAxis(c, a) : 0;
}

/* the XINPUT_STATE the host's game reads (packet, buttons, 8 analog
 * buttons A B X Y Black White LT RT, sticks LX LY RX RY) */
static void read_pad(SDL_GameController *c, uint8_t st[22], uint32_t packet)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint32_t mouse = SDL_GetMouseState(NULL, NULL);
    uint16_t b = 0;
    int lx = 0, ly = 0, rx = 0, ry = 0, i;
    uint8_t an[8] = {0};
    memset(st, 0, 22);
    memcpy(st, &packet, 4);
    if (c) {
        static const SDL_GameControllerButton dp[4] = {SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                                                       SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
        static const SDL_GameControllerButton face[6] = {SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
                                                         SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
                                                         SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
                                                         SDL_CONTROLLER_BUTTON_LEFTSHOULDER};
        for (i = 0; i < 4; i++)
            if (SDL_GameControllerGetButton(c, dp[i]))
                b |= (uint16_t)(1u << i);
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)) b |= 0x10;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK)) b |= 0x20;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK)) b |= 0x40;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) b |= 0x80;
        for (i = 0; i < 6; i++)                 /* bumpers: Black (RB) and White (LB) */
            if (SDL_GameControllerGetButton(c, face[i]))
                an[i] = 255;
        an[6] = (uint8_t)(axis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
        an[7] = (uint8_t)(axis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
        lx = axis(c, SDL_CONTROLLER_AXIS_LEFTX);
        ly = -axis(c, SDL_CONTROLLER_AXIS_LEFTY) - 1;
        rx = axis(c, SDL_CONTROLLER_AXIS_RIGHTX);
        ry = -axis(c, SDL_CONTROLLER_AXIS_RIGHTY) - 1;
    }
    /* the keyboard as well */
    if (k[SDL_SCANCODE_W]) ly = 32767;
    if (k[SDL_SCANCODE_S]) ly = -32768;
    if (k[SDL_SCANCODE_A]) lx = -32768;
    if (k[SDL_SCANCODE_D]) lx = 32767;
    if (k[SDL_SCANCODE_UP]) ry = 32767;
    if (k[SDL_SCANCODE_DOWN]) ry = -32768;
    if (k[SDL_SCANCODE_LEFT]) rx = -32768;
    if (k[SDL_SCANCODE_RIGHT]) rx = 32767;
    if (k[SDL_SCANCODE_SPACE]) an[0] = 255;
    if (k[SDL_SCANCODE_LCTRL]) an[1] = 255;
    if (k[SDL_SCANCODE_E]) an[2] = 255;
    if (k[SDL_SCANCODE_R]) an[3] = 255;
    if (k[SDL_SCANCODE_C]) an[4] = 255;
    if (k[SDL_SCANCODE_V]) an[5] = 255;
    if (k[SDL_SCANCODE_Q] || (mouse & SDL_BUTTON(SDL_BUTTON_RIGHT))) an[6] = 255;
    if (k[SDL_SCANCODE_F] || (mouse & SDL_BUTTON(SDL_BUTTON_LEFT))) an[7] = 255;
    if (k[SDL_SCANCODE_RETURN]) b |= 0x10;
    if (k[SDL_SCANCODE_TAB]) b |= 0x20;
    if (k[SDL_SCANCODE_LSHIFT]) b |= 0x40;
    if (k[SDL_SCANCODE_I]) b |= 1;
    if (k[SDL_SCANCODE_K]) b |= 2;
    if (k[SDL_SCANCODE_J]) b |= 4;
    if (k[SDL_SCANCODE_L]) b |= 8;
    memcpy(st + 4, &b, 2);
    memcpy(st + 6, an, 8);
    put16(st + 14, lx);
    put16(st + 16, ly);
    put16(st + 18, rx);
    put16(st + 20, ry);
}

/* Tests (no person at the window): RDR_VIEWER_LIVE=<file>, lines
 * "<button>:<ms>" appended while it runs (a b x y start back lt rt black
 * white lup ldown lleft lright) hold that input for that long; and
 * RDR_VIEWER_DUMP=<prefix> writes every 30th picture received as a .jpg. */
static uint64_t s_hold_until[16];
static const char *s_hold_names[16] = {"a", "b", "x", "y", "black", "white", "lt", "rt",
                                       "start", "back", "lup", "ldown", "lleft", "lright", NULL, NULL};

static void live_poll(void)
{
    static const char *path = (const char *)-1;
    static long pos;
    FILE *f;
    char line[64];
    if (path == (const char *)-1)
        path = getenv("RDR_VIEWER_LIVE");
    if (!path || !(f = fopen(path, "r")))
        return;
    fseek(f, pos, SEEK_SET);
    while (fgets(line, sizeof line, f)) {
        char *c = strchr(line, ':');
        int i;
        if (!c)
            continue;
        *c = 0;
        for (i = 0; s_hold_names[i]; i++)
            if (!strcmp(line, s_hold_names[i]))
                s_hold_until[i] = rdr_net_now_ms() + (uint64_t)atoi(c + 1);
    }
    pos = ftell(f);
    fclose(f);
}

static void live_apply(uint8_t st[22])
{
    uint64_t now = rdr_net_now_ms();
    uint16_t b;
    int i;
    memcpy(&b, st + 4, 2);
    for (i = 0; i < 8; i++)
        if (s_hold_until[i] > now)
            st[6 + i] = 255;
    if (s_hold_until[8] > now) b |= 0x10;
    if (s_hold_until[9] > now) b |= 0x20;
    memcpy(st + 4, &b, 2);
    if (s_hold_until[10] > now) put16(st + 16, 32767);
    if (s_hold_until[11] > now) put16(st + 16, -32768);
    if (s_hold_until[12] > now) put16(st + 14, -32768);
    if (s_hold_until[13] > now) put16(st + 14, 32767);
}

static void send_msg(int s, const RdrAddr *to, uint8_t type, const void *body, int n)
{
    uint8_t pkt[96];
    uint32_t magic = RDR_NET_MAGIC;
    memcpy(pkt, &magic, 4);
    pkt[4] = type;
    if (n)
        memcpy(pkt + 5, body, (size_t)n);
    rdr_sock_send(s, to, pkt, 5 + n);
}

int rdr_viewer_main(const char *host, const char *name)
{
    RdrAddr ha;
    int s, slot = -1, w = 960, h = 540, running = 1;
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex = NULL;
    SDL_GameController *pad = NULL;
    SDL_AudioDeviceID au = 0;
    Assembly as;
    uint8_t buf[2048];
    uint64_t last_hello = 0, last_pad = 0, last_rx = 0;
    uint32_t packet = 0, shown = 0;
    char title[256];

    if (rdr_addr_parse(host, &ha) != 0) {
        fprintf(stderr, "[NET] cannot find %s\n", host);
        return 1;
    }
    s = rdr_sock_open(0);
    if (s < 0) {
        fprintf(stderr, "[NET] no UDP socket\n");
        return 1;
    }
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[NET] SDL: %s\n", SDL_GetError());
        return 1;
    }
    snprintf(title, sizeof title, "Red Dead Revolver - joining %s", host);
    win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (!ren && win)
        ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) {
        fprintf(stderr, "[NET] no window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    {
        SDL_AudioSpec want, got;
        memset(&want, 0, sizeof want);
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        au = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
        if (au)
            SDL_PauseAudioDevice(au, 0);
    }
    memset(&as, 0, sizeof as);
    as.data = (uint8_t *)malloc(1024 * RDR_NET_PART);

    while (running) {
        SDL_Event e;
        uint64_t now = rdr_net_now_ms();
        int n;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE))
                running = 0;
            else if (e.type == SDL_CONTROLLERDEVICEADDED && !pad)
                pad = SDL_GameControllerOpen(e.cdevice.which);
            else if (e.type == SDL_CONTROLLERDEVICEREMOVED && pad
                     && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
                SDL_GameControllerClose(pad);
                pad = NULL;
            }
        }
        if (slot < 0 && now - last_hello > 500) {
            char nm[32] = {0};
            strncpy(nm, name ? name : "Guest", sizeof nm - 1);
            send_msg(s, &ha, RDRN_HELLO, nm, 32);
            last_hello = now;
        }
        if (slot >= 0 && now - last_pad >= 16) {
            uint8_t body[4 + 22];
            uint8_t st[22];
            read_pad(pad, st, ++packet);
            live_poll();
            live_apply(st);
            memcpy(body, &packet, 4);
            memcpy(body + 4, st, 22);
            send_msg(s, &ha, RDRN_PAD, body, 26);
            last_pad = now;
        }
        while ((n = rdr_sock_recv(s, NULL, buf, sizeof buf)) > 0) {
            uint32_t magic;
            if (n < 5 || (memcpy(&magic, buf, 4), magic != RDR_NET_MAGIC))
                continue;
            last_rx = now;
            if (buf[4] == RDRN_WELCOME && n >= 10) {
                uint16_t sw, sh;
                memcpy(&sw, buf + 6, 2);
                memcpy(&sh, buf + 8, 2);
                if (slot < 0) {
                    fprintf(stderr, "[NET] joined: player on port %d, %ux%u\n", buf[5], sw, sh);
                    snprintf(title, sizeof title, "Red Dead Revolver - %s (player %d)", host, buf[5] + 1);
                    SDL_SetWindowTitle(win, title);
                }
                slot = buf[5];
                w = sw;
                h = sh;
            } else if (buf[4] == RDRN_FULL) {
                SDL_SetWindowTitle(win, "Red Dead Revolver - the host's game is full");
            } else if (buf[4] == RDRN_FRAME && n > 13) {
                uint32_t id;
                uint16_t part, parts;
                memcpy(&id, buf + 5, 4);
                memcpy(&part, buf + 9, 2);
                memcpy(&parts, buf + 11, 2);
                if (parts == 0 || parts > 1024 || part >= parts || id < as.id)
                    continue;
                if (id != as.id) {          /* a newer picture: the older one is given up */
                    as.id = id;
                    as.parts = parts;
                    as.got = 0;
                    memset(as.have, 0, sizeof as.have);
                }
                if (!as.have[part]) {
                    as.have[part] = 1;
                    as.len[part] = n - 13;
                    memcpy(as.data + (size_t)part * RDR_NET_PART, buf + 13, (size_t)(n - 13));
                    as.got++;
                }
                if (as.got == as.parts && id > shown) {
                    int total = (as.parts - 1) * RDR_NET_PART + as.len[as.parts - 1], iw, ih, ic;
                    uint8_t *px = stbi_load_from_memory(as.data, total, &iw, &ih, &ic, 3);
                    shown = id;
                    {
                        static const char *dump = (const char *)-1;
                        if (dump == (const char *)-1)
                            dump = getenv("RDR_VIEWER_DUMP");
                        if (dump && id % 30 == 0) {
                            char path[512];
                            FILE *f;
                            snprintf(path, sizeof path, "%s%06u.jpg", dump, id);
                            if ((f = fopen(path, "wb"))) {
                                fwrite(as.data, 1, (size_t)total, f);
                                fclose(f);
                            }
                        }
                    }
                    if (px) {
                        if (!tex || iw != w || ih != h) {
                            if (tex)
                                SDL_DestroyTexture(tex);
                            w = iw;
                            h = ih;
                            tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, w, h);
                        }
                        SDL_UpdateTexture(tex, NULL, px, w * 3);
                        stbi_image_free(px);
                    }
                }
            } else if (buf[4] == RDRN_AUDIO && n > 15 && au) {
                uint16_t fr;
                memcpy(&fr, buf + 13, 2);
                if (15 + fr * 4 <= n && SDL_GetQueuedAudioSize(au) < 48000 * 4 / 8)   /* under 125 ms queued */
                    SDL_QueueAudio(au, buf + 15, (Uint32)fr * 4u);
            } else if (buf[4] == RDRN_BYE) {
                SDL_SetWindowTitle(win, "Red Dead Revolver - the host left");
                slot = -1;
            }
        }
        if (slot >= 0 && last_rx && now - last_rx > 5000) {
            SDL_SetWindowTitle(win, "Red Dead Revolver - lost the host, trying again");
            slot = -1;
        }
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        if (tex) {
            int ww, wh;
            SDL_Rect r;
            SDL_GetRendererOutputSize(ren, &ww, &wh);
            if ((int64_t)ww * h > (int64_t)wh * w) {      /* the picture's shape kept */
                r.h = wh;
                r.w = (int)((int64_t)wh * w / h);
            } else {
                r.w = ww;
                r.h = (int)((int64_t)ww * h / w);
            }
            r.x = (ww - r.w) / 2;
            r.y = (wh - r.h) / 2;
            SDL_RenderCopy(ren, tex, NULL, &r);
        }
        SDL_RenderPresent(ren);
        rdr_net_sleep_ms(1);
    }
    {
        int i;
        for (i = 0; i < 3; i++)
            send_msg(s, &ha, RDRN_BYE, NULL, 0);
    }
    rdr_sock_close(s);
    SDL_Quit();
    return 0;
}

#else
int rdr_viewer_main(const char *host, const char *name)
{
    (void)host;
    (void)name;
    fprintf(stderr, "[NET] this build has no SDL2: joining needs it\n");
    return 1;
}
#endif
