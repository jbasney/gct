#include  <globus_common.h>
#include <setjmp.h>
#include "config.h"
#include "proto.h"
#include "../support/ftp.h"
#include <syslog.h>

#if defined(USE_GLOBUS_DATA_CODE)
extern globus_ftp_control_layout_t		g_layout;
extern globus_ftp_control_parallelism_t		g_parallelism;
extern globus_bool_t				g_send_restart_info;
extern int mode;

typedef struct
{
    globus_size_t		offset;
    globus_size_t		length;
}
globus_l_wu_range_t;

typedef struct globus_i_wu_montor_s
{
    globus_mutex_t             mutex;
    globus_cond_t              cond;
    globus_bool_t              done;

    globus_bool_t              timed_out;
    globus_bool_t              abort;
    int                        count;
    int                        fd;

    int                        offset;

    /* Range response messages */
    time_t		       last_range_update;
    globus_fifo_t	       ranges;

    /* Performance update messages */
    struct timeval	       last_perf_update;
    globus_size_t	       all_transferred;
    globus_size_t	       accum_bytes;
    globus_callback_handle_t   callback_handle;
    globus_ftp_control_handle_t *
			       handle;
} globus_i_wu_montor_t;

/*
 *  global varials from ftpd.c
 */
extern int logged_in;
extern int transflag;
extern int retrieve_is_data;
extern int type;
extern unsigned int timeout_data;
extern unsigned int timeout_connect;
extern unsigned int timeout_accept;
extern int data_count_total;
extern int data_count_in;
extern int data_count_out;
extern int byte_count_total;
extern int byte_count_in;
extern int byte_count_out;
extern off_t file_size;
extern off_t byte_count;
extern int file_count_total;
extern int file_count_in;
extern int file_count_out;
extern int xfer_count_total;
extern int xfer_count_in;
extern int xfer_count_out;
extern struct sockaddr_in ctrl_addr;
extern struct sockaddr_in his_addr;
/**********************************************************n
 * local function prototypes
 ************************************************************/
void
g_force_close(
    int                                         cb_count);

void
connect_callback(
    void *                                      callback_arg,
    struct globus_ftp_control_handle_s *        handle,
    unsigned int                                stripe_ndx,
    globus_object_t *                           error);

void
data_read_callback(
    void *                                      callback_arg,
    globus_ftp_control_handle_t *               handle,
    globus_object_t *                           error,
    globus_byte_t *                             buffer,
    globus_size_t                               length,
    globus_size_t                               offset,
    globus_bool_t                               eof);

void
data_write_callback(
    void *                                      callback_arg,
    globus_ftp_control_handle_t *               handle,
    globus_object_t *                           error,
    globus_byte_t *                             buffer,
    globus_size_t                               length,
    globus_size_t                               offset,
    globus_bool_t                               eof);

void 
data_close_callback(
    void *                                      callback_arg,
    struct globus_ftp_control_handle_s *        handle,
    globus_object_t *                           error);

static void
globus_l_wu_insert_range(globus_fifo_t * ranges,
			 globus_size_t offset,
			 globus_size_t length);
static char *
globus_l_wu_create_range_string(globus_fifo_t * ranges);

static
globus_bool_t
globus_l_wu_perf_update(
    globus_abstime_t *			time_stop,
    void *				user_args);
/*************************************************************
 *   global vairables 
 ************************************************************/
static globus_bool_t                            g_timeout_occured;
globus_ftp_control_handle_t                     g_data_handle;

static globus_i_wu_montor_t                     g_monitor;


void
wu_monitor_reset(
    globus_i_wu_montor_t *                      mon)
{
    mon->done = GLOBUS_FALSE;
    mon->timed_out = GLOBUS_FALSE;
    mon->abort = GLOBUS_FALSE;
    mon->count = 0;
    mon->offset = -1;
    mon->fd = -1;
    gettimeofday(&mon->last_perf_update, GLOBUS_NULL);
    mon->last_range_update = mon->last_perf_update.tv_sec;
    mon->accum_bytes = 0;
    mon->callback_handle = 0;
    globus_fifo_init(&mon->ranges);
}

