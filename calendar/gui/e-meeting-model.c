/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* itip-model.c
 *
 * Copyright (C) 2001  Ximian, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: JP Rosevear
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib.h>
#include <liboaf/liboaf.h>
#include <bonobo/bonobo-control.h>
#include <bonobo/bonobo-widget.h>
#include <bonobo/bonobo-exception.h>
#include <libgnome/gnome-defs.h>
#include <libgnome/gnome-i18n.h>
#include <libgnome/gnome-util.h>
#include <libgnomevfs/gnome-vfs.h>
#include <gal/e-table/e-table-without.h>
#include <gal/e-table/e-cell-text.h>
#include <gal/e-table/e-cell-popup.h>
#include <gal/e-table/e-cell-combo.h>
#include <gal/util/e-unicode-i18n.h>
#include <ebook/e-book.h>
#include <ebook/e-card-types.h>
#include <ebook/e-card-cursor.h>
#include <ebook/e-card.h>
#include <ebook/e-card-simple.h>
#include <ebook/e-destination.h>
#include <cal-util/cal-component.h>
#include <cal-util/cal-util.h>
#include <cal-util/timeutil.h>
#include "Evolution-Addressbook-SelectNames.h"
#include "calendar-config.h"
#include "itip-utils.h"
#include "e-meeting-utils.h"
#include "e-meeting-attendee.h"
#include "e-meeting-model.h"

#define SELECT_NAMES_OAFID "OAFIID:GNOME_Evolution_Addressbook_SelectNames"

struct _EMeetingModelPrivate 
{
	GPtrArray *attendees;
	GList *edit_rows;

	ETableWithout *without;
	GList *tables;
	
	CalClient *client;
	icaltimezone *zone;
	
	EBook *ebook;
	gboolean book_loaded;
	gboolean book_load_wait;

	GPtrArray *refresh_queue;
	GHashTable *refresh_data;
	gint refresh_idle_id;

	/* For invite others dialogs */
        GNOME_Evolution_Addressbook_SelectNames corba_select_names;
};

#define BUF_SIZE 1024

static char *sections[] = {N_("Chair Persons"), 
			   N_("Required Participants"), 
			   N_("Optional Participants"), 
			   N_("Non-Participants"),
			   NULL};
static icalparameter_role roles[] = {ICAL_ROLE_CHAIR,
				     ICAL_ROLE_REQPARTICIPANT,
				     ICAL_ROLE_OPTPARTICIPANT,
				     ICAL_ROLE_NONPARTICIPANT,
				     ICAL_ROLE_NONE};

typedef struct _EMeetingModelQueueData EMeetingModelQueueData;
struct _EMeetingModelQueueData {
	EMeetingModel *im;
	EMeetingAttendee *ia;

	gboolean refreshing;
	
	EMeetingTime start;
	EMeetingTime end;

	char buffer[BUF_SIZE];
	GString *string;
	
	GPtrArray *call_backs;
	GPtrArray *data;
};


static void class_init	(EMeetingModelClass	 *klass);
static void init	(EMeetingModel		 *model);
static void destroy	(GtkObject *obj);

static void refresh_queue_add (EMeetingModel *im, int row,
			       EMeetingTime *start,
			       EMeetingTime *end,
			       EMeetingModelRefreshCallback call_back,
			       gpointer data);
static void refresh_queue_remove (EMeetingModel *im,
				  EMeetingAttendee *ia);
static gboolean refresh_busy_periods (gpointer data);

static void attendee_changed_cb (EMeetingAttendee *ia, gpointer data);
static void select_names_ok_cb (BonoboListener    *listener,
				char              *event_name,
				CORBA_any         *arg,
				CORBA_Environment *ev,
				gpointer           data);

static void table_destroy_state_cb (ETableScrolled *etable, gpointer data);
static void table_destroy_list_cb (ETableScrolled *etable, gpointer data);

static ETableModelClass *parent_class = NULL;

GtkType
e_meeting_model_get_type (void)
{
	static GtkType type = 0;

	if (type == 0)
	{
		static const GtkTypeInfo info =
		{
			"EMeetingModel",
			sizeof (EMeetingModel),
			sizeof (EMeetingModelClass),
			(GtkClassInitFunc) class_init,
			(GtkObjectInitFunc) init,
			/* reserved_1 */ NULL,
			/* reserved_2 */ NULL,
			(GtkClassInitFunc) NULL,
		};

		type = gtk_type_unique (e_table_model_get_type (), &info);
	}

	return type;
}

static void
book_open_cb (EBook *book, EBookStatus status, gpointer data)
{
	EMeetingModel *im = E_MEETING_MODEL (data);
	EMeetingModelPrivate *priv;
	
	priv = im->priv;

	if (status == E_BOOK_STATUS_SUCCESS)
		priv->book_loaded = TRUE;
	else
		g_warning ("Book not loaded");
	
	if (priv->book_load_wait) {
		priv->book_load_wait = FALSE;
		gtk_main_quit ();
	}
}

static void
start_addressbook_server (EMeetingModel *im)
{
	EMeetingModelPrivate *priv;
	gchar *uri, *path;

	priv = im->priv;
	
	priv->ebook = e_book_new ();

	path = g_concat_dir_and_file (g_get_home_dir (),
				      "evolution/local/Contacts/addressbook.db");
	uri = g_strdup_printf ("file://%s", path);
	g_free (path);

	e_book_load_uri (priv->ebook, uri, book_open_cb, im);

	g_free (uri);
}

static EMeetingAttendee *
find_match (EMeetingModel *im, const char *address, int *pos)
{
	EMeetingModelPrivate *priv;
	EMeetingAttendee *ia;
	const gchar *ia_address;
	int i;
	
	priv = im->priv;
	
	if (address == NULL)
		return NULL;
	
	/* Make sure we can add the new delegatee person */
	for (i = 0; i < priv->attendees->len; i++) {
		ia = g_ptr_array_index (priv->attendees, i);

		ia_address = e_meeting_attendee_get_address (ia);
		if (ia_address != NULL && !g_strcasecmp (itip_strip_mailto (ia_address), itip_strip_mailto (address))) {
			if (pos != NULL)
				*pos = i;
			return ia;
		}
	}

	return NULL;
}

static icalparameter_cutype
text_to_type (const char *type)
{
	if (!g_strcasecmp (type, _("Individual")))
		return ICAL_CUTYPE_INDIVIDUAL;
	else if (!g_strcasecmp (type, _("Group")))
		return ICAL_CUTYPE_GROUP;
	else if (!g_strcasecmp (type, _("Resource")))
		return ICAL_CUTYPE_RESOURCE;
	else if (!g_strcasecmp (type, _("Room")))
		return ICAL_CUTYPE_ROOM;
	else
		return ICAL_CUTYPE_NONE;
}

static char *
type_to_text (icalparameter_cutype type)
{
	switch (type) {
	case ICAL_CUTYPE_INDIVIDUAL:
		return _("Individual");
	case ICAL_CUTYPE_GROUP:
		return _("Group");
	case ICAL_CUTYPE_RESOURCE:
		return _("Resource");
	case ICAL_CUTYPE_ROOM:
		return _("Room");
	default:
		return _("Unknown");
	}

	return NULL;

}

