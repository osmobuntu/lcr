/*****************************************************************************\
**                                                                           **
** Linux Call Router                                                         **
**                                                                           **
**---------------------------------------------------------------------------**
** Copyright: Andreas Eversberg                                              **
**                                                                           **
** SIP port                                                                  **
**                                                                           **
\*****************************************************************************/ 

#include "main.h"
#include <sofia-sip/sip_status.h>
#include <sofia-sip/su_log.h>
#include <sofia-sip/sdp.h>
#include <sofia-sip/sip_header.h>

unsigned char flip[256];

//pthread_mutex_t mutex_msg;
su_home_t	sip_home[1];

struct sip_inst {
	struct interface	*interface;
	su_root_t		*root;
	nua_t			*nua;
};

static int delete_event(struct lcr_work *work, void *instance, int index);

/*
 * initialize SIP port
 */
Psip::Psip(int type, struct mISDNport *mISDNport, char *portname, struct port_settings *settings, int channel, int exclusive, int mode, struct interface *interface) : PmISDN(type, mISDNport, portname, settings, channel, exclusive, mode)
{
	p_m_s_sip_inst = interface->sip_inst;
	memset(&p_m_s_delete, 0, sizeof(p_m_s_delete));
	add_work(&p_m_s_delete, delete_event, this, 0);
	p_m_s_handle = 0;
	p_m_s_magic = 0;
	memset(&p_m_s_rtp_fd, 0, sizeof(p_m_s_rtp_fd));
	memset(&p_m_s_rtcp_fd, 0, sizeof(p_m_s_rtcp_fd));
	memset(&p_m_s_rtp_sin_local, 0, sizeof(p_m_s_rtp_sin_local));
	memset(&p_m_s_rtcp_sin_local, 0, sizeof(p_m_s_rtcp_sin_local));
	memset(&p_m_s_rtp_sin_remote, 0, sizeof(p_m_s_rtp_sin_remote));
	memset(&p_m_s_rtcp_sin_remote, 0, sizeof(p_m_s_rtcp_sin_remote));
	p_m_s_rtp_ip_local = 0;
	p_m_s_rtp_ip_remote = 0;
	p_m_s_rtp_port_local = 0;
	p_m_s_rtp_port_remote = 0;
	p_m_s_b_sock = -1;
	p_m_s_b_index = -1;
	p_m_s_b_active = 0;
	p_m_s_rxpos = 0;
	p_m_s_rtp_tx_action = 0;

	PDEBUG(DEBUG_SIP, "Created new Psip(%s).\n", portname);
}


/*
 * destructor
 */
Psip::~Psip()
{
	PDEBUG(DEBUG_SIP, "Destroyed SIP process(%s).\n", p_name);

	del_work(&p_m_s_delete);

	/* close audio transfer socket */
	if (p_m_s_b_sock > -1)
		bchannel_close();

	rtp_close();
}


static void sip_trace_header(class Psip *sip, const char *message, int direction)
{
	/* init trace with given values */
	start_trace(-1,
		    NULL,
		    sip?numberrize_callerinfo(sip->p_callerinfo.id, sip->p_callerinfo.ntype, options.national, options.international):NULL,
		    sip?sip->p_dialinginfo.id:NULL,
		    direction,
		    CATEGORY_CH,
		    sip?sip->p_serial:0,
		    message);
}

/*
 * RTP
 */

/* according to RFC 3550 */
struct rtp_hdr {
#if __BYTE_ORDER == __LITTLE_ENDIAN
	uint8_t  csrc_count:4,
		  extension:1,
		  padding:1,
		  version:2;
	uint8_t  payload_type:7,
		  marker:1;
#elif __BYTE_ORDER == __BIG_ENDIAN
	uint8_t  version:2,
		  padding:1,
		  extension:1,
		  csrc_count:4;
	uint8_t  marker:1,
		  payload_type:7;
#endif
	uint16_t sequence;
	uint32_t timestamp;
	uint32_t ssrc;
} __attribute__((packed));

struct rtp_x_hdr {
	uint16_t by_profile;
	uint16_t length;
} __attribute__((packed));

#define RTP_VERSION	2

#define RTP_PT_ULAW 0
#define RTP_PT_ALAW 8
#define RTP_PT_GSM_FULL 3
#define RTP_PT_GSM_HALF 96
#define RTP_PT_GSM_EFR 97
#define RTP_PT_AMR 98

/* decode an rtp frame  */
static int rtp_decode(class Psip *psip, unsigned char *data, int len)
{
	struct rtp_hdr *rtph = (struct rtp_hdr *)data;
	struct rtp_x_hdr *rtpxh;
	uint8_t *payload;
	int payload_len;
	int x_len;

	if (len < 12) {
		PDEBUG(DEBUG_SIP, "received RTP frame too short (len = %d)\n", len);
		return -EINVAL;
	}
	if (rtph->version != RTP_VERSION) {
		PDEBUG(DEBUG_SIP, "received RTP version %d not supported.\n", rtph->version);
		return -EINVAL;
	}
	payload = data + sizeof(struct rtp_hdr) + (rtph->csrc_count << 2);
	payload_len = len - sizeof(struct rtp_hdr) - (rtph->csrc_count << 2);
	if (payload_len < 0) {
		PDEBUG(DEBUG_SIP, "received RTP frame too short (len = %d, "
			"csrc count = %d)\n", len, rtph->csrc_count);
		return -EINVAL;
	}
	if (rtph->extension) {
		if (payload_len < (int)sizeof(struct rtp_x_hdr)) {
			PDEBUG(DEBUG_SIP, "received RTP frame too short for "
				"extension header\n");
			return -EINVAL;
		}
		rtpxh = (struct rtp_x_hdr *)payload;
		x_len = ntohs(rtpxh->length) * 4 + sizeof(struct rtp_x_hdr);
		payload += x_len;
		payload_len -= x_len;
		if (payload_len < 0) {
			PDEBUG(DEBUG_SIP, "received RTP frame too short, "
				"extension header exceeds frame length\n");
			return -EINVAL;
		}
	}
	if (rtph->padding) {
		if (payload_len < 0) {
			PDEBUG(DEBUG_SIP, "received RTP frame too short for "
				"padding length\n");
			return -EINVAL;
		}
		payload_len -= payload[payload_len - 1];
		if (payload_len < 0) {
			PDEBUG(DEBUG_SIP, "received RTP frame with padding "
				"greater than payload\n");
			return -EINVAL;
		}
	}

	switch (rtph->payload_type) {
	case RTP_PT_GSM_FULL:
		if (payload_len != 33) {
			PDEBUG(DEBUG_SIP, "received RTP full rate frame with "
				"payload length != 33 (len = %d)\n",
				payload_len);
			return -EINVAL;
		}
		break;
	case RTP_PT_GSM_EFR:
		if (payload_len != 31) {
			PDEBUG(DEBUG_SIP, "received RTP full rate frame with "
				"payload length != 31 (len = %d)\n",
				payload_len);
			return -EINVAL;
		}
		break;
	case RTP_PT_ALAW:
		if (options.law != 'a') {
			PDEBUG(DEBUG_SIP, "received Alaw, but we don't do Alaw\n");
			return -EINVAL;
		}
		break;
	case RTP_PT_ULAW:
		if (options.law == 'a') {
			PDEBUG(DEBUG_SIP, "received Ulaw, but we don't do Ulaw\n");
			return -EINVAL;
		}
		break;
	default:
		PDEBUG(DEBUG_SIP, "received RTP frame with unknown payload "
			"type %d\n", rtph->payload_type);
		return -EINVAL;
	}

	if (payload_len <= 0) {
		PDEBUG(DEBUG_SIP, "received RTP payload is too small: %d\n", payload_len);
		return 0;
	}

	psip->bchannel_send(PH_DATA_REQ, 0, payload, payload_len);

	return 0;
}

static int rtp_sock_callback(struct lcr_fd *fd, unsigned int what, void *instance, int index)
{
	class Psip *psip = (class Psip *) instance;
	int len;
	unsigned char buffer[256];
	int rc = 0;

	if ((what & LCR_FD_READ)) {
		len = read(fd->fd, &buffer, sizeof(buffer));
		if (len <= 0) {
			PDEBUG(DEBUG_SIP, "read result=%d\n", len);
//			psip->rtp_close();
//			psip->rtp_shutdown();
			return len;
		}
		rc = rtp_decode(psip, buffer, len);
	}

	return rc;
}

