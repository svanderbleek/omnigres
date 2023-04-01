#include <signal.h>
#include <stdatomic.h>

#include <h2o.h>

#include "event_loop.h"

#ifdef POSTGRES_H
#error "event_loop.c can't use any Postgres API"
#endif

h2o_evloop_t *worker_event_loop;

atomic_bool worker_running;
atomic_bool worker_reload;
bool event_loop_suspended;
bool event_loop_resumed;
pthread_mutex_t event_loop_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t event_loop_resume_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t event_loop_resume_cond_ack = PTHREAD_COND_INITIALIZER;

h2o_multithread_receiver_t event_loop_receiver;
h2o_multithread_queue_t *event_loop_queue;

static size_t requests_in_flight = 0;

typedef struct {
  h2o_multithread_message_t super;
  request_message_t *reqmsg;
  size_t bufcnt;
  h2o_send_state_t state;
  h2o_sendvec_t vecs;
} send_message_t;

static void h2o_queue_send(request_message_t *msg, h2o_iovec_t *bufs, size_t bufcnt,
                           h2o_send_state_t state) {
  if (state == H2O_SEND_STATE_FINAL) {
    requests_in_flight--;
  }
  h2o_req_t *req = msg->req;
  if (req == NULL) {
    // The connection is gone, bail
    return;
  }

  send_message_t *message = malloc(sizeof(*message) - (sizeof(h2o_sendvec_t) * (bufcnt - 1)));
  size_t i;
  message->reqmsg = msg;
  message->bufcnt = bufcnt;
  message->state = state;

  for (i = 0; i != bufcnt; ++i)
    h2o_sendvec_init_raw(&message->vecs + i, bufs[i].base, bufs[i].len);

  message->super = (h2o_multithread_message_t){{NULL}};

  h2o_multithread_send_message(&event_loop_receiver, &message->super);
}

void h2o_queue_send_inline(request_message_t *msg, const char *body, size_t len) {
  h2o_req_t *req = msg->req;
  if (req == NULL) {
    // The connection is gone, bail
    return;
  }
  static h2o_generator_t generator = {NULL, NULL};

  h2o_iovec_t buf = h2o_strdup(&req->pool, body, len);

  /* the function intentionally does not set the content length, since it may be used for generating
   * 304 response, etc. */
  /* req->res.content_length = buf.len; */

  h2o_start_response(req, &generator);

  if (h2o_memis(req->input.method.base, req->input.method.len, H2O_STRLIT("HEAD")))
    h2o_queue_send(msg, NULL, 0, H2O_SEND_STATE_FINAL);
  else
    h2o_queue_send(msg, &buf, 1, H2O_SEND_STATE_FINAL);
}

static void on_message(h2o_multithread_receiver_t *receiver, h2o_linklist_t *messages) {
  while (!h2o_linklist_is_empty(messages)) {
    h2o_multithread_message_t *message =
        H2O_STRUCT_FROM_MEMBER(h2o_multithread_message_t, link, messages->next);
    send_message_t *send_msg = (send_message_t *)messages->next;
    request_message_t *reqmsg = send_msg->reqmsg;
    pthread_mutex_lock(&reqmsg->mutex);
    if (reqmsg->req == NULL) {
      // Connection is gone, bail
      // Can release request message
      free(reqmsg);
      goto done;
    }
    h2o_sendvec(reqmsg->req, &send_msg->vecs, send_msg->bufcnt, send_msg->state);
  done:
    pthread_mutex_unlock(&reqmsg->mutex);
    h2o_linklist_unlink(&message->link);
    free(send_msg);
  }
}

void *event_loop(void *arg) {
  assert(worker_event_loop != NULL);
  assert(handler_queue != NULL);
  assert(event_loop_queue != NULL);

  h2o_multithread_register_receiver(event_loop_queue, &event_loop_receiver, on_message);

  bool running = atomic_load(&worker_running);
  bool reload = atomic_load(&worker_reload);

  while (running) {
    if (event_loop_suspended) {
      pthread_mutex_lock(&event_loop_mutex);
      while (event_loop_suspended) {
        pthread_cond_wait(&event_loop_resume_cond, &event_loop_mutex);
      }
      event_loop_resumed = true;
      pthread_cond_signal(&event_loop_resume_cond_ack);
      pthread_mutex_unlock(&event_loop_mutex);
    }

    running = true;
    reload = false;

    while (running && !reload) {
      while ((running = atomic_load(&worker_running)) && !(reload = atomic_load(&worker_reload)) &&
             h2o_evloop_run(worker_event_loop, INT32_MAX) == 0)
        ;
    }

    // Make sure request handler no longer waits
    pthread_mutex_lock(&event_loop_mutex);
    event_loop_resumed = false;
    event_loop_suspended = true;
    pthread_cond_signal(&event_loop_resume_cond_ack);
    pthread_mutex_unlock(&event_loop_mutex);
  }
  return NULL;
}

void on_accept(h2o_socket_t *listener, const char *err) {
  if (false /* TODO */) {
    // Don't accept new connections if this instance is busy as we'd likely
    // have to proxy it
    return;
  }
  h2o_socket_t *sock;

  if (err != NULL) {
    return;
  }

  if ((sock = h2o_evloop_socket_accept(listener)) == NULL) {
    return;
  }
  h2o_accept(listener_accept_ctx(listener), sock);
}

void req_dispose(void *ptr) {
  request_message_t **message_ptr = (request_message_t **)ptr;
  request_message_t *message = *message_ptr;
  pthread_mutex_lock(&message->mutex);
  message->req = NULL;
  pthread_mutex_unlock(&message->mutex);
}

int event_loop_req_handler(h2o_handler_t *self, h2o_req_t *req) {
  if (requests_in_flight > 0 && req->version >= 0x0200) {
    // If HTTP/2 or above is used, they can return results of requests
    // before the previous result is retrieved, so we'll try to proxy it
    // to another worker.
    h2o_req_overrides_t *overrides = h2o_mem_alloc_pool(&req->pool, h2o_req_overrides_t, 1);

    *overrides = (h2o_req_overrides_t){NULL};
    overrides->use_proxy_protocol = false;
    overrides->proxy_preserve_host = true;
    overrides->forward_close_connection = true;

    // request reprocess (note: path may become an empty string, to which one of the target URL
    // within the socketpool will be right-padded when lib/core/proxy connects to upstream; see
    // #1563)
    h2o_iovec_t path = h2o_build_destination(req, NULL, 0, 0);
    h2o_reprocess_request(req, req->method, req->scheme, req->authority, path, overrides, 0);
    return 0;
  }
  requests_in_flight++;
  request_message_t *msg = malloc(sizeof(*msg));
  msg->super = (h2o_multithread_message_t){{NULL}};
  msg->req = req;
  pthread_mutex_init(&msg->mutex, NULL);

  // Track request deallocation
  request_message_t **msgref =
      (request_message_t **)h2o_mem_alloc_shared(&req->pool, sizeof(msg), req_dispose);
  *msgref = msg;

  h2o_multithread_send_message(&handler_receiver, &msg->super);
  return 0;
}
