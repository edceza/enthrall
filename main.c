#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
#include <getopt.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "types.h"
#include "misc.h"
#include "proto.h"
#include "platform.h"
#include "keycodes.h"

#include "cfg-parse.tab.h"

struct remote* focused_remote = NULL;
opmode_t opmode;

static char* progname;

static struct config* config;

#define for_each_remote(r) for (r = config->remotes; r; r = r->next)

struct scheduled_call {
	void (*fn)(void* arg);
	void* arg;
	uint64_t calltime;
	struct scheduled_call* next;
};

static struct scheduled_call* scheduled_calls;

void elog(const char* fmt, ...)
{
	va_list va;
	struct message* msg;

	va_start(va, fmt);
	if (opmode == MASTER) {
		vfprintf(stderr, fmt, va);
	} else {
		msg = new_message(MT_LOGMSG);
		msg->extra.buf = xvasprintf(fmt, va);
		msg->extra.len = strlen(msg->extra.buf);

		/*
		 * There are a few potential error messages during setup
		 * before we go O_NONBLOCK; handle both situations here.
		 */
		if (get_fd_nonblock(STDOUT_FILENO)) {
			mc_enqueue_message(&stdio_msgchan, msg);
		} else {
			write_message(STDOUT_FILENO, msg);
			free_message(msg);
		}
	}
	va_end(va);
}

static void schedule_call(void (*fn)(void* arg), void* arg, uint64_t when)
{
	struct scheduled_call* call;
	struct scheduled_call** prevnext;
	struct scheduled_call* newcall = xmalloc(sizeof(*newcall));
	newcall->fn = fn;
	newcall->arg = arg;
	newcall->calltime = when;

	for (prevnext = &scheduled_calls, call = *prevnext;
	     call;
	     prevnext = &call->next, call = call->next) {
		if (newcall->calltime < call->calltime)
			break;
	}

	newcall->next = call;
	*prevnext = newcall;
}

static void run_scheduled_calls(uint64_t when)
{
	struct scheduled_call* call;

	while (scheduled_calls && scheduled_calls->calltime <= when) {
		call = scheduled_calls;
		scheduled_calls = call->next;
		call->fn(call->arg);
		xfree(call);
	}
}

static void focus_master(void);

static void disconnect_remote(struct remote* rmt)
{
	pid_t pid;
	int status;
	struct message* msg;

	/* Close fds and reset send & receive queues/buffers */
	mc_close(&rmt->msgchan);

	/* Clear out scheduled messages */
	while (rmt->scheduled_messages) {
		msg = rmt->scheduled_messages;
		rmt->scheduled_messages = msg->next;
		free_message(msg);
	}

	/*
	 * A note on signal choice here: initially this used SIGTERM (which
	 * seemed more appropriate), but it appears ssh has a tendency to
	 * (under certain connection-failure conditions) block for long
	 * periods of time with SIGTERM blocked/ignored, meaning we end up
	 * blocking in wait().  So instead we just skip straight to the big
	 * gun here.  I don't think it's likely to have any terribly important
	 * cleanup to do anyway (at least in this case).
	 */
	if (rmt->sshpid > 0) {
		if (kill(rmt->sshpid, SIGKILL) && errno != ESRCH)
			perror("failed to kill remote shell");
		pid = waitpid(rmt->sshpid, &status, 0);
		if (pid != rmt->sshpid)
			perror("wait() on remote shell");
	}

	rmt->sshpid = -1;

	if (rmt == focused_remote)
		focus_master();
}

#define RECONNECT_INTERVAL_UNIT (500 * 1000) /* half a second */
#define MAX_RECONNECT_INTERVAL ((30 * 1000 * 1000) / RECONNECT_INTERVAL_UNIT)
#define MAX_RECONNECT_ATTEMPTS 10