static int rtcp_sock_callback(struct lcr_fd *fd, unsigned int what, void *instance, int index)
{
//	class Psip *psip = (class Psip *) instance;
	int len;
	unsigned char buffer[256];

	if ((what & LCR_FD_READ)) {
		len = read(fd->fd, &buffer, sizeof(buffer));
		if (len <= 0) {
			PDEBUG(DEBUG_SIP, "read result=%d\n", len);
//			psip->rtp_close();
//			psip->rtp_shutdown();
			return len;
		}
		PDEBUG(DEBUG_SIP, "rtcp!");
	}

	return 0;
}

#define RTP_PORT_BASE	30000
static unsigned int next_udp_port = RTP_PORT_BASE;

static int rtp_sub_socket_bind(int fd, struct sockaddr_in *sin_local, uint32_t ip, uint16_t port)
{
	int rc;
	socklen_t alen = sizeof(*sin_local);

	sin_local->sin_family = AF_INET;
	sin_local->sin_addr.s_addr = htonl(ip);
	sin_local->sin_port = htons(port);

	rc = bind(fd, (struct sockaddr *) sin_local, sizeof(*sin_local));
	if (rc < 0)
		return rc;

	/* retrieve the address we actually bound to, in case we
	 * passed INADDR_ANY as IP address */
	return getsockname(fd, (struct sockaddr *) sin_local, &alen);
}

static int rtp_sub_socket_connect(int fd, struct sockaddr_in *sin_local, struct sockaddr_in *sin_remote, uint32_t ip, uint16_t port)
{
	int rc;
	socklen_t alen = sizeof(*sin_local);

	sin_remote->sin_family = AF_INET;
	sin_remote->sin_addr.s_addr = htonl(ip);
	sin_remote->sin_port = htons(port);

	rc = connect(fd, (struct sockaddr *) sin_remote, sizeof(*sin_remote));
	if (rc < 0) {
		PERROR("failed to connect to ip %08x port %d rc=%d\n", ip, port, rc);
		return rc;
	}

	return getsockname(fd, (struct sockaddr *) sin_local, &alen);
}

int Psip::rtp_open(void)
{
	int rc;
	struct in_addr ia;
	unsigned int ip;

	/* create socket */
	rc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (!rc) {
		rtp_close();
		return -EIO;
	}
	p_m_s_rtp_fd.fd = rc;
	register_fd(&p_m_s_rtp_fd, LCR_FD_READ, rtp_sock_callback, this, 0);

	rc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (!rc) {
		rtp_close();
		return -EIO;
	}
	p_m_s_rtcp_fd.fd = rc;
	register_fd(&p_m_s_rtcp_fd, LCR_FD_READ, rtcp_sock_callback, this, 0);

	/* bind socket */
	ip = htonl(INADDR_ANY);
	ia.s_addr = ip;
	for (next_udp_port = next_udp_port % 0xffff;
	     next_udp_port < 0xffff; next_udp_port += 2) {
		rc = rtp_sub_socket_bind(p_m_s_rtp_fd.fd, &p_m_s_rtp_sin_local, ip, next_udp_port);
		if (rc != 0)
			continue;

		rc = rtp_sub_socket_bind(p_m_s_rtcp_fd.fd, &p_m_s_rtcp_sin_local, ip, next_udp_port+1);
		if (rc == 0)
			break;
	}
	if (rc < 0) {
		PDEBUG(DEBUG_SIP, "failed to find port\n");
		rtp_close();
		return rc;
	}
	p_m_s_rtp_port_local = next_udp_port;
	p_m_s_rtp_ip_local = ntohl(p_m_s_rtp_sin_local.sin_addr.s_addr);
	PDEBUG(DEBUG_SIP, "local ip %08x port %d\n", p_m_s_rtp_ip_local, p_m_s_rtp_port_local);
	PDEBUG(DEBUG_SIP, "remote ip %08x port %d\n", p_m_s_rtp_ip_remote, p_m_s_rtp_port_remote);

	return p_m_s_rtp_port_local;
}

int Psip::rtp_connect(void)
{
	int rc;
	struct in_addr ia;

	ia.s_addr = htonl(p_m_s_rtp_ip_remote);
	PDEBUG(DEBUG_SIP, "rtp_connect(ip=%s, port=%u)\n", inet_ntoa(ia), p_m_s_rtp_port_remote);

	rc = rtp_sub_socket_connect(p_m_s_rtp_fd.fd, &p_m_s_rtp_sin_local, &p_m_s_rtp_sin_remote, p_m_s_rtp_ip_remote, p_m_s_rtp_port_remote);
	if (rc < 0)
		return rc;

	rc = rtp_sub_socket_connect(p_m_s_rtcp_fd.fd, &p_m_s_rtcp_sin_local, &p_m_s_rtcp_sin_remote, p_m_s_rtp_ip_remote, p_m_s_rtp_port_remote + 1);
	if (rc < 0)
		return rc;

	p_m_s_rtp_ip_local = ntohl(p_m_s_rtp_sin_local.sin_addr.s_addr);
	PDEBUG(DEBUG_SIP, "local ip %08x port %d\n", p_m_s_rtp_ip_local, p_m_s_rtp_port_local);
	PDEBUG(DEBUG_SIP, "remote ip %08x port %d\n", p_m_s_rtp_ip_remote, p_m_s_rtp_port_remote);
	p_m_s_rtp_is_connected = 1;

	return 0;
}
void Psip::rtp_close(void)
{
	if (p_m_s_rtp_fd.fd > 0) {
		unregister_fd(&p_m_s_rtp_fd);
		close(p_m_s_rtp_fd.fd);
		p_m_s_rtp_fd.fd = 0;
	}
	if (p_m_s_rtcp_fd.fd > 0) {
		unregister_fd(&p_m_s_rtcp_fd);
		close(p_m_s_rtcp_fd.fd);
		p_m_s_rtcp_fd.fd = 0;
	}
	if (p_m_s_rtp_is_connected) {
		PDEBUG(DEBUG_SIP, "rtp closed\n");
		p_m_s_rtp_is_connected = 0;
	}
}

/* "to - from" */
void tv_difference(struct timeval *diff, const struct timeval *from,
			  const struct timeval *__to)
{
	struct timeval _to = *__to, *to = &_to;

	if (to->tv_usec < from->tv_usec) {
		to->tv_sec -= 1;
		to->tv_usec += 1000000;
	}

	diff->tv_usec = to->tv_usec - from->tv_usec;
	diff->tv_sec = to->tv_sec - from->tv_sec;
}

