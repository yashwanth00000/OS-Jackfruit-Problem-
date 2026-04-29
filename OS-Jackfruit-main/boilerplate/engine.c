/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * Implement producer-side insertion into the bounded buffer.
 *
 * Policy: Block when buffer is full (wait for consumer to drain)
 * - Wakes consumer on insert
 * - Returns -1 if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* Wait until there's space or shutdown begins */
    while (buffer->count >= LOG_BUFFER_CAPACITY) {
        if (buffer->shutting_down) {
            pthread_mutex_unlock(&buffer->mutex);
            return -1;
        }
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    /* Insert at tail */
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    /* Signal consumer if it's waiting */
    pthread_cond_signal(&buffer->not_empty);

    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * Implement consumer-side removal from the bounded buffer.
 *
 * Returns:
 *   0: successfully popped an item
 *   1: shutdown is in progress and buffer is empty (consumer should exit)
 *   -1: error
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* Wait until there's data or shutdown begins */
    while (buffer->count == 0) {
        if (buffer->shutting_down) {
            pthread_mutex_unlock(&buffer->mutex);
            return 1;  /* Signal consumer to exit */
        }
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    if (buffer->count == 0) {
        if (buffer->shutting_down) {
            pthread_mutex_unlock(&buffer->mutex);
            return 1;
        }
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    /* Remove from head */
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    /* Signal producer if it's waiting */
    pthread_cond_signal(&buffer->not_full);

    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * Implement the logging consumer thread.
 *
 * Responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    int status;
    FILE *log_file;
    char log_path[PATH_MAX];

    while (1) {
        status = bounded_buffer_pop(&ctx->log_buffer, &item);

        if (status == 1) {
            /* Shutdown signal and buffer empty - exit cleanly */
            break;
        }

        if (status == 0) {
            /* Got an item - write to the appropriate log file */
            snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);

            log_file = fopen(log_path, "ab");
            if (log_file) {
                fwrite(item.data, 1, item.length, log_file);
                fflush(log_file);
                fclose(log_file);
            } else {
                fprintf(stderr, "Warning: Could not write to log file %s\n", log_path);
            }
        }
    }

    return NULL;
}

/*
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    char *shell_argv[] = {(char *)"/bin/sh", (char *)"-c", cfg->command, NULL};

    /* Redirect stdout and stderr to the log pipe */
    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    close(cfg->log_write_fd);

    /* Set nice value if specified */
    if (cfg->nice_value != 0) {
        if (nice(cfg->nice_value) == -1)
            perror("nice");
    }

    /* Change to rootfs directory */
    if (chdir(cfg->rootfs) != 0) {
        perror("chdir to rootfs");
        return 1;
    }

    /* Use chroot to isolate filesystem */
    if (chroot(".") != 0) {
        perror("chroot");
        return 1;
    }

    /* Change to root directory inside the container */
    if (chdir("/") != 0) {
        perror("chdir to /");
        return 1;
    }

    /* Mount /proc inside container so tools like ps work */
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount /proc");
        /* Not fatal, continue anyway */
    }

    /* Execute the command via /bin/sh */
    execvp("/bin/sh", shell_argv);

    /* If exec fails, return error */
    perror("execvp");
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/* Structure for pipe reader thread */
typedef struct {
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} pipe_reader_arg_t;

/* Pipe reader thread - captures container stdout/stderr */
void *pipe_reader_thread(void *arg)
{
    pipe_reader_arg_t *reader_arg = (pipe_reader_arg_t *)arg;
    log_item_t log_item;
    ssize_t n;

    memset(&log_item, 0, sizeof(log_item));
    strncpy(log_item.container_id, reader_arg->container_id,
            sizeof(log_item.container_id) - 1);

    while (1) {
        n = read(reader_arg->read_fd, log_item.data, sizeof(log_item.data));
        if (n <= 0)
            break;

        log_item.length = (size_t)n;

        /* Try to push to buffer, but don't block indefinitely */
        if (bounded_buffer_push(reader_arg->buffer, &log_item) != 0)
            break;
    }

    close(reader_arg->read_fd);
    free(reader_arg);
    return NULL;
}