static void fail_remote(struct remote* rmt, const char* reason)
{
	uint64_t tmp, lshift;

	elog("disconnecting remote '%s': %s\n", rmt->alias, reason);
	disconnect_remote(rmt);
	rmt->failcount += 1;

	if (rmt->failcount > MAX_RECONNECT_ATTEMPTS) {
		elog("remote '%s' exceeds failure limits, permfailing.\n", rmt->alias);
		rmt->state = CS_PERMFAILED;
		return;
	}

	rmt->state = CS_FAILED;

	/* 0.5s, 1s, 2s, 4s, 8s...capped at MAX_RECONNECT_INTERVAL */
	lshift = rmt->failcount - 1;
	if (lshift > (CHAR_BIT * sizeof(uint64_t) - 1))
		lshift = (CHAR_BIT * sizeof(uint64_t)) - 1;
	tmp = (1ULL << lshift);
	if (tmp > MAX_RECONNECT_INTERVAL)
		tmp = MAX_RECONNECT_INTERVAL;

	rmt->next_reconnect_time = get_microtime() + (tmp * RECONNECT_INTERVAL_UNIT);
}

static void enqueue_message(struct remote* rmt, struct message* msg)
{
	if (mc_enqueue_message(&rmt->msgchan, msg))
		fail_remote(rmt, "send backlog exceeded");
}

static void schedule_message(struct remote* rmt, struct message* newmsg)
{
	struct message* msg;
	struct message** prevnext;

	for (prevnext = &rmt->scheduled_messages, msg = *prevnext;
	     msg;
	     prevnext = &msg->next, msg = msg->next) {
		if (newmsg->sendtime < msg->sendtime)
			break;
	}

	newmsg->next = msg;
	*prevnext = newmsg;
}

#define SSH_DEFAULT(type, name) \
	static inline type get_##name(const struct remote* rmt) \
	{ \
		return rmt->sshcfg.name ? rmt->sshcfg.name \
			: config->ssh_defaults.name; \
	}

SSH_DEFAULT(char*, remoteshell)
SSH_DEFAULT(int, port)
SSH_DEFAULT(char*, bindaddr)
SSH_DEFAULT(char*, identityfile)
SSH_DEFAULT(char*, username)
SSH_DEFAULT(char*, remotecmd)

static void exec_remote_shell(const struct remote* rmt)
{
	int nargs;
	char* remote_shell = get_remoteshell(rmt) ? get_remoteshell(rmt) : "ssh";
	char* argv[] = {
		remote_shell,
		"-oBatchMode=yes",
		"-oServerAliveInterval=2",
		"-oServerAliveCountMax=3",

		/* placeholders */
		NULL, /* -b */
		NULL, /* bind address */
		NULL, /* -oIdentitiesOnly=yes */
		NULL, /* -i */
		NULL, /* identity file */
		NULL, /* -p */
		NULL, /* port */
		NULL, /* -l */
		NULL, /* username */
		NULL, /* hostname */
		NULL, /* remote command */

		NULL, /* argv terminator */
	};

	for (nargs = 0; argv[nargs]; nargs++) /* just find first NULL entry */;

	if (get_port(rmt)) {
		argv[nargs++] = "-p";
		argv[nargs++] = xasprintf("%d", get_port(rmt));
	}

	if (get_bindaddr(rmt)) {
		argv[nargs++] = "-b";
		argv[nargs++] = get_bindaddr(rmt);
	}

	if (get_identityfile(rmt)) {
		argv[nargs++] = "-oIdentitiesOnly=yes";
		argv[nargs++] = "-i";
		argv[nargs++] = get_identityfile(rmt);
	}

	if (get_username(rmt)) {
		argv[nargs++] = "-l";
		argv[nargs++] = get_username(rmt);
	}

	argv[nargs++] = rmt->hostname;

	argv[nargs++] = get_remotecmd(rmt) ? get_remotecmd(rmt) : progname;

	assert(nargs < ARR_LEN(argv));

	execvp(remote_shell, argv);
	perror("execvp");
	exit(1);
}

