/*
 * Copyright (c) 2023 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app_work, LOG_LEVEL_DBG);

#include <stdlib.h>
#include <net/golioth/system_client.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>

#include "app_work.h"
#include "app_state.h"
#include "app_settings.h"

#ifdef CONFIG_LIB_OSTENTUS
#include <libostentus.h>
#endif

#ifdef CONFIG_ALUDEL_BATTERY_MONITOR
#include "battery_monitor/battery.h"
#endif

#define ADC_RAW_TO_AMP (0.003529412f)
#define SPI_OP	SPI_OP_MODE_MASTER | SPI_MODE_CPOL | SPI_MODE_CPHA | SPI_WORD_SET(8) | SPI_LINES_SINGLE

static struct golioth_client *client;

struct k_sem adc_data_sem;

/* Formatting string for sending sensor JSON to Golioth */
#define JSON_FMT "{\"ch0\":%d,\"ch1\":%d}"
#define ADC_STREAM_ENDP	"sensor"
#define ADC_CUMULATIVE_ENDP	"state/cumulative"

#define ADC_CH0 0
#define ADC_CH1 1

adc_node_t adc_ch0 = {
	.spi = SPI_DT_SPEC_GET(DT_NODELABEL(mcp3201_ch0), SPI_OP, 0),
	.ch_num = ADC_CH0,
	.laston = -1,
	.runtime = 0,
	.total_unreported = 0,
	.total_cloud = 0,
	.loaded_from_cloud = false
};

adc_node_t adc_ch1 = {
	.spi = SPI_DT_SPEC_GET(DT_NODELABEL(mcp3201_ch1), SPI_OP, 0),
	.ch_num = ADC_CH1,
	.laston = -1,
	.runtime = 0,
	.total_unreported = 0,
	.total_cloud = 0,
	.loaded_from_cloud = false
};

/* Store two values for each ADC reading */
struct mcp3201_data {
	uint16_t val1;
	uint16_t val2;
};

void get_ontime(struct ontime *ot)
{
	ot->ch0 = adc_ch0.runtime;
	ot->ch1 = adc_ch1.runtime;
}

/* Callback for LightDB Stream */
static int async_error_handler(struct golioth_req_rsp *rsp)
{
	if (rsp->err) {
		LOG_ERR("Async task failed: %d", rsp->err);
		return rsp->err;
	}

	return 0;
}

/*
 * Validate data received from MCP3201
 */
static int process_adc_reading(uint8_t buf_data[4], struct mcp3201_data *adc_data)
{
	if (buf_data[0] & 1<<5) {	/* Missing NULL bit */
		return -ENOTSUP;
	}

	uint16_t data_msb = 0;
	uint16_t data_lsb = 0;

	data_msb = buf_data[0] & 0x1F;
	data_msb |= (data_msb << 7) | (buf_data[1] >> 1);

	for (uint8_t i = 0; i < 12; i++) {
		bool bit_set = false;

		if (i < 2) {
			if (buf_data[1] & (1 << (1 - i))) {
				bit_set = true;
			}
		} else if (i < 10) {
			if (buf_data[2] & (1 << (2 + 7 - i))) {
				bit_set = true;
			}
		} else {
			if (buf_data[3] & (1 << (10 + 7 - i))) {
				bit_set = true;
			}
		}
		if (bit_set) {
			data_lsb |= (1 << i);
		}
	}

	adc_data->val1 = data_msb;
	adc_data->val2 = data_lsb;

	return 0;
}

static int get_adc_reading(adc_node_t *adc, struct mcp3201_data *adc_data)
{
	int err;
	static uint8_t my_buffer[4] = {0};
	struct spi_buf my_spi_buffer[1];

	my_spi_buffer[0].buf = my_buffer;
	my_spi_buffer[0].len = 4;
	const struct spi_buf_set rx_buff = { my_spi_buffer, 1 };

	err = spi_read_dt(&(adc->spi), &rx_buff);
	if (err) {
		LOG_INF("spi_read status: %d", err);
		return err;
	}
	LOG_DBG("Received 4 bytes: %d %d %d %d", my_buffer[0], my_buffer[1], my_buffer[2], my_buffer[3]);

	err = process_adc_reading(my_buffer, adc_data);
	if (err) {
		LOG_ERR("Failed to process ADC readings %d", err);
		return err;
	}

	LOG_INF("mcp3201_ch%d received two ADC readings: 0x%04x\t0x%04x",
		adc->ch_num, adc_data->val1, adc_data->val2);

	return 0;
}