void
wu_monitor_init(
    globus_i_wu_montor_t *                      mon)
{
    globus_mutex_init(&mon->mutex, GLOBUS_NULL);
    globus_cond_init(&mon->cond, GLOBUS_NULL);

    wu_monitor_reset(mon);
}

void
wu_monitor_destroy(
    globus_i_wu_montor_t *                      mon)
{
    globus_mutex_destroy(&mon->mutex);
    globus_cond_destroy(&mon->cond);

    globus_fifo_destroy(&mon->ranges);
}

static int
g_timeout_wakeup(
    globus_abstime_t *                           time_stop,
    void *                                       user_args)
{
    return GLOBUS_TRUE;
}

void
g_start()
{
    char *                            a;
    globus_ftp_control_host_port_t    host_port;
    int			              rc;
    globus_reltime_t                  delay_time;
    globus_reltime_t                  period_time;
    globus_result_t		      res;

    rc = globus_module_activate(GLOBUS_FTP_CONTROL_MODULE);
    assert(rc == GLOBUS_SUCCESS);

    wu_monitor_init(&g_monitor);
    g_monitor.handle = &g_data_handle;

    a = (char *)&his_addr;
    host_port.host[0] = (int)a[0];
    host_port.host[1] = (int)a[1];
    host_port.host[2] = (int)a[2];
    host_port.host[3] = (int)a[3];
    host_port.port = 21;

    globus_ftp_control_handle_init(&g_data_handle);
    res = globus_ftp_control_local_port(
              &g_data_handle,
              &host_port);
    assert(res == GLOBUS_SUCCESS);

    g_parallelism.mode = GLOBUS_FTP_CONTROL_PARALLELISM_NONE;
    g_parallelism.base.size = 1;
    g_layout.mode = GLOBUS_FTP_CONTROL_STRIPING_NONE;

    globus_ftp_control_local_parallelism(
              &g_data_handle,
              &g_parallelism);

    GlobusTimeReltimeSet(delay_time, 0, 0);
    GlobusTimeReltimeSet(period_time, 0, timeout_connect / 2);

    globus_callback_register_periodic(
        GLOBUS_NULL,
        &delay_time,
        &period_time,
        g_timeout_wakeup,
        GLOBUS_NULL,
        GLOBUS_NULL,
        GLOBUS_NULL);
}

void
g_end()
{
    globus_i_wu_montor_t                            monitor;
    globus_result_t                                 res;
    int i = 0;

    while(i)
    {
        usleep(1);
    }

    wu_monitor_init(&monitor);
    /*
     *  force close the data connection
     */
    monitor.done = GLOBUS_FALSE;
    res = globus_ftp_control_data_force_close(
              &g_data_handle,
              data_close_callback,
              (void*)&monitor);
    if(res == GLOBUS_SUCCESS)
    {
        globus_mutex_lock(&monitor.mutex);
        {   
            while(!monitor.done)
            {
                globus_cond_wait(&monitor.cond, &monitor.mutex);
            }
        }
        globus_mutex_unlock(&monitor.mutex);
    }

    wu_monitor_destroy(&monitor);

    globus_ftp_control_handle_destroy(&g_data_handle);
    globus_module_deactivate(GLOBUS_FTP_CONTROL_MODULE);
}

void
g_abort()
{
    globus_mutex_lock(&g_monitor.mutex);
    {   
        g_monitor.abort = GLOBUS_TRUE;
        globus_cond_signal(&g_monitor.cond);
    }
    globus_mutex_unlock(&g_monitor.mutex);
}

