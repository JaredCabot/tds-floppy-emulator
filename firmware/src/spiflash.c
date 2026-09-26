/**
 * spiflash.c - SST25VF016B (16 Mbit SPI NOR, U8) driver on SPI2.
 *
 * Opcodes and the power-on block-protect gotcha are per the datasheet
 * (Datasheets/SST25VF016B-...pdf, tables 4-4 / 4-5, sections 4.4.x):
 *   - At power-up the BP bits are SET, so erase/program are silently ignored
 *     until we clear them with EWSR + WRSR(0x00). spiflash_init() does this.
 *   - Program only clears 1->0; erase (to 0xFF) is required first.
 *
 * Programming uses AAI word-program (0xAD, datasheet 4.4.4): ~2x faster than
 * byte-program. Reads and erases wait for any operation still in progress.
 */
#include "spiflash.h"
#include "at32f415_conf.h"
#include <string.h>

/* ---- pins (docs/02-pinmap.csv) ---- */
#define FLASH_SPI            SPI2
#define FLASH_SPI_CLK        CRM_SPI2_PERIPH_CLOCK
#define FLASH_GPIO           GPIOB
#define FLASH_GPIO_CLK       CRM_GPIOB_PERIPH_CLOCK
#define FLASH_CE_PIN         GPIO_PINS_12   /* PB12, software CS  */
#define FLASH_SCK_PIN        GPIO_PINS_13   /* PB13 SPI2_SCK      */
#define FLASH_MISO_PIN       GPIO_PINS_14   /* PB14 SPI2_MISO     */
#define FLASH_MOSI_PIN       GPIO_PINS_15   /* PB15 SPI2_MOSI     */

/* ---- opcodes ---- */
#define CMD_READ        0x03u
#define CMD_BYTE_PROG   0x02u
#define CMD_AAI_WORD    0xADu   /* auto address increment word program */
#define CMD_SECTOR_ERA  0x20u
#define CMD_RDSR        0x05u
#define CMD_EWSR        0x50u
#define CMD_WRSR        0x01u
#define CMD_WREN        0x06u
#define CMD_WRDI        0x04u
#define CMD_JEDEC_ID    0x9Fu

#define SR_BUSY         0x01u   /* status register bit 0 */

/* ---- low-level CS + byte transfer ---- */
static inline void ce_low(void)  { gpio_bits_reset(FLASH_GPIO, FLASH_CE_PIN); }
static inline void ce_high(void) { gpio_bits_set(FLASH_GPIO, FLASH_CE_PIN); }

static uint8_t xfer(uint8_t b)
{
  /* Bounded spins: if SPI2 never clocks (miswire / dead flash), don't hang the
   * whole boot - time out so the self-test reports failure (5 Hz LED) instead.
   * ~1e6 loop iterations is several ms, far longer than a real byte transfer at
   * 18 MHz (~0.5 us). (A missing flash chip does not trigger it: SPI still
   * clocks and reads 0xFF, which the JEDEC-ID check catches.) */
  uint32_t t = 1000000u;
  while(spi_i2s_flag_get(FLASH_SPI, SPI_I2S_TDBE_FLAG) == RESET) { if(--t == 0) return 0xFF; }
  spi_i2s_data_transmit(FLASH_SPI, b);
  t = 1000000u;
  while(spi_i2s_flag_get(FLASH_SPI, SPI_I2S_RDBF_FLAG) == RESET) { if(--t == 0) return 0xFF; }
  return (uint8_t)spi_i2s_data_receive(FLASH_SPI);
}

/* single-byte command with CS framing */
static void cmd(uint8_t op)
{
  ce_low();
  xfer(op);
  ce_high();
}

static void send_addr(uint32_t addr)
{
  xfer((addr >> 16) & 0xFF);
  xfer((addr >> 8) & 0xFF);
  xfer(addr & 0xFF);
}

static uint8_t read_status(void)
{
  ce_low();
  xfer(CMD_RDSR);
  uint8_t sr = xfer(0x00);
  ce_high();
  return sr;
}

