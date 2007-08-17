/*
 *
 * HTTP redirection functions.
 * Copyright (C) 2004, 2005 Mondru AB.
 * Copyright (c) 2006-2007 David Bird <david@cova.com>
 *
 * The contents of this file may be used under the terms of the GNU
 * General Public License Version 2, provided that the above copyright
 * notice and this permission notice is included in all copies or
 * substantial portions of the software.
 *
 */

#include "system.h"
#include "syserr.h"
#include "radius.h"
#include "radius_wispr.h"
#include "radius_chillispot.h"
#include "redir.h"
#include "md5.h"
#include "dhcp.h"
#include "chilli.h"
#include "options.h"
#include "bstrlib.h"

static int optionsdebug = 0; /* TODO: Should be changed to instance */

static int keep_going = 1;   /* OK as global variable for child process */

static int termstate = REDIR_TERM_INIT;    /* When we were terminated */

char credits[] =
"<H1>ChilliSpot " VERSION "</H1>"
"<p>Copyright 2002-2005 Mondru AB</p>"
"<p>Copyright 2006-2007 <a href=\"http://coova.org/\">Coova Technologies Ltd</a></p>"
"ChilliSpot is an Open Source captive portal or wireless LAN access point "
"controller developed by the community at "
"<a href=\"http://coova.org\">coova.org</a> and "
"<a href=\"http://www.chillispot.org\">www.chillispot.org</a>. It is licensed "
"under the GPL.</p><p>ChilliSpot acknowledges all community members, "
"especially those mentioned at "
"<a href=\"http://www.chillispot.org/credits.html\">http://www.chillispot.org/credits.html</a>.";

struct redir_socket{int fd[2];};
static unsigned char redir_radius_id=0;
static int redir_getparam(struct redir_t *redir, char *src, char *param, bstring dst);

/* Termination handler for clean shutdown */
static void redir_termination(int signum) {
  if (optionsdebug) log_dbg("Terminating redir client!\n");
  keep_going = 0;
}

/* Alarm handler for ensured shutdown */
static void redir_alarm(int signum) {
  log_warn(0, "Client process timed out: %d", termstate);
  exit(0);
}

/* Generate a 16 octet random challenge */
static int redir_challenge(unsigned char *dst) {
  FILE *file;

  if ((file = fopen("/dev/urandom", "r")) == NULL) {
    log_err(errno, "fopen(/dev/urandom, r) failed");
    return -1;
  }
  
  if (fread(dst, 1, REDIR_MD5LEN, file) != REDIR_MD5LEN) {
    log_err(errno, "fread() failed");
    return -1;
  }
  
  fclose(file);
  return 0;
}

/* Convert 32+1 octet ASCII hex string to 16 octet unsigned char */
static int redir_hextochar(unsigned char *src, unsigned char * dst) {
  char x[3];
  int n;
  int y;
  
  for (n=0; n< REDIR_MD5LEN; n++) {
    x[0] = src[n*2+0];
    x[1] = src[n*2+1];
    x[2] = 0;
    if (sscanf (x, "%2x", &y) != 1) {
      log_err(0, "HEX conversion failed!");
      return -1;
    }
    dst[n] = (unsigned char) y;
  }

  return 0;
}

/* Convert 16 octet unsigned char to 32+1 octet ASCII hex string */
static int redir_chartohex(unsigned char *src, char *dst) {
  char x[3];
  int n;
  
  for (n=0; n<REDIR_MD5LEN; n++) {
    snprintf(x, 3, "%.2x", src[n]);
    dst[n*2+0] = x[0];
    dst[n*2+1] = x[1];
  }

  dst[REDIR_MD5LEN*2] = 0;
  return 0;
}

static int redir_xmlencode(char *src, int srclen, char *dst, int dstsize) {
  char *x;
  int n;
  int i = 0;
  
  for (n=0; n<srclen; n++) {
    x=0;
    switch(src[n]) {
    case '&':  x = "&amp;";  break;
    case '\"': x = "&quot;"; break;
    case '<':  x = "&lt;";   break;
    case '>':  x = "&gt;";   break;
    default:
      if (i < dstsize - 1) dst[i++] = src[n];
      break;
    }
    if (x) {
      if (i < dstsize - strlen(x)) {
	strncpy(dst + i, x, strlen(x));
	i += strlen(x);
      }
    }
  }
  dst[i] = 0;
  return 0;
}

static int bstrtocstr(bstring src, char *dst, unsigned int len) {
  int l;

  if (!src || src->slen == 0) {
    strcpy(dst,"");
    return 0;
  }

  l = src->slen;
  if (l > len) l = len;
  strncpy(dst, (char*)src->data, len);
  return 0;
}

/* Encode src as urlencoded and place null terminated result in dst */
static int redir_urlencode(bstring src, bstring dst) {
  char x[3];
  int n;
  
  bassigncstr(dst, "");
  for (n=0; n<src->slen; n++) {
    if ((('A' <= src->data[n]) && (src->data[n] <= 'Z')) ||
	(('a' <= src->data[n]) && (src->data[n] <= 'z')) ||
	(('0' <= src->data[n]) && (src->data[n] <= '9')) ||
	('-' == src->data[n]) ||
	('_' == src->data[n]) ||
	('.' == src->data[n]) ||
	('!' == src->data[n]) ||
	('~' == src->data[n]) ||
	('*' == src->data[n]) ||
	('\'' == src->data[n]) ||
	('(' == src->data[n]) ||
	(')' == src->data[n])) {
      bconchar(dst,src->data[n]);
    }
    else {
      snprintf(x, 3, "%.2x", src->data[n]);
      bconchar(dst, '%');
      bconchar(dst, x[0]);
      bconchar(dst, x[1]);
    }
  }
  return 0;
}

/* Decode urlencoded src and place null terminated result in dst */
static int redir_urldecode(bstring src, bstring dst) {
  char x[3];
  int n = 0;
  unsigned int c;

  bassigncstr(dst, "");
  while (n<src->slen) {
    if (src->data[n] == '%') {
      if ((n+2) < src->slen) {
	x[0] = src->data[n+1];
	x[1] = src->data[n+2];
	x[2] = 0;
	c = '_';
	sscanf(x, "%x", &c);
	bconchar(dst,c);
      }
      n += 3;
    }
    else {
      bconchar(dst,src->data[n]);
      n++;
    }
  }
  return 0;
}

