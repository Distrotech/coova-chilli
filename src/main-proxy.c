/* 
 * Copyright (C) 2009 Coova Technologies, LLC. <support@coova.com>
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
#include "cmdline.h"
#include "dhcp.h"
#include "radius.h"
#include "radius_chillispot.h"
#include "radius_wispr.h"
#include "redir.h"
#include "chilli.h"
#include "options.h"
#include "cmdsock.h"
#include "md5.h"

#include <curl/curl.h>
#include <curl/types.h>
#include <curl/easy.h>

/*
 * Plans (todo):
 *  - "Chilli Dog" will provide a simple RADIUS->HTTP AAA proxy (loosly based on WiFiDog). 
 *  - It should also be able to proxy to an alternate RADIUS server(s). 
 *  - It should also be able to establish and use a RadSec Tunnel. 
 *
 * Also see: http://www.coova.org/CoovaChilli/Proxy
 *
 * To enable, be sure to configure with "./configure --enable-chilliproxy"
 *
 */

typedef struct _proxy_request {
  int index;

  char reserved:6;
  char authorized:1;
  char inuse:1;

  bstring url;
  bstring data;
  bstring post;

  struct radius_packet_t radius_req;
  struct radius_packet_t radius_res;

  struct sockaddr_in peer;

  CURL *curl;
  struct radius_t *radius;

  struct _proxy_request *prev, *next;
  
} proxy_request;

static int max_requests = 0;
static proxy_request * requests = 0;
static proxy_request * requests_free = 0;

static CURLM * curl_multi;
static int still_running = 0;

static proxy_request * get_request() {
  proxy_request * req = 0;
  int i;

  if (!max_requests) {

    max_requests = 16; /* hard maximum! (should be configurable) */

    requests = (proxy_request *) calloc(max_requests, sizeof(proxy_request));
    for (i=0; i < max_requests; i++) {
      requests[i].index = i;
      if (i > 0) 
	requests[i].prev = &requests[i-1];
      if ((i+1) < max_requests) 
	requests[i].next = &requests[i+1];
    }
    
    requests_free = requests;
  }
  
  if (requests_free) {
    req = requests_free;
    requests_free = requests_free->next;
    if (requests_free)
      requests_free->prev = 0;
  }
  
  if (!req) {
    /* problem */
    log_err(0,"out of connections\n");
    return 0;
  }
  
  req->next = req->prev = 0;
  req->inuse = 1;
  return req;
}

static int radius_reply(struct radius_t *this,
			struct radius_packet_t *pack,
			struct sockaddr_in *peer) {
  
  size_t len = ntohs(pack->length);
  
  if (sendto(this->fd, pack, len, 0,(struct sockaddr *) peer, 
	     sizeof(struct sockaddr_in)) < 0) {
    log_err(errno, "sendto() failed!");
    return -1;
  } 
  
  return 0;
}

static void close_request(proxy_request *req) {
  req->inuse = 0;
  if (requests_free) {
    requests_free->prev = req;
    req->next = requests_free;
  }
  requests_free = req;
}

static int bstring_data(void *ptr, size_t size, size_t nmemb, void *userdata) {
  bstring s = (bstring) userdata;
  int rsize = size * nmemb;
  bcatblk(s,ptr,rsize);
  return rsize;
}

