#include "tcp.h"
#include "tcp_sock.h"
#include "tcp_timer.h"

#include "log.h"
#include "ring_buffer.h"

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
		log(ERROR, "received packet with invalid seq, drop it.");
		return 0;
	}
}

struct tcp_cb *fin_cb ;

// Process the incoming packet according to TCP state machine. 
void tcp_process(struct tcp_sock *tsk, struct tcp_cb *cb, char *packet)
{
	if (!is_tcp_seq_valid (tsk, cb))
		return ;

	// Connect
	if (tsk->state == TCP_LISTEN && cb->flags == TCP_SYN)
	{
		struct tcp_sock *csk = alloc_tcp_sock () ;
		csk->sk_sip = cb->daddr ;
		csk->sport = cb->dport ;
		csk->sk_dip = cb->saddr ;
		csk->dport = cb->sport ;
		csk->rcv_nxt = cb->seq_end ;
		csk->parent = tsk ;

		list_add_tail (&(csk->list), &(tsk->listen_queue)) ;
		
		tcp_set_state (csk, TCP_SYN_RECV) ;
		tcp_hash (csk) ;

		tcp_send_control_packet (csk, TCP_SYN | TCP_ACK) ;
	}
	else if (tsk->state == TCP_SYN_SENT && 
		cb->flags == (TCP_SYN | TCP_ACK))
	{
		tsk->rcv_nxt = cb->seq_end ;

		tcp_set_state (tsk, TCP_ESTABLISHED) ;
		tcp_send_control_packet (tsk, TCP_ACK) ;
		
		wake_up (tsk->wait_connect) ;
	}
	else if (tsk->state == TCP_SYN_RECV && cb->flags == TCP_ACK)
	{
		list_delete_entry (&tsk->list) ;
		tcp_sock_accept_enqueue (tsk) ;
		
		tcp_set_state (tsk, TCP_ESTABLISHED) ;
		
		wake_up (tsk->parent->wait_accept) ;
	}

	// Close - FIN
	else if (tsk->state == TCP_ESTABLISHED && cb->flags == TCP_FIN)
	{
		tsk->rcv_nxt = cb->seq_end ;

		tcp_set_state (tsk, TCP_CLOSE_WAIT) ;
		tcp_send_control_packet (tsk, TCP_ACK) ;
	}
	else if (tsk->state == TCP_FIN_WAIT_1 && cb->flags == TCP_ACK)
		tcp_set_state (tsk, TCP_FIN_WAIT_2) ;
	else if ((tsk->state == TCP_FIN_WAIT_2 || 
		tsk->state == TCP_TIME_WAIT) && cb->flags == TCP_FIN)
	{
		tsk->rcv_nxt = cb->seq_end ;
		
		tcp_set_state (tsk, TCP_TIME_WAIT) ;
		tcp_send_control_packet (tsk, TCP_ACK) ;
		
		tcp_set_timewait_timer (tsk) ;
	}
	else if (tsk->state == TCP_LAST_ACK && cb->flags == TCP_ACK)
	{
		tcp_set_state (tsk, TCP_CLOSED) ;

		tcp_unhash (tsk) ;  // auto free memory
	}

	// Close - RST
	else if (cb->flags == TCP_RST)
	{
		tcp_set_state (tsk, TCP_CLOSED) ;
		
		if (!tsk->parent)
			tcp_bind_unhash (tsk) ;
		tcp_unhash (tsk) ; // auto free memory
	}
	
	fprintf(stdout, "TODO: implement %s please.\n", __FUNCTION__);
}
