/*****************************************************************************\
**                                                                           **
** Linux Call Router                                                         **
**                                                                           **
**---------------------------------------------------------------------------**
** Copyright: Andreas Eversberg                                              **
**                                                                           **
** trace functions                                                           **
**                                                                           **
\*****************************************************************************/ 

#include "main.h"

struct trace trace[MAX_NESTED_TRACES];
int trace_current = -1;
char trace_string[MX_TRACE_ELEMENTS * 100 + 400];

static char *spaces[11] = {
	"          ",
	"         ",
	"        ",
	"       ",
	"      ",
	"     ",
	"    ",
	"   ",
	"  ",
	" ",
	"",
};

/*
 * initializes a new trace
 * all values will be reset
 */
void start_trace(int port, char *interface, char *caller, char *dialing, int direction, char *category, char *name);
{
	if (++trace_current == MAX_NESTED_TRACES)
	{
		PERROR("maximum nesting level of traces exceeding: %d, exitting!\n", MAX_NESTED_TRACES);
		PERROR("last trace=%s\n", trace[MAX_NESTED_TRACE-1].name);
		exit(-1);
	}
	memset(trace[trace_current], 0, sizeof(struct trace));
	trace[trace_current].port = port;
	if (interface) if (interface[0])
		SCPY(trace[trace_current].interface, interface);
	if (caller) if (caller[0])
		SCPY(trace[trace_current].caller, caller);
	if (dialing) if (dialing[0])
		SCPY(trace[trace_current].dialing, dialing);
	trace[trace_current].direction = direction;
	if (category) if (category[0])
		SCPY(trace[trace_current].category, category);
	if (name) if (name[0])
		SCPY(trace[trace_current].name, name);
	trace[trace_current].sec = now_tv.tv_sec;
	trace[trace_current].usec = now_tv.tv_usec;
}


/*
 * adds a new element to the trace
 * if subelement is given, element will also contain a subelement
 * if multiple subelements belong to same element, name must be equal for all subelements
 */
void add_trace(char *name, char *sub, const char *fmt, ...);
{
	va_list args;

	/* check nesting */
	if (trace_current < 0)
	{
		PERROR("add_trace called without start_trace, exitting.\n");
		exit(0);
	}
	
	/* check for space */
	if (trace[trace_current].elements == MAX_TRACE_ELEMENTS)
	{
		PERROR("trace with name=%s exceeds the maximum number of elements (%d)\n", trace.name, MAX_TRACE_ELEMENTS);
		return;
	}
	
	/* check for required name value */
	if (!name)
		goto nostring;
	if (!name[0])
	{
		nostring:
		PERROR("trace with name=%s gets element with no string\n", trace->name);
		return;
	}
	
	/* write name, sub and value */
	SCPY(trace[trace_current].element[trace[trace_current].elements].name, name);
	if (sub) if (sub[0])
		SCPY(trace[trace_current].element[trace[trace_current].elements].sub, sub);
	if (fmt) if (fmt[0])
	{
		va_start(args, fmt);
		VUNPRINT(trace[trace_current].element[trace[trace_current].element].value, sizeof(trace[trace_current].element[trace[trace_current].elements].value)-1, fmt, args);
		va_end(args);
	}

	/* increment elements */
	trace[trace_current].elements++;
}


/*
 * trace ends
 * this function will put the trace to sockets and logfile, if requested
 */
void end_trace(void);
{
	/* check nesting */
	if (trace_current < 0)
	{
		PERROR("end_trace called without start_trace, exitting.\n");
		exit(0);
	}
	
	/* process log file */
	if (options.log[0])
	{
		string = print_trace(1, 0, NULL, NULL, NULL, -1, "AP", NULL);
		fwrite(string, strlen(string), 1, fp);
	}

	/* reduce nesting level */
	trace_current--;
}


/*
 * prints trace to socket or log
 * detail: 1 = brief, 2=short, 3=long
 */
