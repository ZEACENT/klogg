/* Effects for verbatim pinned ostrace definitions, not a copied implementation.
 * Failure cases use a manual thread effect. Worker cases use real pthreads with
 * condition-variable barriers, including callback -> stop/join -> free ordering.
 * These schedules establish ownership, not freedom from all possible data races.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define THREAD_T_NULL NULL
#define debug_info(...) ((void)0)
/* The scripted worker supplies a little-endian type-2 record on native macOS/Linux. */
#define be32toh(value) (value)
#define le32toh(value) (value)
typedef void *THREAD_T;
typedef void *service_client_t;
typedef enum {
    SERVICE_E_SUCCESS, SERVICE_E_INVALID_ARG, SERVICE_E_MUX_ERROR,
    SERVICE_E_SSL_ERROR, SERVICE_E_NOT_ENOUGH_DATA, SERVICE_E_TIMEOUT
} service_error_t;
typedef struct test_plist { const char *status; } *plist_t;

/* PINNED_DECLARATIONS */

static int failures;
static const char *wrapper_name;
static const char *scenario_name;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s/%s: %s (line %d)\n", wrapper_name, scenario_name, #condition, __LINE__); \
    ++failures; \
} } while (0)

static int malloc_failure, thread_failure, send_failure, reply_rejected;
static int receive_failure, missing_status, invalid_status, with_options;
static int running_worker, natural_terminal, free_in_callback;
static int worker_allocations, worker_frees, allocation_attempts;
static int payload_allocations, payload_frees;
static int thread_attempts, joins, thread_releases, service_releases;
static int sends, replies, status_reads, plist_count, option_merges;
static int data_callbacks, terminal_callbacks, reader_calls;
static void *worker_memory, *payload_memory, *client_memory;
static void *(*pending_worker)(void *);
static void *pending_argument;
static pthread_t native_thread;
static pthread_mutex_t barrier_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t barrier_condition = PTHREAD_COND_INITIALIZER;
static int callback_entered, callback_exited, join_entered, worker_done;
static const char payload[] = "borrowed-payload";

static void *test_malloc(size_t size)
{
    ++allocation_attempts;
    CHECK(status_reads == 1);
    if (thread_attempts) {
        CHECK(size == sizeof(payload) && payload_memory == NULL);
        payload_memory = malloc(size);
        if (!payload_memory) abort();
        ++payload_allocations;
        return payload_memory;
    }
    if (malloc_failure) return NULL;
    CHECK(worker_memory == NULL);
    worker_memory = malloc(size);
    if (!worker_memory) abort();
    ++worker_allocations;
    return worker_memory;
}
static void test_free(void *memory)
{
    if (!memory) return;
    if (memory == worker_memory) {
        ++worker_frees;
        worker_memory = NULL;
    } else if (memory == payload_memory) {
        CHECK(callback_exited);
        ++payload_frees;
        payload_memory = NULL;
    } else {
        CHECK(memory == client_memory);
        if (running_worker) CHECK(worker_done && callback_exited);
        client_memory = NULL;
    }
    free(memory);
}
static plist_t plist_new_dict(void)
{
    plist_t result = calloc(1, sizeof(*result));
    if (!result) abort();
    ++plist_count;
    return result;
}
static plist_t plist_new_uint(uint64_t value) { (void)value; return plist_new_dict(); }
static plist_t plist_new_string(const char *value)
{
    plist_t result = plist_new_dict();
    result->status = value;
    return result;
}
static void plist_free(plist_t value)
{
    if (value) { --plist_count; free(value); }
}
static void plist_dict_set_item(plist_t dict, const char *key, plist_t value)
{
    CHECK(dict != NULL && key != NULL);
    plist_free(value);
}
static void plist_dict_merge(plist_t *dict, plist_t options)
{
    CHECK(*dict != NULL && options != NULL);
    ++option_merges;
}
static plist_t plist_dict_get_item(plist_t dict, const char *key)
{
    CHECK(strcmp(key, "Status") == 0);
    return missing_status ? NULL : dict;
}
static const char *plist_get_string_ptr(plist_t value, uint64_t *length)
{
    (void)length;
    ++status_reads;
    return invalid_status ? NULL : value->status;
}
/* Negotiation I/O is deliberately substituted. This does not test the real
 * receive-plist allocation/parser, or transport framing on a physical device. */