static int http_aaa_setup(struct radius_t *radius, proxy_request *req) {
  int result = -2;
  CURL *curl;
  CURLcode res;

  char *user = 0;
  char *pwd = 0;
  char *ca = 0;
  char *cert = 0;
  char *key = 0;
  char *keypwd = 0;

  req->radius = radius;

  if ((curl = req->curl = curl_easy_init()) != NULL) {
    struct curl_httppost *formpost=NULL;
    struct curl_httppost *lastptr=NULL;
    char error_buffer[CURL_ERROR_SIZE + 1];

    memset(&error_buffer, 0, sizeof(error_buffer));

    if (req->post) {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->post->data);
    }

    if (user && pwd) {
    }

#ifdef HAVE_OPENSSL

    if (cert && strlen(cert)) {
      log_dbg("using cert [%s]",cert);
      curl_easy_setopt(curl, CURLOPT_SSLCERT, cert);
      curl_easy_setopt(curl, CURLOPT_SSLCERTTYPE, "PEM");
    }

    if (key && strlen(key)) {
      log_dbg("using key [%s]",key);
      curl_easy_setopt(curl, CURLOPT_SSLKEY, key);
      curl_easy_setopt(curl, CURLOPT_SSLKEYTYPE, "PEM");
      if (keypwd && strlen(keypwd)) {
	log_dbg("using key pwd [%s]",keypwd);
#ifdef CURLOPT_SSLCERTPASSWD
	curl_easy_setopt(curl, CURLOPT_SSLCERTPASSWD, keypwd);
#else
#ifdef CURLOPT_SSLKEYPASSWD
	curl_easy_setopt(curl, CURLOPT_SSLKEYPASSWD, keypwd);
#else
#ifdef CURLOPT_KEYPASSWD
	curl_easy_setopt(curl, CURLOPT_KEYPASSWD, keypwd);
#endif
#endif
#endif
      }
    }

    if (ca && strlen(ca)) {
#ifdef CURLOPT_ISSUERCERT
      log_dbg("using ca [%s]",ca);
      curl_easy_setopt(curl, CURLOPT_ISSUERCERT, ca);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1);
#else
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
#endif
    }
    else {
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
    }

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_SSLv3);
#endif

    curl_easy_setopt(curl, CURLOPT_VERBOSE, /*debug ? 1 :*/ 0);

    curl_easy_setopt(curl, CURLOPT_URL, req->url->data);
    
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CoovaChilli " VERSION);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1); 
    curl_easy_setopt(curl, CURLOPT_NETRC, 0);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, bstring_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, req->data);

    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, &error_buffer);

    result = 0;
  }

  return result;
}

static int http_aaa(struct radius_t *radius, proxy_request *req) {

  if (http_aaa_setup(radius, req) == 0) {
    
    curl_multi_add_handle(curl_multi, req->curl);

    while(CURLM_CALL_MULTI_PERFORM ==
	  curl_multi_perform(curl_multi, &still_running));

    return 0;
  }

  return -1;
}

