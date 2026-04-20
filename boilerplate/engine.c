/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
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
#include <sys/mman.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "/tmp/container_logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 64
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)
#define MAX_CONTAINERS 16
#define MONITOR_DEV "/dev/container_monitor"

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
    CONTAINER_EXITED,
    CONTAINER_HARD_LIMIT_KILLED
} container_state_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
    int exit_code;
    int exit_signal;
    int stop_requested;
    char log_path[PATH_MAX];
    int pipe_read_fd;
    int in_use;
    /* for run: client fd waiting on exit */
    int run_client_fd;
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
    int pipe_write_fd;
} child_config_t;

typedef struct {
    bounded_buffer_t *buf;
    int pipe_read_fd;
    char container_id[CONTAINER_ID_LEN];
} producer_arg_t;

/* Global supervisor state */
static volatile sig_atomic_t g_shutdown = 0;
static container_record_t g_containers[MAX_CONTAINERS];
static pthread_mutex_t g_meta_lock = PTHREAD_MUTEX_INITIALIZER;
static bounded_buffer_t g_log_buf;
static int g_monitor_fd = -1;

/* ------------------------------------------------------------------ */
/* Utility                                                              */
/* ------------------------------------------------------------------ */

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

static const char *state_to_string(container_state_t s)
{
    switch (s) {
    case CONTAINER_STARTING:          return "starting";
    case CONTAINER_RUNNING:           return "running";
    case CONTAINER_STOPPED:           return "stopped";
    case CONTAINER_KILLED:            return "killed";
    case CONTAINER_EXITED:            return "exited";
    case CONTAINER_HARD_LIMIT_KILLED: return "hard_limit_killed";
    default:                          return "unknown";
    }
}

static int parse_mib_flag(const char *flag, const char *value,
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

static int parse_optional_flags(control_request_t *req, int argc, char *argv[],
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
                fprintf(stderr, "Invalid value for --nice: %s\n", argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Bounded Buffer                                                        */
/* ------------------------------------------------------------------ */

static int bounded_buffer_init(bounded_buffer_t *b)
{
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->mutex, NULL);
    pthread_cond_init(&b->not_empty, NULL);
    pthread_cond_init(&b->not_full, NULL);
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *b)
{
    pthread_cond_destroy(&b->not_full);
    pthread_cond_destroy(&b->not_empty);
    pthread_mutex_destroy(&b->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

/* Returns 0 on success, -1 if shutting down */
static int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);
    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }
    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* Returns 1 if item was popped, 0 if shutting down and empty */
static int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);
    if (b->count == 0) {
        pthread_mutex_unlock(&b->mutex);
        return 0; /* done */
    }
    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Consumer (logging) thread                                            */
/* ------------------------------------------------------------------ */

static void *logging_thread(void *arg)
{
    bounded_buffer_t *buf = (bounded_buffer_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(buf, &item)) {
        /* Find log path for this container */
        char log_path[PATH_MAX] = "";
        pthread_mutex_lock(&g_meta_lock);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (g_containers[i].in_use &&
                strncmp(g_containers[i].id, item.container_id,
                        CONTAINER_ID_LEN) == 0) {
                strncpy(log_path, g_containers[i].log_path, PATH_MAX - 1);
                break;
            }
        }
        pthread_mutex_unlock(&g_meta_lock);

        if (log_path[0] == '\0') continue;

        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) continue;
        write(fd, item.data, item.length);
        close(fd);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Producer thread (one per container)                                  */
/* ------------------------------------------------------------------ */

static void *producer_thread(void *arg)
{
    producer_arg_t *p = (producer_arg_t *)arg;
    char buf[LOG_CHUNK_SIZE];
    ssize_t n;

    while ((n = read(p->pipe_read_fd, buf, sizeof(buf))) > 0) {
        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, p->container_id, CONTAINER_ID_LEN - 1);
        item.length = (size_t)n;
        memcpy(item.data, buf, (size_t)n);
        bounded_buffer_push(p->buf, &item);
    }

    close(p->pipe_read_fd);
    free(p);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Container child entrypoint                                           */
/* ------------------------------------------------------------------ */

static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout and stderr to pipe */
    dup2(cfg->pipe_write_fd, STDOUT_FILENO);
    dup2(cfg->pipe_write_fd, STDERR_FILENO);
    close(cfg->pipe_write_fd);

    /* Set nice value */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* chroot into container rootfs */
    if (chroot(cfg->rootfs) < 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("chdir");
        return 1;
    }

    /* Mount /proc */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        /* non-fatal, best effort */
    }

    /* Set hostname to container id */
    sethostname(cfg->id, strlen(cfg->id));

    /* Execute command */
    char *argv_exec[] = { "/bin/sh", "-c", cfg->command, NULL };
    execv("/bin/sh", argv_exec);
    perror("execv");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Metadata helpers                                                     */
