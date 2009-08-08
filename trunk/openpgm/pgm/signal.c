/* vim:ts=8:sts=8:sw=4:noai:noexpandtab
 *
 * Re-entrant safe signal handling.
 *
 * Copyright (c) 2006-2009 Miru Limited.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>

#include "pgm/signal.h"


//#define SIGNAL_DEBUG

#ifndef SIGNAL_DEBUG
#define g_trace(...)		while (0)
#else
#define g_trace(...)		g_debug(__VA_ARGS__)
#endif


/* globals */

static pgm_sighandler_t g_signal_list[NSIG];
static int g_signal_pipe[2];
static GIOChannel* g_signal_io = NULL;

static void on_signal (int);
static gboolean on_io_signal (GIOChannel*, GIOCondition, gpointer);
static const char* cond_string (GIOCondition);


/* install signal handler and return unix fd to add to event loop
 */

gboolean
pgm_signal_install (
	int			signum,
	pgm_sighandler_t	handler,
	gpointer		user_data
	)
{
	g_trace ("pgm_signal_install (signum:%d handler:%p user_data:%p)",
		signum, (gpointer)handler, user_data);

	if (NULL == g_signal_io)
	{
		if (pipe (g_signal_pipe))
			return FALSE;
/* write-end */
		int fd_flags = fcntl (g_signal_pipe[1], F_GETFL);
		if (fd_flags < 0)
			return FALSE;
		if (fcntl (g_signal_pipe[1], F_SETFL, fd_flags | O_NONBLOCK))
			return FALSE;

/* read-end */
		fd_flags = fcntl (g_signal_pipe[0], F_GETFL);
		if (fd_flags < 0)
			return FALSE;
		if (fcntl (g_signal_pipe[0], F_SETFL, fd_flags | O_NONBLOCK))
			return FALSE;

/* add to evm */
		g_signal_io = g_io_channel_unix_new (g_signal_pipe[0]);
		g_io_add_watch (g_signal_io, G_IO_IN, on_io_signal, user_data);
	}

	g_signal_list[signum] = handler;
	return (SIG_ERR != signal (signum, on_signal));
}

/* process signal from operating system
 */

static
void
on_signal (
	int		signum
	)
{
	g_trace ("on_signal (signum:%d)", signum);
	if (write (g_signal_pipe[1], &signum, sizeof(signum)) != sizeof(signum))
	{
		g_critical ("Unix signal %s (%i) lost", strsignal(signum), signum);
	}
}

/* process signal from pipe
 */

static gboolean
on_io_signal (
	GIOChannel*	source,
	GIOCondition	cond,
	gpointer	user_data
	)
{
/* pre-conditions */
	g_assert (NULL != source);
	g_assert (G_IO_IN == cond);

	g_trace ("on_io_signal (source:%p cond:%s user_data:%p)",
		(gpointer)source, cond_string (cond), user_data);

	int signum;
	const gsize bytes_read = read (g_io_channel_unix_get_fd (source), &signum, sizeof(signum));

	if (sizeof(signum) == bytes_read)
	{
		g_signal_list[signum] (signum, user_data);
	}
	else
	{
		g_critical ("Lost data in signal pipe, read %" G_GSIZE_FORMAT " byte%s expected %" G_GSIZE_FORMAT ".",
				bytes_read, bytes_read > 1 ? "s" : "", sizeof(signum));
	}

	return TRUE;
}

static
const char*
cond_string (
	GIOCondition	cond
	)
{
	const char* c;

	switch (cond) {
	case G_IO_IN:		c = "G_IO_IN"; break;
	case G_IO_OUT:		c = "G_IO_OUT"; break;
	case G_IO_PRI:		c = "G_IO_PRI"; break;
	case G_IO_ERR:		c = "G_IO_ERR"; break;
	case G_IO_HUP:		c = "G_IO_HUP"; break;
	case G_IO_NVAL:		c = "G_IO_NVAL"; break;
	default: c = "(unknown)"; break;
	}

	return c;
}


/* eof */