static ostrace_error_t ostrace_send_plist(ostrace_client_t client, plist_t dict)
{
    CHECK(client->parent != NULL && dict != NULL);
    ++sends;
    return send_failure ? OSTRACE_E_MUX_ERROR : OSTRACE_E_SUCCESS;
}
static ostrace_error_t ostrace_receive_plist(ostrace_client_t client, plist_t *reply)
{
    CHECK(client->parent != NULL);
    ++replies;
    if (receive_failure) return OSTRACE_E_MUX_ERROR;
    *reply = plist_new_string(reply_rejected ? "RequestDenied" : "RequestSuccessful");
    return OSTRACE_E_SUCCESS;
}
static service_error_t service_receive_with_timeout(
    service_client_t client, char *data, uint32_t size, uint32_t *received, unsigned int timeout)
{
    CHECK(client != NULL && timeout == OSTRACE_RECEIVE_POLL_INTERVAL_MS);
    ++reader_calls;
    *received = size;
    if (reader_calls == 1) {
        CHECK(size == 1);
        *data = 2;
    } else if (reader_calls == 2) {
        uint32_t length = sizeof(payload);
        CHECK(size == sizeof(length));
        memcpy(data, &length, sizeof(length));
    } else if (reader_calls == 3) {
        CHECK(size == sizeof(payload));
        memcpy(data, payload, sizeof(payload));
    } else {
        *received = 0;
        return SERVICE_E_MUX_ERROR;
    }
    return SERVICE_E_SUCCESS;
}
static service_error_t service_client_free(service_client_t client)
{
    CHECK(client != NULL);
    if (running_worker) CHECK(worker_done && callback_exited && joins == 1);
    ++service_releases;
    return SERVICE_E_SUCCESS;
}
static void *run_worker(void *argument)
{
    void *result = pending_worker(argument);
    pthread_mutex_lock(&barrier_mutex);
    worker_done = 1;
    pthread_cond_broadcast(&barrier_condition);
    pthread_mutex_unlock(&barrier_mutex);
    return result;
}
static int thread_new(THREAD_T *thread, void *(*worker)(void *), void *argument)
{
    ++thread_attempts;
    CHECK(*thread == THREAD_T_NULL && worker == ostrace_worker);
    if (thread_failure) {
        /* POSIX leaves the output unspecified on failure. */
        if (!strcmp(scenario_name, "thread-failure-mutated")) *thread = &thread_failure;
        return -1;
    }
    pending_worker = worker;
    pending_argument = argument;
    if (running_worker) {
        if (pthread_create(&native_thread, NULL, run_worker, argument)) abort();
        pthread_mutex_lock(&barrier_mutex);
        /* Force real worker execution BEFORE thread_new publishes its handle
         * and returns. Natural completion may also precede startup's return. */
        while (natural_terminal ? !worker_done : !callback_entered)
            pthread_cond_wait(&barrier_condition, &barrier_mutex);
        pthread_mutex_unlock(&barrier_mutex);
        *thread = &native_thread;
    } else {
        *thread = argument;
    }
    return 0;
}
static void thread_join(THREAD_T thread)
{
    ++joins;
    if (running_worker) {
        CHECK(thread == &native_thread);
        pthread_mutex_lock(&barrier_mutex);
        CHECK(callback_entered && client_memory != NULL && service_releases == 0);
        if (!natural_terminal) {
            CHECK(!callback_exited && payload_memory != NULL && worker_memory != NULL);
            CHECK(((ostrace_client_t)client_memory)->parent == NULL);
        }
        join_entered = 1;
        pthread_cond_broadcast(&barrier_condition);
        pthread_mutex_unlock(&barrier_mutex);
        CHECK(pthread_join(native_thread, NULL) == 0);
        CHECK(callback_exited && worker_done);
    } else {
        CHECK(thread == pending_argument && pending_worker != NULL);
        if (pending_worker) pending_worker(pending_argument);
    }
    pending_worker = NULL;
    pending_argument = NULL;
}
static void thread_free(THREAD_T thread) { CHECK(thread != NULL); ++thread_releases; }
static void activity_callback(const void *data, size_t size, void *context)
{
    pthread_mutex_lock(&barrier_mutex);
    CHECK(context == &failures && size == sizeof(payload) && data == payload_memory);
    CHECK(memcmp(data, payload, size) == 0);
    ++data_callbacks;
    callback_entered = 1;
    pthread_cond_broadcast(&barrier_condition);
    if (!natural_terminal) {
        while (!join_entered) pthread_cond_wait(&barrier_condition, &barrier_mutex);
        CHECK(payload_frees == 0 && client_memory != NULL && service_releases == 0);
        CHECK(memcmp(data, payload, size) == 0);
    }
    callback_exited = 1;
    pthread_mutex_unlock(&barrier_mutex);
}
static void record_callback(uint8_t type, const void *data, size_t size, void *context)
{
    CHECK(type == 2);
    activity_callback(data, size, context);
}
static void terminal_callback(ostrace_error_t error, void *context)
{
    CHECK(natural_terminal && error == OSTRACE_E_MUX_ERROR && context == &failures);
    CHECK(data_callbacks == 1 && callback_exited && payload_frees == 1);
    ++terminal_callbacks;
}

