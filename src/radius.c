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
#include "md5.h"
#include "dhcp.h"
#include "redir.h"
#include "chilli.h"
#include "options.h"
#include "radius_wispr.h"
#include "radius_chillispot.h"


void radius_addnasip(struct radius_t *radius, struct radius_packet_t *pack)  {
  struct in_addr inaddr;
  struct in_addr *paddr = 0;

  if (options()->nasip && *options()->nasip)
    if (inet_aton(options()->nasip, &inaddr))
      paddr = &inaddr;

  if (!paddr && options()->radiuslisten.s_addr != 0)
    paddr = &options()->radiuslisten;

  if (!paddr)
    paddr = &options()->uamlisten;
    
  radius_addattr(radius, pack, RADIUS_ATTR_NAS_IP_ADDRESS, 0, 0, ntohl(paddr->s_addr), NULL, 0); 
}

void radius_addcalledstation(struct radius_t *radius, struct radius_packet_t *pack)  {
  uint8_t b[24];
  uint8_t *mac= (uint8_t*)"";

  if (options()->nasmac)
    mac = (uint8_t *)options()->nasmac;
  else 
    sprintf((char*)(mac=b), "%.2X-%.2X-%.2X-%.2X-%.2X-%.2X", 
	    radius->nas_hwaddr[0],radius->nas_hwaddr[1],radius->nas_hwaddr[2],
	    radius->nas_hwaddr[3],radius->nas_hwaddr[4],radius->nas_hwaddr[5]);

  radius_addattr(radius, pack, RADIUS_ATTR_CALLED_STATION_ID, 0, 0, 0, mac, strlen((char*)mac)); 
}

int radius_printqueue(struct radius_t *this) {
  int n;
  printf("next %d, first %d, last %d\n", 
	 this->next, this->first, this ->last);

  for(n=0; n<256; n++) {
    if (this->queue[n].state) {
      printf("%3d %3d %3d %3d %8d %8d %d\n",
	     n, this->queue[n].state,
	     this->queue[n].next,
	     this->queue[n].prev,
	     (int) this->queue[n].timeout.tv_sec,
	     (int) this->queue[n].timeout.tv_usec,
	     (int) this->queue[n].retrans);
    }
  }

  return 0;
}

/* 
 * radius_hmac_md5()
 * Calculate HMAC MD5 on a radius packet. 
 */
int radius_hmac_md5(struct radius_t *this, struct radius_packet_t *pack, 
		    char *secret, int secretlen, uint8_t *dst) {
  unsigned char digest[RADIUS_MD5LEN];
  size_t length;

  MD5_CTX context;

  uint8_t *key;
  size_t key_len;

  unsigned char k_ipad[65];
  unsigned char k_opad[65];
  unsigned char tk[RADIUS_MD5LEN];
  int i;

  if (secretlen > 64) { /* TODO: If Microsoft truncate to 64 instead */
    MD5Init(&context);
    MD5Update(&context, (uint8_t*)secret, secretlen);
    MD5Final(tk, &context);
    key = tk;
    key_len = 16;
  }
  else {
    key = (uint8_t*)secret;
    key_len = secretlen;
  }

  length = ntohs(pack->length);

  memset(k_ipad, 0x36, sizeof k_ipad);
  memset(k_opad, 0x5c, sizeof k_opad);

  for (i=0; i<key_len; i++) {
    k_ipad[i] ^= key[i];
    k_opad[i] ^= key[i];
  }

  /* Perform inner MD5 */
  MD5Init(&context);
  MD5Update(&context, k_ipad, 64);
  MD5Update(&context, (uint8_t*) pack, length);
  MD5Final(digest, &context);

  /* Perform outer MD5 */
  MD5Init(&context);
  MD5Update(&context, k_opad, 64);
  MD5Update(&context, digest, 16);
  MD5Final(digest, &context);
  
  memcpy(dst, digest, RADIUS_MD5LEN);

  return 0;
}

/* 
 * radius_acctreq_authenticator()
 * Update a packet with an accounting request authenticator
 */
int radius_acctreq_authenticator(struct radius_t *this,
				 struct radius_packet_t *pack) {

  /* From RFC 2866: Authenticator is the MD5 hash of:
     Code + Identifier + Length + 16 zero octets + request attributes +
     shared secret */
  
  MD5_CTX context;

  memset(pack->authenticator, 0, RADIUS_AUTHLEN);

  /* Get MD5 hash on secret + authenticator */
  MD5Init(&context);
  MD5Update(&context, (void*) pack, ntohs(pack->length));
  MD5Update(&context, (uint8_t*) this->secret, this->secretlen);
  MD5Final(pack->authenticator, &context);
  
  return 0;
}

/* 
 * radius_authresp_authenticator()
 * Update a packet with an authentication response authenticator
 */
int radius_authresp_authenticator(struct radius_t *this,
				 struct radius_packet_t *pack,
				 uint8_t *req_auth,
				 char *secret, size_t secretlen) {

  /* From RFC 2865: Authenticator is the MD5 hash of:
     Code + Identifier + Length + request authenticator + request attributes +
     shared secret */
  
  MD5_CTX context;

  memcpy(pack->authenticator, req_auth, RADIUS_AUTHLEN);

  /* Get MD5 hash on secret + authenticator */
  MD5Init(&context);
  MD5Update(&context, (void*) pack, ntohs(pack->length));
  MD5Update(&context, (uint8_t*) secret, secretlen);
  MD5Final(pack->authenticator, &context);
  
  return 0;
}


/* 
 * radius_queue_in()
 * Place data in queue for later retransmission.
 */
int radius_queue_in(struct radius_t *this, struct radius_packet_t *pack,
		    void *cbp) {
  struct timeval *tv;
  struct radius_attr_t *ma = NULL; /* Message authenticator */

  if (this->debug) {
    log_dbg("radius_queue_in");
    radius_printqueue(this);
  }

  if (this->queue[this->next].state == 1) {
    log_err(0, "radius queue is full!");
    /* Queue is not really full. It only means that the next space
       in queue is not available, but there might be space elsewhere */
    return -1;
  }

  pack->id = this->next;

  /* If packet contains message authenticator: Calculate it! */
  if (!radius_getattr(pack, &ma, RADIUS_ATTR_MESSAGE_AUTHENTICATOR, 0,0,0)) {
    radius_hmac_md5(this, pack, this->secret, this->secretlen, ma->v.t);
  }
  
  /* If accounting request: Calculate authenticator */
  if (pack->code == RADIUS_CODE_ACCOUNTING_REQUEST)
    radius_acctreq_authenticator(this, pack);

  memcpy(&this->queue[this->next].p, pack, RADIUS_PACKSIZE);
  this->queue[this->next].state = 1;
  this->queue[this->next].cbp = cbp;
  this->queue[this->next].retrans = 0;

  tv = &this->queue[this->next].timeout;
  gettimeofday(tv, NULL);

  tv->tv_sec += options()->radiustimeout;

  this->queue[this->next].lastsent = this->lastreply;

  /* Insert in linked list for handling timeouts */
  this->queue[this->next].next = -1;         /* Last in queue */
  this->queue[this->next].prev = this->last; /* Link to previous */

  if (this->last != -1)
    this->queue[this->last].next=this->next; /* Link previous to us */
  this->last = this->next;                   /* End of queue */

  if (this->first == -1)
    this->first = this->next; /* First and last */

  this->next++; /* next = next % RADIUS_QUEUESIZE */

  if (this->debug) {
    printf("radius_queue_out end\n");
    radius_printqueue(this);
  }

  return 0;
}


