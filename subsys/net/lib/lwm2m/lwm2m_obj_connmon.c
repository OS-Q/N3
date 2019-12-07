/*
 * Copyright (c) 2019 Foundries.io
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME net_lwm2m_obj_conn_mon
#define LOG_LEVEL CONFIG_LWM2M_LOG_LEVEL

#include <logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#include <string.h>
#include <init.h>
#include <net/net_if.h>
#include <net/net_ip.h>

#include "lwm2m_object.h"
#include "lwm2m_engine.h"

/* Connectivity Monitoring resource IDs */
#define CONNMON_NETWORK_BEARER_ID		0
#define CONNMON_AVAIL_NETWORK_BEARER_ID		1
#define CONNMON_RADIO_SIGNAL_STRENGTH		2
#define CONNMON_LINK_QUALITY			3
#define CONNMON_IP_ADDRESSES			4
#define CONNMON_ROUTER_IP_ADDRESSES		5
#define CONNMON_LINK_UTILIZATION		6
#define CONNMON_APN				7
#define CONNMON_CELLID				8
#define CONNMON_SMNC				9
#define CONNMON_SMCC				10

#define CONNMON_MAX_ID				11

#define CONNMON_STRING_SHORT			8

#define CONNMON_AVAIL_BEARER_MAX	CONFIG_LWM2M_CONN_MON_BEARER_MAX
#define CONNMON_APN_MAX			CONFIG_LWM2M_CONN_MON_APN_MAX

#if defined(CONFIG_NET_IF_MAX_IPV6_COUNT) \
	&& defined(CONFIG_NET_IF_MAX_IPV4_COUNT)
#define CONNMON_IP_ADDRESS_MAX		(CONFIG_NET_IF_MAX_IPV6_COUNT + \
					CONFIG_NET_IF_MAX_IPV4_COUNT)
#elif defined(CONFIG_NET_IF_MAX_IPV6_COUNT)
#define CONNMON_IP_ADDRESS_MAX		CONFIG_NET_IF_MAX_IPV6_COUNT
#elif defined(CONFIG_NET_IF_MAX_IPV4_COUNT)
#define CONNMON_IP_ADDRESS_MAX		CONFIG_NET_IF_MAX_IPV4_COUNT
#else
#define CONNMON_IP_ADDRESS_MAX		1
#endif

#if defined(CONFIG_NET_MAX_ROUTERS)
#define CONNMON_ROUTER_IP_ADDRESS_MAX	CONFIG_NET_MAX_ROUTERS
#else
#define CONNMON_ROUTER_IP_ADDRESS_MAX	1
#endif

/*
 * Calculate resource instances as follows:
 * start with CONNMON_MAX_ID
 * subtract MULTI resources because their counts include 0 resource (4)
 * add CONNMON_AVAIL_BEARER_MAX resource instances
 * add CONNMON_APN_MAX resource instances
 * add CONNMON_IP_ADDRESS_MAX resource instances
 * add CONNMON_ROUTER_IP_ADDRESS_MAX resource instances
 */
#define RESOURCE_INSTANCE_COUNT        (CONNMON_MAX_ID - 4 + \
					CONNMON_AVAIL_BEARER_MAX + \
					CONNMON_APN_MAX + \
					CONNMON_IP_ADDRESS_MAX + \
					CONNMON_ROUTER_IP_ADDRESS_MAX)

/* resource state variables */
static s8_t net_bearer;
static s8_t rss;
static u8_t link_quality;
static u16_t mnc;
static u16_t mcc;

