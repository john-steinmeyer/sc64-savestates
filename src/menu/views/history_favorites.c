#include <stdarg.h>
#include "../bookkeeping.h"
#include "../fonts.h"
#include "../ui_components/constants.h"
#include "../sound.h"
#include "views.h"


typedef enum {
    BOOKKEEPING_TAB_CONTEXT_HISTORY,
    BOOKKEEPING_TAB_CONTEXT_FAVORITE,
    BOOKKEEPING_TAB_CONTEXT_NONE
} bookkeeping_tab_context_t;

// SC64SS: one line a game with a scroll bar, as the file list; a favorite can be picked up
// and carried to another place (Z), and R asks before it takes a game off either list
typedef enum {
    LIST_MODE_BROWSE,   // moving through the list
    LIST_MODE_MOVE,     // a favorite picked up: up and down carry it
    LIST_MODE_REMOVE,   // R pressed: A removes the game from the list, B keeps it
} list_mode_t;

#define LIST_FAST_STEP  (10)    // C-up and C-down, as in the file list


static bookkeeping_tab_context_t tab_context = BOOKKEEPING_TAB_CONTEXT_NONE;
static list_mode_t mode = LIST_MODE_BROWSE;
static int selected_item = -1;
static int move_from = -1;          // where the picked-up favorite was
static bookkeeping_item_t *item_list;
static uint16_t item_max = 0;
static int item_count = 0;
static char row_text[LIST_ENTRIES][192];


static void item_reset_selected(menu_t *menu) {
    item_count = bookkeeping_count(item_list, item_max);
    if (item_count == 0) {
        selected_item = -1;
    } else if (selected_item < 0) {
        selected_item = 0;
    } else if (selected_item >= item_count) {
        selected_item = item_count - 1;
    }
}

static void item_select(int target) {
    if (target >= item_count) {
        target = item_count - 1;
    }
    if (target < 0) {
        target = 0;
    }
    if ((item_count > 0) && (target != selected_item)) {
        selected_item = target;
        sound_play_effect(SFX_CURSOR);
    }
}

// the picked-up favorite to another place, the ones in between moving by one
static void item_carry(menu_t *menu, int target) {
    if (target >= item_count) {
        target = item_count - 1;
    }
    if (target < 0) {
        target = 0;
    }
    while (selected_item < target) {
        bookkeeping_favorite_swap(&menu->bookkeeping, selected_item, selected_item + 1);
        selected_item++;
    }
    while (selected_item > target) {
        bookkeeping_favorite_swap(&menu->bookkeeping, selected_item, selected_item - 1);
        selected_item--;
    }
}

static void process(menu_t *menu) {
    int step = menu->actions.go_fast ? LIST_FAST_STEP : 1;

    if (mode == LIST_MODE_REMOVE) {
        if (menu->actions.enter) {
            if (tab_context == BOOKKEEPING_TAB_CONTEXT_FAVORITE) {
                bookkeeping_favorite_remove(&menu->bookkeeping, selected_item);
            } else {
                bookkeeping_history_remove(&menu->bookkeeping, selected_item);
            }
            item_reset_selected(menu);
            mode = LIST_MODE_BROWSE;
            sound_play_effect(SFX_SETTING);
        } else if (menu->actions.back) {
            mode = LIST_MODE_BROWSE;
            sound_play_effect(SFX_EXIT);
        }
        return;
    }

    if (mode == LIST_MODE_MOVE) {
        if (menu->actions.go_up || menu->actions.go_down) {
            int from = selected_item;
            item_carry(menu, selected_item + (menu->actions.go_up ? -step : step));
            if (selected_item != from) {
                sound_play_effect(SFX_CURSOR);
            }
        } else if (menu->actions.enter) {
            bookkeeping_save(&menu->bookkeeping);
            mode = LIST_MODE_BROWSE;
            sound_play_effect(SFX_SETTING);
        } else if (menu->actions.back) {
            item_carry(menu, move_from);      // back where it was; the file never changed
            mode = LIST_MODE_BROWSE;
            sound_play_effect(SFX_EXIT);
        }
        return;
    }

    if(menu->actions.go_down) {
        item_select(selected_item + step);
    } else if(menu->actions.go_up) {
        item_select(selected_item - step);
    } else if(menu->actions.enter && selected_item != -1) {

        if(tab_context == BOOKKEEPING_TAB_CONTEXT_FAVORITE) {
            menu->load.load_favorite_id = selected_item;
            menu->load.load_history_id = -1;
        } else if(tab_context == BOOKKEEPING_TAB_CONTEXT_HISTORY) {
            menu->load.load_history_id = selected_item;
            menu->load.load_favorite_id = -1;
        }

        if(item_list[selected_item].bookkeeping_type == BOOKKEEPING_TYPE_DISK) {
            menu->next_mode = MENU_MODE_LOAD_DISK;
            sound_play_effect(SFX_ENTER);
        } else if(item_list[selected_item].bookkeeping_type == BOOKKEEPING_TYPE_ROM) {
            menu->next_mode = MENU_MODE_LOAD_ROM;
            sound_play_effect(SFX_ENTER);
        }
    } else if (menu->actions.go_left) {
        if(tab_context == BOOKKEEPING_TAB_CONTEXT_FAVORITE) {
            menu->next_mode = MENU_MODE_HISTORY;
        } else if(tab_context == BOOKKEEPING_TAB_CONTEXT_HISTORY) {
            menu->next_mode = MENU_MODE_BROWSER;
        }
        sound_play_effect(SFX_CURSOR);
    } else if (menu->actions.go_right) {
        if(tab_context == BOOKKEEPING_TAB_CONTEXT_FAVORITE) {
            menu->next_mode = MENU_MODE_BROWSER;
        } else if(tab_context == BOOKKEEPING_TAB_CONTEXT_HISTORY) {
            menu->next_mode = MENU_MODE_FAVORITE;
        }
        sound_play_effect(SFX_CURSOR);
    } else if (menu->actions.options && selected_item != -1) {
        mode = LIST_MODE_REMOVE;
        sound_play_effect(SFX_SETTING);
    } else if (menu->actions.lz_context && (tab_context == BOOKKEEPING_TAB_CONTEXT_FAVORITE) && (item_count > 1)) {
        mode = LIST_MODE_MOVE;
        move_from = selected_item;
        sound_play_effect(SFX_SETTING);
    }
}

