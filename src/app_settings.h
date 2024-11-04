/*
 * Copyright (c) 2023 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * Process changes received from the Golioth Settings Service and return a code
 * to Golioth to indicate the success or failure of the update.
 *
 * https://docs.golioth.io/firmware/zephyr-device-sdk/device-settings-service
 */

#ifndef __APP_SETTINGS_H__
#define __APP_SETTINGS_H__

#include <stdint.h>
#include <golioth/client.h>

uint16_t get_adc_floor(uint8_t ch_num);
int32_t get_loop_delay_s(void);
void app_settings_register(struct golioth_client *client);

#endif /* __APP_SETTINGS_H__ */
