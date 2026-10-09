// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2004-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "command.h"
#include "input.h"
#include "init.h"
#include "io.h"
#include "xfs_errortag.h"

static cmdinfo_t inject_cmd;

#define ARRAY_SIZE(x)		(sizeof(x) / sizeof((x)[0]))
#define __stringify_1(x...)	#x
#define __stringify(x...)	__stringify_1(x)

#define XFS_ERRTAG(_tag, _name, _default) \
        [XFS_ERRTAG_##_tag]	=  __stringify(_name),
#include "xfs_errortag.h"
static const char *tag_names[] = { XFS_ERRTAGS };
#undef XFS_ERRTAG

/* Search for a name */
static int
error_tag(
	char		*name)
{
	unsigned int	i;

	for (i = 0; i < ARRAY_SIZE(tag_names); i++)
		if (tag_names[i] && strcmp(name, tag_names[i]) == 0)
			return i;
	return -1;
}

/* Dump all the names */
static void
list_tags(void)
{
	unsigned int	count = 0, i;

	fputs("tags: [ ", stdout);
	for (i = 0; i < ARRAY_SIZE(tag_names); i++) {
		if (count > 0) {
			fputs(", ", stdout);
			if (!(count % 5))
				fputs("\n\t", stdout);
		}
		if (tag_names[i]) {
			fputs(tag_names[i], stdout);
			count++;
		}

	}
	fputs(" ]\n", stdout);
}

static void
inject_help(void)
{
	printf(_(
"\n"
" inject errors into the filesystem of the currently open file\n"
"\n"
" Example:\n"
" 'inject readagf' - cause errors on allocation group freespace reads\n"
"\n"
" Causes the kernel to generate and react to errors within XFS, provided\n"
" the XFS kernel code has been built with debugging features enabled.\n"
" With no arguments, displays the list of error injection tags.\n"
"\n"));
}

static int
inject_f(
	int			argc,
	char			**argv)
{
	xfs_error_injection_t	error;
	int			command = XFS_IOC_ERROR_INJECTION;

	if (argc == 1) {
		list_tags();
		return 0;
	}

	while (--argc > 0) {
		error.fd = file->fd;
		if ((error.errtag = error_tag(argv[argc])) < 0) {
			fprintf(stderr, _("no such tag -- %s\n"), argv[1]);
			continue;
		}
		if (error.errtag == XFS_ERRTAG_NOERROR)
			command = XFS_IOC_ERROR_CLEARALL;
		if ((xfsctl(file->name, file->fd, command, &error)) < 0) {
			perror("XFS_IOC_ERROR_INJECTION");
			exitcode = 1;
			continue;
		}
	}
	return 0;
}

void
inject_init(void)
{
	inject_cmd.name = "inject";
	inject_cmd.cfunc = inject_f;
	inject_cmd.argmin = 0;
	inject_cmd.argmax = -1;
	inject_cmd.flags = CMD_NOMAP_OK | CMD_FLAG_ONESHOT;
	inject_cmd.args = _("[tag ...]");
	inject_cmd.oneline = _("inject errors into a filesystem");
	inject_cmd.help = inject_help;

	if (expert)
		add_command(&inject_cmd);
}
