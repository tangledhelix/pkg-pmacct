/*
    pmacct (Promiscuous mode IP Accounting package)
    pmacct is Copyright (C) 2003-2006 by Paolo Lucente
*/

/*
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if no, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#define __PLUGIN_HOOKS_C

/* includes */
#include "pmacct.h"
#include "plugin_hooks.h"
#include "pkt_handlers.h"

/* functions */

/* load_plugins() starts plugin processes; creates pipes
   and handles them inserting in channels_list structure */

/* when not using map_shared, 'pipe_size' is the size of
   the pipe created with socketpair(); when map_shared is
   enabled, it refers to the size of the shared memory
   area */
void load_plugins(struct plugin_requests *req)
{
  int x, v, socklen;
  struct plugins_list_entry *list = plugins_list;
  int l = sizeof(list->cfg.pipe_size);
  struct channels_list_entry *chptr = NULL;

  init_random_seed(); 
  init_pipe_channels();

  while (list) {
    if ((*list->type.func)) {
#if defined (HAVE_MMAP)
    /* If nothing is supplied, let's hint some working default values */
    if (list->cfg.pcap_savefile && !list->cfg.pipe_size && !list->cfg.buffer_size) {
      list->cfg.pipe_size = 4096000; /* 4Mb */
      list->cfg.buffer_size = 10240; /* 10Kb */
    }
#endif
      /* creating communication channel */
      socketpair(AF_UNIX, SOCK_DGRAM, 0, list->pipe);
      if (list->cfg.pipe_size) {
	if (list->cfg.pipe_size < (sizeof(struct pkt_data)+sizeof(struct ch_buf_hdr)))
	  list->cfg.pipe_size = sizeof(struct pkt_data)+sizeof(struct ch_buf_hdr);
#if !defined (HAVE_MMAP)
	Setsocksize(list->pipe[0], SOL_SOCKET, SO_RCVBUF, &list->cfg.pipe_size, l);
	Setsocksize(list->pipe[1], SOL_SOCKET, SO_SNDBUF, &list->cfg.pipe_size, l);
#endif
      }
      else {
        x = DEFAULT_PIPE_SIZE;
	Setsocksize(list->pipe[0], SOL_SOCKET, SO_RCVBUF, &x, l);
        Setsocksize(list->pipe[1], SOL_SOCKET, SO_SNDBUF, &x, l);
      }

      /* checking SO_RCVBUF and SO_SNDBUF values; if different we take the smaller one */
      getsockopt(list->pipe[0], SOL_SOCKET, SO_RCVBUF, &v, &l);
      x = v;
      getsockopt(list->pipe[1], SOL_SOCKET, SO_SNDBUF, &v, &l);
      socklen = (v < x) ? v : x;

#if !defined (HAVE_MMAP)
      if ((socklen < list->cfg.pipe_size) || (list->cfg.debug)) 
	Log(LOG_INFO, "INFO ( %s/%s ): Pipe size obtained: %d / %d.\n", 
		list->name, list->type.string, socklen, list->cfg.pipe_size);
#endif

      /* checking transfer buffer size between 'core' <-> 'plugins' */
      if (list->cfg.buffer_size < (sizeof(struct pkt_data)+sizeof(struct ch_buf_hdr)))
        list->cfg.buffer_size = sizeof(struct pkt_data)+sizeof(struct ch_buf_hdr);
#if !defined (HAVE_MMAP)
      if (list->cfg.buffer_size > socklen)
        list->cfg.buffer_size = socklen; 
#else
      /* if we are not supplied a 'plugin_pipe_size', then we calculate it using
         buffer size and given socket size; if 'plugin_pipe_size' is known, we
	 reverse the method: we try to obtain needed socket size to accomodate
	 given pipe and buffer size */
      if (!list->cfg.pipe_size) { 
        list->cfg.pipe_size = (socklen/sizeof(char *))*list->cfg.buffer_size;
	if ((list->cfg.debug) || (list->cfg.pipe_size > WARNING_PIPE_SIZE))  {
          Log(LOG_INFO, "INFO ( %s/%s ): %d bytes are available to address shared memory segment; buffer size is %d bytes.\n",
			list->name, list->type.string, socklen, list->cfg.buffer_size);
	  Log(LOG_INFO, "INFO ( %s/%s ): Trying to allocate a shared memory segment of %d bytes.\n",
			list->name, list->type.string, list->cfg.pipe_size);
	}
      }
      else {
        if (list->cfg.buffer_size > list->cfg.pipe_size)
	  list->cfg.buffer_size = list->cfg.pipe_size;

        x = (list->cfg.pipe_size/list->cfg.buffer_size)*sizeof(char *);
        if (x > socklen) {
	  Setsocksize(list->pipe[0], SOL_SOCKET, SO_RCVBUF, &x, l);
	  Setsocksize(list->pipe[1], SOL_SOCKET, SO_SNDBUF, &x, l);
        }

        socklen = x;
        getsockopt(list->pipe[0], SOL_SOCKET, SO_RCVBUF, &v, &l);
        x = v;
        getsockopt(list->pipe[1], SOL_SOCKET, SO_SNDBUF, &v, &l);
        if (x > v) x = v;

        if ((x < socklen) || (list->cfg.debug))
          Log(LOG_INFO, "INFO ( %s/%s ): Pipe size obtained: %d / %d.\n", list->name, list->type.string, x, socklen);
      }
#endif

      /* compiling aggregation filter if needed */
      if (list->cfg.a_filter) {
	pcap_t *dev_desc = pcap_open_dead(1, 128); /* 128 bytes should be long enough */
	bpf_u_int32 localnet, netmask;  /* pcap library stuff */
	char errbuf[PCAP_ERRBUF_SIZE];

	pcap_lookupnet(config.dev, &localnet, &netmask, errbuf);
	if (pcap_compile(dev_desc, &list->cfg.bpfp_a_filter, list->cfg.a_filter, 0, netmask) < 0)
          Log(LOG_WARNING, "WARN: %s\nWARN ( %s/%s ): aggregation filter disabled.\n", 
			  pcap_geterr(dev_desc), config.name, config.type);
	pcap_close(dev_desc);
      }

      list->cfg.name = list->name;
      list->cfg.type = list->type.string;
      chptr = insert_pipe_channel(&list->cfg, list->pipe[1]);
      if (!chptr) {
	Log(LOG_ERR, "ERROR: Unable to setup a new Core Process <-> Plugin channel.\nExiting.\n"); 
	exit_all(1);
      }

#if defined (HAVE_MMAP)
      if (!memcmp(list->type.string, "memory", strlen(list->type.string))) // || !memcmp(list->type.string, "print", strlen(list->type.string)))
	chptr->request = TRUE; /* sets new value to be assigned to 'wakeup'; 'TRUE' disables on-request wakeup */ 
#endif

      switch (list->pid = fork()) {  
      case 0: /* Child */
	/* SIGCHLD handling issue: SysV avoids zombies by ignoring SIGCHLD; to emulate
	   such semantics on BSD systems, we need an handler like handle_falling_child() */
#if defined (IRIX) || (SOLARIS)
	signal(SIGCHLD, SIG_IGN);
#else
	signal(SIGCHLD, ignore_falling_child);
#endif
	close(list->pipe[1]);
	(*list->type.func)(list->pipe[0], &list->cfg, chptr);
	exit(0);
      default: /* Parent */
	close(list->pipe[0]);
	setnonblocking(list->pipe[1]);
	break;
      }

      /* some residual check */
      if (chptr && list->cfg.a_filter) req->bpf_filter = TRUE;
    }
    list = list->next;
  }

  sort_pipe_channels();
}

