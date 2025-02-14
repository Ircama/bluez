// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022  Intel Corporation. All rights reserved.
 *
 */

#define _GNU_SOURCE
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>

#include "lib/bluetooth.h"
#include "lib/uuid.h"

#include "src/shared/queue.h"
#include "src/shared/util.h"
#include "src/shared/timeout.h"
#include "src/shared/att.h"
#include "src/shared/gatt-db.h"
#include "src/shared/gatt-server.h"
#include "src/shared/gatt-client.h"
#include "src/shared/vcp.h"

#define DBG(_vcp, fmt, arg...) \
	vcp_debug(_vcp, "%s:%s() " fmt, __FILE__, __func__, ## arg)

#define VCP_STEP_SIZE 1

#define VOCS_VOL_OFFSET_UPPER_LIMIT	 255
#define VOCS_VOL_OFFSET_LOWER_LIMIT	-255

/* Apllication Error Code */
#define BT_ATT_ERROR_INVALID_CHANGE_COUNTER	0x80
#define BT_ATT_ERROR_OPCODE_NOT_SUPPORTED	0x81
#define BT_ATT_ERROR_VALUE_OUT_OF_RANGE		0x82

#define BT_VCP_NA                   BIT(0)
#define BT_VCP_FRONT_LEFT           BIT(1)
#define BT_VCP_FRONT_RIGHT          BIT(2)
#define BT_VCP_FRONT_CENTER         BIT(3)
#define BT_VCP_LOW_FRQ_EFF_1        BIT(4)
#define BT_VCP_BACK_LEFT            BIT(5)
#define BT_VCP_BACK_RIGHT           BIT(6)
#define BT_VCP_FRONT_LEFT_CENTER    BIT(7)
#define BT_VCP_FRONT_RIGHT_CENTER   BIT(8)
#define BT_VCP_BACK_CENTER          BIT(9)
#define BT_VCP_LOW_FRQ_EFF_2        BIT(10)
#define BT_VCP_SIDE_LEFT            BIT(11)
#define BT_VCP_SIDE_RIGHT           BIT(12)
#define BT_VCP_TOP_FRONT_LEFT       BIT(13)
#define BT_VCP_TOP_FRONT_RIGHT      BIT(14)
#define BT_VCP_TOP_FRONT_CENTER     BIT(15)
#define BT_VCP_TOP_CENTER           BIT(16)
#define BT_VCP_TOP_BACK_LEFT        BIT(17)
#define BT_VCP_TOP_BACK_RIGHT       BIT(18)
#define BT_VCP_TOP_SIDE_LEFT        BIT(19)
#define BT_VCP_TOP_SIDE_RIGHT       BIT(20)
#define BT_VCP_TOP_BACK_CENTER      BIT(21)
#define BT_VCP_BOTTOM_FRONT_CENTER  BIT(22)
#define BT_VCP_BOTTOM_FRONT_LEFT    BIT(23)
#define BT_VCP_BOTTOM_FRONT_RIGHT   BIT(24)
#define BT_VCP_FRONT_LEFT_WIDE      BIT(25)
#define BT_VCP_FRONT_RIGHT_WIDE     BIT(26)
#define BT_VCP_LEFT_SURROUND        BIT(27)
#define BT_VCP_RIGHT_SURROUND       BIT(28)

struct bt_vcp_db {
	struct gatt_db *db;
	struct bt_vcs *vcs;
	struct bt_vocs *vocs;
};

typedef void (*vcp_func_t)(struct bt_vcp *vcp, bool success, uint8_t att_ecode,
					const uint8_t *value, uint16_t length,
					void *user_data);

struct bt_vcp_pending {
	unsigned int id;
	struct bt_vcp *vcp;
	vcp_func_t func;
	void *user_data;
};

struct bt_vcs_param {
	uint8_t	op;
	uint8_t	change_counter;
} __packed;

struct bt_vocs_param {
	uint8_t	op;
	uint8_t	change_counter;
} __packed;

struct bt_vcs_ab_vol {
	uint8_t	change_counter;
	uint8_t	vol_set;
} __packed;

struct bt_vocs_set_vol_off {
	uint8_t	change_counter;
	int16_t set_vol_offset;
} __packed;

struct bt_vcp_cb {
	unsigned int id;
	bt_vcp_func_t attached;
	bt_vcp_func_t detached;
	void *user_data;
};

typedef void (*vcp_notify_t)(struct bt_vcp *vcp, uint16_t value_handle,
				const uint8_t *value, uint16_t length,
				void *user_data);

struct bt_vcp_notify {
	unsigned int id;
	struct bt_vcp *vcp;
	vcp_notify_t func;
	void *user_data;
};

struct bt_vcp {
	int ref_count;
	struct bt_vcp_db *ldb;
	struct bt_vcp_db *rdb;
	struct bt_gatt_client *client;
	struct bt_att *att;
	unsigned int vstate_id;
	unsigned int vflag_id;

	unsigned int state_id;
	unsigned int audio_loc_id;
	unsigned int ao_dec_id;

	struct queue *notify;
	struct queue *pending;

	bt_vcp_debug_func_t debug_func;
	bt_vcp_destroy_func_t debug_destroy;
	void *debug_data;
	void *user_data;
};

#define RESET_VOLUME_SETTING 0x00
#define USERSET_VOLUME_SETTING 0x01

/* Contains local bt_vcp_db */
struct vol_state {
	uint8_t	vol_set;
	uint8_t	mute;
	uint8_t counter;
} __packed;

struct bt_vcs {
	struct bt_vcp_db *vdb;
	struct vol_state *vstate;
	uint8_t vol_flag;
	struct gatt_db_attribute *service;
	struct gatt_db_attribute *vs;
	struct gatt_db_attribute *vs_ccc;
	struct gatt_db_attribute *vol_cp;
	struct gatt_db_attribute *vf;
	struct gatt_db_attribute *vf_ccc;
};

/* Contains local bt_vcp_db */
struct vol_offset_state {
	int16_t vol_offset;
	uint8_t counter;
} __packed;

struct bt_vocs {
	struct bt_vcp_db *vdb;
	struct vol_offset_state *vostate;
	uint32_t vocs_audio_loc;
	char *vocs_ao_dec;
	struct gatt_db_attribute *service;
	struct gatt_db_attribute *vos;
	struct gatt_db_attribute *vos_ccc;
	struct gatt_db_attribute *voal;
	struct gatt_db_attribute *voal_ccc;
	struct gatt_db_attribute *vo_cp;
	struct gatt_db_attribute *voaodec;
	struct gatt_db_attribute *voaodec_ccc;
};

static struct queue *vcp_db;
static struct queue *vcp_cbs;
static struct queue *sessions;

static void *iov_pull_mem(struct iovec *iov, size_t len)
{
	void *data = iov->iov_base;

	if (iov->iov_len < len)
		return NULL;

	iov->iov_base += len;
	iov->iov_len -= len;

	return data;
}

static struct bt_vcp_db *vcp_get_vdb(struct bt_vcp *vcp)
{
	if (!vcp)
		return NULL;

	if (vcp->ldb)
		return vcp->ldb;

	return NULL;
}

static struct vol_state *vdb_get_vstate(struct bt_vcp_db *vdb)
{
	if (!vdb->vcs)
		return NULL;

	if (vdb->vcs->vstate)
		return vdb->vcs->vstate;

	return NULL;
}

static struct vol_offset_state *vdb_get_vostate(struct bt_vcp_db *vdb)
{
	if (!vdb->vocs)
		return NULL;

	if (vdb->vocs->vostate)
		return vdb->vocs->vostate;

	return NULL;
}

static struct bt_vcs *vcp_get_vcs(struct bt_vcp *vcp)
{
	if (!vcp)
		return NULL;

	if (vcp->rdb->vcs)
		return vcp->rdb->vcs;

	vcp->rdb->vcs = new0(struct bt_vcs, 1);
	vcp->rdb->vcs->vdb = vcp->rdb;

	return vcp->rdb->vcs;
}

static struct bt_vocs *vcp_get_vocs(struct bt_vcp *vcp)
{
	if (!vcp)
		return NULL;

	if (vcp->rdb->vocs)
		return vcp->rdb->vocs;

	vcp->rdb->vocs = new0(struct bt_vocs, 1);
	vcp->rdb->vocs->vdb = vcp->rdb;

	return vcp->rdb->vocs;
}

static void vcp_detached(void *data, void *user_data)
{
	struct bt_vcp_cb *cb = data;
	struct bt_vcp *vcp = user_data;

	cb->detached(vcp, cb->user_data);
}

void bt_vcp_detach(struct bt_vcp *vcp)
{
	if (!queue_remove(sessions, vcp))
		return;

	bt_gatt_client_unref(vcp->client);
	vcp->client = NULL;

	queue_foreach(vcp_cbs, vcp_detached, vcp);
}

static void vcp_db_free(void *data)
{
	struct bt_vcp_db *vdb = data;

	if (!vdb)
		return;

	gatt_db_unref(vdb->db);

	free(vdb->vcs);
	free(vdb->vocs);
	free(vdb);
}

static void vcp_free(void *data)
{
	struct bt_vcp *vcp = data;

	bt_vcp_detach(vcp);

	vcp_db_free(vcp->rdb);

	queue_destroy(vcp->pending, NULL);

	free(vcp);
}
bool bt_vcp_set_user_data(struct bt_vcp *vcp, void *user_data)
{
	if (!vcp)
		return false;

	vcp->user_data = user_data;

	return true;
}

static bool vcp_db_match(const void *data, const void *match_data)
{
	const struct bt_vcp_db *vdb = data;
	const struct gatt_db *db = match_data;

	return (vdb->db == db);
}

struct bt_att *bt_vcp_get_att(struct bt_vcp *vcp)
{
	if (!vcp)
		return NULL;

	if (vcp->att)
		return vcp->att;

	return bt_gatt_client_get_att(vcp->client);
}

struct bt_vcp *bt_vcp_ref(struct bt_vcp *vcp)
{
	if (!vcp)
		return NULL;

	__sync_fetch_and_add(&vcp->ref_count, 1);

	return vcp;
}

void bt_vcp_unref(struct bt_vcp *vcp)
{
	if (!vcp)
		return;

	if (__sync_sub_and_fetch(&vcp->ref_count, 1))
		return;

	vcp_free(vcp);
}

static void vcp_debug(struct bt_vcp *vcp, const char *format, ...)
{
	va_list ap;

	if (!vcp || !format || !vcp->debug_func)
		return;

	va_start(ap, format);
	util_debug_va(vcp->debug_func, vcp->debug_data, format, ap);
	va_end(ap);
}

static void vcp_disconnected(int err, void *user_data)
{
	struct bt_vcp *vcp = user_data;

	DBG(vcp, "vcp %p disconnected err %d", vcp, err);

	bt_vcp_detach(vcp);
}

static struct bt_vcp *vcp_get_session(struct bt_att *att, struct gatt_db *db)
{
	const struct queue_entry *entry;
	struct bt_vcp *vcp;

	for (entry = queue_get_entries(sessions); entry; entry = entry->next) {
		struct bt_vcp *vcp = entry->data;

		if (att == bt_vcp_get_att(vcp))
			return vcp;
	}

	vcp = bt_vcp_new(db, NULL);
	vcp->att = att;

	bt_att_register_disconnect(att, vcp_disconnected, vcp, NULL);

	bt_vcp_attach(vcp, NULL);

	return vcp;

}

static uint8_t vcs_rel_vol_down(struct bt_vcs *vcs, struct bt_vcp *vcp,
				struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct vol_state *vstate;
	uint8_t	*change_counter;

	DBG(vcp, "Volume Down");

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		return 0;
	}

	vstate = vdb_get_vstate(vdb);
	if (!vstate) {
		DBG(vcp, "error: VSTATE not available");
		return 0;
	}

	change_counter = iov_pull_mem(iov, sizeof(*change_counter));
	if (!change_counter)
		return 0;

	if (*change_counter != vstate->counter) {
		DBG(vcp, "Change Counter Mismatch Volume not decremented!");
		return BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
	}

	vstate->vol_set = MAX((vstate->vol_set - VCP_STEP_SIZE), 0);
	vstate->counter = -~vstate->counter; /*Increment Change Counter*/

	gatt_db_attribute_notify(vdb->vcs->vs, (void *)vstate,
				 sizeof(struct vol_state),
				 bt_vcp_get_att(vcp));
	return 0;
}

