/*
 * Copyright (c) 2023 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __APP_WORK_H__
#define __APP_WORK_H__

/** The `app_work.c` file performs the important work of this application which
 * is to read sensor values and report them to the Golioth LightDB Stream as
 * time-series data.
 *
 * For this demonstration, a `counter` value is periodically logged and pushed
 * to the Golioth time-series database. This simulated sensor reading occurs
 * when the loop in `main.c` calls `app_work_sensor_read()`. The frequency of
 * this loop is determined by values received from the Golioth Settings Service
 * (see app_settings.h).
 *
 * https://docs.golioth.io/firmware/zephyr-device-sdk/light-db-stream/
 */

#include <stdint.h>
#include <zephyr/drivers/spi.h>
#include <net/golioth/system_client.h>

extern struct k_sem adc_data_sem;

struct ontime {
	uint64_t ch0;
	uint64_t ch1;
};

typedef struct {
	const struct spi_dt_spec spi;
	uint8_t ch_num;
	int64_t laston;
	uint64_t runtime;
	uint64_t total_unreported;
	uint64_t total_cloud;
	bool loaded_from_cloud;

} adc_node_t;

/* Ostentus slide labels */
#define CH0_CUR_LABEL "Current ch0"
#define CH1_CUR_LABEL "Current ch1"
#define CH0_ONTIME_LBL   "Ontime ch0"
#define CH1_ONTIME_LBL   "Ontime ch1"
#define LABEL_BATTERY	 "Battery"
#define LABEL_FIRMWARE	 "Firmware"
#define SUMMARY_TITLE	 "Channel 0:"

/**
 * Each Ostentus slide needs a unique key. You may add additional slides by
 * inserting elements with the name of your choice to this enum.
 */
typedef enum {
	CH0_CURRENT,
	CH1_CURRENT,
	CH0_CURRENT_RAW,
	CH1_CURRENT_RAW,
	CH0_ONTIME,
	CH1_ONTIME,
#ifdef CONFIG_ALUDEL_BATTERY_MONITOR
	BATTERY_V,
	BATTERY_LVL,
#endif
	FIRMWARE
} slide_key;

void app_work_init(struct golioth_client *work_client);
void app_work_on_connect(void);
void app_work_sensor_read(void);
void get_ontime(struct ontime *ot);
int reset_cumulative_totals(void);

#endif /* __APP_WORK_H__ */
