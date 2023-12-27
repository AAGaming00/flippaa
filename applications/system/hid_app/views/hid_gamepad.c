#include "hid_gamepad.h"
#include <furi.h>
#include <gui/elements.h>
#include <gui/icon_i.h>
#include "../hid.h"
#include "hid_icons.h"

#define TAG "HidGamepad"

struct HidGamepad {
    View* view;
    Hid* hid;
};

typedef struct {
    bool btn[16];
    uint8_t  HAT;    // HAT switch; one nibble w/ unused nibble
	uint8_t  LX;     // Left  Stick X
	uint8_t  LY;     // Left  Stick Y
	uint8_t  RX;     // Right Stick X
	uint8_t  RY;     // Right Stick Y
	uint8_t  LS;     // Right Shoulder
	uint8_t  RS;     // Right Stick
    uint8_t x;
    uint8_t y;
    uint8_t last_key;
    bool ok_pressed;
    bool back_pressed;
    bool connected;
    HidTransport transport;
} HidGamepadModel;

typedef struct {
    uint8_t width;
    char* name;
    const Icon* icon;
    uint8_t value;
} HidGamepadButton;

typedef struct {
    int8_t x;
    int8_t y;
} HidGamepadPoint;
// 4 BY 12
#define MARGIN_TOP 0
#define MARGIN_LEFT 4
#define KEY_WIDTH 9
#define KEY_HEIGHT 12
#define KEY_PADDING 1
#define ROW_COUNT 7
#define COLUMN_COUNT 12

// 0 width items are not drawn, but there value is used
const HidGamepadButton hid_gamepad_keyset[ROW_COUNT][COLUMN_COUNT] = {
    {
        {.width = 1, .icon = NULL, .name = "1", .value = HID_BUTTON_1},
        {.width = 1, .icon = NULL, .name = "2", .value = HID_BUTTON_2},
        {.width = 1, .icon = NULL, .name = "3", .value = HID_BUTTON_3},
        {.width = 1, .icon = NULL, .name = "4", .value = HID_BUTTON_4},
        {.width = 1, .icon = NULL, .name = "5", .value = HID_BUTTON_5},
        {.width = 1, .icon = NULL, .name = "6", .value = HID_BUTTON_6},
        {.width = 1, .icon = NULL, .name = "7", .value = HID_BUTTON_7},
        {.width = 1, .icon = NULL, .name = "8", .value = HID_BUTTON_8},
        {.width = 1, .icon = NULL, .name = "9", .value = HID_BUTTON_9},
        {.width = 1, .icon = NULL, .name = "10", .value = HID_BUTTON_10},
        {.width = 1, .icon = NULL, .name = "11", .value = HID_BUTTON_11},
        {.width = 1, .icon = NULL, .name = "12", .value = HID_BUTTON_12},
    },
    {
        {.width = 1, .icon = NULL, .name = "13", .value = HID_BUTTON_13},
        {.width = 1, .icon = NULL, .name = "14", .value = HID_BUTTON_14},
        {.width = 1, .icon = NULL, .name = "15", .value = HID_BUTTON_15},
        {.width = 1, .icon = NULL, .name = "16", .value = HID_BUTTON_16},
    }
};

// static void hid_gamepad_to_upper(char* str) {
//     while(*str) {
//         *str = toupper((unsigned char)*str);
//         str++;
//     }
// }