/* ------------------------------------------------------------------ */

static container_record_t *find_container(const char *id)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (g_containers[i].in_use &&
            strncmp(g_containers[i].id, id, CONTAINER_ID_LEN) == 0)
            return &g_containers[i];
    return NULL;
}

static container_record_t *alloc_container_slot(void)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (!g_containers[i].in_use)
            return &g_containers[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Kernel monitor integration                                           */
/* ------------------------------------------------------------------ */

static int register_with_monitor(const char *id, pid_t pid,
                                 unsigned long soft, unsigned long hard)
{
    if (g_monitor_fd < 0) return 0; /* module not loaded, skip */
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    req.soft_limit_bytes = soft;
    req.hard_limit_bytes = hard;
    strncpy(req.container_id, id, MONITOR_NAME_LEN - 1);
    return ioctl(g_monitor_fd, MONITOR_REGISTER, &req);
}

static int unregister_from_monitor(const char *id, pid_t pid)
{
    if (g_monitor_fd < 0) return 0;
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    strncpy(req.container_id, id, MONITOR_NAME_LEN - 1);
    return ioctl(g_monitor_fd, MONITOR_UNREGISTER, &req);
}

/* ------------------------------------------------------------------ */
/* Spawn a container                                                    */
/* ------------------------------------------------------------------ */

static int spawn_container(const control_request_t *req, int client_fd_for_run)
{
    /* Check duplicate id */
    pthread_mutex_lock(&g_meta_lock);
    if (find_container(req->container_id)) {
        pthread_mutex_unlock(&g_meta_lock);
        return -1; /* already exists */
    }
    container_record_t *rec = alloc_container_slot();
    if (!rec) {
        pthread_mutex_unlock(&g_meta_lock);
        return -1;
    }

    memset(rec, 0, sizeof(*rec));
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->state = CONTAINER_STARTING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->nice_value = req->nice_value;
    rec->started_at = time(NULL);
    rec->pipe_read_fd = -1;
    rec->run_client_fd = client_fd_for_run;
    rec->in_use = 1;

    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);
    pthread_mutex_unlock(&g_meta_lock);

    /* Create pipe */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        pthread_mutex_lock(&g_meta_lock);
        rec->in_use = 0;
        pthread_mutex_unlock(&g_meta_lock);
        return -1;
    }

    /* Set up child config on the stack — clone child gets a copy */
    static child_config_t cfg; /* static so child sees it after clone */
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg.rootfs, req->rootfs, PATH_MAX - 1);
    strncpy(cfg.command, req->command, CHILD_COMMAND_LEN - 1);
    cfg.nice_value = req->nice_value;
    cfg.pipe_write_fd = pipefd[1];

    /* Allocate stack for clone */
    char *stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) {
        close(pipefd[0]); close(pipefd[1]);
        pthread_mutex_lock(&g_meta_lock);
        rec->in_use = 0;
        pthread_mutex_unlock(&g_meta_lock);
        return -1;
    }

    pid_t pid = clone(child_fn, stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      &cfg);
    if (pid < 0) {
        munmap(stack, STACK_SIZE);
        close(pipefd[0]); close(pipefd[1]);
        pthread_mutex_lock(&g_meta_lock);
        rec->in_use = 0;
        pthread_mutex_unlock(&g_meta_lock);
        return -1;
    }

    /* Parent: close write end */
    close(pipefd[1]);

    pthread_mutex_lock(&g_meta_lock);
    rec->host_pid = pid;
    rec->pipe_read_fd = pipefd[0];
    rec->state = CONTAINER_RUNNING;
    pthread_mutex_unlock(&g_meta_lock);

    /* Register with kernel module */
    register_with_monitor(req->container_id, pid,
                          req->soft_limit_bytes, req->hard_limit_bytes);

    /* Start producer thread */
    producer_arg_t *parg = malloc(sizeof(*parg));
    parg->buf = &g_log_buf;
    parg->pipe_read_fd = pipefd[0];
    strncpy(parg->container_id, req->container_id, CONTAINER_ID_LEN - 1);
    pthread_t ptid;
    pthread_create(&ptid, NULL, producer_thread, parg);
    pthread_detach(ptid);

    return 0;
}

/* ------------------------------------------------------------------ */
/* SIGCHLD handler                                                      */
/* ------------------------------------------------------------------ */