/* Make an XML Reply */
static int redir_xmlreply(struct redir_t *redir, 
			  struct redir_conn_t *conn, int res, long int timeleft, char* hexchal, 
			  char* reply, char* redirurl, bstring b) {
  bstring bt;

  if (redir->no_uamwispr && 
      !(redir->chillixml)) return 0;

  bt = bfromcstr("");

  bcatcstr(b,
	   "<!--\r\n"
	   "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n");
  
  if (!redir->no_uamwispr) {
    bcatcstr(b, 
	     "<WISPAccessGatewayParam\r\n"
	     "  xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\r\n"
	     "  xsi:noNamespaceSchemaLocation=\"http://www.acmewisp.com/WISPAccessGatewayParam.xsd\""
	     ">\r\n");
    
    switch (res) {
      
    case REDIR_ALREADY:
      bcatcstr(b, 
	       "<AuthenticationPollReply>\r\n"
	       "<MessageType>140</MessageType>\r\n"
	       "<ResponseCode>102</ResponseCode>\r\n"
	       "<ReplyMessage>Already logged on</ReplyMessage>\r\n"
	       "</AuthenticationPollReply>\r\n");
      break;
      
    case REDIR_FAILED_REJECT:
      bcatcstr(b, 
	       "<AuthenticationPollReply>\r\n"
	       "<MessageType>140</MessageType>\r\n"
	       "<ResponseCode>100</ResponseCode>\r\n");
      
      if (reply) {
	bassignformat(bt, "<ReplyMessage>%s</ReplyMessage>\r\n", reply);
	bconcat(b, bt);
      }
      else {
	bcatcstr(b, "<ReplyMessage>Invalid Password</ReplyMessage>\r\n");
      }
      
      bcatcstr(b, "</AuthenticationPollReply>\r\n");
      break;
      
    case REDIR_FAILED_OTHER:
      bcatcstr(b, 
	       "<AuthenticationPollReply>\r\n"
	       "<MessageType>140</MessageType>\r\n"
	       "<ResponseCode>102</ResponseCode>\r\n");
      
      if (reply) {
	bassignformat(bt, "<ReplyMessage>%s</ReplyMessage>\r\n", reply);
	bconcat(b, bt);
      }
      else {
	bcatcstr(b, "<ReplyMessage>Radius error</ReplyMessage>\r\n");
      }
      
      bcatcstr(b, "</AuthenticationPollReply>\r\n");
      break;
      
    case REDIR_SUCCESS:
      bcatcstr(b, 
	       "<AuthenticationPollReply>\r\n"
	       "<MessageType>140</MessageType>\r\n"
	       "<ResponseCode>50</ResponseCode>\r\n");
      
      if (reply) {
	bassignformat(bt, "<ReplyMessage>%s</ReplyMessage>\r\n", reply);
	bconcat(b, bt);
      }
      
      bassignformat(bt, "<LogoffURL>http://%s:%d/logoff</LogoffURL>\r\n",
		    inet_ntoa(redir->addr), redir->port);
      bconcat(b, bt);
      
      if (redirurl) {
	bassignformat(bt, "<RedirectionURL>%s</RedirectionURL>\r\n", redirurl);
	bconcat(b, bt);
      }
      bcatcstr(b, "</AuthenticationPollReply>\r\n");
      break;
      
    case REDIR_LOGOFF:
      bcatcstr(b, 
	       "<LogoffReply>\r\n"
	       "<MessageType>130</MessageType>\r\n"
	       "<ResponseCode>150</ResponseCode>\r\n"
	       "</LogoffReply>\r\n");
      break;
      
    case REDIR_NOTYET:
      bcatcstr(b, 
	       "<Redirect>\r\n"
	       "<AccessProcedure>1.0</AccessProcedure>\r\n");

      if (redir->radiuslocationid) {
	bassignformat(bt, "<AccessLocation>%s</AccessLocation>\r\n", redir->radiuslocationid);
	bconcat(b, bt);
      }

      if (redir->radiuslocationname) {
	bassignformat(bt, "<LocationName>%s</LocationName>\r\n", redir->radiuslocationname);
	bconcat(b, bt);
      }
      
      bassignformat(bt, "<LoginURL>%s?res=smartclient&amp;uamip=%s&amp;uamport=%d&amp;challenge=%s</LoginURL>\r\n",
		    options.wisprlogin ? options.wisprlogin : redir->url, 
		    inet_ntoa(redir->addr), redir->port, hexchal); 
      bconcat(b, bt);
      
      bassignformat(bt, "<AbortLoginURL>http://%s:%d/abort</AbortLoginURL>\r\n",
		    inet_ntoa(redir->addr), redir->port);
      bconcat(b, bt);
      
      bcatcstr(b, 
	       "<MessageType>100</MessageType>\r\n"
	       "<ResponseCode>0</ResponseCode>\r\n"
	       "</Redirect>\r\n");
      break;
      
    case REDIR_ABORT_ACK:
      bcatcstr(b, 
	       "<AbortLoginReply>\r\n"
	       "<MessageType>150</MessageType>\r\n"
	       "<ResponseCode>151</ResponseCode>\r\n"
	       "</AbortLoginReply>\r\n");
      break;

    case REDIR_ABORT_NAK:
      bcatcstr(b, 
	       "<AbortLoginReply>\r\n"
	       "<MessageType>150</MessageType>\r\n"
	       "<ResponseCode>50</ResponseCode>\r\n");
      bassignformat(bt, "<LogoffURL>http://%s:%d/logoff</LogoffURL>\r\n",
		    inet_ntoa(redir->addr), redir->port);
      bconcat(b, bt);
      bcatcstr(b, "</AbortLoginReply>\r\n");
      break;

    case REDIR_STATUS:
      bcatcstr(b, 
	       "<AuthenticationPollReply>\r\n"
	       "<MessageType>140</MessageType>\r\n");
      if (conn->authenticated != 1) {
	bcatcstr(b, 
		 "<ResponseCode>150</ResponseCode>\r\n"
		 "<ReplyMessage>Not logged on</ReplyMessage>\r\n");
      } else {
	bcatcstr(b, 
		 "<ResponseCode>50</ResponseCode>\r\n"
		 "<ReplyMessage>Already logged on</ReplyMessage>\r\n");
      }
      bcatcstr(b, "</AuthenticationPollReply>\r\n");
      break;
      
    default:
      log_err(0, "Unknown res in switch");
      bdestroy(bt);
      return -1;
      
    }
    bcatcstr(b, "</WISPAccessGatewayParam>\r\n");
  }

  if (redir->chillixml) {
    bcatcstr(b, "<ChilliSpotSession>\r\n");
    switch (res) {
    case REDIR_NOTYET:
      bassignformat(bt, "<Challenge>%s</Challenge>\r\n", hexchal);
      bconcat(b, bt);
      break;
    case REDIR_STATUS:
      if (conn->authenticated == 1) {
        time_t timenow = time(0);
        uint32_t sessiontime;

        sessiontime = timenow - conn->start_time;

        bcatcstr(b, "<State>1</State>\r\n");

        bassignformat(bt, "<StartTime>%d</StartTime>\r\n" , conn->start_time);
	bconcat(b, bt);

        bassignformat(bt, "<SessionTime>%d</SessionTime>\r\n", sessiontime);
	bconcat(b, bt);

        if (timeleft) {
	  bassignformat(bt, "<TimeLeft>%d</TimeLeft>\r\n", timeleft);
	  bconcat(b, bt);
        }

        bassignformat(bt, "<Timeout>%d</Timeout>\r\n", conn->params.sessiontimeout);
	bconcat(b, bt);

        bassignformat(bt, "<InputOctets>%d</InputOctets>\r\n", conn->input_octets);
	bconcat(b, bt);

        bassignformat(bt, "<OutputOctets>%d</OutputOctets>\r\n", conn->output_octets);
	bconcat(b, bt);
	
        bassignformat(bt, "<MaxInputOctets>%d</MaxInputOctets>\r\n", conn->params.maxinputoctets);
	bconcat(b, bt);
	
        bassignformat(bt, "<MaxOutputOctets>%d</MaxOutputOctets>\r\n", conn->params.maxoutputoctets);
	bconcat(b, bt);

        bassignformat(bt, "<MaxTotalOctets>%d</MaxTotalOctets>\r\n", conn->params.maxtotaloctets);
	bconcat(b, bt);
      }
      else {
        bcatcstr(b, "<State>0</State>\r\n");
      }
      
      break;

    case REDIR_ALREADY:
      bcatcstr(b, "<Already>1</Already>\r\n");
      break;

    case REDIR_FAILED_REJECT:
    case REDIR_FAILED_OTHER:
      if (reply) {
        bassignformat(bt, "<ReplyMessage>%s</ReplyMessage>\r\n", reply);
	bconcat(b, bt);
      }
      bcatcstr(b, "<State>0</State>\r\n");

      break;
    case REDIR_SUCCESS:
      if (reply) {
        bassignformat(bt, "<ReplyMessage>%s</ReplyMessage>\r\n", reply);
	bconcat(b, bt);
      }
      bcatcstr(b, "<State>1</State>\r\n");
      break;
    case REDIR_LOGOFF:
      bcatcstr(b, "<State>0</State>\r\n");
      break;
    case REDIR_ABORT_ACK:
      bcatcstr(b, "<Abort_ack>1</Abort_ack>\r\n");
      break;
    case REDIR_ABORT_NAK:
      bcatcstr(b, "<Abort_nak>1</Abort_nak>\r\n");
      break;
    default:
      log_err(0, "Unknown res in switch");
      bdestroy(bt);
      return -1;
    }
    bcatcstr(b, "</ChilliSpotSession>\r\n");  
  }
  
  bcatcstr(b, "-->\r\n");
  bdestroy(bt);
  return 0;
}

static int redir_buildurl(struct redir_conn_t *conn, bstring str,
			  struct redir_t *redir, char *resp,
			  long int timeleft, char* hexchal, char* uid, 
			  char* userurl, char* reply, char* redirurl,
			  uint8_t *hismac, struct in_addr *hisip) {

  bstring bt = bfromcstr("");
  bstring bt2 = bfromcstr("");

  bassignformat(str, "%s?res=%s&uamip=%s&uamport=%d", 
		redir->url, resp, inet_ntoa(redir->addr), redir->port);

  if (hexchal) {
    bassignformat(bt, "&challenge=%s", hexchal);
    bconcat(str, bt);
    bassigncstr(bt,"");
  }
  
  if (conn->type == REDIR_STATUS) {
    int starttime = conn->start_time;
    if (starttime) {
      int sessiontime;
      time_t timenow = time(0);

      sessiontime = timenow - starttime;

      bassignformat(bt, "&starttime=%ld", starttime);
      bconcat(str, bt);
      bassignformat(bt, "&sessiontime=%ld", sessiontime);
      bconcat(str, bt);
    }

    if (conn->params.sessiontimeout) {
      bassignformat(bt, "&sessiontimeout=%ld", conn->params.sessiontimeout);
      bconcat(str, bt);
    }

    if (conn->params.sessionterminatetime) {
      bassignformat(bt, "&stoptime=%ld", conn->params.sessionterminatetime);
      bconcat(str, bt);
    }
  }
 
  if (uid) {
    bcatcstr(str, "&uid=");
    bassigncstr(bt, uid);
    redir_urlencode(bt, bt2);
    bconcat(str, bt2);
  }

  if (timeleft) {
    bassignformat(bt, "&timeleft=%ld", timeleft);
    bconcat(str, bt);
  }
  
  if (hismac) {
    bcatcstr(str, "&mac=");
    bassignformat(bt, "%.2X-%.2X-%.2X-%.2X-%.2X-%.2X",
		  hismac[0], hismac[1], 
		  hismac[2], hismac[3],
		  hismac[4], hismac[5]);
    redir_urlencode(bt, bt2);
    bconcat(str, bt2);
  }

  if (hisip) {
    bassignformat(bt, "&ip=%s", inet_ntoa(*hisip));
    bconcat(str, bt);
  }

  if (reply) {
    bcatcstr(str, "&reply=");
    bassigncstr(bt, reply);
    redir_urlencode(bt, bt2);
    bconcat(str, bt2);
  }

  if (redir->ssid) {
    bcatcstr(str, "&ssid=");
    bassigncstr(bt, redir->ssid);
    redir_urlencode(bt, bt2);
    bconcat(str, bt2);
  }

  if (redir->nasmac) {
    bcatcstr(str, "&called=");
    bassigncstr(bt, redir->nasmac);
    redir_urlencode(bt, bt2);
    bconcat(str, bt2);
  }

  if (redir->radiusnasid) {
    bcatcstr(str, "&nasid=");
    bassigncstr(bt, redir->radiusnasid);
    redir_urlencode(bt, bt2);
    bconcat(str, bt2);
  }

  if (redirurl) {
    bcatcstr(str, "&redirurl=");
    bassigncstr(bt, redirurl);
    redir_urlencode(bt, bt2);
    bconcat(str, bt2);
  }

  if (userurl) {
    bcatcstr(str, "&userurl=");
    bassigncstr(bt, userurl);
    redir_urlencode(bt, bt2);
    bconcat(str, bt2);
  }

  if (redir->secret && *redir->secret) { /* take the md5 of the url+uamsecret as a checksum */
    MD5_CTX context;
    unsigned char cksum[16];
    char hex[32+1];
    int i;

    MD5Init(&context);
    MD5Update(&context, (uint8_t*)str->data, str->slen);
    MD5Update(&context, (uint8_t*)redir->secret, strlen(redir->secret));
    MD5Final(cksum, &context);

    hex[0]=0;
    for (i=0; i<16; i++)
      sprintf(hex+strlen(hex), "%.2X", cksum[i]);

    bcatcstr(str, "&md=");
    bcatcstr(str, hex);
  }

  bdestroy(bt);
  bdestroy(bt2);
  return 0;
}

int 
tcp_write_timeout(int timeout, struct redir_socket *sock, char *buf, int len) {
  fd_set fdset;
  struct timeval tv;
  int fd = sock->fd[1];

  FD_ZERO(&fdset);
  FD_SET(fd,&fdset);

  tv.tv_sec = timeout;
  tv.tv_usec = 0;

  if (select(fd + 1,(fd_set *) 0,&fdset,(fd_set *) 0,&tv) == -1)
    return -1;

  if (FD_ISSET(fd, &fdset))
#if WIN32
    return send(fd,buf,len,0);
#else
    return write(fd,buf,len);
#endif

  return -1;
}

