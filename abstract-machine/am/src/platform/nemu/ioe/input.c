#include <am.h>
#include <nemu.h>
#include <stdio.h>

#define KEYDOWN_MASK 0x8000

#define MOUSE_REG_TYPE     0x00
#define MOUSE_REG_X        0x04
#define MOUSE_REG_Y        0x08
#define MOUSE_REG_BUTTON   0x0c
#define MOUSE_REG_BUTTONS  0x10
#define MOUSE_REG_WHEEL_X  0x14
#define MOUSE_REG_WHEEL_Y  0x18

void __am_input_keybrd(AM_INPUT_KEYBRD_T *kbd) 
{
	const uint32_t key = inl(KBD_ADDR);
	kbd->keydown = key & KEYDOWN_MASK;
	kbd->keycode = key & KEYDOWN_MASK ? key ^ KEYDOWN_MASK : key;
}

void __am_input_mouse(AM_INPUT_MOUSE_T *mouse)
{
  mouse->type = inl(MOUSE_ADDR + MOUSE_REG_TYPE);

  if (mouse->type == AM_MOUSE_NONE)
  {
    mouse->x = 0;
    mouse->y = 0;
    mouse->button = AM_MOUSE_BUTTON_NONE;
    mouse->buttons = 0;
    mouse->wheel_x = 0;
    mouse->wheel_y = 0;
    return;
  }

  /*
   * Reading the type register latches a complete event in NEMU. The remaining
   * reads below must therefore come from the same latched event, not subsequent
   * host SDL input.
   */
  mouse->x = inl(MOUSE_ADDR + MOUSE_REG_X);
  mouse->y = inl(MOUSE_ADDR + MOUSE_REG_Y);
  mouse->button = inl(MOUSE_ADDR + MOUSE_REG_BUTTON);
  mouse->buttons = inl(MOUSE_ADDR + MOUSE_REG_BUTTONS);
  mouse->wheel_x = inl(MOUSE_ADDR + MOUSE_REG_WHEEL_X);
  mouse->wheel_y = inl(MOUSE_ADDR + MOUSE_REG_WHEEL_Y);
}