static uint8_t vcs_rel_vol_up(struct bt_vcs *vcs, struct bt_vcp *vcp,
				struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct vol_state *vstate;
	uint8_t	*change_counter;

	DBG(vcp, "Volume Up");

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		return 0;
	}

	vstate = vdb_get_vstate(vdb);
	if (!vstate) {
		DBG(vcp, "error: VCP database not available");
		return 0;
	}

	change_counter = iov_pull_mem(iov, sizeof(*change_counter));
	if (!change_counter)
		return 0;

	if (*change_counter != vstate->counter) {
		DBG(vcp, "Change Counter Mismatch Volume not decremented!");
		return BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
	}

	vstate->vol_set = MIN((vstate->vol_set + VCP_STEP_SIZE), 255);
	vstate->counter = -~vstate->counter; /*Increment Change Counter*/

	gatt_db_attribute_notify(vdb->vcs->vs, (void *)vstate,
				 sizeof(struct vol_state),
				 bt_vcp_get_att(vcp));
	return 0;
}

static uint8_t vcs_unmute_rel_vol_down(struct bt_vcs *vcs, struct bt_vcp *vcp,
				struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct vol_state *vstate;
	uint8_t	*change_counter;

	DBG(vcp, "Un Mute and Volume Down");

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		return 0;
	}

	vstate = vdb_get_vstate(vdb);
	if (!vstate) {
		DBG(vcp, "error: VCP database not available");
		return 0;
	}

	change_counter = iov_pull_mem(iov, sizeof(*change_counter));
	if (!change_counter)
		return 0;

	if (*change_counter != vstate->counter) {
		DBG(vcp, "Change Counter Mismatch Volume not decremented!");
		return BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
	}

	vstate->mute = 0x00;
	vstate->vol_set = MAX((vstate->vol_set - VCP_STEP_SIZE), 0);
	vstate->counter = -~vstate->counter; /*Increment Change Counter*/

	gatt_db_attribute_notify(vdb->vcs->vs, (void *)vstate,
				 sizeof(struct vol_state),
				 bt_vcp_get_att(vcp));
	return 0;
}