static int push_adc_to_golioth(uint16_t ch0_data, uint16_t ch1_data)
{
	int err;
	char json_buf[128];

	snprintk(json_buf, sizeof(json_buf), JSON_FMT, ch0_data, ch1_data);

	err = golioth_stream_push_cb(client, ADC_STREAM_ENDP, GOLIOTH_CONTENT_FORMAT_APP_JSON,
				     json_buf, strlen(json_buf), async_error_handler, NULL);
	if (err) {
		LOG_ERR("Failed to send sensor data to Golioth: %d", err);
		return err;
	}

	app_state_report_ontime(&adc_ch0, &adc_ch1);

	return 0;
}

static int update_ontime(uint16_t adc_value, adc_node_t *ch)
{
	if (k_sem_take(&adc_data_sem, K_MSEC(300)) == 0) {
		if (adc_value <= get_adc_floor(ch->ch_num)) {
			ch->runtime = 0;
			ch->laston = -1;
		} else {
			int64_t ts = k_uptime_get();
			int64_t duration;

			if (ch->laston > 0) {
				duration = ts - ch->laston;
			} else {
				duration = 1;
			}
			ch->runtime += duration;
			ch->laston = ts;
			ch->total_unreported += duration;
		}
		k_sem_give(&adc_data_sem);

		return 0;
	}

	return -EACCES;
}

int reset_cumulative_totals(void)
{
	if (k_sem_take(&adc_data_sem, K_MSEC(5000)) == 0) {
		k_sem_give(&adc_data_sem);
		adc_ch0.total_cloud = 0;
		adc_ch1.total_cloud = 0;
		adc_ch0.total_unreported = 0;
		adc_ch1.total_unreported = 0;
		k_sem_give(&adc_data_sem);

		return 0;
	}

	LOG_ERR("Could not reset cumulative values; blocked by semaphore.");

	return -EACCES;
}

static int get_cumulative_handler(struct golioth_req_rsp *rsp)
{
	if (rsp->err) {
		LOG_ERR("Failed to receive cumulative value: %d", rsp->err);
		return rsp->err;
	}

	uint64_t decoded_ch0 = 0;
	uint64_t decoded_ch1 = 0;
	bool found_ch0 = false;
	bool found_ch1 = false;

	struct zcbor_string key;
	uint64_t data;
	bool ok;

	ZCBOR_STATE_D(decoding_state, 1, rsp->data, rsp->len, 1);
	ok = zcbor_map_start_decode(decoding_state);
	if (!ok) {
		goto cumulative_decode_error;
	}

	while (decoding_state->elem_count > 1) {
		ok = zcbor_tstr_decode(decoding_state, &key) &&
		     zcbor_uint64_decode(decoding_state, &data);
		if (!ok) {
			goto cumulative_decode_error;
		}

		if (strncmp(key.value, "ch0", 3) == 0) {
			found_ch0 = true;
			decoded_ch0 = data;
		} else if (strncmp(key.value, "ch1", 3) == 0) {
			found_ch1 = true;
			decoded_ch1 = data;
		} else {
			continue;
		}
	}

	if ((found_ch0 && found_ch1) == false) {
		goto cumulative_decode_error;
	} else {
		LOG_DBG("Decoded: ch0: %lld, ch1: %lld", decoded_ch0, decoded_ch1);
		if (k_sem_take(&adc_data_sem, K_MSEC(300)) == 0) {
			adc_ch0.total_cloud = decoded_ch0;
			adc_ch1.total_cloud = decoded_ch1;
			adc_ch0.loaded_from_cloud = true;
			adc_ch1.loaded_from_cloud = true;
			k_sem_give(&adc_data_sem);
		}
		return 0;
	}

cumulative_decode_error:
	LOG_ERR("ZCBOR Decoding Error");
	LOG_HEXDUMP_ERR(rsp->data, rsp->len, "cbor_payload");

	return -EBADMSG;
}