/* encode and send a rtp frame */
int Psip::rtp_send_frame(unsigned char *data, unsigned int len, int payload_type)
{
	struct rtp_hdr *rtph;
	int payload_len;
	int duration; /* in samples */
	unsigned char buffer[256];

	if (!p_m_s_rtp_is_connected) {
		/* drop silently */
		return 0;
	}

	if (!p_m_s_rtp_tx_action) {
		/* initialize sequences */
		p_m_s_rtp_tx_action = 1;
		p_m_s_rtp_tx_ssrc = rand();
		p_m_s_rtp_tx_sequence = random();
		p_m_s_rtp_tx_timestamp = random();
		memset(&p_m_s_rtp_tx_last_tv, 0, sizeof(p_m_s_rtp_tx_last_tv));
	}

	switch (payload_type) {
	case RTP_PT_GSM_FULL:
		payload_len = 33;
		duration = 160;
		break;
	case RTP_PT_GSM_EFR:
		payload_len = 31;
		duration = 160;
		break;
	case RTP_PT_ALAW:
	case RTP_PT_ULAW:
		payload_len = len;
		duration = len;
		break;
	default:
		PERROR("unsupported message type %d\n", payload_type);
		return -EINVAL;
	}

#if 0
	{
		struct timeval tv, tv_diff;
		long int usec_diff, frame_diff;

		gettimeofday(&tv, NULL);
		tv_difference(&tv_diff, &p_m_s_rtp_tx_last_tv, &tv);
		p_m_s_rtp_tx_last_tv = tv;

		usec_diff = tv_diff.tv_sec * 1000000 + tv_diff.tv_usec;
		frame_diff = (usec_diff / 20000);

		if (abs(frame_diff) > 1) {
			long int frame_diff_excess = frame_diff - 1;

			PDEBUG(DEBUG_SIP, "Correcting frame difference of %ld frames\n", frame_diff_excess);
			p_m_s_rtp_tx_sequence += frame_diff_excess;
			p_m_s_rtp_tx_timestamp += frame_diff_excess * duration;
		}
	}
#endif

	rtph = (struct rtp_hdr *) buffer;
	rtph->version = RTP_VERSION;
	rtph->padding = 0;
	rtph->extension = 0;
	rtph->csrc_count = 0;
	rtph->marker = 0;
	rtph->payload_type = payload_type;
	rtph->sequence = htons(p_m_s_rtp_tx_sequence++);
	rtph->timestamp = htonl(p_m_s_rtp_tx_timestamp);
	p_m_s_rtp_tx_timestamp += duration;
	rtph->ssrc = htonl(p_m_s_rtp_tx_ssrc);
	memcpy(buffer + sizeof(struct rtp_hdr), data, payload_len);

	if (p_m_s_rtp_fd.fd > 0) {
		len = write(p_m_s_rtp_fd.fd, &buffer, sizeof(struct rtp_hdr) + payload_len);
		if (len != sizeof(struct rtp_hdr) + payload_len) {
			PDEBUG(DEBUG_SIP, "write result=%d\n", len);
//			rtp_close();
//			rtp_shutdown();
			return -EIO;
		}
	}

	return 0;
}

/*
 * bchannel handling
 */

/* select free bchannel from loopback interface */
int Psip::hunt_bchannel(void)
{
	return loop_hunt_bchannel(this, p_m_mISDNport);
}

/* close SIP side bchannel */
void Psip::bchannel_close(void)
{
	if (p_m_s_b_sock > -1) {
		unregister_fd(&p_m_s_b_fd);
		close(p_m_s_b_sock);
	}
	p_m_s_b_sock = -1;
	p_m_s_b_index = -1;
	p_m_s_b_active = 0;
}

static int b_handler(struct lcr_fd *fd, unsigned int what, void *instance, int index);

/* open external side bchannel */
int Psip::bchannel_open(int index)
{
	int ret;
	struct sockaddr_mISDN addr;
	struct mISDNhead act;

	if (p_m_s_b_sock > -1) {
		PERROR("Socket already created for index %d\n", index);
		return(-EIO);
	}

	/* open socket */
	ret = p_m_s_b_sock = socket(PF_ISDN, SOCK_DGRAM, ISDN_P_B_RAW);
	if (ret < 0) {
		PERROR("Failed to open bchannel-socket for index %d\n", index);
		bchannel_close();
		return(ret);
	}
	memset(&p_m_s_b_fd, 0, sizeof(p_m_s_b_fd));
	p_m_s_b_fd.fd = p_m_s_b_sock;
	register_fd(&p_m_s_b_fd, LCR_FD_READ, b_handler, this, 0);


	/* bind socket to bchannel */
	addr.family = AF_ISDN;
	addr.dev = mISDNloop.port;
	addr.channel = index+1+(index>15);
	ret = bind(p_m_s_b_sock, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		PERROR("Failed to bind bchannel-socket for index %d\n", index);
		bchannel_close();
		return(ret);
	}
	/* activate bchannel */
	PDEBUG(DEBUG_SIP, "Activating SIP side channel index %i.\n", index);
	act.prim = PH_ACTIVATE_REQ; 
	act.id = 0;
	ret = sendto(p_m_s_b_sock, &act, MISDN_HEADER_LEN, 0, NULL, 0);
	if (ret < 0) {
		PERROR("Failed to activate index %d\n", index);
		bchannel_close();
		return(ret);
	}

	p_m_s_b_index = index;

	return(0);
}

/* receive from bchannel */
void Psip::bchannel_receive(struct mISDNhead *hh, unsigned char *data, int len)
{
	/* write to rx buffer */
	while(len--) {
		p_m_s_rxdata[p_m_s_rxpos++] = flip[*data++];
		if (p_m_s_rxpos == 160) {
			p_m_s_rxpos = 0;

			/* transmit data via rtp */
			rtp_send_frame(p_m_s_rxdata, 160, (options.law=='a')?RTP_PT_ALAW:RTP_PT_ULAW);
		}
	}
}

/* transmit to bchannel */
void Psip::bchannel_send(unsigned int prim, unsigned int id, unsigned char *data, int len)
{
	unsigned char buf[MISDN_HEADER_LEN+len];
	struct mISDNhead *hh = (struct mISDNhead *)buf;
	unsigned char *to = buf + MISDN_HEADER_LEN;
	int n = len;
	int ret;

	if (!p_m_s_b_active)
		return;

	/* make and send frame */
	hh->prim = prim;
	hh->id = 0;
	while(n--)
		*to++ = flip[*data++];
	ret = sendto(p_m_s_b_sock, buf, MISDN_HEADER_LEN+len, 0, NULL, 0);
	if (ret <= 0)
		PERROR("Failed to send to socket index %d\n", p_m_s_b_index);
}

/* handle socket input */
static int b_handler(struct lcr_fd *fd, unsigned int what, void *instance, int index)
{
	class Psip *psip = (class Psip *)instance;
	int ret;
	unsigned char buffer[2048+MISDN_HEADER_LEN];
	struct mISDNhead *hh = (struct mISDNhead *)buffer;

	/* handle message from bchannel */
	if (psip->p_m_s_b_sock > -1) {
		ret = recv(psip->p_m_s_b_sock, buffer, sizeof(buffer), 0);
		if (ret >= (int)MISDN_HEADER_LEN) {
			switch(hh->prim) {
				/* we don't care about confirms, we use rx data to sync tx */
				case PH_DATA_CNF:
				break;
				/* we receive audio data, we respond to it AND we send tones */
				case PH_DATA_IND:
				psip->bchannel_receive(hh, buffer+MISDN_HEADER_LEN, ret-MISDN_HEADER_LEN);
				break;
				case PH_ACTIVATE_IND:
				psip->p_m_s_b_active = 1;
				break;
				case PH_DEACTIVATE_IND:
				psip->p_m_s_b_active = 0;
				break;
			}
		} else {
			if (ret < 0 && errno != EWOULDBLOCK)
				PERROR("Read from GSM port, index %d failed with return code %d\n", ret);
		}
	}

	return 0;
}

/* taken from freeswitch */
/* map sip responses to QSIG cause codes ala RFC4497 section 8.4.4 */
static int status2cause(int status)
{
	switch (status) {
	case 200:
		return 16; //SWITCH_CAUSE_NORMAL_CLEARING;
	case 401:
	case 402:
	case 403:
	case 407:
	case 603:
		return 21; //SWITCH_CAUSE_CALL_REJECTED;
	case 404:
		return 1; //SWITCH_CAUSE_UNALLOCATED_NUMBER;
	case 485:
	case 604:
		return 3; //SWITCH_CAUSE_NO_ROUTE_DESTINATION;
	case 408:
	case 504:
		return 102; //SWITCH_CAUSE_RECOVERY_ON_TIMER_EXPIRE;
	case 410:
		return 22; //SWITCH_CAUSE_NUMBER_CHANGED;
	case 413:
	case 414:
	case 416:
	case 420:
	case 421:
	case 423:
	case 505:
	case 513:
		return 127; //SWITCH_CAUSE_INTERWORKING;
	case 480:
		return 180; //SWITCH_CAUSE_NO_USER_RESPONSE;
	case 400:
	case 481:
	case 500:
	case 503:
		return 41; //SWITCH_CAUSE_NORMAL_TEMPORARY_FAILURE;
	case 486:
	case 600:
		return 17; //SWITCH_CAUSE_USER_BUSY;
	case 484:
		return 28; //SWITCH_CAUSE_INVALID_NUMBER_FORMAT;
	case 488:
	case 606:
		return 88; //SWITCH_CAUSE_INCOMPATIBLE_DESTINATION;
	case 502:
		return 38; //SWITCH_CAUSE_NETWORK_OUT_OF_ORDER;
	case 405:
		return 63; //SWITCH_CAUSE_SERVICE_UNAVAILABLE;
	case 406:
	case 415:
	case 501:
		return 79; //SWITCH_CAUSE_SERVICE_NOT_IMPLEMENTED;
	case 482:
	case 483:
		return 25; //SWITCH_CAUSE_EXCHANGE_ROUTING_ERROR;
	case 487:
		return 31; //??? SWITCH_CAUSE_ORIGINATOR_CANCEL;
	default:
		return 31; //SWITCH_CAUSE_NORMAL_UNSPECIFIED;
	}
}

