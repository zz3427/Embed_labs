/*
 *
 * CSEE 4840 Lab 2 for 2019
 *
 * Name/UNI: zz3427 Gerald Zhao; dc3949 Derrick Chen; zw3161 Zening Wang
 */

 /*
 * References:
 *
 * https://web.archive.org/web/20130307100215/http://beej.us/guide/bgnet/output/html/singlepage/bgnet.html
 *
 * http://www.thegeekstuff.com/2011/12/c-socket-programming/
 * 
 */


#include "fbputchar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "usbkeyboard.h"
#include <pthread.h>
#include <linux/fb.h>


/* Update SERVER_HOST to be the IP address of
 * the chat server you are connecting to
 */
/* arthur.cs.columbia.edu */
#define SERVER_HOST "128.59.19.114"
#define SERVER_PORT 42000

#define BUFFER_SIZE 256
// #define COLS 64
// #define ROWS 24

// /* Layout: receive region + separator + input region (bottom 2 rows) */
// #define RX_ROWS (ROWS - 3)     /* top region height */
// #define SEP_ROW (ROWS - 3)     /* separator line row */
// #define IN_ROW0 (ROWS - 2)     /* input row 0 */
// #define IN_ROW1 (ROWS - 1)     /* input row 1 */

#define INPUT_MAX 256
#define PROMPT "> "

/* Key repeat (for “hold r” demo) */
#define REPEAT_DELAY_MS 400
#define REPEAT_RATE_MS  45

//  Forward declarations 
static void render_input(void);
static int max_input_chars(void);
static void clear_screen(void);
static void draw_separator(void);
static void clear_line(int row);

static void rx_newline(void);
static void rx_putc(char ch);
static void rx_puts_wrapped(const char *s);

static void input_insert_char(char ch);
static void input_backspace(void);
static void input_left(void);
static void input_right(void);
static void input_clear(void);

// Globals

static int cols, rows;
static int rx_rows, sep_row, in_row0, in_row1;

static int sockfd = -1; /* Socket file descriptor */
static struct libusb_device_handle *keyboard = NULL;
static uint8_t endpoint_address = 0;

static pthread_t network_thread;
static pthread_mutex_t fb_lock = PTHREAD_MUTEX_INITIALIZER;

static uint8_t prev_mods = 0;

/* Receive cursor state */
static int rx_row = 0;
static int rx_col = 0;

/* Input editing state */
static char input_buf[INPUT_MAX];
static int input_len = 0;
static int input_cur = 0;

/* Key repeat state */
static uint8_t held_keycode = 0;
static uint8_t held_mods = 0;
static int held_active = 0;
static long long held_next_ms = 0;

// defined in fbputchar.c 
extern struct fb_var_screeninfo fb_vinfo;

static void compute_screen_layout(void) {
  cols = (int)(fb_vinfo.xres / 16);
  rows = (int)(fb_vinfo.yres / 32);

  if (cols < 10) cols = 10;
  if (rows < 6)  rows = 6;

  rx_rows = rows - 3;
  sep_row = rows - 3;
  in_row0 = rows - 2;
  in_row1 = rows - 1;
}


static long long now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
}

static void clear_line(int row){
  for (int c = 0; c < cols; c++){
    fbputchar(' ', row, c);
  }
}

static void clear_screen(void) {
  for (int r = 0; r < rows; r++) {
    clear_line(r);
  }
}

static void draw_separator(void) {
  for (int c = 0; c < cols; c++) {
    fbputchar('-', sep_row, c);
  }
}

static void rx_newline(void) {
  rx_row++;
  rx_col = 0;

  if (rx_row >= rx_rows) {
    // Clear whole screen 
    clear_screen();
    draw_separator();
    rx_row = 0;
    rx_col = 0;

    // re-render input so it’s still visible 
    render_input();
  }

  clear_line(rx_row);
}

static void rx_putc(char ch) {
  if (ch == '\r') return;

  if (ch == '\n') {
    rx_newline();
    return;
  }

  if (rx_col >= cols) {
    rx_newline();
  }

  fbputchar(ch, rx_row, rx_col);
  rx_col++;

  if (rx_col >= cols) {
    rx_newline();
  }
}

static void rx_puts_wrapped(const char *s) {
  for (const char *p = s; *p; p++) {
    rx_putc(*p);
  }
}

