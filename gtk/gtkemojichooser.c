/* gtkemojichooser.c: An Emoji chooser widget
 * Copyright 2017, Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "gtkemojichooser.h"

#include "gtkadjustment.h"
#include "gtkbox.h"
#include "gtkbutton.h"
#include "gtkcssprovider.h"
#include "gtkentry.h"
#include "gtkflowbox.h"
#include "gtkstack.h"
#include "gtklabel.h"
#include "gtkgesturelongpress.h"
#include "gtkpopover.h"
#include "gtkscrolledwindow.h"
#include "gtkintl.h"
#include "gtkprivate.h"

typedef struct {
  GtkWidget *box;
  GtkWidget *heading;
  GtkWidget *button;
  const char *first;
  gunichar label;
  gboolean empty;
} EmojiSection;

struct _GtkEmojiChooser
{
  GtkPopover parent_instance;

  GtkWidget *search_entry;
  GtkWidget *stack;
  GtkWidget *scrolled_window;

  EmojiSection recent;
  EmojiSection people;
  EmojiSection body;
  EmojiSection nature;
  EmojiSection food;
  EmojiSection travel;
  EmojiSection activities;
  EmojiSection objects;
  EmojiSection symbols;
  EmojiSection flags;

  GtkGesture *recent_press;
  GtkGesture *people_press;
  GtkGesture *body_press;

  GVariant *data;

  GSettings *settings;
};

struct _GtkEmojiChooserClass {
  GtkPopoverClass parent_class;
};

enum {
  EMOJI_PICKED,
  LAST_SIGNAL
};

static int signals[LAST_SIGNAL];

G_DEFINE_TYPE (GtkEmojiChooser, gtk_emoji_chooser, GTK_TYPE_POPOVER)

static void
gtk_emoji_chooser_finalize (GObject *object)
{
  GtkEmojiChooser *chooser = GTK_EMOJI_CHOOSER (object);

  g_variant_unref (chooser->data);
  g_object_unref (chooser->settings);

  G_OBJECT_CLASS (gtk_emoji_chooser_parent_class)->finalize (object);
}

static void
scroll_to_section (GtkButton *button,
                   gpointer   data)
{
  EmojiSection *section = data;
  GtkEmojiChooser *chooser;
  GtkAdjustment *adj;
  GtkAllocation alloc = { 0, 0, 0, 0 };
  double page_increment, value;
  gboolean dummy;

  chooser = GTK_EMOJI_CHOOSER (gtk_widget_get_ancestor (GTK_WIDGET (button), GTK_TYPE_EMOJI_CHOOSER));

  adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (chooser->scrolled_window));
  if (section->heading)
    gtk_widget_get_allocation (section->heading, &alloc);
  page_increment = gtk_adjustment_get_page_increment (adj);
  value = gtk_adjustment_get_value (adj);
  gtk_adjustment_set_page_increment (adj, alloc.y - value);
  g_signal_emit_by_name (chooser->scrolled_window, "scroll-child", GTK_SCROLL_PAGE_FORWARD, FALSE, &dummy);
  gtk_adjustment_set_page_increment (adj, page_increment);
}

static void
add_emoji (GtkWidget    *box,
           gboolean      prepend,
           GVariantIter *iter,
           GVariant     *data);

#define MAX_RECENT (7*3)

static void
add_recent_item (GtkEmojiChooser *chooser,
                 GVariant       *item)
{
  GList *children, *l;
  GVariantIter *codes;
  const char *name;
  int i;
  GVariantBuilder builder;

  g_variant_ref (item);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ausaau)"));
  g_variant_builder_add_value (&builder, item);

  g_variant_get_child (item, 1, "&s", &name);

  children = gtk_container_get_children (GTK_CONTAINER (chooser->recent.box));
  for (l = children, i = 1; l; l = l->next, i++)
    {
      GVariant *item2 = g_object_get_data (G_OBJECT (l->data), "emoji-data");
      const char *name2;

      g_variant_get_child (item2, 1, "&s", &name2);
      if (strcmp (name, name2) == 0)
        {
          gtk_widget_destroy (GTK_WIDGET (l->data));
          i--;
          continue;
        }
      if (i >= MAX_RECENT)
        {
          gtk_widget_destroy (GTK_WIDGET (l->data));
          continue;
        }

      g_variant_builder_add_value (&builder, item2);
    }
  g_list_free (children);

  g_variant_get_child (item, 0, "au", &codes);
  add_emoji (chooser->recent.box, TRUE, codes, item);
  g_variant_iter_free (codes);

  g_settings_set_value (chooser->settings, "recent-emoji", g_variant_builder_end (&builder));

  g_variant_unref (item);
}

static void
emoji_activated (GtkFlowBox      *box,
                 GtkFlowBoxChild *child,
                 gpointer         data)
{
  GtkEmojiChooser *chooser = data;
  char *text;
  GtkWidget *label;
  GVariant *item;

  gtk_popover_popdown (GTK_POPOVER (chooser));

  label = gtk_bin_get_child (GTK_BIN (child));
  text = g_strdup (gtk_label_get_label (GTK_LABEL (label)));

  item = (GVariant*) g_object_get_data (G_OBJECT (child), "emoji-data");
  add_recent_item (chooser, item);

  g_signal_emit (data, signals[EMOJI_PICKED], 0, text);
  g_free (text);
}

static void
long_pressed_cb (GtkGesture *gesture,
                 double      x,
                 double      y,
                 gpointer    data)
{
  GtkWidget *child;
  GtkWidget *popover;
  GtkWidget *view;
  GtkWidget *box;
  GVariant *emoji_data;
  GtkWidget *parent_popover;
  GVariantIter *iter;
  GVariantIter *codes;

  box = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  child = GTK_WIDGET (gtk_flow_box_get_child_at_pos (GTK_FLOW_BOX (box), x, y));
  if (!child)
    return;

  emoji_data = (GVariant*) g_object_get_data (G_OBJECT (child), "emoji-data");
  if (!emoji_data)
    return;

  g_variant_get_child (emoji_data, 2, "aau", &iter);
  if (g_variant_iter_n_children (iter) == 0)
    {
      g_variant_iter_free (iter);
      return;
    }

  parent_popover = gtk_widget_get_ancestor (child, GTK_TYPE_POPOVER);
  popover = gtk_popover_new (child);
  view = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_style_context_add_class (gtk_widget_get_style_context (view), "view");
  box = gtk_flow_box_new ();
  gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (box), TRUE);
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (box), 6);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (box), 6);
  gtk_flow_box_set_activate_on_single_click (GTK_FLOW_BOX (box), TRUE);
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (box), GTK_SELECTION_NONE);
  gtk_container_add (GTK_CONTAINER (popover), view);
  gtk_container_add (GTK_CONTAINER (view), box);

  g_signal_connect (box, "child-activated", G_CALLBACK (emoji_activated), parent_popover);

  g_variant_get_child (emoji_data, 0, "au", &codes);
  add_emoji (box, FALSE, codes, emoji_data);
  g_variant_iter_free (codes);
  while (g_variant_iter_next (iter, "au", &codes))
    {
      add_emoji (box, FALSE, codes, emoji_data);
      g_variant_iter_free (codes);
    }

  gtk_popover_popup (GTK_POPOVER (popover));
}

static void
add_emoji (GtkWidget    *box,
           gboolean      prepend,
           GVariantIter *iter,
           GVariant     *data)
{
  GtkWidget *child;
  GtkWidget *label;
  PangoAttrList *attrs;
  char text[64];
  char *p = text;
  gunichar code;

  while (g_variant_iter_next (iter, "u", &code))
    p += g_unichar_to_utf8 (code, p);
  p[0] = 0;

  label = gtk_label_new (text);
  attrs = pango_attr_list_new ();
  pango_attr_list_insert (attrs, pango_attr_scale_new (PANGO_SCALE_X_LARGE));
  gtk_label_set_attributes (GTK_LABEL (label), attrs);
  pango_attr_list_unref (attrs);

  child = gtk_flow_box_child_new ();
  gtk_style_context_add_class (gtk_widget_get_style_context (child), "emoji");
  g_object_set_data_full (G_OBJECT (child), "emoji-data",
                          g_variant_ref (data),
                          (GDestroyNotify)g_variant_unref);

  gtk_container_add (GTK_CONTAINER (child), label);
  gtk_flow_box_insert (GTK_FLOW_BOX (box), child, prepend ? 0 : -1);
}

static void
populate_emoji_chooser (GtkEmojiChooser *chooser)
{
  g_autoptr(GBytes) bytes = NULL;
  GVariantIter iter;
  GVariant *item;
  GtkWidget *box;

  bytes = g_resources_lookup_data ("/org/gtk/libgtk/emoji/emoji.data", 0, NULL);
  chooser->data = g_variant_ref_sink (g_variant_new_from_bytes (G_VARIANT_TYPE ("a(ausaau)"), bytes, TRUE));

  g_variant_iter_init (&iter, chooser->data);
  box = chooser->people.box;
  while ((item = g_variant_iter_next_value (&iter)))
    {
      GVariantIter *codes;
      const char *name;

      g_variant_get_child (item, 0, "au", &codes);
      g_variant_get_child (item, 1, "&s", &name);

      if (strcmp (name, chooser->body.first) == 0)
        box = chooser->body.box;
      else if (strcmp (name, chooser->nature.first) == 0)
        box = chooser->nature.box;
      else if (strcmp (name, chooser->food.first) == 0)
        box = chooser->food.box;
      else if (strcmp (name, chooser->travel.first) == 0)
        box = chooser->travel.box;
      else if (strcmp (name, chooser->activities.first) == 0)
        box = chooser->activities.box;
      else if (strcmp (name, chooser->objects.first) == 0)
        box = chooser->objects.box;
      else if (strcmp (name, chooser->symbols.first) == 0)
        box = chooser->symbols.box;
      else if (strcmp (name, chooser->flags.first) == 0)
        box = chooser->flags.box;

      add_emoji (box, FALSE, codes, item);
      g_variant_iter_free (codes);
    }
}

static void
update_state (EmojiSection *section,
              double        value)
{
  GtkAllocation alloc = { 0, 0, 0, 20 };

  if (section->heading)
    gtk_widget_get_allocation (section->heading, &alloc);

  if (alloc.y <= value && value < alloc.y + alloc.height)
    gtk_widget_set_state_flags (section->button, GTK_STATE_FLAG_CHECKED, FALSE);
  else
    gtk_widget_unset_state_flags (section->button, GTK_STATE_FLAG_CHECKED);
}

static void
adj_value_changed (GtkAdjustment *adj,
                   gpointer       data)
{
  GtkEmojiChooser *chooser = data;
  double value = gtk_adjustment_get_value (adj);

  update_state (&chooser->recent, value);
  update_state (&chooser->people, value);
  update_state (&chooser->body, value);
  update_state (&chooser->nature, value);
  update_state (&chooser->food, value);
  update_state (&chooser->travel, value);
  update_state (&chooser->activities, value);
  update_state (&chooser->objects, value);
  update_state (&chooser->symbols, value);
  update_state (&chooser->flags, value);
}

static gboolean
filter_func (GtkFlowBoxChild *child,
             gpointer         data)
{
  EmojiSection *section = data;
  GtkEmojiChooser *chooser;
  GVariant *emoji_data;
  const char *text;
  const char *name;
  gboolean res;

  res = TRUE;

  chooser = GTK_EMOJI_CHOOSER (gtk_widget_get_ancestor (GTK_WIDGET (child), GTK_TYPE_EMOJI_CHOOSER));
  text = gtk_entry_get_text (GTK_ENTRY (chooser->search_entry));
  emoji_data = (GVariant *) g_object_get_data (G_OBJECT (child), "emoji-data");

  if (text[0] == 0)
    goto out;

  if (!emoji_data)
    goto out;

  g_variant_get_child (emoji_data, 1, "&s", &name);
  res = strstr (name, text) != NULL;

out:
  if (res)
    section->empty = FALSE;

  return res;
}

static void
invalidate_section (EmojiSection *section)
{
  section->empty = TRUE;
  gtk_flow_box_invalidate_filter (GTK_FLOW_BOX (section->box));
}

static gboolean
update_headings (gpointer data)
{
  GtkEmojiChooser *chooser = data;

  gtk_widget_set_visible (chooser->people.heading, !chooser->people.empty);
  gtk_widget_set_visible (chooser->body.heading, !chooser->body.empty);
  gtk_widget_set_visible (chooser->nature.heading, !chooser->nature.empty);
  gtk_widget_set_visible (chooser->food.heading, !chooser->food.empty);
  gtk_widget_set_visible (chooser->travel.heading, !chooser->travel.empty);
  gtk_widget_set_visible (chooser->activities.heading, !chooser->activities.empty);
  gtk_widget_set_visible (chooser->objects.heading, !chooser->objects.empty);
  gtk_widget_set_visible (chooser->symbols.heading, !chooser->symbols.empty);
  gtk_widget_set_visible (chooser->flags.heading, !chooser->flags.empty);

  if (chooser->recent.empty && chooser->people.empty &&
      chooser->body.empty && chooser->nature.empty &&
      chooser->food.empty && chooser->travel.empty &&
      chooser->activities.empty && chooser->objects.empty &&
      chooser->symbols.empty && chooser->flags.empty)
    gtk_stack_set_visible_child_name (GTK_STACK (chooser->stack), "empty");
  else
    gtk_stack_set_visible_child_name (GTK_STACK (chooser->stack), "list");

  return G_SOURCE_REMOVE;
}

static void
search_changed (GtkEntry *entry,
                gpointer  data)
{
  GtkEmojiChooser *chooser = data;

  invalidate_section (&chooser->recent);
  invalidate_section (&chooser->people);
  invalidate_section (&chooser->body);
  invalidate_section (&chooser->nature);
  invalidate_section (&chooser->food);
  invalidate_section (&chooser->travel);
  invalidate_section (&chooser->activities);
  invalidate_section (&chooser->objects);
  invalidate_section (&chooser->symbols);
  invalidate_section (&chooser->flags);

  g_idle_add (update_headings, data);
}

static void
setup_section (GtkEmojiChooser *chooser,
               EmojiSection   *section,
               const char     *first,
               gunichar        label)
{
  char text[14];
  char *p;
  GtkAdjustment *adj;

  section->first = first;

  p = text;
  p += g_unichar_to_utf8 (label, p);
  p += g_unichar_to_utf8 (0xfe0e, p);
  p[0] = 0;
  gtk_button_set_label (GTK_BUTTON (section->button), text);

  adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (chooser->scrolled_window));

  gtk_container_set_focus_vadjustment (GTK_CONTAINER (section->box), adj);
  gtk_flow_box_set_filter_func (GTK_FLOW_BOX (section->box), filter_func, section, NULL);
  g_signal_connect (section->button, "clicked", G_CALLBACK (scroll_to_section), section);
}

static void
gtk_emoji_chooser_init (GtkEmojiChooser *chooser)
{
  GtkAdjustment *adj;
  GVariant *variant;
  GVariantIter iter;
  GVariant *item;

  gtk_widget_init_template (GTK_WIDGET (chooser));

  chooser->recent_press = gtk_gesture_long_press_new (chooser->recent.box);
  g_signal_connect (chooser->recent_press, "pressed", G_CALLBACK (long_pressed_cb), chooser);

  chooser->people_press = gtk_gesture_long_press_new (chooser->people.box);
  g_signal_connect (chooser->people_press, "pressed", G_CALLBACK (long_pressed_cb), chooser);

  chooser->body_press = gtk_gesture_long_press_new (chooser->body.box);
  g_signal_connect (chooser->body_press, "pressed", G_CALLBACK (long_pressed_cb), chooser);

  adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (chooser->scrolled_window));
  g_signal_connect (adj, "value-changed", G_CALLBACK (adj_value_changed), chooser);

  setup_section (chooser, &chooser->recent, NULL, 0x1f557);
  setup_section (chooser, &chooser->people, "grinning face", 0x1f642);
  setup_section (chooser, &chooser->body, "selfie", 0x1f44d);
  setup_section (chooser, &chooser->nature, "monkey face", 0x1f337);
  setup_section (chooser, &chooser->food, "grapes", 0x1f374);
  setup_section (chooser, &chooser->travel, "globe showing Europe-Africa", 0x2708);
  setup_section (chooser, &chooser->activities, "jack-o-lantern", 0x1f3c3);
  setup_section (chooser, &chooser->objects, "muted speaker", 0x1f514);
  setup_section (chooser, &chooser->symbols, "ATM sign", 0x2764);
  setup_section (chooser, &chooser->flags, "chequered flag", 0x1f3f4);

  populate_emoji_chooser (chooser);

  chooser->settings = g_settings_new ("org.gtk.Settings.EmojiChooser");
  variant = g_settings_get_value (chooser->settings, "recent-emoji");
  g_variant_iter_init (&iter, variant);
  while ((item = g_variant_iter_next_value (&iter)))
    {
      GVariantIter *codes;

      g_variant_get_child (item, 0, "au", &codes);
      add_emoji (chooser->recent.box, FALSE, codes, item);
      g_variant_iter_free (codes);
    }
  g_variant_unref (variant);
}

static void
gtk_emoji_chooser_show (GtkWidget *widget)
{
  GtkEmojiChooser *chooser = GTK_EMOJI_CHOOSER (widget);
  GtkAdjustment *adj;

  adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (chooser->scrolled_window));
  gtk_adjustment_set_value (adj, 0);

  gtk_entry_set_text (GTK_ENTRY (chooser->search_entry), "");

  GTK_WIDGET_CLASS (gtk_emoji_chooser_parent_class)->show (widget);
}

static void
gtk_emoji_chooser_class_init (GtkEmojiChooserClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = gtk_emoji_chooser_finalize;
  widget_class->show = gtk_emoji_chooser_show;

  signals[EMOJI_PICKED] = g_signal_new ("emoji-picked",
                                        G_OBJECT_CLASS_TYPE (object_class),
                                        G_SIGNAL_RUN_LAST,
                                        0,
                                        NULL, NULL,
                                        NULL,
                                        G_TYPE_NONE, 1, G_TYPE_STRING|G_SIGNAL_TYPE_STATIC_SCOPE);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gtk/libgtk/ui/gtkemojichooser.ui");

  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, search_entry);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, stack);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, scrolled_window);

  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, recent.box);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, recent.button);

  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, people.box);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, people.heading);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, people.button);

  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, body.box);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, body.heading);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, body.button);

  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, nature.box);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, nature.heading);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, nature.button);

  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, food.box);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, food.heading);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, food.button);

  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, travel.box);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, travel.heading);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, travel.button);

  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, activities.box);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, activities.heading);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, activities.button);

  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, objects.box);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, objects.heading);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, objects.button);

  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, symbols.box);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, symbols.heading);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, symbols.button);

  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, flags.box);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, flags.heading);
  gtk_widget_class_bind_template_child (widget_class, GtkEmojiChooser, flags.button);

  gtk_widget_class_bind_template_callback (widget_class, emoji_activated);
  gtk_widget_class_bind_template_callback (widget_class, search_changed);
}

GtkWidget *
gtk_emoji_chooser_new (void)
{
  return GTK_WIDGET (g_object_new (GTK_TYPE_EMOJI_CHOOSER, NULL));
}