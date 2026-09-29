/* win/poll.h - the game only polls to wait; there are no pollable fds on this host */
#ifndef WIN_POLL_H
#define WIN_POLL_H
struct pollfd { int fd; short events, revents; };
int poll(struct pollfd *fds, unsigned long nfds, int timeout);
#endif
