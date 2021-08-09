/*
 * Copyright (C) 2018, 2019 Purism SPC
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "callaudiod-pulse"

#include "cad-pulse.h"

#include "libcallaudio.h"

#include <glib/gi18n.h>
#include <glib-object.h>
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>
#include <alsa/use-case.h>

#include <string.h>
#include <stdio.h>

#define APPLICATION_NAME "CallAudio"
#define APPLICATION_ID   "org.mobian-project.CallAudio"

#define SINK_CLASS "sound"
#define CARD_BUS_PATH_PREFIX "platform-"
#define CARD_FORM_FACTOR "internal"
#define CARD_MODEM_CLASS "modem"

#define WITH_DROID_SUPPORT 1 /* FIXME: wire into meson */

#ifdef WITH_DROID_SUPPORT
#define DROID_API_NAME "droid-hal"
#define DROID_PROFILE_HIFI "default"
#define DROID_PROFILE_VOICECALL "voicecall"
#define DROID_OUTPUT_PORT_PARKING "output-parking"
#define DROID_OUTPUT_PORT_SPEAKER "output-speaker"
#define DROID_OUTPUT_PORT_EARPIECE "output-earpiece"
#define DROID_OUTPUT_PORT_WIRED_HEADSET "output-wired_headset"
#define DROID_INPUT_PORT_PARKING "input-parking"
#define DROID_INPUT_PORT_BUILTIN_MIC "input-builtin_mic"
#define DROID_INPUT_PORT_WIRED_HEADSET_MIC "input-wired_headset"
#endif /* WITH_DROID_SUPPORT */

struct _CadPulse
{
    GObject parent_instance;

    pa_glib_mainloop  *loop;
    pa_context        *ctx;

    int card_id;
    int sink_id;
    int source_id;

#ifdef WITH_DROID_SUPPORT
    gboolean sink_is_droid;
    gboolean source_is_droid;
#endif /* WITH_DROID_SUPPORT */

    gboolean has_voice_profile;
    gchar *speaker_port;

    GHashTable *sink_ports;
    GHashTable *source_ports;

    CallAudioMode current_mode;
};

G_DEFINE_TYPE(CadPulse, cad_pulse, G_TYPE_OBJECT);

typedef struct _CadPulseOperation {
    CadPulse *pulse;
    CadOperation *op;
    guint value;
} CadPulseOperation;

#ifdef WITH_DROID_SUPPORT
static void set_output_port(pa_context *ctx, const pa_sink_info *info, int eol, void *data);
static void set_input_port(pa_context *ctx, const pa_source_info *info, int eol, void *data);
#endif /* WITH_DROID_SUPPORT */

static void pulseaudio_cleanup(CadPulse *self);
static gboolean pulseaudio_connect(CadPulse *self);

/******************************************************************************
 * Source management
 *
 * The following functions take care of monitoring and configuring the default
 * source (input)
 ******************************************************************************/

#ifdef WITH_DROID_SUPPORT
static const gchar *get_available_source_port(const pa_source_info *source, const gchar *exclude,
                                              gboolean source_is_droid)
#else
static const gchar *get_available_source_port(const pa_source_info *source, const gchar *exclude)
#endif /* WITH_DROID_SUPPORT */
{
    /*
     * get_available_source_port() works a bit differently than get_available_output():
     *
     * On droid, the input is chosen between the builtin_mic and the
     * wired_headset mic if available.
     *
     * On native devices the mic with the highest priority gets
     * chosen.
    */

    pa_source_port_info *available_port = NULL;
    guint i;

    g_debug("looking for available input excluding '%s'", exclude);

    for (i = 0; i < source->n_ports; i++) {
        pa_source_port_info *port = source->ports[i];

        if ((exclude && strcmp(port->name, exclude) == 0) ||
            port->available == PA_PORT_AVAILABLE_NO) {
            continue;
        }

#ifdef WITH_DROID_SUPPORT
        if (source_is_droid) {
            if (strcmp(port->name, DROID_INPUT_PORT_WIRED_HEADSET_MIC) == 0) {
                /* wired_headset is the preferred one */
                available_port = port;
                break;
            } else if (strcmp(port->name, DROID_INPUT_PORT_BUILTIN_MIC) == 0) {
                /* builtin mic */
                available_port = port;
            }
        } else if (!available_port || port->priority > available_port->priority) {
            available_port = port;
        }
#else
        if (!available_port || port->priority > available_port->priority)
            available_port = port;
#endif /* WITH_DROID_SUPPORT */
    }

    if (available_port) {
        g_debug("found available input '%s'", available_port->name);
        return available_port->name;
    }

    g_warning("no available input found!");

    return NULL;
}

