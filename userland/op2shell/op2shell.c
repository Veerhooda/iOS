/*
 * op2shell: a root shell on the USB console (docs/USB-CONSOLE-PLAN.md stage 2).
 *
 * Installed IN PLACE over /usr/local/bin/op2-air-run (9 MB slot, tools/rootfs_g3.py), whose launchd job
 * com.op2.air-run runs it once at every boot as root. It carries dash (out/shell/dash-0.5.12), toybox,
 * dropbear+scp and an authorized_keys inside itself (.incbin), installs them (plus toybox applet symlinks and
 * a /bin/sh link for dropbear's login shell), starts dropbear on port 22, then forks away from launchd and
 * supervises: open /dev/op2usb (bsd/dev/op2_usb.c, the CDC-ACM gadget's line-disciplined stream), run `dash -i`
 * on it as stdin/stdout/stderr, and when the shell exits (or the device is not ready yet) wait a second and
 * start another.
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
extern const unsigned char op2_toybox_start[], op2_toybox_end[];
__asm__(".section __DATA,__toybox\n"
        ".globl _op2_toybox_start\n_op2_toybox_start:\n"
        ".incbin \"" TOYBOX_BIN "\"\n"
        ".globl _op2_toybox_end\n_op2_toybox_end:\n"
        ".text\n");
extern const unsigned char op2_dropbear_start[], op2_dropbear_end[];
__asm__(".section __DATA,__dropbear\n"
        ".globl _op2_dropbear_start\n_op2_dropbear_start:\n"
        ".incbin \"" DROPBEAR_BIN "\"\n"
        ".globl _op2_dropbear_end\n_op2_dropbear_end:\n"
        ".text\n");
extern const unsigned char op2_scp_start[], op2_scp_end[];
__asm__(".section __DATA,__scpbinary\n"
        ".globl _op2_scp_start\n_op2_scp_start:\n"
        ".incbin \"" SCP_BIN "\"\n"
        ".globl _op2_scp_end\n_op2_scp_end:\n"
        ".text\n");
extern const unsigned char op2_authkeys_start[], op2_authkeys_end[];
__asm__(".section __DATA,__authkeys\n"
        ".globl _op2_authkeys_start\n_op2_authkeys_start:\n"
        ".incbin \"" AUTHKEYS_BIN "\"\n"
        ".globl _op2_authkeys_end\n_op2_authkeys_end:\n"
        ".text\n");

#define DASH "/usr/local/bin/dash"
#define TOYBOX "/usr/local/bin/toybox"
#define DROPBEAR "/usr/local/bin/dropbear"
#define SCP "/usr/local/bin/scp"
#define AUTHKEYS "/var/root/.ssh/authorized_keys"

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
install_blob(const char *path, const unsigned char *start, const unsigned char *end)
{
	size_t n = (size_t)(end - start);
	struct stat st;
	char *tmp;
	if (stat(path, &st) == 0 && (size_t)st.st_size == n) {
		int fd = open(path, O_RDONLY);
		if (fd >= 0) {
			unsigned char *b = malloc(n);
			ssize_t r = b ? read(fd, b, n) : -1;
			close(fd);
			int same = r == (ssize_t)n && memcmp(b, start, n) == 0;
			free(b);
			if (same)
				return 0;
		}
	}
	mkdir("/usr/local", 0755);
	mkdir("/usr/local/bin", 0755);
	tmp = malloc(strlen(path) + 5);
	if (tmp == NULL)
		return -1;
	sprintf(tmp, "%s.new", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	ssize_t w = fd < 0 ? -1 : write(fd, start, n);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
	int rc = (w != (ssize_t)n || rename(tmp, path) != 0) ? -1 : 0;
	free(tmp);
	if (rc == 0)
		chmod(path, 0755);
	return rc;
}

static int
install_authkeys(void)
{
	size_t n = (size_t)(op2_authkeys_end - op2_authkeys_start);
	struct stat st;
	mkdir("/var/root", 0755);
	mkdir("/var/root/.ssh", 0700);
	chmod("/var/root/.ssh", 0700);
	if (stat(AUTHKEYS, &st) == 0 && (size_t)st.st_size == n) {
		int fd = open(AUTHKEYS, O_RDONLY);
		if (fd >= 0) {
			unsigned char *b = malloc(n);
			ssize_t r = b ? read(fd, b, n) : -1;
			close(fd);
			int same = r == (ssize_t)n && memcmp(b, op2_authkeys_start, n) == 0;
			free(b);
			if (same) {
				chmod(AUTHKEYS, 0600);
				return 0;
			}
		}
	}
	int fd = open(AUTHKEYS, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -1;
	ssize_t w = write(fd, op2_authkeys_start, n);
	fsync(fd);
	close(fd);
	chmod(AUTHKEYS, 0600);
	return w == (ssize_t)n ? 0 : -1;
}

/* applet symlinks for toybox; never shadow an existing file */
static void
install_toybox_links(void)
{
	static const char *applets[] = { "ls", "cat", "grep", "od", "wc", "id", "uname", "file", "df", "du",
		"head", "tail", "tee", "tr", "basename", "dirname", "mkdir", "rm", "cp", "mv", "ln",
		"touch", "chmod", NULL };
	char path[64];
	struct stat st;
	for (unsigned i = 0; applets[i] != NULL; i++) {
		snprintf(path, sizeof(path), "/usr/local/bin/%s", applets[i]);
		if (lstat(path, &st) == 0)
			continue;
		symlink("toybox", path);
	}
}

int
main(void)
{
	if (fork() != 0)
		return 0;   /* launchd's job ends; the supervisor lives on */
	setsid();
	signal(SIGHUP, SIG_IGN);
	int ok = install_blob(DASH, op2_dash_start, op2_dash_end) == 0 &&
	    install_blob(TOYBOX, op2_toybox_start, op2_toybox_end) == 0 &&
	    install_blob(DROPBEAR, op2_dropbear_start, op2_dropbear_end) == 0 &&
	    install_blob(SCP, op2_scp_start, op2_scp_end) == 0 && install_authkeys() == 0;
	if (ok) {
		struct stat st;
		install_toybox_links();
		if (lstat("/bin/sh", &st) != 0)   /* dropbear runs the login shell */
			symlink(DASH, "/bin/sh");
		mkdir("/etc/dropbear", 0755);   /* -R generates host keys here on first boot */
		pid_t db = fork();
		if (db == 0) {   /* ssh daemon: host keys auto-created (-R), log to /tmp */
			int lfd = open("/tmp/dropbear.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
			if (lfd >= 0) {
				dup2(lfd, 1);
				dup2(lfd, 2);
				if (lfd > 2)
					close(lfd);
			}
			execl(DROPBEAR, "dropbear", "-R", "-E", "-p", "22", (char *)NULL);
			_exit(1);
		} else if (db > 0) {
			waitpid(db, NULL, 0);   /* reap the starter; the daemon lives on */
		}
	}
	say(ok ? "op2shell: dash+toybox+ssh ready, waiting for /dev/op2usb\n" :
	    "op2shell: cannot install shell\n");
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
