/* Copyright (c) 2019-2023 Stanford University
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "homa_impl.h"
#include "homa_lcache.h"
#define KSELFTEST_NOT_MAIN 1
#include "kselftest_harness.h"
#include "ccutils.h"
#include "mock.h"
#include "utils.h"

/* The following variable (and hook function) are used to mark an RPC
 * ready with an error (but only if thread is sleeping).
 */
struct homa_rpc *hook_rpc = NULL;
struct homa_sock *hook_hsk = NULL;
int delete_count = 0;
void handoff_hook(char *id)
{
	if (strcmp(id, "schedule") != 0)
		return;
	if (task_is_running(current))
		return;
	hook_rpc->error = -EFAULT;
	homa_rpc_handoff(hook_rpc);
	unit_log_printf("; ",
			"%d in ready_requests, %d in ready_responses, "
			"%d in request_interests, %d in response_interests",
			unit_list_length(&hook_rpc->hsk->ready_requests),
			unit_list_length(&hook_rpc->hsk->ready_responses),
			unit_list_length(&hook_rpc->hsk->request_interests),
			unit_list_length(&hook_rpc->hsk->response_interests));
}

/* The following hook function marks an RPC ready after several calls. */
int poll_count = 0;
void poll_hook(char *id)
{
	if (strcmp(id, "schedule") != 0)
		return;
	if (poll_count <= 0)
		return;
	poll_count--;
	if (poll_count == 0) {
		hook_rpc->error = -EFAULT;
		homa_rpc_handoff(hook_rpc);
	}
}

/* The following hook function hands off an RPC (with an error). */
void handoff_hook2(char *id)
{
	if (strcmp(id, "found_rpc") != 0)
		return;

	hook_rpc->error = -ETIMEDOUT;
	homa_rpc_handoff(hook_rpc);
}

/* The following hook function first hands off an RPC, then deletes it. */
int hook3_count = 0;
void handoff_hook3(char *id)
{
	if (hook3_count || (strcmp(id, "found_rpc") != 0))
		return;
	hook3_count++;

	homa_rpc_handoff(hook_rpc);
	homa_rpc_free(hook_rpc);
}

/* The following hook function deletes an RPC. */
void delete_hook(char *id)
{
	if (strcmp(id, "schedule") != 0)
		return;
	if (delete_count == 0) {
		homa_rpc_free(hook_rpc);
	}
	delete_count--;
}

/* The following function is used via unit_hook to delete an RPC after it
 * has been matched in homa_wait_for_message. */
void match_delete_hook(char *id)
{
	if (strcmp(id, "found_rpc") == 0)
		homa_rpc_free(hook_rpc);
}

/* The following hook function shuts down a socket. */
void shutdown_hook(char *id)
{
	if (strcmp(id, "schedule") != 0)
		return;
	homa_sock_shutdown(hook_hsk);
}

FIXTURE(homa_incoming) {
	struct in6_addr client_ip[5];
	int client_port;
	struct in6_addr server_ip[2];
	int server_port;
	__u64 client_id;
	__u64 server_id;
	sockaddr_in_union server_addr;
	struct homa homa;
	struct homa_sock hsk;
	struct data_header data;
	struct homa_interest interest;
	struct homa_lcache lcache;
	int incoming_delta;
};
FIXTURE_SETUP(homa_incoming)
{
	self->client_ip[0] = unit_get_in_addr("196.168.0.1");
	self->client_ip[1] = unit_get_in_addr("197.168.0.1");
	self->client_ip[2] = unit_get_in_addr("198.168.0.1");
	self->client_ip[3] = unit_get_in_addr("199.168.0.1");
	self->client_ip[4] = unit_get_in_addr("200.168.0.1");
	self->client_port = 40000;
	self->server_ip[0] = unit_get_in_addr("1.2.3.4");
	self->server_ip[1] = unit_get_in_addr("2.2.3.4");
	self->server_port = 99;
	self->client_id = 1234;
	self->server_id = 1235;
	homa_init(&self->homa);
	self->homa.num_priorities = 1;
	self->homa.poll_cycles = 0;
	self->homa.flags |= HOMA_FLAG_DONT_THROTTLE;
	self->homa.pacer_fifo_fraction = 0;
	self->homa.grant_fifo_fraction = 0;
	self->homa.grant_threshold = self->homa.rtt_bytes;
	mock_sock_init(&self->hsk, &self->homa, 0);
	self->server_addr.in6.sin6_family = self->hsk.inet.sk.sk_family;
	self->server_addr.in6.sin6_addr = self->server_ip[0];
	self->server_addr.in6.sin6_port =  htons(self->server_port);
	self->data = (struct data_header){.common = {
			.sport = htons(self->client_port),
	                .dport = htons(self->server_port),
			.type = DATA,
			.sender_id = cpu_to_be64(self->client_id)},
			.message_length = htonl(10000),
			.incoming = htonl(10000), .cutoff_version = 0,
		        .retransmit = 0,
			.seg = {.offset = 0, .segment_length = htonl(1400),
				.ack = {0, 0, 0}}};
	unit_log_clear();
	delete_count = 0;
	homa_lcache_init(&self->lcache);
	self->incoming_delta = 0;
}
FIXTURE_TEARDOWN(homa_incoming)
{
	homa_lcache_release(&self->lcache);
	homa_destroy(&self->homa);
	unit_teardown();
}

TEST_F(homa_incoming, homa_message_in_init)
{
	struct homa_message_in msgin;
	homa_message_in_init(&msgin, 127, 100);
	EXPECT_EQ(1, msgin.scheduled);
	EXPECT_EQ(100, msgin.incoming);
	homa_message_in_init(&msgin, 128, 500);
	EXPECT_EQ(128, msgin.incoming);
	EXPECT_EQ(0, msgin.scheduled);
	homa_message_in_init(&msgin, 130, 0);
	homa_message_in_init(&msgin, 0xfff, 0);
	homa_message_in_init(&msgin, 0xfff0, 0);
	homa_message_in_init(&msgin, 0x3000, 0);
	homa_message_in_init(&msgin, 1000000, 0);
	homa_message_in_init(&msgin, 2000000, 0);
	EXPECT_EQ(255, homa_cores[cpu_number]->metrics.small_msg_bytes[1]);
	EXPECT_EQ(130, homa_cores[cpu_number]->metrics.small_msg_bytes[2]);
	EXPECT_EQ(0xfff, homa_cores[cpu_number]->metrics.small_msg_bytes[63]);
	EXPECT_EQ(0x3000, homa_cores[cpu_number]->metrics.medium_msg_bytes[11]);
	EXPECT_EQ(0, homa_cores[cpu_number]->metrics.medium_msg_bytes[15]);
	EXPECT_EQ(3000000, homa_cores[cpu_number]->metrics.large_msg_bytes);
}

TEST_F(homa_incoming, homa_add_packet__basics)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, 99, 1000, 1000);
	homa_message_in_init(&crpc->msgin, 10000, 0);
	unit_log_clear();
	self->data.seg.offset = htonl(1400);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 1400, 1400));

	self->data.seg.offset = htonl(4200);
	self->data.seg.segment_length = htonl(800);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 800, 4200));

	self->data.seg.offset = 0;
	self->data.seg.segment_length = htonl(1400);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 1400, 0));
	unit_log_skb_list(&crpc->msgin.packets, 0);
	EXPECT_STREQ("DATA 1400@0; DATA 1400@1400; DATA 800@4200",
			unit_log_get());
	EXPECT_EQ(6400, crpc->msgin.bytes_remaining);

	unit_log_clear();
	self->data.seg.offset = htonl(2800);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 1400, 2800));
	unit_log_skb_list(&crpc->msgin.packets, 0);
	EXPECT_STREQ("DATA 1400@0; DATA 1400@1400; DATA 1400@2800; "
			"DATA 800@4200", unit_log_get());
}
TEST_F(homa_incoming, homa_add_packet__ignore_resends_of_copied_out_data)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, 99, 1000, 1000);
	homa_message_in_init(&crpc->msgin, 10000, 0);
	unit_log_clear();
	crpc->msgin.copied_out = 1500;
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 1400, 0));
	unit_log_skb_list(&crpc->msgin.packets, 0);
	EXPECT_STREQ("",
			unit_log_get());
	EXPECT_EQ(10000, crpc->msgin.bytes_remaining);

	self->data.seg.offset = htonl(1400);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 1400, 1400));
	unit_log_skb_list(&crpc->msgin.packets, 0);
	EXPECT_STREQ("DATA 1400@1400",
			unit_log_get());
	EXPECT_EQ(8700, crpc->msgin.bytes_remaining);
}
TEST_F(homa_incoming, homa_add_packet__varying_sizes)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, 99, 1000, 1000);
	homa_message_in_init(&crpc->msgin, 10000, 0);
	unit_log_clear();
	self->data.seg.offset = 0;
	self->data.seg.segment_length = htonl(4000);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 4000, 0));

	self->data.seg.offset = htonl(4000);
	self->data.seg.segment_length = htonl(6000);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 6000, 4000));
	unit_log_skb_list(&crpc->msgin.packets, 0);
	EXPECT_STREQ("DATA 4000@0; DATA 6000@4000",
			unit_log_get());
	EXPECT_EQ(0, crpc->msgin.bytes_remaining);
}
TEST_F(homa_incoming, homa_add_packet__redundant_packet)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, 99, 1000, 1000);
	homa_message_in_init(&crpc->msgin, 10000, 0);
	unit_log_clear();
	self->data.seg.offset = htonl(1400);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 1400, 1400));
	EXPECT_EQ(1, crpc->msgin.num_skbs);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 1400, 1400));
	unit_log_skb_list(&crpc->msgin.packets, 0);
	EXPECT_STREQ("DATA 1400@1400", unit_log_get());
	EXPECT_EQ(1, crpc->msgin.num_skbs);
}
TEST_F(homa_incoming, homa_add_packet__overlapping_ranges)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, 99, 1000, 1000);
	homa_message_in_init(&crpc->msgin, 10000, 0);
	unit_log_clear();
	self->data.seg.offset = htonl(1400);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 1400, 1400));
	self->data.seg.offset = htonl(2000);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 1400, 2000));
	unit_log_skb_list(&crpc->msgin.packets, 0);
	EXPECT_STREQ("DATA 1400@1400; DATA 1400@2000", unit_log_get());
	EXPECT_EQ(2, crpc->msgin.num_skbs);
	EXPECT_EQ(8000, crpc->msgin.bytes_remaining);

	unit_log_clear();
	self->data.seg.offset = htonl(1800);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 1400, 1800));
	unit_log_skb_list(&crpc->msgin.packets, 0);
	EXPECT_STREQ("DATA 1400@1400; DATA 1400@2000", unit_log_get());
	EXPECT_EQ(2, crpc->msgin.num_skbs);
	EXPECT_EQ(8000, crpc->msgin.bytes_remaining);
}

TEST_F(homa_incoming, homa_copy_to_user__basics)
{
	struct homa_rpc *crpc;

	mock_bpage_size = 2048;
	mock_bpage_shift = 11;
	EXPECT_EQ(0, -homa_pool_init(&self->hsk.buffer_pool, &self->homa,
			(void *) 0x1000000, 100*HOMA_BPAGE_SIZE));
	crpc = unit_client_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->server_port, self->client_id,
			1000, 4000);
	ASSERT_NE(NULL, crpc);
	self->data.message_length = htonl(4000);
	self->data.seg.offset = htonl(1000);
	homa_data_pkt(mock_skb_new(self->server_ip, &self->data.common,
			1400, 101000), crpc, NULL, &self->incoming_delta);
	self->data.seg.offset = htonl(1800);
	homa_data_pkt(mock_skb_new(self->server_ip, &self->data.common,
			1400, 201800), crpc, NULL, &self->incoming_delta);
	self->data.seg.offset = htonl(3200);
	self->data.seg.segment_length = htonl(800);
	homa_data_pkt(mock_skb_new(self->server_ip, &self->data.common,
			800, 303200), crpc, NULL, &self->incoming_delta);

	unit_log_clear();
	mock_copy_to_user_dont_copy = -1;
	EXPECT_EQ(0, -homa_copy_to_user(crpc));
	EXPECT_STREQ("skb_copy_datagram_iter: 1400 bytes to 0x1000000: 0-1399; "
			"skb_copy_datagram_iter: 648 bytes to 0x1000578: "
			"101400-102047; "
			"skb_copy_datagram_iter: 352 bytes to 0x1000800: "
			"102048-102399; "
			"skb_copy_datagram_iter: 800 bytes to 0x1000960: "
			"202400-203199; "
			"skb_copy_datagram_iter: 800 bytes to 0x1000c80: "
			"303200-303999",
			unit_log_get());
	EXPECT_EQ(crpc->msgin.total_length, crpc->msgin.copied_out);
	EXPECT_EQ(NULL, skb_peek(&crpc->msgin.packets));
	EXPECT_EQ(0, crpc->msgin.num_skbs);
}
TEST_F(homa_incoming, homa_copy_to_user__message_data_exceeds_length)
{
	struct homa_rpc *crpc;

	mock_bpage_size = 2048;
	mock_bpage_shift = 11;
	EXPECT_EQ(0, -homa_pool_init(&self->hsk.buffer_pool, &self->homa,
			(void *) 0x1000000, 100*HOMA_BPAGE_SIZE));
	crpc = unit_client_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->server_port, self->client_id,
			1000, 4000);
	ASSERT_NE(NULL, crpc);
	crpc->msgin.total_length = 1000;

	unit_log_clear();
	mock_copy_to_user_dont_copy = -1;
	EXPECT_EQ(0, -homa_copy_to_user(crpc));
	EXPECT_STREQ("skb_copy_datagram_iter: 1000 bytes to 0x1000000: 0-999",
			unit_log_get());
	EXPECT_EQ(1000, crpc->msgin.copied_out);
	EXPECT_EQ(1, crpc->msgin.num_skbs);
}
TEST_F(homa_incoming, homa_copy_to_user__gap_in_packets)
{
	struct homa_rpc *crpc;

	mock_bpage_size = 2048;
	mock_bpage_shift = 11;
	EXPECT_EQ(0, -homa_pool_init(&self->hsk.buffer_pool, &self->homa,
			(void *) 0x1000000, 100*HOMA_BPAGE_SIZE));
	crpc = unit_client_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->server_port, self->client_id,
			1000, 4000);
	ASSERT_NE(NULL, crpc);
	self->data.message_length = htonl(4000);
	self->data.seg.offset = htonl(2000);
	homa_data_pkt(mock_skb_new(self->server_ip, &self->data.common,
			1400, 101000), crpc, NULL, &self->incoming_delta);

	unit_log_clear();
	mock_copy_to_user_dont_copy = -1;
	EXPECT_EQ(0, -homa_copy_to_user(crpc));
	EXPECT_STREQ("skb_copy_datagram_iter: 1400 bytes to 0x1000000: 0-1399",
			unit_log_get());
	EXPECT_EQ(1400, crpc->msgin.copied_out);
	EXPECT_EQ(1, crpc->msgin.num_skbs);
}
TEST_F(homa_incoming, homa_copy_to_user__no_buffer_pool_available)
{
	struct homa_rpc *crpc;

	crpc = unit_client_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->server_port, self->client_id,
			1000, 4000);
	ASSERT_NE(NULL, crpc);
	EXPECT_EQ(12, -homa_copy_to_user(crpc));
	EXPECT_EQ(0, crpc->msgin.copied_out);
}
TEST_F(homa_incoming, homa_copy_to_user__error_in_copy_to_user)
{
	struct homa_rpc *crpc;

	mock_bpage_size = 2048;
	mock_bpage_shift = 11;
	EXPECT_EQ(0, -homa_pool_init(&self->hsk.buffer_pool, &self->homa,
			(void *) 0x1000000, 100*HOMA_BPAGE_SIZE));
	crpc = unit_client_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->server_port, self->client_id,
			1000, 4000);
	ASSERT_NE(NULL, crpc);
	self->data.message_length = htonl(4000);
	self->data.seg.offset = htonl(1400);
	homa_data_pkt(mock_skb_new(self->server_ip, &self->data.common,
			1400, 101000), crpc, NULL, &self->incoming_delta);
	self->data.seg.offset = htonl(2800);
	self->data.seg.segment_length = htonl(1200);
	homa_data_pkt(mock_skb_new(self->server_ip, &self->data.common,
			1400, 101000), crpc, NULL, &self->incoming_delta);

	unit_log_clear();
	mock_copy_data_errors = 2;
	EXPECT_EQ(14, -homa_copy_to_user(crpc));
	EXPECT_STREQ("skb_copy_datagram_iter: 1400 bytes to 0x1000000: 0-1399",
			unit_log_get());
	EXPECT_EQ(2800, crpc->msgin.copied_out);
	EXPECT_EQ(1, crpc->msgin.num_skbs);
}
TEST_F(homa_incoming, homa_copy_to_user__many_chunks_for_one_skb)
{
	struct homa_rpc *crpc;

	mock_bpage_size = 512;
	mock_bpage_shift = 9;
	EXPECT_EQ(0, -homa_pool_init(&self->hsk.buffer_pool, &self->homa,
			(void *) 0x1000000, 100*HOMA_BPAGE_SIZE));
	crpc = unit_client_rpc(&self->hsk, UNIT_OUTGOING, self->client_ip,
			self->server_ip, self->server_port, self->client_id,
			1000, 4000);
	ASSERT_NE(NULL, crpc);
	self->data.message_length = htonl(4000);
	self->data.seg.segment_length = htonl(3000);
	homa_data_pkt(mock_skb_new(self->server_ip, &self->data.common,
			3000, 101000), crpc, NULL, &self->incoming_delta);

	unit_log_clear();
	mock_copy_to_user_dont_copy = -1;
	EXPECT_EQ(0, -homa_copy_to_user(crpc));
	EXPECT_STREQ("skb_copy_datagram_iter: 512 bytes to 0x1000000: "
			"101000-101511; "
			"skb_copy_datagram_iter: 512 bytes to 0x1000200: "
			"101512-102023; "
			"skb_copy_datagram_iter: 512 bytes to 0x1000400: "
			"102024-102535; "
			"skb_copy_datagram_iter: 512 bytes to 0x1000600: "
			"102536-103047; "
			"skb_copy_datagram_iter: 512 bytes to 0x1000800: "
			"103048-103559; "
			"skb_copy_datagram_iter: 440 bytes to 0x1000a00: "
			"103560-103999",
			unit_log_get());
	EXPECT_EQ(3000, crpc->msgin.copied_out);
	EXPECT_EQ(0, crpc->msgin.num_skbs);
}

