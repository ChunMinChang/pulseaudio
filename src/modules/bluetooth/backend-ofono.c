/***
  This file is part of PulseAudio.

  Copyright 2013 João Paulo Rechi Vita

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/core-util.h>
#include <pulsecore/dbus-shared.h>

#include "bluez5-util.h"

#define HFP_AUDIO_CODEC_CVSD    0x01
#define HFP_AUDIO_CODEC_MSBC    0x02

#define OFONO_SERVICE "org.ofono"
#define HF_AUDIO_AGENT_INTERFACE OFONO_SERVICE ".HandsfreeAudioAgent"
#define HF_AUDIO_MANAGER_INTERFACE OFONO_SERVICE ".HandsfreeAudioManager"

#define HF_AUDIO_AGENT_PATH "/HandsfreeAudioAgent"

#define HF_AUDIO_AGENT_XML                                          \
    DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE                       \
    "<node>"                                                        \
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">"    \
    "    <method name=\"Introspect\">"                              \
    "      <arg direction=\"out\" type=\"s\" />"                    \
    "    </method>"                                                 \
    "  </interface>"                                                \
    "  <interface name=\"org.ofono.HandsfreeAudioAgent\">"          \
    "    <method name=\"Release\">"                                 \
    "    </method>"                                                 \
    "    <method name=\"NewConnection\">"                           \
    "      <arg direction=\"in\"  type=\"o\" name=\"card_path\" />" \
    "      <arg direction=\"in\"  type=\"h\" name=\"sco_fd\" />"    \
    "      <arg direction=\"in\"  type=\"y\" name=\"codec\" />"     \
    "    </method>"                                                 \
    "  </interface>"                                                \
    "</node>"

struct pa_bluetooth_backend {
    pa_core *core;
    pa_bluetooth_discovery *discovery;
    pa_dbus_connection *connection;
    pa_hashmap *cards;
    char *ofono_bus_id;

    PA_LLIST_HEAD(pa_dbus_pending, pending);
};

static pa_dbus_pending* hf_dbus_send_and_add_to_pending(pa_bluetooth_backend *backend, DBusMessage *m,
                                                    DBusPendingCallNotifyFunction func, void *call_data) {
    pa_dbus_pending *p;
    DBusPendingCall *call;

    pa_assert(backend);
    pa_assert(m);

    pa_assert_se(dbus_connection_send_with_reply(pa_dbus_connection_get(backend->connection), m, &call, -1));

    p = pa_dbus_pending_new(pa_dbus_connection_get(backend->connection), m, call, backend, call_data);
    PA_LLIST_PREPEND(pa_dbus_pending, backend->pending, p);
    dbus_pending_call_set_notify(call, func, p, NULL);

    return p;
}

static void hf_audio_agent_register_reply(DBusPendingCall *pending, void *userdata) {
    DBusMessage *r;
    pa_dbus_pending *p;
    pa_bluetooth_backend *backend;

    pa_assert_se(p = userdata);
    pa_assert_se(backend = p->context_data);
    pa_assert_se(r = dbus_pending_call_steal_reply(pending));

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log_error("Failed to register as a handsfree audio agent with ofono: %s: %s",
                     dbus_message_get_error_name(r), pa_dbus_get_error_message(r));
        goto finish;
    }

    backend->ofono_bus_id = pa_xstrdup(dbus_message_get_sender(r));

    /* TODO: List all HandsfreeAudioCard objects */

finish:
    dbus_message_unref(r);

    PA_LLIST_REMOVE(pa_dbus_pending, backend->pending, p);
    pa_dbus_pending_free(p);
}

static void hf_audio_agent_register(pa_bluetooth_backend *hf) {
    DBusMessage *m;
    uint8_t codecs[2];
    const uint8_t *pcodecs = codecs;
    int ncodecs = 0;
    const char *path = HF_AUDIO_AGENT_PATH;

    pa_assert(hf);

    pa_assert_se(m = dbus_message_new_method_call(OFONO_SERVICE, "/", HF_AUDIO_MANAGER_INTERFACE, "Register"));

    codecs[ncodecs++] = HFP_AUDIO_CODEC_CVSD;

    pa_assert_se(dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &pcodecs, ncodecs,
                                          DBUS_TYPE_INVALID));

    hf_dbus_send_and_add_to_pending(hf, m, hf_audio_agent_register_reply, NULL);
}

static void hf_audio_agent_unregister(pa_bluetooth_backend *backend) {
    DBusMessage *m;
    const char *path = HF_AUDIO_AGENT_PATH;

    pa_assert(backend);
    pa_assert(backend->connection);

    if (backend->ofono_bus_id) {
        pa_assert_se(m = dbus_message_new_method_call(backend->ofono_bus_id, "/", HF_AUDIO_MANAGER_INTERFACE, "Unregister"));
        pa_assert_se(dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID));
        pa_assert_se(dbus_connection_send(pa_dbus_connection_get(backend->connection), m, NULL));

        pa_xfree(backend->ofono_bus_id);
        backend->ofono_bus_id = NULL;
    }
}

