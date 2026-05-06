#include <amtest.h>

static const char *mouse_type_name(int type)
{
  switch (type)
  {
    case AM_MOUSE_MOVE: return "MOVE";
    case AM_MOUSE_BUTTON_DOWN: return "BUTTON_DOWN";
    case AM_MOUSE_BUTTON_UP: return "BUTTON_UP";
    case AM_MOUSE_WHEEL: return "WHEEL";
    default: return "NONE";
  }
}

void mouse_test(void)
{
  printf("Mouse test: move, click, and wheel the mouse. Press ESC to exit.\n");

  while (1)
  {
    AM_INPUT_KEYBRD_T key = io_read(AM_INPUT_KEYBRD);
    if (key.keydown && key.keycode == AM_KEY_ESCAPE)
    {
      printf("Mouse test exit\n");
      return;
    }

    AM_INPUT_MOUSE_T mouse = io_read(AM_INPUT_MOUSE);
    if (mouse.type != AM_MOUSE_NONE)
    {
      printf("mouse %s x=%d y=%d button=%d buttons=%d wheel=(%d,%d)\n",
          mouse_type_name(mouse.type), mouse.x, mouse.y, mouse.button,
          mouse.buttons, mouse.wheel_x, mouse.wheel_y);
    }
  }
}