static int timeout = 10;

int
tcp_write(struct redir_socket *sock, char *buf, int len) {
  int c, r = 0;
  while (r < len) {
    c = tcp_write_timeout(timeout,sock,buf+r,len-r);
    if (c <= 0) return r;
    r += c;
  }
  return r;
}

static int redir_json_reply(struct redir_t *redir, int res, struct redir_conn_t *conn,  
			    char *hexchal, char *userurl, char *redirurl, uint8_t *hismac, 
			    char *reply, char *qs, bstring s) {
  bstring tmp = bfromcstr("");
  bstring json = bfromcstr("");

  unsigned char flg = 0;
#define FLG_cb     1
#define FLG_acct   2
#define FLG_chlg   4
#define FLG_sess   8
#define FLG_loc   16
#define FLG_redir 32

  int auth = conn->authenticated;

  redir_getparam(redir, qs, "callback", tmp);

  if (tmp->slen) {
    bconcat(json, tmp);
    bcatcstr(json, "(");
    flg |= FLG_cb;
  }
  
  switch (res) {
  case REDIR_ALREADY:
    flg |= FLG_acct;
    break;
  case REDIR_FAILED_REJECT:
  case REDIR_FAILED_OTHER:
    flg |= FLG_chlg;
    break;
  case REDIR_SUCCESS:
    flg |= FLG_acct;
    flg |= FLG_sess;
    flg |= FLG_redir;
    auth = 1;
    break;
  case REDIR_LOGOFF:
    flg |= FLG_acct | FLG_chlg;
    break;
  case REDIR_NOTYET:
    flg |= FLG_chlg;
    flg |= FLG_loc;
    flg |= FLG_redir;
    break;
  case REDIR_ABORT_ACK:
    break;
  case REDIR_ABORT_NAK:
    break;
  case REDIR_ABOUT:
    break;
  case REDIR_STATUS:
    if (conn->authenticated == 1) {
      flg |= FLG_acct;
      flg |= FLG_sess;
    } else {
      flg |= FLG_chlg;
      flg |= FLG_loc;
    }
    flg |= FLG_redir;
    break;
  default:
    break;
  }

  bcatcstr(json, "{\"version\":\"1.0\",\"clientState\":");

  bassignformat(tmp, "%d", auth);
  bconcat(json, tmp);

  if (auth == 1) {
    bcatcstr(json,",\"sessionId\":\"");
    bcatcstr(json,conn->sessionid);
    bcatcstr(json,"\"");
  }

  if (reply) {
    bcatcstr(json, ",\"message\":\"");
    bcatcstr(json, reply);
    bcatcstr(json, "\"");
  }

  if (flg & FLG_chlg && hexchal) {
      bcatcstr(json, ",\"challenge\":\"");
      bcatcstr(json, hexchal);
      bcatcstr(json, "\"");
  }

  if (flg & FLG_loc) {
    bcatcstr(json,",\"location\":{\"name\":\"");
    if (redir->locationname)
      bcatcstr(json, redir->locationname);
    else if (redir->radiuslocationname)
      bcatcstr(json, redir->radiuslocationname);
    bcatcstr(json,"\"");
    bcatcstr(json,"}");
  }

  if (flg & FLG_redir) {
    bcatcstr(json,",\"redir\":{\"originalURL\":\"");
    bcatcstr(json, userurl?userurl:"");
    bcatcstr(json,"\",\"redirectionURL\":\"");
    bcatcstr(json, redirurl?redirurl:"");
    bcatcstr(json,"\",\"macAddress\":\"");
    if (hismac) {
      char mac[REDIR_MACSTRLEN+2];
      snprintf(mac, REDIR_MACSTRLEN+1, "%.2X-%.2X-%.2X-%.2X-%.2X-%.2X",
	       hismac[0], hismac[1],
	       hismac[2], hismac[3],
	       hismac[4], hismac[5]);
      bcatcstr(json, mac);
    }
    bcatcstr(json,"\"}");
  }


  if (flg & FLG_acct || flg & FLG_sess) {
    time_t starttime = conn->start_time;
    uint32_t inoctets = conn->input_octets;
    uint32_t outoctets = conn->output_octets;
    uint32_t ingigawords = (conn->input_octets >> 32);
    uint32_t outgigawords = (conn->output_octets >> 32);
    time_t timenow = time(0);
    uint32_t sessiontime;
    uint32_t idletime;

    sessiontime = timenow - conn->start_time;
    idletime    = timenow - conn->last_time;

    switch (res) {
    case REDIR_SUCCESS:
      /* they haven't be set yet in conn */
      starttime = time(0);
      inoctets = outoctets = 0;
      ingigawords = outgigawords = 0;
      sessiontime=0; 
      idletime=0;
      break;
    default:
      {
      }
      break;
    }

    if (flg & FLG_sess) {
      bcatcstr(json,",\"session\":{\"startTime\":");
      bassignformat(tmp, "%ld", starttime);
      bconcat(json, tmp);
      bcatcstr(json,",\"sessionTimeout\":");
      bassignformat(tmp, "%ld", conn->params.sessiontimeout);
      bconcat(json, tmp);
      bcatcstr(json,",\"idleTimeout\":");
      bassignformat(tmp, "%ld", conn->params.idletimeout);
      bconcat(json, tmp);
      bcatcstr(json,"}");
    }

    if (flg & FLG_acct) {
      bcatcstr(json,",\"accounting\":{\"sessionTime\":");
      bassignformat(tmp, "%ld", sessiontime);
      bconcat(json, tmp);
      bcatcstr(json,",\"idleTime\":");
      bassignformat(tmp, "%ld", idletime);
      bconcat(json, tmp);
      bcatcstr(json,",\"inputOctets\":");
      bassignformat(tmp, "%ld", inoctets);
      bconcat(json, tmp);
      bcatcstr(json,",\"outputOctets\":");
      bassignformat(tmp, "%ld", outoctets);
      bconcat(json, tmp);
      bcatcstr(json,",\"inputGigawords\":");
      bassignformat(tmp, "%ld", ingigawords);
      bconcat(json, tmp);
      bcatcstr(json,",\"outputGigawords\":");
      bassignformat(tmp, "%ld", outgigawords);
      bconcat(json, tmp);
      bcatcstr(json,"}");
    }
  }

  bcatcstr(json, "}");

  if (flg & FLG_cb) {
    bcatcstr(json, ")");
  }

  bassigncstr(s, "HTTP/1.1 200 OK\r\n");
  bcatcstr(s, "Cache-Control: no-cache, must-revalidate\r\n");

  bcatcstr(s, "Content-Length: ");
  bassignformat(tmp , "%ld", blength(json) );
  bconcat(s, tmp);

  bcatcstr(s, "\r\nContent-type: ");
  if (tmp->slen) bcatcstr(s, "text/javascript");
  else bcatcstr(s, "application/json");

  bcatcstr(s, "\r\n\r\n");
  bconcat(s, json);

  if (options.debug) {
    log_dbg("sending json: %s\n", json->data);
  }

  bdestroy(json);
  bdestroy(tmp);

  return 0;
}

/* Make an HTTP redirection reply and send it to the client */
static int redir_reply(struct redir_t *redir, struct redir_socket *sock, 
		       struct redir_conn_t *conn, int res, bstring url,
		       long int timeleft, char* hexchal, char* uid, 
		       char* userurl, char* reply, char* redirurl,
		       uint8_t *hismac, struct in_addr *hisip, char *qs) {

  char *resp = NULL;
  bstring buffer;

  switch (res) {
  case REDIR_ALREADY:
    resp = "already";
    break;
  case REDIR_FAILED_REJECT:
  case REDIR_FAILED_OTHER:
    resp = "failed";
    break;
  case REDIR_SUCCESS:
    resp = "success";
    break;
  case REDIR_LOGOFF:
    resp = "logoff";
    break;
  case REDIR_NOTYET:
    resp = "notyet";
    break;
  case REDIR_ABORT_ACK:
    resp = "logoff";
    break;
  case REDIR_ABORT_NAK:
    resp = "already";
    break;
  case REDIR_ABOUT:
    break;
  case REDIR_STATUS:
    resp = conn->authenticated == 1 ? "already" : "notyet";
    break;
  default:
    log_err(0, "Unknown res in switch");
    return -1;
  }

  buffer = bfromcstralloc(1024, "");

  if (conn->format == REDIR_FMT_JSON) {

    redir_json_reply(redir, res, conn, hexchal, userurl, redirurl, hismac, reply, qs, buffer);
    
  } else if (resp) {
    bcatcstr(buffer, "HTTP/1.0 302 Moved Temporarily\r\nLocation: ");
    
    if (url) {
      bconcat(buffer, url);
    } else {
      bstring bt = bfromcstralloc(1024,"");
      if (redir_buildurl(conn, bt, redir, resp, timeleft, hexchal, 
			 uid, userurl, reply, redirurl, hismac, hisip) == -1) {
	bdestroy(bt);
	bdestroy(buffer);
	return -1;
      }
      log_dbg("here: %s\n", bt->data);
      bconcat(buffer, bt);
      bdestroy(bt);
    }
    
    bcatcstr(buffer, 
	     "\r\n\r\n<HTML><BODY><H2>Browser error!</H2>"
	     "Browser does not support redirects!</BODY>\r\n");
    
    redir_xmlreply(redir, conn, res, timeleft, hexchal, reply, redirurl, buffer);
    
    bcatcstr(buffer, "\r\n</HTML>\r\n");
    
  } else {
    bassigncstr(buffer, 
		"HTTP/1.0 200 OK\r\nContent-type: text/html\r\n\r\n"
		"<HTML><HEAD><TITLE>(Coova-)ChilliSpot</TITLE></HEAD><BODY>");
    bcatcstr(buffer, credits);
    bcatcstr(buffer, "</BODY></HTML>\r\n");
  }

  if (tcp_write(sock, (char*)buffer->data, buffer->slen) < 0) {
    log_err(errno, "tcp_write() failed!");
    bdestroy(buffer);
    return -1;
  }

  /*XXX:FLASH    
  if (strstr(conn->useragent, "Flash")) {
    char *c = "";
    if (tcp_write(sock, c, 1) < 0) {
      log_err(errno, "tcp_write() failed!");
    }
    }*/
  
  bdestroy(buffer);
  return 0;
}

