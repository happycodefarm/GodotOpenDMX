// ---------------------------------------------------------------------------
// opendmx_helper : drives an Enttec Open DMX USB (FTDI FT232R) from stdin.
//
// The Open DMX has no microcontroller: the host must generate the DMX512
// stream itself (break, mark-after-break, start code, 512 slots at 250 kbaud
// 8N2) and repeat it continuously. Godot has no serial port API, so the addon
// spawns this small program and writes raw 512-byte frames to its stdin.
//
// Usage:
//   opendmx_helper --list            list candidate serial ports
//   opendmx_helper [--hold] [--pro|--open] [port]
//                                    run; first candidate port if none given
//
// Two kinds of interface are supported:
//   - Open DMX (FT232R, no microcontroller): the helper generates the whole
//     DMX signal itself (--open);
//   - DMX USB Pro (FT245R + microcontroller): the helper only sends framed
//     "Output Only Send DMX" messages and the interface does the timing (--pro).
// Without either flag the kind is guessed from the port name: the Pro's USB
// serial number starts with "EN" (usb:EN..., /dev/cu.usbserial-EN...). On
// Linux (ttyUSB*) and Windows (COMx) the name says nothing: pass --pro.
//
// stdin  : raw 512-byte blocks, one per DMX universe update
// stdout : "READY <port>" once the port is open
// stderr : error messages
//
// When stdin closes (Godot quit or closed the port) the helper sends one
// all-zero frame (unless --hold) and exits.
//
// Two ways to reach the interface:
//   - as a serial port: built-in driver on macOS and Linux
//     (/dev/cu.usbserial*, /dev/ttyUSB*), FTDI VCP driver on Windows (COMx);
//   - straight over USB with libusb, when built with -DUSE_LIBUSB. Ports are
//     then named "usb:<serial number>". This is the only way on a Mac where
//     FTDI's D2XXHelper is installed (QLC+ and others need it), because it
//     stops the serial port from appearing.
//
// Build, from this folder:
//   macOS   : clang -O2 -DUSE_LIBUSB -I/usr/local/include/libusb-1.0 \
//               -o ../bin/macos/opendmx_helper opendmx_helper.c \
//               /usr/local/lib/libusb-1.0.a \
//               -framework IOKit -framework CoreFoundation -framework Security
//             (without libusb: clang -O2 -o ../bin/macos/opendmx_helper opendmx_helper.c)
//   Linux   : cc -O2 -o ../bin/linux/opendmx_helper opendmx_helper.c
//             (with libusb: add -DUSE_LIBUSB $(pkg-config --cflags --libs libusb-1.0))
//   Windows : x86_64-w64-mingw32-gcc -O2 -o ../bin/windows/opendmx_helper.exe opendmx_helper.c
//             (or: cl /O2 opendmx_helper.c)
// ---------------------------------------------------------------------------
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DMX_CHANNELS 512
#define DMX_BAUD 250000

// Timings (µs). The standard asks for break >= 88 and MAB >= 8; longer is fine.
#define BREAK_US 200
#define MAB_US 20

// Time for 513 slots of 11 bits at 250 kbaud, plus margin for the bytes still
// sitting in the FTDI chip's transmit buffer after the drain call returns.
#define FRAME_US (513 * 44 + 3000)

// DMX USB Pro: message framing and refresh period (~33 Hz, below the ~40 Hz
// the interface can output)
#define PRO_START 0x7E
#define PRO_END 0xE7
#define PRO_LABEL_SEND_DMX 6
#define PRO_HEADER 4
#define PRO_MESSAGE_LEN (PRO_HEADER + DMX_CHANNELS + 1 + 1)
#define PRO_FRAME_US 30000

#define MAX_PORTS 32
#define PORT_NAME_LEN 256

// --- Platform layer ---------------------------------------------------------
// Every platform section below implements these functions.

// Fills names[] with the candidate ports, returns how many were found.
static int find_ports(char names[][PORT_NAME_LEN], int max);
// Opens and configures the port. Returns 0, or -1 after printing an error.
static int port_open(const char *path);
static void port_close(void);
// Each returns 0 on success, -1 if the port is gone.
static int port_break(int on);
static int port_write(const uint8_t *data, size_t len);
static void port_drain(void);
static const char *port_error(void);
// Reads what is waiting on stdin without blocking.
// Returns the byte count, 0 if nothing is waiting, -1 once stdin is closed.
static int stdin_read(uint8_t *buf, size_t max);
static uint64_t now_us(void);
static void sleep_us(uint64_t us);