static int http_aaa_finish(proxy_request *req) {

  struct radius_t *radius = req->radius;

  curl_multi_remove_handle(curl_multi, req->curl);

  curl_easy_cleanup(req->curl);

  req->curl = 0;

  if (req->data->slen) {
    log_dbg("Received: %s\n",req->data->data);
    req->authorized = !memcmp(req->data->data, "Auth: 1", 7);
  }

  /* initialize response packet */
  switch(req->radius_req.code) {
  case RADIUS_CODE_ACCOUNTING_REQUEST:
    log_dbg("Accounting-Response");
    radius_default_pack(radius, &req->radius_res, RADIUS_CODE_ACCOUNTING_RESPONSE);
    break;
    
  case RADIUS_CODE_ACCESS_REQUEST:
    log_dbg("Access-%s", req->authorized ? "Accept" : "Reject");
    if (req->authorized) {
      radius_default_pack(radius, &req->radius_res, RADIUS_CODE_ACCESS_ACCEPT);
      break;
    }

  default:
    radius_default_pack(radius, &req->radius_res, RADIUS_CODE_ACCESS_REJECT);
    break;
  }

  req->radius_res.id = req->radius_req.id;

  /* process attributes */
  if (req->data->slen) {
    char *parse = req->data->data;
    if (parse) {
      char *s, *ptr;
      while ((ptr = strtok(parse,"\n"))) {
	parse = 0;

	struct {
	  char *n;
	  int a;
	  int v;
	  int va;
	  char t;
	} attrs[] = {
	  { "Idle-Timeout:", RADIUS_ATTR_IDLE_TIMEOUT, 0, 0, 0 },
	  { "Reply-Message:", RADIUS_ATTR_REPLY_MESSAGE, 0, 0, 1 },
	  { "Session-Timeout:", RADIUS_ATTR_SESSION_TIMEOUT, 0, 0, 0 },
	  { "Acct-Interim-Interval:", RADIUS_ATTR_ACCT_INTERIM_INTERVAL, 0, 0, 0 },
	  { "ChilliSpot-Config:", 
	    RADIUS_ATTR_VENDOR_SPECIFIC, RADIUS_VENDOR_CHILLISPOT, 
	    RADIUS_ATTR_CHILLISPOT_CONFIG, 1 },
	  { "ChilliSpot-Bandwidth-Max-Up:", 
	    RADIUS_ATTR_VENDOR_SPECIFIC, RADIUS_VENDOR_CHILLISPOT, 
	    RADIUS_ATTR_CHILLISPOT_BANDWIDTH_MAX_UP, 0 },
	  { "ChilliSpot-Bandwidth-Max-Down:", 
	    RADIUS_ATTR_VENDOR_SPECIFIC, RADIUS_VENDOR_CHILLISPOT, 
	    RADIUS_ATTR_CHILLISPOT_BANDWIDTH_MAX_DOWN, 0 },
	  { "ChilliSpot-Max-Input-Octets:", 
	    RADIUS_ATTR_VENDOR_SPECIFIC, RADIUS_VENDOR_CHILLISPOT, 
	    RADIUS_ATTR_CHILLISPOT_MAX_INPUT_OCTETS, 0 },
	  { "ChilliSpot-Max-Output-Octets:", 
	    RADIUS_ATTR_VENDOR_SPECIFIC, RADIUS_VENDOR_CHILLISPOT, 
	    RADIUS_ATTR_CHILLISPOT_MAX_OUTPUT_OCTETS, 0 },
	  { "ChilliSpot-Max-Total-Octets:", 
	    RADIUS_ATTR_VENDOR_SPECIFIC, RADIUS_VENDOR_CHILLISPOT, 
	    RADIUS_ATTR_CHILLISPOT_MAX_TOTAL_OCTETS, 0 },
	  { "ChilliSpot-Max-Input-Gigawords:", 
	    RADIUS_ATTR_VENDOR_SPECIFIC, RADIUS_VENDOR_CHILLISPOT, 
	    RADIUS_ATTR_CHILLISPOT_MAX_INPUT_GIGAWORDS, 0 },
	  { "ChilliSpot-Max-Output-Gigawords:", 
	    RADIUS_ATTR_VENDOR_SPECIFIC, RADIUS_VENDOR_CHILLISPOT, 
	    RADIUS_ATTR_CHILLISPOT_MAX_OUTPUT_GIGAWORDS, 0 },
	  { "ChilliSpot-Max-Total-Gigawords:", 
	    RADIUS_ATTR_VENDOR_SPECIFIC, RADIUS_VENDOR_CHILLISPOT, 
	    RADIUS_ATTR_CHILLISPOT_MAX_TOTAL_GIGAWORDS, 0 },
	  { "WISPr-Bandwidth-Max-Up:", 
	    RADIUS_ATTR_VENDOR_SPECIFIC, RADIUS_VENDOR_WISPR, 
	    RADIUS_ATTR_WISPR_BANDWIDTH_MAX_UP, 0 },
	  { "WISPr-Bandwidth-Max-Down:", 
	    RADIUS_ATTR_VENDOR_SPECIFIC, RADIUS_VENDOR_WISPR, 
	    RADIUS_ATTR_WISPR_BANDWIDTH_MAX_DOWN, 0 },
	  { "WISPr-Redirection-URL:", 
	    RADIUS_ATTR_VENDOR_SPECIFIC, RADIUS_VENDOR_WISPR, 
	    RADIUS_ATTR_WISPR_REDIRECTION_URL, 1 },
	  { 0 }
	};
	
	int i;
	for (i=0; attrs[i].n; i++) {
	  if (!strncmp(ptr,attrs[i].n,strlen(attrs[i].n))) {
	    switch(attrs[i].t) {
	    case 0:
	      {
		uint32_t v = (uint32_t) atoi(ptr+strlen(attrs[i].n));
		if (v > 0) {
		  radius_addattr(radius, &req->radius_res, attrs[i].a, attrs[i].v, attrs[i].va, v, NULL, 0);
		  log_dbg("Setting %s = %d", attrs[i].n, v);
		}
	      }
	      break;
	    case 1:
	      {
		radius_addattr(radius, &req->radius_res, attrs[i].a, attrs[i].v, attrs[i].va, 0, 
			       ptr+strlen(attrs[i].n), strlen(ptr)-strlen(attrs[i].n));
		log_dbg("Setting %s = %s", attrs[i].n, ptr);
	      }
	      break;
	    }
	  }
	}
      }
    }
  }

  /* finish off RADIUS response */
  switch(req->radius_req.code) {
    
  case RADIUS_CODE_ACCESS_REQUEST:
    {
      struct radius_attr_t *ma = NULL;
      
      radius_addattr(radius, &req->radius_res, RADIUS_ATTR_MESSAGE_AUTHENTICATOR, 
		     0, 0, 0, NULL, RADIUS_MD5LEN);
      
      memset(req->radius_res.authenticator, 0, RADIUS_AUTHLEN);
      memcpy(req->radius_res.authenticator, req->radius_req.authenticator, RADIUS_AUTHLEN);
      
      if (!radius_getattr(&req->radius_res, &ma, RADIUS_ATTR_MESSAGE_AUTHENTICATOR, 0,0,0)) {
	radius_hmac_md5(radius, &req->radius_res, radius->secret, radius->secretlen, ma->v.t);
      }
      
      radius_authresp_authenticator(radius, &req->radius_res, 
				    req->radius_req.authenticator,
				    radius->secret,
				    radius->secretlen);
    }
    break;
    
  case RADIUS_CODE_ACCOUNTING_REQUEST:
    radius_authresp_authenticator(radius, &req->radius_res, 
				  req->radius_req.authenticator,
				  radius->secret,
				  radius->secretlen);
    break;
  }

  radius_reply(req->radius, &req->radius_res, &req->peer);

  close_request(req);

  return 0;
}

