/*****************************************************************************\
**                                                                           **
** Linux Call Router                                                         **
**                                                                           **
**---------------------------------------------------------------------------**
** Copyright: Andreas Eversberg                                              **
**                                                                           **
** Asterisk socket client                                                    **
**                                                                           **
\*****************************************************************************/

/*

Registering to LCR:

To connect, open an LCR socket and send a MESSAGE_HELLO to socket with
the application name. This name is unique an can be used for routing calls.
Now the channel driver is linked to LCR and can receive and make calls.


Call is initiated by LCR:

If a call is received from LCR, a MESSAGE_NEWREF is received first.
A new chan_call instance is created. The call reference (ref) is given by
MESSAGE_NEWREF. The state is CHAN_LCR_STATE_IN_PREPARE.
After receiving MESSAGE_SETUP from LCR, the ast_channel instance is created
using ast_channel_alloc(1).  The setup information is given to asterisk.
The new Asterisk instance pointer (ast) is stored to chan_call structure.
The state changes to CHAN_LCR_STATE_IN_SETUP.


Call is initiated by Asterisk:

If a call is reveiced from Asterisk, a new chan_call instance is created.
The new Asterisk instance pointer (ast) is stored to chan_call structure.
A MESSASGE_NEWREF is sent to LCR requesting a new call reference (ref).
The current call ref is set to 0, the state is CHAN_LCR_STATE_OUT_PREPARE.
Further dialing information is queued.
After the new callref is received by special MESSAGE_NEWREF reply, new ref
is stored in the chan_call structure. 
The setup information is sent to LCR using MESSAGE_SETUP.
The state changes to CHAN_LCR_STATE_OUT_SETUP.


Call is in process:

During call process, messages are received and sent.
The state changes accordingly.
Any message is allowed to be sent to LCR at any time except MESSAGE_RELEASE.
If a MESSAGE_OVERLAP is received, further dialing is required.
Queued dialing information, if any, is sent to LCR using MESSAGE_DIALING.
In this case, the state changes to CHAN_LCR_STATE_OUT_DIALING.


Call is released by LCR:

A MESSAGE_RELEASE is received with the call reference (ref) to be released.
The current ref is set to 0, to indicate released reference.
The state changes to CHAN_LCR_STATE_RELEASE.
ast_queue_hangup() is called, if asterisk instance (ast) exists, if not,
the chan_call instance is destroyed.
After lcr_hangup() is called-back by Asterisk, the chan_call instance
is destroyed, because the current ref is set to 0 and the state equals
CHAN_LCR_STATE_RELEASE.
If the ref is 0 and the state is not CHAN_LCR_STATE_RELEASE, see the proceedure
"Call is released by Asterisk".


Call is released by Asterisk:

lcr_hangup() is called-back by Asterisk. If the call reference (ref) is set,
a MESSAGE_RELEASE is sent to LCR and the chan_call instance is destroyed.
If the ref is 0 and the state is not CHAN_LCR_STATE_RELEASE, the new state is
set to CHAN_LCR_STATE_RELEASE.
Later, if the MESSAGE_NEWREF reply is received, a MESSAGE_RELEASE is sent to
LCR and the chan_call instance is destroyed.
If the ref is 0 and the state is CHAN_LCR_STATE_RELEASE, see the proceedure
"Call is released by LCR".

*/

locking asterisk process and handler
reconnect after socket closed, release all calls.
debug of call handling
denke an alle info-elements in jeder message (from asterisk & from lcr)


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <pthread.h>
#include <semaphore.h>

#include "extension.h"
#include "message.h"
#include "lcrsocket.h"
#include "cause.h"
#include "bchannel.h"
#include "chan_lcr.h"



#include <asterisk/module.h>
#include <asterisk/channel.h>
#include <asterisk/config.h>
#include <asterisk/logger.h>
#include <asterisk/pbx.h>
#include <asterisk/options.h>
#include <asterisk/io.h>
#include <asterisk/frame.h>
#include <asterisk/translate.h>
#include <asterisk/cli.h>
#include <asterisk/musiconhold.h>
#include <asterisk/dsp.h>
#include <asterisk/translate.h>
#include <asterisk/file.h>
#include <asterisk/callerid.h>
#include <asterisk/indications.h>
#include <asterisk/app.h>
#include <asterisk/features.h>
#include <asterisk/sched.h>