static void setup_remote(struct remote* rmt)
{
	int sockfds[2];
	struct message* setupmsg;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockfds)) {
		perror("socketpair");
		exit(1);
	}

	rmt->sshpid = fork();
	if (rmt->sshpid < 0) {
		perror("fork");
		exit(1);
	}

	rmt->state = CS_SETTINGUP;

	if (!rmt->sshpid) {
		/* ssh child */
		if (dup2(sockfds[1], STDIN_FILENO) < 0
		    || dup2(sockfds[1], STDOUT_FILENO) < 0) {
			perror("dup2");
			exit(1);
		}

		if (close(sockfds[0]))
			perror("close");

		exec_remote_shell(rmt);
	}

	set_fd_nonblock(sockfds[0], 1);
	set_fd_cloexec(sockfds[0], 1);

	mc_init(&rmt->msgchan, sockfds[0], sockfds[0]);

	if (close(sockfds[1]))
		perror("close");

	setupmsg = new_message(MT_SETUP);
	setupmsg->type = MT_SETUP;
	setupmsg->setup.prot_vers = PROT_VERSION;
	setupmsg->extra.buf = flatten_kvmap(rmt->params, &setupmsg->extra.len);

	enqueue_message(rmt, setupmsg);
}

static struct remote* find_remote(const char* name)
{
	struct remote* rmt;

	/* First search by alias */
	for_each_remote (rmt) {
		if (!strcmp(name, rmt->alias))
			return rmt;
	}

	/* if that fails, try hostnames */
	for_each_remote (rmt) {
		if (!strcmp(name, rmt->hostname))
			return rmt;
	}

	return NULL;
}

static void resolve_noderef(struct noderef* n)
{
	char* name;
	struct remote* rmt;
	if (n->type == NT_REMOTE_TMPNAME) {
		name = n->name;
		rmt = find_remote(name);
		if (!rmt) {
			elog("No such remote: '%s'\n", n->name);
			exit(1);
		}
		n->type = NT_REMOTE;
		n->node = rmt;
		xfree(name);
	}
}

static void mark_reachable(struct noderef* n)
{
	int seen;
	direction_t dir;
	struct remote* rmt;

	switch (n->type) {
	case NT_REMOTE_TMPNAME:
		resolve_noderef(n);
		/* fallthrough */
	case NT_REMOTE:
		rmt = n->node;
		break;
	default:
		return;
	}

	seen = rmt->reachable;

	rmt->reachable = 1;

	if (!seen) {
		for_each_direction (dir)
			mark_reachable(&rmt->neighbors[dir]);
	}
}

static void check_remotes(void)
{
	direction_t dir;
	struct remote* rmt;
	int num_neighbors;

	for_each_direction (dir)
		mark_reachable(&config->master.neighbors[dir]);

	for_each_remote (rmt) {
		if (!rmt->reachable)
			elog("Warning: remote '%s' is not reachable\n", rmt->alias);

		num_neighbors = 0;
		for_each_direction (dir) {
			if (rmt->neighbors[dir].type != NT_NONE)
				num_neighbors += 1;
		}

		if (!num_neighbors)
			elog("Warning: remote '%s' has no neighbors\n", rmt->alias);
	}
}


static void transfer_clipboard(struct remote* from, struct remote* to)
{
	struct message* msg;

	if (!from && !to) {
		elog("switching from master to master??\n");
		return;
	}

	if (from) {
		msg = new_message(MT_GETCLIPBOARD);
		enqueue_message(from, msg);
	} else if (to) {
		msg = new_message(MT_SETCLIPBOARD);
		msg->extra.buf = get_clipboard_text();
		msg->extra.len = strlen(msg->extra.buf);
		assert(msg->extra.len <= UINT32_MAX);
		enqueue_message(to, msg);
	}
}

static void transfer_modifiers(struct remote* from, struct remote* to, const keycode_t* modkeys)
{
	int i;
	struct message* msg;

	if (from) {
		for (i = 0; modkeys[i] != ET_null; i++) {
			msg = new_message(MT_KEYEVENT);
			msg->keyevent.pressrel = PR_RELEASE;
			msg->keyevent.keycode = modkeys[i];
			enqueue_message(from, msg);
		}
	}

	if (to) {
		for (i = 0; modkeys[i] != ET_null; i++) {
			msg = new_message(MT_KEYEVENT);
			msg->keyevent.pressrel = PR_PRESS;
			msg->keyevent.keycode = modkeys[i];
			enqueue_message(to, msg);
		}
	}
}