void
g_passive(globus_bool_t spas)
{
    globus_result_t                             res;
    globus_ftp_control_host_port_t              host_port;
    int                                         hi;
    int                                         low;
    unsigned char *                             a;

    if (!logged_in)   
    {
        reply(530, "Login with USER first.");
        return;
    }

    host_port.port = 0;
    res = globus_ftp_control_local_pasv(
              &g_data_handle,
              &host_port);
    if(res != GLOBUS_SUCCESS)
    {
        perror_reply(425, 
                 globus_object_printable_to_string(globus_error_get(res)));

        return;
    }

    a = (unsigned char *)&ctrl_addr.sin_addr;
    host_port.host[0] = (int) a[0];
    host_port.host[1] = (int) a[1];
    host_port.host[2] = (int) a[2];
    host_port.host[3] = (int) a[3];

    hi = host_port.port / 256;
    low = host_port.port % 256;

    if(spas)
    {
	lreply(229, "Entering Striped Passive Mode");
	lreply(0, " %d,%d,%d,%d,%d,%d",
	       host_port.host[0],
	       host_port.host[1],
	       host_port.host[2],
	       host_port.host[3],
	       hi,
	       low);
	reply(229, "End");

    }
    else
    {
	reply(227, "Entering Passive Mode (%d,%d,%d,%d,%d,%d)", 
	      host_port.host[0],
	      host_port.host[1],
	      host_port.host[2],
	      host_port.host[3],
	      hi,
	      low);
    }
}


/*
 *  what to do if it times out
 *
 *  send error message on control connection
 */
static SIGNAL_TYPE 
g_alarm_signal(
    int                                             sig)
{
    globus_mutex_lock(&g_monitor.mutex);
    {
        g_monitor.timed_out = GLOBUS_TRUE;
        g_monitor.done = GLOBUS_TRUE;
        globus_cond_signal(&g_monitor.cond);
    }
    globus_mutex_unlock(&g_monitor.mutex);
}

int
g_connect_write(
    FILE *                                          instr,
    globus_ftp_control_handle_t *                   handle)
{
    globus_result_t                                 res;
    globus_bool_t                                   l_timed_out = GLOBUS_FALSE;

    wu_monitor_reset(&g_monitor);

    (void) signal(SIGALRM, g_alarm_signal);
    alarm(timeout_connect);

    if(mode == MODE_E)
    {
	if(g_layout.mode == GLOBUS_FTP_CONTROL_STRIPING_PARTITIONED)
	{
	    if(!retrieve_is_data)
	    {
		struct stat s;

		fstat(fileno(instr), &s);
		
		g_layout.partitioned.size = s.st_size;

		globus_ftp_control_local_layout(handle, &g_layout, 0);
	    }
	}
	else
	{
	    globus_ftp_control_local_layout(handle, &g_layout, 0);
	}
	globus_ftp_control_local_parallelism(handle,
					     &g_parallelism);
    }
    res = globus_ftp_control_data_connect_write(
              handle,
              connect_callback,
              (void *)&g_monitor);
    if(res != GLOBUS_SUCCESS)
    {
        goto data_err;
    }

    globus_mutex_lock(&g_monitor.mutex);
    {
        while(!g_monitor.done)
        {
            globus_cond_wait(&g_monitor.cond, &g_monitor.mutex);
        }
        l_timed_out = g_monitor.timed_out;
    }
    globus_mutex_unlock(&g_monitor.mutex);

    if(l_timed_out)
    {
        return -1;
    }

    return 0;

  /* 
   *  DATA_ERR
   */
  data_err:
    alarm(0);
    transflag = 0;

    retrieve_is_data = 1;

    return -1;
}

/*
 *  globusified send data routine
 */
int
#ifdef THROUGHPUT
g_send_data(
    char *                                          name,
    FILE *                                          instr,
    globus_ftp_control_handle_t *                   handle,
    int                                             offset,
    off_t                                           blksize,
    off_t                                           length)
#else
g_send_data(
    FILE *                                          instr,
    globus_ftp_control_handle_t *                   handle,
    int                                             offset,
    off_t                                           blksize,
    off_t                                           length)
