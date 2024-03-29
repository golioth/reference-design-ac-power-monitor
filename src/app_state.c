/*
 * Copyright (c) 2023 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app_state, LOG_LEVEL_DBG);

#include <net/golioth/system_client.h>

#include "app_state.h"
#include "app_work.h"

#define LIVE_RUNTIME_FMT "{\"live_runtime\":{\"ch0\":%lld,\"ch1\":%lld}"
#define CUMULATIVE_RUNTIME_FMT ",\"cumulative\":{\"ch0\":%lld,\"ch1\":%lld}}"
#define DEVICE_STATE_FMT LIVE_RUNTIME_FMT "}"
#define DEVICE_STATE_FMT_CUMULATIVE LIVE_RUNTIME_FMT CUMULATIVE_RUNTIME_FMT
#define DESIRED_RESET_KEY "reset_cumulative"

static struct golioth_client *client;
static struct ontime ot;

static K_SEM_DEFINE(update_actual, 0, 1);

static int async_handler(struct golioth_req_rsp *rsp)
{
	if (rsp->err) {
		LOG_WRN("Failed to set state: %d", rsp->err);
		return rsp->err;
	}

	LOG_DBG("State successfully set");

	return 0;
}

static int app_state_reset_desired(void)
{
	int err;
	uint8_t cbor_payload[32];
	bool ok;

	LOG_INF("Resetting \"%s\" LightDB State endpoint to defaults.", APP_STATE_DESIRED_ENDP);

	ZCBOR_STATE_E(encoding_state, 16, cbor_payload, sizeof(cbor_payload), 0);
	ok = zcbor_map_start_encode(encoding_state, 2) &&
	     zcbor_tstr_put_lit(encoding_state, DESIRED_RESET_KEY) &&
	     zcbor_bool_put(encoding_state, false) &&
	     zcbor_map_end_encode(encoding_state, 2);

	if (!ok) {
		LOG_ERR("Error encoding CBOR to reset desired endpoint");
		return -ENODATA;
	}

	LOG_HEXDUMP_DBG(cbor_payload, encoding_state->payload - cbor_payload, "cbor_payload");

	err = golioth_lightdb_set_cb(client, APP_STATE_DESIRED_ENDP, GOLIOTH_CONTENT_FORMAT_APP_CBOR,
				     cbor_payload, (encoding_state->payload - cbor_payload),
				     async_handler, NULL);
	if (err) {
		LOG_ERR("Unable to write to LightDB State: %d", err);
		return err;
	}

	return 0;
}

void app_state_init(struct golioth_client *state_client)
{
	client = state_client;
	k_sem_give(&update_actual);
}

int app_state_update_actual(void)
{
	int err;
	char sbuf[sizeof(DEVICE_STATE_FMT) + 10]; /* space for uint16 values */

	err = get_ontime(&ot);

	if (err) {
		LOG_ERR("Failed to retrieve ontime: %d", err);
		return err;
	}

	snprintk(sbuf, sizeof(sbuf), DEVICE_STATE_FMT, ot.ch0, ot.ch1);
	err = golioth_lightdb_set_cb(client, APP_STATE_ACTUAL_ENDP, GOLIOTH_CONTENT_FORMAT_APP_JSON,
				     sbuf, strlen(sbuf), async_handler, NULL);
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

		err = golioth_lightdb_set_cb(client, APP_STATE_ACTUAL_ENDP, GOLIOTH_CONTENT_FORMAT_APP_JSON,
					     json_buf, strlen(json_buf), async_handler, NULL);

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

int app_state_desired_handler(struct golioth_req_rsp *rsp)
{
	if (rsp->err) {
		LOG_ERR("Failed to receive '%s' endpoint: %d", APP_STATE_DESIRED_ENDP, rsp->err);
		return rsp->err;
	}

	LOG_HEXDUMP_DBG(rsp->data, rsp->len, APP_STATE_DESIRED_ENDP);

	if ((rsp->len == 1) && (rsp->data[0] == 0xf6)) {
		/* This is `null` in CBOR */
		LOG_ERR("Endpoint is null, resetting desired to defaults");
		app_state_reset_desired();
		return -EFAULT;
	}

	struct zcbor_string key;
	bool reset_cumulative;
	bool ok;

	ZCBOR_STATE_D(decoding_state, 1, rsp->data, rsp->len, 1);
	ok = zcbor_map_start_decode(decoding_state) &&
	     zcbor_tstr_decode(decoding_state, &key) &&
	     zcbor_bool_decode(decoding_state, &reset_cumulative) &&
	     zcbor_map_end_decode(decoding_state);

	if (!ok) {
		LOG_ERR("ZCBOR Decoding Error");
		LOG_HEXDUMP_ERR(rsp->data, rsp->len, "cbor_payload");
		app_state_reset_desired();
		return -ENOTSUP;
	}

	if (strncmp(key.value, DESIRED_RESET_KEY, strlen(DESIRED_RESET_KEY)) != 0) {
		LOG_ERR("Unexpected key received: %.*s", key.len, key.value);
		app_state_reset_desired();
		return -ENODATA;
	}

	LOG_DBG("Decoded: %.*s == %s", key.len, key.value, reset_cumulative ? "true" : "false");
	if (reset_cumulative) {
		LOG_INF("Request to reset cumulative values received. Processing now.");
		reset_cumulative_totals();
		app_state_reset_desired();
	}

	return 0;
}

int app_state_observe(void)
{
	int err = golioth_lightdb_observe_cb(client, APP_STATE_DESIRED_ENDP, GOLIOTH_CONTENT_FORMAT_APP_CBOR,
					     app_state_desired_handler, NULL);
	if (err) {
		LOG_WRN("failed to observe lightdb path: %d", err);
	}

	/* This will only run once. It updates the actual state of the device
	 * with the Golioth servers. Future updates will be sent whenever
	 * changes occur.
	 */
	if (k_sem_take(&update_actual, K_NO_WAIT) == 0) {
		err = app_state_update_actual();
	}

	return err;
}
