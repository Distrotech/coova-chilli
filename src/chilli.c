/* 
 * Copyright (C) 2007-2009 Coova Technologies, LLC. <support@coova.com>
 * Copyright (C) 2006 PicoPoint B.V.
 * Copyright (C) 2003-2005 Mondru AB., 
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */

#include "system.h"
#include "tun.h"
#include "ippool.h"
#include "radius.h"
#include "radius_wispr.h"
#include "radius_chillispot.h"
#include "redir.h"
#include "syserr.h"
#include "dhcp.h"
#include "cmdline.h"
#include "chilli.h"
#include "options.h"
#include "cmdsock.h"
#include "net.h"

struct tun_t *tun;                /* TUN instance            */
struct ippool_t *ippool;          /* Pool of IP addresses */
struct radius_t *radius;          /* Radius client instance */
struct dhcp_t *dhcp = NULL;       /* DHCP instance */
struct redir_t *redir = NULL;     /* Redir instance */

#ifdef ENABLE_RTMON
#include "rtmon.h"
#endif

static int connections=0;
struct app_conn_t *firstfreeconn=0; /* First free in linked list */
struct app_conn_t *lastfreeconn=0;  /* Last free in linked list */
struct app_conn_t *firstusedconn=0; /* First used in linked list */
struct app_conn_t *lastusedconn=0;  /* Last used in linked list */

extern struct app_conn_t admin_session;

time_t mainclock;
time_t checktime;
time_t rereadtime;

static int *p_keep_going = 0;
static int *p_reload_config = 0;
/*static int do_timeouts = 1;*/
static int do_interval = 0;


/* some IPv4LL/APIPA(rfc 3927) specific stuff for uamanyip */
struct in_addr ipv4ll_ip;
struct in_addr ipv4ll_mask;

/* Forward declarations */
static int acct_req(struct app_conn_t *conn, uint8_t status_type);

static pid_t chilli_pid = 0;

#ifdef ENABLE_RTMON_
static pid_t rtmon_pid = 0;
#endif

#if defined(ENABLE_CHILLIPROXY) || defined(ENABLE_CHILLIRADSEC)
static pid_t proxy_pid = 0;
#endif

#ifdef ENABLE_CHILLIREDIR
static pid_t redir_pid = 0;
#endif

static void _sigchld(int signum) { 
  log_dbg("received %d signal", signum);
  /* catches falling childs and eliminates zombies */
  while (wait3(NULL, WNOHANG, NULL) > 0);
}

static void _sigterm(int signum) {
  log_dbg("SIGTERM: shutdown");
  if (p_keep_going)
    *p_keep_going = 0;
}

static void _sigvoid(int signum) {
  log_dbg("received %d signal", signum);
}

static void _sigusr1(int signum) {
  log_dbg("SIGUSR1: reloading configuration");

  if (p_reload_config)
    *p_reload_config = 1;

#ifdef ENABLE_CHILLIREDIR
  if (redir_pid) 
    kill(redir_pid, SIGUSR1);
#endif

#if defined(ENABLE_CHILLIPROXY) || defined(ENABLE_CHILLIRADSEC)
  if (proxy_pid) 
    kill(proxy_pid, SIGUSR1);
#endif
}

static void _sighup(int signum) {
  log_dbg("SIGHUP: rereading configuration");

  do_interval = 1;
}

void chilli_signals(int *with_term, int *with_hup) {
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  
  act.sa_handler = _sigchld;
  sigaction(SIGCHLD, &act, NULL);
  
  act.sa_handler = _sigvoid;
  sigaction(SIGPIPE, &act, NULL);

  if (with_hup) {
    p_reload_config = with_hup;
    act.sa_handler = _sighup;
    sigaction(SIGHUP, &act, NULL);

    act.sa_handler = _sigusr1;
    sigaction(SIGUSR1, &act, NULL);
  }

  if (with_term) {
    p_keep_going = with_term;
    act.sa_handler = _sigterm;
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGINT, &act, NULL);
  }
}

int chilli_binconfig(char *file, size_t flen, pid_t pid) {
  if (pid == 0) {
    char * bc = _options.binconfig;
    if (bc) {
      return snprintf(file, flen, "%s", bc);
    } else {
      if (pid == 0) pid = chilli_pid;
      if (pid == 0) pid = getpid();
    }
  }
  return snprintf(file, flen, "/tmp/chilli-%d/config.bin", pid);
}

time_t mainclock_tick() {
#ifdef HAVE_LIBRT
  struct timespec ts;
#if defined(CLOCK_SECOND)
  clockid_t cid = CLOCK_SECOND;
#elif defined(CLOCK_MONOTONIC)
  clockid_t cid = CLOCK_MONOTONIC;
#else
  clockid_t cid = CLOCK_REALTIME;
#endif
  int res = clock_gettime(cid, &ts);
  if (res == -1 && errno == EINVAL) {
    cid = CLOCK_REALTIME;
    res = clock_gettime(cid, &ts);
  }
  if (res == -1) {
    log_err(errno, "clock_gettime()");
    /* drop through to old time() */
  } else {
    mainclock = ts.tv_sec;
    return mainclock;
  }
#endif
  if (time(&mainclock) == (time_t)-1) {
    log_err(errno, "time()");
  }
  return mainclock;
}

time_t mainclock_now() {
  return mainclock;
}

time_t mainclock_rt() {
  time_t rt = 0;
#ifdef HAVE_LIBRT
  struct timespec ts;
  clockid_t cid = CLOCK_REALTIME;
  if (clock_gettime(cid, &ts) < 0) {
    log_err(errno, "clock_gettime()");
    /* drop through to old time() */
  } else {
    rt = ts.tv_sec;
    return rt;
  }
#endif
  if (time(&rt) == (time_t)-1) {
    log_err(errno, "time()");
  }
  return rt;
}

int mainclock_rtdiff(time_t past) {
  time_t rt = mainclock_rt();
  return (int) difftime(rt, past);
}

int mainclock_diff(time_t past) {
  return (int) (mainclock - past);
}

uint32_t mainclock_diffu(time_t past) {
  int i = mainclock_diff(past);
  if (i > 0) return (uint32_t) i;
  return 0;
}

static void set_sessionid(struct app_conn_t *appconn) {
  snprintf(appconn->s_state.sessionid, sizeof(appconn->s_state.sessionid), 
	   "%.8x%.8x", (int) mainclock_rt(), appconn->unit);
  /*log_dbg("!!!! RESET CLASSLEN !!!!");*/
  appconn->s_state.redir.classlen = 0;
}

/* Used to write process ID to file. Assume someone else will delete */
static void log_pid(char *pidfile) {
  FILE *file;
  mode_t oldmask;

  oldmask = umask(022);
  file = fopen(pidfile, "w");
  umask(oldmask);
  if(!file) return;
  fprintf(file, "%d\n", getpid());
  fclose(file);
}

#ifdef ENABLE_LEAKYBUCKET
/* Perform leaky bucket on up- and downlink traffic */
static inline int leaky_bucket(struct app_conn_t *conn, uint64_t octetsup, uint64_t octetsdown) {
  int result = 0;
  uint64_t timediff; 

  timediff = (uint64_t) mainclock_diffu(conn->s_state.last_time);

  if (_options.debug && (conn->s_params.bandwidthmaxup || conn->s_params.bandwidthmaxdown))
    log_dbg("Leaky bucket timediff: %lld, bucketup: %lld/%lld, bucketdown: %lld/%lld, up: %lld, down: %lld", 
	    timediff, 
	    conn->s_state.bucketup, conn->s_state.bucketupsize,
	    conn->s_state.bucketdown, conn->s_state.bucketdownsize,
	    octetsup, octetsdown);
  
  if (conn->s_params.bandwidthmaxup) {
    uint64_t bytes = (timediff * conn->s_params.bandwidthmaxup) / 8;

    if (!conn->s_state.bucketupsize) {
#ifdef BUCKET_SIZE
      conn->s_state.bucketupsize = BUCKET_SIZE;
#else
      conn->s_state.bucketupsize = conn->s_params.bandwidthmaxup / 8000 * BUCKET_TIME;
      if (conn->s_state.bucketupsize < BUCKET_SIZE_MIN) 
	conn->s_state.bucketupsize = BUCKET_SIZE_MIN;
#endif
    }

    if (conn->s_state.bucketup > bytes) {
      conn->s_state.bucketup -= bytes;
    }
    else {
      conn->s_state.bucketup = 0;
    }
    
    if ((conn->s_state.bucketup + octetsup) > conn->s_state.bucketupsize) {
      if (_options.debug) log_dbg("Leaky bucket deleting uplink packet");
      result = -1;
    }
    else {
      conn->s_state.bucketup += octetsup;
    }
  }

  if (conn->s_params.bandwidthmaxdown) {
    uint64_t bytes = (timediff * conn->s_params.bandwidthmaxdown) / 8;

    if (!conn->s_state.bucketdownsize) {
#ifdef BUCKET_SIZE
      conn->s_state.bucketdownsize = BUCKET_SIZE;
#else
      conn->s_state.bucketdownsize = conn->s_params.bandwidthmaxdown / 8000 * BUCKET_TIME;
      if (conn->s_state.bucketdownsize < BUCKET_SIZE_MIN) 
	conn->s_state.bucketdownsize = BUCKET_SIZE_MIN;
#endif
    }

    if (conn->s_state.bucketdown > bytes) {
      conn->s_state.bucketdown -= bytes;
    }
    else {
      conn->s_state.bucketdown = 0;
    }
    
    if ((conn->s_state.bucketdown + octetsdown) > conn->s_state.bucketdownsize) {
      if (_options.debug) log_dbg("Leaky bucket deleting downlink packet");
      result = -1;
    }
    else {
      conn->s_state.bucketdown += octetsdown;
    }
  }

  conn->s_state.last_time = mainclock;
    
  return result;
}
#endif


/* Run external script */
#define VAL_STRING   0
#define VAL_IN_ADDR  1
#define VAL_MAC_ADDR 2
#define VAL_ULONG    3
#define VAL_ULONG64  4
#define VAL_USHORT   5

int set_env(char *name, char type, void *value, int len) {
  char *v=0;
  char s[1024];

  memset(s,0,sizeof(s));

  switch(type) {

  case VAL_IN_ADDR:
    strncpy(s, inet_ntoa(*(struct in_addr *)value), sizeof(s)); 
    v = s;
    break;

  case VAL_MAC_ADDR:
    {
      uint8_t * mac = (uint8_t*)value;
      snprintf(s, sizeof(s)-1, "%.2X-%.2X-%.2X-%.2X-%.2X-%.2X",
	       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      v = s;
    }
    break;

  case VAL_ULONG:
    snprintf(s, sizeof(s)-1, "%ld", (long int)*(uint32_t *)value);
    v = s;
    break;

  case VAL_ULONG64:
    snprintf(s, sizeof(s)-1, "%ld", (long int)*(uint64_t *)value);
    v = s;
    break;

  case VAL_USHORT:
    snprintf(s, sizeof(s)-1, "%d", (int)(*(uint16_t *)value));
    v = s;
    break;

  case VAL_STRING:
    if (len != 0) {
      if (len >= sizeof(s)) {
	return -1;
      }
      strncpy(s, (char*)value, len);
      s[len] = 0;
      v = s;
    } else {
      v = (char*)value;
    }
    break;
  }

  if (name != NULL && v != NULL) {
    if (setenv(name, v, 1) != 0) {
      log_err(errno, "setenv(%s, %s, 1) did not return 0!", name, v);
      return -1;
    }
  }

  return 0;
}

int runscript(struct app_conn_t *appconn, char* script) {  
  int status;

  if ((status = fork()) < 0) {
    log_err(errno,
	    "fork() returned -1!");
    return 0;
  }

  if (status > 0) { /* Parent */
    return 0; 
  }

/*
  if (clearenv() != 0) {
    log_err(errno,
	    "clearenv() did not return 0!");
    exit(0);
  }
*/

  set_env("DEV", VAL_STRING, tun->_interfaces[0].devname, 0);
  set_env("NET", VAL_IN_ADDR, &appconn->net, 0);
  set_env("MASK", VAL_IN_ADDR, &appconn->mask, 0);
  set_env("ADDR", VAL_IN_ADDR, &appconn->ourip, 0);
  set_env("USER_NAME", VAL_STRING, appconn->s_state.redir.username, 0);
  set_env("NAS_IP_ADDRESS", VAL_IN_ADDR,&_options.radiuslisten, 0);
  set_env("SERVICE_TYPE", VAL_STRING, "1", 0);
  set_env("FRAMED_IP_ADDRESS", VAL_IN_ADDR, &appconn->hisip, 0);
  set_env("FILTER_ID", VAL_STRING, appconn->s_params.filteridbuf, 0);
  set_env("STATE", VAL_STRING, appconn->s_state.redir.statebuf, appconn->s_state.redir.statelen);
  set_env("CLASS", VAL_STRING, appconn->s_state.redir.classbuf, appconn->s_state.redir.classlen);
  set_env("SESSION_TIMEOUT", VAL_ULONG64, &appconn->s_params.sessiontimeout, 0);
  set_env("IDLE_TIMEOUT", VAL_ULONG, &appconn->s_params.idletimeout, 0);
  set_env("CALLING_STATION_ID", VAL_MAC_ADDR, appconn->hismac, 0);
  set_env("CALLED_STATION_ID", VAL_MAC_ADDR, /*appconn->ourmac*/dhcp_nexthop(dhcp), 0);
  set_env("NAS_ID", VAL_STRING, _options.radiusnasid, 0);
  set_env("NAS_PORT_TYPE", VAL_STRING, "19", 0);
  set_env("ACCT_SESSION_ID", VAL_STRING, appconn->s_state.sessionid, 0);
  set_env("ACCT_INTERIM_INTERVAL", VAL_USHORT, &appconn->s_params.interim_interval, 0);
  set_env("WISPR_LOCATION_ID", VAL_STRING, _options.radiuslocationid, 0);
  set_env("WISPR_LOCATION_NAME", VAL_STRING, _options.radiuslocationname, 0);
  set_env("WISPR_BANDWIDTH_MAX_UP", VAL_ULONG, &appconn->s_params.bandwidthmaxup, 0);
  set_env("WISPR_BANDWIDTH_MAX_DOWN", VAL_ULONG, &appconn->s_params.bandwidthmaxdown, 0);
  /*set_env("WISPR-SESSION_TERMINATE_TIME", VAL_USHORT, &appconn->sessionterminatetime, 0);*/
  set_env("CHILLISPOT_MAX_INPUT_OCTETS", VAL_ULONG64, &appconn->s_params.maxinputoctets, 0);
  set_env("CHILLISPOT_MAX_OUTPUT_OCTETS", VAL_ULONG64, &appconn->s_params.maxoutputoctets, 0);
  set_env("CHILLISPOT_MAX_TOTAL_OCTETS", VAL_ULONG64, &appconn->s_params.maxtotaloctets, 0);

  if (execl(script, script, (char *) 0) != 0) {
      log_err(errno,
	      "execl() did not return 0!");
      exit(0);
  }

  exit(0);
}

/***********************************************************
 *
 * Functions handling uplink protocol authentication.
 * Called in response to radius access request response.
 *
 ***********************************************************/

static int newip(struct ippoolm_t **ipm, struct in_addr *hisip, uint8_t *hismac) {
  struct in_addr tmpip;

  if (_options.autostatip && hismac) {
    if (!hisip) hisip = &tmpip;
    hisip->s_addr = htonl((_options.autostatip % 255) * 0x1000000 + 
			  hismac[3] * 0x10000 + 
			  hismac[4] * 0x100 + 
			  hismac[5]);
  }

  if (ippool_newip(ippool, ipm, hisip, 1)) {
    if (ippool_newip(ippool, ipm, hisip, 0)) {
      log_err(0, "Failed to allocate either static or dynamic IP address");
      return -1;
    }
  }
  return 0;
}


/* 
 * A few functions to manage connections 
 */

static int initconn() {
  checktime = rereadtime = mainclock;
  return 0;
}

int chilli_new_conn(struct app_conn_t **conn) {
  int n;

  if (!firstfreeconn) {

    if (connections == _options.max_clients) {
      log_err(0, "reached max connections!");
      return -1;
    }

    n = ++connections;

    if (!(*conn = calloc(1, sizeof(struct app_conn_t)))) {
      log_err(0, "Out of memory!");
      return -1;
    }

  } else {

    *conn = firstfreeconn;
    n = (*conn)->unit;

    /* Remove from link of free */
    if (firstfreeconn->next) {
      firstfreeconn->next->prev = NULL;
      firstfreeconn = firstfreeconn->next;
    }
    else { /* Took the last one */
      firstfreeconn = NULL; 
      lastfreeconn = NULL;
    }

    /* Initialise structures */
    memset(*conn, 0, sizeof(struct app_conn_t));
  }

  /* Insert into link of used */
  if (firstusedconn) {
    firstusedconn->prev = *conn;
    (*conn)->next = firstusedconn;
  }
  else { /* First insert */
    lastusedconn = *conn;
  }
  
  firstusedconn = *conn;
  
  (*conn)->inuse = 1;
  (*conn)->unit = n;
  
  return 0; /* Success */
}

int static freeconn(struct app_conn_t *conn) {
  int n = conn->unit;

  /* Remove from link of used */
  if ((conn->next) && (conn->prev)) {
    conn->next->prev = conn->prev;
    conn->prev->next = conn->next;
  }
  else if (conn->next) { /* && prev == 0 */
    conn->next->prev = NULL;
    firstusedconn = conn->next;
  }
  else if (conn->prev) { /* && next == 0 */
    conn->prev->next = NULL;
    lastusedconn = conn->prev;
  }
  else { /* if ((next == 0) && (prev == 0)) */
    firstusedconn = NULL;
    lastusedconn = NULL;
  }
  
  /* Initialise structures */
  memset(conn, 0, sizeof(struct app_conn_t));
  conn->unit = n;
  
  /* Insert into link of free */
  if (firstfreeconn) {
    firstfreeconn->prev = conn;
  }
  else { /* First insert */
    lastfreeconn = conn;
  }

  conn->next = firstfreeconn;
  firstfreeconn = conn;

  return 0;
}

int static getconn(struct app_conn_t **conn, uint32_t nasip, uint32_t nasport) {
  struct app_conn_t *appconn;
  
  /* Count the number of used connections */
  appconn = firstusedconn;
  while (appconn) {
    if (!appconn->inuse) {
      log_err(0, "Connection with inuse == 0!");
    }
    if ((appconn->nasip == nasip) && (appconn->nasport == nasport)) {
      *conn = appconn;
      return 0;
    }
    appconn = appconn->next;
  }

  return -1; /* Not found */
}

int static dnprot_terminate(struct app_conn_t *appconn) {
  appconn->s_state.authenticated = 0;
#ifdef HAVE_NETFILTER_COOVA
  kmod_coova_update(appconn);
#endif
  switch (appconn->dnprot) {
  case DNPROT_WPA:
  case DNPROT_EAPOL:
    if (appconn->dnlink)
      ((struct dhcp_conn_t*) appconn->dnlink)->authstate = DHCP_AUTH_NONE;
    break;
  case DNPROT_MAC:
  case DNPROT_UAM:
  case DNPROT_DHCP_NONE:
  case DNPROT_NULL:
    if (appconn->dnlink)
      ((struct dhcp_conn_t*) appconn->dnlink)->authstate = DHCP_AUTH_DNAT;
    break;
  default: 
    log_err(0, "Unknown downlink protocol"); 
    break;
  }
  return 0;
}



/* Check for:
 * - Session-Timeout
 * - Idle-Timeout
 * - Interim-Interim accounting
 * - Reread configuration file and DNS entries
 */

void session_interval(struct app_conn_t *conn) {
  uint32_t sessiontime;
  uint32_t idletime;
  uint32_t interimtime;

  sessiontime = mainclock_diffu(conn->s_state.start_time);
  idletime    = mainclock_diffu(conn->s_state.last_sent_time);
  interimtime = mainclock_diffu(conn->s_state.interim_time);

  /* debugging timeout information
  log_dbg("now:%d  sessiontime:%d  idle=%d  interim=%d  conn:(timeout:%d  idle:%d  interim:%d)", 
	  (int)mainclock,(int)sessiontime,(int)idletime,(int)interimtime,
	  (int)conn->s_params.sessiontimeout,
	  (int)conn->s_params.idletimeout,
	  (int)conn->s_params.interim_interval);
  */
  
  if ((conn->s_params.sessiontimeout) &&
      (sessiontime > conn->s_params.sessiontimeout)) {
    terminate_appconn(conn, RADIUS_TERMINATE_CAUSE_SESSION_TIMEOUT);
  }
  else if ((conn->s_params.sessionterminatetime) && 
	   (mainclock_rtdiff(conn->s_params.sessionterminatetime) > 0)) {
    terminate_appconn(conn, RADIUS_TERMINATE_CAUSE_SESSION_TIMEOUT);
  }
  else if ((conn->s_params.idletimeout) && 
	   (idletime > conn->s_params.idletimeout)) {
    terminate_appconn(conn, RADIUS_TERMINATE_CAUSE_IDLE_TIMEOUT);
  }
  else if ((conn->s_params.maxinputoctets) &&
	   (conn->s_state.input_octets > conn->s_params.maxinputoctets)) {
    terminate_appconn(conn, RADIUS_TERMINATE_CAUSE_SESSION_TIMEOUT);
  }
  else if ((conn->s_params.maxoutputoctets) &&
	   (conn->s_state.output_octets > conn->s_params.maxoutputoctets)) {
    terminate_appconn(conn, RADIUS_TERMINATE_CAUSE_SESSION_TIMEOUT);
  }
  else if ((conn->s_params.maxtotaloctets) &&
	   ((conn->s_state.input_octets + conn->s_state.output_octets) > 
	    conn->s_params.maxtotaloctets)) {
    terminate_appconn(conn, RADIUS_TERMINATE_CAUSE_SESSION_TIMEOUT);
  }
  else if ((conn->s_params.interim_interval) &&
	   (interimtime >= conn->s_params.interim_interval)) {
    acct_req(conn, RADIUS_STATUS_TYPE_INTERIM_UPDATE);
  }
}

static int checkconn() {
  struct app_conn_t *conn;
  struct dhcp_conn_t* dhcpconn;
  uint32_t checkdiff;
  uint32_t rereaddiff;

#ifdef HAVE_NETFILTER_COOVA
  kmod_coova_sync();
#endif

  checkdiff = mainclock_diffu(checktime);

  /*log_dbg("checkconn: %d %d %d", checktime, checkdiff, CHECK_INTERVAL);*/

  if (checkdiff < CHECK_INTERVAL)
    return 0;

  checktime = mainclock;
  
  if (admin_session.s_state.authenticated) {
    session_interval(&admin_session);
  }

  for (conn = firstusedconn; conn; conn=conn->next) {
    if ((conn->inuse != 0) && (conn->s_state.authenticated == 1)) {
      if (!(dhcpconn = (struct dhcp_conn_t *)conn->dnlink)) {
	log_warn(0, "No downlink protocol");
	continue;
      }
      session_interval(conn);
    }
  }
  
  /* Reread configuration file and recheck DNS */
  if (_options.interval) {
    rereaddiff = mainclock_diffu(rereadtime);
    if (rereaddiff >= _options.interval) {
      rereadtime = mainclock;
      do_interval = 1;
    }
  }
  
  return 0;
}

/* Kill all connections and send Radius Acct Stop */
int static killconn()
{
  struct app_conn_t *conn;

  for (conn = firstusedconn; conn; conn=conn->next) {
    if ((conn->inuse != 0) && (conn->s_state.authenticated == 1)) {
      terminate_appconn(conn, RADIUS_TERMINATE_CAUSE_NAS_REBOOT);
    }
  }

  if (admin_session.s_state.authenticated) {
    admin_session.s_state.terminate_cause = RADIUS_TERMINATE_CAUSE_NAS_REBOOT;
    acct_req(&admin_session, RADIUS_STATUS_TYPE_STOP);
  }

  acct_req(&admin_session, RADIUS_STATUS_TYPE_ACCOUNTING_OFF);

  return 0;
}

/* Compare a MAC address to the addresses given in the macallowed option */
int static maccmp(unsigned char *mac) {
  int i; 

  for (i=0; i<_options.macoklen; i++)
    if (!memcmp(mac, _options.macok[i], PKT_ETH_ALEN))
      return 0;

  return -1;
}

int static auth_radius(struct app_conn_t *appconn, 
		       char *username, char *password, 
		       uint8_t *dhcp_pkt, size_t dhcp_len) {
  
  struct dhcp_conn_t *dhcpconn = (struct dhcp_conn_t *)appconn->dnlink;
  struct radius_packet_t radius_pack;
  char mac[MACSTRLEN+1];

  uint32_t service_type = RADIUS_SERVICE_TYPE_LOGIN;

  log_dbg("Starting radius authentication");

  if (radius_default_pack(radius, &radius_pack, RADIUS_CODE_ACCESS_REQUEST)) {
    log_err(0, "radius_default_pack() failed");
    return -1;
  }

  /* Include his MAC address */
  snprintf(mac, MACSTRLEN+1, "%.2X-%.2X-%.2X-%.2X-%.2X-%.2X",
	   dhcpconn->hismac[0], dhcpconn->hismac[1],
	   dhcpconn->hismac[2], dhcpconn->hismac[3],
	   dhcpconn->hismac[4], dhcpconn->hismac[5]);

  if (!username) {
    service_type = RADIUS_SERVICE_TYPE_FRAMED;

    strncpy(appconn->s_state.redir.username, mac, USERNAMESIZE);

    if (_options.macsuffix)
      strncat(appconn->s_state.redir.username, _options.macsuffix, USERNAMESIZE);
  
    username = appconn->s_state.redir.username;
  } else {
    strncpy(appconn->s_state.redir.username, username, USERNAMESIZE);
  }

  if (!password) {
    password = _options.macpasswd;
    if (!password) {
      password = appconn->s_state.redir.username;
    }
  }

  radius_addattr(radius, &radius_pack, RADIUS_ATTR_USER_NAME, 0, 0, 0,
		 (uint8_t *) username, strlen(username));
  
  radius_addattr(radius, &radius_pack, RADIUS_ATTR_USER_PASSWORD, 0, 0, 0,
		 (uint8_t *) password, strlen(password)); 
  
  appconn->authtype = PAP_PASSWORD;
  
  radius_addattr(radius, &radius_pack, RADIUS_ATTR_CALLING_STATION_ID, 0, 0, 0,
		 (uint8_t *) mac, MACSTRLEN);
  
  radius_addcalledstation(radius, &radius_pack);

  radius_addattr(radius, &radius_pack, RADIUS_ATTR_NAS_PORT, 0, 0,
		 appconn->unit, NULL, 0);

  radius_addnasip(radius, &radius_pack);

  radius_addattr(radius, &radius_pack, RADIUS_ATTR_SERVICE_TYPE, 0, 0,
		 service_type, NULL, 0); 

  /* Include NAS-Identifier if given in configuration options */
  if (_options.radiusnasid)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_NAS_IDENTIFIER, 0, 0, 0,
		   (uint8_t*) _options.radiusnasid, strlen(_options.radiusnasid));

  radius_addattr(radius, &radius_pack, RADIUS_ATTR_ACCT_SESSION_ID, 0, 0, 0,
		 (uint8_t*) appconn->s_state.sessionid, REDIR_SESSIONID_LEN-1);

  radius_addattr(radius, &radius_pack, RADIUS_ATTR_NAS_PORT_TYPE, 0, 0,
		 _options.radiusnasporttype, NULL, 0);


  if (_options.radiuslocationid)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
		   RADIUS_VENDOR_WISPR, RADIUS_ATTR_WISPR_LOCATION_ID, 0,
		   (uint8_t*) _options.radiuslocationid, 
		   strlen(_options.radiuslocationid));

  if (_options.radiuslocationname)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
		   RADIUS_VENDOR_WISPR, RADIUS_ATTR_WISPR_LOCATION_NAME, 0,
		   (uint8_t*) _options.radiuslocationname, 
		   strlen(_options.radiuslocationname));
  
