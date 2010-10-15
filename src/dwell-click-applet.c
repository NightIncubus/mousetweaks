/*
 * Copyright © 2007-2010 Gerd Kohlberger <gerdko gmail com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <panel-applet.h>

#include "mt-common.h"

#define WID(n) (GTK_WIDGET (gtk_builder_get_object (dd->ui, n)))

typedef struct _DwellData DwellData;
struct _DwellData
{
    GSettings   *mt_settings;
    GSettings   *gsd_settings;
    GDBusProxy  *proxy;
    GtkBuilder  *ui;
    GtkWidget   *box;
    GtkWidget   *ct_box;
    GtkWidget   *enable;
    GtkWidget   *button;
    GdkPixbuf   *click[4];
    gint         button_width;
    gint         button_height;
    MtClickType  cct;
    gboolean     active;
    GTimer      *timer;
    guint        tid;
    gdouble      delay;
    gdouble      elapsed;
};

static const gchar *img_widgets[] =
{
    "right_click_img",
    "drag_click_img",
    "double_click_img",
    "single_click_img"
};

static const gchar *img_widgets_v[] =
{
    "right_click_img_v",
    "drag_click_img_v",
    "double_click_img_v",
    "single_click_img_v"
};

static void preferences_dialog (BonoboUIComponent *component,
                                gpointer           data,
                                const char        *cname);
static void help_dialog        (BonoboUIComponent *component,
                                DwellData         *dd,
                                const char        *cname);
static void about_dialog       (BonoboUIComponent *component,
                                DwellData         *dd,
                                const char        *cname);

static const BonoboUIVerb menu_verb[] =
{
    BONOBO_UI_UNSAFE_VERB ("PropertiesVerb", preferences_dialog),
    BONOBO_UI_UNSAFE_VERB ("HelpVerb", help_dialog),
    BONOBO_UI_UNSAFE_VERB ("AboutVerb", about_dialog),
    BONOBO_UI_VERB_END
};

static void button_cb (GtkToggleButton *button, DwellData *dd);

static void
update_sensitivity (DwellData *dd)
{
    gboolean dwell, sensitive;
    gint mode;

    dwell = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (dd->enable));
    mode = g_settings_get_int (dd->mt_settings, KEY_DWELL_MODE);
    sensitive = dd->active && dwell && mode == DWELL_MODE_CTW;
    gtk_widget_set_sensitive (dd->ct_box, sensitive);
}

static gboolean
mt_proxy_has_owner (GDBusProxy *proxy)
{
    gchar *owner;

    owner = g_dbus_proxy_get_name_owner (proxy);
    if (owner)
    {
        g_free (owner);
        return TRUE;
    }
    return FALSE;
}

static void
mt_proxy_owner_notify (GObject    *object,
                       GParamSpec *pspec,
                       DwellData  *dd)
{
    dd->active = mt_proxy_has_owner (G_DBUS_PROXY (object));
    update_sensitivity (dd);
}

static void
handle_click_type_changed (DwellData  *dd,
                           MtClickType click_type)
{
    GtkToggleButton *button;
    GSList *group;

    if (click_type != dd->cct)
    {
        dd->cct = click_type;
        group = gtk_radio_button_get_group (GTK_RADIO_BUTTON (dd->button));
        button = GTK_TOGGLE_BUTTON (g_slist_nth_data (group, dd->cct));

        g_signal_handlers_block_by_func (button, button_cb, dd);
        gtk_toggle_button_set_active (button, TRUE);
        g_signal_handlers_unblock_by_func (button, button_cb, dd);
    }
}

static void
mt_proxy_prop_changed (GDBusProxy          *proxy,
                       GVariant            *changed,
                       const gchar* const  *invalidated,
                       DwellData           *dd)
{
    GVariantIter iter;
    gchar *key;
    GVariant *value;

    if (g_variant_n_children (changed) < 1)
        return;

    g_variant_iter_init (&iter, changed);
    while (g_variant_iter_loop (&iter, "{sv}", &key, &value))
    {
        if (g_strcmp0 (key, "ClickType") == 0)
        {
            handle_click_type_changed (dd, g_variant_get_int32 (value));
        }
    }
}

static void
mt_proxy_init (DwellData *dd)
{
    GError *error = NULL;

    dd->proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                               G_DBUS_PROXY_FLAGS_NONE, NULL,
                                               MOUSETWEAKS_DBUS_NAME,
                                               MOUSETWEAKS_DBUS_PATH,
                                               MOUSETWEAKS_DBUS_IFACE,
                                               NULL, &error);
    if (error)
    {
        g_warning ("%s\n", error->message);
        g_error_free (error);
        return;
    }

    dd->active = mt_proxy_has_owner (dd->proxy);

    g_signal_connect (dd->proxy, "notify::g-name-owner",
                      G_CALLBACK (mt_proxy_owner_notify), dd);
    g_signal_connect (dd->proxy, "g-properties-changed",
                      G_CALLBACK (mt_proxy_prop_changed), dd);
}

static void
mt_proxy_set_click_type (GDBusProxy *proxy,
                         MtClickType click_type)
{
    GVariant *ct;

    g_return_if_fail (proxy != NULL);

    ct = g_variant_new_int32 (click_type);

    g_dbus_proxy_call (proxy,
                       "org.freedesktop.DBus.Properties.Set",
                       g_variant_new ("(ssv)",
                                      MOUSETWEAKS_DBUS_IFACE,
                                      "ClickType", ct),
                       G_DBUS_CALL_FLAGS_NONE,
                       -1, NULL, NULL, NULL);
    g_variant_unref (ct);
}

static gboolean
do_not_eat (GtkWidget *widget, GdkEventButton *bev, gpointer user)
{
    if (bev->button != 1)
        g_signal_stop_emission_by_name (widget, "button_press_event");

    return FALSE;
}

static void
enable_dwell_changed (GtkToggleButton *button, DwellData *dd)
{
    g_settings_set_boolean (dd->gsd_settings,
                            KEY_DWELL_ENABLED,
                            gtk_toggle_button_get_active (button));
}

static gboolean
activation_timeout (DwellData *dd)
{
    dd->elapsed = g_timer_elapsed (dd->timer, NULL);
    if (dd->elapsed >= dd->delay)
    {
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (dd->enable), TRUE);
        dd->tid = 0;
        dd->elapsed = 0.0;
        return FALSE;
    }
    gtk_widget_queue_draw (dd->enable);

    return TRUE;
}

static gboolean
enable_dwell_crossing (GtkWidget        *widget,
                       GdkEventCrossing *event,
                       DwellData        *dd)
{
    if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (dd->enable)))
        return FALSE;

    if (event->type == GDK_ENTER_NOTIFY)
    {
        if (!dd->tid)
        {
            g_timer_start (dd->timer);
            dd->tid = g_timeout_add (100, (GSourceFunc) activation_timeout, dd);
        }
    }
    else
    {
        if (dd->tid)
        {
            g_source_remove (dd->tid);
            dd->tid = 0;
            dd->elapsed = 0.0;
        }
    }
    return FALSE;
}

static gboolean
enable_dwell_exposed (GtkWidget      *widget,
                      GdkEventExpose *event,
                      DwellData      *dd)
{
    cairo_t *cr;
    GdkColor c;
    gdouble x, y, w, h;
    gint fwidth, fpad;
    GtkAllocation allocation;
    GtkStyle *style;

    style = gtk_widget_get_style (widget);
    c = style->bg[GTK_STATE_SELECTED];
    gtk_widget_style_get (widget,
                          "focus-line-width", &fwidth,
                          "focus-padding", &fpad,
                          NULL);

    gtk_widget_get_allocation (widget, &allocation);
    x = allocation.x + fwidth + fpad;
    y = allocation.y + fwidth + fpad;
    w = allocation.width - (fwidth + fpad) * 2;
    h = allocation.height - (fwidth + fpad) * 2;

    cr = gdk_cairo_create (gtk_widget_get_window (widget));
    cairo_set_source_rgba (cr,
                           c.red   / 65535.,
                           c.green / 65535.,
                           c.blue  / 65535.,
                           0.5);
    cairo_rectangle (cr, x, y, w, h * dd->elapsed / dd->delay);
    cairo_fill (cr);
    cairo_destroy (cr);

    return FALSE;
}

static void
button_cb (GtkToggleButton *button, DwellData *dd)
{
    if (gtk_toggle_button_get_active (button))
    {
        GSList *group;

        group = gtk_radio_button_get_group (GTK_RADIO_BUTTON (button));
        dd->cct = g_slist_index (group, button);
        mt_proxy_set_click_type (dd->proxy, dd->cct);
    }
}

static void
button_size_allocate (GtkWidget     *widget,
                      GtkAllocation *alloc,
                      DwellData     *dd)
{
    GtkWidget *w;
    GdkPixbuf *tmp;
    gint i;

    if (dd->button_width == alloc->width &&
        dd->button_height == alloc->height)
        return;

    if (g_str_equal (gtk_widget_get_name (dd->box), "box_vert"))
    {
        /* vertical */
        for (i = 0; i < N_CLICK_TYPES; i++)
        {
            w = WID (img_widgets_v[i]);
            if (alloc->width < 32)
            {
                tmp = gdk_pixbuf_scale_simple (dd->click[i],
                                               alloc->width - 7,
                                               alloc->width - 7,
                                               GDK_INTERP_HYPER);
                if (tmp)
                {
                    gtk_image_set_from_pixbuf (GTK_IMAGE (w), tmp);
                    g_object_unref (tmp);
                }
            }
            else
                gtk_image_set_from_pixbuf (GTK_IMAGE (w), dd->click[i]);
        }
    }
    else
    {
        /* horizontal */
        for (i = 0; i < N_CLICK_TYPES; i++)
        {
            w = WID (img_widgets[i]);
            if (alloc->height < 32)
            {
                tmp = gdk_pixbuf_scale_simple (dd->click[i],
                                               alloc->height - 7,
                                               alloc->height - 7,
                                               GDK_INTERP_HYPER);
                if (tmp)
                {
                    gtk_image_set_from_pixbuf (GTK_IMAGE (w), tmp);
                    g_object_unref (tmp);
                }
            }
            else
                gtk_image_set_from_pixbuf (GTK_IMAGE (w), dd->click[i]);
        }
    }
    dd->button_width = alloc->width;
    dd->button_height = alloc->height;
}