void exec_plugins(struct packet_ptrs *pptrs) 
{
  int num;
  struct pkt_data *pdata;
  char *bptr;
  int index;

  for (index = 0; channels_list[index].aggregation; index++) {
    if (bpf_filter(channels_list[index].filter->bf_insns, pptrs->packet_ptr, pptrs->pkthdr->len,
        pptrs->pkthdr->caplen) && (!channels_list[index].tag_filter.num ||
	!evaluate_tags(&channels_list[index].tag_filter, pptrs->tag)) && 
	!evaluate_sampling(&channels_list[index].s)) {
      /* constructing buffer: supported primitives + packet total length */
      num = 0;
#if !defined (HAVE_MMAP)
      pdata = (struct pkt_data *) channels_list[index].bufptr;
#else
      /* rg.ptr points to slot's base address into the ring (shared memory); bufptr works
	 as a displacement into the slot to place sequentially packets */
      /* bufptr -> bptr -> pdata: avoids lvalue crap. Signalled by Andreas Jochens on AMD64/gcc4.0 */
      bptr = channels_list[index].rg.ptr+ChBufHdrSz+channels_list[index].bufptr; 
      pdata = (struct pkt_data *)bptr; 
      memset(pdata, 0, PdataSz);
#endif
      while (channels_list[index].phandler[num]) {
        (*channels_list[index].phandler[num])(&channels_list[index], pptrs, pdata);
        num++;
      }

#if !defined (HAVE_MMAP)
      ((struct ch_buf_hdr *)channels_list[index].buf)->num++;
#else
      channels_list[index].hdr.num++;
#endif
      channels_list[index].bufptr += PdataSz;

      if ((channels_list[index].bufptr+PdataSz) > channels_list[index].bufend) {
#if !defined (HAVE_MMAP)
        if (write(channels_list[index].pipe, channels_list[index].buf, channels_list[index].bufsize) == -1) {
	  if ((errno == EAGAIN) || (errno == ENOBUFS))
	    Log(LOG_ERR, "ERROR: Pipe full. Raise maximum socket size for your system or try with a larger 'plugin_buffer_size' value. We are missing data.\n");
        }

	/* rewind pointer */
        channels_list[index].bufptr = channels_list[index].buf+ChBufHdrSz;
        ((struct ch_buf_hdr *)channels_list[index].buf)->num = 0;
#else
	channels_list[index].hdr.seq++;
	channels_list[index].hdr.seq %= MAX_SEQNUM;

	((struct ch_buf_hdr *)channels_list[index].rg.ptr)->seq = channels_list[index].hdr.seq;
	((struct ch_buf_hdr *)channels_list[index].rg.ptr)->num = channels_list[index].hdr.num;

	if (channels_list[index].status->wakeup) {
	  channels_list[index].status->wakeup = channels_list[index].request;
	  write(channels_list[index].pipe, &channels_list[index].rg.ptr, CharPtrSz); 
	}
	channels_list[index].rg.ptr += channels_list[index].bufsize;

	if ((channels_list[index].rg.ptr+channels_list[index].bufsize) > channels_list[index].rg.end)
	  channels_list[index].rg.ptr = channels_list[index].rg.base;

        /* rewind pointer */
        channels_list[index].bufptr = channels_list[index].buf;
        channels_list[index].hdr.num = 0;
#endif
      }
    }
  }
}

