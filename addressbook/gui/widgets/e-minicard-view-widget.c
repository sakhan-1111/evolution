/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* 
 * e-minicard-view-widget.c
 * Copyright (C) 2000  Ximian, Inc.
 * Author: Chris Lahey <clahey@ximian.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <config.h>

#include <gtk/gtksignal.h>
#include <gal/widgets/e-canvas-background.h>
#include <gal/widgets/e-canvas.h>

#include "e-minicard-view-widget.h"

static void e_minicard_view_widget_init		 (EMinicardViewWidget		 *widget);
static void e_minicard_view_widget_class_init	 (EMinicardViewWidgetClass	 *klass);
static void e_minicard_view_widget_set_arg       (GtkObject *o, GtkArg *arg, guint arg_id);
static void e_minicard_view_widget_get_arg       (GtkObject *object, GtkArg *arg, guint arg_id);
static void e_minicard_view_widget_destroy       (GtkObject *object);
static void e_minicard_view_widget_reflow        (ECanvas *canvas);
static void e_minicard_view_widget_size_allocate (GtkWidget *widget, GtkAllocation *allocation);
static void e_minicard_view_widget_realize       (GtkWidget *widget);

static ECanvasClass *parent_class = NULL;

/* The arguments we take */
enum {
	ARG_0,
	ARG_BOOK,
	ARG_QUERY,
	ARG_EDITABLE,
	ARG_COLUMN_WIDTH
};

enum {
	SELECTION_CHANGE,
	COLUMN_WIDTH_CHANGED,
	RIGHT_CLICK,
	LAST_SIGNAL
};

static guint signals [LAST_SIGNAL] = {0, };

GtkType
e_minicard_view_widget_get_type (void)
{
  static GtkType type = 0;

  if (!type)
    {
      static const GtkTypeInfo info =
      {
        "EMinicardViewWidget",
        sizeof (EMinicardViewWidget),
        sizeof (EMinicardViewWidgetClass),
        (GtkClassInitFunc) e_minicard_view_widget_class_init,
        (GtkObjectInitFunc) e_minicard_view_widget_init,
        /* reserved_1 */ NULL,
        /* reserved_2 */ NULL,
        (GtkClassInitFunc) NULL,
      };

      type = gtk_type_unique (e_canvas_get_type (), &info);
    }

  return type;
}

static void
e_minicard_view_widget_class_init (EMinicardViewWidgetClass *klass)
{
	GtkObjectClass *object_class;
	GtkWidgetClass *widget_class;
	ECanvasClass *canvas_class;

	object_class = (GtkObjectClass*) klass;
	widget_class = GTK_WIDGET_CLASS (klass);
	canvas_class = E_CANVAS_CLASS (klass);

	parent_class = gtk_type_class (e_canvas_get_type ());

	gtk_object_add_arg_type ("EMinicardViewWidget::book", GTK_TYPE_OBJECT, 
				 GTK_ARG_READWRITE, ARG_BOOK);
	gtk_object_add_arg_type ("EMinicardViewWidget::query", GTK_TYPE_STRING,
				 GTK_ARG_READWRITE, ARG_QUERY);
	gtk_object_add_arg_type ("EMinicardViewWidget::editable", GTK_TYPE_BOOL,
				 GTK_ARG_READWRITE, ARG_EDITABLE);
	gtk_object_add_arg_type ("EMinicardViewWidget::column_width", GTK_TYPE_INT,
				 GTK_ARG_READWRITE, ARG_COLUMN_WIDTH);

	signals [SELECTION_CHANGE] =
		gtk_signal_new ("selection_change",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (EMinicardViewWidgetClass, selection_change),
				gtk_marshal_NONE__NONE,
				GTK_TYPE_NONE, 0);

	signals [COLUMN_WIDTH_CHANGED] =
		gtk_signal_new ("column_width_changed",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (EMinicardViewWidgetClass, column_width_changed),
				e_marshal_NONE__DOUBLE,
				GTK_TYPE_NONE, 1, GTK_TYPE_DOUBLE);

	signals [RIGHT_CLICK] =
		gtk_signal_new ("right_click",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (EMinicardViewWidgetClass, right_click),
				gtk_marshal_INT__POINTER,
				GTK_TYPE_INT, 1, GTK_TYPE_GDK_EVENT);

	gtk_object_class_add_signals (object_class, signals, LAST_SIGNAL);

	object_class->set_arg       = e_minicard_view_widget_set_arg;
	object_class->get_arg       = e_minicard_view_widget_get_arg;
	object_class->destroy       = e_minicard_view_widget_destroy;

	widget_class->realize       = e_minicard_view_widget_realize;
	widget_class->size_allocate = e_minicard_view_widget_size_allocate;

	canvas_class->reflow        = e_minicard_view_widget_reflow;

	klass->selection_change     = NULL;
	klass->column_width_changed = NULL;
	klass->right_click          = NULL;
}

