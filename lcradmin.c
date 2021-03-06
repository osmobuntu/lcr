/*****************************************************************************\
**                                                                           **
** Linux Call Router                                                         **
**                                                                           **
**---------------------------------------------------------------------------**
** Copyright: Andreas Eversberg                                              **
**                                                                           **
** Administration tool                                                       **
**                                                                           **
\*****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <errno.h>
#include <curses.h>
#include "macro.h"
#include "options.h"
#include "join.h"
#include "select.h"
#include "joinpbx.h"
#include "extension.h"
#include "message.h"
#include "lcrsocket.h"
#include "cause.h"

#define LTEE {addch(ACS_LTEE);addch(ACS_HLINE);addch(ACS_HLINE);}
#define LLCORNER {addch(ACS_LLCORNER);addch(ACS_HLINE);addch(ACS_HLINE);}
#define VLINE {addch(ACS_VLINE);addstr("  ");}
#define EMPTY {addstr("   ");}
//char rotator[] = {'-', '\\', '|', '/'};
int	lastlines, lastcols;
int	show_interfaces = 2,
	show_calls = 1,
	show_log = 1;

enum {
	MODE_STATE,
	MODE_PORTINFO,
	MODE_INTERFACE,
	MODE_ROUTE,
	MODE_DIAL,
	MODE_RELEASE,
	MODE_UNBLOCK,
	MODE_BLOCK,
	MODE_UNLOAD,
	MODE_TESTCALL,
	MODE_TRACE,
};

const char *text_interfaces[] = {
	"off",
	"brief",
	"active channels",
	"all channels",
};

const char *text_calls[] = {
	"off",
	"brief",
	"structured",
};

char	red = 1,
	green = 2,
	yellow = 3,
	blue = 4,
	mangenta = 5,
	cyan = 6,
	white = 7;

#define LOGLINES 128
char logline[LOGLINES][512];
unsigned int logcur = 0;
int logfh = -1;
char logfile[128];

/*
 * curses
 */
void init_curses(void)
{
	/* init curses */
        initscr(); cbreak(); noecho();
        start_color();
        nodelay(stdscr, TRUE);
        if (COLOR_PAIRS>=8 && COLORS>=8)
        {
                init_pair(1,1,0);
                init_pair(2,2,0);
                init_pair(3,3,0);
                init_pair(4,4,0);
                init_pair(5,5,0);
                init_pair(6,6,0);
                init_pair(7,7,0);
        }
	lastlines = LINES;
	lastcols = COLS;
}

void cleanup_curses(void)
{
        endwin();
}

void color(int color)
{
        if (COLOR_PAIRS>=8 && COLORS>=8)
		attrset(COLOR_PAIR(color));
}

/*
 * permanently show current state using ncurses
 */
int debug_port(struct admin_message *msg, struct admin_message *m, int line, int i, int vline)
{
	char buffer[256];

	color(white);
	addstr("PORT:");
	color(yellow);
	SPRINT(buffer,"%s(%d)", m[i].u.p.name,m[i].u.p.serial);
	addstr(buffer);
	color(cyan);
	addstr(" state=");
	switch (m[i].u.p.state) {
		case ADMIN_STATE_IDLE:
		color(red);
		addstr("'idle'");
		break;
		case ADMIN_STATE_IN_SETUP:
		color(red);
		addstr("'in << setup'");
		break;
		case ADMIN_STATE_OUT_SETUP:
		color(red);
		addstr("'out >> setup'");
		break;
		case ADMIN_STATE_IN_OVERLAP:
		color(yellow);
		addstr("'in << overlap'");
		break;
		case ADMIN_STATE_OUT_OVERLAP:
		color(yellow);
		addstr("'out >> overlap'");
		break;
		case ADMIN_STATE_IN_PROCEEDING:
		color(mangenta);
		addstr("'in << proc'");
		break;
		case ADMIN_STATE_OUT_PROCEEDING:
		color(mangenta);
		addstr("'out >> proc'");
		break;
		case ADMIN_STATE_IN_ALERTING:
		color(cyan);
		addstr("'in << alert'");
		break;
		case ADMIN_STATE_OUT_ALERTING:
		color(cyan);
		addstr("'out >> alert'");
		break;
		case ADMIN_STATE_CONNECT:
		color(white);
		addstr("'connect'");
		break;
		case ADMIN_STATE_IN_DISCONNECT:
		color(blue);
		addstr("'in  << disc'");
		break;
		case ADMIN_STATE_OUT_DISCONNECT:
		color(blue);
		addstr("'out >> disc'");
		break;
		case ADMIN_STATE_RELEASE:
		color(blue);
		addstr("'release'");
		break;
		default:
		color(blue);
		addstr("'--NONE--'");
	}

	if (m[i].u.p.isdn) {	
		color(cyan);
		addstr(" bchannel=");
		color(white);
		SPRINT(buffer,"%d", m[i].u.p.isdn_chan);
		addstr(buffer);
		if (m[i].u.p.isdn_ces >= 0) {
			color(cyan);
			addstr(" ces=");
			color(yellow);
			SPRINT(buffer, "%d", m[i].u.p.isdn_ces);
			addstr(buffer);
		}
		if (m[i].u.p.isdn_hold) {
			color(red);
			addstr(" hold");
		}
	}

	return(line);
}
int debug_epoint(struct admin_message *msg, struct admin_message *m, int line, int i, int vline)
{
	unsigned int epoint = m[i].u.e.serial;
	char buffer[256];
	unsigned char c;
	int j, jj;
	int ltee;

	color(white);
	SPRINT(buffer,"EPOINT(%d)", epoint);
	addstr(buffer);
	color(cyan);
	addstr(" state=");
	switch (m[i].u.e.state) {
		case ADMIN_STATE_IDLE:
		color(red);
		addstr("'idle'");
		break;
		case ADMIN_STATE_IN_SETUP:
		color(red);
		addstr("'in << setup'");
		break;
		case ADMIN_STATE_OUT_SETUP:
		color(red);
		addstr("'out >> setup'");
		break;
		case ADMIN_STATE_IN_OVERLAP:
		color(yellow);
		addstr("'in << overlap'");
		break;
		case ADMIN_STATE_OUT_OVERLAP:
		color(yellow);
		addstr("'out >> overlap'");
		break;
		case ADMIN_STATE_IN_PROCEEDING:
		color(mangenta);
		addstr("'in << proc'");
		break;
		case ADMIN_STATE_OUT_PROCEEDING:
		color(mangenta);
		addstr("'out >> proc'");
		break;
		case ADMIN_STATE_IN_ALERTING:
		color(cyan);
		addstr("'in << alert'");
		break;
		case ADMIN_STATE_OUT_ALERTING:
		color(cyan);
		addstr("'out >> alert'");
		break;
		case ADMIN_STATE_CONNECT:
		color(white);
		addstr("'connect'");
		break;
		case ADMIN_STATE_IN_DISCONNECT:
		color(blue);
		addstr("'in  << disc'");
		break;
		case ADMIN_STATE_OUT_DISCONNECT:
		color(blue);
		addstr("'out >> disc'");
		break;
		default:
		color(blue);
		addstr("'--NONE--'");
	}
	if (m[i].u.e.terminal[0]) {
		color(cyan);
		addstr(" terminal=");
		color(green);
		addstr(m[i].u.e.terminal);
	}
	color(white);
	SPRINT(buffer, " %s", m[i].u.e.callerid);
	addstr(buffer);
	color(cyan);
	addstr("->");
	color(white);
	addstr(m[i].u.e.dialing);
	if (m[i].u.e.action[0]) {
		color(cyan);
		addstr(" action=");
		color(yellow);
		addstr(m[i].u.e.action);
	}
	if (m[i].u.e.park) {
		color(cyan);
		addstr(" park="); /* 9 digits */
		color(green);
		UCPY(buffer, "\""); /* 9 digits */
		j = 0;
		jj = m[i].u.e.park_len;
		while(j < jj) {
			c = m[i].u.e.park_callid[j];
			if (c >= 32 && c < 127 && c != '[') {
				SCCAT(buffer, c);
			} else
				UPRINT(buffer+strlen(buffer), "[%02x]", c);
			j++;
		}
		SCAT(buffer, "\"");
		addstr(buffer);
	} else {
		color(red);
		switch(m[i].u.e.rx_state) {
			case NOTIFY_STATE_SUSPEND:
			addstr(" in=suspend");
			break;
			case NOTIFY_STATE_HOLD:
			addstr(" in=hold");
			break;
			case NOTIFY_STATE_CONFERENCE:
			addstr(" in=conference");
			break;
		}
		switch(m[i].u.e.tx_state) {
			case NOTIFY_STATE_SUSPEND:
			addstr(" out=suspend");
			break;
			case NOTIFY_STATE_HOLD:
			addstr(" out=hold");
			break;
			case NOTIFY_STATE_CONFERENCE:
			addstr(" out=conference");
			break;
		}
	}
	if (m[i].u.e.crypt) {
		color(cyan);
		addstr(" crypt=");
		if (m[i].u.e.crypt) { /* crypt on */
			color(green);
			addstr("active");
		} else {
			color(yellow);
			addstr("pending");
		}
	}
	/* loop all related ports */
	ltee = 0;
	j = msg->u.s.interfaces+msg->u.s.remotes+msg->u.s.joins+msg->u.s.epoints;
	jj = j + msg->u.s.ports;
	while(j < jj) {
		if (m[j].u.p.epoint == epoint) {
			color(cyan);
			move(++line>1?line:1, 1);
			if (vline)
				VLINE
			else
				EMPTY
			move(line>1?line:1, 5);
			LTEE
			ltee = line;
			move(line>1?line:1, 8);
			if (line+2 >= LINES) break;
			line = debug_port(msg, m, line, j, vline);
			if (line+2 >= LINES) break;
		}
		j++;
	}
	if (ltee) {
		color(cyan);
		move(ltee>1?line:1, 5);
		LLCORNER
	}

	return(line);
}
int debug_join(struct admin_message *msg, struct admin_message *m, int line, int i)
{
	unsigned int	join = m[i].u.j.serial;
	char		buffer[256];
	int		j, jj;

	color(white);
	SPRINT(buffer,"JOIN(%d)", join);
	addstr(buffer);
	if (m[i].u.j.partyline) {
		color(cyan);
		addstr(" partyline=");
		color(white);
		SPRINT(buffer, "%d\n", m[i].u.j.partyline);
		addstr(buffer);
	}
	if (m[i].u.j.remote[0]) {
		color(cyan);
		addstr(" remote=");
		color(white);
		SPRINT(buffer, "%s\n", m[i].u.j.remote);
		addstr(buffer);
	}
	/* find number of epoints */
	j = msg->u.s.interfaces+msg->u.s.remotes+msg->u.s.joins;
	jj = j + msg->u.s.epoints;
	i = 0;
	while(j < jj) {
		if (m[j].u.e.join == join)
			i++;
		j++;
	}
	/* loop all related endpoints */
	j = msg->u.s.interfaces+msg->u.s.remotes+msg->u.s.joins;
	jj = j + msg->u.s.epoints;
	while(j < jj) {
		if (m[j].u.e.join == join) {
			i--;
			move(++line>1?line:1, 1);
			color(cyan);
			if (i)
				LTEE
			else
				LLCORNER
			move(line>1?line:1, 4);
			if (line+2 >= LINES) break;
			line = debug_epoint(msg, m, line, j, i?1:0);
			if (line+2 >= LINES) break;
		}
		j++;
	}

	return(line);
}