CHAN_LCR_STATE // state description structure

int lcr_debug=1;
int mISDN_created=1;


int lcr_sock = -1;

struct admin_list {
	struct admin_list *next;
	struct admin_msg msg;
} *admin_first = NULL;

/*
 * channel and call instances
 */
struct chan_call *call_first;

struct chan_call *find_call_ref(unsigned long ref)
{
	struct chan_call *call = call_first;

	while(call)
	{
		if (call->ref == ref)
			break;
		call = call->next;
	}
	return(call);
}

struct chan_call *find_call_handle(unsigned long handle)
{
	struct chan_call *call = call_first;

	while(call)
	{
		if (call->bchannel_handle == handle)
			break;
		call = call->next;
	}
	return(call);
}

struct chan_call *alloc_call(void)
{
	struct chan_call **callp = &call_first;

	while(*callp)
		callp = &((*callp)->next);

	*callp = (struct chan_call *)malloc(sizeof(struct chan_call));
	if (*callp)
		memset(*callp, 0, sizeof(struct chan_call));
	return(*callp);
}

void free_call(struct chan_call *call)
{
	struct chan_call **temp = &call_first;

	while(*temp)
	{
		if (*temp == call)
		{
			*temp = (*temp)->next;
			free(call);
			return;
		}
		temp = &((*temp)->next);
	}
}

unsigned short new_brige_id(void)
{
	struct chan_call *call;
	unsigned short id = 1;

	/* search for lowest bridge id that is not in use and not 0 */
	while(id)
	{
		call = call_first;
		while(call)
		{
			if (call->bridge_id == id)
				break;
			call = call->next;
		}
		if (!call)
			break;
		id++;
	}
	return(id);
}


/*
 * receive bchannel data
 */
void rx_data(struct bchannel *bchannel, unsigned char *data, int len)
{
}

void rx_dtmf(struct bchannel *bchannel, char tone)
{
}

/*
 * enque message to LCR
 */
int send_message(int message_type, unsigned long ref, union parameter *param)
{
	struct admin_list *admin, **adminp;

	adminp = &admin_first;
	while(*adminp)
		adminp = &((*adminp)->next);
	admin = (struct admin_list *)malloc(sizeof(struct admin_list));
	*adminp = admin;

	admin->msg.type = message_type;
	admin->msg.ref = ref;
	memcpy(&admin->msg.param, param, sizeof(union parameter));

	return(0);
}

/*
 * incoming setup from LCR
 */
static void lcr_in_setup(struct chan_call *call, int message_type, union parameter *param)
{
	struct ast_channel *ast;
	union parameter newparam;

	/* create asterisk channel instrance */
	ast = ast_channel_alloc(1);
	if (!ast)
	{
		/* release */
		memset(&newparam, 0, sizeof(union parameter));
		newparam.disconnectinfo.cause = CAUSE_RESSOURCEUNAVAIL;
		newparam.disconnectinfo.location = LOCATION_PRIVATE_LOCAL;
		send_message(MESSAGE_RELEASE, call->ref, &newparam);
		/* remove call */
		free_call(call);
		return;
	}
	/* set ast pointer */
	call->ast = ast;
	
	/* fill setup information */
#warning todo: setup-info reinschreiben

	/* send setup to asterisk */
#warning todo: setup bei der asterisk triggern

	/* change state */
	call->state = CHAN_LCR_STATE_IN_SETUP;
}

/*
 * incoming setup acknowledge from LCR
 */
static void lcr_in_overlap(struct chan_call *call, int message_type, union parameter *param)
{
	/* send pending digits in dialque */
	if (call->dialque)
		send_dialing_to_lcr(call);
	/* change to overlap state */
	call->state = CHAN_LCR_STATE_OUT_DIALING;
}

/*
 * incoming proceeding from LCR
 */
