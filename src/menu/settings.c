#include <libdragon.h>
#include "ini_parser.h"

#include "settings.h"
#include "utils/fs.h"


static char *settings_path = NULL;


static settings_t init = {
    .schema_revision = 1,
    .first_run = true,
    .pal60_enabled = false,
    .force_progressive_scan = false,
    .show_protected_entries = false,
    .default_directory = "/",
    .use_saves_folder = true,
    .show_saves_folder = false,
    .show_save_files = false,
    .show_cheat_files = false,
    .show_rom_configuration_files = false,
    .soundfx_enabled = false,
    .bgm_enabled = false,
#ifdef FEATURE_AUTOLOAD_ROM_ENABLED
    .rom_autoload_enabled = false,
    .rom_autoload_path = "",
    .rom_autoload_filename = "",
    .loading_progress_bar_enabled = true,
#else
    .rom_fast_reboot_enabled = false,
#endif    
    /* Beta feature flags (should always init to default) */
    .show_browser_file_extensions = true,
    .show_browser_rom_tags = true,
    .wrap_file_list_scrolling = false,
    .rumble_enabled = false,
    /* SC64SS: the save state hotkeys */
    .ss_key_save = "L+R+Up",
    .ss_key_load = "L+R+Down",
    .ss_key_panel = "R+Z+Start",
    .ss_key_step = "L",
    .ss_key_shot = "",
};


void settings_init (char *path) {
    if (settings_path) {
        free(settings_path);
    }
    settings_path = strdup(path);
}

void settings_load (settings_t *settings) {
    if (!file_exists(settings_path)) {
        settings_save(&init);
    }

    ini_t *ini = ini_try_load(settings_path);

    settings->schema_revision = ini_get_int(ini, "menu", "schema_revision", init.schema_revision);
    settings->first_run = ini_get_bool(ini, "menu", "first_run", init.first_run);
    settings->pal60_enabled = ini_get_bool(ini, "menu", "pal60", init.pal60_enabled);
    settings->force_progressive_scan = ini_get_bool(ini, "menu", "force_progressive_scan", init.force_progressive_scan);
    settings->show_protected_entries = ini_get_bool(ini, "menu", "show_protected_entries", init.show_protected_entries);
    free(settings->default_directory);
    settings->default_directory = strdup(ini_get_string(ini, "menu", "default_directory", init.default_directory));
    settings->use_saves_folder = ini_get_bool(ini, "menu", "use_saves_folder", init.use_saves_folder);
    settings->show_saves_folder = ini_get_bool(ini, "menu", "show_saves_folder", init.show_saves_folder);
    settings->show_save_files = ini_get_bool(ini, "menu", "show_save_files", init.show_save_files);
    settings->show_cheat_files = ini_get_bool(ini, "menu", "show_cheat_files", init.show_cheat_files);
    settings->show_rom_configuration_files = ini_get_bool(ini, "menu", "show_rom_configuration_files", init.show_rom_configuration_files);
    settings->soundfx_enabled = ini_get_bool(ini, "menu", "soundfx_enabled", init.soundfx_enabled);
    settings->bgm_enabled = ini_get_bool(ini, "menu", "bgm_enabled", init.bgm_enabled);

#ifdef FEATURE_AUTOLOAD_ROM_ENABLED
    settings->rom_autoload_enabled = ini_get_bool(ini, "menu", "autoload_rom_enabled", init.rom_autoload_enabled);
    free(settings->rom_autoload_path);
    settings->rom_autoload_path = strdup(ini_get_string(ini, "autoload", "rom_path", init.rom_autoload_path));
    free(settings->rom_autoload_filename);
    settings->rom_autoload_filename = strdup(ini_get_string(ini, "autoload", "rom_filename", init.rom_autoload_filename));
    settings->loading_progress_bar_enabled = ini_get_bool(ini, "menu", "loading_progress_bar_enabled", init.loading_progress_bar_enabled);
#else
    settings->rom_fast_reboot_enabled = ini_get_bool(ini, "menu", "reboot_rom_enabled", init.rom_fast_reboot_enabled);
#endif
    /* Beta feature flags, they might not be in the file */
    settings->show_browser_file_extensions = ini_get_bool(ini, "menu", "show_browser_file_extensions", init.show_browser_file_extensions);
    settings->show_browser_rom_tags = ini_get_bool(ini, "menu", "show_browser_rom_tags", init.show_browser_rom_tags);
    settings->wrap_file_list_scrolling = ini_get_bool(ini, "menu", "wrap_file_list_scrolling", init.wrap_file_list_scrolling);
    settings->rumble_enabled = ini_get_bool(ini, "menu_beta_flag", "rumble_enabled", init.rumble_enabled);

    /* SC64SS: the save state hotkeys */
    free(settings->ss_key_save);
    settings->ss_key_save = strdup(ini_get_string(ini, "savestates", "hotkey_save", init.ss_key_save));
    free(settings->ss_key_load);
    settings->ss_key_load = strdup(ini_get_string(ini, "savestates", "hotkey_load", init.ss_key_load));
    free(settings->ss_key_panel);
    settings->ss_key_panel = strdup(ini_get_string(ini, "savestates", "hotkey_panel", init.ss_key_panel));
    free(settings->ss_key_step);
    settings->ss_key_step = strdup(ini_get_string(ini, "savestates", "hotkey_step", init.ss_key_step));
    settings->ss_key_set_save = ini_get_int(ini, "savestates", "hotkey_save_set", 0);
    settings->ss_key_set_load = ini_get_int(ini, "savestates", "hotkey_load_set", 0);
    settings->ss_key_set_panel = ini_get_int(ini, "savestates", "hotkey_panel_set", 0);
    settings->ss_key_set_step = ini_get_int(ini, "savestates", "hotkey_step_set", 0);
    free(settings->ss_key_shot);
    settings->ss_key_shot = strdup(ini_get_string(ini, "savestates", "screenshot_button", init.ss_key_shot));
    settings->ss_key_set_shot = ini_get_int(ini, "savestates", "screenshot_button_set", 0);

    ini_free(ini);
}

