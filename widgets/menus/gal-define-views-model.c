/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 *
 * Author:
 *   Christopher James Lahey <clahey@helixcode.com>
 *
 * (C) 1999 Helix Code, Inc.
 */

#include <config.h>
#include "gal-define-views-model.h"
#include <gnome-xml/tree.h>
#include <gnome-xml/parser.h>
#include <gnome-xml/xmlmemory.h>
#include <gnome.h>

#define PARENT_TYPE e_table_model_get_type()
ETableModelClass *parent_class;

/*
 * GalDefineViewsModel callbacks
 * These are the callbacks that define the behavior of our custom model.
 */
static void gal_define_views_model_set_arg (GtkObject *o, GtkArg *arg, guint arg_id);
static void gal_define_views_model_get_arg (GtkObject *object, GtkArg *arg, guint arg_id);


enum {
	ARG_0,
	ARG_EDITABLE,
};

static void
gdvm_destroy(GtkObject *object)
{
	GalDefineViewsModel *model = GAL_DEFINE_VIEWS_MODEL(object);
	int i;

	for ( i = 0; i < model->data_count; i++ ) {
		gtk_object_unref(GTK_OBJECT(model->data[i]));
	}
	g_free(model->data);
}

/* This function returns the number of columns in our ETableModel. */
static int
gdvm_col_count (ETableModel *etc)
{
	return 1;
}

/* This function returns the number of rows in our ETableModel. */
static int
gdvm_row_count (ETableModel *etc)
{
	GalDefineViewsModel *views = GAL_DEFINE_VIEWS_MODEL(etc);
	return views->data_count;
}

/* This function returns the value at a particular point in our ETableModel. */
static void *
gdvm_value_at (ETableModel *etc, int col, int row)
{
	GalDefineViewsModel *views = GAL_DEFINE_VIEWS_MODEL(etc);
	const char *value;
	if (col != 0 || row < 0 || row > views->data_count)
		return NULL;

	value = views->data[row]->name;

	return (void *)(value ? value : "");
}

/* This function sets the value at a particular point in our ETableModel. */
static void
gdvm_set_value_at (ETableModel *etc, int col, int row, const void *val)
{
	GalDefineViewsModel *views = GAL_DEFINE_VIEWS_MODEL(etc);
	if (views->editable) {
		if (col != 0 || row < 0 || row > views->data_count)
			return;
		gtk_object_set(GTK_OBJECT(views->data[row]),
			       "name", val,
			       NULL);
		e_table_model_cell_changed(etc, col, row);
	}
}

/* This function returns whether a particular cell is editable. */
static gboolean
gdvm_is_cell_editable (ETableModel *etc, int col, int row)
{
	return GAL_DEFINE_VIEWS_MODEL(etc)->editable;
}

static void
gdvm_append_row (ETableModel *etm, ETableModel *source, gint row)
{
	GalDefineViewsModel *views = GAL_DEFINE_VIEWS_MODEL(etm);
	GalView *view;
	const void *val = e_table_model_value_at(source, 0, row);

	e_table_model_pre_change(etm);
	view = gal_view_new();
	gtk_object_set(GTK_OBJECT(view),
		       "name", val,
		       NULL);
	views->data = g_renew(GalView *, views->data, views->data_count + 1);
	views->data[views->data_count] = view;
	views->data_count++;
	e_table_model_row_inserted(etm, views->data_count - 1);
}

/* This function duplicates the value passed to it. */
static void *
gdvm_duplicate_value (ETableModel *etc, int col, const void *value)
{
	return g_strdup(value);
}

/* This function frees the value passed to it. */
static void
gdvm_free_value (ETableModel *etc, int col, void *value)
{
	g_free(value);
}

static void *
gdvm_initialize_value (ETableModel *etc, int col)
{
	return g_strdup("");
}

static gboolean
gdvm_value_is_empty (ETableModel *etc, int col, const void *value)
{
	return !(value && *(char *)value);
}

static char *
gdvm_value_to_string (ETableModel *etc, int col, const void *value)
{
	return g_strdup(value);
}

void
gal_define_views_model_append (GalDefineViewsModel *model,
			       GalView             *view)
{
	ETableModel *etm = E_TABLE_MODEL(model);

	e_table_model_pre_change(etm);
	model->data = g_renew(GalView *, model->data, model->data_count + 1);
	model->data[model->data_count] = view;
	model->data_count++;
	gtk_object_ref(GTK_OBJECT(view));
	e_table_model_row_inserted(etm, model->data_count - 1);
}

static void
gal_define_views_model_class_init (GtkObjectClass *object_class)
{
	ETableModelClass *model_class = (ETableModelClass *) object_class;

	parent_class = gtk_type_class (PARENT_TYPE);

	object_class->destroy = gdvm_destroy;
	object_class->set_arg   = gal_define_views_model_set_arg;
	object_class->get_arg   = gal_define_views_model_get_arg;

	gtk_object_add_arg_type ("GalDefineViewsModel::editable", GTK_TYPE_BOOL,
				 GTK_ARG_READWRITE, ARG_EDITABLE);
	
	model_class->column_count     = gdvm_col_count;
	model_class->row_count        = gdvm_row_count;
	model_class->value_at         = gdvm_value_at;
	model_class->set_value_at     = gdvm_set_value_at;
	model_class->is_cell_editable = gdvm_is_cell_editable;
	model_class->append_row       = gdvm_append_row;
	model_class->duplicate_value  = gdvm_duplicate_value;
	model_class->free_value       = gdvm_free_value;
	model_class->initialize_value = gdvm_initialize_value;
	model_class->value_is_empty   = gdvm_value_is_empty;
	model_class->value_to_string  = gdvm_value_to_string;
}

static void
gal_define_views_model_init (GtkObject *object)
{
	GalDefineViewsModel *model = GAL_DEFINE_VIEWS_MODEL(object);

	model->data           = NULL;
	model->data_count     = 0;
	model->editable       = TRUE;
}

static void
gal_define_views_model_set_arg (GtkObject *o, GtkArg *arg, guint arg_id)
{
	GalDefineViewsModel *model;

	model = GAL_DEFINE_VIEWS_MODEL (o);
	
	switch (arg_id){
	case ARG_EDITABLE:
		model->editable = GTK_VALUE_BOOL (*arg);
		break;
	}
}

static void
gal_define_views_model_get_arg (GtkObject *object, GtkArg *arg, guint arg_id)
{
	GalDefineViewsModel *gal_define_views_model;

	gal_define_views_model = GAL_DEFINE_VIEWS_MODEL (object);

	switch (arg_id) {
	case ARG_EDITABLE:
		GTK_VALUE_BOOL (*arg) = gal_define_views_model->editable;
		break;
	default:
		arg->type = GTK_TYPE_INVALID;
		break;
	}
}

GtkType
gal_define_views_model_get_type (void)
{
	static GtkType type = 0;

	if (!type){
		GtkTypeInfo info = {
			"GalDefineViewsModel",
			sizeof (GalDefineViewsModel),
			sizeof (GalDefineViewsModelClass),
			(GtkClassInitFunc) gal_define_views_model_class_init,
			(GtkObjectInitFunc) gal_define_views_model_init,
			NULL, /* reserved 1 */
			NULL, /* reserved 2 */
			(GtkClassInitFunc) NULL
		};

		type = gtk_type_unique (PARENT_TYPE, &info);
	}

	return type;
}

ETableModel *
gal_define_views_model_new (void)
{
	GalDefineViewsModel *et;

	et = gtk_type_new (gal_define_views_model_get_type ());
	
	return E_TABLE_MODEL(et);
}
