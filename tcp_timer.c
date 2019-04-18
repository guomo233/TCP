#include "tcp.h"
#include "tcp_timer.h"
#include "tcp_sock.h"

#include "retrans.h" // fix
#include "congestion_control.h" // fix

#include <stdio.h>
#include <unistd.h>

static struct list_head timer_list;

// scan the timer_list, find the tcp sock which stays for at 2*MSL, release it
void tcp_scan_timer_list()
{
	struct tcp_timer *timer, *temp ;
	list_for_each_entry_safe (timer, temp, &timer_list, list)
	{
		timer->timeout -= TCP_TIMER_SCAN_INTERVAL ;
		if (timer->timeout > 0)
			continue ;
		
		if (timer->type == 0)
		{
			struct tcp_sock *tsk = timewait_to_tcp_sock (timer) ;
			
			tcp_set_state (tsk, TCP_CLOSED) ;
			tcp_unhash (tsk) ;
			tcp_bind_unhash (tsk) ; // auto free memory
		
			list_delete_entry (&(timer->list)) ;
		}
		else if (timer->type == 1) // retrans
		{
			struct tcp_sock *tsk = retranstimer_to_tcp_sock (timer) ;
			retrans_pkt (tsk) ;
			
			tsk->ssthresh = tsk->cwnd / 2 ;
			tsk->cwnd = TCP_MSS ;
			tsk->cg_state = TCP_CG_LOSS ;
		}
		else // type = 2, zero window probe
		{
			struct tcp_sock *tsk = zwptimer_to_tcp_sock (timer) ;
			int hdr_size = ETHER_HDR_SIZE + IP_BASE_HDR_SIZE + TCP_BASE_HDR_SIZE ;
			char *packet = (char *) malloc (sizeof(char) * hdr_size) ;
			
			tcp_send_packet (tsk, packet, hdr_size) ;
			tsk->zwp_times++ ;

			if (tsk->zwp_times >= 3)
			{
				log(DEBUG, "3 times zero window probe") ;
				tcp_sock_close (tsk) ;
			}
			else
			{
				timer->timeout = TCP_ZERO_WINDOW_PROBE_INITIAL ;
				for (int i=0; i<tsk->zwp_times; i++)
					timer->timeout *= 2 ;
			}
		}
	}

	//fprintf(stdout, "TODO: implement %s please.\n", __FUNCTION__);
}

// set the timewait timer of a tcp sock, by adding the timer into timer_list
void tcp_set_timewait_timer(struct tcp_sock *tsk)
{
	tsk->timewait.type = 0 ;
	tsk->timewait.timeout = TCP_TIMEWAIT_TIMEOUT ;
	
	if (list_empty(&(tsk->timewait.list)))
		list_add_tail (&(tsk->timewait.list), &timer_list) ;

	//fprintf(stdout, "TODO: implement %s please.\n", __FUNCTION__);
}

// set the retrans timer of a tcp sock, by adding the timer into timer_list
void tcp_set_retrans_timer(struct tcp_sock *tsk)
{
	tsk->retrans_timer.type = 1 ;
	tsk->retrans_timer.timeout = TCP_RETRANS_INTERVAL_INITIAL ;

	list_add_tail (&(tsk->retrans_timer.list), &timer_list) ;

	//fprintf(stdout, "TODO: implement %s please.\n", __FUNCTION__);
}

// unset the retrans timer of a tcp sock, by removing the timer from timer_list
void tcp_unset_retrans_timer(struct tcp_sock *tsk)
{
	if (!list_empty (&(tsk->retrans_timer.list)))
		list_delete_entry (&(tsk->retrans_timer.list)) ;

	//fprintf(stdout, "TODO: implement %s please.\n", __FUNCTION__);
}

// fix - zero window probe
void tcp_set_zwp_timer (struct tcp_sock *tsk)
{
	tsk->zwp_times = 0 ;
	tsk->zwp_timer.type = 2 ;
	tsk->zwp_timer.timeout = TCP_ZERO_WINDOW_PROBE_INITIAL ;

	list_add_tail (&(tsk->zwp_timer.list), &timer_list) ;
}

// fix - zero window probe
void tcp_unset_zwp_timer (struct tcp_sock *tsk)
{
	if (!list_empty (&(tsk->zwp_timer.list)))
		list_delete_entry (&(tsk->zwp_timer.list)) ;
}

// scan the timer_list periodically by calling tcp_scan_timer_list
void *tcp_timer_thread(void *arg)
{
	init_list_head(&timer_list);
	while (1) {
		usleep(TCP_TIMER_SCAN_INTERVAL);
		tcp_scan_timer_list();
	}

	return NULL;
}
