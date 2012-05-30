/**
 * @file merge.c
 * Deals with the widgets of the merged window panes and menu bar
 *
 * @section LICENSE
 * Copyright (C) 2012 David Michael <fedora.dm0@gmail.com>
 *
 * This file is part of Window Merge.
 *
 * Window Merge is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Window Merge is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Window Merge.  If not, see <http://www.gnu.org/licenses/>.
**/

#include "plugin.h"

#include <gtkblist.h>
#include <gtkconv.h>
#include <gtkimhtml.h>

#include <prefs.h>

#include <gdk/gdkkeysyms.h>

#include "window_merge.h"


/**
 * A callback for when the position of a GtkPaned slider changes
 *
 * This function is responsible for storing the width or height of the Buddy
 * List as a preference after the user changes it by dragging the slider.
 *
 * @param[in] gobject    Pointer to the GtkPaned structure that was resized
 * @param[in] pspec      Unused
 * @param[in] data       Pointer to the Buddy List that is a parent of gobject
**/
static void
notify_position_cb(GObject *gobject, U GParamSpec *pspec, gpointer data)
{
  PidginBuddyList *gtkblist;    /*< Buddy List window containing these panes */
  gint max_position;            /*< The "max-position" property of gobject   */
  gint size;                    /*< Current size of the Buddy List pane      */

  gtkblist = data;
  size = gtk_paned_get_position(GTK_PANED(gobject));

  /* If the Buddy List is not the first pane, invert the size preference. */
  if ( gtk_paned_get_child1(GTK_PANED(gobject)) != gtkblist->notebook ) {
    g_object_get(gobject, "max-position", &max_position, NULL);
    size = max_position - size;
  }

  /* Store this size as a user preference (depending on paned orientation). */
  if ( GTK_IS_VPANED(gobject) )
    purple_prefs_set_int(PREF_HEIGHT, size);
  else
    purple_prefs_set_int(PREF_WIDTH, size);
}


/**
 * A callback for when the total size of a GtkPaned changes
 *
 * This should be called after a new GtkPaned finds its parent and calculates
 * its "max-position" property.  It is only intended to be run on this single
 * occassion, so it removes itself on completion.  The call is used to set the
 * initial size of the Buddy List to the user's preference.
 *
 * @param[in] gobject    Pointer to the GtkPaned structure that was resized
 * @param[in] pspec      Unused
 * @param[in] data       Pointer to the Buddy List that is a parent of gobject
**/
static void
notify_max_position_cb(GObject *gobject, U GParamSpec *pspec, gpointer data)
{
  PidginBuddyList *gtkblist;    /*< Buddy List window containing these panes */
  gint max_position;            /*< The "max-position" property of gobject   */
  gint size;                    /*< Desired size of the Buddy List pane      */

  gtkblist = data;

  /* Fetch the user's preferred Buddy List size (depending on orientation). */
  if ( GTK_IS_VPANED(gobject) )
    size = purple_prefs_get_int(PREF_HEIGHT);
  else
    size = purple_prefs_get_int(PREF_WIDTH);

  /* If the Buddy List is not the first pane, invert the size preference. */
  if ( gtk_paned_get_child1(GTK_PANED(gobject)) != gtkblist->notebook ) {
    g_object_get(gobject, "max-position", &max_position, NULL);
    size = max_position - size;
  }

  /* Adjust the panes' slider to set the Buddy List to its desired size. */
  gtk_paned_set_position(GTK_PANED(gobject), size);

  /* Disconnect this callback.  This initial setting was only needed once. */
  g_object_disconnect(gobject, "any_signal",
                      G_CALLBACK(notify_max_position_cb), data, NULL);

  /* Now that system-induced slider changes are done, monitor user changes. */
  g_object_connect(gobject, "signal::notify::position",
                   G_CALLBACK(notify_position_cb), data, NULL);
}


