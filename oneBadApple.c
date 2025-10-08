/**********************************************************
 *
 * oneBadApple.c
 * CIS 452 Project 1 (F25)
 *
 * Authors: Gerrit Mitchell + Shah Kamali
 *
 * Build:   gcc -Wall -Wextra -O2 -std=c11 oneBadApple.c -o oneBadApple (code I used to run in docker)
 * Run:     ./oneBadApple
 *
 * Summary:
 *   k processes are arranged in a ring with unidirectional pipes.
 *   Each node only writes to its right neighbor and reads from its left neighbor.
 *   A single "apple" message circulates. When the header is empty, node 0
 *   asks the user for a destination and text and injects a message.
 *   When a node receives a message addressed to it, it prints/handles it
 *   and clears the header back to empty so the apple can return to node 0.
 *   Ctrl-C (SIGINT) or type q in the parent sends SIGUSR1 to all children for a graceful exit.
 *
 *************************************************************/
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>

#define MAX_K 64
#define MAX_TEXT 1024

#define DEST_EMPTY (-1)   // "apple" with empty header (free to use)
typedef struct {
    int dest;             // -1 means empty; 0..k-1 otherwise
    int origin;           // node id that created the message
    char text[MAX_TEXT];  // payload (NUL-terminated)
} apple_t;

/* Globals used by parent (node 0) for cleanup */
static pid_t child_pids[MAX_K];
static int   num_children = 0;
static int   g_k = 0;
static int   g_parent = 1;

/* Per-process globals (each process keeps only the fds it needs) */
static int read_fd = -1;
static int write_fd = -1;
static int my_id = -1;

/* Children set this flag when asked to stop via SIGUSR1 */
static volatile sig_atomic_t stop_requested = 0;

/* Utility: safe write of exactly n bytes */
static int write_full(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    size_t left = n;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        left -= (size_t)w;
        p += w;
    }
    return 0;
}

/* Utility: safe read of exactly n bytes (blocking) */
static int read_full(int fd, void *buf, size_t n) {
    char *p = (char *)buf;
    size_t left = n;
    while (left > 0) {
        ssize_t r = read(fd, p, left);
        if (r < 0) {
            if (errno == EINTR) {
                if (stop_requested) return -1; /* allow graceful exit */
                continue;
            }
            return -1;
        } else if (r == 0) {
            /* pipe closed */
            return -1;
        }
        left -= (size_t)r;
        p += r;
    }
    return 0;
}

/* Trim trailing newline from fgets */
static void chomp(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    if (n && s[n-1] == '\n') s[n-1] = '\0';
}

/* SIGUSR1: ask children to exit gracefully */
static void sigusr1_handler(int sig) {
    (void)sig;
    stop_requested = 1;
}

/* SIGINT: parent broadcasts stop to children, then exits */
static void sigint_parent_handler(int sig) {
    (void)sig;
    fprintf(stderr, "\n[Node 0] Caught Ctrl-C: initiating graceful shutdown...\n");
    for (int i = 0; i < num_children; ++i) {
        if (child_pids[i] > 0) kill(child_pids[i], SIGUSR1);
    }
    /* Close our ends to unblock any reads/writes */
    if (read_fd  >= 0) close(read_fd);
    if (write_fd >= 0) close(write_fd);
    /* Give children a moment to exit; not using sleep(), rely on signal/pipe close */
    /* Reap children */
    while (wait(NULL) > 0) {}
    _exit(0);
}

/* Validate integer input (>=0 and < k) */
static int parse_destination(const char *s, int k, int *out) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return -1;
    if (v < 0 || v >= k) return -1;
    *out = (int)v;
    return 0;
}

