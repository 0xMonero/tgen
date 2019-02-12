/*
 * The Shadow Simulator
 * See LICENSE for licensing information
 */

#include "tgen.h"

struct _TGenIO {
    gint epollD;

    GHashTable* children;

    gint refcount;
    guint magic;
};

typedef struct _TGenIOChild {
    gint descriptor;
    TGenIO_notifyEventFunc notify;
    TGenIO_notifyCheckTimeoutFunc checkTimeout;
    gpointer data;
    GDestroyNotify destructData;
} TGenIOChild;

static TGenIOChild* _tgeniochild_new(gint descriptor, TGenIO_notifyEventFunc notify,
        TGenIO_notifyCheckTimeoutFunc checkTimeout, gpointer data, GDestroyNotify destructData) {
    TGenIOChild* child = g_new0(TGenIOChild, 1);
    child->descriptor = descriptor;
    child->notify = notify;
    child->checkTimeout = checkTimeout;
    child->data = data;
    child->destructData = destructData;
    return child;
}

static void _tgeniochild_free(TGenIOChild* child) {
    g_assert(child);
    if(child->destructData && child->data) {
        child->destructData(child->data);
    }
    memset(child, 0, sizeof(TGenIOChild));
    g_free(child);
}

TGenIO* tgenio_new() {
    /* create an epoll descriptor so we can manage events */
    gint epollD = epoll_create(1);
    if (epollD < 0) {
        tgen_critical("epoll_create(): returned %i error %i: %s", epollD, errno, g_strerror(errno));
        return NULL;
    }

    /* allocate the new server object and return it */
    TGenIO* io = g_new0(TGenIO, 1);
    io->magic = TGEN_MAGIC;
    io->refcount = 1;

    io->children = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)_tgeniochild_free);

    io->epollD = epollD;

    return io;
}

static void _tgenio_free(TGenIO* io) {
    TGEN_ASSERT(io);
    g_assert(io->refcount == 0);

    if(io->children) {
        g_hash_table_destroy(io->children);
    }

    io->magic = 0;
    g_free(io);
}

void tgenio_ref(TGenIO* io) {
    TGEN_ASSERT(io);
    io->refcount++;
}

void tgenio_unref(TGenIO* io) {
    TGEN_ASSERT(io);
    if(--(io->refcount) <= 0) {
        _tgenio_free(io);
    }
}

void tgenio_deregister(TGenIO* io, gint descriptor) {
    TGEN_ASSERT(io);

    gint result = epoll_ctl(io->epollD, EPOLL_CTL_DEL, descriptor, NULL);
    if(result != 0) {
        tgen_warning("epoll_ctl(): epoll %i descriptor %i returned %i error %i: %s",
                io->epollD, descriptor, result, errno, g_strerror(errno));
    }

    g_hash_table_remove(io->children, GINT_TO_POINTER(descriptor));
}

gboolean tgenio_register(TGenIO* io, gint descriptor, TGenIO_notifyEventFunc notify,
        TGenIO_notifyCheckTimeoutFunc checkTimeout, gpointer data, GDestroyNotify destructData) {
    TGEN_ASSERT(io);

    if(g_hash_table_lookup(io->children, GINT_TO_POINTER(descriptor))) {
        tgenio_deregister(io, descriptor);
        tgen_warning("removed existing entry at descriptor %i to make room for a new one", descriptor);
    }

    /* start watching */
    struct epoll_event ee;
    memset(&ee, 0, sizeof(struct epoll_event));
    ee.events = EPOLLIN|EPOLLOUT;
    ee.data.fd = descriptor;

    gint result = epoll_ctl(io->epollD, EPOLL_CTL_ADD, descriptor, &ee);

    if (result != 0) {
        tgen_critical("epoll_ctl(): epoll %i socket %i returned %i error %i: %s",
                io->epollD, descriptor, result, errno, g_strerror(errno));
        return FALSE;
    }

    TGenIOChild* child = _tgeniochild_new(descriptor, notify, checkTimeout, data, destructData);
    g_hash_table_replace(io->children, GINT_TO_POINTER(child->descriptor), child);

    return TRUE;
}

static void _tgenio_helper(TGenIO* io, TGenIOChild* child, gboolean in, gboolean out) {
    TGEN_ASSERT(io);
    g_assert(child);

    TGenEvent inEvents = TGEN_EVENT_NONE;

    /* check if we need read flag */
    if(in) {
        tgen_debug("descriptor %i is readable", child->descriptor);
        inEvents |= TGEN_EVENT_READ;
    }

    /* check if we need write flag */
    if(out) {
        tgen_debug("descriptor %i is writable", child->descriptor);
        inEvents |= TGEN_EVENT_WRITE;
    }

    /* activate the transfer */
    TGenEvent outEvents = child->notify(child->data, child->descriptor, inEvents);

    /* now check if we should update our epoll events */
    if(outEvents & TGEN_EVENT_DONE) {
        tgenio_deregister(io, child->descriptor);
    } else if(inEvents != outEvents) {
        guint32 newEvents = 0;
        if(outEvents & TGEN_EVENT_READ) {
            newEvents |= EPOLLIN;
        }
        if(outEvents & TGEN_EVENT_WRITE) {
            newEvents |= EPOLLOUT;
        }

        struct epoll_event ee;
        memset(&ee, 0, sizeof(struct epoll_event));
        ee.events = newEvents;
        ee.data.fd = child->descriptor;

        gint result = epoll_ctl(io->epollD, EPOLL_CTL_MOD, child->descriptor, &ee);
        if(result != 0) {
            tgen_warning("epoll_ctl(): epoll %i descriptor %i returned %i error %i: %s",
                    io->epollD, child->descriptor, result, errno, g_strerror(errno));
        }
    }
}

