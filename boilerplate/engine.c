/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 * Ubuntu 24.04 / kernel 6.17 compatible
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

#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN   256
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT  (40UL << 20)
#define DEFAULT_HARD_LIMIT  (64UL << 20)

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
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
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
    int  status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;
} child_config_t;

typedef struct {
    int               server_fd;
    int               monitor_fd;
    int               should_stop;
    pthread_t         logger_thread;
    bounded_buffer_t  log_buffer;
    pthread_mutex_t   metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* ---------------------------------------------------------------
 * Global supervisor context pointer — used by signal handlers
 * --------------------------------------------------------------- */
static supervisor_ctx_t *g_ctx = NULL;

/* ---------------------------------------------------------------
 * Usage
 * --------------------------------------------------------------- */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

/* ---------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------- */
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
                                int argc, char *argv[], int start_index)
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
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i+1]);
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
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ---------------------------------------------------------------
 * Bounded Buffer
 * --------------------------------------------------------------- */
static int bounded_buffer_init(bounded_buffer_t *buf)
{
    int rc;
    memset(buf, 0, sizeof(*buf));
    rc = pthread_mutex_init(&buf->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buf->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buf->mutex); return rc; }
    rc = pthread_cond_init(&buf->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buf->not_empty);
        pthread_mutex_destroy(&buf->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buf)
{
    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
    pthread_mutex_destroy(&buf->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buf)
{
    pthread_mutex_lock(&buf->mutex);
    buf->shutting_down = 1;
    pthread_cond_broadcast(&buf->not_empty);
    pthread_cond_broadcast(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
}

/*
 * Producer: insert a log chunk into the buffer.
 * Blocks if full. Returns 0 on success, -1 if shutting down.
 */
int bounded_buffer_push(bounded_buffer_t *buf, const log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);

    while (buf->count == LOG_BUFFER_CAPACITY && !buf->shutting_down)
        pthread_cond_wait(&buf->not_full, &buf->mutex);

    if (buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }

    buf->items[buf->tail] = *item;
    buf->tail = (buf->tail + 1) % LOG_BUFFER_CAPACITY;
    buf->count++;

    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/*
 * Consumer: remove a log chunk from the buffer.
 * Returns 0 on success, -1 if shutdown and buffer drained.
 */
int bounded_buffer_pop(bounded_buffer_t *buf, log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);

    while (buf->count == 0 && !buf->shutting_down)
        pthread_cond_wait(&buf->not_empty, &buf->mutex);

    if (buf->count == 0 && buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }

    *item = buf->items[buf->head];
    buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
    buf->count--;

    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/*
 * Logger thread: drain the bounded buffer and write to per-container log files.
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        container_record_t *rec;
        char log_path[PATH_MAX];
        int fd;

        /* Find the log path for this container */
        log_path[0] = '\0';
        pthread_mutex_lock(&ctx->metadata_lock);
        for (rec = ctx->containers; rec; rec = rec->next) {
            if (strncmp(rec->id, item.container_id, CONTAINER_ID_LEN) == 0) {
                strncpy(log_path, rec->log_path, sizeof(log_path) - 1);
                break;
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (log_path[0] == '\0')
            continue;

        fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0)
            continue;
        write(fd, item.data, item.length);
        close(fd);
    }

    return NULL;
}

/* ---------------------------------------------------------------
 * Log reader thread: reads from a pipe and pushes into bounded buffer
 * --------------------------------------------------------------- */
typedef struct {
    int               read_fd;
    char              container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buf;
} log_reader_args_t;

static void *log_reader_thread(void *arg)
{
    log_reader_args_t *lra = (log_reader_args_t *)arg;
    log_item_t item;
    ssize_t n;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, lra->container_id, CONTAINER_ID_LEN - 1);

    while ((n = read(lra->read_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        bounded_buffer_push(lra->buf, &item);
        memset(item.data, 0, LOG_CHUNK_SIZE);
    }

    close(lra->read_fd);
    free(lra);
    return NULL;
}

/* ---------------------------------------------------------------
 * Clone child entrypoint
 * --------------------------------------------------------------- */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    char *argv_exec[4];

    /* Set hostname to container id */
    sethostname(cfg->id, strlen(cfg->id));

    /* chroot into container rootfs */
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("chdir");
        return 1;
    }

    /* Mount /proc */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        /* Non-fatal: /proc may already be mounted */
        perror("mount /proc");
    }

    /* Redirect stdout and stderr to the log pipe */
    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        close(cfg->log_write_fd);
    }

    /* Apply nice value */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* Execute the command */
    argv_exec[0] = "/bin/sh";
    argv_exec[1] = "-c";
    argv_exec[2] = cfg->command;
    argv_exec[3] = NULL;

    execv("/bin/sh", argv_exec);
    perror("execv");
    return 1;
}

