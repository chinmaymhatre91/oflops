#include "config.h"
#include <assert.h>
#include <dlfcn.h>
#include <pcap.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <sys/ioctl.h>
#include <sys/socket.h>


#include "module_run.h"
#include "module_default.h"
#include "test_module.h"
#include "utils.h"



static void test_module_loop(oflops_context *ctx, test_module *mod);
static void process_event(oflops_context *ctx, test_module * mod, struct pollfd *fd);
static void process_control_event(oflops_context *ctx, test_module * mod, struct pollfd *fd);
static void process_pcap_event(oflops_context *ctx, test_module * mod, struct pollfd *fd, oflops_channel_name ch);


/******************************************************
 * setup the test and call the main loop
 * 	
 */
int run_test_module(oflops_context *ctx, test_module * mod)
{
	int i;
	for(i=0;i<ctx->n_channels;i++)
		setup_channel( ctx, mod, i);

	mod->start(ctx);

	test_module_loop(ctx,mod);
	return 0;
}




/********************************************************
 * main loop()
 * 	1) setup poll
 * 	2) call poll with a min timeout of the next event
 * 	3) dispatch events as appropriate
 */
static void test_module_loop(oflops_context *ctx, test_module *mod)
{
	struct pollfd * poll_set;
	int ret;
	int len; 
	int ch;
	int n_fds=0;

	len = sizeof(struct pollfd) * (ctx->n_channels + 1);
	poll_set = malloc_and_check(len);

	while(!ctx->should_end)
	{
		int next_event;
		n_fds=0;
		bzero(poll_set,len);

		for(ch=0; ch< ctx->n_channels; ch++)
		{
			if( ctx->channels[ch].pcap_handle)
			{
				poll_set[ch].fd = ctx->channels[ch].pcap_fd;
				poll_set[ch].events = POLLIN;
				n_fds++;
			}
		}
		poll_set[n_fds].fd = ctx->control_fd;	// add the control channel at the end
		poll_set[n_fds].events = POLLIN;
		n_fds++;
		
		next_event = timer_get_next_event(ctx);
		while(next_event <= 0 )
		{
			timer_run_next_event(ctx);
			next_event = timer_get_next_event(ctx);
		}
		ret = poll(poll_set, n_fds, next_event);

		if(( ret == -1 ) && ( errno != EINTR))
			perror_and_exit("poll",1);
		else if(ret == 0 )
			timer_run_next_event(ctx);
		else // found something to read
		{
			int i;	
			for(i=0; i<n_fds; i++)
				if(poll_set[i].revents & POLLIN)
					process_event(ctx, mod, &poll_set[i]);
		}
	}
}

/*******************************************************
 * static void process_event(oflops_context *ctx, test_module * mod, struct pollfd *pfd)
 * a channel got an event
 * 	map the event to the correct channel, and call the appropriate event handler
 *
 * 	FIXME: for efficency, we really should have a faster fd-> channel map, but 
 * 		since the number of channels is small, we can just be fugly
 */


static void process_event(oflops_context *ctx, test_module * mod, struct pollfd *pfd)
{
	int ch;
	if(pfd->fd == ctx->control_fd)
		return process_control_event(ctx, mod, pfd);
	// this is inefficient, but ok since there are really typically only ~8  cases
	for(ch=0; ch< ctx->n_channels; ch++)
		if (pfd->fd == ctx->channels[ch].pcap_fd)
			return process_pcap_event(ctx, mod, pfd,ch);
	// only get here if we've screwed up somehow
	fprintf(stderr, "Event on unknown fd %d .. dying", pfd->fd);
	abort();
}

/***********************************************************************************************
 * static void process_control_event(oflops_context *ctx, test_module * mod, struct pollfd *fd);
 * 	if POLLIN is set, read an openflow message from the control channel
 * 	FIXME: handle a control channel reset here
 */