static void lcr_in_proceeding(struct chan_call *call, int message_type, union parameter *param)
{
	/* change state */
	call->state = CHAN_LCR_STATE_OUT_PROCEEDING;
	/* send event to asterisk */
	ast_queue_... todo
}

/*
 * incoming alerting from LCR
 */
static void lcr_in_alerting(struct chan_call *call, int message_type, union parameter *param)
{
	/* change state */
	call->state = CHAN_LCR_STATE_OUT_ALERTING;
	/* send event to asterisk */
	ast_queue_... todo
}

/*
 * incoming connect from LCR
 */
static void lcr_in_connect(struct chan_call *call, int message_type, union parameter *param)
{
	/* change state */
	call->state = CHAN_LCR_STATE_CONNECT;
	/* copy connectinfo */
	todo
	/* send event to asterisk */
	ast_queue_... todo
}

/*
 * incoming disconnect from LCR
 */
static void lcr_in_disconnect(struct chan_call *call, int message_type, union parameter *param)
{
	/* change state */
	call->state = CHAN_LCR_STATE_IN_DISCONNECT;
	/* copy disconnect info */
	todo
	/* send event to asterisk */
	ast_queue_... todo
}

/*
 * incoming setup acknowledge from LCR
 */
static void lcr_in_release(struct chan_call *call, int message_type, union parameter *param)
{
	/* release ref */
	call->ref = NULL;
	/* change to release state */
	call->state = CHAN_LCR_STATE_RELEASE;
	/* copy release info */
	todo
	/* if we have an asterisk instance, send hangup, else we are done */
	if (call->ast)
	{
		ast_queue_hangup(call->ast);
	} else
	{
		free_call(call);
	}
	
}

/*
 * incoming information from LCR
 */
static void lcr_in_information(struct chan_call *call, int message_type, union parameter *param)
{
	/* copy digits */
	todo and write them, maybe queue them for asterisk
	/* send event to asterisk */
	ast_queue_... todo
}

/*
 * incoming information from LCR
 */
static void lcr_in_facility(struct chan_call *call, int message_type, union parameter *param)
{
	/* copy progress info */
	todo and write them, maybe queue them for asterisk
	/* send event to asterisk */
	ast_queue_... todo
	or maybe use bride info to forward facility.
}

/*
 * message received from LCR
 */