static int cause2status(int cause, int location, const char **st)
{
	int s;

	switch (cause) {
	case 1:
		s = 404; *st = sip_404_Not_found;
		break;
	case 2:
		s = 404; *st = sip_404_Not_found;
		break;
	case 3:
		s = 404; *st = sip_404_Not_found;
		break;
	case 17:
		s = 486; *st = sip_486_Busy_here;
		break;
	case 18:
		s = 408; *st = sip_408_Request_timeout;
		break;
	case 19:
		s = 480; *st = sip_480_Temporarily_unavailable;
		break;
	case 20:
		s = 480; *st = sip_480_Temporarily_unavailable;
		break;
	case 21:
		if (location == LOCATION_USER) {
			s = 603; *st = sip_603_Decline;
		} else {
			s = 403; *st = sip_403_Forbidden;
		}
		break;
	case 22:
		//s = 301; *st = sip_301_Moved_permanently;
		s = 410; *st = sip_410_Gone;
		break;
	case 23:
		s = 410; *st = sip_410_Gone;
		break;
	case 27:
		s = 502; *st = sip_502_Bad_gateway;
		break;
	case 28:
		s = 484; *st = sip_484_Address_incomplete;
		break;
	case 29:
		s = 501; *st = sip_501_Not_implemented;
		break;
	case 31:
		s = 480; *st = sip_480_Temporarily_unavailable;
		break;
	case 34:
		s = 503; *st = sip_503_Service_unavailable;
		break;
	case 38:
		s = 503; *st = sip_503_Service_unavailable;
		break;
	case 41:
		s = 503; *st = sip_503_Service_unavailable;
		break;
	case 42:
		s = 503; *st = sip_503_Service_unavailable;
		break;
	case 47:
		s = 503; *st = sip_503_Service_unavailable;
		break;
	case 55:
		s = 403; *st = sip_403_Forbidden;
		break;
	case 57:
		s = 403; *st = sip_403_Forbidden;
		break;
	case 58:
		s = 503; *st = sip_503_Service_unavailable;
		break;
	case 65:
		s = 488; *st = sip_488_Not_acceptable;
		break;
	case 69:
		s = 501; *st = sip_501_Not_implemented;
		break;
	case 70:
		s = 488; *st = sip_488_Not_acceptable;
		break;
	case 79:
		s = 501; *st = sip_501_Not_implemented;
		break;
	case 87:
		s = 403; *st = sip_403_Forbidden;
		break;
	case 88:
		s = 503; *st = sip_503_Service_unavailable;
		break;
	case 102:
		s = 504; *st = sip_504_Gateway_time_out;
		break;
	default:
		s = 468; *st = sip_486_Busy_here;
	}

	return s;
}

/*
 * endpoint sends messages to the SIP port
 */

int Psip::message_connect(unsigned int epoint_id, int message_id, union parameter *param)
{
	char sdp_str[256];
	struct in_addr ia;
	struct lcr_msg *message;

	if (rtp_connect() < 0) {
		nua_cancel(p_m_s_handle, TAG_END());
		nua_handle_destroy(p_m_s_handle);
		p_m_s_handle = NULL;
		sip_trace_header(this, "CANCEL", DIRECTION_OUT);
		add_trace("reason", NULL, "failed to connect RTP/RTCP sockts");
		end_trace();
		message = message_create(p_serial, epoint_id, PORT_TO_EPOINT, MESSAGE_RELEASE);
		message->param.disconnectinfo.cause = 41;
		message->param.disconnectinfo.location = LOCATION_PRIVATE_LOCAL;
		message_put(message);
		new_state(PORT_STATE_RELEASE);
		trigger_work(&p_m_s_delete);
		return 0;
	}
	ia.s_addr = htonl(p_m_s_rtp_ip_local);

	SPRINT(sdp_str,
		"v=0\n"
		"o=LCR-Sofia-SIP 0 0 IN IP4 %s\n"
		"s=SIP Call\n"
		"c=IN IP4 %s\n"
		"t=0 0\n"
		"m=audio %d RTP/AVP %d\n"
		"a=rtpmap:%d %s/8000\n"
		, inet_ntoa(ia), inet_ntoa(ia), p_m_s_rtp_port_local, (options.law=='a')?RTP_PT_ALAW:RTP_PT_ULAW, (options.law=='a')?RTP_PT_ALAW:RTP_PT_ULAW, (options.law=='a')?"PCMA":"PCMU");
	PDEBUG(DEBUG_SIP, "Using SDP response: %s\n", sdp_str);

	nua_respond(p_m_s_handle, SIP_200_OK,
		NUTAG_MEDIA_ENABLE(0),
		SIPTAG_CONTENT_TYPE_STR("application/sdp"),
		SIPTAG_PAYLOAD_STR(sdp_str), TAG_END());
	new_state(PORT_STATE_CONNECT);
	sip_trace_header(this, "RESPOND", DIRECTION_OUT);
	add_trace("respond", "value", "200 OK");
	add_trace("reason", NULL, "call connected");
	end_trace();

	return 0;
}

int Psip::message_release(unsigned int epoint_id, int message_id, union parameter *param)
{
	struct lcr_msg *message;
	char cause_str[128] = "";
	int cause = param->disconnectinfo.cause;
	int location = param->disconnectinfo.cause;
	int status;
	const char *status_text;

	if (cause > 0 && cause <= 127) {
		SPRINT(cause_str, "Q.850;cause=%d;text=\"%s\"", cause, isdn_cause[cause].english);
	}

	switch (p_state) {
	case PORT_STATE_OUT_SETUP:
	case PORT_STATE_OUT_PROCEEDING:
	case PORT_STATE_OUT_ALERTING:
		PDEBUG(DEBUG_SIP, "RELEASE/DISCONNECT will cancel\n");
		sip_trace_header(this, "CANCEL", DIRECTION_OUT);
		if (cause_str[0])
			add_trace("cause", "value", "%d", cause);
		end_trace();
		nua_cancel(p_m_s_handle, TAG_IF(cause_str[0], SIPTAG_REASON_STR(cause_str)), TAG_END());
		break;
	case PORT_STATE_IN_SETUP:
	case PORT_STATE_IN_PROCEEDING:
	case PORT_STATE_IN_ALERTING:
		PDEBUG(DEBUG_SIP, "RELEASE/DISCONNECT will respond\n");
		status = cause2status(cause, location, &status_text);
		sip_trace_header(this, "RESPOND", DIRECTION_OUT);
		if (cause_str[0])
			add_trace("cause", "value", "%d", cause);
		add_trace("respond", "value", "%d %s", status, status_text);
		end_trace();
		nua_respond(p_m_s_handle, status, status_text, TAG_IF(cause_str[0], SIPTAG_REASON_STR(cause_str)), TAG_END());
		nua_handle_destroy(p_m_s_handle);
		p_m_s_handle = NULL;
		trigger_work(&p_m_s_delete);
		break;
	default:
		PDEBUG(DEBUG_SIP, "RELEASE/DISCONNECT will perform nua_bye\n");
		sip_trace_header(this, "BYE", DIRECTION_OUT);
		if (cause_str[0])
			add_trace("cause", "value", "%d", cause);
		end_trace();
		nua_bye(p_m_s_handle, TAG_IF(cause_str[0], SIPTAG_REASON_STR(cause_str)), TAG_END());
	}

	if (message_id == MESSAGE_DISCONNECT) {
		while(p_epointlist) {
			message = message_create(p_serial, p_epointlist->epoint_id, PORT_TO_EPOINT, MESSAGE_RELEASE);
			message->param.disconnectinfo.cause = CAUSE_NORMAL;
			message->param.disconnectinfo.location = LOCATION_BEYOND;
			message_put(message);
			/* remove epoint */
			free_epointlist(p_epointlist);
		}
	}

	new_state(PORT_STATE_RELEASE);

	return(0);
}