TEST_F(homa_incoming, homa_get_resend_range__uninitialized_rpc)
{
	struct homa_message_in msgin;
	struct resend_header resend;

	msgin.total_length = -1;
	homa_get_resend_range(&msgin, &resend);
	EXPECT_EQ(0, resend.offset);
	EXPECT_EQ(100, ntohl(resend.length));
}
TEST_F(homa_incoming, homa_get_resend_range__empty_range)
{
	struct resend_header resend;
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_OUTGOING,
			self->client_ip, self->server_ip, self->client_port,
			self->server_id, 5000, 5000);

	ASSERT_NE(NULL, srpc);
	homa_get_resend_range(&srpc->msgin, &resend);
	EXPECT_EQ(0, resend.offset);
	EXPECT_EQ(0, ntohl(resend.length));
}
TEST_F(homa_incoming, homa_get_resend_range__various_gaps)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, 99, 1000, 1000);
	homa_message_in_init(&crpc->msgin, 10000, 0);
	crpc->msgin.incoming = 10000;
	struct resend_header resend;

	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 1400, 1400));
	homa_get_resend_range(&crpc->msgin, &resend);
	EXPECT_EQ(1400, ntohl(resend.offset));
	EXPECT_EQ(8600, ntohl(resend.length));

	self->data.seg.offset = htonl(8600);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 1400, 8600));
	homa_get_resend_range(&crpc->msgin, &resend);
	EXPECT_EQ(1400, ntohl(resend.offset));
	EXPECT_EQ(7200, ntohl(resend.length));

	self->data.seg.offset = htonl(6000);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 1400, 6000));
	homa_get_resend_range(&crpc->msgin, &resend);
	EXPECT_EQ(1400, ntohl(resend.offset));
	EXPECT_EQ(4600, ntohl(resend.length));

	self->data.seg.offset = htonl(4600);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 1400, 4600));
	homa_get_resend_range(&crpc->msgin, &resend);
	EXPECT_EQ(1400, ntohl(resend.offset));
	EXPECT_EQ(3200, ntohl(resend.length));
}
TEST_F(homa_incoming, homa_get_resend_range__received_past_granted)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, 99, 1000, 1000);
	homa_message_in_init(&crpc->msgin, 10000, 0);
	struct resend_header resend;

	self->data.message_length = htonl(2500);
	self->data.seg.offset = htonl(0);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 1400, 0));
	self->data.seg.offset = htonl(1500);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 1400, 0));
	self->data.seg.offset = htonl(2900);
	self->data.seg.segment_length = htonl(1100);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 1100, 0));
	crpc->msgin.incoming = 2000;
	homa_get_resend_range(&crpc->msgin, &resend);
	EXPECT_EQ(1400, ntohl(resend.offset));
	EXPECT_EQ(100, ntohl(resend.length));
}
TEST_F(homa_incoming, homa_get_resend_range__gap_at_beginning)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, 99, 1000, 1000);
	homa_message_in_init(&crpc->msgin, 10000, 0);
	struct resend_header resend;

	self->data.seg.offset = htonl(6200);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 1400, 6200));
	homa_get_resend_range(&crpc->msgin, &resend);
	EXPECT_EQ(0, ntohl(resend.offset));
	EXPECT_EQ(6200, ntohl(resend.length));
}
TEST_F(homa_incoming, homa_get_resend_range__gap_starts_just_after_copied_out)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, 99, 1000, 1000);
	homa_message_in_init(&crpc->msgin, 10000, 0);
	struct resend_header resend;

	self->data.seg.offset = htonl(5000);
	homa_add_packet(crpc, mock_skb_new(self->client_ip,
			&self->data.common, 1400, 6200));
	crpc->msgin.bytes_remaining = 6600;
	crpc->msgin.incoming = 7000;
	crpc->msgin.copied_out = 2000;
	homa_get_resend_range(&crpc->msgin, &resend);
	EXPECT_EQ(2000, ntohl(resend.offset));
	EXPECT_EQ(3000, ntohl(resend.length));
}

TEST_F(homa_incoming, homa_pkt_dispatch__handle_ack)
{
	struct homa_sock hsk;
	mock_sock_init(&hsk, &self->homa, self->server_port);
	//mock_sock_init(&self->hsk, &self->homa, 0);
	struct homa_rpc *srpc = unit_server_rpc(&hsk, UNIT_OUTGOING,
			self->client_ip, self->server_ip, self->client_port,
			self->server_id, 100, 3000);
	ASSERT_NE(NULL, srpc);
	self->data.seg.ack = (struct homa_ack) {
			.client_port = htons(self->client_port),
		       .server_port = htons(self->server_port),
		       .client_id = cpu_to_be64(self->client_id)};
	self->data.common.sender_id = cpu_to_be64(self->client_id+10);
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &self->data.common,
			1400, 0), &self->hsk, &self->lcache,
			&self->incoming_delta);
	EXPECT_STREQ("DEAD", homa_symbol_for_state(srpc));
	homa_sock_shutdown(&hsk);
}
TEST_F(homa_incoming, homa_pkt_dispatch__new_server_rpc)
{
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &self->data.common,
			1400, 0), &self->hsk, &self->lcache,
			&self->incoming_delta);
	EXPECT_EQ(1, unit_list_length(&self->hsk.active_rpcs));
	EXPECT_EQ(1, mock_skb_count());
}
TEST_F(homa_incoming, homa_pkt_dispatch__existing_server_rpc)
{
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT,
			self->client_ip, self->server_ip, self->client_port,
			self->server_id, 10000, 100);
	ASSERT_NE(NULL, srpc);
	EXPECT_EQ(8600, srpc->msgin.bytes_remaining);
	self->data.seg.offset = htonl(1400);
	self->data.common.sender_id = cpu_to_be64(self->client_id);
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &self->data.common,
			1400, 0), &self->hsk, &self->lcache,
			&self->incoming_delta);
	EXPECT_EQ(7200, srpc->msgin.bytes_remaining);
}
TEST_F(homa_incoming, homa_pkt_dispatch__cant_create_rpc)
{
	mock_kmalloc_errors = 1;
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &self->data.common,
			1400, 0), &self->hsk, &self->lcache,
			&self->incoming_delta);
	EXPECT_EQ(0, unit_list_length(&self->hsk.active_rpcs));
	EXPECT_EQ(0, mock_skb_count());
}
TEST_F(homa_incoming, homa_pkt_dispatch__non_data_packet_for_existing_server_rpc)
{
	struct resend_header resend = {.common = {
		.sport = htons(self->client_port),
		.dport = htons(self->server_port),
		.type = RESEND,
		.sender_id = cpu_to_be64(self->client_id)},
		.offset = 0,
		.length = 1000,
		.priority = 3};
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT,
			self->client_ip, self->server_ip, self->client_port,
			self->server_id, 10000, 100);
	ASSERT_NE(NULL, srpc);
	unit_log_clear();
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &resend.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_STREQ("xmit BUSY", unit_log_get());
}
TEST_F(homa_incoming, homa_pkt_dispatch__unknown_client_rpc)
{
	struct resend_header h = {{.sport = htons(self->server_port),
	                .dport = htons(self->client_port),
			.sender_id = cpu_to_be64(99991),
			.type = UNKNOWN}};
	mock_xmit_log_verbose = 1;
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_EQ(1, homa_cores[cpu_number]->metrics.unknown_rpcs);
}
TEST_F(homa_incoming, homa_pkt_dispatch__unknown_server_rpc)
{
	struct resend_header h = {{.sport = htons(self->client_port),
	                .dport = htons(self->server_port),
			.sender_id = cpu_to_be64(99990),
			.type = UNKNOWN}};
	mock_xmit_log_verbose = 1;
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_EQ(1, homa_cores[cpu_number]->metrics.unknown_rpcs);
}
TEST_F(homa_incoming, homa_pkt_dispatch__existing_client_rpc)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);
	EXPECT_EQ(11200, crpc->msgout.granted);
	unit_log_clear();

	struct grant_header h = {{.sport = htons(self->server_port),
	                .dport = htons(self->client_port),
			.sender_id = cpu_to_be64(self->server_id),
			.type = GRANT},
			.offset = htonl(12600),
			.priority = 3};
	homa_pkt_dispatch(mock_skb_new(self->server_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_EQ(12600, crpc->msgout.granted);
}
TEST_F(homa_incoming, homa_pkt_dispatch__lcached_client_rpc)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);
	EXPECT_EQ(11200, crpc->msgout.granted);
	unit_log_clear();

	struct grant_header h = {{.sport = htons(self->server_port),
	                .dport = htons(self->client_port),
			.sender_id = cpu_to_be64(self->server_id),
			.type = GRANT},
			.offset = htonl(12600),
			.priority = 3};
	homa_pkt_dispatch(mock_skb_new(self->server_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_EQ(12600, crpc->msgout.granted);
	h.offset = htonl(14000);
	homa_pkt_dispatch(mock_skb_new(self->server_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_EQ(14000, crpc->msgout.granted);
}
TEST_F(homa_incoming, homa_pkt_dispatch__cutoffs_for_unknown_client_rpc)
{
	struct homa_peer *peer;
	struct cutoffs_header h = {{.sport = htons(self->server_port),
	                .dport = htons(self->client_port),
			.sender_id = cpu_to_be64(99991),
			.type = CUTOFFS},
		        .unsched_cutoffs = {htonl(10), htonl(9), htonl(8),
			htonl(7), htonl(6), htonl(5), htonl(4),
			htonl(3)},
			.cutoff_version = 400};
	homa_pkt_dispatch(mock_skb_new(self->server_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	peer = homa_peer_find(&self->homa.peers, self->server_ip,
			&self->hsk.inet);
	ASSERT_FALSE(IS_ERR(peer));
	EXPECT_EQ(400, peer->cutoff_version);
	EXPECT_EQ(9, peer->unsched_cutoffs[1]);
	EXPECT_EQ(3, peer->unsched_cutoffs[7]);
}
TEST_F(homa_incoming, homa_pkt_dispatch__resend_for_unknown_server_rpc)
{
	struct resend_header h = {{.sport = htons(self->client_port),
	                .dport = htons(self->server_port),
			.sender_id = cpu_to_be64(99990),
			.type = RESEND},
			.offset = 0, .length = 2000, .priority = 5};
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_STREQ("xmit UNKNOWN", unit_log_get());
}
TEST_F(homa_incoming, homa_pkt_dispatch__reset_counters)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);
	EXPECT_EQ(11200, crpc->msgout.granted);
	unit_log_clear();
	crpc->silent_ticks = 5;
	crpc->peer->outstanding_resends = 2;

	struct grant_header h = {.common = {.sport = htons(self->server_port),
	                .dport = htons(self->client_port),
			.sender_id = cpu_to_be64(self->server_id),
			.type = GRANT},
			.offset = htonl(12600), .priority = 3};
	homa_pkt_dispatch(mock_skb_new(self->server_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_EQ(0, crpc->silent_ticks);
	EXPECT_EQ(0, crpc->peer->outstanding_resends);

	/* Don't reset silent_ticks for some packet types. */
	h.common.type = NEED_ACK;
	crpc->silent_ticks = 5;
	crpc->peer->outstanding_resends = 2;
	homa_pkt_dispatch(mock_skb_new(self->server_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_EQ(5, crpc->silent_ticks);
	EXPECT_EQ(0, crpc->peer->outstanding_resends);
}
TEST_F(homa_incoming, homa_pkt_dispatch__forced_reap)
{
	struct homa_rpc *dead = unit_client_rpc(&self->hsk,
			UNIT_RCVD_MSG, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 20000);
	homa_rpc_free(dead);
	EXPECT_EQ(30, self->hsk.dead_skbs);
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_OUTGOING,
			self->client_ip, self->server_ip, self->client_port,
			self->server_id, 10000, 5000);
	ASSERT_NE(NULL, srpc);
	self->homa.dead_buffs_limit = 16;
	mock_cycles = ~0;

	/* First packet: below the threshold for reaps. */
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &self->data.common,
			1400, 0), &self->hsk, &self->lcache,
			&self->incoming_delta);
	EXPECT_EQ(30, self->hsk.dead_skbs);
	EXPECT_EQ(0, homa_cores[cpu_number]->metrics.data_pkt_reap_cycles);

	/* Second packet: must reap. */
	self->homa.dead_buffs_limit = 15;
	self->homa.reap_limit = 10;
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &self->data.common,
			1400, 0), &self->hsk, &self->lcache,
			&self->incoming_delta);
	EXPECT_EQ(20, self->hsk.dead_skbs);
	EXPECT_NE(0, homa_cores[cpu_number]->metrics.data_pkt_reap_cycles);
}
TEST_F(homa_incoming, homa_pkt_dispatch__unknown_type)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);
	EXPECT_EQ(11200, crpc->msgout.granted);
	unit_log_clear();

	struct common_header h = {.sport = htons(self->server_port),
	                .dport = htons(self->client_port),
			.sender_id = cpu_to_be64(self->server_id), .type = 99};
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &h, 0, 0), &self->hsk,
			&self->lcache, &self->incoming_delta);
	EXPECT_EQ(1, homa_cores[cpu_number]->metrics.unknown_packet_types);
}
TEST_F(homa_incoming, homa_pkt_dispatch__new_server_rpc_but_socket_shutdown)
{
	self->hsk.shutdown = 1;
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &self->data.common,
			1400, 0), &self->hsk, &self->lcache,
			&self->incoming_delta);
	EXPECT_EQ(0, unit_list_length(&self->hsk.active_rpcs));
	self->hsk.shutdown = 0;
}