void send_keyevent(struct remote* rmt, keycode_t kc, pressrel_t pr)
{
	struct message* msg;

	if (!rmt)
		return;

	msg = new_message(MT_KEYEVENT);

	msg->keyevent.keycode = kc;
	msg->keyevent.pressrel = pr;

	enqueue_message(rmt, msg);
}

void send_moverel(struct remote* rmt, int32_t dx, int32_t dy)
{
	struct message* msg;

	if (!rmt)
		return;

	msg = new_message(MT_MOVEREL);

	msg->moverel.dx = dx;
	msg->moverel.dy = dy;

	enqueue_message(rmt, msg);
}

void send_clickevent(struct remote* rmt, mousebutton_t button, pressrel_t pr)
{
	struct message* msg;

	if (!rmt)
		return;

	msg = new_message(MT_CLICKEVENT);

	msg->clickevent.button = button;
	msg->clickevent.pressrel = pr;

	enqueue_message(rmt, msg);
}

void send_setbrightness(struct remote* rmt, float f)
{
	struct message* msg;

	if (!rmt)
		return;

	msg = new_message(MT_SETBRIGHTNESS);

	msg->setbrightness.brightness = f;

	enqueue_message(rmt, msg);
}

static void set_node_display_brightness(struct remote* rmt, float f)
{
	if (!rmt)
		set_display_brightness(f);
	else
		send_setbrightness(rmt, f);
}

static void set_brightness_cb(void* arg)
{
	float* fp = arg;

	set_display_brightness(*fp);

	xfree(fp);
}

static void schedule_brightness_change(struct remote* rmt, float f, uint64_t when)
{
	struct message* msg;
	float* fp;
	if (rmt) {
		msg = new_message(MT_SETBRIGHTNESS);
		msg->setbrightness.brightness = f;
		msg->sendtime = when;
		schedule_message(rmt, msg);
	} else {
		fp = xmalloc(sizeof(*fp));
		*fp = f;
		schedule_call(set_brightness_cb, fp, when);
	}
}

static void transition_brightness(struct remote* node, float from, float to,
                                  uint64_t duration, int steps)
{
	int i;
	float frac, level;
	uint64_t time, now_us = get_microtime();

	set_node_display_brightness(node, from);
	for (i = 1; i < steps; i++) {
		frac = (float)i / (float)steps;
		time = now_us + (uint64_t)(frac * (float)duration);
		level = from + (frac * (to - from));
		schedule_brightness_change(node, level, time);
	}
	schedule_brightness_change(node, to, now_us + duration);
}

static void indicate_switch(struct remote* from, struct remote* to)
{
	struct focus_hint* fh = &config->focus_hint;

	switch (fh->type) {
	case FH_NONE:
		break;

	case FH_DIM_INACTIVE:
		if (from != to)
			transition_brightness(from, 1.0, fh->brightness, fh->duration,
			                      fh->fade_steps);
		transition_brightness(to, fh->brightness, 1.0, fh->duration,
		                      fh->fade_steps);
		break;

	case FH_FLASH_ACTIVE:
		transition_brightness(to, fh->brightness, 1.0, fh->duration,
		                      fh->fade_steps);
		break;

	default:
		elog("unknown focus_hint type %d\n", fh->type);
		break;
	}
}

static struct xypoint saved_master_mousepos;

/*
 * Returns non-zero on a successful "real" switch, or zero if no actual switch
 * was performed (i.e. the switched-to node is the same as the current node).
 */