#endif
{
    int                                             jb_count;
    int                                             jb_i;
    register int                                    c;
    register int                                    cnt = 0;
    globus_byte_t *                                 buf = GLOBUS_NULL;
    int                                             filefd;
    globus_bool_t                                   eof = GLOBUS_FALSE;
    globus_bool_t                                   aborted;
    int                                             cb_count = 0;
    globus_result_t                                 res;
    int                                             buffer_ndx;
    int                                             file_ndx;
    int                                             connection_count = 4;
    globus_bool_t                                   l_timed_out = GLOBUS_FALSE;
#ifdef THROUGHPUT
    int                                             bps;
    double                                          bpsmult;
    time_t                                          t1;
    time_t                                          t2;
#endif

#ifdef THROUGHPUT
    throughput_calc(name, &bps, &bpsmult);
#endif

    wu_monitor_reset(&g_monitor);

    if(offset == -1)
    {
        offset = 0;
    }

    transflag++;
    switch (type) 
    {

    case TYPE_A:
    case TYPE_I:
    case TYPE_L:

#       ifdef THROUGHPUT
        {
            if (bps != -1)
            {
                blksize = bps;
            }
        }
#       endif

        filefd = fileno(instr);

        /*
         *  set timeout
         */
        (void) signal(SIGALRM, g_alarm_signal);
        alarm(timeout_data);

#       ifdef THROUGHPUT
        {
            if (bps != -1)
            {
                t1 = time(NULL);
            }
       }
#      endif

        jb_count = 0;
        while ((jb_count < length || length == -1) && !eof)
        {
            /*
             *  allocate a buffer for each send
             */
            if ((buf = (char *) globus_malloc(blksize)) == NULL)  
            {
                transflag = 0;
                perror_reply(451, "Local resource failure: malloc");
                retrieve_is_data = 1;
                return (0);
            }

            if(length == -1 || length - jb_count >= blksize)
            {
                jb_i = blksize;
            }
            else
            {
                jb_i = length - jb_count;
            }

            cnt = read(filefd, buf, jb_i);

            /*
             *  if file eof, or we have read the portion we want  to 
             *  send in a partial file transfer set eof to true
             */
            if(cnt <= 0 || 
               (jb_count + cnt == length && length != -1))
            {
                eof = GLOBUS_TRUE;
            }

            res = globus_ftp_control_data_write(
                      handle,
                      buf,
                      cnt,
                      jb_count + offset,
                      eof,
                      data_write_callback,
                      &g_monitor);
            if(res != GLOBUS_SUCCESS)
            {
                goto data_err;
            }
            cb_count++;

            jb_count += cnt;

            res = globus_ftp_control_data_query_channels(
                      handle,
                      &connection_count,
                      0);
            assert(res == GLOBUS_SUCCESS);
            globus_mutex_lock(&g_monitor.mutex);
            {   
                g_monitor.count++;
                while(g_monitor.count == connection_count && 
                      !g_monitor.abort &&
                      !g_monitor.timed_out)
                {
                    globus_cond_wait(&g_monitor.cond, &g_monitor.mutex);
                }
            }
            globus_mutex_unlock(&g_monitor.mutex);

            byte_count += cnt;
#           ifdef TRANSFER_COUNT
            {
                if (retrieve_is_data)
                {
#                   ifdef RATIO
                    {
                        if(freefile)
                        {
                            total_free_dl += cnt;
                        }
                    }
#                   endif /* RATIO */

                    data_count_total += cnt;
                    data_count_out += cnt;
                }
                byte_count_total += cnt;
                byte_count_out += cnt;

            }    
#           endif

#           ifdef THROUGHPUT
            {
                if (bps != -1)
                {
                    t2 = time(NULL);
                    if (t2 == t1)
                    {
                        sleep(1);
                    }
                    t1 = time(NULL);
                }
            }
#           endif
        } /* end while */
        globus_mutex_lock(&g_monitor.mutex);
        {   
            while(g_monitor.count > 0)
            {
                globus_cond_wait(&g_monitor.cond, &g_monitor.mutex);
            }
        }
        globus_mutex_unlock(&g_monitor.mutex);

#       ifdef THROUGHPUT
        {
            if (bps != -1)
            {
                throughput_adjust(name);
            }
        }
#       endif


        /*
         *  wait until the eof callback is received
         */
        globus_mutex_lock(&g_monitor.mutex);
        {   
            while(g_monitor.count > 0 && 
                  !g_monitor.abort &&
                  !g_monitor.timed_out)
            {
                globus_cond_wait(&g_monitor.cond, &g_monitor.mutex);
            }
            l_timed_out = g_monitor.timed_out;
            aborted = g_monitor.abort;
        }
        globus_mutex_unlock(&g_monitor.mutex);

        transflag = 0;
        if(aborted)
        {
            alarm(0);
            transflag = 0;
            retrieve_is_data = 1;
    
            g_force_close(cb_count);

            return 0;
        }

        if(l_timed_out)
        {
            goto data_err;
        }

        /* 
         *  unset alarm 
         */
        alarm(0);
        reply(226, "Transfer complete.");

#       ifdef TRANSFER_COUNT
        {
            if (retrieve_is_data) 
            {
                file_count_total++;
                file_count_out++;
            }
            xfer_count_total++;
            xfer_count_out++;
        }
#       endif
 
        retrieve_is_data = 1;

        /*
         *  EXIT POINT 
         */
        goto clean_exit;


    default:
        transflag = 0;
        reply(550, "Unimplemented TYPE %d in send_data", type);
        retrieve_is_data = 1;

        goto clean_exit;
    }

  /* 
   *  DATA_ERR
   */
  data_err:
    alarm(0);
    transflag = 0;

    g_force_close(cb_count);

    if(buf != GLOBUS_NULL)
    {
        globus_free(buf);
    }

    perror_reply(426, "Data connection");
    retrieve_is_data = 1;

    return (0);

  /*
   *  FILE_ERR
   */
  file_err:
    alarm(0);
    transflag = 0;
    perror_reply(551, "Error on input file");
    retrieve_is_data = 1;

    return (0);

  clean_exit:

    return (1);
}