int receive_message(int message_type, unsigned long ref, union parameter *param)
{
	union parameter newparam;
	struct bchannel *bchannel;
	struct chan_call *call;

	memset(&newparam, 0, sizeof(union parameter));

	/* handle bchannel message*/
	if (message_type == MESSAGE_BCHANNEL)
	{
		switch(param->bchannel.type)
		{
			case BCHANNEL_ASSIGN:
			if ((bchannel = find_bchannel_handle(param->bchannel.handle)))
			{
				fprintf(stderr, "error: bchannel handle %x already assigned.\n", (int)param->bchannel.handle);
				return(-1);
			}
			/* create bchannel */
			bchannel = alloc_bchannel(param->bchannel.handle);
			if (!bchannel)
			{
				fprintf(stderr, "error: alloc bchannel handle %x failed.\n", (int)param->bchannel.handle);
				return(-1);
			}

			/* configure channel */
			bchannel->b_tx_gain = param->bchannel.tx_gain;
			bchannel->b_rx_gain = param->bchannel.rx_gain;
			strncpy(bchannel->b_pipeline, param->bchannel.pipeline, sizeof(bchannel->b_pipeline)-1);
			if (param->bchannel.crypt_len)
			{
				bchannel->b_crypt_len = param->bchannel.crypt_len;
				bchannel->b_crypt_type = param->bchannel.crypt_type;
				memcpy(bchannel->b_crypt_key, param->bchannel.crypt, param->bchannel.crypt_len);
			}
			bchannel->b_txdata = 0;
			bchannel->b_dtmf = 1;
			bchannel->b_tx_dejitter = 1;

			/* in case, ref is not set, this bchannel instance must
			 * be created until it is removed again by LCR */
			/* link to call */
			if ((call = find_call_ref(ref)))
			{
				bchannel->ref = ref;
				call->bchannel_handle = param->bchannel.handle;
#warning hier muesen alle stati gesetzt werden falls sie vor dem b-kanal verf�gbar waren
				bchannel_join(bchannel, call->bridge_id);
			}
			if (bchannel_create(bchannel))
				bchannel_activate(bchannel, 1);

			/* acknowledge */
			newparam.bchannel.type = BCHANNEL_ASSIGN_ACK;
			newparam.bchannel.handle = param->bchannel.handle;
			send_message(MESSAGE_BCHANNEL, 0, &newparam);
			break;

			case BCHANNEL_REMOVE:
			if (!(bchannel = find_bchannel_handle(param->bchannel.handle)))
			{
				#warning alle fprintf nach ast_log
				fprintf(stderr, "error: bchannel handle %x not assigned.\n", (int)param->bchannel.handle);
				return(-1);
			}
			/* unlink from call */
			if ((call = find_call_ref(bchannel->ref)))
			{
				call->bchannel_handle = 0;
			}
			/* destroy and remove bchannel */
			free_bchannel(bchannel);

			/* acknowledge */
			newparam.bchannel.type = BCHANNEL_REMOVE_ACK;
			newparam.bchannel.handle = param->bchannel.handle;
			send_message(MESSAGE_BCHANNEL, 0, &newparam);
			
			break;

			default:
			fprintf(stderr, "received unknown bchannel message %d\n", param->bchannel.type);
		}
		return(0);
	}

	/* handle new ref */
	if (message_type == MESSAGE_NEWREF)
	{
		if (param->direction)
		{
			/* new ref from lcr */
			if (!ref || find_call_ref(ref))
			{
				fprintf(stderr, "illegal new ref %d received\n", ref);
				return(-1);
			}
			/* allocate new call instance */
			call = alloc_call();
			/* new state */
			call->state = CHAN_LCR_STATE_IN_PREPARE;
			/* set ref */
			call->ref = ref;
			/* wait for setup (or release from asterisk) */
		} else
		{
			/* new ref, as requested from this remote application */
			call = find_call_ref(0);
			if (!call)
			{
				/* send release, if ref does not exist */
				newparam.disconnectinfo.cause = CAUSE_NORMAL;
				newparam.disconnectinfo.location = LOCATION_PRIVATE_LOCAL;
				send_message(MESSAGE_RELEASE, ref, &newparam);
				return(0);
			}
			/* store new ref */
			call->ref = ref;
			/* send pending setup info */
			if (call->state == CHAN_LCR_STATE_OUT_PREPARE)
				send_setup_to_lcr(call);
			/* release if asterisk has signed off */
			else if (call->state == CHAN_LCR_STATE_RELEASE)
			{
				/* send release */
				newparam.disconnectinfo.cause = todo
				newparam.disconnectinfo.location = todo
				send_message(MESSAGE_RELEASE, ref, &newparam);
				/* free call */
				free_call(call);
				return(0);
			}
		}
		return(0);
	}

	/* check ref */
	if (!ref)
	{
		fprintf(stderr, "received message %d without ref\n", message_type);
		return(-1);
	}
	call = find_call_ref(ref);
	if (!call)
	{
		/* ignore ref that is not used (anymore) */
		return(0);
	}

	/* handle messages */
	switch(message_type)
	{
		case MESSAGE_SETUP:
		lcr_in_setup(call, message_type, param);
		break;

		case MESSAGE_OVERLAP:
		lcr_in_overlap(call, message_type, param);
		break;

		case MESSAGE_PROCEEDING:
		lcr_in_proceeding(call, message_type, param);
		break;

		case MESSAGE_ALERTING:
		lcr_in_alerting(call, message_type, param);
		break;

		case MESSAGE_CONNECT:
		lcr_in_connect(call, message_type, param);
		break;

		case MESSAGE_DISCONNECT:
		lcr_in_disconnect(call, message_type, param);
		break;

		case MESSAGE_RELEASE:
		lcr_in_release(call, message_type, param);
		break;

		case MESSAGE_INFORMATION:
		lcr_in_disconnect(call, message_type, param);
		break;

		case MESSAGE_FACILITY:
		lcr_in_disconnect(call, message_type, param);
		break;

		case MESSAGE_PATTERN:
#warning todo
		break;

		case MESSAGE_NOPATTERN:
#warning todo
		break;

		case MESSAGE_AUDIOPATH:
#warning todo
		break;

		default:
#warning unhandled
	}
	return(0);
}