/* Global supervisor context for signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

static void signal_handler(int sig)
{
    if (!g_ctx)
        return;

    if (sig == SIGCHLD) {
        /* Mark that we need to reap children */
        /* Will be handled in main loop */
    } else if (sig == SIGINT || sig == SIGTERM) {
        g_ctx->should_stop = 1;
    }
}

static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *rec;
    for (rec = ctx->containers; rec; rec = rec->next) {
        if (strcmp(rec->id, id) == 0)
            return rec;
    }
    return NULL;
}

static container_record_t *find_container_by_pid(supervisor_ctx_t *ctx, pid_t pid)
{
    container_record_t *rec;
    for (rec = ctx->containers; rec; rec = rec->next) {
        if (rec->host_pid == pid)
            return rec;
    }
    return NULL;
}

static int execute_start_cmd(supervisor_ctx_t *ctx, const control_request_t *req)
{
    container_record_t *rec;
    char stack_mem[STACK_SIZE];
    pid_t child_pid;
    child_config_t child_cfg;
    int pipe_fds[2];
    struct timespec ts;
    pipe_reader_arg_t *reader_arg;
    int rc;

    /* Check for duplicate ID */
    if (find_container(ctx, req->container_id)) {
        return 1;
    }

    /* Create pipe for container's stdout/stderr */
    if (pipe(pipe_fds) != 0) {
        perror("pipe");
        return 1;
    }

    /* Prepare child config */
    memset(&child_cfg, 0, sizeof(child_cfg));
    strncpy(child_cfg.id, req->container_id, sizeof(child_cfg.id) - 1);
    strncpy(child_cfg.rootfs, req->rootfs, sizeof(child_cfg.rootfs) - 1);
    strncpy(child_cfg.command, req->command, sizeof(child_cfg.command) - 1);
    child_cfg.nice_value = req->nice_value;
    child_cfg.log_write_fd = pipe_fds[1];

    /* Clone with isolated namespaces */
    child_pid = clone(child_fn, stack_mem + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      &child_cfg);

    close(pipe_fds[1]);  /* Close write end in parent */

    if (child_pid < 0) {
        perror("clone");
        close(pipe_fds[0]);
        return 1;
    }

    /* Create container record */
    rec = malloc(sizeof(*rec));
    if (!rec) {
        close(pipe_fds[0]);
        return 1;
    }

    clock_gettime(CLOCK_REALTIME, &ts);
    strncpy(rec->id, req->container_id, sizeof(rec->id) - 1);
    rec->host_pid = child_pid;
    rec->started_at = ts.tv_sec;
    rec->state = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->exit_code = 0;
    rec->exit_signal = 0;
    snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, req->container_id);

    /* Register with kernel monitor */
    if (ctx->monitor_fd >= 0) {
        register_with_monitor(ctx->monitor_fd, req->container_id, child_pid,
                              req->soft_limit_bytes, req->hard_limit_bytes);
    }

    /* Add to list */
    rec->next = ctx->containers;
    ctx->containers = rec;

    /* Spawn thread to read from pipe and feed to log buffer */
    reader_arg = malloc(sizeof(*reader_arg));
    if (reader_arg) {
        pthread_t reader_tid;

        reader_arg->read_fd = pipe_fds[0];
        strncpy(reader_arg->container_id, req->container_id,
                sizeof(reader_arg->container_id) - 1);
        reader_arg->buffer = &ctx->log_buffer;

        rc = pthread_create(&reader_tid, NULL, pipe_reader_thread, reader_arg);
        if (rc != 0) {
            errno = rc;
            perror("pthread_create pipe_reader");
            close(pipe_fds[0]);
            free(reader_arg);
            /* Don't fail container - continue anyway */
        } else {
            pthread_detach(reader_tid);  /* Let it clean up on its own */
        }
    } else {
        close(pipe_fds[0]);
    }

    return 0;
}