/* applet callbacks */
static void
applet_orient_changed (PanelApplet *applet,
                       guint        orient,
                       DwellData   *dd)
{
    gboolean dwell;

    gtk_container_remove (GTK_CONTAINER (applet), g_object_ref (dd->box));

    switch (orient)
    {
        case PANEL_APPLET_ORIENT_UP:
        case PANEL_APPLET_ORIENT_DOWN:
            dd->box = WID ("box_hori");
            dd->ct_box = WID ("ct_box");
            dd->enable = WID ("enable");
            dd->button = WID ("single_click");
            break;
        case PANEL_APPLET_ORIENT_LEFT:
        case PANEL_APPLET_ORIENT_RIGHT:
            dd->box = WID ("box_vert");
            dd->ct_box = WID ("ct_box_v");
            dd->enable = WID ("enable_v");
            dd->button = WID ("single_click_v");
        default:
            break;
    }

    if (gtk_widget_get_parent (dd->box))
    {
        gtk_widget_reparent (dd->box, GTK_WIDGET (applet));
    }
    else
    {
        gtk_container_add (GTK_CONTAINER (applet), dd->box);
        g_object_unref (dd->box);
    }

    dwell = g_settings_get_boolean (dd->gsd_settings, KEY_DWELL_ENABLED);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (dd->enable), dwell);
    update_sensitivity (dd);
}