/*
 *  force close the data connection
 *  wait for all of the callbacks to return and for the 
 *  close callback
 */
void
g_force_close(
    int                                     cb_count)
{
    globus_result_t                         res;

    g_monitor.done = GLOBUS_FALSE;
    res = globus_ftp_control_data_force_close(
              &g_data_handle,
              data_close_callback,
              (void*)&g_monitor);
    if(res == GLOBUS_SUCCESS)
    {
        /*
         *  wait for close all of the calbacks and for the
         *  close callback.
         */
        globus_mutex_lock(&g_monitor.mutex);
        {   
            while(!g_monitor.done || g_monitor.count < cb_count)
            {
                globus_cond_wait(&g_monitor.cond, &g_monitor.mutex);
            }
        }
        globus_mutex_unlock(&g_monitor.mutex);
    }
}

void 
data_close_callback(
    void *                                      callback_arg,
    struct globus_ftp_control_handle_s *        handle,
    globus_object_t *                           error)
{
    globus_i_wu_montor_t *                      monitor;

    monitor = (globus_i_wu_montor_t *)callback_arg;

    globus_mutex_lock(&monitor->mutex);
    {
        monitor->done = GLOBUS_TRUE;
        globus_cond_signal(&monitor->cond);
    }
    globus_mutex_unlock(&monitor->mutex);
}


void
data_write_callback(
    void *                                      callback_arg,
    globus_ftp_control_handle_t *               handle,
    globus_object_t *                           error,
    globus_byte_t *                             buffer,
    globus_size_t                               length,
    globus_size_t                               offset,
    globus_bool_t                               eof)
{
    globus_i_wu_montor_t *                         monitor;

    monitor = (globus_i_wu_montor_t *)callback_arg;

    globus_mutex_lock(&monitor->mutex);
    {
        monitor->count--;
        globus_cond_signal(&monitor->cond);
    }
    globus_mutex_unlock(&monitor->mutex);

    globus_free(buffer);
}

/*
 *  globus hacked receive data
 *  --------------------------
 */
