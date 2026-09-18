/**
 * @file hotkeys.c
 * @brief The save state hotkeys view: the quick save, quick load, slot panel, frame step
 *        and screenshot buttons, for the menu as a whole or for one ROM.
 * @ingroup view
 *
 * A row is set by holding the buttons on the controller for a moment; the menu's own
 * actions are off while that goes on, and letting go of everything cancels. The ROM's
 * own settings live in its ini (hotkey_save and friends, screenshot_button); the
 * menu's in config.ini. A hotkey is one button or more and may not sit inside another
 * (the frame step button aside: it acts at the panel's Step speed only); the game never
 * sees a hotkey's buttons while it is held. L + R + Start is refused: an original
 * controller answers it with its stick reset (Start reported released, the reset flag
 * set), so it could never match.
 */

#include <libdragon.h>
#include <stdio.h>
#include <string.h>
#include "../rom_info.h"
#include "../settings.h"
#include "../sound.h"
#include "views.h"

enum { ROW_SAVE, ROW_LOAD, ROW_PANEL, ROW_STEP, ROW_SHOT, ROWS };

static const char *row_names[ROWS] = { "Quick save", "Quick load", "Slot panel", "Frame step", "Screenshot" };
static const char *row_defaults[ROWS] = { SC64SS_KEY_DEFAULT_SAVE, SC64SS_KEY_DEFAULT_LOAD, SC64SS_KEY_DEFAULT_PANEL, SC64SS_KEY_DEFAULT_STEP, "" };
static const char *row_ids[ROWS] = { "hotkey_save", "hotkey_load", "hotkey_panel", "hotkey_step", "screenshot_button" };

static int row = 0;
static int capture = -1;            // the row being set from the controller, or -1
static bool capture_armed = false;  // the button that started it has been let go
static uint16_t held_last = 0;
static int held_frames = 0;
static int idle_frames = 0;
static bool release_wait = false;   // after a capture: everything up before the view takes input again
static char message[96] = "";
static int message_frames = 0;


static int rows_shown (menu_t *menu) {
    return menu->hotkeys_for_rom ? ROWS : (ROWS - 1);   // the screenshot button is a game's own
}

static uint16_t buttons_now (void) {
    uint16_t raw = joypad_get_buttons_held(JOYPAD_PORT_1).raw;
    if (raw & 0x0080) {
        raw |= 0x1000;              // the reset flag: an original controller under L + R + Start
    }
    return raw & 0xFF3F;            // never the reset bit or the unused one
}

static int popcount16 (uint16_t v) {
    int n = 0;
    while (v) {
        n += (v & 1);
        v >>= 1;
    }
    return n;
}

static char *rom_field (menu_t *menu, int r) {
    switch (r) {
        case ROW_SAVE: return menu->load.rom_info.settings.hotkey_save;
        case ROW_LOAD: return menu->load.rom_info.settings.hotkey_load;
        case ROW_PANEL: return menu->load.rom_info.settings.hotkey_panel;
        case ROW_STEP: return menu->load.rom_info.settings.hotkey_step;
        default: return menu->load.rom_info.settings.screenshot_button;
    }
}

static char **menu_field (menu_t *menu, int r) {
    switch (r) {
        case ROW_SAVE: return &menu->settings.ss_key_save;
        case ROW_LOAD: return &menu->settings.ss_key_load;
        case ROW_PANEL: return &menu->settings.ss_key_panel;
        case ROW_STEP: return &menu->settings.ss_key_step;
        default: return NULL;
    }
}

// the text a row stands at: the ROM's own when set, else the menu's; *own says which
static const char *row_text (menu_t *menu, int r, bool *own) {
    if (menu->hotkeys_for_rom) {
        const char *t = rom_field(menu, r);
        if (t[0]) {
            *own = true;
            return t;
        }
    }
    *own = false;
    char **m = menu_field(menu, r);
    return (m && *m) ? *m : "";
}

static uint16_t row_mask (menu_t *menu, int r) {
    bool own;
    uint16_t m = sc64ss_keys_parse(row_text(menu, r, &own));
    if (!m && (r != ROW_SHOT)) {
        m = sc64ss_keys_parse(row_defaults[r]);   // a setting that reads as nothing: the built-in one
    }
    return m;
}

static void show_message (const char *text) {
    snprintf(message, sizeof(message), "%s", text);
    message_frames = 150;
}

// a new setting for a row against the others: NULL when it is fine, else why not
static const char *check_mask (menu_t *menu, int r, uint16_t m) {
    if (!m) {
        return "Nothing held.";
    }
    if ((m & 0x1030) == 0x1030) {
        return "L + R + Start is an original controller's\nstick reset: it never reaches a game.";
    }
    if (r == ROW_STEP) {
        return NULL;                // acts at the panel's Step speed only: free to overlap
    }
    for (int o = ROW_SAVE; o <= ROW_SHOT; o++) {
        uint16_t om = (o == ROW_STEP) ? 0 : row_mask(menu, o);
        if ((o != r) && om && (((om & m) == m) || ((om & m) == om))) {
            return "That sits inside another hotkey,\nor another hotkey inside it.";
        }
    }
    return NULL;
}

// a row's new text (NULL: back to the default)
static void store (menu_t *menu, int r, const char *text) {
    if (menu->hotkeys_for_rom) {
        snprintf(rom_field(menu, r), 32, "%s", text ? text : "");
        rom_config_setting_set_text(menu->load.rom_path, row_ids[r], text);
    } else {
        char **m = menu_field(menu, r);
        if (!m) {
            return;
        }
        free(*m);
        *m = strdup(text ? text : row_defaults[r]);
        settings_save(&menu->settings);
    }
}

