/*
 *      notebook.c - this file is part of Geany, a fast and lightweight IDE
 *
 *      Copyright 2006-2008 Enrico Tröger <enrico(dot)troeger(at)uvena(dot)de>
 *      Copyright 2006-2008 Nick Treleaven <nick(dot)treleaven(at)btinternet(dot)com>
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * $Id$
 */

/*
 * Notebook tab Drag 'n' Drop reordering and tab management.
 */

#include "geany.h"
#include "notebook.h"
#include "document.h"
#include "editor.h"
#include "documentprivate.h"
#include "ui_utils.h"
#include "treeviews.h"
#include "support.h"
#include "callbacks.h"

#define GEANY_DND_NOTEBOOK_TAB_TYPE	"geany_dnd_notebook_tab"

static const GtkTargetEntry drag_targets[] =
{
	{GEANY_DND_NOTEBOOK_TAB_TYPE, GTK_TARGET_SAME_APP | GTK_TARGET_SAME_WIDGET, 0}
};

static GtkTargetEntry files_drop_targets[] = {
	{ "STRING",			0, 0 },
	{ "UTF8_STRING",	0, 0 },
	{ "text/plain",		0, 0 },
	{ "text/uri-list",	0, 0 }
};


static gboolean
notebook_drag_motion_cb(GtkWidget *widget, GdkDragContext *dc,
	gint x, gint y, guint event_time, gpointer user_data);

static void
notebook_page_reordered_cb(GtkNotebook *notebook, GtkWidget *child, guint page_num,
	gpointer user_data);

#if ! GTK_CHECK_VERSION(2, 8, 0)
static gboolean
notebook_motion_notify_event_cb(GtkWidget *widget, GdkEventMotion *event,
	gpointer user_data);
#endif

static void
on_window_drag_data_received(GtkWidget *widget, GdkDragContext *drag_context,
                             gint x, gint y, GtkSelectionData *data, guint info,
                             guint event_time, gpointer user_data);

static gint
notebook_find_tab_num_at_pos(GtkNotebook *notebook, gint x, gint y);

static void
notebook_tab_close_clicked_cb(GtkButton *button, gpointer user_data);

static void setup_tab_dnd(void);


static gboolean focus_sci(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	GeanyDocument *doc = document_get_current();

	if (doc != NULL)
		gtk_widget_grab_focus(GTK_WIDGET(doc->editor->sci));

	return FALSE;
}


static gboolean gtk_notebook_show_arrows(GtkNotebook *notebook)
{
	return notebook->scrollable;
#if 0
	/* To get this working we would need to define at least the first two fields of
	 * GtkNotebookPage since it is a private field. The better way would be to
	 * subclass GtkNotebook.
struct _FakeGtkNotebookPage
{
	GtkWidget *child;
	GtkWidget *tab_label;
};
 */
	gboolean show_arrow = FALSE;
	GList *children;

	if (! notebook->scrollable)
		return FALSE;

	children = notebook->children;
	while (children)
	{
		struct _FakeGtkNotebookPage *page = children->data;

		if (page->tab_label && ! gtk_widget_get_child_visible(page->tab_label))
			show_arrow = TRUE;

		children = children->next;
	}
	return show_arrow;
#endif
}