/* Allocate new instance of redir */
int redir_new(struct redir_t **redir,
	      struct in_addr *addr, int port, int uiport) {
  struct sockaddr_in address;
  int optval = 1;
  int n = 0;

  if (!(*redir = calloc(1, sizeof(struct redir_t)))) {
    log_err(errno, "calloc() failed");
    return EOF;
  }

  (*redir)->addr = *addr;
  (*redir)->port = port;
  (*redir)->uiport = uiport;
  (*redir)->starttime = 0;
  
  if (((*redir)->fd[0] = socket(AF_INET ,SOCK_STREAM ,0)) < 0) {
    log_err(errno, "socket() failed");
    return -1;
  }

  if (uiport && ((*redir)->fd[1] = socket(AF_INET ,SOCK_STREAM ,0)) < 0) {
    log_err(errno, "socket() failed");
    return -1;
  }

  /* TODO: FreeBSD
  if (setsockopt((*redir)->fd, SOL_SOCKET, SO_REUSEPORT,
		 &optval, sizeof(optval))) {
    log_err(errno, "setsockopt() failed");
    close((*redir)->fd);
    return -1;
  }
  */

  /* Set up address */
  address.sin_family = AF_INET;
#if defined(__FreeBSD__) || defined (__APPLE__) || defined (__OpenBSD__) || defined (__NetBSD__)
  address.sin_len = sizeof (struct sockaddr_in);
#endif

  for (n = 0; n < 2 && (*redir)->fd[n]; n++) {

    switch(n) {
    case 0:
      address.sin_addr.s_addr = addr->s_addr;
      address.sin_port = htons(port);
      break;
    case 1:
      /* XXX: binding to 0.0.0.0:uiport (should be configurable?) */
      address.sin_addr.s_addr = INADDR_ANY;
      address.sin_port = htons(uiport);
      break;
    }

    if (setsockopt((*redir)->fd[n], SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))) {
      log_err(errno, "setsockopt() failed");
      close((*redir)->fd[n]);
      (*redir)->fd[n]=0;
      break;
    }
    while (bind((*redir)->fd[n], (struct sockaddr *)&address, sizeof(address))) {
      if ((EADDRINUSE == errno) && (10 > n++)) {
	log_warn(0, "UAM port already in use. Waiting for retry.");
	if (sleep(30)) { /* In case we got killed */
	  close((*redir)->fd[n]);
	  (*redir)->fd[n]=0;
	  break;
	}
      }
      else {
	log_err(errno, "bind() failed");
	close((*redir)->fd[n]);
	(*redir)->fd[n]=0;
	break;
      }
    }
    if (listen((*redir)->fd[n], REDIR_MAXLISTEN)) {
      log_err(errno, "listen() failed");
      close((*redir)->fd[n]);
      (*redir)->fd[n]=0;
      break;
    }
  }
  
  if (((*redir)->msgid = msgget(IPC_PRIVATE, 0)) < 0) {
    log_err(errno, "msgget() failed");
    log_err(0, "Most likely your computer does not have System V IPC installed");
    return -1;
  }
  
  return 0;
}


/* Free instance of redir */
int redir_free(struct redir_t *redir) {
  int n;
  for (n = 0; n < 2 && redir->fd[n]; n++) {
    if (close(redir->fd[n])) {
      log_err(errno, "close() failed");
    }
  }

  if (msgctl(redir->msgid, IPC_RMID, NULL)) {
    log_err(errno, "msgctl() failed");
  }
  
  free(redir);
  return 0;
}

/* Set redir parameters */
void redir_set(struct redir_t *redir, int debug) { 
  optionsdebug = debug; /* TODO: Do not change static variable from instance */
  redir->debug = debug;
  redir->no_uamsuccess = options.no_uamsuccess;
  redir->no_uamwispr = options.no_uamwispr;
  redir->chillixml = options.chillixml;
  redir->url = options.uamurl;
  redir->homepage = options.uamhomepage;
  redir->secret = options.uamsecret;
  redir->ssid = options.ssid;
  redir->nasmac = options.nasmac;
  redir->nasip = options.nasip;
  redir->radiusserver0 = options.radiusserver1;
  redir->radiusserver1 = options.radiusserver2;
  redir->radiusauthport = options.radiusauthport;
  redir->radiusacctport = options.radiusacctport;
  redir->radiussecret  = options.radiussecret;
  redir->radiusnasid  = options.radiusnasid;
  redir->radiuslocationid  = options.radiuslocationid;
  redir->radiuslocationname  = options.radiuslocationname;
  redir->locationname  = options.locationname;
  redir->radiusnasporttype = options.radiusnasporttype;
  return;
}

/* Get a parameter of an HTTP request. Parameter is url decoded */
/* TODO: Should be merged with other parsers */
static int redir_getparam(struct redir_t *redir, char *src, char *param, bstring dst) {
  char *p1;
  char *p2;
  char sstr[255];
  int len = 0;

  strncpy(sstr, param, sizeof(sstr));
  sstr[sizeof(sstr)-1] = 0;
  strncat(sstr, "=", sizeof(sstr));
  sstr[sizeof(sstr)-1] = 0;

  if (!(p1 = strcasestr(src, sstr))) return -1;
  p1 += strlen(sstr);

  /* The parameter ends with a & or null */
  p2 = strstr(p1, "&");

  if (p2) len = p2 - p1;
  else len = strlen(p1);

  if (len) {
    bstring s = blk2bstr(p1, len);
    redir_urldecode(s, dst);
    bdestroy(s);
  } else 
    bassigncstr(dst, "");

  log_dbg("The parameter %s is: [%.*s]", param, dst->slen, dst->data);/**/

  return 0;
}

/* Read the an HTTP request from a client */
/* If POST is allowed, 1 is the input value of ispost */
static int redir_getreq(struct redir_t *redir, struct redir_socket *sock,
			struct redir_conn_t *conn, int *ispost, int *clen,
			char *qs, int qslen) {
  int fd = sock->fd[0];
  fd_set fds;
  struct timeval idleTime;
  int status;
  int recvlen = 0;
  int buflen = 0;
  char buffer[REDIR_MAXBUFFER];
  char host[256];
  char path[256];
  int i, lines=0, done=0;
  char *eol;

  memset(buffer, 0, sizeof(buffer));
  memset(host,   0, sizeof(host));
  memset(path,   0, sizeof(path));
  
  /* read whatever the client send to us */
  while (!done && (redir->starttime + REDIR_HTTP_MAX_TIME) > time(NULL)) {
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    idleTime.tv_sec = 0;
    idleTime.tv_usec = REDIR_HTTP_SELECT_TIME;

    switch (status = select(fd + 1, &fds, NULL, NULL, &idleTime)) {
    case -1:
      log_err(errno,"select() returned -1!");
      return -1;
    case 0:
      break; 
    default:
      break;
    }

    if ((status > 0) && FD_ISSET(fd, &fds)) {
      if (buflen + 2 >= sizeof(buffer)) { /* ensure space for a least one more byte + null */
        log_err(errno, "Too much data in http request!");
        return -1;
      }
      /* if post is allowed, we do not buffer on the read (to not eat post data) */
      if ((recvlen = recv(fd, buffer+buflen, (*ispost) ? 1 : sizeof(buffer)-1-buflen, 0)) < 0) {
	if (errno != ECONNRESET)
	  log_err(errno, "recv() failed!");
	return -1;
      }

      /* TODO: Hack to make Flash work 
      for (i = 0; i < recvlen; i++) 
	if (buffer[buflen+i] == 0) 
	  buffer[buflen+i] = 0x0a; 
      */

      buflen += recvlen;
      buffer[buflen] = 0;
    }

    if (buflen <= 0) {
      if (optionsdebug) 
	log_dbg("No HTTP request received!");
      return -1;
    }

    while ((eol = strstr(buffer, "\r\n"))) {
      int linelen = eol - buffer;
      *eol = 0;

      if (lines++ == 0) { /* first line */
	char *p1 = buffer;
	char *p2;
	int dstlen = 0;

	if (optionsdebug)
	  log_dbg("http-request: %s", buffer);

	if      (!strncmp("GET ",  p1, 4)) { p1 += 4; *ispost = 0; }
	else if (!strncmp("HEAD ", p1, 5)) { p1 += 5; *ispost = 0; }
	else if ((*ispost) && 
		 !strncmp("POST ", p1, 5)) { p1 += 5; *ispost = 1; }
	else { 
	  if (optionsdebug)
	    log_dbg("Unhandled http request: %s", buffer);
	  return -1;
	}

	while (*p1 == ' ') p1++; /* Advance through additional white space */
	if (*p1 == '/') p1++;
	else return -1;
	
	/* The path ends with a ? or a space */
	p2 = strchr(p1, '?');
	if (!p2) p2 = strchr(p1, ' ');
	if (!p2) return -1;
	dstlen = p2 - p1;

	if (dstlen >= sizeof(path)-1) 
	  dstlen = sizeof(path)-1;

	strncpy(path, p1, dstlen);
	if (optionsdebug)
	  log_dbg("The path: %s", path); 

	/* TODO: Should also check the Host: to make sure we are talking directly to uamlisten */

	if (!strncmp(path, "json/", 5) && strlen(path) > 6) {
	  int i, last=strlen(path)-5;
	  conn->format = REDIR_FMT_JSON;
	  for (i=0; i < last; i++)
	    path[i] = path[i+5];
	  path[last]=0;
	  log_dbg("The (json format) path: %s", path); 
	}

	if ((!strcmp(path, "logon")) || (!strcmp(path, "login")))
	  conn->type = REDIR_LOGIN;
	else if ((!strcmp(path, "logoff")) || (!strcmp(path, "logout")))
	  conn->type = REDIR_LOGOUT;
	else if (!strncmp(path, "www/", 4) && strlen(path) > 4)
	  conn->type = REDIR_WWW;
	else if (!strcmp(path, "status"))
	  conn->type = REDIR_STATUS;
	else if (!strncmp(path, "msdownload", 10))
	  { conn->type = REDIR_MSDOWNLOAD; return 0; }
	else if (!strcmp(path, "prelogin"))
	  { conn->type = REDIR_PRELOGIN; return 0; }
	else if (!strcmp(path, "abort"))
	  { conn->type = REDIR_ABORT; return 0; }

	if (*p2 == '?') {
	  p1 = p2 + 1;
	  p2 = strchr(p1, ' ');
	  if (p2) {
	    dstlen = p2 - p1;
	    if (dstlen >= qslen-1) 
	      dstlen = qslen-1;
	    strncpy(qs, p1, dstlen);
	    if (optionsdebug)
	      log_dbg("Query string: %s", qs); 
	  }
	}
      } else if (linelen == 0) { 
	/* end of headers */
	/*log_dbg("end of http-request");*/
	done = 1;
	break;
      } else { 
	/* headers */
	char *p;
	int len;

	if (!strncasecmp(buffer,"Host:",5)) {
	  p = buffer + 5;
	  while (*p && isspace(*p)) p++;
	  len = strlen(p);
	  if (len >= sizeof(host)-1)
	    len = sizeof(host)-1;
	  strncpy(host, p, len);
	  host[len]=0;
	  if (optionsdebug)
	    log_dbg("Host: %s",host);
	} 
	else if (!strncasecmp(buffer,"Content-Length:",15)) {
	  p = buffer + 15;
	  while (*p && isspace(*p)) p++;
	  len = strlen(p);
	  if (len > 0) *clen = atoi(p);
	  if (optionsdebug)
	    log_dbg("Content-Length: %s",p);
	}
	else if (!strncasecmp(buffer,"User-Agent:",11)) {
	  p = buffer + 11;
	  while (*p && isspace(*p)) p++;
	  len = strlen(p);
	  if (len >= sizeof(conn->useragent)-1)
	    len = sizeof(conn->useragent)-1;
	  strncpy(conn->useragent, p, len);
	  conn->useragent[len]=0;
	  if (optionsdebug)
	    log_dbg("User-Agent: %s",conn->useragent);
	}
      }

      /* shift buffer */
      linelen += 2;
      for (i=0; i < buflen - linelen; i++)
	buffer[i] = buffer[linelen+i];
      buflen -= linelen;
    }
  }

  switch(conn->type) {

  case REDIR_STATUS:
    return 0;

  case REDIR_LOGIN:
    {
      bstring bt = bfromcstr("");

      if (!redir_getparam(redir, qs, "lang", bt))
	bstrtocstr(bt, conn->lang, sizeof(conn->lang));
      
      if (!redir_getparam(redir, qs, "ident", bt) && bt->slen)
	conn->chap_ident = atoi((char*)bt->data);
      
      if (redir_getparam(redir, qs, "username", bt)) {
	log_err(0, "No username found in login request");
	bdestroy(bt);
	return -1;
      }

      bstrtocstr(bt, conn->username, sizeof(conn->username));
      
      if (!redir_getparam(redir, qs, "userurl", bt)) {
	bstring bt2 = bfromcstr("");
	redir_urldecode(bt, bt2);
	bstrtocstr(bt2, conn->userurl, sizeof(conn->userurl));
	if (optionsdebug) 
	  log_dbg("-->> Setting userurl=[%s]",conn->userurl);
	bdestroy(bt2);
      }
      
      if (!redir_getparam(redir, qs, "response", bt)) {
	redir_hextochar(bt->data, conn->chappassword);
	conn->chap = 1;
	conn->password[0] = 0;
      }
      else if (!redir_getparam(redir, qs, "password", bt)) {
	redir_hextochar(bt->data, conn->password);
	conn->chap = 0;
	conn->chappassword[0] = 0;
      } else {
	if (optionsdebug) 
	  log_dbg("No password found!");
	bdestroy(bt);
	return -1;
      }
      bdestroy(bt);
    }
    break;

  case REDIR_LOGOUT:
  case REDIR_PRELOGIN:
    {
      bstring bt = bfromcstr("");
      if (!redir_getparam(redir, qs, "userurl", bt)) {
	bstring bt2 = bfromcstr("");
	redir_urldecode(bt, bt2);
	bstrtocstr(bt2, conn->userurl, sizeof(conn->userurl));
	if (optionsdebug) 
	  log_dbg("-->> Setting userurl=[%s]",conn->userurl);
	bdestroy(bt2);
      }
      bdestroy(bt);
    } 
    break;

  case REDIR_WWW:
    {
      bstring bt = bfromcstr(path+4);
      bstring bt2 = bfromcstr("");
      redir_urldecode(bt, bt2);
      bstrtocstr(bt2,conn->wwwfile, sizeof(conn->wwwfile));
      if (optionsdebug) 
	log_dbg("Serving file %s", conn->wwwfile);
      bdestroy(bt2);
      bdestroy(bt);
    } 
    break;

  default:
    {
      /* some basic checks for urls we don't care about */
      
      snprintf(conn->userurl, sizeof(conn->userurl), "http://%s/%s%s%s", 
	       host, path, qs[0] ? "?" : "", qs[0] ? qs : "");

      if (optionsdebug) 
	log_dbg("-->> Setting userurl=[%s]",conn->userurl);
    }
    break;

  }

  return 0;
}