static uint8_t vcs_unmute_rel_vol_up(struct bt_vcs *vcs, struct bt_vcp *vcp,
				struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct vol_state *vstate;
	uint8_t	*change_counter;

	DBG(vcp, "UN Mute and Volume Up");

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		return 0;
	}

	vstate = vdb_get_vstate(vdb);
	if (!vstate) {
		DBG(vcp, "error: VSTATE not available");
		return 0;
	}

	change_counter = iov_pull_mem(iov, sizeof(*change_counter));
	if (!change_counter)
		return 0;

	if (*change_counter != vstate->counter) {
		DBG(vcp, "Change Counter Mismatch Volume not decremented!");
		return BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
	}

	vstate->mute = 0x00;
	vstate->vol_set = MIN((vstate->vol_set + VCP_STEP_SIZE), 255);
	vstate->counter = -~vstate->counter; /*Increment Change Counter*/

	gatt_db_attribute_notify(vdb->vcs->vs, (void *)vstate,
				 sizeof(struct vol_state),
				 bt_vcp_get_att(vcp));
	return 0;
}

static uint8_t vcs_set_absolute_vol(struct bt_vcs *vcs, struct bt_vcp *vcp,
				struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct vol_state *vstate;
	struct bt_vcs_ab_vol *req;

	DBG(vcp, "Set Absolute Volume");

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		return 0;
	}

	vstate = vdb_get_vstate(vdb);
	if (!vstate) {
		DBG(vcp, "error: VSTATE not available");
		return 0;
	}

	req = iov_pull_mem(iov, sizeof(*req));
	if (!req)
		return 0;

	if (req->change_counter != vstate->counter) {
		DBG(vcp, "Change Counter Mismatch Volume not decremented!");
		return BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
	}

	vstate->vol_set = req->vol_set;
	vstate->counter = -~vstate->counter; /*Increment Change Counter*/

	gatt_db_attribute_notify(vdb->vcs->vs, (void *)vstate,
				 sizeof(struct vol_state),
				 bt_vcp_get_att(vcp));
	return 0;
}

static uint8_t vcs_unmute(struct bt_vcs *vcs, struct bt_vcp *vcp,
				struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct vol_state *vstate;
	uint8_t	*change_counter;

	DBG(vcp, "Un Mute");

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		return 0;
	}

	vstate = vdb_get_vstate(vdb);
	if (!vstate) {
		DBG(vcp, "error: VSTATE not available");
		return 0;
	}

	change_counter = iov_pull_mem(iov, sizeof(*change_counter));
	if (!change_counter)
		return 0;

	if (*change_counter != vstate->counter) {
		DBG(vcp, "Change Counter Mismatch Volume not decremented!");
		return BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
	}

	vstate->mute = 0x00;
	vstate->counter = -~vstate->counter; /*Increment Change Counter*/

	gatt_db_attribute_notify(vdb->vcs->vs, (void *)vstate,
				 sizeof(struct vol_state),
				 bt_vcp_get_att(vcp));
	return 0;
}

static uint8_t vcs_mute(struct bt_vcs *vcs, struct bt_vcp *vcp,
				struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct vol_state *vstate;
	uint8_t	*change_counter;

	DBG(vcp, "MUTE");

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		return 0;
	}

	vstate = vdb_get_vstate(vdb);
	if (!vstate) {
		DBG(vcp, "error: VSTATE not available");
		return 0;
	}

	change_counter = iov_pull_mem(iov, sizeof(*change_counter));
	if (!change_counter)
		return 0;

	if (*change_counter != vstate->counter) {
		DBG(vcp, "Change Counter Mismatch Volume not decremented!");
		return BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
	}

	vstate->mute = 0x01;
	vstate->counter = -~vstate->counter; /*Increment Change Counter*/

	return 0;
}

static uint8_t vocs_set_vol_offset(struct bt_vocs *vocs, struct bt_vcp *vcp,
				struct iovec *iov)
{
	struct bt_vcp_db *vdb;
	struct vol_offset_state *vstate;
	struct bt_vocs_set_vol_off *req;

	DBG(vcp, "Set Volume Offset");

	vdb = vcp_get_vdb(vcp);
	if (!vdb) {
		DBG(vcp, "error: VDB not available");
		return 0;
	}