TEST_F(homa_incoming, homa_data_pkt__basics)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 1000, 1600);
	ASSERT_NE(NULL, crpc);
	unit_log_clear();
	crpc->msgout.next_xmit_offset = crpc->msgout.length;
	self->data.message_length = htonl(1600);
	homa_data_pkt(mock_skb_new(self->server_ip, &self->data.common,
			1400, 0), crpc, NULL, &self->incoming_delta);
	EXPECT_EQ(RPC_INCOMING, crpc->state);
	EXPECT_EQ(1, unit_list_length(&self->hsk.ready_responses));
	EXPECT_EQ(200, crpc->msgin.bytes_remaining);
	EXPECT_EQ(1, crpc->msgin.num_skbs);
	EXPECT_EQ(1600, crpc->msgin.incoming);
	EXPECT_EQ(1, homa_cores[cpu_number]->metrics.responses_received);
}
TEST_F(homa_incoming, homa_data_pkt__wrong_client_rpc_state)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_RCVD_ONE_PKT, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 1000, 2000);
	ASSERT_NE(NULL, crpc);

	crpc->state = RPC_DEAD;
	self->data.message_length = htonl(2000);
	self->data.seg.offset = htonl(1400);
	self->data.seg.segment_length = htonl(600);
	homa_data_pkt(mock_skb_new(self->server_ip, &self->data.common,
			600, 1400), crpc, NULL, &self->incoming_delta);
	EXPECT_EQ(600, crpc->msgin.bytes_remaining);
	EXPECT_EQ(1, crpc->msgin.num_skbs);
	crpc->state = RPC_INCOMING;
}
TEST_F(homa_incoming, homa_data_pkt__wrong_server_rpc_state)
{
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_OUTGOING,
			self->client_ip, self->server_ip, self->client_port,
			self->server_id, 1400, 5000);
	ASSERT_NE(NULL, srpc);
	int skbs = mock_skb_count();
	homa_data_pkt(mock_skb_new(self->client_ip, &self->data.common,
			1400, 0), srpc, NULL, &self->incoming_delta);
	EXPECT_EQ(RPC_OUTGOING, srpc->state);
	EXPECT_EQ(skbs, mock_skb_count());
}
TEST_F(homa_incoming, homa_data_pkt__initialize_msgin)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 1000, 1600);
	ASSERT_NE(NULL, crpc);
	self->data.message_length = htonl(1600);
	homa_data_pkt(mock_skb_new(self->server_ip, &self->data.common,
			1400, 0), crpc, NULL, &self->incoming_delta);
	EXPECT_EQ(200, crpc->msgin.bytes_remaining);
	EXPECT_EQ(1600, crpc->msgin.incoming);
	EXPECT_EQ(200, self->incoming_delta);
}
TEST_F(homa_incoming, homa_data_pkt__update_delta)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 1000, 5000);
	EXPECT_NE(NULL, crpc);
	unit_log_clear();

	/* Total incoming goes up on first packet (count unscheduled bytes). */
	self->data.message_length = htonl(5000);
	self->data.incoming = htonl(4000);
	homa_data_pkt(mock_skb_new(self->server_ip, &self->data.common,
			1400, 0), crpc, NULL, &self->incoming_delta);
	EXPECT_EQ(2600, self->incoming_delta);

	/* Total incoming drops on subsequent packet. */
	self->data.seg.offset = htonl(2800);
	homa_data_pkt(mock_skb_new(self->server_ip, &self->data.common,
			1400, 2800), crpc, NULL, &self->incoming_delta);
	EXPECT_EQ(1200, self->incoming_delta);

	/* Duplicate packet should have no effect. */
	self->data.seg.offset = htonl(2800);
	homa_data_pkt(mock_skb_new(self->server_ip, &self->data.common,
			1400, 2800), crpc, NULL, &self->incoming_delta);
	EXPECT_EQ(1200, self->incoming_delta);
}
TEST_F(homa_incoming, homa_data_pkt__handoff)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 1000, 3000);
	ASSERT_NE(NULL, crpc);
	unit_log_clear();
	crpc->msgout.next_xmit_offset = crpc->msgout.length;

	/* First packet is not first in sequence, so can't hand off. */
	self->data.message_length = htonl(3000);
	self->data.seg.offset = htonl(1400);
	homa_data_pkt(mock_skb_new(self->server_ip, &self->data.common,
			1400, 0), crpc, NULL, &self->incoming_delta);
	EXPECT_EQ(0, unit_list_length(&self->hsk.ready_responses));
	EXPECT_FALSE(atomic_read(&crpc->flags) & RPC_PKTS_READY);
	EXPECT_EQ(1600, crpc->msgin.bytes_remaining);
	EXPECT_EQ(1, crpc->msgin.num_skbs);

	/* Second packet fills the gap. */
	self->data.message_length = htonl(3000);
	self->data.seg.offset = htonl(0);
	homa_data_pkt(mock_skb_new(self->server_ip, &self->data.common,
			1400, 0), crpc, NULL, &self->incoming_delta);
	EXPECT_EQ(1, unit_list_length(&self->hsk.ready_responses));
	EXPECT_TRUE(atomic_read(&crpc->flags) & RPC_PKTS_READY);
	EXPECT_EQ(200, crpc->msgin.bytes_remaining);
	EXPECT_EQ(2, crpc->msgin.num_skbs);
}
TEST_F(homa_incoming, homa_data_pkt__add_to_grantables)
{
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT,
			self->client_ip, self->server_ip, self->client_port,
			self->server_id, 100000, 1000);
	ASSERT_NE(NULL, srpc);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_SUBSTR("id 1235", unit_log_get());
}
TEST_F(homa_incoming, homa_data_pkt__send_cutoffs)
{
	self->homa.cutoff_version = 2;
	self->homa.unsched_cutoffs[0] = 19;
	self->homa.unsched_cutoffs[1] = 18;
	self->homa.unsched_cutoffs[2] = 17;
	self->homa.unsched_cutoffs[3] = 16;
	self->homa.unsched_cutoffs[4] = 15;
	self->homa.unsched_cutoffs[5] = 14;
	self->homa.unsched_cutoffs[6] = 13;
	self->homa.unsched_cutoffs[7] = 12;
	self->data.message_length = htonl(5000);
	mock_xmit_log_verbose = 1;
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &self->data.common,
			1400, 0), &self->hsk, &self->lcache,
			&self->incoming_delta);
	EXPECT_SUBSTR("cutoffs 19 18 17 16 15 14 13 12, version 2",
			unit_log_get());

	/* Try again, but this time no comments should be sent because
	 * no time has elapsed since the last cutoffs were sent.
	 */
	unit_log_clear();
	self->homa.cutoff_version = 3;
	self->data.seg.offset = 1400;
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &self->data.common,
			1400, 0), &self->hsk, &self->lcache,
			&self->incoming_delta);
	EXPECT_STREQ("", unit_log_get());
}
TEST_F(homa_incoming, homa_data_pkt__cutoffs_up_to_date)
{
	self->homa.cutoff_version = 123;
	self->data.cutoff_version = htons(123);
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &self->data.common,
			1400, 0), &self->hsk, &self->lcache,
			&self->incoming_delta);
	EXPECT_STREQ("sk->sk_data_ready invoked", unit_log_get());
}

TEST_F(homa_incoming, homa_grant_pkt__basics)
{
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_OUTGOING,
			self->client_ip, self->server_ip, self->client_port,
			self->server_id, 100, 20000);
	ASSERT_NE(NULL, srpc);
	homa_xmit_data(srpc, false);
	unit_log_clear();

	struct grant_header h = {{.sport = htons(srpc->dport),
	                .dport = htons(self->hsk.port),
			.sender_id = cpu_to_be64(self->client_id),
			.type = GRANT},
		        .offset = htonl(12600),
			.priority = 3};
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_EQ(12600, srpc->msgout.granted);
	EXPECT_STREQ("xmit DATA 1400@11200", unit_log_get());

	/* Don't let grant offset go backwards. */
	h.offset = htonl(10000);
	unit_log_clear();
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_EQ(12600, srpc->msgout.granted);
	EXPECT_STREQ("", unit_log_get());

	/* Wrong state. */
	h.offset = htonl(20000);
	srpc->state = RPC_INCOMING;
	unit_log_clear();
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_EQ(12600, srpc->msgout.granted);
	EXPECT_STREQ("", unit_log_get());

	/* Must restore old state to avoid potential crashes. */
	srpc->state = RPC_OUTGOING;
}
TEST_F(homa_incoming, homa_grant_pkt__grant_past_end_of_message)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);
	unit_log_clear();

	struct grant_header h = {{.sport = htons(self->server_port),
	                .dport = htons(self->client_port),
			.sender_id = cpu_to_be64(self->server_id),
			.type = GRANT},
		        .offset = htonl(25000),
			.priority = 3};
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_EQ(20000, crpc->msgout.granted);
}

