# coil commands

## Command line

Press `:` in Normal or Select mode to open the command line in the status
row. The typed text appears after the colon, with a cursor cell at its end.
In Insert mode `:` types a colon.

| Key | Effect |
|-----|--------|
| Enter | run the line |
| Esc | cancel; mode and selection are kept |
| Backspace | erase one rune; on an empty line closes the prompt |
| Ctrl-W | erase trailing blanks and the word before them |
| Ctrl-U | erase the whole line |
| Up / Down | recall earlier / later commands (32 are kept; empty lines and an immediate repeat are not stored) |

The line is edited only at its end and is at most 1024 bytes ("prompt full").

## Ex commands

Paths are literal: no expansion, no shell.

| Command | Effect | Messages |
|---------|--------|----------|
| `:w` | write the file | `wrote PATH N` |
| `:w file` | write a copy; the buffer keeps its name and modified flag (an unnamed buffer takes the name, only once the write succeeds) | `PATH exists; :w! PATH overwrites` when the file exists |
| `:w! file` | write a copy over an existing file | |
| `:q` | quit | `unsaved changes; :q! quits` when modified; `q: too many arguments` |
| `:q!` | quit, discarding changes | |
| `:wq` | write and quit | |
| `:e file` | open another file; `file:N` jumps to line N | `unsaved changes; :e! discards them` when modified |
| `:e! file` | open another file, discarding changes | |
| `:e!` | reload the current file, keeping the cursor line | |
| `:N` | go to line N (one selection); `:0` is line 1; past the end clamps to the last line | |
| `:num [on\|off]` | show or hide the line-number gutter (on by default, per window, not saved; hidden when the window is too narrow); no argument toggles | `line numbers on` / `line numbers off`; `usage: num [on\|off]` |
| `:lang [c\|rc\|mkfile\|markdown\|plain]` | force the language (highlighting and tab setting); no argument reports it. `:w name` on an unnamed buffer detects again and overrides a forced language | `lang: NAME`; `unknown language 'X'` |

Any other line is a structural command (next section). Ctrl-S still saves and
Ctrl-Q still quits after a second press when the buffer is modified.

## Saving safely

`:w`, `:wq`, `:w file` and Ctrl-S write the file in place. An existing file keeps
its permissions and owner; a new file is created. There is no temporary file and
no rename.

