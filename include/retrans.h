#ifndef __RETRANSE_H__
#define __RETRANSE_H__

struct snt_pkt   // fix - retrans
{
	struct list_head list ;
	char *packet ;
	int len ;
	int retrans_times ;
} ;

#endif
