#ifndef foopulsesourcehfoo
#define foopulsesourcehfoo

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

typedef struct pa_source pa_source;

#include <inttypes.h>

#include <pulse/sample.h>
#include <pulse/channelmap.h>
#include <pulse/volume.h>

#include <pulsecore/core.h>
#include <pulsecore/idxset.h>
#include <pulsecore/memblock.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/asyncmsgq.h>
#include <pulsecore/msgobject.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/source-output.h>
#include <pulsecore/card.h>
#include <pulsecore/queue.h>
#include <pulsecore/thread-mq.h>

#define PA_MAX_OUTPUTS_PER_SOURCE 32

/* Returns true if source is linked: registered and accessible from client side. */
static inline pa_bool_t PA_SOURCE_IS_LINKED(pa_source_state_t x) {
    return x == PA_SOURCE_RUNNING || x == PA_SOURCE_IDLE || x == PA_SOURCE_SUSPENDED;
}

struct pa_source {
    pa_msgobject parent;

    uint32_t index;
    pa_core *core;

    pa_source_state_t state;
    pa_source_flags_t flags;
    pa_suspend_cause_t suspend_cause;

    char *name;
    char *driver;                             /* may be NULL */
    pa_proplist *proplist;

    pa_module *module;                        /* may be NULL */
    pa_card *card;                            /* may be NULL */

    pa_sample_spec sample_spec;
    pa_channel_map channel_map;

    pa_idxset *outputs;
    unsigned n_corked;
    pa_sink *monitor_of;                     /* may be NULL */

    pa_volume_t base_volume; /* shall be constant */
    unsigned n_volume_steps; /* shall be constant */

    pa_cvolume virtual_volume, soft_volume;
    pa_bool_t muted:1;

    pa_bool_t refresh_volume:1;
    pa_bool_t refresh_muted:1;

    pa_bool_t save_port:1;
    pa_bool_t save_volume:1;
    pa_bool_t save_muted:1;

    pa_asyncmsgq *asyncmsgq;
    pa_rtpoll *rtpoll;

    pa_memchunk silence;

    pa_usec_t fixed_latency; /* for sources with PA_SOURCE_DYNAMIC_LATENCY this is 0 */

    pa_hashmap *ports;
    pa_device_port *active_port;

    /* Called when the main loop requests a state change. Called from
     * main loop context. If returns -1 the state change will be
     * inhibited */
    int (*set_state)(pa_source*source, pa_source_state_t state); /* may be NULL */

    /* Callled when the volume is queried. Called from main loop
     * context. If this is NULL a PA_SOURCE_MESSAGE_GET_VOLUME message
     * will be sent to the IO thread instead. If refresh_volume is
     * FALSE neither this function is called nor a message is sent. */
    void (*get_volume)(pa_source *s);         /* dito */

    /* Called when the volume shall be changed. Called from main loop
     * context. If this is NULL a PA_SOURCE_MESSAGE_SET_VOLUME message
     * will be sent to the IO thread instead. */
    void (*set_volume)(pa_source *s);         /* dito */

    /* Called when the mute setting is queried. Called from main loop
     * context. If this is NULL a PA_SOURCE_MESSAGE_GET_MUTE message
     * will be sent to the IO thread instead. If refresh_mute is
     * FALSE neither this function is called nor a message is sent.*/
    void (*get_mute)(pa_source *s);           /* dito */

    /* Called when the mute setting shall be changed. Called from main
     * loop context. If this is NULL a PA_SOURCE_MESSAGE_SET_MUTE
     * message will be sent to the IO thread instead. */
    void (*set_mute)(pa_source *s);           /* dito */

    /* Called when a the requested latency is changed. Called from IO
     * thread context. */
    void (*update_requested_latency)(pa_source *s); /* dito */

    /* Called whenever the port shall be changed. Called from main
     * thread. */
    int (*set_port)(pa_source *s, pa_device_port *port); /*dito */

    /* Contains copies of the above data so that the real-time worker
     * thread can work without access locking */
    struct {
        pa_source_state_t state;
        pa_hashmap *outputs;

        pa_cvolume soft_volume;
        pa_bool_t soft_muted:1;

        pa_bool_t requested_latency_valid:1;
        pa_usec_t requested_latency;

        /* Then number of bytes this source will be rewound for at
         * max. (Only used on monitor sources) */
        size_t max_rewind;

        pa_usec_t min_latency; /* we won't go below this latency */
        pa_usec_t max_latency; /* An upper limit for the latencies */
    } thread_info;

    void *userdata;
};

PA_DECLARE_CLASS(pa_source);
#define PA_SOURCE(s) pa_source_cast(s)

typedef enum pa_source_message {
    PA_SOURCE_MESSAGE_ADD_OUTPUT,
    PA_SOURCE_MESSAGE_REMOVE_OUTPUT,
    PA_SOURCE_MESSAGE_GET_VOLUME,
    PA_SOURCE_MESSAGE_SET_VOLUME,
    PA_SOURCE_MESSAGE_GET_MUTE,
    PA_SOURCE_MESSAGE_SET_MUTE,
    PA_SOURCE_MESSAGE_GET_LATENCY,
    PA_SOURCE_MESSAGE_GET_REQUESTED_LATENCY,
    PA_SOURCE_MESSAGE_SET_STATE,
    PA_SOURCE_MESSAGE_ATTACH,
    PA_SOURCE_MESSAGE_DETACH,
    PA_SOURCE_MESSAGE_SET_LATENCY_RANGE,
    PA_SOURCE_MESSAGE_GET_LATENCY_RANGE,
    PA_SOURCE_MESSAGE_GET_MAX_REWIND,
    PA_SOURCE_MESSAGE_SET_MAX_REWIND,
    PA_SOURCE_MESSAGE_MAX
} pa_source_message_t;