#ifdef ENABLE_IEEE8021Q
  if (dhcpconn->tag8021q)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
		   RADIUS_VENDOR_CHILLISPOT, RADIUS_ATTR_CHILLISPOT_VLAN_ID, 
		   (uint32_t)(ntohs(dhcpconn->tag8021q) & 0x0FFF), 0, 0);
#endif

  if (_options.dhcpradius && dhcp_pkt) {
    struct dhcp_tag_t *tag = 0;
    struct pkt_udphdr_t *udph = udphdr(dhcp_pkt);
    struct dhcp_packet_t *dhcppkt = dhcppkt(dhcp_pkt);

#define maptag(OPT,VSA)\
    if (!dhcp_gettag(dhcppkt, ntohs(udph->len)-PKT_UDP_HLEN, &tag, OPT)) { \
      radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC, \
		     RADIUS_VENDOR_CHILLISPOT, VSA, 0, (uint8_t *) tag->v, tag->l); } 
    /*
     *  Mapping of DHCP options to RADIUS Vendor Specific Attributes
     */
    maptag( DHCP_OPTION_PARAMETER_REQUEST_LIST,  RADIUS_ATTR_CHILLISPOT_DHCP_PARAMETER_REQUEST_LIST );
    maptag( DHCP_OPTION_VENDOR_CLASS_IDENTIFIER, RADIUS_ATTR_CHILLISPOT_DHCP_VENDOR_CLASS_ID );
    maptag( DHCP_OPTION_CLIENT_IDENTIFIER,       RADIUS_ATTR_CHILLISPOT_DHCP_CLIENT_ID );
    maptag( DHCP_OPTION_CLIENT_FQDN,             RADIUS_ATTR_CHILLISPOT_DHCP_CLIENT_FQDN );
    maptag( DHCP_OPTION_HOSTNAME,                RADIUS_ATTR_CHILLISPOT_DHCP_HOSTNAME );
#undef maptag
  }

#ifdef ENABLE_PROXYVSA
  radius_addvsa(&radius_pack, &appconn->s_state.redir);
#endif
  
  radius_addattr(radius, &radius_pack, RADIUS_ATTR_MESSAGE_AUTHENTICATOR, 
		 0, 0, 0, NULL, RADIUS_MD5LEN);

  return radius_req(radius, &radius_pack, appconn);
}


/*********************************************************
 *
 * radius proxy functions
 * Used to send a response to a received radius request
 *
 *********************************************************/

/* Reply with an access reject */
int static radius_access_reject(struct app_conn_t *conn) {
  struct radius_packet_t radius_pack;

  conn->radiuswait = 0;

  if (radius_default_pack(radius, &radius_pack, RADIUS_CODE_ACCESS_REJECT)) {
    log_err(0, "radius_default_pack() failed");
    return -1;
  }

  radius_pack.id = conn->radiusid;
  radius_resp(radius, &radius_pack, &conn->radiuspeer, conn->authenticator);
  return 0;
}

/* Reply with an access challenge */
int static radius_access_challenge(struct app_conn_t *conn) {
  struct radius_packet_t radius_pack;
  size_t offset = 0;
  size_t eaplen = 0;

  log_dbg("Sending RADIUS AccessChallenge to client");

  conn->radiuswait = 0;

  if (radius_default_pack(radius, &radius_pack, RADIUS_CODE_ACCESS_CHALLENGE)){
    log_err(0, "radius_default_pack() failed");
    return -1;
  }

  radius_pack.id = conn->radiusid;

  /* Include EAP */
  do {
    if ((conn->challen - offset) > RADIUS_ATTR_VLEN)
      eaplen = RADIUS_ATTR_VLEN;
    else
      eaplen = conn->challen - offset;

    if (radius_addattr(radius, &radius_pack, RADIUS_ATTR_EAP_MESSAGE, 0, 0, 0,
		       conn->chal + offset, eaplen)) {
      log_err(0, "radius_default_pack() failed");
      return -1;
    }
    offset += eaplen;
  }
  while (offset < conn->challen);
  
  if (conn->s_state.redir.statelen) {
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_STATE, 0, 0, 0,
		   conn->s_state.redir.statebuf,
		   conn->s_state.redir.statelen);
  }
  
  radius_addattr(radius, &radius_pack, RADIUS_ATTR_MESSAGE_AUTHENTICATOR, 
		 0, 0, 0, NULL, RADIUS_MD5LEN);
  
  radius_resp(radius, &radius_pack, &conn->radiuspeer, conn->authenticator);
  return 0;
}

/* Send off an access accept */

int static radius_access_accept(struct app_conn_t *conn) {
  struct radius_packet_t radius_pack;
  size_t offset = 0;
  size_t eaplen = 0;

  uint8_t mppekey[RADIUS_ATTR_VLEN];
  size_t mppelen;

  conn->radiuswait = 0;

  if (radius_default_pack(radius, &radius_pack, RADIUS_CODE_ACCESS_ACCEPT)) {
    log_err(0, "radius_default_pack() failed");
    return -1;
  }

  radius_pack.id = conn->radiusid;

  /* Include EAP (if present) */
  offset = 0;
  while (offset < conn->challen) {
    if ((conn->challen - offset) > RADIUS_ATTR_VLEN)
      eaplen = RADIUS_ATTR_VLEN;
    else
      eaplen = conn->challen - offset;

    radius_addattr(radius, &radius_pack, RADIUS_ATTR_EAP_MESSAGE, 0, 0, 0,
		   conn->chal + offset, eaplen);

    offset += eaplen;
  }

  if (conn->sendlen) {
    radius_keyencode(radius, mppekey, RADIUS_ATTR_VLEN,
		     &mppelen, conn->sendkey,
		     conn->sendlen, conn->authenticator,
		     radius->proxysecret, radius->proxysecretlen);

    radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
		   RADIUS_VENDOR_MS, RADIUS_ATTR_MS_MPPE_SEND_KEY, 0,
		   (uint8_t *)mppekey, mppelen);
  }

  if (conn->recvlen) {
    radius_keyencode(radius, mppekey, RADIUS_ATTR_VLEN,
		     &mppelen, conn->recvkey,
		     conn->recvlen, conn->authenticator,
		     radius->proxysecret, radius->proxysecretlen);
    
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
		   RADIUS_VENDOR_MS, RADIUS_ATTR_MS_MPPE_RECV_KEY, 0,
		   (uint8_t *)mppekey, mppelen);
  }
  
  radius_addattr(radius, &radius_pack, RADIUS_ATTR_MESSAGE_AUTHENTICATOR, 
		 0, 0, 0, NULL, RADIUS_MD5LEN);

  radius_resp(radius, &radius_pack, &conn->radiuspeer, conn->authenticator);
  return 0;
}


/*********************************************************
 *
 * radius accounting functions
 * Used to send accounting request to radius server
 *
 *********************************************************/

static int acct_req(struct app_conn_t *conn, uint8_t status_type)
{
  struct radius_packet_t radius_pack;
  char mac[MACSTRLEN+1];
  char portid[16+1];
  uint32_t timediff;

  if (RADIUS_STATUS_TYPE_START == status_type ||
      RADIUS_STATUS_TYPE_ACCOUNTING_ON == status_type) {
    conn->s_state.start_time = mainclock;
    conn->s_state.interim_time = mainclock;
    conn->s_state.last_time = mainclock;
    conn->s_state.last_sent_time = mainclock;
    conn->s_state.input_packets = 0;
    conn->s_state.output_packets = 0;
    conn->s_state.input_octets = 0;
    conn->s_state.output_octets = 0;
  }

  if (RADIUS_STATUS_TYPE_INTERIM_UPDATE == status_type) {
    conn->s_state.interim_time = mainclock;
  }

  if (radius_default_pack(radius, &radius_pack, 
			  RADIUS_CODE_ACCOUNTING_REQUEST)) {
    log_err(0, "radius_default_pack() failed");
    return -1;
  }

  radius_addattr(radius, &radius_pack, RADIUS_ATTR_ACCT_STATUS_TYPE, 0, 0,
		 status_type, NULL, 0);

  if (RADIUS_STATUS_TYPE_ACCOUNTING_ON != status_type &&
      RADIUS_STATUS_TYPE_ACCOUNTING_OFF != status_type) {

    radius_addattr(radius, &radius_pack, RADIUS_ATTR_USER_NAME, 0, 0, 0,
		   (uint8_t*) conn->s_state.redir.username, 
		   strlen(conn->s_state.redir.username));
    
    if (conn->s_state.redir.classlen) {
      radius_addattr(radius, &radius_pack, RADIUS_ATTR_CLASS, 0, 0, 0,
		     conn->s_state.redir.classbuf,
		     conn->s_state.redir.classlen);
    }

    if (conn->is_adminsession) {

      radius_addattr(radius, &radius_pack, RADIUS_ATTR_SERVICE_TYPE, 0, 0,
		     RADIUS_SERVICE_TYPE_ADMIN_USER, NULL, 0); 

#ifdef HAVE_SYS_SYSINFO_H
      {
	struct sysinfo the_info;
	
	if (sysinfo(&the_info)) {
	  
	  log_err(errno, "sysinfo()");
	  
	} else {
	  float shiftfloat;
	  float fav[3];
	  char b[128];
	  
	  shiftfloat = (float) (1<<SI_LOAD_SHIFT);
	  
	  fav[0]=((float)the_info.loads[0])/shiftfloat;
	  fav[1]=((float)the_info.loads[1])/shiftfloat;
	  fav[2]=((float)the_info.loads[2])/shiftfloat;

	  radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
			 RADIUS_VENDOR_CHILLISPOT, RADIUS_ATTR_CHILLISPOT_SYS_UPTIME, 
			 (uint32_t) the_info.uptime, NULL, 0);
	  
	  snprintf(b, sizeof(b), "%f %f %f",fav[0],fav[1],fav[2]);
	  
	  radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
			 RADIUS_VENDOR_CHILLISPOT, RADIUS_ATTR_CHILLISPOT_SYS_LOADAVG, 
			 0, (uint8_t *) b, strlen(b));
	  
	  snprintf(b, sizeof(b), "%ld %ld %ld %ld",
		   the_info.totalram,
		   the_info.freeram,
		   the_info.sharedram,
		   the_info.bufferram);
	  
	  radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
			 RADIUS_VENDOR_CHILLISPOT, RADIUS_ATTR_CHILLISPOT_SYS_MEMORY, 
			 0, (uint8_t *) b, strlen(b));
	}
      }
#endif

    } else {
      snprintf(mac, MACSTRLEN+1, "%.2X-%.2X-%.2X-%.2X-%.2X-%.2X",
	       conn->hismac[0], conn->hismac[1],
	       conn->hismac[2], conn->hismac[3],
	       conn->hismac[4], conn->hismac[5]);
      
      radius_addattr(radius, &radius_pack, RADIUS_ATTR_CALLING_STATION_ID, 0, 0, 0,
		     (uint8_t*) mac, MACSTRLEN);
      
      radius_addattr(radius, &radius_pack, RADIUS_ATTR_NAS_PORT_TYPE, 0, 0,
		     _options.radiusnasporttype, NULL, 0);
      
      radius_addattr(radius, &radius_pack, RADIUS_ATTR_NAS_PORT, 0, 0,
		     conn->unit, NULL, 0);

      snprintf(portid, 16+1, "%.8d", conn->unit);
      radius_addattr(radius, &radius_pack, RADIUS_ATTR_NAS_PORT_ID, 0, 0, 0,
		     (uint8_t*) portid, strlen(portid));

      radius_addattr(radius, &radius_pack, RADIUS_ATTR_FRAMED_IP_ADDRESS, 0, 0,
		     ntohl(conn->hisip.s_addr), NULL, 0);

#ifdef ENABLE_IEEE8021Q
      if (conn->s_state.tag8021q)
	radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
		       RADIUS_VENDOR_CHILLISPOT, RADIUS_ATTR_CHILLISPOT_VLAN_ID, 
		       (uint32_t)(ntohs(conn->s_state.tag8021q) & 0x0FFF), 0, 0);
#endif
      
    }
    
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_ACCT_SESSION_ID, 0, 0, 0,
		   (uint8_t*) conn->s_state.sessionid, REDIR_SESSIONID_LEN-1);
    
  }

  radius_addnasip(radius, &radius_pack);

  radius_addcalledstation(radius, &radius_pack);


  /* Include NAS-Identifier if given in configuration options */
  if (_options.radiusnasid)
    (void) radius_addattr(radius, &radius_pack, RADIUS_ATTR_NAS_IDENTIFIER, 0, 0, 0,
		   (uint8_t*) _options.radiusnasid, 
		   strlen(_options.radiusnasid));

  /*
  (void) radius_addattr(radius, &radius_pack, RADIUS_ATTR_FRAMED_MTU, 0, 0,
  conn->mtu, NULL, 0);*/

  if ((status_type == RADIUS_STATUS_TYPE_STOP) ||
      (status_type == RADIUS_STATUS_TYPE_INTERIM_UPDATE)) {

    radius_addattr(radius, &radius_pack, RADIUS_ATTR_ACCT_INPUT_OCTETS, 0, 0,
		   (uint32_t) conn->s_state.input_octets, NULL, 0);
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_ACCT_OUTPUT_OCTETS, 0, 0,
		   (uint32_t) conn->s_state.output_octets, NULL, 0);

    radius_addattr(radius, &radius_pack, RADIUS_ATTR_ACCT_INPUT_GIGAWORDS, 
		   0, 0, (uint32_t) (conn->s_state.input_octets >> 32), NULL, 0);
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_ACCT_OUTPUT_GIGAWORDS, 
		   0, 0, (uint32_t) (conn->s_state.output_octets >> 32), NULL, 0);

    radius_addattr(radius, &radius_pack, RADIUS_ATTR_ACCT_INPUT_PACKETS, 0, 0,
		   conn->s_state.input_packets, NULL, 0);
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_ACCT_OUTPUT_PACKETS, 0, 0,
		   conn->s_state.output_packets, NULL, 0);

    timediff = mainclock_diffu(conn->s_state.start_time);

    radius_addattr(radius, &radius_pack, RADIUS_ATTR_ACCT_SESSION_TIME, 0, 0,
		   timediff, NULL, 0);  
  }

  if (_options.radiuslocationid)
    (void) radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
		   RADIUS_VENDOR_WISPR, RADIUS_ATTR_WISPR_LOCATION_ID, 0,
		   (uint8_t*) _options.radiuslocationid,
		   strlen(_options.radiuslocationid));

  if (_options.radiuslocationname)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
		   RADIUS_VENDOR_WISPR, RADIUS_ATTR_WISPR_LOCATION_NAME, 0,
		   (uint8_t*) _options.radiuslocationname, 
		   strlen(_options.radiuslocationname));


  if (status_type == RADIUS_STATUS_TYPE_STOP ||
      status_type == RADIUS_STATUS_TYPE_ACCOUNTING_OFF) {

    radius_addattr(radius, &radius_pack, RADIUS_ATTR_ACCT_TERMINATE_CAUSE, 
		   0, 0, conn->s_state.terminate_cause, NULL, 0);

    if (status_type == RADIUS_STATUS_TYPE_STOP) {
      /* TODO: This probably belongs somewhere else */
      if (_options.condown) {
	log_dbg("Calling connection down script: %s\n",_options.condown);
	runscript(conn, _options.condown);
      }
    }
  }
  
  radius_req(radius, &radius_pack, conn);
  
  return 0;
}

/**
 * Assigns an ip from the dynamic pool, for SNAT'ing anyip connections.
 * If anyip is on and the clients address is outside of our network,
 * we need to SNAT to an ip of our network.
 */