static void hid_gamepad_draw_key(
    Canvas* canvas,
    HidGamepadModel* model,
    uint8_t x,
    uint8_t y,
    HidGamepadButton key,
    bool selected) {
    UNUSED(model);
    if(!key.width) return;

    canvas_set_color(canvas, ColorBlack);
    uint8_t keyWidth = KEY_WIDTH * key.width + KEY_PADDING * (key.width - 1);
    if(selected) {
        // Draw a filled box
        elements_slightly_rounded_box(
            canvas,
            MARGIN_LEFT + x * (KEY_WIDTH + KEY_PADDING),
            MARGIN_TOP + y * (KEY_HEIGHT + KEY_PADDING),
            keyWidth,
            KEY_HEIGHT);
        canvas_set_color(canvas, ColorWhite);
    } else {
        // Draw a framed box
        elements_slightly_rounded_frame(
            canvas,
            MARGIN_LEFT + x * (KEY_WIDTH + KEY_PADDING),
            MARGIN_TOP + y * (KEY_HEIGHT + KEY_PADDING),
            keyWidth,
            KEY_HEIGHT);
    }
    if(key.icon != NULL) {
        // Draw the icon centered on the button
        canvas_draw_icon(
            canvas,
            MARGIN_LEFT + x * (KEY_WIDTH + KEY_PADDING) + keyWidth / 2 - key.icon->width / 2,
            MARGIN_TOP + y * (KEY_HEIGHT + KEY_PADDING) + KEY_HEIGHT / 2 - key.icon->height / 2,
            key.icon);
    } else {
        canvas_draw_str_aligned(
            canvas,
            MARGIN_LEFT + x * (KEY_WIDTH + KEY_PADDING) + keyWidth / 2 + 1,
            MARGIN_TOP + y * (KEY_HEIGHT + KEY_PADDING) + KEY_HEIGHT / 2,
            AlignCenter,
            AlignCenter,
            key.name);
    }
}

static void hid_gamepad_draw_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    HidGamepadModel* model = context;

    // Header
    if((!model->connected) && (model->transport == HidTransportBle)) {
        canvas_draw_icon(canvas, 0, 0, &I_Ble_disconnected_15x15);
        canvas_set_font(canvas, FontPrimary);
        elements_multiline_text_aligned(canvas, 17, 3, AlignLeft, AlignTop, "Keyboard");

        canvas_draw_icon(canvas, 68, 3, &I_Pin_back_arrow_10x8);
        canvas_set_font(canvas, FontSecondary);
        elements_multiline_text_aligned(canvas, 127, 4, AlignRight, AlignTop, "Hold to exit");

        elements_multiline_text_aligned(
            canvas, 4, 60, AlignLeft, AlignBottom, "Waiting for Connection...");
        return; // Dont render the gamepad if we are not yet connected
    }

    canvas_set_font(canvas, FontKeyboard);
    // Start shifting the all keys up if on the next row (Scrolling)
    uint8_t initY = model->y == 0 ? 0 : 1;

    if(model->y > 5) {
        initY = model->y - 4;
    }

    for(uint8_t y = initY; y < ROW_COUNT; y++) {
        const HidGamepadButton* gamepadKeyRow = hid_gamepad_keyset[y];
        uint8_t x = 0;
        for(uint8_t i = 0; i < COLUMN_COUNT; i++) {
            HidGamepadButton key = gamepadKeyRow[i];
            // Select when the button is hovered
            // Select if the button is hovered within its width
            // Select if back is clicked and its the backspace key
            // Deselect when the button clicked or not hovered
            bool keySelected = (x <= model->x && model->x < (x + key.width)) && y == model->y;
            bool backSelected = model->back_pressed && key.value == HID_KEYBOARD_DELETE;
            hid_gamepad_draw_key(
                canvas,
                model,
                x,
                y - initY,
                key,
                (!model->ok_pressed && keySelected) || backSelected);
            x += key.width;
        }
    }
}

static uint8_t hid_gamepad_get_selected_key(HidGamepadModel* model) {
    HidGamepadButton key = hid_gamepad_keyset[model->y][model->x];
    return key.value;
}

static void hid_gamepad_get_select_key(HidGamepadModel* model, HidGamepadPoint delta) {
    // Keep going until a valid spot is found, this allows for nulls and zero width keys in the map
    do {
        const int delta_sum = model->y + delta.y;
        model->y = delta_sum < 0 ? ROW_COUNT - 1 : delta_sum % ROW_COUNT;
    } while(delta.y != 0 && hid_gamepad_keyset[model->y][model->x].value == 0);

    do {
        const int delta_sum = model->x + delta.x;
        model->x = delta_sum < 0 ? COLUMN_COUNT - 1 : delta_sum % COLUMN_COUNT;
    } while(delta.x != 0 && hid_gamepad_keyset[model->y][model->x].width ==
                                0); // Skip zero width keys, pretend they are one key
}