static int focus_node(struct noderef* n, keycode_t* modkeys, int from_hotkey)
{
	struct remote* switch_to;

	switch (n->type) {
	case NT_NONE:
		switch_to = focused_remote;
		break;

	case NT_MASTER:
		switch_to = NULL;
		break;

	case NT_REMOTE:
		switch_to = n->node;
		if (switch_to->state != CS_CONNECTED) {
			elog("remote '%s' not connected, can't focus\n",
			     switch_to->alias);
			return 0;
		}
		break;

	default:
		elog("unexpected neighbor type %d\n", n->type);
		return 0;
	}

	/*
	 * If configured to do so, give visual indication even if no actual
	 * switch is performed.
	 */
	if (switch_to != focused_remote
	    || config->show_nullswitch == NS_YES
	    || (config->show_nullswitch == NS_HOTKEYONLY && from_hotkey))
		indicate_switch(focused_remote, switch_to);

	if (switch_to == focused_remote)
		return 0;

	if (focused_remote && !switch_to) {
		ungrab_inputs();
		set_mousepos(saved_master_mousepos);
	} else if (!focused_remote && switch_to) {
		saved_master_mousepos = get_mousepos();
		grab_inputs();
	}

	if (switch_to)
		set_mousepos(screen_center);

	transfer_clipboard(focused_remote, switch_to);
	transfer_modifiers(focused_remote, switch_to, modkeys);

	focused_remote = switch_to;

	return 1;
}

static void focus_master(void)
{
	struct noderef m = { .type = NT_MASTER, .node = NULL, };
	keycode_t* modkeys = get_current_modifiers();

	focus_node(&m, modkeys, 0);

	xfree(modkeys);
}

static int focus_neighbor(direction_t dir, keycode_t* modkeys, int from_hotkey)
{
	struct noderef* n = &(focused_remote ? focused_remote->neighbors
	                      : config->master.neighbors)[dir];
	return focus_node(n, modkeys, from_hotkey);
}

static void clear_ssh_config(struct ssh_config* c)
{
	xfree(c->remoteshell);
	xfree(c->bindaddr);
	xfree(c->identityfile);
	xfree(c->username);
	xfree(c->remotecmd);
	memset(c, 0, sizeof(*c));
}

static void free_remote(struct remote* rmt)
{
	if (rmt->alias != rmt->hostname)
		xfree(rmt->alias);
	xfree(rmt->hostname);
	destroy_kvmap(rmt->params);
	clear_ssh_config(&rmt->sshcfg);
	xfree(rmt);
}

static void shutdown_master(void)
{
	struct remote* rmt;
	struct scheduled_call* sc;
	struct hotkey* hk;

	while (config->remotes) {
		rmt = config->remotes;
		config->remotes = rmt->next;
		disconnect_remote(rmt);
		free_remote(rmt);
	}

	while (scheduled_calls) {
		sc = scheduled_calls;
		scheduled_calls = sc->next;
		xfree(sc);
	}

	while (config->hotkeys) {
		hk = config->hotkeys;
		config->hotkeys = hk->next;
		xfree(hk->key_string);
		xfree(hk);
	}

	clear_ssh_config(&config->ssh_defaults);

	platform_exit();
}

static void action_cb(hotkey_context_t ctx, void* arg)
{
	struct remote* rmt;
	uint64_t now_us;
	struct action* a = arg;
	keycode_t* modkeys = get_hotkey_modifiers(ctx);

	switch (a->type) {
	case AT_SWITCH:
		focus_neighbor(a->dir, modkeys, 1);
		break;

	case AT_SWITCHTO:
		focus_node(&a->node, modkeys, 1);
		break;

	case AT_RECONNECT:
		now_us = get_microtime();
		for_each_remote (rmt) {
			if (rmt->state == CS_PERMFAILED)
				rmt->state = CS_FAILED;
			rmt->failcount = 0;
			rmt->next_reconnect_time = now_us;
		}
		break;

	case AT_QUIT:
		xfree(modkeys);
		shutdown_master();
		exit(0);

	default:
		elog("unknown action type %d\n", a->type);
		break;
	}

	xfree(modkeys);
}

static void bind_hotkeys(void)
{
	struct hotkey* k;

	for (k = config->hotkeys; k; k = k->next) {
		if (k->action.type == AT_SWITCHTO)
			resolve_noderef(&k->action.node);
		if (bind_hotkey(k->key_string, action_cb, &k->action))
			exit(1);
	}
}

static int record_edgeevent(struct edge_state* es, edgeevent_t evtype, uint64_t when)
{
	if (evtype == es->last_evtype)
		return 1;

	es->evidx = (es->evidx + 1) % EDGESTATE_HISTLEN;
	es->event_times[es->evidx] = when;
	es->last_evtype = evtype;
	return 0;
}