int chilli_assign_snat(struct app_conn_t *appconn, int force) {
  struct ippoolm_t *newipm;
  
  if (!_options.uamanyip) return 0;
  if (!_options.uamnatanyip) return 0;
  if (appconn->natip.s_addr && !force) return 0;

  /* check if excluded from anyip */
  if (_options.anyipexclude_addr.s_addr &&
      (appconn->hisip.s_addr & _options.anyipexclude_mask.s_addr) == _options.anyipexclude_addr.s_addr) {
    log_dbg("Excluding ip %s from SNAT becuase it is in anyipexclude", inet_ntoa(appconn->hisip));
    return 0;
  }
  
  if ((appconn->hisip.s_addr & appconn->mask.s_addr) ==
      (appconn->ourip.s_addr & appconn->mask.s_addr))
    return 0;

  log_dbg("Request SNAT ip for client ip: %s",
	  inet_ntoa(appconn->hisip));

  if (ippool_newip(ippool, &newipm, &appconn->natip, 0)) {
    log_err(0, "Failed to allocate SNAT IP address");
    /*
     *  Clean up the static pool listing too, it's misconfigured now.
     */ 
    if (appconn->dnlink) {
      dhcp_freeconn((struct dhcp_conn_t *)appconn->dnlink, 0);
    }
    return -1;
  }
  
  appconn->natip.s_addr = newipm->addr.s_addr;
  newipm->peer = appconn;

  log_dbg("SNAT IP %s assigned", inet_ntoa(appconn->natip));

  return 0;
}



/***********************************************************
 *
 * Functions handling downlink protocol authentication.
 * Called in response to radius access request response.
 *
 ***********************************************************/

int static dnprot_reject(struct app_conn_t *appconn) {
  struct dhcp_conn_t* dhcpconn = NULL;
  /*struct ippoolm_t *ipm;*/

  if (appconn->is_adminsession) return 0;

  switch (appconn->dnprot) {

  case DNPROT_EAPOL:
    if (!(dhcpconn = (struct dhcp_conn_t*) appconn->dnlink)) {
      log_err(0, "No downlink protocol");
      return 0;
    }

    dhcp_sendEAPreject(dhcpconn, NULL, 0);
    return 0;

  case DNPROT_UAM:
    log_dbg(0, "Rejecting UAM");
    return 0;

  case DNPROT_WPA:
    return radius_access_reject(appconn);

  case DNPROT_MAC:
    /* remove the username since we're not logged in */
    if (!appconn->s_state.authenticated)
      strncpy(appconn->s_state.redir.username, "-", USERNAMESIZE);

    if (!(dhcpconn = (struct dhcp_conn_t *)appconn->dnlink)) {
      log_err(0, "No downlink protocol");
      return 0;
    }

    if (_options.macauthdeny) {
      dhcpconn->authstate = DHCP_AUTH_DROP;
      appconn->dnprot = DNPROT_NULL;
    }
    else {
      dhcpconn->authstate = DHCP_AUTH_NONE;
      appconn->dnprot = DNPROT_UAM;
    }

    return 0;    

  case DNPROT_NULL:
    return 0;

  default:
    log_err(0, "Unknown downlink protocol");
    return 0;
  }
}

int static dnprot_challenge(struct app_conn_t *appconn) {
  struct dhcp_conn_t* dhcpconn = NULL;

  switch (appconn->dnprot) {

  case DNPROT_EAPOL:
    if (!(dhcpconn = (struct dhcp_conn_t *)appconn->dnlink)) {
      log_err(0, "No downlink protocol");
      return 0;
    }

    dhcp_sendEAP(dhcpconn, appconn->chal, appconn->challen);
    break;

  case DNPROT_NULL:
  case DNPROT_UAM:
  case DNPROT_MAC:
    break;

  case DNPROT_WPA:
    radius_access_challenge(appconn);
    break;

  default:
    log_err(0, "Unknown downlink protocol");
  }

  return 0;
}

int static dnprot_accept(struct app_conn_t *appconn) {
  struct dhcp_conn_t* dhcpconn = NULL;
  
  if (appconn->is_adminsession) return 0;

  if (!appconn->hisip.s_addr) {
    log_err(0, "IP address not allocated");
    return 0;
  }

  switch (appconn->dnprot) {
  case DNPROT_EAPOL:
    if (!(dhcpconn = (struct dhcp_conn_t *)appconn->dnlink)) {
      log_err(0, "No downlink protocol");
      return 0;
    }

    dhcp_set_addrs(dhcpconn, 
		   &appconn->hisip, &appconn->mask,
		   &appconn->ourip, &appconn->mask,
		   &appconn->dns1, &appconn->dns2,
		   _options.domain);
    
    /* This is the one and only place eapol authentication is accepted */

    dhcpconn->authstate = DHCP_AUTH_PASS;

    /* Tell client it was successful */
    dhcp_sendEAP(dhcpconn, appconn->chal, appconn->challen);

    log_warn(0, "Do not know how to set encryption keys on this platform!");
    break;

  case DNPROT_UAM:
    if (!(dhcpconn = (struct dhcp_conn_t *)appconn->dnlink)) {
      log_err(0, "No downlink protocol");
      return 0;
    }

    dhcp_set_addrs(dhcpconn, 
		   &appconn->hisip, &appconn->mask,
		   &appconn->ourip, &appconn->mask,
		   &appconn->dns1, &appconn->dns2,
		   _options.domain);

    /* This is the one and only place UAM authentication is accepted */
    dhcpconn->authstate = DHCP_AUTH_PASS;
    appconn->s_params.flags &= ~REQUIRE_UAM_AUTH;
    break;

  case DNPROT_WPA:
    if (!(dhcpconn = (struct dhcp_conn_t *)appconn->dnlink)) {
      log_err(0, "No downlink protocol");
      return 0;
    }

    dhcp_set_addrs(dhcpconn, 
		   &appconn->hisip, &appconn->mask, 
		   &appconn->ourip, &appconn->mask, 
		   &appconn->dns1, &appconn->dns2,
		   _options.domain);
    
    /* This is the one and only place WPA authentication is accepted */
    if (appconn->s_params.flags & REQUIRE_UAM_AUTH) {
      appconn->dnprot = DNPROT_DHCP_NONE;
      dhcpconn->authstate = DHCP_AUTH_NONE;
    }
    else {
      dhcpconn->authstate = DHCP_AUTH_PASS;
    }
    
    /* Tell access point it was successful */
    radius_access_accept(appconn);

    break;

  case DNPROT_MAC:
    if (!(dhcpconn = (struct dhcp_conn_t *)appconn->dnlink)) {
      log_err(0, "No downlink protocol");
      return 0;
    }

    dhcp_set_addrs(dhcpconn, 
		   &appconn->hisip, &appconn->mask, 
		   &appconn->ourip, &appconn->mask, 
		   &appconn->dns1, &appconn->dns2,
		   _options.domain);
    
    dhcpconn->authstate = DHCP_AUTH_PASS;
    break;

  case DNPROT_NULL:
  case DNPROT_DHCP_NONE:
    return 0;

  default:
    log_err(0, "Unknown downlink protocol");
    return 0;
  }

  if (appconn->s_params.flags & REQUIRE_UAM_SPLASH)
    dhcpconn->authstate = DHCP_AUTH_SPLASH;
  
  if (!(appconn->s_params.flags & REQUIRE_UAM_AUTH)) {
    /* This is the one and only place state is switched to authenticated */
    appconn->s_state.authenticated = 1;

#ifdef HAVE_NETFILTER_COOVA
    kmod_coova_update(appconn);
#endif
    
    /* Run connection up script */
    if (_options.conup) {
      log_dbg("Calling connection up script: %s\n", _options.conup);
      runscript(appconn, _options.conup);
    }
    
    if (!(appconn->s_params.flags & IS_UAM_REAUTH))
      acct_req(appconn, RADIUS_STATUS_TYPE_START);
  }
  
  appconn->s_params.flags &= ~IS_UAM_REAUTH;

#ifdef ENABLE_STATFILE
  printstatus();
#endif
    
  return 0;
}


/*
 * Tun callbacks
 *
 * Called from the tun_decaps function. This method is passed either
 * a Ethernet frame or an IP packet. 
 */

int cb_tun_ind(struct tun_t *tun, void *pack, size_t len, int idx) {
  struct in_addr dst;
  struct ippoolm_t *ipm;
  struct app_conn_t *appconn;
  struct pkt_ipphdr_t *ipph;
  
  int ethhdr = (tun(tun, idx).flags & NET_ETHHDR) != 0;

  if (ethhdr) {
    /*
     *   Will never be 802.1Q
     */
    struct pkt_ethhdr_t *ethh = ethhdr(pack);
    uint16_t prot = ntohs(ethh->prot);

    ipph = (struct pkt_ipphdr_t *)((char *)pack + PKT_ETH_HLEN);

    if (prot == PKT_ETH_PROTO_ARP) {
      /*
       *  Send arp reply 
       */
      uint8_t packet[PKT_BUFFER];

      struct pkt_ethhdr_t *p_ethh = ethhdr(pack);
      struct arp_packet_t *p_arp = arppkt(pack);
      struct pkt_ethhdr_t *packet_ethh = ethhdr(packet);
      struct arp_packet_t *packet_arp = ((struct arp_packet_t *)(((uint8_t*)(pack)) + PKT_ETH_HLEN));

      size_t length = PKT_ETH_HLEN + sizeof(struct arp_packet_t);

      struct in_addr reqaddr;
      
      /* 
       *   Get local copy of the target address to resolve
       */
      memcpy(&reqaddr.s_addr, p_arp->tpa, PKT_IP_ALEN);

      if (_options.debug)
	log_dbg("arp: ifidx=%d src=%.2x:%.2x:%.2x:%.2x:%.2x:%.2x dst=%.2x:%.2x:%.2x:%.2x:%.2x:%.2x "
		"prot=%.4x (asking for %s)",
		tun(tun,idx).ifindex,
		ethh->src[0],ethh->src[1],ethh->src[2],ethh->src[3],ethh->src[4],ethh->src[5],
		ethh->dst[0],ethh->dst[1],ethh->dst[2],ethh->dst[3],ethh->dst[4],ethh->dst[5],
		ntohs(ethh->prot), inet_ntoa(reqaddr));
      
      /*
       *  Lookup request address, see if we control it.
       */
      if (ippool_getip(ippool, &ipm, &reqaddr)) {
	if (_options.debug) 
	  log_dbg("ARP for unknown IP %s", inet_ntoa(reqaddr));
	return 0;
      }
      
      if ((appconn  = (struct app_conn_t *)ipm->peer) == NULL ||
	  (appconn->dnlink) == NULL) {
	log_err(0, "No peer protocol defined for ARP request");
	return 0;
      }
      
      /* Get packet default values */
      memset(&packet, 0, sizeof(packet));
      
      /* ARP Payload */
      packet_arp->hrd = htons(DHCP_HTYPE_ETH);
      packet_arp->pro = htons(PKT_ETH_PROTO_IP);
      packet_arp->hln = PKT_ETH_ALEN;
      packet_arp->pln = PKT_IP_ALEN;
      packet_arp->op  = htons(DHCP_ARP_REPLY);
      
      /* Source address */
      memcpy(packet_arp->sha, appconn->hismac, PKT_ETH_ALEN);
      memcpy(packet_arp->spa, &appconn->hisip.s_addr, PKT_IP_ALEN);

      /* Target address */
      memcpy(packet_arp->tha, p_arp->sha, PKT_ETH_ALEN);
      memcpy(packet_arp->tpa, p_arp->spa, PKT_IP_ALEN);

      /* Ethernet header */
      memcpy(packet_ethh->dst, p_ethh->src, PKT_ETH_ALEN);
      memcpy(packet_ethh->src, appconn->hismac, PKT_ETH_ALEN);

      /*memcpy(packet.ethh.src, dhcp->rawif.hwaddr, PKT_ETH_ALEN);*/

      packet_ethh->prot = htons(PKT_ETH_PROTO_ARP);

      if (_options.debug) {
	log_dbg("arp-reply: src=%.2x:%.2x:%.2x:%.2x:%.2x:%.2x dst=%.2x:%.2x:%.2x:%.2x:%.2x:%.2x",
		packet_ethh->src[0],packet_ethh->src[1],packet_ethh->src[2],
		packet_ethh->src[3],packet_ethh->src[4],packet_ethh->src[5],
		packet_ethh->dst[0],packet_ethh->dst[1],packet_ethh->dst[2],
		packet_ethh->dst[3],packet_ethh->dst[4],packet_ethh->dst[5]);
	
	memcpy(&reqaddr.s_addr, packet_arp->spa, PKT_IP_ALEN);
	log_dbg("arp-reply: source sha=%.2x:%.2x:%.2x:%.2x:%.2x:%.2x spa=%s",
		packet_arp->sha[0],packet_arp->sha[1],packet_arp->sha[2],
		packet_arp->sha[3],packet_arp->sha[4],packet_arp->sha[5],
		inet_ntoa(reqaddr));	      
	
	memcpy(&reqaddr.s_addr, packet_arp->tpa, PKT_IP_ALEN);
	log_dbg("arp-reply: target tha=%.2x:%.2x:%.2x:%.2x:%.2x:%.2x tpa=%s",
		packet_arp->tha[0],packet_arp->tha[1],packet_arp->tha[2],
		packet_arp->tha[3],packet_arp->tha[4],packet_arp->tha[5],
		inet_ntoa(reqaddr));
      }
	
      return tun_write(tun, (uint8_t*)&packet, length, idx);
    }
  } else {
    ipph = (struct pkt_ipphdr_t *)pack;
  }

  dst.s_addr = ipph->daddr;

  if (ippool_getip(ippool, &ipm, &dst)) {

    /*
     *  TODO: If within statip range, allow the packet through (?)
     */

    if (_options.debug) 
      log_dbg("dropping packet with unknown destination: %s", inet_ntoa(dst));
    
    return 0;
  }
  
  if ((appconn = (struct app_conn_t *)ipm->peer) == NULL ||
      (appconn->dnlink) == NULL) {
    log_err(0, "No %s protocol defined for %s", appconn ? "dnlink" : "peer", inet_ntoa(dst));
    return 0;
  }

  /**
   * connection needs to be NAT'ed, since client is an anyip client
   * outside of our network.
   * So, let's NAT the SNAT ip back to it's client ip.
   */
  if (_options.uamanyip && appconn->natip.s_addr) {
    if (_options.debug) {
      char ip[56];
      char snatip[56];
      strcpy(ip, inet_ntoa(appconn->hisip));
      strcpy(snatip, inet_ntoa(appconn->natip));
      log_dbg("SNAT anyip replace %s back to %s; snat was: %s",
	      inet_ntoa(dst), ip, snatip);
    }
    ipph->daddr = appconn->hisip.s_addr;
    chksum((struct pkt_iphdr_t *) ipph);
  }
  
  /* If the ip src is uamlisten and psrc is uamport we won't call leaky_bucket */
  if ( ! (ipph->saddr  == _options.uamlisten.s_addr && 
	  (ipph->sport == htons(_options.uamport) ||
	   ipph->sport == htons(_options.uamuiport)))) {
    if (appconn->s_state.authenticated == 1) {
      if (chilli_acct_tosub(appconn, len))
	return 0;
    }
  }

  switch (appconn->dnprot) {
  case DNPROT_NULL:
  case DNPROT_DHCP_NONE:
    log_dbg("Dropping...");
    break;
    
  case DNPROT_UAM:
  case DNPROT_WPA:
  case DNPROT_MAC:
  case DNPROT_EAPOL:
    dhcp_data_req((struct dhcp_conn_t *)appconn->dnlink, pack, len, ethhdr);
    break;
    
  default:
    log_err(0, "Unknown downlink protocol: %d", appconn->dnprot);
    break;
  }
  
  return 0;
}

/*********************************************************
 *
 * Redir callbacks
 *
 *********************************************************/

int cb_redir_getstate(struct redir_t *redir, 
		      struct sockaddr_in *address,
		      struct sockaddr_in *baddress,
		      struct redir_conn_t *conn) {
  struct in_addr *addr = &address->sin_addr;
  struct ippoolm_t *ipm;
  struct app_conn_t *appconn;
  struct dhcp_conn_t *dhcpconn;
  uint8_t flags = 0;

#if defined(HAVE_NETFILTER_QUEUE) || defined(HAVE_NETFILTER_COOVA)
  if (_options.uamlisten.s_addr != _options.dhcplisten.s_addr) {
    addr->s_addr  = addr->s_addr & ~(_options.mask.s_addr);
    addr->s_addr |= _options.dhcplisten.s_addr & _options.mask.s_addr;
  }
#endif

  if (ippool_getip(ippool, &ipm, addr)) {
    return -1;
  }
  
  if ( (appconn  = (struct app_conn_t *)ipm->peer)        == NULL || 
       (dhcpconn = (struct dhcp_conn_t *)appconn->dnlink) == NULL ) {
    log_warn(0, "No peer protocol defined");
    return -1;
  }
  
  conn->nasip = _options.radiuslisten;
  conn->nasport = appconn->unit;
  memcpy(conn->hismac, dhcpconn->hismac, PKT_ETH_ALEN);
  conn->ourip = appconn->ourip;
  conn->hisip = appconn->hisip;

#ifdef HAVE_SSL
  /*
   *  Determine if the connection is SSL or not.
   */
  {
    int n;
    for (n=0; n < DHCP_DNAT_MAX; n++) {
      /*
       *  First, search the dnat list to see if we are tracking the port.
       */
      /*log_dbg("%d(%d) == %d",ntohs(dhcpconn->dnat[n].src_port),ntohs(dhcpconn->dnat[n].dst_port),ntohs(address->sin_port));*/
      if (dhcpconn->dnat[n].src_port == address->sin_port) {
	if (dhcpconn->dnat[n].dst_port == htons(DHCP_HTTPS) ||
	    (_options.uamuissl && dhcpconn->dnat[n].dst_port == htons(_options.uamuiport))) {
	  flags |= USING_SSL;
	}
	break;
      }
    }
    /*
     *  If not in dnat, if uamuissl is enabled, and this is indeed that 
     *  port, then we also know it is SSL (directly to https://uamlisten:uamuiport). 
     */
    if (n == DHCP_DNAT_MAX && _options.uamuissl && 
	ntohs(baddress->sin_port) == _options.uamuiport) {
      flags |= USING_SSL;
    }
  }
#endif

  conn->flags = flags;

  memcpy(&conn->s_params, &appconn->s_params, sizeof(appconn->s_params));
  memcpy(&conn->s_state,  &appconn->s_state,  sizeof(appconn->s_state));

  /* reset state */
  appconn->uamexit = 0;

  return conn->s_state.authenticated == 1;
}


/*********************************************************
 *
 * Functions supporting radius callbacks
 *
 *********************************************************/

/* Handle an accounting request */
int accounting_request(struct radius_packet_t *pack,
		       struct sockaddr_in *peer) {
  struct radius_attr_t *hismacattr = NULL;
  struct radius_attr_t *typeattr = NULL;
  struct radius_attr_t *nasipattr = NULL;
  struct radius_attr_t *nasportattr = NULL;
  struct radius_packet_t radius_pack;
  struct app_conn_t *appconn = NULL;
  struct dhcp_conn_t *dhcpconn = NULL;
  uint8_t hismac[PKT_ETH_ALEN];
  char macstr[RADIUS_ATTR_VLEN];
  size_t macstrlen;
  unsigned int temp[PKT_ETH_ALEN];
  uint32_t nasip = 0;
  uint32_t nasport = 0;
  int i;


  if (radius_default_pack(radius, &radius_pack, 
			  RADIUS_CODE_ACCOUNTING_RESPONSE)) {
    log_err(0, "radius_default_pack() failed");
    return -1;
  }

  radius_pack.id = pack->id;
  
  /* Status type */
  if (radius_getattr(pack, &typeattr, RADIUS_ATTR_ACCT_STATUS_TYPE, 0, 0, 0)) {
    log_err(0, "Status type is missing from radius request");
    radius_resp(radius, &radius_pack, peer, pack->authenticator);
    return 0;
  }

  /* Only interested in the disconnect, if one */
  if (typeattr->v.i != htonl(RADIUS_STATUS_TYPE_STOP)) {
    radius_resp(radius, &radius_pack, peer, pack->authenticator);
    return 0;
  }

  /* NAS IP */
  if (!radius_getattr(pack, &nasipattr, RADIUS_ATTR_NAS_IP_ADDRESS, 0, 0, 0)) {
    if ((nasipattr->l-2) != sizeof(appconn->nasip)) {
      log_err(0, "Wrong length of NAS IP address");
      return radius_resp(radius, &radius_pack, peer, pack->authenticator);
    }
    nasip = nasipattr->v.i;
  }
  
  /* NAS PORT */
  if (!radius_getattr(pack, &nasportattr, RADIUS_ATTR_NAS_PORT, 0, 0, 0)) {
    if ((nasportattr->l-2) != sizeof(appconn->nasport)) {
      log_err(0, "Wrong length of NAS port");
      return radius_resp(radius, &radius_pack, peer, pack->authenticator);
    }
    nasport = nasportattr->v.i;
  }
  
  /* Calling Station ID (MAC Address) */
  if (!radius_getattr(pack, &hismacattr, RADIUS_ATTR_CALLING_STATION_ID, 0, 0, 0)) {
    if (_options.debug) {
      log_dbg("Calling Station ID is: %.*s", hismacattr->l-2, hismacattr->v.t);
    }

    if ((macstrlen = (size_t)hismacattr->l-2) >= (RADIUS_ATTR_VLEN-1)) {
      log_err(0, "Wrong length of called station ID");
      return radius_resp(radius, &radius_pack, peer, pack->authenticator);
    }

    memcpy(macstr, hismacattr->v.t, macstrlen);
    macstr[macstrlen] = 0;
    
    /* Replace anything but hex with space */
    for (i=0; i<macstrlen; i++) 
      if (!isxdigit(macstr[i])) macstr[i] = 0x20;
    
    if (sscanf (macstr, "%2x %2x %2x %2x %2x %2x",
		&temp[0], &temp[1], &temp[2], 
		&temp[3], &temp[4], &temp[5]) != 6) {
      log_err(0, "Failed to convert Calling Station ID to MAC Address");
      return radius_resp(radius, &radius_pack, peer, pack->authenticator);
    }
    
    for (i = 0; i < PKT_ETH_ALEN; i++) 
      hismac[i] = temp[i];
  }

  if (hismacattr) { /* Look for mac address.*/
    if (dhcp_hashget(dhcp, &dhcpconn, hismac)) {
      log_err(0, "Unknown connection");
      radius_resp(radius, &radius_pack, peer, pack->authenticator);
      return 0;
    }

    if (!(dhcpconn->peer) || !((struct app_conn_t *)dhcpconn->peer)->uplink) {
      log_err(0, "No peer protocol defined");
      return radius_resp(radius, &radius_pack, peer, pack->authenticator);
    }

    appconn = (struct app_conn_t*) dhcpconn->peer;
  }
  else if (nasipattr && nasportattr) { /* Look for NAS IP / Port */
    if (getconn(&appconn, nasip, nasport)) {
      log_err(0, "Unknown connection");
      radius_resp(radius, &radius_pack, peer, pack->authenticator);
      return 0;
    }
  }
  else {
    log_err(0, "Calling Station ID or NAS IP/Port is missing from radius request");
    radius_resp(radius, &radius_pack, peer, pack->authenticator);
    return 0;
  }
  
  /* Silently ignore radius request if allready processing one */
  if (appconn->radiuswait) {
    if (appconn->radiuswait == 2) {
      log_dbg("Giving up on previous packet.. not dropping this one");
      appconn->radiuswait=0;
    } else {
      log_dbg("Dropping RADIUS while waiting");
      appconn->radiuswait++;
      return 0;
    }
  }
  
  switch (appconn->dnprot) {
  case DNPROT_WPA:
    break;
  case DNPROT_UAM:
    /* might error here to indicate wrong type (should be WPA) */
    break;
  default:
    log_err(0,"Unhandled downlink protocol %d", appconn->dnprot);
    radius_resp(radius, &radius_pack, peer, pack->authenticator);
    return 0;
  }

  dhcpconn = (struct dhcp_conn_t*) appconn->dnlink;
  if (!dhcpconn) {
    log_err(0,"No downlink protocol");
    return 0;
  }

  dhcp_freeconn(dhcpconn, RADIUS_TERMINATE_CAUSE_LOST_CARRIER);

  radius_resp(radius, &radius_pack, peer, pack->authenticator);

  return 0;
}


