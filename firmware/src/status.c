/**
 * status.c - the text of EMUSTAT.TXT (status.h). Small formatting helpers
 * instead of printf, which the firmware does not carry.
 */
#include "status.h"

typedef struct { char *p; size_t cap, n; } out_t;

static void put(out_t *o, const char *s)
{
  while(*s)
  {
    if(o->n + 1 < o->cap) o->p[o->n] = *s;
    o->n++;
    s++;
  }
}

/* n with thousands separators: 1457664 -> "1,457,664" */
static void num(out_t *o, uint64_t v)
{
  char d[32];
  int k = 0, g = 0;
  do
  {
    if(g == 3) { d[k++] = ','; g = 0; }
    d[k++] = (char)('0' + v % 10u);
    v /= 10u;
    g++;
  } while(v);
  char s[32];
  for(int i = 0; i < k; i++) s[i] = d[k - 1 - i];
  s[k] = 0;
  put(o, s);
}

static void hex(out_t *o, uint32_t v, int digits)
{
  char s[9];
  for(int i = digits - 1; i >= 0; i--) { s[i] = "0123456789ABCDEF"[v & 15u]; v >>= 4; }
  s[digits] = 0;
  put(o, s);
}

static void line(out_t *o, const char *label, const char *value)
{
  put(o, label); put(o, value); put(o, "\r\n");
}

const char *status_reset_text(uint32_t rc)
{
  if(rc & (1u << 29)) return "watchdog";
  if(rc & (1u << 28)) return "software (update or restart)";
  if(rc & (1u << 27)) return "power-on";
  if(rc & (1u << 26)) return "reset pin";
  return "unknown";
}

size_t status_text(char *out, size_t cap, const status_t *s)
{
  out_t o = { out, cap, 0 };
  put(&o, "TDS Floppy Emulator status\r\n==========================\r\n\r\n");

  line(&o, "Firmware version  : ", s->version);
  put(&o, "Build ID          : "); hex(&o, s->build, 8); put(&o, "\r\n");
  line(&o, "Board             : ", s->board);
  put(&o, "MCU unique ID     : ");
  for(int i = 2; i >= 0; i--) { hex(&o, s->uid[i], 8); if(i) put(&o, "-"); }
  put(&o, "\r\nSPI flash ID      : "); hex(&o, s->flash_id, 6); put(&o, "\r\n");
  line(&o, "Clock             : ", s->hick ? "internal oscillator: the 8 MHz crystal did not start"
                                          : "8 MHz crystal");
  put(&o, "\r\n");

  put(&o, "Internal disk\r\n");
  line(&o, "Density           : ", s->dd ? "720 KB (DD)" : "1.44 MB (HD)");
  if(s->disk_ok)
  {
    put(&o, "Files             : "); num(&o, s->files); put(&o, "\r\n");
    put(&o, "Folders           : "); num(&o, s->dirs); put(&o, "\r\n");
    put(&o, "Used              : "); num(&o, s->used_bytes); put(&o, " bytes\r\n");
    put(&o, "Free              : "); num(&o, s->free_bytes); put(&o, " bytes\r\n\r\n");
  }
  else put(&o, "Contents          : not a readable FAT12 disk (format it in the host)\r\n\r\n");

  put(&o, "Health\r\n");
  line(&o, "Flash fault       : ", s->fault ? "YES: save the files with DATA OUT" : "no");
  put(&o, "Write-backs       : "); num(&o, s->writebacks); put(&o, " since power-on\r\n");
  put(&o, "Journal recoveries: "); num(&o, s->recovered); put(&o, " at the last power-on\r\n");
  put(&o, "Marker repairs    : "); num(&o, s->repaired); put(&o, " at the last power-on\r\n");
  line(&o, "Last reset        : ", status_reset_text(s->reset_cause));
  uint32_t t = s->uptime_ms / 1000u;
  put(&o, "Uptime            : ");
  num(&o, t / 3600u); put(&o, " h "); num(&o, t / 60u % 60u); put(&o, " min ");
  num(&o, t % 60u); put(&o, " s\r\n\r\n");

  put(&o, "USB flash drive\r\n");
  line(&o, "File system       : ", s->stick_fs);
  put(&o, "Capacity          : "); num(&o, s->stick_bytes); put(&o, " bytes\r\n\r\n");

  put(&o, "Last transfer\r\n");
  if(s->last_what)
  {
    put(&o, "Transfer          : "); put(&o, s->last_what); put(&o, ", ");
    put(&o, s->last_result); put(&o, ", "); num(&o, s->last_files); put(&o, s->last_files == 1 ? " file, " : " files, ");
    num(&o, s->last_bytes); put(&o, " bytes\r\n");
  }
  else put(&o, "None since power-on\r\n");

  if(cap == 0) return 0;
  out[o.n < cap ? o.n : cap - 1] = 0;
  return o.n < cap ? o.n : cap - 1;
}