/* asterisk handler
 * warning! not thread safe
 * returns -1 for socket error, 0 for no work, 1 for work
 */
int handle_socket(void)
{
	int work = 0;
	int len;
	struct admin_message msg;
	struct admin_list *admin;

	int sock;

	#warning SOCKET FEHLT!
	/* read from socket */
	len = read(sock, &msg, sizeof(msg));
	if (len == 0)
	{
		printf("Socket closed\n");
		return(-1); // socket closed
	}
	if (len > 0)
	{
		if (len != sizeof(msg))
		{
			fprintf(stderr, "Socket short read (%d)\n", len);
			return(-1); // socket error
		}
		if (msg.message != ADMIN_MESSAGE)
		{
			fprintf(stderr, "Socket received illegal message %d\n", msg.message);
			return(-1); // socket error
		}
		receive_message(msg.u.msg.type, msg.u.msg.ref, &msg.u.msg.param);
		printf("message received %d\n", msg.u.msg.type);
		work = 1;
	} else
	{
		if (errno != EWOULDBLOCK)
		{
			fprintf(stderr, "Socket error %d\n", errno);
			return(-1);
		}
	}

	/* write to socket */
	if (!admin_first)
		return(work);
	admin = admin_first;
	len = write(sock, &admin->msg, sizeof(msg));
	if (len == 0)
	{
		printf("Socket closed\n");
		return(-1); // socket closed
	}
	if (len > 0)
	{
		if (len != sizeof(msg))
		{
			fprintf(stderr, "Socket short write (%d)\n", len);
			return(-1); // socket error
		}
		/* free head */
		admin_first = admin->next;
		free(admin);

		work = 1;
	} else
	{
		if (errno != EWOULDBLOCK)
		{
			fprintf(stderr, "Socket error %d\n", errno);
			return(-1);
		}
	}

	return(work);
}

/*
 * open and close socket
 */
int open_socket(void)
{
	int ret;
	int sock;
	char *socket_name = SOCKET_NAME;
	int conn;
	struct sockaddr_un sock_address;
	unsigned long on = 1;
	union parameter param;

	/* open socket */
	if ((sock = socket(PF_UNIX, SOCK_STREAM, 0)) < 0)
	{
		ast_log(LOG_ERROR, "Failed to create socket.\n");
		return(sock);
	}

	/* set socket address and name */
	memset(&sock_address, 0, sizeof(sock_address));
	sock_address.sun_family = PF_UNIX;
	strcpy(sock_address.sun_path, socket_name);

	/* connect socket */
	if ((conn = connect(sock, (struct sockaddr *)&sock_address, SUN_LEN(&sock_address))) < 0)
	{
		close(sock);
		ast_log(LOG_ERROR, "Failed to connect to socket \"%s\". Is LCR running?\n", sock_address.sun_path);
		return(conn);
	}

	/* set non-blocking io */
	if ((ret = ioctl(sock, FIONBIO, (unsigned char *)(&on))) < 0)
	{
		close(sock);
		ast_log(LOG_ERROR, "Failed to set socket into non-blocking IO.\n");
		return(ret);
	}

	/* enque hello message */
	memset(&param, 0, sizeof(param));
	strcpy(param.hello.application, "asterisk");
	send_message(MESSAGE_HELLO, 0, &param);

	return(sock);
}

void close_socket(int sock)
{
	/* close socket */
	if (socket >= 0)	
		close(sock);
}


hander thread muss noch
socket muss per timer fuer das �ffnen checken
void lcr_thread(void)
{
	int work;

	while(42)
	{
		work = 0;

		/* handle socket */
		int ret = handle_socket();
		if (ret < 0)
			break;
		if (ret)
			work = 1;

		/* handle mISDN */
		ret = bchannel_handle();
		if (ret)
			work = 1;
		
		if (!work)
			usleep(30000);
	}
}