TEST_F(homa_incoming, homa_resend_pkt__unknown_rpc)
{
	struct resend_header h = {{.sport = htons(self->client_port),
	                .dport = htons(self->server_port),
			.sender_id = cpu_to_be64(self->client_id),
			.type = RESEND},
		        .offset = htonl(100),
			.length = htonl(200),
			.priority = 3};

	homa_pkt_dispatch(mock_skb_new(self->client_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_STREQ("xmit UNKNOWN", unit_log_get());
}
TEST_F(homa_incoming, homa_resend_pkt__server_sends_busy)
{
	struct resend_header h = {{.sport = htons(self->client_port),
	                .dport = htons(self->server_port),
			.sender_id = cpu_to_be64(self->client_id),
			.type = RESEND},
		        .offset = htonl(100),
			.length = htonl(200),
			.priority = 3};
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_RCVD_MSG,
			self->client_ip, self->server_ip, self->client_port,
			self->server_id, 100, 20000);
	ASSERT_NE(NULL, srpc);
	unit_log_clear();

	homa_pkt_dispatch(mock_skb_new(self->client_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_STREQ("xmit BUSY", unit_log_get());
}
TEST_F(homa_incoming, homa_resend_pkt__client_not_outgoing)
{
	/* Important to respond to resends even if client thinks the
	 * server must already have received everything.
	 */
	struct resend_header h = {{.sport = htons(self->server_port),
	                .dport = htons(self->client_port),
			.sender_id = cpu_to_be64(self->server_id),
			.type = RESEND},
		        .offset = htonl(100),
			.length = htonl(200),
			.priority = 3};
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_RCVD_ONE_PKT, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 2000, 3000);
	ASSERT_NE(NULL, crpc);
	unit_log_clear();

	homa_pkt_dispatch(mock_skb_new(self->server_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_STREQ("xmit DATA retrans 1400@0", unit_log_get());
}
TEST_F(homa_incoming, homa_resend_pkt__send_busy_instead_of_data)
{
	struct resend_header h = {{.sport = htons(self->server_port),
	                .dport = htons(self->client_port),
			.sender_id = cpu_to_be64(self->server_id),
			.type = RESEND},
		        .offset = htonl(100),
			.length = htonl(200),
			.priority = 3};
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 2000, 100);
	ASSERT_NE(NULL, crpc);
	unit_log_clear();

	homa_pkt_dispatch(mock_skb_new(self->server_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_STREQ("xmit BUSY", unit_log_get());
}
TEST_F(homa_incoming, homa_resend_pkt__client_send_data)
{
	struct resend_header h = {{.sport = htons(self->server_port),
	                .dport = htons(self->client_port),
			.sender_id = cpu_to_be64(self->server_id),
			.type = RESEND},
		        .offset = htonl(100),
			.length = htonl(200),
			.priority = 3};
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 2000, 100);
	ASSERT_NE(NULL, crpc);
	homa_xmit_data(crpc, false);
	unit_log_clear();
	mock_clear_xmit_prios();

	homa_pkt_dispatch(mock_skb_new(self->server_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_STREQ("xmit DATA retrans 1400@0", unit_log_get());
	EXPECT_STREQ("3", mock_xmit_prios);
}
TEST_F(homa_incoming, homa_resend_pkt__server_send_data)
{
	struct resend_header h = {{.sport = htons(self->client_port),
	                .dport = htons(self->server_port),
			.sender_id = cpu_to_be64(self->client_id),
			.type = RESEND},
		        .offset = htonl(100),
			.length = htonl(2000),
			.priority = 4};
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_OUTGOING,
			self->client_ip, self->server_ip, self->client_port,
			self->server_id, 100, 20000);
	ASSERT_NE(NULL, srpc);
	homa_xmit_data(srpc, false);
	unit_log_clear();
	mock_clear_xmit_prios();

	homa_pkt_dispatch(mock_skb_new(self->client_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_STREQ("xmit DATA retrans 1400@0; "
			"xmit DATA retrans 1400@1400", unit_log_get());
	EXPECT_STREQ("4 4", mock_xmit_prios);
}

TEST_F(homa_incoming, homa_unknown_pkt__client_resend_all)
{
	struct unknown_header h = {{.sport = htons(self->server_port),
	                .dport = htons(self->client_port),
			.sender_id = cpu_to_be64(self->server_id),
			.type = UNKNOWN}};
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 2000, 2000);
	ASSERT_NE(NULL, crpc);
	homa_xmit_data(crpc, false);
	unit_log_clear();

	mock_xmit_log_verbose = 1;
	homa_pkt_dispatch(mock_skb_new(self->server_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_STREQ("xmit DATA from 0.0.0.0:32768, dport 99, id 1234, "
			"message_length 2000, offset 0, data_length 1400, "
			"incoming 2000, RETRANSMIT; "
			"xmit DATA from 0.0.0.0:32768, dport 99, id 1234, "
			"message_length 2000, offset 1400, data_length 600, "
			"incoming 2000, RETRANSMIT",
			unit_log_get());
	EXPECT_EQ(-1, crpc->msgin.total_length);
}
TEST_F(homa_incoming, homa_unknown_pkt__client_resend_part)
{
	struct unknown_header h = {{.sport = htons(self->server_port),
	                .dport = htons(self->client_port),
			.sender_id = cpu_to_be64(self->server_id),
			.type = UNKNOWN}};
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 2000, 2000);
	ASSERT_NE(NULL, crpc);
	crpc->msgout.granted = 1400;
	homa_xmit_data(crpc, false);
	unit_log_clear();

	mock_xmit_log_verbose = 1;
	homa_pkt_dispatch(mock_skb_new(self->server_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_STREQ("xmit DATA from 0.0.0.0:32768, dport 99, id 1234, "
			"message_length 2000, offset 0, data_length 1400, "
			"incoming 1400, RETRANSMIT",
			unit_log_get());
	EXPECT_EQ(-1, crpc->msgin.total_length);
}
TEST_F(homa_incoming, homa_unknown_pkt__free_server_rpc)
{
	struct unknown_header h = {{.sport = htons(self->client_port),
	                .dport = htons(self->server_port),
			.sender_id = cpu_to_be64(self->client_id),
			.type = UNKNOWN}};
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_OUTGOING,
			self->client_ip, self->server_ip, self->client_port,
			self->server_id, 100, 20000);
	ASSERT_NE(NULL, srpc);
	unit_log_clear();

	homa_pkt_dispatch(mock_skb_new(self->client_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_STREQ("DEAD", homa_symbol_for_state(srpc));
}

TEST_F(homa_incoming, homa_cutoffs_pkt_basics)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);
	EXPECT_EQ(11200, crpc->msgout.granted);
	unit_log_clear();

	struct cutoffs_header h = {{.sport = htons(self->server_port),
	                .dport = htons(self->client_port),
			.sender_id = cpu_to_be64(self->server_id),
			.type = CUTOFFS},
		        .unsched_cutoffs = {htonl(10), htonl(9), htonl(8),
			htonl(7), htonl(6), htonl(5), htonl(4), htonl(3)},
			.cutoff_version = 400};
	homa_pkt_dispatch(mock_skb_new(self->server_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_EQ(400, crpc->peer->cutoff_version);
	EXPECT_EQ(9, crpc->peer->unsched_cutoffs[1]);
	EXPECT_EQ(3, crpc->peer->unsched_cutoffs[7]);
}
TEST_F(homa_incoming, homa_cutoffs__cant_find_peer)
{
	struct homa_peer *peer;
	struct cutoffs_header h = {{.sport = htons(self->server_port),
	                .dport = htons(self->client_port),
			.sender_id = cpu_to_be64(self->server_id),
			.type = CUTOFFS},
		        .unsched_cutoffs = {htonl(10), htonl(9), htonl(8),
			htonl(7), htonl(6), htonl(5), htonl(4), htonl(3)},
			.cutoff_version = 400};
	struct sk_buff *skb = mock_skb_new(self->server_ip, &h.common, 0, 0);
	mock_kmalloc_errors = 1;
	homa_cutoffs_pkt(skb, &self->hsk);
	EXPECT_EQ(1, homa_cores[cpu_number]->metrics.peer_kmalloc_errors);
	peer = homa_peer_find(&self->homa.peers, self->server_ip,
			&self->hsk.inet);
	ASSERT_FALSE(IS_ERR(peer));
	EXPECT_EQ(0, peer->cutoff_version);
}

TEST_F(homa_incoming, homa_need_ack_pkt__rpc_response_fully_received)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_RCVD_MSG, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 100, 3000);
	ASSERT_NE(NULL, crpc);
	unit_log_clear();
	mock_xmit_log_verbose = 1;
	struct need_ack_header h = {.common = {
			.sport = htons(self->server_port),
	                .dport = htons(self->client_port),
			.sender_id = cpu_to_be64(self->server_id),
			.type = NEED_ACK}};
	homa_pkt_dispatch(mock_skb_new(self->server_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_STREQ("xmit ACK from 0.0.0.0:40000, dport 99, id 1234, acks",
			unit_log_get());
	EXPECT_EQ(1, homa_cores[cpu_number]->metrics.packets_received[
			NEED_ACK - DATA]);
}
TEST_F(homa_incoming, homa_need_ack_pkt__rpc_response_not_fully_received)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_RCVD_ONE_PKT, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 100, 3000);
	ASSERT_NE(NULL, crpc);
	unit_log_clear();
	mock_xmit_log_verbose = 1;
	struct need_ack_header h = {.common = {
			.sport = htons(self->server_port),
	                .dport = htons(self->client_port),
			.sender_id = cpu_to_be64(self->server_id),
			.type = NEED_ACK}};
	homa_pkt_dispatch(mock_skb_new(self->server_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_STREQ("", unit_log_get());
	EXPECT_EQ(1, homa_cores[cpu_number]->metrics.packets_received[
			NEED_ACK - DATA]);
}
TEST_F(homa_incoming, homa_need_ack_pkt__rpc_not_incoming)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 100, 3000);
	ASSERT_NE(NULL, crpc);
	unit_log_clear();
	mock_xmit_log_verbose = 1;
	struct need_ack_header h = {.common = {
			.sport = htons(self->server_port),
	                .dport = htons(self->client_port),
			.sender_id = cpu_to_be64(self->server_id),
			.type = NEED_ACK}};
	homa_pkt_dispatch(mock_skb_new(self->server_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_STREQ("", unit_log_get());
	EXPECT_EQ(1, homa_cores[cpu_number]->metrics.packets_received[
			NEED_ACK - DATA]);
}
TEST_F(homa_incoming, homa_need_ack_pkt__rpc_doesnt_exist)
{
	struct homa_peer *peer = homa_peer_find(&self->homa.peers,
			self->server_ip, &self->hsk.inet);
	peer->acks[0].client_port = htons(self->client_port);
	peer->acks[0].server_port = htons(self->server_port);
	peer->acks[0].client_id = cpu_to_be64(self->client_id+2);
	peer->num_acks = 1;
	mock_xmit_log_verbose = 1;
	struct need_ack_header h = {.common = {
			.sport = htons(self->server_port),
	                .dport = htons(self->client_port),
			.sender_id = cpu_to_be64(self->server_id),
			.type = NEED_ACK}};
	homa_pkt_dispatch(mock_skb_new(self->server_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_STREQ("xmit ACK from 0.0.0.0:40000, dport 99, id 1234, "
			"acks [cp 40000, sp 99, id 1236]", unit_log_get());
}

TEST_F(homa_incoming, homa_ack_pkt__target_rpc_exists)
{
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_OUTGOING,
			self->client_ip, self->server_ip, self->client_port,
			self->server_id, 100, 5000);
	ASSERT_NE(NULL, srpc);
	EXPECT_EQ(1, unit_list_length(&self->hsk.active_rpcs));
	unit_log_clear();
	mock_xmit_log_verbose = 1;
	struct ack_header h = {.common = {
			.sport = htons(self->client_port),
	                .dport = htons(self->server_port),
			.sender_id = cpu_to_be64(self->client_id),
			.type = ACK},
			.num_acks = htons(0)};
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &h.common, 0, 0),
			&self->hsk, &self->lcache, &self->incoming_delta);
	EXPECT_EQ(0, unit_list_length(&self->hsk.active_rpcs));
	EXPECT_EQ(1, homa_cores[cpu_number]->metrics.packets_received[
			ACK - DATA]);
}
TEST_F(homa_incoming, homa_ack_pkt__target_rpc_doesnt_exist)
{
	struct homa_sock hsk1;
	mock_sock_init(&hsk1, &self->homa, self->server_port);
	struct homa_rpc *srpc1 = unit_server_rpc(&hsk1, UNIT_OUTGOING,
			self->client_ip, self->server_ip, self->client_port,
			self->server_id, 100, 5000);
	struct homa_rpc *srpc2 = unit_server_rpc(&hsk1, UNIT_OUTGOING,
			self->client_ip, self->server_ip, self->client_port,
			self->server_id+2, 100, 5000);
	ASSERT_NE(NULL, srpc1);
	ASSERT_NE(NULL, srpc2);
	EXPECT_EQ(2, unit_list_length(&hsk1.active_rpcs));
	unit_log_clear();
	mock_xmit_log_verbose = 1;
	struct ack_header h = {.common = {
			.sport = htons(self->client_port + 1),
	                .dport = htons(self->server_port),
			.sender_id = cpu_to_be64(self->client_id),
			.type = ACK},
			.num_acks = htons(2)};
	h.acks[0] = (struct homa_ack) {.client_port = htons(self->client_port),
	              .server_port = htons(self->server_port),
	              .client_id = cpu_to_be64(self->server_id+5)};
	h.acks[1] = (struct homa_ack) {.client_port = htons(self->client_port),
	              .server_port = htons(self->server_port),
	              .client_id = cpu_to_be64(self->server_id+1)};
	homa_pkt_dispatch(mock_skb_new(self->client_ip, &h.common, 0, 0),
			&hsk1, &self->lcache, &self->incoming_delta);
	EXPECT_EQ(1, unit_list_length(&hsk1.active_rpcs));
	EXPECT_STREQ("OUTGOING", homa_symbol_for_state(srpc1));
	EXPECT_STREQ("DEAD", homa_symbol_for_state(srpc2));
	homa_sock_shutdown(&hsk1);
}

TEST_F(homa_incoming, homa_check_grantable__not_ready_for_grant)
{
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT,
			self->client_ip, self->server_ip, self->client_port,
			self->server_id, 5000, 100);
	ASSERT_NE(NULL, srpc);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("", unit_log_get());

	srpc->msgin.total_length = 20000;
	srpc->msgin.bytes_remaining = 15000;
	srpc->msgin.incoming = 18000;
	homa_check_grantable(&self->homa, srpc);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("", unit_log_get());

	srpc->msgin.incoming = 20000;
	homa_check_grantable(&self->homa, srpc);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("", unit_log_get());

	srpc->msgin.incoming = 18000;
	srpc->msgin.bytes_remaining = 10000;
	homa_check_grantable(&self->homa, srpc);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 1235, remaining 10000",
			unit_log_get());
}
TEST_F(homa_incoming, homa_check_grantable__insert_in_peer_list)
{
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 100000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 3, 50000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 5, 120000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 7, 70000, 100);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 3, remaining 48600; "
			"request from 196.168.0.1, id 7, remaining 68600; "
			"request from 196.168.0.1, id 1, remaining 98600; "
			"request from 196.168.0.1, id 5, remaining 118600",
			unit_log_get());
	EXPECT_EQ(1, self->homa.num_grantable_peers);
}
TEST_F(homa_incoming, homa_check_grantable__adjust_order_in_peer_list)
{
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 20000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 3, 30000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 5, 40000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 7, 50000, 100);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 1, remaining 18600; "
			"request from 196.168.0.1, id 3, remaining 28600; "
			"request from 196.168.0.1, id 5, remaining 38600; "
			"request from 196.168.0.1, id 7, remaining 48600",
			unit_log_get());

	struct homa_rpc *srpc = homa_find_server_rpc(&self->hsk,
			self->client_ip, self->client_port, 5);
	ASSERT_NE(NULL, srpc);
	homa_rpc_unlock(srpc);
	srpc->msgin.bytes_remaining = 28600;
	homa_check_grantable(&self->homa, srpc);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 1, remaining 18600; "
			"request from 196.168.0.1, id 3, remaining 28600; "
			"request from 196.168.0.1, id 5, remaining 28600; "
			"request from 196.168.0.1, id 7, remaining 48600",
			unit_log_get());

	srpc->msgin.bytes_remaining = 28599;
	homa_check_grantable(&self->homa, srpc);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 1, remaining 18600; "
			"request from 196.168.0.1, id 5, remaining 28599; "
			"request from 196.168.0.1, id 3, remaining 28600; "
			"request from 196.168.0.1, id 7, remaining 48600",
			unit_log_get());

	srpc = homa_find_server_rpc(&self->hsk, self->client_ip,
			self->client_port, 7);
	ASSERT_NE(NULL, srpc);
	homa_rpc_unlock(srpc);;
	srpc->msgin.bytes_remaining = 1000;
	homa_check_grantable(&self->homa, srpc);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 7, remaining 1000; "
			"request from 196.168.0.1, id 1, remaining 18600; "
			"request from 196.168.0.1, id 5, remaining 28599; "
			"request from 196.168.0.1, id 3, remaining 28600",
			unit_log_get());
}
TEST_F(homa_incoming, homa_check_grantable__age_tiebreaker_in_peer_list)
{
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 20000, 100);
	struct homa_rpc *srpc2 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT,
			self->client_ip, self->server_ip, self->client_port,
			3, 30000, 100);
	struct homa_rpc *srpc3 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT,
			self->client_ip, self->server_ip, self->client_port,
			5, 30000, 100);
	struct homa_rpc *srpc4 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT,
			self->client_ip, self->server_ip, self->client_port,
			7, 50000, 100);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 1, remaining 18600; "
			"request from 196.168.0.1, id 3, remaining 28600; "
			"request from 196.168.0.1, id 5, remaining 28600; "
			"request from 196.168.0.1, id 7, remaining 48600",
			unit_log_get());
	srpc4->msgin.bytes_remaining = 28600;
	srpc4->msgin.birth = 1000;
	srpc3->msgin.birth = 2000;
	srpc2->msgin.birth = 500;
	homa_check_grantable(&self->homa, srpc4);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 1, remaining 18600; "
			"request from 196.168.0.1, id 3, remaining 28600; "
			"request from 196.168.0.1, id 7, remaining 28600; "
			"request from 196.168.0.1, id 5, remaining 28600",
			unit_log_get());
}
TEST_F(homa_incoming, homa_check_grantable__insert_in_homa_list)
{
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 100000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+1,
			self->server_ip, self->client_port, 3, 50000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+2,
			self->server_ip, self->client_port, 5, 120000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+3,
			self->server_ip, self->client_port, 7, 70000, 100);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 197.168.0.1, id 3, remaining 48600; "
			"request from 199.168.0.1, id 7, remaining 68600; "
			"request from 196.168.0.1, id 1, remaining 98600; "
			"request from 198.168.0.1, id 5, remaining 118600",
			unit_log_get());
	EXPECT_EQ(4, self->homa.num_grantable_peers);
}
TEST_F(homa_incoming, homa_check_grantable__age_tiebreaker_inserting_in_homa_list)
{
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 20000, 100);
	struct homa_rpc *srpc2 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT,
			self->client_ip+1, self->server_ip, self->client_port,
			3, 30000, 100);
	struct homa_rpc *srpc3 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT,
			self->client_ip+2, self->server_ip, self->client_port,
			5, 30000, 100);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 1, remaining 18600; "
			"request from 197.168.0.1, id 3, remaining 28600; "
			"request from 198.168.0.1, id 5, remaining 28600",
			unit_log_get());

	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip+1,
			self->server_port, self->client_id, 1400, 30000);
	srpc2->msgin.birth = 1000;
	srpc3->msgin.birth = 2000;
	mock_cycles = 1500;
	self->data.message_length = htonl(30000);
	homa_data_pkt(mock_skb_new(self->server_ip+1, &self->data.common,
			1400, 0), crpc, NULL, &self->incoming_delta);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 1, remaining 18600; "
			"request from 197.168.0.1, id 3, remaining 28600; "
			"response from 2.2.3.4, id 1234, remaining 28600; "
			"request from 198.168.0.1, id 5, remaining 28600",
			unit_log_get());
	EXPECT_EQ(4, self->homa.num_grantable_peers);
}
TEST_F(homa_incoming, homa_check_grantable__move_upward_in_homa_list)
{
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 20000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+1,
			self->server_ip, self->client_port, 3, 30000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+2,
			self->server_ip, self->client_port, 5, 40000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+3,
			self->server_ip, self->client_port, 7, 50000, 100);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 1, remaining 18600; "
			"request from 197.168.0.1, id 3, remaining 28600; "
			"request from 198.168.0.1, id 5, remaining 38600; "
			"request from 199.168.0.1, id 7, remaining 48600",
			unit_log_get());

	struct homa_rpc *srpc = homa_find_server_rpc(&self->hsk,
			self->client_ip+2, self->client_port, 5);
	ASSERT_NE(NULL, srpc);
	homa_rpc_unlock(srpc);
	srpc->msgin.bytes_remaining = 28600;
	homa_check_grantable(&self->homa, srpc);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 1, remaining 18600; "
			"request from 197.168.0.1, id 3, remaining 28600; "
			"request from 198.168.0.1, id 5, remaining 28600; "
			"request from 199.168.0.1, id 7, remaining 48600",
			unit_log_get());

	srpc->msgin.bytes_remaining = 28599;
	homa_check_grantable(&self->homa, srpc);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 1, remaining 18600; "
			"request from 198.168.0.1, id 5, remaining 28599; "
			"request from 197.168.0.1, id 3, remaining 28600; "
			"request from 199.168.0.1, id 7, remaining 48600",
			unit_log_get());

	srpc = homa_find_server_rpc(&self->hsk, self->client_ip+3,
			self->client_port, 7);
	ASSERT_NE(NULL, srpc);
	homa_rpc_unlock(srpc);;
	srpc->msgin.bytes_remaining = 1000;
	homa_check_grantable(&self->homa, srpc);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 199.168.0.1, id 7, remaining 1000; "
			"request from 196.168.0.1, id 1, remaining 18600; "
			"request from 198.168.0.1, id 5, remaining 28599; "
			"request from 197.168.0.1, id 3, remaining 28600",
			unit_log_get());
}
TEST_F(homa_incoming, homa_check_grantable__age_tiebreaker_moving_upward_in_homa_list)
{
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 20000, 100);
	struct homa_rpc *srpc2 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT,
			self->client_ip+1, self->server_ip, self->client_port,
			3, 30000, 100);
	struct homa_rpc *srpc3 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT,
			self->client_ip+2, self->server_ip, self->client_port,
			5, 30000, 100);
	struct homa_rpc *srpc4 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT,
			self->client_ip+3, self->server_ip, self->client_port,
			7, 50000, 100);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 1, remaining 18600; "
			"request from 197.168.0.1, id 3, remaining 28600; "
			"request from 198.168.0.1, id 5, remaining 28600; "
			"request from 199.168.0.1, id 7, remaining 48600",
			unit_log_get());

	srpc2->msgin.birth = 1000;
	srpc3->msgin.birth = 2000;
	srpc4->msgin.birth = 1500;
	srpc4->msgin.bytes_remaining = 28600;
	homa_check_grantable(&self->homa, srpc4);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 1, remaining 18600; "
			"request from 197.168.0.1, id 3, remaining 28600; "
			"request from 199.168.0.1, id 7, remaining 28600; "
			"request from 198.168.0.1, id 5, remaining 28600",
			unit_log_get());
}

