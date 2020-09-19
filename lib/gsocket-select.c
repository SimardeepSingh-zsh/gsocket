
#include "gs-common.h"
#include <fcntl.h>
#include <gsocket/gsocket.h>
#include <gsocket/gs-select.h>
#include "gsocket-engine.h"
#include "gs-externs.h"


/********* FUNCTIONS ********************/

int
GS_SELECT_CTX_init(GS_SELECT_CTX *ctx, fd_set *rfd, fd_set *wfd, fd_set *r, fd_set *w, struct timeval *tv_now, int frequency)
{

	memset(ctx, 0, sizeof *ctx);
	ctx->rfd = rfd;
	ctx->wfd = wfd;
	ctx->r = r;
	ctx->w = w;

	ctx->tv_now = tv_now;

	gettimeofday(ctx->tv_now, NULL);
	ctx->hb_init = GS_TV_TO_USEC(ctx->tv_now);
	ctx->hb_freq = frequency;

	int i;
	for (i = 0; i < FD_SETSIZE; i++)
	{
		ctx->mgr_r[i].func = NULL;
		ctx->mgr_w[i].func = NULL;
	}

	return 0;
}

void
gs_select_rw_save_state(GS_SELECT_CTX *ctx, int fd, char *idstr)
{
	/* Save rfd/wfd state */
	if (ctx->is_rw_state_saved[fd] == 1)
	{
		// DEBUGF_R("*** WARNING ***: RWFD already saved. SKIPPING (fd = %d, %s)\n", fd, idstr);
		return;		
	}

	DEBUGF_M("Saving state (fd = %d, %s):\n", fd, idstr);
	gs_fds_out_fd(ctx->rfd, 'r', fd);
	gs_fds_out_fd(ctx->wfd, 'w', fd);

	ctx->saved_rw_state[fd] = 0;
	ctx->is_rw_state_saved[fd] = 1;

	if (FD_ISSET(fd, ctx->rfd))
		ctx->saved_rw_state[fd] |= 0x01;
	if (FD_ISSET(fd, ctx->wfd))
		ctx->saved_rw_state[fd] |= 0x02;
}

void
gs_select_rw_restore_state(GS_SELECT_CTX *ctx, int fd, char *idstr)
{
	if (ctx->is_rw_state_saved[fd] == 0)
	{
		// DEBUGF("RWFD was not saved. Nothing to restore (fd = %d, %s).\n", fd, idstr);
		return;
	}

	// DEBUGF_B("Restoring RWFD state (fd = %d, %s, %d)\n", fd, idstr, ctx->is_rw_state_saved[fd]);
	/* This can happen when FD was half-closed (shutdown received):
	 * - We stopped reading (rfd not set)
	 * - Write() triggered would-block (wfd set) and then restored to 0.
	 */
	if (ctx->saved_rw_state[fd] == 0)
		DEBUGF_Y("*** NOTE ***: Restoring empty RW state (fd = %d, %s)\n", fd, idstr);
	ctx->is_rw_state_saved[fd] = 0;

	FD_CLR(fd, ctx->rfd);
	FD_CLR(fd, ctx->wfd);
	if (ctx->saved_rw_state[fd] & 0x01)
		FD_SET(fd, ctx->rfd);
	if (ctx->saved_rw_state[fd] & 0x02)
		FD_SET(fd, ctx->wfd);
	ctx->saved_rw_state[fd] = 0;

	if (fd > 0)
	{
		DEBUGF_M("Restored state:\n");
		gs_fds_out_fd(ctx->rfd, 'r', fd);
		gs_fds_out_fd(ctx->wfd, 'w', fd);
	}
}

void
gs_select_set_rdata_pending(GS_SELECT_CTX *ctx, int fd)
{
	ctx->rdata_pending_count++;
	ctx->rdata_pending[fd] = 1;
}

