#include "utils/fs.h"
#include "views.h"
#include <stdlib.h>
#include <string.h>


static void draw (menu_t *menu, surface_t *d) {
    rdpq_attach_clear(d, NULL);
    rdpq_detach_show();
}


void view_startup_init (menu_t *menu) {
#ifdef FEATURE_AUTOLOAD_ROM_ENABLED
    // FIXME: rather than use a controller button, would it be better to use the cart button?
    JOYPAD_PORT_FOREACH (port) {
        joypad_poll();
        joypad_buttons_t b_held = joypad_get_buttons_held(port);

        if (menu->settings.rom_autoload_enabled && b_held.start) {
            menu->settings.rom_autoload_enabled = false;
            // SC64SS: set_autoload_type() frees these, so they stay heap strings.
            free(menu->settings.rom_autoload_path);
            menu->settings.rom_autoload_path = strdup("");
            free(menu->settings.rom_autoload_filename);
            menu->settings.rom_autoload_filename = strdup("");
            settings_save(&menu->settings);
        }
    }
    // SC64SS: not at a start the routine asked for (Exit to menu, a suspend): the autoload is
    // for power-on and the reset button, and stays set for them
    if (menu->settings.rom_autoload_enabled && !menu->exit_start) {
        menu->browser.directory = path_init(menu->storage_prefix, menu->settings.rom_autoload_path);
        menu->load.rom_path = path_clone_push(menu->browser.directory, menu->settings.rom_autoload_filename);
        menu->load_pending.rom_file = true;
        menu->next_mode = MENU_MODE_LOAD_ROM;

        return;
    }
#endif

    // SC64SS: the reset button restarts the game that was running (Menu settings, Reset Button).
    // A launch notes its ROM; every start takes the note away, and a start after the reset
    // button launches the ROM again as the autoload above does, unless Start is held on the way
    // back up: the menu then. The launch notes it again, so each reset restarts it. The reset
    // button is known from the routine's word (menu.c), not from the reset type, which is warm
    // after a power-on and after Exit to menu as well. A game without the routine leaves no
    // word, and its reset comes to the menu.
    bool reset = menu->reset_start && !menu->exit_start;
    debugf("SC64SS: menu start after a %s reset%s%s, restart note '%s'\n", (sys_reset_type() == RESET_WARM) ? "warm" : "cold",
           menu->exit_start ? " (Exit to menu)" : "", menu->reset_start ? " (the reset button)" : "",
           menu->settings.reset_rom_path);
    if (menu->settings.reset_rom_path[0] != '\0') {
        char *rom = menu->settings.reset_rom_path;
        bool start_held = false;
        joypad_poll();
        JOYPAD_PORT_FOREACH (port) {
            if (joypad_get_buttons_held(port).start) {
                start_held = true;
            }
        }
        menu->settings.reset_rom_path = strdup("");
        settings_save(&menu->settings);
        if (reset && menu->settings.reset_restarts_rom && !start_held) {
            menu->load.rom_path = path_create(rom);
            menu->browser.directory = path_clone(menu->load.rom_path);
            path_pop(menu->browser.directory);
            menu->load_pending.rom_file = true;
            menu->next_mode = MENU_MODE_LOAD_ROM;
            free(rom);
            return;
        }
        free(rom);
    }

    if (menu->settings.first_run) {
        menu->settings.first_run = false;
        settings_save(&menu->settings);
        menu->next_mode = MENU_MODE_CREDITS;
    }
    else {
        menu->next_mode = MENU_MODE_BROWSER;
    }
}

void view_startup_display (menu_t *menu, surface_t *display) {
    draw(menu, display);
}