/**
 * Pass a focus (in) event from one widget to another
 *
 * This is only used to trigger the signal handlers on the conversation window
 * for removing message notifications when the Buddy List window is focused.
 *
 * @param[in] widget     Unused
 * @param[in] event      Unused
 * @param[in] data       Pointer to the widget to receive the passed event
 * @return               Whether to stop processing other event handlers
**/
static gboolean
focus_in_event_cb(U GtkWidget *widget, U GdkEventFocus *event, gpointer data)
{
  GtkWidget *other_widget;      /*< Widget pretending to receive an event    */
  GdkWindow *window;            /*< Window of the widget receiving the event */
  GdkEvent *focus;              /*< New focus event for the specified widget */

  focus = gdk_event_new(GDK_FOCUS_CHANGE);
  other_widget = data;
  window = gtk_widget_get_window(other_widget);

  focus->focus_change.window = g_object_ref(window);
  focus->focus_change.send_event = TRUE;
  focus->focus_change.in = TRUE;

  gtk_widget_event(other_widget, focus);

  gdk_event_free(focus);

  return FALSE;
}


/**
 * Create a conversation window and merge it with the given Buddy List window
 *
 * This is the real core of the plugin right here.  It initializes the Buddy
 * List with a conversation window just like the project advertises.  See the
 * function pwm_split_conversation() to reverse this effect.
 *
 * @param[in] gtkblist   The Buddy List that will be able to show conversations
**/
void
pwm_merge_conversation(PidginBuddyList *gtkblist)
{
  PidginWindow *gtkconvwin;     /*< The mutilated conversations for gtkblist */
  GtkBindingSet *binding_set;   /*< The binding set of GtkIMHtml widgets     */
  GtkWidget *blist_menu;        /*< The Buddy List menu bar                  */
  GtkWidget *submenu;           /*< A submenu of a conversation menu item    */
  GList *items;                 /*< List of conversation window menu items   */
  GList *item;                  /*< A menu item in the list (iteration)      */

  /* Sanity check: If the Buddy List is already merged, don't mess with it. */
  if ( pwm_blist_get_convs(gtkblist) != NULL )
    return;

  binding_set = gtk_binding_set_by_class(g_type_class_ref(GTK_TYPE_IMHTML));
  blist_menu = gtk_widget_get_parent(gtkblist->menutray);
  gtkconvwin = pidgin_conv_window_new();

  /* Tie the Buddy List and conversation window instances together. */
  g_object_set_data(G_OBJECT(gtkblist->notebook), "pwm_convs", gtkconvwin);
  g_object_set_data(G_OBJECT(gtkconvwin->notebook), "pwm_blist", gtkblist);

  /* Backup the Buddy List window title for restoring it later. */
  pwm_store(gtkblist, "title",
            g_strdup(gtk_window_get_title(GTK_WINDOW(gtkblist->window))));

  /* Move the conversation notebook into the Buddy List window. */
  pwm_create_paned_layout(gtkblist, purple_prefs_get_string(PREF_SIDE));

  /* Migrate conversation menu items into the Buddy List bar. */
  items = gtk_container_get_children(GTK_CONTAINER(gtkconvwin->menu.menubar));
  gtk_widget_reparent(gtkblist->menutray, gtkconvwin->menu.menubar);
  for ( item = items; item != NULL; item = item->next ) {
    gtk_widget_reparent(GTK_WIDGET(item->data), blist_menu);

    /* Register the submenus' accelerator groups with the Buddy List window. */
    if ( GTK_IS_MENU_ITEM(item->data) ) {
      submenu = gtk_menu_item_get_submenu(GTK_MENU_ITEM(item->data));
      if ( GTK_IS_MENU(submenu) )
        gtk_window_add_accel_group
        (GTK_WINDOW(gtkblist->window),
         gtk_menu_get_accel_group(GTK_MENU(submenu)));
    }

  }
  gtk_widget_reparent(gtkblist->menutray, blist_menu);
  pwm_store(gtkblist, "conv_menus", items);

  /* Display instructions for users, and hide menu items for real convs. */
  pwm_init_dummy_conversation(gtkblist);
  pwm_show_dummy_conversation(gtkblist);
  pwm_set_conv_menus_visible(gtkblist, FALSE);

  /* Pass focus events from Buddy List to conversation window. */
  g_object_connect(G_OBJECT(gtkblist->window), "signal::focus-in-event",
                   G_CALLBACK(focus_in_event_cb), gtkconvwin->window, NULL);

  /* Point the conversation window structure at the Buddy List's window. */
  pwm_store(gtkblist, "conv_window", gtkconvwin->window);
  gtkconvwin->window = gtkblist->window;

  /* Block these "move-cursor" bindings for conversation event handlers. */
  /* XXX: These are skipped in any GtkIMHtml, not just the conversations. */
  /* XXX: Furthermore, there is no event to undo this effect. */
  gtk_binding_entry_skip(binding_set, GDK_Up,           GDK_CONTROL_MASK);
  gtk_binding_entry_skip(binding_set, GDK_Down,         GDK_CONTROL_MASK);
  gtk_binding_entry_skip(binding_set, GDK_Page_Up,      GDK_CONTROL_MASK);
  gtk_binding_entry_skip(binding_set, GDK_Page_Down,    GDK_CONTROL_MASK);
  gtk_binding_entry_skip(binding_set, GDK_KP_Page_Up,   GDK_CONTROL_MASK);
  gtk_binding_entry_skip(binding_set, GDK_KP_Page_Down, GDK_CONTROL_MASK);
}