int Psip::message_setup(unsigned int epoint_id, int message_id, union parameter *param)
{
	struct sip_inst *inst = (struct sip_inst *) p_m_s_sip_inst;
	char from[128];
	char to[128];
	const char *local = inst->interface->sip_local_ip;
	const char *remote = inst->interface->sip_remote_ip;
	char sdp_str[256];
	struct in_addr ia;
	struct epoint_list *epointlist;
	sip_cseq_t *cseq = NULL;
	int ret;
	int channel;

	PDEBUG(DEBUG_SIP, "Doing Setup (inst %p)\n", inst);

	/* release if port is blocked */
	if (p_m_mISDNport->ifport->block) {
		struct lcr_msg *message;

		sip_trace_header(this, "INVITE", DIRECTION_OUT);
		add_trace("failure", NULL, "Port blocked.");
		end_trace();
		message = message_create(p_serial, epoint_id, PORT_TO_EPOINT, MESSAGE_RELEASE);
		message->param.disconnectinfo.cause = 27; // temp. unavail.
		message->param.disconnectinfo.location = LOCATION_PRIVATE_LOCAL;
		message_put(message);
		new_state(PORT_STATE_RELEASE);
		trigger_work(&p_m_s_delete);
		return 0;
	}

	/* hunt channel */
	ret = channel = hunt_bchannel();
	if (ret < 0)
		goto no_channel;
	/* open channel */
	ret = seize_bchannel(channel, 1);
	if (ret < 0) {
		struct lcr_msg *message;

		no_channel:
		sip_trace_header(this, "INVITE", DIRECTION_OUT);
		add_trace("failure", NULL, "No internal audio channel available.");
		end_trace();
		message = message_create(p_serial, epoint_id, PORT_TO_EPOINT, MESSAGE_RELEASE);
		message->param.disconnectinfo.cause = 34;
		message->param.disconnectinfo.location = LOCATION_PRIVATE_LOCAL;
		message_put(message);
		new_state(PORT_STATE_RELEASE);
		trigger_work(&p_m_s_delete);
		return 0;
	}
	bchannel_event(p_m_mISDNport, p_m_b_index, B_EVENT_USE);
	if (bchannel_open(p_m_b_index))
		goto no_channel;

	memcpy(&p_dialinginfo, &param->setup.dialinginfo, sizeof(p_dialinginfo));
	memcpy(&p_callerinfo, &param->setup.callerinfo, sizeof(p_callerinfo));
	memcpy(&p_redirinfo, &param->setup.redirinfo, sizeof(p_redirinfo));

	/* connect to remote RTP */
	if (rtp_open() < 0) {
		struct lcr_msg *message;

		PERROR("Failed to open RTP sockets\n");
		/* send release message to endpoit */
		message = message_create(p_serial, epoint_id, PORT_TO_EPOINT, MESSAGE_RELEASE);
		message->param.disconnectinfo.cause = 41;
		message->param.disconnectinfo.location = LOCATION_PRIVATE_LOCAL;
		message_put(message);
		new_state(PORT_STATE_RELEASE);
		trigger_work(&p_m_s_delete);
		return 0;
	}
	SPRINT(from, "sip:%s@%s", param->setup.callerinfo.id, local);
	SPRINT(to, "sip:%s@%s", param->setup.dialinginfo.id, remote);

	sip_trace_header(this, "INVITE", DIRECTION_OUT);
	add_trace("from", "uri", "%s", from);
	add_trace("to", "uri", "%s", to);
	add_trace("rtp", "port", "%d,%d", p_m_s_rtp_port_local, p_m_s_rtp_port_local + 1);
	end_trace();

	p_m_s_handle = nua_handle(inst->nua, NULL, TAG_END());
	if (!p_m_s_handle) {
		struct lcr_msg *message;

		PERROR("Failed to create handle\n");
		/* send release message to endpoit */
		message = message_create(p_serial, epoint_id, PORT_TO_EPOINT, MESSAGE_RELEASE);
		message->param.disconnectinfo.cause = 41;
		message->param.disconnectinfo.location = LOCATION_PRIVATE_LOCAL;
		message_put(message);
		new_state(PORT_STATE_RELEASE);
		trigger_work(&p_m_s_delete);
		return 0;
	}
	/* apply handle */
	sip_trace_header(this, "NEW handle", DIRECTION_IN);
	add_trace("handle", "new", "0x%x", p_m_s_handle);
	end_trace();

	inet_pton(AF_INET, local, &p_m_s_rtp_ip_local);
	p_m_s_rtp_ip_local = ntohl(p_m_s_rtp_ip_local);
	ia.s_addr = htonl(p_m_s_rtp_ip_local);
	SPRINT(sdp_str,
		"v=0\n"
		"o=LCR-Sofia-SIP 0 0 IN IP4 %s\n"
		"s=SIP Call\n"
		"c=IN IP4 %s\n"
		"t=0 0\n"
		"m=audio %d RTP/AVP %d\n"
		"a=rtpmap:%d %s/8000\n"
		, inet_ntoa(ia), inet_ntoa(ia), p_m_s_rtp_port_local, (options.law=='a')?RTP_PT_ALAW:RTP_PT_ULAW, (options.law=='a')?RTP_PT_ALAW:RTP_PT_ULAW, (options.law=='a')?"PCMA":"PCMU");
	PDEBUG(DEBUG_SIP, "Using SDP for invite: %s\n", sdp_str);

//	cseq = sip_cseq_create(sip_home, 123, SIP_METHOD_INVITE);

	nua_invite(p_m_s_handle,
		TAG_IF(from[0], SIPTAG_FROM_STR(from)),
		TAG_IF(to[0], SIPTAG_TO_STR(to)),
		TAG_IF(cseq, SIPTAG_CSEQ(cseq)),
		NUTAG_MEDIA_ENABLE(0),
		SIPTAG_CONTENT_TYPE_STR("application/sdp"),
		SIPTAG_PAYLOAD_STR(sdp_str), TAG_END());
	new_state(PORT_STATE_OUT_SETUP);

	/* attach only if not already */
	epointlist = p_epointlist;
	while(epointlist) {
		if (epointlist->epoint_id == epoint_id)
			break;
		epointlist = epointlist->next;
	}
	if (!epointlist)
		epointlist_new(epoint_id);

	return 0;
}
	
int Psip::message_epoint(unsigned int epoint_id, int message_id, union parameter *param)
{
	class Endpoint *epoint;

	if (PmISDN::message_epoint(epoint_id, message_id, param))
		return(1);

	epoint = find_epoint_id(epoint_id);
	if (!epoint) {
		PDEBUG(DEBUG_SIP, "PORT(%s) no endpoint object found where the message is from.\n", p_name);
		return(0);
	}

	switch(message_id) {
		case MESSAGE_DATA:
		return(1);

		case MESSAGE_ALERTING: /* call is ringing on LCR side */
		if (p_state != PORT_STATE_IN_SETUP
		 && p_state != PORT_STATE_IN_PROCEEDING)
			return 0;
		nua_respond(p_m_s_handle, SIP_180_RINGING, TAG_END());
		sip_trace_header(this, "RESPOND", DIRECTION_OUT);
		add_trace("respond", "value", "180 Ringing");
		end_trace();
		new_state(PORT_STATE_IN_ALERTING);
		return(1);

		case MESSAGE_CONNECT: /* call is connected on LCR side */
		if (p_state != PORT_STATE_IN_SETUP
		 && p_state != PORT_STATE_IN_PROCEEDING
		 && p_state != PORT_STATE_IN_ALERTING)
			return 0;
		message_connect(epoint_id, message_id, param);
		return(1);

		case MESSAGE_DISCONNECT: /* call has been disconnected */
		case MESSAGE_RELEASE: /* call has been released */
		message_release(epoint_id, message_id, param);
		return(1);

		case MESSAGE_SETUP: /* dial-out command received from epoint */
		message_setup(epoint_id, message_id, param);
		return(1);

		default:
		PDEBUG(DEBUG_SIP, "PORT(%s) SP port with (caller id %s) received an unsupported message: %d\n", p_name, p_callerinfo.id, message_id);
	}

	return(0);
}