/* 
 * radius_queue_in()
 * Remove data from queue.
 */
int radius_queue_out(struct radius_t *this, struct radius_packet_t *pack,
		     int id, void **cbp) {

  if (this->debug) if (this->debug) printf("radius_queue_out\n");

  if (this->queue[id].state != 1) {
    log_err(0, "No such id in radius queue: %d!", id);
    return -1;
  }

  if (this->debug) {
    log_dbg("radius_queue_out");
    radius_printqueue(this);
  }
  
  memcpy(pack, &this->queue[id].p, RADIUS_PACKSIZE);
  *cbp = this->queue[id].cbp;

  this->queue[id].state = 0;

  /* Remove from linked list */
  if (this->queue[id].next == -1) /* Are we the last in queue? */
    this->last = this->queue[id].prev;
  else
    this->queue[this->queue[id].next].prev = this->queue[id].prev;
    
  if (this->queue[id].prev == -1) /* Are we the first in queue? */
    this->first = this->queue[id].next;
  else
    this->queue[this->queue[id].prev].next = this->queue[id].next;

  if (this->debug) {
    log_dbg("radius_queue_out end");
    radius_printqueue(this);
  }

  return 0;
}

/* 
 * radius_queue_reschedule()
 * Recalculate the timeout value of a packet in the queue.
 */
int radius_queue_reschedule(struct radius_t *this, int id) {
  struct timeval *tv;

  /* sanity check */
  if (id < 0 || id >= RADIUS_QUEUESIZE) {
    log_err(0, "bad id (%d)", id);
    return -1;
  }

  if (this->debug) 
    log_dbg("radius_queue_reschedule");

  if (this->queue[id].state != 1) {
    log_err(0, "No such id in radius queue: %d!", id);
    return -1;
  }

  if (this->debug) {
    log_dbg("radius_reschedule");
    radius_printqueue(this);
  }

  this->queue[id].retrans++;

  tv = &this->queue[id].timeout;
  gettimeofday(tv, NULL);

  tv->tv_sec += options()->radiustimeout;

  /* Remove from linked list */
  if (this->queue[id].next == -1) /* Are we the last in queue? */
    this->last = this->queue[id].prev;
  else
    this->queue[this->queue[id].next].prev = this->queue[id].prev;
    
  if (this->queue[id].prev == -1) /* Are we the first in queue? */
    this->first = this->queue[id].next;
  else
    this->queue[this->queue[id].prev].next = this->queue[id].next;

  /* Insert in linked list for handling timeouts */
  this->queue[id].next = -1;         /* Last in queue */
  this->queue[id].prev = this->last; /* Link to previous (could be -1) */

  if (this->last != -1)
    this->queue[this->last].next=id; /* If not empty: link previous to us */

  this->last = id;                   /* End of queue */

  if (this->first == -1)
    this->first = id;                /* First and last */

  if (this->debug) {
    radius_printqueue(this);
  }

  return 0;
}


/* 
 * radius_cmptv()
 * Returns an integer less than, equal to or greater than zero if tv1
 * is found, respectively, to be less than, to match or be greater than tv2.
 */
int 
radius_cmptv(struct timeval *tv1, struct timeval *tv2)
{
  struct timeval diff;

  if (0) {
    printf("tv1 %8d %8d tv2 %8d %8d\n", 
	   (int) tv1->tv_sec, (int) tv1->tv_usec, 
	   (int) tv2->tv_sec, (int) tv2->tv_usec);
  }
  
  /* First take the difference with |usec| < 1000000 */
  diff.tv_sec = (tv1->tv_usec  - tv2->tv_usec) / 1000000 +
                (tv1->tv_sec   - tv2->tv_sec);

  diff.tv_usec = (tv1->tv_usec - tv2->tv_usec) % 1000000;

  if (0) {
    printf("tv1 %8d %8d tv2 %8d %8d diff %8d %8d\n", 
	   (int) tv1->tv_sec, (int) tv1->tv_usec, 
	   (int) tv2->tv_sec, (int) tv2->tv_usec, 
	   (int) diff.tv_sec, (int) diff.tv_usec);
  }

  /* If sec and usec have different polarity add or subtract 1 second */
  if ((diff.tv_sec > 0) & (diff.tv_usec < 0)) {
    diff.tv_sec--;
    diff.tv_usec += 1000000;
  }
  if ((diff.tv_sec < 0) & (diff.tv_usec > 0)) {
    diff.tv_sec++;
    diff.tv_usec -= 1000000;
  }
  if (0) {
    printf("tv1 %8d %8d tv2 %8d %8d diff %8d %8d\n", 
	   (int) tv1->tv_sec, (int) tv1->tv_usec, 
	   (int) tv2->tv_sec, (int) tv2->tv_usec, 
	   (int) diff.tv_sec, (int) diff.tv_usec);
  }

  if (diff.tv_sec < 0) {if (0) printf("-1\n"); return -1; }
  if (diff.tv_sec > 0) {if (0) printf("1\n"); return  1; }

  if (diff.tv_usec < 0) {if (0) printf("-1\n"); return -1;}
  if (diff.tv_usec > 0) {if (0) printf("1\n"); return  1;}
  if (0) printf("0 \n");
  return 0;

}


/* 
 * radius_timeleft()
 * Determines how nuch time is left until we need to call 
 * radius_timeout().
 * Only modifies timeout if new value is lower than current value.
 */
int 
radius_timeleft(struct radius_t *this, struct timeval *timeout) 
{
  struct timeval now, later, diff;

  if (this->first == -1) /* Queue is empty */
    return 0;

  gettimeofday(&now, NULL);
  later.tv_sec = this->queue[this->first].timeout.tv_sec;
  later.tv_usec = this->queue[this->first].timeout.tv_usec;

  /* First take the difference with |usec| < 1000000 */
  diff.tv_sec = (later.tv_usec  - now.tv_usec) / 1000000 +
                (later.tv_sec   - now.tv_sec);
  diff.tv_usec = (later.tv_usec - now.tv_usec) % 1000000;

  /* If sec and usec have different polarity add or subtract 1 second */
  if ((diff.tv_sec > 0) & (diff.tv_usec < 0)) {
    diff.tv_sec--;
    diff.tv_usec += 1000000;
  }
  if ((diff.tv_sec < 0) & (diff.tv_usec > 0)) {
    diff.tv_sec++;
    diff.tv_usec -= 1000000;
  }

  /* If negative set to zero */
  if ((diff.tv_sec < 0) || (diff.tv_usec < 0)) {
    diff.tv_sec = 0;
    diff.tv_usec = 0;
  }

  /* If original was smaller do nothing */
  if (radius_cmptv(timeout, &diff) <=0) 
    return 0;

  timeout->tv_sec = diff.tv_sec;
  timeout->tv_usec = diff.tv_usec;
  return 0;
}