int access_request(struct radius_packet_t *pack,
		   struct sockaddr_in *peer) {
  int n;
  struct radius_packet_t radius_pack;

  struct ippoolm_t *ipm = NULL;

  struct radius_attr_t *hisipattr = NULL;
  struct radius_attr_t *nasipattr = NULL;
  struct radius_attr_t *nasportattr = NULL;
  struct radius_attr_t *hismacattr = NULL;
  struct radius_attr_t *uidattr = NULL;
  struct radius_attr_t *pwdattr = NULL;
  struct radius_attr_t *eapattr = NULL;

  struct in_addr hisip;
  char pwd[RADIUS_ATTR_VLEN];
  size_t pwdlen;
  uint8_t hismac[PKT_ETH_ALEN];
  char macstr[RADIUS_ATTR_VLEN];
  size_t macstrlen;
  unsigned int temp[PKT_ETH_ALEN];
  char mac[MACSTRLEN+1];
  int i;

  struct app_conn_t *appconn = NULL;
  struct dhcp_conn_t *dhcpconn = NULL;

  uint8_t resp[EAP_LEN];         /* EAP response */
  size_t resplen;                /* Length of EAP response */

  size_t offset = 0;
  size_t eaplen = 0;
  int instance = 0;
  int id;

  if (_options.debug) 
    log_dbg("RADIUS Access-Request received");

  if (radius_default_pack(radius, &radius_pack, RADIUS_CODE_ACCESS_REJECT)) {
    log_err(0, "radius_default_pack() failed");
    return -1;
  }

  id = radius_pack.id;
  radius_pack.id = pack->id;

  /* User is identified by either IP address OR MAC address */
  
  /* Framed IP address (Conditional) */
  if (!radius_getattr(pack, &hisipattr, RADIUS_ATTR_FRAMED_IP_ADDRESS, 0, 0, 0)) {
    if (_options.debug) {
      log_dbg("Framed IP address is: ");
      for (n=0; n<hisipattr->l-2; n++) log_dbg("%.2x", hisipattr->v.t[n]); 
      log_dbg("\n");
    }
    if ((hisipattr->l-2) != sizeof(hisip.s_addr)) {
      log_err(0, "Wrong length of framed IP address");
      return radius_resp(radius, &radius_pack, peer, pack->authenticator);
    }
    hisip.s_addr = hisipattr->v.i;
  }

  /* Calling Station ID: MAC Address (Conditional) */
  if (!radius_getattr(pack, &hismacattr, RADIUS_ATTR_CALLING_STATION_ID, 0, 0, 0)) {
    if (_options.debug) {
      log_dbg("Calling Station ID is: %.*s", hismacattr->l-2, hismacattr->v.t);
    }
    if ((macstrlen = (size_t)hismacattr->l-2) >= (RADIUS_ATTR_VLEN-1)) {
      log_err(0, "Wrong length of called station ID");
      return radius_resp(radius, &radius_pack, peer, pack->authenticator);
    }
    memcpy(macstr, hismacattr->v.t, macstrlen);
    macstr[macstrlen] = 0;

    /* Replace anything but hex with space */
    for (i=0; i<macstrlen; i++) 
      if (!isxdigit(macstr[i])) macstr[i] = 0x20;

    if (sscanf (macstr, "%2x %2x %2x %2x %2x %2x",
		&temp[0], &temp[1], &temp[2], 
		&temp[3], &temp[4], &temp[5]) != 6) {
      log_err(0, "Failed to convert Calling Station ID to MAC Address");
      return radius_resp(radius, &radius_pack, peer, pack->authenticator);
    }
    
    for (i = 0; i < PKT_ETH_ALEN; i++) 
      hismac[i] = temp[i];
  }

  /* Framed IP address or MAC Address must be given in request */
  if ((!hisipattr) && (!hismacattr)) {
    log_err(0, "Framed IP address or Calling Station ID is missing from radius request");
    return radius_resp(radius, &radius_pack, peer, pack->authenticator);
  }

  /* Username (Mandatory) */
  if (radius_getattr(pack, &uidattr, RADIUS_ATTR_USER_NAME, 0, 0, 0)) {
    log_err(0, "User-Name is missing from radius request");
    return radius_resp(radius, &radius_pack, peer, pack->authenticator);
  } 

  if (hisipattr) { /* Find user based on IP address */
    if (ippool_getip(ippool, &ipm, &hisip)) {
      log_err(0, "RADIUS-Request: IP Address not found");
      return radius_resp(radius, &radius_pack, peer, pack->authenticator);
    }
    
    if ((appconn  = (struct app_conn_t *)ipm->peer)        == NULL || 
	(dhcpconn = (struct dhcp_conn_t *)appconn->dnlink) == NULL) {
      log_err(0, "RADIUS-Request: No peer protocol defined");
      return radius_resp(radius, &radius_pack, peer, pack->authenticator);
    }
  }
  else if (hismacattr) { /* Look for mac address. If not found allocate new */
    if (dhcp_hashget(dhcp, &dhcpconn, hismac)) {
      if (dhcp_newconn(dhcp, &dhcpconn, hismac, 0)) {
	log_err(0, "Out of connections");
	return radius_resp(radius, &radius_pack, peer, pack->authenticator);
      }
    }
    if (!(dhcpconn->peer)) {
      log_err(0, "No peer protocol defined");
      return radius_resp(radius, &radius_pack, peer, pack->authenticator);
    }
    appconn = (struct app_conn_t *)dhcpconn->peer;
  }
  else {
    log_err(0, "Framed IP address or Calling Station ID is missing from radius request");
    return radius_resp(radius, &radius_pack, peer, pack->authenticator);
  }

  /* Silently ignore radius request if allready processing one */
  if (appconn->radiuswait) {
    if (appconn->radiuswait == 2) {
      log_dbg("Giving up on previous packet.. not dropping this one");
      appconn->radiuswait=0;
    } else {
      log_dbg("Dropping RADIUS while waiting");
      appconn->radiuswait++;
      return 0;
    }
  }

  dhcpconn->lasttime = mainclock_now();

  /* Password */
  if (!radius_getattr(pack, &pwdattr, RADIUS_ATTR_USER_PASSWORD, 0, 0, 0)) {
    if (radius_pwdecode(radius, (uint8_t*) pwd, RADIUS_ATTR_VLEN, &pwdlen, 
			pwdattr->v.t, pwdattr->l-2, pack->authenticator,
			radius->proxysecret,
			radius->proxysecretlen)) {
      log_err(0, "radius_pwdecode() failed");
      return -1;
    }
    if (_options.debug) log_dbg("Password is: %s\n", pwd);
  }

  /* Get EAP message */
  resplen = 0;
  do {
    eapattr=NULL;
    if (!radius_getattr(pack, &eapattr, RADIUS_ATTR_EAP_MESSAGE, 0, 0, 
			instance++)) {
      if ((resplen + (size_t)eapattr->l-2) > EAP_LEN) {
	log(LOG_INFO, "EAP message too long");
	return radius_resp(radius, &radius_pack, peer, pack->authenticator);
      }
      memcpy(resp + resplen, eapattr->v.t, (size_t)eapattr->l-2);
      resplen += (size_t)eapattr->l-2;
    }
  } while (eapattr);
  
  if (resplen) {
    appconn->dnprot = DNPROT_WPA;
  }

#ifdef ENABLE_PROXYVSA
  {
    struct radius_attr_t *attr = NULL;
    instance=0;
    appconn->s_state.redir.vsalen = 0;
    do {
      attr=NULL;
      if (!radius_getattr(pack, &attr, RADIUS_ATTR_VENDOR_SPECIFIC, 0, 0, 
			  instance++)) {
	
	if ((appconn->s_state.redir.vsalen + (size_t) attr->l) > RADIUS_PROXYVSA) {
	  log_warn(0, "VSAs too long");
	  return radius_resp(radius, &radius_pack, peer, pack->authenticator);
	}
	
	memcpy(appconn->s_state.redir.vsa + appconn->s_state.redir.vsalen, 
	       (void *)attr, (size_t) attr->l);
	
	appconn->s_state.redir.vsalen += (size_t)attr->l;
	
	log_dbg("Remembering VSA");
      }
    } while (attr);
    if (_options.proxy_loc_attr) {
      struct radius_attr_t nattr;
      memset(&nattr, 0, sizeof(nattr));
      nattr.t = RADIUS_ATTR_VENDOR_SPECIFIC;
      nattr.l = 6;
      nattr.v.vv.i = htonl(RADIUS_VENDOR_CHILLISPOT);
      nattr.v.vv.t = RADIUS_ATTR_CHILLISPOT_LOCATION;
      nattr.v.vv.l = 0;
      attr = 0;
      if (!_options.proxy_loc_attr_vsa) {
	/*
	 *  We have a loc_attr, but it isn't a VSA (so not included above)
	 */

	log_dbg("looking for attr %d", _options.proxy_loc_attr);

	if (radius_getattr(pack, &attr, _options.proxy_loc_attr, 
			   0, 0, 0)) {
	  log_dbg("didn't find attr %d", _options.proxy_loc_attr);
	  attr = 0;
	}
      } else {
	/*
	 *  We have a loc_attr and VSA number (so it is included above).
	 */

	log_dbg("looking for attr %d/%d", _options.proxy_loc_attr_vsa, _options.proxy_loc_attr);

	if (radius_getattr(pack, &attr, 
			   RADIUS_ATTR_VENDOR_SPECIFIC, 
			   _options.proxy_loc_attr_vsa, 
			   _options.proxy_loc_attr, 0)) {
	  log_dbg("didn't find attr %d/%d", _options.proxy_loc_attr_vsa, _options.proxy_loc_attr);
	  attr = 0;
	}
      }
      if (attr) {
	memcpy(&nattr.v.vv.v.t, attr->v.t, attr->l - 2);
	nattr.v.vv.l = attr->l;
	nattr.l += attr->l;

	memcpy(appconn->s_state.redir.vsa + appconn->s_state.redir.vsalen, 
	       (void *)&nattr, (size_t)nattr.l);
	
	appconn->s_state.redir.vsalen += (size_t)nattr.l;
      }
    }
  }
#endif

  /* Passwd or EAP must be given in request */
  if ((!pwdattr) && (!resplen)) {
    log_err(0, "Password or EAP meaasge is missing from radius request");
    return radius_resp(radius, &radius_pack, peer, pack->authenticator);
  }

#ifdef ENABLE_PROXYVSA
  if (_options.proxymacaccept && !resplen) {
    log_info("Accepting MAC login");
    radius_pack.code = RADIUS_CODE_ACCESS_ACCEPT;
    return radius_resp(radius, &radius_pack, peer, pack->authenticator);
  }
#endif

  /* ChilliSpot Notes:
     Dublicate logins should be allowed as it might be the terminal
     moving from one access point to another. It is however
     unacceptable to login with another username on top of an allready
     existing connection 

     TODO: New username should be allowed, but should result in
     a accounting stop message for the old connection.
     this does however pose a denial of service attack possibility 
  
     If allready logged in send back accept message with username
     TODO ? Should this be a reject: Dont login twice ? 
  */

  /* Terminate previous session if trying to login with another username */
  if ((appconn->s_state.authenticated == 1) && 
      ((strlen(appconn->s_state.redir.username) != uidattr->l-2) ||
       (memcmp(appconn->s_state.redir.username, uidattr->v.t, uidattr->l-2)))) {
    terminate_appconn(appconn, RADIUS_TERMINATE_CAUSE_USER_REQUEST);
    /* DWB: But, let's not reject someone who is trying to authenticate under
       a new (potentially) valid account - that is for the up-stream RADIUS to discern
      return radius_resp(radius, &radius_pack, peer, pack->authenticator);*/
  }

  /* Radius auth only for DHCP */
  /*if ((appconn->dnprot != DNPROT_UAM) && (appconn->dnprot != DNPROT_WPA))  { */
  /*return radius_resp(radius, &radius_pack, peer, pack->authenticator);*/
  /*  }*/

  /* NAS IP */
  if (!radius_getattr(pack, &nasipattr, RADIUS_ATTR_NAS_IP_ADDRESS, 0, 0, 0)) {
    if ((nasipattr->l-2) != sizeof(appconn->nasip)) {
      log_err(0, "Wrong length of NAS IP address");
      return radius_resp(radius, &radius_pack, peer, pack->authenticator);
    }
    appconn->nasip = nasipattr->v.i;
  }

  /* NAS PORT */
  if (!radius_getattr(pack, &nasportattr, RADIUS_ATTR_NAS_PORT, 0, 0, 0)) {
    if ((nasportattr->l-2) != sizeof(appconn->nasport)) {
      log_err(0, "Wrong length of NAS port");
      return radius_resp(radius, &radius_pack, peer, pack->authenticator);
    }
    appconn->nasport = nasportattr->v.i;
  }

  /* Store parameters for later use */
  if (uidattr->l-2<=USERNAMESIZE) {
    strncpy(appconn->s_state.redir.username, 
	    (char *)uidattr->v.t, uidattr->l-2);
  }

  appconn->radiuswait = 1;
  appconn->radiusid = pack->id;

  if (pwdattr)
    appconn->authtype = PAP_PASSWORD;
  else
    appconn->authtype = EAP_MESSAGE;

  memcpy(&appconn->radiuspeer, peer, sizeof(*peer));
  memcpy(appconn->authenticator, pack->authenticator, RADIUS_AUTHLEN);
  memcpy(appconn->hismac, dhcpconn->hismac, PKT_ETH_ALEN);
  /*memcpy(appconn->ourmac, dhcpconn->ourmac, PKT_ETH_ALEN);*/

  /* Build up radius request */
  radius_pack.code = RADIUS_CODE_ACCESS_REQUEST;
  radius_addattr(radius, &radius_pack, RADIUS_ATTR_USER_NAME, 0, 0, 0,
		 uidattr->v.t, uidattr->l - 2);

  if (appconn->s_state.redir.statelen) {
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_STATE, 0, 0, 0,
		   appconn->s_state.redir.statebuf,
		   appconn->s_state.redir.statelen);
  }

  if (pwdattr)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_USER_PASSWORD, 0, 0, 0,
		   (uint8_t*) pwd, pwdlen);

  /* Include EAP (if present) */
  offset = 0;
  while (offset < resplen) {

    if ((resplen - offset) > RADIUS_ATTR_VLEN)
      eaplen = RADIUS_ATTR_VLEN;
    else
      eaplen = resplen - offset;

    radius_addattr(radius, &radius_pack, RADIUS_ATTR_EAP_MESSAGE, 0, 0, 0,
		   resp + offset, eaplen);

    offset += eaplen;
  } 

  if (resplen) {
    if (_options.wpaguests)
      radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
		     RADIUS_VENDOR_CHILLISPOT, RADIUS_ATTR_CHILLISPOT_CONFIG, 
		     0, (uint8_t*)"allow-wpa-guests", 16);
  }

  /* Include his MAC address */
  snprintf(mac, MACSTRLEN+1, "%.2X-%.2X-%.2X-%.2X-%.2X-%.2X",
	   appconn->hismac[0], appconn->hismac[1],
	   appconn->hismac[2], appconn->hismac[3],
	   appconn->hismac[4], appconn->hismac[5]);
  
  radius_addattr(radius, &radius_pack, RADIUS_ATTR_CALLING_STATION_ID, 0, 0, 0,
		 (uint8_t*) mac, MACSTRLEN);
  
  radius_addcalledstation(radius, &radius_pack);
  
  radius_addattr(radius, &radius_pack, RADIUS_ATTR_NAS_PORT_TYPE, 0, 0,
		 _options.radiusnasporttype, NULL, 0);
  
  radius_addattr(radius, &radius_pack, RADIUS_ATTR_NAS_PORT, 0, 0,
		 appconn->unit, NULL, 0);

  radius_addattr(radius, &radius_pack, RADIUS_ATTR_SERVICE_TYPE, 0, 0,
		 _options.framedservice ? RADIUS_SERVICE_TYPE_FRAMED :
		 RADIUS_SERVICE_TYPE_LOGIN, NULL, 0); 
  
  radius_addnasip(radius, &radius_pack);
  
  /* Include NAS-Identifier if given in configuration options */
  if (_options.radiusnasid)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_NAS_IDENTIFIER, 0, 0, 0,
		   (uint8_t*) _options.radiusnasid, strlen(_options.radiusnasid));

#ifdef ENABLE_PROXYVSA
  radius_addvsa(&radius_pack, &appconn->s_state.redir);
#endif

  radius_addattr(radius, &radius_pack, RADIUS_ATTR_MESSAGE_AUTHENTICATOR, 
		 0, 0, 0, NULL, RADIUS_MD5LEN);

  radius_pack.id = id;

  return radius_req(radius, &radius_pack, appconn);
}

/*********************************************************
 *
 * radius proxy callback functions (request from radius server)
 *
 *********************************************************/

/* Radius callback when radius request has been received */
int cb_radius_ind(struct radius_t *rp, struct radius_packet_t *pack,
		  struct sockaddr_in *peer) {

  if (rp != radius) {
    log_err(0, "Radius callback from unknown instance");
    return 0;
  }
  
  switch (pack->code) {
  case RADIUS_CODE_ACCOUNTING_REQUEST: /* TODO: Exclude ??? */
    return accounting_request(pack, peer);
  case RADIUS_CODE_ACCESS_REQUEST:
    return access_request(pack, peer);
  default:
    log_err(0, "Unsupported radius request received: %d", pack->code);
    return 0;
  }
}

int upprot_getip(struct app_conn_t *appconn, 
		 struct in_addr *hisip, int statip) {
  struct ippoolm_t *ipm;

  /* If IP address is allready allocated: Fill it in */
  /* This should only happen for UAM */
  /* TODO */
  if (appconn->uplink) {
    ipm = (struct ippoolm_t *)appconn->uplink;
  }
  else {
    /* Allocate static or dynamic IP address */

    struct dhcp_conn_t *dhcpconn = (struct dhcp_conn_t *)appconn->dnlink;

    if (newip(&ipm, hisip, dhcpconn ? dhcpconn->hismac : 0))
      return dnprot_reject(appconn);

    appconn->hisip.s_addr = ipm->addr.s_addr;

    /* TODO: Too many "listen" and "ourip" addresses! */
    appconn->ourip.s_addr = _options.dhcplisten.s_addr;
    
    appconn->uplink = ipm;
    ipm->peer = appconn; 
  }

  if (chilli_assign_snat(appconn, 0) != 0) {
    return -1;
  }

  return dnprot_accept(appconn);
}

static void session_param_defaults(struct session_params *params) {
  
  if (_options.defsessiontimeout && !params->sessiontimeout)
    params->sessiontimeout = _options.defsessiontimeout;
  
  if (_options.defidletimeout && !params->idletimeout)
    params->idletimeout = _options.defidletimeout;
  
  if (_options.defbandwidthmaxdown && !params->bandwidthmaxdown)
    params->bandwidthmaxdown = _options.defbandwidthmaxdown;
  
  if (_options.defbandwidthmaxup && !params->bandwidthmaxup)
    params->bandwidthmaxup = _options.defbandwidthmaxup;
  
  if (_options.definteriminterval && !params->interim_interval)
    params->interim_interval = _options.definteriminterval;
}