static void
applet_unrealized (GtkWidget *widget, DwellData *dd)
{
    gint i;

    for (i = 0; i < N_CLICK_TYPES; i++)
        if (dd->click[i])
            g_object_unref (dd->click[i]);

    g_object_unref (dd->ui);
    g_object_unref (dd->mt_settings);
    g_object_unref (dd->gsd_settings);
    g_object_unref (dd->proxy);
    g_timer_destroy (dd->timer);

    g_slice_free (DwellData, dd);
}

static void
preferences_dialog (BonoboUIComponent *component,
                    gpointer           data,
                    const char        *cname)
{
    GError *error = NULL;

    if (!g_spawn_command_line_async ("gnome-control-center universal-access", &error))
    {
         mt_common_show_dialog (_("Failed to Open the Univeral Access Panel"),
                                error->message,
                                MT_MESSAGE_WARNING);
         g_error_free (error);
    }
}

static void
help_dialog (BonoboUIComponent *component,
             DwellData         *dd,
             const char        *cname)
{
    mt_common_show_help (gtk_widget_get_screen (dd->box),
                         gtk_get_current_event_time ());
}

static void
about_dialog (BonoboUIComponent *component,
              DwellData         *dd,
              const char        *cname)
{
    gtk_window_present (GTK_WINDOW (WID ("about")));
}

