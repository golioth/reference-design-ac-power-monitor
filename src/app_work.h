/*
 * Copyright (c) 2022 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __APP_WORK_H__
#define __APP_WORK_H__

struct ontime {
	uint64_t ch0;
	uint64_t ch1;
};

void get_ontime(struct ontime *ot);
void app_work_init(struct golioth_client* work_client);
void app_work_submit(void);

#endif /* __APP_WORK_H__ */