	vstate = vdb_get_vostate(vdb);
	if (!vstate) {
		DBG(vcp, "error: VSTATE not available");
		return 0;
	}

	req = iov_pull_mem(iov, sizeof(*req));
	if (!req)
		return 0;

	if (req->change_counter != vstate->counter) {
		DBG(vcp, "Change Counter Mismatch Volume not decremented!");
		return BT_ATT_ERROR_INVALID_CHANGE_COUNTER;
	}

	vstate->vol_offset = le16_to_cpu(req->set_vol_offset);

	if (vstate->vol_offset > VOCS_VOL_OFFSET_UPPER_LIMIT ||
		vstate->vol_offset < VOCS_VOL_OFFSET_LOWER_LIMIT) {
		DBG(vcp, "error: Value Out of Range");
		return BT_ATT_ERROR_VALUE_OUT_OF_RANGE;
	}

	/* Increment Change Counter */
	vstate->counter = -~vstate->counter;

	gatt_db_attribute_notify(vdb->vocs->vos, (void *)vstate,
				 sizeof(struct vol_offset_state),
				 bt_vcp_get_att(vcp));
	return 0;
}

#define	BT_VCS_REL_VOL_DOWN		0x00
#define	BT_VCS_REL_VOL_UP		0x01
#define	BT_VCS_UNMUTE_REL_VOL_DOWN	0x02
#define	BT_VCS_UNMUTE_REL_VOL_UP	0x03
#define	BT_VCS_SET_ABSOLUTE_VOL		0x04
#define	BT_VCS_UNMUTE			0x05
#define	BT_VCS_MUTE			0x06

#define BT_VOCS_SET_VOL_OFFSET	0x01

#define VCS_OP(_str, _op, _size, _func) \
	{ \
		.str = _str, \
		.op = _op, \
		.size = _size, \
		.func = _func, \
	}

struct vcs_op_handler {
	const char *str;
	uint8_t	op;
	size_t	size;
	uint8_t	(*func)(struct bt_vcs *vcs, struct bt_vcp *vcp,
			struct iovec *iov);
} vcp_handlers[] = {
	VCS_OP("Relative Volume Down", BT_VCS_REL_VOL_DOWN,
		sizeof(uint8_t), vcs_rel_vol_down),
	VCS_OP("Relative Volume Up", BT_VCS_REL_VOL_UP,
		sizeof(uint8_t), vcs_rel_vol_up),
	VCS_OP("Unmute - Relative Volume Down", BT_VCS_UNMUTE_REL_VOL_DOWN,
		sizeof(uint8_t), vcs_unmute_rel_vol_down),
	VCS_OP("Unmute - Relative Volume Up", BT_VCS_UNMUTE_REL_VOL_UP,
		sizeof(uint8_t), vcs_unmute_rel_vol_up),
	VCS_OP("Set Absolute Volume", BT_VCS_SET_ABSOLUTE_VOL,
		sizeof(struct bt_vcs_ab_vol), vcs_set_absolute_vol),
	VCS_OP("UnMute", BT_VCS_UNMUTE,
		sizeof(uint8_t), vcs_unmute),
	VCS_OP("Mute", BT_VCS_MUTE,
		sizeof(uint8_t), vcs_mute),
	{}
};

#define VOCS_OP(_str, _op, _size, _func) \
	{ \
		.str = _str, \
		.op = _op, \
		.size = _size, \
		.func = _func, \
	}

struct vocs_op_handler {
	const char *str;
	uint8_t	op;
	size_t	size;
	uint8_t	(*func)(struct bt_vocs *vocs, struct bt_vcp *vcp,
			struct iovec *iov);
} vocp_handlers[] = {
	VOCS_OP("Set Volume Offset", BT_VOCS_SET_VOL_OFFSET,
		sizeof(uint8_t), vocs_set_vol_offset),
	{}
};

static void vcs_cp_write(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				const uint8_t *value, size_t len,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_vcs *vcs = user_data;
	struct bt_vcp *vcp = vcp_get_session(att, vcs->vdb->db);
	struct iovec iov = {
		.iov_base = (void *) value,
		.iov_len = len,
	};
	uint8_t	*vcp_op;
	struct vcs_op_handler *handler;
	uint8_t ret = BT_ATT_ERROR_REQUEST_NOT_SUPPORTED;

	DBG(vcp, "VCP Control Point Write");

	if (offset) {
		DBG(vcp, "invalid offset %d", offset);
		ret = BT_ATT_ERROR_INVALID_OFFSET;
		goto respond;
	}

	if (len < sizeof(*vcp_op)) {
		DBG(vcp, "invalid len %ld < %ld sizeof(*param)", len,
							sizeof(*vcp_op));
		ret = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		goto respond;
	}

	vcp_op = iov_pull_mem(&iov, sizeof(*vcp_op));

	for (handler = vcp_handlers; handler && handler->str; handler++) {
		if (handler->op != *vcp_op)
			continue;

		if (iov.iov_len < handler->size) {
			DBG(vcp, "invalid len %ld < %ld handler->size", len,
			    handler->size);
			ret = BT_ATT_ERROR_OPCODE_NOT_SUPPORTED;
			goto respond;
		}

		break;
	}

	if (handler && handler->str) {
		DBG(vcp, "%s", handler->str);

		ret = handler->func(vcs, vcp, &iov);
	} else {
		DBG(vcp, "Unknown opcode 0x%02x", *vcp_op);
		ret = BT_ATT_ERROR_OPCODE_NOT_SUPPORTED;
	}

respond:
	gatt_db_attribute_write_result(attrib, id, ret);
}