static gboolean is_position_on_tab_bar(GtkNotebook *notebook, GdkEventButton *event)
{
	GtkWidget *page;
	GtkWidget *tab;
	GtkWidget *nb;
	GtkPositionType tab_pos;
	gint scroll_arrow_hlength, scroll_arrow_vlength;
	gdouble x, y;

	page = gtk_notebook_get_nth_page(notebook, 0);
	g_return_val_if_fail(page != NULL, FALSE);

	tab = gtk_notebook_get_tab_label(notebook, page);
	g_return_val_if_fail(tab != NULL, FALSE);

	tab_pos = gtk_notebook_get_tab_pos(notebook);
	nb = GTK_WIDGET(notebook);

#if GTK_CHECK_VERSION(2, 10, 0)
	gtk_widget_style_get(GTK_WIDGET(notebook), "scroll-arrow-hlength", &scroll_arrow_hlength,
		"scroll-arrow-vlength", &scroll_arrow_vlength, NULL);
#else
	scroll_arrow_hlength = scroll_arrow_vlength = 16;
#endif

	if (! gdk_event_get_coords((GdkEvent*) event, &x, &y))
	{
		x = event->x;
		y = event->y;
	}

	switch (tab_pos)
	{
		case GTK_POS_TOP:
		case GTK_POS_BOTTOM:
		{
			if (event->y >= 0 && event->y <= tab->allocation.height)
			{
				if (! gtk_notebook_show_arrows(notebook) || (
					x > scroll_arrow_hlength &&
					x < nb->allocation.width - scroll_arrow_hlength))
					return TRUE;
			}
			break;
		}
		case GTK_POS_LEFT:
		case GTK_POS_RIGHT:
		{
			if (event->x >= 0 && event->x <= tab->allocation.width)
			{
				if (! gtk_notebook_show_arrows(notebook) || (
					y > scroll_arrow_vlength &&
					y < nb->allocation.height - scroll_arrow_vlength))
					return TRUE;
			}
		}
	}

	return FALSE;
}


static gboolean notebook_tab_bar_click_cb(GtkWidget *widget, GdkEventButton *event,
										  gpointer user_data)
{
	if (event->type == GDK_2BUTTON_PRESS)
	{
		/* accessing ::event_window is a little hacky but we need to make sure the click
		 * was in the tab bar and not inside the child */
		if (event->window != GTK_NOTEBOOK(main_widgets.notebook)->event_window)
			return FALSE;

		if (is_position_on_tab_bar(GTK_NOTEBOOK(widget), event))
		{
			document_new_file(NULL, NULL, NULL);
			return TRUE;
		}
	}
	return FALSE;
}


void notebook_init()
{
	g_signal_connect_after(main_widgets.notebook, "button-press-event",
		G_CALLBACK(notebook_tab_bar_click_cb), NULL);

	/* focus the current document after clicking on a tab */
	g_signal_connect_after(main_widgets.notebook, "button-release-event",
		G_CALLBACK(focus_sci), NULL);

	g_signal_connect(main_widgets.notebook, "drag-data-received",
		G_CALLBACK(on_window_drag_data_received), NULL);

	setup_tab_dnd();
}


static void setup_tab_dnd()
{
	GtkWidget *notebook = main_widgets.notebook;

	/* Due to a segfault with manual tab DnD setup on GTK 2.10, we must
	*  use the built in gtk_notebook_set_tab_reorderable from GTK 2.10.
	*  This means a binary compiled against < 2.10 but run on >= 2.10
	*  will not have tab DnD support, but this is necessary until
	*  there is a fix for the older tab DnD code or GTK 2.10. */
	if (gtk_check_version(2, 10, 0) == NULL) /* null means version ok */
	{
#if GTK_CHECK_VERSION(2, 10, 0)
		g_signal_connect(notebook, "page-reordered", G_CALLBACK(notebook_page_reordered_cb), NULL);
#endif
		return;
	}

	/* Set up drag movement callback */
	g_signal_connect(notebook, "drag-motion", G_CALLBACK(notebook_drag_motion_cb), NULL);

	/* There is a bug on GTK 2.6 with drag reordering of notebook tabs.
	 * Clicking (not dragging) on a notebook tab, then making a selection in the
	 * Scintilla widget will cause a strange selection bug.
	 * It seems there is a conflict; the drag cursor is shown,
	 * and the selection is blocked; however, when releasing the
	 * mouse button, the selection continues.
	 * Bug is present with gtk+2.6.8, not gtk+2.8.x - ntrel */
#if ! GTK_CHECK_VERSION(2, 8, 0)
	/* handle higher gtk+ runtime than build environment */
	if (gtk_check_version(2, 8, 0) != NULL) /* null means version ok */
	{
		/* workaround GTK+2.6 drag start bug when over sci widget: */
		gtk_widget_add_events(notebook, GDK_POINTER_MOTION_MASK);
		g_signal_connect(notebook, "motion-notify-event",
			G_CALLBACK(notebook_motion_notify_event_cb), NULL);
	}
#endif

	/* set up drag motion for moving notebook pages */
	gtk_drag_dest_set(notebook, GTK_DEST_DEFAULT_MOTION | GTK_DEST_DEFAULT_DROP,
		drag_targets, G_N_ELEMENTS(drag_targets), GDK_ACTION_MOVE);
	/* set drag source, but for GTK+2.6 it's changed in motion-notify-event handler */
	gtk_drag_source_set(notebook, GDK_BUTTON1_MASK,
		drag_targets, G_N_ELEMENTS(drag_targets), GDK_ACTION_MOVE);
}