gint tgenio_loopOnce(TGenIO* io, gint maxEvents) {
    TGEN_ASSERT(io);

    /* storage for collecting events from our epoll descriptor */
    struct epoll_event* epevs = g_new(struct epoll_event, maxEvents);

    /* collect all events that are ready */
    gint nfds = epoll_wait(io->epollD, epevs, maxEvents, 0);

    if(nfds < 0) {
        tgen_critical("epoll_wait(): epoll %i returned %i error %i: %s",
                io->epollD, nfds, errno, g_strerror(errno));
        g_free(epevs);

        /* we didn't process any events */
        return 0;
    }

    /* activate correct component for every descriptor that's ready. */
    for (gint i = 0; i < nfds; i++) {
        gboolean in = (epevs[i].events & EPOLLIN) ? TRUE : FALSE;
        gboolean out = (epevs[i].events & EPOLLOUT) ? TRUE : FALSE;

        gint eventDescriptor = epevs[i].data.fd;
        TGenIOChild* child = g_hash_table_lookup(io->children, GINT_TO_POINTER(eventDescriptor));

        if(child) {
            _tgenio_helper(io, child, in, out);
        } else {
            /* we don't currently have a child for the event descriptor, stop paying attention to it */
            tgen_warning("can't find child for descriptor %i, canceling event now", eventDescriptor);
            tgenio_deregister(io, eventDescriptor);
        }
    }

    g_free(epevs);
    return nfds;
}

void tgenio_checkTimeouts(TGenIO* io) {
    TGEN_ASSERT(io);

    /* TODO this was a quick polling approach to checking for timeouts, which
     * could be more efficient if replaced with an asynchronous notify design. */
    GList* items = g_hash_table_get_values(io->children);
    GList* item = g_list_first(items);

    while(item) {
        TGenIOChild* child = item->data;
        if(child && child->checkTimeout) {
            /* this calls tgentransfer_onCheckTimeout to check and handle if a timeout is present */
            gboolean hasTimeout = child->checkTimeout(child->data, child->descriptor);
            if(hasTimeout) {
                tgenio_deregister(io, child->descriptor);
            }
        }
        item = g_list_next(item);
    }

    if(items != NULL) {
        g_list_free(items);
    }
}

/** Modify the tgenio epoll instance so that it notifies us when the given
 * events occur on the given descriptor. */
void
tgenio_setEvents(TGenIO *io, gint descriptor, TGenEvent events)
{
    /* FIXME
     * RGJ: there is an error here due to the new SCHED transfer code.
     * If a SCHED wants to delay sending, then it uses this function to
     * disable events and then it pauses for a while. In the meantime,
     * the transfer might close because of a timeout or stallout, and
     * that event will destroy the descriptor and deregister it from
     * the children table. Then the SCHED pause timer expires and this
     * function is called with a descriptor that is no longer valid, or
     * worse, with a descriptor that has been re-assigned to a completely
     * different socket.
     *
     * For now I'm going to add the children table check, but this needs
     * to be redesigned to avoid this problem in the first place.
     * (We probably need to ref and store the transport object in the
     * pause event, and then when it expires, we can check a flag somewhere
     * to see if it is in an error state and only continue if it's not.)
     */
    if(g_hash_table_lookup(io->children, GINT_TO_POINTER(descriptor)) == NULL) {
        tgen_warning("transport descriptor %i cannot be found", descriptor);
        return;
    }

    struct epoll_event ee;
    memset(&ee, 0, sizeof(struct epoll_event));
    if (events & TGEN_EVENT_READ) {
        ee.events |= EPOLLIN;
    }
    if (events & TGEN_EVENT_WRITE) {
        ee.events |= EPOLLOUT;
    }
    ee.data.fd = descriptor;
    gint result = epoll_ctl(io->epollD, EPOLL_CTL_MOD, descriptor, &ee);
    if (result != 0) {
        tgen_warning("epoll_ctl(): epoll %i descriptor %i returned %i error %i: %s",
                io->epollD, descriptor, result, errno, g_strerror(errno));
    }

}

gint tgenio_getEpollDescriptor(TGenIO* io) {
    TGEN_ASSERT(io);
    return io->epollD;
}