/* Radius callback when access accept/reject/challenge has been received */
static int redir_cb_radius_auth_conf(struct radius_t *radius,
				     struct radius_packet_t *pack,
				     struct radius_packet_t *pack_req, void *cbp) {
  struct redir_conn_t *conn = (struct redir_conn_t*) cbp;
  struct radius_attr_t *stateattr = NULL;
  struct radius_attr_t *classattr = NULL;
  struct radius_attr_t *attr = NULL;
  char attrs[RADIUS_ATTR_VLEN+1];

  if (optionsdebug)
    log_dbg("Received access request confirmation from radius server\n");
  
  if (!conn) {
    log_err(0, "No peer protocol defined");
    conn->response = REDIR_FAILED_OTHER;
    return 0;
  }
  
  if (!pack) { /* Timeout */
    log_err(0, "Radius request timed out");
    conn->response = REDIR_FAILED_OTHER;
    return 0;
  }

  /* We expect ACCESS-ACCEPT, ACCESS-REJECT (or ACCESS-CHALLENGE) */
  if ((pack->code != RADIUS_CODE_ACCESS_REJECT) && 
      (pack->code != RADIUS_CODE_ACCESS_CHALLENGE) &&
      (pack->code != RADIUS_CODE_ACCESS_ACCEPT)) {
    log_err(0, "Unknown radius access reply code %d", pack->code);
    return 0;
  }

  /* Reply message (might be present in both ACCESS-ACCEPT and ACCESS-REJECT */
  if (!radius_getattr(pack, &attr, RADIUS_ATTR_REPLY_MESSAGE, 0, 0, 0)) {
    memcpy(conn->replybuf, attr->v.t, attr->l-2);
    conn->replybuf[attr->l-2] = 0;
    conn->reply = conn->replybuf;
  }
  else {
    conn->replybuf[0] = 0;
    conn->reply = NULL;
  }

  config_radius_session(&conn->params, pack, 0);
  
  /* Class */
  if (!radius_getattr(pack, &classattr, RADIUS_ATTR_CLASS, 0, 0, 0)) {
    conn->classlen = classattr->l-2;
    memcpy(conn->classbuf, classattr->v.t, classattr->l-2);
  }
  else {
    conn->classlen = 0;
  }

  if (pack->code != RADIUS_CODE_ACCESS_ACCEPT) {
    /* ACCESS-REJECT */
    conn->response = REDIR_FAILED_REJECT;
    return 0;
  }

  /* ACCESS-ACCEPT */

  /* State */
  if (!radius_getattr(pack, &stateattr, RADIUS_ATTR_STATE, 0, 0, 0)) {
    conn->statelen = stateattr->l-2;
    memcpy(conn->statebuf, stateattr->v.t, stateattr->l-2);
  }
  else {
    conn->statelen = 0;
  }
  

  if (conn->params.sessionterminatetime) {
    time_t timenow = time(0);
    if (timenow > conn->params.sessionterminatetime) {
      conn->response = REDIR_FAILED_OTHER;
      log_warn(0, "WISPr-Session-Terminate-Time in the past received: %s", attrs);
    }
  }
  
  conn->response = REDIR_SUCCESS;
  return 0;
  
}

