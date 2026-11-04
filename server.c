// server.c - ระบบคลังสินค้าคอมพิวเตอร์ (ภาษา C, POSIX sockets, ไม่ต้องติดตั้งไลบรารีเพิ่ม)
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define MAX_ITEMS 500
#define DB "data.csv"
typedef struct { int id, qty, min; char name[64], cat[32]; } Item;
static Item items[MAX_ITEMS];
static int n = 0, next_id = 1;

static void save(void) {
    FILE *f = fopen(DB, "w"); if (!f) return;
    for (int i = 0; i < n; i++)
        fprintf(f, "%d,%s,%s,%d,%d\n", items[i].id, items[i].name, items[i].cat, items[i].qty, items[i].min);
    fclose(f);
}
static void load(void) {
    FILE *f = fopen(DB, "r"); if (!f) return;
    char line[256];
    while (n < MAX_ITEMS && fgets(line, sizeof line, f)) {
        Item *t = &items[n];
        if (sscanf(line, "%d,%63[^,],%31[^,],%d,%d", &t->id, t->name, t->cat, &t->qty, &t->min) == 5) {
            if (t->id >= next_id) next_id = t->id + 1;
            n++;
        }
    }
    fclose(f);
}
static Item *find(int id) { for (int i = 0; i < n; i++) if (items[i].id == id) return &items[i]; return NULL; }

// อ่านค่าจาก body แบบ form (key=value&...) พร้อมถอดรหัส URL และกรองอักขระอันตรายออก
static int param(const char *body, const char *key, char *out, size_t sz) {
    size_t kl = strlen(key); const char *p = body;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1; size_t o = 0;
            while (*p && *p != '&' && o + 1 < sz) {
                if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                    char h[3] = {p[1], p[2], 0}; out[o++] = (char)strtol(h, NULL, 16); p += 3;
                } else if (*p == '+') { out[o++] = ' '; p++; }
                else out[o++] = *p++;
            }
            out[o] = 0;
            for (size_t i = 0; i < o; i++)
                if ((unsigned char)out[i] < 32 || strchr(",\"\\<>&", out[i])) out[i] = ' ';
            return 1;
        }
        p = strchr(p, '&'); if (p) p++;
    }
    return 0;
}
static int num(const char *body, const char *key) { char b[32]; return param(body, key, b, sizeof b) ? atoi(b) : 0; }

static void wr(int c, const char *p, size_t len) {
    while (len) { ssize_t k = write(c, p, len); if (k <= 0) return; p += k; len -= (size_t)k; }
}
static void reply(int c, int code, const char *type, const char *body, size_t len) {
    char h[256];
    int hl = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                      code, code == 200 ? "OK" : "Error", type, len);
    wr(c, h, (size_t)hl); wr(c, body, len);
}
static void fail(int c, int code, const char *m) { reply(c, code, "text/plain; charset=utf-8", m, strlen(m)); }

static void send_items(int c) {
    char *b = malloc((size_t)n * 220 + 16), *p = b;
    p += sprintf(p, "[");
    for (int i = 0; i < n; i++) {
        Item *t = &items[i];
        const char *st = t->qty <= 0 ? "out" : t->qty <= t->min ? "low" : "ok";  // หมด / ใกล้หมด / ปกติ
        p += sprintf(p, "%s{\"id\":%d,\"name\":\"%s\",\"cat\":\"%s\",\"qty\":%d,\"min\":%d,\"status\":\"%s\"}",
                     i ? "," : "", t->id, t->name, t->cat, t->qty, t->min, st);
    }
    p += sprintf(p, "]");
    reply(c, 200, "application/json", b, (size_t)(p - b)); free(b);
}
static void send_page(int c) {
    FILE *f = fopen("index.html", "rb");
    if (!f) { fail(c, 404, "index.html not found"); return; }
    fseek(f, 0, SEEK_END); long len = ftell(f); rewind(f);
    char *b = malloc((size_t)len + 1);
    size_t got = fread(b, 1, (size_t)len, f); fclose(f);
    reply(c, 200, "text/html; charset=utf-8", b, got); free(b);
}

static void handle(int c) {
    char req[8192]; int len = 0; ssize_t k;
    while (len < (int)sizeof req - 1 && (k = read(c, req + len, sizeof req - 1 - (size_t)len)) > 0) {
        len += (int)k; req[len] = 0;
        char *he = strstr(req, "\r\n\r\n");
        if (he) {
            char *cl = strcasestr(req, "Content-Length:");
            if (len - (int)(he + 4 - req) >= (cl ? atoi(cl + 15) : 0)) break;
        }
    }
    char method[8], path[64], name[64], cat[32];
    if (len <= 0 || sscanf(req, "%7s %63s", method, path) != 2) return;
    char *body = strstr(req, "\r\n\r\n"); body = body ? body + 4 : "";

    if (!strcmp(method, "GET") && !strcmp(path, "/")) { send_page(c); return; }
    if (!strcmp(method, "GET") && !strcmp(path, "/api/items")) { send_items(c); return; }
    if (strcmp(method, "POST")) { fail(c, 404, "not found"); return; }

    int q = num(body, "qty"); Item *it = find(num(body, "id")); const char *err = NULL;
    if (!strcmp(path, "/api/add")) {                       // เพิ่มสินค้าใหม่
        if (!param(body, "name", name, sizeof name) || !name[0] || q < 0 || n >= MAX_ITEMS) err = "ข้อมูลสินค้าไม่ถูกต้อง";
        else {
            if (!param(body, "cat", cat, sizeof cat) || !cat[0]) strcpy(cat, "ทั่วไป");
            Item *t = &items[n++]; t->id = next_id++; t->qty = q; t->min = num(body, "min");
            snprintf(t->name, sizeof t->name, "%s", name); snprintf(t->cat, sizeof t->cat, "%s", cat);
        }
    } else if (!strcmp(path, "/api/in")) {                 // นำเข้า
        if (!it || q <= 0 || q > 1000000) err = "จำนวนไม่ถูกต้อง"; else it->qty += q;
    } else if (!strcmp(path, "/api/out")) {                // นำออก
        if (!it || q <= 0) err = "จำนวนไม่ถูกต้อง";
        else if (q > it->qty) err = "สินค้าในสต็อกไม่พอ";
        else {
            it->qty -= q;
            if (it->qty <= it->min) fprintf(stderr, "[แจ้งเตือน] %s: %s เหลือ %d\n", it->qty ? "ใกล้หมด" : "หมด", it->name, it->qty);
        }
    } else if (!strcmp(path, "/api/del")) {                // ลบรายการ
        if (!it) err = "ไม่พบสินค้า";
        else { memmove(it, it + 1, (size_t)(items + n - it - 1) * sizeof *it); n--; }
    } else err = "ไม่รู้จักคำสั่งนี้";

    if (err) { fail(c, 400, err); return; }
    save(); reply(c, 200, "text/plain", "ok", 2);
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8080, on = 1;
    signal(SIGPIPE, SIG_IGN); load();
    int s = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((unsigned short)port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 16)) { perror("bind/listen"); return 1; }
    printf("คลังสินค้าทำงานที่ http://localhost:%d\n", port); fflush(stdout);
    for (;;) {
        int c = accept(s, NULL, NULL); if (c < 0) continue;
        struct timeval tv = {5, 0}; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        handle(c); close(c);
    }
}
