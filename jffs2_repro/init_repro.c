#include <sys/mount.h>
#include <sys/reboot.h>
#include <linux/reboot.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

static void out(const char *s) { write(1, s, strlen(s)); }

int main(void)
{
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	usleep(300000);

	out("=== mounting rwfs (mtdblock5) as jffs2 on /mnt ===\n");
	mkdir("/mnt", 0755);
	int rc = mount("/dev/mtdblock5", "/mnt", "jffs2", 0, NULL);
	if (rc) {
		out("mount FAILED\n");
	} else {
		out("mount OK\n");

		out("=== reading /mnt/testfile.bin (triggers rtime decompress) ===\n");
		int fd = open("/mnt/testfile.bin", O_RDONLY);
		if (fd < 0) {
			out("open FAILED\n");
		} else {
			char buf[4096];
			int n = read(fd, buf, sizeof(buf));
			out("read() returned: ");
			char nb[16]; int i = 0;
			int v = n;
			if (v < 0) { nb[i++] = '-'; v = -v; }
			char tmp[16]; int t = 0;
			if (v == 0) tmp[t++] = '0';
			while (v) { tmp[t++] = '0' + v % 10; v /= 10; }
			while (t) nb[i++] = tmp[--t];
			write(1, nb, i);
			out("\n");
			close(fd);
		}
	}

	out("=== idling 20s to observe background GC thread behavior via dmesg ===\n");
	for (int i = 0; i < 20; i++) {
		sleep(1);
		out(".");
	}
	out("\n=== halting ===\n");
	sync();
	reboot(LINUX_REBOOT_CMD_POWER_OFF);
	for (;;) ;
	return 0;
}
