/*
 * call-content.c - Source for TfCallContent
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
 * SECTION:call-content

 * @short_description: Handle the Call interface on a Channel
 *
 * This class handles the
 * org.freedesktop.Telepathy.Channel.Interface.Call on a
 * channel using Farstream.
 */

/* TODO:
 *
 * In MediaDescription:
 * - HasRemoteInformation
 * - SSRCs
 */



#include "call-content.h"

#include <telepathy-glib/telepathy-glib.h>
#include <telepathy-glib/proxy-subclass.h>
#include <farstream/fs-conference.h>
#include <farstream/fs-utils.h>
#include <farstream/fs-rtp.h>
#include <farstream/fs-element-added-notifier.h>

#include <stdarg.h>
#include <string.h>


#include "call-stream.h"
#include "tf-signals-marshal.h"
#include "utils.h"

#define DTMF_TONE_VOLUME (8)

struct _TfCallContent {
  TfContent parent;

  TfCallChannel *call_channel;
  FsConference *fsconference;

  TpCallContent *proxy;

  FsSession *fssession;
  TpMediaStreamType media_type;

  TpProxy *current_media_description;
  guint current_md_contact_handle;
  GList *current_md_fscodecs;
  GList *current_md_rtp_hdrext;

  gboolean current_has_rtp_hdrext;
  gboolean current_has_rtcp_fb;
  gboolean has_rtp_hdrext;
  gboolean has_rtcp_fb;

  GList *last_sent_codecs;

  GHashTable *streams; /* NULL before getting the first streams */
  /* Streams for which we don't have a session yet*/
  GList *outstanding_streams;

  GMutex *mutex;

  gboolean remote_codecs_set;

  TpSendingState dtmf_sending_state;
  guint current_dtmf_event;

  /* Content protected by the Mutex */
  GPtrArray *fsstreams;
  guint fsstreams_cookie;

  gboolean got_media_description_property;

  /* VideoControl API */
  FsElementAddedNotifier *notifier;

  volatile gint bitrate;
  volatile gint mtu;
  gboolean manual_keyframes;

  guint framerate;
  guint width;
  guint height;
};

struct _TfCallContentClass {
  TfContentClass parent_class;
};


static void call_content_async_initable_init (GAsyncInitableIface *asynciface);

G_DEFINE_TYPE_WITH_CODE (TfCallContent, tf_call_content, TF_TYPE_CONTENT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_INITABLE,
                             call_content_async_initable_init))

#define TF_CALL_CONTENT_LOCK(self)   g_mutex_lock ((self)->mutex)
#define TF_CALL_CONTENT_UNLOCK(self) g_mutex_unlock ((self)->mutex)


enum
{
  PROP_TF_CHANNEL = 1,
  PROP_FS_CONFERENCE,
  PROP_FS_SESSION,
  PROP_SINK_PAD,
  PROP_MEDIA_TYPE,
  PROP_OBJECT_PATH,
  PROP_FRAMERATE,
  PROP_WIDTH,
  PROP_HEIGHT
};

enum
{
  RESOLUTION_CHANGED = 0,
  SIGNAL_COUNT
};

static guint signals[SIGNAL_COUNT] = {0};


struct CallFsStream {
  TfCallChannel *parent_channel;
  guint use_count;
  guint contact_handle;
  FsParticipant *fsparticipant;
  FsStream *fsstream;
};

static void
tf_call_content_get_property (GObject    *object,
    guint       property_id,
    GValue     *value,
    GParamSpec *pspec);

static void tf_call_content_dispose (GObject *object);
static void tf_call_content_finalize (GObject *object);

static void tf_call_content_init_async (GAsyncInitable *initable,
    int io_priority,
    GCancellable  *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data);
static gboolean tf_call_content_init_finish (GAsyncInitable *initable,
    GAsyncResult *res,
    GError **error);


static void tf_call_content_try_sending_codecs (TfCallContent *self);
static FsStream * tf_call_content_get_existing_fsstream_by_handle (
    TfCallContent *content, guint contact_handle);

static void src_pad_added (FsStream *fsstream, GstPad *pad, FsCodec *codec,
    TfCallContent *content);
static GstIterator * tf_call_content_iterate_src_pads (TfContent *content,
    guint *handles, guint handle_count);

static void tf_call_content_error (TfContent *content,
    TpCallStateChangeReason reason,
    const gchar *detailed_reason,
    const gchar *message);

