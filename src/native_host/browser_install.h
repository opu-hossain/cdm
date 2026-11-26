#ifndef NATIVE_HOST_BROWSER_INSTALL_H
#define NATIVE_HOST_BROWSER_INSTALL_H

/* Per-user Linux registration: Chrome, Chromium, Firefox, Edge, Brave,
 * Opera (shared Chrome namespace), Vivaldi. New flags require a profile root. */
int browser_install_main(int argc, char **argv);

#endif
