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
static const char *row_set_ids[ROWS] = { "hotkey_save_set", "hotkey_load_set", "hotkey_panel_set", "hotkey_step_set", "screenshot_button_set" };

static int row = 0;
static int capture = -1;            // the row being set from the controller, or -1
static bool capture_armed = false;  // the button that started it has been let go
static uint16_t held_last = 0;
static int held_frames = 0;
static int idle_frames = 0;
static bool release_wait = false;   // after a capture: everything up before the view takes input again
static char message[96] = "";
static int message_frames = 0;
static int ask_row = -1;            // a game's page after a hold: the row waiting for "this game or every game"
static char ask_text[40] = "";


static int rows_shown (menu_t *menu) {
    (void) menu;
    return ROWS;
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
        default: return &menu->settings.ss_key_shot;
    }
}

// the menu's count of every-game sets of a row's hotkey; a game's own key set before the
// last of them no longer counts, so one set for every game reaches every game
static int *menu_set_field (menu_t *menu, int r) {
    switch (r) {
        case ROW_SAVE: return &menu->settings.ss_key_set_save;
        case ROW_LOAD: return &menu->settings.ss_key_set_load;
        case ROW_PANEL: return &menu->settings.ss_key_set_panel;
        case ROW_STEP: return &menu->settings.ss_key_set_step;
        default: return &menu->settings.ss_key_set_shot;
    }
}

// the text a row stands at: the ROM's own when set, else the menu's; *own says which
static const char *row_text (menu_t *menu, int r, bool *own) {
    if (menu->hotkeys_for_rom) {
        const char *t = rom_field(menu, r);
        int *ms = menu_set_field(menu, r);
        if (t[0] && (!ms || (menu->load.rom_info.settings.hotkey_set[r] >= *ms))) {
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
        // acts at the panel's Step speed only, so it may overlap the hotkeys; not the
        // screenshot button, a tap that fires at Step speed too (a shot at every step)
        uint16_t sm = row_mask(menu, ROW_SHOT);
        if (sm && (((sm & m) == m) || ((sm & m) == sm))) {
            return "That sits inside the screenshot button,\nor the screenshot button inside it.";
        }
        return NULL;
    }
    for (int o = ROW_SAVE; o <= ROW_SHOT; o++) {
        uint16_t om = ((o == ROW_STEP) && (r != ROW_SHOT)) ? 0 : row_mask(menu, o);
        if ((o != r) && om && (((om & m) == m) || ((om & m) == om))) {
            return "That sits inside another hotkey,\nor another hotkey inside it.";
        }
    }
    return NULL;
}

// a row's new text for this game (NULL: back to what every game has), with the menu's
// count beside it so the key counts from now on
static void store_rom (menu_t *menu, int r, const char *text) {
    snprintf(rom_field(menu, r), 32, "%s", text ? text : "");
    rom_config_setting_set_text(menu->load.rom_path, row_ids[r], text);
    int *ms = menu_set_field(menu, r);
    if (ms) {
        char n[16];
        menu->load.rom_info.settings.hotkey_set[r] = text ? *ms : 0;
        snprintf(n, sizeof(n), "%d", *ms);
        rom_config_setting_set_text(menu->load.rom_path, row_set_ids[r], text ? n : NULL);
    }
}

// a row's new text for every game (NULL: back to the built-in one): the count goes up, so
// a game's own key from before stops counting; on a game's page that game's own goes too
static void store_menu (menu_t *menu, int r, const char *text) {
    char **m = menu_field(menu, r);
    int *ms = menu_set_field(menu, r);
    if (!m || !ms) {
        return;
    }
    free(*m);
    *m = strdup(text ? text : row_defaults[r]);
    (*ms)++;
    settings_save(&menu->settings);
    if (menu->hotkeys_for_rom && rom_field(menu, r)[0]) {
        store_rom(menu, r, NULL);
    }
}

// a row's new text (NULL: back to the default) on the page's own side
static void store (menu_t *menu, int r, const char *text) {
    if (menu->hotkeys_for_rom) {
        store_rom(menu, r, text);
    } else {
        store_menu(menu, r, text);
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
            } else if (menu->hotkeys_for_rom) {
                // this game only, or every game: asked once everything is let go
                sc64ss_keys_text(now, ask_text, sizeof(ask_text));
                ask_row = capture;
                sound_play_effect(SFX_SETTING);
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

    if (ask_row >= 0) {
        if (menu->actions.enter) {
            store_rom(menu, ask_row, ask_text);
            show_message("Set for this game.");
            sound_play_effect(SFX_SETTING);
            ask_row = -1;
        } else if (menu->actions.lz_context) {
            store_menu(menu, ask_row, ask_text);
            show_message("Set for every game.");
            sound_play_effect(SFX_SETTING);
            ask_row = -1;
        } else if (menu->actions.back) {
            show_message("Left as it was.");
            sound_play_effect(SFX_EXIT);
            ask_row = -1;
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
            show_message("Back to what every game has.");
        } else {
            show_message((row == ROW_SHOT) ? "Screenshot button: none, for every game." : "Back to the built-in setting.");
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
            ? "After the hold, A keeps it for this game\n"
              "and Z gives it to every game. One button\n"
              "or more; a short hold in the game, which\n"
              "never sees a hotkey while it is held, so\n"
              "pick buttons it can spare."
            : "These apply to every game, one given its\n"
              "own before included; a game's own options\n"
              "can set others for it after. One button or\n"
              "more; a short hold in the game, which never\n"
              "sees a hotkey while it is held."
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
    } else if (ask_row >= 0) {
        ui_components_messagebox_draw(
            "%s: %s\n\n"
            "A: this game only\n"
            "Z: every game\n"
            "B: leave it as it was",
            row_names[ask_row],
            ask_text
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
    ask_row = -1;
}

void view_hotkeys_display (menu_t *menu, surface_t *display) {
    process(menu);

    draw(menu, display);
}
