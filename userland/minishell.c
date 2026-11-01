/*
 * Yousj OS - minishell: HOST-SIDE PROTOTYPE of the server shell (yousj-sh).
 *
 * This file is a prototype. It compiles and runs on the Linux host with
 * the ordinary POSIX interface, purely to validate the shell's logic
 * (prompt, line reading, tokenizing, builtins, external commands)
 * before Yousj has a scheduler / execve to run it natively.
 * It does NOT run on Yousj yet. See userland/DESIGN.md section 6/8.
 *
 * Porting map, POSIX (here) -> Yousj (only numbers already defined in
 * kernel/include/yousj/syscall.h are used on the Yousj side):
 *
 *   fgets(stdin) line read      -> read            (YOUSJ_SYS_read,       0)
 *   printf/fputs output         -> write           (YOUSJ_SYS_write,      1)
 *   fork()                      -> clone, fork-style flag combination
 *                                  (YOUSJ_SYS_clone,                    56)
 *                                  (exact flags pending scheduler design)
 *   execvp()                    -> execve + shell-side "/bin" path join
 *                                  (YOUSJ_SYS_execve,                   59)
 *                                  (execvp's PATH search is libc doing
 *                                   execve repeatedly; Yousj v1 does the
 *                                   same search in the shell itself)
 *   waitpid()                   -> wait4           (YOUSJ_SYS_wait4,     61)
 *   exit()                      -> exit            (YOUSJ_SYS_exit,      60)
 *   chdir()                     -> chdir           (YOUSJ_SYS_chdir,     80)
 *   getcwd()                    -> getcwd          (YOUSJ_SYS_getcwd,    79)
 *   getpid() (prompt/debug)     -> getpid          (YOUSJ_SYS_getpid,    39)
 *
 * Scope matches DESIGN.md v1: whitespace tokenizing only; no quotes,
 * escapes, globbing, pipes or redirection (pipes additionally wait on
 * a dup2-class syscall proposal, DESIGN.md P5).
 *
 * Build on the host:  gcc -Wall -Wextra -o /tmp/minishell minishell.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_LINE 1024
#define MAX_ARGS 64

/* Print "yousj:<cwd> $ " and flush so it appears before the read. */
static void print_prompt(void)
{
    char cwd[MAX_LINE];

    if (getcwd(cwd, sizeof(cwd)) == NULL)
        snprintf(cwd, sizeof(cwd), "?");
    printf("yousj:%s $ ", cwd);
    fflush(stdout);
}

/* Split line in place on spaces/tabs/newlines. Returns argc, and
 * argv[] is NULL-terminated. line is modified (separators become \0). */
static int tokenize(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *p = line;

    while (*p != '\0' && argc < max_args - 1) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (*p == '\0')
            break;
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' &&
               *p != '\n' && *p != '\r')
            p++;
        if (*p != '\0')
            *p++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

static void builtin_help(void)
{
    puts("yousj minishell (host prototype) - builtins:");
    puts("  cd [dir]     change directory (no arg: go to $HOME, else /)");
    puts("  pwd          print working directory");
    puts("  echo [args]  print arguments separated by spaces");
    puts("  exit [code]  exit the shell");
    puts("  help         this text");
    puts("Anything else is run as an external command (fork + execvp + wait).");
}

static int builtin_cd(char **argv)
{
    const char *dir = argv[1];

    if (dir == NULL) {
        dir = getenv("HOME");
        if (dir == NULL)
            dir = "/";
    }
    if (chdir(dir) != 0) {
        fprintf(stderr, "yousj-sh: cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    return 0;
}

static int builtin_echo(char **argv)
{
    int i;

    for (i = 1; argv[i] != NULL; i++) {
        if (i > 1)
            putchar(' ');
        fputs(argv[i], stdout);
    }
    putchar('\n');
    return 0;
}

/* Run an external command. Returns its exit status, or 127-style
 * reporting via the child when exec fails (shell convention). */
static int run_external(char **argv)
{
    pid_t pid = fork();
    int status;

    if (pid < 0) {
        fprintf(stderr, "yousj-sh: fork: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        /* Child: replace image. On Yousj: clone + execve, see header. */
        execvp(argv[0], argv);
        fprintf(stderr, "yousj-sh: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    /* Parent: wait for this child specifically. Yousj: wait4(61). */
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "yousj-sh: waitpid: %s\n", strerror(errno));
            return 1;
        }
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "yousj-sh: %s: killed by signal %d\n",
                argv[0], WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return 1;
}

int main(void)
{
    char line[MAX_LINE];
    char *argv[MAX_ARGS];
    int argc;
    int last_status = 0;

    for (;;) {
        print_prompt();

        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* EOF: Ctrl-D at a terminal, or end of piped input. */
            putchar('\n');
            break;
        }

        argc = tokenize(line, argv, MAX_ARGS);
        if (argc == 0)
            continue; /* empty line: just show a fresh prompt */

        if (strcmp(argv[0], "exit") == 0) {
            if (argv[1] != NULL)
                last_status = atoi(argv[1]);
            break;
        } else if (strcmp(argv[0], "cd") == 0) {
            last_status = builtin_cd(argv);
        } else if (strcmp(argv[0], "pwd") == 0) {
            char cwd[MAX_LINE];

            if (getcwd(cwd, sizeof(cwd)) != NULL)
                puts(cwd);
            else {
                fprintf(stderr, "yousj-sh: pwd: %s\n", strerror(errno));
                last_status = 1;
                continue;
            }
            last_status = 0;
        } else if (strcmp(argv[0], "echo") == 0) {
            last_status = builtin_echo(argv);
        } else if (strcmp(argv[0], "help") == 0) {
            builtin_help();
            last_status = 0;
        } else {
            last_status = run_external(argv);
        }
    }

    return last_status;
}