static uint64_t get_edgehist_entry(const struct edge_state* es, int rel_idx)
{
	int idx;

	assert(rel_idx < EDGESTATE_HISTLEN && rel_idx >= 0);
	idx = (es->evidx - rel_idx + EDGESTATE_HISTLEN) % EDGESTATE_HISTLEN;

	return es->event_times[idx];
}

/*
 * Send the screen-relative reposition to make switch-by-mouse look more
 * "natural" -- so the mouse pointer slides semi-continuously from one node's
 * screen to a corresponding position on the next's, rather than jumping to
 * wherever it last was on the destination node.
 */
static void edgeswitch_reposition(direction_t dir, float src_x, float src_y)
{
	float x, y;
	struct message* msg;

	switch (dir) {
	case LEFT:
		x = 1.0;
		y = src_y;
		break;

	case RIGHT:
		x = 0.0;
		y = src_y;
		break;

	case UP:
		x = src_x;
		y = 1.0;
		break;

	case DOWN:
		x = src_x;
		y = 0.0;
		break;

	default:
		elog("bad direction %d in edgeswitch_reposition()\n", dir);
		return;
	}

	if (focused_remote) {
		msg = new_message(MT_SETMOUSEPOSSCREENREL);
		msg->setmouseposscreenrel.xpos = x;
		msg->setmouseposscreenrel.ypos = y;
		enqueue_message(focused_remote, msg);
	} else {
		set_mousepos_screenrel(x, y);
	}
}

static int trigger_edgeevent(struct edge_state* ehist, direction_t dir, edgeevent_t evtype,
                             float src_xpos, float src_ypos)
{
	int status, start_idx;
	keycode_t* modkeys;
	uint64_t duration, now_us = get_microtime();

	status = record_edgeevent(ehist, evtype, now_us);

	if (status)
		return status;

	if (config->mouseswitch.type == MS_MULTITAP && evtype == EE_ARRIVE) {
		/*
		 * How many entries back to look in the edge-event history to
		 * find the first event of the multi-tap sequence of which
		 * this might be the final element: single-tap looks at the
		 * just-recorded entry (#0), double tap looks back at #2
		 * (skipping over the EE_DEPART at #1), triple-tap looks at #4
		 * (skipping over two EE_DEPARTs and an EE_ARRIVE), etc.
		 */
		start_idx = (config->mouseswitch.num - 1) * 2;

		duration = now_us - get_edgehist_entry(ehist, start_idx);
		if (duration < config->mouseswitch.window) {
			modkeys = get_current_modifiers();
			if (focus_neighbor(dir, modkeys, 0))
				edgeswitch_reposition(dir, src_xpos, src_ypos);
			xfree(modkeys);
		}
	}

	return 0;
}

static void check_edgeevents(struct edge_state hist[NUM_DIRECTIONS], const char* srcname,
                             uint32_t old, uint32_t new, float xpos, float ypos)
{
	direction_t dir;
	dirmask_t dirmask;
	edgeevent_t edgeevtype;

	for_each_direction (dir) {
		dirmask = 1U << dir;
		if ((old & dirmask) != (new & dirmask)) {
			edgeevtype = (new & dirmask) ? EE_ARRIVE : EE_DEPART;
			if (trigger_edgeevent(&hist[dir], dir, edgeevtype, xpos, ypos))
				elog("out-of-sync edge event on %s ignored\n", srcname);
		}
	}
}

static void trigger_edgeevent_cb(uint32_t old, uint32_t new, float xpos, float ypos)
{
	check_edgeevents(config->master.edgehist, "master", old, new, xpos, ypos);
}