static void draw_list(menu_t *menu, surface_t *display) {
    if (item_count == 0) {
        ui_components_main_text_draw(
            STL_DEFAULT,
            ALIGN_LEFT, VALIGN_TOP,
            "\n"
            "^%02X%s",
            STL_GRAY,
            (tab_context == BOOKKEEPING_TAB_CONTEXT_FAVORITE)
                ? "** no favorites yet **\n\nR on a ROM in the file list, or Add to favorites\non a game's page, puts one here."
                : "** no games played yet **"
        );
        return;
    }

    int starting_position = 0;
    if (item_count > LIST_ENTRIES && selected_item >= (LIST_ENTRIES / 2)) {
        starting_position = selected_item - (LIST_ENTRIES / 2);
        if (starting_position >= item_count - LIST_ENTRIES) {
            starting_position = item_count - LIST_ENTRIES;
        }
    }
    int visible_entries = item_count - starting_position;
    if (visible_entries > LIST_ENTRIES) {
        visible_entries = LIST_ENTRIES;
    }

    ui_components_list_scrollbar_draw(selected_item, item_count, LIST_ENTRIES);

    rdpq_paragraph_builder_begin(
        &(rdpq_textparms_t) {
            .width = VISIBLE_AREA_WIDTH - LIST_SCROLLBAR_WIDTH - (TEXT_MARGIN_HORIZONTAL * 2),
            .height = LAYOUT_ACTIONS_SEPARATOR_Y - VISIBLE_AREA_Y0 - (TEXT_MARGIN_VERTICAL * 2),
            .wrap = WRAP_ELLIPSES,
            .line_spacing = TEXT_LINE_SPACING_ADJUST,
        },
        FNT_DEFAULT,
        NULL
    );

    for (int i = 0; i < visible_entries; i++) {
        int index = starting_position + i;
        bookkeeping_item_t *item = &item_list[index];
        bool carried = (mode == LIST_MODE_MOVE) && (index == selected_item);
        int length;

        if (path_has_value(item->secondary_path)) {
            length = snprintf(row_text[i], sizeof(row_text[i]), "%3d  %s%s  +  %s", index + 1, carried ? "▲▼ " : "",
                              path_last_get(item->primary_path), path_last_get(item->secondary_path));
        } else {
            length = snprintf(row_text[i], sizeof(row_text[i]), "%3d  %s%s", index + 1, carried ? "▲▼ " : "",
                              path_has_value(item->primary_path) ? path_last_get(item->primary_path) : "");
        }
        if (length >= (int) sizeof(row_text[i])) {
            length = sizeof(row_text[i]) - 1;
        }

        rdpq_paragraph_builder_style(carried ? STL_YELLOW : STL_DEFAULT);
        rdpq_paragraph_builder_span(row_text[i], length);

        if ((i + 1) >= visible_entries) {
            break;
        }

        rdpq_paragraph_builder_newline();
    }

    rdpq_paragraph_t *layout = rdpq_paragraph_builder_end();

    int highlight_height = (layout->nlines > 0) ? ((layout->bbox.y1 - layout->bbox.y0) / layout->nlines) : 0;
    int highlight_y = VISIBLE_AREA_Y0 + TAB_HEIGHT + TEXT_MARGIN_VERTICAL + TEXT_OFFSET_VERTICAL + ((selected_item - starting_position) * highlight_height);

    ui_components_box_draw(
        FILE_LIST_HIGHLIGHT_X,
        highlight_y,
        FILE_LIST_HIGHLIGHT_X + FILE_LIST_HIGHLIGHT_WIDTH,
        highlight_y + highlight_height,
        FILE_LIST_HIGHLIGHT_COLOR
    );

    rdpq_paragraph_render(
        layout,
        VISIBLE_AREA_X0 + TEXT_MARGIN_HORIZONTAL,
        VISIBLE_AREA_Y0 + TEXT_MARGIN_VERTICAL + TAB_HEIGHT + TEXT_OFFSET_VERTICAL
    );

    rdpq_paragraph_free(layout);
}