#ifdef _WIN32
// --- Windows ----------------------------------------------------------------
#include <windows.h>
#ifdef _MSC_VER
#pragma comment(lib, "advapi32")
#endif

static HANDLE port = INVALID_HANDLE_VALUE;

static const char *port_error(void) {
  static char msg[256];
  DWORD code = GetLastError();
  if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                      NULL, code, 0, msg, sizeof(msg), NULL)) {
    snprintf(msg, sizeof(msg), "error %lu", (unsigned long)code);
  }
  msg[strcspn(msg, "\r\n")] = 0;
  return msg;
}

// Serial ports are listed in the registry; FTDI virtual COM ports appear
// there under a device name containing "VCP".
static int find_ports(char names[][PORT_NAME_LEN], int max) {
  HKEY key;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0,
                    KEY_READ, &key) != ERROR_SUCCESS) {
    return 0;
  }
  int count = 0;
  for (DWORD i = 0; count < max; i++) {
    char device[256];
    BYTE com[PORT_NAME_LEN];
    DWORD device_len = sizeof(device), com_len = sizeof(com) - 1, type;
    if (RegEnumValueA(key, i, device, &device_len, NULL, &type, com,
                      &com_len) != ERROR_SUCCESS) {
      break;
    }
    if (type != REG_SZ || !strstr(device, "VCP")) continue;
    com[com_len] = 0;
    snprintf(names[count++], PORT_NAME_LEN, "%s", (const char *)com);
  }
  RegCloseKey(key);
  return count;
}

static int port_open(const char *path) {
  // COM10 and above only open with the \\.\ prefix; it is harmless below
  char full[PORT_NAME_LEN + 8];
  if (strncmp(path, "\\\\.\\", 4) == 0) snprintf(full, sizeof(full), "%s", path);
  else snprintf(full, sizeof(full), "\\\\.\\%s", path);

  port = CreateFileA(full, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                     0, NULL);
  if (port == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "ERROR cannot open %s: %s\n", path, port_error());
    return -1;
  }

  DCB dcb = {0};
  dcb.DCBlength = sizeof(dcb);
  if (!GetCommState(port, &dcb)) {
    fprintf(stderr, "ERROR %s is not a serial port: %s\n", path, port_error());
    port_close();
    return -1;
  }
  // 8 data bits, no parity, 2 stop bits, no flow control
  dcb.BaudRate = DMX_BAUD;
  dcb.ByteSize = 8;
  dcb.Parity = NOPARITY;
  dcb.StopBits = TWOSTOPBITS;
  dcb.fBinary = TRUE;
  dcb.fParity = FALSE;
  dcb.fOutxCtsFlow = FALSE;
  dcb.fOutxDsrFlow = FALSE;
  dcb.fDsrSensitivity = FALSE;
  dcb.fOutX = FALSE;
  dcb.fInX = FALSE;
  dcb.fDtrControl = DTR_CONTROL_DISABLE;
  // The Open DMX enables its RS-485 driver when RTS is cleared
  dcb.fRtsControl = RTS_CONTROL_DISABLE;
  if (!SetCommState(port, &dcb)) {
    fprintf(stderr, "ERROR cannot set %d baud 8N2 on %s: %s\n", DMX_BAUD, path,
            port_error());
    port_close();
    return -1;
  }

  COMMTIMEOUTS timeouts = {0};
  timeouts.WriteTotalTimeoutConstant = 1000;
  SetCommTimeouts(port, &timeouts);
  return 0;
}

static void port_close(void) {
  if (port != INVALID_HANDLE_VALUE) CloseHandle(port);
  port = INVALID_HANDLE_VALUE;
}

static int port_break(int on) {
  return (on ? SetCommBreak(port) : ClearCommBreak(port)) ? 0 : -1;
}