/* Wait for the flash to finish an erase/program. Bounded so a stuck-busy status
 * (e.g. MISO floating high -> SR reads 0xFF -> BUSY always set) can't hang boot.
 * Returns true if it went idle, false on timeout. A 4KB sector erase is spec'd
 * up to ~25 ms; each read_status() is several SPI bytes, so ~2M tries is ample. */
static bool s_fault;                /* sticky: the flash stopped responding (spiflash_fault) */

static bool wait_busy(void)
{
  uint32_t t = 2000000u;
  while(read_status() & SR_BUSY) { if(--t == 0) { s_fault = true; return false; } }
  return true;
}

bool spiflash_fault(void) { return s_fault; }

/* ---- init ---- */
static void spi_pins_init(void)
{
  gpio_init_type gi;
  crm_periph_clock_enable(FLASH_GPIO_CLK, TRUE);
  crm_periph_clock_enable(FLASH_SPI_CLK, TRUE);

  /* CE# as push-pull output, idle high */
  gpio_default_para_init(&gi);
  gi.gpio_pins = FLASH_CE_PIN;
  gi.gpio_mode = GPIO_MODE_OUTPUT;
  gi.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gi.gpio_pull = GPIO_PULL_NONE;
  gi.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
  gpio_init(FLASH_GPIO, &gi);
  ce_high();

  /* SCK + MOSI as mux push-pull; MISO as mux input */
  gi.gpio_pins = FLASH_SCK_PIN | FLASH_MOSI_PIN;
  gi.gpio_mode = GPIO_MODE_MUX;
  gpio_init(FLASH_GPIO, &gi);

  gi.gpio_pins = FLASH_MISO_PIN;
  gi.gpio_mode = GPIO_MODE_INPUT;
  gi.gpio_pull = GPIO_PULL_UP;
  gpio_init(FLASH_GPIO, &gi);
}

static void spi_periph_init(void)
{
  spi_init_type si;
  spi_default_para_init(&si);
  si.transmission_mode = SPI_TRANSMIT_FULL_DUPLEX;
  si.master_slave_mode = SPI_MODE_MASTER;
  si.mclk_freq_division = SPI_MCLK_DIV_4;   /* 72 MHz APB1 / 4 = 18 MHz */
  si.first_bit_transmission = SPI_FIRST_BIT_MSB;
  si.frame_bit_num = SPI_FRAME_8BIT;
  si.clock_polarity = SPI_CLOCK_POLARITY_LOW;   /* SPI mode 0 */
  si.clock_phase = SPI_CLOCK_PHASE_1EDGE;
  si.cs_mode_selection = SPI_CS_SOFTWARE_MODE;  /* we drive CE# by GPIO */
  spi_init(FLASH_SPI, &si);

  /* software CS internal level high so the peripheral stays master-active */
  spi_software_cs_internal_level_set(FLASH_SPI, SPI_SWCS_INTERNAL_LEVEL_HIGHT);
  spi_enable(FLASH_SPI, TRUE);
}

/* clear power-on block protection so erase/program take effect */
static void unprotect(void)
{
  cmd(CMD_WREN);
  ce_low();
  xfer(CMD_EWSR);
  ce_high();
  ce_low();
  xfer(CMD_WRSR);
  xfer(0x00);          /* clear BP3..BP0 and BPL */
  ce_high();
}

bool spiflash_init(void)
{
  spi_pins_init();
  spi_periph_init();
  unprotect();
  return spiflash_read_jedec_id() == SPIFLASH_JEDEC_ID;
}

/* ---- public API ---- */
uint32_t spiflash_read_jedec_id(void)
{
  ce_low();
  xfer(CMD_JEDEC_ID);
  uint8_t m = xfer(0x00);
  uint8_t t = xfer(0x00);
  uint8_t d = xfer(0x00);
  ce_high();
  return ((uint32_t)m << 16) | ((uint32_t)t << 8) | d;
}

void spiflash_read(uint32_t addr, void *buf, uint32_t len)
{
  uint8_t *p = (uint8_t *)buf;
  wait_busy();                        /* an erase started without waiting may still run */
  ce_low();
  xfer(CMD_READ);
  send_addr(addr);
  while(len--) *p++ = xfer(0x00);
  ce_high();
}