/* ---------------------------------------------------------------
 * ioctl helpers
 * --------------------------------------------------------------- */
int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid               = host_pid;
    req.soft_limit_bytes  = soft_limit_bytes;
    req.hard_limit_bytes  = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd,
                            const char *container_id,
                            pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ---------------------------------------------------------------
 * Container launch helper (used by supervisor on CMD_START/CMD_RUN)
 * --------------------------------------------------------------- */
static int launch_container(supervisor_ctx_t *ctx,
                            const control_request_t *req,
                            control_response_t *resp)
{
    char stack_buf[STACK_SIZE];
    child_config_t *cfg;
    container_record_t *rec;
    int pipefd[2];
    pid_t child_pid;
    pthread_t reader_tid;
    log_reader_args_t *lra;

    /* Check for duplicate ID */
    pthread_mutex_lock(&ctx->metadata_lock);
    for (rec = ctx->containers; rec; rec = rec->next) {
        if (strncmp(rec->id, req->container_id, CONTAINER_ID_LEN) == 0 &&
            (rec->state == CONTAINER_RUNNING ||
             rec->state == CONTAINER_STARTING)) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp->status = -1;
            snprintf(resp->message, CONTROL_MESSAGE_LEN,
                     "Container '%s' already running", req->container_id);
            return -1;
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Create log directory and path */
    mkdir(LOG_DIR, 0755);

    rec = calloc(1, sizeof(*rec));
    if (!rec) {
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "Out of memory");
        return -1;
    }
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->state            = CONTAINER_STARTING;
    rec->started_at       = time(NULL);
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

    /* Create pipe for container stdout/stderr */
    if (pipe(pipefd) != 0) {
        free(rec);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "pipe() failed: %s",
                 strerror(errno));
        return -1;
    }

    /* Set up child config */
    cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        close(pipefd[0]); close(pipefd[1]);
        free(rec);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "Out of memory");
        return -1;
    }
    strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs, req->rootfs, PATH_MAX - 1);
    strncpy(cfg->command, req->command, CHILD_COMMAND_LEN - 1);
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    /* Clone with namespaces */
    child_pid = clone(child_fn,
                      stack_buf + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      cfg);

    /* Close write end in parent */
    close(pipefd[1]);
    free(cfg);

    if (child_pid < 0) {
        close(pipefd[0]);
        free(rec);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "clone() failed: %s",
                 strerror(errno));
        return -1;
    }

    rec->host_pid = child_pid;
    rec->state    = CONTAINER_RUNNING;

    /* Register with kernel monitor */
    if (ctx->monitor_fd >= 0) {
        register_with_monitor(ctx->monitor_fd, req->container_id, child_pid,
                              req->soft_limit_bytes, req->hard_limit_bytes);
    }

    /* Spawn a reader thread to drain the pipe into the bounded buffer */
    lra = calloc(1, sizeof(*lra));
    if (lra) {
        lra->read_fd = pipefd[0];
        strncpy(lra->container_id, req->container_id, CONTAINER_ID_LEN - 1);
        lra->buf = &ctx->log_buffer;
        pthread_create(&reader_tid, NULL, log_reader_thread, lra);
        pthread_detach(reader_tid);
    } else {
        close(pipefd[0]);
    }

    /* Insert record */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next        = ctx->containers;
    ctx->containers  = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    resp->status = 0;
    snprintf(resp->message, CONTROL_MESSAGE_LEN,
             "Container '%s' started with pid=%d", req->container_id, child_pid);
    return 0;
}

/* ---------------------------------------------------------------
 * Signal handlers
 * --------------------------------------------------------------- */