void settings_save (settings_t *settings) {
    ini_t *ini = ini_create();

    ini_set_int(ini, "menu", "schema_revision", settings->schema_revision);
    ini_set_bool(ini, "menu", "first_run", settings->first_run);
    ini_set_bool(ini, "menu", "pal60", settings->pal60_enabled);
    ini_set_bool(ini, "menu", "force_progressive_scan", settings->force_progressive_scan);
    ini_set_bool(ini, "menu", "show_protected_entries", settings->show_protected_entries);
    ini_set_string(ini, "menu", "default_directory", settings->default_directory);
    ini_set_bool(ini, "menu", "use_saves_folder", settings->use_saves_folder);
    ini_set_bool(ini, "menu", "show_saves_folder", settings->show_saves_folder);
    ini_set_bool(ini, "menu", "show_save_files", settings->show_save_files);
    ini_set_bool(ini, "menu", "show_cheat_files", settings->show_cheat_files);
    ini_set_bool(ini, "menu", "show_rom_configuration_files", settings->show_rom_configuration_files);
    ini_set_bool(ini, "menu", "soundfx_enabled", settings->soundfx_enabled);
    ini_set_bool(ini, "menu", "bgm_enabled", settings->bgm_enabled);
#ifdef FEATURE_AUTOLOAD_ROM_ENABLED
    ini_set_bool(ini, "menu", "autoload_rom_enabled", settings->rom_autoload_enabled);
    ini_set_string(ini, "autoload", "rom_path", settings->rom_autoload_path);
    ini_set_string(ini, "autoload", "rom_filename", settings->rom_autoload_filename);
    ini_set_bool(ini, "menu", "loading_progress_bar_enabled", settings->loading_progress_bar_enabled);
#else
    ini_set_bool(ini, "menu", "reboot_rom_enabled", settings->rom_fast_reboot_enabled);
#endif

    /* Beta feature flags, they should not save until production ready! */
    // ini_set_bool(ini, "menu", "show_browser_file_extensions", settings->show_browser_file_extensions);
    // ini_set_bool(ini, "menu", "show_browser_rom_tags", settings->show_browser_rom_tags);
    ini_set_bool(ini, "menu", "wrap_file_list_scrolling", settings->wrap_file_list_scrolling);
    // ini_set_bool(ini, "menu_beta_flag", "rumble_enabled", settings->rumble_enabled);

    /* SC64SS: the save state hotkeys */
    ini_set_string(ini, "savestates", "hotkey_save", settings->ss_key_save);
    ini_set_string(ini, "savestates", "hotkey_load", settings->ss_key_load);
    ini_set_string(ini, "savestates", "hotkey_panel", settings->ss_key_panel);
    ini_set_string(ini, "savestates", "hotkey_step", settings->ss_key_step);
    ini_set_int(ini, "savestates", "hotkey_save_set", settings->ss_key_set_save);
    ini_set_int(ini, "savestates", "hotkey_load_set", settings->ss_key_set_load);
    ini_set_int(ini, "savestates", "hotkey_panel_set", settings->ss_key_set_panel);
    ini_set_int(ini, "savestates", "hotkey_step_set", settings->ss_key_set_step);
    ini_set_string(ini, "savestates", "screenshot_button", settings->ss_key_shot);
    ini_set_int(ini, "savestates", "screenshot_button_set", settings->ss_key_set_shot);

    if (!ini_save(ini, settings_path)) {
        debugf("[SETTINGS] Failed to save settings to %s\n", settings_path);
    }

    ini_free(ini);
}

void settings_reset_to_defaults() {
    remove(settings_path);
}