int 
g_receive_data(
    globus_ftp_control_handle_t *            handle,
    FILE *                                   outstr,
    int                                      offset)
{
    register int                             c;
    int                                      cnt = 0;
    int                                      bare_lfs = 0;
    globus_byte_t *                          buf;
    int                                      netfd;
    int                                      filefd;
    globus_bool_t                            eof = GLOBUS_FALSE;
    globus_bool_t                            l_timed_out;
    globus_bool_t                            l_error;
    globus_bool_t                            aborted;
    globus_result_t                          res;
    int                                      ctr;
    int                                      cb_count = 0;
    int                                      data_connection_count = 1;
    globus_ftp_control_parallelism_t         parallel;
    globus_reltime_t			     five_seconds;
#ifdef BUFFER_SIZE
    size_t                                   buffer_size = BUFFER_SIZE;
#else
    size_t                                   buffer_size = BUFSIZ;
#endif

    int i = 1;

    wu_monitor_reset(&g_monitor);
    g_monitor.offset = offset;

    (void) signal(SIGALRM, g_alarm_signal);
    alarm(timeout_accept);

    res = globus_ftp_control_data_connect_read(
              handle,
              connect_callback,
              (void *)&g_monitor);
    if(res != GLOBUS_SUCCESS)
    {
        goto data_err;
    }

    globus_mutex_lock(&g_monitor.mutex);
    {
        while(!g_monitor.done)
        {
            globus_cond_wait(&g_monitor.cond, &g_monitor.mutex);
        }
        l_timed_out = g_monitor.timed_out;
    }
    globus_mutex_unlock(&g_monitor.mutex);

    if(l_timed_out)
    {
        goto data_err;
    }
        
    transflag++;
    switch (type) 
    {

    case TYPE_I:
    case TYPE_L:
    case TYPE_A:

    /* the globus code should handle ascii mode */

        filefd = fileno(outstr);

        (void) signal(SIGALRM, g_alarm_signal);
        alarm(timeout_data);

        g_monitor.count = 0;
        g_monitor.done = GLOBUS_FALSE;
        g_monitor.fd = filefd;
        cb_count = 0;
        res = globus_ftp_control_get_parallelism(
                  handle,
                  &parallel);
        if(res == GLOBUS_SUCCESS)
        {
            data_connection_count = parallel.base.size;
        }
        else
        {
            data_connection_count = 2;
        }


	GlobusTimeReltimeSet(five_seconds, 5, 0);

	globus_callback_register_periodic(
	    &g_monitor.callback_handle,
	    &five_seconds,
	    &five_seconds,
	    globus_l_wu_perf_update,
	    &g_monitor,
	    GLOBUS_NULL,
	    GLOBUS_NULL);
					  
        for(ctr = 0; ctr < data_connection_count; ctr++)
        {
            if ((buf = (globus_byte_t *) globus_malloc(buffer_size)) == NULL)
            {
                transflag = 0;
		globus_callback_unregister(g_monitor.callback_handle);
                perror_reply(451, "Local resource failure: malloc");
                return (-1);
            }
            res = globus_ftp_control_data_read(
                      handle,
                      buf,
                      buffer_size,
                      data_read_callback,
                      (void *)&g_monitor);
            if(res != GLOBUS_SUCCESS)
            {
                globus_free(buf);
                goto data_err;
            }
            cb_count++;
        }

        globus_mutex_lock(&g_monitor.mutex);
        {
            while(!g_monitor.done && 
                  g_monitor.count < cb_count &&
                  !g_monitor.abort &&
                  !g_monitor.timed_out)
            {
                globus_cond_wait(&g_monitor.cond, &g_monitor.mutex);
            }
            l_timed_out = g_monitor.timed_out;
            l_error = g_monitor.done;
            aborted = g_monitor.abort;
        }
        globus_mutex_unlock(&g_monitor.mutex);

        transflag = 0;
        if(aborted)
        {
            alarm(0);
            g_force_close(cb_count);

	    globus_callback_unregister(g_monitor.callback_handle);
            return -1;
        }

        if(l_timed_out || l_error)
        {
            goto data_err;
        }

        alarm(0);

#       ifdef TRANSFER_COUNT
        {
            file_count_total++;
            file_count_in++;
            xfer_count_total++;
            xfer_count_in++;
        }
#       endif

        goto clean_exit;

    case TYPE_E:
        reply(553, "TYPE E not implemented.");
        transflag = 0;
        return (-1);

    default:
        reply(550, "Unimplemented TYPE %d in receive_data", type);
        transflag = 0;
        return (-1);
    }

  data_err:

    g_force_close(cb_count);

    globus_callback_unregister(g_monitor.callback_handle);

    alarm(0);
    transflag = 0;
    perror_reply(426, "Data Connection");
    return (-1);

  file_err:
    globus_callback_unregister(g_monitor.callback_handle);

    alarm(0);
    transflag = 0;
    perror_reply(452, "Error writing file");
    return (-1);

  clean_exit:
    globus_callback_unregister(g_monitor.callback_handle);

    return (0);
}