static void sigchld_handler(int sig)
{
    (void)sig;
    int saved_errno = errno;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_meta_lock);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (!g_containers[i].in_use || g_containers[i].host_pid != pid)
                continue;
            container_record_t *rec = &g_containers[i];

            if (WIFEXITED(status)) {
                rec->exit_code = WEXITSTATUS(status);
                rec->exit_signal = 0;
                if (rec->stop_requested)
                    rec->state = CONTAINER_STOPPED;
                else
                    rec->state = CONTAINER_EXITED;
            } else if (WIFSIGNALED(status)) {
                rec->exit_signal = WTERMSIG(status);
                rec->exit_code = 128 + rec->exit_signal;
                if (rec->stop_requested)
                    rec->state = CONTAINER_STOPPED;
                else if (rec->exit_signal == SIGKILL)
                    rec->state = CONTAINER_HARD_LIMIT_KILLED;
                else
                    rec->state = CONTAINER_KILLED;
            }

            /* Notify waiting run client */
            if (rec->run_client_fd >= 0) {
                control_response_t resp;
                resp.status = rec->exit_code;
                snprintf(resp.message, sizeof(resp.message),
                         "container %s exited code=%d signal=%d state=%s",
                         rec->id, rec->exit_code, rec->exit_signal,
                         state_to_string(rec->state));
                write(rec->run_client_fd, &resp, sizeof(resp));
                close(rec->run_client_fd);
                rec->run_client_fd = -1;
            }

            unregister_from_monitor(rec->id, pid);
            break;
        }
        pthread_mutex_unlock(&g_meta_lock);
    }
    errno = saved_errno;
}

static void sigterm_handler(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

/* ------------------------------------------------------------------ */
/* Handle one CLI command received over the socket                     */
/* ------------------------------------------------------------------ */

static void handle_request(int client_fd, const control_request_t *req)
{
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    switch (req->kind) {

    case CMD_START: {
        int rc = spawn_container(req, -1);
        if (rc == 0) {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "started container %s", req->container_id);
        } else {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "failed to start container %s", req->container_id);
        }
        write(client_fd, &resp, sizeof(resp));
        break;
    }

    case CMD_RUN: {
        /* Hold the client fd open; response sent when container exits */
        int rc = spawn_container(req, client_fd);
        if (rc != 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "failed to start container %s", req->container_id);
            write(client_fd, &resp, sizeof(resp));
        }
        /* Do NOT close client_fd here — sigchld_handler will close it */
        return;
    }

    case CMD_PS: {
        char buf[4096] = "";
        pthread_mutex_lock(&g_meta_lock);
        int any = 0;
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (!g_containers[i].in_use) continue;
            container_record_t *r = &g_containers[i];
            char line[256];
            char ts[32];
            struct tm *tm = localtime(&r->started_at);
            strftime(ts, sizeof(ts), "%H:%M:%S", tm);
            snprintf(line, sizeof(line),
                     "%-16s %-8d %-20s soft=%-6luMiB hard=%-6luMiB exit=%-4d signal=%-4d\n",
                     r->id, r->host_pid, state_to_string(r->state),
                     r->soft_limit_bytes >> 20, r->hard_limit_bytes >> 20,
                     r->exit_code, r->exit_signal);
            strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
            any = 1;
        }
        pthread_mutex_unlock(&g_meta_lock);
        if (!any) strcpy(buf, "no containers\n");
        resp.status = 0;
        strncpy(resp.message, buf, sizeof(resp.message) - 1);
        write(client_fd, &resp, sizeof(resp));
        break;
    }

    case CMD_LOGS: {
        pthread_mutex_lock(&g_meta_lock);
        container_record_t *r = find_container(req->container_id);
        char log_path[PATH_MAX] = "";
        if (r) strncpy(log_path, r->log_path, PATH_MAX - 1);
        pthread_mutex_unlock(&g_meta_lock);

        if (log_path[0] == '\0') {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "no such container: %s", req->container_id);
            write(client_fd, &resp, sizeof(resp));
            break;
        }

        /* Send header response */
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "--- logs for %s ---\n", req->container_id);
        write(client_fd, &resp, sizeof(resp));

        /* Stream file contents */
        int fd = open(log_path, O_RDONLY);
        if (fd >= 0) {
            char fbuf[4096];
            ssize_t n;
            while ((n = read(fd, fbuf, sizeof(fbuf))) > 0)
                write(client_fd, fbuf, (size_t)n);
            close(fd);
        }
        break;
    }

    case CMD_STOP: {
        pthread_mutex_lock(&g_meta_lock);
        container_record_t *r = find_container(req->container_id);
        if (!r) {
            pthread_mutex_unlock(&g_meta_lock);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "no such container: %s", req->container_id);
            write(client_fd, &resp, sizeof(resp));
            break;
        }
        r->stop_requested = 1;
        pid_t pid = r->host_pid;
        pthread_mutex_unlock(&g_meta_lock);

        kill(pid, SIGTERM);
        /* Give it 3 seconds then SIGKILL */
        for (int i = 0; i < 30; i++) {
            usleep(100000);
            pthread_mutex_lock(&g_meta_lock);
            container_record_t *r2 = find_container(req->container_id);
            int dead = !r2 || r2->state == CONTAINER_STOPPED ||
                       r2->state == CONTAINER_KILLED ||
                       r2->state == CONTAINER_EXITED ||
                       r2->state == CONTAINER_HARD_LIMIT_KILLED;
            pthread_mutex_unlock(&g_meta_lock);
            if (dead) break;
        }
        kill(pid, SIGKILL); /* harmless if already dead */

        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "stopped container %s", req->container_id);
        write(client_fd, &resp, sizeof(resp));
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "unknown command");
        write(client_fd, &resp, sizeof(resp));
        break;
    }

    close(client_fd);
}