/* 
 * radius_timeout()
 * Retransmit any outstanding packets. This function should be called at
 * regular intervals. Use radius_timeleft() to determine how much time is 
 * left before this function should be called.
 */
int radius_timeout(struct radius_t *this) {
  /* Retransmit any outstanding packets */
  /* Remove from queue if maxretrans exceeded */
  struct timeval now;
  struct sockaddr_in addr;
  struct radius_packet_t pack_req;
  void *cbp;

  gettimeofday(&now, NULL);

#if(1)
  if (this->debug) {
    log_dbg("radius_timeout %8d %8d", (int)now.tv_sec, (int)now.tv_usec);
    radius_printqueue(this);
  }
#endif

  while (this->first != -1 && 
	 radius_cmptv(&now, &this->queue[this->first].timeout) >= 0) {
    
    if (this->queue[this->first].retrans < options()->radiusretry) {
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      
      if (this->queue[this->first].retrans == (options()->radiusretrysec - 1)) {
	/* Use the other server for next retransmission */
	if (this->queue[this->first].lastsent) {
	  addr.sin_addr = this->hisaddr0;
	  this->queue[this->first].lastsent = 0;
	}
	else {
	  addr.sin_addr = this->hisaddr1;
	  this->queue[this->first].lastsent = 1;
	}
      } 
      else {
	/* Use the same server for next retransmission */
	if (this->queue[this->first].lastsent) {
	  addr.sin_addr = this->hisaddr1;
	}
	else {
	  addr.sin_addr = this->hisaddr0;
	}
      }
      
      /* Use the correct port for accounting and authentication */
      if (this->queue[this->first].p.code == RADIUS_CODE_ACCOUNTING_REQUEST)
	addr.sin_port = htons(this->acctport);
      else
	addr.sin_port = htons(this->authport);
      
      if (sendto(this->fd, &this->queue[this->first].p,
		 ntohs(this->queue[this->first].p.length), 0,
		 (struct sockaddr *) &addr, sizeof(addr)) < 0) {
	log_err(errno, "sendto() failed!");
	radius_queue_reschedule(this, this->first);
	return -1;
      }

      radius_queue_reschedule(this, this->first);
    }
    else { /* Finished retrans */
      if (radius_queue_out(this, &pack_req, this->first, &cbp)) {
	log_warn(0, "Matching request was not found in queue: %d!", this->first);
	return -1;
      }

      if ((pack_req.code == RADIUS_CODE_ACCOUNTING_REQUEST) &&
	  (this->cb_acct_conf))
	  return this->cb_acct_conf(this, NULL, &pack_req, cbp);

      if ((pack_req.code == RADIUS_CODE_ACCESS_REQUEST) &&
	  (this->cb_auth_conf))
	return this->cb_auth_conf(this, NULL, &pack_req, cbp);
    }    
  }
  
  if (this->debug) {
    printf("radius_timeout\n");
    if (this->first > 0) {
      printf("first %d, timeout %8d %8d\n", this->first, 
	     (int) this->queue[this->first].timeout.tv_sec, 
	     (int) this->queue[this->first].timeout.tv_usec); 
    }
    radius_printqueue(this);
  }

  return 0;
}



/* 
 * radius_addattr()
 * Add an attribute to a packet. The packet length is modified 
 * accordingly.
 * If data==NULL and dlen!=0 insert null attribute.
 */
int 
radius_addattr(struct radius_t *this, struct radius_packet_t *pack, 
	       uint8_t type, uint32_t vendor_id, uint8_t vendor_type,
	       uint32_t value, uint8_t *data, uint16_t dlen) {
  struct radius_attr_t *a;
  char passwd[RADIUS_PWSIZE];
  uint16_t length = ntohs(pack->length);
  uint16_t vlen;
  size_t pwlen;

  a = (struct radius_attr_t *)((uint8_t*)pack + length);

  if (type == RADIUS_ATTR_USER_PASSWORD) {
    radius_pwencode(this, 
		    (uint8_t*) passwd, RADIUS_PWSIZE, 
		    &pwlen, 
		    data, dlen, 
		    pack->authenticator,
		    this->secret, this->secretlen);
    data = (uint8_t *)passwd;
    dlen = (uint16_t)pwlen;
  }

  if (type != RADIUS_ATTR_VENDOR_SPECIFIC) {
    if (dlen) { /* If dlen != 0 it is a text/string attribute */
      vlen = dlen;
    }
    else {
      vlen = 4; /* address, integer or time */
    }
    
    if (vlen > RADIUS_ATTR_VLEN) {
      log_warn(0, "Truncating RADIUS attribute (type:%d/%d/%d) from %d to %d bytes [%s]", 
	       type, vendor_id, vendor_type, vlen, RADIUS_ATTR_VLEN, data);
      vlen = RADIUS_ATTR_VLEN;
    }

    if ((length+vlen+2) > RADIUS_PACKSIZE) {
      log_err(0, "No more space!");
      return -1;
    }

    length += vlen + 2;

    pack->length = htons(length);

    a->t = type;
    a->l = vlen+2;

    if (data)
      memcpy(a->v.t, data, vlen);
    else if (dlen)
      memset(a->v.t, 0, vlen);
    else
      a->v.i = htonl(value);
  }
  else { /* Vendor specific */
    if (dlen) { /* If dlen != 0 it is a text/string attribute */
      vlen = dlen;
    }
    else {
      vlen = 4; /* address, integer or time */
    }

    if (vlen > RADIUS_ATTR_VLEN-8) {
      log_warn(0, "Truncating RADIUS attribute (type:%d/%d/%d) from %d to %d [%s]", 
	       type, vendor_id, vendor_type, vlen, RADIUS_ATTR_VLEN-8, data);
      vlen = RADIUS_ATTR_VLEN-8;
    }

    if ((length+vlen+2) > RADIUS_PACKSIZE) { 
      log_err(0, "No more space!");
      return -1;
    }

    length += vlen + 8;

    pack->length = htons(length);

    a->t = type;
    a->l = vlen+8;

    a->v.vv.i = htonl(vendor_id);
    a->v.vv.t = vendor_type;
    a->v.vv.l = vlen+2;

    if (data)
      memcpy(((void*) a)+8, data, dlen);
    else if (dlen)
      memset(((void*) a)+8, 0, dlen); 
    else
      a->v.vv.v.i = htonl(value);
  }

  return 0;
}


/* 
 * radius_getattr()
 * Search for an attribute in a packet. Returns -1 if attribute is not found.
 * The first instance matching attributes will be skipped
 */