static void vocs_cp_write(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				const uint8_t *value, size_t len,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_vocs *vocs = user_data;
	struct bt_vcp *vcp = vcp_get_session(att, vocs->vdb->db);
	struct iovec iov = {
		.iov_base = (void *) value,
		.iov_len = len,
	};
	uint8_t	*vcp_op;
	struct vocs_op_handler *handler;
	uint8_t ret = BT_ATT_ERROR_REQUEST_NOT_SUPPORTED;

	DBG(vcp, "VOCP Control Point Write");

	if (offset) {
		DBG(vcp, "invalid offset %d", offset);
		ret = BT_ATT_ERROR_INVALID_OFFSET;
		goto respond;
	}

	if (len < sizeof(*vcp_op)) {
		DBG(vcp, "invalid len %ld < %ld sizeof(*param)", len,
							sizeof(*vcp_op));
		ret = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		goto respond;
	}

	vcp_op = iov_pull_mem(&iov, sizeof(*vcp_op));

	for (handler = vocp_handlers; handler && handler->str; handler++) {
		if (handler->op != *vcp_op)
			continue;

		if (iov.iov_len < handler->size) {
			DBG(vcp, "invalid len %ld < %ld handler->size", len,
			    handler->size);
			ret = BT_ATT_ERROR_OPCODE_NOT_SUPPORTED;
			goto respond;
		}

		break;
	}

	if (handler && handler->str) {
		DBG(vcp, "%s", handler->str);

		ret = handler->func(vocs, vcp, &iov);
	} else {
		DBG(vcp, "Unknown opcode 0x%02x", *vcp_op);
		ret = BT_ATT_ERROR_OPCODE_NOT_SUPPORTED;
	}

respond:
	gatt_db_attribute_write_result(attrib, id, ret);
}

static void vcs_state_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_vcs *vcs = user_data;
	struct iovec iov;

	iov.iov_base = vcs->vstate;
	iov.iov_len = sizeof(*vcs->vstate);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void vocs_state_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_vocs *vocs = user_data;
	struct vol_offset_state state;

	state.vol_offset = cpu_to_le16(vocs->vostate->vol_offset);
	state.counter = vocs->vostate->counter;

	gatt_db_attribute_read_result(attrib, id, 0, (void *)&state,
					sizeof(state));
}

static void vcs_flag_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_vcs *vcs = user_data;
	struct iovec iov;

	iov.iov_base = &vcs->vol_flag;
	iov.iov_len = sizeof(vcs->vol_flag);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static void vocs_voal_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_vocs *vocs = user_data;
	uint32_t loc;

	loc = cpu_to_le32(vocs->vocs_audio_loc);

	gatt_db_attribute_read_result(attrib, id, 0, (void *)&loc,
							sizeof(loc));
}

static void vocs_voaodec_read(struct gatt_db_attribute *attrib,
				unsigned int id, uint16_t offset,
				uint8_t opcode, struct bt_att *att,
				void *user_data)
{
	struct bt_vocs *vocs = user_data;
	struct iovec iov;

	iov.iov_base = vocs->vocs_ao_dec;
	iov.iov_len = strlen(vocs->vocs_ao_dec);

	gatt_db_attribute_read_result(attrib, id, 0, iov.iov_base,
							iov.iov_len);
}

static struct bt_vcs *vcs_new(struct gatt_db *db, struct bt_vcp_db *vdb)
{
	struct bt_vcs *vcs;
	struct vol_state *vstate;
	bt_uuid_t uuid;

	if (!db)
		return NULL;

	vcs = new0(struct bt_vcs, 1);

	vstate = new0(struct vol_state, 1);

	vcs->vstate = vstate;
	vcs->vol_flag = USERSET_VOLUME_SETTING;

	/* Populate DB with VCS attributes */
	bt_uuid16_create(&uuid, VCS_UUID);
	vcs->service = gatt_db_add_service(db, &uuid, true, 10);
	gatt_db_service_add_included(vcs->service, vdb->vocs->service);
	gatt_db_service_set_active(vdb->vocs->service, true);

	bt_uuid16_create(&uuid, VOL_STATE_CHRC_UUID);
	vcs->vs = gatt_db_service_add_characteristic(vcs->service,
					&uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ |
					BT_GATT_CHRC_PROP_NOTIFY,
					vcs_state_read, NULL,
					vcs);

	vcs->vs_ccc = gatt_db_service_add_ccc(vcs->service,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);

	bt_uuid16_create(&uuid, VOL_CP_CHRC_UUID);
	vcs->vol_cp = gatt_db_service_add_characteristic(vcs->service,
					&uuid,
					BT_ATT_PERM_WRITE,
					BT_GATT_CHRC_PROP_WRITE,
					NULL, vcs_cp_write,
					vcs);

	bt_uuid16_create(&uuid, VOL_FLAG_CHRC_UUID);
	vcs->vf = gatt_db_service_add_characteristic(vcs->service,
					&uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ |
					BT_GATT_CHRC_PROP_NOTIFY,
					vcs_flag_read, NULL,
					vcs);

	vcs->vf_ccc = gatt_db_service_add_ccc(vcs->service,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);


	gatt_db_service_set_active(vcs->service, true);

	return vcs;
}

static struct bt_vocs *vocs_new(struct gatt_db *db)
{
	struct bt_vocs *vocs;
	struct vol_offset_state *vostate;
	bt_uuid_t uuid;

	if (!db)
		return NULL;

	vocs = new0(struct bt_vocs, 1);

	vostate = new0(struct vol_offset_state, 1);

	vocs->vostate = vostate;
	vocs->vocs_audio_loc = BT_VCP_FRONT_LEFT;
	vocs->vocs_ao_dec = "Left Speaker";

	/* Populate DB with VOCS attributes */
	bt_uuid16_create(&uuid, VOL_OFFSET_CS_UUID);

	vocs->service = gatt_db_add_service(db, &uuid, false, 12);

	bt_uuid16_create(&uuid, VOCS_STATE_CHAR_UUID);
	vocs->vos = gatt_db_service_add_characteristic(vocs->service,
					&uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ |
					BT_GATT_CHRC_PROP_NOTIFY,
					vocs_state_read, NULL,
					vocs);

	vocs->vos_ccc = gatt_db_service_add_ccc(vocs->service,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);

	bt_uuid16_create(&uuid, VOCS_AUDIO_LOC_CHRC_UUID);
	vocs->voal = gatt_db_service_add_characteristic(vocs->service,
					&uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ |
					BT_GATT_CHRC_PROP_NOTIFY,
					vocs_voal_read, NULL,
					vocs);

	vocs->voal_ccc = gatt_db_service_add_ccc(vocs->service,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);

	bt_uuid16_create(&uuid, VOCS_CP_CHRC_UUID);
	vocs->vo_cp = gatt_db_service_add_characteristic(vocs->service,
					&uuid,
					BT_ATT_PERM_WRITE,
					BT_GATT_CHRC_PROP_WRITE,
					NULL, vocs_cp_write,
					vocs);

