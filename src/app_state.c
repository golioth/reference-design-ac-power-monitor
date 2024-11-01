/*
 * Copyright (c) 2023 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app_state, LOG_LEVEL_DBG);

#include <golioth/client.h>
#include <golioth/lightdb_state.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>

#include "main.h"
#include "app_sensors.h"
#include "app_state.h"

#define LIVE_RUNTIME_FMT "{\"live_runtime\":{\"ch0\":%lld,\"ch1\":%lld}"
#define CUMULATIVE_RUNTIME_FMT ",\"cumulative\":{\"ch0\":%lld,\"ch1\":%lld}}"
#define DEVICE_STATE_FMT LIVE_RUNTIME_FMT "}"
#define DEVICE_STATE_FMT_CUMULATIVE LIVE_RUNTIME_FMT CUMULATIVE_RUNTIME_FMT

static struct golioth_client *client;
static struct ontime ot;

static K_SEM_DEFINE(update_actual, 0, 1);

static void async_handler(struct golioth_client *client,
				       const struct golioth_response *response,
				       const char *path,
				       void *arg)
{
	if (response->status != GOLIOTH_OK) {
		LOG_WRN("Failed to set state: %d", response->status);
		return;
	}

	LOG_DBG("State successfully set");
}

static int app_state_update_actual(void)
{
	int err;
	char sbuf[sizeof(DEVICE_STATE_FMT) + 10]; /* space for uint16 values */

	err = get_ontime(&ot);

	if (err) {
		LOG_ERR("Failed to retrieve ontime: %d", err);
		return err;
	}

	snprintk(sbuf, sizeof(sbuf), DEVICE_STATE_FMT, ot.ch0, ot.ch1);
	err = golioth_lightdb_set_async(client,
					APP_STATE_ACTUAL_ENDP,
					GOLIOTH_CONTENT_TYPE_JSON,
					sbuf,
					strlen(sbuf),
					async_handler,
					NULL);
	if (err) {
		LOG_ERR("Unable to write to LightDB State: %d", err);
	}

	return err;
}

int app_state_report_ontime(adc_node_t *ch0, adc_node_t *ch1)
{
	int err;
	char json_buf[128];

	if (k_sem_take(&adc_data_sem, K_MSEC(300)) == 0) {

		if (ch0->loaded_from_cloud) {
			snprintk(json_buf, sizeof(json_buf), DEVICE_STATE_FMT_CUMULATIVE,
				 ch0->runtime, ch1->runtime,
				 (ch0->total_cloud + ch0->total_unreported),
				 (ch1->total_cloud + ch1->total_unreported));
		} else {
			snprintk(json_buf, sizeof(json_buf), DEVICE_STATE_FMT, ch0->runtime, ch1->runtime);
			/* Cumulative not yet loaded from LightDB State */
			/* Try to load it now */
			app_work_on_connect();
		}


		err = golioth_lightdb_set_async(client,
						APP_STATE_ACTUAL_ENDP,
						GOLIOTH_CONTENT_TYPE_JSON,
						json_buf,
						strlen(json_buf),
						async_handler,
						NULL);
		if (err) {
			LOG_ERR("Failed to send sensor data to Golioth: %d", err);
			k_sem_give(&adc_data_sem);

			return err;
		}

		if (ch0->loaded_from_cloud) {
			ch0->total_cloud += ch0->total_unreported;
			ch0->total_unreported = 0;
			ch1->total_cloud += ch1->total_unreported;
			ch1->total_unreported = 0;
		}

		k_sem_give(&adc_data_sem);
	}

	return 0;
}

int app_state_observe(struct golioth_client *state_client)
{
	int err;

	client = state_client;

	/* This will only run once. It updates the actual state of the device
	 * with the Golioth servers. Future updates will be sent whenever
	 * changes occur.
	 */
	err = app_state_update_actual();

	return err;
}