void config_radius_session(struct session_params *params, 
			   struct radius_packet_t *pack, 
			   struct dhcp_conn_t *dhcpconn,
			   int reconfig) {

  struct radius_attr_t *attr = NULL;

  /* Session timeout */
  if (!radius_getattr(pack, &attr, RADIUS_ATTR_SESSION_TIMEOUT, 0, 0, 0))
    params->sessiontimeout = ntohl(attr->v.i);
  else if (!reconfig)
    params->sessiontimeout = 0;

  /* Idle timeout */
  if (!radius_getattr(pack, &attr, RADIUS_ATTR_IDLE_TIMEOUT, 0, 0, 0))
    params->idletimeout = ntohl(attr->v.i);
  else if (!reconfig) 
    params->idletimeout = 0;

  /* Filter ID */
  if (!radius_getattr(pack, &attr, RADIUS_ATTR_FILTER_ID, 0, 0, 0)) {
    params->filteridlen = attr->l-2;
    memcpy(params->filteridbuf, attr->v.t, attr->l-2);
    params->filteridbuf[attr->l-2] = 0;
  }
  else if (!reconfig) {
    params->filteridlen = 0;
    params->filteridbuf[0] = 0;
  }

  /* Interim interval */
  if (!radius_getattr(pack, &attr, RADIUS_ATTR_ACCT_INTERIM_INTERVAL, 0, 0, 0)) {
    params->interim_interval = ntohl(attr->v.i);
    if (params->interim_interval < 60) {
      log_err(0, "Received too small radius Acct-Interim-Interval: %d; resettings to default.",
	      params->interim_interval);
      params->interim_interval = 0;
    } 
  }
  else if (!reconfig)
    params->interim_interval = 0;

  /* Bandwidth up */
  if (!radius_getattr(pack, &attr, RADIUS_ATTR_VENDOR_SPECIFIC,
		      RADIUS_VENDOR_WISPR, 
		      RADIUS_ATTR_WISPR_BANDWIDTH_MAX_UP, 0))
    params->bandwidthmaxup = ntohl(attr->v.i);
  else if (!reconfig)
    params->bandwidthmaxup = 0;
  
  /* Bandwidth down */
  if (!radius_getattr(pack, &attr, RADIUS_ATTR_VENDOR_SPECIFIC,
		      RADIUS_VENDOR_WISPR, 
		      RADIUS_ATTR_WISPR_BANDWIDTH_MAX_DOWN, 0))
    params->bandwidthmaxdown = ntohl(attr->v.i);
  else if (!reconfig)
    params->bandwidthmaxdown = 0;

#ifdef RADIUS_ATTR_CHILLISPOT_BANDWIDTH_MAX_UP
  /* Bandwidth up */
  if (!radius_getattr(pack, &attr, RADIUS_ATTR_VENDOR_SPECIFIC,
		      RADIUS_VENDOR_CHILLISPOT, 
		      RADIUS_ATTR_CHILLISPOT_BANDWIDTH_MAX_UP, 0))
    params->bandwidthmaxup = ntohl(attr->v.i) * 1000;
#endif

#ifdef RADIUS_ATTR_CHILLISPOT_BANDWIDTH_MAX_DOWN
  /* Bandwidth down */
  if (!radius_getattr(pack, &attr, RADIUS_ATTR_VENDOR_SPECIFIC,
		      RADIUS_VENDOR_CHILLISPOT, 
		      RADIUS_ATTR_CHILLISPOT_BANDWIDTH_MAX_DOWN, 0))
    params->bandwidthmaxdown = ntohl(attr->v.i) * 1000;
#endif

  /* Max input octets */
  if (!radius_getattr(pack, &attr, RADIUS_ATTR_VENDOR_SPECIFIC,
		      RADIUS_VENDOR_CHILLISPOT, 
		      RADIUS_ATTR_CHILLISPOT_MAX_INPUT_OCTETS, 0))
    params->maxinputoctets = ntohl(attr->v.i);
  else if (!reconfig)
    params->maxinputoctets = 0;

  /* Max output octets */
  if (!radius_getattr(pack, &attr, RADIUS_ATTR_VENDOR_SPECIFIC,
		      RADIUS_VENDOR_CHILLISPOT, 
		      RADIUS_ATTR_CHILLISPOT_MAX_OUTPUT_OCTETS, 0))
    params->maxoutputoctets = ntohl(attr->v.i);
  else if (!reconfig)
    params->maxoutputoctets = 0;

  /* Max total octets */
  if (!radius_getattr(pack, &attr, RADIUS_ATTR_VENDOR_SPECIFIC,
		      RADIUS_VENDOR_CHILLISPOT, 
		      RADIUS_ATTR_CHILLISPOT_MAX_TOTAL_OCTETS, 0))
    params->maxtotaloctets = ntohl(attr->v.i);
  else if (!reconfig)
    params->maxtotaloctets = 0;


  /* Max input gigawords */
  if (!radius_getattr(pack, &attr, RADIUS_ATTR_VENDOR_SPECIFIC,
		      RADIUS_VENDOR_CHILLISPOT, 
		      RADIUS_ATTR_CHILLISPOT_MAX_INPUT_GIGAWORDS, 0))
    params->maxinputoctets |= ((uint64_t)ntohl(attr->v.i) & 0xffffffff) << 32;

  /* Max output gigawords */
  if (!radius_getattr(pack, &attr, RADIUS_ATTR_VENDOR_SPECIFIC,
		      RADIUS_VENDOR_CHILLISPOT, 
		      RADIUS_ATTR_CHILLISPOT_MAX_OUTPUT_GIGAWORDS, 0))
    params->maxoutputoctets |= ((uint64_t)ntohl(attr->v.i) & 0xffffffff) << 32;

  /* Max total octets */
  if (!radius_getattr(pack, &attr, RADIUS_ATTR_VENDOR_SPECIFIC,
		      RADIUS_VENDOR_CHILLISPOT, 
		      RADIUS_ATTR_CHILLISPOT_MAX_TOTAL_GIGAWORDS, 0))
    params->maxtotaloctets |= ((uint64_t)ntohl(attr->v.i) & 0xffffffff) << 32;


  if (tun) {
    /* Route Index, look-up by interface name */
    if (!radius_getattr(pack, &attr, RADIUS_ATTR_VENDOR_SPECIFIC, 
			RADIUS_VENDOR_CHILLISPOT, 
			RADIUS_ATTR_CHILLISPOT_ROUTE_TO_INTERFACE, 0)) {
      char name[256];
      memcpy(name, attr->v.t, attr->l-2);
      name[attr->l-2] = 0;
      params->routeidx = tun_name2idx(tun, name);
    }
    else if (!reconfig) {
      params->routeidx = tun->routeidx;
    }
  }

  {
    const char *uamauth = "require-uam-auth";
    const char *splash = "splash";
    const char *logout = "logout";
    const char *adminreset = "admin-reset";

    size_t offset = 0;
    int is_splash = 0;

#ifdef ENABLE_SESSGARDEN
    const char *uamallowed = "uamallowed=";

    /* Always reset the per session passthroughs */
    params->pass_through_count = 0;
#endif

    while (!radius_getnextattr(pack, &attr, RADIUS_ATTR_VENDOR_SPECIFIC,
			       RADIUS_VENDOR_CHILLISPOT, RADIUS_ATTR_CHILLISPOT_CONFIG, 
			       0, &offset)) { 
      size_t len = (size_t)attr->l-2;
      char *val = (char *)attr->v.t;

      if (_options.wpaguests && len == strlen(uamauth) && !memcmp(val, uamauth, len)) {
	log_dbg("received wpaguests");
	params->flags |= REQUIRE_UAM_AUTH;
      } 
      else if (len == strlen(splash) && !memcmp(val, splash, strlen(splash))) {
	log_dbg("received splash response");
	params->flags |= REQUIRE_UAM_SPLASH;
	is_splash = 1;
      }
#ifdef ENABLE_SESSGARDEN
      else if (len > strlen(uamallowed) && !memcmp(val, uamallowed, strlen(uamallowed)) && len < 255) {
	char name[256];
	strncpy(name, val, len);
	name[len] = 0;
	pass_throughs_from_string(params->pass_throughs,
				  SESSION_PASS_THROUGH_MAX,
				  &params->pass_through_count,
				  name + strlen(uamallowed));
      }
#endif
      else if (dhcpconn && len >= strlen(logout) && !memcmp(val, logout, strlen(logout))) {
	struct app_conn_t* appconn = (struct app_conn_t*) dhcpconn->peer;
	if (appconn) terminate_appconn(appconn, RADIUS_TERMINATE_CAUSE_USER_REQUEST);
      } else if (dhcpconn && len >= strlen(adminreset) && !memcmp(val, adminreset, strlen(adminreset))) {
	dhcp_release_mac(dhcp, dhcpconn->hismac, RADIUS_TERMINATE_CAUSE_ADMIN_RESET);
      }
    }

    offset = 0;
    params->url[0]=0;
    while (!radius_getnextattr(pack, &attr, RADIUS_ATTR_VENDOR_SPECIFIC,
			       RADIUS_VENDOR_WISPR, RADIUS_ATTR_WISPR_REDIRECTION_URL, 
			       0, &offset)) { 
      size_t clen, nlen = (size_t)attr->l-2;
      char *url = (char*)attr->v.t;
      clen = strlen((char*)params->url);

      if (clen + nlen > sizeof(params->url)-1) 
	nlen = sizeof(params->url)-clen-1;

      strncpy((char*)(params->url + clen), url, nlen);
      params->url[nlen+clen]=0;

      if (!is_splash) {
	params->flags |= REQUIRE_REDIRECT;
      }
    }
  }

  /* Session-Terminate-Time */
  if (!radius_getattr(pack, &attr, RADIUS_ATTR_VENDOR_SPECIFIC,
		      RADIUS_VENDOR_WISPR,
		      RADIUS_ATTR_WISPR_SESSION_TERMINATE_TIME, 0)) {
    char attrs[RADIUS_ATTR_VLEN + 1];
    struct tm stt;
    int tzhour, tzmin;
    char *tz;
    int result;

    memcpy(attrs, attr->v.t, attr->l-2);
    attrs[attr->l-2] = 0;

    memset(&stt, 0, sizeof(stt));

    result = sscanf(attrs, "%d-%d-%dT%d:%d:%d %d:%d",
		    &stt.tm_year, &stt.tm_mon, &stt.tm_mday,
		    &stt.tm_hour, &stt.tm_min, &stt.tm_sec,
		    &tzhour, &tzmin);

    if (result == 8) { /* Timezone */
      /* tzhour and tzmin is hours and minutes east of GMT */
      /* timezone is defined as seconds west of GMT. Excludes DST */
      stt.tm_year -= 1900;
      stt.tm_mon  -= 1;
      stt.tm_hour -= tzhour; /* Adjust for timezone */
      stt.tm_min  -= tzmin;  /* Adjust for timezone */
      /*      stt.tm_hour += daylight;*/
      /*stt.tm_min  -= (timezone / 60);*/
      tz = getenv("TZ");
      setenv("TZ", "", 1); /* Set environment to UTC */
      tzset();
      params->sessionterminatetime = mktime(&stt);
      if (tz) setenv("TZ", tz, 1); 
      else    unsetenv("TZ");
      tzset();
    }
    else if (result >= 6) { /* Local time */
      tzset();
      stt.tm_year -= 1900;
      stt.tm_mon  -= 1;
      stt.tm_isdst = -1; /*daylight;*/
      params->sessionterminatetime = mktime(&stt);
    }
    else {
      params->sessionterminatetime = 0;
      log_warn(0, "Invalid WISPr-Session-Terminate-Time received: %s", attrs);
    }
  }
  else if (!reconfig)
    params->sessionterminatetime = 0;


  session_param_defaults(params);
}

static int chilliauth_cb(struct radius_t *radius,
			 struct radius_packet_t *pack,
			 struct radius_packet_t *pack_req, 
			 void *cbp) {

  struct radius_attr_t *attr = NULL;
  size_t offset = 0;

  if (!pack) { 
    log_err(0, "Radius request timed out");
    return 0;
  }

  if ((pack->code != RADIUS_CODE_ACCESS_REJECT) && 
      (pack->code != RADIUS_CODE_ACCESS_CHALLENGE) &&
      (pack->code != RADIUS_CODE_ACCESS_ACCEPT)) {
    log_err(0, "Unknown radius access reply code %d", pack->code);
    return 0;
  }

  /* ACCESS-ACCEPT */
  if (pack->code != RADIUS_CODE_ACCESS_ACCEPT) {
    log_err(0, "Administrative-User Login Failed");
    return 0;
  }

  if (_options.adminupdatefile) {

    log_dbg("looking to replace: %s", _options.adminupdatefile);

    if (!radius_getnextattr(pack, &attr, 
			    RADIUS_ATTR_VENDOR_SPECIFIC,
			    RADIUS_VENDOR_CHILLISPOT,
			    RADIUS_ATTR_CHILLISPOT_CONFIG, 
			    0, &offset)) {

      char * hs_conf = _options.adminupdatefile;
      char * hs_temp = "/tmp/hs.conf";
      
      /* 
       *  We have configurations in the administrative-user session.
       *  Save to a temporary file.
       */

      log_dbg("using temp: %s", hs_temp);
      
      int fd = open(hs_temp, O_RDWR | O_TRUNC | O_CREAT, 0644);

      if (fd > 0) {

	do {
	  if (write(fd, (const char *) attr->v.t, attr->l - 2) < 0 ||
	      write(fd, "\n", 1) < 0) {
	    log_err(errno, "adminupdatefile");
	    break;
	  }
	} 
	while (!radius_getnextattr(pack, &attr, 
				   RADIUS_ATTR_VENDOR_SPECIFIC,
				   RADIUS_VENDOR_CHILLISPOT,
				   RADIUS_ATTR_CHILLISPOT_CONFIG, 
				   0, &offset));
	close(fd);
      }
      
      /* 
       *  Check to see if this file is different from the chilli/hs.conf
       */
      {
	int newfd = open(hs_temp, O_RDONLY);
	int oldfd = open(hs_conf, O_RDONLY);
	
	if (newfd > 0 && oldfd > 0) {
	  int differ = 0;
	  char b1[100], b2[100];
	  ssize_t r1, r2;
	  
	  do {
	    r1 = read(newfd, b1, sizeof(b1));
	    r2 = read(oldfd, b2, sizeof(b2));
	    
	    if (r1 != r2 || strncmp(b1, b2, r1)) 
	      differ = 1;
	  } 
	  while (!differ && r1 > 0 && r2 > 0);
	  
	  close(newfd); newfd=0;
	  close(oldfd); oldfd=0;
	  
	  if (differ) {
	    log_dbg("Writing out new hs.conf file with administraive-user settings");
	    
	    newfd = open(hs_temp, O_RDONLY);
	    oldfd = open(hs_conf, O_RDWR | O_TRUNC | O_CREAT, 0644);
	    
	    if (newfd > 0 && oldfd > 0) {

	      while ((r1 = read(newfd, b1, sizeof(b1))) > 0 &&
		     write(oldfd, b1, r1) > 0);
	      
	      close(newfd); newfd=0;
	      close(oldfd); oldfd=0;
	      do_interval = 1;
	    }
	  }
	}
	if (newfd > 0) close(newfd);
	if (oldfd > 0) close(oldfd);
      }
    }
  }

  if (!admin_session.s_state.authenticated) {
    admin_session.s_state.authenticated = 1;
    acct_req(&admin_session, RADIUS_STATUS_TYPE_START);
  }

  /* reset these values to zero */
  admin_session.s_params.idletimeout = 0;
  admin_session.s_params.sessionterminatetime = 0;

  /* should instead honor this with a re-auth (see interval) */
  admin_session.s_params.sessiontimeout = 0;

  return 0;
}

int cb_radius_acct_conf(struct radius_t *radius, 
			struct radius_packet_t *pack,
			struct radius_packet_t *pack_req, void *cbp) {
  struct app_conn_t *appconn = (struct app_conn_t*) cbp;
  if (!appconn) {
    log_err(0,"No peer protocol defined");
    return 0;
  }

  if (!pack) /* Timeout */
    return 0;

  config_radius_session(&appconn->s_params, pack, (struct dhcp_conn_t *)appconn->dnlink, 1);
  return 0;
}

/*********************************************************
 *
 * radius callback functions (response from radius server)
 *
 *********************************************************/

/* Radius callback when access accept/reject/challenge has been received */
int cb_radius_auth_conf(struct radius_t *radius, 
			struct radius_packet_t *pack,
			struct radius_packet_t *pack_req, void *cbp) {
  struct radius_attr_t *hisipattr = NULL;
  struct radius_attr_t *lmntattr = NULL;
  struct radius_attr_t *sendattr = NULL;
  struct radius_attr_t *recvattr = NULL;
  struct radius_attr_t *succattr = NULL;
  struct radius_attr_t *policyattr = NULL;
  struct radius_attr_t *typesattr = NULL;

  struct radius_attr_t *eapattr = NULL;
  struct radius_attr_t *stateattr = NULL;
  struct radius_attr_t *classattr = NULL;

  int instance = 0;
  struct in_addr *hisip = NULL;
  int statip = 0;

  struct app_conn_t *appconn = (struct app_conn_t*) cbp;

  struct dhcp_conn_t *dhcpconn = (struct dhcp_conn_t *)appconn->dnlink;

  if (_options.debug)
    log_dbg("Received access request confirmation from radius server\n");
  
  if (!appconn) {
    log_err(0,"No peer protocol defined");
    return 0;
  }

  /* Initialise */
  appconn->s_state.redir.statelen = 0;
  appconn->challen  = 0;
  appconn->sendlen  = 0;
  appconn->recvlen  = 0;
  appconn->lmntlen  = 0;
  
  if (!pack) { /* Timeout */
    log_err(0, "Radius request timed out");
    return dnprot_reject(appconn);
  }

  /* ACCESS-REJECT */
  if (pack->code == RADIUS_CODE_ACCESS_REJECT) {
    if (_options.debug)
      log_dbg("Received access reject from radius server");
    config_radius_session(&appconn->s_params, pack, dhcpconn, 0); /*XXX*/
    return dnprot_reject(appconn);
  }

  /* Get State */
  if (!radius_getattr(pack, &stateattr, RADIUS_ATTR_STATE, 0, 0, 0)) {
    appconn->s_state.redir.statelen = stateattr->l-2;
    memcpy(appconn->s_state.redir.statebuf, stateattr->v.t, stateattr->l-2);
  }

  /* ACCESS-CHALLENGE */
  if (pack->code == RADIUS_CODE_ACCESS_CHALLENGE) {
    if (_options.debug)
      log_dbg("Received access challenge from radius server");

    /* Get EAP message */
    appconn->challen = 0;
    do {
      eapattr=NULL;
      if (!radius_getattr(pack, &eapattr, RADIUS_ATTR_EAP_MESSAGE, 0, 0, instance++)) {
	if ((appconn->challen + eapattr->l-2) > EAP_LEN) {
	  log(LOG_INFO, "EAP message too long");
	  return dnprot_reject(appconn);
	}
	memcpy(appconn->chal+appconn->challen, eapattr->v.t, eapattr->l-2);
	appconn->challen += eapattr->l-2;
      }
    } while (eapattr);
    
    if (!appconn->challen) {
      log(LOG_INFO, "No EAP message found");
      return dnprot_reject(appconn);
    }
    
    return dnprot_challenge(appconn);
  }
  
  /* ACCESS-ACCEPT */
  if (pack->code != RADIUS_CODE_ACCESS_ACCEPT) {
    log_err(0, "Unknown code of radius access request confirmation");
    return dnprot_reject(appconn);
  }

  /* Class */
  if (!radius_getattr(pack, &classattr, RADIUS_ATTR_CLASS, 0, 0, 0)) {
    appconn->s_state.redir.classlen = classattr->l-2;
    memcpy(appconn->s_state.redir.classbuf, classattr->v.t, classattr->l-2);
    /*log_dbg("!!!! CLASSLEN = %d !!!!", appconn->s_state.redir.classlen);*/
  }
  else {
    /*log_dbg("!!!! RESET CLASSLEN !!!!");*/
    appconn->s_state.redir.classlen = 0;
  }

  /* Framed IP address (Optional) */
  if (!radius_getattr(pack, &hisipattr, RADIUS_ATTR_FRAMED_IP_ADDRESS, 0, 0, 0)) {
    if ((hisipattr->l-2) != sizeof(struct in_addr)) {
      log_err(0, "Wrong length of framed IP address");
      return dnprot_reject(appconn);
    }
    hisip = (struct in_addr*) &(hisipattr->v.i);
    statip = 1;
  }
  else {
    hisip = (struct in_addr*) &appconn->reqip.s_addr;
  }

  config_radius_session(&appconn->s_params, pack, dhcpconn, 0);

  if (appconn->is_adminsession) {
    /* for the admin session */
    return chilliauth_cb(radius, pack, pack_req, cbp);
  }

  if (_options.dhcpradius) {
    struct radius_attr_t *attr = NULL;
    if (dhcpconn) {
      if (!radius_getattr(pack, &attr, RADIUS_ATTR_VENDOR_SPECIFIC, RADIUS_VENDOR_CHILLISPOT, 
			  RADIUS_ATTR_CHILLISPOT_DHCP_SERVER_NAME, 0)) {
	memcpy(dhcpconn->dhcp_opts.sname, attr->v.t, attr->l-2);
      }
      if (!radius_getattr(pack, &attr, RADIUS_ATTR_VENDOR_SPECIFIC, RADIUS_VENDOR_CHILLISPOT, 
			  RADIUS_ATTR_CHILLISPOT_DHCP_FILENAME, 0)) {
	memcpy(dhcpconn->dhcp_opts.file, attr->v.t, attr->l-2);
      }
      if (!radius_getattr(pack, &attr, RADIUS_ATTR_VENDOR_SPECIFIC, RADIUS_VENDOR_CHILLISPOT, 
			  RADIUS_ATTR_CHILLISPOT_DHCP_OPTION, 0)) {
	memcpy(dhcpconn->dhcp_opts.options, attr->v.t, 
	       dhcpconn->dhcp_opts.option_length = attr->l-2);
      }
    }
  }

  if (appconn->s_params.sessionterminatetime) {
    if (mainclock_rtdiff(appconn->s_params.sessionterminatetime) > 0) {
      log(LOG_WARNING, "WISPr-Session-Terminate-Time in the past received, rejecting");
      return dnprot_reject(appconn);
    }
  }

  /* EAP Message */
  appconn->challen = 0;
  do {
    eapattr=NULL;
    if (!radius_getattr(pack, &eapattr, RADIUS_ATTR_EAP_MESSAGE, 0, 0, 
			instance++)) {
      if ((appconn->challen + eapattr->l-2) > EAP_LEN) {
	log(LOG_INFO, "EAP message too long");
	return dnprot_reject(appconn);
      }
      memcpy(appconn->chal+appconn->challen,
	     eapattr->v.t, eapattr->l-2);
      appconn->challen += eapattr->l-2;
    }
  } while (eapattr);

  /* Get sendkey */
  if (!radius_getattr(pack, &sendattr, RADIUS_ATTR_VENDOR_SPECIFIC,
		      RADIUS_VENDOR_MS,
		      RADIUS_ATTR_MS_MPPE_SEND_KEY, 0)) {
    if (radius_keydecode(radius, appconn->sendkey, RADIUS_ATTR_VLEN, &appconn->sendlen, 
			 (uint8_t *)&sendattr->v.t, sendattr->l-2, 
			 pack_req->authenticator,
			 radius->secret, radius->secretlen)) {
      log_err(0, "radius_keydecode() failed!");
      return dnprot_reject(appconn);
    }
  }
    
  /* Get recvkey */
  if (!radius_getattr(pack, &recvattr, RADIUS_ATTR_VENDOR_SPECIFIC,
		      RADIUS_VENDOR_MS,
		      RADIUS_ATTR_MS_MPPE_RECV_KEY, 0)) {
    if (radius_keydecode(radius, appconn->recvkey, RADIUS_ATTR_VLEN, &appconn->recvlen, 
			 (uint8_t *)&recvattr->v.t, recvattr->l-2, 
			 pack_req->authenticator,
			 radius->secret, radius->secretlen) ) {
      log_err(0, "radius_keydecode() failed!");
      return dnprot_reject(appconn);
    }
  }

  /* Get LMNT keys */
  if (!radius_getattr(pack, &lmntattr, RADIUS_ATTR_VENDOR_SPECIFIC,
		      RADIUS_VENDOR_MS,
		      RADIUS_ATTR_MS_CHAP_MPPE_KEYS, 0)) {

    /* TODO: Check length of vendor attributes */
    if (radius_pwdecode(radius, appconn->lmntkeys, RADIUS_MPPEKEYSSIZE,
			&appconn->lmntlen, (uint8_t *)&lmntattr->v.t,
			lmntattr->l-2, pack_req->authenticator,
			radius->secret, radius->secretlen)) {
      log_err(0, "radius_pwdecode() failed");
      return dnprot_reject(appconn);
    }
  }
  
  /* Get encryption policy */
  if (!radius_getattr(pack, &policyattr, RADIUS_ATTR_VENDOR_SPECIFIC,
		      RADIUS_VENDOR_MS, 
		      RADIUS_ATTR_MS_MPPE_ENCRYPTION_POLICY, 0)) {
    appconn->policy = ntohl(policyattr->v.i);
  }
  
  /* Get encryption types */
  if (!radius_getattr(pack, &typesattr, RADIUS_ATTR_VENDOR_SPECIFIC,
		      RADIUS_VENDOR_MS, 
		      RADIUS_ATTR_MS_MPPE_ENCRYPTION_TYPES, 0)) {
    appconn->types = ntohl(typesattr->v.i);
  }
  

  /* Get MS_Chap_v2 SUCCESS */
  if (!radius_getattr(pack, &succattr, RADIUS_ATTR_VENDOR_SPECIFIC,
		      RADIUS_VENDOR_MS,
		      RADIUS_ATTR_MS_CHAP2_SUCCESS, 0)) {
    if ((succattr->l-5) != MS2SUCCSIZE) {
      log_err(0, "Wrong length of MS-CHAP2 success: %d", succattr->l-5);
      return dnprot_reject(appconn);
    }
    memcpy(appconn->ms2succ, ((void*)&succattr->v.t)+3, MS2SUCCSIZE);
  }

  switch(appconn->authtype) {

  case PAP_PASSWORD:
    appconn->policy = 0; /* TODO */
    break;

  case EAP_MESSAGE:
    if (!appconn->challen) {
      log(LOG_INFO, "No EAP message found");
      return dnprot_reject(appconn);
    }
    break;

  case CHAP_DIGEST_MD5:
    appconn->policy = 0; /* TODO */
    break;

  case CHAP_MICROSOFT:
    if (!lmntattr) {
      log(LOG_INFO, "No MPPE keys found");
      return dnprot_reject(appconn);
      }
    if (!succattr) {
      log_err(0, "No MS-CHAP2 success found");
      return dnprot_reject(appconn);
    }
    break;

  case CHAP_MICROSOFT_V2:
    if (!sendattr) {
      log(LOG_INFO, "No MPPE sendkey found");
      return dnprot_reject(appconn);
    }
    
    if (!recvattr) {
      log(LOG_INFO, "No MPPE recvkey found");
      return dnprot_reject(appconn);
    }
    
    break;

  default:
    log_err(0, "Unknown authtype");
    return dnprot_reject(appconn);
  }
  
  return upprot_getip(appconn, hisip, statip);
}