static void change_source_info(pa_context *ctx, const pa_source_info *info, int eol, void *data)
{
    CadPulse *self = data;
    const gchar *target_port;
    pa_operation *op;
    gboolean change = FALSE;
    guint i;

    if (eol != 0)
        return;

    if (!info) {
        g_critical("PA returned no source info (eol=%d)", eol);
        return;
    }

    if (info->index != self->source_id)
        return;

    for (i = 0; i < info->n_ports; i++) {
        pa_source_port_info *port = info->ports[i];

        if (port->available != PA_PORT_AVAILABLE_UNKNOWN) {
            enum pa_port_available available;
            available = GPOINTER_TO_INT(g_hash_table_lookup(self->source_ports, port->name));
            if (available != port->available) {
                g_hash_table_insert(self->source_ports, g_strdup(port->name),
                                    GINT_TO_POINTER(port->available));
                change = TRUE;
            }
        }
    }

    if (change) {
#ifdef WITH_DROID_SUPPORT
        target_port = get_available_source_port(info, NULL, self->source_is_droid);
#else
        target_port = get_available_source_port(info, NULL);
#endif /* WITH_DROID_SUPPORT */
        if (target_port) {
            op = pa_context_set_source_port_by_index(ctx, self->source_id,
                                                   target_port, NULL, NULL);
            if (op)
                pa_operation_unref(op);
        }
    }
}

static void process_new_source(CadPulse *self, const pa_source_info *info)
{
    const gchar *prop;
    int i;

    prop = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_CLASS);
    if (prop && strcmp(prop, SINK_CLASS) != 0)
        return;
    if (info->card != self->card_id || self->source_id != -1)
        return;

#ifdef WITH_DROID_SUPPORT
    prop = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_API);
    self->source_is_droid = (prop && strcmp(prop, DROID_API_NAME) == 0);
#endif /* WITH_DROID_SUPPORT */

    self->source_id = info->index;
    if (self->source_ports)
        g_hash_table_destroy(self->source_ports);
    self->source_ports = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (i = 0; i < info->n_ports; i++) {
        pa_source_port_info *port = info->ports[i];

        if (port->available != PA_PORT_AVAILABLE_UNKNOWN) {
            g_hash_table_insert (self->source_ports,
                                 g_strdup(port->name),
                                 GINT_TO_POINTER(port->available));
        }
    }

    g_debug("SOURCE: idx=%u name='%s'", info->index, info->name);
}

static void init_source_info(pa_context *ctx, const pa_source_info *info, int eol, void *data)
{
    CadPulse *self = data;
    const gchar *target_port;
    pa_operation *op;

    if (eol != 0)
        return;

    if (!info) {
        g_critical("PA returned no source info (eol=%d)", eol);
        return;
    }

    process_new_source(self, info);
    if (self->source_id < 0)
        return;

#ifdef WITH_DROID_SUPPORT
    target_port = get_available_source_port(info, NULL, self->source_is_droid);
#else
    target_port = get_available_source_port(info, NULL);
#endif /* WITH_DROID_SUPPORT */
    if (target_port) {
        op = pa_context_set_source_port_by_index(ctx, self->source_id,
                                                 target_port, NULL, NULL);
        if (op)
            pa_operation_unref(op);
    }
}

/******************************************************************************
 * Sink management
 *
 * The following functions take care of monitoring and configuring the default
 * sink (output)
 ******************************************************************************/

#ifdef WITH_DROID_SUPPORT
static const gchar *get_available_sink_port(const pa_sink_info *sink, const gchar *exclude, gboolean source_is_droid)
#else
static const gchar *get_available_sink_port(const pa_sink_info *sink, const gchar *exclude)
#endif /* WITH_DROID_SUPPORT */
{
    pa_sink_port_info *available_port = NULL;
    guint i;

    g_debug("looking for available output excluding '%s'", exclude);

    for (i = 0; i < sink->n_ports; i++) {
        pa_sink_port_info *port = sink->ports[i];

        if ((exclude && strcmp(port->name, exclude) == 0) ||
            port->available == PA_PORT_AVAILABLE_NO) {
            continue;
        }

#ifdef WITH_DROID_SUPPORT
        if (source_is_droid) {
            if (strcmp(port->name, DROID_OUTPUT_PORT_WIRED_HEADSET) == 0) {
                /* wired_headset is the preferred one */
                available_port = port;
                break;
            } else if (strcmp(port->name, DROID_OUTPUT_PORT_SPEAKER) == 0 || strcmp(port->name, DROID_OUTPUT_PORT_EARPIECE) == 0) {
                /* builtin mic */
                available_port = port;
            }
        } else if (!available_port || port->priority > available_port->priority) {
            available_port = port;
        }
#else
        if (!available_port || port->priority > available_port->priority)
            available_port = port;
#endif /* WITH_DROID_SUPPORT */
    }

    if (available_port) {
        g_debug("found available output '%s'", available_port->name);
        return available_port->name;
    }

    g_warning("no available output found!");

    return NULL;
}

