// Copyright (c) 2026 Cesanta Software Limited
// All rights reserved

#include "hal.h"
#include "mongoose.h"

#ifndef UART_DEBUG
#define UART_DEBUG USART3
#define UART_DEBUG_TX_PIN PIN('D', 8)
#define UART_DEBUG_RX_PIN PIN('D', 9)
#else
#define UART_DEBUG_TX_PIN PIN('A', 9)
#define UART_DEBUG_RX_PIN PIN('A', 10)
#endif

#define LED1 PIN('B', 0)
#define LED2 PIN('B', 7)
#define LED3 PIN('B', 14)

#define STM32F_FLASH_SIZE_REG ((uintptr_t) 0x1FFF7A22U)

struct upload_state {
  bool active;
  size_t expected;
  size_t received;
  size_t written;
  uint32_t crc32;
  char tail[512];
  size_t tail_len;
};

static struct upload_state s_upload;

static void log_fn(char ch, void *param) {
  hal_uart_write_buf(param, &ch, 1);
}

static void blink_task(void) {
  static uint64_t blink_timer = 0;
  if (hal_timer_expired(&blink_timer, 500, hal_get_tick())) {
    hal_gpio_toggle(LED1);
  }
}

uint64_t mg_millis(void) {
  return hal_get_tick();
}

bool mg_random(void *buf, size_t len) {
  for (size_t n = 0; n < len; n += sizeof(uint32_t)) {
    uint32_t r = hal_rng_read();
    memcpy((char *) buf + n, &r, n + sizeof(r) > len ? len - n : sizeof(r));
  }
  return true;
}

static bool flash_write_aligned(const void *buf, size_t len) {
  char *dst = (char *) mg_flash->start + mg_flash->size / 2;
  dst += s_upload.written;
  bool ok = mg_flash->write_fn(dst, buf, len);
  if (ok) s_upload.written += len;
  return ok;
}

static bool flash_flush_tail(void) {
  if (s_upload.tail_len == 0) return true;
  size_t len = MG_ROUND_UP(s_upload.tail_len, mg_flash->align);
  memset(s_upload.tail + s_upload.tail_len, 0xff, len - s_upload.tail_len);
  return flash_write_aligned(s_upload.tail, len);
}

static bool flash_stream_write(const void *buf, size_t len) {
  const char *p = (const char *) buf;
  while (len > 0) {
    size_t n = sizeof(s_upload.tail) - s_upload.tail_len;
    if (n > len) n = len;
    memcpy(s_upload.tail + s_upload.tail_len, p, n);
    s_upload.tail_len += n;
    p += n, len -= n;
    if (s_upload.tail_len == sizeof(s_upload.tail)) {
      if (!flash_write_aligned(s_upload.tail, sizeof(s_upload.tail)))
        return false;
      s_upload.tail_len = 0;
    }
  }
  return true;
}

static void finish_upload(struct mg_connection *c, const char *error) {
  if (error == NULL && !flash_flush_tail()) error = "flash write failed";
  if (error == NULL) {
    MG_INFO(("Upload complete: %lu bytes, crc32 %#lx",
             (unsigned long) s_upload.received, (unsigned long) s_upload.crc32));
    mg_http_reply(c, 200, "", "stored %lu bytes, crc32 %#lx\n",
                  (unsigned long) s_upload.received,
                  (unsigned long) s_upload.crc32);
  } else {
    MG_ERROR(("Upload failed: %s", error));
    mg_http_reply(c, 500, "", "%s\n", error);
  }
  memset(&s_upload, 0, sizeof(s_upload));
  c->is_draining = 1;
}

static void upload_handler(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_READ && c->recv.len > 0) {
    size_t n = c->recv.len;
    const char *error = NULL;
    if (s_upload.received + n > s_upload.expected) {
      error = "body larger than Content-Length";
    } else if (!flash_stream_write(c->recv.buf, n)) {
      error = "flash write failed";
    } else {
      s_upload.crc32 = mg_crc32(s_upload.crc32, (char *) c->recv.buf, n);
      s_upload.received += n;
      mg_iobuf_del(&c->recv, 0, n);
      if (s_upload.received == s_upload.expected) finish_upload(c, NULL);
    }
    if (error != NULL) {
      mg_iobuf_del(&c->recv, 0, c->recv.len);
      finish_upload(c, error);
    }
  } else if (ev == MG_EV_CLOSE && s_upload.active) {
    MG_ERROR(("Upload closed at %lu/%lu bytes",
              (unsigned long) s_upload.received,
              (unsigned long) s_upload.expected));
    memset(&s_upload, 0, sizeof(s_upload));
  }
  (void) ev_data;
}