/**
 * Restore the Buddy List to its former glory by splitting off conversations
 *
 * This effectively will undo everything done by pwm_merge_conversation().  The
 * Buddy List should be returned to its original state, and any conversations
 * should be in a separate window.
 *
 * @param[in] gtkblist   The Buddy List that has had enough of this plugin
**/
void
pwm_split_conversation(PidginBuddyList *gtkblist)
{
  PidginWindow *gtkconvwin;     /*< Conversation window merged into gtkblist */
  GtkWidget *paned;             /*< The panes on the Buddy List window       */
  GtkWidget *submenu;           /*< A submenu of a conversation menu item    */
  GList *items;                 /*< List of conversation window menu items   */
  GList *item;                  /*< A menu item in the list (iteration)      */
  gchar *title;                 /*< Original title of the Buddy List window  */

  gtkconvwin = pwm_blist_get_convs(gtkblist);
  items = pwm_fetch(gtkblist, "conv_menus");
  paned = pwm_fetch(gtkblist, "paned");
  title = pwm_fetch(gtkblist, "title");

  /* End the association between the Buddy List and its conversation window. */
  g_object_steal_data(G_OBJECT(gtkblist->notebook), "pwm_convs");
  g_object_steal_data(G_OBJECT(gtkconvwin->notebook), "pwm_blist");

  /* Point the conversation window's structure back to its original window. */
  gtkconvwin->window = pwm_fetch(gtkblist, "conv_window");
  pwm_clear(gtkblist, "conv_window");

  /* Stop passing focus events from Buddy List to conversation window. */
  g_object_disconnect(G_OBJECT(gtkblist->window), "any_signal",
                      G_CALLBACK(focus_in_event_cb), gtkconvwin->window, NULL);

  /* Return the conversation menu items to their original window's menu bar. */
  for ( item = items; item != NULL; item = item->next ) {

    /* Remove the submenus' accelerator groups from the Buddy List window. */
    if ( GTK_IS_MENU_ITEM(item->data) ) {
      submenu = gtk_menu_item_get_submenu(GTK_MENU_ITEM(item->data));
      if ( GTK_IS_MENU(submenu) )
        gtk_window_remove_accel_group
        (GTK_WINDOW(gtkblist->window),
         gtk_menu_get_accel_group(GTK_MENU(submenu)));
    }

    gtk_widget_reparent(GTK_WIDGET(item->data), gtkconvwin->menu.menubar);
  }
  g_list_free(items);
  pwm_clear(gtkblist, "conv_menus");

  /* Restore the conversation window's notebook. */
  pwm_widget_replace(pwm_fetch(gtkblist, "placeholder"),
                     gtkconvwin->notebook, NULL);
  pwm_clear(gtkblist, "placeholder");

  /* Display the conversation window, and free its instructions tab. */
  pidgin_conv_window_show(gtkconvwin);
  pwm_free_dummy_conversation(gtkblist);

  /* Restore the Buddy List's original structure, and destroy the panes. */
  pwm_widget_replace(paned, gtkblist->notebook, NULL);
  pwm_clear(gtkblist, "paned");

  /* Restore the window title and icons from before conversations set them. */
  gtk_window_set_icon_list(GTK_WINDOW(gtkblist->window), NULL);
  gtk_window_set_title(GTK_WINDOW(gtkblist->window), title);
  g_free(title);
  pwm_clear(gtkblist, "title");
}