int Psip::parse_sdp(sip_t const *sip, unsigned int *ip, unsigned short *port)
{
	int codec_supported = 0;

	if (!sip->sip_payload) {
		PDEBUG(DEBUG_SIP, "no payload given\n");
		return 0;
	}

	sdp_parser_t *parser;
	sdp_session_t *sdp;
	sdp_media_t *m;
	sdp_attribute_t *attr;
	sdp_rtpmap_t *map;
	sdp_connection_t *conn;

	PDEBUG(DEBUG_SIP, "payload given: %s\n", sip->sip_payload->pl_data);

	parser = sdp_parse(NULL, sip->sip_payload->pl_data, (int) strlen(sip->sip_payload->pl_data), 0);
	if (!parser) {
		return 400;
	}
	if (!(sdp = sdp_session(parser))) {
		sdp_parser_free(parser);
		return 400;
	}
	for (m = sdp->sdp_media; m; m = m->m_next) {
		if (m->m_proto != sdp_proto_rtp)
			continue;
		if (m->m_type != sdp_media_audio)
			continue;
		PDEBUG(DEBUG_SIP, "RTP port:'%u'\n", m->m_port);
		*port = m->m_port;
		for (attr = m->m_attributes; attr; attr = attr->a_next) {
			PDEBUG(DEBUG_SIP, "ATTR: name:'%s' value='%s'\n", attr->a_name, attr->a_value);
		}
		if (m->m_connections) {
			conn = m->m_connections;
			PDEBUG(DEBUG_SIP, "CONN: address:'%s'\n", conn->c_address);
			inet_pton(AF_INET, conn->c_address, ip);
			*ip = ntohl(p_m_s_rtp_ip_remote);
		} else {
			char *p = sip->sip_payload->pl_data;
			char addr[16];

			PDEBUG(DEBUG_SIP, "sofia cannot find connection tag, so we try ourself\n");
			p = strstr(p, "c=IN IP4 ");
			if (!p) {
				PDEBUG(DEBUG_SIP, "missing c-tag with internet address\n");
				sdp_parser_free(parser);
				return 400;
			}
			SCPY(addr, p + 9);
			if ((p = strchr(addr, '\n'))) *p = '\0';
			if ((p = strchr(addr, '\r'))) *p = '\0';
			PDEBUG(DEBUG_SIP, "CONN: address:'%s'\n", addr);
			inet_pton(AF_INET, addr, ip);
			*ip = ntohl(p_m_s_rtp_ip_remote);
		}
		for (map = m->m_rtpmaps; map; map = map->rm_next) {
			PDEBUG(DEBUG_SIP, "RTPMAP: coding:'%s' rate='%d'\n", map->rm_encoding, map->rm_rate);
			if (!strcmp(map->rm_encoding, (options.law=='a')?"PCMA":"PCMU") && map->rm_rate == 8000) {
				PDEBUG(DEBUG_SIP, "supported codec found\n");
				codec_supported = 1;
				goto done_codec;
			}
		}
	}
	done_codec:

	sdp_parser_free(parser);

	if (!codec_supported)
		return 415;

	return 0;
}

void Psip::i_invite(int status, char const *phrase, nua_t *nua, nua_magic_t *magic, nua_handle_t *nh, nua_hmagic_t *hmagic, sip_t const *sip, tagi_t tagss[])
{
	const char *from = "", *to = "";
	int ret;
	class Endpoint *epoint;
	struct lcr_msg *message;
	int channel;

	if (sip->sip_from && sip->sip_from->a_url)
		from = sip->sip_from->a_url->url_user;
	if (sip->sip_to && sip->sip_to->a_url)
		to = sip->sip_to->a_url->url_user;
	PDEBUG(DEBUG_SIP, "invite received (%s->%s)\n", from, to);

	ret = parse_sdp(sip, &p_m_s_rtp_ip_remote, &p_m_s_rtp_port_remote);
	if (ret) {
		if (ret == 400)
			nua_respond(nh, SIP_400_BAD_REQUEST, TAG_END());
		else
			nua_respond(nh, SIP_415_UNSUPPORTED_MEDIA, TAG_END());
		nua_handle_destroy(nh);
		p_m_s_handle = NULL;
		sip_trace_header(this, "RESPOND", DIRECTION_OUT);
		if (ret == 400)
			add_trace("respond", "value", "415 Unsupported Media");
		else
			add_trace("respond", "value", "400 Bad Request");
		add_trace("reason", NULL, "offered codec does not match");
		end_trace();
		new_state(PORT_STATE_RELEASE);
		trigger_work(&p_m_s_delete);
		return;
	}

	/* connect to remote RTP */
	if (rtp_open() < 0) {
		nua_respond(nh, SIP_500_INTERNAL_SERVER_ERROR, TAG_END());
		nua_handle_destroy(nh);
		p_m_s_handle = NULL;
		sip_trace_header(this, "RESPOND", DIRECTION_OUT);
		add_trace("respond", "value", "500 Internal Server Error");
		add_trace("reason", NULL, "failed to open RTP/RTCP sockts");
		end_trace();
		new_state(PORT_STATE_RELEASE);
		trigger_work(&p_m_s_delete);
		return;
	}

	/* apply handle */
	sip_trace_header(this, "NEW handle", DIRECTION_IN);
	add_trace("handle", "new", "0x%x", nh);
	p_m_s_handle = nh;
	end_trace();

	/* if blocked, release call */
	if (p_m_mISDNport->ifport->block) {
		nua_respond(nh, SIP_503_SERVICE_UNAVAILABLE, TAG_END());
		nua_handle_destroy(nh);
		p_m_s_handle = NULL;
		sip_trace_header(this, "RESPOND", DIRECTION_OUT);
		add_trace("respond", "value", "503 Service Unavailable");
		add_trace("reason", NULL, "port is blocked");
		end_trace();
		new_state(PORT_STATE_RELEASE);
		trigger_work(&p_m_s_delete);
		return;
	}
	sip_trace_header(this, "INVITE", DIRECTION_IN);
	add_trace("RTP", "port", "%d", p_m_s_rtp_port_remote);
	/* caller information */
	if (!from[0]) {
		p_callerinfo.present = INFO_PRESENT_NOTAVAIL;
		p_callerinfo.ntype = INFO_NTYPE_NOTPRESENT;
		add_trace("calling", "present", "unavailable");
	} else {
		p_callerinfo.present = INFO_PRESENT_ALLOWED;
		add_trace("calling", "present", "allowed");
		p_callerinfo.screen = INFO_SCREEN_NETWORK;
		p_callerinfo.ntype = INFO_NTYPE_UNKNOWN;
		SCPY(p_callerinfo.id, from);
		add_trace("calling", "number", "%s", from);
	}
	p_callerinfo.isdn_port = p_m_portnum;
	SCPY(p_callerinfo.interface, p_m_mISDNport->ifport->interface->name);
	/* dialing information */
	if (to[0]) {
		p_dialinginfo.ntype = INFO_NTYPE_UNKNOWN;
		SCAT(p_dialinginfo.id, to);
		add_trace("dialing", "number", "%s", to);
	}
	/* redir info */
	/* bearer capability */
	p_capainfo.bearer_capa = INFO_BC_SPEECH;
	p_capainfo.bearer_info1 = (options.law=='a')?3:2;
	p_capainfo.bearer_mode = INFO_BMODE_CIRCUIT;
	add_trace("bearer", "capa", "speech");
	add_trace("bearer", "mode", "circuit");
	/* if packet mode works some day, see dss1.cpp for conditions */
	p_capainfo.source_mode = B_MODE_TRANSPARENT;

	end_trace();

	/* hunt channel */
	ret = channel = hunt_bchannel();
	if (ret < 0)
		goto no_channel;

	/* open channel */
	ret = seize_bchannel(channel, 1);
	if (ret < 0) {
		no_channel:
		nua_respond(nh, SIP_480_TEMPORARILY_UNAVAILABLE, TAG_END());
		nua_handle_destroy(nh);
		p_m_s_handle = NULL;
		sip_trace_header(this, "RESPOND", DIRECTION_OUT);
		add_trace("respond", "value", "480 Temporarily Unavailable");
		add_trace("reason", NULL, "no channel");
		end_trace();
		new_state(PORT_STATE_RELEASE);
		trigger_work(&p_m_s_delete);
		return;
	}
	bchannel_event(p_m_mISDNport, p_m_b_index, B_EVENT_USE);
	if (bchannel_open(p_m_b_index))
		goto no_channel;

	/* create endpoint */
	if (p_epointlist)
		FATAL("Incoming call but already got an endpoint.\n");
	if (!(epoint = new Endpoint(p_serial, 0)))
		FATAL("No memory for Endpoint instance\n");
	if (!(epoint->ep_app = new DEFAULT_ENDPOINT_APP(epoint, 0))) //incoming
		FATAL("No memory for Endpoint Application instance\n");
	epointlist_new(epoint->ep_serial);

	/* send trying (proceeding) */
	nua_respond(nh, SIP_100_TRYING, TAG_END());
	sip_trace_header(this, "RESPOND", DIRECTION_OUT);
	add_trace("respond", "value", "100 Trying");
	end_trace();

	new_state(PORT_STATE_IN_PROCEEDING);

	/* send setup message to endpoit */
	message = message_create(p_serial, ACTIVE_EPOINT(p_epointlist), PORT_TO_EPOINT, MESSAGE_SETUP);
	message->param.setup.isdn_port = p_m_portnum;
	message->param.setup.port_type = p_type;
//	message->param.setup.dtmf = 0;
	memcpy(&message->param.setup.dialinginfo, &p_dialinginfo, sizeof(struct dialing_info));
	memcpy(&message->param.setup.callerinfo, &p_callerinfo, sizeof(struct caller_info));
	memcpy(&message->param.setup.capainfo, &p_capainfo, sizeof(struct capa_info));
//	SCPY((char *)message->param.setup.useruser.data, useruser.info);
//	message->param.setup.useruser.len = strlen(mncc->useruser.info);
//	message->param.setup.useruser.protocol = mncc->useruser.proto;
	message_put(message);
}