static void
tf_call_content_class_init (TfCallContentClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  TfContentClass *content_class = TF_CONTENT_CLASS (klass);

  content_class->iterate_src_pads = tf_call_content_iterate_src_pads;

  content_class->content_error = tf_call_content_error;

  object_class->dispose = tf_call_content_dispose;
  object_class->finalize = tf_call_content_finalize;
  object_class->get_property = tf_call_content_get_property;

  g_object_class_override_property (object_class, PROP_TF_CHANNEL,
      "tf-channel");
  g_object_class_override_property (object_class, PROP_FS_CONFERENCE,
      "fs-conference");
  g_object_class_override_property (object_class, PROP_FS_SESSION,
      "fs-session");
  g_object_class_override_property (object_class, PROP_SINK_PAD,
      "sink-pad");
  g_object_class_override_property (object_class, PROP_MEDIA_TYPE,
      "media-type");
  g_object_class_override_property (object_class, PROP_OBJECT_PATH,
      "object-path");

  g_object_class_install_property (object_class, PROP_FRAMERATE,
    g_param_spec_uint ("framerate",
      "Framerate",
      "The framerate as indicated by the VideoControl interface"
      "or the media layer",
      0, G_MAXUINT, 0,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_WIDTH,
    g_param_spec_uint ("width",
      "Width",
      "The video width indicated by the VideoControl interface"
      "or the media layer",
      0, G_MAXUINT, 0,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_HEIGHT,
    g_param_spec_uint ("height",
      "Height",
      "The video height as indicated by the VideoControl interface"
      "or the media layer",
      0, G_MAXUINT, 0,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  signals[RESOLUTION_CHANGED] = g_signal_new ("resolution-changed",
      G_OBJECT_CLASS_TYPE (klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL,
      _tf_marshal_VOID__UINT_UINT,
      G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_UINT);
}

static void
call_content_async_initable_init (GAsyncInitableIface *asynciface)
{
  asynciface->init_async = tf_call_content_init_async;
  asynciface->init_finish = tf_call_content_init_finish;
}


static void
free_content_fsstream (gpointer data)
{
  struct CallFsStream *cfs = data;

  g_object_run_dispose (G_OBJECT (cfs->fsstream));
  g_object_unref (cfs->fsstream);
  _tf_call_channel_put_participant (cfs->parent_channel, cfs->fsparticipant);
  g_slice_free (struct CallFsStream, cfs);
}

static void
tf_call_content_init (TfCallContent *self)
{
  self->fsstreams = g_ptr_array_new ();
  self->dtmf_sending_state = TP_SENDING_STATE_NONE;

  self->mutex = g_mutex_new ();
}

static void
tf_call_content_dispose (GObject *object)
{
  TfCallContent *self = TF_CALL_CONTENT (object);

  g_debug (G_STRFUNC);

  if (self->streams)
    g_hash_table_destroy (self->streams);
  self->streams = NULL;

  if (self->fssession)
    {
      g_object_run_dispose (G_OBJECT (self->fssession));
      g_object_unref (self->fssession);
    }
  self->fssession = NULL;

  if (self->fsstreams)
    {
      while (self->fsstreams->len)
        free_content_fsstream (
            g_ptr_array_remove_index_fast (self->fsstreams, 0));
      g_ptr_array_unref (self->fsstreams);
  }
  self->fsstreams = NULL;

  if (self->notifier)
    g_object_unref (self->notifier);
  self->notifier = NULL;

  if (self->fsconference)
    _tf_call_channel_put_conference (self->call_channel,
        self->fsconference);
  self->fsconference = NULL;

  if (self->proxy)
    g_object_unref (self->proxy);
  self->proxy = NULL;

  fs_codec_list_destroy (self->last_sent_codecs);
  self->last_sent_codecs = NULL;

  /* We do not hold a ref to the call channel, and use it as a flag to ensure
   * we will bail out when disposed */
  self->call_channel = NULL;

  if (G_OBJECT_CLASS (tf_call_content_parent_class)->dispose)
    G_OBJECT_CLASS (tf_call_content_parent_class)->dispose (object);
}


static void
tf_call_content_finalize (GObject *object)
{
  TfCallContent *self = TF_CALL_CONTENT (object);

  g_mutex_free (self->mutex);

  if (G_OBJECT_CLASS (tf_call_content_parent_class)->finalize)
    G_OBJECT_CLASS (tf_call_content_parent_class)->finalize (object);
}


static void
tf_call_content_get_property (GObject    *object,
    guint       property_id,
    GValue     *value,
    GParamSpec *pspec)
{
  TfCallContent *self = TF_CALL_CONTENT (object);

  switch (property_id)
    {
    case PROP_TF_CHANNEL:
      if (self->call_channel)
        g_value_set_object (value, self->call_channel);
      break;
    case PROP_FS_CONFERENCE:
      if (self->fsconference)
        g_value_set_object (value, self->fsconference);
      break;
    case PROP_FS_SESSION:
      if (self->fssession)
        g_value_set_object (value, self->fssession);
      break;
    case PROP_SINK_PAD:
      if (self->fssession)
        g_object_get_property (G_OBJECT (self->fssession), "sink-pad", value);
      break;
    case PROP_MEDIA_TYPE:
      g_value_set_enum (value, tf_call_content_get_fs_media_type (self));
      break;
    case PROP_OBJECT_PATH:
      g_object_get_property (G_OBJECT (self->proxy), "object-path", value);
      break;
    case PROP_FRAMERATE:
      g_value_set_uint (value, self->framerate);
      break;
    case PROP_WIDTH:
      g_value_set_uint (value, self->width);
      break;
    case PROP_HEIGHT:
      g_value_set_uint (value, self->height);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
create_stream (TfCallContent *self, gchar *stream_path)
{
  GError *error = NULL;
  TfCallStream *stream = tf_call_stream_new (self->call_channel, self,
      stream_path, &error);

  if (error)
    {
      /* TODO: Use per-stream errors */
      tf_content_error (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
          TP_ERROR_STR_MEDIA_STREAMING_ERROR,
          "Error creating the stream object: %s", error->message);
      return;
    }

  g_hash_table_insert (self->streams, stream_path, stream);
}

static void
add_stream (TfCallContent *self, const gchar *stream_path)
{

  if (!self->fsconference)
  {
    self->outstanding_streams = g_list_prepend (self->outstanding_streams,
      g_strdup (stream_path));
  } else {
    create_stream (self, g_strdup (stream_path));
  }
}

static void
update_streams (TfCallContent *self)
{
  GList *l;

  g_assert (self->fsconference);

  for (l = self->outstanding_streams ; l != NULL; l = l->next)
    create_stream (self, l->data);

  g_list_free (self->outstanding_streams);
  self->outstanding_streams = NULL;
}

static void
tpparam_to_fsparam (gpointer key, gpointer value, gpointer user_data)
{
  gchar *name = key;
  gchar *val = value;
  FsCodec *fscodec = user_data;

  fs_codec_add_optional_parameter (fscodec, name, val);
}

static GList *
tpcodecs_to_fscodecs (FsMediaType fsmediatype, const GPtrArray *tpcodecs,
    gboolean does_avpf, GHashTable *rtcp_fb)
{
  GList *fscodecs = NULL;
  guint i;

  for (i = 0; i < tpcodecs->len; i++)
    {
      GValueArray *tpcodec = g_ptr_array_index (tpcodecs, i);
      guint pt;
      gchar *name;
      guint clock_rate;
      guint channels;
      GHashTable *params;
      FsCodec *fscodec;
      gchar *tmp;
      GValueArray *feedback_params;

      tp_value_array_unpack (tpcodec, 5, &pt, &name, &clock_rate, &channels,
          &params);

      fscodec = fs_codec_new (pt, name, fsmediatype, clock_rate);
      fscodec->channels = channels;

      g_hash_table_foreach (params, tpparam_to_fsparam, fscodec);

      if (does_avpf)
        fscodec->minimum_reporting_interval = 0;

      feedback_params = g_hash_table_lookup (rtcp_fb, GUINT_TO_POINTER (pt));

      if (feedback_params)
        {
          guint rtcp_minimum_interval;
          GPtrArray *messages;
          guint j;

          tp_value_array_unpack (feedback_params, 2, &rtcp_minimum_interval,
              &messages);
          if (rtcp_minimum_interval != G_MAXUINT)
            fscodec->minimum_reporting_interval = rtcp_minimum_interval;

          for (j = 0; j < messages->len ; j++)
            {
              GValueArray *message = g_ptr_array_index (messages, j);
              const gchar *type, *subtype, *extra_params;

              tp_value_array_unpack (message, 3, &type, &subtype,
                  &extra_params);

              fs_codec_add_feedback_parameter (fscodec, type, subtype,
                  extra_params);
            }
        }

      tmp = fs_codec_to_string (fscodec);
      g_debug ("%s", tmp);
      g_free (tmp);
      fscodecs = g_list_prepend (fscodecs, fscodec);
    }

  fscodecs = g_list_reverse (fscodecs);

  return fscodecs;
}

static GList *
tprtphdrext_to_fsrtphdrext (GPtrArray *rtp_hdrext)
{
  GQueue ret = G_QUEUE_INIT;
  guint i;

  if (!rtp_hdrext)
    return NULL;

  for (i = 0; i < rtp_hdrext->len; i++)
    {
      GValueArray *extension = g_ptr_array_index (rtp_hdrext, i);
      guint id;
      TpMediaStreamDirection direction;
      const char *uri;
      const gchar *parameters;

      tp_value_array_unpack (extension, 4, &id, &direction, &uri, &parameters);

      g_queue_push_tail (&ret, fs_rtp_header_extension_new (id,
              tpdirection_to_fsdirection (direction), uri));
    }

  return ret.head;
}

static gboolean
object_has_property (GObject *object, const gchar *property)
{
  return g_object_class_find_property (G_OBJECT_GET_CLASS (object),
      property) != NULL;
}

static void
on_content_dtmf_change_requested (TpProxy *proxy,
    guchar arg_Event,
    guint arg_State,
    gpointer user_data,
    GObject *weak_object)
{
  TfCallContent *self = TF_CALL_CONTENT (weak_object);

  /* Ignore the signal until we've got the original properties and codecs */
  if (!self->fssession || !self->remote_codecs_set) {
    self->dtmf_sending_state = arg_State;
    self->current_dtmf_event = arg_Event;
    return;
  }

  switch (arg_State)
    {
    case TP_SENDING_STATE_PENDING_STOP_SENDING:
      if (self->dtmf_sending_state != TP_SENDING_STATE_SENDING)
        {
          tf_content_error (TF_CONTENT (self),
              TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
              TP_ERROR_STR_CONFUSED,
              "Tried to stop a %u DTMF event while state is %d",
              arg_Event, self->dtmf_sending_state);
        }

      if (fs_session_stop_telephony_event (self->fssession))
        {
          self->dtmf_sending_state = TP_SENDING_STATE_PENDING_STOP_SENDING;
        }
      else
        {
          tf_content_error (TF_CONTENT (self),
              TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
              TP_ERROR_STR_MEDIA_STREAMING_ERROR,
              "Could not stop DTMF event %d", arg_Event);
          tp_cli_call_content_interface_media_call_acknowledge_dtmf_change (
              self->proxy, -1, arg_Event, TP_SENDING_STATE_SENDING,
              NULL, NULL, NULL, NULL);
        }
      break;
    case TP_SENDING_STATE_PENDING_SEND:
      if (self->dtmf_sending_state != TP_SENDING_STATE_NONE)
        {
          tf_content_error (TF_CONTENT (self),
              TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
              TP_ERROR_STR_CONFUSED,
              "Tried to start a new DTMF event %u while %d is already playing",
              arg_Event, self->current_dtmf_event);
          fs_session_stop_telephony_event (self->fssession);
        }

      if (fs_session_start_telephony_event (self->fssession,
              arg_Event, DTMF_TONE_VOLUME))
        {
          self->current_dtmf_event = arg_Event;
          self->dtmf_sending_state = TP_SENDING_STATE_PENDING_SEND;
        }
      else
        {
          tf_content_error (TF_CONTENT (self),
              TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
              TP_ERROR_STR_MEDIA_STREAMING_ERROR,
              "Could not start DTMF event %d", arg_Event);
          tp_cli_call_content_interface_media_call_acknowledge_dtmf_change (
              self->proxy, -1, arg_Event, TP_SENDING_STATE_NONE,
              NULL, NULL, NULL, NULL);
        }
      break;
    default:
      tf_content_error (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
          TP_ERROR_STR_CONFUSED,
          "Invalid State %d in DTMFChangeRequested signal for event %d",
          arg_State, arg_Event);
      break;
    }
}


static void
process_media_description_try_codecs (TfCallContent *self, FsStream *fsstream,
    TpProxy *media_description, GList *fscodecs, GList *rtp_hdrext)
{
  gboolean success = TRUE;
  GError *error = NULL;

  if (fscodecs != NULL)
    {
      GList *old_rtp_hdrext = NULL;

      if (object_has_property (G_OBJECT (fsstream), "rtp-header-extensions"))
        {
          g_object_get (fsstream, "rtp-header-extensions", &old_rtp_hdrext,
              NULL);
          g_object_set (fsstream, "rtp-header-extensions", rtp_hdrext, NULL);
        }

      success = fs_stream_set_remote_codecs (fsstream, fscodecs, &error);

      if (success)
        {
          if (!self->remote_codecs_set)
            on_content_dtmf_change_requested (NULL, self->current_dtmf_event,
                self->dtmf_sending_state, NULL, G_OBJECT (self));
          self->remote_codecs_set = TRUE;
        }

      if (!success &&
          object_has_property (G_OBJECT (fsstream), "rtp-header-extensions"))
        g_object_set (fsstream, "rtp-header-extensions", old_rtp_hdrext, NULL);

      fs_rtp_header_extension_list_destroy (old_rtp_hdrext);
    }

  fs_rtp_header_extension_list_destroy (rtp_hdrext);
  fs_codec_list_destroy (fscodecs);

  if (success)
    {
      self->current_media_description = media_description;
      tf_call_content_try_sending_codecs (self);
    }
  else
    {
      tp_cli_call_content_media_description_call_reject (media_description,
          -1, NULL, NULL, NULL, NULL);
      g_object_unref (media_description);
    }
}

static void
process_media_description (TfCallContent *self,
    const gchar *media_description_objpath,
    guint contact_handle, const GHashTable *properties)
{
  TpProxy *proxy;
  GError *error = NULL;
  FsStream *fsstream;
  GPtrArray *codecs;
  GPtrArray *rtp_hdrext = NULL;
  GHashTable *rtcp_fb = NULL;
  gboolean does_avpf = FALSE;
  GList *fscodecs;
  const gchar * const *interfaces;
  guint i;
  GList *fsrtp_hdrext;

  /* Guard against early disposal */
  if (self->call_channel == NULL)
      return;

  if (!tp_dbus_check_valid_object_path (media_description_objpath, &error))
    {
      tf_content_error (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR, TP_ERROR_STR_CONFUSED,
          "Invalid MediaDescription path: %s", error->message);
      g_clear_error (&error);
      return;
    }

  codecs = tp_asv_get_boxed (properties,
      TP_PROP_CALL_CONTENT_MEDIA_DESCRIPTION_CODECS, TP_ARRAY_TYPE_CODEC_LIST);

  if (!codecs)
    {
      tf_content_error_literal (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR, TP_ERROR_STR_CONFUSED,
          "MediaDescription does not contain codecs");
      g_clear_error (&error);
      return;
    }

  proxy = g_object_new (TP_TYPE_PROXY,
      "dbus-daemon", tp_proxy_get_dbus_daemon (self->proxy),
      "bus-name", tp_proxy_get_bus_name (self->proxy),
      "object-path", media_description_objpath,
      NULL);
  tp_proxy_add_interface_by_id (TP_PROXY (proxy),
      TP_IFACE_QUARK_CALL_CONTENT_MEDIA_DESCRIPTION);

  interfaces = tp_asv_get_strv (properties,
      TP_PROP_CALL_CONTENT_MEDIA_DESCRIPTION_INTERFACES);


  self->current_has_rtcp_fb = FALSE;
  self->current_has_rtp_hdrext = FALSE;
  for (i = 0; interfaces[i]; i++)
    {
      if (!strcmp (interfaces[i],
              TP_IFACE_CALL_CONTENT_MEDIA_DESCRIPTION_INTERFACE_RTCP_FEEDBACK))
        {
          gboolean valid;

          self->current_has_rtcp_fb = TRUE;
          rtcp_fb = tp_asv_get_boxed (properties,
              TP_PROP_CALL_CONTENT_MEDIA_DESCRIPTION_INTERFACE_RTCP_FEEDBACK_FEEDBACK_MESSAGES,
              TP_HASH_TYPE_RTCP_FEEDBACK_MESSAGE_MAP);
          does_avpf = tp_asv_get_boolean (properties,
              TP_PROP_CALL_CONTENT_MEDIA_DESCRIPTION_INTERFACE_RTCP_FEEDBACK_DOES_AVPF, &valid);
          if (!valid)
            does_avpf = FALSE;
        }
      else if (!strcmp (interfaces[i],
              TP_IFACE_CALL_CONTENT_MEDIA_DESCRIPTION_INTERFACE_RTP_HEADER_EXTENSIONS))
        {
          self->current_has_rtp_hdrext = TRUE;
          rtp_hdrext = tp_asv_get_boxed (properties,
              TP_PROP_CALL_CONTENT_MEDIA_DESCRIPTION_INTERFACE_RTP_HEADER_EXTENSIONS_HEADER_EXTENSIONS,
              TP_ARRAY_TYPE_RTP_HEADER_EXTENSIONS_LIST);
        }
    }


  g_debug ("Got MediaDescription");
  fscodecs = tpcodecs_to_fscodecs (tp_media_type_to_fs (self->media_type),
      codecs, does_avpf, rtcp_fb);

  fsrtp_hdrext = tprtphdrext_to_fsrtphdrext (rtp_hdrext);

  fsstream = tf_call_content_get_existing_fsstream_by_handle (self,
      contact_handle);

  if (!fsstream)
    {
      g_debug ("Delaying codec media_description processing");
      self->current_media_description = proxy;
      self->current_md_contact_handle = contact_handle;
      self->current_md_fscodecs = fscodecs;
      self->current_md_rtp_hdrext = fsrtp_hdrext;
      return;
    }

  process_media_description_try_codecs (self, fsstream, proxy, fscodecs,
      fsrtp_hdrext);
}

static void
on_content_video_keyframe_requested (TpProxy *proxy,
  gpointer user_data,
  GObject *weak_object)
{
  TfCallContent *self = TF_CALL_CONTENT (weak_object);
  GstPad *pad;

  /* Guard against early disposal */
  if (self->call_channel == NULL)
    return;

  /* In case there is no session, ignore the request a new session should start
   * with sending a KeyFrame in any case */
  if (self->fssession == NULL)
    return;

  g_object_get (self->fssession, "sink-pad", &pad, NULL);

  if (pad == NULL)
    {
      g_warning ("Failed to get a pad for the keyframe request");
      return;
    }

  g_message ("Sending out a keyframe request");
  gst_pad_send_event (pad,
      gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM,
          gst_structure_new ("GstForceKeyUnit",
            "all-headers", G_TYPE_BOOLEAN, TRUE,
            NULL)));

  g_object_unref (pad);
}

static void
on_content_video_resolution_changed (TpProxy *proxy,
  const GValueArray *resolution,
  gpointer user_data,
  GObject *weak_object)
{
  TfCallContent *self = TF_CALL_CONTENT (weak_object);
  guint width, height;

  /* Guard against early disposal */
  if (self->call_channel == NULL)
    return;

  tp_value_array_unpack ((GValueArray *)resolution, 2,
    &width, &height, NULL);

  /* Can be 0 in the initial property dump, shouldn't be at any other time */
  if (width == 0 || height == 0)
    return;

  self->width = width;
  self->height = height;

  g_signal_emit (self, signals[RESOLUTION_CHANGED], 0, width, height);
  g_signal_emit_by_name (self, "restart-source");

  g_message ("requested video resolution: %dx%d", width, height);
}

static void
on_content_video_bitrate_changed (TpProxy *proxy,
  guint bitrate,
  gpointer user_data,
  GObject *weak_object)
{
  TfCallContent *self = TF_CALL_CONTENT (weak_object);

  /* Guard against early disposal */
  if (self->call_channel == NULL)
    return;

  g_message ("Setting bitrate to %d bits/s", bitrate);
  self->bitrate = bitrate;

  if (self->fssession != NULL && self->bitrate > 0)
    g_object_set (self->fssession, "send-bitrate", self->bitrate, NULL);
}

static void
on_content_video_framerate_changed (TpProxy *proxy,
  guint framerate,
  gpointer user_data,
  GObject *weak_object)
{
  TfCallContent *self = TF_CALL_CONTENT (weak_object);

  /* Guard against early disposal */
  if (self->call_channel == NULL)
    return;

  g_message ("updated framerate requested: %d", framerate);

  self->framerate = framerate;
  g_object_notify (G_OBJECT (self), "framerate");
  g_signal_emit_by_name (self, "restart-source");
}

static void
on_content_video_mtu_changed (TpProxy *proxy,
  guint mtu,
  gpointer user_data,
  GObject *weak_object)
{
  TfCallContent *self = TF_CALL_CONTENT (weak_object);

  /* Guard against early disposal */
  if (self->call_channel == NULL)
    return;

  g_atomic_int_set (&self->mtu, mtu);

  if (self->fsconference != NULL)
    {
      fs_element_added_notifier_remove (self->notifier,
          GST_BIN (self->fsconference));

      if (mtu > 0 || self->manual_keyframes)
        fs_element_added_notifier_add (self->notifier,
          GST_BIN (self->fsconference));
    }
}


static void
got_content_media_properties (TpProxy *proxy, GHashTable *properties,
    const GError *error, gpointer user_data, GObject *weak_object)
{
  TfCallContent *self = TF_CALL_CONTENT (weak_object);
  GSimpleAsyncResult *res = user_data;
  GValueArray *gva;
  const gchar *media_description_objpath = NULL;
  GHashTable *media_description_properties;
  guint contact;
  GError *myerror = NULL;
  guint32 packetization;
  const gchar *conference_type;
  gboolean valid;
  GList *codec_prefs;
  guchar dtmf_event;
  guint dtmf_state;
  const GValue *dtmf_event_value;

  /* Guard against early disposal */
  if (self->call_channel == NULL)
    {
      g_simple_async_result_set_error (res, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
          "Call content has been disposed of");
      g_simple_async_result_complete (res);
      g_object_unref (res);
      return;
    }

  if (error != NULL)
    {
      tf_content_error (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
          TP_ERROR_STR_MEDIA_STREAMING_ERROR,
          "Error getting the Content's media properties: %s", error->message);
      g_simple_async_result_set_from_error (res, error);
      g_simple_async_result_complete (res);
      g_object_unref (res);
      return;
    }

  packetization = tp_asv_get_uint32 (properties, "Packetization", &valid);

  if (!valid)
    goto invalid_property;

  g_assert (self->fssession == NULL);

  switch (packetization)
    {
      case TP_CALL_CONTENT_PACKETIZATION_TYPE_RTP:
        conference_type = "rtp";
        break;
      case TP_CALL_CONTENT_PACKETIZATION_TYPE_RAW:
        conference_type = "raw";
        break;
      default:
        tf_content_error (TF_CONTENT (self),
            TP_CALL_STATE_CHANGE_REASON_MEDIA_ERROR,
            TP_ERROR_STR_MEDIA_UNSUPPORTED_TYPE,
            "Could not create FsConference for type %d", packetization);
        g_simple_async_result_set_error (res, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
          "Could not create FsConference for type %d", packetization);
        g_simple_async_result_complete (res);
        g_object_unref (res);
        return;
    }

  self->fsconference = _tf_call_channel_get_conference (self->call_channel,
      conference_type);
  if (!self->fsconference)
    {
      tf_content_error (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_MEDIA_ERROR,
          TP_ERROR_STR_MEDIA_UNSUPPORTED_TYPE,
          "Could not create FsConference for type %s", conference_type);
      g_simple_async_result_set_error (res, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
          "Error getting the Content's properties: invalid type");
      g_simple_async_result_complete (res);
      g_object_unref (res);
      return;
    }

  self->fssession = fs_conference_new_session (self->fsconference,
      tp_media_type_to_fs (self->media_type), &myerror);

  if (!self->fssession)
    {
      tf_content_error (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_MEDIA_ERROR,
          TP_ERROR_STR_MEDIA_UNSUPPORTED_TYPE,
          "Could not create FsSession: %s", myerror->message);
      g_simple_async_result_set_from_error (res, myerror);
      g_simple_async_result_complete (res);
      g_clear_error (&myerror);
      g_object_unref (res);
      return;
    }

  if (self->notifier != NULL)
    fs_element_added_notifier_add (self->notifier,
      GST_BIN (self->fsconference));

  /* Now process outstanding streams */
  update_streams (self);

  gva = tp_asv_get_boxed (properties, "MediaDescriptionOffer",
      TP_STRUCT_TYPE_MEDIA_DESCRIPTION_OFFER);
  if (gva == NULL)
    {
      goto invalid_property;
    }

  codec_prefs = fs_utils_get_default_codec_preferences (
      GST_ELEMENT (self->fsconference));

  if (codec_prefs)
    {
      if (!fs_session_set_codec_preferences (self->fssession, codec_prefs,
              &myerror))
        {
          g_warning ("Could not set codec preference: %s", myerror->message);
          g_clear_error (&myerror);
        }
    }

  /* First complete so we get signalled and the preferences can be set, then
   * start looking at the media_description. We only unref the result later, to avoid
   * self possibly being disposed early */
  g_simple_async_result_set_op_res_gboolean (res, TRUE);
  g_simple_async_result_complete (res);

  tp_value_array_unpack (gva, 3, &media_description_objpath, &contact,
      &media_description_properties);

  if (strcmp (media_description_objpath, "/"))
    {
      process_media_description (self, media_description_objpath, contact,
          media_description_properties);
    }
  self->got_media_description_property = TRUE;


  dtmf_state = tp_asv_get_uint32 (properties, "CurrentDTMFState", &valid);
  if (!valid)
    goto invalid_property;

  dtmf_event_value = tp_asv_lookup (properties, "CurrentDTMFEvent");
  if (!dtmf_event_value || !G_VALUE_HOLDS_UCHAR (dtmf_event_value))
    goto invalid_property;
  dtmf_event = g_value_get_uchar (dtmf_event_value);

  on_content_dtmf_change_requested (NULL, dtmf_event, dtmf_state, NULL,
      G_OBJECT (self));

  /* The async result holds a ref to self which may be the last one, so this
   * comes after we're done with self */
  g_object_unref (res);
  return;

 invalid_property:
  tf_content_error_literal (TF_CONTENT (self),
      TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
      TP_ERROR_STR_CONFUSED,
      "Error getting the Content's properties: invalid type");
  g_simple_async_result_set_error (res, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
      "Error getting the Content's properties: invalid type");
  g_simple_async_result_complete (res);
  g_object_unref (res);
  return;
}

static void
setup_content_media_properties (TfCallContent *self,
  TpProxy *proxy,
  GSimpleAsyncResult *res)
{
  GError *error = NULL;


  if (tp_cli_call_content_interface_media_connect_to_dtmf_change_requested (
          TP_CALL_CONTENT (proxy),
          on_content_dtmf_change_requested,
          NULL, NULL, G_OBJECT (self), &error) == NULL)
    goto connect_failed;

  tp_cli_dbus_properties_call_get_all (proxy, -1,
      TP_IFACE_CALL_CONTENT_INTERFACE_MEDIA,
      got_content_media_properties, res, NULL, G_OBJECT (self));

  return;

 connect_failed:
  tf_content_error (TF_CONTENT (self),
      TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
      TP_ERROR_STR_CONFUSED,
      "Error getting the Content's VideoControl properties: %s",
      error->message);
  g_simple_async_result_take_error (res, error);
  g_simple_async_result_complete (res);
  g_object_unref (res);
}

static void
content_video_element_added (FsElementAddedNotifier *notifier,
  GstBin *conference,
  GstElement *element,
  TfCallContent *self)
{
  gint mtu = g_atomic_int_get (&self->mtu);

  if (G_UNLIKELY (mtu == 0 && !self->manual_keyframes))
    return;

  if (G_UNLIKELY (mtu > 0 && object_has_property (G_OBJECT (element), "mtu")))
    {
      g_message ("Setting %d as mtu on payloader", mtu);
      g_object_set (element, "mtu", mtu, NULL);
    }

  if (G_UNLIKELY (self->manual_keyframes))
    {
      if (object_has_property (G_OBJECT (element), "key-int-max"))
        {
          g_message ("Setting key-int-max to max uint");
          g_object_set (element, "key-int-max", G_MAXINT, NULL);
        }

      if (object_has_property (G_OBJECT (element), "intra-period"))
        {
          g_message ("Setting intra-period to 0");
          g_object_set (element, "intra-period", 0, NULL);
        }
    }
}

static void
got_content_video_control_properties (TpProxy *proxy, GHashTable *properties,
    const GError *error, gpointer user_data, GObject *weak_object)
{
  TfCallContent *self = TF_CALL_CONTENT (weak_object);
  GSimpleAsyncResult *res = user_data;
  GValueArray *array;
  guint32 bitrate, mtu;
  gboolean valid;
  gboolean manual_keyframes;

  if (error)
    {
      tf_content_error (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
          TP_ERROR_STR_CONFUSED,
          "Error getting the Content's VideoControl properties: %s",
          error->message);
      g_simple_async_result_set_from_error (res, error);
      goto error;
    }

  /* Guard against early disposal */
  if (self->call_channel == NULL)
    {
      g_simple_async_result_set_error (res, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
          "Call content has been disposed of");
      goto error;
    }

  if (properties == NULL)
    {
      tf_content_error_literal (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
          TP_ERROR_STR_CONFUSED,
          "Error getting the Content's VideoControl properties: "
          "there are none");
      g_simple_async_result_set_error (res, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
          "Error getting the VideoControl Content's properties: "
          "there are none");
      goto error;
    }


  /* Only get the various variables, we will not have an FsSession untill the
   * media properties are retrieved so no need to act just yet */
  bitrate = tp_asv_get_uint32 (properties, "Bitrate", &valid);
  if (valid)
    self->bitrate = bitrate;

  mtu = tp_asv_get_uint32 (properties, "MTU", &valid);
  if (valid)
    self->mtu = mtu;

  manual_keyframes = tp_asv_get_boolean (properties,
    "ManualKeyFrames", &valid);
  if (valid)
    self->manual_keyframes = manual_keyframes;

  array = tp_asv_get_boxed (properties, "VideoResolution",
      TP_STRUCT_TYPE_VIDEO_RESOLUTION);
  if (array)
    on_content_video_resolution_changed (proxy, array,
        NULL, G_OBJECT (self));

  self->notifier = fs_element_added_notifier_new ();
  g_signal_connect (self->notifier, "element-added",
    G_CALLBACK (content_video_element_added), self);

  setup_content_media_properties (self, proxy, res);
  return;

error:
  g_simple_async_result_complete (res);
  g_object_unref (res);
  return;
}


static void
setup_content_video_control (TfCallContent *self,
  TpProxy *proxy,
  GSimpleAsyncResult *res)
{
  GError *error = NULL;

  tp_proxy_add_interface_by_id (proxy,
    TP_IFACE_QUARK_CALL_CONTENT_INTERFACE_VIDEO_CONTROL);

  if (tp_cli_call_content_interface_video_control_connect_to_key_frame_requested (
      TP_CALL_CONTENT (proxy),
      on_content_video_keyframe_requested,
      NULL, NULL, G_OBJECT (self), &error) == NULL)
    goto connect_failed;

  if (tp_cli_call_content_interface_video_control_connect_to_video_resolution_changed (
      TP_CALL_CONTENT (proxy),
      on_content_video_resolution_changed,
      NULL, NULL, G_OBJECT (self), &error) == NULL)
    goto connect_failed;

  if (tp_cli_call_content_interface_video_control_connect_to_bitrate_changed (
      TP_CALL_CONTENT (proxy),
      on_content_video_bitrate_changed,
      NULL, NULL, G_OBJECT (self), NULL) == NULL)
    goto connect_failed;

  if (tp_cli_call_content_interface_video_control_connect_to_framerate_changed (
      TP_CALL_CONTENT (proxy),
      on_content_video_framerate_changed,
      NULL, NULL, G_OBJECT (self), NULL) == NULL)
    goto connect_failed;

  if (tp_cli_call_content_interface_video_control_connect_to_mtu_changed (
      TP_CALL_CONTENT (proxy),
      on_content_video_mtu_changed,
      NULL, NULL, G_OBJECT (self), NULL) == NULL)
    goto connect_failed;

  tp_cli_dbus_properties_call_get_all (proxy, -1,
      TP_IFACE_CALL_CONTENT_INTERFACE_VIDEO_CONTROL,
      got_content_video_control_properties, res, NULL, G_OBJECT (self));

  return;

connect_failed:
  tf_content_error (TF_CONTENT (self),
      TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
      TP_ERROR_STR_CONFUSED,
      "Error getting the Content's VideoControl properties: %s",
      error->message);
  g_simple_async_result_take_error (res, error);
  g_simple_async_result_complete (res);
  g_object_unref (res);
}

static void
new_media_description_offer (TpProxy *proxy,
    const gchar *arg_Media_Description,
    guint arg_Contact,
    GHashTable *arg_Properties,
    gpointer user_data,
    GObject *weak_object)
{
  TfCallContent *self = TF_CALL_CONTENT (weak_object);

  /* Guard against early disposal */
  if (self->call_channel == NULL)
    return;

  /* Ignore signals before we get the first codec MediaDescription property */
  if (!self->got_media_description_property)
    return;

  if (self->current_media_description) {
    g_object_unref (self->current_media_description);
    fs_codec_list_destroy (self->current_md_fscodecs);
    fs_rtp_header_extension_list_destroy (self->current_md_rtp_hdrext);
    self->current_media_description = NULL;
    self->current_md_fscodecs = NULL;
    self->current_md_rtp_hdrext = NULL;
  }

  process_media_description (self, arg_Media_Description, arg_Contact, arg_Properties);
}


static void
got_content_properties (TpProxy *proxy, GHashTable *out_Properties,
    const GError *error, gpointer user_data, GObject *weak_object)
{
  TfCallContent *self = TF_CALL_CONTENT (weak_object);
  GSimpleAsyncResult *res = user_data;
  gboolean valid;
  GPtrArray *streams;
  GError *myerror = NULL;
  guint i;
  const gchar * const *interfaces;
  gboolean got_media_interface = FALSE;
  gboolean got_video_control_interface = FALSE;

  if (error)
    {
      tf_content_error (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
          TP_ERROR_STR_CONFUSED,
          "Error getting the Content's properties: %s", error->message);
      g_simple_async_result_set_from_error (res, error);
      g_simple_async_result_complete (res);
      g_object_unref (res);
      return;
    }

  /* Guard against early disposal */
  if (self->call_channel == NULL)
    {
      g_simple_async_result_set_error (res, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
          "Call content has been disposed of");
      g_simple_async_result_complete (res);
      g_object_unref (res);
      return;
    }

  if (!out_Properties)
    {
      tf_content_error_literal (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
          TP_ERROR_STR_CONFUSED,
          "Error getting the Content's properties: there are none");
      g_simple_async_result_set_error (res, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
          "Error getting the Content's properties: there are none");
      g_simple_async_result_complete (res);
      g_object_unref (res);
      return;
    }

  interfaces = tp_asv_get_strv (out_Properties, "Interfaces");

  if (interfaces == NULL)
    {
      tf_content_error_literal (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
          TP_ERROR_STR_CONFUSED,
          "Content does not have the Interfaces property, "
          "but HardwareStreaming was NOT true");
      g_simple_async_result_set_error (res, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
          "Content does not have the Interfaces property, "
          "but HardwareStreaming was NOT true");
      g_simple_async_result_complete (res);
      g_object_unref (res);
      return;
    }

  for (i = 0; interfaces[i]; i++)
    {
      if (!strcmp (interfaces[i],
            TP_IFACE_CALL_CONTENT_INTERFACE_MEDIA))
        got_media_interface = TRUE;

      if (!strcmp (interfaces[i],
            TP_IFACE_CALL_CONTENT_INTERFACE_VIDEO_CONTROL))
        got_video_control_interface = TRUE;
    }

  if (!got_media_interface)
    {
      tf_content_error_literal (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
          TP_ERROR_STR_CONFUSED,
          "Content does not have the media interface,"
          " but HardwareStreaming was NOT true");
      g_simple_async_result_set_error (res, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
          "Content does not have the media interface,"
          " but HardwareStreaming was NOT true");
      g_simple_async_result_complete (res);
      g_object_unref (res);
      return;
    }

  self->media_type = tp_asv_get_uint32 (out_Properties, "Type", &valid);
  if (!valid)
    goto invalid_property;


  streams = tp_asv_get_boxed (out_Properties, "Streams",
      TP_ARRAY_TYPE_OBJECT_PATH_LIST);
  if (!streams)
    goto invalid_property;

  self->streams = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, g_object_unref);

  for (i = 0; i < streams->len; i++)
    add_stream (self, g_ptr_array_index (streams, i));

  tp_proxy_add_interface_by_id (TP_PROXY (self->proxy),
      TP_IFACE_QUARK_CALL_CONTENT_INTERFACE_MEDIA);


  tp_cli_call_content_interface_media_connect_to_new_media_description_offer (
      TP_CALL_CONTENT (proxy), new_media_description_offer, NULL, NULL,
      G_OBJECT (self), &myerror);

  if (myerror)
    {
      tf_content_error (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
          TP_ERROR_STR_CONFUSED,
          "Error connectiong to NewCodecMediaDescription signal: %s",
          myerror->message);
      g_simple_async_result_set_from_error (res, myerror);
      g_simple_async_result_complete (res);
      g_object_unref (res);
      g_clear_error (&myerror);
      return;
    }

  if (got_video_control_interface)
    setup_content_video_control (self, proxy, res);
  else
    setup_content_media_properties (self, proxy, res);

  return;

 invalid_property:
  tf_content_error_literal (TF_CONTENT (self),
      TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR, TP_ERROR_STR_CONFUSED,
      "Error getting the Content's properties: invalid type");
  g_simple_async_result_set_error (res, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
      "Error getting the Content's properties: invalid type");
  g_simple_async_result_complete (res);
  g_object_unref (res);
  return;
}

static void
streams_added (TpProxy *proxy,
    const GPtrArray *arg_Streams,
    gpointer user_data,
    GObject *weak_object)
{
  TfCallContent *self = TF_CALL_CONTENT (weak_object);
  guint i;

  /* Ignore signals before we got the "Contents" property to avoid races that
   * could cause the same content to be added twice
   */

  if (!self->streams)
    return;

  for (i = 0; i < arg_Streams->len; i++)
    add_stream (self, g_ptr_array_index (arg_Streams, i));
}

static void
streams_removed (TpProxy *proxy,
    const GPtrArray *arg_Streams,
    const GValueArray *arg_Reason,
    gpointer user_data,
    GObject *weak_object)
{
  TfCallContent *self = TF_CALL_CONTENT (weak_object);
  guint i;

  if (!self->streams)
    return;

  for (i = 0; i < arg_Streams->len; i++)
    g_hash_table_remove (self->streams, g_ptr_array_index (arg_Streams, i));
}


static void
tf_call_content_init_async (GAsyncInitable *initable,
    int io_priority,
    GCancellable  *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  TfCallContent *self = TF_CALL_CONTENT (initable);
  GError *myerror = NULL;
  GSimpleAsyncResult *res;

  if (cancellable != NULL)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (self), callback, user_data,
          G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
          "TfCallChannel initialisation does not support cancellation");
      return;
    }

  tp_cli_call_content_connect_to_streams_added (
      TP_CALL_CONTENT (self->proxy), streams_added, NULL, NULL,
      G_OBJECT (self), &myerror);
  if (myerror)
    {
      tf_content_error (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR, TP_ERROR_STR_CONFUSED,
          "Error connectiong to StreamAdded signal: %s", myerror->message);
      g_simple_async_report_gerror_in_idle (G_OBJECT (self), callback,
          user_data, myerror);
      return;
    }

  tp_cli_call_content_connect_to_streams_removed (
      TP_CALL_CONTENT (self->proxy), streams_removed, NULL, NULL,
      G_OBJECT (self), &myerror);
  if (myerror)
    {
      tf_content_error (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR, TP_ERROR_STR_CONFUSED,
          "Error connectiong to StreamRemoved signal: %s", myerror->message);
      g_simple_async_report_gerror_in_idle (G_OBJECT (self), callback,
          user_data, myerror);
      return;
    }


  res = g_simple_async_result_new (G_OBJECT (self), callback, user_data,
      tf_call_content_init_async);

  tp_cli_dbus_properties_call_get_all (self->proxy, -1,
      TP_IFACE_CALL_CONTENT, got_content_properties, res,
      NULL, G_OBJECT (self));
}

static gboolean
tf_call_content_init_finish (GAsyncInitable *initable,
    GAsyncResult *res,
    GError **error)
{
  GSimpleAsyncResult *simple_res;

  g_return_val_if_fail (g_simple_async_result_is_valid (res,
          G_OBJECT (initable), tf_call_content_init_async), FALSE);
  simple_res = G_SIMPLE_ASYNC_RESULT (res);

  if (g_simple_async_result_propagate_error (simple_res, error))
    return FALSE;

  return g_simple_async_result_get_op_res_gboolean (simple_res);
}

TfCallContent *
tf_call_content_new_async (TfCallChannel *call_channel,
    const gchar *object_path, GError **error,
    GAsyncReadyCallback callback, gpointer user_data)
{
  TfCallContent *self;
  TpCallContent *proxy = tp_call_content_new (
      call_channel->proxy, object_path, error);

  if (!proxy)
    return NULL;

  self = g_object_new (TF_TYPE_CALL_CONTENT, NULL);

  self->call_channel = call_channel;
  self->proxy = proxy;

  g_async_initable_init_async (G_ASYNC_INITABLE (self), 0, NULL,
      callback, user_data);

  return g_object_ref (self);
}

static gboolean
find_codec (GList *codecs, FsCodec *codec)
{
  GList *item;

  for (item = codecs; item ; item = item->next)
    if (fs_codec_are_equal (item->data, codec))
      return TRUE;

  return FALSE;
}


static GHashTable *
fscodecs_to_media_descriptions (TfCallContent *self, GList *codecs)
{
  GPtrArray *tpcodecs = g_ptr_array_new ();
  GList *item;
  GList *resend_codecs = NULL;
  GHashTable *retval;
  GPtrArray *rtp_hdrext = NULL;
  GHashTable *rtcp_fb = NULL;
  GPtrArray *interfaces;

  if (self->last_sent_codecs)
    resend_codecs = fs_session_codecs_need_resend (self->fssession,
        self->last_sent_codecs, codecs);

  if (!self->current_media_description && !resend_codecs)
    return NULL;

  if ((self->current_media_description && self->current_has_rtp_hdrext)
      || self->has_rtp_hdrext)
    rtp_hdrext = dbus_g_type_specialized_construct (
        TP_ARRAY_TYPE_RTP_HEADER_EXTENSIONS_LIST);

  if ((self->current_media_description && self->current_has_rtcp_fb)
      || self->has_rtcp_fb)
    rtcp_fb = dbus_g_type_specialized_construct (
        TP_HASH_TYPE_RTCP_FEEDBACK_MESSAGE_MAP);

  for (item = codecs; item; item = item->next)
    {
      FsCodec *fscodec = item->data;
      GValue tpcodec = { 0, };
      GHashTable *params;
      GList *param_item;
      gboolean updated;

      params = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

      for (param_item = fscodec->optional_params;
           param_item;
           param_item = param_item->next)
        {
          FsCodecParameter *param = (FsCodecParameter *) param_item->data;

          g_hash_table_insert (params, g_strdup (param->name),
                               g_strdup (param->value));
        }

      updated = find_codec (resend_codecs, fscodec);

      g_value_init (&tpcodec, TP_STRUCT_TYPE_CODEC);
      g_value_take_boxed (&tpcodec,
          dbus_g_type_specialized_construct (TP_STRUCT_TYPE_CODEC));

      dbus_g_type_struct_set (&tpcodec,
          0, fscodec->id,
          1, fscodec->encoding_name,
          2, fscodec->clock_rate,
          3, fscodec->channels,
          4, updated,
          5, params,
          G_MAXUINT);


      g_hash_table_destroy (params);

      g_ptr_array_add (tpcodecs, g_value_get_boxed (&tpcodec));

      if (fscodec->minimum_reporting_interval != G_MAXUINT ||
          fscodec->feedback_params)
        {
          GPtrArray *messages = g_ptr_array_new ();
          GList *item2;

          for (item2 = fscodec->feedback_params; item2; item2 = item2->next)
            {
              FsFeedbackParameter *fb = item2->data;

              g_ptr_array_add (messages, tp_value_array_build (3,
                      G_TYPE_STRING, fb->type,
                      G_TYPE_STRING, fb->subtype,
                      G_TYPE_STRING, fb->extra_params));
            }

          g_hash_table_insert (rtcp_fb, GUINT_TO_POINTER (fscodec->id),
              tp_value_array_build (2,
                  G_TYPE_UINT,
                  fscodec->minimum_reporting_interval != G_MAXUINT ?
                  fscodec->minimum_reporting_interval : 5000,
                  TP_ARRAY_TYPE_RTCP_FEEDBACK_MESSAGE_LIST,
                  messages));

          g_boxed_free (TP_ARRAY_TYPE_RTCP_FEEDBACK_MESSAGE_LIST, messages);
        }
    }

  fs_codec_list_destroy (resend_codecs);


  if (rtp_hdrext)
    {
      GList *fs_rtp_hdrexts;

      g_object_get (self->fssession, "rtp-header-extensions", &fs_rtp_hdrexts,
                    NULL);

      for (item = fs_rtp_hdrexts; item; item = item->next)
        {
          FsRtpHeaderExtension *hdrext = item->data;

          g_ptr_array_add (rtp_hdrext, tp_value_array_build (4,
                  G_TYPE_UINT, hdrext->id,
                  G_TYPE_UINT, fsdirection_to_tpdirection (hdrext->direction),
                  G_TYPE_STRING, hdrext->uri,
                  G_TYPE_STRING, "",
                  NULL));
        }

      fs_rtp_header_extension_list_destroy (fs_rtp_hdrexts);
    }

  retval = tp_asv_new (
      TP_PROP_CALL_CONTENT_MEDIA_DESCRIPTION_CODECS,
      TP_ARRAY_TYPE_CODEC_LIST, tpcodecs,
      TP_PROP_CALL_CONTENT_MEDIA_DESCRIPTION_FURTHER_NEGOTIATION_REQUIRED,
      G_TYPE_BOOLEAN, !!resend_codecs,
      NULL);

  interfaces = g_ptr_array_new ();

  if (rtp_hdrext)
    {
      tp_asv_take_boxed (retval, TP_PROP_CALL_CONTENT_MEDIA_DESCRIPTION_INTERFACE_RTP_HEADER_EXTENSIONS_HEADER_EXTENSIONS,
          TP_ARRAY_TYPE_RTP_HEADER_EXTENSIONS_LIST,
          rtp_hdrext);
      g_ptr_array_add (interfaces,
          g_strdup (TP_IFACE_CALL_CONTENT_MEDIA_DESCRIPTION_INTERFACE_RTP_HEADER_EXTENSIONS));
    }

  if (rtcp_fb)
    {
      tp_asv_set_boolean (retval, TP_PROP_CALL_CONTENT_MEDIA_DESCRIPTION_INTERFACE_RTCP_FEEDBACK_DOES_AVPF, g_hash_table_size (rtcp_fb));
      tp_asv_take_boxed (retval, TP_PROP_CALL_CONTENT_MEDIA_DESCRIPTION_INTERFACE_RTCP_FEEDBACK_FEEDBACK_MESSAGES,
          TP_HASH_TYPE_RTCP_FEEDBACK_MESSAGE_MAP, rtcp_fb);
      g_ptr_array_add (interfaces,
          g_strdup (TP_IFACE_CALL_CONTENT_MEDIA_DESCRIPTION_INTERFACE_RTP_HEADER_EXTENSIONS));
    }

  g_ptr_array_add (interfaces, NULL);
  tp_asv_take_boxed (retval, TP_PROP_CALL_CONTENT_MEDIA_DESCRIPTION_INTERFACES,
      G_TYPE_STRV, interfaces->pdata);
  g_ptr_array_free (interfaces, FALSE);

  return retval;
}

static void
tf_call_content_try_sending_codecs (TfCallContent *self)
{
  GList *codecs;
  GHashTable *media_description;
  const gchar *codecs_prop = NULL;

  if (self->current_md_fscodecs != NULL)
  {
    g_debug ("Ignoring updated codecs unprocessed media description"
        " outstanding");
    return;
  }

  g_debug ("updating local codecs");

  if (TF_CONTENT (self)->sending_count == 0)
    codecs_prop = "codecs-without-config";
  else
    codecs_prop = "codecs";

  g_object_get (self->fssession, codecs_prop, &codecs, NULL);

  if (!codecs)
    return;

  if (fs_codec_list_are_equal (codecs, self->last_sent_codecs))
    {
      fs_codec_list_destroy (codecs);
      return;
    }

  media_description = fscodecs_to_media_descriptions (self, codecs);

  if (self->current_media_description)
    {

      tp_cli_call_content_media_description_call_accept (
          self->current_media_description, -1, media_description,
          NULL, NULL, NULL, NULL);

      g_object_unref (self->current_media_description);
      self->current_media_description = NULL;
    }
  else
    {
      if (media_description)
        {
          tp_cli_call_content_interface_media_call_update_local_media_description (
              self->proxy, -1, 0, media_description, NULL, NULL, NULL, NULL);
        }
      else
        {
          fs_codec_list_destroy (codecs);
        }
    }

  if (media_description)
    {
      fs_codec_list_destroy (self->last_sent_codecs);
      self->last_sent_codecs = codecs;
      self->has_rtcp_fb = self->current_has_rtcp_fb;
      self->has_rtp_hdrext = self->current_has_rtp_hdrext;

      g_boxed_free (TP_HASH_TYPE_MEDIA_DESCRIPTION_PROPERTIES, media_description);
    }
}

static void
tf_call_content_dtmf_started (TfCallContent *self, FsDTMFMethod method,
    FsDTMFEvent event, guint8 volume)
{
  if (volume != DTMF_TONE_VOLUME)
    {
      tf_content_error (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
          TP_ERROR_STR_MEDIA_STREAMING_ERROR,
          "DTMF volume is %d, while we use %d", volume, DTMF_TONE_VOLUME);
      return;
    }

  if (self->dtmf_sending_state != TP_SENDING_STATE_PENDING_SEND)
    {
      tf_content_error (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
          TP_ERROR_STR_MEDIA_STREAMING_ERROR,
          "Farstream started a DTMFevent, but we were in the %d state",
          self->dtmf_sending_state);
      return;
    }

  if (self->current_dtmf_event != event)
    {
      tf_content_error (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
          TP_ERROR_STR_MEDIA_STREAMING_ERROR,
          "Farstream started the wrong dtmf event, got %d but "
          "expected %d", event, self->current_dtmf_event);
      return;
    }

  tp_cli_call_content_interface_media_call_acknowledge_dtmf_change (
      self->proxy, -1, event, TP_SENDING_STATE_SENDING,
      NULL, NULL, NULL, NULL);
  self->dtmf_sending_state = TP_SENDING_STATE_SENDING;
}

static void
tf_call_content_dtmf_stopped (TfCallContent *self, FsDTMFMethod method)
{
  if (self->dtmf_sending_state != TP_SENDING_STATE_PENDING_STOP_SENDING)
    {
      tf_content_error (TF_CONTENT (self),
          TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
          TP_ERROR_STR_MEDIA_STREAMING_ERROR,
          "Farstream stopped a DTMFevent, but we were in the %d state",
          self->dtmf_sending_state);
      return;
    }

  tp_cli_call_content_interface_media_call_acknowledge_dtmf_change (
      self->proxy, -1, self->current_dtmf_event, TP_SENDING_STATE_NONE,
      NULL, NULL, NULL, NULL);
  self->dtmf_sending_state = TP_SENDING_STATE_NONE;
}


gboolean
tf_call_content_bus_message (TfCallContent *content,
    GstMessage *message)
{
  const GstStructure *s;
  gboolean ret = FALSE;
  const gchar *debug;
  GHashTableIter iter;
  gpointer key, value;
  FsDTMFMethod method;
  FsDTMFEvent event;
  guint8 volume;
  FsCodec *codec;
  GList *secondary_codecs;


  /* Guard against early disposal */
  if (content->call_channel == NULL)
    return FALSE;

  if (!content->fssession)
    return FALSE;

  if (GST_MESSAGE_TYPE (message) != GST_MESSAGE_ELEMENT)
    return FALSE;

  s = gst_message_get_structure (message);

  if (gst_structure_has_name (s, "farstream-error"))
    {
      GObject *object;
      const GValue *value = NULL;

      value = gst_structure_get_value (s, "src-object");
      object = g_value_get_object (value);

      if (object == (GObject*) content->fssession)
        {
          const gchar *msg;
          FsError errorno;
          GEnumClass *enumclass;
          GEnumValue *enumvalue;

          value = gst_structure_get_value (s, "error-no");
          errorno = g_value_get_enum (value);
          msg = gst_structure_get_string (s, "error-msg");
          debug = gst_structure_get_string (s, "debug-msg");

          enumclass = g_type_class_ref (FS_TYPE_ERROR);
          enumvalue = g_enum_get_value (enumclass, errorno);
          g_warning ("error (%s (%d)): %s : %s",
              enumvalue->value_nick, errorno, msg, debug);
          g_type_class_unref (enumclass);

          tf_content_error_literal (TF_CONTENT (content),
              TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
              TP_ERROR_STR_MEDIA_STREAMING_ERROR, msg);

          ret = TRUE;
        }
    }
  else if (fs_session_parse_codecs_changed (message, content->fssession))
    {
      g_debug ("Codecs changed");

      tf_call_content_try_sending_codecs (content);

      ret = TRUE;
    }
  else if (fs_session_parse_telephony_event_started (message,
          content->fssession, &method, &event, &volume))
    {
      tf_call_content_dtmf_started (content, method, event, volume);

      ret = TRUE;
    }
  else if (fs_session_parse_telephony_event_stopped (message,
          content->fssession, &method))
    {
      tf_call_content_dtmf_stopped (content, method);

      ret = TRUE;
    }
  else if (fs_session_parse_send_codec_changed (message, content->fssession,
          &codec, &secondary_codecs))
    {
      gchar *tmp;
      guint i = 1;

      tmp = fs_codec_to_string (codec);
      g_debug ("Send codec changed: %s", tmp);
      g_free (tmp);

      while (secondary_codecs)
        {
          tmp = fs_codec_to_string (secondary_codecs->data);
          g_debug ("Secondary send codec %u changed: %s", i++, tmp);
          g_free (tmp);
          secondary_codecs = secondary_codecs->next;
        }
    }

  g_hash_table_iter_init (&iter, content->streams);
  while (g_hash_table_iter_next (&iter, &key, &value))
    if (tf_call_stream_bus_message (value, message))
      return TRUE;

  return ret;
}

static void
tf_call_content_error (TfContent *content,
    TpCallStateChangeReason reason,
    const gchar *detailed_reason,
    const gchar *message)
{
  TfCallContent *self = TF_CALL_CONTENT (content);

  g_warning ("%s", message);
  tp_cli_call_content_interface_media_call_fail (
      self->proxy, -1,
      tp_value_array_build (0, reason, detailed_reason, message),
      NULL, NULL, NULL, NULL);
}



static FsStream *
tf_call_content_get_existing_fsstream_by_handle (TfCallContent *content,
    guint contact_handle)
{
  guint i;

  TF_CALL_CONTENT_LOCK (content);

  for (i = 0; i < content->fsstreams->len; i++)
    {
      struct CallFsStream *cfs = g_ptr_array_index (content->fsstreams, i);

      if (cfs->contact_handle == contact_handle)
        {
          cfs->use_count++;
          TF_CALL_CONTENT_UNLOCK (content);
          return cfs->fsstream;
        }
    }

  TF_CALL_CONTENT_UNLOCK (content);

  return NULL;
}


FsStream *
_tf_call_content_get_fsstream_by_handle (TfCallContent *content,
    guint contact_handle,
    const gchar *transmitter,
    guint stream_transmitter_n_parameters,
    GParameter *stream_transmitter_parameters,
    GError **error)
{
  struct CallFsStream *cfs;
  FsParticipant *p;
  FsStream *s;

  s = tf_call_content_get_existing_fsstream_by_handle (content,
      contact_handle);
  if (s)
    return s;

  p = _tf_call_channel_get_participant (content->call_channel,
      content->fsconference, contact_handle, error);
  if (!p)
    return NULL;

  s = fs_session_new_stream (content->fssession, p, FS_DIRECTION_RECV, error);
  if (!s)
    {
      _tf_call_channel_put_participant (content->call_channel, p);
      return NULL;
    }

  if (!fs_stream_set_transmitter (s, transmitter,
          stream_transmitter_parameters, stream_transmitter_n_parameters,
          error))
    {
      g_object_unref (s);
      _tf_call_channel_put_participant (content->call_channel, p);
      return NULL;
    }

  cfs = g_slice_new (struct CallFsStream);
  cfs->use_count = 1;
  cfs->contact_handle = contact_handle;
  cfs->parent_channel = content->call_channel;
  cfs->fsparticipant = p;
  cfs->fsstream = s;

  tp_g_signal_connect_object (s, "src-pad-added",
      G_CALLBACK (src_pad_added), content, 0);

  g_ptr_array_add (content->fsstreams, cfs);
  content->fsstreams_cookie ++;
  if (content->current_media_description != NULL
      && content->current_md_contact_handle == contact_handle)
  {
    GList *codecs = content->current_md_fscodecs;
    TpProxy *current_media_description = content->current_media_description;
    GList *rtp_hdrext = content->current_md_rtp_hdrext;

    content->current_md_fscodecs = NULL;
    content->current_media_description = NULL;
    content->current_md_rtp_hdrext = NULL;

    /* ownership transfers to try_codecs */
    process_media_description_try_codecs (content, s,
        current_media_description, codecs, rtp_hdrext);
  }

  return s;
}

void
_tf_call_content_put_fsstream (TfCallContent *content, FsStream *fsstream)
{
  guint i;
  struct CallFsStream *fs_cfs = NULL;

  TF_CALL_CONTENT_LOCK (content);
  for (i = 0; i < content->fsstreams->len; i++)
    {
      struct CallFsStream *cfs = g_ptr_array_index (content->fsstreams, i);

      if (cfs->fsstream == fsstream)
        {
          cfs->use_count--;
          if (cfs->use_count <= 0)
            {
              fs_cfs = g_ptr_array_remove_index_fast (content->fsstreams, i);
              content->fsstreams_cookie++;
            }
          break;
        }
    }
  TF_CALL_CONTENT_UNLOCK (content);

  if (fs_cfs)
    free_content_fsstream (fs_cfs);
}

FsMediaType
tf_call_content_get_fs_media_type (TfCallContent *content)
{
  return tp_media_type_to_fs (content->media_type);
}

static void
src_pad_added (FsStream *fsstream, GstPad *pad, FsCodec *codec,
    TfCallContent *content)
{
  guint handle = 0;
  guint i;

  TF_CALL_CONTENT_LOCK (content);

  if (!content->fsstreams)
    {
      TF_CALL_CONTENT_UNLOCK (content);
      return;
    }

  for (i = 0; i < content->fsstreams->len; i++)
    {
      struct CallFsStream *cfs = g_ptr_array_index (content->fsstreams, i);
      if (cfs->fsstream == fsstream)
        {
          handle = cfs->contact_handle;
          break;
        }
    }

  TF_CALL_CONTENT_UNLOCK (content);

  _tf_content_emit_src_pad_added (TF_CONTENT (content), handle,
      fsstream, pad, codec);
}

struct StreamSrcPadIterator {
  GstIterator iterator;

  GArray *handles;
  GArray *handles_backup;

  TfCallContent *self;
};

static GstIteratorResult
streams_src_pads_iter_next (GstIterator *it, gpointer *result)
{
  struct StreamSrcPadIterator *iter = (struct StreamSrcPadIterator *) it;
  guint i;

  if (iter->handles->len == 0)
    return GST_ITERATOR_DONE;

  for (i = 0; i < iter->self->fsstreams->len; i++)
    {
      struct CallFsStream *cfs = g_ptr_array_index (iter->self->fsstreams, i);

      if (cfs->contact_handle == g_array_index (iter->handles, guint, 0))
        {
          g_array_remove_index_fast (iter->handles, 0);
          *result = cfs;
          return GST_ITERATOR_OK;
        }
    }

  return GST_ITERATOR_ERROR;

}

static GstIteratorItem
streams_src_pads_iter_item (GstIterator *it, gpointer item)
{
  struct CallFsStream *cfs = item;

  gst_iterator_push (it, fs_stream_iterate_src_pads (cfs->fsstream));

  return GST_ITERATOR_ITEM_SKIP;
}

static void
streams_src_pads_iter_resync (GstIterator *it)
{
  struct StreamSrcPadIterator *iter = (struct StreamSrcPadIterator *) it;

  g_array_set_size (iter->handles, iter->handles_backup->len);
  memcpy (iter->handles->data, iter->handles_backup->data,
      iter->handles_backup->len * sizeof(guint));
}

static void
streams_src_pads_iter_free (GstIterator *it)
{
  struct StreamSrcPadIterator *iter = (struct StreamSrcPadIterator *) it;

  g_array_unref (iter->handles);
  g_array_unref (iter->handles_backup);
  g_object_unref (iter->self);
}

static GstIterator *
tf_call_content_iterate_src_pads (TfContent *content, guint *handles,
    guint handle_count)
{
  TfCallContent *self = TF_CALL_CONTENT (content);
  struct StreamSrcPadIterator *iter;

  iter = (struct StreamSrcPadIterator *) gst_iterator_new (
      sizeof (struct StreamSrcPadIterator), GST_TYPE_PAD,
      self->mutex, &self->fsstreams_cookie,
      streams_src_pads_iter_next,
      streams_src_pads_iter_item,
      streams_src_pads_iter_resync,
      streams_src_pads_iter_free);

  iter->handles = g_array_sized_new (TRUE, FALSE, sizeof(guint), handle_count);
  iter->handles_backup = g_array_sized_new (TRUE, FALSE, sizeof(guint),
      handle_count);
  g_array_append_vals (iter->handles, handles, handle_count);
  g_array_append_vals (iter->handles_backup, handles, handle_count);
  iter->self = g_object_ref (self);

  return (GstIterator *) iter;
}