static icalparameter_role
text_to_role (const char *role)
{
	if (!g_strcasecmp (role, _("Chair")))
		return ICAL_ROLE_CHAIR;
	else if (!g_strcasecmp (role, _("Required Participant")))
		return ICAL_ROLE_REQPARTICIPANT;
	else if (!g_strcasecmp (role, _("Optional Participant")))
		return ICAL_ROLE_OPTPARTICIPANT;
	else if (!g_strcasecmp (role, _("Non-Participant")))
		return ICAL_ROLE_NONPARTICIPANT;
	else
		return ICAL_ROLE_NONE;
}

static char *
role_to_text (icalparameter_role role) 
{
	switch (role) {
	case ICAL_ROLE_CHAIR:
		return _("Chair");
	case ICAL_ROLE_REQPARTICIPANT:
		return _("Required Participant");
	case ICAL_ROLE_OPTPARTICIPANT:
		return _("Optional Participant");
	case ICAL_ROLE_NONPARTICIPANT:
		return _("Non-Participant");
	default:
		return _("Unknown");
	}

	return NULL;
}

static gboolean
text_to_boolean (const char *role)
{
	if (!g_strcasecmp (role, _("Yes")))
		return TRUE;
	else
		return FALSE;
}

static char *
boolean_to_text (gboolean b) 
{
	if (b)
		return _("Yes");
	else
		return _("No");
}

static icalparameter_partstat
text_to_partstat (const char *partstat)
{
	if (!g_strcasecmp (partstat, _("Needs Action")))
		return ICAL_PARTSTAT_NEEDSACTION;
	else if (!g_strcasecmp (partstat, _("Accepted")))
		return ICAL_PARTSTAT_ACCEPTED;
	else if (!g_strcasecmp (partstat, _("Declined")))
		return ICAL_PARTSTAT_DECLINED;
	else if (!g_strcasecmp (partstat, _("Tentative")))
		return ICAL_PARTSTAT_TENTATIVE;
	else if (!g_strcasecmp (partstat, _("Delegated")))
		return ICAL_PARTSTAT_DELEGATED;
	else if (!g_strcasecmp (partstat, _("Completed")))
		return ICAL_PARTSTAT_COMPLETED;
	else if (!g_strcasecmp (partstat, _("In Process")))
		return ICAL_PARTSTAT_INPROCESS;
	else
		return ICAL_PARTSTAT_NONE;
}

static char *
partstat_to_text (icalparameter_partstat partstat) 
{
	switch (partstat) {
	case ICAL_PARTSTAT_NEEDSACTION:
		return _("Needs Action");
	case ICAL_PARTSTAT_ACCEPTED:
		return _("Accepted");
	case ICAL_PARTSTAT_DECLINED:
		return _("Declined");
	case ICAL_PARTSTAT_TENTATIVE:
		return _("Tentative");
	case ICAL_PARTSTAT_DELEGATED:
		return _("Delegated");
	case ICAL_PARTSTAT_COMPLETED:
		return _("Completed");
	case ICAL_PARTSTAT_INPROCESS:
		return _("In Process");
	case ICAL_PARTSTAT_NONE:
	default:
		return _("Unknown");
	}

	return NULL;
}

static int
column_count (ETableModel *etm)
{
	return E_MEETING_MODEL_COLUMN_COUNT;
}

static int
row_count (ETableModel *etm)
{
	EMeetingModel *im;
	EMeetingModelPrivate *priv;

	im = E_MEETING_MODEL (etm);	
	priv = im->priv;

	return (priv->attendees->len);
}

static void
append_row (ETableModel *etm, ETableModel *source, int row)
{	
	EMeetingModel *im;
	EMeetingModelPrivate *priv;
	EMeetingAttendee *ia;
	char *address;
	
	im = E_MEETING_MODEL (etm);	
	priv = im->priv;
	
	address = (char *) e_table_model_value_at (source, E_MEETING_MODEL_ADDRESS_COL, row);
	if (find_match (im, address, NULL) != NULL) {
		return;
	}
	
	ia = E_MEETING_ATTENDEE (e_meeting_attendee_new ());
	
	e_meeting_attendee_set_address (ia, g_strdup_printf ("MAILTO:%s", address));
	e_meeting_attendee_set_member (ia, g_strdup (e_table_model_value_at (source, E_MEETING_MODEL_MEMBER_COL, row)));
	e_meeting_attendee_set_cutype (ia, text_to_type (e_table_model_value_at (source, E_MEETING_MODEL_TYPE_COL, row)));
	e_meeting_attendee_set_role (ia, text_to_role (e_table_model_value_at (source, E_MEETING_MODEL_ROLE_COL, row)));
	e_meeting_attendee_set_rsvp (ia, text_to_boolean (e_table_model_value_at (source, E_MEETING_MODEL_RSVP_COL, row)));
	e_meeting_attendee_set_delto (ia, g_strdup (e_table_model_value_at (source, E_MEETING_MODEL_DELTO_COL, row)));
	e_meeting_attendee_set_delfrom (ia, g_strdup (e_table_model_value_at (source, E_MEETING_MODEL_DELFROM_COL, row)));
	e_meeting_attendee_set_status (ia, text_to_partstat (e_table_model_value_at (source, E_MEETING_MODEL_STATUS_COL, row)));
	e_meeting_attendee_set_cn (ia, g_strdup (e_table_model_value_at (source, E_MEETING_MODEL_CN_COL, row)));
	e_meeting_attendee_set_language (ia, g_strdup (e_table_model_value_at (source, E_MEETING_MODEL_LANGUAGE_COL, row)));

	e_meeting_model_add_attendee (E_MEETING_MODEL (etm), ia);
}

static void *
value_at (ETableModel *etm, int col, int row)
{
	EMeetingModel *im;
	EMeetingModelPrivate *priv;
	EMeetingAttendee *ia;

	im = E_MEETING_MODEL (etm);	
	priv = im->priv;

	ia = g_ptr_array_index (priv->attendees, row);
	
	switch (col) {
	case E_MEETING_MODEL_ADDRESS_COL:
		return (void *)itip_strip_mailto (e_meeting_attendee_get_address (ia));
	case E_MEETING_MODEL_MEMBER_COL:
		return (void *)e_meeting_attendee_get_member (ia);
	case E_MEETING_MODEL_TYPE_COL:
		return type_to_text (e_meeting_attendee_get_cutype (ia));
	case E_MEETING_MODEL_ROLE_COL:
		return role_to_text (e_meeting_attendee_get_role (ia));
	case E_MEETING_MODEL_RSVP_COL:
		return boolean_to_text (e_meeting_attendee_get_rsvp (ia));
	case E_MEETING_MODEL_DELTO_COL:
		return (void *)itip_strip_mailto (e_meeting_attendee_get_delto (ia));
	case E_MEETING_MODEL_DELFROM_COL:
		return (void *)itip_strip_mailto (e_meeting_attendee_get_delfrom (ia));
	case E_MEETING_MODEL_STATUS_COL:
		return partstat_to_text (e_meeting_attendee_get_status (ia));
	case E_MEETING_MODEL_CN_COL:
		return (void *)e_meeting_attendee_get_cn (ia);
	case E_MEETING_MODEL_LANGUAGE_COL:
		return (void *)e_meeting_attendee_get_language (ia);
	}
	
	return NULL;
}