static void hid_gamepad_process(HidGamepad* hid_gamepad, InputEvent* event) {
    with_view_model(
        hid_gamepad->view,
        HidGamepadModel * model,
        {
            if(event->key == InputKeyOk) {
                if(event->type == InputTypePress) {
                    model->ok_pressed = true;
                    model->last_key = hid_gamepad_get_selected_key(model);
                    hid_hal_gamepad_press(
                        hid_gamepad->hid, model->last_key);
                } else if(event->type == InputTypeRelease) {
                    // Release happens after short and long presses
                    hid_hal_gamepad_release(
                        hid_gamepad->hid, model->last_key);
                    model->ok_pressed = false;
                }
            } else if(event->key == InputKeyBack) {
                // If back is pressed for a short time, backspace
                if(event->type == InputTypePress) {
                    model->back_pressed = true;
                // } else if(event->type == InputTypeShort) {
                //     hid_hal_gamepad_press(hid_gamepad->hid, HID);
                //     hid_hal_gamepad_release(hid_gamepad->hid, HID_KEYBOARD_DELETE);
                // } 
                } else if(event->type == InputTypeRelease) {
                    model->back_pressed = false;
                }
            } else if(event->type == InputTypePress || event->type == InputTypeRepeat) {
                // Cycle the selected keys
                if(event->key == InputKeyUp) {
                    hid_gamepad_get_select_key(model, (HidGamepadPoint){.x = 0, .y = -1});
                } else if(event->key == InputKeyDown) {
                    hid_gamepad_get_select_key(model, (HidGamepadPoint){.x = 0, .y = 1});
                } else if(event->key == InputKeyLeft) {
                    hid_gamepad_get_select_key(model, (HidGamepadPoint){.x = -1, .y = 0});
                } else if(event->key == InputKeyRight) {
                    hid_gamepad_get_select_key(model, (HidGamepadPoint){.x = 1, .y = 0});
                }
            }
        },
        true);
}

static bool hid_gamepad_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    HidGamepad* hid_gamepad = context;
    bool consumed = false;

    if(event->type == InputTypeLong && event->key == InputKeyBack) {
        hid_hal_gamepad_release_all(hid_gamepad->hid);
    } else {
        hid_gamepad_process(hid_gamepad, event);
        consumed = true;
    }

    return consumed;
}

HidGamepad* hid_gamepad_alloc(Hid* bt_hid) {
    HidGamepad* hid_gamepad = malloc(sizeof(HidGamepad));
    hid_gamepad->view = view_alloc();
    hid_gamepad->hid = bt_hid;
    view_set_context(hid_gamepad->view, hid_gamepad);
    view_allocate_model(hid_gamepad->view, ViewModelTypeLocking, sizeof(HidGamepadModel));
    view_set_draw_callback(hid_gamepad->view, hid_gamepad_draw_callback);
    view_set_input_callback(hid_gamepad->view, hid_gamepad_input_callback);

    with_view_model(
        hid_gamepad->view,
        HidGamepadModel * model,
        {
            model->transport = bt_hid->transport;
            model->y = 1;
        },
        true);

    return hid_gamepad;
}

void hid_gamepad_free(HidGamepad* hid_gamepad) {
    furi_assert(hid_gamepad);
    view_free(hid_gamepad->view);
    free(hid_gamepad);
}

View* hid_gamepad_get_view(HidGamepad* hid_gamepad) {
    furi_assert(hid_gamepad);
    return hid_gamepad->view;
}

void hid_gamepad_set_connected_status(HidGamepad* hid_gamepad, bool connected) {
    furi_assert(hid_gamepad);
    with_view_model(
        hid_gamepad->view, HidGamepadModel * model, { model->connected = connected; }, true);
}