/*
 * send setup info to LCR
 * this function is called, when asterisk call is received and ref is received
 */
static void send_setup_to_lcr(struct chan_call *call)
{
	if (!ast || !call->ref)
		return;

	/* send setup message to LCR */
	memset(&newparam, 0, sizeof(union parameter));
	newparam.setup.xxxxxx = 
	send_message(MESSAGE_SETUP, call->ref, &newparam);
	/* change to outgoing setup state */
	call->state = CHAN_LCR_STATE_OUT_SETUP;
}

/*
 * send dialing info to LCR
 * this function is called, when setup acknowledge is received and dialing
 * info is available.
 */
static void send_dialing_to_lcr(struct chan_call *call)
{
	if (!ast || !call->ref || !call->dialque)
		return;
	
	/* send setup message to LCR */
	memset(&newparam, 0, sizeof(union parameter));
	strncpy(newparam.dialinginfo.id, call->dialque, sizeof(newparam.dialinginfo.id)-1);
	call->dialque[0] = '\0';
	send_message(MESSAGE_INFORMATION, call->ref, &newparam);
}

/*
 * new asterisk instance
 */
static struct ast_channel *lcr_request(const char *type, int format, void *data, int *cause)
{
	/* create call instance */
	call = alloc_call();
	if (!call)
	{
		/* failed to create instance */
		return NULL;
	}
	/* create asterisk channel instrance */
	ast = ast_channel_alloc(1);
	if (!ast)
	{
		free_call(call);
		/* failed to create instance */
		return NULL;
	}
	/* link together */
	ast->tech_pvt = call;
	call->ast = ast;
	/* send MESSAGE_NEWREF */
	memset(&newparam, 0, sizeof(union parameter));
	newparam.direction = 0; /* request from app */
	send_message(MESSAGE_NEWREF, 0, &newparam);
	/* set state */
	call->state = CHAN_LCR_STATE_OUT_PREPARE;
}

/*
 * call from asterisk
 */
static int lcr_call(struct ast_channel *ast, char *dest, int timeout)
{
        struct lcr_pvt *lcr=ast->tech_pvt;

        if (!lcr) return -1;

        char buf[128];
        char *port_str, *dad, *p;

	hier muss noch
        ast_copy_string(buf, dest, sizeof(buf)-1);
        p=buf;
        port_str=strsep(&p, "/");
        dad=strsep(&p, "/");

	/* send setup message, if we already have a callref */
	if (call->ref)
		send_setup_to_lcr(call);

        if (lcr_debug)
                ast_verbose("Call: ext:%s dest:(%s) -> dad(%s) \n", ast->exten,dest, dad);


	return 0; 
}

static int lcr_digit(struct ast_channel *ast, char digit)
{
	char buf[]="x";

	/* only pass IA5 number space */
	if (digit > 126 || digit < 32)
		return 0;

	/* send information or queue them */
	if (call->ref && call->state == CHAN_LCR_STATE_OUT_DIALING)
	{
		send_dialing_to_lcr(call);
	} else
	if (!call->ref
	 && (call->state == CHAN_LCR_STATE_OUT_PREPARE || call->state == CHAN_LCR_STATE_OUT_SETUP));
	{
		*buf = digit;
		strncat(call->dialque, buf, strlen(char->dialque)-1);
	} else
	{
digits kommen, koennen aber nicht verwendet werden.
	sollen wir sie als info senden (im connect zb.)
	}
}

static int lcr_answer(struct ast_channel *c)
{
        struct lcr_pvt *lcr=c->tech_pvt;
        return 0;
}