static int port_write(const uint8_t *data, size_t len) {
  DWORD written = 0;
  if (!WriteFile(port, data, (DWORD)len, &written, NULL)) return -1;
  return written == len ? 0 : -1;
}

static void port_drain(void) {
  FlushFileBuffers(port);
}

static int stdin_read(uint8_t *buf, size_t max) {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  DWORD waiting = 0, got = 0;
  // Fails with a broken pipe error once the other end is closed
  if (!PeekNamedPipe(in, NULL, 0, NULL, &waiting, NULL)) return -1;
  if (waiting == 0) return 0;
  if (waiting > max) waiting = (DWORD)max;
  if (!ReadFile(in, buf, waiting, &got, NULL) || got == 0) return -1;
  return (int)got;
}

static uint64_t now_us(void) {
  LARGE_INTEGER freq, count;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&count);
  return (uint64_t)(count.QuadPart * 1000000 / freq.QuadPart);
}

// Sleep() is only accurate to a few milliseconds: use it for the bulk of
// long waits and spin for the rest.
static void sleep_us(uint64_t us) {
  uint64_t end = now_us() + us;
  if (us > 4000) Sleep((DWORD)((us - 3000) / 1000));
  while (now_us() < end) {
  }
}

#else
// --- macOS and Linux --------------------------------------------------------
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <IOKit/serial/ioss.h>
#include <termios.h>
#define PORT_GLOB "/dev/cu.usbserial*"
#else
// The kernel header gives termios2, the only way to ask for 250000 baud.
// It cannot be combined with <termios.h>.
#include <asm/termbits.h>
#define PORT_GLOB "/dev/ttyUSB*"
#endif

static int port = -1;

static const char *port_error(void) {
  return strerror(errno);
}

static int find_ports(char names[][PORT_NAME_LEN], int max) {
  glob_t g;
  int count = 0;
  if (glob(PORT_GLOB, 0, NULL, &g) == 0) {
    for (size_t i = 0; i < g.gl_pathc && count < max; i++) {
      snprintf(names[count++], PORT_NAME_LEN, "%s", g.gl_pathv[i]);
    }
  }
  globfree(&g);
  return count;
}

// Sets 250000 baud, 8 data bits, no parity, 2 stop bits, no flow control
static int port_configure(const char *path) {
#ifdef __APPLE__
  struct termios tio;
  if (tcgetattr(port, &tio) < 0) {
    fprintf(stderr, "ERROR %s is not a serial port: %s\n", path, port_error());
    return -1;
  }
  cfmakeraw(&tio);
  tio.c_cflag &= ~(CSIZE | PARENB | CRTSCTS);
  tio.c_cflag |= CS8 | CSTOPB | CLOCAL | CREAD;
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0;
  if (tcsetattr(port, TCSANOW, &tio) < 0) {
    fprintf(stderr, "ERROR cannot configure %s: %s\n", path, port_error());
    return -1;
  }
  // 250000 baud is not a POSIX rate: macOS needs this ioctl, after tcsetattr
  speed_t speed = DMX_BAUD;
  if (ioctl(port, IOSSIOSPEED, &speed) < 0) {
    fprintf(stderr, "ERROR cannot set %d baud on %s: %s\n", DMX_BAUD, path,
            port_error());
    return -1;
  }
#else
  struct termios2 tio;
  if (ioctl(port, TCGETS2, &tio) < 0) {
    fprintf(stderr, "ERROR %s is not a serial port: %s\n", path, port_error());
    return -1;
  }
  tio.c_iflag = 0;
  tio.c_oflag = 0;
  tio.c_lflag = 0;
  // BOTHER = take the rate from c_ispeed / c_ospeed instead of a Bxxx code
  tio.c_cflag = CS8 | CSTOPB | CLOCAL | CREAD | BOTHER;
  tio.c_ispeed = DMX_BAUD;
  tio.c_ospeed = DMX_BAUD;
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0;
  if (ioctl(port, TCSETS2, &tio) < 0) {
    fprintf(stderr, "ERROR cannot set %d baud 8N2 on %s: %s\n", DMX_BAUD, path,
            port_error());
    return -1;
  }
#endif
  return 0;
}