const char *admin_state(int sock, char *argv[])
{
	struct admin_message	msg,
				*m;
	char			buffer[512],
				*p;
	int			line, offset = 0, hoffset = 0;
	int			i, ii, j, jj, k;
	unsigned int		l, ll;
	int			num;
	int			len;
	int			off;
	int			ltee;
	int			anything;
	int			enter = 0;
	char			enter_string[128] = "", ch;
	fd_set			select_rfds;
	struct timeval		select_tv;

	/* flush logfile name */
	logfile[0] = '\0';

	/* init curses */
	init_curses();

	again:
	/* send reload command */
	memset(&msg, 0, sizeof(msg));
	msg.message = ADMIN_REQUEST_STATE;
//	printf("sizeof=%d\n",sizeof(msg));fflush(stdout);
	if (write(sock, &msg, sizeof(msg)) != sizeof(msg)) {
		cleanup_curses();
		return("Broken pipe while sending command.");
	}

	/* receive response */
	if (read(sock, &msg, sizeof(msg)) != sizeof(msg)) {
		cleanup_curses();
		return("Broken pipe while receiving response.");
	}

	if (msg.message != ADMIN_RESPONSE_STATE) {
		cleanup_curses();
		return("Response not valid. Expecting state response.");
	}
	num = msg.u.s.interfaces + msg.u.s.remotes + msg.u.s.joins + msg.u.s.epoints + msg.u.s.ports;
	m = (struct admin_message *)MALLOC(num*sizeof(struct admin_message));
	off=0;
	if (num) {
		readagain:
		if ((len = read(sock, ((unsigned char *)(m))+off, num*sizeof(struct admin_message)-off)) != num*(int)sizeof(struct admin_message)-off) {
			if (len <= 0) {
				FREE(m, 0);
	//			fprintf(stderr, "got=%d expected=%d\n", i, num*sizeof(struct admin_message));
				cleanup_curses();
				return("Broken pipe while receiving state infos.");
			}
			if (len < num*(int)sizeof(struct admin_message)) {
				off+=len;
				goto readagain;
			}
		}
	}
	j = 0;
	i = 0;
//	fprintf("getting =%d interfaces\n", msg.u.s.interfaces);
	while(i < msg.u.s.interfaces) {
//		fprintf(stderr, "j=%d message=%d\n", j, m[j].message);
		if (m[j].message != ADMIN_RESPONSE_S_INTERFACE) {
			FREE(m, 0);
			cleanup_curses();
			return("Response not valid. Expecting interface information.");
		}
		i++;
		j++;
	}
	i = 0;
	while(i < msg.u.s.remotes) {
		if (m[j].message != ADMIN_RESPONSE_S_REMOTE) {
			FREE(m, 0);
			cleanup_curses();
			return("Response not valid. Expecting remote application information.");
		}
		i++;
		j++;
	}
	i = 0;
	while(i < msg.u.s.joins) {
		if (m[j].message != ADMIN_RESPONSE_S_JOIN) {
			FREE(m, 0);
			cleanup_curses();
			return("Response not valid. Expecting join information.");
		}
		i++;
		j++;
	}
	i = 0;
	while(i < msg.u.s.epoints) {
		if (m[j].message != ADMIN_RESPONSE_S_EPOINT) {
			FREE(m, 0);
			cleanup_curses();
			return("Response not valid. Expecting endpoint information.");
		}
		i++;
		j++;
	}
	i = 0;
	while(i < msg.u.s.ports) {
		if (m[j].message != ADMIN_RESPONSE_S_PORT) {
			FREE(m, 0);
			cleanup_curses();
			return("Response not valid. Expecting port information.");
		}
		i++;
		j++;
	}
	// now j is the number of message blocks

	/* display start */
	erase();

	line = 1-offset; 

	/* change log */
	if (!!strcmp(logfile, msg.u.s.logfile)) {
		SCPY(logfile, msg.u.s.logfile);
		if (logfh >= 0)
			close(logfh);
		i = 0;
		ii = LOGLINES;
		while(i < ii) {
			logline[i][0] = '~';
			logline[i][1] = '\0';
			i++;
		}
		logcur = 0;
		logfh = open(logfile, O_RDONLY|O_NONBLOCK);
		if (logfh >= 0) {
			/* seek at the end -8000 chars */
			lseek(logfh, -8000, SEEK_END);
			/* if not at the beginning, read until endofline */
			logline[logcur % LOGLINES][0] = '\0';
			l = read(logfh, logline[logcur % LOGLINES], sizeof(logline[logcur % LOGLINES])-1);
			if (l > 0) {
				/* read first line and skip junk */
				logline[logcur % LOGLINES][l] = '\0';
				if ((p = strchr(logline[logcur % LOGLINES],'\n'))) {
					logcur++;
					SCPY(logline[logcur % LOGLINES], p+1);
					SCPY(logline[(logcur-1) % LOGLINES], "...");
				}
				goto finish_line;
			}
		}
	}

	/* read log */
	if (logfh >= 0) {
		while(42) {
			ll = strlen(logline[logcur % LOGLINES]);
			l = read(logfh, logline[logcur % LOGLINES]+ll, sizeof(logline[logcur % LOGLINES])-ll-1);
			if (l<=0)
				break;
			logline[logcur % LOGLINES][ll+l] = '\0';
			finish_line:
			/* put data to lines */
			while ((p = strchr(logline[logcur % LOGLINES],'\n'))) {
				*p = '\0';
				logcur++;
				SCPY(logline[logcur % LOGLINES], p+1);
			}
			/* if line is full without return, go next line */
			if (strlen(logline[logcur % LOGLINES]) == sizeof(logline[logcur % LOGLINES])-1) {
				logcur++;
				logline[logcur % LOGLINES][0] = '\0';
			}
		}
	}

	/* display interfaces */
	if (show_interfaces > 0) {
		anything = 0;
		i = 0;
		ii = i + msg.u.s.interfaces;
		while(i < ii) {
			/* show interface summary */
			move(++line>1?line:1, 0);
			color(white);
			if (m[i].u.i.portnum == -100) {
				SPRINT(buffer, "%s %s", m[i].u.i.interface_name, (m[i].u.i.extension)?" exten":"");
				addstr(buffer);
			} else if (m[i].u.i.block >= 2) {
				if (m[i].u.i.portnum < 0)
					SPRINT(buffer, "%s (port ?: %s)%s", m[i].u.i.interface_name, m[i].u.i.portname, (m[i].u.i.extension)?" exten":"");
				else
					SPRINT(buffer, "%s (port %d: %s)%s", m[i].u.i.interface_name, m[i].u.i.portnum, m[i].u.i.portname, (m[i].u.i.extension)?" extension":"");
				addstr(buffer);
				color(red);
				addstr("  not loaded");
			} else {
				SPRINT(buffer, "%s", m[i].u.i.interface_name);
				addstr(buffer);
				color(yellow);
				SPRINT(buffer, "(port %d: %s)", m[i].u.i.portnum, m[i].u.i.portname);
				addstr(buffer);
				color(cyan);
				SPRINT(buffer, " %s %s%s%s%s", (m[i].u.i.ntmode)?"NT":"TE", (m[i].u.i.ptp)?"ptp":"ptmp", (m[i].u.i.l1hold)?" l1hold":"", (m[i].u.i.l2hold)?" l2hold":"", (m[i].u.i.extension)?" extension":"");
				addstr(buffer);
				if (m[i].u.i.use)
					color(green);
				else
					color(blue);
				SPRINT(buffer, " use:%d", m[i].u.i.use);
				addstr(buffer);
				if (m[i].u.i.ptp || !m[i].u.i.ntmode) {
					color((m[i].u.i.l2link > 0)?green:red);
					if (m[i].u.i.l2link < 0)
						addstr("  L2 unkn");
					else
						addstr((m[i].u.i.l2link)?"  L2 UP":"  L2 down");
				} else {
					k = 0;
					color(green);
					j = 0;
					while(j < 128) {
						if (m[i].u.i.l2mask[j>>3] & (1 << (j&7))) {
							SPRINT(buffer, "%s%d", k?",":"  TEI(", j);
							addstr(buffer);
							k = 1;
						}
						j++;
					}
					if (k)
						addstr(")");
				}
				color((m[i].u.i.l1link > 0)?green:blue);
				if (m[i].u.i.l1link < 0)
					addstr("  L1 unkn");
				else
					addstr((m[i].u.i.l1link)?"  L1 UP":"  L1 down");
				if (m[i].u.i.los) {
					color(red);
					addstr(" LOS");
				}
				if (m[i].u.i.ais) {
					color(red);
					addstr(" AIS");
				}
				if (m[i].u.i.rdi) {
					color(red);
					addstr(" RDI");
				}
				if (m[i].u.i.slip_tx || m[i].u.i.slip_rx) {
					color(red);
					SPRINT(buffer, " SLIP(tx:%d rx:%d)", m[i].u.i.slip_tx, m[i].u.i.slip_rx);
					addstr(buffer);
				}
				if (m[i].u.i.block) {
					color(red);
					addstr("  blocked");
				}
				if (line+2 >= LINES) goto end;
				/* show channels */
				if (show_interfaces > 1) {
					ltee = 0;
					j = k =0;
					jj = m[i].u.i.channels;
					while(j < jj) {
						/* show all channels */
						if (show_interfaces>2 || m[i].u.i.busy[j]>0) {
							color(cyan);
							/* show left side / right side */
							if ((k & 1) && (COLS > 70)) {
								move(line>1?line:1,4+((COLS-4)/2));
							} else {
								move(++line>1?line:1, 1);
								LTEE
								ltee = 1;
							}
							k++;
							color(white);
							if (m[i].u.i.pri)
								SPRINT(buffer,"S%2d: ", j+1+(j>=15));
							else
								SPRINT(buffer,"B%2d: ", j+1);
							if (m[i].u.i.mode[j] == B_MODE_HDLC)
								SCAT(buffer,"HDLC ");
							addstr(buffer);
							switch(m[i].u.i.busy[j]) {
								case B_STATE_IDLE:
								if ((!m[i].u.i.l2link && m[i].u.i.ptp) || m[i].u.i.block) {
									color(red);
									addstr("blocked ");
								} else {
									color(blue);
									addstr("idle    ");
								}
								break;
								case B_STATE_ACTIVATING:
								color(yellow);
								addstr("act'ing ");
								break;
								case B_STATE_ACTIVE:
								color(green);
								addstr("active  ");
								break;
								case B_STATE_DEACTIVATING:
								color(yellow);
								addstr("dact'ing");
								break;
								case B_STATE_EXPORTING:
								color(yellow);
								addstr("exp'ing ");
								break;
								case B_STATE_REMOTE:
								color(green);
								addstr("remote  ");
								break;
								case B_STATE_IMPORTING:
								color(yellow);
								addstr("imp'ing ");
								break;
							}
							if (m[i].u.i.port[j]) {
								/* search for port */
								l = msg.u.s.interfaces+msg.u.s.remotes+msg.u.s.joins+msg.u.s.epoints;
								ll = l+msg.u.s.ports;
								while(l < ll) {
									if (m[l].u.p.serial == m[i].u.i.port[j]) {
										SPRINT(buffer, " %s(%ld)", m[l].u.p.name, m[l].u.p.serial);
										addstr(buffer);
									}
									l++;
								}
							}
							if (line+2 >= LINES) {
								if (ltee) {
									color(cyan);
									move(line>1?line:1, 1);
									LLCORNER
								}
								goto end;
							}
						}
						j++;
					}
					if (ltee) {
						color(cyan);
						move(line>1?line:1, 1);
						LLCORNER
					}
					if (line+2 >= LINES) goto end;
					/* show summary if no channels were shown */
					if (show_interfaces<2 && ltee==0) {
						color(cyan);
						move(++line>1?line:1, 1);
						LLCORNER
							
						if (m[i].u.i.l2link && m[i].u.i.block==0) {
							color(green);
							SPRINT(buffer,"all %d channels free", m[i].u.i.channels);
						} else {
							color(red);
							SPRINT(buffer,"all %d channels blocked", m[i].u.i.channels);
						}
						addstr(buffer);
					}
					if (line+2 >= LINES) goto end;
				}
			}
			i++;
			anything = 1;
		}
		i = msg.u.s.interfaces;
		ii = i + msg.u.s.remotes;
		while(i < ii) {
			/* show remote summary */
			move(++line>1?line:1, 0);
			color(white);
			SPRINT(buffer, "Remote: %s", m[i].u.r.name);
			addstr(buffer);
			i++;
		}
		if (anything)
			line++;
		if (line+2 >= LINES) goto end;
	}		
	/* display calls (brief) */
	if (show_calls == 1) {
		anything = 0;
		i = msg.u.s.interfaces+msg.u.s.remotes+msg.u.s.joins;
		ii = i+msg.u.s.epoints;
		while(i < ii) {
			/* for each endpoint... */
			if (!m[i].u.e.join) {
				move(++line>1?line:1, 0);
				color(white);
				SPRINT(buffer, "(%d): ", m[i].u.e.serial);
				addstr(buffer);
				color(cyan);
				if (m[i].u.e.terminal[0]) {
					addstr("intern=");
					color(green);
					addstr(m[i].u.e.terminal);
				} else
					addstr("extern");
				color(white);
				SPRINT(buffer, " %s", m[i].u.e.callerid);
				addstr(buffer);
				color(cyan);
				addstr("->");
				color(white);
				SPRINT(buffer, "%s", m[i].u.e.dialing);
				addstr(buffer);
				if (m[i].u.e.action[0]) {
					color(cyan);
					addstr(" action=");
					color(yellow);
					addstr(m[i].u.e.action);
				}
				if (line+2 >= LINES) goto end;
			}
			i++;
			anything = 1;
		}
		j = msg.u.s.interfaces+msg.u.s.remotes;
		jj = j+msg.u.s.joins;
		while(j < jj) {
			/* for each call... */
			move(++line>1?line:1, 0);
			color(white);
			SPRINT(buffer, "(%d):", m[j].u.j.serial);
			addstr(buffer);
			i = msg.u.s.interfaces+msg.u.s.remotes+msg.u.s.joins;
			ii = i+msg.u.s.epoints;
			while(i < ii) {
				/* for each endpoint... */
				if (m[i].u.e.join == m[j].u.j.serial) {
					color(white);
					SPRINT(buffer, " (%d)", m[i].u.e.serial);
					addstr(buffer);
					color(cyan);
					if (m[i].u.e.terminal[0]) {
						addstr("int=");
						color(green);
						addstr(m[i].u.e.terminal);
					} else
						addstr("ext");
					color(white);
					SPRINT(buffer, "-%s", m[i].u.e.callerid);
					addstr(buffer);
					color(cyan);
					addstr(">");
					color(white);
					SPRINT(buffer, "%s", m[i].u.e.dialing);
					addstr(buffer);
				}
				i++;
				anything = 1;
			}
			if (line+2 >= LINES) goto end;
			j++;
		}
		if (anything)
			line++;
		if (line+2 >= LINES) goto end;
	}
	/* display calls (structurd) */
	if (show_calls == 2) {
		/* show all ports with no epoint */
		anything = 0;
		i = msg.u.s.interfaces+msg.u.s.remotes+msg.u.s.joins+msg.u.s.epoints;
		ii = i+msg.u.s.ports;
		while(i < ii) {
			if (!m[i].u.p.epoint) {
				move(++line>1?line:1, 8);
				if (line+2 >= LINES) goto end;
				line = debug_port(&msg, m, line, i, 0);
				if (line+2 >= LINES) goto end;
				anything = 1;
			}
			i++;
		}
		if (anything)
			line++;
		if (line+2 >= LINES) goto end;

		/* show all epoints with no call */
		anything = 0;
		i = msg.u.s.interfaces+msg.u.s.remotes+msg.u.s.joins;
		ii = i+msg.u.s.epoints;
		while(i < ii) {
			if (!m[i].u.e.join) {
				move(++line>1?line:1, 4);
				if (line+2 >= LINES) goto end;
				line = debug_epoint(&msg, m, line, i, 0);
				if (line+2 >= LINES) goto end;
				anything = 1;
			}
			i++;
		}
		if (anything)
			line++;
		if (line+2 >= LINES) goto end;

		/* show all joins */
		anything = 0;
		i = msg.u.s.interfaces+msg.u.s.remotes;
		ii = i+msg.u.s.joins;
		while(i < ii) {
			move(++line>1?line:1, 0);
			if (line+2 >= LINES) goto end;
			line = debug_join(&msg, m, line, i);
			if (line+2 >= LINES) goto end;
			i++;
			anything = 1;
		}
		if (anything)
			line++;
		if (line+2 >= LINES) goto end;

	}

	/* show log */
	if (show_log) {
		if (line+2 < LINES) {
			move(line++>1?line-1:1, 0);
			color(blue);
			hline(ACS_HLINE, COLS);
			color(white);
			
			l = logcur-(LINES-line-2);
			ll = logcur;
			if (ll-l >= LOGLINES)
				l = ll-LOGLINES+1;
			while(l!=ll) {
				move(line++>1?line-1:1, 0);
				if ((int)strlen(logline[l % LOGLINES]) > hoffset)
					SCPY(buffer, logline[l % LOGLINES] + hoffset);
				else
					buffer[0] = '\0';
				if (COLS < (int)strlen(buffer)) {
					buffer[COLS-1] = '\0';
					addstr(buffer);
					color(mangenta);
					addch('*');
					color(white);
				} else
					addstr(buffer);
				l++;
			}
		}
	}

	end:
	/* free memory */
	FREE(m, 0);
	/* display name/time */
//	move(0, 0);
//	hline(' ', COLS);
	move(0, 0);
	color(cyan);
	msg.u.s.version_string[sizeof(msg.u.s.version_string)-1] = '\0';
	SPRINT(buffer, "LCR %s", msg.u.s.version_string);
	addstr(buffer);
	if (COLS>50) {
		move(0, COLS-19);
		SPRINT(buffer, "%04d-%02d-%02d %02d:%02d:%02d",
			msg.u.s.tm.tm_year+1900, msg.u.s.tm.tm_mon+1, msg.u.s.tm.tm_mday,
			msg.u.s.tm.tm_hour, msg.u.s.tm.tm_min, msg.u.s.tm.tm_sec);
		addstr(buffer);
	}
	/* displeay head line */
	move(1, 0);
	color(blue);
	hline(ACS_HLINE, COLS);
	if (offset) {
		move(1, 1);
		SPRINT(buffer, "Offset +%d", offset);
		color(red);
		addstr(buffer);
	}
	if (hoffset) {
		move(1, 13);
		SPRINT(buffer, "H-Offset +%d", hoffset);
		color(red);
		addstr(buffer);
	}
	/* display end */
	move(LINES-2, 0);
	color(blue);
	hline(ACS_HLINE, COLS);
	move(LINES-1, 0);
	if (enter) {
		color(white);
		SPRINT(buffer, "-> %s", enter_string);
	} else {
		color(cyan);
		SPRINT(buffer, "i=interfaces '%s'  c=calls '%s'  l=log  q=quit  +-*/=scroll  enter", text_interfaces[show_interfaces], text_calls[show_calls]);
	}
	addstr(buffer);
	refresh();

	/* resize */
	if (lastlines!=LINES || lastcols!=COLS) {
		cleanup_curses();
		init_curses();
		goto again;
	}

	if (enter) {
		/* user input in enter mode */
		ch = getch();
		enter_again:
		if (ch == 10) {
			FILE *fp;

			enter = 0;
			if (!enter_string[0])
				goto again;

			SPRINT(logline[logcur++ % LOGLINES], "> %s", enter_string);
			if (!!strncmp(enter_string, "interface", 10) &&
			    !!strncmp(enter_string, "route", 6) &&
			    !!strncmp(enter_string, "release ", 8) &&
			    !!strncmp(enter_string, "block ", 6) &&
			    !!strncmp(enter_string, "unblock ", 8) &&
			    !!strncmp(enter_string, "load ", 5) &&
			    !!strncmp(enter_string, "unload ", 7)) {
				SPRINT(logline[logcur++ % LOGLINES], "usage:");
				SPRINT(logline[logcur++ % LOGLINES], "interface (reload interface.conf)");
				SPRINT(logline[logcur++ % LOGLINES], "route (reload routing.conf)");
				SPRINT(logline[logcur++ % LOGLINES], "release <EP> (release endpoint with given ID)");
				SPRINT(logline[logcur++ % LOGLINES], "block <port> (block port for further calls)");
				SPRINT(logline[logcur++ % LOGLINES], "unblock/load <port> (unblock port for further calls, load if not loaded)");
				SPRINT(logline[logcur++ % LOGLINES], "unload <port> (unload mISDN stack, release call calls)");
			} else {
				/* applend output to log window */
				SPRINT(buffer, "%s %s", argv[0], enter_string);
				fp = popen(buffer, "r");
				if (fp) {
					while(fgets(logline[logcur % LOGLINES], sizeof(logline[0]), fp))
						logline[logcur++ % LOGLINES][sizeof(logline[0])-1] = '\0';
					pclose(fp);
				} else {
					SPRINT(logline[logcur++ % LOGLINES], "failed to execute '%s'", buffer);
				}
			}
			logline[logcur % LOGLINES][0] = '\0';
			enter_string[0] = '\0';
			goto again;
		}
		if (ch>=32 && ch<=126) {
			SCCAT(enter_string, ch);
			ch = getch();
			if (ch > 0)
				goto enter_again;
			goto again;
		} else
		if (ch==8 || ch==127) {
			if (enter_string[0])
				enter_string[strlen(enter_string)-1] = '\0';
			ch = getch();
			if (ch > 0)
				goto enter_again;
			goto again;
		} else
		if (ch != 3) {
			ch = getch();
			if (ch > 0)
				goto enter_again;
			FD_ZERO(&select_rfds);
			FD_SET(0, &select_rfds);
			select_tv.tv_sec = 0;
			select_tv.tv_usec = 250000;
			select(1, &select_rfds, NULL, NULL, &select_tv);
			goto again;
		}
	} else {
		/* user input in normal mode */
		switch(getch()) {
			case 12: /* refresh */
			cleanup_curses();
			init_curses();
			goto again;
			break;

			case 3: /* abort */
			case 'q':
			case 'Q':
			break;

			case 'i': /* toggle interface */
			show_interfaces++;
			if (show_interfaces > 3) show_interfaces = 0;
			goto again;

			case 'c': /* toggle calls */
			show_calls++;
			if (show_calls > 2) show_calls = 0;
			goto again;

			case 'l': /* toggle log */
			show_log++;
			if (show_log > 1) show_log = 0;
			goto again;

			case '+': /* scroll down */
			offset++;
			goto again;
			
			case '-': /* scroll up */
			if (offset)
				offset--;
			goto again;

			case '*': /* scroll right */
			hoffset += 2;
			goto again;
			
			case '/': /* scroll left */
			if (hoffset)
				hoffset -= 2;
			goto again;

			case 10: /* entermode */
			enter = 1;
			goto again;

			default:
			FD_ZERO(&select_rfds);
			FD_SET(0, &select_rfds);
			select_tv.tv_sec = 0;
			select_tv.tv_usec = 250000;
			select(1, &select_rfds, NULL, NULL, &select_tv);
			goto again;
		}
	}

	/* check for logfh */
	if (logfh >= 0)
		close(logfh);
	logfh = -1;

	/* cleanup curses and exit */
	cleanup_curses();

	return(NULL);
}

