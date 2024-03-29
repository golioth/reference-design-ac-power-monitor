/*
 * Copyright (c) 2023 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __APP_SETTINGS_H__
#define __APP_SETTINGS_H__

/**
 * Process changes received from the Golioth Settings Service and return a code
 * to Golioth to indicate the success or failure of the update.
 *
 * https://docs.golioth.io/firmware/zephyr-device-sdk/device-settings-service
 */

#include <stdint.h>
#include <net/golioth/system_client.h>

int app_settings_init(struct golioth_client *state_client);
int app_settings_observe(void);
int app_settings_register(struct golioth_client *settings_client);
uint16_t get_adc_floor(uint8_t ch_num);
int32_t get_loop_delay_s(void);

#endif /* __APP_SETTINGS_H__ */
