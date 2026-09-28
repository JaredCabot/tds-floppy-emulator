#ifndef VERSION_H
#define VERSION_H

/* Firmware version: 0xMMmmpp (major.minor.patch). Bump it for every release;
 * tools/mkupdate.py stamps it into UPDATE.UPD, and the application exposes it
 * as dbg_fw_version (SWD). */
#define FW_VERSION      0x010200u
#define FW_VERSION_STR  "1.2.0"

#endif /* VERSION_H */