static void sigchld_handler(int sig)
{
    (void)sig;
    /* Reap all children that have exited */
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (!g_ctx) continue;
        container_record_t *rec;
        pthread_mutex_lock(&g_ctx->metadata_lock);
        for (rec = g_ctx->containers; rec; rec = rec->next) {
            if (rec->host_pid == pid) {
                if (WIFEXITED(status)) {
                    rec->state     = CONTAINER_EXITED;
                    rec->exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    rec->state       = CONTAINER_KILLED;
                    rec->exit_signal = WTERMSIG(status);
                    rec->exit_code   = 128 + WTERMSIG(status);
                }
                if (g_ctx->monitor_fd >= 0)
                    unregister_from_monitor(g_ctx->monitor_fd,
                                            rec->id, rec->host_pid);
                break;
            }
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->should_stop = 1;
}

/* ---------------------------------------------------------------
 * Supervisor: handle one accepted client connection
 * --------------------------------------------------------------- */
static void handle_client(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t  req;
    control_response_t resp;
    ssize_t n;

    memset(&resp, 0, sizeof(resp));

    n = recv(client_fd, &req, sizeof(req), 0);
    if (n != (ssize_t)sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, CONTROL_MESSAGE_LEN, "Bad request");
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    switch (req.kind) {

    case CMD_START:
    case CMD_RUN:
        launch_container(ctx, &req, &resp);
        send(client_fd, &resp, sizeof(resp), 0);

        /* For CMD_RUN, block until the container exits, then send final status */
        if (req.kind == CMD_RUN && resp.status == 0) {
            pid_t target_pid = -1;
            container_record_t *rec;

            pthread_mutex_lock(&ctx->metadata_lock);
            for (rec = ctx->containers; rec; rec = rec->next) {
                if (strncmp(rec->id, req.container_id, CONTAINER_ID_LEN) == 0) {
                    target_pid = rec->host_pid;
                    break;
                }
            }
            pthread_mutex_unlock(&ctx->metadata_lock);

            if (target_pid > 0) {
                int wstatus;
                waitpid(target_pid, &wstatus, 0);
                memset(&resp, 0, sizeof(resp));

                pthread_mutex_lock(&ctx->metadata_lock);
                for (rec = ctx->containers; rec; rec = rec->next) {
                    if (rec->host_pid == target_pid) {
                        if (WIFEXITED(wstatus)) {
                            rec->state     = CONTAINER_EXITED;
                            rec->exit_code = WEXITSTATUS(wstatus);
                        } else if (WIFSIGNALED(wstatus)) {
                            rec->state       = CONTAINER_KILLED;
                            rec->exit_signal = WTERMSIG(wstatus);
                            rec->exit_code   = 128 + WTERMSIG(wstatus);
                        }
                        resp.status = rec->exit_code;
                        snprintf(resp.message, CONTROL_MESSAGE_LEN,
                                 "Container '%s' exited with code %d",
                                 req.container_id, rec->exit_code);
                        break;
                    }
                }
                pthread_mutex_unlock(&ctx->metadata_lock);
            }
            send(client_fd, &resp, sizeof(resp), 0);
        }
        break;

    case CMD_PS: {
        container_record_t *rec;
        char buf[4096] = {0};
        int  off = 0;

        off += snprintf(buf + off, sizeof(buf) - off,
                        "%-16s %-8s %-10s %-10s %-10s\n",
                        "ID", "PID", "STATE", "SOFT(MB)", "HARD(MB)");
        pthread_mutex_lock(&ctx->metadata_lock);
        for (rec = ctx->containers; rec; rec = rec->next) {
            off += snprintf(buf + off, sizeof(buf) - off,
                            "%-16s %-8d %-10s %-10lu %-10lu\n",
                            rec->id,
                            rec->host_pid,
                            state_to_string(rec->state),
                            rec->soft_limit_bytes >> 20,
                            rec->hard_limit_bytes >> 20);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        resp.status = 0;
        strncpy(resp.message, buf, CONTROL_MESSAGE_LEN - 1);
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    case CMD_LOGS: {
        container_record_t *rec;
        char log_path[PATH_MAX] = {0};

        pthread_mutex_lock(&ctx->metadata_lock);
        for (rec = ctx->containers; rec; rec = rec->next) {
            if (strncmp(rec->id, req.container_id, CONTAINER_ID_LEN) == 0) {
                strncpy(log_path, rec->log_path, PATH_MAX - 1);
                break;
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (log_path[0] == '\0') {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "Container '%s' not found", req.container_id);
            send(client_fd, &resp, sizeof(resp), 0);
            break;
        }

        resp.status = 0;
        snprintf(resp.message, CONTROL_MESSAGE_LEN, "%s", log_path);
        send(client_fd, &resp, sizeof(resp), 0);

        /* Stream the log file contents */
        {
            FILE *f = fopen(log_path, "r");
            if (f) {
                char chunk[1024];
                size_t nr;
                while ((nr = fread(chunk, 1, sizeof(chunk), f)) > 0)
                    send(client_fd, chunk, nr, 0);
                fclose(f);
            }
        }
        break;
    }

    case CMD_STOP: {
        container_record_t *rec;
        pid_t target_pid = -1;

        pthread_mutex_lock(&ctx->metadata_lock);
        for (rec = ctx->containers; rec; rec = rec->next) {
            if (strncmp(rec->id, req.container_id, CONTAINER_ID_LEN) == 0 &&
                (rec->state == CONTAINER_RUNNING ||
                 rec->state == CONTAINER_STARTING)) {
                target_pid = rec->host_pid;
                rec->state = CONTAINER_STOPPED;
                break;
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (target_pid < 0) {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "Container '%s' not found or not running", req.container_id);
        } else {
            kill(target_pid, SIGTERM);
            resp.status = 0;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "Sent SIGTERM to container '%s' (pid=%d)",
                     req.container_id, target_pid);
        }
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, CONTROL_MESSAGE_LEN, "Unknown command");
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }
}

/* ---------------------------------------------------------------
 * Supervisor main
 * --------------------------------------------------------------- */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    int rc;

    (void)rootfs;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { errno = rc; perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) { errno = rc; perror("bounded_buffer_init"); return 1; }

    /* 1. Open kernel monitor device */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] Warning: /dev/container_monitor not found "
                        "(kernel module not loaded?)\n");

    /* 2. Create UNIX domain socket */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    unlink(CONTROL_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ctx.server_fd, 16) < 0) {
        perror("listen"); return 1;
    }

    /* 3. Signal handlers */
    {
        struct sigaction sa_chld, sa_term;
        memset(&sa_chld, 0, sizeof(sa_chld));
        sa_chld.sa_handler = sigchld_handler;
        sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
        sigaction(SIGCHLD, &sa_chld, NULL);

        memset(&sa_term, 0, sizeof(sa_term));
        sa_term.sa_handler = sigterm_handler;
        sigaction(SIGTERM, &sa_term, NULL);
        sigaction(SIGINT,  &sa_term, NULL);
    }

    /* 4. Start logger thread */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) { errno = rc; perror("pthread_create"); return 1; }

    fprintf(stdout, "[supervisor] Started. Listening on %s\n", CONTROL_PATH);
    fflush(stdout);

    /* 5. Event loop */
    while (!ctx.should_stop) {
        fd_set rfds;
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);

        rc = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (rc == 0) continue; /* timeout — loop again to check should_stop */

        if (FD_ISSET(ctx.server_fd, &rfds)) {
            int client_fd = accept(ctx.server_fd, NULL, NULL);
            if (client_fd >= 0) {
                handle_client(&ctx, client_fd);
                close(client_fd);
            }
        }
    }

    fprintf(stdout, "[supervisor] Shutting down...\n");

    /* Shutdown: stop all running containers */
    {
        container_record_t *rec;
        pthread_mutex_lock(&ctx.metadata_lock);
        for (rec = ctx.containers; rec; rec = rec->next) {
            if (rec->state == CONTAINER_RUNNING ||
                rec->state == CONTAINER_STARTING) {
                kill(rec->host_pid, SIGTERM);
            }
        }
        pthread_mutex_unlock(&ctx.metadata_lock);
    }

    /* Wait a moment for containers to exit */
    sleep(1);

    /* Drain logger and join */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    /* Free container records */
    {
        container_record_t *rec = ctx.containers, *next;
        while (rec) {
            next = rec->next;
            if (ctx.monitor_fd >= 0)
                unregister_from_monitor(ctx.monitor_fd, rec->id, rec->host_pid);
            free(rec);
            rec = next;
        }
    }

    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    pthread_mutex_destroy(&ctx.metadata_lock);
    fprintf(stdout, "[supervisor] Clean exit.\n");
    return 0;
}