const char *admin_portinfo(int sock, int argc, char *argv[])
{
	struct admin_message	msg,
				*m;
	int			i, ii, j;
	int			num;
	int			len;
	int			off;

	/* send state request command */
	memset(&msg, 0, sizeof(msg));
	msg.message = ADMIN_REQUEST_STATE;
	if (write(sock, &msg, sizeof(msg)) != sizeof(msg)) {
		cleanup_curses();
		return("Broken pipe while sending command.");
	}

	/* receive response */
	if (read(sock, &msg, sizeof(msg)) != sizeof(msg)) {
		cleanup_curses();
		return("Broken pipe while receiving response.");
	}

	if (msg.message != ADMIN_RESPONSE_STATE) {
		cleanup_curses();
		return("Response not valid. Expecting state response.");
	}
	num = msg.u.s.interfaces + msg.u.s.remotes + msg.u.s.joins + msg.u.s.epoints + msg.u.s.ports;
	m = (struct admin_message *)MALLOC(num*sizeof(struct admin_message));
	off=0;
	if (num) {
		readagain:
		if ((len = read(sock, ((unsigned char *)(m))+off, num*sizeof(struct admin_message)-off)) != num*(int)sizeof(struct admin_message)-off) {
			if (len <= 0) {
				FREE(m, 0);
				cleanup_curses();
				return("Broken pipe while receiving state infos.");
			}
			if (len < num*(int)sizeof(struct admin_message)) {
				off+=len;
				goto readagain;
			}
		}
	}
	j = 0;
	i = 0;
	while(i < msg.u.s.interfaces) {
		if (m[j].message != ADMIN_RESPONSE_S_INTERFACE) {
			FREE(m, 0);
			cleanup_curses();
			return("Response not valid. Expecting interface information.");
		}
		i++;
		j++;
	}
	i = 0;
	while(i < msg.u.s.remotes) {
		if (m[j].message != ADMIN_RESPONSE_S_REMOTE) {
			FREE(m, 0);
			cleanup_curses();
			return("Response not valid. Expecting remote application information.");
		}
		i++;
		j++;
	}
	i = 0;
	while(i < msg.u.s.joins) {
		if (m[j].message != ADMIN_RESPONSE_S_JOIN) {
			FREE(m, 0);
			cleanup_curses();
			return("Response not valid. Expecting join information.");
		}
		i++;
		j++;
	}
	i = 0;
	while(i < msg.u.s.epoints) {
		if (m[j].message != ADMIN_RESPONSE_S_EPOINT) {
			FREE(m, 0);
			cleanup_curses();
			return("Response not valid. Expecting endpoint information.");
		}
		i++;
		j++;
	}
	i = 0;
	while(i < msg.u.s.ports) {
		if (m[j].message != ADMIN_RESPONSE_S_PORT) {
			FREE(m, 0);
			cleanup_curses();
			return("Response not valid. Expecting port information.");
		}
		i++;
		j++;
	}
	// now j is the number of message blocks

	/* output interfaces */
	i = 0;
	ii = i + msg.u.s.interfaces;
	while(i < ii) {
		if (argc > 2) {
			if (!!strcmp(argv[2], m[i].u.i.interface_name)) {
				i++;
				continue;
			}
		}
		printf("%s:\n", m[i].u.i.interface_name);
		if (m[i].u.i.portnum < 0)
			printf("\t port = unknown\n");
		else
			printf("\t port = %d \"%s\"\n",m[i].u.i.portnum, m[i].u.i.portname);
		printf("\t extension = %s\n", (m[i].u.i.extension)?"yes":"no");
		if (m[i].u.i.block >= 2) {
			printf("\t status = not loaded\n");
		} else {
			if (m[i].u.i.block)
				printf("\t status = blocked\n");
			else
				printf("\t status = unblocked\n");
			printf("\t mode = %s %s%s%s\n", (m[i].u.i.ntmode)?"NT-mode":"TE-mode", (m[i].u.i.ptp)?"ptp":"ptmp", (m[i].u.i.l1hold)?" l1hold":"", (m[i].u.i.l2hold)?" l2hold":"");
			printf("\t out-channel = %s\n", m[i].u.i.out_channel);
			printf("\t in-channel = %s\n", m[i].u.i.in_channel);
			if (m[i].u.i.l1link < 0)
				printf("\t l1 link = unknown\n");
			else
				printf("\t l1 link = %s\n", (m[i].u.i.l1link)?"up":"down");
			if (m[i].u.i.ptp || !m[i].u.i.ntmode) {
				if (m[i].u.i.l2link < 0)
					printf("\t l2 link = unknown\n");
				else
					printf("\t l2 link = %s\n", (m[i].u.i.l2link)?"up":"down");
			}
			printf("\t usage = %d\n", m[i].u.i.use);
		}
		i++;
	}

	/* free memory */
	FREE(m, 0);

	return(NULL);
}