//  Render input area (bottom two rows)
static void render_input(void) {
  clear_line(in_row0);
  clear_line(in_row1);

  // Compose PROMPT + input
  char disp[INPUT_MAX + 8];
  snprintf(disp, sizeof(disp), "%s%.*s", PROMPT, input_len, input_buf);

  int total = (int)strlen(disp);
  int max_chars = 2 * cols;

  if (total > max_chars) {
    // If user typed too much, show only the last 2*COLS chars
    int start = total - max_chars;
    memmove(disp, disp + start, (size_t)max_chars);
    disp[max_chars] = '\0';
    total = max_chars;
  }

  for (int i = 0; i < total && i < 2 * cols; i++) {
    int r = (i < cols) ? in_row0 : in_row1;
    int c = (i < cols) ? i : (i - cols);
    fbputchar(disp[i], r, c);
  }

  /* Cursor position relative to start of input area */
  int cursor_pos = (int)strlen(PROMPT) + input_cur;
  if (cursor_pos < 0) cursor_pos = 0;
  if (cursor_pos >= 2 * cols) cursor_pos = 2 * cols - 1;

  int cr = (cursor_pos < cols) ? in_row0 : in_row1;
  int cc = (cursor_pos < cols) ? cursor_pos : (cursor_pos - cols);

  /* Draw a simple visible cursor */
  fbputchar('_', cr, cc);
}

//  Input editing helpers 
static void input_clear(void) {
  memset(input_buf, 0, sizeof(input_buf));
  input_len = 0;
  input_cur = 0;
}

static void input_insert_char(char ch) {
  int cap = max_input_chars();
  if (input_len >= cap) return;
  if (input_len >= INPUT_MAX - 1) return;
  if (input_cur < 0) input_cur = 0;
  if (input_cur > input_len) input_cur = input_len;

  memmove(&input_buf[input_cur + 1],
          &input_buf[input_cur],
          (size_t)(input_len - input_cur));
  input_buf[input_cur] = ch;
  input_len++;
  input_cur++;
}

static void input_backspace(void) {
  if (input_cur <= 0 || input_len <= 0) return;

  memmove(&input_buf[input_cur - 1],
          &input_buf[input_cur],
          (size_t)(input_len - input_cur));
  input_len--;
  input_cur--;
}

static void input_left(void) {
  if (input_cur > 0) input_cur--;
}

static void input_right(void) {
  if (input_cur < input_len) input_cur++;
}

static int shift_down(uint8_t mods) {
  /* Left shift bit 0x02, right shift bit 0x20 */
  return (mods & 0x02) || (mods & 0x20);
}

static int max_input_chars(void) {
  int prompt_len = (int)strlen(PROMPT);
  int cap = 2 * cols - prompt_len - 1;   // -1 so cursor fits
  if (cap < 0) cap = 0;
  if (cap > INPUT_MAX - 1) cap = INPUT_MAX - 1;
  return cap;
}

// kayboard HID Mapping
static char hid_to_ascii(uint8_t keycode, int shift) {
  // a-z 
  if (keycode >= 0x04 && keycode <= 0x1d) {
    char base = (char)('a' + (keycode - 0x04));
    return shift ? (char)(base - 32) : base;
  }

  // 1-0 
  if (keycode >= 0x1e && keycode <= 0x27) {
    static const char normal[]  = "1234567890";
    static const char shifted[] = "!@#$%^&*()";
    int idx = keycode - 0x1e;
    return shift ? shifted[idx] : normal[idx];
  }

  //  space + punctuation 
  switch (keycode) {
    case 0x2c: return ' ';               // space
    case 0x2d: return shift ? '_' : '-';
    case 0x2e: return shift ? '+' : '=';
    case 0x2f: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    case 0x2b: return '\t';              // Tab
    default:   return 0;
  }
}

static void send_current_input(void) {
  char out[INPUT_MAX + 4];
  int n = snprintf(out, sizeof(out), "%.*s\n", input_len, input_buf);
  if (n <= 0) return;

  (void)write(sockfd, out, (size_t)n);

  pthread_mutex_lock(&fb_lock);
  input_clear();
  render_input();
  pthread_mutex_unlock(&fb_lock);
}

static void handle_keycode(uint8_t mods, uint8_t kc) {
  if (kc == 0) return;

  /* ESC: quit */
  if (kc == 0x29) {
    pthread_cancel(network_thread);
    if (sockfd >= 0) close(sockfd);
    exit(0);
  }

  /* Enter */
  if (kc == 0x28) {
    pthread_mutex_lock(&fb_lock);
    /* If you want Enter to send even empty line, remove this guard */
    if (input_len > 0) {
      pthread_mutex_unlock(&fb_lock);
      send_current_input();
    } else {
      input_clear();
      render_input();
      pthread_mutex_unlock(&fb_lock);
    }
    return;
  }

  /* Backspace */
  if (kc == 0x2a) {
    pthread_mutex_lock(&fb_lock);
    input_backspace();
    render_input();
    pthread_mutex_unlock(&fb_lock);
    return;
  }

  /* Left / Right arrows */
  if (kc == 0x50) {
    pthread_mutex_lock(&fb_lock);
    input_left();
    render_input();
    pthread_mutex_unlock(&fb_lock);
    return;
  }
  if (kc == 0x4f) {
    pthread_mutex_lock(&fb_lock);
    input_right();
    render_input();
    pthread_mutex_unlock(&fb_lock);
    return;
  }

  /* Printable */
  char ch = hid_to_ascii(kc, shift_down(mods));
  if (ch) {
    pthread_mutex_lock(&fb_lock);
    input_insert_char(ch);
    render_input();
    pthread_mutex_unlock(&fb_lock);
  }
}