static void process (menu_t *menu) {
    uint16_t now = buttons_now();

    if (capture >= 0) {
        // the row's buttons, straight from the controller
        if (!capture_armed) {
            if (now == 0) {
                capture_armed = true;
            }
            return;
        }
        if (now == 0) {
            held_frames = 0;
            held_last = 0;
            if (++idle_frames > 240) {
                capture = -1;
                show_message("Nothing held: left as it was.");
            }
            return;
        }
        idle_frames = 0;
        if (now == held_last) {
            held_frames++;
        } else {
            held_last = now;
            held_frames = 0;
        }
        if (held_frames == ((popcount16(now) > 1) ? 45 : 20)) {
            const char *why = check_mask(menu, capture, now);
            if (why) {
                show_message(why);
                sound_play_effect(SFX_ERROR);
            } else {
                char t[40];
                sc64ss_keys_text(now, t, sizeof(t));
                store(menu, capture, t);
                show_message("Set.");
                sound_play_effect(SFX_SETTING);
            }
            capture = -1;
            release_wait = true;
        }
        return;
    }

    if (release_wait) {
        if (now == 0) {
            release_wait = false;
        }
        return;
    }

    if (message_frames && (menu->actions.enter || menu->actions.back)) {
        message_frames = 0;
        return;
    }

    if (menu->actions.go_up) {
        row = (row + rows_shown(menu) - 1) % rows_shown(menu);
        sound_play_effect(SFX_CURSOR);
    } else if (menu->actions.go_down) {
        row = (row + 1) % rows_shown(menu);
        sound_play_effect(SFX_CURSOR);
    } else if (menu->actions.enter) {
        capture = row;
        capture_armed = false;
        held_last = 0;
        held_frames = 0;
        idle_frames = 0;
        sound_play_effect(SFX_ENTER);
    } else if (menu->actions.options) {
        store(menu, row, NULL);
        if (menu->hotkeys_for_rom) {
            show_message((row == ROW_SHOT) ? "Screenshot button: none." : "Back to the menu's setting.");
        } else {
            show_message("Back to the built-in setting.");
        }
        sound_play_effect(SFX_SETTING);
    } else if (menu->actions.back) {
        sound_play_effect(SFX_EXIT);
        menu->next_mode = menu->hotkeys_for_rom ? MENU_MODE_LOAD_ROM : MENU_MODE_SETTINGS_EDITOR;
    }
}

static void draw (menu_t *menu, surface_t *d) {
    char body[640], t[40];
    size_t used = 0;

    rdpq_attach(d, NULL);

    ui_components_background_draw();

    ui_components_layout_draw();

    ui_components_main_text_draw(
        STL_DEFAULT,
        ALIGN_CENTER, VALIGN_TOP,
        "SAVE STATE HOTKEYS\n"
        "%.40s\n",
        menu->hotkeys_for_rom ? menu->load.rom_info.title : "for every game"
    );

    body[0] = 0;
    for (int r = 0; r < rows_shown(menu); r++) {
        bool own = false;
        const char *text = row_text(menu, r, &own);
        uint16_t m = sc64ss_keys_parse(text);
        const char *tag = "";
        if (menu->hotkeys_for_rom) {
            tag = own ? "  (this game)" : "";
        }
        if ((r == ROW_SHOT) && !m) {
            snprintf(t, sizeof(t), "None");
        } else if (!m) {
            sc64ss_keys_text(sc64ss_keys_parse(row_defaults[r]), t, sizeof(t));
        } else {
            sc64ss_keys_text(m, t, sizeof(t));
        }
        used += snprintf(body + used, (used < sizeof(body)) ? (sizeof(body) - used) : 0,
                         "%s %s:\t\t%s%s\n", (r == row) ? ">" : " ", row_names[r], t, tag);
    }

    ui_components_main_text_draw(
        STL_DEFAULT,
        ALIGN_LEFT, VALIGN_TOP,
        "\n\n\n"
        "%s"
        "\n"
        "%s",
        body,
        menu->hotkeys_for_rom
            ? "One button or more; a short hold in the\n"
              "game. The game never sees a hotkey while\n"
              "it is held, so pick buttons it can spare.\n"
              "The frame step button works at the\n"
              "panel's Step speed."
            : "These apply to every game; a game's own\n"
              "options can set others for it.\n"
              "One button or more; a short hold in the\n"
              "game. The game never sees a hotkey while\n"
              "it is held, so pick buttons it can spare."
    );

    ui_components_actions_bar_text_draw(
        STL_DEFAULT,
        ALIGN_LEFT, VALIGN_TOP,
        "A: Set from the controller\n"
        "B: Back"
    );

    ui_components_actions_bar_text_draw(
        STL_DEFAULT,
        ALIGN_RIGHT, VALIGN_TOP,
        "R: Back to the default\n"
        "\n"
    );

    if (capture >= 0) {
        sc64ss_keys_text(held_last, t, sizeof(t));
        ui_components_messagebox_draw(
            "%s\n\n"
            "Hold the buttons for it on the controller.\n\n"
            "%s\n\n"
            "Keep them still for a moment.\n"
            "Let go of everything to cancel.",
            row_names[capture],
            held_last ? t : "..."
        );
    } else if (message_frames) {
        message_frames--;
        ui_components_messagebox_draw("%s", message);
    }

    rdpq_detach_show();
}


void view_hotkeys_init (menu_t *menu) {
    (void) menu;
    row = 0;
    capture = -1;
    release_wait = false;
    message_frames = 0;
}

void view_hotkeys_display (menu_t *menu, surface_t *display) {
    process(menu);

    draw(menu, display);
}
