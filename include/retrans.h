// fix - retrans

#ifndef __RETRANSE_H__
#define __RETRANSE_H__

#include "tcp_sock.h"
#include "list.h"
#include "tcp_timer.h"
#include "ip.h"
#include "log.h"

#define TCP_CONTROL_ACK 0
#define TCP_DATA_ACK 1

struct snt_pkt
{
	struct list_head list ;
	char *packet ;
	int len ;
	int retrans_times ;
} ;

struct rcvd_pkt
{
	struct list_head list ;
	char *payload ;
	u32 seq ;
	int pl_len ;	
} ;

static inline void retrans_pkt (struct tcp_sock *tsk)
{
	struct snt_pkt *pkt_bak = list_entry (tsk->send_buf.next, struct snt_pkt, list) ;
	char *packet = (char *) malloc (sizeof(char) * pkt_bak->len) ;
	memcpy (packet, pkt_bak->packet, pkt_bak->len) ;

	ip_send_packet (packet, pkt_bak->len) ;
	pkt_bak->retrans_times++ ;

	if (pkt_bak->retrans_times >= 3)
	{
		struct iphdr *ip = packet_to_ip_hdr(packet);
		struct tcphdr *tcp = (struct tcphdr *)((char *)ip + IP_BASE_HDR_SIZE);
		int pl_len = ntohs(ip->tot_len) - IP_HDR_SIZE(ip) - TCP_HDR_SIZE(tcp) ;
		log(DEBUG, "3 times seq:%d, pl_len:%d", ntohl(tcp->seq), pl_len) ;

		tcp_sock_close (tsk) ;
	}
	else
	{
		tsk->retrans_timer.timeout = TCP_RETRANS_INTERVAL_INITIAL ;
		for (int i=0; i<pkt_bak->retrans_times; i++)
			tsk->retrans_timer.timeout *= 2 ;
	}
}

static inline void remove_ack_pkt (struct tcp_sock *tsk, int ack, int ack_type)
{
	tcp_unset_retrans_timer (tsk);
	struct snt_pkt *pkt, *temp ;
	list_for_each_entry_safe (pkt, temp, &(tsk->send_buf), list)
	{
		struct tcphdr *tcp = packet_to_tcp_hdr (pkt->packet) ;
		struct iphdr *ip = packet_to_ip_hdr (pkt->packet) ;
		int pl_len = ntohs(ip->tot_len) - IP_BASE_HDR_SIZE - TCP_BASE_HDR_SIZE ;
		int seq = ntohl(tcp->seq) ;
		int seq_end = seq + pl_len ;
		if ((ack_type == TCP_DATA_ACK && seq_end > seq && ack >= seq_end) ||
			(ack_type == TCP_CONTROL_ACK))
		{
			tsk->snd_wnd += pl_len ;
			//log(DEBUG, "ack:(%d, %d)", seq, seq_end);
			if (tsk->snd_wnd - pl_len <= 0)
				wake_up (tsk->wait_send) ;
			
			free (pkt->packet) ;
			list_delete_entry (&(pkt->list)) ;
		}
	}

	if (!list_empty (&(tsk->send_buf)))
		tcp_set_retrans_timer (tsk) ;
}

#endif