/* Send radius Access-Request and wait for answer */
static int redir_radius(struct redir_t *redir, struct in_addr *addr,
			struct redir_conn_t *conn) {
  unsigned char chap_password[REDIR_MD5LEN + 2];
  unsigned char chap_challenge[REDIR_MD5LEN];
  unsigned char user_password[REDIR_MD5LEN+1];
  struct radius_packet_t radius_pack;
  struct radius_t *radius;      /* Radius client instance */
  struct timeval idleTime;	/* How long to select() */
  int endtime, now;             /* for radius wait */
  int maxfd = 0;	        /* For select() */
  fd_set fds;			/* For select() */
  int status;

  MD5_CTX context;

  char mac[REDIR_MACSTRLEN+1];
  char url[REDIR_URL_LEN];
  int n;

  if (radius_new(&radius,
		 &redir->radiuslisten, 0, 0,
		 NULL, 0, NULL, NULL, NULL)) {
    log_err(0, "Failed to create radius");
    return -1;
  }

  radius->next = redir_radius_id;

  if (radius->fd > maxfd)
    maxfd = radius->fd;

  radius_set(radius, dhcp ? dhcp->hwaddr : 0, (options.debug & DEBUG_RADIUS));
  
  radius_set_cb_auth_conf(radius, redir_cb_radius_auth_conf);

  radius_default_pack(radius, &radius_pack, RADIUS_CODE_ACCESS_REQUEST);
  
  if (optionsdebug) 
    log_dbg("created radius packet (code=%d, id=%d, len=%d)\n",
	    radius_pack.code, radius_pack.id, ntohs(radius_pack.length));
  
  radius_addattr(radius, &radius_pack, RADIUS_ATTR_USER_NAME, 0, 0, 0,
		 (uint8_t*) conn->username, strlen(conn->username));

  /* If lang on logon url, then send it with attribute ChilliSpot-Lang */
  if(conn->lang[0]) 
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC, 
		   RADIUS_VENDOR_CHILLISPOT, RADIUS_ATTR_CHILLISPOT_LANG, 
		   0, (uint8_t*) conn->lang, strlen(conn->lang));

  if (redir->secret && *redir->secret) {
    fprintf(stderr,"SECRET: [%s]\n",redir->secret);
    /* Get MD5 hash on challenge and uamsecret */
    MD5Init(&context);
    MD5Update(&context, conn->uamchal, REDIR_MD5LEN);
    MD5Update(&context, (uint8_t*) redir->secret, strlen(redir->secret));
    MD5Final(chap_challenge, &context);
  }
  else {
    memcpy(chap_challenge, conn->uamchal, REDIR_MD5LEN);
  }
  
  if (conn->chap == 0) {
    for (n=0; n<REDIR_MD5LEN; n++) 
      user_password[n] = conn->password[n] ^ chap_challenge[n];
    user_password[REDIR_MD5LEN] = 0;
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_USER_PASSWORD, 0, 0, 0,
		   (uint8_t*)user_password, strlen((char*)user_password));
  }
  else if (conn->chap == 1) {
    chap_password[0] = conn->chap_ident; /* Chap ident found on logon url */
    memcpy(chap_password+1, conn->chappassword, REDIR_MD5LEN);
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_CHAP_CHALLENGE, 0, 0, 0,
		   chap_challenge, REDIR_MD5LEN);
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_CHAP_PASSWORD, 0, 0, 0,
		   chap_password, REDIR_MD5LEN+1);
  }

  radius_addnasip(radius, &radius_pack);

  radius_addattr(radius, &radius_pack, RADIUS_ATTR_SERVICE_TYPE, 0, 0,
		 RADIUS_SERVICE_TYPE_LOGIN, NULL, 0); /* WISPr_V1.0 */

  radius_addattr(radius, &radius_pack, RADIUS_ATTR_FRAMED_IP_ADDRESS, 0, 0,
		 ntohl(conn->hisip.s_addr), NULL, 0); /* WISPr_V1.0 */

  /* Include his MAC address */
  snprintf(mac, REDIR_MACSTRLEN+1, "%.2X-%.2X-%.2X-%.2X-%.2X-%.2X",
	   conn->hismac[0], conn->hismac[1],
	   conn->hismac[2], conn->hismac[3],
	   conn->hismac[4], conn->hismac[5]);
  
  radius_addattr(radius, &radius_pack, RADIUS_ATTR_CALLING_STATION_ID, 0, 0, 0,
		 (uint8_t*) mac, REDIR_MACSTRLEN);

  radius_addcalledstation(radius, &radius_pack);


  if (redir->radiusnasid)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_NAS_IDENTIFIER, 0, 0, 0,
		   (uint8_t*) redir->radiusnasid, 
		   strlen(redir->radiusnasid)); /* WISPr_V1.0 */


  radius_addattr(radius, &radius_pack, RADIUS_ATTR_ACCT_SESSION_ID, 0, 0, 0,
		 (uint8_t*) conn->sessionid, REDIR_SESSIONID_LEN-1);

  if (conn->classlen) {
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_CLASS, 0, 0, 0,
		   conn->classbuf,
		   conn->classlen);
  }

  radius_addattr(radius, &radius_pack, RADIUS_ATTR_NAS_PORT_TYPE, 0, 0,
		 redir->radiusnasporttype, NULL, 0);

  radius_addattr(radius, &radius_pack, RADIUS_ATTR_NAS_PORT, 0, 0,
		 conn->nasport, NULL, 0);
  
  if (redir->radiuslocationid)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
		   RADIUS_VENDOR_WISPR, RADIUS_ATTR_WISPR_LOCATION_ID, 0,
		   (uint8_t*) redir->radiuslocationid,
		   strlen(redir->radiuslocationid));

  if (redir->radiuslocationname)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
		   RADIUS_VENDOR_WISPR, RADIUS_ATTR_WISPR_LOCATION_NAME, 0,
		   (uint8_t*) redir->radiuslocationname, 
		   strlen(redir->radiuslocationname));

  if (snprintf(url, sizeof(url)-1, "http://%s:%d/logoff", 
	       inet_ntoa(redir->addr), redir->port) > 0)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
		   RADIUS_VENDOR_WISPR, RADIUS_ATTR_WISPR_LOGOFF_URL, 0,
		   (uint8_t*)url, strlen(url));

  if (options.openidauth)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
		   RADIUS_VENDOR_CHILLISPOT, RADIUS_ATTR_CHILLISPOT_CONFIG, 
		   0, (uint8_t*)"allow-openidauth", 16);
  
  radius_addattr(radius, &radius_pack, RADIUS_ATTR_MESSAGE_AUTHENTICATOR, 
		 0, 0, 0, NULL, RADIUS_MD5LEN);

  if (optionsdebug) 
    log_dbg("sending radius packet (code=%d, id=%d, len=%d)\n",
	    radius_pack.code, radius_pack.id, ntohs(radius_pack.length));

  radius_req(radius, &radius_pack, conn);

  now = time(NULL);
  endtime = now + REDIR_RADIUS_MAX_TIME;

  while (endtime > now) {

    FD_ZERO(&fds);
    if (radius->fd != -1) FD_SET(radius->fd, &fds);
    if (radius->proxyfd != -1) FD_SET(radius->proxyfd, &fds);
    
    idleTime.tv_sec = 0;
    idleTime.tv_usec = REDIR_RADIUS_SELECT_TIME;
    radius_timeleft(radius, &idleTime);

    switch (status = select(maxfd + 1, &fds, NULL, NULL, &idleTime)) {
    case -1:
      log_err(errno, "select() returned -1!");
      break;  
    case 0:
      /*log_dbg("Select returned 0");*/
      radius_timeout(radius);
      break; 
    default:
      break;
    }

    if (status > 0) {
      if ((radius->fd != -1) && FD_ISSET(radius->fd, &fds) && 
	  radius_decaps(radius) < 0) {
	log_err(0, "radius_ind() failed!");
      }
      
      if ((radius->proxyfd != -1) && FD_ISSET(radius->proxyfd, &fds) && 
	  radius_proxy_ind(radius) < 0) {
	log_err(0, "radius_proxy_ind() failed!");
      }
    }
  
    if (conn->response) {
      radius_free(radius);
      return 0;
    }

    now = time(NULL);
  }

  return 0;
}

int set_nonblocking(int fd)
{
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0) return -1;
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  return 0;
}

int clear_nonblocking(int fd)
{
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0) return -1;
  fcntl(fd, F_SETFL, flags & (~O_NONBLOCK));
  return 0;
}

int is_local_user(struct redir_t *redir, struct redir_conn_t *conn) {
  unsigned char user_password[REDIR_MD5LEN+1];
  unsigned char chap_challenge[REDIR_MD5LEN];
  unsigned char tmp[REDIR_MD5LEN+1];
  char u[256]; char p[256];
  size_t sz=1024;
  int len, match=0;
  char *line=0;
  MD5_CTX context;
  FILE *f;

  if (!options.localusers) return 0;

  log_dbg("checking %s for user %s", options.localusers, conn->username);

  if (!(f = fopen(options.localusers, "r"))) {
    log_err(errno, "fopen() failed opening %s!", options.localusers);
    return 0;
  }

  if (options.debug) {/*debug*/
    char buffer[64];
    redir_chartohex(conn->uamchal, buffer);
    log_dbg("challenge: %s", buffer);
  }/**/

  if (redir->secret && *redir->secret) {
    MD5Init(&context);
    MD5Update(&context, (uint8_t*)conn->uamchal, REDIR_MD5LEN);
    MD5Update(&context, (uint8_t*)redir->secret, strlen(redir->secret));
    MD5Final(chap_challenge, &context);
  }
  else {
    memcpy(chap_challenge, conn->uamchal, REDIR_MD5LEN);
  }

  if (options.debug) {/*debug*/
    char buffer[64];
    redir_chartohex(chap_challenge, buffer);
    log_dbg("chap challenge: %s", buffer);
  }/**/

  if (conn->chap == 0) {
    int n;
    for (n=0; n < REDIR_MD5LEN; n++)
      user_password[n] = conn->password[n] ^ chap_challenge[n];
  }
  else if (conn->chap == 1) {
    memcpy(user_password, conn->chappassword, REDIR_MD5LEN);
  }
  
  user_password[REDIR_MD5LEN] = 0;
	
  log_dbg("looking for %s", conn->username);

  line=(char*)malloc(sz);
  while ((len = getline(&line, &sz, f)) >= 0) {
    if (len > 3 && len < sizeof(u) && line[0] != '#') {
      char *pl=line, *pu=u, *pp=p;
      while (*pl && *pl != ':') *pu++ = *pl++;
      if (*pl == ':') pl++;
      while (*pl && *pl != ':' && *pl != '\n') *pp++ = *pl++;
      *pu = 0; *pp = 0;

      if (!strcmp(conn->username, u)) {

	log_dbg("found %s, checking password", u);

	if (conn->chap == 0) {
	  int n;
	  for (n=0; n < REDIR_MD5LEN; n++)
	    tmp[n] = p[n] ^ chap_challenge[n];
	}
	else if (conn->chap == 1) {
	  MD5Init(&context);
	  MD5Update(&context, (uint8_t*)&conn->chap_ident, 1);	  
	  MD5Update(&context, (uint8_t*)p, strlen(p));
	  MD5Update(&context, chap_challenge, REDIR_MD5LEN);
	  MD5Final(tmp, &context);
	}

	tmp[REDIR_MD5LEN] = 0;

	if (!memcmp(user_password, tmp, REDIR_MD5LEN)) 
	  match = 1; 

	break;
      }
    }
  }
  
  log_dbg("user %s %s", conn->username, match ? "found" : "not found");

  fclose(f);
  free(line);
  return match;
}


/* redir_accept() does the following:
 1) forks a child process
 2) Accepts the tcp connection 
 3) Analyses a HTTP get request
 4) GET request can be one of the following:
    a) Logon request with username and challenge response
       - Does a radius request
       - If OK send result to parent and redirect to welcome page
       - Else redirect to error login page
    b) Logoff request
       - Send logoff request to parent
       - Redirect to login page?
    c) Request for another server
       - Redirect to login server.

 Incoming requests are identified only by their IP address. No MAC
 address information is obtained. The main security problem is denial
 of service attacks by malicious hosts sending logoff requests for
 clients. This can be prevented by checking incoming packets for
 matching MAC and src IP addresses.
*/

