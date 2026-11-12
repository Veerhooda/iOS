/*
 * op2shell: a root shell on the USB console (docs/USB-CONSOLE-PLAN.md stage 2).
 *
 * Installed IN PLACE over /usr/local/bin/op2-air-run (9 MB slot, tools/rootfs_g3.py), whose launchd job
 * com.op2.air-run runs it once at every boot as root. It carries the dash shell (built by the user from
 * out/shell/dash-0.5.12, BSD licence) inside itself (.incbin), writes it to /usr/local/bin/dash when it is not
 * there or differs, then forks away from launchd and supervises: open /dev/op2usb (bsd/dev/op2_usb.c, the CDC-ACM
 * gadget's line-disciplined stream), run `dash -i` on it as stdin/stdout/stderr, and when the shell exits (or the
 * device is not ready yet) wait a second and start another.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern const unsigned char op2_dash_start[], op2_dash_end[];
__asm__(".section __DATA,__dash\n"
        ".globl _op2_dash_start\n_op2_dash_start:\n"
        ".incbin \"" DASH_BIN "\"\n"
        ".globl _op2_dash_end\n_op2_dash_end:\n"
        ".text\n");

#define DASH "/usr/local/bin/dash"

static void
say(const char *s)   /* to the console (and so the USB log until the shell opens it) */
{
	int fd = open("/dev/console", O_WRONLY);
	if (fd >= 0) {
		write(fd, s, strlen(s));
		close(fd);
	}
}

static int
install_dash(void)
{
	size_t n = (size_t)(op2_dash_end - op2_dash_start);
	struct stat st;
	if (stat(DASH, &st) == 0 && (size_t)st.st_size == n) {
		int fd = open(DASH, O_RDONLY);
		if (fd >= 0) {
			unsigned char *b = malloc(n);
			ssize_t r = b ? read(fd, b, n) : -1;
			close(fd);
			int same = r == (ssize_t)n && memcmp(b, op2_dash_start, n) == 0;
			free(b);
			if (same)
				return 0;
		}
	}
	mkdir("/usr/local", 0755);
	mkdir("/usr/local/bin", 0755);
	int fd = open(DASH ".new", O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (fd < 0)
		return -1;
	ssize_t w = write(fd, op2_dash_start, n);
	fsync(fd);
	close(fd);
	if (w != (ssize_t)n || rename(DASH ".new", DASH) != 0)
		return -1;
	chmod(DASH, 0755);
	return 0;
}

int
main(void)
{
	if (fork() != 0)
		return 0;   /* launchd's job ends; the supervisor lives on */
	setsid();
	signal(SIGHUP, SIG_IGN);
	say(install_dash() == 0 ? "op2shell: dash ready, waiting for /dev/op2usb\n" : "op2shell: cannot install dash\n");
	for (unsigned tries = 0;; tries++) {
		int fd = open("/dev/op2usb", O_RDWR);
		if (fd < 0) {
			sleep(tries < 60 ? 1 : 5);
			continue;
		}
		pid_t pid = fork();
		if (pid == 0) {
			setsid();
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
			if (fd > 2)
				close(fd);
			chdir("/");
			static char *env[] = { "PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", "HOME=/var/root",
				"TERM=dumb", "USER=root", "SHELL=" DASH, "PS1=op2# ", NULL };
			write(1, "\r\nOnePlus 2 iOS root shell (dash). ^D or exit restarts it.\n", 59);
			execle(DASH, "-dash", "-i", (char *)NULL, env);
			_exit(127);
		}
		close(fd);
		int status;
		while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR)
			;
		sleep(1);
	}
}
