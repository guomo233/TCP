#include "tcp.h"
#include "tcp_sock.h"
#include "tcp_timer.h"

#include "log.h"
#include "ring_buffer.h"

#include "retrans.h" // fix
#include "congestion_control.h" // fix
#include <stdlib.h>
// update the snd_wnd of tcp_sock
//
// if the snd_wnd before updating is zero, notify tcp_sock_send (wait_send)
static inline void tcp_update_window(struct tcp_sock *tsk, struct tcp_cb *cb)
{
	u16 old_snd_wnd = tsk->snd_wnd;
	tsk->snd_wnd = cb->rwnd;
	if (old_snd_wnd == 0)
		wake_up(tsk->wait_send);
}

// update the snd_wnd safely: cb->ack should be between snd_una and snd_nxt
static inline void tcp_update_window_safe(struct tcp_sock *tsk, struct tcp_cb *cb)
{
	if (less_or_equal_32b(tsk->snd_una, cb->ack) && less_or_equal_32b(cb->ack, tsk->snd_nxt))
		tcp_update_window(tsk, cb);
}

#ifndef max
#	define max(x,y) ((x)>(y) ? (x) : (y))
#endif

// check whether the sequence number of the incoming packet is in the receiving
// window
static inline int is_tcp_seq_valid(struct tcp_sock *tsk, struct tcp_cb *cb)
{
	u32 rcv_end = tsk->rcv_nxt + max(tsk->rcv_wnd, 1);
	if (less_than_32b(cb->seq, rcv_end) && less_or_equal_32b(tsk->rcv_nxt, cb->seq_end)) {
		return 1;
	}
	else {
		//log(ERROR, "received packet with invalid seq, drop it.");
		//log(ERROR, "need (%d,%d), drop (%d,%d)", tsk->rcv_nxt, tsk->rcv_nxt + tsk->rcv_wnd, cb->seq, cb->seq_end);
		return 0;
	}
}

struct tcp_cb *fin_cb ;