int
radius_getattr(struct radius_packet_t *pack, struct radius_attr_t **attr,
	       uint8_t type, uint32_t vendor_id, uint8_t vendor_type,
	       int instance) {
  size_t offset = 0;
  return radius_getnextattr(pack, attr, type, vendor_id, vendor_type, instance, &offset);
}

int
radius_getnextattr(struct radius_packet_t *pack, struct radius_attr_t **attr,
	       uint8_t type, uint32_t vendor_id, uint8_t vendor_type,
	       int instance, size_t *roffset) {
  struct radius_attr_t *t;
  size_t len = ntohs(pack->length) - RADIUS_HDRSIZE;
  size_t offset = *roffset;
  int count = 0;

  if (0) {
    printf("radius_getattr payload(len=%d,off=%d) %.2x %.2x %.2x %.2x\n",
	   len, offset, pack->payload[offset], pack->payload[offset+1], 
	   pack->payload[offset+2], pack->payload[offset+3]);
  }

  while (offset < len) {
    t = (struct radius_attr_t *)(&pack->payload[offset]);

    if (0) {
      printf("radius_getattr %d %d %d %.2x %.2x \n", t->t, t->l, 
	     ntohl(t->v.vv.i), (int) t->v.vv.t, (int) t->v.vv.l);
    }

    offset +=  t->l;

    if (t->t == 0)
      return -1;
    
    if (t->t != type) 
      continue;
    
    if (t->t == RADIUS_ATTR_VENDOR_SPECIFIC && 
	(ntohl(t->v.vv.i) != vendor_id || t->v.vv.t != vendor_type))
      continue;
    
    if (count == instance) {
      
      if (type == RADIUS_ATTR_VENDOR_SPECIFIC)
	*attr = (struct radius_attr_t *) &t->v.vv.t;
      else
	*attr = t;
      
      if (0) printf("Found %.*s\n", (*attr)->l - 2, (char *)(*attr)->v.t);
      
      *roffset = offset;
      return 0;
    }
    else {
      count++;
    }
  }
  
  return -1; /* Not found */
}

/* 
 * radius_countattr()
 * Count the number of instances of an attribute in a packet.
 */
int 
radius_countattr(struct radius_packet_t *pack, uint8_t type) {
  struct radius_attr_t *t;
  size_t offset = 0;
  int count = 0;
  
  /* Need to check pack -> length */
  
  do {
    t = (struct radius_attr_t*)(&pack->payload[offset]);
    if (t->t == type) {
      count++;
    }
    offset +=  2 + t->l;
  } while (offset < ntohs(pack->length));
  
  if (0) printf("Count %d\n", count);
  return count;
}


/* 
 * radius_cmpattr()
 * Compare two attributes to see if they are the same.
 */
int 
radius_cmpattr(struct radius_attr_t *t1, struct radius_attr_t *t2) {
  if (t1->t != t2->t) return -1;
  if (t1->l != t2->l) return -1;
  if (memcmp(t1->v.t, t2->v.t, t1->l)) return -1; /* Also int/time/addr */
  return 0;
}


/*
 * radius_keydecode()
 * Decode an MPPE key using MD5.
 */
int radius_keydecode(struct radius_t *this, 
		     uint8_t *dst, size_t dstsize, size_t *dstlen, 
		     uint8_t *src, size_t srclen,
		     uint8_t *authenticator, 
		     char *secret, size_t secretlen) {
  MD5_CTX context;
  unsigned char b[RADIUS_MD5LEN];
  int blocks;
  int i, n;

  blocks = ((int)srclen - 2) / RADIUS_MD5LEN;

  if ((blocks * RADIUS_MD5LEN + 2) != (int)srclen) {
    log_err(0, "radius_keydecode: srclen must be 2 plus n*16");
    return -1;
  }

  if (blocks < 1) {
    log_err(0, "radius_keydecode srclen must be at least 18");
    return -1;
  }

  /* Get MD5 hash on secret + authenticator (First 16 octets) */
  MD5Init(&context);
  MD5Update(&context, (uint8_t *)secret, secretlen);
  MD5Update(&context, authenticator, RADIUS_AUTHLEN);
  MD5Update(&context, src, 2);
  MD5Final(b, &context);

  if ((src[2] ^ b[0]) > dstsize) {
    log_err(0,"radius_keydecode dstsize too small");
    return -1; 
  }

  if ((src[2] ^ b[0]) > (srclen - 3)) {
    log_err(0,"radius_keydecode dstlen > srclen - 3");
    return -1; 
  }

  *dstlen = (size_t)(src[2] ^ b[0]);

  for (i = 1; i < RADIUS_MD5LEN; i++)
    if ((i-1) < (int)*dstlen)
      dst[i-1] = src[i+2] ^ b[i];

  /* Next blocks of 16 octets */
  for (n=1; n < blocks; n++) {
    MD5Init(&context);
    MD5Update(&context, (uint8_t *)secret, secretlen);
    MD5Update(&context, &src[2 + ((n-1) * RADIUS_MD5LEN)], RADIUS_MD5LEN);
    MD5Final(b, &context);
    for (i = 0; i < RADIUS_MD5LEN; i++)
      if ((i-1+n*RADIUS_MD5LEN) < (int)*dstlen)
	dst[i-1+n*RADIUS_MD5LEN] = src[i+2+n*RADIUS_MD5LEN] ^ b[i];
  }

  return 0;
}

/* 
 * radius_keyencode()
 * Encode an MPPE key using MD5.
 */
int radius_keyencode(struct radius_t *this, 
		     uint8_t *dst, size_t dstsize, size_t *dstlen, 
		     uint8_t *src, size_t srclen,
		     uint8_t *authenticator, 
		     char *secret, size_t secretlen) {
  MD5_CTX context;
  unsigned char b[RADIUS_MD5LEN];
  int blocks;
  int i, n;

  blocks = ((int)srclen + 1) / RADIUS_MD5LEN;
  if ((blocks * RADIUS_MD5LEN) < ((int)srclen + 1)) blocks++;
  
  if (((blocks * RADIUS_MD5LEN) + 2) > (int)dstsize) {
    log_err(0, "radius_keyencode dstsize too small");
    return -1;
  }

  *dstlen = (size_t)((blocks * RADIUS_MD5LEN) + 2);

  /* Read two salt octets */
  if (fread(dst, 1, 2, this->urandom_fp) != 2) {
    log_err(errno, "fread() failed");
    return -1;
  }

  /* Get MD5 hash on secret + authenticator (First 16 octets) */
  MD5Init(&context);
  MD5Update(&context, (uint8_t *)secret, secretlen);
  MD5Update(&context, authenticator, RADIUS_AUTHLEN);
  MD5Update(&context, dst, 2);
  MD5Final(b, &context);
  dst[2] = (uint8_t)srclen ^ b[0]; /* Length of key */
  for (i = 1; i < RADIUS_MD5LEN; i++)
    if ((i-1) < (int)srclen)
      dst[i+2] = src[i-1] ^ b[i];
    else
      dst[i+2] = b[i];

  /* Get MD5 hash on secret + c(n-1) (Next j 16 octets) */
  for (n=1; n < blocks; n++) {
    MD5Init(&context);
    MD5Update(&context, (uint8_t *)secret, secretlen);
    MD5Update(&context, &dst[2 + ((n-1) * RADIUS_MD5LEN)], RADIUS_MD5LEN);
    MD5Final(b, &context);
    for (i = 0; i < RADIUS_MD5LEN; i++)
      if ((i-1) < (int)srclen)
	dst[i+2+n*RADIUS_MD5LEN] = src[i-1+n*RADIUS_MD5LEN] ^ b[i];
      else
	dst[i+2+n*RADIUS_MD5LEN] = b[i];
  }

  return 0;
}