static void
set_value_at (ETableModel *etm, int col, int row, const void *val)
{
	EMeetingModel *im;
	EMeetingModelPrivate *priv;
	EMeetingAttendee *ia;

	im = E_MEETING_MODEL (etm);	
	priv = im->priv;

	ia = g_ptr_array_index (priv->attendees, row);

	e_table_model_pre_change (etm);
	
	switch (col) {
	case E_MEETING_MODEL_ADDRESS_COL:
		e_meeting_attendee_set_address (ia, g_strdup_printf ("MAILTO:%s", (char *) val));
		break;
	case E_MEETING_MODEL_MEMBER_COL:
		e_meeting_attendee_set_member (ia, g_strdup (val));
		break;
	case E_MEETING_MODEL_TYPE_COL:
		e_meeting_attendee_set_cutype (ia, text_to_type (val));
		break;
	case E_MEETING_MODEL_ROLE_COL:
		e_meeting_attendee_set_role (ia, text_to_role (val));
		break;
	case E_MEETING_MODEL_RSVP_COL:
		e_meeting_attendee_set_rsvp (ia, text_to_boolean (val));
		break;
	case E_MEETING_MODEL_DELTO_COL:
		e_meeting_attendee_set_delto (ia, g_strdup (val));
		break;
	case E_MEETING_MODEL_DELFROM_COL:
		e_meeting_attendee_set_delfrom (ia, g_strdup (val));
		break;
	case E_MEETING_MODEL_STATUS_COL:
		e_meeting_attendee_set_status (ia, text_to_partstat (val));
		break;
	case E_MEETING_MODEL_CN_COL:
		e_meeting_attendee_set_cn (ia, g_strdup (val));
		break;
	case E_MEETING_MODEL_LANGUAGE_COL:
		e_meeting_attendee_set_language (ia, g_strdup (val));
		break;
	}

	e_table_model_cell_changed (etm, col, row);
}

static gboolean
is_cell_editable (ETableModel *etm, int col, int row)
{
	EMeetingModel *im;
	EMeetingModelPrivate *priv;
	GList *l;
	
	im = E_MEETING_MODEL (etm);	
	priv = im->priv;
	
	if (priv->edit_rows != NULL) {
		if (col != E_MEETING_MODEL_STATUS_COL)
			return FALSE;
		
		for (l = priv->edit_rows; l != NULL; l = l->next) {
			if (GPOINTER_TO_INT (l->data) == row)
				return TRUE;
		}

		return FALSE;
	}
	
	switch (col) {
	case E_MEETING_MODEL_DELTO_COL:
	case E_MEETING_MODEL_DELFROM_COL:
		return FALSE;

	default:
	}

	return TRUE;
}

static void *
duplicate_value (ETableModel *etm, int col, const void *val)
{
	return g_strdup (val);
}

static void
free_value (ETableModel *etm, int col, void *val)
{
	g_free (val);
}

static void *
init_value (ETableModel *etm, int col)
{
	switch (col) {
	case E_MEETING_MODEL_ADDRESS_COL:
		return g_strdup ("");
	case E_MEETING_MODEL_MEMBER_COL:
		return g_strdup ("");
	case E_MEETING_MODEL_TYPE_COL:
		return g_strdup (_("Individual"));
	case E_MEETING_MODEL_ROLE_COL:
		return g_strdup (_("Required Participant"));
	case E_MEETING_MODEL_RSVP_COL:
		return g_strdup (_("Yes"));
	case E_MEETING_MODEL_DELTO_COL:
		return g_strdup ("");
	case E_MEETING_MODEL_DELFROM_COL:
		return g_strdup ("");
	case E_MEETING_MODEL_STATUS_COL:
		return g_strdup (_("Needs Action"));
	case E_MEETING_MODEL_CN_COL:
		return g_strdup ("");
	case E_MEETING_MODEL_LANGUAGE_COL:
		return g_strdup ("en");
	}
	
	return g_strdup ("");
}

static gboolean
value_is_empty (ETableModel *etm, int col, const void *val)
{
	
	switch (col) {
	case E_MEETING_MODEL_ADDRESS_COL:
	case E_MEETING_MODEL_MEMBER_COL:
	case E_MEETING_MODEL_DELTO_COL:
	case E_MEETING_MODEL_DELFROM_COL:
	case E_MEETING_MODEL_CN_COL:
		if (val && !g_strcasecmp (val, ""))
			return TRUE;
		else
			return FALSE;
	default:
	}
	
	return TRUE;
}

static char *
value_to_string (ETableModel *etm, int col, const void *val)
{
	return g_strdup (val);
}

static void *
get_key (ETableModel *source, int row, gpointer data) 
{
	EMeetingModel *im;
	EMeetingModelPrivate *priv;
	char *str;
	
	im = E_MEETING_MODEL (source);
	priv = im->priv;

	str = value_at (source, E_MEETING_MODEL_DELTO_COL, row);
	if (str && *str)
		return g_strdup ("delegator");

	return g_strdup ("none");
}

static void *
duplicate_key (const void *key, gpointer data) 
{
	return g_strdup (key);
}

static void
free_gotten_key (void *key, gpointer data)
{
	g_free (key);
}

static void
free_duplicated_key (void *key, gpointer data)
{
	g_free (key);
}

static void
class_init (EMeetingModelClass *klass)
{
	GtkObjectClass *object_class;
	ETableModelClass *etm_class;
	
	object_class = GTK_OBJECT_CLASS (klass);
	etm_class = E_TABLE_MODEL_CLASS (klass);

	parent_class = gtk_type_class (E_TABLE_MODEL_TYPE);

	object_class->destroy = destroy;

	etm_class->column_count = column_count;
	etm_class->row_count = row_count;
	etm_class->value_at = value_at;
	etm_class->set_value_at = set_value_at;
	etm_class->is_cell_editable = is_cell_editable;
	etm_class->append_row = append_row;
	etm_class->duplicate_value = duplicate_value;
	etm_class->free_value = free_value;
	etm_class->initialize_value = init_value;
	etm_class->value_is_empty = value_is_empty;
	etm_class->value_to_string = value_to_string;
}


static void
init (EMeetingModel *im)
{
	EMeetingModelPrivate *priv;

	priv = g_new0 (EMeetingModelPrivate, 1);

	im->priv = priv;

	priv->attendees = g_ptr_array_new ();
	priv->edit_rows = NULL;
	
	priv->without = E_TABLE_WITHOUT (e_table_without_new (E_TABLE_MODEL (im),
							      g_str_hash,
							      g_str_equal,
							      get_key,
							      duplicate_key,
							      free_gotten_key,
							      free_duplicated_key,
							      NULL));
	e_table_without_hide (priv->without, g_strdup ("delegator"));
	priv->tables = NULL;

	priv->client = NULL;
	priv->zone = icaltimezone_get_builtin_timezone (calendar_config_get_timezone ());
	
	priv->ebook = NULL;
	priv->book_loaded = FALSE;
	priv->book_load_wait = FALSE;

	priv->refresh_queue = g_ptr_array_new ();
	priv->refresh_data = g_hash_table_new (g_direct_hash, g_direct_equal);
	priv->refresh_idle_id = -1;
	
	start_addressbook_server (im);
}