	bt_uuid16_create(&uuid, VOCS_AUDIO_OP_DESC_CHAR_UUID);
	vocs->voaodec = gatt_db_service_add_characteristic(vocs->service,
					&uuid,
					BT_ATT_PERM_READ,
					BT_GATT_CHRC_PROP_READ |
					BT_GATT_CHRC_PROP_NOTIFY,
					vocs_voaodec_read, NULL,
					vocs);

	vocs->voaodec_ccc = gatt_db_service_add_ccc(vocs->service,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);

	return vocs;
}

static struct bt_vcp_db *vcp_db_new(struct gatt_db *db)
{
	struct bt_vcp_db *vdb;

	if (!db)
		return NULL;

	vdb = new0(struct bt_vcp_db, 1);
	vdb->db = gatt_db_ref(db);

	if (!vcp_db)
		vcp_db = queue_new();

	vdb->vocs = vocs_new(db);
	vdb->vocs->vdb = vdb;
	vdb->vcs = vcs_new(db, vdb);
	vdb->vcs->vdb = vdb;

	queue_push_tail(vcp_db, vdb);

	return vdb;
}

static struct bt_vcp_db *vcp_get_db(struct gatt_db *db)
{
	struct bt_vcp_db *vdb;

	vdb = queue_find(vcp_db, vcp_db_match, db);
	if (vdb)
		return vdb;

	return vcp_db_new(db);
}

void bt_vcp_add_db(struct gatt_db *db)
{
	vcp_db_new(db);
}

bool bt_vcp_set_debug(struct bt_vcp *vcp, bt_vcp_debug_func_t func,
			void *user_data, bt_vcp_destroy_func_t destroy)
{
	if (!vcp)
		return false;

	if (vcp->debug_destroy)
		vcp->debug_destroy(vcp->debug_data);

	vcp->debug_func = func;
	vcp->debug_destroy = destroy;
	vcp->debug_data = user_data;

	return true;
}

unsigned int bt_vcp_register(bt_vcp_func_t attached, bt_vcp_func_t detached,
							void *user_data)
{
	struct bt_vcp_cb *cb;
	static unsigned int id;

	if (!attached && !detached)
		return 0;

	if (!vcp_cbs)
		vcp_cbs = queue_new();

	cb = new0(struct bt_vcp_cb, 1);
	cb->id = ++id ? id : ++id;
	cb->attached = attached;
	cb->detached = detached;
	cb->user_data = user_data;

	queue_push_tail(vcp_cbs, cb);

	return cb->id;
}

static bool match_id(const void *data, const void *match_data)
{
	const struct bt_vcp_cb *cb = data;
	unsigned int id = PTR_TO_UINT(match_data);

	return (cb->id == id);
}

bool bt_vcp_unregister(unsigned int id)
{
	struct bt_vcp_cb *cb;

	cb = queue_remove_if(vcp_cbs, match_id, UINT_TO_PTR(id));
	if (!cb)
		return false;

	free(cb);

	return true;
}

struct bt_vcp *bt_vcp_new(struct gatt_db *ldb, struct gatt_db *rdb)
{
	struct bt_vcp *vcp;
	struct bt_vcp_db *vdb;

	if (!ldb)
		return NULL;

	vdb = vcp_get_db(ldb);
	if (!vdb)
		return NULL;

	vcp = new0(struct bt_vcp, 1);
	vcp->ldb = vdb;
	vcp->pending = queue_new();

	if (!rdb)
		goto done;

	vdb = new0(struct bt_vcp_db, 1);
	vdb->db = gatt_db_ref(rdb);

	vcp->rdb = vdb;

done:
	bt_vcp_ref(vcp);

	return vcp;
}

static void vcp_vstate_notify(struct bt_vcp *vcp, uint16_t value_handle,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	struct vol_state vstate;

	memcpy(&vstate, value, sizeof(struct vol_state));

	DBG(vcp, "Vol Settings 0x%x", vstate.vol_set);
	DBG(vcp, "Mute Status 0x%x", vstate.mute);
	DBG(vcp, "Vol Counter 0x%x", vstate.counter);
}

static void vcp_voffset_state_notify(struct bt_vcp *vcp, uint16_t value_handle,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	struct vol_offset_state vostate;

	memcpy(&vostate, value, sizeof(struct vol_offset_state));

	DBG(vcp, "Vol Offset 0x%x", vostate.vol_offset);
	DBG(vcp, "Vol Offset Counter 0x%x", vostate.counter);
}

static void vcp_audio_loc_notify(struct bt_vcp *vcp, uint16_t value_handle,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	uint32_t *vocs_audio_loc_n = malloc(sizeof(uint32_t));
	*vocs_audio_loc_n = 0;

	if (value != NULL)
		memcpy(vocs_audio_loc_n, value, sizeof(uint32_t));

	DBG(vcp, "VOCS Audio Location 0x%x", *vocs_audio_loc_n);

	free(vocs_audio_loc_n);
}


static void vcp_audio_descriptor_notify(struct bt_vcp *vcp,
					uint16_t value_handle,
					const uint8_t *value,
					uint16_t length,
					void *user_data)
{
	char vocs_audio_dec_n[256] = {'\0'};

	memcpy(vocs_audio_dec_n, value, length);

	DBG(vcp, "VOCS Audio Descriptor 0x%s", *vocs_audio_dec_n);
}

static void vcp_vflag_notify(struct bt_vcp *vcp, uint16_t value_handle,
			     const uint8_t *value, uint16_t length,
			     void *user_data)
{
	uint8_t vflag;

	memcpy(&vflag, value, sizeof(vflag));

	DBG(vcp, "Vol Flag 0x%x", vflag);
}

static void read_vol_flag(struct bt_vcp *vcp, bool success, uint8_t att_ecode,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	uint8_t *vol_flag;
	struct iovec iov = {
		.iov_base = (void *) value,
		.iov_len = length,
	};

	if (!success) {
		DBG(vcp, "Unable to read Vol Flag: error 0x%02x", att_ecode);
		return;
	}

	vol_flag = iov_pull_mem(&iov, sizeof(*vol_flag));
	if (!vol_flag) {
		DBG(vcp, "Unable to get Vol Flag");
		return;
	}

	DBG(vcp, "Vol Flag:%x", *vol_flag);
}