TEST_F(homa_incoming, homa_send_grants__basics)
{
	struct homa_rpc *srpc1, *srpc2, *srpc3, *srpc4;
	srpc1 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 20000, 100);
	srpc2 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+1,
			self->server_ip, self->client_port, 3, 30000, 100);
	srpc3 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+2,
			self->server_ip, self->client_port, 5, 40000, 100);
	srpc4 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+3,
			self->server_ip, self->client_port, 7, 50000, 100);
	EXPECT_EQ(34400, atomic_read(&self->homa.total_incoming));

	/* First attempt: no headroom for grants */
	self->homa.max_incoming = 30000;
	unit_log_clear();
	homa_send_grants(&self->homa);
	EXPECT_STREQ("", unit_log_get());

	/* Second attempt: can grant only the first message and part of
	 * the second. */
	self->homa.max_incoming = 36000;
	unit_log_clear();
	homa_send_grants(&self->homa);
	EXPECT_STREQ("xmit GRANT 11400@3; xmit GRANT 10200@2", unit_log_get());
	EXPECT_EQ(11400, srpc1->msgin.incoming);

	/* Third attempt: finish granting to second message. */

	self->homa.max_incoming = 37200;
	unit_log_clear();
	homa_send_grants(&self->homa);
	EXPECT_STREQ("xmit GRANT 11400@2", unit_log_get());

	/* Try again (no new grants, since nothing has changed). */
	unit_log_clear();
	homa_send_grants(&self->homa);
	EXPECT_STREQ("", unit_log_get());

	/* Now create enough headroom for all of the messages. */
	self->homa.max_incoming = 50000;
	unit_log_clear();
	homa_send_grants(&self->homa);
	EXPECT_STREQ("xmit GRANT 11400@1; xmit GRANT 11400@0", unit_log_get());
	EXPECT_EQ(11400, srpc2->msgin.incoming);
	EXPECT_EQ(11400, srpc3->msgin.incoming);
	EXPECT_EQ(11400, srpc4->msgin.incoming);
	EXPECT_EQ(40000, atomic_read(&self->homa.total_incoming));
}
TEST_F(homa_incoming, homa_send_grants__enlarge_window)
{
	struct homa_rpc *srpc1, *srpc2;
	srpc1 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 40000, 100);
	srpc2 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+1,
			self->server_ip, self->client_port, 3, 40000, 100);
	EXPECT_EQ(17200, atomic_read(&self->homa.total_incoming));

	self->homa.max_incoming = 40000;
	self->homa.max_grant_window = 40000;
	unit_log_clear();
	homa_send_grants(&self->homa);
	EXPECT_STREQ("xmit GRANT 16400@1; xmit GRANT 16400@0", unit_log_get());
	EXPECT_EQ(16400, srpc1->msgin.incoming);
	EXPECT_EQ(16400, srpc2->msgin.incoming);
	EXPECT_EQ(30000, atomic_read(&self->homa.total_incoming));
}
TEST_F(homa_incoming, homa_send_grants__one_grant_per_peer)
{
	struct homa_rpc *srpc1, *srpc2, *srpc3, *srpc4;
	srpc1 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 20000, 100);
	srpc2 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 3, 30000, 100);
	srpc3 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 5, 40000, 100);
	srpc4 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+1,
			self->server_ip, self->client_port, 7, 50000, 100);
	srpc1->msgin.incoming = 1400;
	srpc2->msgin.incoming = 1400;
	srpc3->msgin.incoming = 1400;
	srpc4->msgin.incoming = 1400;
	atomic_set(&self->homa.total_incoming, 0);
	self->homa.max_incoming = 25000;
	homa_send_grants(&self->homa);
	EXPECT_EQ(11400, srpc1->msgin.incoming);
	EXPECT_EQ(1400, srpc2->msgin.incoming);
	EXPECT_EQ(1400, srpc3->msgin.incoming);
	EXPECT_EQ(11400, srpc4->msgin.incoming);
}
TEST_F(homa_incoming, homa_send_grants__truncate_grant_to_message_length)
{
	struct homa_rpc *srpc;
	srpc = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 11000, 100);
	EXPECT_NE(NULL, srpc);
	EXPECT_EQ(8600, atomic_read(&self->homa.total_incoming));

	self->homa.max_incoming = 50000;
	unit_log_clear();
	homa_send_grants(&self->homa);
	EXPECT_STREQ("xmit GRANT 11000@0", unit_log_get());
	EXPECT_EQ(11000, srpc->msgin.incoming);
	EXPECT_EQ(9600, atomic_read(&self->homa.total_incoming));
}
TEST_F(homa_incoming, homa_send_grants__choose_priority_level)
{
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 40000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+1,
			self->server_ip, self->client_port, 3, 30000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+2,
			self->server_ip, self->client_port, 5, 20000, 100);
	atomic_set(&self->homa.total_incoming, 0);
	self->homa.max_incoming = 30000;
	homa_send_grants(&self->homa);
	EXPECT_SUBSTR("xmit GRANT 11400@2; "
			"xmit GRANT 11400@1; "
			"xmit GRANT 11400@0", unit_log_get());
}
TEST_F(homa_incoming, homa_send_grants__share_lowest_priority_level)
{
	struct homa_rpc *srpc1, *srpc2, *srpc3, *srpc4;
	srpc1 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 20000, 100);
	srpc2 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+1,
			self->server_ip, self->client_port, 3, 30000, 100);
	srpc3 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+2,
			self->server_ip, self->client_port, 5, 40000, 100);
	srpc4 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+3,
			self->server_ip, self->client_port, 5, 50000, 100);
	srpc1->msgin.incoming = 15000;
	atomic_set(&self->homa.total_incoming, 0);
	self->homa.max_incoming = 30000;
	self->homa.max_sched_prio = 2;
	homa_send_grants(&self->homa);
	EXPECT_SUBSTR("xmit GRANT 11400@1; "
			"xmit GRANT 11400@0; "
			"xmit GRANT 11400@0", unit_log_get());
	EXPECT_EQ(11400, srpc2->msgin.incoming);
	EXPECT_EQ(11400, srpc3->msgin.incoming);
	EXPECT_EQ(11400, srpc4->msgin.incoming);
}
TEST_F(homa_incoming, homa_send_grants__remove_from_grantable)
{
	struct homa_rpc *srpc1, *srpc2, *srpc3;
	srpc1 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 11000, 100);
	srpc2 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+1,
			self->server_ip, self->client_port, 3, 30000, 100);
	srpc3 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 5, 20000, 100);
	atomic_set(&self->homa.total_incoming, 0);
	self->homa.max_incoming = 3000;
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 1, remaining 9600; "
			"request from 196.168.0.1, id 5, remaining 18600; "
			"request from 197.168.0.1, id 3, remaining 28600",
			unit_log_get());

	/* First attempt grants to one message per host. */
	unit_log_clear();
	homa_send_grants(&self->homa);
	EXPECT_STREQ("xmit GRANT 11000@1; xmit GRANT 11400@0", unit_log_get());
	EXPECT_EQ(11000, srpc1->msgin.incoming);
	EXPECT_EQ(11400, srpc2->msgin.incoming);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 5, remaining 18600; "
			"request from 197.168.0.1, id 3, remaining 28600",
			unit_log_get());

	/* Second attempt will now get second message from host. */
	unit_log_clear();
	homa_send_grants(&self->homa);
	EXPECT_STREQ("xmit GRANT 10600@1", unit_log_get());
	EXPECT_EQ(10600, srpc3->msgin.incoming);
}
TEST_F(homa_incoming, homa_send_grants__MAX_GRANTS_exceeded)
{
	mock_max_grants = 3;
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 20000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+1,
			self->server_ip, self->client_port, 32, 30000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+2,
			self->server_ip, self->client_port, 5, 40000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+3,
			self->server_ip, self->client_port, 7, 50000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+4,
			self->server_ip, self->client_port, 9, 60000, 100);
	atomic_set(&self->homa.total_incoming, 0);
	self->homa.max_incoming = 10000;
	unit_log_clear();
	homa_send_grants(&self->homa);
	EXPECT_STREQ("xmit GRANT 11400@3; xmit GRANT 11400@2; "
			"xmit GRANT 11400@1", unit_log_get());
}
TEST_F(homa_incoming, homa_send_grants__grant_fifo)
{
	struct homa_rpc *srpc1, *srpc2;
	self->homa.fifo_grant_increment = 5000;
	self->homa.grant_fifo_fraction = 100;
	self->homa.grant_nonfifo_left = 6000;
	self->homa.grant_nonfifo = 10000;
	self->homa.max_overcommit = 1;
	self->homa.max_incoming = 10000;
	mock_cycles = ~0;
	srpc1 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 30000, 100);
	ASSERT_NE(NULL, srpc1);
	EXPECT_EQ(10000, srpc1->msgin.incoming);
	srpc2 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+1,
			self->server_ip, self->client_port, 1, 20000, 100);
	ASSERT_NE(NULL, srpc2);
	srpc2->msgin.incoming = 9000;
	atomic_set(&self->homa.total_incoming, 7600);

	/* First call: not time for FIFO grants yet. */
	unit_log_clear();
	homa_send_grants(&self->homa);
	EXPECT_STREQ("xmit GRANT 11400@1", unit_log_get());
	EXPECT_EQ(11400, srpc2->msgin.incoming);
	EXPECT_EQ(3600, self->homa.grant_nonfifo_left);
	EXPECT_EQ(10000, atomic_read(&self->homa.total_incoming));

	/* Second call: time for a FIFO grant. */
	unit_log_clear();
	srpc2->msgin.incoming = 5000;
	atomic_set(&self->homa.total_incoming, 5400);
	homa_send_grants(&self->homa);
	EXPECT_STREQ("xmit GRANT 15000@3; xmit GRANT 9600@1", unit_log_get());
	EXPECT_EQ(15000, srpc1->msgin.incoming);
	EXPECT_EQ(9600, srpc2->msgin.incoming);
	EXPECT_EQ(9000, self->homa.grant_nonfifo_left);
	EXPECT_EQ(15000, atomic_read(&self->homa.total_incoming));

	/* Third call: time for a FIFO grant, but FIFO fraction is zero. */
	unit_log_clear();
	srpc1->msgin.incoming = 5000;
	srpc2->msgin.incoming = 5000;
	atomic_set(&self->homa.total_incoming, 8000);
	self->homa.grant_nonfifo_left = 1000;
	self->homa.grant_fifo_fraction = 0;
	homa_send_grants(&self->homa);
	EXPECT_STREQ("xmit GRANT 7000@1", unit_log_get());
	EXPECT_EQ(7000, srpc2->msgin.incoming);
	EXPECT_EQ(9000, self->homa.grant_nonfifo_left);
}
TEST_F(homa_incoming, homa_send_grants__dont_grant_fifo_no_inactive_rpcs)
{
	struct homa_rpc *srpc1, *srpc2;
	self->homa.rtt_bytes = 10000;
	self->homa.fifo_grant_increment = 5000;
	self->homa.max_sched_prio = 3;
	self->homa.grant_fifo_fraction = 100;
	self->homa.grant_nonfifo_left = 1000;
	self->homa.grant_nonfifo = 10000;
	self->homa.max_overcommit = 2;
	self->homa.max_incoming = 10000;
	mock_cycles = ~0;
	srpc1 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 30000, 100);
	ASSERT_NE(NULL, srpc1);
	srpc1->msgin.incoming = 10000;
	srpc2 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+1,
			self->server_ip, self->client_port, 1, 20000, 100);
	ASSERT_NE(NULL, srpc2);
	srpc2->msgin.incoming = 9000;
	atomic_set(&self->homa.total_incoming, 8000);

	unit_log_clear();
	homa_send_grants(&self->homa);
	EXPECT_STREQ("xmit GRANT 11000@1", unit_log_get());
	EXPECT_EQ(10000, srpc1->msgin.incoming);
	EXPECT_EQ(11000, srpc2->msgin.incoming);
	EXPECT_EQ(9000, self->homa.grant_nonfifo_left);
}