/*
 * Implement the long-running supervisor process.
 *
 * Responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc, should_continue;
    struct sockaddr_un server_addr;
    struct sigaction sa;

    (void)rootfs;  /* rootfs is unused by supervisor itself */

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    ctx.should_stop = 0;

    /* Create logs directory */
    mkdir(LOG_DIR, 0755);

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Open kernel monitor device */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        fprintf(stderr, "Warning: Could not open /dev/container_monitor: %s\n", strerror(errno));
        /* Not fatal - continue without kernel monitoring */
        ctx.monitor_fd = -1;
    }

    /* Create UNIX domain socket for control IPC */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }

    unlink(CONTROL_PATH);  /* Remove any stale socket */

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, CONTROL_PATH, sizeof(server_addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        goto cleanup;
    }

    if (listen(ctx.server_fd, 5) < 0) {
        perror("listen");
        goto cleanup;
    }

    /* Spawn logging thread */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create");
        goto cleanup;
    }

    /* Set up signal handlers */
    g_ctx = &ctx;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    fprintf(stderr, "Supervisor started on socket %s\n", CONTROL_PATH);

    /* Main event loop */
    should_continue = 1;
    while (should_continue && !ctx.should_stop) {
        int client_fd;
        control_request_t req;
        control_response_t resp;
        ssize_t n;
        pid_t reaped_pid;
        int wstatus;

        /* Reap any exited children */
        while ((reaped_pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
            container_record_t *rec;

            pthread_mutex_lock(&ctx.metadata_lock);
            rec = find_container_by_pid(&ctx, reaped_pid);
            if (rec) {
                if (WIFEXITED(wstatus)) {
                    rec->exit_code = WEXITSTATUS(wstatus);
                    rec->state = CONTAINER_EXITED;
                    fprintf(stderr, "Container %s exited with code %d\n",
                            rec->id, rec->exit_code);
                } else if (WIFSIGNALED(wstatus)) {
                    rec->exit_signal = WTERMSIG(wstatus);
                    if (rec->exit_signal == SIGKILL) {
                        rec->state = CONTAINER_KILLED;
                    } else {
                        rec->state = CONTAINER_STOPPED;
                    }
                    fprintf(stderr, "Container %s terminated by signal %d\n",
                            rec->id, rec->exit_signal);
                }
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
        }

        /* Accept a client connection (non-blocking with timeout) */
        fd_set readfds;
        struct timeval tv;

        FD_ZERO(&readfds);
        FD_SET(ctx.server_fd, &readfds);

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int select_rc = select(ctx.server_fd + 1, &readfds, NULL, NULL, &tv);
        if (select_rc < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }

        if (select_rc == 0)
            continue;

        client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        /* Read request */
        memset(&req, 0, sizeof(req));
        n = read(client_fd, &req, sizeof(req));

        if (n < 0) {
            perror("read request");
            close(client_fd);
            continue;
        }

        if (n == 0) {
            close(client_fd);
            continue;
        }

        /* Dispatch command */
        memset(&resp, 0, sizeof(resp));
        resp.status = 0;

        pthread_mutex_lock(&ctx.metadata_lock);

        switch (req.kind) {
        case CMD_START:
            pthread_mutex_unlock(&ctx.metadata_lock);
            resp.status = execute_start_cmd(&ctx, &req);
            if (resp.status == 0) {
                snprintf(resp.message, sizeof(resp.message),
                         "Container %s started", req.container_id);
            } else {
                snprintf(resp.message, sizeof(resp.message),
                         "Failed to start container %s", req.container_id);
            }
            pthread_mutex_lock(&ctx.metadata_lock);
            break;

        case CMD_PS: {
            container_record_t *rec;
            int offset = 0;

            offset += snprintf(resp.message + offset, sizeof(resp.message) - offset,
                               "ID                            PID   STATE     EXIT\n");
            for (rec = ctx.containers; rec && (size_t)offset < sizeof(resp.message) - 64; rec = rec->next) {
                offset += snprintf(resp.message + offset, sizeof(resp.message) - offset,
                                   "%-30s %5d %-9s %d\n",
                                   rec->id, rec->host_pid, state_to_string(rec->state),
                                   rec->exit_code);
            }
            break;
        }

        case CMD_LOGS: {
            container_record_t *rec = find_container(&ctx, req.container_id);
            if (rec) {
                FILE *f = fopen(rec->log_path, "r");
                if (f) {
                    size_t nread;
                    size_t available = sizeof(resp.message) - 1;
                    nread = fread(resp.message, 1, available, f);
                    resp.message[nread] = '\0';
                    fclose(f);
                } else {
                    char path_buf[256];
                    if (strlen(rec->log_path) >= sizeof(path_buf)) {
                        snprintf(path_buf, sizeof(path_buf), "%.240s...", rec->log_path);
                    } else {
                        strcpy(path_buf, rec->log_path);
                    }
                    snprintf(resp.message, sizeof(resp.message),
                             "Log file not found: %s", path_buf);
                }
            } else {
                snprintf(resp.message, sizeof(resp.message),
                         "Container not found: %s", req.container_id);
            }
            break;
        }

        case CMD_STOP: {
            container_record_t *rec = find_container(&ctx, req.container_id);
            if (rec && rec->state == CONTAINER_RUNNING) {
                if (kill(rec->host_pid, SIGTERM) == 0) {
                    rec->state = CONTAINER_STOPPED;
                    snprintf(resp.message, sizeof(resp.message),
                             "Container %s stopped", req.container_id);
                } else {
                    resp.status = 1;
                    snprintf(resp.message, sizeof(resp.message),
                             "Failed to stop container %s", req.container_id);
                }
            } else {
                resp.status = 1;
                snprintf(resp.message, sizeof(resp.message),
                         "Container not found or not running: %s", req.container_id);
            }
            break;
        }

        default:
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message), "Unknown command");
        }

        pthread_mutex_unlock(&ctx.metadata_lock);

        /* Send response */
        if (write(client_fd, &resp, sizeof(resp)) != sizeof(resp)) {
            perror("write response");
        }
        close(client_fd);
    }

    fprintf(stderr, "Supervisor shutting down...\n");