static void
about_response (GtkWidget *about, gint response, DwellData *dd)
{
    gtk_widget_hide (WID ("about"));
}

static inline void
init_button (DwellData *dd, GtkWidget *button)
{
    gtk_toggle_button_set_mode (GTK_TOGGLE_BUTTON (button), FALSE);
    g_signal_connect (button, "button-press-event",
                      G_CALLBACK (do_not_eat), NULL);
    g_signal_connect (button, "toggled",
                      G_CALLBACK (button_cb), dd);
}

static void
setup_box (DwellData *dd)
{
    GtkWidget *widget;
    GtkSizeGroup *hgroup, *vgroup;
    gint i;

    /* horizontal */
    hgroup = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
    widget = WID ("single_click");
    init_button (dd, widget);
    gtk_size_group_add_widget (hgroup, widget);
    g_signal_connect (widget, "size-allocate",
                      G_CALLBACK (button_size_allocate), dd);

    init_button (dd, WID ("double_click"));
    init_button (dd, WID ("drag_click"));
    init_button (dd, WID ("right_click"));

    widget = WID ("enable");
    gtk_size_group_add_widget (hgroup, widget);
    g_signal_connect (widget, "button-press-event",
                      G_CALLBACK (do_not_eat), NULL);
    g_signal_connect (widget, "toggled",
                      G_CALLBACK (enable_dwell_changed), dd);
    g_signal_connect (widget, "enter-notify-event",
                      G_CALLBACK (enable_dwell_crossing), dd);
    g_signal_connect (widget, "leave-notify-event",
                      G_CALLBACK (enable_dwell_crossing), dd);
    g_signal_connect_after (widget, "expose-event",
                            G_CALLBACK (enable_dwell_exposed), dd);

    /* vertical */
    vgroup = gtk_size_group_new (GTK_SIZE_GROUP_VERTICAL);
    widget = WID ("single_click_v");
    gtk_size_group_add_widget (vgroup, widget);
    init_button (dd, widget);
    g_signal_connect (WID ("single_click_v"), "size-allocate",
                      G_CALLBACK (button_size_allocate), dd);
    init_button (dd, WID ("double_click_v"));
    init_button (dd, WID ("drag_click_v"));
    init_button (dd, WID ("right_click_v"));

    widget = WID ("enable_v");
    gtk_size_group_add_widget (vgroup, widget);
    g_signal_connect (widget, "button-press-event",
                      G_CALLBACK (do_not_eat), NULL);
    g_signal_connect (widget, "toggled",
                      G_CALLBACK (enable_dwell_changed), dd);
    g_signal_connect (widget, "enter-notify-event",
                      G_CALLBACK (enable_dwell_crossing), dd);
    g_signal_connect (widget, "leave-notify-event",
                      G_CALLBACK (enable_dwell_crossing), dd);
    g_signal_connect_after (widget, "expose-event",
                            G_CALLBACK (enable_dwell_exposed), dd);

    for (i = 0; i < N_CLICK_TYPES; i++)
    {
        gtk_image_set_from_pixbuf (GTK_IMAGE (WID (img_widgets[i])),
                                   dd->click[i]);
        gtk_image_set_from_pixbuf (GTK_IMAGE (WID (img_widgets_v[i])),
                                   dd->click[i]);
    }
}

static void
dwell_enabled_changed (GSettings *settings, gchar *key, DwellData *dd)
{
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (dd->enable),
                                  g_settings_get_boolean (settings, key));
    update_sensitivity (dd);
}

static void
dwell_mode_changed (GSettings *settings, gchar *key, DwellData *dd)
{
    update_sensitivity (dd);
}

static void
dwell_time_changed (GSettings *settings, gchar *key, DwellData *dd)
{
    dd->delay = g_settings_get_double (settings, key);
}

