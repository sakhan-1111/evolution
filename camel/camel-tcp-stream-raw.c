/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 *  Authors: Jeffrey Stedfast <fejj@ximian.com>
 *
 *  Copyright 2001 Ximian, Inc. (www.ximian.com)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Street #330, Boston, MA 02111-1307, USA.
 *
 */


#include <config.h>
#include "camel-tcp-stream-raw.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

static CamelTcpStreamClass *parent_class = NULL;

/* Returns the class for a CamelTcpStreamRaw */
#define CTSR_CLASS(so) CAMEL_TCP_STREAM_RAW_CLASS (CAMEL_OBJECT_GET_CLASS (so))

static ssize_t stream_read (CamelStream *stream, char *buffer, size_t n);
static ssize_t stream_write (CamelStream *stream, const char *buffer, size_t n);
static int stream_flush  (CamelStream *stream);
static int stream_close  (CamelStream *stream);

static int stream_connect (CamelTcpStream *stream, struct hostent *host, int port);
static int stream_disconnect (CamelTcpStream *stream);

static void
camel_tcp_stream_raw_class_init (CamelTcpStreamRawClass *camel_tcp_stream_raw_class)
{
	CamelTcpStreamClass *camel_tcp_stream_class =
		CAMEL_TCP_STREAM_CLASS (camel_tcp_stream_raw_class);
	CamelStreamClass *camel_stream_class =
		CAMEL_STREAM_CLASS (camel_tcp_stream_raw_class);
	
	parent_class = CAMEL_STREAM_CLASS (camel_type_get_global_classfuncs (camel_tcp_stream_get_type ()));
	
	/* virtual method overload */
	camel_stream_class->read = stream_read;
	camel_stream_class->write = stream_write;
	camel_stream_class->flush = stream_flush;
	camel_stream_class->close = stream_close;
	
	camel_tcp_stream_class->connect = stream_connect;
	camel_tcp_stream_class->disconnect = stream_disconnect;
}

static void
camel_tcp_stream_raw_init (gpointer object, gpointer klass)
{
	CamelTcpStreamRaw *stream = CAMEL_TCP_STREAM_RAW (object);
	
	stream->sockfd = -1;
}

static void
camel_tcp_stream_raw_finalize (CamelObject *object)
{
	CamelTcpStreamRaw *stream = CAMEL_TCP_STREAM_RAW (object);
	
	if (stream->sockfd != -1)
		close (stream->sockfd);
}


CamelType
camel_tcp_stream_raw_get_type (void)
{
	static CamelType type = CAMEL_INVALID_TYPE;
	
	if (type == CAMEL_INVALID_TYPE) {
		type = camel_type_register (camel_stream_get_type (),
					    "CamelTcpStreamRaw",
					    sizeof (CamelTcpStreamRaw),
					    sizeof (CamelTcpStreamRawClass),
					    (CamelObjectClassInitFunc) camel_tcp_stream_raw_class_init,
					    NULL,
					    (CamelObjectInitFunc) camel_tcp_stream_raw_init,
					    (CamelObjectFinalizeFunc) camel_tcp_stream_raw_finalize);
	}
	
	return type;
}


/**
 * camel_tcp_stream_raw_new:
 *
 * Return value: a tcp stream
 **/
CamelStream *
camel_tcp_stream_raw_new ()
{
	CamelTcpStreamRaw *stream;
	
	stream = CAMEL_TCP_STREAM_RAW (camel_object_new (camel_tcp_stream_raw_get_type ()));
	
	return CAMEL_STREAM (stream);
}

static ssize_t
stream_read (CamelStream *stream, char *buffer, size_t n)
{
	CamelTcpStreamRaw *tcp_stream_raw = CAMEL_TCP_STREAM_RAW (stream);
	ssize_t nread;
	
	do {
		nread = read (tcp_stream_raw->sockfd, buffer, n);
	} while (nread == -1 && errno == EINTR);
	
	return nread;
}

static ssize_t
stream_write (CamelStream *stream, const char *buffer, size_t n)
{
	CamelTcpStreamRaw *tcp_stream_raw = CAMEL_TCP_STREAM_RAW (stream);
	ssize_t v, written = 0;
	
	do {
		v = write (tcp_stream_raw->sockfd, buffer, n);
		if (v > 0)
			written += v;
	} while (v == -1 && errno == EINTR);
	
	if (v == -1)
		return -1;
	else
		return written;
}

static int
stream_flush (CamelStream *stream)
{
	return fsync (((CamelTcpStreamRaw *)stream)->sockfd);
}

static int
stream_close (CamelStream *stream)
{
	g_warning ("CamelTcpStreamRaw::close: Better to call ::disconnect.\n");
	return close (((CamelTcpStreamRaw *)stream)->sockfd);
}


static int
stream_connect (CamelTcpStream *stream, struct hostent *host, int port)
{
	CamelTcpStreamRaw *raw = CAMEL_TCP_STREAM_RAW (stream);
	struct sockaddr_in sin;
	int fd;
	
	g_return_val_if_fail (host != NULL, -1);
	
	sin.sin_family = host->h_addrtype;
	sin.sin_port = htons (port);
	
	memcpy (&sin.sin_addr, host->h_addr, sizeof (sin.sin_addr));
	
	fd = socket (host->h_addrtype, SOCK_STREAM, 0);
	
	if (fd == -1 || connect (fd, (struct sockaddr *)&sin, sizeof (sin)) == -1) {
		if (fd > -1)
			close (fd);
		
		return -1;
	}
	
	raw->sockfd = fd;
	
	return 0;
}

static int
stream_disconnect (CamelTcpStream *stream)
{
	return close (((CamelTcpStreamRaw *)stream)->sockfd);
}