/* ------------------------------------------------------------------ */
/* Supervisor main loop                                                 */
/* ------------------------------------------------------------------ */

static int run_supervisor(const char *rootfs)
{
    (void)rootfs;

    /* Create log directory */
    mkdir(LOG_DIR, 0755);

    /* Open kernel monitor (optional — ok if not loaded) */
    g_monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (g_monitor_fd < 0)
        fprintf(stderr, "[supervisor] kernel module not loaded, memory limits disabled\n");

    /* Init shared state */
    memset(g_containers, 0, sizeof(g_containers));
    bounded_buffer_init(&g_log_buf);

    /* Install signal handlers */
    struct sigaction sa_chld = {0};
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_term = {0};
    sa_term.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT,  &sa_term, NULL);

    /* Start consumer thread */
    pthread_t logger_tid;
    pthread_create(&logger_tid, NULL, logging_thread, &g_log_buf);

    /* Create UNIX domain socket */
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    unlink(CONTROL_PATH);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(server_fd); return 1;
    }
    if (listen(server_fd, 16) < 0) {
        perror("listen"); close(server_fd); return 1;
    }

    fprintf(stderr, "[supervisor] ready on %s\n", CONTROL_PATH);

    /* Event loop */
    while (!g_shutdown) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        int rc = select(server_fd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) continue;

        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        control_request_t req;
        ssize_t n = recv(client_fd, &req, sizeof(req), MSG_WAITALL);
        if (n == (ssize_t)sizeof(req)) {
            handle_request(client_fd, &req);
        } else {
            close(client_fd);
        }
    }

    fprintf(stderr, "[supervisor] shutting down...\n");

    /* Stop all running containers */
    pthread_mutex_lock(&g_meta_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (g_containers[i].in_use &&
            g_containers[i].state == CONTAINER_RUNNING) {
            g_containers[i].stop_requested = 1;
            kill(g_containers[i].host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&g_meta_lock);

    sleep(2);

    /* Force kill any remaining */
    pthread_mutex_lock(&g_meta_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (g_containers[i].in_use &&
            g_containers[i].state == CONTAINER_RUNNING)
            kill(g_containers[i].host_pid, SIGKILL);
    }
    pthread_mutex_unlock(&g_meta_lock);

    /* Shutdown logging */
    bounded_buffer_begin_shutdown(&g_log_buf);
    pthread_join(logger_tid, NULL);
    bounded_buffer_destroy(&g_log_buf);

    close(server_fd);
    unlink(CONTROL_PATH);
    if (g_monitor_fd >= 0) close(g_monitor_fd);

    fprintf(stderr, "[supervisor] exited cleanly\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* CLI client                                                           */
/* ------------------------------------------------------------------ */

static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(fd);
        return 1;
    }

    if (send(fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send");
        close(fd);
        return 1;
    }

    /* For run: forward SIGINT/SIGTERM to supervisor as a stop */
    /* Read response(s) */
    control_response_t resp;
    ssize_t n;

    if (req->kind == CMD_LOGS) {
        /* First: structured response */
        n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
        if (n == (ssize_t)sizeof(resp)) {
            printf("%s", resp.message);
            /* Then: raw file data */
            char buf[4096];
            while ((n = read(fd, buf, sizeof(buf))) > 0)
                fwrite(buf, 1, (size_t)n, stdout);
        }
    } else {
        n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
        if (n == (ssize_t)sizeof(resp)) {
            printf("%s\n", resp.message);
            if (resp.status != 0) {
                close(fd);
                return 1;
            }
        }
    }

    close(fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* CLI command wrappers                                                 */
/* ------------------------------------------------------------------ */

static int cmd_start(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr, "Usage: %s start <id> <rootfs> <command> [opts]\n", argv[0]);
        return 1;
    }
    control_request_t req = {0};
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs, argv[3], PATH_MAX - 1);
    strncpy(req.command, argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr, "Usage: %s run <id> <rootfs> <command> [opts]\n", argv[0]);
        return 1;
    }
    control_request_t req = {0};
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs, argv[3], PATH_MAX - 1);
    strncpy(req.command, argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req = {0};
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    control_request_t req = {0};
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    control_request_t req = {0};
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

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