void Psip::i_bye(int status, char const *phrase, nua_t *nua, nua_magic_t *magic, nua_handle_t *nh, nua_hmagic_t *hmagic, sip_t const *sip, tagi_t tagss[])
{
	struct lcr_msg *message;
	int cause = 0;

	PDEBUG(DEBUG_SIP, "bye received\n");

	sip_trace_header(this, "BYE", DIRECTION_IN);
	if (sip->sip_reason && sip->sip_reason->re_protocol && !strcasecmp(sip->sip_reason->re_protocol, "Q.850") && sip->sip_reason->re_cause) {
		cause = atoi(sip->sip_reason->re_cause);
		add_trace("cause", "value", "%d", cause);
	}
	end_trace();

// let stack do bye automaticall, since it will not accept our response for some reason
//	nua_respond(nh, SIP_200_OK, TAG_END());
	sip_trace_header(this, "RESPOND", DIRECTION_OUT);
	add_trace("respond", "value", "200 OK");
	end_trace();
//	nua_handle_destroy(nh);
	p_m_s_handle = NULL;

	rtp_close();

	while(p_epointlist) {
		/* send setup message to endpoit */
		message = message_create(p_serial, p_epointlist->epoint_id, PORT_TO_EPOINT, MESSAGE_RELEASE);
		message->param.disconnectinfo.cause = cause ? : 16;
		message->param.disconnectinfo.location = LOCATION_BEYOND;
		message_put(message);
		/* remove epoint */
		free_epointlist(p_epointlist);
	}
	new_state(PORT_STATE_RELEASE);
	trigger_work(&p_m_s_delete);
}

void Psip::i_cancel(int status, char const *phrase, nua_t *nua, nua_magic_t *magic, nua_handle_t *nh, nua_hmagic_t *hmagic, sip_t const *sip, tagi_t tagss[])
{
	struct lcr_msg *message;

	PDEBUG(DEBUG_SIP, "cancel received\n");

	sip_trace_header(this, "CANCEL", DIRECTION_IN);
	end_trace();

	nua_handle_destroy(nh);
	p_m_s_handle = NULL;

	rtp_close();

	while(p_epointlist) {
		/* send setup message to endpoit */
		message = message_create(p_serial, p_epointlist->epoint_id, PORT_TO_EPOINT, MESSAGE_RELEASE);
		message->param.disconnectinfo.cause = 16;
		message->param.disconnectinfo.location = LOCATION_BEYOND;
		message_put(message);
		/* remove epoint */
		free_epointlist(p_epointlist);
	}
	new_state(PORT_STATE_RELEASE);
	trigger_work(&p_m_s_delete);
}

void Psip::r_bye(int status, char const *phrase, nua_t *nua, nua_magic_t *magic, nua_handle_t *nh, nua_hmagic_t *hmagic, sip_t const *sip, tagi_t tagss[])
{
	PDEBUG(DEBUG_SIP, "bye response received\n");

	nua_handle_destroy(nh);
	p_m_s_handle = NULL;

	rtp_close();

	trigger_work(&p_m_s_delete);
}

void Psip::r_cancel(int status, char const *phrase, nua_t *nua, nua_magic_t *magic, nua_handle_t *nh, nua_hmagic_t *hmagic, sip_t const *sip, tagi_t tagss[])
{
	PDEBUG(DEBUG_SIP, "cancel response received\n");

	nua_handle_destroy(nh);
	p_m_s_handle = NULL;

	rtp_close();

	trigger_work(&p_m_s_delete);
}

void Psip::r_invite(int status, char const *phrase, nua_t *nua, nua_magic_t *magic, nua_handle_t *nh, nua_hmagic_t *hmagic, sip_t const *sip, tagi_t tagss[])
{
	struct lcr_msg *message;
	int cause = 0, location = 0;

	PDEBUG(DEBUG_SIP, "response to invite received (status = %d)\n", status);

	sip_trace_header(this, "RESPOND", DIRECTION_OUT);
	add_trace("respond", "value", "%d", status);
	end_trace();

	/* process 1xx */
	switch (status) {
	case 100:
		PDEBUG(DEBUG_SIP, "do proceeding\n");
		new_state(PORT_STATE_OUT_PROCEEDING);
		message = message_create(p_serial, ACTIVE_EPOINT(p_epointlist), PORT_TO_EPOINT, MESSAGE_PROCEEDING);
		message_put(message);
		return;
	case 180:
		PDEBUG(DEBUG_SIP, "do alerting\n");
		new_state(PORT_STATE_OUT_ALERTING);
		message = message_create(p_serial, ACTIVE_EPOINT(p_epointlist), PORT_TO_EPOINT, MESSAGE_ALERTING);
		message_put(message);
		return;
	default:
		if (status < 100 || status > 199)
			break;
		PDEBUG(DEBUG_SIP, "skipping 1xx message\n");

		return;
	}

	/* process 2xx */
	if (status >= 200 && status <= 299) {
		int ret;

		ret = parse_sdp(sip, &p_m_s_rtp_ip_remote, &p_m_s_rtp_port_remote);
		if (ret) {
			if (ret == 400)
				nua_cancel(nh, TAG_END());
			else
				nua_cancel(nh, TAG_END());
			sip_trace_header(this, "CANCEL", DIRECTION_OUT);
			add_trace("reason", NULL, "offered codec does not match");
			end_trace();
			cause = 88;
			location = LOCATION_PRIVATE_LOCAL;
			goto release_with_cause;
		}

		/* connect to remote RTP */
		if (rtp_connect() < 0) {
			nua_cancel(nh, TAG_END());
			sip_trace_header(this, "CANCEL", DIRECTION_OUT);
			add_trace("reason", NULL, "failed to open RTP/RTCP sockts");
			end_trace();
			cause = 31;
			location = LOCATION_PRIVATE_LOCAL;
			goto release_with_cause;
		}

		PDEBUG(DEBUG_SIP, "do connect\n");
		nua_ack(nh, TAG_END());
		new_state(PORT_STATE_CONNECT);
		message = message_create(p_serial, ACTIVE_EPOINT(p_epointlist), PORT_TO_EPOINT, MESSAGE_CONNECT);
		message_put(message);

		return;
	}

	cause = status2cause(status);
	location = LOCATION_BEYOND;

release_with_cause:
	PDEBUG(DEBUG_SIP, "do release (cause %d)\n", cause);

	while(p_epointlist) {
		/* send setup message to endpoit */
		message = message_create(p_serial, p_epointlist->epoint_id, PORT_TO_EPOINT, MESSAGE_RELEASE);
		message->param.disconnectinfo.cause = cause;
		message->param.disconnectinfo.location = location;
		message_put(message);
		/* remove epoint */
		free_epointlist(p_epointlist);
	}

	new_state(PORT_STATE_RELEASE);

	rtp_close();

	trigger_work(&p_m_s_delete);
}