#define malloc test_malloc
#define free test_free
/* PINNED_FUNCTIONS */
#undef malloc
#undef free

static ostrace_error_t start(ostrace_client_t client)
{
    plist_t options = with_options ? plist_new_dict() : NULL;
    ostrace_error_t result;
    if (!strcmp(wrapper_name, "typed"))
        result = ostrace_start_activity_with_record_type_and_error(
            client, options, record_callback, terminal_callback, &failures);
    else if (!strcmp(wrapper_name, "legacy-error"))
        result = ostrace_start_activity_with_error(
            client, options, activity_callback, terminal_callback, &failures);
    else
        result = ostrace_start_activity(client, options, activity_callback, &failures);
    plist_free(options);
    return result;
}
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    wrapper_name = argv[1];
    scenario_name = argv[2];
    malloc_failure = !strcmp(scenario_name, "malloc-failure");
    thread_failure = !strcmp(scenario_name, "thread-failure") ||
        !strcmp(scenario_name, "thread-failure-mutated");
    send_failure = !strcmp(scenario_name, "send-failure");
    receive_failure = !strcmp(scenario_name, "receive-failure");
    reply_rejected = !strcmp(scenario_name, "reply-rejected");
    missing_status = !strcmp(scenario_name, "missing-status");
    invalid_status = !strcmp(scenario_name, "invalid-status");
    with_options = !strcmp(scenario_name, "with-options");
    natural_terminal = !strcmp(scenario_name, "natural-terminal");
    free_in_callback = !strcmp(scenario_name, "callback-free");
    running_worker = natural_terminal || free_in_callback || !strcmp(scenario_name, "callback-stop");
    const int successful = running_worker || with_options || !strcmp(scenario_name, "success");
    const int rejected = reply_rejected || missing_status || invalid_status;
    const int negotiated = !(send_failure || receive_failure || rejected);
    ostrace_client_t client = calloc(1, sizeof(*client));
    if (!client) abort();
    client_memory = client;
    client->parent = &service_releases;
    const ostrace_error_t result = start(client);
    const int ready = result == OSTRACE_E_SUCCESS;
    printf("%s/%s result=%d ready=%d reader_armed=%d\n",
           wrapper_name, scenario_name, result, ready, client->worker != THREAD_T_NULL);
    CHECK(ready == successful);
    CHECK((client->worker != THREAD_T_NULL) == successful);
    CHECK((pending_worker != NULL) == successful);
    CHECK(sends == 1 && replies == !send_failure);
    CHECK(status_reads == !(send_failure || receive_failure || missing_status));
    CHECK(allocation_attempts == negotiated + running_worker);
    CHECK(thread_attempts == (negotiated && !malloc_failure));
    CHECK(option_merges == with_options);
    if (malloc_failure || thread_failure) CHECK(result == OSTRACE_E_UNKNOWN_ERROR);
    if (send_failure || receive_failure) CHECK(result == OSTRACE_E_MUX_ERROR);
    if (rejected) CHECK(result == OSTRACE_E_REQUEST_FAILED);
    if (successful && !running_worker) {
        struct ostrace_worker_thread *worker = pending_argument;
        CHECK(worker->client == client && worker->user_data == &failures);
        CHECK(worker->record_cbfunc == (!strcmp(wrapper_name, "typed") ? record_callback : NULL));
        CHECK(worker->cbfunc == (!strcmp(wrapper_name, "typed") ? NULL : activity_callback));
        CHECK(worker->terminal_callback == (!strcmp(wrapper_name, "legacy") ? NULL : terminal_callback));
        CHECK(start(client) != OSTRACE_E_SUCCESS);
        CHECK(sends == 1 && allocation_attempts == 1);
    }
    CHECK(plist_count == 0);
    if (!free_in_callback) {
        CHECK(ostrace_stop_activity(client) == OSTRACE_E_SUCCESS);
        CHECK(ostrace_stop_activity(client) == OSTRACE_E_SUCCESS);
        CHECK(client->worker == THREAD_T_NULL);
    }
    CHECK(ostrace_client_free(client) == OSTRACE_E_SUCCESS);
    CHECK(client_memory == NULL && service_releases == 1);
    CHECK(joins == successful && thread_releases == successful);
    CHECK(worker_allocations == worker_frees && worker_memory == NULL);
    CHECK(payload_allocations == running_worker && payload_frees == running_worker);
    CHECK(payload_memory == NULL && data_callbacks == running_worker);
    CHECK(terminal_callbacks == (natural_terminal && strcmp(wrapper_name, "legacy") != 0));
    CHECK(reader_calls == (running_worker ? (natural_terminal ? 4 : 3) : 0));
    return failures ? 1 : 0;
}