/*
 * Send command and show error message.
 */
const char *admin_cmd(int sock, int mode, char *extension, char *number)
{
	static struct admin_message msg;

	/* send reload command */
	memset(&msg, 0, sizeof(msg));
	switch(mode) {
		case MODE_INTERFACE:
		msg.message = ADMIN_REQUEST_CMD_INTERFACE;
		break;
		case MODE_ROUTE:
		msg.message = ADMIN_REQUEST_CMD_ROUTE;
		break;
		case MODE_DIAL:
		msg.message = ADMIN_REQUEST_CMD_DIAL;
		SPRINT(msg.u.x.message, "%s:%s", extension?:"", number?:"");
		break;
		case MODE_RELEASE:
		msg.message = ADMIN_REQUEST_CMD_RELEASE;
		SCPY(msg.u.x.message, number);
		break;
		case MODE_UNBLOCK:
		msg.message = ADMIN_REQUEST_CMD_BLOCK;
		msg.u.x.portnum = atoi(number);
		msg.u.x.block = 0;
		break;
		case MODE_BLOCK:
		msg.message = ADMIN_REQUEST_CMD_BLOCK;
		msg.u.x.portnum = atoi(number);
		msg.u.x.block = 1;
		break;
		case MODE_UNLOAD:
		msg.message = ADMIN_REQUEST_CMD_BLOCK;
		msg.u.x.portnum = atoi(number);
		msg.u.x.block = 2;
		break;
	}

	if (write(sock, &msg, sizeof(msg)) != sizeof(msg))
		return("Broken pipe while sending command.");

	/* receive response */
	if (read(sock, &msg, sizeof(msg)) != sizeof(msg))
		return("Broken pipe while receiving response.");
	switch(mode) {
		case MODE_INTERFACE:
		if (msg.message != ADMIN_RESPONSE_CMD_INTERFACE)
			return("Response not valid.");
		break;
		case MODE_ROUTE:
		if (msg.message != ADMIN_RESPONSE_CMD_ROUTE)
			return("Response not valid.");
		break;
		case MODE_DIAL:
		if (msg.message != ADMIN_RESPONSE_CMD_DIAL)
			return("Response not valid.");
		break;
		case MODE_RELEASE:
		if (msg.message != ADMIN_RESPONSE_CMD_RELEASE)
			return("Response not valid.");
		break;
		case MODE_UNBLOCK:
		case MODE_BLOCK:
		case MODE_UNLOAD:
		if (msg.message != ADMIN_RESPONSE_CMD_BLOCK)
			return("Response not valid.");
		break;
	}

	/* process response */
	if (msg.u.x.error) {
		return(msg.u.x.message);
	}
	printf("Command successfull.\n");
	return(NULL);
}