int redir_accept(struct redir_t *redir, int idx) {
  int status;
  int new_socket;
  struct sockaddr_in address;
  int addrlen = sizeof(address);


  if ((new_socket = accept(redir->fd[idx], (struct sockaddr *)&address, (socklen_t*) &addrlen)) < 0) {
    if (errno != ECONNABORTED)
      log_err(errno, "accept() failed!");
    return 0;
  }

  /* This forks a new process. The child really should close all
     unused file descriptors and free memory allocated. This however
     is performed when the process exits, so currently we don't
     care */

  redir_radius_id++;
  
  if ((status = fork()) < 0) {
    log_err(errno, "fork() returned -1!");
    return 0;
  }

  if (status > 0) { /* Parent */
    close(new_socket);
    return 0; 
  }

#if defined(F_DUPFD)
  if (fcntl(new_socket,F_GETFL,0) == -1) return -1;
  close(0);
  if (fcntl(new_socket,F_DUPFD,0) == -1) return -1;
  if (fcntl(new_socket,F_GETFL,1) == -1) return -1;
  close(1);
  if (fcntl(new_socket,F_DUPFD,1) == -1) return -1;
#else
  if (dup2(new_socket,0) == -1) return -1;
  if (dup2(new_socket,1) == -1) return -1;
#endif
    
  if (idx == 1 && options.uamui) {
    char *binqqargs[2] = { options.uamui, 0 } ;
    char buffer[56];

    snprintf(buffer,sizeof(buffer)-1,"%s",inet_ntoa(address.sin_addr));
    setenv("TCPREMOTEIP",buffer,1);
    setenv("REMOTE_ADDR",buffer,1);
    snprintf(buffer,sizeof(buffer)-1,"%d",ntohs(address.sin_port));
    setenv("TCPREMOTEPORT",buffer,1);
    setenv("REMOTE_PORT",buffer,1);

    execv(*binqqargs, binqqargs);

  } else return redir_main(redir, 0, 1, &address, idx);
  return 0;
}