/* only 1 instance of Connection Monitoring object exists */
static struct lwm2m_engine_obj connmon;
static struct lwm2m_engine_obj_field fields[] = {
	OBJ_FIELD_DATA(CONNMON_NETWORK_BEARER_ID, R, U8),
	OBJ_FIELD_DATA(CONNMON_AVAIL_NETWORK_BEARER_ID, R, U8),
	OBJ_FIELD_DATA(CONNMON_RADIO_SIGNAL_STRENGTH, R, S8),
	OBJ_FIELD_DATA(CONNMON_LINK_QUALITY, R, U8),
	OBJ_FIELD_DATA(CONNMON_IP_ADDRESSES, R, STRING),
	OBJ_FIELD_DATA(CONNMON_ROUTER_IP_ADDRESSES, R_OPT, STRING),
	OBJ_FIELD_DATA(CONNMON_LINK_UTILIZATION, R_OPT, U8),
	OBJ_FIELD_DATA(CONNMON_APN, R_OPT, STRING),
	OBJ_FIELD_DATA(CONNMON_CELLID, R_OPT, S32),
	OBJ_FIELD_DATA(CONNMON_SMNC, R_OPT, U16),
	OBJ_FIELD_DATA(CONNMON_SMCC, R_OPT, U16)
};

static struct lwm2m_engine_obj_inst inst;
static struct lwm2m_engine_res res[CONNMON_MAX_ID];
static struct lwm2m_engine_res_inst res_inst[RESOURCE_INSTANCE_COUNT];

static struct lwm2m_engine_obj_inst *connmon_create(u16_t obj_inst_id)
{
	int i = 0, j = 0;

	/* Set default values */
	net_bearer = 42U; /* Ethernet */
	rss = 0;
	link_quality = 0U;
	mnc = 0U;
	mcc = 0U;

	init_res_instance(res_inst, ARRAY_SIZE(res_inst));

	/* initialize instance resource data */
	INIT_OBJ_RES_DATA(CONNMON_NETWORK_BEARER_ID, res, i, res_inst, j,
			  &net_bearer, sizeof(net_bearer));
	INIT_OBJ_RES_MULTI_OPTDATA(CONNMON_AVAIL_NETWORK_BEARER_ID, res, i,
				   res_inst, j, CONNMON_AVAIL_BEARER_MAX,
				   false);
	INIT_OBJ_RES_DATA(CONNMON_RADIO_SIGNAL_STRENGTH, res, i, res_inst, j,
			  &rss, sizeof(rss));
	INIT_OBJ_RES_DATA(CONNMON_LINK_QUALITY, res, i, res_inst, j,
			  &link_quality, sizeof(link_quality));
	INIT_OBJ_RES_MULTI_OPTDATA(CONNMON_IP_ADDRESSES, res, i,
				   res_inst, j, CONNMON_IP_ADDRESS_MAX, false);
	INIT_OBJ_RES_MULTI_OPTDATA(CONNMON_ROUTER_IP_ADDRESSES, res, i,
				   res_inst, j, CONNMON_ROUTER_IP_ADDRESS_MAX,
				   false);
	INIT_OBJ_RES_MULTI_OPTDATA(CONNMON_APN, res, i, res_inst, j,
				   CONNMON_APN_MAX, false);
	INIT_OBJ_RES_DATA(CONNMON_SMNC, res, i, res_inst, j, &mnc, sizeof(mnc));
	INIT_OBJ_RES_DATA(CONNMON_SMCC, res, i, res_inst, j, &mcc, sizeof(mcc));

	inst.resources = res;
	inst.resource_count = i;
	LOG_DBG("Create LWM2M connectivity monitoring instance: %d",
		obj_inst_id);
	return &inst;
}

static int lwm2m_connmon_init(struct device *dev)
{
	struct lwm2m_engine_obj_inst *obj_inst = NULL;
	int ret = 0;

	/* initialize the Connection Monitoring field data */
	connmon.obj_id = LWM2M_OBJECT_CONNECTIVITY_MONITORING_ID;
	connmon.fields = fields;
	connmon.field_count = ARRAY_SIZE(fields);
	connmon.max_instance_count = 1U;
	connmon.create_cb = connmon_create;
	lwm2m_register_obj(&connmon);

	/* auto create the only instance */
	ret = lwm2m_create_obj_inst(LWM2M_OBJECT_CONNECTIVITY_MONITORING_ID,
				    0, &obj_inst);
	if (ret < 0) {
		LOG_DBG("Create LWM2M instance 0 error: %d", ret);
	}

	return ret;
}

SYS_INIT(lwm2m_connmon_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