static void
call_item(GS_SELECT_CTX *ctx, struct _gs_sel_item *item, int fd)
{
	int wants = 0;
	int ret;

	ret = (*item->func)(ctx, fd, item->cb_arg, item->cb_val);
	// DEBUGF("cb-func ret = %d (fd %d)\n", ret, fd);
	/* 1. Think carefully: STDIN (fd=0) may have succesfully
	 * 1024 bytes but GS_write (fd=3) failed (WANT-WRITE).
	 * - Do not set fd=0 to WANT-WRITE. Instead the GS_write()
	 *   should set that flag on itself (fd=3).
	 *
	 * 2. Think carefully: Not all GS_read/GS_write functions
	 * are called by callbacks: Reading from STDIN calls
	 * GS_write() regardless if GS_write() would block or not.
	 */ 
	wants = ctx->want_io_write[fd];
	wants += ctx->want_io_read[fd];
	if (wants == 0)
	{
		// DEBUGF_B("ctx->want_io_write[fd=%d] == %d\n", fd, ctx->want_io_write[fd]);
		// DEBUGF_Y("SSL has finished. App data...\n");
		/* HERE: SSL has finished */
		if (ret == GS_ECALLAGAIN)
		{
			#if 0
			/* GS_read()/GS_write() has not finished */
			DEBUGF_R("CB wants to be called again...\n");
			if (ctx->blocking_func[fd] & GS_CALLWRITE)
			{
				FD_SET(fd, ctx->wfd);
				//gs_ssl_want_io_rw(ctx, fd, SSL_ERROR_WANT_WRITE);
			} else if (ctx->blocking_func[fd] & GS_CALLREAD) {
				FD_SET(fd, ctx->rfd);
				// gs_ssl_want_io_rw(ctx, fd, SSL_ERROR_WANT_READ);
			} else {
				DEBUGF_R("blocking_func[%d] not set. Not a Read/Write call?\n", fd);
			}
			#endif
		}
	}
}

/*
 * Return 0 on timeout.
 * Return -1 on fatal error. Errno is set.
 */
int
GS_select(GS_SELECT_CTX *ctx)
{
	int n;
	struct timeval tv;
	// int ret;
	int i;


	while (1)
	{
		int max_fd = ctx->max_fd;

		/* Before calling select() to check if there is new data on I/O:
		 * - Check if there is already data in the user-land (such as from
		 *   SSL_pending() before checking I/O [kernel].
		 */
		for (i = 0; i <= max_fd; i++)
		{
			if (ctx->rdata_pending_count <= 0)
				break;
			/* Continue if there is no pending data in the input read buffer */
			if (ctx->rdata_pending[i] == 0)
				continue;
			/* Continue if the app does not want us to submit read data */
			if (!FD_ISSET(i, ctx->rfd))
				continue;
			/* HERE: Call to GS_read() needed because there is still
			 * data in the input buffer (not i/o buffer).
			 */
			// DEBUGF_Y("Pending data in read input buffer :>\n");
			ctx->rdata_pending_count--;
			call_item(ctx, &ctx->mgr_r[i], i);
		}
		/* Do it again if there are still items that
		 * have data in their input buffer...
		 */
		if (ctx->rdata_pending_count > 0)
			continue;

		memcpy(ctx->r, ctx->rfd, sizeof *ctx->r);
		memcpy(ctx->w, ctx->wfd, sizeof *ctx->w);
		
		/* Calculate timeout */
		if (ctx->hb_freq > 0)
		{
			gettimeofday(ctx->tv_now, NULL);
			uint64_t now = GS_TV_TO_USEC(ctx->tv_now);
			uint64_t skip = (now - ctx->hb_init) / ctx->hb_freq;
			uint64_t next = ctx->hb_init + skip * ctx->hb_freq + ctx->hb_freq;
			ctx->hb_next = next;
			uint64_t delta = next - now;

			tv.tv_sec = delta / 1000000;
			tv.tv_usec = delta % 1000000;
		} else {
			tv.tv_sec = 10;
			tv.tv_usec = 0;
		}
		// gs_fds_out(ctx->r, 'R');
		// gs_fds_out(ctx->w, 'W');

		n = select(max_fd + 1, ctx->r, ctx->w, NULL, &tv);
		// DEBUGF("max-fd = %d, *************** select = %d\n", max_fd, n);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			return -1;
		}

		gettimeofday(ctx->tv_now, NULL);

		// gs_fds_out(ctx->r, 'r');
		// gs_fds_out(ctx->w, 'w');
		// int wants = 0;
		for (i = 0; i <= max_fd; i++)
		{
			/* 'n' is not reliable as a listening gsocket might handle more than 1 fd
			 * if more than 1 are readable. Only 1 listen-callback will be called
			 * but the callback may serve more than 1 fd.
			 */
			if (n <= 0)
				break;
			struct _gs_sel_item *item = NULL;
			char c;
			/* Must check r and rfd in case app deselected rfd to stop reading */
			if (FD_ISSET(i, ctx->r) && FD_ISSET(i, ctx->rfd))
			{
				// DEBUGF_B("I/O == READ (fd = %d)\n", i);
				/* GS_CALLREAD or GS_CALLDEFAULT */
				/* Find out if read-i/o was required because GS_write() set
				 * WANT_READ.
				 */
				item = &ctx->mgr_r[i];
				c = 'r';
				if (ctx->want_io_read[i])
				{
					if (ctx->blocking_func[i] & GS_CALLWRITE)
					{
						item = &ctx->mgr_w[i];
						c = 'W';
					} 
				}

				DEBUGF_B("CTX-R: %c fd=%d\n", c, i);
				XASSERT(item->func != NULL, "%c fd = %d has no function to call\n", c, i);
				call_item(ctx, item, i);
				n--;
			}

			if (FD_ISSET(i, ctx->w) && FD_ISSET(i, ctx->wfd))
			{
				// DEBUGF_B("I/O == WRITE (fd = %d)\n", i);
				item = &ctx->mgr_w[i];
				c = 'w';
				if (ctx->want_io_write[i])
				{
					if (ctx->blocking_func[i] & GS_CALLREAD)
					{
						item = &ctx->mgr_r[i];
						c = 'R';
					}
				}

				DEBUGF_B("CTX-W: %c fd=%d\n", c, i);
				XASSERT(item->func != NULL, "%c fd = %d has no function to call\n", c, i);
				call_item(ctx, item, i);
				n--;
			} /* FD_ISSET(i, ctx->w) */
		} /* select() */

		/* Time to return control to caller? */
		if (((ctx->hb_freq > 0) && GS_TV_TO_USEC(ctx->tv_now) > ctx->hb_next))
		{
			return 0;
		}
	} /* while (1) */

	ERREXIT("NOT REACHED\n");
	return -1;
}