static void
destroy (GtkObject *obj)
{
	EMeetingModel *im = E_MEETING_MODEL (obj);
	EMeetingModelPrivate *priv;
	int i;
	
	priv = im->priv;

	for (i = 0; i < priv->attendees->len; i++)
		gtk_object_unref (GTK_OBJECT (g_ptr_array_index (priv->attendees, i)));
	g_ptr_array_free (priv->attendees, TRUE); 
	
	g_list_free (priv->tables);
	
	if (priv->client != NULL)
		gtk_object_unref (GTK_OBJECT (priv->client));

	if (priv->ebook != NULL)
		gtk_object_unref (GTK_OBJECT (priv->ebook));

	if (priv->corba_select_names != CORBA_OBJECT_NIL) {
		CORBA_Environment ev;
		CORBA_exception_init (&ev);
		bonobo_object_release_unref (priv->corba_select_names, &ev);
		CORBA_exception_free (&ev);
	}

 	while (priv->refresh_queue->len > 0)
 		refresh_queue_remove (im, g_ptr_array_index (priv->refresh_queue, 0));
 	g_ptr_array_free (priv->refresh_queue, TRUE);
 	g_hash_table_destroy (priv->refresh_data);
 	
 	if (priv->refresh_idle_id)
 		g_source_remove (priv->refresh_idle_id);
 		
	g_free (priv);
}

GtkObject *
e_meeting_model_new (void)
{
	return gtk_type_new (E_TYPE_MEETING_MODEL);
}


CalClient *
e_meeting_model_get_cal_client (EMeetingModel *im)
{
	EMeetingModelPrivate *priv;
	
	priv = im->priv;

	return priv->client;
}

void
e_meeting_model_set_cal_client (EMeetingModel *im, CalClient *client)
{
	EMeetingModelPrivate *priv;
	
	priv = im->priv;

	if (priv->client != NULL)
		gtk_object_unref (GTK_OBJECT (priv->client));
	
	if (client != NULL)
		gtk_object_ref (GTK_OBJECT (client));
	priv->client = client;
}

icaltimezone *
e_meeting_model_get_zone (EMeetingModel *im)
{
	EMeetingModelPrivate *priv;
	
	g_return_val_if_fail (im != NULL, NULL);
	g_return_val_if_fail (E_IS_MEETING_MODEL (im), NULL);

	priv = im->priv;
	
	return priv->zone;
}

void
e_meeting_model_set_zone (EMeetingModel *im, icaltimezone *zone)
{
	EMeetingModelPrivate *priv;
	
	g_return_if_fail (im != NULL);
	g_return_if_fail (E_IS_MEETING_MODEL (im));

	priv = im->priv;
	
	priv->zone = zone;
}

static ETableScrolled *
build_etable (ETableModel *model, const gchar *spec_file, const gchar *state_file)
{
	GtkWidget *etable;
	ETable *real_table;
	ETableExtras *extras;
	GList *strings;
	ECell *popup_cell, *cell;
	
	extras = e_table_extras_new ();

	/* For type */
	cell = e_cell_text_new (NULL, GTK_JUSTIFY_LEFT);
	popup_cell = e_cell_combo_new ();
	e_cell_popup_set_child (E_CELL_POPUP (popup_cell), cell);
	gtk_object_unref (GTK_OBJECT (cell));
	
	strings = NULL;
	strings = g_list_append (strings, (char*) U_("Individual"));
	strings = g_list_append (strings, (char*) U_("Group"));
	strings = g_list_append (strings, (char*) U_("Resource"));
	strings = g_list_append (strings, (char*) U_("Room"));
	strings = g_list_append (strings, (char*) U_("Unknown"));

	e_cell_combo_set_popdown_strings (E_CELL_COMBO (popup_cell), strings);
	e_table_extras_add_cell (extras, "typeedit", popup_cell);
	
	/* For role */
	cell = e_cell_text_new (NULL, GTK_JUSTIFY_LEFT);
	popup_cell = e_cell_combo_new ();
	e_cell_popup_set_child (E_CELL_POPUP (popup_cell), cell);
	gtk_object_unref (GTK_OBJECT (cell));
	
	strings = NULL;
	strings = g_list_append (strings, (char*) U_("Chair"));
	strings = g_list_append (strings, (char*) U_("Required Participant"));
	strings = g_list_append (strings, (char*) U_("Optional Participant"));
	strings = g_list_append (strings, (char*) U_("Non-Participant"));
	strings = g_list_append (strings, (char*) U_("Unknown"));

	e_cell_combo_set_popdown_strings (E_CELL_COMBO (popup_cell), strings);
	e_table_extras_add_cell (extras, "roleedit", popup_cell);

	/* For rsvp */
	cell = e_cell_text_new (NULL, GTK_JUSTIFY_LEFT);
	popup_cell = e_cell_combo_new ();
	e_cell_popup_set_child (E_CELL_POPUP (popup_cell), cell);
	gtk_object_unref (GTK_OBJECT (cell));

	strings = NULL;
	strings = g_list_append (strings, (char*) U_("Yes"));
	strings = g_list_append (strings, (char*) U_("No"));

	e_cell_combo_set_popdown_strings (E_CELL_COMBO (popup_cell), strings);
	e_table_extras_add_cell (extras, "rsvpedit", popup_cell);

	/* For status */
	cell = e_cell_text_new (NULL, GTK_JUSTIFY_LEFT);
	popup_cell = e_cell_combo_new ();
	e_cell_popup_set_child (E_CELL_POPUP (popup_cell), cell);
	gtk_object_unref (GTK_OBJECT (cell));

	strings = NULL;
	strings = g_list_append (strings, (char*) U_("Needs Action"));
	strings = g_list_append (strings, (char*) U_("Accepted"));
	strings = g_list_append (strings, (char*) U_("Declined"));
	strings = g_list_append (strings, (char*) U_("Tentative"));
	strings = g_list_append (strings, (char*) U_("Delegated"));

	e_cell_combo_set_popdown_strings (E_CELL_COMBO (popup_cell), strings);
	e_table_extras_add_cell (extras, "statusedit", popup_cell);

	etable = e_table_scrolled_new_from_spec_file (model, extras, spec_file, NULL);
	real_table = e_table_scrolled_get_table (E_TABLE_SCROLLED (etable));
	gtk_object_set (GTK_OBJECT (real_table), "uniform_row_height", TRUE, NULL);
	e_table_load_state (real_table, state_file);

#if 0
	gtk_signal_connect (GTK_OBJECT (real_table),
			    "right_click", GTK_SIGNAL_FUNC (right_click_cb), mpage);
#endif

	gtk_signal_connect (GTK_OBJECT (etable), "destroy", 
			    GTK_SIGNAL_FUNC (table_destroy_state_cb), g_strdup (state_file));

	gtk_object_unref (GTK_OBJECT (extras));
	
	return E_TABLE_SCROLLED (etable);
}

void
e_meeting_model_add_attendee (EMeetingModel *im, EMeetingAttendee *ia)
{
	EMeetingModelPrivate *priv;
	
	priv = im->priv;
	
	e_table_model_pre_change (E_TABLE_MODEL (im));

	gtk_object_ref (GTK_OBJECT (ia));
	g_ptr_array_add (priv->attendees, ia);
	
	gtk_signal_connect (GTK_OBJECT (ia), "changed",
			    GTK_SIGNAL_FUNC (attendee_changed_cb), im);

	e_table_model_row_inserted (E_TABLE_MODEL (im), row_count (E_TABLE_MODEL (im)) - 1);
}