static int lcr_hangup(struct ast_channel *ast)
{
        struct chan_call *call = ast->tech_pvt;

	/* disconnect asterisk, maybe not required */
	ast->tech_pvt = NULL;
	if (ref)
	{
		/* release */
		memset(&newparam, 0, sizeof(union parameter));
		newparam.disconnectinfo.cause = CAUSE_RESSOURCEUNAVAIL;
		newparam.disconnectinfo.location = LOCATION_PRIVATE_LOCAL;
		send_message(MESSAGE_RELEASE, call->ref, &newparam);
		/* remove call */
		free_call(call);
		return;
	} else
	{
		/* ref is not set, due to prepare setup or release */
		if (call->state == CHAN_LCR_STATE_RELEASE)
		{
			/* we get the response to our release */
			free_call(call);
		} else
		{
			/* during prepare, we change to release state */
			call->state = CHAN_LCR_STATE_RELEASE;
		}
	} 
}

static int lcr_write(struct ast_channel *c, struct ast_frame *f)
{
        struct lcr_pvt *lcrm= c->tech_pvt;
}


static struct ast_frame *lcr_read(struct ast_channel *c)
{
        struct lcr_pvt *lcr = c->tech_pvt;
}

static int lcr_indicate(struct ast_channel *c, int cond, const void *data, size_t datalen)
{
        int res = -1;

        switch (cond) {
                case AST_CONTROL_BUSY:
                case AST_CONTROL_CONGESTION:
                case AST_CONTROL_RINGING:
                        return -1;
                case -1:
                        return 0;

                case AST_CONTROL_VIDUPDATE:
                        res = -1;
                        break;
                case AST_CONTROL_HOLD:
                        ast_verbose(" << Console Has Been Placed on Hold >> \n");
                        //ast_moh_start(c, data, g->mohinterpret);
                        break;
                case AST_CONTROL_UNHOLD:
                        ast_verbose(" << Console Has Been Retrieved from Hold >> \n");
                        //ast_moh_stop(c);
                        break;

                default:
                        ast_log(LOG_WARNING, "Don't know how to display condition %d on %s\n", cond, c->name);
                        return -1;
        }

        return 0;
}

static struct ast_channel_tech lcr_tech = {
	.type="lcr",
	.description="Channel driver for connecting to Linux-Call-Router",
	.capabilities=AST_FORMAT_ALAW,
	.requester=lcr_request,
	.send_digit=lcr_digit,
	.call=lcr_call,
//	.bridge=lcr_bridge, 
	.hangup=lcr_hangup,
	.answer=lcr_answer,
	.read=lcr_read,
	.write=lcr_write,
	.indicate=lcr_indicate,
//	.fixup=lcr_fixup,
//	.send_text=lcr_send_text,
	.properties=0
};

/*
 * cli
 */
static int lcr_show_lcr (int fd, int argc, char *argv[])
{
}

static int lcr_show_calls (int fd, int argc, char *argv[])
{
}

static int lcr_reload_routing (int fd, int argc, char *argv[])
{
}

static int lcr_reload_interfaces (int fd, int argc, char *argv[])
{
}

static int lcr_port_block (int fd, int argc, char *argv[])
{
}

static int lcr_port_unblock (int fd, int argc, char *argv[])
{
}

static int lcr_port_unload (int fd, int argc, char *argv[])
{
}

static struct ast_cli_entry cli_show_lcr =
{ {"lcr", "show", "lcr", NULL},
 lcr_show_lcr,
 "Shows current states of LCR core",
 "Usage: lcr show lcr\n",
};

static struct ast_cli_entry cli_show_calls =
{ {"lcr", "show", "calls", NULL},
 lcr_show_calls,
 "Shows current calls made by LCR and Asterisk",
 "Usage: lcr show calls\n",
};

static struct ast_cli_entry cli_reload_routing =
{ {"lcr", "reload", "routing", NULL},
 lcr_reload_routing,
 "Reloads routing conf of LCR, current uncomplete calls will be disconnected",
 "Usage: lcr reload routing\n",
};

static struct ast_cli_entry cli_reload_interfaces =
{ {"lcr", "reload", "interfaces", NULL},
 lcr_reload_interfaces,
 "Reloads interfaces conf of LCR",
 "Usage: lcr reload interfaces\n",
};

static struct ast_cli_entry cli_port_block =
{ {"lcr", "port", "block", NULL},
 lcr_port_block,
 "Blocks LCR port for further calls",
 "Usage: lcr port block \"<port>\"\n",
};