/* Radius callback when coa or disconnect request has been received */
int cb_radius_coa_ind(struct radius_t *radius, struct radius_packet_t *pack,
		      struct sockaddr_in *peer) {
  struct app_conn_t *appconn;
  struct radius_attr_t *uattr = NULL;
  struct radius_attr_t *sattr = NULL;
  struct radius_packet_t radius_pack;
  int found = 0;
  int iscoa = 0;

  if (_options.debug)
    log_dbg("Received coa or disconnect request\n");
  
  if (pack->code != RADIUS_CODE_DISCONNECT_REQUEST &&
      pack->code != RADIUS_CODE_COA_REQUEST) {
    log_err(0, "Radius packet not supported: %d,\n", pack->code);
    return -1;
  }

  iscoa = pack->code == RADIUS_CODE_COA_REQUEST;

  /* Get username */
  if (radius_getattr(pack, &uattr, RADIUS_ATTR_USER_NAME, 0, 0, 0)) {
    log_warn(0, "Username must be included in disconnect request");
    return -1;
  }

  if (!radius_getattr(pack, &sattr, RADIUS_ATTR_ACCT_SESSION_ID, 0, 0, 0))
    if (_options.debug) 
      log_dbg("Session-id present in disconnect. Only disconnecting that session\n");


  if (_options.debug)
    log_dbg("Looking for session [username=%.*s,sessionid=%.*s]", 
	    uattr->l-2, uattr->v.t, sattr ? sattr->l-2 : 3, sattr ? (char*)sattr->v.t : "all");
  
  for (appconn = firstusedconn; appconn; appconn = appconn->next) {
    if (!appconn->inuse) { log_err(0, "Connection with inuse == 0!"); }

    if ((appconn->s_state.authenticated) && 
	(strlen(appconn->s_state.redir.username) == uattr->l-2 && 
	 !memcmp(appconn->s_state.redir.username, uattr->v.t, uattr->l-2)) &&
	(!sattr || 
	 (strlen(appconn->s_state.sessionid) == sattr->l-2 && 
	  !strncasecmp(appconn->s_state.sessionid, (char*)sattr->v.t, sattr->l-2)))) {

      if (_options.debug)
	log_dbg("Found session\n");

      if (iscoa)
	config_radius_session(&appconn->s_params, pack, 0, 0);
      else
	terminate_appconn(appconn, RADIUS_TERMINATE_CAUSE_ADMIN_RESET);

      found = 1;
    }
  }

  if (found) {
    if (radius_default_pack(radius, &radius_pack, 
			    iscoa ? RADIUS_CODE_COA_ACK : RADIUS_CODE_DISCONNECT_ACK)) {
      log_err(0, "radius_default_pack() failed");
      return -1;
    }
  }
  else {
    if (radius_default_pack(radius, &radius_pack, 
			    iscoa ? RADIUS_CODE_COA_NAK : RADIUS_CODE_DISCONNECT_NAK)) {
      log_err(0, "radius_default_pack() failed");
      return -1;
    }
  }

  radius_pack.id = pack->id;
  (void) radius_coaresp(radius, &radius_pack, peer, pack->authenticator);

  return 0;
}


/***********************************************************
 *
 * dhcp callback functions
 *
 ***********************************************************/

/* DHCP callback for allocating new IP address */
/* In the case of WPA it is allready allocated,
 * for UAM address is allocated before authentication */
int cb_dhcp_request(struct dhcp_conn_t *conn, struct in_addr *addr, 
		    uint8_t *dhcp_pkt, size_t dhcp_len) {
  struct app_conn_t *appconn = conn->peer;
  struct ippoolm_t *ipm = 0;
  char allocate = 1;

  if (_options.debug) 
    log_dbg("DHCP request for IP address");

  if (!appconn) {
    log_err(0, "Peer protocol not defined");
    return -1;
  }

  /* if uamanyip is on we have to filter out which ip's are allowed */
  if (_options.uamanyip && addr && addr->s_addr) {
    if ((addr->s_addr & ipv4ll_mask.s_addr) == ipv4ll_ip.s_addr) {
      /* clients with an IPv4LL ip normally have no default gw assigned, rendering uamanyip useless
	 They must rather get a proper dynamic ip via dhcp */
      log_dbg("IPv4LL/APIPA address requested, ignoring %s", 
	      inet_ntoa(*addr));
      return -1;
    }
  }
  
  appconn->reqip.s_addr = addr->s_addr; /* Save for MAC auth later */
  
  if (appconn->uplink) {
    
    /*
     *  IP Address is already known and allocated.
     */
    ipm = (struct ippoolm_t*) appconn->uplink;

  } else {

    if ( ! conn->is_reserved) {
      if ((_options.macoklen) && 
	  (appconn->dnprot == DNPROT_DHCP_NONE) &&
	  !maccmp(conn->hismac)) {
	
	/*
	 *  When using macallowed option, and hismac matches.
	 */
	appconn->dnprot = DNPROT_MAC;
	
	if (_options.macallowlocal) {
	  
	  /*
	   *  Local MAC allowed list, authenticate without RADIUS.
	   */
	  upprot_getip(appconn, &appconn->reqip, 0);
	  
	  dnprot_accept(appconn);
	  
	  log_info("Granted MAC=%.2X-%.2X-%.2X-%.2X-%.2X-%.2X with IP=%s access without radius auth" ,
		   conn->hismac[0], conn->hismac[1],
		   conn->hismac[2], conn->hismac[3],
		   conn->hismac[4], conn->hismac[5],
		   inet_ntoa(appconn->hisip));
	  
	  ipm = (struct ippoolm_t*) appconn->uplink;
	  
	} else {
	  /*
	   *  Otherwise, authenticate with RADIUS.
	   */
	  auth_radius(appconn, 0, 0, dhcp_pkt, dhcp_len);
	  allocate = !_options.strictmacauth;
	}
	
      } else if ((_options.macauth) && 
		 (appconn->dnprot == DNPROT_DHCP_NONE)) {
	
	/*
	 *  Using macauth option to authenticate via RADIUS.
	 */
	appconn->dnprot = DNPROT_MAC;
	
	auth_radius(appconn, 0, 0, dhcp_pkt, dhcp_len);
	
	allocate = !_options.strictmacauth;
      }
    }
  }

  if (!ipm) {

    if (!allocate) return -1;

    if (appconn->dnprot != DNPROT_DHCP_NONE && appconn->hisip.s_addr) {
      log_warn(0, "Requested IP address when already allocated (hisip %s)",
	       inet_ntoa(appconn->hisip));
      appconn->reqip.s_addr = appconn->hisip.s_addr;
    }
    
    /* Allocate dynamic IP address */
    /*XXX    if (ippool_newip(ippool, &ipm, &appconn->reqip, 0)) {*/
    if (newip(&ipm, &appconn->reqip, conn->hismac)) {
      log_err(0, "Failed allocate dynamic IP address");
      return -1;
    }
    
    appconn->hisip.s_addr = ipm->addr.s_addr;
    
    log(LOG_NOTICE, "Client MAC=%.2X-%.2X-%.2X-%.2X-%.2X-%.2X assigned IP %s" , 
	conn->hismac[0], conn->hismac[1], 
	conn->hismac[2], conn->hismac[3],
	conn->hismac[4], conn->hismac[5], 
	inet_ntoa(appconn->hisip));
    
    /* TODO: Too many "listen" and "our" addresses hanging around */
    appconn->ourip.s_addr = _options.dhcplisten.s_addr;
    
    appconn->uplink = ipm;
    ipm->peer = appconn; 
    
    if (chilli_assign_snat(appconn, 0) != 0) {
	return -1;
    }
  }

  if (ipm) {
    dhcp_set_addrs(conn, 
		   &ipm->addr, &_options.mask, 
		   &appconn->ourip, &appconn->mask,
		   &_options.dns1, &_options.dns2, 
		   _options.domain);
  }

  /* if not already authenticated, ensure DNAT authstate */
  if (!appconn->s_state.authenticated)
    conn->authstate = DHCP_AUTH_DNAT;

  /* If IP was requested before authentication it was UAM */
  if (appconn->dnprot == DNPROT_DHCP_NONE)
    appconn->dnprot = DNPROT_UAM;

  return 0;
}

/* DHCP callback for establishing new connection */
int cb_dhcp_connect(struct dhcp_conn_t *conn) {
  struct app_conn_t *appconn;

  log(LOG_NOTICE, "New DHCP request from MAC=%.2X-%.2X-%.2X-%.2X-%.2X-%.2X" , 
      conn->hismac[0], conn->hismac[1], 
      conn->hismac[2], conn->hismac[3],
      conn->hismac[4], conn->hismac[5]);
  
  if (_options.debug) 
    log_dbg("New DHCP connection established");

  /* Allocate new application connection */
  if (chilli_new_conn(&appconn)) {
    log_err(0, "Failed to allocate connection");
    return 0;
  }

  appconn->dnlink =  conn;
  appconn->dnprot =  DNPROT_DHCP_NONE;
  conn->peer = appconn;

  appconn->net.s_addr = _options.net.s_addr;
  appconn->mask.s_addr = _options.mask.s_addr;
  appconn->dns1.s_addr = _options.dns1.s_addr;
  appconn->dns2.s_addr = _options.dns2.s_addr;

  memcpy(appconn->hismac, conn->hismac, PKT_ETH_ALEN);
  /*memcpy(appconn->ourmac, conn->ourmac, PKT_ETH_ALEN);*/
  
  set_sessionid(appconn);

  conn->authstate = DHCP_AUTH_NONE; /* TODO: Not yet authenticated */

#ifdef ENABLE_BINSTATFILE
  printstatus();
#endif

  return 0;
}

int cb_dhcp_getinfo(struct dhcp_conn_t *conn, bstring b, int fmt) {
  struct app_conn_t *appconn;
  uint32_t sessiontime = 0;
  uint32_t idletime = 0;

  if (!conn->peer) return 2;
  appconn = (struct app_conn_t*) conn->peer;
  if (!appconn->inuse) return 2;

  if (appconn->s_state.authenticated) {
    sessiontime = mainclock_diffu(appconn->s_state.start_time);
    idletime    = mainclock_diffu(appconn->s_state.last_sent_time);
  }

  switch(fmt) {
#ifdef ENABLE_JSON
  case LIST_JSON_FMT:
    if (appconn->s_state.authenticated)
      session_json_fmt(&appconn->s_state, &appconn->s_params, b, 0);
    break;
#endif
  default:
    {
      bstring tmp = bfromcstr("");

      /* adding: session-id auth-state user-name */
      bassignformat(tmp, " %.*s %d %.*s",
		    appconn->s_state.sessionid[0] ? strlen(appconn->s_state.sessionid) : 1,
		    appconn->s_state.sessionid[0] ? appconn->s_state.sessionid : "-",
		    appconn->s_state.authenticated,
		    appconn->s_state.redir.username[0] ? strlen(appconn->s_state.redir.username) : 1,
		    appconn->s_state.redir.username[0] ? appconn->s_state.redir.username : "-");
      bconcat(b, tmp);

      /* adding: session-time/session-timeout idle-time/idle-timeout */
      bassignformat(tmp, " %d/%d %d/%d",
		    sessiontime, (int)appconn->s_params.sessiontimeout,
		    idletime, (int)appconn->s_params.idletimeout);
      bconcat(b, tmp);

      /* adding: input-octets/max-input-octets */
      bassignformat(tmp, " %lld/%lld",
		    appconn->s_state.input_octets, appconn->s_params.maxinputoctets);
      bconcat(b, tmp);

      /* adding: output-octets/max-output-octets */
      bassignformat(tmp, " %lld/%lld",
		    appconn->s_state.output_octets, appconn->s_params.maxoutputoctets);
      bconcat(b, tmp);

      /* adding: max-total-octets option-swapoctets */
      bassignformat(tmp, " %lld %d", 
		    appconn->s_params.maxtotaloctets, _options.swapoctets);
      bconcat(b, tmp);

#ifdef ENABLE_LEAKYBUCKET
      /* adding: max-bandwidth-up max-bandwidth-down */
      if (appconn->s_state.bucketupsize) {
	bassignformat(tmp, " %d/%lld", 
		      (int) (appconn->s_state.bucketup * 100 / appconn->s_state.bucketupsize),
		      appconn->s_params.bandwidthmaxup);
	bconcat(b, tmp);
      } else 
#endif
	bcatcstr(b, " 0/0");

#ifdef ENABLE_LEAKYBUCKET
      if (appconn->s_state.bucketdownsize) {
	bassignformat(tmp, " %d/%lld ", 
		      (int) (appconn->s_state.bucketdown * 100 / appconn->s_state.bucketdownsize),
		      appconn->s_params.bandwidthmaxdown);
	bconcat(b, tmp);
      } else 
#endif
	bcatcstr(b, " 0/0 ");

      /* adding: original url */
      if (appconn->s_state.redir.userurl[0])
	bcatcstr(b, appconn->s_state.redir.userurl);
      else
	bcatcstr(b, "-");

#ifdef ENABLE_IEEE8021Q
      /* adding: vlan, if one */
      if (appconn->s_state.tag8021q) {
	bassignformat(tmp, " vlan=%d", (int)(ntohs(appconn->s_state.tag8021q) & 0x0FFF));
	bconcat(b, tmp);
      }
#endif
      
      bdestroy(tmp);
    }
  }
  return 0;
}

int terminate_appconn(struct app_conn_t *appconn, int terminate_cause) {
  if (appconn->s_state.authenticated == 1) { /* Only send accounting if logged in */
    dnprot_terminate(appconn);
    appconn->s_state.terminate_cause = terminate_cause;
    acct_req(appconn, RADIUS_STATUS_TYPE_STOP);

    /* should memory be cleared here?? */
    memset(&appconn->s_params, 0, sizeof(appconn->s_params));
    set_sessionid(appconn);
#ifdef ENABLE_STATFILE
    printstatus();
#endif
  }
  return 0;
}

/* Callback when a dhcp connection is deleted */
int cb_dhcp_disconnect(struct dhcp_conn_t *conn, int term_cause) {
  struct app_conn_t *appconn;

  log(LOG_INFO, "DHCP Released MAC=%.2X-%.2X-%.2X-%.2X-%.2X-%.2X IP=%s", 
      conn->hismac[0], conn->hismac[1], 
      conn->hismac[2], conn->hismac[3],
      conn->hismac[4], conn->hismac[5], 
      inet_ntoa(conn->hisip));
  
  if (_options.debug) log_dbg("DHCP connection removed");

  if (!conn->peer) {
    /* No appconn allocated. Stop here */
#ifdef ENABLE_BINSTATFILE
    printstatus();
#endif
    return 0;
  }

  appconn = (struct app_conn_t*) conn->peer;

  if ((appconn->dnprot != DNPROT_DHCP_NONE) &&
      (appconn->dnprot != DNPROT_UAM) &&
      (appconn->dnprot != DNPROT_MAC) &&
      (appconn->dnprot != DNPROT_WPA) &&
      (appconn->dnprot != DNPROT_EAPOL))  {
    return 0; /* DNPROT_WPA and DNPROT_EAPOL are unaffected by dhcp release? */
  }

  terminate_appconn(appconn, 
		    term_cause ? term_cause : 
		    appconn->s_state.terminate_cause ? 
		    appconn->s_state.terminate_cause :
		    RADIUS_TERMINATE_CAUSE_LOST_CARRIER);

  if (appconn->uplink) {
    struct ippoolm_t *member = (struct ippoolm_t *) appconn->uplink;

    if (_options.uamanyip) {
      if (!appconn->natip.s_addr) {
	if (member->in_use && member->is_static) {
	  struct in_addr mask;
	  int res;
	  mask.s_addr = 0xffffffff;
	  res = net_del_route(&member->addr, &appconn->ourip, &mask);
	  log_dbg("Removing route: %s %d\n", inet_ntoa(member->addr), res);
	}
      } else {
	struct ippoolm_t *natipm;
	if (ippool_getip(ippool, &natipm, &appconn->natip) == 0) {
	  if (ippool_freeip(ippool, natipm)) {
	    log_err(0, "ippool_freeip(%s) failed for nat ip!",
		    inet_ntoa(appconn->natip));
	  }
	}
      }
    }

    if (member->in_use && !conn->is_reserved) {
      if (ippool_freeip(ippool, member)) {
	log_err(0, "ippool_freeip(%s) failed!", 
		inet_ntoa(member->addr));
      }
    }
    
    if (_options.usetap) {
      /*
       *    USETAP ARP
       */
      int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
      if (sockfd > 0) {
	struct arpreq req;

	memset(&req, 0, sizeof(req));

	SET_SA_FAMILY(req.arp_pa, AF_INET);
	((struct sockaddr_in *) &req.arp_pa)->sin_addr.s_addr = appconn->hisip.s_addr;
	req.arp_flags = ATF_PERM | ATF_PUBL;

	strncpy(req.arp_dev, tuntap(tun).devname, sizeof(req.arp_dev));

	if (ioctl(sockfd, SIOCDARP, &req) < 0) {
	  perror("ioctrl()");
	}
	close(sockfd);
      }
    }
  }
  
  if (!conn->is_reserved) {
    freeconn(appconn);
  }

#ifdef ENABLE_BINSTATFILE
  printstatus();
#endif

  return 0;
}


