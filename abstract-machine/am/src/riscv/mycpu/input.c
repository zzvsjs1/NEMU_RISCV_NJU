#include <am.h>

void __am_input_keybrd(AM_INPUT_KEYBRD_T *kbd) {
  kbd->keydown = 0;
  kbd->keycode = AM_KEY_NONE;
}

void __am_input_mouse(AM_INPUT_MOUSE_T *mouse) {
  mouse->type = AM_MOUSE_NONE;
  mouse->x = 0;
  mouse->y = 0;
  mouse->button = AM_MOUSE_BUTTON_NONE;
  mouse->buttons = 0;
  mouse->wheel_x = 0;
  mouse->wheel_y = 0;
}
