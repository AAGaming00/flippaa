#pragma once

#include <gui/view.h>

typedef struct Hid Hid;
typedef struct HidGamepad HidGamepad;

HidGamepad* hid_gamepad_alloc(Hid* bt_hid);

void hid_gamepad_free(HidGamepad* hid_gamepad);

View* hid_gamepad_get_view(HidGamepad* hid_gamepad);

void hid_gamepad_set_connected_status(HidGamepad* hid_gamepad, bool connected);