/* Callback for receiving messages from dhcp */
int cb_dhcp_data_ind(struct dhcp_conn_t *conn, uint8_t *pack, size_t len) {
  struct app_conn_t *appconn = conn->peer;
  struct pkt_ipphdr_t *ipph = ipphdr(pack);

  /*if (_options.debug)
    log_dbg("cb_dhcp_data_ind. Packet received. DHCP authstate: %d\n", 
    conn->authstate);*/

  if (!appconn) {
    log_err(0, "No peer protocol defined");
    return -1;
  }

  switch (appconn->dnprot) {
  case DNPROT_NULL:
  case DNPROT_DHCP_NONE:
    log_dbg("NULL: %d", appconn->dnprot);
    return -1;

  case DNPROT_UAM:
  case DNPROT_WPA:
  case DNPROT_MAC:
  case DNPROT_EAPOL:
    break;

  default:
    log_err(0, "Unknown downlink protocol: %d", appconn->dnprot);
    break;
  }

  /**
   * packet is coming from an anyip client, therefore SNAT address
   * has been assigned from dynamic pool. So, let's do the SNAT here.
   */
  if (_options.uamanyip && appconn->natip.s_addr) {
    log_dbg("SNAT to: %s", inet_ntoa(appconn->natip));
    ipph->saddr = appconn->natip.s_addr;
    chksum((struct pkt_iphdr_t *) ipph);
  }
  

  /* If the ip dst is uamlisten and pdst is uamport we won't call leaky_bucket,
  *  and we always send these packets through to the tun/tap interface (index 0) 
  */
  if (ipph->daddr  == _options.uamlisten.s_addr && 
      (ipph->dport == htons(_options.uamport) ||
       ipph->dport == htons(_options.uamuiport)))
    return tun_encaps(tun, pack, len, 0);
  
  if (appconn->s_state.authenticated == 1) {
    if (chilli_acct_fromsub(appconn, len))
      return 0;
  }

  return tun_encaps(tun, pack, len, appconn->s_params.routeidx);
}

int chilli_acct_fromsub(struct app_conn_t *appconn, size_t len) {
#ifndef ENABLE_LEAKYBUCKET
  appconn->s_state.last_time = mainclock;
#endif
  
#ifdef ENABLE_LEAKYBUCKET
#ifndef COUNT_UPLINK_DROP
  if (leaky_bucket(appconn, len, 0)) return 1;
#endif
#endif
  
  if (_options.swapoctets) {
    appconn->s_state.input_packets++;
    appconn->s_state.input_octets +=len;
    if (admin_session.s_state.authenticated) {
      admin_session.s_state.input_packets++;
      admin_session.s_state.input_octets+=len;
    }
  } else {
    appconn->s_state.last_sent_time = mainclock;
    appconn->s_state.output_packets++;
    appconn->s_state.output_octets +=len;
    if (admin_session.s_state.authenticated) {
      admin_session.s_state.last_sent_time = mainclock;
      admin_session.s_state.output_packets++;
      admin_session.s_state.output_octets+=len;
    }
  }

#ifdef ENABLE_LEAKYBUCKET
#ifdef COUNT_UPLINK_DROP
  if (leaky_bucket(appconn, len, 0)) return 1;
#endif
#endif

  return 0;
}

int chilli_acct_tosub(struct app_conn_t *appconn, size_t len) {
#ifndef ENABLE_LEAKYBUCKET
  appconn->s_state.last_time = mainclock;
#endif
  
#ifdef ENABLE_LEAKYBUCKET
#ifndef COUNT_DOWNLINK_DROP
  if (leaky_bucket(appconn, 0, len)) return 1;
#endif
#endif
  
  if (_options.swapoctets) {
    appconn->s_state.last_sent_time = mainclock;
    appconn->s_state.output_packets++;
    appconn->s_state.output_octets += len;
    if (admin_session.s_state.authenticated) {
      admin_session.s_state.last_sent_time = mainclock;
      admin_session.s_state.output_packets++;
      admin_session.s_state.output_octets+=len;
    }
  } else {
    appconn->s_state.input_packets++;
    appconn->s_state.input_octets += len;
    if (admin_session.s_state.authenticated) {
      admin_session.s_state.input_packets++;
      admin_session.s_state.input_octets+=len;
    }
  }
  
#ifdef ENABLE_LEAKYBUCKET
#ifdef COUNT_DOWNLINK_DROP
  if (leaky_bucket(appconn, 0, len)) return 1;
#endif
#endif
  
  return 0;
}

/* Callback for receiving messages from eapol */
int cb_dhcp_eap_ind(struct dhcp_conn_t *conn, uint8_t *pack, size_t len) {
  struct eap_packet_t *eap = eappkt(pack);
  struct app_conn_t *appconn = conn->peer;
  struct radius_packet_t radius_pack;
  char mac[MACSTRLEN+1];
  size_t offset;

  if (_options.debug) log_dbg("EAP Packet received");

  /* If this is the first EAPOL authentication request */
  if ((appconn->dnprot == DNPROT_DHCP_NONE) || 
      (appconn->dnprot == DNPROT_EAPOL)) {
    if ((eap->code == 2) && /* Response */
	(eap->type == 1) && /* Identity */
	(len > 5) &&        /* Must be at least 5 octets */
	((len - 5) <= USERNAMESIZE )) {
      memcpy(appconn->s_state.redir.username, eap->payload, len - 5); 
      appconn->dnprot = DNPROT_EAPOL;
      appconn->authtype = EAP_MESSAGE;
    }
    else if (appconn->dnprot == DNPROT_DHCP_NONE) {
      log_err(0, "Initial EAP response was not a valid identity response!");
      return 0;
    }
  }

  /* Return if not EAPOL */
  if (appconn->dnprot != DNPROT_EAPOL) {
    log_warn(0, "Received EAP message, processing for authentication");
    appconn->dnprot = DNPROT_EAPOL;
    return 0;
  }
  
  if (radius_default_pack(radius, &radius_pack, RADIUS_CODE_ACCESS_REQUEST)) {
    log_err(0, "radius_default_pack() failed");
    return -1;
  }

  /* Build up radius request */
  radius_pack.code = RADIUS_CODE_ACCESS_REQUEST;

  radius_addattr(radius, &radius_pack, RADIUS_ATTR_USER_NAME, 0, 0, 0,
			(uint8_t*) appconn->s_state.redir.username, 
			strlen(appconn->s_state.redir.username));

  if (appconn->s_state.redir.statelen) {
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_STATE, 0, 0, 0,
		   appconn->s_state.redir.statebuf,
		   appconn->s_state.redir.statelen);
  }
  
  /* Include EAP (if present) */
  offset = 0;
  while (offset < len) {
    size_t eaplen;

    if ((len - offset) > RADIUS_ATTR_VLEN)
      eaplen = RADIUS_ATTR_VLEN;
    else
      eaplen = len - offset;

    radius_addattr(radius, &radius_pack, RADIUS_ATTR_EAP_MESSAGE, 0, 0, 0,
		   pack + offset, eaplen);

    offset += eaplen;
  } 
  
  if (len)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_MESSAGE_AUTHENTICATOR, 
		   0, 0, 0, NULL, RADIUS_MD5LEN);
  
  radius_addattr(radius, &radius_pack, RADIUS_ATTR_NAS_PORT_TYPE, 0, 0,
		 _options.radiusnasporttype, NULL, 0);
  
  radius_addattr(radius, &radius_pack, RADIUS_ATTR_NAS_PORT, 0, 0,
		 appconn->unit, NULL, 0);
  
  radius_addnasip(radius, &radius_pack);

  snprintf(mac, MACSTRLEN+1, "%.2X-%.2X-%.2X-%.2X-%.2X-%.2X",
	   appconn->hismac[0], appconn->hismac[1],
	   appconn->hismac[2], appconn->hismac[3],
	   appconn->hismac[4], appconn->hismac[5]);
  
  radius_addattr(radius, &radius_pack, RADIUS_ATTR_CALLING_STATION_ID, 0, 0, 0,
		 (uint8_t*) mac, MACSTRLEN);
  
  radius_addcalledstation(radius, &radius_pack);
  
  /* Include NAS-Identifier if given in configuration options */
  if (_options.radiusnasid)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_NAS_IDENTIFIER, 0, 0, 0,
		   (uint8_t*) _options.radiusnasid,
		   strlen(_options.radiusnasid));
  
  return radius_req(radius, &radius_pack, appconn);
}


/***********************************************************
 *
 * uam message handling functions
 *
 ***********************************************************/

int static uam_msg(struct redir_msg_t *msg) {

  struct ippoolm_t *ipm;
  struct app_conn_t *appconn = NULL;
  struct dhcp_conn_t* dhcpconn;

#if defined(HAVE_NETFILTER_QUEUE) || defined(HAVE_NETFILTER_COOVA)
  if (_options.uamlisten.s_addr != _options.dhcplisten.s_addr) {
    msg->mdata.address.sin_addr.s_addr  = msg->mdata.address.sin_addr.s_addr & ~(_options.mask.s_addr);
    msg->mdata.address.sin_addr.s_addr |= _options.dhcplisten.s_addr & _options.mask.s_addr;
  }
#endif

  if (ippool_getip(ippool, &ipm, &msg->mdata.address.sin_addr)) {
    if (_options.debug) 
      log_dbg("UAM login with unknown IP address: %s", inet_ntoa(msg->mdata.address.sin_addr));
    return 0;
  }

  if ((appconn  = (struct app_conn_t *)ipm->peer)        == NULL || 
      (dhcpconn = (struct dhcp_conn_t *)appconn->dnlink) == NULL) {
    log_err(0, "No peer protocol defined");
    return 0;
  }

  if (msg->mdata.opt & REDIR_MSG_OPT_REDIR)
    memcpy(&appconn->s_state.redir, &msg->mdata.redir, sizeof(msg->mdata.redir));

  if (msg->mdata.opt & REDIR_MSG_OPT_PARAMS)
    memcpy(&appconn->s_params, &msg->mdata.params, sizeof(msg->mdata.params));

  if (msg->mdata.opt & REDIR_MSG_NSESSIONID)
    set_sessionid(appconn);

  switch(msg->mtype) {

  case REDIR_LOGIN:
    if (appconn->uamabort) {
      log_info("UAM login from username=%s IP=%s was aborted!", 
	       msg->mdata.redir.username, inet_ntoa(appconn->hisip));
      appconn->uamabort = 0;
      return 0;
    }

    log_info("Successful UAM login from username=%s IP=%s", 
	     msg->mdata.redir.username, inet_ntoa(appconn->hisip));
    
    /* Initialise */
    appconn->s_params.routeidx = tun->routeidx;
    appconn->s_state.redir.statelen = 0;
    appconn->challen  = 0;
    appconn->sendlen  = 0;
    appconn->recvlen  = 0;
    appconn->lmntlen  = 0;
    
    memcpy(appconn->hismac, dhcpconn->hismac, PKT_ETH_ALEN);
    /*memcpy(appconn->ourmac, dhcpconn->ourmac, PKT_ETH_ALEN);*/
    
    appconn->policy = 0; /* TODO */

#ifdef ENABLE_LEAKYBUCKET
#ifdef BUCKET_SIZE
    appconn->s_state.bucketupsize = BUCKET_SIZE;
#else
    appconn->s_state.bucketupsize = appconn->s_params.bandwidthmaxup / 8000 * BUCKET_TIME;
    if (appconn->s_state.bucketupsize < BUCKET_SIZE_MIN) 
      appconn->s_state.bucketupsize = BUCKET_SIZE_MIN;
#endif
#endif

#ifdef ENABLE_LEAKYBUCKET
#ifdef BUCKET_SIZE
    appconn->s_state.bucketdownsize = BUCKET_SIZE;
#else
    appconn->s_state.bucketdownsize = appconn->s_params.bandwidthmaxdown / 8000 * BUCKET_TIME;
    if (appconn->s_state.bucketdownsize < BUCKET_SIZE_MIN) 
      appconn->s_state.bucketdownsize = BUCKET_SIZE_MIN;
#endif
#endif

    return upprot_getip(appconn, NULL, 0);

  case REDIR_LOGOUT:

    log_info("Received UAM logoff from username=%s IP=%s",
	     appconn->s_state.redir.username, inet_ntoa(appconn->hisip));

    if (_options.debug)
      log_dbg("Received logoff from UAM\n");

    if (appconn->s_state.authenticated == 1) {
      terminate_appconn(appconn, RADIUS_TERMINATE_CAUSE_USER_REQUEST);
      appconn->s_params.sessiontimeout = 0;
      appconn->s_params.idletimeout = 0;
    }

    appconn->s_state.uamtime = mainclock;
    dhcpconn->authstate = DHCP_AUTH_DNAT;
    appconn->uamabort = 0;

    break;

  case REDIR_ABORT:
    
    log_info("Received UAM abort from IP=%s", inet_ntoa(appconn->hisip));

    appconn->uamabort = 1; /* Next login will be aborted */
    appconn->s_state.uamtime = 0;  /* Force generation of new challenge */
    dhcpconn->authstate = DHCP_AUTH_DNAT;

    terminate_appconn(appconn, RADIUS_TERMINATE_CAUSE_USER_REQUEST);

    break;

  case REDIR_CHALLENGE:
    appconn->s_state.uamtime = mainclock;
    appconn->uamabort = 0;
    break;

  case REDIR_MACREAUTH:
    auth_radius(appconn, 0, 0, 0, 0);
    break;

  case REDIR_NOTYET:
    break;
  }

  return 0;
}

static int cmdsock_accept(void *nullData, int sock) {
  struct sockaddr_un remote; 
  struct cmdsock_request req;

  bstring s = 0;
  size_t len;
  int csock;
  int rval = 0;

  if (_options.debug) 
    log_dbg("Processing cmdsock request...\n");

  len = sizeof(remote);
  if ((csock = accept(sock, (struct sockaddr *)&remote, &len)) == -1) {
    log_err(errno, "cmdsock_accept()/accept()");
    return -1;
  }

  if (read(csock, &req, sizeof(req)) != sizeof(req)) {
    log_err(errno, "cmdsock_accept()/read()");
    close(csock);
    return -1;
  }

#ifdef HAVE_NETFILTER_COOVA
  kmod_coova_sync();
#endif

  switch(req.type) {

  case CMDSOCK_DHCP_LIST:
    s = bfromcstr("");
    if (dhcp) dhcp_list(dhcp, s, 0, 0,
			req.options & CMDSOCK_OPT_JSON ? 
			LIST_JSON_FMT : LIST_SHORT_FMT);
    if (write(csock, s->data, s->slen) < 0)
      log_err(errno, "write()");
    break;
    
  case CMDSOCK_DHCP_DROP:
    if (dhcp) dhcp_block_mac(dhcp, req.data.mac);
    break;

  case CMDSOCK_DHCP_RELEASE:
    if (dhcp) dhcp_release_mac(dhcp, req.data.mac, RADIUS_TERMINATE_CAUSE_ADMIN_RESET);
    break;

  case CMDSOCK_LIST:
    s = bfromcstr("");
    if (dhcp) dhcp_list(dhcp, s, 0, 0,
			req.options & CMDSOCK_OPT_JSON ?
			LIST_JSON_FMT : LIST_LONG_FMT);
    if (write(csock, s->data, s->slen) < 0)
      log_err(errno, "write()");
    break;

  case CMDSOCK_LIST_IPPOOL:
    ippool_print(csock, ippool);
    break;

  case CMDSOCK_ENTRY_FOR_IP:
    s = bfromcstr("");
    if (dhcp) dhcp_entry_for_ip(dhcp, s, &req.data.sess.ip,
			req.options & CMDSOCK_OPT_JSON ?
			LIST_JSON_FMT : LIST_LONG_FMT);
    if (write(csock, s->data, s->slen) < 0)
      log_err(errno, "write()");
    break;

  case CMDSOCK_ENTRY_FOR_MAC:
    s = bfromcstr("");
    if (dhcp) dhcp_entry_for_mac(dhcp, s, req.data.mac,
			req.options & CMDSOCK_OPT_JSON ?
			LIST_JSON_FMT : LIST_LONG_FMT);
    if (write(csock, s->data, s->slen) < 0)
      log_err(errno, "write()");
    break;

  case CMDSOCK_SHOW:
    /*ToDo*/
    break;

  case CMDSOCK_ROUTE_SET:
  case CMDSOCK_ROUTE_GW:
    {
      if (req.type == CMDSOCK_ROUTE_GW) {
	log_dbg("setting route for idx %d", req.data.sess.params.routeidx);
	copy_mac6(tun(tun, req.data.sess.params.routeidx).gwaddr, req.data.mac);
      } else {
	struct dhcp_conn_t *conn = dhcp->firstusedconn;
	log_dbg("looking to authorized session %s",inet_ntoa(req.data.sess.ip));
	while (conn && conn->inuse) {
	  if (conn->peer) {
	    struct app_conn_t * appconn = (struct app_conn_t*)conn->peer;
	    if (!memcmp(appconn->hismac, req.data.mac, 6)) {
	      log_dbg("routeidx %s %d",appconn->s_state.sessionid, req.data.sess.params.routeidx);
	      appconn->s_params.routeidx = req.data.sess.params.routeidx;
	      break;
	    }
	  }
	  conn = conn->next;
	}
      }
    }
    /* drop through */
  case CMDSOCK_ROUTE:
    {
      int i;
      bstring b = bfromcstr("routes:\n");
      int err = 0;
      if (write(csock, b->data, b->slen) == b->slen) {
	for (i=0; !err && i<tun->_interface_count; i++) {
	  char gw[56];

	  strncpy(gw, inet_ntoa(tun->_interfaces[i].gateway), sizeof(gw)-1);

	  bassignformat(b, "idx: %d dev: %s %s %.2X-%.2X-%.2X-%.2X-%.2X-%.2X %s %.2X-%.2X-%.2X-%.2X-%.2X-%.2X%s\n", 
			i, tun->_interfaces[i].devname,
			inet_ntoa(tun->_interfaces[i].address),
			tun->_interfaces[i].hwaddr[0],
			tun->_interfaces[i].hwaddr[1],
			tun->_interfaces[i].hwaddr[2],
			tun->_interfaces[i].hwaddr[3],
			tun->_interfaces[i].hwaddr[4],
			tun->_interfaces[i].hwaddr[5],
			gw,
			tun->_interfaces[i].gwaddr[0],
			tun->_interfaces[i].gwaddr[1],
			tun->_interfaces[i].gwaddr[2],
			tun->_interfaces[i].gwaddr[3],
			tun->_interfaces[i].gwaddr[4],
			tun->_interfaces[i].gwaddr[5],
			i == 0 ? " (tun/tap)":"");

	  if (write(csock, b->data, b->slen) < 0)
	    err = 1;
	}
	
	if (!err) { 
	  struct dhcp_conn_t *conn = dhcp->firstusedconn;
	  bassignformat(b, "subscribers:\n");
	  if (write(csock, b->data, b->slen) == b->slen) {
	    while (conn) {
	      struct app_conn_t *appconn = (struct app_conn_t *)conn->peer;
	      
	      bassignformat(b, "mac: %.2X-%.2X-%.2X-%.2X-%.2X-%.2X -> idx: %d\n", 
			    appconn->hismac[0], appconn->hismac[1],
			    appconn->hismac[2], appconn->hismac[3],
			    appconn->hismac[4], appconn->hismac[5],
			    appconn->s_params.routeidx);
	      
	      if (write(csock, b->data, b->slen) < 0)
		break;

	      conn = conn->next;
	    }
	  }
	}
      }
      bdestroy(b);
    }
    break;

  case CMDSOCK_LOGIN:
  case CMDSOCK_AUTHORIZE:
    if (dhcp) {
      struct dhcp_conn_t *dhcpconn = dhcp->firstusedconn;
      log_dbg("looking to authorized session %s",inet_ntoa(req.data.sess.ip));
      while (dhcpconn && dhcpconn->inuse) {
	if (dhcpconn->peer) {
	  struct app_conn_t * appconn = (struct app_conn_t*) dhcpconn->peer;
	  if (  (req.data.sess.ip.s_addr == 0    || appconn->hisip.s_addr == req.data.sess.ip.s_addr) &&
		(req.data.sess.sessionid[0] == 0 || !strcmp(appconn->s_state.sessionid,req.data.sess.sessionid))
		){
	    char *uname = req.data.sess.username;

	    log_dbg("remotely authorized session %s",appconn->s_state.sessionid);
	    memcpy(&appconn->s_params, &req.data.sess.params, sizeof(req.data.sess.params));

	    if (uname[0]) strncpy(appconn->s_state.redir.username, uname, USERNAMESIZE);
	    session_param_defaults(&appconn->s_params);

	    if (req.type == CMDSOCK_LOGIN) {
	      auth_radius(appconn, uname, req.data.sess.password, 0, 0);
	    } else {
	      dnprot_accept(appconn);
	    }
	    break;
	  }
	}
	dhcpconn = dhcpconn->next;
      }
    }
    break;

  case CMDSOCK_RELOAD:
    _sigusr1(SIGUSR1);
    break;

#ifdef ENABLE_STATFILE
  case CMDSOCK_STATUSFILE:
    printstatus();
    break;
#endif

  default:
    perror("unknown command");
    close(csock);
    rval = -1;
  }

  if (s) bdestroy(s);
  shutdown(csock, 2);
  close(csock);

  return rval;
}

#if XXX_IO_DAEMON 
int chilli_io(int fd_ctrl_r, int fd_ctrl_w, int fd_pkt_r, int fd_pkt_w) {
  int maxfd = 0;
  fd_set fds;
  int status;

  while (1) {
    fd_zero(&fds);

    fd_set(fd_ctrl_r, &fds);
    fd_set(fd_ctrl_w, &fds);
    fd_set(fd_pkt_r, &fds);
    fd_set(fd_pkt_w, &fds);

    if  ((status = select(maxfd + 1, &fds, NULL, NULL, NULL)) == -1) {
      if (EINTR != errno) {
	log_err(errno, "select() returned -1!");
      }
    }

    if (status > 0) {
      if (fd_isset(fd_ctrl_r, &fds)) {
      }
      if (fd_isset(fd_ctrl_w, &fds)) {
      }
      if (fd_isset(fd_pkt_r, &fds)) {
      }
      if (fd_isset(fd_pkt_w, &fds)) {
      }
    } else {
      log_err(errno, "problem in select");
      break;
    }
  }

  exit(1);
}
#endif