cleanup:
    /* Graceful shutdown */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);

    /* No need to explicitly kill containers - they will be reaped */
    pthread_join(ctx.logger_thread, NULL);

    /* Clean up resources */
    if (ctx.server_fd >= 0) {
        close(ctx.server_fd);
        unlink(CONTROL_PATH);
    }

    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    /* Free container records */
    while (ctx.containers) {
        container_record_t *next = ctx.containers->next;
        free(ctx.containers);
        ctx.containers = next;
    }

    g_ctx = NULL;
    return 0;
}

/*
 * Implement the client-side control request path.
 *
 * Connects to the supervisor over UNIX domain socket,
 * sends request, receives response, and prints result.
 */
static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;
    ssize_t n;
    int exit_status = 0;

    /* Connect to supervisor socket */
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Error: Could not connect to supervisor\n");
        fprintf(stderr, "Make sure supervisor is running: engine supervisor <base-rootfs>\n");
        close(fd);
        return 1;
    }

    /* Send request */
    if (write(fd, req, sizeof(*req)) != sizeof(*req)) {
        perror("write request");
        close(fd);
        return 1;
    }

    /* Read response */
    memset(&resp, 0, sizeof(resp));
    n = read(fd, &resp, sizeof(resp));

    if (n < 0) {
        perror("read response");
        close(fd);
        return 1;
    }

    if (n == 0) {
        fprintf(stderr, "Error: Supervisor closed connection\n");
        close(fd);
        return 1;
    }

    /* Print response message */
    if (resp.message[0] != '\0') {
        printf("%s\n", resp.message);
    }

    exit_status = resp.status;

    close(fd);
    return exit_status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    printf("Expected states include: %s, %s, %s, %s, %s\n",
           state_to_string(CONTAINER_STARTING),
           state_to_string(CONTAINER_RUNNING),
           state_to_string(CONTAINER_STOPPED),
           state_to_string(CONTAINER_KILLED),
           state_to_string(CONTAINER_EXITED));
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
