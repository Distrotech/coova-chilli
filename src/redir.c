/* 
 * Copyright (C) 2003, 2004, 2005 Mondru AB.
 * Copyright (C) 2007-2009 Coova Technologies, LLC. <support@coova.com>
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
#include "syserr.h"
#include "radius.h"
#include "radius_wispr.h"
#include "radius_chillispot.h"
#include "redir.h"
#include "md5.h"
#include "dhcp.h"
#include "chilli.h"
#include "options.h"
#include "ssl.h"

static int optionsdebug = 0; /* TODO: Should be changed to instance */

static int termstate = REDIR_TERM_INIT;    /* When we were terminated */

char credits[] =
"<H1>CoovaChilli(ChilliSpot) " VERSION "</H1>"
"<p>Copyright 2002-2005 Mondru AB</p>"
"<p>Copyright 2006-2009 Coova Technologies, LLC</p>"
"ChilliSpot is an Open Source captive portal or wireless LAN access point "
"controller developed by the community at <a href=\"http://www.coova.org\">www.coova.org</a>. "
"It is licensed under the GNU General Public License (GPL). ";

static int redir_getparam(struct redir_t *redir, char *src, char *param, bstring dst);
static uint8_t radius_packet_id = 0;
extern time_t mainclock;

/* Termination handler for clean shutdown */
static void redir_termination(int signum) {
  log_dbg("Terminating redir client!");
  exit(0);
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
    log_err(errno, "fopen(/dev/urandom)");
    return -1;
  }
  
  if (fread(dst, 1, REDIR_MD5LEN, file) != REDIR_MD5LEN) {
    log_err(errno, "fread() failed");
    fclose(file);
    return -1;
  }
  
  fclose(file);
  return 0;
}

static int redir_hextochar(unsigned char *src, int slen, unsigned char * dst, int len) {
  char x[3];
  int n;
  int i;
  int y;
  
  for (n=0; n < len; n++) {
    i = n * 2;
    y = 0;
    if (i < slen) {
      x[0] = src[i];
      x[1] = src[i+1];
      x[2] = 0;
      switch (sscanf(x, "%2x", &y)) {
      case 0:  y = 0;
      case 1:  break;
      default:
	log_err(errno, "HEX conversion failed (src='%s', len=%d, n=%d, y=%d)!", src, len, n, y);
	return -1;
      }
    }
    dst[n] = (unsigned char) y;
  }

  return 0;
}

/* Convert 'len' octet unsigned char to 'len'*2+1 octet ASCII hex string */
int redir_chartohex(unsigned char *src, char *dst, size_t len) {
  char x[3];
  int i = 0;
  int n;
 
  for (n=0; n < len; n++) {
    snprintf(x, 3, "%.2x", src[n]);
    dst[i++] = x[0];
    dst[i++] = x[1];
  }

  dst[i] = 0;
  return 0;
}

/*
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
*/