static void process_control_event(oflops_context *ctx, test_module * mod, struct pollfd *pfd)
{
    char * neobuf;
    static char * buf;
    static int buflen   = -1;
    static int bufstart =  0;       // begin of unprocessed data
    static int bufend   =  0;       // end of unprocessed data
	unsigned int msglen;
	struct ofp_header * ofph;
	int count;

    if ( buflen == - 1 )
    {
        buflen = BUFLEN;
        buf = malloc_and_check(BUFLEN);
    }
    if(bufend >= buflen )   // if we've filled up our buffer, resize it
    {
        buflen *=2 ;
        buf = realloc_and_check(buf, buflen);
    }

	assert(pfd->revents & POLLIN);		// FIXME: only know how to handle POLLIN events for now
	count = read(pfd->fd,&buf[bufend], buflen - bufend);
	if(count < 0)
	{
		perror("process_control_event:read() ::");
		return ;
	}
	if(count == 0)
	{
		fprintf(stderr, "Switch Control Connection reset! wtf!?!...exiting\n");
		exit(0);
	}
    bufend += count;            // extend buf by amount read
    count = bufend - bufstart;  // re-purpose count

    while(count > 0 )
    {
        if(count <  sizeof(ofph))   // if we didn't get full openflow header
            return;                 // come back later

        ofph = (struct ofp_header * ) &buf[bufstart];

	//debug info added by Xin
#ifdef OFLOPS_DEBUG
	fprintf(stderr, "process_control_event() in module_run.c. packet hearder:\n");
	fprintf(stderr, "tp=%p, version:%2x\n", &(ofph->version),ofph->version);
	fprintf(stderr, "tp=%p, type:%2x\n", &(ofph->type), ofph->type);
	fprintf(stderr, "length:%d\n", ntohs(ofph->length));
	fprintf(stderr, "xid: %d\n", ofph->xid);
	//end of debug info added by Xin
#endif
        msglen = ntohs(ofph->length);
        if( ( msglen > count) ||    // if we don't yet have the whole msg
                    (buflen < (msglen + bufstart)))  // or our buffer is full
                return;     // get the rest on the next pass

        neobuf = malloc_and_check(msglen);

        memcpy(neobuf, ofph, msglen);
#ifdef OFLOPS_DEBUG
	//debug info added by Xin
	fprintf(stderr, "allocated %d bytes for neobuf\n", msglen);
	fprintf(stderr, "after copying, the value of neobuf is:\n");
	int i=0;
	for(i=0;i<msglen;i++)
	{
		fprintf(stderr, "%x\t", neobuf[i]);
	}
	fprintf(stderr, "\n");
	//end of debug info added by Xin
#endif

	//first check version added by Xin
	if(ofph->version!=OFP_VERSION)
	{
	    fprintf(stderr, "Error! wrong OFP_VERSION! drop this message\n");
	}
	else
	{
		switch(ofph->type)
		{
		    case OFPT_HELLO:
			//need to fill in;
			break;
                    case OFPT_ERROR:
			mod->of_event_error(ctx, (struct ofp_error_msg *)neobuf);
			break;
		    case OFPT_PACKET_IN:
		        mod->of_event_packet_in(ctx, (struct ofp_packet_in *)neobuf);
		        break;
	            case OFPT_STATS_REPLY:
			mod->of_event_stats_reply(ctx, (struct ofp_stats_reply *)neobuf);
			break;
		    case OFPT_FLOW_EXPIRED:
		        #if OFP_VERSION == 0x97
		            mod->of_event_flow_removed(ctx, (struct ofp_flow_expired *)neobuf);
		        #elif OFP_VERSION == 0x98
		            mod->of_event_flow_removed(ctx, (struct ofp_flow_removed *)neobuf);
		        #else
		            #error "Unknown version of openflow"
		        #endif
		        break;
		    case OFPT_PORT_STATUS:
		        mod->of_event_port_status(ctx, (struct ofp_port_status *)neobuf);
		        break;
		    case OFPT_ECHO_REQUEST:
		        mod->of_event_echo_request(ctx, (struct ofp_header *)neobuf);
		        break;
		    default:
		        if(ofph->type > OFPT_STATS_REPLY)   // FIXME: update for new openflow versions
		        {
		            fprintf(stderr, "%s:%zd :: Data buffer probably trashed : unknown openflow type %d\n",
		                    __FILE__,__LINE__, ofph->type);
		            abort();
		        }
		        mod->of_event_other(ctx, (struct ofp_header * ) neobuf);
		        break;
		};
	}//end else()

        free(neobuf);               
        bufstart += msglen;
        count = bufend - bufstart;  // repurpose count
    }       // end while()

    if ( bufstart >= bufend)        // if no outstanding bytes
        bufstart = bufend = 0;      // reset our buffer
}


