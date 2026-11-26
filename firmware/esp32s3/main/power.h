/**
 * @file power.h
 * @brief Power management: PWM backlight dimming + battery voltage gauge
 */
#ifndef POWER_H
#define POWER_H

#ifdef __cplusplus
extern "C" {
#endif

/** Init backlight PWM (default brightness) + battery ADC. Call after UI init. */
void power_init(void);

/** Call periodically from the UI loop: samples battery every few seconds. */
void power_process(void);

/** Set backlight brightness 0..100 */
void power_set_backlight(int percent);

/** Latest battery voltage in mV (0 if not sampled yet) */
int power_get_battery_mv(void);

#ifdef __cplusplus
}
#endif

#endif /* POWER_H */