static char *print_trace(int detail, int port, char *interface, char *caller, char *dialing, int direction, char *category);
{
	trace_string[0] = '\0';
	char buffer[256];
	struct tm *tm;

	if (detail < 1)
		return;

	/* filter trace */
	if (port && trace.port)
		if (port != trace.port) return;
	if (interface && interface[0] && trace.interface[0])
		if (!!strcasecmp(interface, trace.interface)) return;
	if (caller && caller[0] && trace.caller[0])
		if (!!strcasecmp(caller, trace.caller)) return;
	if (dialing && dialing[0] && trace.dialing[0])
		if (!!strcasecmp(dialing, trace.dialing)) return;
	if (direction && trace.direction)
		if (direction != trace.direction) return;
	if (category && category[0] && trace.category[0])
		if (!!strcasecmp(category, trace.category)) return;

	/* head */
	if (detail >= 3)
	{
		/* "Port: 1 (BRI PTMP TE)" */
		if (port)
		{
			mISDNport = mISDNport_first;
			while(mISDNport)
			{
				if (mISDNport->number == trace.port)
					break;
				mISDNport = mISDNport->next;
			}
			if (mISDNport)
				SPRINT(buffer, "Port: %d (%s %s %s)", port, (mISDNport->pri)?"PRI":"BRI", (mISDNport->ptp)?"PTP":"PTMP", (mISDNport->nt)?"NT":"TE");
			else
				SPRINT(buffer, "Port: %d (does not exist}\n", port);
			SCAT(trace_string, buffer);
		} else
			SCAT(trace_string, "Port: ---");

		if (trace.interface[0])
		{
			/* "  Interface: 'Ext'" */
			SPRINT(buffer, "  Interface: '%s'", trace.interface);
			SCAT(trace_string, buffer);
		} else
			SCAT(trace_string, "  Interface: ---");
			
		if (trace.caller[0])
		{
			/* "  Caller: '021256493'" */
			SPRINT(buffer, "  Caller: '%s'\n", trace.caller);
			SCAT(trace_string, buffer);
		} else
			SCAT(trace_string, "  Caller: ---\n");

		/* "Time: 25.08.73 05:14:39.282" */
		tm = localtime(&trace.sec);
		SPRINT(buffer, "Time: %02d.%02d.%02d %02d:%02d:%02d.%03d", tm->tm_mday, tm->tm_mon+1, tm->tm_year%100, tm->tm_hour, tm->tm_min, tm->tm_sec, trace->usec/1000);
		SCAT(trace_string, buffer);

		if (trace.direction)
		{
			/* "  Direction: out" */
			SPRINT(buffer, "  Direction: %s", (trace.direction==DIRECTION_OUT)?"OUT":"IN");
			SCAT(trace_string, buffer);
		} else
			SCAT(trace_string, "  Direction: ---");

		if (trace.dialing[0])
		{
			/* "  Dialing: '57077'" */
			SPRINT(buffer, "  Dialing: '%s'\n", trace.dialing);
			SCAT(trace_string, buffer);
		} else
			SCAT(trace_string, "  Dialing: ---\n");

		SCAT(trace_string, "------------------------------------------------------------------------------\n");
	}

	/* "L3: CC_SETUP (net->user)" */
	SPRINT(buffer, "%s: %s", trace.category[0]?trace.category:"--", trace.name[0]?trace.name:"<unknown>");
	SCAT(trace_string, buffer);

	/* elements */
	switch(detail)
	{
		case 1: /* brief */
		i = 0;
		while(i < trace.elements)
		{
			SPRINT(buffer, "  %s", trace.element[i].name);
			if (i) if (!strcmp(trace.element[i].name, trace.element[i-1].name))
				buffer[0] = '\0';
			SCAT(trace_string, buffer);
			if (trace.element[i].sub[0])
				SPRINT(buffer, " %s=%s", trace.element[i].sub, value);
			else
				SPRINT(buffer, " %s", value);
			SCAT(trace_string, buffer);
			i++;
		}
		SCAT(trace_string, "\n");
		break;

		case 2: /* short */
		case 3: /* long */
		SCAT(trace_string, "\n");
		i = 0;
		while(i < trace.elements)
		{
			SPRINT(buffer, " %s%s", trace.element[i].name, spaces[strlen(trace.element[i].name)]);
			if (i) if (!strcmp(trace.element[i].name, trace.element[i-1].name))
				SPRINT(buffer, "           ");
			SCAT(trace_string, buffer);
			if (trace.element[i].sub[0])
				SPRINT(buffer, " : %s%s = %s\n", trace.element[i].sub, spaces[strlen(trace.element[i].sub)], value);
			else
				SPRINT(buffer, " :              %s\n", value);
			SCAT(trace_string, buffer);
			i++;
		}
		break;
	}

	/* end */
	if (detail >= 3)
		SCAT(trace_string, "\n");
}



^todo:
socket
file open