EMeetingAttendee *
e_meeting_model_add_attendee_with_defaults (EMeetingModel *im)
{
	EMeetingAttendee *ia;
	char *str;
	
	ia = E_MEETING_ATTENDEE (e_meeting_attendee_new ());

	e_meeting_attendee_set_address (ia, init_value (E_TABLE_MODEL (im), E_MEETING_MODEL_ADDRESS_COL));
	e_meeting_attendee_set_member (ia, init_value (E_TABLE_MODEL (im), E_MEETING_MODEL_MEMBER_COL));

	str = init_value (E_TABLE_MODEL (im), E_MEETING_MODEL_TYPE_COL);
	e_meeting_attendee_set_cutype (ia, text_to_type (str));
	g_free (str);
	str = init_value (E_TABLE_MODEL (im), E_MEETING_MODEL_ROLE_COL);
	e_meeting_attendee_set_role (ia, text_to_role (str));
	g_free (str);	
	str = init_value (E_TABLE_MODEL (im), E_MEETING_MODEL_RSVP_COL);
	e_meeting_attendee_set_role (ia, text_to_boolean (str));
	g_free (str);
	
	e_meeting_attendee_set_delto (ia, init_value (E_TABLE_MODEL (im), E_MEETING_MODEL_DELTO_COL));
	e_meeting_attendee_set_delfrom (ia, init_value (E_TABLE_MODEL (im), E_MEETING_MODEL_DELFROM_COL));

	str = init_value (E_TABLE_MODEL (im), E_MEETING_MODEL_STATUS_COL);
	e_meeting_attendee_set_status (ia, text_to_partstat (str));
	g_free (str);

	e_meeting_attendee_set_cn (ia, init_value (E_TABLE_MODEL (im), E_MEETING_MODEL_CN_COL));
	e_meeting_attendee_set_language (ia, init_value (E_TABLE_MODEL (im), E_MEETING_MODEL_LANGUAGE_COL));

	e_meeting_model_add_attendee (im, ia);

	return ia;
}

void
e_meeting_model_remove_attendee (EMeetingModel *im, EMeetingAttendee *ia)
{
	EMeetingModelPrivate *priv;
	gint i, row = -1;
	
	priv = im->priv;
	
	for (i = 0; i < priv->attendees->len; i++) {
		if (ia == g_ptr_array_index (priv->attendees, i)) {
			row = i;
			break;
		}
	}
	
	if (row != -1) {
		e_table_model_pre_change (E_TABLE_MODEL (im));

		g_ptr_array_remove_index (priv->attendees, row);		
		gtk_object_unref (GTK_OBJECT (ia));

		e_table_model_row_deleted (E_TABLE_MODEL (im), row);
	}
}

void
e_meeting_model_remove_all_attendees (EMeetingModel *im)
{
	EMeetingModelPrivate *priv;
	gint i, len;
	
	priv = im->priv;
	
	e_table_model_pre_change (E_TABLE_MODEL (im));

	len = priv->attendees->len;

	for (i = 0; i < len; i++) {
		EMeetingAttendee *ia = g_ptr_array_index (priv->attendees, i);
		gtk_object_unref (GTK_OBJECT (ia));
	}

	g_ptr_array_set_size (priv->attendees, 0);
	e_table_model_rows_deleted (E_TABLE_MODEL (im), 0, len);
}

EMeetingAttendee *
e_meeting_model_find_attendee (EMeetingModel *im, const gchar *address, gint *row)
{
	EMeetingModelPrivate *priv;
	EMeetingAttendee *ia;
	int i;
	
	priv = im->priv;
	
	if (address == NULL)
		return NULL;
	
	for (i = 0; i < priv->attendees->len; i++) {
		const gchar *ia_address;
		
		ia = g_ptr_array_index (priv->attendees, i);
			
		ia_address = e_meeting_attendee_get_address (ia);
		if (ia_address && !g_strcasecmp (itip_strip_mailto (ia_address), itip_strip_mailto (address))) {
			if (row != NULL)
				*row = i;

			return ia;
		}
	}

	return NULL;
}

EMeetingAttendee *
e_meeting_model_find_attendee_at_row (EMeetingModel *im, gint row)
{
	EMeetingModelPrivate *priv;

	g_return_val_if_fail (im != NULL, NULL);
	g_return_val_if_fail (E_IS_MEETING_MODEL (im), NULL);
	g_return_val_if_fail (row >= 0, NULL);

	priv = im->priv;
	g_return_val_if_fail (row < priv->attendees->len, NULL);

	return g_ptr_array_index (priv->attendees, row);
}

gint 
e_meeting_model_count_actual_attendees (EMeetingModel *im)
{
	EMeetingModelPrivate *priv;
	
	priv = im->priv;

	return e_table_model_row_count (E_TABLE_MODEL (priv->without));	
}

const GPtrArray *
e_meeting_model_get_attendees (EMeetingModel *im)
{
	EMeetingModelPrivate *priv;
	
	priv = im->priv;
	
	return priv->attendees;
}

void
e_meeting_model_restricted_add (EMeetingModel *im, int row)
{
	EMeetingModelPrivate *priv;

	g_return_if_fail (im != NULL);
	g_return_if_fail (E_IS_MEETING_MODEL (im));
	
	priv = im->priv;

	priv->edit_rows = g_list_append (priv->edit_rows, GINT_TO_POINTER (row));
}

void
e_meeting_model_restricted_remove (EMeetingModel *im, int row)
{
	EMeetingModelPrivate *priv;
	
	g_return_if_fail (im != NULL);
	g_return_if_fail (E_IS_MEETING_MODEL (im));
	
	priv = im->priv;

	priv->edit_rows = g_list_remove (priv->edit_rows, GINT_TO_POINTER (row));
}

void
e_meeting_model_restricted_clear (EMeetingModel *im)
{
	EMeetingModelPrivate *priv;

	g_return_if_fail (im != NULL);
	g_return_if_fail (E_IS_MEETING_MODEL (im));
	
	priv = im->priv;
	
	if (priv->edit_rows)
		g_list_free (priv->edit_rows);
	priv->edit_rows = NULL;
}

static icaltimezone *
find_zone (icalproperty *ip, icalcomponent *tz_top_level)
{
	icalparameter *param;
	icalcomponent *sub_comp;
	const char *tzid;
	icalcompiter iter;

	if (tz_top_level == NULL)
		return NULL;
	
	param = icalproperty_get_first_parameter (ip, ICAL_TZID_PARAMETER);
	if (param == NULL)
		return NULL;
	tzid = icalparameter_get_tzid (param);

	iter = icalcomponent_begin_component (tz_top_level, ICAL_VTIMEZONE_COMPONENT);
	while ((sub_comp = icalcompiter_deref (&iter)) != NULL) {
		icalcomponent *clone;
		const char *tz_tzid;
		
		tz_tzid = icalproperty_get_tzid (sub_comp);
		if (!strcmp (tzid, tz_tzid)) {
			icaltimezone *zone;

			zone = icaltimezone_new ();
			clone = icalcomponent_new_clone (sub_comp);
			icaltimezone_set_component (zone, clone);
			
			return zone;
		}
		
		icalcompiter_next (&iter);
	}

	return NULL;
}


