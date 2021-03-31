# Dlog

Dlog is a log file aggregator and a relaying/filtering server that can also be used as a logging backend with log file rotation built-in.

It's written in plain C and has no external library dependencies. To compile it you'll need a new-ish C compiler, gnumake and yacc/bison.

Compile it with typical `[g]make [debug]`, clean build with `make clean`. It has no explicit installation step as it produces a single executable which you can copy anywhere you like.

Dlog was written to solve a very particular problem of decoupling servers from logging and monitoring/alerting services, while staying as close to realtime response as possible.

## Features

Dlog can multiplex any number of _sources_ to any number of _destinations_, while applying processing _rules_ on the data in flight.

Dlog works on text input, line by line. Incoming empty lines are ignored.

Dlog has its own language to describe endpoints and processing rules for extracting data from incoming log lines, recombining it in various ways, and sending it out.

Dlog supports log rotation when used as a logging backend - simply create a _rotlog_ destination and point the incoming data stream to it. It will then make sure to rotate the resulting files based on a predefined file size, while maintaining the integrity of the full line of text (i.e. a log line will never be split between two files). Rotation is based on file size, but rotation can be also triggered with the USR1 signal (if you prefer to have time-based rotation triggered from cron).

Dlog works on Linux and BSDs (including macOS) and will use the system's native support for asynchronous IO (inotify/epoll on Linux, and kqueue on BSDs). To keep it simple to mantain and integrate, it's a single-threaded forking daemon, using asynchronous IO where possible.

Dlog uses Lua's "patterns" in place of a full-blown POSIX regex library.This keeps the code small with no external dependencies, while providing most of the regex needs.

Dlog supports uninterrupted binary upgrades. Move (`mv`) the new binary in place of the current one and send `SIGHUP` to Dlog. Open socket connections will be maintained and the upgrade process should be transparent to the client process(es). 

## Sources

Dlog supports collecting data from files, FIFOs (named pipes) and TCP sockets.

All the sources will be opened once physically available - it is valid to start Dlog early while sources might still be missing.

1. File source is built with expectation of typical log files. A file watcher will be set up, if the file is not available, and the file will be read once it appears. A file will be monitored for deletion and similar events and will be reopened once available again (e.g. the file went through log rotation).

2. FIFOs work similarly to files, and

3. TCP socket sources come through a permanent listening socket, which can be disabled if you don't expect network traffic.

## Destinations

Similar core set as with sources:

1. File destination is a raw file to be written to (can be an existing file), which will not be rotated and will potentially grow indefinitely. If the file is deleted by external process it will be recreated.

2. FIFO is similar to files

3. TCP socket will, similarly to files, keep trying to (re)connect to remote host if not available.

4. Rotation log (_rotlog_) is built on top of the basic file destination. It supports rotation based on file size (configurable, global), and will rotate the logs if you send USR1 signal to dlog.

A certain amount of buffering is available for destinations; new lines will be dropped if the buffer is full (e.g. due to destination disappearing).

## Configuration language

Dlog has its own configuration language used to describe three distinctive sections: server configuration, sources and destinations, and data processing rules.

Here is a small example of a very simple configuration file that connects to a source file `app.log`, prepends a timestamp and the value of an environment variable `$APPNAME` to each line, and sends it to `rotlog` destination.

	source file "/var/log/app.log" as SRC;
	destination rotlog "/var/log/dlog.rotlog" 1000000 as DST;

	rule {
		matchall from SRC {
			write "%{t} - %{env:APPNAME} - %{m}" DST;
		}
	}
	

### Basics

Whitespaces are ignored. 
Comments start with `#` and extend to end of the line.
One statement per line, ending with `;`.
No arithmetic operations, the language is based around strings.

`include <config file>` can be used in any point of the file to include additional file, verbatim. Declarations from the included file are inserted at the point of invocation. This is useful for including declarations based on environment (dev, test, prod, etc.)

### Variables

A variable is introduced with

	var <symbol> = <partial: string>
	
