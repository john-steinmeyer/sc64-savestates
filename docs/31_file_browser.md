## File Browser

<!-- Could use a beauty screenshot here -->

The File Browser allows you to navigate and manage files on your N64 flashcart. Below are the key features and instructions on how to use the File Browser effectively.

### Features

- **File and folder navigation**: Browse through directories and files on your flashcart.
- **File operations**: Perform operations such as delete and show properties.
- **File information**: View detailed information about each file, including size and date modified.
- **Load files**: Load files from the file system.
- **Extract files**: Extract files from ZIP archives.
- **Switching tabs**: Switch between the file browser, favorites and history tabs.
- **Favorites and history**: Keep up to 256 favorites in the order you choose, and the last 50 games played. See [Favorites and History](#favorites-and-history).
- **Hide (system) files and folders**: Hides specific files/folders from view. See [menu settings documentation](./32_menu_settings.md#show-hidden-files).

### Usage Instructions

<!-- Maybe all the Control pages could be merged into this section? -->

1. **Navigating Files**:
   - Use the directional `Up` and `Down` buttons (`C-Up` and `C-Down` for fast scrolling) to move through the list of files and directories.
   - Press the `A` Button to open a directory or load a supported file.

2. **Performing File Operations**:
   - Highlight the file or directory you want to operate on.
   - Press the `R` Button to open the operations menu.
   - Select the desired operation (delete, show properties, set as default, extract, and for a ROM, add to favorites) and follow the on-screen prompts.

3. **Viewing Settings menu**:
   - Press the `Z` Button to display the menu.

4. **Switching tabs**:
   - Press the `C-Right` and `C-Left` Buttons to switch between the file browser, favorites and history tabs.

5. **Extract files**:
   - Press the `A` Button on a ZIP file to open the archive.
   - Navigate to the file you want to extract.
   - Press the `A` Button to open the file info and press `A` again to extract the file.

### Favorites and History

- **Favorites** holds up to 256 games in the order you set. Add a ROM with the `R` Button on it in the file browser (**Add to favorites**), or from the `R` menu of its information screen; a new favorite goes to the end of the list.
- To move a favorite, highlight it and press the `Z` Button: `Up` and `Down` carry it (`C-Up` and `C-Down` ten places at a time), `A` puts it there, `B` puts it back where it was.
- **History** keeps the last 50 games played, the latest first.
- The `R` Button on an entry in either list removes it, after asking (`A` removes it, `B` keeps it).
- Both lists are stored in `sd:/menu/history.ini`. An older version of the menu keeps only the first eight of each if it saves that file.
