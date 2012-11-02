#ifndef _BACKEND_H
#define _BACKEND_H

struct backend;

struct backend_template {
	const char *name;
	int (*parse_options) (struct backend *be, char *args);
	struct backend * (*new) (void);
	int (*open) (struct backend *be, char *fname);
	int (*check) (struct backend *be, char *fname);
	int (*migrate) (struct backend *be, int fe_fd);
	int (*unmigrate) (struct backend *be, int fe_fd);
	void (*close) (struct backend *be);
};

struct backend {
	struct backend_template *template;
};

struct backend *new_backend(const char *name);
int parse_backend_options(struct backend *be, char *args);
int open_backend(struct backend *be, char *fname);
int check_backend(struct backend *be, char *fname);
int setup_backend(struct backend *be);
int migrate_backend(struct backend *be, int fe_fd);
int unmigrate_backend(struct backend *be, int fe_fd);
void close_backend(struct backend *be);

#endif
