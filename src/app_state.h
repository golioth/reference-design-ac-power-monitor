/*
 * Copyright (c) 2022 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** Observe and write to example endpoints for stateful data on the Golioth
 * LightDB State Service.
 *
 * https://docs.golioth.io/firmware/zephyr-device-sdk/light-db/
 */

#ifndef __APP_STATE_H__
#define __APP_STATE_H__

#include <golioth/client.h>
#include "app_sensors.h"

#define APP_STATE_DESIRED_ENDP "desired"
#define APP_STATE_ACTUAL_ENDP  "state"

int app_state_observe(struct golioth_client *state_client);
int app_state_report_ontime(adc_node_t *ch0, adc_node_t *ch1);

#endif /* __APP_STATE_H__ */