static void reject_upload(struct mg_connection *c, int status,
                          const char *message) {
  mg_http_reply(c, status, "", "%s\n", message);
  c->pfn = NULL;
  mg_iobuf_del(&c->recv, 0, c->recv.len);
  c->is_draining = 1;
}

static void start_upload(struct mg_connection *c, struct mg_http_message *hm) {
  const size_t flash_size =
      (size_t) (MG_REG(STM32F_FLASH_SIZE_REG) & 0xffff) * 1024;
  struct mg_str *cl = mg_http_get_header(hm, "Content-Length");
  size_t len = 0;
  if (s_upload.active) {
    reject_upload(c, 409, "upload already in progress");
  } else if (cl == NULL) {
    reject_upload(c, 411, "Content-Length required");
  } else {
    for (size_t i = 0; i < cl->len; i++) {
      if (cl->buf[i] < '0' || cl->buf[i] > '9') {
        reject_upload(c, 400, "bad Content-Length");
        return;
      }
      len = len * 10 + (size_t) (cl->buf[i] - '0');
    }
    if (flash_size == 0 || mg_flash == NULL || mg_flash->write_fn == NULL) {
      reject_upload(c, 500, "flash is not available");
    } else if (MG_ROUND_UP(len, mg_flash->align) > flash_size / 2) {
      mg_http_reply(c, 413, "", "body too large, max %lu bytes\n",
                    (unsigned long) (flash_size / 2));
      c->pfn = NULL;
      mg_iobuf_del(&c->recv, 0, c->recv.len);
      c->is_draining = 1;
    } else {
      memset(&s_upload, 0, sizeof(s_upload));
      mg_flash->size = flash_size;
      s_upload.active = true;
      s_upload.expected = len;
      c->fn = upload_handler;
      c->pfn = NULL;  // Stop buffering full HTTP body
      mg_iobuf_del(&c->recv, 0, hm->head.len);  // Keep body bytes only
      MG_INFO(("Upload start: %lu bytes to %p", (unsigned long) len,
               (char *) mg_flash->start + flash_size / 2));
      if (len == 0) finish_upload(c, NULL);
      else upload_handler(c, MG_EV_READ, &c->recv.len);
    }
  }
}

static void http_ev_handler(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_HTTP_HDRS) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    if (mg_strcmp(hm->method, mg_str("POST")) == 0 &&
        mg_match(hm->uri, mg_str("/upload"), NULL)) {
      start_upload(c, hm);
    }
  } else if (ev == MG_EV_HTTP_MSG) {  // New HTTP request received
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    if (mg_match(hm->uri, mg_str("/api/tick"), NULL)) {
      mg_http_reply(c, 200, "", "{%m:%llu}\n", MG_ESC("tick"), hal_get_tick());
    } else if (mg_match(hm->uri, mg_str("/upload"), NULL)) {
      mg_http_reply(c, 405, "", "POST a binary body to /upload\n");
    } else {
      mg_http_reply(c, 200, "", "Hi from Mongoose, tick %llu\n",
                    hal_get_tick());
    }
  }
}

int main(void) {
  hal_clock_init();
  hal_rng_init();
  hal_gpio_output(LED1);
  hal_gpio_output(LED2);
  hal_gpio_output(LED3);
  hal_uart_init(UART_DEBUG, UART_DEBUG_TX_PIN, UART_DEBUG_RX_PIN, 115200);
  mg_log_set_fn(log_fn, UART_DEBUG);

  hal_ethernet_init();

  // Start a minimal web server
  struct mg_mgr mgr;
  mg_mgr_init(&mgr);
  mg_http_listen(&mgr, "http://0.0.0.0", http_ev_handler, NULL);

  for (;;) {
    mg_mgr_poll(&mgr, 0);
    blink_task();
  }

  return 0;
}