/*
 * makes a testcall
 */
#define GET_NOW() { \
	gettimeofday(&now_tv, &now_tz); \
	now_d = ((double)(now_tv.tv_usec))/1000000 + now_tv.tv_sec; \
	}
const char *admin_testcall(int sock, int argc, char *argv[])
{
	static struct admin_message msg;
	int ar = 2;
	int stimeout = 0, ptimeout = 0, atimeout = 0, ctimeout = 0;
	int l;
	double timer = 0, now_d;
	unsigned int on = 1;
	struct timeval now_tv;
	struct timezone now_tz;

	printf("pid=%d\n", getpid()); fflush(stdout);

	while (argc > ar) {
		if (!strcmp(argv[ar], "--setup-timeout")) {
			ar++;
			if (argc == ar)
				return("Missing setup timeout value.\n");
			stimeout = atoi(argv[ar]);
			ar++;
		} else
		if (!strcmp(argv[ar], "--proceeding-timeout")) {
			ar++;
			if (argc == ar)
				return("Missing proceeding timeout value.\n");
			ptimeout = atoi(argv[ar]);
			ar++;
		} else
		if (!strcmp(argv[ar], "--alerting-timeout")) {
			ar++;
			if (argc == ar)
				return("Missing alerting timeout value.\n");
			atimeout = atoi(argv[ar]);
			ar++;
		} else
		if (!strcmp(argv[ar], "--connect-timeout")) {
			ar++;
			if (argc == ar)
				return("Missing connect timeout value.\n");
			ctimeout = atoi(argv[ar]);
			ar++;
		} else {
			break;
		}
	}

	/* send reload command */
	memset(&msg, 0, sizeof(msg));
	msg.message = ADMIN_CALL_SETUP;
	msg.u.call.present = 1;

	if (argc > ar) {
		SCPY(msg.u.call.interface, argv[ar]);
	}
	ar++;
	if (argc > ar) {
		SCPY(msg.u.call.callerid, argv[ar]);
	}
	ar++;
	if (argc > ar) {
		SCPY(msg.u.call.dialing, argv[ar]);
	}
	ar++;
	if (argc > ar) {
		if (argv[ar][0] == 'r')
			msg.u.call.present = 0;
	}
	ar++;
	msg.u.call.bc_capa = 0x00; /*INFO_BC_SPEECH*/
	msg.u.call.bc_mode = 0x00; /*INFO_BMODE_CIRCUIT*/
	msg.u.call.bc_info1 = 0;
	msg.u.call.hlc = 0;
	msg.u.call.exthlc = 0;
	if (argc > ar)
		msg.u.call.bc_capa = strtol(argv[ar],NULL,0);
	else
		msg.u.call.bc_info1 = 3 | 0x80; /* alaw, if no capability is given at all */
	ar++;
	if (argc > ar) {
		msg.u.call.bc_mode = strtol(argv[ar],NULL,0);
		if (msg.u.call.bc_mode) msg.u.call.bc_mode = 2;
	}
	ar++;
	if (argc > ar) {
		msg.u.call.bc_info1 = strtol(argv[ar],NULL,0);
		if (msg.u.call.bc_info1 < 0)
			msg.u.call.bc_info1 = 0;
		else
			msg.u.call.bc_info1 |= 0x80;
	}
	ar++;
	if (argc > ar) {
		msg.u.call.hlc = strtol(argv[ar],NULL,0);
		if (msg.u.call.hlc < 0)
			msg.u.call.hlc = 0;
		else
			msg.u.call.hlc |= 0x80;
	}
	ar++;
	if (argc > ar) {
		msg.u.call.exthlc = strtol(argv[ar],NULL,0);
		if (msg.u.call.exthlc < 0)
			msg.u.call.exthlc = 0;
		else
			msg.u.call.exthlc |= 0x80;
	}
	ar++;

	if (write(sock, &msg, sizeof(msg)) != sizeof(msg))
		return("Broken pipe while sending command.");

	if (ioctl(sock, FIONBIO, (unsigned char *)(&on)) < 0)
		return("Failed to set socket into non-blocking IO.");

	if (stimeout) {
		GET_NOW();
		timer = now_d + (double)stimeout;
	}
	
	/* receive response */
next:
	l = read(sock, &msg, sizeof(msg));
	if (l < 0) {
		if (errno == EWOULDBLOCK) {
			if (timer) {
				GET_NOW();
				if (timer <= now_d) {
					printf("Timeout\n"); fflush(stdout);
					return(NULL);
				}
			}
			usleep(30000);
			goto next;
		}
		return("Broken pipe while receiving response.");
	}
	if (l != sizeof(msg))
		return("Response has unexpected message size.");
	switch(msg.message) {
		case ADMIN_CALL_SETUP_ACK:
		printf("SETUP ACKNOWLEDGE\n"); fflush(stdout);
		goto next;

		case ADMIN_CALL_PROCEEDING:
		printf("PROCEEDING\n"); fflush(stdout);
		if (ptimeout) {
			GET_NOW();
			timer = now_d + (double)ptimeout;
		}
		goto next;

		case ADMIN_CALL_ALERTING:
		printf("ALERTING\n"); fflush(stdout);
		if (atimeout) {
			GET_NOW();
			timer = now_d + (double)atimeout;
		}
		goto next;

		case ADMIN_CALL_CONNECT:
		printf("CONNECT\n number=%s\n", msg.u.call.callerid); fflush(stdout);
		if (ctimeout) {
			GET_NOW();
			timer = now_d + (double)ctimeout;
		}
		goto next;

		case ADMIN_CALL_NOTIFY:
		printf("NOTIFY\n notify=%d\n number=%s\n", msg.u.call.notify_progress, msg.u.call.callerid); fflush(stdout);
		goto next;

		case ADMIN_CALL_PROGRESS:
		printf("PROGRESS\n progress=%d\n location=%d\n", msg.u.call.notify_progress, msg.u.call.location); fflush(stdout);
		goto next;

		case ADMIN_CALL_DISCONNECT:
		printf("DISCONNECT\n cause=%d %s\n location=%d %s\n", msg.u.call.cause, (msg.u.call.cause>0 && msg.u.call.cause<128)?isdn_cause[msg.u.call.cause].german:"", msg.u.call.location, (msg.u.call.location>=0 && msg.u.call.location<128)?isdn_location[msg.u.call.location].german:""); fflush(stdout);
		break;

		case ADMIN_CALL_RELEASE:
		printf("RELEASE\n cause=%d %s\n location=%d %s\n", msg.u.call.cause, (msg.u.call.cause>0 && msg.u.call.cause<128)?isdn_cause[msg.u.call.cause].german:"", msg.u.call.location, (msg.u.call.location>=0 && msg.u.call.location<128)?isdn_location[msg.u.call.location].german:""); fflush(stdout);
		break;

		default:
		return("Response not valid.");
	}

	printf("Call released.\n"); fflush(stdout);
	return(NULL);
}