static void
refresh_queue_add (EMeetingModel *im, int row,
		   EMeetingTime *start,
		   EMeetingTime *end,
		   EMeetingModelRefreshCallback call_back,
		   gpointer data) 
{
	EMeetingModelPrivate *priv;
	EMeetingAttendee *ia;
	EMeetingModelQueueData *qdata;

	priv = im->priv;
	
	ia = g_ptr_array_index (priv->attendees, row);
	if (ia == NULL)
		return;

	qdata = g_hash_table_lookup (priv->refresh_data, ia);
	if (qdata == NULL) {
		qdata = g_new0 (EMeetingModelQueueData, 1);

		qdata->im = im;
		qdata->ia = ia;
		e_meeting_attendee_clear_busy_periods (ia);
		e_meeting_attendee_set_has_calendar_info (ia, FALSE);

	        qdata->start = *start;
	        qdata->end = *end;
		qdata->string = g_string_new (NULL);
		qdata->call_backs = g_ptr_array_new ();
		qdata->data = g_ptr_array_new ();
		g_ptr_array_add (qdata->call_backs, call_back);
		g_ptr_array_add (qdata->data, data);

		g_hash_table_insert (priv->refresh_data, ia, qdata);
	} else {
		if (e_meeting_time_compare_times (start, &qdata->start) == -1)
			qdata->start = *start;
		if (e_meeting_time_compare_times (end, &qdata->end) == 1)
			qdata->end = *end;
		g_ptr_array_add (qdata->call_backs, call_back);
		g_ptr_array_add (qdata->data, data);
	}

	gtk_object_ref (GTK_OBJECT (ia));
	g_ptr_array_add (priv->refresh_queue, ia);

	if (priv->refresh_idle_id == -1)
		priv->refresh_idle_id = g_idle_add (refresh_busy_periods, im);
}

static void
refresh_queue_remove (EMeetingModel *im, EMeetingAttendee *ia) 
{
	EMeetingModelPrivate *priv;
	EMeetingModelQueueData *qdata;
	
	priv = im->priv;
	
	/* Free the queue data */
	qdata = g_hash_table_lookup (priv->refresh_data, ia);
	g_assert (qdata != NULL);

	g_hash_table_remove (priv->refresh_data, ia);
	g_ptr_array_free (qdata->call_backs, TRUE);
	g_ptr_array_free (qdata->data, TRUE);
	g_free (qdata);

	/* Unref the attendee */
	g_ptr_array_remove (priv->refresh_queue, ia);
	gtk_object_unref (GTK_OBJECT (ia));
}

static void
process_callbacks (EMeetingModelQueueData *qdata) 
{
	int i;

	for (i = 0; i < qdata->call_backs->len; i++) {
		EMeetingModelRefreshCallback call_back;
		gpointer *data;

		call_back = g_ptr_array_index (qdata->call_backs, i);
		data = g_ptr_array_index (qdata->data, i);

		call_back (data);
	}

	refresh_queue_remove (qdata->im, qdata->ia);
}

static void
process_free_busy_comp (EMeetingAttendee *ia,
			icalcomponent *fb_comp,
			icaltimezone *zone,
			icalcomponent *tz_top_level)
{
	icalproperty *ip;
	
	ip = icalcomponent_get_first_property (fb_comp, ICAL_DTSTART_PROPERTY);
	if (ip != NULL) {
		struct icaltimetype dtstart;
		icaltimezone *ds_zone;
		
		dtstart = icalproperty_get_dtstart (ip);
		if (!dtstart.is_utc)
			ds_zone = find_zone (ip, tz_top_level);
		else
			ds_zone = icaltimezone_get_utc_timezone ();
		icaltimezone_convert_time (&dtstart, ds_zone, zone);
		e_meeting_attendee_set_start_busy_range (ia,
							 dtstart.year,
							 dtstart.month,
							 dtstart.day,
							 dtstart.hour,
							 dtstart.minute);
	}
	
	ip = icalcomponent_get_first_property (fb_comp, ICAL_DTEND_PROPERTY);
	if (ip != NULL) {
		struct icaltimetype dtend;
		icaltimezone *de_zone;
		
		dtend = icalproperty_get_dtend (ip);
		if (!dtend.is_utc)
			de_zone = find_zone (ip, tz_top_level);
		else
			de_zone = icaltimezone_get_utc_timezone ();
		icaltimezone_convert_time (&dtend, de_zone, zone);
		e_meeting_attendee_set_end_busy_range (ia,
						       dtend.year,
						       dtend.month,
						       dtend.day,
						       dtend.hour,
						       dtend.minute);
	}
	
	ip = icalcomponent_get_first_property (fb_comp, ICAL_FREEBUSY_PROPERTY);
	while (ip != NULL) {
		icalparameter *param;
		struct icalperiodtype fb;
		EMeetingFreeBusyType busy_type = E_MEETING_FREE_BUSY_LAST;
		icalparameter_fbtype fbtype = ICAL_FBTYPE_BUSY;
			
		fb = icalproperty_get_freebusy (ip);
		param = icalproperty_get_first_parameter (ip, ICAL_FBTYPE_PARAMETER);
		if (param != NULL)
			fbtype =  icalparameter_get_fbtype (param);
			
		switch (fbtype) {
		case ICAL_FBTYPE_BUSY:
			busy_type = E_MEETING_FREE_BUSY_BUSY;
			break;

		case ICAL_FBTYPE_BUSYUNAVAILABLE:
			busy_type = E_MEETING_FREE_BUSY_OUT_OF_OFFICE;
			break;

		case ICAL_FBTYPE_BUSYTENTATIVE:
			busy_type = E_MEETING_FREE_BUSY_TENTATIVE;
			break;

		default:
		}
			
		if (busy_type != E_MEETING_FREE_BUSY_LAST) {
			icaltimezone *utc_zone = icaltimezone_get_utc_timezone ();

			icaltimezone_convert_time (&fb.start, utc_zone, zone);
			icaltimezone_convert_time (&fb.end, utc_zone, zone);
			e_meeting_attendee_add_busy_period (ia,
							    fb.start.year,
							    fb.start.month,
							    fb.start.day,
							    fb.start.hour,
							    fb.start.minute,
							    fb.end.year,
							    fb.end.month,
							    fb.end.day,
							    fb.end.hour,
							    fb.end.minute,
							    busy_type);
		}
		
		ip = icalcomponent_get_next_property (fb_comp, ICAL_FREEBUSY_PROPERTY);
	}
}

static void
process_free_busy (EMeetingModelQueueData *qdata, char *text)
{
 	EMeetingModel *im = qdata->im;
 	EMeetingModelPrivate *priv;
 	EMeetingAttendee *ia = qdata->ia;
	icalcomponent *main_comp;
	icalcomponent_kind kind = ICAL_NO_COMPONENT;

	priv = im->priv;

	main_comp = icalparser_parse_string (text);
 	if (main_comp == NULL) {
 		process_callbacks (qdata);
  		return;
 	}

	kind = icalcomponent_isa (main_comp);
	if (kind == ICAL_VCALENDAR_COMPONENT) {	
		icalcompiter iter;
		icalcomponent *tz_top_level, *sub_comp;

		tz_top_level = cal_util_new_top_level ();
		
		iter = icalcomponent_begin_component (main_comp, ICAL_VTIMEZONE_COMPONENT);
		while ((sub_comp = icalcompiter_deref (&iter)) != NULL) {
			icalcomponent *clone;
			
			clone = icalcomponent_new_clone (sub_comp);
			icalcomponent_add_component (tz_top_level, clone);
			
			icalcompiter_next (&iter);
		}

		iter = icalcomponent_begin_component (main_comp, ICAL_VFREEBUSY_COMPONENT);
		while ((sub_comp = icalcompiter_deref (&iter)) != NULL) {
			process_free_busy_comp (ia, sub_comp, priv->zone, tz_top_level);

			icalcompiter_next (&iter);
		}
		icalcomponent_free (tz_top_level);
	} else if (kind == ICAL_VFREEBUSY_COMPONENT) {
		process_free_busy_comp (ia, main_comp, priv->zone, NULL);
	} else {
		return;
	}
	
	icalcomponent_free (main_comp);

	process_callbacks (qdata);
}