/* Start a 4 KB sector erase and return at once (poll spiflash_busy()). */
void spiflash_erase_sector_start(uint32_t addr)
{
  wait_busy();
  cmd(CMD_WREN);
  ce_low();
  xfer(CMD_SECTOR_ERA);
  send_addr(addr);
  ce_high();
}

bool spiflash_busy(void) { return (read_status() & SR_BUSY) != 0; }

void spiflash_erase_sector(uint32_t addr)
{
  wait_busy();
  cmd(CMD_WREN);
  ce_low();
  xfer(CMD_SECTOR_ERA);
  send_addr(addr);
  ce_high();
  wait_busy();
}

void spiflash_erase_chip(void)
{
  wait_busy();                        /* like every other operation: a busy chip ignores WREN */
  cmd(CMD_WREN);
  cmd(0x60);                          /* chip erase */
  wait_busy();
}

static void prog_byte(uint32_t addr, uint8_t v)
{
  if(v == 0xFF) return;               /* erased cells already read 0xFF */
  cmd(CMD_WREN);
  ce_low();
  xfer(CMD_BYTE_PROG);
  send_addr(addr);
  xfer(v);
  ce_high();
  wait_busy();
}

/* AAI word programming (datasheet 4.4.4): WREN, then AD + address + 2 bytes,
 * then AD + 2 bytes per word; RDSR busy-polling between words (software
 * end-of-write mode); WRDI ends it. Address even; range must be erased. About
 * twice as fast as byte programming: one ~10 us program wait per 2 bytes and
 * no per-byte WREN/address. */
static void prog_aai(uint32_t addr, const uint8_t *p, uint32_t words)
{
  cmd(CMD_WREN);
  ce_low();
  xfer(CMD_AAI_WORD);
  send_addr(addr);
  xfer(p[0]); xfer(p[1]);
  ce_high();
  wait_busy();
  for(uint32_t i = 1; i < words; i++)
  {
    ce_low();
    xfer(CMD_AAI_WORD);
    xfer(p[2 * i]); xfer(p[2 * i + 1]);
    ce_high();
    wait_busy();
  }
  cmd(CMD_WRDI);                      /* leave AAI mode */
  wait_busy();
}

/* Program len bytes onto erased flash. Runs of 16-bit words are written with
 * AAI; words that are entirely FFFF are skipped (already erased); an odd first
 * or last byte uses byte programming. */
void spiflash_program(uint32_t addr, const void *buf, uint32_t len)
{
  const uint8_t *p = (const uint8_t *)buf;
  wait_busy();
  if(len && (addr & 1u)) { prog_byte(addr, *p); addr++; p++; len--; }
  while(len >= 2)
  {
    if(p[0] == 0xFF && p[1] == 0xFF) { addr += 2; p += 2; len -= 2; continue; }
    uint32_t n = 0;                   /* run of words that need programming */
    while(2 * (n + 1) <= len && !(p[2 * n] == 0xFF && p[2 * n + 1] == 0xFF)) n++;
    prog_aai(addr, p, n);
    addr += 2 * n; p += 2 * n; len -= 2 * n;
  }
  if(len) prog_byte(addr, *p);
}

/* ---- self-test (scratch = last sector) ---- */
bool spiflash_selftest(void)
{
  if(spiflash_read_jedec_id() != SPIFLASH_JEDEC_ID) return false;

  const uint32_t scratch = SPIFLASH_SIZE - SPIFLASH_SECTOR_SIZE;
  uint8_t wr[16], rd[16];
  for(int i = 0; i < 16; i++) wr[i] = (uint8_t)(0xA5 ^ i);

  spiflash_erase_sector(scratch);
  spiflash_read(scratch, rd, sizeof rd);
  for(int i = 0; i < 16; i++) if(rd[i] != 0xFF) return false;       /* erase failed */

  spiflash_program(scratch, wr, sizeof wr);
  spiflash_read(scratch, rd, sizeof rd);
  return memcmp(rd, wr, sizeof rd) == 0;                            /* program / read-back */
}