/* 
 * radius_pwdecode()
 * Decode a password using MD5. Also used for MSCHAPv1 MPPE keys.
 */
int radius_pwdecode(struct radius_t *this, 
		    uint8_t *dst, size_t dstsize, size_t *dstlen, 
		    uint8_t *src, size_t srclen, 
		    uint8_t *authenticator, 
		    char *secret, size_t secretlen) {
  int i, n;
  MD5_CTX context;
  unsigned char output[RADIUS_MD5LEN];

  if (srclen > dstsize) {
    log_err(0, "radius_pwdecode srclen larger than dstsize");
    return -1;
  }

  if (srclen % RADIUS_MD5LEN) {
    log_err(0, "radius_pwdecode srclen is not multiple of 16 octets");
    return -1;
  }

  *dstlen = srclen;

  if (this->debug) {
    printf("pwdecode srclen %d\n", srclen);
    for (n=0; n< srclen; n++) {
      printf("%.2x ", src[n]);
      if ((n % 16) == 15)
	printf("\n");
    }
    printf("\n");

    printf("pwdecode authenticator \n");
    for (n=0; n< RADIUS_AUTHLEN; n++) {
      printf("%.2x ", authenticator[n]);
      if ((n % 16) == 15)
	printf("\n");
    }
    printf("\n");

    printf("pwdecode secret \n");
    for (n=0; n< secretlen; n++) {
      printf("%.2x ", secret[n]);
      if ((n % 16) == 15)
	printf("\n");
    }
    printf("\n");
  }

  /* Get MD5 hash on secret + authenticator */
  MD5Init(&context);
  MD5Update(&context, (uint8_t*) secret, secretlen);
  MD5Update(&context, authenticator, RADIUS_AUTHLEN);
  MD5Final(output, &context);

  /* XOR first 16 octets of passwd with MD5 hash */
  for (i = 0; i < RADIUS_MD5LEN; i++)
    dst[i] = src[i] ^ output[i];

  /* Continue with the remaining octets of passwd if any */
  for (n = RADIUS_MD5LEN; n < 128 && n < *dstlen; n += RADIUS_MD5LEN) {
    MD5Init(&context);
    MD5Update(&context, (uint8_t*) secret, secretlen);
    MD5Update(&context, src + n - RADIUS_MD5LEN, RADIUS_MD5LEN);
    MD5Final(output, &context);
    for (i = 0; i < RADIUS_MD5LEN; i++)
      dst[i + n] = src[i + n] ^ output[i];
  }    

  if (this->debug) {
    printf("pwdecode dest \n");
    for (n=0; n< 32; n++) {
      printf("%.2x ", dst[n]);
      if ((n % 16) == 15)
	printf("\n");
    }
    printf("\n");
  }

  return 0;
}


/* 
 * radius_pwencode()
 * Encode a password using MD5.
 */
int radius_pwencode(struct radius_t *this, 
		    uint8_t *dst, size_t dstsize,
		    size_t *dstlen, 
		    uint8_t *src, size_t srclen, 
		    uint8_t *authenticator, 
		    char *secret, size_t secretlen) {

  unsigned char output[RADIUS_MD5LEN];
  MD5_CTX context;
  size_t i, n;

  memset(dst, 0, dstsize);

  /* Make dstlen multiple of 16 */
  if (srclen & 0x0f) 
    *dstlen = (srclen & 0xf0) + 0x10; /* Padding 1 to 15 zeros */
  else
    *dstlen = srclen;                 /* No padding */

  /* Is dstsize too small ? */
  if (dstsize <= *dstlen) {
    *dstlen = 0;
    return -1;
  }

  /* Copy first 128 octets of src into dst */
  if (srclen > 128) 
    memcpy(dst, src, 128);
  else
    memcpy(dst, src, srclen);

  /* Get MD5 hash on secret + authenticator */
  MD5Init(&context);
  MD5Update(&context, (uint8_t*) secret, secretlen);
  MD5Update(&context, authenticator, RADIUS_AUTHLEN);
  MD5Final(output, &context);

  /* XOR first 16 octets of dst with MD5 hash */
  for (i = 0; i < RADIUS_MD5LEN; i++)
    dst[i] ^= output[i];

  /* Continue with the remaining octets of dst if any */
  for (n = RADIUS_MD5LEN; n < *dstlen; n += RADIUS_MD5LEN) {
    MD5Init(&context);
    MD5Update(&context, (uint8_t*) secret, secretlen);
    MD5Update(&context, dst + n - RADIUS_MD5LEN, RADIUS_MD5LEN);
    MD5Final(output, &context);
    for (i = 0; i < RADIUS_MD5LEN; i++)
      dst[i + n] ^= output[i];
  }    

  return 0;
}


/* 
 * radius_new()
 * Allocate a new radius instance.
 */