/*
 * makes a trace
 */
const char *admin_trace(int sock, int argc, char *argv[])
{
	static struct admin_message msg;
	int i;

	/* show help */
	if (argc > 2) if (!strcasecmp(argv[2], "help")) {
		printf("Trace Help\n----------\n");
		printf("%s trace [brief|short] [<filter>=<value> [...]]\n\n", argv[0]);
		printf("By default a complete trace is shown in detailed format.\n");
		printf("To show a more compact format, use 'brief' or 'short' keyword.\n");
		printf("Use filter values to select specific trace messages.\n");
		printf("All given filter values must match. If no filter is given, anything matches.\n\n");
		printf("Filters:\n");
		printf(" category=<mask bits>\n");
		printf("  0x01 = CH: channel object trace\n");
		printf("  0x02 = EP: endpoint object trace\n");
		printf(" port=<mISDN port>  select only given port for trace\n");
		printf(" interface=<interface name>  select only given interface for trace\n");
		printf(" caller=<caller id>  select only given caller id for trace\n");
		printf(" dialing=<number>  select only given dialed number for trace\n");
		return(NULL);
	}

	/* init trace request */	
	memset(&msg, 0, sizeof(msg));
	msg.message = ADMIN_TRACE_REQUEST;
	msg.u.trace_req.detail = 3;
	msg.u.trace_req.port = -1;

	/* parse args */
	i = 2;
	while(i < argc) {
		if (!strcasecmp(argv[i], "brief"))
			msg.u.trace_req.detail = 1;
		else if (!strcasecmp(argv[i], "short"))
			msg.u.trace_req.detail = 2;
		else if (!strncasecmp(argv[i], "category=", 9))
			msg.u.trace_req.category = atoi(argv[i]+9);
		else if (!strncasecmp(argv[i], "port=", 5))
			msg.u.trace_req.port = atoi(argv[i]+5);
		else if (!strncasecmp(argv[i], "interface=", 10))
			SCPY(msg.u.trace_req.interface, argv[i]+10);
		else if (!strncasecmp(argv[i], "caller=", 7))
			SCPY(msg.u.trace_req.caller, argv[i]+7);
		else if (!strncasecmp(argv[i], "dialing=", 8))
			SCPY(msg.u.trace_req.dialing, argv[i]+8);
		else return("Invalid trace option, try 'trace help'.");

		i++;
	}

	/* send trace request */
	if (write(sock, &msg, sizeof(msg)) != sizeof(msg))
		return("Broken pipe while sending trace request.");

	/* receive response */
next:
	if (read(sock, &msg, sizeof(msg)) != sizeof(msg))
		return("Broken pipe while receiving response.");

	if (msg.message != ADMIN_TRACE_RESPONSE)
		return("Response not valid.");

	printf("%s", msg.u.trace_rsp.text);
	goto next;
}