static void draw(menu_t *menu, surface_t *display) {
    rdpq_attach(display, NULL);

    ui_components_background_draw();

    if(tab_context == BOOKKEEPING_TAB_CONTEXT_FAVORITE) {
        ui_components_tabs_common_draw(2);
    } else if(tab_context == BOOKKEEPING_TAB_CONTEXT_HISTORY) {
        ui_components_tabs_common_draw(1);
    }

    ui_components_layout_draw_tabbed();

    draw_list(menu, display);

    if (mode == LIST_MODE_MOVE) {
        ui_components_actions_bar_text_draw(
            STL_DEFAULT,
            ALIGN_LEFT, VALIGN_TOP,
            "A: Put it here\n"
            "B: Put it back"
        );
        ui_components_actions_bar_text_draw(
            STL_DEFAULT,
            ALIGN_RIGHT, VALIGN_TOP,
            "▲▼: Move it\n"
            "C-▼▲: Ten at a time"
        );
    } else if (mode == LIST_MODE_REMOVE) {
        ui_components_actions_bar_text_draw(
            STL_DEFAULT,
            ALIGN_LEFT, VALIGN_TOP,
            "A: Remove it\n"
            "B: Keep it"
        );
        ui_components_actions_bar_text_draw(
            STL_DEFAULT,
            ALIGN_CENTER, VALIGN_TOP,
            "%s\n"
            "\n",
            (tab_context == BOOKKEEPING_TAB_CONTEXT_FAVORITE) ? "Remove from Favorites?" : "Remove from History?"
        );
    } else {
        if(selected_item != -1) {
            ui_components_actions_bar_text_draw(
                STL_DEFAULT,
                ALIGN_LEFT, VALIGN_TOP,
                "A: Load Game\n"
                "%s",
                ((tab_context == BOOKKEEPING_TAB_CONTEXT_FAVORITE) && (item_count > 1)) ? "Z: Move" : ""
            );
            ui_components_actions_bar_text_draw(
                STL_DEFAULT,
                ALIGN_RIGHT, VALIGN_TOP,
                "R: Remove\n"
                "\n"
            );
        }

        ui_components_actions_bar_text_draw(
            STL_DEFAULT,
            ALIGN_CENTER, VALIGN_TOP,
            "C-▼▲ Fast Scroll | ◀ Tabs ▶\n"
            "\n"
        );
    }

    rdpq_detach_show();
}

void view_favorite_init (menu_t *menu) {
    tab_context = BOOKKEEPING_TAB_CONTEXT_FAVORITE;
    item_list = menu->bookkeeping.favorite_items;
    item_max = FAVORITES_COUNT;
    mode = LIST_MODE_BROWSE;
    selected_item = 0;

    item_reset_selected(menu);
}

void view_favorite_display (menu_t *menu, surface_t *display) {
    process(menu);
    draw(menu, display);
}

void view_history_init (menu_t *menu) {
    tab_context = BOOKKEEPING_TAB_CONTEXT_HISTORY;
    item_list = menu->bookkeeping.history_items;
    item_max = HISTORY_COUNT;
    mode = LIST_MODE_BROWSE;
    selected_item = 0;

    item_reset_selected(menu);
}

void view_history_display (menu_t *menu, surface_t *display) {
    process(menu);
    draw(menu, display);
}