#if ! GTK_CHECK_VERSION(2, 8, 0)
/* This is used to disable tab DnD when the cursor is over the
 * Scintilla widget, and re-enable tab DnD when over the notebook tabs
 */
static gboolean
notebook_motion_notify_event_cb(GtkWidget *widget, GdkEventMotion *event,
	gpointer user_data)
{
	static gboolean drag_enabled = TRUE; /* stores current state */
	GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(main_widgets.notebook),
			gtk_notebook_get_current_page(GTK_NOTEBOOK(main_widgets.notebook)));

	if (page == NULL || event->x < 0 || event->y < 0) return FALSE;

	if (event->window == page->window) /* cursor over sci widget */
	{
		if (drag_enabled) gtk_drag_source_unset(widget); /* disable */
		drag_enabled = FALSE;
	}
	else /* assume cursor over notebook tab */
	{
		if (! drag_enabled)
			gtk_drag_source_set(widget, GDK_BUTTON1_MASK,
				drag_targets, G_N_ELEMENTS(drag_targets), GDK_ACTION_MOVE);
		drag_enabled = TRUE;
	}
	return FALSE; /* propagate event */
}
#endif


static void
notebook_page_reordered_cb(GtkNotebook *notebook, GtkWidget *child, guint page_num,
	gpointer user_data)
{
	/* Not necessary to update open files treeview if it's sorted.
	 * Note: if enabled, it's best to move the item instead of recreating all items. */
	/*treeviews_openfiles_update_all();*/
}


static gboolean
notebook_drag_motion_cb(GtkWidget *widget, GdkDragContext *dc,
	gint x, gint y, guint event_time, gpointer user_data)
{
	static gint oldx, oldy; /* for determining direction of mouse drag */
	GtkNotebook *notebook = GTK_NOTEBOOK(widget);
	gint ndest = notebook_find_tab_num_at_pos(notebook, x, y);
	gint ncurr = gtk_notebook_get_current_page(notebook);

	if (ndest >= 0 && ndest != ncurr)
	{
		gboolean ok = FALSE;
		/* prevent oscillation between non-homogeneous sized tabs */
		switch(gtk_notebook_get_tab_pos(notebook))
		{
			case GTK_POS_LEFT:
			case GTK_POS_RIGHT:
			ok = ((ndest > ncurr) && (y > oldy)) || ((ndest < ncurr) && (y < oldy));
			break;

			case GTK_POS_TOP:
			case GTK_POS_BOTTOM:
			ok = ((ndest > ncurr) && (x > oldx)) || ((ndest < ncurr) && (x < oldx));
			break;
		}

		if (ok)
		{
			gtk_notebook_reorder_child(notebook,
							gtk_notebook_get_nth_page(notebook, ncurr), ndest);
			notebook_page_reordered_cb(NULL, NULL, ndest, NULL);
		}
	}

	oldx = x; oldy = y;
	return FALSE;
}


/* Adapted from Epiphany absolute version in ephy-notebook.c, thanks.
 * x,y are co-ordinates local to the notebook (not including border padding)
 * notebook tab label widgets must not be NULL.
 * N.B. This only checks the dimension that the tabs are in,
 * e.g. for GTK_POS_TOP it does not check the y coordinate. */
