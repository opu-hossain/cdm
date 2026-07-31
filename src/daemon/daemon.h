// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef DAEMON_DAEMON_H
#define DAEMON_DAEMON_H

/**
 * Start the download manager daemon.
 *
 * If stdin is not a terminal the process detaches from the controlling
 * terminal (double fork).  The daemon initialises the database, restores
 * any persisted queue, opens the IPC socket and enters the main event
 * loop (tick every ~200 ms).  It does not return unless initialisation
 * fails.
 *
 * @return 0 on clean shutdown, non‑zero on error
 */
int run_daemon(void);

#endif /* DAEMON_DAEMON_H */