/* ---------------------------------------------------------------
 * Client: send a control request to the running supervisor
 * --------------------------------------------------------------- */
static int send_control_request(const control_request_t *req)
{
    int sock_fd;
    struct sockaddr_un addr;
    control_response_t resp;

    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(sock_fd);
        return 1;
    }

    if (send(sock_fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send");
        close(sock_fd);
        return 1;
    }

    /* Read response */
    if (recv(sock_fd, &resp, sizeof(resp), 0) != (ssize_t)sizeof(resp)) {
        perror("recv");
        close(sock_fd);
        return 1;
    }

    if (req->kind == CMD_LOGS && resp.status == 0) {
        /* Stream remaining log data */
        fprintf(stdout, "=== logs: %s ===\n", resp.message);
        char chunk[1024];
        ssize_t n;
        while ((n = recv(sock_fd, chunk, sizeof(chunk), 0)) > 0)
            fwrite(chunk, 1, n, stdout);
        printf("\n=== end of logs ===\n");
    } else if (req->kind == CMD_RUN && resp.status == 0) {
        /* Wait for second response (exit code) */
        control_response_t final_resp;
        if (recv(sock_fd, &final_resp, sizeof(final_resp), 0) ==
            (ssize_t)sizeof(final_resp)) {
            printf("%s\n", final_resp.message);
            close(sock_fd);
            return final_resp.status;
        }
    } else {
        printf("%s\n", resp.message);
    }

    close(sock_fd);
    return (resp.status == 0) ? 0 : 1;
}

/* ---------------------------------------------------------------
 * CLI command handlers
 * --------------------------------------------------------------- */
static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
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
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
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
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

/* ---------------------------------------------------------------
 * main
 * --------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}