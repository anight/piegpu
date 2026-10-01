/*
 * IRQ-safe console output. See psdl_pico_log.c for why printf is not.
 *
 * psdl_log() may be called from any core and from an interrupt. The text is
 * queued; psdl_log_drain(), which must run on core 0 outside an interrupt, does
 * the printf. The input backend drains once per poll, so output is at most a
 * frame late.
 */
#ifndef PSDL_PICO_LOG_H
#define PSDL_PICO_LOG_H

void psdl_log_init(void);
void psdl_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void psdl_log_drain(void);

#endif /* PSDL_PICO_LOG_H */