static int port_open(const char *path) {
  // O_NONBLOCK so open() does not wait for a carrier signal
  port = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (port < 0) {
    fprintf(stderr, "ERROR cannot open %s: %s\n", path, port_error());
    return -1;
  }
  ioctl(port, TIOCEXCL);
  fcntl(port, F_SETFL, fcntl(port, F_GETFL) & ~O_NONBLOCK);

  if (port_configure(path) < 0) {
    port_close();
    return -1;
  }

  // The Open DMX enables its RS-485 driver when RTS is cleared
  int rts = TIOCM_RTS;
  ioctl(port, TIOCMBIC, &rts);

  signal(SIGPIPE, SIG_IGN);
  return 0;
}

static void port_close(void) {
  if (port >= 0) close(port);
  port = -1;
}

static int port_break(int on) {
  return ioctl(port, on ? TIOCSBRK : TIOCCBRK) < 0 ? -1 : 0;
}

static int port_write(const uint8_t *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = write(port, data + sent, len - sent);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    sent += (size_t)n;
  }
  return 0;
}

static void port_drain(void) {
#ifdef __APPLE__
  tcdrain(port);
#else
  // Same as tcdrain(), which <termios.h> would have provided
  ioctl(port, TCSBRK, 1);
#endif
}

static int stdin_read(uint8_t *buf, size_t max) {
  struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
  if (poll(&pfd, 1, 0) <= 0) return 0;
  ssize_t n = read(STDIN_FILENO, buf, max);
  return n > 0 ? (int)n : -1;
}

static uint64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void sleep_us(uint64_t us) {
  usleep((useconds_t)us);
}

#endif

#ifdef USE_LIBUSB
// --- Direct USB access to the FTDI chip (libusb) ----------------------------
#include <libusb.h>

#define FTDI_VID 0x0403
#define FT232R_PID 0x6001
#define USB_PREFIX "usb:"

// FTDI vendor requests
#define SIO_RESET 0
#define SIO_SET_MODEM_CTRL 1
#define SIO_SET_FLOW_CTRL 2
#define SIO_SET_BAUDRATE 3
#define SIO_SET_DATA 4

// The FT232R clock is 3 MHz: divisor 12 gives exactly 250000 baud
#define FTDI_DIVISOR (3000000 / DMX_BAUD)
// 8 data bits, no parity, 2 stop bits; bit 14 holds the line in break
#define FTDI_LINE_8N2 (8 | (2 << 11))
#define FTDI_LINE_BREAK (1 << 14)
#define FTDI_RTS_LOW 0x0200
#define FTDI_OUT_ENDPOINT 0x02

static libusb_context *usb_ctx = NULL;
static libusb_device_handle *usb_dev = NULL;
static int usb_status = 0;

static const char *usb_error(void) {
  return libusb_strerror(usb_status);
}

static int usb_control(int request, int value) {
  usb_status = libusb_control_transfer(
      usb_dev, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
                   LIBUSB_ENDPOINT_OUT,
      (uint8_t)request, (uint16_t)value, 0, NULL, 0, 1000);
  return usb_status < 0 ? -1 : 0;
}

// Reads the serial number of an FT232R, or returns -1 for any other device
static int usb_serial(libusb_device *dev, char *serial, int len) {
  struct libusb_device_descriptor desc;
  if (libusb_get_device_descriptor(dev, &desc) < 0) return -1;
  if (desc.idVendor != FTDI_VID || desc.idProduct != FT232R_PID) return -1;

  serial[0] = 0;
  libusb_device_handle *h;
  if (libusb_open(dev, &h) == 0) {
    libusb_get_string_descriptor_ascii(h, desc.iSerialNumber,
                                       (unsigned char *)serial, len);
    libusb_close(h);
  }
  return 0;
}

static int usb_find_ports(char names[][PORT_NAME_LEN], int max) {
  if (!usb_ctx && libusb_init(&usb_ctx) < 0) return 0;
  libusb_device **list;
  ssize_t total = libusb_get_device_list(usb_ctx, &list);
  int count = 0;
  for (ssize_t i = 0; i < total && count < max; i++) {
    char serial[64];
    if (usb_serial(list[i], serial, sizeof(serial)) < 0) continue;
    snprintf(names[count++], PORT_NAME_LEN, USB_PREFIX "%s", serial);
  }
  if (total >= 0) libusb_free_device_list(list, 1);
  return count;
}

