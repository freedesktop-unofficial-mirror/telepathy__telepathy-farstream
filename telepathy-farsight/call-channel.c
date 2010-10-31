/*
 * call-channel.c - Source for TfCallChannel
 * Copyright (C) 2010 Collabora Ltd.
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/**
 * SECTION:call-channel

 * @short_description: Handle the Call interface on a Channel
 *
 * This class handles the
 * org.freedesktop.Telepathy.Channel.Interface.Call on a
 * channel using Farsight2.
 */


#include "call-channel.h"

#include <telepathy-glib/util.h>
#include <telepathy-glib/interfaces.h>
#include <gst/farsight/fs-conference-iface.h>

#include "extensions/extensions.h"

#include "call-content.h"
#include "tf-signals-marshal.h"



G_DEFINE_TYPE (TfCallChannel, tf_call_channel, G_TYPE_OBJECT);


enum
{
  PROP_FS_CONFERENCES = 1
};


enum
{
  SIGNAL_FS_CONFERENCE_ADDED,
  SIGNAL_FS_CONFERENCE_REMOVED,
  SIGNAL_COUNT
};

static guint signals[SIGNAL_COUNT] = {0};

struct CallConference {
  gint use_count;
  gchar *conference_type;
  FsConference *fsconference;
};

static void
tf_call_channel_get_property (GObject    *object,
    guint       property_id,
    GValue     *value,
    GParamSpec *pspec);

static void tf_call_channel_dispose (GObject *object);

