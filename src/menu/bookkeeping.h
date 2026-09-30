/**
 * @file bookkeeping.h
 * @brief Bookkeeping of loaded ROMs.
 * @ingroup menu
 */

#ifndef BOOKKEEPING_H__
#define BOOKKEEPING_H__

#include "path.h"

#define FAVORITES_COUNT 256 /**< Maximum number of favorite items */
#define HISTORY_COUNT 50 /**< Maximum number of history items */

/** @brief Bookkeeping item types enumeration */
typedef enum {
    BOOKKEEPING_TYPE_EMPTY, /**< Empty item */
    BOOKKEEPING_TYPE_ROM, /**< ROM item */
    BOOKKEEPING_TYPE_DISK, /**< Disk item */
} bookkeeping_item_types_t;

/** @brief Bookkeeping item structure */
typedef struct {
    path_t *primary_path; /**< Primary path */
    path_t *secondary_path; /**< Secondary path */
    bookkeeping_item_types_t bookkeeping_type; /**< Bookkeeping item type */
} bookkeeping_item_t;

/** @brief ROM bookkeeping structure (each list keeps its items first, with no gaps) */
typedef struct {
    bookkeeping_item_t history_items[HISTORY_COUNT]; /**< History items */
    bookkeeping_item_t favorite_items[FAVORITES_COUNT]; /**< Favorite items */
} bookkeeping_t;

/** @brief What adding a favorite did */
typedef enum {
    BOOKKEEPING_FAVORITE_ADDED, /**< Added at the end of the list */
    BOOKKEEPING_FAVORITE_PRESENT, /**< Already in the list, left where it is */
    BOOKKEEPING_FAVORITE_FULL, /**< The list is full */
} bookkeeping_favorite_result_t;

/**
 * @brief Initialize ROM bookkeeping path.
 *
 * @param path The path to initialize.
 */
void bookkeeping_init(char *path);

/**
 * @brief Load ROM bookkeeping.
 *
 * @param history Pointer to the bookkeeping structure to load.
 */
void bookkeeping_load(bookkeeping_t *history);

/**
 * @brief Save ROM bookkeeping.
 *
 * @param history Pointer to the bookkeeping structure to save.
 */
void bookkeeping_save(bookkeeping_t *history);

/**
 * @brief Add a ROM to the history.
 *
 * @param bookkeeping The bookkeeping structure.
 * @param primary_path The primary path of the ROM.
 * @param secondary_path The secondary path of the ROM.
 * @param type The type of the bookkeeping item.
 */
void bookkeeping_history_add(bookkeeping_t *bookkeeping, path_t *primary_path, path_t *secondary_path, bookkeeping_item_types_t type);

/**
 * @brief Add a ROM to the end of the favorites.
 *
 * @param bookkeeping The bookkeeping structure.
 * @param primary_path The primary path of the ROM.
 * @param secondary_path The secondary path of the ROM.
 * @param type The type of the bookkeeping item.
 * @return What was done: added, already there, or the list full.
 */
bookkeeping_favorite_result_t bookkeeping_favorite_add(bookkeeping_t *bookkeeping, path_t *primary_path, path_t *secondary_path, bookkeeping_item_types_t type);

/**
 * @brief Find a ROM in the favorites.
 *
 * @param bookkeeping The bookkeeping structure.
 * @param primary_path The primary path of the ROM.
 * @param secondary_path The secondary path of the ROM.
 * @param type The type of the bookkeeping item.
 * @return Its index, or -1 when it is not a favorite.
 */
int bookkeeping_favorite_find(bookkeeping_t *bookkeeping, path_t *primary_path, path_t *secondary_path, bookkeeping_item_types_t type);

/**
 * @brief Count the items in use at the start of a list.
 *
 * @param list The list of bookkeeping items.
 * @param count The size of the list.
 * @return The number of items in use.
 */
int bookkeeping_count(bookkeeping_item_t *list, int count);

/**
 * @brief Remove a ROM from the favorites.
 *
 * @param bookkeeping The bookkeeping structure.
 * @param selection The index of the favorite item to remove.
 */
void bookkeeping_favorite_remove(bookkeeping_t *bookkeeping, int selection);

/**
 * @brief Remove a ROM from the history.
 *
 * @param bookkeeping The bookkeeping structure.
 * @param selection The index of the history item to remove.
 */
void bookkeeping_history_remove(bookkeeping_t *bookkeeping, int selection);

/**
 * @brief Swap two favorites (the list is not saved: bookkeeping_save does that).
 *
 * @param bookkeeping The bookkeeping structure.
 * @param a The index of one favorite.
 * @param b The index of the other.
 */
void bookkeeping_favorite_swap(bookkeeping_t *bookkeeping, int a, int b);

#endif /* BOOKKEEPING_H__ */