static void change_sink_info(pa_context *ctx, const pa_sink_info *info, int eol, void *data)
{
    CadPulse *self = data;
    const gchar *target_port;
    pa_operation *op;
    gboolean change = FALSE;
    guint i;

    if (eol != 0)
        return;

    if (!info) {
        g_critical("PA returned no sink info (eol=%d)", eol);
        return;
    }

    if (info->index != self->sink_id)
        return;

    for (i = 0; i < info->n_ports; i++) {
        pa_sink_port_info *port = info->ports[i];

        if (port->available != PA_PORT_AVAILABLE_UNKNOWN) {
            enum pa_port_available available;
            available = GPOINTER_TO_INT(g_hash_table_lookup(self->sink_ports, port->name));
            if (available != port->available) {
                g_hash_table_insert(self->sink_ports, g_strdup(port->name),
                                    GINT_TO_POINTER(port->available));
                change = TRUE;
            }
        }
    }

    if (change) {
#ifdef WITH_DROID_SUPPORT
        target_port = get_available_sink_port(info, NULL, self->source_is_droid);
#else
        target_port = get_available_sink_port(info, NULL);
#endif /* WITH_DROID_SUPPORT */
        if (target_port) {
            op = pa_context_set_sink_port_by_index(ctx, self->sink_id,
                                                   target_port, NULL, NULL);
            if (op)
                pa_operation_unref(op);
        }
    }
}

static void process_new_sink(CadPulse *self, const pa_sink_info *info)
{
    const gchar *prop;
    guint i;

    prop = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_CLASS);
    if (prop && strcmp(prop, SINK_CLASS) != 0)
        return;
    if (info->card != self->card_id || self->sink_id != -1)
        return;

#ifdef WITH_DROID_SUPPORT
    prop = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_API);
    self->sink_is_droid = (prop && strcmp(prop, DROID_API_NAME) == 0);
#endif /* WITH_DROID_SUPPORT */

    self->sink_id = info->index;
    if (self->sink_ports)
        g_hash_table_destroy(self->sink_ports);
    self->sink_ports = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    g_debug("SINK: idx=%u name='%s'", info->index, info->name);

    for (i = 0; i < info->n_ports; i++) {
        pa_sink_port_info *port = info->ports[i];

#ifdef WITH_DROID_SUPPORT
        if ((self->sink_is_droid && strcmp(port->name, DROID_OUTPUT_PORT_SPEAKER) == 0) ||
            (!self->sink_is_droid && strstr(port->name, SND_USE_CASE_DEV_SPEAKER) != 0)) {
#else
        if (strstr(port->name, SND_USE_CASE_DEV_SPEAKER) != NULL) {
#endif /* WITH_DROID_SUPPORT */
            if (self->speaker_port) {
                if (strcmp(port->name, self->speaker_port) != 0) {
                    g_free(self->speaker_port);
                    self->speaker_port = g_strdup(port->name);
                }
            } else {
                self->speaker_port = g_strdup(port->name);
            }
        }

        if (port->available != PA_PORT_AVAILABLE_UNKNOWN) {
            g_hash_table_insert (self->sink_ports,
                                 g_strdup(port->name),
                                 GINT_TO_POINTER(port->available));
        }
    }

    g_debug("SINK:   speaker_port='%s'", self->speaker_port);
}

static void init_sink_info(pa_context *ctx, const pa_sink_info *info, int eol, void *data)
{
    CadPulse *self = data;
    const gchar *target_port;
    pa_operation *op;

    if (eol != 0)
        return;

    if (!info) {
        g_critical("PA returned no sink info (eol=%d)", eol);
        return;
    }

    process_new_sink(self, info);
    if (self->sink_id < 0)
        return;

#ifdef WITH_DROID_SUPPORT
    target_port = get_available_sink_port(info, NULL, self->source_is_droid);
#else
    target_port = get_available_sink_port(info, NULL);
#endif /* WITH_DROID_SUPPORT */
    if (target_port) {
        g_debug("  Using sink port '%s'", target_port);
        op = pa_context_set_sink_port_by_index(ctx, self->sink_id,
                                               target_port, NULL, NULL);
        if (op)
            pa_operation_unref(op);
    }
}

/******************************************************************************
 * Card management
 *
 * The following functions take care of gathering information about the default
 * sound card
 ******************************************************************************/

