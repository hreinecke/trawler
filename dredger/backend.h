#ifndef _BACKEND_H
#define _BACKEND_H

struct backend {
	const char *name;
	int (*parse_options) (struct backend *be, char *optionstr);
	int (*open) (struct backend *be, char *fname);
	int (*check) (struct backend *be, char *fname);
	int (*migrate) (struct backend *be, int dst_fd, int src_fd);
	int (*unmigrate) (struct backend *be, int dst_fd, int src_fd);
	void (*close) (struct backend *be, char *fname, int fd);
	void *options;
};

extern struct backend backend_file;

#define open_backend(be,fname) (be)->open(be,fname)
#define check_backend(be,fname) (be)->check(be, fname)
#define migrate_backend(be,dst_fd,src_fd) (be)->migrate(be, dst_fd, src_fd)
#define unmigrate_backend(be,dst_fd,src_fd) (be)->unmigrate(be, dst_fd, src_fd)
#define close_backend(be,fname,fd) (be)->close(be, fname, fd)

#endif