static void
e_minicard_view_widget_init (EMinicardViewWidget *view)
{
	view->emv = NULL;

	view->book = NULL;
	view->query = NULL;
	view->editable = FALSE;
	view->column_width = 150;
}

GtkWidget *
e_minicard_view_widget_new (EAddressbookReflowAdapter *adapter)
{
	EMinicardViewWidget *widget = E_MINICARD_VIEW_WIDGET (gtk_type_new (e_minicard_view_widget_get_type ()));

	widget->adapter = adapter;
	gtk_object_ref (GTK_OBJECT (widget->adapter));

	return GTK_WIDGET (widget);
}

static void
e_minicard_view_widget_set_arg (GtkObject *o, GtkArg *arg, guint arg_id)
{
	EMinicardViewWidget *emvw;

	emvw = E_MINICARD_VIEW_WIDGET (o);

	switch (arg_id){
	case ARG_BOOK:
		if (emvw->book)
			gtk_object_unref(GTK_OBJECT(emvw->book));
		if (GTK_VALUE_OBJECT (*arg)) {
			emvw->book = E_BOOK(GTK_VALUE_OBJECT (*arg));
			if (emvw->book)
				gtk_object_ref(GTK_OBJECT(emvw->book));
		} else
			emvw->book = NULL;
		if (emvw->emv)
			gtk_object_set(GTK_OBJECT(emvw->emv),
				       "book", emvw->book,
				       NULL);
		break;
	case ARG_QUERY:
		emvw->query = g_strdup(GTK_VALUE_STRING (*arg));
		if (emvw->emv)
			gtk_object_set(GTK_OBJECT(emvw->emv),
				       "query", emvw->query,
				       NULL);
		break;
	case ARG_EDITABLE:
		emvw->editable = GTK_VALUE_BOOL(*arg);
		if (emvw->emv)
			gtk_object_set (GTK_OBJECT(emvw->emv),
					"editable", emvw->editable,
					NULL);
		break;
	case ARG_COLUMN_WIDTH:
		emvw->column_width = GTK_VALUE_INT (*arg);
		if (emvw->emv) {
			gtk_object_set (GTK_OBJECT(emvw->emv),
					"column_width", (int) emvw->column_width,
					NULL);
		}
		break;
	}
}

static void
e_minicard_view_widget_get_arg (GtkObject *object, GtkArg *arg, guint arg_id)
{
	EMinicardViewWidget *emvw;

	emvw = E_MINICARD_VIEW_WIDGET (object);

	switch (arg_id) {
	case ARG_BOOK:
		GTK_VALUE_OBJECT (*arg) = GTK_OBJECT(emvw->book);
		break;
	case ARG_QUERY:
		GTK_VALUE_STRING (*arg) = g_strdup(emvw->query);
		break;
	case ARG_EDITABLE:
		GTK_VALUE_BOOL (*arg) = emvw->editable;
		break;
	case ARG_COLUMN_WIDTH:
		GTK_VALUE_INT (*arg) = emvw->column_width;
		break;
	default:
		arg->type = GTK_TYPE_INVALID;
		break;
	}
}

static void
e_minicard_view_widget_destroy (GtkObject *object)
{
	EMinicardViewWidget *view = E_MINICARD_VIEW_WIDGET(object);

	if (view->book)
		gtk_object_unref(GTK_OBJECT(view->book));
	g_free(view->query);

	gtk_object_unref (GTK_OBJECT (view->adapter));

	GTK_OBJECT_CLASS(parent_class)->destroy (object);
}

