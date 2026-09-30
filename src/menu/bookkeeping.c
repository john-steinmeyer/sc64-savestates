/**
 * @file bookkeeping.c
 * @brief Bookkeeping functions for history and favorites
 * @ingroup menu
 */

#include <string.h>
#include <libdragon.h>
#include "ini_parser.h"

#include "bookkeeping.h"
#include "utils/fs.h"
#include "path.h"

/** @brief The entries a list had in the fixed-size format older menus write. */
#define BOOKKEEPING_OLD_COUNT 8

static char *history_path = NULL;
static bookkeeping_t init;

/**
 * @brief Initialize the bookkeeping system with the specified path.
 *
 * @param path Path to the history file.
 */
void bookkeeping_init (char *path) {
    if (history_path) {
        free(history_path);
    }
    history_path = strdup(path);
}

/**
 * @brief Empty a bookkeeping item, freeing its paths.
 *
 * @param item Pointer to the bookkeeping item.
 */
static void bookkeeping_free_item(bookkeeping_item_t *item) {
    if (item->primary_path != NULL) {
        path_free(item->primary_path);
    }
    if (item->secondary_path != NULL) {
        path_free(item->secondary_path);
    }
    item->primary_path = NULL;
    item->secondary_path = NULL;
    item->bookkeeping_type = BOOKKEEPING_TYPE_EMPTY;
}

/**
 * @brief Load a list of bookkeeping items from an INI file.
 *
 * The items in use are packed at the start of the list. A file from an older menu
 * holds eight entries a list, empty ones included; past those the list ends at its
 * first empty entry.
 *
 * @param list Pointer to the list of bookkeeping items.
 * @param count Number of items in the list.
 * @param ini Pointer to the INI file structure.
 * @param group Name of the group in the INI file.
 */
static void bookkeeping_ini_load_list(bookkeeping_item_t *list, uint16_t count, ini_t *ini, const char *group) {
    char buf[64];
    uint16_t used = 0;
    for(uint16_t i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "%d_primary_path", i);
        const char *primary = ini_get_string(ini, group, buf, "");

        snprintf(buf, sizeof(buf), "%d_type", i);
        int type = ini_get_int(ini, group, buf, BOOKKEEPING_TYPE_EMPTY);

        if ((primary[0] == '\0') || (type == BOOKKEEPING_TYPE_EMPTY)) {
            if (i >= BOOKKEEPING_OLD_COUNT) {
                break;
            }
            continue;
        }

        snprintf(buf, sizeof(buf), "%d_secondary_path", i);
        const char *secondary = ini_get_string(ini, group, buf, "");

        list[used].primary_path = path_create(primary);
        list[used].secondary_path = (secondary[0] != '\0') ? path_create(secondary) : NULL;
        list[used].bookkeeping_type = type;
        used++;
    }
    for(; used < count; used++) {
        list[used].primary_path = NULL;
        list[used].secondary_path = NULL;
        list[used].bookkeeping_type = BOOKKEEPING_TYPE_EMPTY;
    }
}

/**
 * @brief Load the bookkeeping history and favorites from the history file.
 *
 * @param history Pointer to the bookkeeping structure.
 */
void bookkeeping_load (bookkeeping_t *history) {
    if (!file_exists(history_path)) {
        bookkeeping_save(&init);
    }

    ini_t *bookkeeping_ini = ini_try_load(history_path);
    if (bookkeeping_ini == NULL) {
        debugf("[BOOKKEEPING] Failed to load INI from %s\n", history_path);
        return;
    }
    bookkeeping_ini_load_list(history->history_items, HISTORY_COUNT, bookkeeping_ini, "history");
    bookkeeping_ini_load_list(history->favorite_items, FAVORITES_COUNT, bookkeeping_ini, "favorite");

    ini_free(bookkeeping_ini);
}

/**
 * @brief Save a list of bookkeeping items to an INI file, the items in use only.
 *
 * @param list Pointer to the list of bookkeeping items.
 * @param count Number of items in the list.
 * @param ini Pointer to the INI file structure.
 * @param group Name of the group in the INI file.
 */
static void bookkeeping_ini_save_list(bookkeeping_item_t *list, uint16_t count, ini_t *ini, const char *group) {
    char buf[64];
    uint16_t used = 0;
    for(uint16_t i = 0; i < count; i++) {
        if (list[i].bookkeeping_type == BOOKKEEPING_TYPE_EMPTY) {
            continue;
        }

        snprintf(buf, sizeof(buf), "%d_primary_path", used);
        path_t* path = list[i].primary_path;
        ini_set_string(ini, group, buf, path != NULL ? path_get(path) : "");

        snprintf(buf, sizeof(buf), "%d_secondary_path", used);
        path = list[i].secondary_path;
        ini_set_string(ini, group, buf, path != NULL ? path_get(path) : "");

        snprintf(buf, sizeof(buf), "%d_type", used);
        ini_set_int(ini, group, buf, list[i].bookkeeping_type);
        used++;
    }
}