static void usb_close(void) {
  if (usb_dev) {
    libusb_release_interface(usb_dev, 0);
    libusb_close(usb_dev);
  }
  usb_dev = NULL;
}

// path is "usb:<serial>"; an empty serial takes the first FT232R found
static int usb_open(const char *path) {
  const char *wanted = path + strlen(USB_PREFIX);
  if (!usb_ctx && (usb_status = libusb_init(&usb_ctx)) < 0) {
    fprintf(stderr, "ERROR cannot start libusb: %s\n", usb_error());
    return -1;
  }

  libusb_device **list;
  ssize_t total = libusb_get_device_list(usb_ctx, &list);
  usb_status = LIBUSB_ERROR_NO_DEVICE;
  for (ssize_t i = 0; i < total && !usb_dev; i++) {
    char serial[64];
    if (usb_serial(list[i], serial, sizeof(serial)) < 0) continue;
    if (wanted[0] && strcmp(wanted, serial) != 0) continue;
    usb_status = libusb_open(list[i], &usb_dev);
    if (usb_status < 0) usb_dev = NULL;
  }
  if (total >= 0) libusb_free_device_list(list, 1);
  if (!usb_dev) {
    fprintf(stderr, "ERROR cannot open %s: %s\n", path, usb_error());
    return -1;
  }

  // On Linux the ftdi_sio serial driver holds the interface: take it over
  libusb_set_auto_detach_kernel_driver(usb_dev, 1);
  usb_status = libusb_claim_interface(usb_dev, 0);
  if (usb_status < 0) {
    fprintf(stderr, "ERROR %s is in use by another program: %s\n", path,
            usb_error());
    usb_close();
    return -1;
  }

  // RTS low enables the Open DMX's RS-485 driver (the Pro ignores it)
  if (usb_control(SIO_RESET, 0) < 0 || usb_control(SIO_SET_FLOW_CTRL, 0) < 0 ||
      usb_control(SIO_SET_BAUDRATE, FTDI_DIVISOR) < 0 ||
      usb_control(SIO_SET_DATA, FTDI_LINE_8N2) < 0 ||
      usb_control(SIO_SET_MODEM_CTRL, FTDI_RTS_LOW) < 0) {
    fprintf(stderr, "ERROR cannot configure %s: %s\n", path, usb_error());
    usb_close();
    return -1;
  }
  return 0;
}

static int usb_break(int on) {
  return usb_control(SIO_SET_DATA,
                     FTDI_LINE_8N2 | (on ? FTDI_LINE_BREAK : 0));
}

static int usb_write(const uint8_t *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    int n = 0;
    usb_status = libusb_bulk_transfer(usb_dev, FTDI_OUT_ENDPOINT,
                                      (unsigned char *)data + sent,
                                      (int)(len - sent), &n, 1000);
    if (usb_status < 0) return -1;
    sent += (size_t)n;
  }
  return 0;
}
#endif

// --- Serial port or USB, chosen from the port name --------------------------

static int use_usb = 0;

static int io_find_ports(char names[][PORT_NAME_LEN], int max) {
  int count = 0;
#ifdef USE_LIBUSB
  count = usb_find_ports(names, max);
#endif
  return count + find_ports(names + count, max - count);
}

static int io_open(const char *path) {
#ifdef USE_LIBUSB
  use_usb = strncmp(path, USB_PREFIX, strlen(USB_PREFIX)) == 0;
  if (use_usb) return usb_open(path);
#endif
  return port_open(path);
}

static void io_close(void) {
#ifdef USE_LIBUSB
  if (use_usb) {
    usb_close();
    return;
  }
#endif
  port_close();
}

static int io_break(int on) {
#ifdef USE_LIBUSB
  if (use_usb) return usb_break(on);
#endif
  return port_break(on);
}

static int io_write(const uint8_t *data, size_t len) {
#ifdef USE_LIBUSB
  if (use_usb) return usb_write(data, len);
#endif
  return port_write(data, len);
}

// Waits until the driver has handed everything to the chip. A USB bulk
// transfer already does.
static void io_drain(void) {
  if (!use_usb) port_drain();
}

