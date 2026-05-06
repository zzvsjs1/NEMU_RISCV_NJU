#include <device/map.h>
#include <utils.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_TARGET_AM
#include <SDL2/SDL.h>
#endif

enum {
  MOUSE_EVENT_NONE = 0,
  MOUSE_EVENT_MOVE = 1,
  MOUSE_EVENT_BUTTON_DOWN = 2,
  MOUSE_EVENT_BUTTON_UP = 3,
  MOUSE_EVENT_WHEEL = 4,
};

enum {
  MOUSE_BUTTON_NONE = 0,
  MOUSE_BUTTON_LEFT = 1,
  MOUSE_BUTTON_MIDDLE = 2,
  MOUSE_BUTTON_RIGHT = 3,
  MOUSE_BUTTON_WHEELUP = 4,
  MOUSE_BUTTON_WHEELDOWN = 5,
};

enum {
  MOUSE_BUTTON_LEFT_MASK = 1u << 0,
  MOUSE_BUTTON_MIDDLE_MASK = 1u << 1,
  MOUSE_BUTTON_RIGHT_MASK = 1u << 2,
};

#define MOUSE_SUPPORTED_BUTTON_MASK \
  (MOUSE_BUTTON_LEFT_MASK | MOUSE_BUTTON_MIDDLE_MASK | MOUSE_BUTTON_RIGHT_MASK)

typedef struct {
  uint32_t type;
  uint32_t x;
  uint32_t y;
  uint32_t button;
  uint32_t buttons;
  int32_t wheel_x;
  int32_t wheel_y;
} MouseEvent;

#define MOUSE_QUEUE_LEN 1024

static MouseEvent mouse_queue[MOUSE_QUEUE_LEN];
static int mouse_f = 0;
static int mouse_r = 0;
static int mouse_count = 0;
static MouseEvent mouse_latched;
static uint32_t *mouse_base = NULL;
static uint32_t mouse_x = 0;
static uint32_t mouse_y = 0;
static uint32_t mouse_buttons = 0;

static uint32_t mouse_button_bit(uint32_t button)
{
  switch (button)
  {
    case MOUSE_BUTTON_LEFT: return MOUSE_BUTTON_LEFT_MASK;
    case MOUSE_BUTTON_MIDDLE: return MOUSE_BUTTON_MIDDLE_MASK;
    case MOUSE_BUTTON_RIGHT: return MOUSE_BUTTON_RIGHT_MASK;
    default: return 0;
  }
}

static uint32_t normalise_mouse_buttons(uint32_t buttons)
{
  return buttons & MOUSE_SUPPORTED_BUTTON_MASK;
}

static void mouse_enqueue(MouseEvent event)
{
  /*
   * Keep an explicit occupancy count so front == rear can represent both
   * "empty" and "all 1024 slots are full" without sacrificing one slot.
   */
  Assert(mouse_count < MOUSE_QUEUE_LEN, "mouse queue overflow!");
  mouse_queue[mouse_r] = event;
  mouse_r = (mouse_r + 1) % MOUSE_QUEUE_LEN;
  mouse_count ++;
}

static MouseEvent mouse_dequeue(void)
{
  if (mouse_count == 0)
  {
    return (MouseEvent) { .type = MOUSE_EVENT_NONE };
  }

  MouseEvent event = mouse_queue[mouse_f];
  mouse_f = (mouse_f + 1) % MOUSE_QUEUE_LEN;
  mouse_count --;
  return event;
}

static uint32_t sdl_button_to_mouse(uint8_t button)
{
#ifndef CONFIG_TARGET_AM
  switch (button)
  {
    case SDL_BUTTON_LEFT: return MOUSE_BUTTON_LEFT;
    case SDL_BUTTON_MIDDLE: return MOUSE_BUTTON_MIDDLE;
    case SDL_BUTTON_RIGHT: return MOUSE_BUTTON_RIGHT;
    default: return MOUSE_BUTTON_NONE;
  }
#else
  return MOUSE_BUTTON_NONE;
#endif
}

void send_mouse_motion(int x, int y, uint32_t buttons)
{
  mouse_x = x < 0 ? 0 : (uint32_t)x;
  mouse_y = y < 0 ? 0 : (uint32_t)y;
  mouse_buttons = normalise_mouse_buttons(buttons);

  mouse_enqueue((MouseEvent) {
    .type = MOUSE_EVENT_MOVE,
    .x = mouse_x,
    .y = mouse_y,
    .button = MOUSE_BUTTON_NONE,
    .buttons = mouse_buttons,
  });
}