static void sip_callback(nua_event_t event, int status, char const *phrase, nua_t *nua, nua_magic_t *magic, nua_handle_t *nh, nua_hmagic_t *hmagic, sip_t const *sip, tagi_t tags[])
{
	struct sip_inst *inst = (struct sip_inst *) magic;
	class Port *port;
	class Psip *psip = NULL;

	PDEBUG(DEBUG_SIP, "Event %d from stack received (handle=%p)\n", event, nh);
	if (!nh)
		return;

	/* create or find port instance */
	if (event == nua_i_invite)
	{
		char name[64];
		/* create call instance */
		SPRINT(name, "%s-%d-in", inst->interface->name, 0);
		if (!(psip = new Psip(PORT_TYPE_SIP_IN, (inst->interface->ifport) ? inst->interface->ifport->mISDNport : NULL, name, NULL, 0, 0, B_MODE_TRANSPARENT, inst->interface)))
			FATAL("Cannot create Port instance.\n");
	} else {
		port = port_first;
		while(port) {
			if ((port->p_type & PORT_CLASS_mISDN_MASK) == PORT_CLASS_SIP) {
				psip = (class Psip *)port;
				if (psip->p_m_s_handle == nh) {
					break;
				}
			}
			port = port->next;
		}
	}
	if (!psip) {
		PERROR("no SIP Port found for handel %p\n", nh);
		nua_respond(nh, SIP_500_INTERNAL_SERVER_ERROR, TAG_END());
		nua_handle_destroy(nh);
		return;
	}

	switch (event) {
	case nua_r_set_params:
		PDEBUG(DEBUG_SIP, "setparam response\n");
		break;
	case nua_i_error:
		PDEBUG(DEBUG_SIP, "error received\n");
		break;
	case nua_i_state:
		PDEBUG(DEBUG_SIP, "state change received\n");
		break;
	case nua_i_register:
		PDEBUG(DEBUG_SIP, "register received\n");
		break;
	case nua_i_invite:
		psip->i_invite(status, phrase, nua, magic, nh, hmagic, sip, tags);
		break;
	case nua_i_ack:
		PDEBUG(DEBUG_SIP, "ack received\n");
		break;
	case nua_i_active:
		PDEBUG(DEBUG_SIP, "active received\n");
		break;
	case nua_i_bye:
		psip->i_bye(status, phrase, nua, magic, nh, hmagic, sip, tags);
		break;
	case nua_i_cancel:
		psip->i_cancel(status, phrase, nua, magic, nh, hmagic, sip, tags);
		break;
	case nua_r_bye:
		psip->r_bye(status, phrase, nua, magic, nh, hmagic, sip, tags);
		break;
	case nua_r_cancel:
		psip->r_cancel(status, phrase, nua, magic, nh, hmagic, sip, tags);
		break;
	case nua_r_invite:
		psip->r_invite(status, phrase, nua, magic, nh, hmagic, sip, tags);
		break;
	case nua_i_terminated:
		PDEBUG(DEBUG_SIP, "terminated received\n");
		break;
	default:
		PDEBUG(DEBUG_SIP, "Event %d not handled\n", event);
	}
}

/* received shutdown due to termination of RTP */
void Psip::rtp_shutdown(void)
{
	struct lcr_msg *message;

	PDEBUG(DEBUG_SIP, "RTP stream terminated\n");

	sip_trace_header(this, "RTP terminated", DIRECTION_IN);
	end_trace();

	nua_handle_destroy(p_m_s_handle);
	p_m_s_handle = NULL;

	while(p_epointlist) {
		/* send setup message to endpoit */
		message = message_create(p_serial, p_epointlist->epoint_id, PORT_TO_EPOINT, MESSAGE_RELEASE);
		message->param.disconnectinfo.cause = 16;
		message->param.disconnectinfo.location = LOCATION_BEYOND;
		message_put(message);
		/* remove epoint */
		free_epointlist(p_epointlist);
	}
	new_state(PORT_STATE_RELEASE);
	trigger_work(&p_m_s_delete);
}

int sip_init_inst(struct interface *interface)
{
	struct sip_inst *inst = (struct sip_inst *) MALLOC(sizeof(*inst));

	interface->sip_inst = inst;
	inst->interface = interface;

	/* init root object */
	inst->root = su_root_create(inst);
	if (!inst->root) {
		PERROR("Failed to create SIP root\n");
		sip_exit_inst(interface);
		return -EINVAL;
	}

	inst->nua = nua_create(inst->root, sip_callback, inst, TAG_NULL());
	if (!inst->nua) {
		PERROR("Failed to create SIP stack object\n");
		sip_exit_inst(interface);
		return -EINVAL;
	}
	nua_set_params(inst->nua,
		SIPTAG_ALLOW_STR("INVITE,ACK,BYE,CANCEL,OPTIONS,NOTIFY,INFO"),
		NUTAG_APPL_METHOD("INVITE"),
		NUTAG_APPL_METHOD("ACK"),
//		NUTAG_APPL_METHOD("BYE"), /* we must reply to BYE */
		NUTAG_APPL_METHOD("CANCEL"),
		NUTAG_APPL_METHOD("OPTIONS"),
		NUTAG_APPL_METHOD("NOTIFY"),
		NUTAG_APPL_METHOD("INFO"),
		NUTAG_AUTOACK(0),
		NUTAG_AUTO100(0),
		NUTAG_AUTOALERT(0),
		NUTAG_AUTOANSWER(0),
		TAG_NULL());

	PDEBUG(DEBUG_SIP, "SIP interface created (inst=%p)\n", inst);

	return 0;
}

void sip_exit_inst(struct interface *interface)
{
	struct sip_inst *inst = (struct sip_inst *) interface->sip_inst;

	if (!inst)
		return;
	if (inst->root)
		su_root_destroy(inst->root);
	if (inst->nua) {
		nua_destroy(inst->nua);
	}
	FREE(inst, sizeof(*inst));
	interface->sip_inst = NULL;

	PDEBUG(DEBUG_SIP, "SIP interface removed\n");
}

extern su_log_t su_log_default[];
extern su_log_t nua_log[];
//extern su_log_t soa_log[];

int sip_init(void)
{
	int i;

	/* init SOFIA lib */
	su_init();
	su_home_init(sip_home);

	if (options.deb & DEBUG_SIP) {
		su_log_set_level(su_log_default, 9);
		su_log_set_level(nua_log, 9);
		//su_log_set_level(soa_log, 9);
	}

	for (i = 0; i < 256; i++)
		flip[i] = ((i & 1) << 7) + ((i & 2) << 5) + ((i & 4) << 3) + ((i & 8) << 1) + ((i & 16) >> 1) + ((i & 32) >> 3) + ((i & 64) >> 5) + ((i & 128) >> 7);

	PDEBUG(DEBUG_SIP, "SIP globals initialized\n");

	return 0;
}

void sip_exit(void)
{
	su_home_deinit(sip_home);
	su_deinit();

	PDEBUG(DEBUG_SIP, "SIP globals de-initialized\n");
}

void sip_handle(void)
{
	struct interface *interface = interface_first;
	struct sip_inst *inst;

	while (interface) {
		if (interface->sip_inst) {
			inst = (struct sip_inst *) interface->sip_inst;
			su_root_step(inst->root, 0);
		}
		interface = interface->next;
	}
}

/* deletes when back in event loop */
static int delete_event(struct lcr_work *work, void *instance, int index)
{
	class Psip *psip = (class Psip *)instance;

	delete psip;

	return 0;
}