static void http_aaa_register(int argc, char **argv, int i) {
  proxy_request req = {0};

  bstring tmp = bfromcstr("");
  bstring tmp2 = bfromcstr("");

  /* end with removing options */
  argv[i] = 0;
  process_options(i, argv, 1);

  if (!options()->uamaaaurl) {
    log_err(0, "uamaaaurl not defined in configuration");
    exit(-1);
  }

  req.url = bfromcstr("");
  req.data = bfromcstr("");
  req.post = bfromcstr("");

  bstring_fromfd(req.post, 0);

  bassignformat(req.url, "%s%c", 
		options()->uamaaaurl,
		strchr(options()->uamaaaurl, '?') > 0 ? '&' : '?');

  bcatcstr(req.url, "stage=register");

  bcatcstr(req.url, "&ap=");
  if (options()->nasmac) {
    bcatcstr(req.url, options()->nasmac);
  } else {
    char nas_hwaddr[PKT_ETH_ALEN];
    struct ifreq ifr;
    char mac[32];

    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    strncpy(ifr.ifr_name, options()->dhcpif, sizeof(ifr.ifr_name));

    if (ioctl(fd, SIOCGIFHWADDR, (caddr_t)&ifr) == 0) {
      memcpy(nas_hwaddr, ifr.ifr_hwaddr.sa_data, PKT_ETH_ALEN);
      sprintf(mac, "%.2X-%.2X-%.2X-%.2X-%.2X-%.2X", 
	      nas_hwaddr[0],nas_hwaddr[1],nas_hwaddr[2],
	      nas_hwaddr[3],nas_hwaddr[4],nas_hwaddr[5]);
      bcatcstr(req.url, mac);
    }
      
    close(fd);
  }

  bcatcstr(req.url, "&nasid=");
  if (options()->radiusnasid) {
    char *nasid = options()->radiusnasid;
    bassignblk(tmp, nasid, strlen(nasid));
    redir_urlencode(tmp, tmp2);
    bconcat(req.url, tmp2);
  }
    
  for (i=i+1; i < argc; i++) {
    bcatcstr(req.url, "&");
    bcatcstr(req.url, argv[i]);
    bcatcstr(req.url, "=");
    i++;
    if (i < argc) {
      bassignblk(tmp, argv[i], strlen(argv[i]));
      redir_urlencode(tmp, tmp2);
      bconcat(req.url, tmp2);
    }
  }

  redir_md_param(req.url, options()->uamsecret, "&");

  curl_global_init(CURL_GLOBAL_ALL);

  if (http_aaa_setup(0, &req) == 0) {

    log_dbg("==> %s\npost:%s", req.url->data, req.post->data);

    curl_easy_perform(req.curl);

    log_dbg("<== %s", req.data->data);

    curl_easy_cleanup(req.curl);
  }

  bdestroy(req.url);
  bdestroy(req.data);
  bdestroy(req.post);
  bdestroy(tmp);
  bdestroy(tmp2);

  curl_global_cleanup();
  exit(0);
}

