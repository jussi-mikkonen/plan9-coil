# coil

A modal text editor for 9front, in the style of Helix and Kakoune: select
first, then act on the selection.

coil is an ordinary libdraw program: it opens its own window and draws
syntax-highlighted text through libcell, so it runs unchanged in
rio, in rini and nested. There is no terminal and no escape sequences, so Esc
is just a key.

- Multiple selections; motions move or extend them, verbs act on all of them
  as one undoable change.
- Structural commands in the spirit of sam: `x/re/`, `y/re/`, `g/re/`,
  `v/re/` and `s/re/text/`, using the system regexp(6).
- Syntax highlighting for C, rc, mk and Markdown.
- Snarf through `/dev/snarf`, shared with sam, acme and rio.
- A piece-table buffer over the raw bytes: files with NULs or invalid UTF-8
  are saved back exactly as read, and tens-of-MB files open and scroll
  without stalls.
- Refuses to overwrite a file changed on disk since it was read.

## Build and install

libcell must be installed first (`mk install` in plan9-libcell). On 9front:

	cd src
	mk install	# $home/bin/$objtype/coil, /sys/man/1/coil
	mk test

Then `coil file` or `coil file:N`. See `man 1 coil` for the keys and
commands, and `docs/COMMANDS.md` for the command line in more detail.

Colours come from `$home/lib/coil/theme` (format in cell(2)); without it a
built-in dark theme is used. A light, acme-like theme:

	mkdir -p $home/lib/coil
	cp lib/theme.light $home/lib/coil/theme

## Development

The buffer, selections, commands, view and highlighter are plain C with no
Plan 9 dependencies. `mk test` runs their golden tests on 9front; on Linux:

	make test

The Linux tests link a vendored copy of 9front's libregexp
(`src/tools/libregexp`) so they run the same regexp engine as 9front; the
9front build uses the system library. `make check9` checks the Plan 9 sources
with gcc against a copy of the 9front tree in libcell's `.front-ref/`.
`make bench` times large batched edits. `tools/vmcheck.rc` builds, tests and
installs libcell and coil on a 9front machine and logs the output.

## Notes

Written with the help of an AI coding assistant.

## License

MIT, see [LICENSE](LICENSE). The vendored libregexp is 9front's, under its
MIT licence; see `src/tools/libregexp/NOTICE`.