static void handle_message(struct remote* rmt, const struct message* msg)
{
	int loglen;
	char* logmsg;
	struct message* resp;

	switch (msg->type) {
	case MT_READY:
		if (rmt->state != CS_SETTINGUP) {
			fail_remote(rmt, "unexpected READY message");
			break;
		}
		rmt->state = CS_CONNECTED;
		rmt->failcount = 0;
		elog("remote '%s' becomes ready...\n", rmt->alias);
		if (config->focus_hint.type == FH_DIM_INACTIVE)
			transition_brightness(rmt, 1.0, config->focus_hint.brightness,
			                      config->focus_hint.duration,
			                      config->focus_hint.fade_steps);
		break;

	case MT_SETCLIPBOARD:
		if (rmt->state != CS_CONNECTED) {
			elog("got unexpected SETCLIPBOARD from non-connected "
			     "remote '%s', ignoring.\n", rmt->alias);
			break;
		}
		set_clipboard_from_buf(msg->extra.buf, msg->extra.len);
		if (focused_remote) {
			resp = new_message(MT_SETCLIPBOARD);
			resp->extra.buf = get_clipboard_text();
			resp->extra.len = strlen(resp->extra.buf);
			enqueue_message(focused_remote, resp);
		}
		break;

	case MT_LOGMSG:
		loglen = msg->extra.len > INT_MAX ? INT_MAX : msg->extra.len;
		logmsg = msg->extra.buf;
		elog("%s: %.*s%s", rmt->alias, loglen, logmsg,
		     logmsg[msg->extra.len-1] == '\n' ? "" : "\n");
		break;

	case MT_EDGEMASKCHANGE:
		if ((msg->edgemaskchange.old & ~ALLDIRS_MASK)
		    || (msg->edgemaskchange.new & ~ALLDIRS_MASK))
			fail_remote(rmt, "invalid edge mask");
		else
			check_edgeevents(rmt->edgehist, rmt->alias,
			                 msg->edgemaskchange.old, msg->edgemaskchange.new,
			                 msg->edgemaskchange.xpos, msg->edgemaskchange.ypos);
		break;

	default:
		fail_remote(rmt, "unexpected message type");
		break;
	}
}

static void read_rmtdata(struct remote* rmt)
{
	int status;
	struct message msg;

	status = recv_message(&rmt->msgchan, &msg);
	if (!status)
		return;

	if (status < 0) {
		fail_remote(rmt, "failed to receive valid message");
		return;
	}

	handle_message(rmt, &msg);

	if (msg.extra.len)
		xfree(msg.extra.buf);
}

static void write_rmtdata(struct remote* rmt)
{
	int status;

	status = send_message(&rmt->msgchan);
	if (status < 0)
		fail_remote(rmt, "failed to send message");
	else /* this function should only be called with pending send data */
		assert(status > 0);
}

static inline int have_outbound_data(const struct remote* rmt)
{
	return mc_have_outbound_data(&rmt->msgchan);
}

/*
 * Check if the given remote is in a state that would be eligible for sending
 * or receiving messages.  (remote_live() is admittedly not a great name for
 * this, but I can't think of anything better at the moment.)
 */
static inline int remote_live(const struct remote* rmt)
{
	return rmt->state == CS_CONNECTED || rmt->state == CS_SETTINGUP;
}

static void enqueue_scheduled_messages(struct remote* rmt, uint64_t when)
{
	struct message* msg;

	while (rmt->scheduled_messages && rmt->scheduled_messages->sendtime <= when) {
		msg = rmt->scheduled_messages;
		rmt->scheduled_messages = msg->next;
		enqueue_message(rmt, msg);
	}
}

static struct timeval* get_select_timeout(struct timeval* tv, uint64_t now_us)
{
	const struct remote* rmt;
	uint64_t maxwait_us;
	uint64_t next_scheduled_event = UINT64_MAX;

	if (scheduled_calls && scheduled_calls->calltime < next_scheduled_event)
		next_scheduled_event = scheduled_calls->calltime;

	for_each_remote (rmt) {
		if (rmt->state == CS_FAILED
		    && rmt->next_reconnect_time < next_scheduled_event)
			next_scheduled_event = rmt->next_reconnect_time;
		else if (rmt->scheduled_messages
		         && rmt->scheduled_messages->sendtime < next_scheduled_event)
			next_scheduled_event = rmt->scheduled_messages->sendtime;
	}

	if (next_scheduled_event == UINT64_MAX)
		return NULL;

	maxwait_us = next_scheduled_event - now_us;
	tv->tv_sec = maxwait_us / 1000000;
	tv->tv_usec = maxwait_us % 1000000;
	return tv;
}