TEST_F(homa_incoming, homa_grant_fifo__basics)
{
	struct homa_rpc *srpc;
	self->homa.rtt_bytes = 10000;
	self->homa.fifo_grant_increment = 5000;
	self->homa.max_sched_prio = 2;
	mock_cycles = ~0;
	srpc = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 40000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+1,
			self->server_ip, self->client_port, 3, 30000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 5, 20000, 100);
	ASSERT_NE(NULL, srpc);
	EXPECT_EQ(10000, srpc->msgin.incoming);

	unit_log_clear();
	EXPECT_EQ(5000, homa_grant_fifo(&self->homa));
	EXPECT_STREQ("xmit GRANT 15000@2", unit_log_get());
	EXPECT_EQ(15000, srpc->msgin.incoming);
	EXPECT_EQ(1, homa_cores[cpu_number]->metrics.fifo_grants);
	EXPECT_EQ(0, homa_cores[cpu_number]->metrics.fifo_grants_no_incoming);
}
TEST_F(homa_incoming, homa_grant_fifo__pity_grant_still_active)
{
	struct homa_rpc *srpc1, *srpc2;
	self->homa.rtt_bytes = 10000;
	self->homa.fifo_grant_increment = 5000;
	self->homa.max_sched_prio = 2;
	mock_cycles = ~0;
	srpc1 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 40000, 100);
	srpc2 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+1,
			self->server_ip, self->client_port, 3, 30000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 5, 20000, 100);
	ASSERT_NE(NULL, srpc1);
	ASSERT_NE(NULL, srpc2);
	srpc1->msgin.incoming = 16400;

	unit_log_clear();
	EXPECT_EQ(5000, homa_grant_fifo(&self->homa));
	EXPECT_STREQ("xmit GRANT 15000@2", unit_log_get());
	EXPECT_EQ(16400, srpc1->msgin.incoming);
	EXPECT_EQ(15000, srpc2->msgin.incoming);
}
TEST_F(homa_incoming, homa_grant_fifo__no_good_candidates)
{
	struct homa_rpc *srpc1;
	self->homa.rtt_bytes = 10000;
	self->homa.fifo_grant_increment = 5000;
	self->homa.max_sched_prio = 2;
	mock_cycles = ~0;
	srpc1 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 40000, 100);
	ASSERT_NE(NULL, srpc1);
	srpc1->msgin.incoming = 16400;

	unit_log_clear();
	EXPECT_EQ(0, homa_grant_fifo(&self->homa));
	EXPECT_STREQ("", unit_log_get());
	EXPECT_EQ(16400, srpc1->msgin.incoming);
}
TEST_F(homa_incoming, homa_grant_fifo__increment_fifo_grants_no_incoming)
{
	struct homa_rpc *srpc1;
	self->homa.rtt_bytes = 10000;
	self->homa.fifo_grant_increment = 5000;
	self->homa.max_sched_prio = 2;
	mock_cycles = ~0;
	srpc1 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 40000, 100);
	ASSERT_NE(NULL, srpc1);
	srpc1->msgin.incoming = 1400;

	unit_log_clear();
	EXPECT_EQ(5000, homa_grant_fifo(&self->homa));
	EXPECT_STREQ("xmit GRANT 6400@2", unit_log_get());
	EXPECT_EQ(6400, srpc1->msgin.incoming);
	EXPECT_EQ(1, homa_cores[cpu_number]->metrics.fifo_grants_no_incoming);
}
TEST_F(homa_incoming, homa_grant_fifo__remove_from_grantable)
{
	struct homa_rpc *srpc1;
	self->homa.rtt_bytes = 10000;
	self->homa.fifo_grant_increment = 5000;
	self->homa.max_sched_prio = 2;
	mock_cycles = ~0;
	srpc1 = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 14000, 100);
	ASSERT_NE(NULL, srpc1);

	unit_log_clear();
	EXPECT_EQ(4000, homa_grant_fifo(&self->homa));
	EXPECT_STREQ("xmit GRANT 14000@2", unit_log_get());
	EXPECT_EQ(14000, srpc1->msgin.incoming);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("", unit_log_get());
}

TEST_F(homa_incoming, homa_remove_grantable_locked__basics)
{
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT,
			self->client_ip, self->server_ip, self->client_port,
			1, 20000, 100);
	ASSERT_NE(NULL, srpc);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 1, remaining 18600",
			unit_log_get());

	/* First time: on the list. */
	homa_remove_grantable_locked(&self->homa, srpc);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("", unit_log_get());
	EXPECT_EQ(0, self->homa.num_grantable_peers);

	/* Second time: not on the list. */
	homa_remove_grantable_locked(&self->homa, srpc);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("", unit_log_get());
	EXPECT_EQ(0, self->homa.num_grantable_peers);
};
TEST_F(homa_incoming, homa_remove_grantable_locked__not_head_of_peer_list)
{
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 20000, 100);
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT,
			self->client_ip, self->server_ip, self->client_port,
			3, 50000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+1,
			self->server_ip, self->client_port, 5, 30000, 100);
	ASSERT_NE(NULL, srpc);
	homa_remove_grantable_locked(&self->homa, srpc);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 1, remaining 18600; "
			"request from 197.168.0.1, id 5, remaining 28600",
			unit_log_get());
	EXPECT_EQ(2, self->homa.num_grantable_peers);
}
TEST_F(homa_incoming, homa_remove_grantable_locked__remove_peer_from_homa_list)
{
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 20000, 100);
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT,
			self->client_ip+1, self->server_ip, self->client_port,
			3, 30000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+2,
			self->server_ip, self->client_port, 5, 40000, 100);
	ASSERT_NE(NULL, srpc);
	homa_remove_grantable_locked(&self->homa, srpc);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 1, remaining 18600; "
			"request from 198.168.0.1, id 5, remaining 38600",
			unit_log_get());
	EXPECT_EQ(2, self->homa.num_grantable_peers);
}
TEST_F(homa_incoming, homa_remove_grantable_locked__peer_moves_down)
{
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT,
			self->client_ip, self->server_ip, self->client_port,
			1, 20000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 3, 40000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+1,
			self->server_ip, self->client_port, 5, 30000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip+2,
			self->server_ip, self->client_port, 7, 40000, 100);
	ASSERT_NE(NULL, srpc);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 1, remaining 18600; "
			"request from 196.168.0.1, id 3, remaining 38600; "
			"request from 197.168.0.1, id 5, remaining 28600; "
			"request from 198.168.0.1, id 7, remaining 38600",
			unit_log_get());
	EXPECT_EQ(3, self->homa.num_grantable_peers);

	homa_remove_grantable_locked(&self->homa, srpc);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 197.168.0.1, id 5, remaining 28600; "
			"request from 198.168.0.1, id 7, remaining 38600; "
			"request from 196.168.0.1, id 3, remaining 38600",
			unit_log_get());
	EXPECT_EQ(3, self->homa.num_grantable_peers);
}

TEST_F(homa_incoming, homa_remove_from_grantable__basics)
{
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 20000, 100);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("request from 196.168.0.1, id 1, remaining 18600",
			unit_log_get());
	struct homa_rpc *srpc = homa_find_server_rpc(&self->hsk,
			self->client_ip, self->client_port, 1);
	ASSERT_NE(NULL, srpc);
	homa_rpc_unlock(srpc);

	/* First time: on the list. */
	homa_remove_from_grantable(&self->homa, srpc);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("", unit_log_get());

	/* Second time: not on the list (make sure it doesn't attempt to
	 * acquire homa_grantable_lock). */
	homa_grantable_lock(&self->homa);
	homa_remove_from_grantable(&self->homa, srpc);
	homa_grantable_unlock(&self->homa);
	unit_log_clear();
	unit_log_grantables(&self->homa);
	EXPECT_STREQ("", unit_log_get());
}
TEST_F(homa_incoming, homa_remove_from_grantable__grant_to_other_message)
{
	self->homa.max_overcommit = 1;
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 1, 20000, 100);
	unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT, self->client_ip,
			self->server_ip, self->client_port, 3, 30000, 100);

	struct homa_rpc *srpc = homa_find_server_rpc(&self->hsk,
			self->client_ip, self->client_port, 1);
	ASSERT_NE(NULL, srpc);
	homa_rpc_unlock(srpc);
	homa_send_grants(&self->homa);
	unit_log_clear();

	mock_xmit_log_verbose = 1;
	homa_rpc_free(srpc);
	EXPECT_SUBSTR("xmit GRANT", unit_log_get());
	EXPECT_SUBSTR("id 3,", unit_log_get());
}

TEST_F(homa_incoming, homa_rpc_abort__basics)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);
	unit_log_clear();
	homa_rpc_abort(crpc, -EFAULT);
	EXPECT_EQ(1, unit_list_length(&self->hsk.ready_responses));
	EXPECT_EQ(0, list_empty(&crpc->ready_links));
	EXPECT_EQ(EFAULT, -crpc->error);
	EXPECT_STREQ("homa_remove_from_grantable invoked; "
			"sk->sk_data_ready invoked", unit_log_get());
}
TEST_F(homa_incoming, homa_rpc_abort__socket_shutdown)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);
	unit_log_clear();
	self->hsk.shutdown = 1;
	homa_rpc_abort(crpc, -EFAULT);
	EXPECT_EQ(RPC_OUTGOING, crpc->state);
	EXPECT_EQ(EFAULT, -crpc->error);
	EXPECT_STREQ("homa_remove_from_grantable invoked", unit_log_get());
	self->hsk.shutdown = 0;
}

