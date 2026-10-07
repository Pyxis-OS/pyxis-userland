# echo

`echo [ -n ] [ARG...]` writes arguments to inherited stdout, separated by one
space, followed by a newline. An exact first argument `-n` suppresses the
newline; later arguments, including `-n`, are literal. No other options or
escape processing are provided. With no arguments it writes a newline;
`echo -n` writes nothing.

Output errors are reported on stderr and return failure. The command needs
only its stdout binding and performs no path lookup or file opening; the shell
supplies file redirection and pipeline streams.