void
connect_callback(
    void *                                      callback_arg,
    struct globus_ftp_control_handle_s *        handle,
    unsigned int                                stripe_ndx,
    globus_object_t *                           error)
{
    globus_i_wu_montor_t *                      monitor;

    monitor = (globus_i_wu_montor_t *)callback_arg;

    globus_mutex_lock(&monitor->mutex);
    {
         reply(150, "Opening %s mode data connection.",
               type == TYPE_A ? "ASCII" : "BINARY");

         monitor->done = GLOBUS_TRUE;
         globus_cond_signal(&monitor->cond);
    }
    globus_mutex_unlock(&monitor->mutex);
}

void
data_read_callback(
    void *                                      callback_arg,
    globus_ftp_control_handle_t *               handle,
    globus_object_t *                           error,
    globus_byte_t *                             buffer,
    globus_size_t                               length,
    globus_size_t                               offset,
    globus_bool_t                               eof)
{
    globus_i_wu_montor_t *                      monitor;
    globus_result_t                             res;
#ifdef BUFFER_SIZE
    size_t                                      buffer_size = BUFFER_SIZE;
#else
    size_t                                      buffer_size = BUFSIZ;
#endif

    monitor = (globus_i_wu_montor_t *)callback_arg;

    globus_mutex_lock(&monitor->mutex);
    {
        if(error != GLOBUS_NULL)
        {
            monitor->count++;
            globus_cond_signal(&monitor->cond);
            globus_mutex_unlock(&monitor->mutex);

            return;
        }

        if(length > 0)
        {
            if(monitor->offset > 0)
            {
                offset = offset + monitor->offset;
            }

            lseek(monitor->fd, offset, SEEK_SET);
            write(monitor->fd, buffer, length);
            byte_count += length;
#           ifdef TRANSFER_COUNT
            {
                data_count_total += length;
                data_count_in += length;
                byte_count_total += length;
                byte_count_in += length;
            }
#           endif
        }

	if(g_send_restart_info)
	{
	    time_t				t;
	    char *				range_str;

	    t = time(NULL);

	    globus_l_wu_insert_range(&monitor->ranges, offset, length);

	    if(t - monitor->last_range_update > 10)
	    {
		range_str =
		    globus_l_wu_create_range_string(&monitor->ranges);
		    
		reply(111, "Range Marker %s", range_str);
		globus_libc_free(range_str);
		monitor->last_range_update = t;
	    }
	}
	monitor->accum_bytes += length;
	monitor->all_transferred += length;

        if(eof)
        {
            monitor->count++;
            globus_cond_signal(&monitor->cond);
            globus_free(buffer);
        }
        else
        {
            res = globus_ftp_control_data_read(
                      handle,
                      buffer,
                      buffer_size,
                      data_read_callback,
                      (void *)monitor);
            if(res != GLOBUS_SUCCESS)
            {
                globus_free(buffer);
                monitor->done = GLOBUS_TRUE;
                globus_cond_signal(&monitor->cond);
            
                globus_mutex_unlock(&monitor->mutex);

                return;
            }
        }

        (void) signal(SIGALRM, g_alarm_signal);
        alarm(timeout_data);
    }
    globus_mutex_unlock(&monitor->mutex);
}