/**
 * Construct (or reconstruct when settings change) the window's paned layout
 *
 * It is worth noting that the string preferences only use the first character
 * to determine orientation since they are all unique (and it avoids calling
 * extra string functions).  The full strings are just for readable prefs.xml.
 *
 * @param[in] gtkblist   The Buddy List that needs a new paned window structure
 * @param[in] side       The pref where convs are placed relative to the blist
 *
 * @note This is structured to default to "right" on an invalid pref setting.
**/
void
pwm_create_paned_layout(PidginBuddyList *gtkblist, const char *side)
{
  PidginWindow *gtkconvwin;     /*< Conversation window merged into gtkblist */
  GtkWidget *old_paned;         /*< The existing paned layout, if it exists  */
  GtkWidget *paned;             /*< The new layout panes being created       */
  GtkWidget *placeholder;       /*< Marks the conv notebook's original spot  */
  GValue value = G_VALUE_INIT;  /*< For passing a property value to a widget */

  gtkconvwin = pwm_blist_get_convs(gtkblist);
  old_paned = pwm_fetch(gtkblist, "paned");

  /* Create the requested vertical or horizontal paned layout. */
  if ( side != NULL && (*side == 't' || *side == 'b') )
    paned = gtk_vpaned_new();
  else
    paned = gtk_hpaned_new();
  gtk_widget_show(paned);
  pwm_store(gtkblist, "paned", paned);

  /* When the size of the panes is determined, reset the Buddy List size. */
  g_object_connect(G_OBJECT(paned), "signal::notify::max-position",
                   G_CALLBACK(notify_max_position_cb), gtkblist, NULL);

  /* If the Buddy List is pristine, make the panes and replace its notebook. */
  if ( old_paned == NULL ) {
    placeholder = gtk_label_new(NULL);
    if ( side != NULL && (*side == 't' || *side == 'l') ) {
      pwm_widget_replace(gtkconvwin->notebook, placeholder, paned);
      pwm_widget_replace(gtkblist->notebook, paned, paned);
    } else {
      pwm_widget_replace(gtkblist->notebook, paned, paned);
      pwm_widget_replace(gtkconvwin->notebook, placeholder, paned);
    }
    pwm_store(gtkblist, "placeholder", placeholder);
  }

  /* If existing panes are being replaced, define the new layout and use it. */
  else {
    if ( side != NULL && (*side == 't' || *side == 'l') ) {
      gtk_widget_reparent(gtkconvwin->notebook, paned);
      gtk_widget_reparent(gtkblist->notebook, paned);
    } else {
      gtk_widget_reparent(gtkblist->notebook, paned);
      gtk_widget_reparent(gtkconvwin->notebook, paned);
    }
    pwm_widget_replace(old_paned, paned, NULL);
  }

  /* Make conversations resize with the window so the Buddy List is fixed. */
  g_value_init(&value, G_TYPE_BOOLEAN);
  g_value_set_boolean(&value, TRUE);
  gtk_container_child_set_property(GTK_CONTAINER(paned), gtkconvwin->notebook,
                                   "resize", &value);
  g_value_set_boolean(&value, FALSE);
  gtk_container_child_set_property(GTK_CONTAINER(paned), gtkblist->notebook,
                                   "resize", &value);
}


/**
 * Toggle the visibility of conversation window menu items
 *
 * @param[in] gtkblist   The Buddy List whose menu needs adjusting
 * @param[in] visible    Whether the menu items are being shown or hidden
**/
void
pwm_set_conv_menus_visible(PidginBuddyList *gtkblist, gboolean visible)
{
  GList *items;                 /*< List of conversation window menu items   */
  GList *item;                  /*< A menu item in the list (iteration)      */

  items = pwm_fetch(gtkblist, "conv_menus");

  if ( visible )
    for ( item = items; item != NULL; item = item->next )
      gtk_widget_show(GTK_WIDGET(item->data));

  else
    for ( item = items; item != NULL; item = item->next )
      gtk_widget_hide(GTK_WIDGET(item->data));
}
