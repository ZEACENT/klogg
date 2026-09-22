# Klogg documentation

## Table of Contents

1. [Getting started](#getting-started)
    * [Quick start](#quick-start)
1. [Exploring log files](#exploring-log-files)
    * [Opening files](#opening-files)
    * [Searching folders](#searching-folders)
    * [Predefined filters](#predefined-filters)
    * [Using highlighters](#using-highlighters)
    * [Color labels](#color-labels)
    * [Browsing changing log files](#browsing-changing-log-files)
1. [Live device logs](#live-device-logs)
    * [Android Logcat](#android-logcat)
    * [iOS log streams](#ios-log-streams)
    * [Saving live logs](#saving-live-logs)
    * [Disconnecting and restoring live sessions](#disconnecting-and-restoring-live-sessions)
    * [Integrity warnings and closing](#integrity-warnings-and-closing)
1. [Settings](#settings)
    * [Live Source](#live-source)
1. [Crash reporting](#crash-reporting)
1. [Keyboard commands](#keyboard-commands)
1. [Mouse navigation](#mouse-navigation)
1. [Command line options](#command-line-options)


<a name="getting-started"></a>

## Getting started

*klogg* can be started from the command line, optionally passing the
file to open as an argument, or via the desktop environment's menu or
file association. If no file name is passed, *klogg* normally restores
the previous session, subject to the session settings.

<a name="quick-start"></a>

### Quick start

* **Open a file:** choose `File -> Open...`, select a log file, then enter a
  search term in the search bar and run the search. Enable regular expressions
  when you need pattern matching rather than a literal text search.
* **Search a folder:** choose `File -> Open Folder...`, select a directory,
  and run a search. Results are grouped by source file and include subfolders.
  Click a result to open its source file in the upper view.
* **Read device live logs:** choose `File -> Open ADB Logcat...` for Android,
  or `File -> Open iOS Log Stream...` on macOS. Connect and authorize the
  device, select it in the dialog, and start capture. Search the captured text
  as you would a file. Use `Edit -> Save Live Log As` to keep the capture;
  temporary storage is not a permanent saved log.

See [Searching folders](#searching-folders) and
[Live device logs](#live-device-logs) for mode-specific controls and limitations.

The main window is divided into three parts: the top displays the log
file. The bottom part, called the "filtered view", displays the results of
the search. The line separating the two contains the regular expression
used as a filter.

Entering a new regular expression or a simple search term will update
the bottom view, displaying the results of the search. The lines
matching the search criteria are listed in order in the results, and are
marked with a red circle in both windows.

<a name="exploring-log-files"></a>

## Exploring log files

Regular expressions are a powerful way to extract the information you
want from the log file. *klogg* uses *extended regular
expressions*.

One of the most useful regexp features when exploring logs is the
*alternation* feature, using parentheses and the | operator. It searches for
several alternatives and displays several line types in the
filtered window, in the same order they appear in the log file.

For example, to verify that every connection opened is also closed, one
can use an expression similar to:

`Entering (Open|Close)Connection`

Any 'open' call without a matching 'close' will immediately stand out
in the filtered window. The alternation also works with the whole search
line. If you would like to know what kind of connection has
been opened:

`Entering (Open|Close)Connection|Created a .* connection`

`.*` will match any sequence of characters on a single line, but *klogg*
will only display lines with a space and the word `connection` somewhere
after `Created a`

Sometimes alternation using regular expression syntax is cumbersome.
For such cases *klogg* can do logical search pattern combinations using
`and`, `or`, and `not` operators. This mode can be enabled using button
from search input panel. In this mode all patterns must be enclosed in `"`.
Following logic operations are supported:

|Operator        |Actions                                                                   |
|----------------|--------------------------------------------------------------------------|
|`and`           |Logical AND, True only if x and y both match input line. (eg: `"x" and "y"`)|
|`or`            |Logical OR, True if either x or y match input line. (eg: `"x" or "y"`)      |
|`&`             |Similar to AND but with left to right expression short circuiting optimization  |
|`\|`             |Similar to OR but with left to right expression short circuiting optimization   |
|`not`           |Logical NOT, Negate the logical sense of the input. Input must be enclosed in `()` (eg: `not("x")`)|

Boolean operands use C-style escaping for quotes and backslashes: write `\"`
for a quote within an operand, and `\\` for a backslash. Other sequences, such
as `\d`, are left unchanged by the Boolean parser; regular expression syntax
is then applied if regex mode is enabled. For clarity, double each backslash
in the underlying pattern when writing a quoted Boolean operand.

Examples entered directly in the search bar (not in a shell):

* `"error" and not("timeout")` matches lines containing `error` but not `timeout`.
* `"say \"hello\""` searches for `say "hello"`.
* In fixed-string mode, `"C:\\logs\\"` searches for `C:\logs\`, including the
  trailing backslash.
* In regex mode, `"\\d+" and "error"` combines the regex `\d+` with `error`.
  To match a literal backslash in regex mode, use four backslashes inside the
  quoted operand: `"C:\\\\logs\\\\"` matches `C:\logs\`.

*klogg* keeps track of used search patterns and provides autocomplete
for them. This history can be edited or cleared from the search text box context menu.
Autocomplete is case-sensitive if this option is selected for matching 
regular expressions. The size of autocomplete history is configured in general options.

In addition to the filtered window, the match overview on the right-hand
side of the screen offers a view of the position of matches in the log
file. Matches are shown as small red lines.

In addition to regexp matches, *klogg* provides line marks for interesting
lines in the log. To add a line mark, click on the round bullet in
the left margin in front of the line that needs to be marked. Or, select
the line and press the `'m'` hotkey.
To mark several lines at once select them and use the `'m'` hotkey or context menu.

By default, filtered view always shows all marked lines. It is possible to switch filtered
view mode to show either only the lines matching search pattern or only marked lines.

Marks also appear as blue lines in the match overview.

It is possible to quickly jump to a specific line using the `Ctrl+G` shortcut.

*klogg* uses Vectorscan library to perform regular expressions search. Vectorscan is very
fast, but it doesn't support some patterns, most notably any lookahead is not supported
(check the Vectorscan syntax documentation for supported syntax). To overcome this
*klogg* will switch to Qt regular expression engine with full PCRE syntax support if
Vectorscan can't handle the search pattern. However, in this case search will be significantly slower.

<a name="opening-files"></a>

### Opening files

*klogg* provides several options for opening files:

* using dedicated open file item in `File` menu or toolbar
* dragging files from the file manager
* downloading files from a provided url
* providing one or many files via the command line
* using recent files or favorite menu items.

On Windows and Mac OS, the *klogg* installer configures the operating system to open `.log` files by
clicking them in the file manager.

#### Archives

*klogg* can open archives (`zip`, `7z`, and `tar`). The archive is extracted
to a temporary directory and standard open file dialog is presented to
select files. The type of archive is determined automatically by file
content or extension.

*klogg* can open compressed files (`gzip`, `bzip2`, `xz`, `lzma`). Such files are
decompressed to a temporary folder and then opened. The compression type is
determined automatically by file content or extension.

#### Remote URLs

*klogg* can open files from remote URLs. In that case, *klogg* will
download the file to a temporary directory and open it from there.

#### Recent files

*klogg* saves a history of recent opened files. Up to 5 recent files are
available from the `File` menu.

#### Favorite Files

Opened files can be added to the `Favorite Files` menu using
`Favorite Files -> Add Current File` or the toolbar.

This menu is used to provide fast access to files that are opened less
often and don't end up in the recent files section.

#### Clipboard

Pasting text from the clipboard to *klogg* also works. In this case, *klogg*
will save pasted text to a temporary file and open that file for
exploring.

#### Switching between opened files

Switching from one opened file to another can be done from the
`View -> Opened Files` menu or by using the `Ctrl+Shift+O` shortcut
which displays special dialogue to choose between opened files.

<a name="searching-folders"></a>

### Searching folders

Choose `File -> Open Folder...` to search a directory recursively. Folder
search is a snapshot search: each run scans the files and groups the results
under their source paths. Expand or collapse file groups to navigate the
results. Clicking a matching line opens its source file in the upper view at
that line, so you can inspect the surrounding log without leaving the folder tab.

The search bar supports literal or regular expression searches, case matching,
inverse matching, Boolean combinations, search history, and filter favorites.
`Keep Results` preserves the current results while the next search opens in a
new tab. Line marks, highlighters, and color labels are also available. Use
`Marks and matches`, `Marks`, or `Matches` to choose which results to display.

To include neighboring lines, set the number of context lines and select:

* `None`: matching lines only.
* `Before (-B)`: the selected number of lines before each match.
* `After (-A)`: the selected number of lines after each match.
* `Around (-C)`: the selected number of lines on each side of each match.

Folder mode does not support Auto-refresh, file watching, or a restricted
search range. Run the search again to reflect changes on disk; folder results
do not continuously follow growing files. Use a normal file tab or a device
live-log tab when you need continuous updates.

### Encodings

*klogg* tries to guess the encoding of an opened file. If that guess happens to
be wrong, then the desired encoding can be selected from the `Encoding` menu.

<a name="predefined-filters"></a>

### Predefined filters

Frequently used search patterns can be saved as filter favorites (predefined
filters). Configure them with `Tools -> Filter Favorites...`. Filter favorites
are shared by file, folder, and live-log searches.

Predefined filters are added to a dropdown near the search input and allow to 
add several patterns to regular expression. Predefined filter has a name
which is displayed in the dropdown, a pattern to add to search regular expression
and a setting to treat pattern as a regular expression
or simple text search.

It is possible to save the current search pattern as a predefined filter from
search input context menu.

<a name="using-highlighters"></a>

### Using highlighters

*Highlighters* can colorize some lines of the log being displayed
to draw attention to lines indicating an error, or to associate
a color with a certain type of event. 

Highlighters are grouped into sets. Multiple sets can be active at the same
time. Select the active sets using either the context menu or the
`Tools -> Highlighters` menu.

Any number of highlighters can be defined in a single set.
Highlighter configuration includes a regular expression to match
as well as color options. Another option is to use plain text patterns
in cases when complex regular expression are unnecessary.
Highlighters don't have support for logical search pattern combinations.

Each highlighter can be configured to apply foreground and 
background colors either to the whole line that matched its regular
expression or only to matching parts of the line. In the latter case,
if the regular expression contains capture groups then only the captured
parts of the matching line are highlighted.

It is possible to set a color variance. In that case different strings
that match the same regular expression will have slightly different color.

The order of highlighters in the set and the order of sets in configuration is important.
For each line all highlighters are tried from bottom to top. Each new matching 
highlighter overrides colors for the current line. 

Highlighter configuration can be exported to a file and 
imported on another machine. Each set is identified
by unique id. Only new sets are imported from the file. Please export the file
with a `.conf` extension to ensure *klogg* will be able to import it.

<a name="color-labels"></a>

### Color labels

In addition to predefined highlighters sets it is possible to create quick highlight rules
from selected text. These are called color labels. By default, *klogg* has 9
color labels enabled. Adding color label to selected text is done either
via the context menu or with keys `1` through `9` while the log view has focus.
Any number of strings can use a single color label. The `Ctrl+D` shortcut
applies the next color label to selected text.

To remove a color label from selected text, use the context menu or press `0`.
Use `Ctrl+Shift+0` to remove all color labels. Color labels are distinct from
line marks, which identify individual lines for navigation and filtering.

The colors that are used for text highlight can be configured from the color labels
tab of highlighters configuration dialog.

<a name="browsing-changing-log-files"></a>

### Browsing changing log files

In ordinary file tabs, *klogg* can display and search through logs while they
are written to disk. This might be the case when debugging a running program or
server. The log is automatically updated when it grows, but the
'Auto-refresh' option must be enabled if you want the search results to
be automatically refreshed.

The `'f'` key may be used to follow the end of the file as it grows (a
la `tail -f`).

*klogg* detects if new lines have been appended to the file or if the file has
been overwritten. In the former case, search results will be updated as new
matching lines appear in the file. If the file is overwritten, then
search results will be cleared. 

*klogg* has two options to distinguish appends from overwrites.
The general and more stable option is to recalculate the hash of the 
indexed part of the file and check if it matches current file on disk. 
This is reliable but can be slow for large files and for slow file systems
(e.g. network shares). The other option is to check hashes for only the 
first and last parts of the file. This usually works quickly 
but can skip over changes in the middle of the file. You can choose your 
preferred option in `Settings->File` tab.

The following file mode requires monitoring of the file system for any changes.
If native monitoring or polling are both disabled in settings, then the 
following file mode is also disabled.

### Scratchpad

Sometimes in log files there are text in base64 encoding, unformatted
xml/json, etc. For such cases *klogg* provides Scratchpad tool. Text can
be copied to this window and transformed to human-readable form.
Use context menu to either add data to the current scratchpad tab or 
replace its content with selected text. The default shortcuts are `Ctrl+E`
and `Ctrl+Shift+E`, respectively.

New tabs can be opened in Scratchpad using the `Ctrl+N` hotkey.

<a name="live-device-logs"></a>

## Live device logs

Device capture opens a live-log tab whose text can be searched, highlighted,
and marked like an ordinary log file. Use follow mode (`f`) to stay at the end
and Auto-refresh to update search results as data arrives. The supported
sources are Android Logcat and, on macOS, native iOS logs; there is no general
user command for capturing an arbitrary process.

<a name="android-logcat"></a>

### Android Logcat

1. Connect the Android device and enable USB debugging in its developer options.
1. Unlock the device and accept the USB debugging authorization prompt for this
   computer. The device must be online, not offline or unauthorized.
1. Choose `File -> Open ADB Logcat...`, refresh the device list if needed, and
   select the device. Choose the available Logcat filters and capture settings,
   then start capture.

*klogg* uses its bundled, managed ADB service through the ADB smart socket and
bundled helper. Installing an `adb` command on `PATH` is not a prerequisite.
If the device is missing, check the connection and authorization on the device,
then refresh the list before reconnecting.

<a name="ios-log-streams"></a>

### iOS log streams

`File -> Open iOS Log Stream...` is available only on macOS. Connect and unlock
the device, accept **Trust This Computer**, and complete pairing with the Mac.
Open the dialog, refresh the device list if needed, select the device, and
start capture.

Capture uses bundled native libimobiledevice components; it does not require
Python or start a Python capture process. The native source currently provides
unfiltered text output only, not source-side level, category, subsystem, or
JSON-output selection. You can still search and filter that text in *klogg*.

<a name="saving-live-logs"></a>

### Saving live logs

A new live capture uses temporary storage until you explicitly save it or close
it. Save important data before closing the tab; temporary capture storage is
not a durable archive.

Choose `Edit -> Save Live Log As`, then one of:

* `Without ANSI Sequences...`: remove terminal ANSI sequences from saved output.
* `With ANSI Sequences...`: preserve ANSI sequences in saved output.

Choose an output file. Saving writes the current retained capture snapshot,
catches up with data arriving during the save, and continues writing future
captured data to that output. It is not just a one-time export. Saving to a new
file does not concatenate an older family of rotated output files or recover
capture history that is no longer retained. Keep older output files separately
if you need that history.

Output can rotate according to the maximum file size and backup count; see
[Live Source settings](#live-source). These limits may delete older rotated
files, so select retention settings appropriate for your recording.

<a name="disconnecting-and-restoring-live-sessions"></a>

### Disconnecting and restoring live sessions

Use `Edit -> Disconnect Source` to stop incoming data without closing the tab,
and `Edit -> Reconnect Source` to start the source again. The default shortcuts
are `Ctrl+Shift+D` and `Ctrl+Shift+R`. A deliberate disconnect leaves the source
stopped even when automatic reconnection is enabled. Data produced by the
device while disconnected is not guaranteed to be recovered.

Saved live sessions restore stopped, even if they were running when *klogg*
exited or auto-reconnect was enabled. Inspect the restored capture and choose
`Reconnect Source` when you want to resume. Restoring a saved output does not
rewrite it; resumed output is appended, subject to the rotation settings.

Older sessions using a legacy process-based capture backend restore inert and
read-only; they do not launch saved commands. Open a new device capture through
the appropriate File menu action to resume using a supported source.

<a name="integrity-warnings-and-closing"></a>

### Integrity warnings and closing

A capture integrity warning means there may be missing or replayed data,
unpersisted bytes, or uncertain saved-output progress. Check the warning
shown for the live tab before relying on the capture as a complete record.
Saving may also ask you to acknowledge an integrity warning; saving cannot
repair data that was never captured.

When closing, *klogg* stops the source and finishes pending capture/output
writes. If it cannot finish safely, the close dialog offers:

* `Retry`: try to finish the safe close again.
* `Cancel`: leave the tab open so you can investigate or save the data.
* `Close Anyway (Possible Loss)`: close despite the warning; unsettled input
  or data still held only in memory may be lost.

<a name="settings"></a>

## Settings

### General

#### Search options

Determines which type of regular expression *klogg* will use when
filtering lines for the bottom window, and when using QuickFind.

*   Extended Regexp. The default, uses regular expressions similar to
    those used by Perl
*   Fixed Strings. Searches for the text exactly as it is written, no
    character is special

If incremental quickfind is selected, *klogg* will automatically restart
quickfind search when the search pattern changes.

Turning on highlight of matched text will cause the text that matched the
search pattern to be highlighted in both main view and filtered view.
Enabling color variation will cause the highlight color of different strings
that match the same pattern be slightly different.

Search size history controls the number of patterns that are saved for autocompletion
in the search input box.

Turning on option to run search on add or replace pattern will cause *klogg* to
immediately perform search when pattern is update from context menu.

#### Session options

*   Load last session -- if enabled, *klogg* will reopen files that were
    opened when *klogg* was closed. View configuration, marked lines and
    `follow` mode settings are restored for each file.
*   Follow file on load -- if enabled, *klogg* will enter `follow` mode
    for for all new opened files.
*   Minimize to tray -- if enabled, *klogg* will minimize to tray instead
    of closing main window. Use tray icon context menu of `File->Exit`
    to exit application. This option is not available on Mac OS.
*   Enable multiple windows -- if enabled *klogg* will allow opening
    more than one main window using `File->New window`. In this mode last
    closed windows will be saved to open session on next *klogg* start.
    When exiting *klogg* using `File->Exit` all windows are saved and
    will be reopened.

#### Version checking options

If version checking is enabled then *klogg* will try to grab a version
information file from the GitHub repository and see if a new version has been released
once per week.

Stable builds will check if a new stable version is available and pop a dialogue about it.
Testing builds will check for new testing versions.

### View

#### Font

The font used to display the log file. A clear, monospace font (like the
free, open source, [DejaVu Mono](https://dejavu-fonts.org/) for
example, is recommended.

Font antialiasing can be forced if auto-detected options result in low-quality
text rendering.

Font size can be changed from either main or filtered view using `Ctrl+Mouse wheel`
to zoom in/out.

#### Style

Qt usually comes with several options for drawing application widgets.
By default, *klogg* uses a style that matches current operating systems.
Other styles can be chosen from the dropdown menu.

*klogg* will try to respect current display manager theme and to
use white icons for dark themes. 

Another option is to select Dark or Windows Dark style. In this case *klogg*
will use a custom dark mode stylesheet. 

#### High DPI

Options in this group can be used in case *klogg* window looks 
bad on High DPI monitors. Usually, Qt detects the correct settings.
However, these options may be useful, especially for non-integer
scale factors manual overrides.

#### Miscellaneous

Some log files contain ANSI color codes to be displayed by terminals with
color support. These color codes create visual noise, so *klogg* provides
an option to hide them from both main and filtered view. However, enabling
this option will cause regular expression search to be slower.

### File

#### File change monitoring

If file change monitoring is enabled, *klogg* will use facilities
provided by the operating system to reload the file when data is changed on the
disk.

Sometimes this kind of monitoring is unreliable on
network shares or directories mounted via sftp. In that case, polling can
be enabled to make *klogg* check for changes.

*klogg* tries to detect if the file was changed in the already indexed
area. This mechanism involves hash recalculation and can be slow for
large files and network filesystems. If fast modification detection
is enabled *klogg* will check hash for the first and last parts of
changed files. This is faster but can skip over changes in the middle of
the file. This feature should be used with caution.

It is possible to enable follow file mode by scrolling past the end of file.
This behavior can be disabled.

#### Encoding

*klogg* tries to detect file encoding automatically. If encoding detection
is not required then it is possible to specify the encoding that will be
used for all new opened files.

#### Archives

If extract archives is selected then *klogg* will detect if opened file
is of one of supported archives type or a single compressed file and
will ask user permission to extract archives content to a temporary folder.

If you do not want *klogg* to ask for permission, check 
"extract archives without confirmation" option.

#### File download

By default, *klogg* will not download files using HTTPS if certificates
can't be checked. In some development environments self-signed 
certificates are used. In this case, *klogg* can be instructed to ignore
SSL errors.

<a name="live-source"></a>

### Live Source

The `Live Source` settings tab provides defaults for new device connections.
The device dialogs also expose capture and reconnection controls. Changes to
these defaults take effect for new live source connections.

* **Enable auto-reconnect on connection loss:** retry after an unexpected
  disconnection or error. Retry delays increase from 1 second up to 30 seconds.
  This does not automatically start a restored or deliberately stopped session.
* **Max reconnect attempts:** limit automatic retries; `0` means unlimited.
* **Max capture file size (MB):** rotate output when it reaches the size limit.
  `0` means unlimited size, with no size-based rotation.
* **Rolling backup count:** retain this many older rotated files and delete
  older files beyond the limit. `0` means keep all rotated files, not keep none.

Unlimited size or retention can consume all available disk space. Monitor free
space during long captures and save important output outside temporary storage.

### Advanced options

These options refer to the customization of performance related settings.

If parallel search is enabled, *klogg* will try to use several CPU cores
for regular expression matching. This does not work with quickfind.

*klogg* has several strategies for regular expression search based on file 
encoding. By default, it is optimized for files with UTF8 or single-byte
encodings. If most of the files are in multi-byte encodings then enabling
search optimization for non-latin encodings could improve performance.

If search results cache is enabled, *klogg* will store numbers of lines
that matched the search pattern in its memory. Repeating searches for the same
pattern will not go through all files but will use cached line numbers
instead.

In case there is an issue with *klogg*, logging can be enabled with
a desired level of verbosity. Log files are saved to a temporary directory.
A log level of 4 or 5 is usually enough. Enabling logging can slow down 
regular expressions search.

<a name="crash-reporting"></a>

## Crash reporting

Crash reporting depends on how *klogg* was built. The `KLOGG_USE_SENTRY`
build option is off by default; do not assume that every installation collects
or uploads crash reports. Builds with this integration can use the Crashpad
handler to collect minidumps and may offer to send them to developers.

When enabled, a crash report can provide information about:

* operating system: name, version, architecture, cpu features, system memory
* Qt version
* modules that were loaded into *klogg* process: filename, size and hashes for symbols
* stacktraces for all running threads in *klogg* process

Minidumps are not full process-memory dumps, but may still contain sensitive
fragments from memory. Review the reporting prompt and your organization's
policy before sharing a crash report.

<a name="keyboard-commands"></a>

## Keyboard commands

*klogg* keyboard commands try to approximately emulate the default
bindings used by the classic Unix utilities *vi* and *less*.

The main default commands are listed below. Menu shortcuts generally use
Command instead of Ctrl on macOS; consult the menu labels for your platform.
The jump-to-line shortcut is explicitly `Ctrl+G`.

Shortcuts apply when the corresponding log view or control has focus.

|Keys            |Actions                                                           |
|----------------|------------------------------------------------------------------|
|Up / Down       |move the selection one line up/down                               |
|Left / Right    |scroll one column left/right                                      |
|Alt+Up / Alt+Down|scroll one line up/down                                           |
|\[number\] j/k  |move the selection 'number' (or one) line down/up                 |
|h/l             |scroll left/right                                                 |
|\^ or \$        |scroll to beginning or end of selected line                       |
|\[number\] g    |jump to the line number given or the first one if no number is    |
|                |entered                                                           |
|Ctrl+Home       |jump to the first line of the file                                |
|Shift+G / Ctrl+End|jump to the last line of the file                               |
|Ctrl+G          |show jump to line dialog                                          |
|' or "          |start a quickfind search in the current screen                    |
|                |(forward and backward)                                            |
|F3 / Shift+N    |repeat the previous quickfind search forward/backward             |
|\* or .         |search for the next occurrence of the currently selected text      |
|/ or ,          |search for the previous occurrence of the currently selected text  |
|f               |activate 'follow' mode, which keep the display as the tail of the |
|                |file (like "tail -f")                                             |
|m               |add line marks to selected lines                                 |
|n               |delete line marks from selected lines                            |
|\[ or \]        |jump to previous or next marked line                              |
|+ or -          |decrease/increase filtered view size                              |
|v               |switch filtered view visibility mode                               |
|                |(Marks and Matches -&gt; Marks -&gt; Matches)                     |
|F5              |reload current file                                               |
|Ctrl+S          |Set focus to search string edit box                               |
|Ctrl+Shift+O    |Open dialog to switch to another file                             |

Shortcuts can be configured from the Shortcuts tab in the options dialog.

<a name="mouse-navigation"></a>

## Mouse navigation

Click a line to select it. `Shift+click` selects a range from the selection
anchor. `Ctrl+click` (`Command+click` on macOS) adds or removes individual lines
from a disjoint selection, so non-adjacent lines can be copied or given line
marks together. Drag across
text to select a fragment for searching or a color label.

Holding `Alt` while scrolling will scroll horizontally.
Holding `Shift` while scrolling will scroll faster.

<a name="command-line-options"></a>

## Command line options

Pass one or more file paths after the options, quoting paths containing spaces.
These are options for the graphical *klogg* application:

|Switch                         |Actions                                                   |
|-------------------------------|----------------------------------------------------------|
|`-h`, `--help`                  |print help message and exit                               |
|`-v`, `--version`               |print version information                                 |
|`-m`, `--multi`                 |allow multiple instances (use with `-s` to load the saved session) |
|`-s`, `--load-session`          |load the previous session (default when no file is passed) |
|`-n`, `--new-session`           |do not load the previous session (default when a file is passed) |
|`-l`, `--log`                   |write klogg's own diagnostic log to a file, not the viewed log |
|`-f`, `--follow`                |follow initially opened files                             |
|`-d <debug_level>`, `--debug <debug_level>` |enable diagnostic output with a numeric verbosity increment; for example, `--debug 2` |
|`--window-width <width>`        |set new-window width (default 1024)                        |
|`--window-height <height>`      |set new-window height (default 768)                        |

For example:

`klogg --new-session --follow "server.log"`

`klogg --debug 2 --log --window-width 1280 --window-height 800 "server.log"`

The debug option takes a number; repeated letters such as `-dddd` are not the
supported syntax. To save a device capture, use `Edit -> Save Live Log As`,
not the diagnostic `--log` option.