static gboolean
fill_applet (PanelApplet *applet)
{
    DwellData *dd;
    GError *error = NULL;
    GtkWidget *about;
    PanelAppletOrient orient;

    bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    g_set_application_name (_("Dwell Click"));
    gtk_window_set_default_icon_name (MT_ICON_NAME);

    dd = g_slice_new0 (DwellData);

    /* user interface */
    dd->ui = gtk_builder_new ();
    gtk_builder_add_from_file (dd->ui, DATADIR "/dwell-click-applet.ui", &error);
    if (error)
    {
        g_print ("%s\n", error->message);
        g_error_free (error);

        g_object_unref (dd->ui);
        g_slice_free (DwellData, dd);

        return FALSE;
    }

    /* dbus */
    mt_proxy_init (dd);

    dd->cct = DWELL_CLICK_TYPE_SINGLE;
    dd->timer = g_timer_new ();

    /* about dialog */
    about = WID ("about");
    g_object_set (about, "version", VERSION, NULL);
    g_signal_connect (about, "delete-event",
                      G_CALLBACK (gtk_widget_hide_on_delete), NULL);
    g_signal_connect (about, "response",
                      G_CALLBACK (about_response), dd);

    /* gsettings */
    dd->mt_settings = g_settings_new (MT_SCHEMA_ID);
    g_signal_connect (dd->mt_settings, "value::" KEY_DWELL_MODE,
                      G_CALLBACK (dwell_mode_changed), dd);
    g_signal_connect (dd->mt_settings, "value::" KEY_DWELL_TIME,
                      G_CALLBACK (dwell_time_changed), dd);

    dd->gsd_settings = g_settings_new (GSD_MOUSE_SCHEMA_ID);
    g_signal_connect (dd->gsd_settings, "value::" KEY_DWELL_ENABLED,
                      G_CALLBACK (dwell_enabled_changed), dd);

    /* icons */
    dd->click[DWELL_CLICK_TYPE_SINGLE] =
        gdk_pixbuf_new_from_file (DATADIR "/single-click.png", NULL);
    dd->click[DWELL_CLICK_TYPE_DOUBLE] =
        gdk_pixbuf_new_from_file (DATADIR "/double-click.png", NULL);
    dd->click[DWELL_CLICK_TYPE_DRAG] =
        gdk_pixbuf_new_from_file (DATADIR "/drag-click.png", NULL);
    dd->click[DWELL_CLICK_TYPE_RIGHT] =
        gdk_pixbuf_new_from_file (DATADIR "/right-click.png", NULL);

    /* applet initialization */
    panel_applet_set_flags (applet,
                            PANEL_APPLET_EXPAND_MINOR |
                            PANEL_APPLET_HAS_HANDLE);
    panel_applet_set_background_widget (applet, GTK_WIDGET(applet));
    panel_applet_setup_menu_from_file (applet,
                                       DATADIR, "DwellClick.xml",
                                       NULL, menu_verb, dd);

    g_signal_connect (applet, "change-orient",
                      G_CALLBACK (applet_orient_changed), dd);
    g_signal_connect (applet, "unrealize",
                      G_CALLBACK (applet_unrealized), dd);

    /* check initial orientation */
    orient = panel_applet_get_orient (applet);
    if (orient == PANEL_APPLET_ORIENT_UP ||
        orient == PANEL_APPLET_ORIENT_DOWN)
    {
            dd->box = WID ("box_hori");
            dd->ct_box = WID ("ct_box");
            dd->enable = WID ("enable");
            dd->button = WID ("single_click");
    }
    else
    {
        dd->box = WID ("box_vert");
        dd->ct_box = WID ("ct_box_v");
        dd->enable = WID ("enable_v");
        dd->button = WID ("single_click_v");
    }

    dd->delay = g_settings_get_double (dd->mt_settings, KEY_DWELL_TIME);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (dd->enable), 
                                  g_settings_get_boolean (dd->gsd_settings,
                                                          KEY_DWELL_ENABLED));

    setup_box (dd);
    gtk_widget_reparent (dd->box, GTK_WIDGET (applet));
    gtk_widget_show (GTK_WIDGET (applet));
    update_sensitivity (dd);

    return TRUE;
}

static gboolean
applet_factory (PanelApplet *applet, const gchar *iid, gpointer data)
{
    if (!g_str_equal (iid, "OAFIID:DwellClickApplet"))
        return FALSE;

    return fill_applet (applet);
}

PANEL_APPLET_BONOBO_FACTORY ("OAFIID:DwellClickApplet_Factory",
                             PANEL_TYPE_APPLET,
                             "Dwell Click Factory",
                             VERSION,
                             applet_factory,
                             NULL);
