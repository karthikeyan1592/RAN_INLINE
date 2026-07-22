#include <sys/mount.h>
#include <sys/reboot.h>
#include <linux/reboot.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>

static void out(const char *s) { write(1, s, strlen(s)); }

int main(void)
{
	mount("proc", "/proc", "proc", 0, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	usleep(300000);

	out("=== /proc/mtd ===\n");
	int fd = open("/proc/mtd", O_RDONLY);
	if (fd >= 0) {
		char buf[2048];
		int n;
		while ((n = read(fd, buf, sizeof(buf))) > 0)
			write(1, buf, n);
		close(fd);
	} else {
		out("open /proc/mtd failed\n");
	}

	out("=== raw /dev listing ===\n");
	DIR *d = opendir("/dev");
	if (d) {
		struct dirent *de;
		while ((de = readdir(d)))
			if (strstr(de->d_name, "mtd")) { out(de->d_name); out("\n"); }
		closedir(d);
	} else {
		out("opendir /dev failed\n");
	}

	out("=== /dev listing (mtd*) ===\n");
	/* crude: just try opening a handful of plausible nodes */
	const char *cands[] = {
		"/dev/mtd0", "/dev/mtd1", "/dev/mtd2", "/dev/mtd3", "/dev/mtd4",
		"/dev/mtd5", "/dev/mtd6",
		"/dev/mtdblock0", "/dev/mtdblock1", "/dev/mtdblock2",
		"/dev/mtdblock3", "/dev/mtdblock4", "/dev/mtdblock5", "/dev/mtdblock6", NULL
	};
	for (int i = 0; cands[i]; i++) {
		int f = open(cands[i], O_RDONLY);
		if (f >= 0) {
			out(cands[i]);
			out(" : OK\n");
			close(f);
		} else {
			out(cands[i]);
			out(" : missing\n");
		}
	}

	out("=== halting ===\n");
	sync();
	reboot(LINUX_REBOOT_CMD_POWER_OFF);
	for (;;) ;
	return 0;
}