static void init_card_info(pa_context *ctx, const pa_card_info *info, int eol, void *data)
{
    CadPulse *self = data;
    const gchar *prop;
    guint i;

    if (eol != 0)
        return;

    if (!info) {
        g_critical("PA returned no card info (eol=%d)", eol);
        return;
    }

    prop = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_BUS_PATH);
    if (prop && !g_str_has_prefix(prop, CARD_BUS_PATH_PREFIX))
        return;
    prop = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_FORM_FACTOR);
    if (prop && strcmp(prop, CARD_FORM_FACTOR) != 0)
        return;
    prop = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_CLASS);
    if (prop && strcmp(prop, CARD_MODEM_CLASS) == 0)
        return;

    self->card_id = info->index;

    g_debug("CARD: idx=%u name='%s'", info->index, info->name);

    for (i = 0; i < info->n_profiles; i++) {
        pa_card_profile_info2 *profile = info->profiles2[i];

#ifdef WITH_DROID_SUPPORT
        if (strstr(profile->name, SND_USE_CASE_VERB_VOICECALL) != NULL || strstr(profile->name, DROID_PROFILE_VOICECALL) != NULL) {
#else
        if (strstr(profile->name, SND_USE_CASE_VERB_VOICECALL) != NULL) {
#endif /* WITH_DROID_SUPPORT */
            self->has_voice_profile = TRUE;
            break;
        }
    }

    g_debug("CARD:   %s voice profile", self->has_voice_profile ? "has" : "doesn't have");
}

/******************************************************************************
 * PulseAudio management
 *
 * The following functions configure the PulseAudio connection and monitor the
 * state of PulseAudio objects
 ******************************************************************************/

 static void init_module_info(pa_context *ctx, const pa_module_info *info, int eol, void *data)
 {
     pa_operation *op;

     if (eol != 0)
         return;

     if (!info) {
         g_critical("PA returned no module info (eol=%d)", eol);
         return;
     }

     g_debug("MODULE: idx=%u name='%s'", info->index, info->name);

#ifndef WITH_DROID_SUPPORT
     if (strcmp(info->name, "module-switch-on-port-available") == 0) {
         g_debug("MODULE: unloading '%s'", info->name);
         op = pa_context_unload_module(ctx, info->index, NULL, NULL);
         if (op)
             pa_operation_unref(op);
     }
#endif /* WITH_DROID_SUPPORT */
 }

 static void init_pulseaudio_objects(CadPulse *self)
 {
     pa_operation *op;

     self->card_id = self->sink_id = self->source_id = -1;
     self->sink_ports = self->source_ports = NULL;

     op = pa_context_get_card_info_list(self->ctx, init_card_info, self);
     if (op)
         pa_operation_unref(op);
     op = pa_context_get_module_info_list(self->ctx, init_module_info, self);
     if (op)
         pa_operation_unref(op);
     op = pa_context_get_sink_info_list(self->ctx, init_sink_info, self);
     if (op)
         pa_operation_unref(op);
     op = pa_context_get_source_info_list(self->ctx, init_source_info, self);
     if (op)
         pa_operation_unref(op);
 }