static DBusHandlerResult filter_cb(DBusConnection *bus, DBusMessage *m, void *data) {
    const char *sender;
    pa_bluetooth_backend *backend = data;

    pa_assert(bus);
    pa_assert(m);
    pa_assert(backend);

    sender = dbus_message_get_sender(m);
    if (!pa_safe_streq(backend->ofono_bus_id, sender) && !pa_streq("org.freedesktop.DBus", sender))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusMessage *hf_audio_agent_release(DBusConnection *c, DBusMessage *m, void *data) {
    DBusMessage *r;
    const char *sender;
    pa_bluetooth_backend *backend = data;

    pa_assert(backend);

    sender = dbus_message_get_sender(m);
    if (!pa_safe_streq(backend->ofono_bus_id, sender)) {
        pa_assert_se(r = dbus_message_new_error(m, "org.ofono.Error.NotAllowed", "Operation is not allowed by this sender"));
        return r;
    }

    r = dbus_message_new_error(m, "org.ofono.Error.NotImplemented", "Operation is not implemented");
    return r;
}

static DBusMessage *hf_audio_agent_new_connection(DBusConnection *c, DBusMessage *m, void *data) {
    DBusMessage *r;
    const char *sender;
    pa_bluetooth_backend *backend = data;

    pa_assert(backend);

    sender = dbus_message_get_sender(m);
    if (!pa_safe_streq(backend->ofono_bus_id, sender)) {
        pa_assert_se(r = dbus_message_new_error(m, "org.ofono.Error.NotAllowed", "Operation is not allowed by this sender"));
        return r;
    }

    r = dbus_message_new_error(m, "org.ofono.Error.NotImplemented", "Operation is not implemented");
    return r;
}

static DBusHandlerResult hf_audio_agent_handler(DBusConnection *c, DBusMessage *m, void *data) {
    pa_bluetooth_backend *backend = data;
    DBusMessage *r = NULL;
    const char *path, *interface, *member;

    pa_assert(backend);

    path = dbus_message_get_path(m);
    interface = dbus_message_get_interface(m);
    member = dbus_message_get_member(m);

    if (!pa_streq(path, HF_AUDIO_AGENT_PATH))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    pa_log_debug("dbus: path=%s, interface=%s, member=%s", path, interface, member);

    if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        const char *xml = HF_AUDIO_AGENT_XML;

        pa_assert_se(r = dbus_message_new_method_return(m));
        pa_assert_se(dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID));

    } else if (dbus_message_is_method_call(m, HF_AUDIO_AGENT_INTERFACE, "NewConnection"))
        r = hf_audio_agent_new_connection(c, m, data);
    else if (dbus_message_is_method_call(m, HF_AUDIO_AGENT_INTERFACE, "Release"))
        r = hf_audio_agent_release(c, m, data);
    else
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (r) {
        pa_assert_se(dbus_connection_send(pa_dbus_connection_get(backend->connection), r, NULL));
        dbus_message_unref(r);
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

pa_bluetooth_backend *pa_bluetooth_backend_new(pa_core *c, pa_bluetooth_discovery *y) {
    pa_bluetooth_backend *backend;
    DBusError err;
    static const DBusObjectPathVTable vtable_hf_audio_agent = {
        .message_function = hf_audio_agent_handler,
    };

    pa_assert(c);

    backend = pa_xnew0(pa_bluetooth_backend, 1);
    backend->core = c;
    backend->discovery = y;
    backend->cards = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    dbus_error_init(&err);

    if (!(backend->connection = pa_dbus_bus_get(c, DBUS_BUS_SYSTEM, &err))) {
        pa_log("Failed to get D-Bus connection: %s", err.message);
        dbus_error_free(&err);
        pa_xfree(backend);
        return NULL;
    }

    /* dynamic detection of handsfree audio cards */
    if (!dbus_connection_add_filter(pa_dbus_connection_get(backend->connection), filter_cb, backend, NULL)) {
        pa_log_error("Failed to add filter function");
        pa_dbus_connection_unref(backend->connection);
        pa_xfree(backend);
        return NULL;
    }

    if (pa_dbus_add_matches(pa_dbus_connection_get(backend->connection), &err,
            "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',"
            "arg0='" OFONO_SERVICE "'",
            "type='signal',sender='" OFONO_SERVICE "',interface='" HF_AUDIO_MANAGER_INTERFACE "',member='CardAdded'",
            "type='signal',sender='" OFONO_SERVICE "',interface='" HF_AUDIO_MANAGER_INTERFACE "',member='CardRemoved'",
            NULL) < 0) {
        pa_log("Failed to add oFono D-Bus matches: %s", err.message);
        dbus_connection_remove_filter(pa_dbus_connection_get(backend->connection), filter_cb, backend);
        pa_dbus_connection_unref(backend->connection);
        pa_xfree(backend);
        return NULL;
    }

    pa_assert_se(dbus_connection_register_object_path(pa_dbus_connection_get(backend->connection), HF_AUDIO_AGENT_PATH,
                                                      &vtable_hf_audio_agent, backend));

    hf_audio_agent_register(backend);

    return backend;
}

void pa_bluetooth_backend_free(pa_bluetooth_backend *backend) {
    pa_assert(backend);

    pa_dbus_free_pending_list(&backend->pending);

    hf_audio_agent_unregister(backend);

    dbus_connection_unregister_object_path(pa_dbus_connection_get(backend->connection), HF_AUDIO_AGENT_PATH);

    pa_dbus_remove_matches(pa_dbus_connection_get(backend->connection),
            "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',"
            "arg0='" OFONO_SERVICE "'",
            "type='signal',sender='" OFONO_SERVICE "',interface='" HF_AUDIO_MANAGER_INTERFACE "',member='CardAdded'",
            "type='signal',sender='" OFONO_SERVICE "',interface='" HF_AUDIO_MANAGER_INTERFACE "',member='CardRemoved'",
            NULL);

    dbus_connection_remove_filter(pa_dbus_connection_get(backend->connection), filter_cb, backend);

    pa_dbus_connection_unref(backend->connection);

    pa_hashmap_free(backend->cards);

    pa_xfree(backend);
}
