// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef CLI_CLI_H
#define CLI_CLI_H

/**
 * Entry point for the CLI mode of the download manager.
 *
 * Parses command‑line arguments, connects to the daemon via IPC, and
 * dispatches the requested operation (add, pause, resume, cancel).
 *
 * @param argc  argument count (as received by main)
 * @param argv  argument vector (as received by main)
 * @return      0 on success, non‑zero on error
 */
int run_cli(int argc, char **argv);

#endif /* CLI_CLI_H */
