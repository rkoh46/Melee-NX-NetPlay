# Slippi Sys files

Copied from the Slippi Ishiiruka repository (GPL-2.0): the Gecko code list the recompiler bakes in
(`GameSettings/GALE01r2.ini`), the code handler and bootloader the port installs at boot, and the
`GameFiles` diffs the EXI device serves to the game (menus, HUD, Slippi UI). The port reads this
folder at run time (`--sys-dir`, default `port/slippi_sys` relative to the working directory).