static void redir_http(bstring s, char *code) {
  bassigncstr(s, "HTTP/1.1 ");
  bcatcstr(s, code);
  bcatcstr(s, "\r\n");
  bcatcstr(s, "Connection: close\r\nCache-Control: no-cache, must-revalidate\r\n");
  bcatcstr(s, "P3P: CP=\"IDC DSP COR ADM DEVi TAIi PSA PSD IVAi IVDi CONi HIS OUR IND CNT\"\r\n");
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
int redir_urlencode(bstring src, bstring dst) {
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
	('*' == src->data[n])) {
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


static int bstring_buildurl(bstring str, struct redir_conn_t *conn,
			    struct redir_t *redir, char *redir_url, char *resp,
			    long int timeleft, char* hexchal, char* uid, 
			    char* userurl, char* reply, char* redirurl,
			    uint8_t *hismac, struct in_addr *hisip, char *amp) {
  bstring bt = bfromcstr("");
  bstring bt2 = bfromcstr("");

  bassignformat(str, "%s%sres=%s%suamip=%s%suamport=%d", 
		redir_url, strchr(redir_url, '?') ? amp : "?", resp, amp,
		inet_ntoa(redir->addr), amp, 
		redir->port);

  if (hexchal) {
    bcatcstr(str, amp);
    bassignformat(bt, "challenge=%s", hexchal);
    bconcat(str, bt);
    bassigncstr(bt,"");
  }
  
  if (conn->type == REDIR_STATUS) {
    int starttime = conn->s_state.start_time;
    if (starttime) {
      int sessiontime;
      time_t timenow = mainclock_now();

      sessiontime = timenow - starttime;

      bcatcstr(str, amp);
      bassignformat(bt, "starttime=%ld", starttime);
      bconcat(str, bt);
      bcatcstr(str, amp);
      bassignformat(bt, "sessiontime=%ld", sessiontime);
      bconcat(str, bt);
    }

    if (conn->s_params.sessiontimeout) {
      bcatcstr(str, amp);
      bassignformat(bt, "sessiontimeout=%ld", conn->s_params.sessiontimeout);
      bconcat(str, bt);
    }

    if (conn->s_params.sessionterminatetime) {
      bcatcstr(str, amp);
      bassignformat(bt, "stoptime=%ld", conn->s_params.sessionterminatetime);
      bconcat(str, bt);
    }
  }
 
  bcatcstr(str, amp);
  bcatcstr(str, "called=");
  if (_options.nasmac)
    bassigncstr(bt, _options.nasmac);
  else 
    bassignformat(bt, "%.2X-%.2X-%.2X-%.2X-%.2X-%.2X", 
		  redir->nas_hwaddr[0], redir->nas_hwaddr[1], redir->nas_hwaddr[2],
		  redir->nas_hwaddr[3], redir->nas_hwaddr[4], redir->nas_hwaddr[5]);

  redir_urlencode(bt, bt2);
  bconcat(str, bt2);

  if (uid) {
    bcatcstr(str, amp);
    bcatcstr(str, "uid=");
    bassigncstr(bt, uid);
    redir_urlencode(bt, bt2);
    bconcat(str, bt2);
  }

  if (timeleft) {
    bcatcstr(str, amp);
    bassignformat(bt, "timeleft=%ld", timeleft);
    bconcat(str, bt);
  }
  
  if (hismac) {
    bcatcstr(str, amp);
    bcatcstr(str, "mac=");
    bassignformat(bt, "%.2X-%.2X-%.2X-%.2X-%.2X-%.2X",
		  hismac[0], hismac[1], 
		  hismac[2], hismac[3],
		  hismac[4], hismac[5]);
    redir_urlencode(bt, bt2);
    bconcat(str, bt2);
  }

  if (hisip) {
    bcatcstr(str, amp);
    bassignformat(bt, "ip=%s", inet_ntoa(*hisip));
    bconcat(str, bt);
  }

  if (reply) {
    bcatcstr(str, amp);
    bcatcstr(str, "reply=");
    bassigncstr(bt, reply);
    redir_urlencode(bt, bt2);
    bconcat(str, bt2);
  }

  if (redir->ssid) {
    bcatcstr(str, amp);
    bcatcstr(str, "ssid=");
    bassigncstr(bt, redir->ssid);
    redir_urlencode(bt, bt2);
    bconcat(str, bt2);
  }

  if (redir->radiusnasid) {
    bcatcstr(str, amp);
    bcatcstr(str, "nasid=");
    bassigncstr(bt, redir->radiusnasid);
    redir_urlencode(bt, bt2);
    bconcat(str, bt2);
  }

#ifdef ENABLE_IEEE8021Q
  if (conn->s_state.tag8021q) {
    bcatcstr(str, amp);
    bcatcstr(str, "vlan=");
    bassignformat(bt, "%d", (int)(ntohs(conn->s_state.tag8021q) & 0x0FFF));
    bconcat(str, bt);
  } else 
#endif
  if (redir->vlan) {
    bcatcstr(str, amp);
    bcatcstr(str, "vlan=");
    bassigncstr(bt, redir->vlan);
    redir_urlencode(bt, bt2);
    bconcat(str, bt2);
  }

  if (conn->lang[0]) {
    bcatcstr(str, amp);
    bcatcstr(str, "lang=");
    bassigncstr(bt, conn->lang);
    redir_urlencode(bt, bt2);
    bconcat(str, bt2);
  }

  if (conn->s_state.sessionid[0]) {
    bcatcstr(str, amp);
    bcatcstr(str, "sessionid=");
    bassigncstr(bt, conn->s_state.sessionid);
    redir_urlencode(bt, bt2);
    bconcat(str, bt2);
  }

  if (redirurl) {
    bcatcstr(str, amp);
    bcatcstr(str, "redirurl=");
    bassigncstr(bt, redirurl);
    redir_urlencode(bt, bt2);
    bconcat(str, bt2);
  }

  if (userurl) {
    bcatcstr(str, amp);
    bcatcstr(str, "userurl=");
    bassigncstr(bt, userurl);
    redir_urlencode(bt, bt2);
    bconcat(str, bt2);
  }

  if (redir->secret && *redir->secret) { /* take the md5 of the url+uamsecret as a checksum */
    redir_md_param(str, redir->secret, amp);
  }

  bdestroy(bt);
  bdestroy(bt2);
  return 0;
}

int redir_md_param(bstring str, char *secret, char *amp) {
  MD5_CTX context;
  unsigned char cksum[16];
  char hex[32+1];
  int i;
  
  MD5Init(&context);
  MD5Update(&context, (uint8_t *)str->data, str->slen);
  MD5Update(&context, (uint8_t *)secret, strlen(secret));
  MD5Final(cksum, &context);
  
  hex[0]=0;
  for (i=0; i<16; i++) {
    sprintf(hex+2*i, "%.2X", cksum[i]);
  }
  
  bcatcstr(str, amp);
  bcatcstr(str, "md=");
  bcatcstr(str, hex);
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
	       "<ResponseCode>50</ResponseCode>\r\n"
	       "<ReplyMessage>Already logged on</ReplyMessage>\r\n");
      
      bassignformat(bt, "<LogoffURL>http://%s:%d/logoff</LogoffURL>\r\n",
		    inet_ntoa(redir->addr), redir->port);
      bconcat(b, bt);
      
      if (redirurl) {
	bassignformat(bt, "<RedirectionURL>%s</RedirectionURL>\r\n", redirurl);
	bconcat(b, bt);
      }

      bcatcstr(b, "</AuthenticationPollReply>\r\n");
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
      
    case REDIR_SPLASH:
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

      bcatcstr(b, "<LoginURL>");

      bstring_buildurl(bt, conn, redir, _options.wisprlogin ? _options.wisprlogin : redir->url, 
		       "smartclient", 0, hexchal, NULL, NULL, NULL, NULL, 
		       conn->hismac, &conn->hisip, "&amp;");
      bconcat(b, bt);

      bcatcstr(b, "</LoginURL>\r\n");
      
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
      if (conn->s_state.authenticated != 1) {
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

#ifdef ENABLE_CHILLIXML
  if (redir->chillixml) {
    bcatcstr(b, "<ChilliSpotSession>\r\n");
    switch (res) {
    case REDIR_SPLASH:
    case REDIR_NOTYET:
      bassignformat(bt, "<Challenge>%s</Challenge>\r\n", hexchal);
      bconcat(b, bt);
      break;
    case REDIR_STATUS:
      if (conn->s_state.authenticated == 1) {
        time_t timenow = mainclock_now();
        uint32_t sessiontime;

        sessiontime = timenow - conn->s_state.start_time;

        bcatcstr(b, "<State>1</State>\r\n");

        bassignformat(bt, "<StartTime>%d</StartTime>\r\n" , conn->s_state.start_time);
	bconcat(b, bt);

        bassignformat(bt, "<SessionTime>%d</SessionTime>\r\n", sessiontime);
	bconcat(b, bt);

        if (timeleft) {
	  bassignformat(bt, "<TimeLeft>%d</TimeLeft>\r\n", timeleft);
	  bconcat(b, bt);
        }

        bassignformat(bt, "<Timeout>%d</Timeout>\r\n", conn->s_params.sessiontimeout);
	bconcat(b, bt);

        bassignformat(bt, "<InputOctets>%d</InputOctets>\r\n", conn->s_state.input_octets);
	bconcat(b, bt);

        bassignformat(bt, "<OutputOctets>%d</OutputOctets>\r\n", conn->s_state.output_octets);
	bconcat(b, bt);
	
        bassignformat(bt, "<MaxInputOctets>%d</MaxInputOctets>\r\n", conn->s_params.maxinputoctets);
	bconcat(b, bt);
	
        bassignformat(bt, "<MaxOutputOctets>%d</MaxOutputOctets>\r\n", conn->s_params.maxoutputoctets);
	bconcat(b, bt);

        bassignformat(bt, "<MaxTotalOctets>%d</MaxTotalOctets>\r\n", conn->s_params.maxtotaloctets);
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
#endif
  
  bcatcstr(b, "-->\r\n");
  bdestroy(bt);

  return 0;
}

static int redir_buildurl(struct redir_conn_t *conn, bstring str,
			  struct redir_t *redir, char *resp,
			  long int timeleft, char* hexchal, char* uid, 
			  char* userurl, char* reply, char* redirurl,
			  uint8_t *hismac, struct in_addr *hisip) {
  char *redir_url = redir->url;

  if ((conn->s_params.flags & REQUIRE_UAM_SPLASH) && 
      conn->s_params.url[0]) {
    redir_url = (char *)conn->s_params.url;
  }
  
  return bstring_buildurl(str, conn, redir, redir_url, resp, timeleft, 
			  hexchal, uid, userurl, reply, redirurl, hismac, hisip, "&");
}

ssize_t
tcp_write_timeout(int timeout, struct redir_socket_t *sock, char *buf, size_t len) {
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
    return send(fd, buf, len, 0);
#else
    return write(fd, buf, len);
#endif

  return -1;
}

static int timeout = 10;

ssize_t
tcp_write(struct redir_socket_t *sock, char *buf, size_t len) {
  ssize_t c;
  size_t r = 0;
  while (r < len) {
#ifdef HAVE_SSL
    if (sock->sslcon) {
      c = openssl_write(sock->sslcon, buf, len, 0);
    } else 
#endif
    c = tcp_write_timeout(timeout, sock, buf+r, len-r);
    if (c <= 0) return (ssize_t)r;
    r += (size_t)c;
  }
  return (ssize_t)r;
}

#ifdef ENABLE_JSON
static int redir_json_reply(struct redir_t *redir, int res, struct redir_conn_t *conn,  
			    char *hexchal, char *userurl, char *redirurl, uint8_t *hismac, 
			    char *reply, char *qs, bstring s) {
  bstring tmp = bfromcstr("");
  bstring json = bfromcstr("");

  unsigned char flg = 0;
#define FLG_cb     1
#define FLG_chlg   2
#define FLG_sess   4
#define FLG_loc    8
#define FLG_redir 16

  int state = conn->s_state.authenticated;
  int splash = (conn->s_params.flags & REQUIRE_UAM_SPLASH) == REQUIRE_UAM_SPLASH;

  redir_getparam(redir, qs, "callback", tmp);

  if (tmp->slen) {
    bconcat(json, tmp);
    bcatcstr(json, "(");
    flg |= FLG_cb;
  }
  
  switch (res) {
  case REDIR_ALREADY:
    flg |= FLG_sess;
    break;

  case REDIR_FAILED_REJECT:
  case REDIR_FAILED_OTHER:
    flg |= FLG_chlg;
    flg |= FLG_redir;
    break;

  case REDIR_SUCCESS:
    flg |= FLG_sess;
    flg |= FLG_redir;
    state = 1;
    break;

  case REDIR_LOGOFF:
    flg |= FLG_sess | FLG_chlg;
    break;

  case REDIR_SPLASH:
  case REDIR_NOTYET:
    flg |= FLG_chlg;
    flg |= FLG_loc;
    flg |= FLG_redir;
    break;

  case REDIR_ABORT_ACK:
  case REDIR_ABORT_NAK:
  case REDIR_ABOUT:
    break;

  case REDIR_STATUS:
    if (state && !splash) {
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

  if (state && splash)
    state = 3;

  bcatcstr(json, "{\"version\":\"1.0\",\"clientState\":");

  bassignformat(tmp, "%d", state);
  bconcat(json, tmp);

  if (reply) {
    bcatcstr(json, ",\"message\":\"");
    bcatcstr(json, reply);
    bcatcstr(json, "\"");
  }

  if ((flg & FLG_chlg) && hexchal) {
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
    bassignformat(tmp , "http://%s:%d/logoff", 
		  inet_ntoa(redir->addr), redir->port);

    session_redir_json_fmt(json, userurl, redirurl, tmp, hismac);
  }

  if (flg & FLG_sess) 
    session_json_fmt(&conn->s_state, &conn->s_params, 
		     json, res == REDIR_SUCCESS);

  bcatcstr(json, "}");

  if (flg & FLG_cb) {
    bcatcstr(json, ")");
  }


  redir_http(s, "200 OK");

  bcatcstr(s, "Content-Length: ");
  bassignformat(tmp , "%d", blength(json));
  bconcat(s, tmp);

  bcatcstr(s, "\r\nContent-Type: ");
  if (tmp->slen) bcatcstr(s, "text/javascript");
  else bcatcstr(s, "application/json");

  bcatcstr(s, "\r\n\r\n");
  bconcat(s, json);

  if (_options.debug) {
    log_dbg("sending json: %s\n", json->data);
  }

  bdestroy(json);
  bdestroy(tmp);

  return 0;
}
#endif

/* Make an HTTP redirection reply and send it to the client */
static int redir_reply(struct redir_t *redir, struct redir_socket_t *sock, 
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
  case REDIR_SPLASH:
    resp = "splash";
    break;
  case REDIR_ABORT_ACK:
    resp = "logoff";
    break;
  case REDIR_ABORT_NAK:
    resp = "already";
    break;
  case REDIR_ABOUT:
  case REDIR_ABORT:
    break;
  case REDIR_STATUS:
    resp = conn->s_state.authenticated == 1 ? "already" : "notyet";
    break;
  default:
    log_err(0, "Unknown res in switch");
    return -1;
  }

  buffer = bfromcstralloc(1024, "");

#ifdef ENABLE_JSON
  if (conn->format == REDIR_FMT_JSON) {

    redir_json_reply(redir, res, conn, hexchal, userurl, redirurl, hismac, reply, qs, buffer);
    
  } else 
#endif
  if (resp) {
    bstring bt;
    bstring bbody;

    redir_http(buffer, "302 Moved Temporarily");
    bcatcstr(buffer, "Location: ");
    
    if (url) {
      bconcat(buffer, url);
    } else {
      bt = bfromcstralloc(1024,"");
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
    
    bcatcstr(buffer, "\r\nContent-Type: text/html; charset=UTF-8\r\n");

    bbody = bfromcstralloc(512, 
			   "<HTML><BODY><H2>Browser error!</H2>"
			   "Browser does not support redirects!</BODY>\r\n");
    redir_xmlreply(redir, conn, res, timeleft, hexchal, reply, redirurl, bbody);
    bcatcstr(bbody, "\r\n</HTML>\r\n");

    bt = bfromcstralloc(128, "");
    bassignformat(bt, "Content-Length: %d\r\n", blength(bbody));
    bconcat(buffer, bt);
    
    bcatcstr(buffer, "\r\n"); /* end of headers */
    bconcat(buffer, bbody);

    bdestroy(bbody);
    bdestroy(bt);
    
  } else {
    redir_http(buffer, "200 OK");
    bcatcstr(buffer, 
	     "Content-type: text/html\r\n\r\n"
	     "<HTML><HEAD><TITLE>CoovaChilli</TITLE></HEAD><BODY>");
    bcatcstr(buffer, credits);
    bcatcstr(buffer, "</BODY></HTML>\r\n");
  }

  if (tcp_write(sock, (char*)buffer->data, buffer->slen) < 0) {
    log_err(errno, "tcp_write() failed!");
    bdestroy(buffer);
    return -1;
  }

  bdestroy(buffer);
  return 0;
}

/* Allocate new instance of redir */
int redir_new(struct redir_t **redir,
	      struct in_addr *addr, int port, int uiport) {

  if (!(*redir = calloc(1, sizeof(struct redir_t)))) {
    log_err(errno, "calloc() failed");
    return EOF;
  }

  (*redir)->addr = *addr;
  (*redir)->port = port;
  (*redir)->uiport = uiport;
  (*redir)->starttime = 0;

  return 0;
}

int redir_listen(struct redir_t *redir) {
  struct sockaddr_in address;
  int n = 0, tries = 0, success = 0;
  int optval;

  if ((redir->fd[0] = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    log_err(errno, "socket() failed");
    return -1;
  }

  if (redir->uiport && (redir->fd[1] = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    log_err(errno, "socket() failed");
    return -1;
  }

  /* Set up address */
  address.sin_family = AF_INET;
#if defined(__FreeBSD__) || defined (__APPLE__) || defined (__OpenBSD__) || defined (__NetBSD__)
  address.sin_len = sizeof (struct sockaddr_in);
#endif

  for (n = 0; n < 2 && redir->fd[n]; n++) {

    switch(n) {
    case 0:
      address.sin_addr.s_addr = redir->addr.s_addr;
      address.sin_port = htons(redir->port);
      break;
    case 1:
      /* XXX: binding to 0.0.0.0:uiport (should be configurable?) */
      address.sin_addr.s_addr = INADDR_ANY;
      address.sin_port = htons(redir->uiport);
      break;
    }

    optval = 1;
    if (setsockopt(redir->fd[n], SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))) {
      log_err(errno, "setsockopt() failed");
      close(redir->fd[n]);
      redir->fd[n]=0;
      break;
    }

#ifdef SO_REUSEPORT
    optval = 1;
    if (setsockopt(redir->fd[n], SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval))) {
      log_err(errno, "setsockopt() failed");
      close(redir->fd[n]);
      redir->fd[n]=0;
      return -1;
    }
#endif
    
    while (bind(redir->fd[n], (struct sockaddr *)&address, sizeof(address)) == -1) {
      if ((EADDRINUSE == errno) && (10 > tries++)) {
	log_warn(errno, "IP: %s Port: %d - Waiting for retry.",
		 inet_ntoa(address.sin_addr), ntohs(address.sin_port));
	if (sleep(5)) { /* In case we got killed */
	  close(redir->fd[n]);
	  redir->fd[n]=0;
	  break;
	}
      }
      else {
	log_err(errno, "bind() failed for %s:%d",
		inet_ntoa(address.sin_addr), ntohs(address.sin_port));
	if (n == 0 && address.sin_addr.s_addr != INADDR_ANY) {
	  log_warn(0, "trying INADDR_ANY instead");
	  address.sin_addr.s_addr = INADDR_ANY;
	} else {
	  close(redir->fd[n]);
	  redir->fd[n]=0;
	  break;
	}
      }
    }

    if (redir->fd[n]) {
      if (listen(redir->fd[n], REDIR_MAXLISTEN)) {
	log_err(errno, "listen() failed for %s:%d",
		inet_ntoa(address.sin_addr), ntohs(address.sin_port));
	close(redir->fd[n]);
	redir->fd[n]=0;
	break;
      } else {
	success++;
      }
    }
  }

  return success ? 0 : -1;
}

int redir_ipc(struct redir_t *redir) {
#ifdef USING_IPC_UNIX
  struct sockaddr_un local;
  int sock;
  
  if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    log_err(errno, "could not allocate UNIX Socket!");
  } else {

    char *statedir = _options.statedir ? _options.statedir : DEFSTATEDIR;
    char filedest[512];
    
    snprintf(filedest, sizeof(filedest), "%s/%s", statedir, 
	     _options.unixipc ? _options.unixipc : "chilli.ipc");
    
    local.sun_family = AF_UNIX;
    
    strcpy(local.sun_path, filedest);
    unlink(local.sun_path);
    
    if (bind(sock, (struct sockaddr *)&local, 
	     sizeof(struct sockaddr_un)) == -1) {
      log_err(errno, "could bind UNIX Socket!");
      close(sock);
      sock = -1;
    } else {
      if (listen(sock, 5) == -1) {
	log_err(errno, "could listen to UNIX Socket!");
	close(sock);
	sock = -1;
      } else {
	redir->msgfd = sock;
      }
    }
  }
#else
  if ((redir->msgid = msgget(IPC_PRIVATE, 0)) < 0) {
    log_err(errno, "msgget() failed");
    log_err(0, "Most likely your computer does not have System V IPC installed");
    return -1;
  }
  
  if (_options.uid) {
    struct msqid_ds ds;
    memset(&ds, 0, sizeof(ds));
    if (msgctl(redir->msgid, IPC_STAT, &ds) < 0) {
      log_err(errno, "msgctl(stat) failed");
      return -1;
    }
    ds.msg_perm.uid = _options.uid;
    if (_options.gid) ds.msg_perm.gid = _options.gid;
    ds.msg_perm.mode = (ds.msg_perm.mode & ~0777) | 0600;
    if (msgctl(redir->msgid, IPC_SET, &ds) < 0) {
      log_err(errno, "msgctl(set) failed");
      return -1;
    }
  }
#endif

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

#ifdef USING_IPC_UNIX
  close(redir->msgfd);
#else
  if (msgctl(redir->msgid, IPC_RMID, NULL)) {
    log_err(errno, "msgctl() failed");
  }
#endif
  
  free(redir);
  return 0;
}

/* Set redir parameters */
void redir_set(struct redir_t *redir, uint8_t *hwaddr, int debug) { 
  optionsdebug = debug; /* TODO: Do not change static variable from instance */
  redir->debug = debug;

  redir->no_uamwispr = _options.no_uamwispr;
  redir->chillixml = _options.chillixml;
  redir->url = _options.uamurl;
  redir->homepage = _options.uamhomepage;
  redir->secret = _options.uamsecret;
  redir->ssid = _options.ssid;
  redir->vlan = _options.vlan;
  redir->nasmac = _options.nasmac;
  redir->nasip = _options.nasip;
  redir->radiusserver0 = _options.radiusserver1;
  redir->radiusserver1 = _options.radiusserver2;
  redir->radiusauthport = _options.radiusauthport;
  redir->radiusacctport = _options.radiusacctport;
  redir->radiussecret  = _options.radiussecret;
  redir->radiusnasid  = _options.radiusnasid;
  redir->radiuslocationid  = _options.radiuslocationid;
  redir->radiuslocationname  = _options.radiuslocationname;
  redir->locationname  = _options.locationname;
  redir->radiusnasporttype = _options.radiusnasporttype;

  if (hwaddr)
    memcpy(redir->nas_hwaddr, hwaddr, sizeof(redir->nas_hwaddr));

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
static int redir_getreq(struct redir_t *redir, struct redir_socket_t *sock,
			struct redir_conn_t *conn, struct redir_httpreq_t *httpreq) {
  int fd = sock->fd[0];
  fd_set fds;
  struct timeval idleTime;
  int status;
  ssize_t recvlen = 0;
  size_t buflen = 0;
  char buffer[REDIR_MAXBUFFER];
  int i, lines=0, done=0;
  char *eol;

  char read_waiting;

  char *path = httpreq->path;

  memset(buffer, 0, sizeof(buffer));
  
  /* read whatever the client send to us */
  while (!done && (redir->starttime + REDIR_HTTP_MAX_TIME) > mainclock_now()) {

    read_waiting = 0;

#ifdef HAVE_SSL
    if (sock->sslcon) {
      read_waiting = (openssl_pending(sock->sslcon) > 0);
    }
#endif
    
    if (!read_waiting) {

      /*
       *  If not already with data from the SSL layer, wait for it. 
       */
      
      do {
	
	FD_ZERO(&fds);
	FD_SET(fd, &fds);
	
	idleTime.tv_sec = 0;
	idleTime.tv_usec = REDIR_HTTP_SELECT_TIME;
	
	status = select(fd + 1, &fds, NULL, NULL, &idleTime);
	
      } while (status == -1 && errno == EINTR);
      
      switch(status) {
      case -1:
	log_err(errno,"select()");
	return -1;
      case 0:
	log_dbg("HTTP request timeout!");
	done = 1;
      default:
	break;
      }
    
      if ((status > 0) && FD_ISSET(fd, &fds)) {
	/*
	 *  We have data pending a read
	 */
	read_waiting = 1;
      }
    }

    if (read_waiting) {

      if (buflen + 2 >= sizeof(buffer)) { /* ensure space for a least one more byte + null */
        log_err(0, "Too much data in http request!");
        return -1;
      }

      /* if post is allowed, we do not buffer on the read (to not eat post data) */
#ifdef HAVE_SSL
      if (sock->sslcon) {
	recvlen = openssl_read(sock->sslcon, buffer + buflen, httpreq->allow_post ? 1 : sizeof(buffer) - 1 - buflen, 0);
      } else
#endif
      recvlen = recv(fd, buffer + buflen, httpreq->allow_post ? 1 : sizeof(buffer) - 1 - buflen, 0);

      if (recvlen < 0) {
	if (errno != ECONNRESET)
	  log_err(errno, "redir read() failed!");
	return -1;
      }

      if (recvlen == 0) done=1;
      else if (httpreq->data_in) {
	bcatblk(httpreq->data_in, buffer + buflen, recvlen);
      }

      buflen += recvlen;
      buffer[buflen] = 0;
    }

    if (buflen == 0) {
      log_dbg("No data in HTTP request!");
      return -1;
    }

    while ((eol = strstr(buffer, "\r\n"))) {
      size_t linelen = eol - buffer;
      *eol = 0;

      if (lines++ == 0) { /* first line */
	size_t dstlen = 0;
	char *p1 = buffer;
	char *p2;

	if (optionsdebug)
	  log_dbg("http-request: %s", buffer);

	if      (!strncmp("GET ",  p1, 4)) { p1 += 4; }
	else if (!strncmp("HEAD ", p1, 5)) { p1 += 5; }
	else if (httpreq->allow_post && !strncmp("POST ", p1, 5)) { 
	  p1 += 5; 
	  httpreq->is_post = 1; 
	} else { 
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

	if (dstlen >= sizeof(httpreq->path)-1) 
	  dstlen = sizeof(httpreq->path)-1;

	strncpy(path, p1, dstlen);

	if (optionsdebug)
	  log_dbg("The path: %s", path); 

	/* TODO: Should also check the Host: to make sure we are talking directly to uamlisten */

#ifdef ENABLE_JSON
	if (!strncmp(path, "json/", 5) && strlen(path) > 6) {
	  int i, last=strlen(path)-5;

	  conn->format = REDIR_FMT_JSON;

	  for (i=0; i < last; i++)
	    path[i] = path[i+5];

	  path[last]=0;

	  log_dbg("The (json format) path: %s", path); 
	}
#endif

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
	else if (!strcmp(path, "macreauth"))
	  { conn->type = REDIR_MACREAUTH; return 0; }
	else if (!strcmp(path, "abort"))
	  { conn->type = REDIR_ABORT; return 0; }

	if (*p2 == '?') {
	  p1 = p2 + 1;
	  p2 = strchr(p1, ' ');

	  if (p2) {
	    dstlen = p2 - p1;

	    if (dstlen >= sizeof(httpreq->qs)-1) 
	      dstlen = sizeof(httpreq->qs)-1;

	    strncpy(httpreq->qs, p1, dstlen);

	    if (optionsdebug)
	      log_dbg("Query string: %s", httpreq->qs); 
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
	size_t len;

	if (!strncasecmp(buffer,"Host:",5)) {
	  p = buffer + 5;
	  while (*p && isspace(*p)) p++;
	  len = strlen(p);
	  if (len >= sizeof(httpreq->host)-1)
	    len = sizeof(httpreq->host)-1;
	  strncpy(httpreq->host, p, len);
	  httpreq->host[len]=0;
	  if (optionsdebug)
	    log_dbg("Host: %s",httpreq->host);
	} 
	else if (!strncasecmp(buffer,"Content-Length:",15)) {
	  p = buffer + 15;
	  while (*p && isspace(*p)) p++;
	  len = strlen(p);
	  if (len > 0) httpreq->clen = atoi(p);
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
	else if (!strncasecmp(buffer,"Cookie:",7)) {
	  p = buffer + 7;
	  while (*p && isspace(*p)) p++;
	  len = strlen(p);
	  if (len >= sizeof(conn->httpcookie)-1)
	    len = sizeof(conn->httpcookie)-1;
	  strncpy(conn->httpcookie, p, len);
	  conn->httpcookie[len]=0;
	  if (optionsdebug)
	    log_dbg("Cookie: %s",conn->httpcookie);
	}
      }

      /* shift buffer */
      linelen += 2;
      for (i = 0; i < (int)(buflen - linelen); i++)
	buffer[i] = buffer[(int)linelen+i];

      buflen -= linelen;
    }
  }

  switch(conn->type) {

  case REDIR_STATUS:
    return 0;

  case REDIR_LOGIN:
    {
      bstring bt = bfromcstr("");

      if (!redir_getparam(redir, httpreq->qs, "lang", bt))
	bstrtocstr(bt, conn->lang, sizeof(conn->lang));
      
      if (!redir_getparam(redir, httpreq->qs, "ident", bt) && bt->slen)
	conn->chap_ident = atoi((char*)bt->data);
      
      if (redir_getparam(redir, httpreq->qs, "username", bt)) {
	log_err(0, "No username found in login request");
	bdestroy(bt);
	return -1;
      }

      bstrtocstr(bt, conn->s_state.redir.username, sizeof(conn->s_state.redir.username));
      log_dbg("-->> Setting username=[%s]",conn->s_state.redir.username);
      
      if (!redir_getparam(redir, httpreq->qs, "userurl", bt)) {
	bstring bt2 = bfromcstr("");
	redir_urldecode(bt, bt2);
	bstrtocstr(bt2, conn->s_state.redir.userurl, sizeof(conn->s_state.redir.userurl));
	if (optionsdebug) 
	  log_dbg("-->> Setting userurl=[%s]",conn->s_state.redir.userurl);
	bdestroy(bt2);
      }
      
      if (!redir_getparam(redir, httpreq->qs, "ntresponse", bt)) {
	redir_hextochar(bt->data, bt->slen, conn->chappassword, 24);
	conn->chap = 2;
	conn->password[0] = 0;
      }
      else if (!redir_getparam(redir, httpreq->qs, "response", bt)) {
	redir_hextochar(bt->data, bt->slen, conn->chappassword, RADIUS_CHAPSIZE);
	conn->chap = 1;
	conn->password[0] = 0;
      }
      else if (!redir_getparam(redir, httpreq->qs, "password", bt)) {
	conn->password_len = bt->slen / 2;
	log_dbg("Password length: %d",conn->password_len);
	if (conn->password_len > RADIUS_PWSIZE)
	  conn->password_len = RADIUS_PWSIZE;
	redir_hextochar(bt->data, bt->slen, conn->password, conn->password_len);
	conn->chap = 0;
	conn->chappassword[0] = 0;
      } else {
	if (optionsdebug) log_dbg("No password found!");
	bdestroy(bt);
	return -1;
      }
      bdestroy(bt);
    }
    break;

  case REDIR_PRELOGIN:
  case REDIR_LOGOUT:
    {
      bstring bt = bfromcstr("");
      if (!redir_getparam(redir, httpreq->qs, "userurl", bt)) {
	bstring bt2 = bfromcstr("");
	redir_urldecode(bt, bt2);
	bstrtocstr(bt2, conn->s_state.redir.userurl, sizeof(conn->s_state.redir.userurl));
	if (optionsdebug) 
	  log_dbg("-->> Setting userurl=[%s]",conn->s_state.redir.userurl);
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
      
      snprintf(conn->s_state.redir.userurl, sizeof(conn->s_state.redir.userurl), "http://%s/%s%s%s", 
	       httpreq->host, httpreq->path, httpreq->qs[0] ? "?" : "", httpreq->qs[0] ? httpreq->qs : "");

      if (optionsdebug) 
	log_dbg("-->> Setting userurl=[%s]",conn->s_state.redir.userurl);
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
    conn->response = REDIR_FAILED_OTHER;
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

  config_radius_session(&conn->s_params, pack, 0, 0);
  
  /* Class */
  if (!radius_getattr(pack, &classattr, RADIUS_ATTR_CLASS, 0, 0, 0)) {
    conn->s_state.redir.classlen = classattr->l-2;
    memcpy(conn->s_state.redir.classbuf, classattr->v.t, classattr->l-2);
    log_dbg("!!!! CLASSLEN = %d !!!!", conn->s_state.redir.classlen);
  }
  /*else {
    log_dbg("!!!! RESET CLASSLEN !!!!");
    conn->s_state.redir.classlen = 0;
    }*/

  if (pack->code != RADIUS_CODE_ACCESS_ACCEPT) {
    /* ACCESS-REJECT */
    conn->response = REDIR_FAILED_REJECT;
    return 0;
  }

  /* ACCESS-ACCEPT */

  /* State */
  if (!radius_getattr(pack, &stateattr, RADIUS_ATTR_STATE, 0, 0, 0)) {
    conn->s_state.redir.statelen = stateattr->l-2;
    memcpy(conn->s_state.redir.statebuf, stateattr->v.t, stateattr->l-2);
  }
  else {
    conn->s_state.redir.statelen = 0;
  }
  
  if (conn->s_params.sessionterminatetime) {
    time_t timenow = mainclock_now();
    if (timenow > conn->s_params.sessionterminatetime) {
      conn->response = REDIR_FAILED_OTHER;
      log_warn(0, "WISPr-Session-Terminate-Time in the past received: %s", attrs);
      return 0;
    }
  }
  
  conn->response = REDIR_SUCCESS;
  return 0;
}


/* Send radius Access-Request and wait for answer */
static int redir_radius(struct redir_t *redir, struct in_addr *addr,
			struct redir_conn_t *conn, char reauth) {
  uint8_t user_password[RADIUS_PWSIZE + 1];
  uint8_t chap_password[REDIR_MD5LEN + 2];
  uint8_t chap_challenge[REDIR_MD5LEN];
  struct radius_packet_t radius_pack;
  struct radius_t *radius;      /* Radius client instance */
  struct timeval idleTime;	/* How long to select() */
  time_t endtime, now;          /* for radius wait */
  int maxfd = 0;	        /* For select() */
  fd_set fds;			/* For select() */
  int status;

  MD5_CTX context;

  char mac[REDIR_MACSTRLEN+1];
  char url[REDIR_URL_LEN];
  int n, m;

  if (radius_new(&radius,
		 &redir->radiuslisten, 0, 0,
		 NULL, 0, NULL, NULL, NULL) ||
      radius_init_q(radius, 4)) {
    log_err(0, "Failed to create radius");
    return -1;
  }

  radius->nextid = radius_packet_id;

  if (radius->fd > maxfd)
    maxfd = radius->fd;

  radius_set(radius, redir->nas_hwaddr, (_options.debug & DEBUG_RADIUS));
  
  radius_set_cb_auth_conf(radius, redir_cb_radius_auth_conf);

  radius_default_pack(radius, &radius_pack, RADIUS_CODE_ACCESS_REQUEST);
  
  if (optionsdebug) 
    log_dbg("created radius packet (code=%d, id=%d, len=%d)\n",
	    radius_pack.code, radius_pack.id, ntohs(radius_pack.length));
  
  radius_addattr(radius, &radius_pack, RADIUS_ATTR_USER_NAME, 0, 0, 0,
		 (uint8_t*) conn->s_state.redir.username, strlen(conn->s_state.redir.username));

  /* If lang on logon url, then send it with attribute ChilliSpot-Lang */
  if(conn->lang[0]) 
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC, 
		   RADIUS_VENDOR_CHILLISPOT, RADIUS_ATTR_CHILLISPOT_LANG, 
		   0, (uint8_t*) conn->lang, strlen(conn->lang));

  if (_options.radiusoriginalurl)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC, 
		   RADIUS_VENDOR_CHILLISPOT, RADIUS_ATTR_CHILLISPOT_ORIGINALURL, 
		   0, (uint8_t*) conn->s_state.redir.userurl, strlen(conn->s_state.redir.userurl));


  if (redir->secret && *redir->secret) {
    /* fprintf(stderr,"SECRET: [%s]\n",redir->secret); */
    /* Get MD5 hash on challenge and uamsecret */
    MD5Init(&context);
    MD5Update(&context, conn->s_state.redir.uamchal, REDIR_MD5LEN);
    MD5Update(&context, (uint8_t *) redir->secret, strlen(redir->secret));
    MD5Final(chap_challenge, &context);
  }
  else {
    memcpy(chap_challenge, conn->s_state.redir.uamchal, REDIR_MD5LEN);
  }

  
  if (conn->chap == 0) {

    /*
     * decode password - encoded by the UAM portal/script. 
     */
    for (m=0; m < RADIUS_PWSIZE;) 
      for (n=0; n < REDIR_MD5LEN; m++, n++)
	user_password[m] = conn->password[m] ^ chap_challenge[n];
    
    user_password[conn->password_len] = 0;

#ifdef HAVE_OPENSSL
    if (_options.mschapv2) {
      uint8_t response[50];
      uint8_t ntresponse[24];
      
      /*uint8_t peer_challenge[16];
	redir_challenge(peer_challenge);*/
      
      GenerateNTResponse(chap_challenge, /*peer*/chap_challenge,
			 conn->s_state.redir.username, strlen(conn->s_state.redir.username),
			 user_password, conn->password_len,
			 ntresponse);
      
      /* peer challenge - same as auth challenge */
      memset(&response[0], 0, sizeof(response));
      memcpy(&response[2], /*peer*/chap_challenge, 16); 
      memcpy(&response[26], ntresponse, 24);
      
      radius_addattr(radius, &radius_pack, 
		     RADIUS_ATTR_VENDOR_SPECIFIC,
		     RADIUS_VENDOR_MS, RADIUS_ATTR_MS_CHAP_CHALLENGE, 0,
		     chap_challenge, 16);
      
      radius_addattr(radius, &radius_pack, 
		     RADIUS_ATTR_VENDOR_SPECIFIC,
		     RADIUS_VENDOR_MS, RADIUS_ATTR_MS_CHAP2_RESPONSE, 0,
		     response, 50);
    } else {
#endif
      
      radius_addattr(radius, &radius_pack, RADIUS_ATTR_USER_PASSWORD, 0, 0, 0,
		     user_password, conn->password_len);

#ifdef HAVE_OPENSSL
    }
#endif

  }
  else if (conn->chap == 1) {
    chap_password[0] = conn->chap_ident; /* Chap ident found on logon url */
    memcpy(chap_password+1, conn->chappassword, REDIR_MD5LEN);

    radius_addattr(radius, &radius_pack, RADIUS_ATTR_CHAP_CHALLENGE, 0, 0, 0,
		   chap_challenge, REDIR_MD5LEN);

    radius_addattr(radius, &radius_pack, RADIUS_ATTR_CHAP_PASSWORD, 0, 0, 0,
		   chap_password, REDIR_MD5LEN+1);
  }
  else if (conn->chap == 2) {
    uint8_t response[50];

    /* peer challenge - same as auth challenge */
    memcpy(response + 2, chap_challenge, 16); 
    memcpy(response + 26, conn->chappassword, 24);

    radius_addattr(radius, &radius_pack, 
		   RADIUS_ATTR_VENDOR_SPECIFIC,
		   RADIUS_VENDOR_MS, RADIUS_ATTR_MS_CHAP_CHALLENGE, 0,
		   chap_challenge, 16);

    radius_addattr(radius, &radius_pack, 
		   RADIUS_ATTR_VENDOR_SPECIFIC,
		   RADIUS_VENDOR_MS, RADIUS_ATTR_MS_CHAP2_RESPONSE, 0,
		   response, 50);
  }

  radius_addnasip(radius, &radius_pack);

  radius_addattr(radius, &radius_pack, RADIUS_ATTR_SERVICE_TYPE, 0, 0,
		 _options.framedservice ? RADIUS_SERVICE_TYPE_FRAMED :
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
		 (uint8_t*) conn->s_state.sessionid, REDIR_SESSIONID_LEN-1);

  log_dbg("!!!! CLASSLEN = %d !!!!", conn->s_state.redir.classlen);
  if (conn->s_state.redir.classlen) {
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_CLASS, 0, 0, 0,
		   conn->s_state.redir.classbuf,
		   conn->s_state.redir.classlen);
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

  if (_options.openidauth)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
		   RADIUS_VENDOR_CHILLISPOT, RADIUS_ATTR_CHILLISPOT_CONFIG, 
		   0, (uint8_t*)"allow-openidauth", 16);

#ifdef ENABLE_IEEE8021Q
  if (conn->s_state.tag8021q)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
		   RADIUS_VENDOR_CHILLISPOT, RADIUS_ATTR_CHILLISPOT_VLAN_ID, 
		   (uint32_t)(ntohl(conn->s_state.tag8021q) & 0x0FFF), 0, 0);
#endif

  radius_addattr(radius, &radius_pack, RADIUS_ATTR_MESSAGE_AUTHENTICATOR, 
		 0, 0, 0, NULL, RADIUS_MD5LEN);

  if (optionsdebug) 
    log_dbg("sending radius packet (code=%d, id=%d, len=%d)\n",
	    radius_pack.code, radius_pack.id, ntohs(radius_pack.length));

  radius_req(radius, &radius_pack, conn);

  now = mainclock_now();
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
	  radius_decaps(radius, 0) < 0) {
	log_err(0, "radius_ind() failed!");
      }
      
      if ((radius->proxyfd != -1) && FD_ISSET(radius->proxyfd, &fds) && 
	  radius_proxy_ind(radius, 0) < 0) {
	log_err(0, "radius_proxy_ind() failed!");
      }
    }
  
    if (conn->response) {
      radius_free(radius);
      return 0;
    }

    now = mainclock_now();
  }

  return 0;
}

int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0) return -1;
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  return 0;
}

int clear_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0) return -1;
  fcntl(fd, F_SETFL, flags & (~O_NONBLOCK));
  return 0;
}

int is_local_user(struct redir_t *redir, struct redir_conn_t *conn) {
  uint8_t user_password[RADIUS_PWSIZE+1];
  uint8_t chap_challenge[REDIR_MD5LEN];
  char u[256]; char p[256];
  size_t usernamelen, sz=1024;
  ssize_t len;
  int match=0;
  char *line=0;
  MD5_CTX context;
  FILE *f;

  if (!_options.localusers) return 0;

  log_dbg("checking %s for user %s", _options.localusers, conn->s_state.redir.username);

  if (!(f = fopen(_options.localusers, "r"))) {
    log_err(errno, "fopen() failed opening %s!", _options.localusers);
    return 0;
  }

  if (_options.debug) {/*debug*/
    char buffer[64];
    redir_chartohex(conn->s_state.redir.uamchal, buffer, REDIR_MD5LEN);
    log_dbg("challenge: %s", buffer);
  }/**/

  if (redir->secret && *redir->secret) {
    MD5Init(&context);
    MD5Update(&context, (uint8_t*)conn->s_state.redir.uamchal, REDIR_MD5LEN);
    MD5Update(&context, (uint8_t*)redir->secret, strlen(redir->secret));
    MD5Final(chap_challenge, &context);
  }
  else {
    memcpy(chap_challenge, conn->s_state.redir.uamchal, REDIR_MD5LEN);
  }

  if (_options.debug) {/*debug*/
    char buffer[64];
    redir_chartohex(chap_challenge, buffer, REDIR_MD5LEN);
    log_dbg("chap challenge: %s", buffer);
  }/**/

  if (conn->chap == 0) {
    int n, m;
    for (m=0; m < RADIUS_PWSIZE;)
      for (n=0; n < REDIR_MD5LEN; m++, n++)
	user_password[m] = conn->password[m] ^ chap_challenge[n];
  }
  else if (conn->chap == 1) {
    memcpy(user_password, conn->chappassword, REDIR_MD5LEN);
  }
  
  user_password[RADIUS_PWSIZE] = 0;
	
  log_dbg("looking for %s", conn->s_state.redir.username);
  usernamelen = strlen(conn->s_state.redir.username);

  line=(char*)malloc(sz);
  while ((len = getline(&line, &sz, f)) > 0) {
    if (len > 3 && len < sizeof(u) && line[0] != '#') {
      char *pl=line,  /* pointer to current line */
	   *pu=u,     /* pointer to username     */
  	   *pp=p;     /* pointer to password     */

      /* username until the first ':' */
      while (*pl && *pl != ':')	*pu++ = *pl++;

      /* skip over ':' otherwise error */
      if (*pl == ':') pl++;
      else {
	log_warn(0, "not a valid localusers line: %s", line);
	continue;
      }

      /* password until the next ':' */
      while (*pl && *pl != ':' && *pl != '\n') *pp++ = *pl++;

      *pu = 0; /* null terminate */
      *pp = 0;

      if (usernamelen == strlen(u) &&
	  !strncmp(conn->s_state.redir.username, u, usernamelen)) {

	log_dbg("found %s, checking password", u);

	if (conn->chap == 0) {

	  if (!strcmp((char*)user_password, p))
	    match = 1;

	}
	else if (conn->chap == 1) {
	  unsigned char tmp[REDIR_MD5LEN];

	  MD5Init(&context);
	  MD5Update(&context, (uint8_t*)&conn->chap_ident, 1);	  
	  MD5Update(&context, (uint8_t*)p, strlen(p));
	  MD5Update(&context, chap_challenge, REDIR_MD5LEN);
	  MD5Final(tmp, &context);

	  if (!memcmp(user_password, tmp,  REDIR_MD5LEN)) 
	    match = 1; 
	}

	break;
      }
    }
  }
  
  log_dbg("user %s %s", conn->s_state.redir.username, match ? "found" : "not found");

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
  socklen_t addrlen;
  char buffer[128];

  addrlen = sizeof(struct sockaddr_in);

  if ((new_socket = accept(redir->fd[idx], (struct sockaddr *)&address, &addrlen)) < 0) {
    if (errno != ECONNABORTED)
      log_err(errno, "accept() failed!");
    return 0;
  }

  radius_packet_id++;

  /* This forks a new process. The child really should close all
     unused file descriptors and free memory allocated. This however
     is performed when the process exits, so currently we don't
     care */

  if ((status = redir_fork(new_socket, new_socket)) < 0) {
    log_err(errno, "fork() returned -1!");
    close(new_socket);
    return 0;
  }

  if (status > 0) { /* parent */
    close(new_socket);
    return 0; 
  }

  snprintf(buffer,sizeof(buffer)-1,"%s",inet_ntoa(address.sin_addr));
  setenv("TCPREMOTEIP",buffer,1);
  setenv("REMOTE_ADDR",buffer,1);
  snprintf(buffer,sizeof(buffer)-1,"%d",ntohs(address.sin_port));
  setenv("TCPREMOTEPORT",buffer,1);
  setenv("REMOTE_PORT",buffer,1);

  if (idx == 1 && _options.uamui) {

    char *binqqargs[2] = { _options.uamui, 0 } ;

    execv(*binqqargs, binqqargs);

  } else {

    return redir_main(redir, 0, 1, &address, idx, 1);

  }

  return 0;
}

static int _redir_close(int infd, int outfd) {
  /*
  char b[128];
  int max = 1000;

  if (shutdown(outfd, SHUT_WR) != 0)
    log_dbg("shutdown socket for writing");
  
  if (!set_nonblocking(infd)) 
    while(read(infd, b, sizeof(b)) > 0 && max--);
  
  if (shutdown(infd, SHUT_RD) != 0)
    log_dbg("shutdown socket for reading");
  */
  close(outfd);
  close(infd);
  return 0;
}

static int _redir_close_exit(int infd, int outfd) {
  _redir_close(infd,outfd);
  options_destroy();
  exit(0);
}

#ifdef USING_IPC_UNIX
int redir_send_msg(struct redir_t *this, struct redir_msg_t *msg) {
  struct sockaddr_un remote; 
  size_t len = sizeof(remote);
  int s;

  char *statedir = _options.statedir ? _options.statedir : DEFSTATEDIR;
  char filedest[512];

  snprintf(filedest, sizeof(filedest), "%s/%s", statedir, 
	   _options.unixipc ? _options.unixipc : "chilli.ipc");

  if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    perror("socket");
    return -1;
  }

  remote.sun_family = AF_UNIX;
  strcpy(remote.sun_path, filedest);

#if defined (__FreeBSD__)  || defined (__APPLE__) || defined (__OpenBSD__)
  remote.sun_len = strlen(remote.sun_path) + 1;
#endif

  len = offsetof(struct sockaddr_un, sun_path) + strlen(remote.sun_path);

  if (connect(s, (struct sockaddr *)&remote, len) == -1) {
    log_err(errno, "could not connect to %s", remote.sun_path);
    close(s);
    return -1;
  }
  
  if (write(s, msg, sizeof(*msg)) != sizeof(*msg)) {
    log_err(errno, "could not write to %s", remote.sun_path);
    close(s);
    return -1;
  }

  shutdown(s,2);
  close(s);
  return 0;
}
#endif

pid_t redir_fork(int in, int out) {
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    struct sigaction act, oldact;
    struct itimerval itval;

    memset(&act, 0, sizeof(act));

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

#if defined(F_DUPFD)
    if (fcntl(in,F_GETFL,0) == -1) return -1; close(0);
    if (fcntl(in,F_DUPFD,0) == -1) return -1;
    if (fcntl(out,F_GETFL,1) == -1) return -1; close(1);
    if (fcntl(out,F_DUPFD,1) == -1) return -1;
#else
    if (dup2(in,0) == -1) return -1;
    if (dup2(out,1) == -1) return -1;
#endif
  }
  return pid;
}

int redir_main_exit(struct redir_t *redir, struct redir_httpreq_t *httpreq, 
		    struct redir_socket_t *socket, int forked) {
  if (httpreq->data_in) bdestroy(httpreq->data_in);
  if (forked) _redir_close_exit(socket->fd[0], socket->fd[1]);
  return _redir_close(socket->fd[0], socket->fd[1]);
}

int redir_main(struct redir_t *redir, 
	       int infd, int outfd, 
	       struct sockaddr_in *address, 
	       int isui, int forked) {
  char hexchal[1+(2*REDIR_MD5LEN)];
  unsigned char challenge[REDIR_MD5LEN];
  size_t bufsize = REDIR_MAXBUFFER;
  char buffer[bufsize+1];
  struct redir_msg_t msg;
  ssize_t buflen;

  /**
   * connection state 
   *  0 == un-authenticated
   *  1 == authenticated
   */
  int state = 0;

  /**
   * require splash or not
   */
  int splash = 0;

  struct redir_conn_t conn;
  struct redir_socket_t socket;
  struct redir_httpreq_t httpreq;

  memset(&httpreq,0,sizeof(httpreq));
  httpreq.allow_post = isui || _options.uamallowpost;

  mainclock_tick();

  if (!forked) {
    httpreq.data_in = bfromcstr("");
  }

#define redir_memcopy(msgtype) \
  redir_challenge(challenge); \
  redir_chartohex(challenge, hexchal, REDIR_MD5LEN); \
  msg.mtype = msgtype; \
  memcpy(conn.s_state.redir.uamchal, challenge, REDIR_MD5LEN); \
  if (_options.debug) { \
    log_dbg("---->>> resetting challenge: %s", hexchal); \
  }

#ifdef USING_IPC_UNIX
#define redir_msg_send(msgopt) \
  msg.mdata.opt = msgopt; \
  msg.mdata.addr = address->sin_addr; \
  memcpy(&msg.mdata.params, &conn.s_params, sizeof(msg.mdata.params)); \
  memcpy(&msg.mdata.redir, &conn.s_state.redir, sizeof(msg.mdata.redir)); \
  if (redir_send_msg(redir, &msg) < 0) { \
    log_err(errno, "write() failed! msgfd=%d type=%d len=%d", redir->msgfd, msg.mtype, sizeof(msg.mdata)); \
    return redir_main_exit(redir, &httpreq, &socket, forked); \
  } 
#else
#define redir_msg_send(msgopt) \
  msg.mdata.opt = msgopt; \
  msg.mdata.addr = address->sin_addr; \
  memcpy(&msg.mdata.params, &conn.s_params, sizeof(msg.mdata.params)); \
  memcpy(&msg.mdata.redir, &conn.s_state.redir, sizeof(msg.mdata.redir)); \
  if (msgsnd(redir->msgid, (void *)&msg, sizeof(msg.mdata), 0) < 0) { \
    log_err(errno, "msgsnd() failed! msgid=%d type=%d len=%d", redir->msgid, msg.mtype, sizeof(msg.mdata)); \
    return redir_main_exit(redir, &httpreq, &socket, forked); \
  } 
#endif

  /*
   *  Initializations
   */
  memset(&socket, 0, sizeof(socket));
  memset(hexchal, 0, sizeof(hexchal));
  memset(&conn, 0, sizeof(conn));
  memset(&msg, 0, sizeof(msg));

  socket.fd[0] = infd;
  socket.fd[1] = outfd;

  redir->starttime = mainclock_now();

  /*
  if (set_nonblocking(socket.fd[0])) {
    log_err(errno, "fcntl() failed");
    return redir_main_exit(redir, &httpreq, &socket, forked);
  }
  */

  if (optionsdebug) 
    log_dbg("Calling redir_getstate()");

  /*
   *  Fetch the state of the client
   */

  termstate = REDIR_TERM_GETSTATE;

  if (!redir->cb_getstate) { 
    log_err(0, "No cb_getstate() defined!"); 
    return redir_main_exit(redir, &httpreq, &socket, forked);
  }

  /* get_state returns 0 for unauth'ed and 1 for auth'ed */
  state = redir->cb_getstate(redir, address,  &conn);

  if (state == -1) {
    if (!_options.debug || !isui)
      return redir_main_exit(redir, &httpreq, &socket, forked);
  }

  splash = (conn.s_params.flags & REQUIRE_UAM_SPLASH) == REQUIRE_UAM_SPLASH;

  /*
   *  Parse the request, updating the status
   */
  if (optionsdebug) 
    log_dbg("Receiving HTTP%s Request", conn.flags & USING_SSL ? "S" : "");

#ifdef HAVE_SSL
  if (_options.uamuissl && isui) {
    socket.sslcon = openssl_accept_fd(initssl(), socket.fd[0], 30);
  }
  else if (conn.flags & USING_SSL) {
    socket.sslcon = openssl_accept_fd(initssl(), socket.fd[0], 30);
  }
#endif

  termstate = REDIR_TERM_GETREQ;
  if (redir_getreq(redir, &socket, &conn, &httpreq)) {
    log_dbg("Error calling get_req. Terminating\n");
    return redir_main_exit(redir, &httpreq, &socket, forked);
  }

  if (optionsdebug) 
    log_dbg("Process HTTP Request");
  
  if (conn.type == REDIR_WWW) {
    pid_t forkpid;
    int fd = -1;

    if (_options.wwwdir && conn.wwwfile && *conn.wwwfile) {
      char *ctype = "text/plain";
      char *filename = conn.wwwfile;
      size_t namelen = strlen(filename);
      int parse = 0;
      
      /* check filename */
      { char *p;
	for (p=filename; *p; p++) {
	  if (*p >= 'a' && *p <= 'z') continue;
	  if (*p >= 'A' && *p <= 'Z') continue;
	  if (*p >= '0' && *p <= '9') continue;
	  if (*p == '.' || *p == '_'|| *p == '-') continue;
	  /* invalid file name! */
	  log_err(0, "invalid www request [%s]!", filename);
	  return redir_main_exit(redir, &httpreq, &socket, forked);
	}
      }
      
      /* serve the local content */
      
      if      (!strcmp(filename + (namelen - 5), ".html")) ctype = "text/html";
      else if (!strcmp(filename + (namelen - 4), ".gif"))  ctype = "image/gif";
      else if (!strcmp(filename + (namelen - 3), ".js"))   ctype = "text/javascript";
      else if (!strcmp(filename + (namelen - 4), ".css"))  ctype = "text/css";
      else if (!strcmp(filename + (namelen - 4), ".jpg"))  ctype = "image/jpeg";
      else if (!strcmp(filename + (namelen - 4), ".dat"))  ctype = "application/x-ns-proxy-autoconfig";
      else if (!strcmp(filename + (namelen - 4), ".png"))  ctype = "image/png";
      else if (!strcmp(filename + (namelen - 4), ".swf"))  ctype = "application/x-shockwave-flash";
      else if (!strcmp(filename + (namelen - 4), ".chi")){ ctype = "text/html"; parse = 1; }
      else { 
	/* we do not serve it! */
	log_err(0, "invalid file extension! [%s]", filename);
	return redir_main_exit(redir, &httpreq, &socket, forked);
      }


      if (!forked) {
	/*
	 *  If not forked off the main process already, fork now
	 *  before doing the chroot(), chrdir(), and so on..
	 */
	forkpid = redir_fork(infd, outfd);
	if (forkpid) { /* parent */
	  return redir_main_exit(redir, &httpreq, &socket, forked);
	}
      }

      if (parse) {

	if (!_options.wwwbin) {
	  log_err(0, "the 'wwwbin' setting must be configured for CGI use");
	  return redir_main_exit(redir, &httpreq, &socket, forked);
	}

	if (clear_nonblocking(socket.fd[0])) {
	  log_err(errno, "fcntl() failed");
	}

	/* XXX: Todo: look for malicious content! */
	
	{
	  char *binqqargs[3] = { _options.wwwbin, buffer, 0 } ;

	  sprintf(buffer,"%d", httpreq.clen > 0 ? httpreq.clen : 0);
	  setenv("CONTENT_LENGTH", buffer, 1);
	  
	  setenv("REQUEST_METHOD", httpreq.is_post ? "POST" : "GET", 1);
	  setenv("REQUEST_URI", httpreq.path, 1);
	  setenv("QUERY_STRING", httpreq.qs, 1);
	  setenv("SERVER_NAME", httpreq.host, 1);
	  setenv("HTTP_COOKIE", conn.httpcookie, 1);
	  
	  sprintf(buffer, "%.2X-%.2X-%.2X-%.2X-%.2X-%.2X",
		  conn.hismac[0], conn.hismac[1], conn.hismac[2], 
		  conn.hismac[3], conn.hismac[4], conn.hismac[5]);
	  
	  setenv("REMOTE_MAC", buffer, 1);
	  setenv("AUTHENTICATED", conn.s_state.authenticated ? "1" : "0", 1);
	  setenv("CHI_SESSION_ID", conn.s_state.sessionid, 1);
	  setenv("CHI_USERNAME", conn.s_state.redir.username, 1);
	  setenv("CHI_USERURL", conn.s_state.redir.userurl, 1);
	  sprintf(buffer, "%ld", conn.s_state.input_octets);
	  setenv("CHI_INPUT_BYTES", buffer, 1);
	  sprintf(buffer, "%ld", conn.s_state.output_octets);
	  setenv("CHI_OUTPUT_BYTES", buffer, 1);
	  sprintf(buffer, "%ld", conn.s_params.sessiontimeout);
	  setenv("CHI_SESSION_TIMEOUT", buffer, 1);
	  
	  redir_chartohex(conn.s_state.redir.uamchal, buffer, REDIR_MD5LEN);
	  setenv("CHI_CHALLENGE", buffer, 1);
	  
	  log_dbg("Running: %s %s/%s",_options.wwwbin, _options.wwwdir, filename);
	  sprintf(buffer, "%s/%s", _options.wwwdir, filename);
	  
	  execv(*binqqargs, binqqargs);
	}
	
	return redir_main_exit(redir, &httpreq, &socket, forked);
      }

      if (!chroot(_options.wwwdir) && !chdir("/")) {
	
	fd = open(filename, O_RDONLY);
	
	if (fd > 0) {
	  
	  if (clear_nonblocking(socket.fd[0])) {
	    log_err(errno, "fcntl() failed");
	  }
	  
	  buflen = snprintf(buffer, bufsize,
			    "HTTP/1.1 200 OK\r\n"
			    "Connection: close\r\n"
			    "Content-type: %s\r\n\r\n", ctype);
	  
	  if (tcp_write(&socket, buffer, (size_t) buflen) < 0) {
	    log_err(errno, "tcp_write() failed!");
	  }
	  
	  while ((buflen = read(fd, buffer, bufsize)) > 0)
	    if (tcp_write(&socket, buffer, (size_t) buflen) < 0)
	      log_err(errno, "tcp_write() failed!");
	  
	  close(fd);
	} 
	else log_err(0, "could not open local content file %s!", filename);
      }
      else log_err(0, "chroot to %s was not successful\n", _options.wwwdir); 

      return _redir_close_exit(infd, outfd); /* which exits */
    }
    else log_err(0, "Required: 'wwwdir' (in chilli.conf) and 'file' query-string param\n"); 
    
    return redir_main_exit(redir, &httpreq, &socket, forked);
  }

  termstate = REDIR_TERM_PROCESS;
  if (optionsdebug) log_dbg("Processing received request");

  /* default hexchal for use in replies */
  redir_chartohex(conn.s_state.redir.uamchal, hexchal, REDIR_MD5LEN);

  switch (conn.type) {

  case REDIR_LOGIN: {
    char reauth = 0;
    
    /* Was client was already logged on? */
    if (state == 1) {
      if (splash) {
	log_dbg("redir_accept: SPLASH reauth");
	reauth = 1;
      } else {
	log_dbg("redir_accept: already logged on");
	redir_reply(redir, &socket, &conn, REDIR_ALREADY, NULL, 0, 
		    NULL, NULL, conn.s_state.redir.userurl, NULL,
		    NULL, conn.hismac, &conn.hisip, httpreq.qs);
	return redir_main_exit(redir, &httpreq, &socket, forked);
      }
    }

    /* Did the challenge expire? */
    if (_options.challengetimeout2 && 
	(conn.s_state.uamtime + _options.challengetimeout2) < mainclock_now()) {
      log_dbg("redir_accept: challenge expired: %d : %d", conn.s_state.uamtime, mainclock_now());

      redir_memcopy(REDIR_CHALLENGE);      
      redir_msg_send(REDIR_MSG_OPT_REDIR);

      redir_reply(redir, &socket, &conn, REDIR_FAILED_OTHER, NULL, 
		  0, hexchal, NULL, NULL, NULL, 
		  NULL, conn.hismac, &conn.hisip, httpreq.qs);

      return redir_main_exit(redir, &httpreq, &socket, forked);
    }

    if (is_local_user(redir, &conn)) { 
       conn.response = REDIR_SUCCESS;
    }
    else {
      termstate = REDIR_TERM_RADIUS;

      if (optionsdebug) 
	log_dbg("redir_accept: Sending RADIUS request");

      redir_radius(redir, &address->sin_addr, &conn, reauth);
      termstate = REDIR_TERM_REPLY;

      if (optionsdebug) 
	log_dbg("Received RADIUS reply");
    }

    if (conn.response == REDIR_SUCCESS) { /* Accept-Accept */

      conn.s_params.flags &= ~REQUIRE_UAM_SPLASH;

      if (reauth) {
	conn.s_params.flags |= IS_UAM_REAUTH;
      }

      msg.mtype = REDIR_LOGIN;
      
      redir_reply(redir, &socket, &conn, REDIR_SUCCESS, NULL, conn.s_params.sessiontimeout,
		  NULL, conn.s_state.redir.username, conn.s_state.redir.userurl, conn.reply, 
		  (char *)conn.s_params.url, conn.hismac, &conn.hisip, httpreq.qs);
      
      /* set params and redir data */
      redir_msg_send(REDIR_MSG_OPT_REDIR | REDIR_MSG_OPT_PARAMS);

    } else { /* Access-Reject */

      bstring besturl = bfromcstr((char *)conn.s_params.url);
      int hasnexturl = (besturl && besturl->slen > 5);

      if (!hasnexturl) {
	redir_memcopy(REDIR_CHALLENGE);
      } else {
	msg.mtype = REDIR_NOTYET;
      }

      redir_reply(redir, &socket, &conn, REDIR_FAILED_REJECT, hasnexturl ? besturl : NULL,
		  0, hexchal, NULL, conn.s_state.redir.userurl, conn.reply,
		  (char *)conn.s_params.url, conn.hismac, &conn.hisip, httpreq.qs);

      bdestroy(besturl);

      /* set params, redir data, and reset session-id */
      redir_msg_send(REDIR_MSG_OPT_REDIR | REDIR_MSG_OPT_PARAMS | REDIR_MSG_NSESSIONID);
    }    

    if (optionsdebug) log_dbg("-->> Msg userurl=[%s]\n",conn.s_state.redir.userurl);
    return redir_main_exit(redir, &httpreq, &socket, forked);
  }

  case REDIR_LOGOUT:
    {
      redir_memcopy(REDIR_LOGOUT); 
      redir_msg_send(REDIR_MSG_OPT_REDIR);

      conn.s_state.authenticated=0;
      
      redir_reply(redir, &socket, &conn, REDIR_LOGOFF, NULL, 0, 
		  hexchal, NULL, conn.s_state.redir.userurl, NULL, 
		  NULL, conn.hismac, &conn.hisip, httpreq.qs);
      
      return redir_main_exit(redir, &httpreq, &socket, forked);
    }
    
  case REDIR_MACREAUTH:
    if (_options.macauth) {
      msg.mtype = REDIR_MACREAUTH;
      redir_msg_send(0);
    }
    /* drop down */

  case REDIR_PRELOGIN:
    /* Did the challenge expire? */
    if (_options.challengetimeout &&
	(conn.s_state.uamtime + _options.challengetimeout) < mainclock_now()) {
      redir_memcopy(REDIR_CHALLENGE);
      redir_msg_send(REDIR_MSG_OPT_REDIR);
    }
    
    if (state == 1) {
      redir_reply(redir, &socket, &conn, REDIR_ALREADY, 
		  NULL, 0, NULL, NULL, conn.s_state.redir.userurl, NULL,
		  NULL, conn.hismac, &conn.hisip, httpreq.qs);
    }
    else {
      redir_reply(redir, &socket, &conn, REDIR_NOTYET, 
		  NULL, 0, hexchal, NULL, conn.s_state.redir.userurl, NULL, 
		  NULL, conn.hismac, &conn.hisip, httpreq.qs);
    }
    return redir_main_exit(redir, &httpreq, &socket, forked);

  case REDIR_ABORT:
    if (state == 1) {
      redir_reply(redir, &socket, &conn, REDIR_ABORT_NAK, 
		  NULL, 0, NULL, NULL, conn.s_state.redir.userurl, NULL, 
		  NULL, conn.hismac, &conn.hisip, httpreq.qs);
    }
    else {
      redir_memcopy(REDIR_ABORT);
      redir_msg_send(0);

      redir_reply(redir, &socket, &conn, REDIR_ABORT_ACK, 
		  NULL, 0, hexchal, NULL, conn.s_state.redir.userurl, NULL, 
		  NULL, conn.hismac, &conn.hisip, httpreq.qs);
    }
    return redir_main_exit(redir, &httpreq, &socket, forked);

  case REDIR_ABOUT:
    redir_reply(redir, &socket, &conn, REDIR_ABOUT, NULL, 
		0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, httpreq.qs);
    return redir_main_exit(redir, &httpreq, &socket, forked);

  case REDIR_STATUS:
    {
      uint32_t sessiontime;
      uint32_t timeleft;
      time_t timenow = mainclock_now();

      /* Did the challenge expire? */
      if (_options.challengetimeout &&
	  (conn.s_state.uamtime + _options.challengetimeout) < timenow) {
	redir_memcopy(REDIR_CHALLENGE);
	redir_msg_send(REDIR_MSG_OPT_REDIR);
      }
      
      sessiontime = timenow - conn.s_state.start_time;

      if (conn.s_params.sessiontimeout)
	timeleft = conn.s_params.sessiontimeout - sessiontime;
      else
	timeleft = 0;

      redir_reply(redir, &socket, &conn, REDIR_STATUS, NULL, timeleft,
		  hexchal, conn.s_state.redir.username, conn.s_state.redir.userurl, conn.reply, 
		  (char *)conn.s_params.url, conn.hismac, &conn.hisip, httpreq.qs);
      
      return redir_main_exit(redir, &httpreq, &socket, forked);
    }

  case REDIR_MSDOWNLOAD:
    buflen = snprintf(buffer, bufsize, "HTTP/1.1 403 Forbidden\r\n\r\n");
    tcp_write(&socket, buffer, buflen);
    return redir_main_exit(redir, &httpreq, &socket, forked);

  }

  /* 
   *  It was not a request for a known path. 
   *  It must be an original request 
   */
  if (optionsdebug) 
    log_dbg("redir_accept: Original request");


  if (redir->cb_handle_url) {
    switch (redir->cb_handle_url(redir, &conn, &httpreq, &socket, redir->cb_handle_url_ctx)) {
    case -1: 
      return redir_main_exit(redir, &httpreq, &socket, forked);
    case 0: 
      return 0;
    default:
      break;
    }
  }


  /* Did the challenge expire? */
  if (_options.challengetimeout &&
      (conn.s_state.uamtime + _options.challengetimeout) < mainclock_now()) {
    redir_memcopy(REDIR_CHALLENGE);
    redir_msg_send(REDIR_MSG_OPT_REDIR);
  }
  else {
    redir_chartohex(conn.s_state.redir.uamchal, hexchal, REDIR_MD5LEN);
    /*
	redir_memcopy(REDIR_CHALLENGE);
	redir_msg_send(REDIR_MSG_OPT_REDIR);
    */
  }

  log_dbg("---->>> challenge: %s", hexchal);

  if (_options.macreauth && 
      !conn.s_state.authenticated) {
    msg.mtype = REDIR_MACREAUTH;
    redir_msg_send(0);
  }

  if (redir->homepage ||
      ((conn.s_params.flags & REQUIRE_REDIRECT) && conn.s_params.url[0])) {

    char * base_url = (conn.s_params.flags & REQUIRE_REDIRECT && 
		       conn.s_params.url[0]) ? (char *)conn.s_params.url : redir->homepage;

    bstring url = bfromcstralloc(1024,"");
    bstring urlenc = bfromcstralloc(1024,"");

    char *resp = splash ? "splash" : "notyet";

    if (redir_buildurl(&conn, url, redir, resp, 0, hexchal, NULL,
		       conn.s_state.redir.userurl, NULL, NULL, conn.hismac, &conn.hisip) == -1) {
      bdestroy(url);
      bdestroy(urlenc);
      log_err(errno, "redir_buildurl failed!");
      return redir_main_exit(redir, &httpreq, &socket, forked);
    }

    redir_urlencode(url, urlenc);

    bassignformat(url, "%s%cloginurl=", base_url,
		  strchr(base_url, '?') ? '&' : '?');

    bconcat(url, urlenc);

    redir_reply(redir, &socket, &conn, splash ? REDIR_SPLASH : REDIR_NOTYET, url, 
		0, hexchal, NULL, conn.s_state.redir.userurl, NULL, 
		NULL, conn.hismac, &conn.hisip, httpreq.qs);

    bdestroy(url);
    bdestroy(urlenc);
  }
  else if (state == 1) {
    redir_reply(redir, &socket, &conn, splash ? REDIR_SPLASH : REDIR_ALREADY, NULL, 0, 
		splash ? hexchal : NULL, NULL, conn.s_state.redir.userurl, NULL,
		NULL, conn.hismac, &conn.hisip, httpreq.qs);
  }
  else {
    redir_reply(redir, &socket, &conn, splash ? REDIR_SPLASH : REDIR_NOTYET, NULL, 
		0, hexchal, NULL, conn.s_state.redir.userurl, NULL, 
		NULL, conn.hismac, &conn.hisip, httpreq.qs);
  }
  
  return redir_main_exit(redir, &httpreq, &socket, forked);
}


/* Set callback to determine state information for the connection */
int redir_set_cb_getstate(struct redir_t *redir,
  int (*cb_getstate) (struct redir_t *redir, 
		      struct sockaddr_in *address, 
		      struct redir_conn_t *conn)) {
  redir->cb_getstate = cb_getstate;
  return 0;
}

