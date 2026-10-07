# ls

`ls [-1] [-l] [--] [directory...]` lists directories, defaulting to the
inherited working directory. It includes dot names, preserves the `/` suffix
for directories, sorts names by unsigned byte order and prints headings for
multiple directory operands. File operands and recursive traversal are absent.
Unknown options fail before any directory is listed. Options may be combined
and appear between operands until `--` ends option parsing.

The actual stdout stream selects presentation. A console gets colored names
in columns sized to its reported width, ordered left to right and then down.
Directories are blue, `.pxe` names green, other files beginning with `#!` cyan,
and ordinary files use the default foreground. `.pxe` takes precedence over a
shebang. Terminal names/headings replace control and non-ASCII bytes with `?`,
one cell per byte; file/pipe output preserves names literally and uses one name
per line without colors. Missing terminal geometry falls back to one per line.

`-1` forces one name per line, retaining terminal colors. `-l` shows kind,
byte size and name on each line, retaining terminal colors and overriding the
column layout even with `-1`. Directory sizes are `-`; unavailable file sizes
are `?`, with diagnostics and failure status. There are no timestamps,
permissions or ownership columns. An unreadable shebang falls back to file
kind/color without failing the listing.

Names are collected before sorting; heap storage grows with the entries and
name bytes. An allocation failure or detected directory change abandons that
directory's listing without automatic retries. Other operands still run.
Enumeration and later file details are live observations, not a snapshot.

Plain file/pipe listings need enumeration alone and open no child files.
Terminal/long listings request only enumeration, lookup and file-read rights,
falling back to enumeration when detail authority is denied. Details use the
same retained directory and at most one owned READ-only file at a time; script
detection reads at most two bytes. All owned handles close before printing.
Stdout remains borrowed through its FILE owner. Output/cleanup failures return
failure; metadata failure still permits printing the complete collected names.