/*
 * main function
 */
int main(int argc, char *argv[])
{
	int mode;
	int sock, conn;
	struct sockaddr_un sock_address;
	const char *ret = "invalid mode";
	char options_error[256];

	/* show options */
	if (argc <= 1) {
		usage:
		printf("\n");
		printf("Usage: %s state | interface | route | dial ...\n", argv[0]);
		printf("state - View current states using graphical console output.\n");
		printf("portinfo - Get info of current ports.\n");
		printf("interface [<portname>] - Tell LCR to reload \"interface.conf\".\n");
		printf("route - Tell LCR to reload \"route.conf\".\n");
		printf("dial <extension> <number> - Tell LCR the next number to dial for extension.\n");
		printf("release <number> - Tell LCR to release endpoint with given number.\n");
		printf("block <port> - Block given port.\n");
		printf("unblock/load <port> - Unblock given port.\n");
		printf("unload <port> - Unload port. To load port use 'block' or 'unblock'.\n");
		printf("testcall [options] <interface> <callerid> <number> [present|restrict [<capability>]] - Testcall\n");
		printf(" -> options = --setup-timeout <seconds> --proceeding-timeout <seconds>\n");
		printf("              --alerting-timeout <seconds> --connect-timeout <seconds>\n");
		printf(" -> capability = <bc> <mode> <codec> <hlc> <exthlc> (Values must be numbers, -1 to omit.)\n");
		printf("trace [brief|short] [<filter> [...]] - Shows call trace. Use filter to reduce output.\n");
		printf(" -> Use 'trace help' to see filter description.\n");
		printf("\n");
		return(0);
	}

	/* check mode */
	if (!(strcasecmp(argv[1],"state"))) {
		mode = MODE_STATE;
	} else
	if (!(strcasecmp(argv[1],"portinfo"))) {
		mode = MODE_PORTINFO;
	} else
	if (!(strcasecmp(argv[1],"interface"))) {
		mode = MODE_INTERFACE;
	} else
	if (!(strcasecmp(argv[1],"route"))) {
		mode = MODE_ROUTE;
	} else
	if (!(strcasecmp(argv[1],"dial"))) {
		if (argc <= 3)
			goto usage;
		mode = MODE_DIAL;
	} else
	if (!(strcasecmp(argv[1],"release"))) {
		if (argc <= 2)
			goto usage;
		mode = MODE_RELEASE;
	} else
	if (!(strcasecmp(argv[1],"unblock"))
	 || !(strcasecmp(argv[1],"load"))) {
		if (argc <= 2)
			goto usage;
		mode = MODE_UNBLOCK;
	} else
	if (!(strcasecmp(argv[1],"block"))) {
		if (argc <= 2)
			goto usage;
		mode = MODE_BLOCK;
	} else
	if (!(strcasecmp(argv[1],"unload"))) {
		if (argc <= 2)
			goto usage;
		mode = MODE_UNLOAD;
	} else
	if (!(strcasecmp(argv[1],"testcall"))) {
		if (argc <= 4)
			goto usage;
		mode = MODE_TESTCALL;
	} else
	if (!(strcasecmp(argv[1],"trace"))) {
		mode = MODE_TRACE;
	} else {
		goto usage;
	}

	if (read_options(options_error) == 0) {
		exit(EXIT_FAILURE);
	}

//pipeagain:
	/* open socket */
	if ((sock = socket(PF_UNIX, SOCK_STREAM, 0)) < 0) {
		fprintf(stderr, "Failed to create socket.\n");
		exit(EXIT_FAILURE);
	}
	memset(&sock_address, 0, sizeof(sock_address));
	SPRINT(sock_address.sun_path, SOCKET_NAME, options.lock);
	sock_address.sun_family = PF_UNIX;
	if ((conn = connect(sock, (struct sockaddr *)&sock_address, SUN_LEN(&sock_address))) < 0) {
		close(sock);
		fprintf(stderr, "Failed to connect to socket \"%s\".\nIs LCR running?\n", sock_address.sun_path);
		exit(EXIT_FAILURE);
	}

	/* process mode */
	switch(mode) {
		case MODE_STATE:
		ret = admin_state(sock, argv);
		break;
	
		case MODE_PORTINFO:
		ret = admin_portinfo(sock, argc, argv);
		break;
	
		case MODE_INTERFACE:
		case MODE_ROUTE:
		ret = admin_cmd(sock, mode, NULL, NULL);
		break;

		case MODE_DIAL:
		ret = admin_cmd(sock, mode, argv[2], argv[3]);
		break;

		case MODE_RELEASE:
		case MODE_UNBLOCK:
		case MODE_BLOCK:
		case MODE_UNLOAD:
		ret = admin_cmd(sock, mode, NULL, argv[2]);
		break;

		case MODE_TESTCALL:
		ret = admin_testcall(sock, argc, argv);
		break;

		case MODE_TRACE:
		ret = admin_trace(sock, argc, argv);
		break;
	}

	close(sock);
	/* now we say good bye */
	if (ret) {
//		if (!strncasecmp(ret, "Broken Pipe", 11))
//			goto pipeagain;
		printf("%s\n", ret);
		exit(EXIT_FAILURE);
	}
}