static void read_vol_state(struct bt_vcp *vcp, bool success, uint8_t att_ecode,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	struct vol_state *vs;
	struct iovec iov = {
		.iov_base = (void *) value,
		.iov_len = length,
	};

	if (!success) {
		DBG(vcp, "Unable to read Vol State: error 0x%02x", att_ecode);
		return;
	}

	vs = iov_pull_mem(&iov, sizeof(*vs));
	if (!vs) {
		DBG(vcp, "Unable to get Vol State");
		return;
	}

	DBG(vcp, "Vol Set:%x", vs->vol_set);
	DBG(vcp, "Vol Mute:%x", vs->mute);
	DBG(vcp, "Vol Counter:%x", vs->counter);
}

static void read_vol_offset_state(struct bt_vcp *vcp, bool success,
				  uint8_t att_ecode,
				  const uint8_t *value, uint16_t length,
				  void *user_data)
{
	struct vol_offset_state *vos;
	struct iovec iov = {
		.iov_base = (void *) value,
		.iov_len = length,
	};

	if (!success) {
		DBG(vcp, "Unable to read Vol Offset State: error 0x%02x",
		    att_ecode);
		return;
	}

	vos = iov_pull_mem(&iov, sizeof(*vos));
	if (!vos) {
		DBG(vcp, "Unable to get Vol Offset State");
		return;
	}

	DBG(vcp, "Vol Offset: 0x%04x", le16_to_cpu(vos->vol_offset));
	DBG(vcp, "Vol Counter: 0x%02x", vos->counter);
}

static void read_vocs_audio_location(struct bt_vcp *vcp, bool success,
				     uint8_t att_ecode,
				     const uint8_t *value, uint16_t length,
				     void *user_data)
{
	uint32_t vocs_audio_loc;
	struct iovec iov;

	if (!value) {
		DBG(vcp, "Unable to get VOCS Audio Location");
		return;
	}

	if (!success) {
		DBG(vcp, "Unable to read VOCS Audio Location: error 0x%02x",
		    att_ecode);
		return;
	}

	iov.iov_base = (void *)value;
	iov.iov_len = length;

	if (!util_iov_pull_le32(&iov, &vocs_audio_loc)) {
		DBG(vcp, "Invalid size for VOCS Audio Location");
		return;
	}

	DBG(vcp, "VOCS Audio Loc: 0x%8x", vocs_audio_loc);
}


static void read_vocs_audio_descriptor(struct bt_vcp *vcp, bool success,
				       uint8_t att_ecode,
				       const uint8_t *value, uint16_t length,
				       void *user_data)
{
	char *vocs_ao_dec_r;

	if (!value) {
		DBG(vcp, "Unable to get VOCS Audio Location");
		return;
	}

	if (!success) {
		DBG(vcp, "Unable to read VOCS Audio Descriptor: error 0x%02x",
			att_ecode);
		return;
	}

	vocs_ao_dec_r = malloc(length+1);
	memset(vocs_ao_dec_r, 0, length+1);
	memcpy(vocs_ao_dec_r, value, length);

	if (!vocs_ao_dec_r) {
		DBG(vcp, "Unable to get VOCS Audio Descriptor");
		return;
	}

	DBG(vcp, "VOCS Audio Descriptor: %s", vocs_ao_dec_r);
	free(vocs_ao_dec_r);
	vocs_ao_dec_r = NULL;
}

static void vcp_pending_destroy(void *data)
{
	struct bt_vcp_pending *pending = data;
	struct bt_vcp *vcp = pending->vcp;

	if (queue_remove_if(vcp->pending, NULL, pending))
		free(pending);
}

static void vcp_pending_complete(bool success, uint8_t att_ecode,
				const uint8_t *value, uint16_t length,
				void *user_data)
{
	struct bt_vcp_pending *pending = user_data;

	if (pending->func)
		pending->func(pending->vcp, success, att_ecode, value, length,
						pending->user_data);
}

static void vcp_read_value(struct bt_vcp *vcp, uint16_t value_handle,
				vcp_func_t func, void *user_data)
{
	struct bt_vcp_pending *pending;

	pending = new0(struct bt_vcp_pending, 1);
	pending->vcp = vcp;
	pending->func = func;
	pending->user_data = user_data;

	pending->id = bt_gatt_client_read_value(vcp->client, value_handle,
						vcp_pending_complete, pending,
						vcp_pending_destroy);
	if (!pending->id) {
		DBG(vcp, "Unable to send Read request");
		free(pending);
		return;
	}

	queue_push_tail(vcp->pending, pending);
}

static void vcp_register(uint16_t att_ecode, void *user_data)
{
	struct bt_vcp_notify *notify = user_data;

	if (att_ecode)
		DBG(notify->vcp, "VCP register failed: 0x%04x", att_ecode);
}

static void vcp_notify(uint16_t value_handle, const uint8_t *value,
				uint16_t length, void *user_data)
{
	struct bt_vcp_notify *notify = user_data;

	if (notify->func)
		notify->func(notify->vcp, value_handle, value, length,
						notify->user_data);
}

static void vcp_notify_destroy(void *data)
{
	struct bt_vcp_notify *notify = data;
	struct bt_vcp *vcp = notify->vcp;

	if (queue_remove_if(vcp->notify, NULL, notify))
		free(notify);
}

static unsigned int vcp_register_notify(struct bt_vcp *vcp,
					uint16_t value_handle,
					vcp_notify_t func,
					void *user_data)
{
	struct bt_vcp_notify *notify;

	notify = new0(struct bt_vcp_notify, 1);
	notify->vcp = vcp;
	notify->func = func;
	notify->user_data = user_data;

	notify->id = bt_gatt_client_register_notify(vcp->client,
						value_handle, vcp_register,
						vcp_notify, notify,
						vcp_notify_destroy);
	if (!notify->id) {
		DBG(vcp, "Unable to register for notifications");
		free(notify);
		return 0;
	}

	queue_push_tail(vcp->notify, notify);

	return notify->id;
}