void send_mouse_button(uint8_t sdl_button, bool is_down, int x, int y)
{
  const uint32_t button = sdl_button_to_mouse(sdl_button);
  if (button == MOUSE_BUTTON_NONE) return;

  mouse_x = x < 0 ? 0 : (uint32_t)x;
  mouse_y = y < 0 ? 0 : (uint32_t)y;

  const uint32_t bit = mouse_button_bit(button);
  if (is_down) mouse_buttons |= bit;
  else mouse_buttons &= ~bit;

  mouse_enqueue((MouseEvent) {
    .type = is_down ? MOUSE_EVENT_BUTTON_DOWN : MOUSE_EVENT_BUTTON_UP,
    .x = mouse_x,
    .y = mouse_y,
    .button = button,
    .buttons = mouse_buttons,
  });
}

void send_mouse_wheel(int dx, int dy)
{
  uint32_t button = MOUSE_BUTTON_NONE;
  if (dy > 0) button = MOUSE_BUTTON_WHEELUP;
  else if (dy < 0) button = MOUSE_BUTTON_WHEELDOWN;

  mouse_enqueue((MouseEvent) {
    .type = MOUSE_EVENT_WHEEL,
    .x = mouse_x,
    .y = mouse_y,
    .button = button,
    .buttons = mouse_buttons,
    .wheel_x = dx,
    .wheel_y = dy,
  });
}

static void mouse_data_io_handler(uint32_t offset, int len, bool is_write)
{
  assert(!is_write);
  assert(len == 4);

  /*
   * AM reads the type first and then six payload words.  Latching on the type
   * read keeps those words from being mixed with a later SDL or scripted event.
   */
  if (offset == 0)
  {
    mouse_latched = mouse_dequeue();
  }

  switch (offset)
  {
    case 0x00: mouse_base[0] = mouse_latched.type; break;
    case 0x04: mouse_base[1] = mouse_latched.x; break;
    case 0x08: mouse_base[2] = mouse_latched.y; break;
    case 0x0c: mouse_base[3] = mouse_latched.button; break;
    case 0x10: mouse_base[4] = mouse_latched.buttons; break;
    case 0x14: mouse_base[5] = (uint32_t)mouse_latched.wheel_x; break;
    case 0x18: mouse_base[6] = (uint32_t)mouse_latched.wheel_y; break;
    default: assert(0);
  }
}

#ifndef CONFIG_TARGET_AM
static bool script_button_to_sdl(const char *button, uint8_t *sdl_button)
{
  if (strcmp(button, "left") == 0)
  {
    *sdl_button = SDL_BUTTON_LEFT;
    return true;
  }
  if (strcmp(button, "middle") == 0)
  {
    *sdl_button = SDL_BUTTON_MIDDLE;
    return true;
  }
  if (strcmp(button, "right") == 0)
  {
    *sdl_button = SDL_BUTTON_RIGHT;
    return true;
  }

  return false;
}

static void enqueue_script_event(const char *token)
{
  int x = 0;
  int y = 0;
  int dx = 0;
  int dy = 0;
  char button[16] = {};

  if (sscanf(token, "move:%d,%d", &x, &y) == 2)
  {
    send_mouse_motion(x, y, mouse_buttons);
  }
  else if (sscanf(token, "down:%15[^;]", button) == 1)
  {
    uint8_t sdl_button = 0;
    if (script_button_to_sdl(button, &sdl_button))
    {
      send_mouse_button(sdl_button, true, mouse_x, mouse_y);
    }
  }
  else if (sscanf(token, "up:%15[^;]", button) == 1)
  {
    uint8_t sdl_button = 0;
    if (script_button_to_sdl(button, &sdl_button))
    {
      send_mouse_button(sdl_button, false, mouse_x, mouse_y);
    }
  }
  else if (sscanf(token, "wheel:%d,%d", &dx, &dy) == 2)
  {
    send_mouse_wheel(dx, dy);
  }
}

static void load_mouse_script(void)
{
  const char *script = getenv("NEMU_MOUSE_SCRIPT");
  if (script == NULL || script[0] == '\0') return;

  /*
   * strtok() mutates its buffer, so copy the environment variable before
   * splitting it into deterministic events for headless tests.
   */
  char *buf = (char *)malloc(strlen(script) + 1);
  Assert(buf != NULL, "failed to allocate mouse script buffer");
  strcpy(buf, script);

  for (char *token = strtok(buf, ";"); token != NULL; token = strtok(NULL, ";"))
  {
    enqueue_script_event(token);
  }

  free(buf);
}
#else
static void load_mouse_script(void) {}
#endif

void init_mouse(void)
{
  mouse_base = (uint32_t *)new_space(7 * sizeof(uint32_t));
  memset(mouse_base, 0, 7 * sizeof(uint32_t));
  mouse_latched = (MouseEvent) { .type = MOUSE_EVENT_NONE };

  add_mmio_map("mouse", CONFIG_MOUSE_DATA_MMIO, mouse_base,
      7 * sizeof(uint32_t), mouse_data_io_handler);

  load_mouse_script();
}