static
void
globus_l_wu_insert_range(
    globus_fifo_t *				ranges,
    globus_size_t				offset,
    globus_size_t				length)
{
    globus_fifo_t				tmp;
    globus_l_wu_range_t *			range;
    globus_l_wu_range_t *			newrange;

    globus_fifo_move(&tmp, ranges);

    while(!globus_fifo_empty(&tmp))
    {
	range = globus_fifo_dequeue(&tmp);
	if(offset <= range->offset)
	{
	    if(offset + length < range->offset)
	    {
		newrange = globus_malloc(sizeof(globus_l_wu_range_t));
		newrange->offset = offset;
		newrange->length = length;

		globus_fifo_enqueue(ranges, newrange);
		globus_fifo_enqueue(ranges, range);
		goto copy_rest;
	    }
	    else if(offset+length == range->offset)
	    {
		length += range->length;
		globus_libc_free(range);
	    }
	    else
	    {
		int newlength;

		/* weird.... overlapping data */
		newlength = range->offset + range->length - offset;
		if(newlength < length)
		{
		    newlength = length;
		}
		length = newlength;
		globus_libc_free(range);
	    }
	}
	else
	{
	    if(range->offset + range->length < offset)
	    {
		globus_fifo_enqueue(ranges, range);
	    }
	    else if(range->offset + range->length == offset)
	    {
		offset = range->offset;
		length += range->length;
		globus_libc_free(range);
	    }
	    else
	    {
		globus_fifo_enqueue(ranges, range);
	    }
	}
    }

    newrange = globus_malloc(sizeof(globus_l_wu_range_t));
    newrange->offset = offset;
    newrange->length = length;
    globus_fifo_enqueue(ranges, newrange);
copy_rest:
    while(! globus_fifo_empty(&tmp))
    {
	globus_fifo_enqueue(ranges, globus_fifo_dequeue(&tmp));
    }
}

static int
globus_l_wu_count_digits(int num)
{
    int digits = 1;

    if(num < 0)
    {
	digits++;
	num = -num;
    }
    while(0 < (num = (num / 10))) digits++;

    return digits;
}

static char *
globus_l_wu_create_range_string(globus_fifo_t * ranges)
{
    int					length = 0, mylen;
    char *				buf = GLOBUS_NULL;
    globus_l_wu_range_t *		range;

    while((! globus_fifo_empty(ranges)) && (length < 4*1024))
    {
	range = globus_fifo_dequeue(ranges);

	mylen = globus_l_wu_count_digits(range->offset);
	mylen++;
	mylen += globus_l_wu_count_digits(range->offset+range->length);
	mylen++;

	buf = realloc(buf, length + mylen + 1);
	sprintf(buf + length,
		"%d-%d,", 
		range->offset, range->offset+range->length);
	globus_libc_free(range);
	length += mylen;
    }
    buf[strlen(buf)-1] = '\0';

    return buf;
}

static
globus_bool_t
globus_l_wu_perf_update(
    globus_abstime_t *			time_stop,
    void *				user_args)
{
    struct timeval tv;
    unsigned int 			num_channels;
    double 				elapsed;
    double 				throughput;
    globus_i_wu_montor_t *		monitor;
    globus_ftp_control_handle_t *	handle;
    time_t				time;
    struct tm *				tm;

    if(!g_send_restart_info)
    {
	return GLOBUS_TRUE;
    }
    monitor = (globus_i_wu_montor_t *) user_args;
    handle = monitor->handle;

    gettimeofday(&tv, NULL);
    time = tv.tv_sec;
    tm = gmtime(&time);
    globus_ftp_control_data_query_channels(handle, &num_channels, 0);
	
    elapsed = (double)(tv.tv_sec - monitor->last_perf_update.tv_sec);
	
    elapsed += (double)((tv.tv_usec - monitor->last_perf_update.tv_usec))
	               / (double) 1000000.0;
    throughput = ((double)monitor->accum_bytes) / elapsed;
	
    lreply(112,
	   "Perf Marker %04d%02d%02d%02d%02d%02d.%06d",
	   tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
	   tm->tm_hour, tm->tm_min, tm->tm_sec,
	   tv.tv_usec);
    lreply(0, " AllTransferred: %ld", (long) monitor->all_transferred);
    lreply(0, " AllConnections: %u", num_channels);
    lreply(0, " AllThroughput: %f", throughput);
    
    lreply(0, " StripeTransferred: 0 %ld", (long) monitor->all_transferred);
    lreply(0, " StripeConnections: 0 %u", num_channels);
    lreply(0, " StripeThroughput: 0 %f", throughput);
    reply(112, "End");
	
    monitor->accum_bytes = 0;
    monitor->last_perf_update = tv;

    return GLOBUS_TRUE;
}
    
#endif /* USE_GLOBUS_DATA_CODE */