static void changed_cb(pa_context *ctx, pa_subscription_event_type_t type, uint32_t idx, void *data)
{
    CadPulse *self = data;
    pa_subscription_event_type_t kind = type & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
    pa_operation *op = NULL;

    switch (type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) {
    case PA_SUBSCRIPTION_EVENT_SINK:
        if (idx == self->sink_id && kind == PA_SUBSCRIPTION_EVENT_REMOVE) {
            g_debug("sink %u removed", idx);
            self->sink_id = -1;
            g_hash_table_destroy(self->sink_ports);
            self->sink_ports = NULL;
        } else if (kind == PA_SUBSCRIPTION_EVENT_NEW) {
            g_debug("new sink %u", idx);
            op = pa_context_get_sink_info_by_index(ctx, idx, init_sink_info, self);
            if (op)
                pa_operation_unref(op);
        }
        break;
    case PA_SUBSCRIPTION_EVENT_SOURCE:
        if (idx == self->source_id && kind == PA_SUBSCRIPTION_EVENT_REMOVE) {
            g_debug("source %u removed", idx);
            self->source_id = -1;
            g_hash_table_destroy(self->source_ports);
            self->source_ports = NULL;
        } else if (kind == PA_SUBSCRIPTION_EVENT_NEW) {
            g_debug("new sink %u", idx);
            op = pa_context_get_source_info_by_index(ctx, idx, init_source_info, self);
            if (op)
                pa_operation_unref(op);
        }
        break;
    case PA_SUBSCRIPTION_EVENT_CARD:
        if (idx == self->card_id && kind == PA_SUBSCRIPTION_EVENT_CHANGE) {
            g_debug("card %u changed", idx);
            /**
             * On Droid, do not change ports automatically
            */
#ifdef WITH_DROID_SUPPORT
            if (!self->sink_is_droid && self->sink_id != -1) {
#else
            if (self->sink_id != -1) {
#endif /* WITH_DROID_SUPPORT */
                op = pa_context_get_sink_info_by_index(ctx, self->sink_id,
                                                       change_sink_info, self);
                if (op)
                    pa_operation_unref(op);
            }
#ifdef WITH_DROID_SUPPORT
            if (!self->source_is_droid && self->source_id != -1) {
#else
            if (self->source_id != -1) {
#endif /* WITH_DROID_SUPPORT */
                op = pa_context_get_source_info_by_index(ctx, self->source_id,
                                                         change_source_info, self);
                if (op)
                    pa_operation_unref(op);
            }
        }
        break;
    default:
        break;
    }
}

static void pulse_state_cb(pa_context *ctx, void *data)
{
    CadPulse *self = data;
    pa_context_state_t state;

    state = pa_context_get_state(ctx);
    switch (state) {
    case PA_CONTEXT_UNCONNECTED:
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_SETTING_NAME:
        g_debug("PA not ready");
        break;
    case PA_CONTEXT_FAILED:
        g_critical("Error in PulseAudio context: %s", pa_strerror(pa_context_errno(ctx)));
        pulseaudio_cleanup(self);
        g_idle_add(G_SOURCE_FUNC(pulseaudio_connect), self);
        break;
    case PA_CONTEXT_TERMINATED:
    case PA_CONTEXT_READY:
        pa_context_set_subscribe_callback(ctx, changed_cb, self);
        pa_context_subscribe(ctx,
                             PA_SUBSCRIPTION_MASK_SINK  | PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_CARD,
                             NULL, self);
        g_debug("PA is ready, initializing cards list");
        init_pulseaudio_objects(self);
        break;
    }
}

static void pulseaudio_cleanup(CadPulse *self)
{
    if (self->ctx) {
        pa_context_disconnect(self->ctx);
        pa_context_unref(self->ctx);
        self->ctx = NULL;
    }
}

static gboolean pulseaudio_connect(CadPulse *self)
{
    pa_proplist *props;
    int err;

    /* Meta data */
    props = pa_proplist_new();
    g_assert(props != NULL);

    err = pa_proplist_sets(props, PA_PROP_APPLICATION_NAME, APPLICATION_NAME);
    err = pa_proplist_sets(props, PA_PROP_APPLICATION_ID, APPLICATION_ID);

    if (!self->loop)
        self->loop = pa_glib_mainloop_new(NULL);
    if (!self->loop)
        g_error ("Error creating PulseAudio main loop");

    if (!self->ctx)
        self->ctx = pa_context_new(pa_glib_mainloop_get_api(self->loop), APPLICATION_NAME);
    if (!self->ctx)
        g_error ("Error creating PulseAudio context");

    pa_context_set_state_callback(self->ctx, (pa_context_notify_cb_t)pulse_state_cb, self);
    err = pa_context_connect(self->ctx, NULL, PA_CONTEXT_NOFAIL, 0);
    if (err < 0)
        g_error ("Error connecting to PulseAudio context: %s", pa_strerror(err));

    return G_SOURCE_REMOVE;
}

/******************************************************************************
 * GObject base functions
 ******************************************************************************/

static void constructed(GObject *object)
{
    GObjectClass *parent_class = g_type_class_peek(G_TYPE_OBJECT);
    CadPulse *self = CAD_PULSE(object);

    pulseaudio_connect(self);

    parent_class->constructed(object);
}


static void dispose(GObject *object)
{
    GObjectClass *parent_class = g_type_class_peek(G_TYPE_OBJECT);
    CadPulse *self = CAD_PULSE(object);

    if (self->speaker_port)
        g_free(self->speaker_port);

    pulseaudio_cleanup(self);

    if (self->loop) {
        pa_glib_mainloop_free(self->loop);
        self->loop = NULL;
    }

    parent_class->dispose(object);
}

static void cad_pulse_class_init(CadPulseClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->constructed = constructed;
    object_class->dispose = dispose;
}

static void cad_pulse_init(CadPulse *self)
{
}

CadPulse *cad_pulse_get_default(void)
{
    static CadPulse *pulse = NULL;

    if (pulse == NULL) {
        g_debug("initializing pulseaudio backend...");
        pulse = g_object_new(CAD_TYPE_PULSE, NULL);
        g_object_add_weak_pointer(G_OBJECT(pulse), (gpointer *)&pulse);
    }

    return pulse;
}

/******************************************************************************
 * Commands management
 *
 * The following functions handle external requests to switch mode, output port
 * or microphone status
 ******************************************************************************/

static void operation_complete_cb(pa_context *ctx, int success, void *data)
{
    CadPulseOperation *operation = data;

    g_debug("operation returned %d", success);

    if (operation) {
        if (operation->op) {
            operation->op->success = (gboolean)!!success;
            operation->op->callback(operation->op);

            if (operation->op->type == CAD_OPERATION_SELECT_MODE &&
                operation->op->success) {
                operation->pulse->current_mode = operation->value;
            }
        }

        free(operation);
    }
}

#ifdef WITH_DROID_SUPPORT
static void droid_source_parked_complete_cb(pa_context *ctx, int success, void *data)
{
    /*
     * Callback fired when the source parking happened. We can now actually
     * change the output port.
    */

    CadPulseOperation *operation = data;
    pa_operation *op = NULL;

    g_debug("droid: parking succeeded, setting real output port");

    op = pa_context_get_sink_info_by_index(operation->pulse->ctx,
                                           operation->pulse->sink_id,
                                           set_output_port, data);

    if (op)
        pa_operation_unref(op);

}

static void droid_sink_parked_complete_cb(pa_context *ctx, int success, void *data)
{
    /*
     * Callback fired when the sink parking happened. We can then park
     * the source too.
    */

    CadPulseOperation *operation = data;
    pa_operation *op = NULL;

    g_debug("droid: parking input to trigger mode change");

    op = pa_context_set_source_port_by_index(ctx, operation->pulse->source_id,
                                            DROID_INPUT_PORT_PARKING,
                                            droid_source_parked_complete_cb, data);

    if (op)
        pa_operation_unref(op);

}

static void droid_mode_change_complete_cb(pa_context *ctx, int success, void *data)
{
    /*
     * Android HAL switches modes once the next routing change happens.
     * Thus, we need to "park" the sink/source before switching to the
     * actual port.
     *
     * pulseaudio-modules-droid provides the input-parking and output-parking
     * ports to accomplish that.
     *
     * It's one more step that needs to be done only on droid devices.
    */

    CadPulseOperation *operation = data;
    pa_operation *op = NULL;

    if (!operation->pulse->sink_is_droid)
        return operation_complete_cb(ctx, success, data);

    g_debug("droid: parking output to trigger mode change");

    op = pa_context_set_sink_port_by_index(ctx, operation->pulse->sink_id,
                                           DROID_OUTPUT_PORT_PARKING,
                                           droid_sink_parked_complete_cb,
                                           operation);
    if (op)
        pa_operation_unref(op);
}

static void droid_output_port_change_complete_cb(pa_context *ctx, int success, void *data)
{
    CadPulseOperation *operation = data;
    pa_operation *op = NULL;

    g_debug("droid: setting real input port");

    op = pa_context_get_source_info_by_index(operation->pulse->ctx,
                                           operation->pulse->source_id,
                                           set_input_port, operation);

    if (op)
        pa_operation_unref(op);
}
#endif /* WITH_DROID_SUPPORT */

static void set_card_profile(pa_context *ctx, const pa_card_info *info, int eol, void *data)
{
    CadPulseOperation *operation = data;
    pa_card_profile_info2 *profile;
    pa_operation *op = NULL;
    gchar *default_profile;
    gchar *voicecall_profile;
    pa_context_success_cb_t complete_callback;

    if (eol != 0)
        return;

    if (!info) {
        g_critical("PA returned no card info (eol=%d)", eol);
        return;
    }

    if (info->index != operation->pulse->card_id)
        return;

#ifdef WITH_DROID_SUPPORT
    default_profile = operation->pulse->sink_is_droid ?
                          DROID_PROFILE_HIFI :
                          SND_USE_CASE_VERB_HIFI;
    voicecall_profile = operation->pulse->sink_is_droid ?
                            DROID_PROFILE_VOICECALL :
                            SND_USE_CASE_VERB_VOICECALL;
    complete_callback = droid_mode_change_complete_cb;
#else
    default_profile = SND_USE_CASE_VERB_HIFI;
    voicecall_profile = SND_USE_CASE_VERB_VOICECALL;
    complete_callback = operation_complete_cb;
#endif /* WITH_DROID_SUPPORT */

    profile = info->active_profile2;

    if (strcmp(profile->name, voicecall_profile) == 0 && operation->value == 0) {
        g_debug("switching to default profile");
        op = pa_context_set_card_profile_by_index(ctx, operation->pulse->card_id,
                                                  default_profile,
                                                  complete_callback, operation);
    } else if (strcmp(profile->name, default_profile) == 0 && operation->value == 1) {
        g_debug("switching to voice profile");
        op = pa_context_set_card_profile_by_index(ctx, operation->pulse->card_id,
                                                  voicecall_profile,
                                                  complete_callback, operation);
    }

    if (op) {
        pa_operation_unref(op);
    } else {
        g_debug("%s: nothing to be done", __func__);
        operation_complete_cb(ctx, 1, operation);
    }
}

static void set_output_port(pa_context *ctx, const pa_sink_info *info, int eol, void *data)
{
    CadPulseOperation *operation = data;
    pa_operation *op = NULL;
    const gchar *target_port;
    pa_context_success_cb_t complete_callback;

#ifdef WITH_DROID_SUPPORT
    complete_callback = droid_output_port_change_complete_cb;
#else
    complete_callback = operation_complete_cb;
#endif

    if (eol != 0)
        return;

    if (!info) {
        g_critical("PA returned no sink info (eol=%d)", eol);
        return;
    }

    if (info->card != operation->pulse->card_id || info->index != operation->pulse->sink_id)
        return;

    if (operation->op->type == CAD_OPERATION_SELECT_MODE) {
        /*
         * When switching to voice call mode, we want to switch to any port
         * other than the speaker; this makes sure we use the headphones if they
         * are connected, and the earpiece otherwise.
         * When switching back to normal mode, the highest priority port is to
         * be selected anyway.
         */
        if (operation->value == CALL_AUDIO_MODE_CALL)
#ifdef WITH_DROID_SUPPORT
            target_port = get_available_sink_port(info, operation->pulse->speaker_port, operation->pulse->source_is_droid);
#else
            target_port = get_available_sink_port(info, operation->pulse->speaker_port);
#endif
        else
#ifdef WITH_DROID_SUPPORT
            target_port = get_available_sink_port(info, NULL, operation->pulse->source_is_droid);
#else
            target_port = get_available_sink_port(info, NULL);
#endif
    } else {
        /*
         * When forcing speaker output, we simply select the speaker port.
         * When disabling speaker output, we want the highest priority port
         * other than the speaker, so that we use the headphones if connected,
         * and the earpiece otherwise.
         */
        if (operation->value)
            target_port = operation->pulse->speaker_port;
        else
#ifdef WITH_DROID_SUPPORT
            target_port = get_available_sink_port(info, operation->pulse->speaker_port, operation->pulse->source_is_droid);
#else
            target_port = get_available_sink_port(info, operation->pulse->speaker_port);
#endif
    }

    g_debug("active port is '%s', target port is '%s'", info->active_port->name, target_port);

    if (strcmp(info->active_port->name, target_port) != 0) {
        g_debug("switching to target port '%s'", target_port);
        op = pa_context_set_sink_port_by_index(ctx, operation->pulse->sink_id,
                                               target_port,
                                               complete_callback, operation);
    }

    if (op) {
        pa_operation_unref(op);
    } else {
        g_debug("%s: nothing to be done", __func__);
        operation_complete_cb(ctx, 1, operation);
    }
}

static void set_input_port(pa_context *ctx, const pa_source_info *info, int eol, void *data)
{
    CadPulseOperation *operation = data;
    pa_operation *op = NULL;
    const gchar *target_port;

    if (eol != 0)
        return;

    if (!info)
        g_error("PA returned no source info (eol=%d)", eol);

    if (info->card != operation->pulse->card_id || info->index != operation->pulse->source_id)
        return;

#ifdef WITH_DROID_SUPPORT
    target_port = get_available_source_port(info, NULL, operation->pulse->source_is_droid);
#else
    target_port = get_available_source_port(info, NULL);
#endif

    g_debug("active source port is '%s', target source port is '%s'", info->active_port->name, target_port);

    if (strcmp(info->active_port->name, target_port) != 0) {
        g_debug("switching to target source port '%s'", target_port);
        op = pa_context_set_source_port_by_index(ctx, operation->pulse->source_id,
                                                 target_port,
                                                 operation_complete_cb, operation);
    }

    if (op) {
        pa_operation_unref(op);
    } else {
        g_debug("%s: nothing to be done", __func__);
        operation_complete_cb(ctx, 1, operation);
    }
}

static void set_mic_mute(pa_context *ctx, const pa_source_info *info, int eol, void *data)
{
    CadPulseOperation *operation = data;
    pa_operation *op = NULL;

    if (eol != 0)
        return;

    if (!info) {
        g_critical("PA returned no source info (eol=%d)", eol);
        return;
    }

    if (info->card != operation->pulse->card_id || info->index != operation->pulse->source_id)
        return;

    if (info->mute && !operation->value) {
        g_debug("mic is muted, unmuting...");
        op = pa_context_set_source_mute_by_index(ctx, operation->pulse->source_id, 0,
                                                 operation_complete_cb, operation);
    } else if (!info->mute && operation->value) {
        g_debug("mic is active, muting...");
        op = pa_context_set_source_mute_by_index(ctx, operation->pulse->source_id, 1,
                                                 operation_complete_cb, operation);
    }

    if (op) {
        pa_operation_unref(op);
    } else {
        g_debug("%s: nothing to be done", __func__);
        operation_complete_cb(ctx, 1, operation);
    }
}

/**
 * cad_pulse_select_mode:
 * @mode:
 * @cad_op:
 *
 * */
void cad_pulse_select_mode(guint mode, CadOperation *cad_op)
{
    CadPulseOperation *operation = g_new(CadPulseOperation, 1);
    pa_operation *op = NULL;

    if (!cad_op) {
        g_critical("%s: no callaudiod operation", __func__);
        goto error;
    }
    if (!operation) {
        g_critical("%s: unable to allocate memory", __func__);
        goto error;
    }

    /*
     * Make sure cad_op is of the correct type!
     */
    g_assert(cad_op->type == CAD_OPERATION_SELECT_MODE);

    operation->pulse = cad_pulse_get_default();
    operation->op = cad_op;
    operation->value = mode;

    if (mode != CALL_AUDIO_MODE_CALL) {
        /*
         * When ending a call, we want to make sure the mic doesn't stay muted
         */
        CadPulseOperation *unmute_op = g_new0(CadPulseOperation, 1);

        unmute_op->pulse = operation->pulse;
        unmute_op->value = FALSE;

        op = pa_context_get_source_info_by_index(unmute_op->pulse->ctx,
                                                 unmute_op->pulse->source_id,
                                                 set_mic_mute, unmute_op);
        if (op)
            pa_operation_unref(op);
    }

    if (operation->pulse->has_voice_profile) {
      /*
       * The pinephone f.e. has a voice profile
       */
        g_debug("card has voice profile, using it");
        op = pa_context_get_card_info_by_index(operation->pulse->ctx,
                                               operation->pulse->card_id,
                                               set_card_profile, operation);
    } else {
        if (operation->pulse->sink_id < 0) {
            g_warning("card has no voice profile and no usable sink");
            goto error;
        }
        g_debug("card doesn't have voice profile, switching output port");

        op = pa_context_get_sink_info_by_index(operation->pulse->ctx,
                                               operation->pulse->sink_id,
                                               set_output_port, operation);
    }

    if (op)
        pa_operation_unref(op);

    return;

error:
    if (cad_op) {
        cad_op->success = FALSE;
        cad_op->callback(cad_op);
    }
    if (operation)
        free(operation);
}

void cad_pulse_enable_speaker(gboolean enable, CadOperation *cad_op)
{
    CadPulseOperation *operation = g_new(CadPulseOperation, 1);
    pa_operation *op = NULL;

    if (!cad_op) {
        g_critical("%s: no callaudiod operation", __func__);
        goto error;
    }
    if (!operation) {
        g_critical("%s: unable to allocate memory", __func__);
        goto error;
    }

    /*
     * Make sure cad_op is of the correct type!
     */
    g_assert(cad_op->type == CAD_OPERATION_ENABLE_SPEAKER);

    operation->pulse = cad_pulse_get_default();

    if (operation->pulse->sink_id < 0) {
        g_warning("card has no usable sink");
        goto error;
    }

    operation->op = cad_op;
    operation->value = (guint)enable;

    op = pa_context_get_sink_info_by_index(operation->pulse->ctx,
                                           operation->pulse->sink_id,
                                           set_output_port, operation);
    if (op)
        pa_operation_unref(op);

    return;

error:
    if (cad_op) {
        cad_op->success = FALSE;
        cad_op->callback(cad_op);
    }
    if (operation)
        free(operation);
}

void cad_pulse_mute_mic(gboolean mute, CadOperation *cad_op)
{
    CadPulseOperation *operation = g_new(CadPulseOperation, 1);
    pa_operation *op = NULL;

    if (!cad_op) {
        g_critical("%s: no callaudiod operation", __func__);
        goto error;
    }
    if (!operation) {
        g_critical("%s: unable to allocate memory", __func__);
        goto error;
    }

    /*
     * Make sure cad_op is of the correct type!
     */
    g_assert(cad_op->type == CAD_OPERATION_MUTE_MIC);

    operation->pulse = cad_pulse_get_default();

    if (operation->pulse->source_id < 0) {
        g_warning("card has no usable source");
        goto error;
    }

    operation->op = cad_op;
    operation->value = (guint)mute;

    op = pa_context_get_source_info_by_index(operation->pulse->ctx,
                                             operation->pulse->source_id,
                                             set_mic_mute, operation);
    if (op)
        pa_operation_unref(op);

    return;

error:
    if (cad_op) {
        cad_op->success = FALSE;
        cad_op->callback(cad_op);
    }
    if (operation)
        free(operation);
}