static void
async_close (GnomeVFSAsyncHandle *handle,
	     GnomeVFSResult result,
	     gpointer data)
{
	EMeetingModelQueueData *qdata = data;

	process_free_busy (qdata, qdata->string->str);
}

static void
async_read (GnomeVFSAsyncHandle *handle,
	    GnomeVFSResult result,
	    gpointer buffer,
	    GnomeVFSFileSize requested,
	    GnomeVFSFileSize read,
	    gpointer data)
{
	EMeetingModelQueueData *qdata = data;
	GnomeVFSFileSize buf_size = BUF_SIZE - 1;

	if (result != GNOME_VFS_OK) {
		gnome_vfs_async_close (handle, async_close, qdata);
		return;
	}
	
	((char *)buffer)[read] = '\0';
	qdata->string = g_string_append (qdata->string, buffer);
	
	if (read < requested) {
		gnome_vfs_async_close (handle, async_close, qdata);
		return;
	}

	gnome_vfs_async_read (handle, qdata->buffer, buf_size, async_read, qdata);	
}

static void
async_open (GnomeVFSAsyncHandle *handle,
	    GnomeVFSResult result,
	    gpointer data)
{
	EMeetingModelQueueData *qdata = data;
	GnomeVFSFileSize buf_size = BUF_SIZE - 1;

	if (result != GNOME_VFS_OK) {
		gnome_vfs_async_close (handle, async_close, qdata);
		return;
	}

	gnome_vfs_async_read (handle, qdata->buffer, buf_size, async_read, qdata);
}

static void
cursor_cb (EBook *book, EBookStatus status, ECardCursor *cursor, gpointer data)
{
	EMeetingModelQueueData *qdata = data;
	int length, i;

	if (status != E_BOOK_STATUS_SUCCESS)
		return;

	length = e_card_cursor_get_length (cursor);
	for (i = 0; i < length; i ++) {
		GnomeVFSAsyncHandle *handle;
		ECard *card = e_card_cursor_get_nth (cursor, i);
		const char *addr;
		
		if (card->fburl == NULL)
			continue;

		addr = itip_strip_mailto (e_meeting_attendee_get_address (qdata->ia));
		if (!e_card_email_match_string (card, addr))
			continue;

		/* Read in free/busy data from the url */
		gnome_vfs_async_open (&handle, card->fburl, GNOME_VFS_OPEN_READ, async_open, qdata);
		return;
	}

	process_callbacks (qdata);
}

static gboolean
refresh_busy_periods (gpointer data)
{	
	EMeetingModel *im = E_MEETING_MODEL (data);
	EMeetingModelPrivate *priv;
	EMeetingAttendee *ia = NULL;
	EMeetingModelQueueData *qdata = NULL;
	char *query;
	int i;
	
	priv = im->priv;

	/* Check to see if there are any remaining attendees in the queue */
	for (i = 0; i < priv->refresh_queue->len; i++) {
		ia = g_ptr_array_index (priv->refresh_queue, i);
		g_assert (ia != NULL);

		qdata = g_hash_table_lookup (priv->refresh_data, ia);
		g_assert (qdata != NULL);

		if (!qdata->refreshing)
			break;
	}

	/* The everything in the queue is being refreshed */
	if (i >= priv->refresh_queue->len) {
		priv->refresh_idle_id = -1;
		return FALSE;
	}
	
	/* Indicate we are trying to refresh it */
	qdata->refreshing = TRUE;
	
	/* Check the server for free busy data */	
	if (priv->client) {
		GList *fb_data, *users = NULL;
		struct icaltimetype itt;
		time_t startt, endt;
		const char *user;
		
		itt = icaltime_null_time ();
		itt.year = g_date_year (&qdata->start.date);
		itt.month = g_date_month (&qdata->start.date);
		itt.day = g_date_day (&qdata->start.date);
		itt.hour = qdata->start.hour;
		itt.minute = qdata->start.minute;
		startt = icaltime_as_timet_with_zone (itt, priv->zone);

		itt = icaltime_null_time ();
		itt.year = g_date_year (&qdata->end.date);
		itt.month = g_date_month (&qdata->end.date);
		itt.day = g_date_day (&qdata->end.date);
		itt.hour = qdata->end.hour;
		itt.minute = qdata->end.minute;
		endt = icaltime_as_timet_with_zone (itt, priv->zone);

		user = itip_strip_mailto (e_meeting_attendee_get_address (ia));
		users = g_list_append (users, g_strdup (user));
		fb_data = cal_client_get_free_busy (priv->client, users, startt, endt);

		g_list_foreach (users, (GFunc)g_free, NULL);
		g_list_free (users);

		if (fb_data != NULL) {
			CalComponent *comp = fb_data->data;
			char *comp_str;
				
			comp_str = cal_component_get_as_string (comp);
			process_free_busy (qdata, comp_str);
			g_free (comp_str);
			return TRUE;
		}
	}

	/* Look for fburl's of attendee with no free busy info on server */
	if (!priv->book_loaded) {
		priv->book_load_wait = TRUE;
		gtk_main ();
	}

	if (!e_meeting_attendee_is_set_address (ia)) {
		process_callbacks (qdata);
		return TRUE;
	}
	
	query = g_strdup_printf ("(contains \"email\" \"%s\")", 
				 itip_strip_mailto (e_meeting_attendee_get_address (ia)));
	e_book_get_cursor (priv->ebook, query, cursor_cb, qdata);
	g_free (query);

	return TRUE;
}
		
void
e_meeting_model_refresh_all_busy_periods (EMeetingModel *im,
					  EMeetingTime *start,
					  EMeetingTime *end,
					  EMeetingModelRefreshCallback call_back,
					  gpointer data)
{
	EMeetingModelPrivate *priv;
	int i;
	
	g_return_if_fail (im != NULL);
	g_return_if_fail (E_IS_MEETING_MODEL (im));

	priv = im->priv;
	
	for (i = 0; i < priv->attendees->len; i++)
		refresh_queue_add (im, i, start, end, call_back, data);
}

void 
e_meeting_model_refresh_busy_periods (EMeetingModel *im,
				      int row,
				      EMeetingTime *start,
				      EMeetingTime *end,
				      EMeetingModelRefreshCallback call_back,
				      gpointer data)
{
	EMeetingModelPrivate *priv;
	
	g_return_if_fail (im != NULL);
	g_return_if_fail (E_IS_MEETING_MODEL (im));

	priv = im->priv;

	refresh_queue_add (im, row, start, end, call_back, data);
}

ETableScrolled *
e_meeting_model_etable_from_model (EMeetingModel *im, const gchar *spec_file, const gchar *state_file)
{
	EMeetingModelPrivate *priv;
	ETableScrolled *ets;
	
	g_return_val_if_fail (im != NULL, NULL);
	g_return_val_if_fail (E_IS_MEETING_MODEL (im), NULL);

	priv = im->priv;
	
	ets = build_etable (E_TABLE_MODEL (priv->without), spec_file, state_file);

	priv->tables = g_list_prepend (priv->tables, ets);

	gtk_signal_connect (GTK_OBJECT (ets), "destroy", 
			    GTK_SIGNAL_FUNC (table_destroy_list_cb), im);
	
	return ets;
}