struct channels_list_entry *insert_pipe_channel(struct configuration *cfg, int pipe)
{
  struct channels_list_entry *chptr; 
  int index = 0, x;  

  while (index < MAX_N_PLUGINS) {
    chptr = &channels_list[index]; 
    if (!chptr->aggregation) { /* found room */
      chptr->aggregation = cfg->what_to_count;
      chptr->pipe = pipe; 
      chptr->filter = &cfg->bpfp_a_filter; 
      chptr->bufsize = cfg->buffer_size;
      chptr->id = cfg->post_tag;
      if (cfg->sampling_rate) chptr->s.rate = cfg->sampling_rate;
      for (x = 0; x < cfg->ptf.num; x++) chptr->tag_filter.table[x] = cfg->ptf.table[x];
      chptr->tag_filter.num = cfg->ptf.num;
#if !defined (HAVE_MMAP)
      chptr->buf = malloc(cfg->buffer_size);
      if (!chptr->buf) {
        Log(LOG_ERR, "ERROR ( %s/%s ): unable to allocate channel buffer. Exiting ...\n", cfg->name, cfg->type);
        exit_all(1);
      } 
      memset(chptr->buf, 0, cfg->buffer_size);
      chptr->bufptr = chptr->buf+sizeof(struct ch_buf_hdr);
      chptr->bufend = chptr->buf+cfg->buffer_size;
#else
      chptr->buf = 0;
      chptr->bufptr = chptr->buf;
      chptr->bufend = cfg->buffer_size-sizeof(struct ch_buf_hdr);

      /* +1550 (NETFLOW_MSG_SIZE) has been introduced as a margin as a
         countermeasure against the reception of malicious NetFlow v9
	 templates */
      chptr->rg.base = map_shared(0, cfg->pipe_size+1550, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
      if (chptr->rg.base == MAP_FAILED) {
        Log(LOG_ERR, "ERROR ( %s/%s ): unable to allocate pipe buffer. Exiting ...\n", cfg->name, cfg->type); 
	exit_all(1);
      }
      memset(chptr->rg.base, 0, cfg->pipe_size);
      chptr->rg.ptr = chptr->rg.base;
      chptr->rg.end = chptr->rg.base+cfg->pipe_size;

      chptr->status = map_shared(0, sizeof(struct ch_status), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
      if (chptr->status == MAP_FAILED) {
        Log(LOG_ERR, "ERROR ( %s/%s ): unable to allocate status buffer. Exiting ...\n", cfg->name, cfg->type);
        exit_all(1);
      }
      memset(chptr->status, 0, sizeof(struct ch_status));
#endif

      break;
    }
    else chptr = NULL; 

    index++;
  }

  return chptr;
}

void delete_pipe_channel(int pipe)
{
  struct channels_list_entry *chptr;
  int index = 0, index2;

  while (index < MAX_N_PLUGINS) {
    chptr = &channels_list[index];

    if (chptr->pipe == pipe) {
      chptr->aggregation = FALSE;
	
      /* we ensure that any plugin is depending on the one
	 being removed via the 'same_aggregate' flag */
      if (!chptr->same_aggregate) {
	index2 = index;
	for (index2++; index2 < MAX_N_PLUGINS; index2++) {
	  chptr = &channels_list[index2];

	  if (!chptr->aggregation) break; /* we finished channels */
	  if (chptr->same_aggregate) {
	    chptr->same_aggregate = FALSE;
	    break; 
	  }
	  else break; /* we have nothing to do */
	}
      }

      index2 = index;
      for (index2++; index2 < MAX_N_PLUGINS; index2++) {
	chptr = &channels_list[index2];
	if (chptr->aggregation) {
	  memcpy(&channels_list[index], chptr, sizeof(struct channels_list_entry)); 
	  memset(chptr, 0, sizeof(struct channels_list_entry)); 
	  index++;
	}
	else break; /* we finished channels */
      }
       
      break;
    }

    index++;
  }
}

/* trivial sorting(tm) :-) */
void sort_pipe_channels()
{
  struct channels_list_entry ctmp;
  int x = 0, y = 0; 

  while (x < MAX_N_PLUGINS) {
    if (!channels_list[x].aggregation) break;
    y = x+1; 
    while (y < MAX_N_PLUGINS) {
      if (!channels_list[y].aggregation) break;
      if (channels_list[x].aggregation == channels_list[y].aggregation) {
	channels_list[y].same_aggregate = TRUE;
	if (y == x+1) x++;
	else {
	  memcpy(&ctmp, &channels_list[x+1], sizeof(struct channels_list_entry));
	  memcpy(&channels_list[x+1], &channels_list[y], sizeof(struct channels_list_entry));
	  memcpy(&channels_list[y], &ctmp, sizeof(struct channels_list_entry));
	  x++;
	}
      }
      y++;
    }
    x++;
  }
}

void init_pipe_channels()
{
  memset(&channels_list, 0, MAX_N_PLUGINS*sizeof(struct channels_list_entry)); 
}

/* return value:
   FALSE: take this packet into sample 
   TRUE: discard this packet
*/ 
int evaluate_sampling(struct sampling *s)
{
  if (!s->rate) return FALSE; /* sampling is disabled */
  
  if (s->counter) {
    s->counter--; 
    return TRUE;
  }
  else {
    /* simple systematic algorithm */
    // s->counter = s->rate;

    /* simple random algorithm */
    if (s->rate > 1) {
      s->counter = ((random() % ((2 * s->rate) - 1)) + 1);
      srandom(random());
    }
    else s->counter = 1; /* s->rate == 1 */

    return FALSE;
  }
}

/* return value:
   FALSE: packet matched the filter
   TRUE: discard this packet
*/
int evaluate_tags(struct pretag_filter *filter, u_int16_t tag)
{
  int index;

  if (filter->num == 0) return FALSE; /* no entries in the filter array: tag filtering disabled */
  
  for (index = 0; index < filter->num; index++) {
    if (filter->table[index] == tag) return FALSE;
  }

  return TRUE;
}

void recollect_pipe_memory(struct channels_list_entry *mychptr)
{
  struct channels_list_entry *chptr;
  int index = 0;

  while (index < MAX_N_PLUGINS) {
    chptr = &channels_list[index];
#if defined (HAVE_MMAP)
    if (mychptr->rg.base != chptr->rg.base) {
      munmap(chptr->rg.base, (chptr->rg.end-chptr->rg.base)+1550);
      munmap(chptr->status, sizeof(struct ch_status));
    }
#endif
    index++;
  }
}

void init_random_seed()
{
  struct timeval tv; struct timezone tz;

  gettimeofday(&tv, &tz);
  srandom((unsigned int)tv.tv_usec);
}

void fill_pipe_buffer()
{
  int index;

  for (index = 0; channels_list[index].aggregation; index++) {
#if !defined (HAVE_MMAP)
    write(channels_list[index].pipe, channels_list[index].buf, channels_list[index].bufsize); 
#else
    channels_list[index].hdr.seq++;
    channels_list[index].hdr.seq %= MAX_SEQNUM;

    ((struct ch_buf_hdr *)channels_list[index].rg.ptr)->seq = channels_list[index].hdr.seq;
    ((struct ch_buf_hdr *)channels_list[index].rg.ptr)->num = channels_list[index].hdr.num;

    if (channels_list[index].status->wakeup) {
      channels_list[index].status->wakeup = channels_list[index].request;
      write(channels_list[index].pipe, &channels_list[index].rg.ptr, CharPtrSz);
    }
#endif
  }
}