int redir_main(struct redir_t *redir, int infd, int outfd, struct sockaddr_in *address, int isui) {
  char hexchal[1+(2*REDIR_MD5LEN)];
  unsigned char challenge[REDIR_MD5LEN];
  int bufsize = REDIR_MAXBUFFER;
  char buffer[bufsize+1];
  char qs[REDIR_USERURLSIZE];
  struct redir_msg_t msg;
  int buflen;
  int state = 0;

  struct redir_conn_t conn;
  struct sigaction act, oldact;
  struct itimerval itval;
  struct redir_socket socket;
  int ispost = isui;
  int clen = 0;

  /* Close of socket */
  void redir_close () {
    if (shutdown(outfd, SHUT_WR) != 0)
      log_dbg("shutdown socket for writing");

    if (!set_nonblocking(infd)) 
      while(read(infd, buffer, sizeof(buffer)) > 0);

    if (shutdown(infd, SHUT_RD) != 0)
      log_dbg("shutdown socket for reading");

    close(outfd);
    close(infd);
    exit(0);
  }
  
  void redir_memcopy(int msg_type) {
    redir_challenge(challenge);
    (void)redir_chartohex(challenge, hexchal);
    msg.type = msg_type;
    msg.addr = address->sin_addr;
    memcpy(msg.uamchal, challenge, REDIR_MD5LEN);
    if (options.debug) {
      log_dbg("---->>> resetting challenge: %s", hexchal);
    }
  }

  memset(&socket,0,sizeof(socket));
  memset(hexchal, 0, sizeof(hexchal));
  memset(qs, 0, sizeof(qs));
  memset(&conn, 0, sizeof(conn));
  memset(&msg, 0, sizeof(msg));
  memset(&act, 0, sizeof(act));

  socket.fd[0] = infd;
  socket.fd[1] = outfd;

  redir->starttime = time(NULL);

  if (set_nonblocking(socket.fd[0])) {
    log_err(errno, "fcntl() failed");
    redir_close();
  }

  act.sa_handler = redir_termination;
  sigaction(SIGTERM, &act, &oldact);
  sigaction(SIGINT, &act, &oldact);
  act.sa_handler = redir_alarm;
  sigaction(SIGALRM, &act, &oldact);

  memset(&itval, 0, sizeof(itval));
  itval.it_interval.tv_sec = REDIR_MAXTIME; 
  itval.it_interval.tv_usec = 0; 
  itval.it_value.tv_sec = REDIR_MAXTIME;
  itval.it_value.tv_usec = 0; 

  if (setitimer(ITIMER_REAL, &itval, NULL)) {
    log_err(errno, "setitimer() failed!");
  }

  termstate = REDIR_TERM_GETREQ;

  if (optionsdebug) 
    log_dbg("Calling redir_getreq()\n");

  if (redir_getreq(redir, &socket, &conn, &ispost, &clen, qs, sizeof(qs))) {
    log_dbg("Error calling get_req. Terminating\n");
    redir_close();
  }

  if (conn.type == REDIR_WWW) {
    int fd = -1;
    if (options.wwwdir && conn.wwwfile && *conn.wwwfile) {
      char *ctype = "text/plain";
      char *filename = conn.wwwfile;
      int namelen = strlen(filename);
      int parse = 0;
      
      /* check filename */
      { char *p;
	for (p=filename; *p; p++) {
	  if (*p >= 'a' && *p <= 'z') 
	    continue;
	  switch(*p) {
	  case '.':
	  case '_':
	  case '-':
	    break;
	  default:
	    /* invalid file name! */
	    log_err(0, "invalid www request [%s]!", filename);
	    redir_close();
	  }
	}
      }
      
      /* serve the local content */
      
      if      (!strcmp(filename + (namelen - 5), ".html")) ctype = "text/html";
      else if (!strcmp(filename + (namelen - 4), ".gif"))  ctype = "image/gif";
      else if (!strcmp(filename + (namelen - 3), ".js"))   ctype = "text/javascript";
      else if (!strcmp(filename + (namelen - 4), ".css"))  ctype = "text/css";
      else if (!strcmp(filename + (namelen - 4), ".jpg"))  ctype = "image/jpeg";
      else if (!strcmp(filename + (namelen - 4), ".png"))  ctype = "image/png";
      else if (!strcmp(filename + (namelen - 4), ".swf"))  ctype = "application/x-shockwave-flash";
      else if (!strcmp(filename + (namelen - 4), ".chi")){ ctype = "text/html"; parse = 1; }
      else { 
	/* we do not serve it! */
	log_err(0, "invalid file extension! [%s]", filename);
	redir_close();
      }
      
      if (parse) {
	if (!options.wwwbin) {
	  log_err(0, "the 'wwwbin' setting must be configured for CGI use");
	  redir_close();
	}
	
	if (clear_nonblocking(socket.fd[0])) {
	  log_err(errno, "fcntl() failed");
	}
	
	/* XXX: Todo: look for malicious content! */
	
	sprintf(buffer,"%d", clen > 0 ? clen : 0);
	setenv("CONTENT_LENGTH", buffer, 1);
	setenv("REQUEST_METHOD", ispost ? "POST" : "GET", 1);
	setenv("QUERY_STRING", qs, 1);
	
	log_dbg("Running: %s %s/%s",options.wwwbin, options.wwwdir, filename);
	sprintf(buffer, "%s/%s", options.wwwdir, filename);
	
	{
	  char *binqqargs[3] = { options.wwwbin, buffer, 0 } ;
	  int status;
	  
	  if ((status = fork()) < 0) {
	    log_err(errno, "fork() returned -1!");
	    /* lets just execv and ignore the extra crlf problem */
	    execv(*binqqargs, binqqargs);
	  }
	  
	  if (status > 0) { /* Parent */
	    /* now wait for the child (the cgi-prog) to finish
	     * and let redir_close remove unwanted data
	     * (for instance) extra crlf from ie7 in POSTs)
	     * to avoid a tcp-reset.
	     */
	    wait(NULL);
	  }
	  else {
	    /* Child */
	    execv(*binqqargs, binqqargs);
	  }
	}
	
	redir_close();
      }
      
      if (!chroot(options.wwwdir) && !chdir("/")) {
	
	fd = open(filename, O_RDONLY);
	
	if (fd > 0) {
	  
	  if (clear_nonblocking(socket.fd[0])) {
	    log_err(errno, "fcntl() failed");
	  }
	  
	  buflen = snprintf(buffer, bufsize,
			    "HTTP/1.0 200 OK\r\nContent-type: %s\r\n\r\n", ctype);
	  
	  if (tcp_write(&socket, buffer, buflen) < 0) {
	    log_err(errno, "tcp_write() failed!");
	  }
	  
	  while ((buflen = read(fd, buffer, bufsize)) > 0)
	    if (tcp_write(&socket, buffer, buflen) < 0)
	      log_err(errno, "tcp_write() failed!");
	  
	  close(fd);
	  redir_close(); /* which exits */
	} 
	else log_err(0, "could not open local content file %s!", filename);
      }
      else log_err(0, "chroot to %s was not successful\n", options.wwwdir); 
    } 
    else log_err(0, "Required: 'wwwdir' (in chilli.conf) and 'file' query-string param\n"); 
    
    redir_close();
  }


  termstate = REDIR_TERM_GETSTATE;
  /*log_dbg("Calling cb_getstate()\n");*/
  if (!redir->cb_getstate) { log_err(0, "No cb_getstate() defined!"); redir_close(); }

  state = redir->cb_getstate(redir, &address->sin_addr, &conn);

  termstate = REDIR_TERM_PROCESS;
  if (optionsdebug) log_dbg("Processing received request\n");

  /* default hexchal for use in replies */
  redir_chartohex(conn.uamchal, hexchal);

  switch (conn.type) {

  case REDIR_LOGIN:
    
    /* Was client was already logged on? */
    if (state == 1) {
      log_dbg("redir_accept: already logged on");
      redir_reply(redir, &socket, &conn, REDIR_ALREADY, NULL, 0, 
		  NULL, NULL, conn.userurl, NULL,
		  NULL, conn.hismac, &conn.hisip, qs);
      redir_close();
    }

    /* Did the challenge expire? */
    if ((conn.uamtime + REDIR_CHALLENGETIMEOUT2) < time(NULL)) {
      log_dbg("redir_accept: challenge expired: %d : %d", conn.uamtime, time(NULL));
      redir_memcopy(REDIR_CHALLENGE);      
      if (msgsnd(redir->msgid, (struct msgbuf*) &msg, 
		 sizeof(struct redir_msg_t), 0) < 0) {
	log_err(errno, "msgsnd() failed!");
	redir_close();
      }

      redir_reply(redir, &socket, &conn, REDIR_FAILED_OTHER, NULL, 
		  0, hexchal, NULL, NULL, NULL, 
		  NULL, conn.hismac, &conn.hisip, qs);
      redir_close();
    }

    if (is_local_user(redir, &conn)) { 
       conn.response = REDIR_SUCCESS;
    }
    else {
      termstate = REDIR_TERM_RADIUS;

      if (optionsdebug) 
	log_dbg("redir_accept: Sending radius request\n");

      redir_radius(redir, &address->sin_addr, &conn);
      termstate = REDIR_TERM_REPLY;

      if (optionsdebug) 
	log_dbg("Received radius reply\n");
    }

    if (options.defsessiontimeout && !conn.params.sessiontimeout)
      conn.params.sessiontimeout = options.defsessiontimeout;

    if (options.defidletimeout && !conn.params.idletimeout)
      conn.params.idletimeout = options.defidletimeout;

    if (conn.response == REDIR_SUCCESS) { /* Radius-Accept */
      bstring besturl = bfromcstr(conn.params.url);
      
      if (! (besturl && besturl->slen)) 
	bassigncstr(besturl, conn.userurl);
      
      if (redir->no_uamsuccess && besturl && besturl->slen)
	redir_reply(redir, &socket, &conn, conn.response, besturl, conn.params.sessiontimeout,
		    NULL, conn.username, conn.userurl, conn.reply,
		    conn.params.url, conn.hismac, &conn.hisip, qs);
      else 
	redir_reply(redir, &socket, &conn, conn.response, NULL, conn.params.sessiontimeout,
		    NULL, conn.username, conn.userurl, conn.reply, 
		    conn.params.url, conn.hismac, &conn.hisip, qs);
      
      bdestroy(besturl);
      
      msg.type = REDIR_LOGIN;
      strncpy(msg.username, conn.username, sizeof(msg.username));
      msg.username[sizeof(msg.username)-1] = 0;
      msg.statelen = conn.statelen;
      memcpy(msg.statebuf, conn.statebuf, conn.statelen);
      msg.classlen = conn.classlen;
      memcpy(msg.classbuf, conn.classbuf, conn.classlen);
      msg.addr = address->sin_addr;

      memcpy(&msg.params, &conn.params, sizeof(msg.params));

      if (conn.userurl && *conn.userurl) {
	strncpy(msg.userurl, conn.userurl, sizeof(msg.userurl));
	msg.userurl[sizeof(msg.userurl)-1] = 0;
	if (optionsdebug) log_dbg("-->> Msg userurl=[%s]\n",conn.userurl);
      }
      
      if (msgsnd(redir->msgid, (struct msgbuf*) &msg, sizeof(struct redir_msg_t), 0) < 0)
	log_err(errno, "msgsnd() failed!");
    }
    else {
      bstring besturl = bfromcstr(conn.params.url);
      int hasnexturl = (besturl && besturl->slen > 5);

      if (!hasnexturl) {
	redir_memcopy(REDIR_CHALLENGE);
      } else {
	msg.type = REDIR_NOTYET;
	msg.addr = address->sin_addr;
	msg.classlen = conn.classlen;
	memcpy(msg.classbuf, conn.classbuf, conn.classlen);
	memcpy(&msg.params, &conn.params, sizeof(msg.params));
      }

      if (msgsnd(redir->msgid, (struct msgbuf *)&msg, sizeof(struct redir_msg_t), 0) < 0) {
	log_err(errno, "msgsnd() failed!");
      } else {
	redir_reply(redir, &socket, &conn, conn.response, 
		    hasnexturl ? besturl : NULL, 
		    0, hexchal, NULL, conn.userurl, conn.reply, 
		    NULL, conn.hismac, &conn.hisip, qs);
      }
      
      bdestroy(besturl);
    }    
    redir_close();

  case REDIR_LOGOUT:
    {
      bstring besturl = bfromcstr(conn.params.url);

      redir_memcopy(REDIR_LOGOUT); 
      if (msgsnd(redir->msgid, (struct msgbuf*) &msg, 
		 sizeof(struct redir_msg_t), 0) < 0) {
	log_err(errno, "msgsnd() failed!");
	redir_close();
      }

      conn.authenticated=0;
      
      if (! (besturl && besturl->slen)) 
	bassigncstr(besturl, conn.userurl);

      if (redir->no_uamsuccess && besturl && besturl->slen)
	redir_reply(redir, &socket, &conn, REDIR_LOGOFF, besturl, 0, 
		    hexchal, NULL, conn.userurl, NULL, 
		    NULL, conn.hismac, &conn.hisip, qs);
      else 
	redir_reply(redir, &socket, &conn, REDIR_LOGOFF, NULL, 0, 
		    hexchal, NULL, conn.userurl, NULL, 
		    NULL, conn.hismac, &conn.hisip, qs);
      
      bdestroy(besturl);
      
      redir_close();    
    }
    
  case REDIR_PRELOGIN:

    /* Did the challenge expire? */
    if ((conn.uamtime + REDIR_CHALLENGETIMEOUT1) < time(NULL)) {
      redir_memcopy(REDIR_CHALLENGE);
      if (msgsnd(redir->msgid, (struct msgbuf*) &msg,  sizeof(msg), 0) < 0) {
	log_err(errno, "msgsnd() failed!");
	redir_close();
      }
    }
    
    if (state == 1) {
      redir_reply(redir, &socket, &conn, REDIR_ALREADY, 
		  NULL, 0, NULL, NULL, conn.userurl, NULL,
		  NULL, conn.hismac, &conn.hisip, qs);
    }
    else {
      redir_reply(redir, &socket, &conn, REDIR_NOTYET, 
		  NULL, 0, hexchal, NULL, conn.userurl, NULL, 
		  NULL, conn.hismac, &conn.hisip, qs);
    }
    redir_close();

  case REDIR_ABORT:

    if (state == 1) {
      redir_reply(redir, &socket, &conn, REDIR_ABORT_NAK, 
		  NULL, 0, NULL, NULL, conn.userurl, NULL, 
		  NULL, conn.hismac, &conn.hisip, qs);
    }
    else {
      redir_memcopy(REDIR_ABORT);
      if (msgsnd(redir->msgid, (struct msgbuf*) &msg, 
		 sizeof(struct redir_msg_t), 0) < 0) {
	log_err(errno, "msgsnd() failed!");
	redir_close();
      }
      redir_reply(redir, &socket, &conn, REDIR_ABORT_ACK, 
		  NULL, 0, hexchal, NULL, conn.userurl, NULL, 
		  NULL, conn.hismac, &conn.hisip, qs);
    }
    redir_close();

  case REDIR_ABOUT:
    redir_reply(redir, &socket, &conn, REDIR_ABOUT, NULL, 
		0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, qs);
    redir_close();

  case REDIR_STATUS:
    {
      uint32_t sessiontime;
      uint32_t timeleft;
      time_t timenow = time(0);

      /* Did the challenge expire? */
      if ((conn.uamtime + REDIR_CHALLENGETIMEOUT1) < time(NULL)) {
	redir_memcopy(REDIR_CHALLENGE);
	if (msgsnd(redir->msgid, (struct msgbuf*) &msg,  sizeof(msg), 0) < 0) {
	  log_err(errno, "msgsnd() failed!");
	  redir_close();
	}
      }
      
      sessiontime = timenow - conn.start_time;

      if (conn.params.sessiontimeout)
	timeleft = conn.params.sessiontimeout - sessiontime;
      else
	timeleft = 0;

      redir_reply(redir, &socket, &conn, REDIR_STATUS, NULL, timeleft,
		  hexchal, conn.username, conn.userurl, conn.reply, 
		  conn.params.url, conn.hismac, &conn.hisip, qs);
      
      redir_close();
    }

  case REDIR_MSDOWNLOAD:
    buflen = snprintf(buffer, bufsize, "HTTP/1.0 403 Forbidden\r\n\r\n");
    tcp_write(&socket, buffer, buflen);
    redir_close();
  }

  /* It was not a request for a known path. It must be an original request */
  if (optionsdebug) 
    log_dbg("redir_accept: Original request");


  /* Did the challenge expire? */
  if ((conn.uamtime + REDIR_CHALLENGETIMEOUT1) < time(NULL)) {
    redir_memcopy(REDIR_CHALLENGE);
    strncpy(msg.userurl, conn.userurl, sizeof(msg.userurl));
    msg.userurl[sizeof(msg.userurl)-1] = 0;
    if (optionsdebug) log_dbg("-->> Msg userurl=[%s]\n",msg.userurl);
    if (msgsnd(redir->msgid, (struct msgbuf*) &msg, 
	       sizeof(struct redir_msg_t), 0) < 0) {
      log_err(errno, "msgsnd() failed!");
      redir_close();
    }
  }
  else {
    redir_chartohex(conn.uamchal, hexchal);
    /*
    msg.type = REDIR_CHALLENGE;
    msg.addr = address->sin_addr;
    strncpy(msg.userurl, conn.userurl, sizeof(msg.userurl));
    memcpy(msg.uamchal, conn.uamchal, REDIR_MD5LEN);
    if (msgsnd(redir->msgid, (struct msgbuf*) &msg, 
	       sizeof(struct redir_msg_t), 0) < 0) {
      log_err(errno, "msgsnd() failed!");
      redir_close();
    }
    */
  }

  if (redir->homepage) {
    bstring url = bfromcstralloc(1024,"");
    bstring urlenc = bfromcstralloc(1024,"");

    if (redir_buildurl(&conn, url, redir, "notyet", 0, hexchal, NULL,
		       conn.userurl, NULL, NULL, conn.hismac, &conn.hisip) == -1) {
      log_err(errno, "redir_buildurl failed!");
      redir_close();
    }

    redir_urlencode(url, urlenc);

    bassignformat(url, "%s%cloginurl=",
		  redir->homepage, strchr(redir->homepage, '?') ? '&' : '?');
    bconcat(url, urlenc);

    redir_reply(redir, &socket, &conn, REDIR_NOTYET, url, 
		0, hexchal, NULL, conn.userurl, NULL, 
		NULL, conn.hismac, &conn.hisip, qs);
  }
  else if (state == 1) {
    redir_reply(redir, &socket, &conn, REDIR_ALREADY, NULL, 0, 
		NULL, NULL, conn.userurl, NULL,
		NULL, conn.hismac, &conn.hisip, qs);
  }
  else {
    redir_reply(redir, &socket, &conn, REDIR_NOTYET, NULL, 
		0, hexchal, NULL, conn.userurl, NULL, 
		NULL, conn.hismac, &conn.hisip, qs);
  }

  redir_close();
  return -1; /* never gets here */
}


/* Set callback to determine state information for the connection */
int redir_set_cb_getstate(struct redir_t *redir,
  int (*cb_getstate) (struct redir_t *redir, struct in_addr *addr,
		      struct redir_conn_t *conn)) {
  redir->cb_getstate = cb_getstate;
  return 0;
}