/**********************************************************************************************
 * static void process_pcap_event(oflops_context *ctx, test_module * mod, struct pollfd *fd, oflops_channel_name ch);
 * 	front end to oflops_pcap_handler
 * 		make sure all of the memory is kosher before and after
 * 		pcap's callback thing has always annoyed me
 */
static void process_pcap_event(oflops_context *ctx, test_module * mod, struct pollfd *fd, oflops_channel_name ch)
{
	struct pcap_event_wrapper wrap;
	int count;

	// read the next packet from the appropriate pcap socket
	assert(ctx->channels[ch].pcap_handle);
	count = pcap_dispatch(ctx->channels[ch].pcap_handle, 1, oflops_pcap_handler, (u_char *) & wrap);
	if (count == 0)
		return;
	if (count < 0)
	{
		fprintf(stderr,"process_pcap_event:pcap_dispatch returned %d :: %s \n", count,
				pcap_geterr(ctx->channels[ch].pcap_handle));
		return;
	}
	// dispatch it to the test module
	mod->handle_pcap_event(ctx, wrap.pe, ch);
	// clean up our mess
	pcap_event_free(wrap.pe);
	return;
}
/*************************************************************************
 * int load_test_module(oflops_context *ctx, 
 * 			char * mod_filename, char * initstr);
 * 	open this module and strip symbols out of it
 * 	and call init() on it
 */
int load_test_module(oflops_context *ctx, char * mod_filename, char * initstr)
{
	void * handle;
	test_module * mod;
	mod = malloc_and_check(sizeof(*mod));
	bzero(mod,sizeof(*mod));

	// open module for dyn symbols
	handle = dlopen(mod_filename,RTLD_NOW);
	if(handle == NULL)
	{	
		fprintf(stderr,"Error reading symbols from %s : %s\n",
				mod_filename, dlerror());
		return 1;
	}
	mod->name = dlsym(handle,"name");
	mod->start = dlsym(handle,"start");
	if(!mod->name)
		fprintf( stderr, "Module %s does not contain a name() function\n", mod_filename);
	if(!mod->start)
		fprintf( stderr, "Module %s does not contain a start() function\n", mod_filename);
	if(!mod->name || !mod->start)
	{
		free(mod);
		dlclose(handle);
		return 1;	// fail for now
	}

#define symbol_fetch(X) \
	mod->X = dlsym(handle, #X);   \
	if(!mod->X) \
		mod->X = default_module_##X
	symbol_fetch(init);
	symbol_fetch(get_pcap_filter);
	symbol_fetch(handle_pcap_event);
	symbol_fetch(of_event_packet_in);
	symbol_fetch(of_event_error);
	symbol_fetch(of_event_stats_reply);
	symbol_fetch(of_event_flow_removed);
	symbol_fetch(of_event_echo_request);
	symbol_fetch(of_event_port_status);
	symbol_fetch(of_event_other);
	symbol_fetch(handle_timer_event);
#undef symbol_fetch
	if(ctx->n_tests >= ctx->max_tests)
	{
		ctx->max_tests *=2;
		ctx->tests = realloc_and_check(ctx->tests, ctx->max_tests * sizeof(struct test_modules *));
	}
	ctx->tests[ctx->n_tests++] = mod;
	mod->symbol_handle=handle;
	return 0;
}