/**
 * @brief Save the bookkeeping history and favorites to the history file.
 *
 * @param history Pointer to the bookkeeping structure.
 */
void bookkeeping_save (bookkeeping_t *history) {
    ini_t *bookkeeping_ini = ini_create();
    if (bookkeeping_ini == NULL) {
        debugf("[BOOKKEEPING] Failed to create INI structure\n");
        return;
    }
    bookkeeping_ini_save_list(history->history_items, HISTORY_COUNT, bookkeeping_ini, "history");
    bookkeeping_ini_save_list(history->favorite_items, FAVORITES_COUNT, bookkeeping_ini, "favorite");
    if (!ini_save(bookkeeping_ini, history_path)) {
        debugf("[BOOKKEEPING] Failed to save history to %s\n", history_path);
    }
    ini_free(bookkeeping_ini);
}

/**
 * @brief Check if two bookkeeping items match.
 *
 * @param left Pointer to the first bookkeeping item.
 * @param right Pointer to the second bookkeeping item.
 * @return true if the items match, false otherwise.
 */
static bool bookkeeping_item_match(bookkeeping_item_t *left, bookkeeping_item_t *right) {
    if(left != NULL && right != NULL) {
        return path_are_match(left->primary_path, right->primary_path) && path_are_match(left->secondary_path, right->secondary_path) && left->bookkeeping_type == right->bookkeeping_type;
    }

    return false;
}

/**
 * @brief Find an item in a list.
 *
 * @param list Pointer to the list of bookkeeping items.
 * @param count Number of items in the list.
 * @param item Pointer to the item to find.
 * @return Its index, or -1.
 */