static void foreach_vcs_char(struct gatt_db_attribute *attr, void *user_data)
{
	struct bt_vcp *vcp = user_data;
	uint16_t value_handle;
	bt_uuid_t uuid, uuid_vstate, uuid_cp, uuid_vflag;
	struct bt_vcs *vcs;

	if (!gatt_db_attribute_get_char_data(attr, NULL, &value_handle,
						NULL, NULL, &uuid))
		return;

	bt_uuid16_create(&uuid_vstate, VOL_STATE_CHRC_UUID);
	bt_uuid16_create(&uuid_cp, VOL_CP_CHRC_UUID);
	bt_uuid16_create(&uuid_vflag, VOL_FLAG_CHRC_UUID);

	if (!bt_uuid_cmp(&uuid, &uuid_vstate)) {
		DBG(vcp, "VCS Vol state found: handle 0x%04x", value_handle);

		vcs = vcp_get_vcs(vcp);
		if (!vcs || vcs->vs)
			return;

		vcs->vs = attr;

		vcp_read_value(vcp, value_handle, read_vol_state, vcp);

		vcp->vstate_id = vcp_register_notify(vcp, value_handle,
						     vcp_vstate_notify, NULL);

		return;
	}

	if (!bt_uuid_cmp(&uuid, &uuid_cp)) {
		DBG(vcp, "VCS Volume CP found: handle 0x%04x", value_handle);

		vcs = vcp_get_vcs(vcp);
		if (!vcs || vcs->vol_cp)
			return;

		vcs->vol_cp = attr;

		return;
	}

	if (!bt_uuid_cmp(&uuid, &uuid_vflag)) {
		DBG(vcp, "VCS Vol Flag found: handle 0x%04x", value_handle);

		vcs = vcp_get_vcs(vcp);
		if (!vcs || vcs->vf)
			return;

		vcs->vf = attr;

		vcp_read_value(vcp, value_handle, read_vol_flag, vcp);
		vcp->vflag_id = vcp_register_notify(vcp, value_handle,
						    vcp_vflag_notify, NULL);

	}
}

static void foreach_vocs_char(struct gatt_db_attribute *attr, void *user_data)
{
	struct bt_vcp *vcp = user_data;
	uint16_t value_handle;
	bt_uuid_t uuid, uuid_vostate, uuid_audio_loc, uuid_vo_cp,
			uuid_audio_op_decs;
	struct bt_vocs *vocs;

	if (!gatt_db_attribute_get_char_data(attr, NULL, &value_handle,
						NULL, NULL, &uuid))
		return;

	bt_uuid16_create(&uuid_vostate, VOCS_STATE_CHAR_UUID);
	bt_uuid16_create(&uuid_audio_loc, VOCS_AUDIO_LOC_CHRC_UUID);
	bt_uuid16_create(&uuid_vo_cp, VOCS_CP_CHRC_UUID);
	bt_uuid16_create(&uuid_audio_op_decs, VOCS_AUDIO_OP_DESC_CHAR_UUID);

	if (!bt_uuid_cmp(&uuid, &uuid_vostate)) {
		DBG(vcp, "VOCS Vol state found: handle 0x%04x", value_handle);

		vocs = vcp_get_vocs(vcp);
		if (!vocs || vocs->vos)
			return;

		vocs->vos = attr;

		vcp_read_value(vcp, value_handle, read_vol_offset_state, vcp);

		vcp->state_id = vcp_register_notify(vcp, value_handle,
					vcp_voffset_state_notify, NULL);

		return;
	}

	if (!bt_uuid_cmp(&uuid, &uuid_audio_loc)) {
		DBG(vcp, "VOCS Volume Audio Location found: handle 0x%04x",
			value_handle);

		vocs = vcp_get_vocs(vcp);
		if (!vocs || vocs->voal)
			return;

		vocs->voal = attr;

		vcp_read_value(vcp, value_handle, read_vocs_audio_location,
				       vcp);

		vcp->audio_loc_id = vcp_register_notify(vcp, value_handle,
						vcp_audio_loc_notify, NULL);

		return;
	}

	if (!bt_uuid_cmp(&uuid, &uuid_vo_cp)) {
		DBG(vcp, "VOCS Volume CP found: handle 0x%04x", value_handle);

		vocs = vcp_get_vocs(vcp);
		if (!vocs || vocs->vo_cp)
			return;

		vocs->vo_cp = attr;

		return;
	}

	if (!bt_uuid_cmp(&uuid, &uuid_audio_op_decs)) {
		DBG(vcp, "VOCS Vol Audio Descriptor found: handle 0x%04x",
			value_handle);

		vocs = vcp_get_vocs(vcp);
		if (!vocs || vocs->voaodec)
			return;

		vocs->voaodec = attr;

		vcp_read_value(vcp, value_handle, read_vocs_audio_descriptor,
			       vcp);
		vcp->ao_dec_id = vcp_register_notify(vcp, value_handle,
					vcp_audio_descriptor_notify, NULL);

	}

}

static void foreach_vcs_service(struct gatt_db_attribute *attr,
						void *user_data)
{
	struct bt_vcp *vcp = user_data;
	struct bt_vcs *vcs = vcp_get_vcs(vcp);

	vcs->service = attr;

	gatt_db_service_set_claimed(attr, true);

	gatt_db_service_foreach_char(attr, foreach_vcs_char, vcp);
}

static void foreach_vocs_service(struct gatt_db_attribute *attr,
						void *user_data)
{
	struct bt_vcp *vcp = user_data;
	struct bt_vocs *vocs = vcp_get_vocs(vcp);

	vocs->service = attr;

	gatt_db_service_set_claimed(attr, true);

	gatt_db_service_foreach_char(attr, foreach_vocs_char, vcp);
}

bool bt_vcp_attach(struct bt_vcp *vcp, struct bt_gatt_client *client)
{
	bt_uuid_t uuid;

	if (!sessions)
		sessions = queue_new();

	queue_push_tail(sessions, vcp);

	if (!client)
		return true;

	if (vcp->client)
		return false;

	vcp->client = bt_gatt_client_clone(client);
	if (!vcp->client)
		return false;

	bt_uuid16_create(&uuid, VCS_UUID);
	gatt_db_foreach_service(vcp->rdb->db, &uuid, foreach_vcs_service, vcp);

	bt_uuid16_create(&uuid, VOL_OFFSET_CS_UUID);
	gatt_db_foreach_service(vcp->rdb->db, &uuid, foreach_vocs_service, vcp);

	return true;
}