static void handle_fds(int platform_event_fd)
{
	int status, nfds = 0;
	fd_set rfds, wfds;
	struct remote* rmt;
	struct timeval sel_wait;
	uint64_t now_us;

	FD_ZERO(&rfds);
	FD_ZERO(&wfds);

	now_us = get_microtime();

	run_scheduled_calls(now_us);

	for_each_remote (rmt) {
		if (rmt->state == CS_FAILED && rmt->next_reconnect_time < now_us)
			setup_remote(rmt);

		if (remote_live(rmt)) {
			enqueue_scheduled_messages(rmt, now_us);
			fdset_add(rmt->msgchan.recv_fd, &rfds, &nfds);
			if (have_outbound_data(rmt))
				fdset_add(rmt->msgchan.send_fd, &wfds, &nfds);
		}
	}

	fdset_add(platform_event_fd, &rfds, &nfds);

	status = select(nfds, &rfds, &wfds, NULL, get_select_timeout(&sel_wait, now_us));
	if (status < 0) {
		perror("select");
		exit(1);
	}

	for_each_remote (rmt) {
		if (remote_live(rmt) && FD_ISSET(rmt->msgchan.recv_fd, &rfds))
			read_rmtdata(rmt);

		/*
		 * read_rmtdata() might have changed the remote's status, so
		 * check remote_live() again.
		 */
		if (remote_live(rmt) && FD_ISSET(rmt->msgchan.send_fd, &wfds))
			write_rmtdata(rmt);
	}

	if (FD_ISSET(platform_event_fd, &rfds))
		process_events();
}

void usage(FILE* out)
{
	fprintf(out, "Usage: %s CONFIGFILE\n", progname);
}

int main(int argc, char** argv)
{
	int opt;
	struct config cfg;
	struct remote* rmt;
	FILE* cfgfile;
	struct stat st;
	int platform_event_fd;

	static const struct option options[] = {
		{ "help", no_argument, NULL, 'h', },
		{ NULL, 0, NULL, 0, },
	};

	if (strrchr(argv[0], '/'))
		progname = strrchr(argv[0], '/') + 1;
	else
		progname = argv[0];

	while ((opt = getopt_long(argc, argv, "h", options, NULL)) != -1) {
		switch (opt) {
		case 'h':
			usage(stdout);
			exit(0);

		default:
			elog("Unrecognized option: %c\n", opt);
			exit(1);
		}
	}

	argc -= optind;
	argv += optind;

	if (!argc) {
		/*
		 * If we've been properly invoked as a remote, stdin and
		 * stdout should not be TTYs...if they are, somebody's just
		 * run it without an argument not knowing any better and
		 * should get an error.
		 */
		if (isatty(STDIN_FILENO) || isatty(STDOUT_FILENO)) {
			usage(stderr);
			exit(1);
		}

		opmode = REMOTE;
		run_remote();
	} else if (argc == 1) {
		opmode = MASTER;
	} else {
		elog("excess arguments\n");
		exit(1);
	}

	if (platform_init(&platform_event_fd, trigger_edgeevent_cb)) {
		elog("platform_init failed\n");
		exit(1);
	}

	cfgfile = fopen(argv[0], "r");
	if (!cfgfile) {
		elog("%s: %s\n", argv[0], strerror(errno));
		exit(1);
	}

	if (fstat(fileno(cfgfile), &st)) {
		elog("fstat(%s): %s\n", argv[0], strerror(errno));
		exit(1);
	}

	if (st.st_uid != getuid()) {
		elog("Error: bad ownership on %s\n", argv[0]);
		exit(1);
	}

	if (st.st_mode & (S_IWGRP|S_IWOTH)) {
		elog("Error: bad permissions on %s (writable by others)\n", argv[0]);
		exit(1);
	}

	memset(&cfg, 0, sizeof(cfg));
	if (parse_cfg(cfgfile, &cfg))
		exit(1);
	fclose(cfgfile);
	config = &cfg;

	check_remotes();
	bind_hotkeys();

	for_each_remote (rmt)
		setup_remote(rmt);

	for (;;)
		handle_fds(platform_event_fd);
}