static int bookkeeping_find(bookkeeping_item_t *list, int count, bookkeeping_item_t *item) {
    for(int i = 0; i < count; i++) {
        if(list[i].bookkeeping_type == BOOKKEEPING_TYPE_EMPTY) {
            break;
        }
        if(bookkeeping_item_match(&list[i], item)) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Fill a list entry with a copy of an item.
 *
 * @param destination Pointer to the (empty) list entry.
 * @param source Pointer to the item, whose paths stay the caller's.
 */
static void bookkeeping_fill_item(bookkeeping_item_t *destination, bookkeeping_item_t *source) {
    destination->primary_path = path_clone(source->primary_path);
    destination->secondary_path = path_has_value(source->secondary_path) ? path_clone(source->secondary_path) : NULL;
    destination->bookkeeping_type = source->bookkeeping_type;
}

/**
 * @brief Insert a bookkeeping item at the top of the list. An item already in the
 * list moves up to the top; a full list drops its last item.
 *
 * @param list Pointer to the list of bookkeeping items.
 * @param count Number of items in the list.
 * @param new_item Pointer to the new bookkeeping item.
 */
static void bookkeeping_insert_top(bookkeeping_item_t *list, int count, bookkeeping_item_t *new_item) {
    int found_at = bookkeeping_find(list, count, new_item);
    if(found_at == 0) {
        return;
    }

    bookkeeping_item_t top;
    int shifted;
    if(found_at > 0) {
        top = list[found_at];
        shifted = found_at;
    } else {
        bookkeeping_free_item(&list[count - 1]);
        bookkeeping_fill_item(&top, new_item);
        shifted = count - 1;
    }
    memmove(&list[1], &list[0], shifted * sizeof(list[0]));
    list[0] = top;
}

/**
 * @brief Remove an item from a list, the items after it moving up one.
 *
 * @param list Pointer to the list of bookkeeping items.
 * @param count Number of items in the list.
 * @param selection Index of the item to remove.
 * @return true if an item was removed.
 */
static bool bookkeeping_remove(bookkeeping_item_t *list, int count, int selection) {
    if((selection < 0) || (selection >= count) || (list[selection].bookkeeping_type == BOOKKEEPING_TYPE_EMPTY)) {
        return false;
    }
    bookkeeping_free_item(&list[selection]);
    memmove(&list[selection], &list[selection + 1], (count - 1 - selection) * sizeof(list[0]));
    list[count - 1].primary_path = NULL;
    list[count - 1].secondary_path = NULL;
    list[count - 1].bookkeeping_type = BOOKKEEPING_TYPE_EMPTY;
    return true;
}

/**
 * @brief Count the items in use at the start of a list.
 *
 * @param list Pointer to the list of bookkeeping items.
 * @param count Number of items in the list.
 * @return The number of items in use.
 */
int bookkeeping_count(bookkeeping_item_t *list, int count) {
    int used = 0;
    while((used < count) && (list[used].bookkeeping_type != BOOKKEEPING_TYPE_EMPTY)) {
        used++;
    }
    return used;
}

/**
 * @brief Add a new item to the bookkeeping history.
 *
 * @param bookkeeping Pointer to the bookkeeping structure.
 * @param primary_path Pointer to the primary path.
 * @param secondary_path Pointer to the secondary path.
 * @param type The type of the bookkeeping item.
 */
void bookkeeping_history_add(bookkeeping_t *bookkeeping, path_t *primary_path, path_t *secondary_path, bookkeeping_item_types_t type) {
    bookkeeping_item_t new_item = {
        .primary_path = primary_path,
        .secondary_path = secondary_path,
        .bookkeeping_type = type
    };

    bookkeeping_insert_top(bookkeeping->history_items, HISTORY_COUNT, &new_item);
    bookkeeping_save(bookkeeping);
}

/**
 * @brief Add a new item to the end of the bookkeeping favorites.
 *
 * @param bookkeeping Pointer to the bookkeeping structure.
 * @param primary_path Pointer to the primary path.
 * @param secondary_path Pointer to the secondary path.
 * @param type The type of the bookkeeping item.
 * @return What was done: added, already there, or the list full.
 */
bookkeeping_favorite_result_t bookkeeping_favorite_add(bookkeeping_t *bookkeeping, path_t *primary_path, path_t *secondary_path, bookkeeping_item_types_t type) {
    bookkeeping_item_t new_item = {
        .primary_path = primary_path,
        .secondary_path = secondary_path,
        .bookkeeping_type = type
    };

    if(bookkeeping_find(bookkeeping->favorite_items, FAVORITES_COUNT, &new_item) >= 0) {
        return BOOKKEEPING_FAVORITE_PRESENT;
    }
    int used = bookkeeping_count(bookkeeping->favorite_items, FAVORITES_COUNT);
    if(used >= FAVORITES_COUNT) {
        return BOOKKEEPING_FAVORITE_FULL;
    }
    bookkeeping_fill_item(&bookkeeping->favorite_items[used], &new_item);
    bookkeeping_save(bookkeeping);
    return BOOKKEEPING_FAVORITE_ADDED;
}

/**
 * @brief Find an item in the favorites.
 *
 * @param bookkeeping Pointer to the bookkeeping structure.
 * @param primary_path Pointer to the primary path.
 * @param secondary_path Pointer to the secondary path.
 * @param type The type of the bookkeeping item.
 * @return Its index, or -1.
 */
int bookkeeping_favorite_find(bookkeeping_t *bookkeeping, path_t *primary_path, path_t *secondary_path, bookkeeping_item_types_t type) {
    bookkeeping_item_t item = {
        .primary_path = primary_path,
        .secondary_path = secondary_path,
        .bookkeeping_type = type
    };

    return bookkeeping_find(bookkeeping->favorite_items, FAVORITES_COUNT, &item);
}

/**
 * @brief Remove an item from the bookkeeping favorites.
 *
 * @param bookkeeping Pointer to the bookkeeping structure.
 * @param selection Index of the item to remove.
 */
void bookkeeping_favorite_remove(bookkeeping_t *bookkeeping, int selection) {
    if(bookkeeping_remove(bookkeeping->favorite_items, FAVORITES_COUNT, selection)) {
        bookkeeping_save(bookkeeping);
    }
}

/**
 * @brief Remove an item from the bookkeeping history.
 *
 * @param bookkeeping Pointer to the bookkeeping structure.
 * @param selection Index of the item to remove.
 */
void bookkeeping_history_remove(bookkeeping_t *bookkeeping, int selection) {
    if(bookkeeping_remove(bookkeeping->history_items, HISTORY_COUNT, selection)) {
        bookkeeping_save(bookkeeping);
    }
}

/**
 * @brief Swap two favorites. The list is not saved here.
 *
 * @param bookkeeping Pointer to the bookkeeping structure.
 * @param a Index of one favorite.
 * @param b Index of the other.
 */
void bookkeeping_favorite_swap(bookkeeping_t *bookkeeping, int a, int b) {
    if((a < 0) || (b < 0) || (a >= FAVORITES_COUNT) || (b >= FAVORITES_COUNT)) {
        return;
    }
    bookkeeping_item_t item = bookkeeping->favorite_items[a];
    bookkeeping->favorite_items[a] = bookkeeping->favorite_items[b];
    bookkeeping->favorite_items[b] = item;
}