void
GS_SELECT_del_cb(GS_SELECT_CTX *ctx, int fd)
{
	int new_max_fd = 0;

	DEBUGF_B("Removing CB for fd = %d\n", fd);
	ctx->mgr_r[fd].func = NULL;
	ctx->mgr_w[fd].func = NULL;
	ctx->mgr_r[fd].cb_arg = NULL;
	ctx->mgr_w[fd].cb_arg = NULL;
	ctx->mgr_w[fd].cb_val = 0;
	ctx->mgr_r[fd].cb_val = 0;
	FD_CLR(fd, ctx->rfd);
	FD_CLR(fd, ctx->wfd);
	/* Calcualte new max-fd */
	int i;
	for (i = 0; i <= ctx->max_fd; i++)
	{
		if ((ctx->mgr_r[i].func == NULL) && (ctx->mgr_w[i].func == NULL))
			continue;
		new_max_fd = i;
	}
	DEBUGF("Setting MAX-FD to %d\n", new_max_fd);
	ctx->max_fd = new_max_fd;
}

void
GS_SELECT_add_cb_r(GS_SELECT_CTX *ctx, gselect_cb_t func, int fd, void *arg, int val)
{
	ctx->mgr_r[fd].func = (void *)func;
	ctx->mgr_r[fd].cb_arg = arg;
	ctx->mgr_r[fd].cb_val = val;
	ctx->max_fd = MAX(ctx->max_fd, fd);
}

void
GS_SELECT_add_cb_w(GS_SELECT_CTX *ctx, gselect_cb_t func, int fd, void *arg, int val)
{
	ctx->mgr_w[fd].func = (void *)func;
	ctx->mgr_w[fd].cb_arg = arg;
	ctx->mgr_w[fd].cb_val = val;
	ctx->max_fd = MAX(ctx->max_fd, fd);
}

void
GS_SELECT_add_cb(GS_SELECT_CTX *ctx, gselect_cb_t func_r, gselect_cb_t func_w, int fd, void *arg, int val)
{
	GS_SELECT_add_cb_r(ctx, func_r, fd, arg, val);
	GS_SELECT_add_cb_w(ctx, func_w, fd, arg, val);
}



