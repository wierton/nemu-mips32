#include <SDL/SDL.h>
#include <ctype.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

#include "device.h"
#include "events.h"
#include "utils.h"

void check_cp0_timer();

SDL_Surface *screen;
static struct itimerval it;

static event_t events[NR_EVENTS];

void event_bind_handler(int event_type, event_handler_t handler) {
  assert(handler);
  assert(0 <= event_type && event_type < NR_EVENTS);

  event_t *evt = &events[event_type];
  assert(!evt->handler);
  evt->handler = handler;
}

int notify_event(int event_type, void *data, int len) {
  assert(0 <= event_type && event_type < NR_EVENTS);

  event_t *evt = &events[event_type];
  return evt->handler(data, len);
}

void detect_sdl_event() {
  SDL_Event event = {0};
  if (!SDL_PollEvent(&event)) return;

  int sdlk_data[2] = {event.type, event.key.keysym.sym};
  switch (event.type) {
  /* If a key was pressed */
  case SDL_KEYUP:
    notify_event(EVENT_SDL_KEY_UP, sdlk_data, sizeof(sdlk_data));
    break;
  case SDL_KEYDOWN:
    notify_event(EVENT_SDL_KEY_DOWN, sdlk_data, sizeof(sdlk_data));
    break;
  case SDL_QUIT: nemu_exit();
  default:
    /* do nothing */
    break;
  }
}

void detect_stdin() {
  /* read stdin */
  int n = nchars_stdin();
  if (n > 0) {
    char *buf = malloc(n);
    int ret = read(0, buf, n);
    assert(ret == n);

#if CONFIG_ENABLE_CTRL_C_Z
    for (int i = 0; i < ret; i++) {
      if (buf[i] == '\x01') {
        printf("Ctrl-A exit the nemu\n");
        nemu_exit();
      } else if (!isprint(buf[i]))
        break;
    }
#endif

    notify_event(EVENT_STDIN_DATA, buf, ret);

    free(buf);
  }
}

void update_timer() {
  /* TIMER */
  int ret = setitimer(ITIMER_VIRTUAL, &it, NULL);
  Assert(ret == 0, "Can not set timer");

  notify_event(EVENT_TIMER, NULL, 0);
}

void device_update(int signum) {
  detect_sdl_event();
  detect_stdin();
#if CONFIG_NETWORK
  net_poll_packet();
#endif
#if CONFIG_INTR
  check_cp0_timer();
#endif
}

#if CONFIG_ENABLE_CTRL_C_Z
static void ctrl_code_handler(int no) {
  if (no == SIGINT) {
    /* https://en.wikipedia.org/wiki/Control-C */
    char data = '\x03';
    notify_event(EVENT_CTRL_C, &data, 1);
  } else if (no == SIGTSTP) {
    /* https://en.wikipedia.org/wiki/Substitute_character */
    char data = '\x1a';
    notify_event(EVENT_CTRL_Z, &data, 1);
  }
}
#endif

void init_timer() {
  struct sigaction s;
  memset(&s, 0, sizeof(s));
  s.sa_handler = device_update;
  int ret = sigaction(SIGVTALRM, &s, NULL);
  Assert(ret == 0, "Can not set signal handler");

  it.it_value.tv_sec = 0;
  it.it_value.tv_usec = 1000000 / TIMER_HZ;
  it.it_interval.tv_sec = 0;
  it.it_interval.tv_usec = 0;
  ret = setitimer(ITIMER_VIRTUAL, &it, NULL);
  Assert(ret == 0, "Can not set timer");
}

void init_sdl() {
  /* sdl */
  int ret = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE);
  Assert(ret == 0, "SDL_Init failed");
  screen =
      SDL_SetVideoMode(WINDOW_W, WINDOW_H, 32, SDL_HWSURFACE | SDL_DOUBLEBUF);
  SDL_WM_SetCaption("NEMU-MIPS32", NULL);
  SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);
}

void *event_loop(void *args) {
  while (1) { device_update(0); }
  return NULL;
}

void init_events() {
#if CONFIG_NETWORK
  init_network();
#endif
#if CONFIG_GRAPHICS
  init_sdl();
#endif
  init_console();

#if 1
  pthread_t thd = 0;
  pthread_create(&thd, NULL, event_loop, NULL);
#else
  init_timer();
#endif

#if CONFIG_ENABLE_CTRL_C_Z
  signal(SIGINT, ctrl_code_handler);
  signal(SIGTSTP, ctrl_code_handler);
#endif
}