typedef struct pa_source_new_data {
    char *name;
    pa_proplist *proplist;

    const char *driver;
    pa_module *module;
    pa_card *card;

    pa_hashmap *ports;
    char *active_port;

    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
    pa_cvolume volume;
    pa_bool_t muted:1;

    pa_bool_t volume_is_set:1;
    pa_bool_t muted_is_set:1;
    pa_bool_t sample_spec_is_set:1;
    pa_bool_t channel_map_is_set:1;

    pa_bool_t namereg_fail:1;

    pa_bool_t save_port:1;
    pa_bool_t save_volume:1;
    pa_bool_t save_muted:1;
} pa_source_new_data;

pa_source_new_data* pa_source_new_data_init(pa_source_new_data *data);
void pa_source_new_data_set_name(pa_source_new_data *data, const char *name);
void pa_source_new_data_set_sample_spec(pa_source_new_data *data, const pa_sample_spec *spec);
void pa_source_new_data_set_channel_map(pa_source_new_data *data, const pa_channel_map *map);
void pa_source_new_data_set_volume(pa_source_new_data *data, const pa_cvolume *volume);
void pa_source_new_data_set_muted(pa_source_new_data *data, pa_bool_t mute);
void pa_source_new_data_set_port(pa_source_new_data *data, const char *port);
void pa_source_new_data_done(pa_source_new_data *data);

/*** To be called exclusively by the source driver, from main context */

pa_source* pa_source_new(
        pa_core *core,
        pa_source_new_data *data,
        pa_source_flags_t flags);

void pa_source_put(pa_source *s);
void pa_source_unlink(pa_source *s);

void pa_source_set_description(pa_source *s, const char *description);
void pa_source_set_asyncmsgq(pa_source *s, pa_asyncmsgq *q);
void pa_source_set_rtpoll(pa_source *s, pa_rtpoll *p);

void pa_source_set_max_rewind(pa_source *s, size_t max_rewind);
void pa_source_set_latency_range(pa_source *s, pa_usec_t min_latency, pa_usec_t max_latency);
void pa_source_set_fixed_latency(pa_source *s, pa_usec_t latency);

void pa_source_detach(pa_source *s);
void pa_source_attach(pa_source *s);

void pa_source_set_soft_volume(pa_source *s, const pa_cvolume *volume);
void pa_source_volume_changed(pa_source *s, const pa_cvolume *new_volume);
void pa_source_mute_changed(pa_source *s, pa_bool_t new_muted);

int pa_source_sync_suspend(pa_source *s);

/*** May be called by everyone, from main context */

/* The returned value is supposed to be in the time domain of the sound card! */
pa_usec_t pa_source_get_latency(pa_source *s);
pa_usec_t pa_source_get_requested_latency(pa_source *s);
void pa_source_get_latency_range(pa_source *s, pa_usec_t *min_latency, pa_usec_t *max_latency);

size_t pa_source_get_max_rewind(pa_source *s);

int pa_source_update_status(pa_source*s);
int pa_source_suspend(pa_source *s, pa_bool_t suspend, pa_suspend_cause_t cause);
int pa_source_suspend_all(pa_core *c, pa_bool_t suspend, pa_suspend_cause_t cause);

void pa_source_set_volume(pa_source *source, const pa_cvolume *volume, pa_bool_t save);
const pa_cvolume *pa_source_get_volume(pa_source *source, pa_bool_t force_refresh);
void pa_source_set_mute(pa_source *source, pa_bool_t mute, pa_bool_t save);
pa_bool_t pa_source_get_mute(pa_source *source, pa_bool_t force_refresh);

pa_bool_t pa_source_update_proplist(pa_source *s, pa_update_mode_t mode, pa_proplist *p);

int pa_source_set_port(pa_source *s, const char *name, pa_bool_t save);

unsigned pa_source_linked_by(pa_source *s); /* Number of connected streams */
unsigned pa_source_used_by(pa_source *s); /* Number of connected streams that are not corked */
unsigned pa_source_check_suspend(pa_source *s); /* Returns how many streams are active that don't allow suspensions */
#define pa_source_get_state(s) ((pa_source_state_t) (s)->state)

/* Moves all inputs away, and stores them in pa_queue */
pa_queue *pa_source_move_all_start(pa_source *s, pa_queue *q);
void pa_source_move_all_finish(pa_source *s, pa_queue *q, pa_bool_t save);
void pa_source_move_all_fail(pa_queue *q);

/*** To be called exclusively by the source driver, from IO context */

void pa_source_post(pa_source*s, const pa_memchunk *chunk);
void pa_source_post_direct(pa_source*s, pa_source_output *o, const pa_memchunk *chunk);
void pa_source_process_rewind(pa_source *s, size_t nbytes);

int pa_source_process_msg(pa_msgobject *o, int code, void *userdata, int64_t, pa_memchunk *chunk);

void pa_source_attach_within_thread(pa_source *s);
void pa_source_detach_within_thread(pa_source *s);

pa_usec_t pa_source_get_requested_latency_within_thread(pa_source *s);

void pa_source_set_max_rewind_within_thread(pa_source *s, size_t max_rewind);
void pa_source_set_latency_range_within_thread(pa_source *s, pa_usec_t min_latency, pa_usec_t max_latency);

/*** To be called exclusively by source output drivers, from IO context */

void pa_source_invalidate_requested_latency(pa_source *s);
pa_usec_t pa_source_get_latency_within_thread(pa_source *s);

#define pa_source_assert_io_context(s) \
    pa_assert(pa_thread_mq_get() || !PA_SOURCE_IS_LINKED((s)->state))

#endif