static const char *io_error(void) {
#ifdef USE_LIBUSB
  if (use_usb) return usb_error();
#endif
  return port_error();
}

// --- Common part ------------------------------------------------------------

static volatile sig_atomic_t running = 1;

static void on_signal(int sig) {
  (void)sig;
  running = 0;
}

static int is_pro = 0;

// DMX USB Pro: wraps the packet (start code + 512 slots) in a message. The
// interface produces the break and timings itself.
static int send_frame_pro(const uint8_t *packet) {
  uint8_t msg[PRO_MESSAGE_LEN];
  uint16_t len = DMX_CHANNELS + 1;
  msg[0] = PRO_START;
  msg[1] = PRO_LABEL_SEND_DMX;
  msg[2] = len & 0xFF;
  msg[3] = len >> 8;
  memcpy(msg + PRO_HEADER, packet, len);
  msg[PRO_HEADER + len] = PRO_END;

  uint64_t start = now_us();
  if (io_write(msg, sizeof(msg)) < 0) return -1;
  io_drain();
  uint64_t elapsed = now_us() - start;
  if (elapsed < PRO_FRAME_US) sleep_us(PRO_FRAME_US - elapsed);
  return 0;
}

// Sends one complete DMX packet. Returns 0 on success, -1 if the port is gone.
static int send_frame(const uint8_t *packet) {
  if (is_pro) return send_frame_pro(packet);
  if (io_break(1) < 0) return -1;
  sleep_us(BREAK_US);
  if (io_break(0) < 0) return -1;
  sleep_us(MAB_US);

  uint64_t start = now_us();
  if (io_write(packet, DMX_CHANNELS + 1) < 0) return -1;
  io_drain();

  // Never start the next break before the last slot has left the wire
  uint64_t elapsed = now_us() - start;
  if (elapsed < FRAME_US) sleep_us(FRAME_US - elapsed);
  return 0;
}

int main(int argc, char **argv) {
  static char ports[MAX_PORTS][PORT_NAME_LEN];
  const char *path = NULL;
  int hold = 0;
  int kind = -1;  // -1 = guess from the port name, 0 = Open DMX, 1 = Pro
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--list") == 0) {
      int count = io_find_ports(ports, MAX_PORTS);
      for (int p = 0; p < count; p++) printf("%s\n", ports[p]);
      return 0;
    }
    if (strcmp(argv[i], "--hold") == 0) hold = 1;
    else if (strcmp(argv[i], "--pro") == 0) kind = 1;
    else if (strcmp(argv[i], "--open") == 0) kind = 0;
    else path = argv[i];
  }
  if (!path) {
    if (io_find_ports(ports, MAX_PORTS) == 0) {
      fprintf(stderr, "ERROR no Open DMX / DMX USB Pro serial port found\n");
      return 1;
    }
    path = ports[0];
  }

  is_pro = kind >= 0 ? kind
                     : (strstr(path, "usbserial-EN") != NULL ||
                        strncmp(path, "usb:EN", 6) == 0);
  if (io_open(path) < 0) return 1;

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  printf("READY %s%s\n", path, is_pro ? " (DMX USB Pro)" : "");
  fflush(stdout);

  // packet[0] is the DMX start code (0), packet[1..512] the channel values
  uint8_t packet[DMX_CHANNELS + 1] = {0};
  uint8_t incoming[DMX_CHANNELS];
  size_t filled = 0;
  int status = 0;

  while (running) {
    // Drain every update waiting on stdin, keeping only the most recent
    int n;
    while ((n = stdin_read(incoming + filled, DMX_CHANNELS - filled)) > 0) {
      filled += (size_t)n;
      if (filled == DMX_CHANNELS) {
        memcpy(packet + 1, incoming, DMX_CHANNELS);
        filled = 0;
      }
    }
    if (n < 0) break;

    if (send_frame(packet) < 0) {
      fprintf(stderr, "ERROR port %s lost: %s\n", path, io_error());
      status = 1;
      break;
    }
  }

  if (!hold && status == 0) {
    memset(packet, 0, sizeof(packet));
    send_frame(packet);
  }
  io_close();
  return status;
}