static gint
notebook_find_tab_num_at_pos(GtkNotebook *notebook, gint x, gint y)
{
	GtkPositionType tab_pos;
	int page_num = 0;
	GtkWidget *page;

	/* deal with less than 2 pages */
	switch(gtk_notebook_get_n_pages(notebook))
	{case 0: return -1; case 1: return 0;}

	tab_pos = gtk_notebook_get_tab_pos(notebook); /* which edge */

	while ((page = gtk_notebook_get_nth_page(notebook, page_num)))
	{
		gint max_x, max_y;
		GtkWidget *tab = gtk_notebook_get_tab_label(notebook, page);

		g_return_val_if_fail(tab != NULL, -1);

		if (!GTK_WIDGET_MAPPED(GTK_WIDGET(tab)))
		{ /* skip hidden tabs, e.g. tabs scrolled out of view */
			page_num++;
			continue;
		}

		/* subtract notebook pos to remove possible border padding */
		max_x = tab->allocation.x + tab->allocation.width - GTK_WIDGET(notebook)->allocation.x;
		max_y = tab->allocation.y + tab->allocation.height - GTK_WIDGET(notebook)->allocation.y;

		if (((tab_pos == GTK_POS_TOP) || (tab_pos == GTK_POS_BOTTOM)) && (x<=max_x))
			return page_num;
		else if (((tab_pos == GTK_POS_LEFT) || (tab_pos == GTK_POS_RIGHT)) && (y<=max_y))
			return page_num;

		page_num++;
	}
	return -1;
}


/* call this after the number of tabs in main_widgets.notebook changes. */
static void tab_count_changed(void)
{
	switch (gtk_notebook_get_n_pages(GTK_NOTEBOOK(main_widgets.notebook)))
	{
		case 0:
		/* Enables DnD for dropping files into the empty notebook widget */
		gtk_drag_dest_set(main_widgets.notebook, GTK_DEST_DEFAULT_ALL,
			files_drop_targets,	G_N_ELEMENTS(files_drop_targets),
			GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK | GDK_ACTION_ASK);
		break;

		case 1:
		/* Disables DnD for dropping files into the notebook widget and enables the DnD for moving file
		 * tabs. Files can still be dropped into the notebook widget because it will be handled by the
		 * active Scintilla Widget (only dropping to the tab bar is not possible but it should be ok) */
		gtk_drag_dest_set(main_widgets.notebook, GTK_DEST_DEFAULT_MOTION | GTK_DEST_DEFAULT_DROP,
			drag_targets, G_N_ELEMENTS(drag_targets), GDK_ACTION_MOVE);
		break;
	}
}


static gboolean notebook_tab_label_cb(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
	/* toggle additional widgets on double click */
	if (event->type == GDK_2BUTTON_PRESS)
	{
		on_menu_toggle_all_additional_widgets1_activate(NULL, NULL);
		return TRUE; /* stop other handlers like notebook_tab_bar_click_cb() */
	}
	/* close tab on middle click */
	if (event->button == 2)
	{
		document_remove_page(gtk_notebook_page_num(GTK_NOTEBOOK(main_widgets.notebook),
			GTK_WIDGET(data)));
		return TRUE; /* stop other handlers like notebook_tab_bar_click_cb() */
	}

	return FALSE;
}


