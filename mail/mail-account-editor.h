/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 *  Authors: Jeffrey Stedfast <fejj@helixcode.com>
 *
 *  Copyright 2001 Helix Code, Inc. (www.helixcode.com)
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

#ifndef MAIL_ACCOUNT_EDITOR_H
#define MAIL_ACCOUNT_EDITOR_H

#ifdef __cplusplus
extern "C" {
#pragma }
#endif /* __cplusplus */

#include <gnome.h>
#include <glade/glade.h>
#include <camel.h>
#include "mail-config.h"

#define MAIL_ACCOUNT_EDITOR_TYPE        (mail_account_editor_get_type ())
#define MAIL_ACCOUNT_EDITOR(o)          (GTK_CHECK_CAST ((o), MAIL_ACCOUNT_EDITOR_TYPE, MailAccountEditor))
#define MAIL_ACCOUNT_EDITOR_CLASS(k)    (GTK_CHECK_CLASS_CAST((k), MAIL_ACCOUNT_EDITOR_TYPE, MailAccountEditorClass))
#define IS_MAIL_ACCOUNT_EDITOR(o)       (GTK_CHECK_TYPE ((o), MAIL_ACCOUNT_EDITOR_TYPE))
#define IS_MAIL_ACCOUNT_EDITOR_CLASS(k) (GTK_CHECK_CLASS_TYPE ((k), MAIL_ACCOUNT_EDITOR_TYPE))

struct _MailAccountEditor {
	GnomeDialog parent;
	
	const MailConfigAccount *account;
	
	GladeXML *gui;
	
	GtkEntry *account_name;
	GtkEntry *name;
	GtkEntry *email;
	GtkEntry *reply_to;
	GtkEntry *organization;
	GnomeFileEntry *signature;
	
	GtkEntry *source_type;
	GtkEntry *source_host;
	GtkEntry *source_user;
	GtkEntry *source_passwd;
	GtkCheckBox *save_passwd;
	GtkOptionMenu *source_auth;
	GtkCheckBox *source_ssl;
	
	GtkOptionMenu *transport_type;
	GtkEntry *transport_host;
	GtkOptionMenu *transport_auth;
	GtkCheckBox *transport_ssl;
	
	GtkSpinButton *auto_mail_check;
	GtkCheckBox *keep_on_server;
	
	const CamelProvider *transport;
};

typedef struct _MailAccountEditor MailAccountEditor;

typedef struct {
	GnomeDialogClass parent_class;
	
	/* signals */
	
} MailAccountEditorClass;

GtkType mail_account_editor_get_type (void);

MailAccountEditor *mail_account_editor_new (void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* MAIL_ACCOUNT_EDITOR_H */