void
e_meeting_model_etable_click_to_add (EMeetingModel *im, gboolean click_to_add)
{
	EMeetingModelPrivate *priv;
	GList *l;
	
	g_return_if_fail (im != NULL);
	g_return_if_fail (E_IS_MEETING_MODEL (im));

	priv = im->priv;

	for (l = priv->tables; l != NULL; l = l->next) {
		ETableScrolled *ets;
		ETable *real_table;
		
		ets = l->data;
		real_table = e_table_scrolled_get_table (ets);

		gtk_object_set (GTK_OBJECT (real_table), "use_click_to_add", click_to_add, NULL);
	}
}

int
e_meeting_model_etable_model_to_view_row (ETable *et, EMeetingModel *im, int model_row)
{
	EMeetingModelPrivate *priv;
	int row;
	
	g_return_val_if_fail (im != NULL, -1);
	g_return_val_if_fail (E_IS_MEETING_MODEL (im), -1);
	
	priv = im->priv;
	
	row = e_table_model_to_view_row (et, model_row);
	if (row == -1)
		return -1;
	
	return e_table_subset_model_to_view_row (E_TABLE_SUBSET (priv->without), row);
}

int
e_meeting_model_etable_view_to_model_row (ETable *et, EMeetingModel *im, int view_row)
{
	EMeetingModelPrivate *priv;
	int row;
	
	g_return_val_if_fail (im != NULL, -1);
	g_return_val_if_fail (E_IS_MEETING_MODEL (im), -1);
	
	priv = im->priv;
	
	row = e_table_view_to_model_row (et, view_row);
	if (row == -1)
		return -1;
	
	return e_table_subset_view_to_model_row (E_TABLE_SUBSET (priv->without), row);
}


static void
add_section (GNOME_Evolution_Addressbook_SelectNames corba_select_names, const char *name)
{
	CORBA_Environment ev;

	CORBA_exception_init (&ev);

	GNOME_Evolution_Addressbook_SelectNames_addSection (corba_select_names,
							    name, 
							    gettext (name),
							    &ev);

	CORBA_exception_free (&ev);
}

static gboolean
get_select_name_dialog (EMeetingModel *im) 
{
	EMeetingModelPrivate *priv;
	CORBA_Environment ev;
	int i;
	
	priv = im->priv;

	if (priv->corba_select_names != CORBA_OBJECT_NIL) {
		Bonobo_Control corba_control;
		GtkWidget *control_widget;
		int i;
		
		CORBA_exception_init (&ev);
		for (i = 0; sections[i] != NULL; i++) {			
			corba_control = GNOME_Evolution_Addressbook_SelectNames_getEntryBySection 
				(priv->corba_select_names, sections[i], &ev);
			if (BONOBO_EX (&ev)) {
				CORBA_exception_free (&ev);
				return FALSE;				
			}
			
			control_widget = bonobo_widget_new_control_from_objref (corba_control, CORBA_OBJECT_NIL);
			
			bonobo_widget_set_property (BONOBO_WIDGET (control_widget), "text", "", NULL);		
		}
		CORBA_exception_free (&ev);

		return TRUE;
	}
	
	CORBA_exception_init (&ev);

	priv->corba_select_names = oaf_activate_from_id (SELECT_NAMES_OAFID, 0, NULL, &ev);

	for (i = 0; sections[i] != NULL; i++)
		add_section (priv->corba_select_names, sections[i]);

	bonobo_event_source_client_add_listener (priv->corba_select_names,
						 select_names_ok_cb,
						 "GNOME/Evolution:ok:dialog",
						 NULL, im);
	
	if (BONOBO_EX (&ev)) {
		CORBA_exception_free (&ev);
		return FALSE;
	}

	CORBA_exception_free (&ev);

	return TRUE;
}

void
e_meeting_model_invite_others_dialog (EMeetingModel *im)
{
	EMeetingModelPrivate *priv;
	CORBA_Environment ev;

	priv = im->priv;
	
	if (!get_select_name_dialog (im))
		return;

	CORBA_exception_init (&ev);

	GNOME_Evolution_Addressbook_SelectNames_activateDialog (
		priv->corba_select_names, _("Required Participants"), &ev);

	CORBA_exception_free (&ev);
}

static void
process_section (EMeetingModel *im, EDestination **destv, icalparameter_role role)
{
	int i;
	
	for (i = 0; destv[i] != NULL; i++) {
		EMeetingAttendee *ia;
		const char *name, *address;
		
		name = e_destination_get_name (destv[i]);		
		address = e_destination_get_email (destv[i]);
		
		if (address == NULL || *address == '\0')
			continue;
		
		if (e_meeting_model_find_attendee (im, address, NULL) == NULL) {
			ia = e_meeting_model_add_attendee_with_defaults (im);

			e_meeting_attendee_set_address (ia, g_strdup_printf ("MAILTO:%s", address));
			e_meeting_attendee_set_role (ia, role);
			e_meeting_attendee_set_cn (ia, g_strdup (name));
		}
	}
}

static void
select_names_ok_cb (BonoboListener    *listener,
		    char              *event_name,
		    CORBA_any         *arg,
		    CORBA_Environment *ev,
		    gpointer           data)
{
	EMeetingModel *im = data;
	EMeetingModelPrivate *priv;
	Bonobo_Control corba_control;
	GtkWidget *control_widget;
	EDestination **destv;
	char *string = NULL;
	int i;
	
	priv = im->priv;

	for (i = 0; sections[i] != NULL; i++) {
		corba_control = GNOME_Evolution_Addressbook_SelectNames_getEntryBySection 
			(priv->corba_select_names, sections[i], ev);
		control_widget = bonobo_widget_new_control_from_objref
			(corba_control, CORBA_OBJECT_NIL);
		
		bonobo_widget_get_property (BONOBO_WIDGET (control_widget), "destinations", &string, NULL);
		destv = e_destination_importv (string);
		if (destv != NULL) {
			process_section (im, destv, roles[i]);
			e_destination_freev (destv);
		}		
	}
}

static void
attendee_changed_cb (EMeetingAttendee *ia, gpointer data) 
{
	EMeetingModel *im = E_MEETING_MODEL (data);
	EMeetingModelPrivate *priv;
	gint row = -1, i;
	
	priv = im->priv;

	for (i = 0; i < priv->attendees->len; i++) {
		if (ia == g_ptr_array_index (priv->attendees, i)) {
			row = 1;
			break;
		}
	}
	
	if (row == -1)
		return;
	
	/* FIXME: Ideally I think you are supposed to call pre_change() before
	   the data structures are changed. */
	e_table_model_pre_change (E_TABLE_MODEL (im));
	e_table_model_row_changed (E_TABLE_MODEL (im), row);
}

static void
table_destroy_state_cb (ETableScrolled *etable, gpointer data)
{
	ETable *real_table;
	char *filename = data;
	
	real_table = e_table_scrolled_get_table (etable);
	e_table_save_state (real_table, filename);

	g_free (data);
}

static void
table_destroy_list_cb (ETableScrolled *etable, gpointer data)
{
	EMeetingModel *im = E_MEETING_MODEL (data);
	EMeetingModelPrivate *priv;

	priv = im->priv;
	
	priv->tables = g_list_remove (priv->tables, etable);
}