/* Returns page number of notebook page, or -1 on error */
gint notebook_new_tab(GeanyDocument *this)
{
	GtkWidget *hbox, *ebox;
	gint tabnum;
	gchar *title;
	GtkWidget *page;

	g_return_val_if_fail(this != NULL, -1);

	page = GTK_WIDGET(this->editor->sci);
	title = g_path_get_basename(DOC_FILENAME(this));

	this->priv->tab_label = gtk_label_new(NULL);

	ebox = gtk_event_box_new();
	GTK_WIDGET_SET_FLAGS(ebox, GTK_NO_WINDOW);
	g_signal_connect(ebox, "button-press-event", G_CALLBACK(notebook_tab_label_cb), page);

	hbox = gtk_hbox_new(FALSE, 2);
	gtk_container_add(GTK_CONTAINER(ebox), this->priv->tab_label);
	gtk_box_pack_start(GTK_BOX(hbox), ebox, FALSE, FALSE, 0);

	if (file_prefs.show_tab_cross)
	{
		GtkWidget *image, *btn, *align;
		GtkRcStyle *rcstyle;

		btn = gtk_button_new();
		gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);

		/* don't allow focus on the close button */
		gtk_button_set_focus_on_click(GTK_BUTTON(btn), FALSE);

		/* make it as small as possible */
		rcstyle = gtk_rc_style_new();
		rcstyle->xthickness = rcstyle->ythickness = 0;
		gtk_widget_modify_style(btn, rcstyle);
		gtk_rc_style_unref(rcstyle);

		image = gtk_image_new_from_stock(GTK_STOCK_CLOSE, GTK_ICON_SIZE_MENU);
		gtk_button_set_image(GTK_BUTTON(btn), image);

		align = gtk_alignment_new(1.0, 0.0, 0.0, 0.0);
		gtk_container_add(GTK_CONTAINER(align), btn);

		gtk_box_pack_start(GTK_BOX(hbox), align, TRUE, TRUE, 0);

		g_signal_connect(btn, "clicked", G_CALLBACK(notebook_tab_close_clicked_cb), page);
	}

	gtk_widget_show_all(hbox);

	this->priv->tabmenu_label = gtk_label_new(NULL);
	gtk_misc_set_alignment(GTK_MISC(this->priv->tabmenu_label), 0.0, 0);

	document_update_tab_label(this);

	if (file_prefs.tab_order_ltr)
		tabnum = gtk_notebook_append_page_menu(GTK_NOTEBOOK(main_widgets.notebook), page,
			hbox, this->priv->tabmenu_label);
	else
		tabnum = gtk_notebook_insert_page_menu(GTK_NOTEBOOK(main_widgets.notebook), page,
			hbox, this->priv->tabmenu_label, 0);

	tab_count_changed();

	/* This is where tab DnD is enabled for GTK 2.10 and higher */
#if GTK_CHECK_VERSION(2, 10, 0)
	if (gtk_check_version(2, 10, 0) == NULL) /* null means version ok */
	{
		gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(main_widgets.notebook), page, TRUE);
	}
#endif
	g_free(title);
	return tabnum;
}


static void
notebook_tab_close_clicked_cb(GtkButton *button, gpointer user_data)
{
	gint cur_page = gtk_notebook_page_num(GTK_NOTEBOOK(main_widgets.notebook),
		GTK_WIDGET(user_data));

	document_remove_page(cur_page);
}


/* Always use this instead of gtk_notebook_remove_page(). */
void notebook_remove_page(gint page_num)
{
	gint curpage = gtk_notebook_get_current_page(GTK_NOTEBOOK(main_widgets.notebook));

	/* Focus the next page, not the previous */
	if (curpage == page_num && file_prefs.tab_order_ltr)
	{
		gtk_notebook_set_current_page(GTK_NOTEBOOK(main_widgets.notebook), curpage + 1);
	}

	/* now remove the page (so we don't temporarily switch to the previous page) */
	gtk_notebook_remove_page(GTK_NOTEBOOK(main_widgets.notebook), page_num);

	tab_count_changed();
}


static void
on_window_drag_data_received(GtkWidget *widget, GdkDragContext *drag_context,
                             gint x, gint y, GtkSelectionData *data, guint target_type,
                             guint event_time, gpointer user_data)
{
	gboolean success = FALSE;

	if (data->length > 0 && data->format == 8)
	{
		if (drag_context->action == GDK_ACTION_ASK)
		{
			drag_context->action = GDK_ACTION_COPY;
		}

		document_open_file_list((const gchar *)data->data, data->length);

		success = TRUE;
	}
	gtk_drag_finish(drag_context, success, FALSE, event_time);
}