Variables (and symbols in general) can start with a letter or an underscore (`_`) and can contain any alphanumeric character after that.

### Variable substitution (partials)

Any string marked as _partial_ in this document is parsed according to variable expansion rules. Anything between `%{` and `}` is interpreted thusly:

- `1-9` Capture group from previous regex (assumes there was a `match` rule)
- `s`	The source symbol where currently processed line comes from
- `d`	Date and time (configurable format)
- `m`	Current line, verbatim
- `T`	Same as `%{d}.%{t}`
- `t`	Sub-second (configurable resolution)
- `<var>`	Current value of variable `var`
- `env:ENV`	The value of environment variable `ENV` when Dlog was started

Any text outside `%{...}` is copied verbatim.

Configuration file consists generally of three sections.

### Server configuration section

The values presented here are optional - if missing, Dlog will use default values hardcoded in `def.h`

- `pidfile <path_to_pidfile>`		Full path to Dlog's pidfile. Default value is `DLOG_OPT_PIDFILE`. 
- `logfile <path_to_logfile>`		Full path to Dlog's log file. If missing the default log file will be `DLOG_OPT_LOGFILE` in working directory. 
- `datetimeformat <string>`			Format string compatible with `man 3 strftime`. Default value is `DLOG_DEFAULT_DATETIME_FORMAT`
- `timestampresolution <none|milisecond|microsecond|nanosecond>` sub-second resolution of the timestamp (see below). Global value for all timestamps.

### Sources and destinations section

This section creates sources and destinations. General format of this section is:

	source|destination <type> <... options ...> as <symbol>
	
"Symbol" is a string alias that you can refer to later in the `rule` section. 

Currently supported sources and destinations are:

	source file <partial: full_path> as <symbol>
	source fifo <partial: full_path> as <symbol>
	destination file <partial: full path> as <symbol>
	destination rotlog <partial: full path> <string:rotation size in bytes> as <symbol>
	destination tcp <partial: hostname> <partial: port number> as <symbol>

TCP socket source is implicitly available via `TCP_SOCKET` symbol.

### Matching and Filtering

Rules section begins with the `rule {` block and contains other rule statements inside. The rules can be nested arbitrarily.
	
Supported rule statements are few:


	match <partial: regex> <partial: target> [from <source symbol>] {
		... execute if matched ...
	} else {
		...
	}
	
This is a general regex matching rule. If the regex matches the target, the enclosing block is executed. In which case capture groups are made available to inner statements and can be accessed with `%{1-9}`.

Optionally, you can filter on the incoming source, to avoid adding additional `match` blocks.

__NOTE__: Regex capture groups are only available to the enclosed block - if there is another `match` block inside the current then the capture groups available further down will be overriden. The way to preserve captures is to use variables.

	matchall from <source symbol> {
		...
	} 
	
A shortcut that lets all the input through for a given source symbol.

### Block statements

The match block supports the following statements:

- `write <partial: string> <destination symbol>`	The write statement will write its first argument (_partial_) to its second argument (destination symbol). This is the main method of recombining the output text and sending it to output (destinations).

- `break`	The break statement will leave the current rule, not just the enclosing block.

- `<var symbol> = <partial: string>`	Variable assignment, resolves the _partial_ and assigns it to a variable.


## Running Dlog

Run Dlog with

	dlog [-ntv] [-l listen_port] -c <config file>
	
### Supported command line options:

- `-n`	Start Dlog in foreground (non-daemon) mode. Useful for testing and debugging.
- `-t`	Test configuration only, and then exit
- `-l <port>` Specify socket listening port. (This value will override that from configuration file. At least one value is required to enable tcp server).
- `-v` and `-?`	Show help message and exit
- `-c <file>` 	Specify configuration file. Required.

### Supported signals:

- `SIGUSR1`		Send this signal to cause _rotlog_ destination files to rotate
- `SIGQUIT`		Orderly shutdown
- `SIGHUP`		Binary upgrade or restart. No interruption, if possible.
