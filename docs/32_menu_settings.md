## Menu Settings

N64FlashcartMenu automatically creates a `config.ini` file in `sd:/menu/`, which contains various settings that can be set within the menu's Settings editor.
If required, you can manually adjust the file (required for some advanced settings) on the SD card using your computer.

### Show Hidden Files

Shows any N64FlashcartMenu system-related and hidden files/folders. This setting is OFF by default.

Besides system-related files, the File Browser respects FAT hidden attributes, allowing you to hide files and folders from the file list. This is useful for organizing your SD card and keeping system-related, homebrew-related, or temporary files out of sight.

#### Setting Hidden Attribute on Windows

1. Right-click the file or folder you want to hide.
2. Select **Properties**.
3. In the **General** tab, check the **Hidden** checkbox.
4. Click **Apply** and then **OK**.

#### Setting Hidden Attribute on Linux

Use the `fatattr` command-line tool to manage FAT attributes:

```bash
# Set a file or folder as hidden
fatattr +h /path/to/file

# Remove the hidden attribute
fatattr -h /path/to/file

# View current attributes
fatattr /path/to/file
```

#### Setting Hidden Attribute on macOS

Use the `chflags` command-line tool to manage FAT attributes:

```bash
# Set a file or folder as hidden
chflags hidden /path/to/file

# Remove the hidden attribute
chflags nohidden /path/to/file

# View current attributes
ls -lO /path/to/file
```

### Use Save Folders

Controls whether N64FlashcartMenu should use `/saves` folders to store ROM save data. This setting is ON by default.
ON: ROM saves are saved in separate subfolders (called `\saves`, will create one `\saves` subfolder per folder).
OFF: ROM saves are saved alongside the ROM file.

### Sound Effects

The menu has default sound effects to improve the user experience. See the [sound documentation](./40_sound.md) for details. This setting is OFF by default.

### Reset Button

What the console's RESET button does while a game runs. **Menu**, the default, goes back to the menu. **Restart game** starts the game you were playing again from its beginning, the way RESET does on a normal cartridge: the menu comes up for a moment and loads the game again, with its save states, virtual pak and hotkeys set up as at any launch, so it takes a few seconds (longer for the biggest games). Hold `START` while pressing RESET to go to the menu instead. It needs the save-state routine in the game, which is what notices RESET: Save States, the virtual pak or a screenshot button on for it. In any other game, and while the game is paused by the save-state panel, FreeCam or the frame step, RESET goes to the menu. **Exit to menu** on the save-state panel always goes to the menu, and a ROM set to autoload is started by RESET either way. Stored as `reset_restarts_rom` in `config.ini`.