// New press” detection
static int key_in_list(uint8_t kc, const uint8_t list[6]) {
  for (int i = 0; i < 6; i++) {
    if (list[i] == kc) return 1;
  }
  return 0;
}

// Network Thread
static void *network_thread_f(void *ignored) {
  char recvBuf[BUFFER_SIZE];
  int n;

  while ((n = (int)read(sockfd, recvBuf, BUFFER_SIZE - 1)) > 0) {
    recvBuf[n] = '\0';

    pthread_mutex_lock(&fb_lock);
    rx_puts_wrapped(recvBuf);
    render_input(); // keep input region stable
    pthread_mutex_unlock(&fb_lock);

    //  print to terminal 
    write(STDOUT_FILENO, recvBuf, (size_t)n);
  }
  return NULL;
}


int main()
{
  int err, col;

  struct sockaddr_in serv_addr;

  struct usb_keyboard_packet packet;
  int transferred;
  char keystate[12];
  uint8_t prev_keys[6] = {0};

  if ((err = fbopen()) != 0) {
    fprintf(stderr, "Error: Could not open framebuffer: %d\n", err);
    exit(1);
  }
  compute_screen_layout();

  pthread_mutex_lock(&fb_lock);
  clear_screen();
  draw_separator();
  rx_row = 0;
  rx_col = 0;
  input_clear();
  render_input();
  pthread_mutex_unlock(&fb_lock);

  //keyboard
  if ((keyboard = openkeyboard(&endpoint_address)) == NULL) {
    fprintf(stderr, "Did not find a keyboard\n");
    exit(1);
  }

  //socket connect
  if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    fprintf(stderr, "Error: Could not create socket\n");
    exit(1);
  }

  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(SERVER_PORT);

  if (inet_pton(AF_INET, SERVER_HOST, &serv_addr.sin_addr) <= 0) {
    fprintf(stderr, "Error: Could not convert host IP \"%s\"\n", SERVER_HOST);
    exit(1);
  }

  if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
    fprintf(stderr, "Error: connect() failed. Is the server reachable?\n");
    exit(1);
  }


  // /* Draw rows of asterisks across the top and bottom of the screen */
  // for (col = 0 ; col < 64 ; col++) {
  //   fbputchar('*', 0, col);
  //   fbputchar('*', 23, col);
  // }

  // fbputs("Hello CSEE 4840 World!", 4, 10);


  /* Start the network thread */
  pthread_create(&network_thread, NULL, network_thread_f, NULL);

  /* Look for and handle keypresses */
  for (;;) {
    libusb_interrupt_transfer(keyboard, endpoint_address,
                              (unsigned char *)&packet, sizeof(packet),
                              &transferred, 0);

    if (transferred != (int)sizeof(packet)) continue;

    uint8_t mods = packet.modifiers;
    int mods_changed = (mods != prev_mods);

    /* Handle newly pressed keys */
    for (int i = 0; i < 6; i++) {
      uint8_t kc = packet.keycode[i];
      if (kc != 0 && !key_in_list(kc, prev_keys)) {

        int is_special =
          (kc == 0x28) || (kc == 0x2a) || (kc == 0x4f) || (kc == 0x50) || (kc == 0x29);

        if (mods_changed && !is_special) {
          continue;  // prevents “shift tap re-types held letter”
        }

        handle_keycode(mods, kc);

        if (!mods_changed) {
          held_keycode = kc;
          held_mods = mods;
          held_active = 1;
          held_next_ms = now_ms() + REPEAT_DELAY_MS;
        }
        
      }
    }
    prev_mods = mods;


    /* Key repeat: if held key still present in current report */
    if (held_active) {
      int still_down = 0;
      for (int i = 0; i < 6; i++) {
        if (packet.keycode[i] == held_keycode) {
          still_down = 1;
          break;
        }
      }

      if (!still_down) {
        held_active = 0;
      } else {
        long long t = now_ms();
        if (t >= held_next_ms) {
          /* Repeat only for sensible keys */
          if (held_keycode == 0x2a || held_keycode == 0x4f || held_keycode == 0x50 ||
              hid_to_ascii(held_keycode, shift_down(held_mods))) {
            handle_keycode(held_mods, held_keycode);
          }
          held_next_ms = t + REPEAT_RATE_MS;
        }
      }
    }

    /* Save previous keycodes */
    memcpy(prev_keys, packet.keycode, 6);
  }
  return 0;
}