static struct ast_cli_entry cli_port_unblock =
{ {"lcr", "port", "unblock", NULL},
 lcr_port_unblock,
 "Unblocks or loads LCR port, port is opened my mISDN",
 "Usage: lcr port unblock \"<port>\"\n",
};

static struct ast_cli_entry cli_port_unload =
{ {"lcr", "port", "unload", NULL},
 lcr_port_unload,
 "Unloads LCR port, port is closes by mISDN",
 "Usage: lcr port unload \"<port>\"\n",
};


/*
 * module loading and destruction
 */
int load_module(void)
{
//	ast_mutex_init(&release_lock);

//	lcr_cfg_update_ptp();

	if (!(lcr_sock = open_socket())) {
		ast_log(LOG_ERROR, "Unable to connect\n");
		lcr_sock = -1;
		/* continue with closed socket */
	}

	if (!bchannel_initialize()) {
		ast_log(LOG_ERROR, "Unable to open mISDN device\n");
		return -1;
	}
	mISDN_created = 1;

	if (ast_channel_register(&lcr_tech)) {
		ast_log(LOG_ERROR, "Unable to register channel class\n");
		return -1;
	}
 
#if 0	
	ast_cli_register(&cli_show_lcr);
	ast_cli_register(&cli_show_calls);

	ast_cli_register(&cli_reload_routing);
	ast_cli_register(&cli_reload_interfaces);
	ast_cli_register(&cli_port_block);
	ast_cli_register(&cli_port_unblock);
	ast_cli_register(&cli_port_unload);
  
	ast_register_application("misdn_set_opt", misdn_set_opt_exec, "misdn_set_opt",
				 "misdn_set_opt(:<opt><optarg>:<opt><optarg>..):\n"
				 "Sets mISDN opts. and optargs\n"
				 "\n"
				 "The available options are:\n"
				 "    d - Send display text on called phone, text is the optparam\n"
				 "    n - don't detect dtmf tones on called channel\n"
				 "    h - make digital outgoing call\n" 
				 "    c - make crypted outgoing call, param is keyindex\n"
				 "    e - perform echo cancelation on this channel,\n"
				 "        takes taps as arguments (32,64,128,256)\n"
				 "    s - send Non Inband DTMF as inband\n"
				 "   vr - rxgain control\n"
				 "   vt - txgain control\n"
		);

	
	lcr_cfg_get( 0, LCR_GEN_TRACEFILE, global_tracefile, BUFFERSIZE);

	chan_lcr_log(0, 0, "-- mISDN Channel Driver Registred -- (BE AWARE THIS DRIVER IS EXPERIMENTAL!)\n");
=======
	//lcr_cfg_get( 0, LCR_GEN_TRACEFILE, global_tracefile, BUFFERSIZE);
#endif

	return 0;
}

int unload_module(void)
{
	/* First, take us out of the channel loop */
	ast_log(LOG_VERBOSE, "-- Unregistering mISDN Channel Driver --\n");
	
	
	ast_channel_unregister(&lcr_tech);

	if (mISDN_created) {
		bchannel_deinitialize();
		mISDN_created = 0;
	}

	if (lcr_sock >= 0) {
		close(lcr_sock);
		lcr_sock = -1;
	}

	return 0;
}

static int reload_module(void)
{
//	reload_config();
	return 0;
}


ast_mutex_t usecnt_lock;
int usecnt;

int usecount(void)
{
	int res;
	ast_mutex_lock(&usecnt_lock);
	res = usecnt;
	ast_mutex_unlock(&usecnt_lock);
	return res;
}


char *desc="Channel driver for lcr";

char *description(void)
{
	return desc;
}

char *key(void)
{
	return ASTERISK_GPL_KEY;
}

#define AST_MODULE "chan_lcr"
AST_MODULE_INFO(ASTERISK_GPL_KEY,
                                AST_MODFLAG_DEFAULT,
                                "Channel driver for LCR",
                                .load = load_module,
                                .unload = unload_module,
                                .reload = reload_module,
                           );

