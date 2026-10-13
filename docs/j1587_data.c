#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>

int set_interface_attribs(int fd, int speed) {
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) return -1;

    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= (CLOCAL | CREAD);

    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY);
    tty.c_lflag &= ~(ECHO | ECHOE | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_oflag &= ~OPOST;

    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;

    return tcsetattr(fd, TCSANOW, &tty);
}

int wait_for_cts(int fd, int timeout_ms) {
    uint8_t buf[64];
    int total = 0;
    int elapsed = 0;

    printf("   Waiting for CTS from sign...\n");
    while (elapsed < timeout_ms) {
        int n = read(fd, buf + total, sizeof(buf) - total);
        if (n > 0) {
            total += n;
            if (total >= 10) {
                printf("   [Sign CTS RX %d bytes]:", total);
                for (int i = 0; i < total; i++) printf(" %02X", buf[i]);
                printf("\n");
                return 1;
            }
        } else {
            usleep(10000);
            elapsed += 10;
        }
    }
    printf("   [Timeout]: Received %d bytes\n", total);
    return 0;
}

void drain_bus(int fd, int timeout_ms, const char *label) {
    uint8_t buf[128];
    int elapsed = 0;
    int total = 0;

    while (elapsed < timeout_ms) {
        int n = read(fd, buf + total, sizeof(buf) - total);
        if (n > 0) {
            total += n;
            elapsed = 0;
        } else {
            usleep(20000);
            elapsed += 20;
        }
    }

    if (total > 0) {
        printf("   [%s RX %d bytes]:", label, total);
        for (int i = 0; i < total; i++) printf(" %02X", buf[i]);
        printf("\n");
    } else {
        printf("   [%s RX]: (quiet / 0 bytes)\n", label);
    }
}

int main(int argc, char **argv) {
    const char *port = (argc > 1) ? argv[1] : "/dev/ttyUSB0";
    int fd = open(port, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    set_interface_attribs(fd, B9600);

    // 1. Line quiet time
    printf("1. Arming UART (600ms silence)...\n");
    tcflush(fd, TCIOFLUSH);
    usleep(600000);

    // 2. Packet 1: Known-good PID 0xC5 RTS (CS: 0x77)
    uint8_t pkt1[] = {
        0xBC, 0xC5, 0x07, 0xBD, 0x01, 0x00, 0x00, 0x00, 0x42, 0x01, 0x77
    };
    printf("2. Sending Packet 1 (PID 0xC5 RTS)...\n");
    write(fd, pkt1, sizeof(pkt1));
    tcdrain(fd);

    // 3. Catch the sign's CTS response frame
    if (!wait_for_cts(fd, 1500)) {
        printf("Failed to get CTS from sign.\n");
        close(fd);
        return 1;
    }

    // 4. Line turnaround pause (allows sign to de-assert RS-485 DE)
    usleep(80000);

    // 5. Packet 2: PID 0xC6 Text Data ("HELLO" + EOT, CS: 0xBB)
    uint8_t pkt2[] = {
	    0xBC, 0xC6, 0x0A, 0xBD, 0x01,
	    'A', 0x42,
	    'H', 'E', 'L', 'L', 'O',
	    0x04,
	    0xBB
    };
    printf("3. Sending Packet 2 (PID 0xC6 Text \"HELLO\")...\n");
    write(fd, pkt2, sizeof(pkt2));
    tcdrain(fd);

    // 6. Check for Sign ACK / End-of-Message response
    drain_bus(fd, 500, "Post-Packet 2 Response");

    usleep(500000);
    close(fd);
    printf("Done.\n");
    return 0;
}