static void node_loop(void) {
    /* Line-buffered stdout for readable interleaved logs */
    setvbuf(stdout, NULL, _IOLBF, 0);

    signal(SIGUSR1, sigusr1_handler);

    /* Node 0 and others share the same receive/forward pattern */
    while (!stop_requested) {
        apple_t a;
        if (read_full(read_fd, &a, sizeof(a)) < 0) break;

        if (a.dest == DEST_EMPTY) {
            /* Empty apple: only node 0 injects new message; others just forward */
            if (my_id == 0) {
                printf("[Node %d, pid=%d] Apple returned empty. Ready for new message.\n",
                       my_id, getpid());
                /* Prompt the user for destination and message */
                char dest_buf[64];
                char text_buf[MAX_TEXT];

                printf("Enter destination node [0..%d] (or 'q' to quit): ", g_k - 1);
                fflush(stdout);
                if (!fgets(dest_buf, sizeof(dest_buf), stdin)) {
                    /* stdin closed; shut down */
                    a.dest = DEST_EMPTY;
                    /* forward empty apple so others keep flowing */
                    if (write_full(write_fd, &a, sizeof(a)) < 0) break;
                    continue;
                }
                chomp(dest_buf);
                if (strcmp(dest_buf, "q") == 0 || strcmp(dest_buf, "Q") == 0) {
                    /* Emulate Ctrl-C path */
                    raise(SIGINT);
                    break;
                }
                int dest;
                if (parse_destination(dest_buf, g_k, &dest) != 0) {
                    printf("Invalid destination '%s'. Forwarding empty apple.\n", dest_buf);
                    /* just forward empty apple */
                    if (write_full(write_fd, &a, sizeof(a)) < 0) break;
                    continue;
                }

                printf("Enter message: ");
                fflush(stdout);
                if (!fgets(text_buf, sizeof(text_buf), stdin)) {
                    text_buf[0] = '\0';
                }
                chomp(text_buf);

                a.dest   = dest;
                a.origin = my_id;
                strncpy(a.text, text_buf, sizeof(a.text)-1);
                a.text[sizeof(a.text)-1] = '\0';

                printf("[Node %d] Injecting message -> dest=%d, text=\"%s\"\n",
                       my_id, a.dest, a.text);
                if (write_full(write_fd, &a, sizeof(a)) < 0) break;
            } else {
                /* Non-zero nodes just forward an empty apple */
                printf("[Node %d, pid=%d] Received empty apple. Forwarding.\n",
                       my_id, getpid());
                if (write_full(write_fd, &a, sizeof(a)) < 0) break;
            }
        } else {
            /* Non-empty message in flight */
            if (a.dest == my_id) {
                /* We are the intended recipient */
                printf("[Node %d, pid=%d] Received message from node %d: \"%s\"\n",
                       my_id, getpid(), a.origin, a.text);
                /* Process it (for demo: just print), then clear header */
                a.dest = DEST_EMPTY;
                a.origin = my_id;
                a.text[0] = '\0';
                printf("[Node %d] Processed message. Returning empty apple.\n", my_id);
                if (write_full(write_fd, &a, sizeof(a)) < 0) break;
            } else {
                /* Not for us: forward unchanged */
                printf("[Node %d, pid=%d] Forwarding message destined for node %d.\n",
                       my_id, getpid(), a.dest);
                if (write_full(write_fd, &a, sizeof(a)) < 0) break;
            }
        }
    }

    /* Graceful exit */
    printf("[Node %d, pid=%d] Exiting.\n", my_id, getpid());
    if (read_fd  >= 0) close(read_fd);
    if (write_fd >= 0) close(write_fd);
    _exit(0);
}

int main(void) {
    /* Make stdout line-buffered for all processes so logs appear quickly */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("=== One Bad Apple (CIS 452) ===\n");
    int k;
    printf("Enter number of nodes k (2..%d): ", MAX_K);
    fflush(stdout);
    if (scanf("%d", &k) != 1 || k < 2 || k > MAX_K) {
        fprintf(stderr, "Invalid k.\n");
        return 1;
    }
    /* Consume the newline left by scanf so that fgets works later */
    int c;
    while ((c = getchar()) != '\n' && c != EOF) {}

    g_k = k;

    /* Allocate k pipes: pipe[i] used from node i -> (i+1)%k */
    int pipes[MAX_K][2];
    for (int i = 0; i < k; ++i) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            return 1;
        }
    }

    /* Fork k-1 children (node ids 1..k-1). Parent is node 0 */
    for (int i = 1; i < k; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        } else if (pid == 0) {
            /* Child process: becomes node i */
            g_parent = 0;
            my_id = i;

            /* Close all unused fds; keep read from left neighbor and write to self pipe */
            for (int j = 0; j < k; ++j) {
                if (j == (i - 1 + k) % k) {
                    /* keep pipes[j][0] open for reading */
                } else {
                    close(pipes[j][0]);
                }
                if (j == i) {
                    /* keep pipes[j][1] open for writing */
                } else {
                    close(pipes[j][1]);
                }
            }
            read_fd  = pipes[(i - 1 + k) % k][0];
            write_fd = pipes[i][1];

            /* Run node loop */
            node_loop();
            /* never returns */
        } else {
            /* Parent: remember child pid */
            child_pids[num_children++] = pid;
        }
    }

    /* Parent (node 0) sets up its own fds */
    my_id   = 0;
    read_fd = pipes[(0 - 1 + k) % k][0];  /* read from k-1 */
    write_fd= pipes[0][1];                /* write to 0 -> 1 */

    /* Close all other unused fds in parent */
    for (int j = 0; j < k; ++j) {
        if (j != (k - 1)) close(pipes[j][0]);
        if (j != 0)       close(pipes[j][1]);
    }

    /* Install Ctrl-C handler in parent */
    struct sigaction sa = {0};
    sa.sa_handler = sigint_parent_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    /* Also handle SIGUSR1 in parent (e.g., if someone signals us) */
    signal(SIGUSR1, sigusr1_handler);

    printf("[Node 0, pid=%d] Ring created with k=%d nodes.\n", getpid(), k);
    printf("[Node 0] Instructions: When prompted, enter a destination [0..%d] and a message.\n", k-1);
    printf("          Press Ctrl-C (or enter 'q' at destination prompt) to exit.\n");

    /* Seed the ring with an empty apple to start the cycle */
    apple_t seed = {.dest = DEST_EMPTY, .origin = 0};
    seed.text[0] = '\0';
    if (write_full(write_fd, &seed, sizeof(seed)) < 0) {
        perror("write(seed)");
        /* try to shutdown */
        raise(SIGINT);
    }

    /* Enter node loop as node 0 */
    node_loop();
    return 0;
}