- **Changed on disk.** coil remembers the file's identity (type, dev, qid.path,
  qid.vers, mtime) when it reads or writes it. If another program changed the file
  since, `:w` refuses with `PATH changed on disk; :w! overwrites`. A file counts as
  changed when its qid.path or qid.vers differ, or its mtime is more than one second
  newer than remembered (the one-second slack is acme's). A file deleted since it was
  read is not "changed": `:w` recreates it. A file that appeared after coil opened a
  non-existent name is changed. If the identity cannot be read back after a write, the
  next write skips the check instead of refusing falsely.
- **`:w!`** skips only the change check. `:e!` re-reads the file and refreshes the
  remembered identity. Writing a copy (`:w file`) never touches it.
- **Always refused, also with `:w!`:** a directory (`PATH: is a directory`) and an
  append-only file that already has content (`PATH: append-only file, not written`).
  An append-only file can still be opened.
- **Write errors** (permission denied, disk full, close failure) show as
  `PATH: ERROR`. The buffer stays modified with its undo history, and `:q` still warns.

## Snarf

`y`, `d` and `c` (which yank first) write the yanked text to `/dev/snarf`, so it pastes in a rio window or in sam. `p`, `P` and `R` read `/dev/snarf` every time, so text snarfed elsewhere pastes in coil. `/dev/snarf` is opened fresh for each operation (it only updates at close).

- Several selections are written joined by newlines. Pasting into the same number of selections still pairs them one to one, because coil recognises its own last write (a hash of the content) and uses its register.
- If another program changed the snarf since coil's last yank, that text replaces the register and pastes into every selection.
- A yank over 100 KiB is not written (rio would commit a partial write). coil says `yank N bytes: too big for snarf, kept in coil`; `p` still pastes it from the register until another program snarfs something.
- If a write fails: `snarf: ERR; kept in coil`. The text stays in the register.
- If `/dev/snarf` does not exist (a `none` attach): `no /dev/snarf; using coil's register`, reported once per session.
- An empty or unreadable snarf pastes from the register. Bytes pass through untouched. A snarf over 16 MiB is not read (the register is used).

## Search

`/` opens a search prompt in the status row (same editing keys as `:`, history of its own). Enter searches forward from the end of the primary selection; the next match becomes the one selection (an empty match becomes a one-rune cursor). In Select mode the primary's anchor is kept, so the selection extends. `n` repeats the last search. An empty pattern (`/` Enter) reuses the last one. The count prefix is ignored. `?` opens the same prompt for a backward search: Enter selects the nearest match that starts before the selection (overlapping matches count, so `aa` in `aaa` found from the end gives the last two letters). `N` repeats the last search in the opposite direction, `n` in its own direction. `*` searches forward for the selected text literally (regexp characters are not special); on a one-rune cursor it first selects the word under it. In Select mode a backward search keeps the anchor and moves the head to the match start.

| Message | Meaning |
|---------|---------|
| `search wrapped` | nothing after the selection (forward) or before it (backward); the match is the first one from the start, or the last one from the end, of the file |
| `no match: PATTERN` | no match anywhere; the selection is unchanged |
| `no previous pattern` | `n`, `N` or `/` `?` Enter before any search |
| `bad regexp: ...` | the pattern did not compile; mode, selection and the previous pattern are unchanged |
| `selection too long to search` | `*` on more than 1024 bytes |
| `regexp too long` | patterns are limited to 1024 bytes |

Patterns are Plan 9 regexp(6), the system libregexp, not POSIX and not Helix/Rust:

- Supported: literal characters, `.` `[...]` `[^...]` `*` `+` `?` `|` `( )` `^` `$`, and a backslash makes the next character literal.
- `\n` matches a newline and `\t` a tab (coil translates them; libregexp itself reads `\n` as the letter n). `\\n` is a backslash followed by n.
- NOT supported: `\d` `\w` `\s`, `{n,m}`, non-greedy `*?`, `(?:...)`. They do not error; they match the wrong thing (`\d` is a literal d, `a{2}` is the text `a{2}`).
- `.` and `[^x]` never match a newline; write `\n` explicitly to cross a line end. `^` and `$` match at every line start and end.
- Matches are found one 256 KiB window at a time. A pattern containing `\n` also looks 64 KiB past the window; a single line longer than 256 KiB can hide a match that straddles an internal cut.

## Structural commands

A `:` line whose first word is not exactly `w`, `wq`, `q`, `e` (with or without `!`) or all digits is a sam-style structural command: select first, then act on what the selection holds.

```
line  := [ % | , ] chain
chain := (x|y) [/re/] chain | (g|v) /re/ chain | term | (nothing)
term  := d | (c|a|i) /text/ | s /re/text/ [g]
```

- Blanks are allowed between elements. The delimiter is any character except letters, digits, backslash, blank and newline. The closing delimiter is optional at the end of the line. Inside a pattern `\/` (the delimiter) means the delimiter itself; every other backslash pair goes to the regexp as is (so `\n` and `\t` work, see Search).
- In text, `\n` is a newline, `\t` a TAB, `\/` the delimiter, `\\` a backslash; any other `\x` stays two characters.
- A chain nests at most 8 steps (`command nested too deeply`).

| Command | Acts on each current dot |
|---------|--------------------------|
| `x/re/` | every non-overlapping match inside the dot becomes the new dot (empty matches follow the search rule; `baab` with `a*` matches before the b, `aa`, and at the end, as in sam) |
| `x` | bare: every line of the dot, with its newline (the last may lack one) |
| `y/re/` | the text between matches, including the text before the first and after the last match (empty gaps too); bare `y` iterates lines like `x` |
| `g/re/` | keep the dot if it contains a match, else skip it |
| `v/re/` | keep the dot if it contains no match |
| `d` | delete the dot |
| `c/text/` | replace the dot with text |
| `i/text/` | insert text before the dot |
| `a/text/` | insert text after the dot |
| `s/re/text/` | replace the first match inside the dot with text (`g` after the closing delimiter: every non-overlapping match) |

Range: the starting dots are the current selections (each one), or the whole buffer with a `%` or `,` prefix. `%x/foo/ c/bar/` is "replace every foo". `g` and `v` test the dot as a whole, so with a single selection `:g/re/ d` tests only that selection; for line-wise deletion of matching lines use `%x g/re/ d` (every line is a dot).

Examples: `%x/foo/ c/bar/`; `%y/,/ c/;/`; `%x g/TODO/ d` (delete lines with TODO); `%x v/^#/ x/int/ c/long/` (int to long on lines not starting with #); `x/ +$/ d` (trailing blanks inside the selection).

Result (what is selected afterwards):

| Command ends in | Selections |
|-----------------|------------|
| `c` `i` `a` | one per edit, covering the inserted text (an empty text leaves a caret); the last is primary |
| `d` | one-rune cursors at the deletion points |
| nothing (bare `x` / `y` chains) | the iterated ranges, the first is primary; the mode becomes Normal |
| nothing, g/v only | unchanged |
| no match | unchanged, message `no match` (`no changes` when matches existed but nothing changed) |

After an edit the message is `N changes` (`1 change`). All the nested commands see the buffer as it was before the command; the edits are collected and applied together, so the whole command is one undo step: one `u` restores text and selections, `U` redoes it. Not applied until the job ends: Esc (or Del) interrupts, leaving the buffer exactly as before.

Esc in Normal mode drops every selection except the primary, so a bare `:%x/re/` followed by Esc collapses back to the first match (A12).

Differences from sam: a bare `x` or `y` selects instead of printing; `\t` works in text; no braces, no line or character addresses (`addresses are not supported; use % or a selection`), no `p u k m t b B n q r w < > | !` or `X Y` (`unknown command 'L'`); no shell is ever started.

### Substitute

`s/re/text/` and `s/re/text/g` are terminals like `c`: they work alone (on each selection) or after x, y, g, v. In the replacement text:

| Written | Becomes |
|---------|---------|
| `&` or `\0` | the whole match |
| `\1` .. `\9` | that group (an unset or missing group is empty) |
| `\&` | a literal `&` |
| `\n` `\t` | a newline, a TAB |
| `\\` | a backslash |
| `\/` (the delimiter) | the delimiter |
| any other `\x` | `x` |

A count (`s2/a/b/`) is an error (`s: counts are not supported`); `s/a` without a replacement is `missing replacement for s`. Afterwards each replacement text is selected (the last is primary), the whole command is one undo step, and the message is `N changes`. Without any match: `no match`.

### The last pattern

An empty pattern (`x//`, `g//`, `v//`, `s//Y/`) reuses the last pattern, the one `/` and `?` use (`no previous pattern` when there is none). A command with a non-empty pattern does not replace the search pattern, so `n` keeps finding what you last searched for.

### Limits

- A command may make at most 250000 changes (`Maxedits`). More stops with `too many changes (more than 250000); narrow the range` and nothing is applied. doc_apply, undo and redo are linear for batches over 16 edits (one merge pass over the piece array). `make -C src bench`, 50 MB buffer, N one-byte replacements, milliseconds, before (quadratic) / after: apply 50000 14828 / 11, 100000 60609 / 21, 200000 316753 / 32; undo 9415 / 5, 38995 / 10, 218659 / 17; redo 9479 / 4, 44992 / 9, 199264 / 17. Confirmed on the VM.
- If the text would grow past the buffer limit the command stops with `buffer full`, again with nothing applied.
- With more than 50000 changes (`Applynote`) the status row first shows `applying N changes...` and the window redraws; applying them is the one step that cannot be interrupted.

### Cookbook

- Rename a whole word in the file (regexp(6) has no word-boundary operator, and `^` `$` inside a dot still mean the real line start and end, as in sam): `:%x/[A-Za-z_0-9]+/ g/old/ v/[A-Za-z_0-9]old|old[A-Za-z_0-9]/ c/new/`
- Delete lines matching: `:%x g/re/ d`
- Change inside the selection: `:x/re/ c/t/`
- Substitute on non-comment lines: `:%x/.*\n/ v/^#/ s/a/b/g`

## Highlighting limits

The highlighter is exact for the common cases and approximate in a few bounded ways:

- A line is highlighted only up to its first 4096 bytes (`Hlcap`); a line longer than 65536 bytes is cut on screen (`Maxline`).
- After a jump far into a file (cold start), the state is rebuilt from at most 2000 lines above (`Hllook`); a block comment or Markdown fence opened earlier is not seen until an edit above it.
- A block comment that ends inside a preprocessor line is not recognised.
- rc here-documents and mkfile backslash continuation lines are not tracked.
- Markdown: setext headings, indented code, reference links, HTML and nested emphasis are not recognised.
- The bracket match looks at most 500 lines (`Brlines`) and 64 KiB (`Brbytes`) from the cursor.

## Long operations

Searches and structural commands run in slices of 256 KiB between input events, so the window keeps redrawing.

- While one runs the status row shows `searching NN% (Esc interrupts)` (`running NN% ...` for a structural command). A search that ends in its first slice shows no progress.
- Esc or Del interrupts it: message `interrupted`, the selection and the text are exactly as before.
- Any other key is dropped with `busy: Esc interrupts`; it is never replayed afterwards.
- The mouse wheel still scrolls; clicks and drags are ignored until the job ends.
