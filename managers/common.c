/*
 * Copyright (c) 2016-2018 Wuklab, Purdue University. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <lego/slab.h>
#include <lego/kernel.h>
#include <lego/string.h>
#include <lego/fit_ibapi.h>
#include <lego/comp_memory.h>
#include <processor/processor.h>

/* Indicate if processor or memory manager is up or not. */
int manager_state = MANAGER_DOWN;

unsigned int LEGO_LOCAL_NID __read_mostly = MY_NODE_ID;

/**
 * @node: target node id
 * @opcode: see <lego/comp_common.h>
 * @payload: payload of your message
 * @len_payload: length of your payload (beaware not to exceed valid *payload)
 * @retbuf: your buffer for the replied message
 * @max_len_retbuf: the maximum length of your return buffer
 *
 * @RETURN: 
 *
 * This function will block until network layer received reply.
 */
int net_send_reply_timeout(u32 node, u32 opcode,
			   void *payload, u32 len_payload,
			   void *retbuf, u32 max_len_retbuf, bool retbuf_is_phys,
			   u32 timeout)
{
	int ret;
	u32 len_msg;
	void *msg, *payload_msg;
	struct common_header *hdr;

	BUG_ON(!payload || !retbuf);

	/* compose message */
	len_msg = len_payload + sizeof(*hdr);
	msg = kmalloc(len_msg, GFP_KERNEL);
	if (unlikely(!msg)) {
		WARN(1, "OOM");
		return -ENOMEM;
	}

	hdr = to_common_header(msg);
	hdr->opcode = opcode;
	hdr->src_nid = LEGO_LOCAL_NID;
	hdr->length = sizeof(*hdr) + len_payload;

	payload_msg = to_payload(msg);
	memcpy(payload_msg, payload, len_payload);

	/* Synchronously send it out */
	ret = ibapi_send_reply_timeout(node, msg, len_msg, retbuf,
				   max_len_retbuf, retbuf_is_phys, timeout);
	if (ret == -ETIMEDOUT)
		pr_info(" %s() caller: %pS\n",
			FUNC, __builtin_return_address(0));

	kfree(msg);
	return ret;
}
