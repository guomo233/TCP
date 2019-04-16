// fix - retrans

#ifndef __RETRANSE_H__
#define __RETRANSE_H__

#include "tcp_sock.h"
#include "list.h"
#include "tcp_timer.h"
#include "ip.h"
#include "log.h"

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

static inline void remove_ack_pkt (struct tcp_sock *tsk, int ack)
{
	tcp_unset_retrans_timer (tsk);
	struct snt_pkt *pkt, *temp ;
	list_for_each_entry_safe (pkt, temp, &(tsk->send_buf), list)
	{
		struct tcphdr *tcp = packet_to_tcp_hdr (pkt->packet) ;
		struct iphdr *ip = packet_to_ip_hdr (pkt->packet) ;
		int pl_len = ntohs(ip->tot_len) - IP_BASE_HDR_SIZE - TCP_BASE_HDR_SIZE ;
		int seq_end = ntohl(tcp->seq) + pl_len ;
		if (ack >= seq_end)
		{
			tsk->snd_wnd += pl_len ;
			//log(DEBUG, "ack:%d", ntohl(tcp->seq));
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