static void process_radius(struct radius_t *radius, struct radius_packet_t *pack, struct sockaddr_in *peer) {
  struct radius_attr_t *attr = NULL; 
  char *error = 0;

  proxy_request *req = get_request();

  bstring tmp;
  bstring tmp2;

  if (!req) return;

  if (!options()->uamaaaurl) {
    log_err(0,"No --uamaaaurl parameter defined");
    return;
  }

  tmp = bfromcstralloc(10,"");
  tmp2 = bfromcstralloc(10,"");

  if (!req->url) req->url = bfromcstr("");
  if (!req->data) req->data = bfromcstr("");

  memcpy(&req->peer, peer, sizeof(req->peer));
  memcpy(&req->radius_req, pack, sizeof(struct radius_packet_t));
  memset(&req->radius_res, '0', sizeof(struct radius_packet_t));

  bassigncstr(req->data, "");

  bassignformat(req->url, "%s%c", 
		options()->uamaaaurl, 
		strchr(options()->uamaaaurl, '?') > 0 ? '&' : '?');

  switch(req->radius_req.code) {
  case RADIUS_CODE_ACCESS_REQUEST:
    bcatcstr(req->url, "stage=login");
    if (radius_getattr(pack, &attr, RADIUS_ATTR_SERVICE_TYPE, 0,0,0)) {
      error = "No service-type in RADIUS packet";
    } else {
      bcatcstr(req->url, "&service=");
      switch (ntohl(attr->v.i)) {
      case RADIUS_SERVICE_TYPE_LOGIN:
	bcatcstr(req->url, "login");
	break;
      case RADIUS_SERVICE_TYPE_FRAMED:
	bcatcstr(req->url, "framed");
	break;
      case RADIUS_SERVICE_TYPE_ADMIN_USER:
	bcatcstr(req->url, "admin");
	break;
      default:
	bassignformat(tmp, "%d", ntohl(attr->v.i));
	bconcat(req->url, tmp);
	break;
      }
    }
    break;
  case RADIUS_CODE_ACCOUNTING_REQUEST:
    bcatcstr(req->url, "stage=counters");
    if (radius_getattr(pack, &attr, RADIUS_ATTR_ACCT_STATUS_TYPE, 0,0,0)) {
      error = "No acct-status-type in RADIUS packet";
    } else {
      bcatcstr(req->url, "&status=");
      switch (ntohl(attr->v.i)) {
      case RADIUS_STATUS_TYPE_START:
	bcatcstr(req->url, "start");
	break;
      case RADIUS_STATUS_TYPE_STOP:
	bcatcstr(req->url, "stop");
	break;
      case RADIUS_STATUS_TYPE_INTERIM_UPDATE:
	bcatcstr(req->url, "update");
	break;
      case RADIUS_STATUS_TYPE_ACCOUNTING_ON:
	bcatcstr(req->url, "up");
	break;
      case RADIUS_STATUS_TYPE_ACCOUNTING_OFF:
	bcatcstr(req->url, "down");
	break;
      default:
	log_err(0,"unsupported acct-status-type %d",ntohl(attr->v.i));
	error = "Unsupported acct-status-type";
	break;
      }
    }
    break;
  default:
    error = "Unsupported RADIUS code";
    break;
  }

  if (!error) {
    if (!radius_getattr(pack, &attr, RADIUS_ATTR_USER_NAME, 0,0,0)) {
      bcatcstr(req->url, "&user=");
      bassignblk(tmp, attr->v.t, attr->l-2);
      redir_urlencode(tmp, tmp2);
      bconcat(req->url, tmp2);
    }

    if (!radius_getattr(pack, &attr, RADIUS_ATTR_USER_PASSWORD, 0,0,0)) {
      char pwd[RADIUS_ATTR_VLEN];
      size_t pwdlen = 0;
      if (!radius_pwdecode(radius, (uint8_t *) pwd, RADIUS_ATTR_VLEN, &pwdlen, 
			   attr->v.t, attr->l-2, pack->authenticator,
			   radius->secret,
			   radius->secretlen)) {
	bcatcstr(req->url, "&pass=");
	bassignblk(tmp, pwd, strlen(pwd));
	redir_urlencode(tmp, tmp2);
	bconcat(req->url, tmp2);
      }
    }

    if (!radius_getattr(pack, &attr, RADIUS_ATTR_CHAP_CHALLENGE, 0,0,0)) {
      char hexchal[1+(2*REDIR_MD5LEN)];
      unsigned char challenge[REDIR_MD5LEN];
      if (attr->l-2 <= sizeof(challenge)) {
	bcatcstr(req->url, "&chap_chal=");
	memcpy(challenge, attr->v.t, attr->l-2);
	redir_chartohex(challenge, hexchal, REDIR_MD5LEN);
	bcatcstr(req->url, hexchal);
      }
    }

    if (!radius_getattr(pack, &attr, RADIUS_ATTR_CHAP_PASSWORD, 0,0,0)) {
      char hexchal[65]; /* more than enough */
      unsigned char resp[32]; 
      if (attr->l-3 <= sizeof(resp)) {
	char chapid = attr->v.t[0];
	bcatcstr(req->url, "&chap_pass=");
	redir_chartohex(attr->v.t+1, hexchal, attr->l-3);
	bcatcstr(req->url, hexchal);
	bassignformat(tmp, "&chap_id=%d", chapid);
	bconcat(req->url, tmp);
      }
    }

    if (!radius_getattr(pack, &attr, RADIUS_ATTR_CALLED_STATION_ID, 0,0,0)) {
      bcatcstr(req->url, "&ap=");
      bassignblk(tmp, attr->v.t, attr->l-2);
      redir_urlencode(tmp, tmp2);
      bconcat(req->url, tmp2);
    }

    if (!radius_getattr(pack, &attr, RADIUS_ATTR_CALLING_STATION_ID, 0,0,0)) {
      bcatcstr(req->url, "&mac=");
      bassignblk(tmp, attr->v.t, attr->l-2);
      redir_urlencode(tmp, tmp2);
      bconcat(req->url, tmp2);
    }

    if (!radius_getattr(pack, &attr, RADIUS_ATTR_FRAMED_IP_ADDRESS, 0, 0, 0)) {
      if ((attr->l-2) == sizeof(struct in_addr)) {
	struct in_addr *ip = (struct in_addr *) &(attr->v.i);
	bcatcstr(req->url, "&ip=");
	bcatcstr(req->url, inet_ntoa(*ip));
      }
    }

    if (!radius_getattr(pack, &attr, RADIUS_ATTR_ACCT_SESSION_ID, 0,0,0)) {
      bcatcstr(req->url, "&sessionid=");
      bassignblk(tmp, attr->v.t, attr->l-2);
      redir_urlencode(tmp, tmp2);
      bconcat(req->url, tmp2);
    }

    if (!radius_getattr(pack, &attr, RADIUS_ATTR_NAS_IDENTIFIER, 0,0,0)) {
      bcatcstr(req->url, "&nasid=");
      bassignblk(tmp, attr->v.t, attr->l-2);
      redir_urlencode(tmp, tmp2);
      bconcat(req->url, tmp2);
    }

    if (!radius_getattr(pack, &attr, RADIUS_ATTR_ACCT_SESSION_TIME, 0,0,0)) {
      uint32_t val = ntohl(attr->v.i);
      bassignformat(tmp, "&duration=%d", val);
      bconcat(req->url, tmp);
    }

    if (!radius_getattr(pack, &attr, RADIUS_ATTR_ACCT_INPUT_OCTETS, 0,0,0)) {
      char *direction = options()->swapoctets ? "up" : "down";
      uint32_t val = ntohl(attr->v.i);
      uint64_t input = val;
      if (!radius_getattr(pack, &attr, RADIUS_ATTR_ACCT_INPUT_GIGAWORDS, 0,0,0)) {
	val = ntohl(attr->v.i);
	input |= (val << 32);
      }
      bassignformat(tmp, "&bytes_%s=%ld", direction, input);
      bconcat(req->url, tmp);
      if (!radius_getattr(pack, &attr, RADIUS_ATTR_ACCT_INPUT_PACKETS, 0,0,0)) {
	val = ntohl(attr->v.i);
	bassignformat(tmp, "&pkts_%s=%ld", direction, val);
	bconcat(req->url, tmp);
      }
    }

    if (!radius_getattr(pack, &attr, RADIUS_ATTR_ACCT_OUTPUT_OCTETS, 0,0,0)) {
      char *direction = options()->swapoctets ? "down" : "up";
      uint32_t val = ntohl(attr->v.i);
      uint64_t output = val;
      if (!radius_getattr(pack, &attr, RADIUS_ATTR_ACCT_OUTPUT_GIGAWORDS, 0,0,0)) {
	val = ntohl(attr->v.i);
	output |= (val << 32);
      }
      bassignformat(tmp, "&bytes_%s=%ld", direction, output);
      bconcat(req->url, tmp);
      if (!radius_getattr(pack, &attr, RADIUS_ATTR_ACCT_OUTPUT_PACKETS, 0,0,0)) {
	val = ntohl(attr->v.i);
	bassignformat(tmp, "&pkts_%s=%ld", direction, val);
	bconcat(req->url, tmp);
      }
    }
  }

  if (!error) {
    redir_md_param(req->url, options()->uamsecret, "&");
  
    log_dbg("==> %s", req->url->data);
    
    http_aaa(radius, req);
    
  } else {
    log_err(0, "problem: %s", error);
  }

  bdestroy(tmp);
  bdestroy(tmp2);
}

