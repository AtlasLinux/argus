#ifndef ARGUS_INPUT_H
#define ARGUS_INPUT_H

/* Initialize libinput (returns 0 on success) */
int input_init(void);

/* Shutdown libinput */
void input_fini(void);

/* Return file descriptor to poll for libinput events (or -1) */
int input_get_fd(void);

/* Poll/process events once (call when fd is readable) */
int input_dispatch(void);

#endif