static void
tf_call_channel_class_init (TfCallChannelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = tf_call_channel_dispose;
  object_class->get_property = tf_call_channel_get_property;

  g_object_class_install_property (object_class, PROP_FS_CONFERENCES,
      g_param_spec_object ("fs-conferences",
          "Farsight2 FsConference object",
          "GPtrArray of Farsight2 FsConferences for this channel",
          G_TYPE_PTR_ARRAY,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  signals[SIGNAL_FS_CONFERENCE_ADDED] = g_signal_new ("fs-conference-added",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL,
      _tf_marshal_VOID__OBJECT,
      G_TYPE_NONE, 1, G_TYPE_OBJECT);

  signals[SIGNAL_FS_CONFERENCE_REMOVED] = g_signal_new ("fs-conference-removed",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL,
      _tf_marshal_VOID__OBJECT,
      G_TYPE_NONE, 1, G_TYPE_OBJECT);

}

static void
free_call_conference (gpointer data)
{
  struct CallConference *cc = data;

  gst_object_unref (cc->fsconference);
  g_slice_free (struct CallConference, data);
}

static void
tf_call_channel_init (TfCallChannel *self)
{
  self->fsconferences = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
      free_call_conference);
}

static void
tf_call_channel_dispose (GObject *object)
{
  TfCallChannel *self = TF_CALL_CHANNEL (object);

  g_debug (G_STRFUNC);

  if (self->contents)
    g_hash_table_destroy (self->contents);
  self->contents = NULL;

  if (self->fsconferences)
      g_hash_table_unref (self->fsconferences);
  self->fsconferences = NULL;

  if (self->proxy)
    g_object_unref (self->proxy);
  self->proxy = NULL;

  if (G_OBJECT_CLASS (tf_call_channel_parent_class)->dispose)
    G_OBJECT_CLASS (tf_call_channel_parent_class)->dispose (object);
}

static void
conf_into_ptr_array (gpointer key, gpointer value, gpointer data)
{
  struct CallConference *cc = value;
  GPtrArray *array = data;

  g_ptr_array_add (array, cc->fsconference);
}

static void
tf_call_channel_get_property (GObject    *object,
    guint       property_id,
    GValue     *value,
    GParamSpec *pspec)
{
  TfCallChannel *self = TF_CALL_CHANNEL (object);

  switch (property_id)
    {
    case PROP_FS_CONFERENCES:
      {
        GPtrArray *array = g_ptr_array_sized_new (
            g_hash_table_size (self->fsconferences));

        g_ptr_array_set_free_func (array, gst_object_unref);
        g_hash_table_foreach (self->fsconferences, conf_into_ptr_array, array);
        g_value_take_boxed (value, array);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static gboolean
add_content (TfCallChannel *self, const gchar *content_path)
{
  GError *error = NULL;
  TfCallContent *content = tf_call_content_new (self, content_path, &error);

  if (error)
    {
      /* Error was already transmitter to the Content by the CM */
      g_clear_error (&error);
      return FALSE;
    }

  g_hash_table_insert (self->contents, g_strdup (content_path), content);

  return TRUE;
}

static void
got_contents (TpProxy *proxy, const GValue *out_value,
    const GError *error, gpointer user_data, GObject *weak_object)
{
  TfCallChannel *self = TF_CALL_CHANNEL (weak_object);
  GPtrArray *contents;
  guint i;

  if (error)
    {
      g_warning ("Error getting the Contents property: %s",
          error->message);
      tf_call_channel_error (self);
      return;
    }

  contents = g_value_get_boxed (out_value);

  self->contents = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, g_object_unref);

  for (i = 0; i < contents->len; i++)
    if (!add_content (self, g_ptr_array_index (contents, i)))
      break;
}

static void
content_added (TpChannel *proxy,
    const gchar *arg_Content,
    gpointer user_data,
    GObject *weak_object)
{
  TfCallChannel *self = TF_CALL_CHANNEL (weak_object);

  /* Ignore signals before we got the "Contents" property to avoid races that
   * could cause the same content to be added twice
   */

  if (!self->contents)
    return;

  add_content (self, arg_Content);
}

static void
content_removed (TpChannel *proxy,
    const gchar *arg_Content,
    gpointer user_data,
    GObject *weak_object)
{
  TfCallChannel *self = TF_CALL_CHANNEL (weak_object);

  if (!self->contents)
    return;

  g_hash_table_remove (self->contents, arg_Content);
}


static void
got_hardware_streaming (TpProxy *proxy, const GValue *out_value,
    const GError *error, gpointer user_data, GObject *weak_object)
{
  TfCallChannel *self = TF_CALL_CHANNEL (weak_object);
  GError *myerror = NULL;

  if (error)
    {
      g_warning ("Error getting the hardware streaming property: %s",
          error->message);
      tf_call_channel_error (self);
      return;
    }

  if (g_value_get_boolean (out_value))
    {
      g_warning ("Hardware streaming property is not TRUE, ignoring");
      return;
    }

  tp_cli_dbus_properties_call_get (proxy, -1,
      TF_FUTURE_IFACE_CHANNEL_TYPE_CALL,
      TF_FUTURE_PROP_CHANNEL_TYPE_CALL_CONTENTS,
      got_contents, NULL, NULL, G_OBJECT (self));

  tf_future_cli_channel_type_call_connect_to_content_added (TP_CHANNEL (proxy),
      content_added, NULL, NULL, G_OBJECT (self), &myerror);
  if (myerror)
    {
      g_warning ("Error connectiong to ContentAdded signal: %s",
          myerror->message);
      g_clear_error (&myerror);
      tf_call_channel_error (self);
      return;
    }

  tf_future_cli_channel_type_call_connect_to_content_removed (
      TP_CHANNEL (proxy), content_removed, NULL, NULL, G_OBJECT (self),
      &myerror);
  if (myerror)
    {
      g_warning ("Error connectiong to ContentRemoved signal: %s",
          myerror->message);
      g_clear_error (&myerror);
      tf_call_channel_error (self);
      return;
    }
}

TfCallChannel *
tf_call_channel_new (TpChannel *channel)
{
  TfCallChannel *self = g_object_new (
      TF_TYPE_CALL_CHANNEL, NULL);

  self->proxy = g_object_ref (channel);

  tp_cli_dbus_properties_call_get (channel, -1,
      TF_FUTURE_IFACE_CHANNEL_TYPE_CALL,
      TF_FUTURE_PROP_CHANNEL_TYPE_CALL_HARDWARE_STREAMING,
      got_hardware_streaming, NULL, NULL, G_OBJECT (self));

  return self;
}


static gboolean
find_conf_func (gpointer key, gpointer value, gpointer data)
{
  FsConference *conf = data;
  struct CallConference *cc = value;

  if (cc->fsconference == conf)
    return TRUE;
  else
    return FALSE;
}

static struct CallConference *
find_call_conference_by_conference (TfCallChannel *channel,
    GstObject *conference)
{
  return g_hash_table_find (channel->fsconferences, find_conf_func,
      conference);
}

gboolean
tf_call_channel_bus_message (TfCallChannel *channel,
    GstMessage *message)
{
  GError *error = NULL;
  gchar *debug;
  GHashTableIter iter;
  gpointer key, value;
  struct CallConference *cc;

  cc = find_call_conference_by_conference (channel, GST_MESSAGE_SRC (message));
  if (!cc)
    return FALSE;

  switch (GST_MESSAGE_TYPE (message))
    {
    case GST_MESSAGE_WARNING:
      gst_message_parse_warning (message, &error, &debug);

      g_warning ("session: %s (%s)", error->message, debug);

      g_error_free (error);
      g_free (debug);
      return TRUE;
    case GST_MESSAGE_ERROR:
      gst_message_parse_error (message, &error, &debug);

      g_warning ("session ERROR: %s (%s)", error->message, debug);

      tf_call_channel_error (channel);

      g_error_free (error);
      g_free (debug);
      return TRUE;
    default:
      break;
    }

  g_hash_table_iter_init (&iter, channel->contents);

  while (g_hash_table_iter_next (&iter, &key, &value))
    if (tf_call_channel_bus_message (value, message))
      return TRUE;

  return FALSE;
}

void
tf_call_channel_error (TfCallChannel *channel)
{
  tf_future_cli_channel_type_call_call_hangup (channel->proxy,
      -1, TF_FUTURE_CALL_STATE_CHANGE_REASON_UNKNOWN, "", "",
      NULL, NULL, NULL, NULL);
}


FsConference *
_tf_call_channel_get_conference (TfCallChannel *channel,
    const gchar *conference_type)
{
  gchar *tmp;
  struct CallConference *cc;

  cc = g_hash_table_lookup (channel->fsconferences, conference_type);

  if (cc)
    {
      cc->use_count++;
      gst_object_ref (cc->fsconference);
      return cc->fsconference;
    }

  cc = g_slice_new (struct CallConference);
  cc->use_count = 1;
  cc->conference_type = g_strdup (conference_type);

  tmp = g_strdup_printf ("fs%sconference", conference_type);
  cc->fsconference = FS_CONFERENCE (gst_element_factory_make (tmp, NULL));
  g_free (tmp);
  gst_object_ref (cc->fsconference);
  g_hash_table_insert (channel->fsconferences, cc->conference_type, cc);

  g_signal_emit (channel, signals[SIGNAL_FS_CONFERENCE_ADDED], 0,
      cc->fsconference);
  g_object_notify (G_OBJECT (channel), "fs-conferences");

  gst_object_ref (cc->fsconference);

  return cc->fsconference;
}

void
_tf_call_channel_put_conference (TfCallChannel *channel,
    FsConference *conference)
{
  struct CallConference *cc;

  cc = find_call_conference_by_conference (channel, GST_OBJECT (conference));
  if (!cc)
    {
      g_warning ("Trying to put conference that does not exist");
      return;
    }

  cc->use_count--;

  if (cc->use_count <= 0)
    {
      g_signal_emit (channel, signals[SIGNAL_FS_CONFERENCE_REMOVED], 0,
          cc->fsconference);
      g_hash_table_remove (channel->fsconferences, cc->conference_type);
      g_object_notify (G_OBJECT (channel), "fs-conferences");
    }
}
