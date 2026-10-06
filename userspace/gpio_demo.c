// SPDX-License-Identifier: GPL-2.0-only
/*
 * gpio_demo - example userspace client for /dev/rpi3gpio0
 *
 * Uses poll() to block until a button event is available, then mirrors
 * the button state onto the LED: PRESSED -> LED on, RELEASED -> LED off.
 * Demonstrates the full open/write/poll/read loop documented in the
 * repo's README.
 */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DEV_PATH "/dev/rpi3gpio0"

static int led_write(int fd, char value)
{
	return write(fd, &value, 1) == 1 ? 0 : -1;
}

int main(void)
{
	int fd = open(DEV_PATH, O_RDWR);

	if (fd < 0) {
		perror("open " DEV_PATH);
		return 1;
	}

	printf("Waiting for button events on %s (Ctrl+C to quit)\n", DEV_PATH);

	struct pollfd pfd = { .fd = fd, .events = POLLIN };

	for (;;) {
		int ret = poll(&pfd, 1, -1);

		if (ret < 0) {
			perror("poll");
			break;
		}

		if (pfd.revents & POLLIN) {
			char buf[16] = {0};
			ssize_t n = read(fd, buf, sizeof(buf) - 1);

			if (n <= 0)
				continue;

			printf("button event: %s", buf);

			if (strncmp(buf, "PRESSED", 7) == 0)
				led_write(fd, '1');
			else if (strncmp(buf, "RELEASED", 8) == 0)
				led_write(fd, '0');
		}
	}

	close(fd);
	return 0;
}