TEST_F(homa_incoming, homa_abort_rpcs__basics)
{
	struct homa_rpc *crpc1 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 5000, 1600);
	struct homa_rpc *crpc2 = unit_client_rpc(&self->hsk,
			UNIT_RCVD_MSG, self->client_ip, self->server_ip,
			self->server_port, self->client_id+2, 5000, 1600);
	struct homa_rpc *crpc3 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip+1,
			self->server_port, self->client_id+4, 5000, 1600);
	ASSERT_NE(NULL, crpc1);
	ASSERT_NE(NULL, crpc2);
	ASSERT_NE(NULL, crpc3);
	unit_log_clear();
	homa_abort_rpcs(&self->homa, self->server_ip, 0, -EPROTONOSUPPORT);
	EXPECT_EQ(2, unit_list_length(&self->hsk.ready_responses));
	EXPECT_EQ(0, list_empty(&crpc1->ready_links));
	EXPECT_EQ(EPROTONOSUPPORT, -crpc1->error);
	EXPECT_EQ(0, list_empty(&crpc2->ready_links));
	EXPECT_EQ(EPROTONOSUPPORT, -crpc2->error);
	EXPECT_EQ(RPC_OUTGOING, crpc3->state);
}
TEST_F(homa_incoming, homa_abort_rpcs__multiple_sockets)
{
	struct homa_sock hsk1, hsk2;
	struct homa_rpc *crpc1 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 5000, 1600);
	struct homa_rpc *crpc2, *crpc3;
	mock_sock_init(&hsk1, &self->homa, self->server_port);
	mock_sock_init(&hsk2, &self->homa, self->server_port+1);
	crpc2 = unit_client_rpc(&hsk1, UNIT_OUTGOING, self->client_ip,
			self->server_ip, self->server_port, self->client_id+2,
			5000, 1600);
	crpc3 = unit_client_rpc(&hsk1, UNIT_OUTGOING, self->client_ip,
			self->server_ip, self->server_port, self->client_id+4,
			5000, 1600);
	ASSERT_NE(NULL, crpc1);
	ASSERT_NE(NULL, crpc2);
	ASSERT_NE(NULL, crpc3);
	unit_log_clear();
	homa_abort_rpcs(&self->homa, self->server_ip, 0, -EPROTONOSUPPORT);
	EXPECT_EQ(1, unit_list_length(&self->hsk.ready_responses));
	EXPECT_EQ(0, list_empty(&crpc1->ready_links));
	EXPECT_EQ(EPROTONOSUPPORT, -crpc1->error);
	EXPECT_EQ(0, list_empty(&crpc2->ready_links));
	EXPECT_EQ(EPROTONOSUPPORT, -crpc2->error);
	EXPECT_EQ(0, list_empty(&crpc3->ready_links));
	EXPECT_EQ(2, unit_list_length(&hsk1.active_rpcs));
	EXPECT_EQ(2, unit_list_length(&hsk1.ready_responses));
	homa_sock_shutdown(&hsk1);
	homa_sock_shutdown(&hsk2);
}
TEST_F(homa_incoming, homa_abort_rpcs__select_addr)
{
	struct homa_rpc *crpc1 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 5000, 1600);
	struct homa_rpc *crpc2 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip+1,
			self->server_port, self->client_id+2, 5000, 1600);
	struct homa_rpc *crpc3 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip+2,
			self->server_port, self->client_id+4, 5000, 1600);
	ASSERT_NE(NULL, crpc1);
	ASSERT_NE(NULL, crpc2);
	ASSERT_NE(NULL, crpc3);
	unit_log_clear();
	homa_abort_rpcs(&self->homa, self->server_ip, self->server_port,
			-ENOTCONN);
	EXPECT_EQ(1, unit_list_length(&self->hsk.ready_responses));
	EXPECT_EQ(0, list_empty(&crpc1->ready_links));
	EXPECT_EQ(RPC_OUTGOING, crpc2->state);
	EXPECT_EQ(RPC_OUTGOING, crpc3->state);
}
TEST_F(homa_incoming, homa_abort_rpcs__select_port)
{
	struct homa_rpc *crpc1 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 5000, 1600);
	struct homa_rpc *crpc2 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port+1, self->client_id+2, 5000, 1600);
	struct homa_rpc *crpc3 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id+4, 5000, 1600);
	ASSERT_NE(NULL, crpc1);
	ASSERT_NE(NULL, crpc2);
	ASSERT_NE(NULL, crpc3);
	unit_log_clear();
	homa_abort_rpcs(&self->homa, self->server_ip, self->server_port,
			-ENOTCONN);
	EXPECT_EQ(2, unit_list_length(&self->hsk.ready_responses));
	EXPECT_EQ(0, list_empty(&crpc1->ready_links));
	EXPECT_EQ(ENOTCONN, -crpc1->error);
	EXPECT_EQ(RPC_OUTGOING, crpc2->state);
	EXPECT_EQ(0, list_empty(&crpc1->ready_links));
	EXPECT_EQ(ENOTCONN, -crpc3->error);
}
TEST_F(homa_incoming, homa_abort_rpcs__any_port)
{
	struct homa_rpc *crpc1 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 5000, 1600);
	struct homa_rpc *crpc2 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port+1, self->client_id+2, 5000, 1600);
	struct homa_rpc *crpc3 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id+4, 5000, 1600);
	ASSERT_NE(NULL, crpc1);
	ASSERT_NE(NULL, crpc2);
	ASSERT_NE(NULL, crpc3);
	unit_log_clear();
	homa_abort_rpcs(&self->homa, self->server_ip, 0, -ENOTCONN);
	EXPECT_EQ(0, list_empty(&crpc1->ready_links));
	EXPECT_EQ(0, list_empty(&crpc2->ready_links));
	EXPECT_EQ(0, list_empty(&crpc3->ready_links));
}
TEST_F(homa_incoming, homa_abort_rpcs__ignore_dead_rpcs)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 5000, 1600);
	ASSERT_NE(NULL, crpc);
	homa_rpc_free(crpc);
	EXPECT_EQ(RPC_DEAD, crpc->state);
	unit_log_clear();
	homa_abort_rpcs(&self->homa, self->server_ip, 0, -ENOTCONN);
	EXPECT_EQ(0, crpc->error);
}
TEST_F(homa_incoming, homa_abort_rpcs__free_server_rpc)
{
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_RCVD_MSG,
			self->client_ip, self->server_ip, self->client_port,
		        self->server_id, 20000, 100);
	ASSERT_NE(NULL, srpc);
	unit_log_clear();
	homa_abort_rpcs(&self->homa, self->client_ip, 0, 0);
	EXPECT_EQ(RPC_DEAD, srpc->state);
}

TEST_F(homa_incoming, homa_abort_sock_rpcs__basics)
{
	struct homa_rpc *crpc1 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 5000, 1600);
	struct homa_rpc *crpc2 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port+1, self->client_id+2, 5000, 1600);
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_RCVD_ONE_PKT,
			self->client_ip, self->server_ip, self->client_port,
		        self->server_id, 20000, 100);
	ASSERT_NE(NULL, crpc1);
	ASSERT_NE(NULL, crpc2);
	ASSERT_NE(NULL, srpc);
	unit_log_clear();
	homa_abort_sock_rpcs(&self->hsk, -ENOTCONN);
	EXPECT_EQ(0, list_empty(&crpc1->ready_links));
	EXPECT_EQ(-ENOTCONN, crpc1->error);
	EXPECT_EQ(0, list_empty(&crpc2->ready_links));
	EXPECT_EQ(-ENOTCONN, crpc2->error);
	EXPECT_EQ(RPC_INCOMING, srpc->state);
}
TEST_F(homa_incoming, homa_abort_sock_rpcs__socket_shutdown)
{
	struct homa_rpc *crpc1 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 5000, 1600);
	ASSERT_NE(NULL, crpc1);
	unit_log_clear();
	self->hsk.shutdown = 1;
	homa_abort_sock_rpcs(&self->hsk, -ENOTCONN);
	self->hsk.shutdown = 0;
	EXPECT_EQ(RPC_OUTGOING, crpc1->state);
}
TEST_F(homa_incoming, homa_abort_sock_rpcs__rpc_already_dead)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 5000, 1600);
	ASSERT_NE(NULL, crpc);
	homa_rpc_free(crpc);
	EXPECT_EQ(RPC_DEAD, crpc->state);
	unit_log_clear();
	homa_abort_sock_rpcs(&self->hsk, -ENOTCONN);
	EXPECT_EQ(0, crpc->error);
}
TEST_F(homa_incoming, homa_abort_sock_rpcs__free_rpcs)
{
	struct homa_rpc *crpc1 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 5000, 1600);
	struct homa_rpc *crpc2 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port+1, self->client_id+2, 5000, 1600);
	ASSERT_NE(NULL, crpc1);
	ASSERT_NE(NULL, crpc2);
	unit_log_clear();
	homa_abort_sock_rpcs(&self->hsk, 0);
	EXPECT_EQ(RPC_DEAD, crpc1->state);
	EXPECT_EQ(RPC_DEAD, crpc2->state);
	EXPECT_EQ(0, unit_list_length(&self->hsk.active_rpcs));
}

TEST_F(homa_incoming, homa_register_interests__id_not_for_client_rpc)
{
	int result;
	result = homa_register_interests(&self->interest, &self->hsk,
			HOMA_RECVMSG_RESPONSE, 45);
	EXPECT_EQ(EINVAL, -result);
}
TEST_F(homa_incoming, homa_register_interests__no_rpc_for_id)
{
	int result;
	result = homa_register_interests(&self->interest, &self->hsk,
			HOMA_RECVMSG_RESPONSE, 44);
	EXPECT_EQ(EINVAL, -result);
}
TEST_F(homa_incoming, homa_register_interests__id_already_has_interest)
{
	struct homa_interest interest;
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_RCVD_MSG, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);

	crpc->interest = &interest;
	int result = homa_register_interests(&self->interest, &self->hsk,
			HOMA_RECVMSG_RESPONSE, self->client_id);
	EXPECT_EQ(EINVAL, -result);
	crpc->interest = NULL;
}
TEST_F(homa_incoming, homa_register_interests__return_response_by_id)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_RCVD_MSG, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);

	int result = homa_register_interests(&self->interest, &self->hsk,
			0, self->client_id);
	EXPECT_EQ(0, result);
	EXPECT_EQ(crpc, (struct homa_rpc *)
			atomic_long_read(&self->interest.ready_rpc));
	homa_rpc_unlock(crpc);
}
TEST_F(homa_incoming, homa_register_interests__socket_shutdown)
{
	int result;
	self->hsk.shutdown = 1;
	result = homa_register_interests(&self->interest, &self->hsk,
			HOMA_RECVMSG_RESPONSE, 0);
	EXPECT_EQ(ESHUTDOWN, -result);
	self->hsk.shutdown = 0;
}
TEST_F(homa_incoming, homa_register_interests__specified_id_has_packets)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_RCVD_MSG, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);

	int result = homa_register_interests(&self->interest, &self->hsk,
			HOMA_RECVMSG_REQUEST, crpc->id);
	EXPECT_EQ(0, result);
	EXPECT_EQ(crpc, (struct homa_rpc *)
			atomic_long_read(&self->interest.ready_rpc));
	homa_rpc_unlock(crpc);
}
TEST_F(homa_incoming, homa_register_interests__specified_id_has_error)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);
	crpc->error = -EFAULT;

	int result = homa_register_interests(&self->interest, &self->hsk,
			HOMA_RECVMSG_REQUEST|HOMA_RECVMSG_NONBLOCKING, crpc->id);
	EXPECT_EQ(0, result);
	EXPECT_EQ(crpc, (struct homa_rpc *)
			atomic_long_read(&self->interest.ready_rpc));
	homa_rpc_unlock(crpc);
}
TEST_F(homa_incoming, homa_register_interests__specified_id_not_ready)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);

	int result = homa_register_interests(&self->interest, &self->hsk,
			HOMA_RECVMSG_REQUEST, crpc->id);
	EXPECT_EQ(0, result);
	EXPECT_EQ(NULL, (struct homa_rpc *)
			atomic_long_read(&self->interest.ready_rpc));
}
TEST_F(homa_incoming, homa_register_interests__return_queued_response)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_RCVD_MSG, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);

	int result = homa_register_interests(&self->interest, &self->hsk,
			HOMA_RECVMSG_REQUEST|HOMA_RECVMSG_RESPONSE, 0);
	EXPECT_EQ(0, result);
	EXPECT_EQ(crpc, (struct homa_rpc *)
			atomic_long_read(&self->interest.ready_rpc));
	EXPECT_EQ(LIST_POISON1, self->interest.request_links.next);
	EXPECT_EQ(LIST_POISON1, self->interest.response_links.next);
	homa_rpc_unlock(crpc);
}
TEST_F(homa_incoming, homa_register_interests__return_queued_request)
{
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_RCVD_MSG,
			self->client_ip, self->server_ip, self->client_port,
		        1, 20000, 100);
	ASSERT_NE(NULL, srpc);

	int result = homa_register_interests(&self->interest, &self->hsk,
			HOMA_RECVMSG_REQUEST|HOMA_RECVMSG_RESPONSE, 0);
	EXPECT_EQ(0, result);
	EXPECT_EQ(srpc, (struct homa_rpc *)
			atomic_long_read(&self->interest.ready_rpc));
	EXPECT_EQ(LIST_POISON1, self->interest.request_links.next);
	EXPECT_EQ(LIST_POISON1, self->interest.response_links.next);
	homa_rpc_unlock(srpc);
}
TEST_F(homa_incoming, homa_register_interests__call_sk_data_ready)
{
	struct homa_rpc *srpc1 = unit_server_rpc(&self->hsk, UNIT_RCVD_MSG,
			self->client_ip, self->server_ip, self->client_port,
		        self->server_id, 20000, 100);
	struct homa_rpc *srpc2 = unit_server_rpc(&self->hsk, UNIT_RCVD_MSG,
			self->client_ip, self->server_ip, self->client_port,
		        self->server_id+2, 20000, 100);

	// First time should call sk_data_ready (for 2nd RPC).
	unit_log_clear();
	int result = homa_register_interests(&self->interest, &self->hsk,
			HOMA_RECVMSG_REQUEST|HOMA_RECVMSG_RESPONSE, 0);
	EXPECT_EQ(0, result);
	EXPECT_EQ(srpc1, (struct homa_rpc *)
			atomic_long_read(&self->interest.ready_rpc));
	EXPECT_STREQ("sk->sk_data_ready invoked", unit_log_get());
	homa_rpc_unlock(srpc1);

	// Second time shouldn't call sk_data_ready (no more RPCs).
	unit_log_clear();
	result = homa_register_interests(&self->interest, &self->hsk,
			HOMA_RECVMSG_REQUEST|HOMA_RECVMSG_RESPONSE
			|HOMA_RECVMSG_NONBLOCKING, 0);
	EXPECT_EQ(0, result);
	EXPECT_EQ(srpc2, (struct homa_rpc *)
			atomic_long_read(&self->interest.ready_rpc));
	EXPECT_STREQ("", unit_log_get());
	homa_rpc_unlock(srpc2);
}