void app_work_on_connect(void)
{
	/* Get cumulative "on" time from Golioth LightDB State */
	int err;

	err = golioth_lightdb_get_cb(client, ADC_CUMULATIVE_ENDP, GOLIOTH_CONTENT_FORMAT_APP_CBOR,
				     get_cumulative_handler, NULL);
	if (err) {
		LOG_WRN("failed to get cumulative channel data from LightDB: %d", err);
	}
}

void app_work_init(struct golioth_client *work_client)
{
	client = work_client;
	k_sem_init(&adc_data_sem, 0, 1);


	LOG_DBG("Setting up current clamp ADCs...");
	LOG_DBG("mcp3201_ch0.bus = %p", adc_ch0.spi.bus);
	LOG_DBG("mcp3201_ch0.config.cs->gpio.port = %p", adc_ch0.spi.config.cs->gpio.port);
	LOG_DBG("mcp3201_ch0.config.cs->gpio.pin = %u", adc_ch0.spi.config.cs->gpio.pin);
	LOG_DBG("mcp3201_ch1.bus = %p", adc_ch1.spi.bus);
	LOG_DBG("mcp3201_ch1.config.cs->gpio.port = %p", adc_ch1.spi.config.cs->gpio.port);
	LOG_DBG("mcp3201_ch1.config.cs->gpio.pin = %u", adc_ch1.spi.config.cs->gpio.pin);

	/* Semaphores to handle data access */
	k_sem_give(&adc_data_sem);
}

/* this will be called by the main() loop */
/* do all of your work here! */
void app_work_sensor_read(void)
{
	int err = 0;
	struct mcp3201_data ch0_data, ch1_data;

	IF_ENABLED(CONFIG_ALUDEL_BATTERY_MONITOR, (
		read_and_report_battery();
		slide_set(BATTERY_V, get_batt_v_str(), strlen(get_batt_v_str()));
		slide_set(BATTERY_LVL, get_batt_lvl_str(), strlen(get_batt_lvl_str()));
	));


	get_adc_reading(&adc_ch0, &ch0_data);
	get_adc_reading(&adc_ch1, &ch1_data);

	/* Calculate the "On" time if readings are not zero */
	err = update_ontime(ch0_data.val1, &adc_ch0);
	err &= update_ontime(ch1_data.val1, &adc_ch1);

	if (err) {
		LOG_ERR("Failed to update ontime: %d", err);
	} else {
		LOG_DBG("Ontime:\t(ch0): %lld\t(ch1): %lld", adc_ch0.runtime, adc_ch1.runtime);
	}

	/* Send sensor data to Golioth */
	/* Two values were read for each sensor but we'll record only one from each
	 * channel as it's unlikely the two readings will be substantially
	 * different.
	 */
	push_adc_to_golioth(ch0_data.val1, ch1_data.val1);

	IF_ENABLED(CONFIG_LIB_OSTENTUS, (
		/* Update slide values on Ostentus
		 * - values should be sent as strings
		 * - use the enum from app_work.h for slide key values
		 */
		char json_buf[128];

		snprintk(json_buf, sizeof(json_buf), "%.2f A", (ch0_data.val1 * ADC_RAW_TO_AMP));
		slide_set(CH0_CURRENT, json_buf, strlen(json_buf));

		snprintk(json_buf, sizeof(json_buf), "%.2f A", (ch1_data.val1 * ADC_RAW_TO_AMP));
		slide_set(CH1_CURRENT, json_buf, strlen(json_buf));

		snprintk(json_buf, sizeof(json_buf), "%d RAW", ch0_data.val1);
		slide_set(CH0_CURRENT_RAW, json_buf, strlen(json_buf));

		snprintk(json_buf, sizeof(json_buf), "%d RAW", ch1_data.val1);
		slide_set(CH1_CURRENT_RAW, json_buf, strlen(json_buf));

		snprintk(json_buf, sizeof(json_buf), "%.2f sec", (adc_ch0.runtime / 1000.0));
		slide_set(CH0_ONTIME, json_buf, strlen(json_buf));

		snprintk(json_buf, sizeof(json_buf), "%.2f sec", (adc_ch1.runtime / 1000.0));
		slide_set(CH1_ONTIME, json_buf, strlen(json_buf));

	));
}