// Process the incoming packet according to TCP state machine. 
void tcp_process(struct tcp_sock *tsk, struct tcp_cb *cb, char *packet)
{
	// RST
	if (!tsk || tsk->state == TCP_CLOSED)
		tcp_send_reset (cb) ;
	else if (cb->flags == TCP_RST)
	{
		tcp_set_state (tsk, TCP_CLOSED) ;
		
		if (!tsk->parent)
			tcp_bind_unhash (tsk) ;
		tcp_unhash (tsk) ; // auto free memory
	}
		
	// Connect
	if (tsk->state == TCP_LISTEN && cb->flags == TCP_SYN)
	{		
		struct tcp_sock *csk = alloc_tcp_sock () ;
		csk->sk_sip = cb->daddr ;
		csk->sk_sport = cb->dport ;
		csk->sk_dip = cb->saddr ;
		csk->sk_dport = cb->sport ;
		csk->rcv_nxt = cb->seq_end ;
		csk->parent = tsk ;

		list_add_tail (&(csk->list), &(tsk->listen_queue)) ;
		
		tcp_set_state (csk, TCP_SYN_RECV) ;
		tcp_hash (csk) ;
			
		tcp_send_control_packet (csk, TCP_SYN | TCP_ACK) ;
	}
	else if (tsk->state == TCP_SYN_SENT &&
		cb->flags == (TCP_SYN | TCP_ACK) &&
		cb->ack == tsk->snd_nxt)
	{
		tsk->rcv_nxt = cb->seq_end ;
		tsk->snd_una++ ;

		remove_ack_pkt (tsk, cb->ack, TCP_CONTROL_ACK) ;
		
		tcp_update_window_safe (tsk, cb) ; // Update snd_wnd
		tsk->adv_wnd = cb->rwnd ;
		tsk->cwnd = TCP_MSS ;
		tsk->ssthresh = cb->rwnd ;

		tcp_set_state (tsk, TCP_ESTABLISHED) ;
		tcp_send_control_packet (tsk, TCP_ACK) ;
		
		wake_up (tsk->wait_connect) ;
	}
	else if (tsk->state == TCP_ESTABLISHED &&
		cb->flags == (TCP_SYN | TCP_ACK) &&
		cb->ack == tsk->snd_nxt - 1)  // prevent ACK loss
		tcp_send_control_packet (tsk, TCP_ACK) ;
	else if (tsk->state == TCP_SYN_RECV &&
		cb->flags == TCP_ACK &&
		cb->ack == tsk->snd_nxt)
	{
		tsk->snd_una++ ;
		tsk->cwnd = TCP_MSS ;
		tsk->adv_wnd = cb->rwnd ;
		tsk->ssthresh = cb->rwnd ;

		remove_ack_pkt (tsk, cb->ack, TCP_CONTROL_ACK) ;
		
		tcp_sock_accept_enqueue (tsk) ;
		
		tcp_set_state (tsk, TCP_ESTABLISHED) ;
		
		wake_up (tsk->parent->wait_accept) ;
	}
	
	// Close
	if (tsk->state == TCP_ESTABLISHED && 
		cb->flags == TCP_FIN &&
		cb->seq == tsk->rcv_nxt)
	{
		tsk->rcv_nxt = cb->seq_end ;

		tcp_set_state (tsk, TCP_CLOSE_WAIT) ;
		tcp_send_control_packet (tsk, TCP_ACK) ;

		wake_up (tsk->wait_recv) ;
	}
	else if (tsk->state == TCP_FIN_WAIT_1 && 
		cb->flags == TCP_ACK &&
		cb->ack == tsk->snd_nxt)
	{
		tsk->snd_una++ ;
	
		remove_ack_pkt (tsk, cb->ack, TCP_CONTROL_ACK) ;
		tcp_set_state (tsk, TCP_FIN_WAIT_2) ;
	}
	else if ((tsk->state == TCP_FIN_WAIT_2 || 
		tsk->state == TCP_TIME_WAIT) && 
		cb->flags == TCP_FIN &&
		cb->seq == tsk->rcv_nxt)
	{
		tsk->rcv_nxt = cb->seq_end ;
		
		tcp_set_state (tsk, TCP_TIME_WAIT) ;
		tcp_send_control_packet (tsk, TCP_ACK) ;
		
		tsk->rcv_nxt = cb->seq ; // for TIME_WAIT to receive FIN
		
		log(DEBUG, "receive FIN") ;
		tcp_set_timewait_timer (tsk) ;
	}
	else if (tsk->state == TCP_LAST_ACK && 
		cb->flags == TCP_ACK &&
		cb->ack == tsk->snd_nxt)
	{
		tsk->snd_una++ ;

		remove_ack_pkt (tsk, cb->ack, TCP_CONTROL_ACK) ;
		
		tcp_set_state (tsk, TCP_CLOSED) ;

		tcp_unhash (tsk) ;  // auto free memory
	}
	
	// drop
	//if (!is_tcp_seq_valid (tsk, cb))
	//	return ;
	
	// Receive data
	if ((tsk->state == TCP_ESTABLISHED || 
		tsk->state == TCP_FIN_WAIT_1 ||
		tsk->state == TCP_FIN_WAIT_2) &&
		cb->pl_len > 0)
	{
		if (cb->seq == tsk->rcv_nxt && cb->seq_end <= tsk->rcv_nxt + tsk->rcv_wnd)
		{
			int seq = cb->seq ;
			int seq_end = cb->seq_end ;
			int pl_len = cb->pl_len ;
			char *payload = cb->payload ;
			int wait_to_ack = 1 ;
			while (wait_to_ack)
			{
				pthread_mutex_lock (&(tsk->rcv_buf->rw_lock)) ;
				write_ring_buffer (tsk->rcv_buf, payload, pl_len) ;
				tsk->rcv_nxt = seq_end ;
				tsk->rcv_wnd -= pl_len ;
				pthread_mutex_unlock (&(tsk->rcv_buf->rw_lock)) ;

				if (list_empty (&(tsk->rcv_ofo_buf)))
					break ;
				
				wait_to_ack = 0 ;
				struct rcvd_pkt *pkt_bak, *temp ;
				list_for_each_entry_safe (pkt_bak, temp, &(tsk->rcv_ofo_buf), list)
				{
					if (pkt_bak->seq == tsk->rcv_nxt && 
						pkt_bak->seq + pkt_bak->pl_len <= tsk->rcv_nxt + tsk->rcv_wnd)
					{
						pl_len = pkt_bak->pl_len ;
						seq = pkt_bak->seq ;
						seq_end = seq + pl_len ;
						payload = pkt_bak->payload ;
						
						list_delete_entry (&(pkt_bak->list)) ;
						wait_to_ack = 1 ;

						break ;
					}
				}
			}
			if (cb->pl_len > 0)  // prevent zero window probe
				wake_up (tsk->wait_recv) ;
		}
		else if (cb->seq > tsk->rcv_nxt && cb->seq_end <= tsk->rcv_nxt + tsk->rcv_wnd) // store
		{
			struct rcvd_pkt *pkt_bak = (struct rcvd_pkt *) malloc (sizeof(struct rcvd_pkt)) ;
			pkt_bak->pl_len = cb->pl_len ;
			pkt_bak->seq = cb->seq ;
			pkt_bak->payload = (char *) malloc (sizeof(char) * cb->pl_len) ;
			memcpy (pkt_bak->payload, cb->payload, cb->pl_len) ;
			
			list_add_tail (&(pkt_bak->list), &(tsk->rcv_ofo_buf)) ;
		}

		if (tsk->rcv_wnd <= 0)
			log(DEBUG, "rcv_wnd = 0") ;
		log(DEBUG, "send ack:%d, rcv_wnd = %d", tsk->rcv_nxt, tsk->rcv_wnd) ;
		tcp_send_control_packet (tsk, TCP_ACK) ;
	}
	
	// Receive ACK about sent data
	if ((tsk->state == TCP_ESTABLISHED ||
		tsk->state == TCP_FIN_WAIT_1 ||
		tsk->state == TCP_FIN_WAIT_2 ||
		tsk->state == TCP_CLOSE_WAIT) &&
		cb->flags == TCP_ACK)
	{
		if (cb->ack > tsk->snd_una) // receive new ack
		{
			remove_ack_pkt (tsk, cb->ack, TCP_DATA_ACK) ;
			
			if (tsk->cg_state == TCP_CG_RECOVERY && 
				cb->ack < tsk->recovery_point) // patrial ack

			{
				log(DEBUG, "RECOVERY: fast retrans") ;
				retrans_pkt (tsk, 0) ;
			}
			else if (tsk->cg_state != TCP_CG_OPEN) // full ack || cg_state == TCP_CG_DISORDER
			{
				log(DEBUG, "change to OPEN") ;
				tsk->cg_state = TCP_CG_OPEN ;
				tsk->dupack_times = 0 ;
			}
			
			if (tsk->cg_state == TCP_CG_OPEN ||
				tsk->cg_state == TCP_CG_DISORDER)
			{
				if (tsk->cwnd < tsk->ssthresh) // slow start
					tsk->cwnd += TCP_MSS ; 
				else                           // congestion advoidance
					tsk->cwnd += (TCP_MSS * 1.0 / tsk->cwnd) * TCP_MSS ;
			}
		}
		else if (cb->ack == tsk->snd_una)  // receive dup ack
		{
			log(DEBUG, "dupack:%d", cb->ack) ;
			
			if (tsk->cg_state != TCP_CG_LOSS)
				tsk->dupack_times++ ;
			
			if (tsk->dupack_times == 1)  // OPEN
			{
				log(DEBUG, "change to DISORDER") ;
				tsk->cg_state = TCP_CG_DISORDER ;
			}
			else if (tsk->dupack_times == 3)  // OPEN -> RECOVERY
			{
				tsk->cg_state = TCP_CG_RECOVERY ;
				tsk->recovery_point = tsk->snd_nxt ;

				log(DEBUG, "change to RECOVERY: fast retrans ") ;
				retrans_pkt (tsk, 0) ;
				
				tsk->ssthresh = tsk->cwnd / 2 ;
				tsk->cwnd = tsk->ssthresh + 3 * TCP_MSS ;
			}
			else if (tsk->dupack_times > 3) // RECOVERY
			{
				tsk->cwnd += TCP_MSS ;
				log(DEBUG, "RECOVERY: fast retrans") ;
				retrans_pkt (tsk, 0) ;
			}
		}
	
		int old_adv_wnd = tsk->adv_wnd ;
		tsk->adv_wnd = cb->rwnd ;
		if (old_adv_wnd <= 0 && tsk->adv_wnd > 0)
		{
			tcp_unset_zwp_timer (tsk) ;
			log(DEBUG, "unset zwp timer") ;

			wake_up (tsk->wait_send) ;
		}
	}
	
	//fprintf(stdout, "TODO: implement %s please.\n", __FUNCTION__);
}
