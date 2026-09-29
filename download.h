#ifndef DOWNLOAD_H
#define DOWNLOAD_H

enum { DL_OK = 0, DL_ERROR = -1, DL_CANCELLED = 1, DL_RESTART = 2 };

/* done/total in bytes (total 0 if unknown), speed in bytes/s; return false to cancel */
typedef bool (*dl_progress_fn)(u64 done, u64 total, double speed, void *ud);

/* downloads url to path (via path.part, resumed if present); size 0 = unknown.
 * DL_OK, DL_CANCELLED (the .part stays for next time) or DL_ERROR with err filled in */
int http_download(const char *url, const char *path, u64 size, dl_progress_fn cb, void *ud, char *err, size_t errn);

#endif