#ifdef USING_IPC_UNIX
int static redir_msg(struct redir_t *this) {
  struct redir_msg_t msg;
  struct sockaddr_un remote; 
  size_t len = sizeof(remote);
  int socket = accept(this->msgfd, (struct sockaddr *)&remote, &len);
  if (socket > 0) {
    int msgresult = read(socket, &msg, sizeof(msg));
    if (msgresult == sizeof(msg)) {
      if (msg.mtype == REDIR_MSG_STATUS_TYPE) {
	struct redir_conn_t conn;
	memset(&conn, 0, sizeof(conn));
	cb_redir_getstate(redir, 
			  &msg.mdata.address, 
			  &msg.mdata.baddress, 
			  &conn);
	if (write(socket, &conn, sizeof(conn)) < 0) {
	  log_err(errno, "redir_msg");
	}
      } else {
	uam_msg(&msg);
      }
    }
    close(socket);
  }
  return 0;
}
#endif

#ifdef ENABLE_RTMON
static int rtmon_proc_route(struct rtmon_t *rtmon, 
			    struct rtmon_iface *iface,
			    struct rtmon_route *route) {
  int i;
  for (i=0; i < tun->_interface_count; i++) {
    if (tun->_interfaces[i].ifindex == route->if_index) {
      memcpy(tun->_interfaces[i].gwaddr, route->gwaddr, sizeof(tun->_interfaces[i].gwaddr));
      tun->_interfaces[i].gateway.s_addr = route->gateway.s_addr;
    }
  }

  return 0;
}

static int rtmon_init(struct rtmon_t *rtmon) {
  rtmon->fd = rtmon_open_netlink();
  if (rtmon->fd > 0) {
    discover_ifaces(rtmon);
    discover_routes(rtmon);
    
    if (_options.debug) {
      print_ifaces(rtmon);
      print_routes(rtmon);
    }
    
    check_updates(rtmon, rtmon_proc_route);
    return 0;
  }
  return -1;
}

static int rtmon_accept(struct rtmon_t *rtmon, int idx) {
  if (rtmon_read_event(rtmon, rtmon_proc_route))
    log_err(errno, "error reading netlink message");
  return 0;
}
#endif

#ifdef HAVE_LIBRT
struct timespec startup_real;
struct timespec startup_mono;
#endif

static inline void macauth_reserved() {
  struct dhcp_conn_t *conn = dhcp->firstusedconn;
  struct app_conn_t *appconn;

  while (conn) {
    if (conn->is_reserved && conn->peer) {
      appconn = (struct app_conn_t *)conn->peer;
      if (!appconn->s_state.authenticated) {
	auth_radius((struct app_conn_t *)conn->peer, 0, 0, 0, 0);
      }
    }
    conn = conn->next;
  }
}

int chilli_main(int argc, char **argv) {
  select_ctx sctx;
  int status;

  /*  struct itimerval itval; */
  int lastSecond = 0;

  int cmdsock = -1;

  pid_t cpid = getpid();

#ifdef USING_IPC_MSG
  struct redir_msg_t msg;
  int msgresult;
#endif

#if XXX_IO_DAEMON 
  pid_t chilli_fork = 0;
  int is_slave = 0;

  int ctrl_main_to_io[2];  /* 0/1 read/write - control messages from main -> io */
  int ctrl_io_to_main[2];  /* 0/1 read/write - control messages from io -> main */
  int pkt_main_to_io[2];
  int pkt_io_to_main[2];
#endif

#ifdef ENABLE_RTMON
  struct rtmon_t _rtmon;
#endif

  int i;

  int keep_going = 1;
  int reload_config = 0;

  /* open a connection to the syslog daemon */
  /*openlog(PACKAGE, LOG_PID, LOG_DAEMON);*/
  openlog(PACKAGE, (LOG_PID | LOG_PERROR), LOG_DAEMON);

  options_init();

  /* Process options given in configuration file and command line */
  if (process_options(argc, argv, 0))
    exit(1);

  /* foreground                                                   */
  /* If flag not given run as a daemon                            */
  if (!_options.foreground) {
    /* Close the standard file descriptors. */
    /* Is this really needed ? */
    if (!freopen("/dev/null", "w", stdout)) log_err(errno,"freopen()");
    if (!freopen("/dev/null", "w", stderr)) log_err(errno,"freopen()");
    if (!freopen("/dev/null", "r", stdin))  log_err(errno,"freopen()");
    if (daemon(1, 1)) {
      log_err(errno, "daemon() failed!");
    }
    else {

      /*
       *  We switched PID when we forked. 
       *  To keep things nice and tity, lets move the
       *  binary configuration file to the new directory. 
       *
       *  TODO: This process isn't ideal. But, the goal remains
       *  that we don't need cmdline.o in the running chilli. We may
       *  want to move away from gengetopt as it isn't exactly the most
       *  flexible or light-weight. 
       */

      mode_t process_mask = umask(0077);
      char file[128];
      char file2[128];
      int ok;

      pid_t new_pid = getpid();

      bstring bt = bfromcstr("");

      /*
       * Create the new temporary directory.
       */
      snprintf(file2, sizeof(file2), "/tmp/chilli-%d", new_pid);
      if (mkdir(file2, S_IRWXU | S_IRWXG | S_IRWXO))
	log_err(errno, file2);

      /*
       * Format the filename of the current (cpid) and new binconfig files.
       */
      chilli_binconfig(file, sizeof(file), cpid);
      chilli_binconfig(file2, sizeof(file2), new_pid);

      /*
       * Reset the binconfig option and save current setttings.
       */
      _options.binconfig = file2;
      ok = options_save(file2, bt);

      if (!ok) {
	log_err(errno, "could not save configuration options! [%s]", file2);
	exit(1);
      }

      /*
       * Reset binconfig (since file2 is a local variable)
       */
      _options.binconfig = 0;

      /* 
       * Remove old file
       */
      unlink(file);
      snprintf(file, sizeof(file), "/tmp/chilli-%d", cpid);
      if (rmdir(file)) log_err(errno, file);
      umask(process_mask);

      cpid = new_pid;
      bdestroy(bt);
    }
  } 

  if (_options.logfacility < 0 || _options.logfacility > LOG_NFACILITIES)
    _options.logfacility= LOG_FAC(LOG_LOCAL6);

  closelog(); 

  openlog(PACKAGE, LOG_PID, (_options.logfacility<<3));

  chilli_signals(&keep_going, &reload_config);

#if XXX_IO_DAEMON 
  pipe(ctrl_main_to_io);
  pipe(ctrl_io_to_main);
  pipe(pkt_main_to_io);
  pipe(pkt_io_to_main);

  chilli_fork = fork();
  is_slave = chilli_fork > 0;
  if (chilli_fork < 0) perror("fork()");
  if (chilli_fork == 0) 
    /* kick off io daemon */
    return chilli_io(ctrl_main_to_io[0],
		     ctrl_io_to_main[1],
		     pkt_main_to_io[0],
		     pkt_io_to_main[1]);
#endif


  chilli_pid = getpid();
  
  /* This has to be done after we have our final pid */
  log_pid((_options.pidfile && *_options.pidfile) ? _options.pidfile : DEFPIDFILE);

  /* setup IPv4LL/APIPA network ip and mask for uamanyip exception */
  inet_aton("169.254.0.0", &ipv4ll_ip);
  inet_aton("255.255.0.0", &ipv4ll_mask);

  syslog(LOG_INFO, "CoovaChilli(ChilliSpot) %s. Copyright 2002-2005 Mondru AB. Licensed under GPL. "
	 "Copyright 2006-2010 Coova Technologies, LLC <support@coova.com>. Licensed under GPL. "
	 "See http://www.coova.org/ for details.", VERSION);

  memset(&sctx, 0, sizeof(sctx));

#ifdef HAVE_LIBRT
  memset(&startup_real, 0, sizeof(startup_real));
  memset(&startup_mono, 0, sizeof(startup_mono));
  if (clock_gettime(CLOCK_REALTIME, &startup_real) < 0) {
    log_err(errno, "getting startup (realtime) time");
  }
  log_dbg("clock realtime sec %d nsec %d", startup_real.tv_sec, startup_real.tv_nsec);
#ifdef CLOCK_MONOTONIC
  if (clock_gettime(CLOCK_MONOTONIC, &startup_mono) < 0) {
    log_err(errno, "getting startup (monotonic) time");
  }
  log_dbg("clock monotonic sec %d nsec %d", startup_mono.tv_sec, startup_mono.tv_nsec);
#endif
#endif

  mainclock_tick();

  /* Create a tunnel interface */
  if (tun_new(&tun)) {
    log_err(0, "Failed to create tun");
    exit(1);
  }
  
  tun_setaddr(tun, &_options.uamlisten,  &_options.uamlisten, &_options.mask);
  
  tun_set_cb_ind(tun, cb_tun_ind);
  
  if (_options.ipup) tun_runscript(tun, _options.ipup);
  
  /* Allocate ippool for dynamic IP address allocation */
  if (ippool_new(&ippool, 
		 _options.dynip, 
		 _options.dhcpstart, 
		 _options.dhcpend, 
		 _options.statip, 
		 _options.allowdyn, 
		 _options.allowstat)) {
    log_err(0, "Failed to allocate IP pool!");
    exit(1);
  }
  
  /* Create an instance of dhcp */
  if (dhcp_new(&dhcp, _options.max_clients, _options.dhcpif,
	       _options.dhcpusemac, _options.dhcpmac, 1, 
	       &_options.dhcplisten, _options.lease, 1, 
	       &_options.uamlisten, _options.uamport, 
	       _options.eapolenable, _options.noc2c)) {
    log_err(0, "Failed to create dhcp listener on %s", _options.dhcpif);
    exit(1);
  }
  
  dhcp_set_cb_request(dhcp, cb_dhcp_request);
  dhcp_set_cb_connect(dhcp, cb_dhcp_connect);
  dhcp_set_cb_disconnect(dhcp, cb_dhcp_disconnect);
  dhcp_set_cb_data_ind(dhcp, cb_dhcp_data_ind);
  dhcp_set_cb_eap_ind(dhcp, cb_dhcp_eap_ind);
  dhcp_set_cb_getinfo(dhcp, cb_dhcp_getinfo);
  
  if (dhcp_set(dhcp, 
	       _options.ethers, 
	       (_options.debug & DEBUG_DHCP))) {
    log_err(0, "Failed to set DHCP parameters");
    exit(1);
  }
  
  /* Create an instance of radius */
  if (radius_new(&radius,
		 &_options.radiuslisten, _options.coaport, _options.coanoipcheck,
		 &_options.proxylisten, _options.proxyport,
		 &_options.proxyaddr, &_options.proxymask,
		 _options.proxysecret) ||
      radius_init_q(radius, 0)) {
    log_err(0, "Failed to create radius");
    return -1;
  }

  radius_set(radius, dhcp ? dhcp->rawif.hwaddr : 0, (_options.debug & DEBUG_RADIUS));
  radius_set_cb_auth_conf(radius, cb_radius_auth_conf);
  radius_set_cb_coa_ind(radius, cb_radius_coa_ind);
  radius_set_cb_ind(radius, cb_radius_ind);
  
  if (_options.acct_update)
    radius_set_cb_acct_conf(radius, cb_radius_acct_conf);
  
  /* Initialise connections */
  initconn();
  
  /* Create an instance of redir */
  if (redir_new(&redir, &_options.uamlisten, _options.uamport, _options.uamuiport)) {
    log_err(0, "Failed to create redir");
    return -1;
  }

#ifndef ENABLE_CHILLIREDIR
  if (redir_listen(redir)) {
    log_err(0, "Failed to create redir listen");
    return -1;
  }
#endif

  if (redir_ipc(redir)) {
    log_err(0, "Failed to create redir IPC");
    return -1;
  }
  
  redir_set(redir, dhcp->rawif.hwaddr, (_options.debug));

  /* not really needed for chilliredir */
  redir_set_cb_getstate(redir, cb_redir_getstate);
  
  if (_options.cmdsocket) {
    cmdsock = cmdsock_init();
  }
  
  if (_options.usetap && _options.rtmonfile) {
#ifdef ENABLE_RTMON_
    pid_t p = fork();
    if (p < 0) {
      perror("fork");
    } else if (p == 0) {
      char pst[16];
      char *newargs[16];
      
      i=0;
      sprintf(pst,"%d",cpid);
      newargs[i++] = "[chilli_rtmon]";
      newargs[i++] = "-file";
      newargs[i++] = _options.rtmonfile;
      newargs[i++] = "-pid";
      newargs[i++] = pst;
      newargs[i++] = NULL;
      
      if (execv(SBINDIR "/chilli_rtmon", newargs) != 0) {
	log_err(errno, "execl() did not return 0!");
	exit(0);
      }

    } else {
      rtmon_pid = p;
    }
#else
    log_err(0, "Feature is not supported; use --enable-rtmon");
#endif
  }

  if (_options.radsec) {
#ifdef ENABLE_CHILLIRADSEC
    pid_t p = fork();
    if (p < 0) {
      perror("fork");
    } else if (p == 0) {
      char *newargs[16];
      char file[128];
      
      i=0;
      chilli_binconfig(file, sizeof(file), cpid);
      
      newargs[i++] = "[chilli_radsec]";
      newargs[i++] = "-b";
      newargs[i++] = file;
      newargs[i++] = NULL;
      
      if (execv(SBINDIR "/chilli_radsec", newargs) != 0) {
	log_err(errno, "execl() did not return 0!");
	exit(0);
      }
      
    } else {
      proxy_pid = p;
    }
#else
    log_err(0, "Feature is not supported; use --enable-chilliradsec");
#endif
  } else if (_options.uamaaaurl) {
#ifdef ENABLE_CHILLIPROXY
    pid_t p = fork();
    if (p < 0) {
      perror("fork");
    } else if (p == 0) {
      char *newargs[16];
      char file[128];

      i=0;
      chilli_binconfig(file, sizeof(file), cpid);

      newargs[i++] = "[chilli_proxy]";
      newargs[i++] = "-b";
      newargs[i++] = file;
      newargs[i++] = NULL;
      
      if (execv(SBINDIR "/chilli_proxy", newargs) != 0) {
	log_err(errno, "execl() did not return 0!");
	exit(0);
      }

    } else {
      proxy_pid = p;
    }
#else
    log_err(0, "Feature is not supported; use --enable-chilliproxy");
#endif
  }

#ifdef ENABLE_CHILLIREDIR
  { 
    pid_t p = fork();
    if (p < 0) {
      perror("fork");
    } else if (p == 0) {
      char *newargs[16];
      char file[128];

      i=0;
      chilli_binconfig(file, sizeof(file), cpid);
      
      newargs[i++] = "[chilli_redir]";
      newargs[i++] = "-b";
      newargs[i++] = file;
      newargs[i++] = NULL;
      
      if (execv(SBINDIR "/chilli_redir", newargs) != 0) {
	log_err(errno, "execl() did not return 0!");
	exit(0);
      }
      
    } else {
      redir_pid = p;
    }
  }
#endif


  if (_options.debug) 
    log_dbg("Waiting for client request...");


  /*
   * Administrative-User session
   */
  memset(&admin_session, 0, sizeof(admin_session));
  
#ifdef ENABLE_BINSTATFILE
  if (loadstatus() != 0) /* Only indicate a fresh start-up if we didn't load keepalive sessions */
#endif
  {
    acct_req(&admin_session, RADIUS_STATUS_TYPE_ACCOUNTING_ON);
#ifdef HAVE_NETFILTER_COOVA
    kmod_coova_clear();
#endif
  }

  if (_options.ethers && *_options.ethers && _options.macauth) 
    macauth_reserved();
  
  if (_options.adminuser) {
    admin_session.is_adminsession = 1;
    strncpy(admin_session.s_state.redir.username, 
	    _options.adminuser, 
	    sizeof(admin_session.s_state.redir.username));
    set_sessionid(&admin_session);
    chilliauth_radius(radius);
  }
  

  /******************************************************************/
  /* Main select loop                                               */
  /******************************************************************/

  if (_options.uid && setuid(_options.uid)) {
    log_err(errno, "setuid(%d) failed while running with uid = %d\n", _options.uid, getuid());
  }

  if (_options.gid && setgid(_options.gid)) {
    log_err(errno, "setgid(%d) failed while running with gid = %d\n", _options.gid, getgid());
  }

  if (net_select_init(&sctx))
    log_err(errno, "select init");

  for (i=0; i < tun->_interface_count; i++) 
    net_select_reg(&sctx, 
		   (tun)->_interfaces[i].fd,
		   SELECT_READ, (select_callback) tun_decaps, 
		   tun, i);

  net_select_reg(&sctx, radius->fd, SELECT_READ, (select_callback)radius_decaps, radius, 0);
  net_select_reg(&sctx, radius->proxyfd, SELECT_READ, (select_callback)radius_proxy_ind, radius, 0);

#if defined(__linux__)
  net_select_reg(&sctx, dhcp->relayfd, SELECT_READ, (select_callback)dhcp_relay_decaps, dhcp, 0);
  net_select_reg(&sctx, dhcp->rawif.fd, SELECT_READ, (select_callback)dhcp_decaps, dhcp, 0);
#ifdef HAVE_NETFILTER_QUEUE
  if (dhcp->qif_in.fd && dhcp->qif_out.fd) {
    net_select_reg(&sctx, dhcp->qif_in.fd, SELECT_READ, (select_callback)dhcp_decaps, dhcp, 1);
    net_select_reg(&sctx, dhcp->qif_out.fd, SELECT_READ, (select_callback)dhcp_decaps, dhcp, 2);
  }
#endif
#elif defined (__FreeBSD__)  || defined (__APPLE__) || defined (__OpenBSD__)
  net_select_reg(&sctx, dhcp->rawif.fd, SELECT_READ, (select_callback)dhcp_receive, dhcp, 0);
#endif

#ifdef USING_IPC_UNIX
  net_select_reg(&sctx, redir->msgfd, SELECT_READ, (select_callback)redir_msg, redir, 0);
#endif

#ifndef ENABLE_CHILLIREDIR
  net_select_reg(&sctx, redir->fd[0], SELECT_READ, (select_callback)redir_accept, redir, 0);
  net_select_reg(&sctx, redir->fd[1], SELECT_READ, (select_callback)redir_accept, redir, 1);
#endif

#ifdef ENABLE_RTMON
  if (!rtmon_init(&_rtmon)) {
    net_select_reg(&sctx, _rtmon.fd, SELECT_READ, (select_callback)rtmon_accept, &_rtmon, 0);
  }
#endif

  net_select_reg(&sctx, cmdsock, SELECT_READ, (select_callback)cmdsock_accept, 0, cmdsock);

  mainclock_tick();
  while (keep_going) {

    if (reload_config) {
      reload_options(argc, argv);

      reload_config = 0;

      /* Reinit DHCP parameters */
      if (dhcp) {
	dhcp_set(dhcp, 
		 _options.ethers,
		 (_options.debug & DEBUG_DHCP));
      }
      
      /* Reinit RADIUS parameters */
      radius_set(radius, dhcp->rawif.hwaddr, (_options.debug & DEBUG_RADIUS));
      
      /* Reinit Redir parameters */
      redir_set(redir, dhcp->rawif.hwaddr, _options.debug);
    }

    if (do_interval) {
      reprocess_options(argc, argv);

      do_interval = 0;
      
      if (_options.adminuser)
	chilliauth_radius(radius);
    }

    if (lastSecond != mainclock) {
      /*
       *  Every second, more or less
       */
      radius_timeout(radius);

      if (dhcp) 
	dhcp_timeout(dhcp);
      
      checkconn();
      lastSecond = mainclock;
      /*do_timeouts = 0;*/
    }

    if (net_select_prepare(&sctx))
      log_err(errno, "select prepare");

    status = net_select(&sctx);

    mainclock_tick();

#ifdef USING_IPC_MSG
    if ((msgresult = 
	 TEMP_FAILURE_RETRY(msgrcv(redir->msgid, (void *)&msg, sizeof(msg.mdata), 0, IPC_NOWAIT)))  == -1) {
      if ((errno != EAGAIN) && (errno != ENOMSG))
	log_err(errno, "msgrcv() failed!");
    }

    if (msgresult > 0) 
      uam_msg(&msg);
#endif

    if (status > 0) {

      net_run_selected(&sctx, status);
      
    }
    
#ifdef USING_MMAP

    net_run(&dhcp->rawif);
    if (tun) {
      for (i=0; i < (tun)->_interface_count; i++) {
	net_run(&(tun)->_interfaces[i]); 
      }
    }

#endif
    
  } /* while(keep_going) */
  
  log_info("CoovaChilli shutting down");
  
  if (_options.seskeepalive) {
#ifdef ENABLE_BINSTATFILE
    if (printstatus() != 0) log_err(errno, "could not save status file");
#else
    log_warn(0, "Not stopping sessions! seskeepalive should be used with compile option --enable-binstatusfile");
#endif
  } else {
    killconn();
#ifdef ENABLE_STATFILE
    if (printstatus() != 0) log_err(errno, "could not save status file");
#endif
  }


  if (redir) 
    redir_free(redir);

  if (radius) 
    radius_free(radius);

  if (dhcp) 
    dhcp_free(dhcp);
  
  if (_options.ipdown)
    tun_runscript(tun, _options.ipdown);

  if (tun) 
    tun_free(tun);

  if (ippool) 
    ippool_free(ippool);

  { /* clean up run-time files */
    char file[128];

    chilli_binconfig(file, sizeof(file), cpid);
    if (remove(file)) log_err(errno, file);

    snprintf(file,sizeof(file),"/tmp/chilli-%d", cpid);
    if (rmdir(file)) log_err(errno, file);
  }

  /*
   *  Terminate nicely
   */

#ifdef ENABLE_RTMON_
  if (rtmon_pid > 0)
    kill(rtmon_pid, SIGTERM);
#endif
  
#if defined(ENABLE_CHILLIPROXY) || defined(ENABLE_CHILLIRADSEC)
  if (proxy_pid > 0)
    kill(proxy_pid, SIGTERM);
#endif
  
#ifdef ENABLE_CHILLIREDIR
  if (redir_pid > 0)
    kill(redir_pid, SIGTERM);
#endif

  options_destroy();

  return 0;
}