TEST_F(homa_incoming, homa_wait_for_message__rpc_from_register_interests)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_RCVD_MSG, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);

	struct homa_rpc *rpc = homa_wait_for_message(&self->hsk,
			HOMA_RECVMSG_RESPONSE|HOMA_RECVMSG_NONBLOCKING,
			self->client_id);
	EXPECT_EQ(crpc, rpc);
	homa_rpc_unlock(crpc);
}
TEST_F(homa_incoming, homa_wait_for_message__error_from_register_interests)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_RCVD_ONE_PKT, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);

	self->hsk.shutdown = 1;
	struct homa_rpc *rpc = homa_wait_for_message(&self->hsk,
			HOMA_RECVMSG_RESPONSE|HOMA_RECVMSG_NONBLOCKING,
			self->client_id);
	EXPECT_EQ(ESHUTDOWN, -PTR_ERR(rpc));
	self->hsk.shutdown = 0;
}
TEST_F(homa_incoming, homa_wait_for_message__rpc_arrives_while_polling)
{
	struct homa_rpc *rpc;
	struct homa_rpc *crpc1 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc1);

	hook_rpc = crpc1;
	poll_count = 5;
	self->homa.poll_cycles = 1000000;
	unit_hook_register(poll_hook);
	unit_log_clear();
	rpc = homa_wait_for_message(&self->hsk, 0, self->client_id);
	EXPECT_EQ(crpc1, rpc);
	EXPECT_EQ(NULL, crpc1->interest);
	EXPECT_STREQ("wake_up_process pid 0", unit_log_get());
	EXPECT_EQ(0, self->hsk.dead_skbs);
	homa_rpc_unlock(rpc);
}
TEST_F(homa_incoming, homa_wait_for_message__nothing_ready_nonblocking)
{
	struct homa_rpc *rpc;
	struct homa_rpc *crpc1 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	unit_client_rpc(&self->hsk, UNIT_RCVD_MSG, self->client_ip, self->server_ip,
			self->server_port, self->client_id+2, 20000, 1600);
	ASSERT_NE(NULL, crpc1);

	rpc = homa_wait_for_message(&self->hsk, HOMA_RECVMSG_NONBLOCKING,
			self->client_id);
	EXPECT_EQ(EAGAIN, -PTR_ERR(rpc));
}
TEST_F(homa_incoming, homa_wait_for_message__rpc_arrives_while_sleeping)
{
	struct homa_rpc *rpc;
	struct homa_rpc *crpc1 = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc1);

        /* Also, check to see that reaping occurs before sleeping. */
	struct homa_rpc *crpc2 = unit_client_rpc(&self->hsk,
			UNIT_RCVD_MSG, self->client_ip, self->server_ip,
			self->server_port, self->client_id+2, 20000, 20000);
	self->homa.reap_limit = 5;
	homa_rpc_free(crpc2);
	EXPECT_EQ(30, self->hsk.dead_skbs);
	unit_log_clear();

	hook_rpc = crpc1;
	unit_hook_register(handoff_hook);
	rpc = homa_wait_for_message(&self->hsk, 0, self->client_id);
	EXPECT_EQ(crpc1, rpc);
	EXPECT_EQ(NULL, crpc1->interest);
	EXPECT_STREQ("reaped 1236; wake_up_process pid 0; 0 in ready_requests, "
			"0 in ready_responses, 0 in request_interests, "
			"0 in response_interests", unit_log_get());
	EXPECT_EQ(0, self->hsk.dead_skbs);
	homa_rpc_unlock(rpc);
}
TEST_F(homa_incoming, homa_wait_for_message__rpc_arrives_after_giving_up)
{
	struct homa_rpc *rpc;
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);

	hook_rpc = crpc;
	unit_hook_register(handoff_hook2);
	unit_log_clear();
	rpc = homa_wait_for_message(&self->hsk,
			HOMA_RECVMSG_NONBLOCKING|HOMA_RECVMSG_RESPONSE, 0);
	ASSERT_EQ(crpc, rpc);
	EXPECT_EQ(NULL, crpc->interest);
	EXPECT_EQ(ETIMEDOUT, -rpc->error);
	homa_rpc_unlock(rpc);
}
TEST_F(homa_incoming, homa_wait_for_message__handoff_rpc_then_delete_after_giving_up)
{
	// A key thing this test does it to ensure that RPC_HANDING_OFF
	// gets cleared even though the RPC has been deleted.
	struct homa_rpc *rpc;
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);

	// Prevent the RPC from being reaped during the test.
	atomic_or(RPC_COPYING_TO_USER, &crpc->flags);

	hook_rpc = crpc;
	hook3_count = 0;
	unit_hook_register(handoff_hook3);
	unit_log_clear();
	rpc = homa_wait_for_message(&self->hsk,
			HOMA_RECVMSG_NONBLOCKING|HOMA_RECVMSG_RESPONSE, 0);
	EXPECT_EQ(EAGAIN, -PTR_ERR(rpc));
	EXPECT_EQ(RPC_COPYING_TO_USER, atomic_read(&crpc->flags));
	EXPECT_EQ(RPC_DEAD, crpc->state);
	atomic_andnot(RPC_COPYING_TO_USER, &crpc->flags);
}
TEST_F(homa_incoming, homa_wait_for_message__explicit_rpc_deleted_while_sleeping)
{
	struct homa_rpc *rpc;
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);
	unit_log_clear();

	hook_rpc = crpc;
	unit_hook_register(delete_hook);
	rpc = homa_wait_for_message(&self->hsk, HOMA_RECVMSG_RESPONSE,
			self->client_id);
	EXPECT_EQ(EINVAL, -PTR_ERR(rpc));
}
TEST_F(homa_incoming, homa_wait_for_message__rpc_deleted_after_matching)
{
	/* Arrange for 2 RPCs to be ready, but delete the first one after
	 * it has matched; this should cause the second one to be matched.
	 */
	struct homa_rpc *rpc;
	struct homa_rpc *crpc1 = unit_client_rpc(&self->hsk,
			UNIT_RCVD_MSG, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc1);
	struct homa_rpc *crpc2 = unit_client_rpc(&self->hsk,
			UNIT_RCVD_MSG, self->client_ip, self->server_ip,
			self->server_port, self->client_id+2, 20000, 1600);
	ASSERT_NE(NULL, crpc2);
	unit_log_clear();

	hook_rpc = crpc1;
	unit_hook_register(match_delete_hook);
	rpc = homa_wait_for_message(&self->hsk,
			HOMA_RECVMSG_RESPONSE|HOMA_RECVMSG_NONBLOCKING, 0);
	EXPECT_EQ(RPC_DEAD, crpc1->state);
	EXPECT_EQ(crpc2, rpc);
	homa_rpc_unlock(rpc);
}
TEST_F(homa_incoming, homa_wait_for_message__socket_shutdown_while_sleeping)
{
	struct homa_rpc *rpc;
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);
	unit_log_clear();

	hook_hsk = &self->hsk;
	unit_hook_register(shutdown_hook);
	rpc = homa_wait_for_message(&self->hsk,
			HOMA_RECVMSG_RESPONSE|HOMA_RECVMSG_REQUEST, 0);
	EXPECT_EQ(ESHUTDOWN, -PTR_ERR(rpc));
}
TEST_F(homa_incoming, homa_wait_for_message__copy_to_user)
{
	struct homa_rpc *rpc;
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_RCVD_ONE_PKT, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);
	EXPECT_EQ(0, -homa_pool_init(&self->hsk.buffer_pool, &self->homa,
			(char *) 0x1000000, 100*HOMA_BPAGE_SIZE));
	mock_copy_to_user_dont_copy = -1;
	unit_log_clear();

	hook_hsk = &self->hsk;
	rpc = homa_wait_for_message(&self->hsk,
			HOMA_RECVMSG_RESPONSE|HOMA_RECVMSG_NONBLOCKING, 0);
	EXPECT_EQ(EAGAIN, -PTR_ERR(rpc));
	EXPECT_EQ(0, atomic_read(&crpc->flags));
	EXPECT_EQ(1400, crpc->msgin.copied_out);
}
TEST_F(homa_incoming, homa_wait_for_message__copy_to_user_fails)
{
	struct homa_rpc *rpc;
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_RCVD_ONE_PKT, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);
	/* We don't set up a buffer pool, so copy_to_user will fail. */
	unit_log_clear();

	hook_hsk = &self->hsk;
	rpc = homa_wait_for_message(&self->hsk,
			HOMA_RECVMSG_RESPONSE|HOMA_RECVMSG_NONBLOCKING, 0);
	ASSERT_FALSE(IS_ERR(rpc));
	EXPECT_EQ(crpc, rpc);
	EXPECT_EQ(RPC_PKTS_READY, atomic_read(&crpc->flags));
	EXPECT_EQ(0, crpc->msgin.copied_out);
	EXPECT_EQ(ENOMEM, -rpc->error);
	homa_rpc_unlock(rpc);
}
TEST_F(homa_incoming, homa_wait_for_message__message_complete)
{
	struct homa_rpc *rpc;
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_RCVD_MSG, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 2000);
	ASSERT_NE(NULL, crpc);
	EXPECT_EQ(0, -homa_pool_init(&self->hsk.buffer_pool, &self->homa,
			(char *) 0x1000000, 100*HOMA_BPAGE_SIZE));
	mock_copy_to_user_dont_copy = -1;
	unit_log_clear();

	hook_hsk = &self->hsk;
	rpc = homa_wait_for_message(&self->hsk,
			HOMA_RECVMSG_RESPONSE|HOMA_RECVMSG_NONBLOCKING, 0);
	ASSERT_FALSE(IS_ERR(rpc));
	EXPECT_EQ(crpc, rpc);
	EXPECT_EQ(0, atomic_read(&crpc->flags));
	EXPECT_EQ(2000, crpc->msgin.copied_out);
	homa_rpc_unlock(rpc);
}
TEST_F(homa_incoming, homa_wait_for_message__signal)
{
	struct homa_rpc *rpc;

	mock_signal_pending = 1;
	rpc = homa_wait_for_message(&self->hsk, HOMA_RECVMSG_REQUEST, 0);
	EXPECT_EQ(EINTR, -PTR_ERR(rpc));
}

TEST_F(homa_incoming, homa_rpc_handoff__handoff_already_in_progress)
{
	struct homa_interest interest;
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);
	EXPECT_EQ(NULL, crpc->interest);
	unit_log_clear();

	homa_interest_init(&interest);
	interest.thread = &mock_task;
	interest.reg_rpc = crpc;
	crpc->interest = &interest;
	atomic_or(RPC_HANDING_OFF, &crpc->flags);
	homa_rpc_handoff(crpc);
	crpc->interest = NULL;
	EXPECT_EQ(NULL, (struct homa_rpc *)
			atomic_long_read(&interest.ready_rpc));
	EXPECT_STREQ("", unit_log_get());
	atomic_andnot(RPC_HANDING_OFF, &crpc->flags);
}
TEST_F(homa_incoming, homa_rpc_handoff__rpc_already_enqueued)
{
	struct homa_interest interest;
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);
	EXPECT_EQ(NULL, crpc->interest);
	unit_log_clear();

	/* First handoff enqueues the RPC. */
	homa_rpc_handoff(crpc);
	EXPECT_FALSE(list_empty(&crpc->ready_links));
	unit_log_clear();

	/* Second handoff does nothing, even though an interest is available. */

	homa_interest_init(&interest);
	interest.thread = &mock_task;
	interest.reg_rpc = crpc;
	crpc->interest = &interest;
	atomic_or(RPC_HANDING_OFF, &crpc->flags);
	homa_rpc_handoff(crpc);
	crpc->interest = NULL;
	EXPECT_EQ(NULL, (struct homa_rpc *)
			atomic_long_read(&interest.ready_rpc));
	EXPECT_STREQ("", unit_log_get());
	atomic_andnot(RPC_HANDING_OFF, &crpc->flags);
}
TEST_F(homa_incoming, homa_rpc_handoff__interest_on_rpc)
{
	struct homa_interest interest;
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);
	EXPECT_EQ(NULL, crpc->interest);
	unit_log_clear();

	homa_interest_init(&interest);
	interest.thread = &mock_task;
	interest.reg_rpc = crpc;
	crpc->interest = &interest;
	homa_rpc_handoff(crpc);
	crpc->interest = NULL;
	EXPECT_EQ(crpc, (struct homa_rpc *)
			atomic_long_read(&interest.ready_rpc));
	EXPECT_EQ(NULL, interest.reg_rpc);
	EXPECT_EQ(NULL, crpc->interest);
	EXPECT_STREQ("wake_up_process pid 0", unit_log_get());
	atomic_andnot(RPC_HANDING_OFF, &crpc->flags);
}
TEST_F(homa_incoming, homa_rpc_handoff__response_interests)
{
	struct homa_interest interest;
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);
	EXPECT_EQ(NULL, crpc->interest);
	unit_log_clear();

	homa_interest_init(&interest);
	interest.thread = &mock_task;
	list_add_tail(&interest.response_links, &self->hsk.response_interests);
	homa_rpc_handoff(crpc);
	EXPECT_EQ(crpc, (struct homa_rpc *)
			atomic_long_read(&interest.ready_rpc));
	EXPECT_EQ(0, unit_list_length(&self->hsk.response_interests));
	EXPECT_STREQ("wake_up_process pid 0", unit_log_get());
	atomic_andnot(RPC_HANDING_OFF, &crpc->flags);
}
TEST_F(homa_incoming, homa_rpc_handoff__queue_on_ready_responses)
{
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);
	unit_log_clear();

	homa_rpc_handoff(crpc);
	EXPECT_STREQ("sk->sk_data_ready invoked", unit_log_get());
	EXPECT_EQ(1, unit_list_length(&self->hsk.ready_responses));
}
TEST_F(homa_incoming, homa_rpc_handoff__request_interests)
{
	struct homa_interest interest;
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_OUTGOING,
			self->client_ip, self->server_ip, self->client_port,
		        self->server_id, 20000, 100);
	ASSERT_NE(NULL, srpc);
	unit_log_clear();

	homa_interest_init(&interest);
	interest.thread = &mock_task;
	list_add_tail(&interest.request_links, &self->hsk.request_interests);
	homa_rpc_handoff(srpc);
	EXPECT_EQ(srpc, (struct homa_rpc *)
			atomic_long_read(&interest.ready_rpc));
	EXPECT_EQ(0, unit_list_length(&self->hsk.request_interests));
	EXPECT_STREQ("wake_up_process pid 0", unit_log_get());
	atomic_andnot(RPC_HANDING_OFF, &srpc->flags);
}
TEST_F(homa_incoming, homa_rpc_handoff__queue_on_ready_requests)
{
	struct homa_rpc *srpc = unit_server_rpc(&self->hsk, UNIT_OUTGOING,
			self->client_ip, self->server_ip, self->client_port,
		        1, 20000, 100);
	ASSERT_NE(NULL, srpc);
	unit_log_clear();

	homa_rpc_handoff(srpc);
	EXPECT_STREQ("sk->sk_data_ready invoked", unit_log_get());
	EXPECT_EQ(1, unit_list_length(&self->hsk.ready_requests));
}
TEST_F(homa_incoming, homa_rpc_handoff__detach_interest)
{
	struct homa_interest interest;
	struct homa_rpc *crpc = unit_client_rpc(&self->hsk,
			UNIT_OUTGOING, self->client_ip, self->server_ip,
			self->server_port, self->client_id, 20000, 1600);
	ASSERT_NE(NULL, crpc);
	EXPECT_EQ(NULL, crpc->interest);
	unit_log_clear();

	homa_interest_init(&interest);
	interest.thread = &mock_task;
	interest.reg_rpc = crpc;
	crpc->interest = &interest;
	list_add_tail(&interest.response_links, &self->hsk.response_interests);
	list_add_tail(&interest.request_links, &self->hsk.request_interests);
	EXPECT_EQ(1, unit_list_length(&self->hsk.response_interests));
	EXPECT_EQ(1, unit_list_length(&self->hsk.request_interests));

	homa_rpc_handoff(crpc);
	crpc->interest = NULL;
	EXPECT_EQ(crpc, (struct homa_rpc *)
			atomic_long_read(&interest.ready_rpc));
	EXPECT_EQ(NULL, interest.reg_rpc);
	EXPECT_EQ(NULL, crpc->interest);
	EXPECT_EQ(0, unit_list_length(&self->hsk.response_interests));
	EXPECT_EQ(0, unit_list_length(&self->hsk.request_interests));
	atomic_andnot(RPC_HANDING_OFF, &crpc->flags);
}

TEST_F(homa_incoming, homa_incoming_sysctl_changed__grant_nonfifo)
{
	cpu_khz = 2000000;
	self->homa.poll_usecs = 40;
	homa_incoming_sysctl_changed(&self->homa);
	EXPECT_EQ(80000, self->homa.poll_cycles);
}
TEST_F(homa_incoming, homa_incoming_sysctl_changed__poll_cycles)
{
	self->homa.fifo_grant_increment = 10000;
	self->homa.grant_fifo_fraction = 0;
	homa_incoming_sysctl_changed(&self->homa);
	EXPECT_EQ(0, self->homa.grant_nonfifo);

	self->homa.grant_fifo_fraction = 100;
	homa_incoming_sysctl_changed(&self->homa);
	EXPECT_EQ(90000, self->homa.grant_nonfifo);

	self->homa.grant_fifo_fraction = 500;
	homa_incoming_sysctl_changed(&self->homa);
	EXPECT_EQ(10000, self->homa.grant_nonfifo);

	self->homa.grant_fifo_fraction = 2000;
	homa_incoming_sysctl_changed(&self->homa);
	EXPECT_EQ(10000, self->homa.grant_nonfifo);
}