int main(int argc, char **argv) {
  struct options_t * opt;
  struct radius_packet_t radius_pack;
  struct radius_t *radius_auth;
  struct radius_t *radius_acct;
  struct in_addr radiuslisten;

  struct timeval timeout;

  int maxfd = 0;
  fd_set fdread;
  fd_set fdwrite;
  fd_set fdexcep;

  int status;

  CURLMsg *msg;
  int msgs_left;
  int i;

  options_set(opt = (struct options_t *)calloc(1, sizeof(struct options_t)));

  /*
   *  Support a --register mode whereby all subsequent arguments are 
   *  used to create a URL for sending to the back-end. 
   */
  for (i=0; i < argc; i++) {
    if (!strcmp(argv[i],"--register")) {
      http_aaa_register(argc, argv, i);
    }
  }

  process_options(argc, argv, 1);

  curl_global_init(CURL_GLOBAL_ALL);
  
  radiuslisten.s_addr = htonl(INADDR_ANY);

  curl_multi = curl_multi_init();

  if (radius_new(&radius_auth, &radiuslisten, 
		 opt->radiusauthport ? opt->radiusauthport : RADIUS_AUTHPORT, 
		 0, NULL, 0, NULL, NULL, NULL)) {
    log_err(0, "Failed to create radius");
    return -1;
  }

  if (radius_new(&radius_acct, &radiuslisten, 
		 opt->radiusacctport ? opt->radiusacctport : RADIUS_ACCTPORT, 
		 0, NULL, 0, NULL, NULL, NULL)) {
    log_err(0, "Failed to create radius");
    return -1;
  }

  radius_set(radius_auth, 0, 0);
  radius_set(radius_acct, 0, 0);

  while (1) {
    FD_ZERO(&fdread);
    FD_ZERO(&fdwrite);
    FD_ZERO(&fdexcep);

    FD_SET(radius_auth->fd, &fdread);
    FD_SET(radius_acct->fd, &fdread);

    curl_multi_fdset(curl_multi, &fdread, &fdwrite, &fdexcep, &maxfd);

    if (radius_auth->fd > maxfd) maxfd = radius_auth->fd;
    if (radius_acct->fd > maxfd) maxfd = radius_acct->fd;
    
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
 
    status = select(maxfd + 1, &fdread, &fdwrite, &fdexcep, &timeout);

    switch (status) {
    case -1:
      log_err(errno, "select() returned -1!");
      break;  

    case 0:
    default:

      if (status > 0) {
	struct sockaddr_in addr;
	socklen_t fromlen = sizeof(addr);
	
	if (FD_ISSET(radius_auth->fd, &fdread)) {
	  /*
	   *    ---> Authentication
	   */
	  
	  if ((status = recvfrom(radius_auth->fd, &radius_pack, sizeof(radius_pack), 0, 
				 (struct sockaddr *) &addr, &fromlen)) <= 0) {
	    log_err(errno, "recvfrom() failed");
	    
	    return -1;
	  }
	  
	  process_radius(radius_auth, &radius_pack, &addr);
	}
	
	if (FD_ISSET(radius_acct->fd, &fdread)) {
	  /*
	   *    ---> Accounting
	   */
	  
	  log_dbg("received accounting");
	  
	  if ((status = recvfrom(radius_acct->fd, &radius_pack, sizeof(radius_pack), 0, 
			       (struct sockaddr *) &addr, &fromlen)) <= 0) {
	    log_err(errno, "recvfrom() failed");
	    return -1;
	  }
	  
	  process_radius(radius_acct, &radius_pack, &addr);
	}
      }
      
      if (still_running) {
	while(CURLM_CALL_MULTI_PERFORM ==
	      curl_multi_perform(curl_multi, &still_running));
      }

      while ((msg = curl_multi_info_read(curl_multi, &msgs_left))) {
	if (msg->msg == CURLMSG_DONE) {
	  
	  int idx, found = 0;
	  
	  /* Find out which handle this message is about */ 
	  for (idx=0; (!found && (idx < max_requests)); idx++) 
	    found = (msg->easy_handle == requests[idx].curl);
	  
	  if (found) {
	    --idx;
	    log_dbg("HTTP completed with status %d\n", msg->data.result);
	    http_aaa_finish(&requests[idx]);
	  }
	}
      }

      break;
    }
  }

  curl_multi_cleanup(curl_multi);

  curl_global_cleanup();
}