int radius_new(struct radius_t **this,
	       struct in_addr *listen, uint16_t port, int coanocheck,
	       struct in_addr *proxylisten, uint16_t proxyport,
	       struct in_addr *proxyaddr, struct in_addr *proxymask,
	       char* proxysecret) {
  struct sockaddr_in addr;
  struct radius_t *new_radius;

  /* Allocate storage for instance */
  if (!(new_radius = calloc(sizeof(struct radius_t), 1))) {
    log_err(0, "calloc() failed");
    return -1;
  }

  new_radius->coanocheck = coanocheck;

  /* Radius parameters */
  new_radius->ouraddr.s_addr = listen->s_addr;
  new_radius->ourport = port;

  /* Proxy parameters */
  if (proxylisten && proxyport && proxysecret) {
    new_radius->proxylisten.s_addr = proxylisten->s_addr;
    new_radius->proxyport = proxyport;
    
    if (proxyaddr)
      new_radius->proxyaddr.s_addr = proxyaddr->s_addr;
    else
      new_radius->proxyaddr.s_addr = ~0;
    
    if (proxymask)
      new_radius->proxymask.s_addr = proxymask->s_addr;
    else
      new_radius->proxymask.s_addr = 0;
    
    if ((new_radius->proxysecretlen = strlen(proxysecret)) < RADIUS_SECRETSIZE) {
      memcpy(new_radius->proxysecret, proxysecret, new_radius->proxysecretlen);
    }
  }

  /* Initialise queue */
  new_radius->next = 0;
  new_radius->first = -1;
  new_radius->last = -1;
  
  if ((new_radius->urandom_fp = fopen("/dev/urandom", "r")) < 0) {
    log_err(errno, "fopen(/dev/urandom, r) failed");
    free(new_radius);
    return -1;
  }
  
  /* Initialise radius socket */
  if ((new_radius->fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 ) {
    log_err(errno, "socket() failed!");
    fclose(new_radius->urandom_fp);
    free(new_radius);
    return -1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr = new_radius->ouraddr;
  addr.sin_port = htons(new_radius->ourport);
  
  if (bind(new_radius->fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
    log_err(errno, "bind() failed!");
    fclose(new_radius->urandom_fp);
    close(new_radius->fd);
    free(new_radius);
    return -1;
  }

  /* Initialise proxy socket */
  if (proxylisten && proxyport && proxysecret) {
    if ((new_radius->proxyfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 ) {
      log_err(errno, "socket() failed for proxyfd!");
      fclose(new_radius->urandom_fp);
      close(new_radius->fd);
      free(new_radius);
      return -1;
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr = new_radius->proxylisten;
    addr.sin_port = htons(new_radius->proxyport);
    
    if (bind(new_radius->proxyfd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
      log_err(errno, "bind() failed for proxylisten!");
      fclose(new_radius->urandom_fp);
      close(new_radius->fd);
      close(new_radius->proxyfd);
      free(new_radius);
      return -1;
    }
  }
  else {
    new_radius->proxyfd = -1; /* Indicate that proxy is not used */
  }

  *this = new_radius;
  return 0;
}


/* 
 * radius_free()
 * Free a radius instance. (Undo radius_new() 
 */
int 
radius_free(struct radius_t *this)
{
  if (fclose(this->urandom_fp)) {
    log_err(errno, "fclose() failed!");
  }
  if (close(this->fd)) {
    log_err(errno, "close() failed!");
  }
  free(this);
  return 0;
}

void radius_set(struct radius_t *this, unsigned char *hwaddr, int debug) {
  this->debug = debug;

  /* Remote radius server parameters */
  this->hisaddr0.s_addr = options()->radiusserver1.s_addr;
  this->hisaddr1.s_addr = options()->radiusserver2.s_addr;

  if (options()->radiusauthport) {
    this->authport = options()->radiusauthport;
  }
  else {
    this->authport = RADIUS_AUTHPORT;
  }
  
  if (options()->radiusacctport) {
    this->acctport = options()->radiusacctport;
  }
  else {
    this->acctport = RADIUS_ACCTPORT;
  }

  if ((this->secretlen = strlen(options()->radiussecret)) > RADIUS_SECRETSIZE) {
    log_err(0, "Radius secret too long. Truncating to %d characters", 
	    RADIUS_SECRETSIZE);
    this->secretlen = RADIUS_SECRETSIZE;
  }

  if (hwaddr)
    memcpy(this->nas_hwaddr, hwaddr, sizeof(this->nas_hwaddr));

  memcpy(this->secret, options()->radiussecret, this->secretlen);

  this->lastreply = 0; /* Start out using server 0 */  
  return;
}


/* 
 * radius_set_cb_ind()
 * Set callback function received requests
 */
int radius_set_cb_ind(struct radius_t *this,
  int (*cb_ind) (struct radius_t *radius, struct radius_packet_t *pack,
		 struct sockaddr_in *peer)) {

  this->cb_ind = cb_ind;
  return 0;
}


/* 
 * radius_set_cb_auth_conf()
 * Set callback function for responses to access request
 */
int
radius_set_cb_auth_conf(struct radius_t *this,
int (*cb_auth_conf) (struct radius_t *radius, struct radius_packet_t *pack,
		       struct radius_packet_t *pack_req, void *cbp)) {

  this->cb_auth_conf = cb_auth_conf;
  return 0;
}

/* 
 * radius_set_cb_acct_conf()
 * Set callback function for responses to accounting request
 */
int
radius_set_cb_acct_conf(struct radius_t *this,
int (*cb_acct_conf) (struct radius_t *radius, struct radius_packet_t *pack,
		     struct radius_packet_t *pack_req, void *cbp)) {

  this->cb_acct_conf = cb_acct_conf;
  return 0;
}

/* 
 * radius_set_cb_coa_ind()
 * Set callback function for coa and disconnect request
 */
int
radius_set_cb_coa_ind(struct radius_t *this,
int (*cb_coa_ind) (struct radius_t *radius, struct radius_packet_t *pack,
		   struct sockaddr_in *peer)) {

  this->cb_coa_ind = cb_coa_ind;
  return 0;
}


/* 
 * radius_req()
 * Send of a packet and place it in the retransmit queue
 */
int radius_req(struct radius_t *this,
	       struct radius_packet_t *pack,
	       void *cbp)
{
  struct sockaddr_in addr;
  size_t len = ntohs(pack->length);

  /* Place packet in queue */
  if (radius_queue_in(this, pack, cbp)) {
    return -1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;

  if (this->debug) printf("Lastreply: %d\n", this->lastreply);

  if (!this->lastreply) {
    addr.sin_addr = this->hisaddr0;
  }
  else {
    addr.sin_addr = this->hisaddr1;
  }

  if (pack->code == RADIUS_CODE_ACCOUNTING_REQUEST)
    addr.sin_port = htons(this->acctport);
  else
    addr.sin_port = htons(this->authport);
      
  
  if (sendto(this->fd, pack, len, 0,
	     (struct sockaddr *) &addr, sizeof(addr)) < 0) {
    log_err(errno, "sendto(%s) failed!", inet_ntoa(addr.sin_addr));
    return -1;
  } 
    
  return 0;
}


/* 
 * radius_resp()
 * Send of a packet (no retransmit queue)
 */
int radius_resp(struct radius_t *this,
		struct radius_packet_t *pack,
		struct sockaddr_in *peer, uint8_t *req_auth) {

  size_t len = ntohs(pack->length);
  struct radius_attr_t *ma = NULL; /* Message authenticator */

  /* Prepare for message authenticator TODO */
  memset(pack->authenticator, 0, RADIUS_AUTHLEN);
  memcpy(pack->authenticator, req_auth, RADIUS_AUTHLEN);

  /* If packet contains message authenticator: Calculate it! */
  if (!radius_getattr(pack, &ma, RADIUS_ATTR_MESSAGE_AUTHENTICATOR, 0,0,0)) {
    radius_hmac_md5(this, pack, this->proxysecret, this->proxysecretlen, ma->v.t);
  }

  radius_authresp_authenticator(this, pack, req_auth, 
				this->proxysecret,
				this->proxysecretlen);
  
  if (sendto(this->proxyfd, pack, len, 0,
	     (struct sockaddr *) peer, sizeof(struct sockaddr_in)) < 0) {
    log_err(errno, "sendto() failed!");
    return -1;
  } 
  
  return 0;
}

/* 
 * radius_coaresp()
 * Send of a packet (no retransmit queue)
 */
int radius_coaresp(struct radius_t *this,
		   struct radius_packet_t *pack,
		   struct sockaddr_in *peer, uint8_t *req_auth) {

  size_t len = ntohs(pack->length);
  struct radius_attr_t *ma = NULL; /* Message authenticator */

  /* Prepare for message authenticator TODO */
  memset(pack->authenticator, 0, RADIUS_AUTHLEN);
  memcpy(pack->authenticator, req_auth, RADIUS_AUTHLEN);

  /* If packet contains message authenticator: Calculate it! */
  if (!radius_getattr(pack, &ma, RADIUS_ATTR_MESSAGE_AUTHENTICATOR, 0,0,0)) {
    radius_hmac_md5(this, pack, this->secret, this->secretlen, ma->v.t);
  }

  radius_authresp_authenticator(this, pack, req_auth,
				this->secret,
				this->secretlen);
  
  if (sendto(this->fd, pack, len, 0,
	     (struct sockaddr *) peer, sizeof(struct sockaddr_in)) < 0) {
    log_err(errno, "sendto() failed!");
    return -1;
  } 
  
  return 0;
}


/* 
 * radius_default_pack()
 * Return an empty packet which can be used in subsequent to 
 * radius_addattr()
 */
int
radius_default_pack(struct radius_t *this,
		    struct radius_packet_t *pack, 
		    int code)
{
  memset(pack, 0, RADIUS_PACKSIZE);
  pack->code = code;
  pack->id = 0; /* Let the send procedure queue the packet and assign id */
  pack->length = htons(RADIUS_HDRSIZE);
  
  if (fread(pack->authenticator, 1, RADIUS_AUTHLEN, this->urandom_fp) != RADIUS_AUTHLEN) {
    log_err(errno, "fread() failed");
    return -1;
  }

  /* always add the chillispot version to requests */
  radius_addattr(this, pack, RADIUS_ATTR_VENDOR_SPECIFIC,
		 RADIUS_VENDOR_CHILLISPOT, RADIUS_ATTR_CHILLISPOT_VERSION, 
		 0, (uint8_t*)VERSION, strlen(VERSION));

  if (code == RADIUS_CODE_ACCOUNTING_REQUEST) {

    /*
     * For accounting, always indicate the "direction" of accounting
     * up / down data measurements.
     */

    uint32_t v = options()->swapoctets ? 
      RADIUS_VALUE_CHILLISPOT_NAS_VIEWPOINT :
      RADIUS_VALUE_CHILLISPOT_CLIENT_VIEWPOINT;
      
    radius_addattr(this, pack, 
		   RADIUS_ATTR_VENDOR_SPECIFIC,
		   RADIUS_VENDOR_CHILLISPOT,
		   RADIUS_ATTR_CHILLISPOT_ACCT_VIEW_POINT, 
		   v, 0, 0);
  }
  
  return 0;
}


/* 
 * radius_authcheck()
 * Check that the authenticator on a reply is correct.
 */
int radius_authcheck(struct radius_t *this, struct radius_packet_t *pack, 
		     struct radius_packet_t *pack_req)
{
  uint8_t auth[RADIUS_AUTHLEN];
  MD5_CTX context;

  MD5Init(&context);
  MD5Update(&context, (uint8_t *) pack, RADIUS_HDRSIZE-RADIUS_AUTHLEN);
  MD5Update(&context, pack_req->authenticator, RADIUS_AUTHLEN);
  MD5Update(&context, ((uint8_t *) pack) + RADIUS_HDRSIZE, 
	    ntohs(pack->length) - RADIUS_HDRSIZE);
  MD5Update(&context, (uint8_t *)this->secret, this->secretlen);
  MD5Final(auth, &context);
  
  return memcmp(pack->authenticator, auth, RADIUS_AUTHLEN);
}

/* 
 * radius_acctcheck()
 * Check that the authenticator on an accounting request is correct.
 */
int radius_acctcheck(struct radius_t *this, struct radius_packet_t *pack)
{
  uint8_t auth[RADIUS_AUTHLEN];
  uint8_t padd[RADIUS_AUTHLEN] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
  MD5_CTX context;
  
  MD5Init(&context);
  MD5Update(&context, (uint8_t *)pack, RADIUS_HDRSIZE-RADIUS_AUTHLEN);
  MD5Update(&context, (uint8_t *)padd, RADIUS_AUTHLEN);
  MD5Update(&context, ((uint8_t *)pack) + RADIUS_HDRSIZE, 
	    ntohs(pack->length) - RADIUS_HDRSIZE);
  MD5Update(&context, (uint8_t *)this->secret, this->secretlen);
  MD5Final(auth, &context);
  
  return memcmp(pack->authenticator, auth, RADIUS_AUTHLEN);
}


/* 
 * radius_decaps()
 * Read and process a received radius packet.
 */
int radius_decaps(struct radius_t *this) {
  ssize_t status;
  struct radius_packet_t pack;
  struct radius_packet_t pack_req;
  void *cbp;
  struct sockaddr_in addr;
  socklen_t fromlen = sizeof(addr);
  int coarequest = 0;

  if (this->debug) 
    log_dbg("Received radius packet");
  
  if ((status = recvfrom(this->fd, &pack, sizeof(pack), 0, 
			 (struct sockaddr *) &addr, &fromlen)) <= 0) {
    log_err(errno, "recvfrom() failed");
    return -1;
  }

  if (status < RADIUS_HDRSIZE) {
    log_warn(0, "Received radius packet which is too short: %d < %d!",
	     status, RADIUS_HDRSIZE);
    return -1;
  }

  if (ntohs(pack.length) != (uint16_t)status) {
    log_warn(errno, "Received radius packet with wrong length field %d != %d!",
	     ntohs(pack.length), status);
    return -1;
  }

  switch (pack.code) {
  case RADIUS_CODE_DISCONNECT_REQUEST:
  case RADIUS_CODE_COA_REQUEST:
    coarequest = 1;
    break;
  default:
    coarequest = 0;
    break;
  }

  if (!coarequest) {
    /* Check that reply is from correct address */
    if ((addr.sin_addr.s_addr != this->hisaddr0.s_addr) &&
	(addr.sin_addr.s_addr != this->hisaddr1.s_addr)) {
      log_warn(0, "Received radius reply from wrong address %.8x!",
	      addr.sin_addr.s_addr);
      return -1;
    }
    
    /* Check that UDP source port is correct */
    if ((addr.sin_port != htons(this->authport)) &&
	(addr.sin_port != htons(this->acctport))) {
      log_warn(0, "Received radius packet from wrong port %.4x!",
	      addr.sin_port);
      return -1;
    }
    
    if (radius_queue_out(this, &pack_req, pack.id, &cbp)) {
      log_warn(0, "Matching request was not found in queue: %d!", pack.id);
      return -1;
    }

    if (radius_authcheck(this, &pack, &pack_req)) {
      log_warn(0, "Authenticator does not match request!");
      return -1;
    }

    /* Set which radius server to use next */
    if (addr.sin_addr.s_addr == this->hisaddr0.s_addr)
      this->lastreply = 0;
    else
      this->lastreply = 1;
    
  }
  else {
    if (!this->coanocheck) {
      /* Check that request is from correct address */
      if ((addr.sin_addr.s_addr != this->hisaddr0.s_addr) &&
	  (addr.sin_addr.s_addr != this->hisaddr1.s_addr)) {
	log_warn(0, "Received radius request from wrong address %.8x!",
		addr.sin_addr.s_addr);
	return -1;
      }
    }

    if (radius_acctcheck(this, &pack)) {
      log_warn(0, "Authenticator did not match MD5 of packet!");
      return -1;
    }
  }
    
  /* TODO: Check consistency of attributes vs packet length */
  
  switch (pack.code) {
  case RADIUS_CODE_ACCESS_ACCEPT:
  case RADIUS_CODE_ACCESS_REJECT:
  case RADIUS_CODE_ACCESS_CHALLENGE:
  case RADIUS_CODE_DISCONNECT_ACK:
  case RADIUS_CODE_DISCONNECT_NAK:
  case RADIUS_CODE_STATUS_ACCEPT:
  case RADIUS_CODE_STATUS_REJECT:
    if (this->cb_auth_conf)
      return this->cb_auth_conf(this, &pack, &pack_req, cbp);
    else
      return 0;
    break;
  case RADIUS_CODE_ACCOUNTING_RESPONSE:
    if (this->cb_acct_conf)
      return this->cb_acct_conf(this, &pack, &pack_req, cbp);
    else
      return 0;
    break;
  case RADIUS_CODE_DISCONNECT_REQUEST:
  case RADIUS_CODE_COA_REQUEST:
    if (this->cb_coa_ind)
      return this->cb_coa_ind(this, &pack, &addr);
    else
      return 0;
    break;
  default:
    log_warn(0, "Received unknown radius packet %d!", pack.code);
    return -1;
  }
  
  log_warn(0, "Received unknown radius packet %d!", pack.code);
  return -1;
}

/* 
 * radius_proxy_ind()
 * Read and process a received radius packet.
 */
int radius_proxy_ind(struct radius_t *this) {
  ssize_t status;
  struct radius_packet_t pack;
  struct sockaddr_in addr;
  socklen_t fromlen = sizeof(addr);

  if (this->debug) 
    log_dbg("Received radius packet");
  
  if ((status = recvfrom(this->proxyfd, &pack, sizeof(pack), 0, 
			 (struct sockaddr *) &addr, &fromlen)) <= 0) {
    log_err(errno, "recvfrom() failed");
    return -1;
  }

  if (status < RADIUS_HDRSIZE) {
    log_warn(0, "Received radius packet which is too short: %d < %d!",
	    status, RADIUS_HDRSIZE);
    return -1;
  }

  if (ntohs(pack.length) != (uint16_t)status) {
    log_err(0, "Received radius packet with wrong length field %d != %d!",
	    ntohs(pack.length), status);
    return -1;
  }

  /* Is this a known request? */
  if ((this->cb_ind) &&
      ((pack.code == RADIUS_CODE_ACCESS_REQUEST) ||
       (pack.code == RADIUS_CODE_ACCOUNTING_REQUEST) ||
       (pack.code == RADIUS_CODE_DISCONNECT_REQUEST) ||
       (pack.code == RADIUS_CODE_STATUS_REQUEST))) {

    /* Check that request is from a known client */
    /* Any of the two servers or from one of the clients */
    if ((addr.sin_addr.s_addr & this->proxymask.s_addr)!= 
	this->proxyaddr.s_addr) {
      log_warn(0, "Received radius request from wrong address %.8x!",
	       addr.sin_addr.s_addr);
      return -1;
    }
    
    return this->cb_ind(this, &pack, &addr);
  }

  log_warn(0, "Received unknown radius packet %d!", pack.code);
  return -1;
}

struct app_conn_t admin_session;

int chilliauth_radius(struct radius_t *radius) {
  struct radius_packet_t radius_pack;
  int ret = -1;

  /*if (admin_session.s_state.authenticated) {
    terminate_appconn(&admin_session, RADIUS_TERMINATE_CAUSE_SESSION_TIMEOUT);
    memset(&admin_session.s_state, 0, sizeof(admin_session.s_state));
    }*/
  
  if (radius_default_pack(radius, &radius_pack, RADIUS_CODE_ACCESS_REQUEST)) {
    log_err(0, "radius_default_pack() failed");
    return ret;
  }

  /* adminuser is required */
  radius_addattr(radius, &radius_pack, RADIUS_ATTR_USER_NAME, 0, 0, 0,
		 (uint8_t *)options()->adminuser, strlen(options()->adminuser));

  if (options()->adminpasswd)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_USER_PASSWORD, 0, 0, 0,
		   (uint8_t *)options()->adminpasswd, strlen(options()->adminpasswd));

  radius_addattr(radius, &radius_pack, RADIUS_ATTR_SERVICE_TYPE, 0, 0,
		 RADIUS_SERVICE_TYPE_ADMIN_USER, NULL, 0); 
  
  
  radius_addattr(radius, &radius_pack, RADIUS_ATTR_NAS_PORT_TYPE, 0, 0,
		 options()->radiusnasporttype, NULL, 0);
  
  radius_addnasip(radius, &radius_pack);
  
  radius_addcalledstation(radius, &radius_pack);

  if (options()->radiusnasid)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_NAS_IDENTIFIER, 0, 0, 0,
		   (uint8_t *)options()->radiusnasid, strlen(options()->radiusnasid));
  
  if (options()->radiuslocationid)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
		   RADIUS_VENDOR_WISPR, RADIUS_ATTR_WISPR_LOCATION_ID, 0,
		   (uint8_t *)options()->radiuslocationid, strlen(options()->radiuslocationid));
  
  if (options()->radiuslocationname)
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_VENDOR_SPECIFIC,
		   RADIUS_VENDOR_WISPR, RADIUS_ATTR_WISPR_LOCATION_NAME, 0,
		   (uint8_t *)options()->radiuslocationname, 
		   strlen(options()->radiuslocationname));

  radius_addattr(radius, &radius_pack, RADIUS_ATTR_ACCT_SESSION_ID, 0, 0, 0,
		 (uint8_t*)admin_session.s_state.sessionid, REDIR_SESSIONID_LEN-1);

  if (admin_session.s_state.redir.classlen) {
    radius_addattr(radius, &radius_pack, RADIUS_ATTR_CLASS, 0, 0, 0,
		   admin_session.s_state.redir.classbuf,
		   admin_session.s_state.redir.classlen);
  }

  radius_addattr(radius, &radius_pack, RADIUS_ATTR_MESSAGE_AUTHENTICATOR, 
		 0, 0, 0, NULL, RADIUS_MD5LEN);

  return radius_req(radius, &radius_pack, &admin_session); 
}