static void
selection_change (ESelectionModel *esm, EMinicardViewWidget *widget)
{
	gtk_signal_emit (GTK_OBJECT(widget),
			 signals [SELECTION_CHANGE]);
}

static void
column_width_changed (ESelectionModel *esm, double width, EMinicardViewWidget *widget)
{
	gtk_signal_emit (GTK_OBJECT(widget),
			 signals [COLUMN_WIDTH_CHANGED], width);
}

static guint
right_click (EMinicardView *view, GdkEvent *event, EMinicardViewWidget *widget)
{
	guint ret_val;
	gtk_signal_emit (GTK_OBJECT(widget),
			 signals [RIGHT_CLICK],
			 event, &ret_val);
	return ret_val;
}

static void
e_minicard_view_widget_realize (GtkWidget *widget)
{
	EMinicardViewWidget *view = E_MINICARD_VIEW_WIDGET(widget);

	gnome_canvas_item_new(gnome_canvas_root( GNOME_CANVAS(view) ),
			      e_canvas_background_get_type(),
			      "fill_color", "white",
			      NULL );

	view->emv = gnome_canvas_item_new(
		gnome_canvas_root( GNOME_CANVAS(view) ),
		e_minicard_view_get_type(),
		"height", (double) 100,
		"minimum_width", (double) 100,
		"adapter", view->adapter,
		"column_width", (int) view->column_width,
		NULL );

	gtk_signal_connect (GTK_OBJECT (E_REFLOW(view->emv)->selection),
			    "selection_changed",
			    selection_change, view);
	gtk_signal_connect (GTK_OBJECT (view->emv),
			    "column_width_changed",
			    column_width_changed, view);
	gtk_signal_connect (GTK_OBJECT (view->emv),
			    "right_click",
			    GTK_SIGNAL_FUNC (right_click), view);

	if (GTK_WIDGET_CLASS(parent_class)->realize)
		GTK_WIDGET_CLASS(parent_class)->realize (widget);
}

static void
e_minicard_view_widget_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
	if (GTK_WIDGET_CLASS(parent_class)->size_allocate)
		GTK_WIDGET_CLASS(parent_class)->size_allocate (widget, allocation);
	
	if (GTK_WIDGET_REALIZED(widget)) {
		double width;
		EMinicardViewWidget *view = E_MINICARD_VIEW_WIDGET(widget);

		gnome_canvas_item_set( view->emv,
				       "height", (double) allocation->height,
				       NULL );
		gnome_canvas_item_set( view->emv,
				       "minimum_width", (double) allocation->width,
				       NULL );
		gtk_object_get(GTK_OBJECT(view->emv),
			       "width", &width,
			       NULL);
		width = MAX(width, allocation->width);
		gnome_canvas_set_scroll_region (GNOME_CANVAS (view), 0, 0, width - 1, allocation->height - 1);
	}
}

static void
e_minicard_view_widget_reflow(ECanvas *canvas)
{
	double width;
	EMinicardViewWidget *view = E_MINICARD_VIEW_WIDGET(canvas);

	if (E_CANVAS_CLASS(parent_class)->reflow)
		E_CANVAS_CLASS(parent_class)->reflow (canvas);

	gtk_object_get(GTK_OBJECT(view->emv),
		       "width", &width,
		       NULL);
	width = MAX(width, GTK_WIDGET(canvas)->allocation.width);
	gnome_canvas_set_scroll_region(GNOME_CANVAS(canvas), 0, 0, width - 1, GTK_WIDGET(canvas)->allocation.height - 1);
}

ESelectionModel *
e_minicard_view_widget_get_selection_model (EMinicardViewWidget *view)
{
	if (view->emv)
		return E_SELECTION_MODEL (E_REFLOW (view->emv)->selection);
	else
		return NULL;
}

EMinicardView *
e_minicard_view_widget_get_view             (EMinicardViewWidget       *view)
{
	if (view->emv)
		return E_MINICARD_VIEW (view->emv);
	else
		return NULL;
}
