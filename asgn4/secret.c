#include <minix/drivers.h>
#include <minix/driver.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <minix/ds.h>
#include "secret.h"

#define O_RDWR 6
#define O_WRONLY 2

char secret[SECRET_SIZE];
uid_t owner = -1;
struct ucred e;
int wr_counter = 0;

/*
 * Function prototypes for the hello driver.
 */
FORWARD _PROTOTYPE( char * secret_name,   (void) );
FORWARD _PROTOTYPE( int secret_open,      (struct driver *d, message *m) );
FORWARD _PROTOTYPE( int secret_ioctl,      (struct driver *d, message *m) );
FORWARD _PROTOTYPE( int secret_close,     (struct driver *d, message *m) );
FORWARD _PROTOTYPE( struct device * secret_prepare, (int device) );
FORWARD _PROTOTYPE( int secret_transfer,  (int procnr, int opcode,
                                          u64_t position, iovec_t *iov,
                                          unsigned nr_req) );
FORWARD _PROTOTYPE( void secret_geometry, (struct partition *entry) );

/* SEF functions and variables. */
FORWARD _PROTOTYPE( void sef_local_startup, (void) );
FORWARD _PROTOTYPE( int sef_cb_init, (int type, sef_init_info_t *info) );
FORWARD _PROTOTYPE( int sef_cb_lu_state_save, (int) );
FORWARD _PROTOTYPE( int lu_state_restore, (void) );

/* Entry points to the hello driver. */
PRIVATE struct driver secret_tab =
{
    secret_name,
    secret_open,
    secret_close,
    secret_ioctl,
    secret_prepare,
    secret_transfer,
    nop_cleanup,
    secret_geometry,
    nop_alarm,
    nop_cancel,
    nop_select,
    nop_ioctl,
    do_nop,
};

/** Represents the /dev/hello device. */
PRIVATE struct device secret_device;

/** State variable to count the number of times the device has been opened. */
PRIVATE int open_counter;

PRIVATE char * secret_name(void)
{
    printf("secret_name()\n");
    return "hello";
}

PRIVATE int secret_ioctl(d, m)
    struct driver *d;
    message *m;
{
    uid_t grantee;
    
    if (m->REQUEST == SSGRANT) 
    {
       sys_safecopyfrom(m->IO_ENDPT, (vir_bytes)m->IO_GRANT,
	  0, (vir_bytes)&grantee, sizeof(grantee), D);    
       owner = grantee;
    }
    else
    {
       printf("invalid ioctl request\n");
       return ENOTTY;
    }

    return OK;
}

PRIVATE int secret_open(d, m)
    struct driver *d;
    message *m;
{
    getnucred(m->IO_ENDPT, &e);    

    /*printf("secret_open(). Called %d time(s).\n", ++open_counter);*/
    if (m->COUNT == O_RDWR)
    {
       printf("Can't open device with read write access\n");
       return EACCES;
    }
    else if (m->COUNT & W_BIT)
    {
       if (e.uid != owner && owner != -1)
         return EACCES;
       if (strlen(secret) != 0)
       {
          /*printf("cannot create /dev/Secret: No space left on device\n");*/
          return ENOSPC;
       }
    }
    ++open_counter;

    if (owner == -1)
       owner = e.uid;
    return OK;
}

PRIVATE int secret_close(d, m)
    struct driver *d;
    message *m;
{
    /*printf("secret_close()\n");*/
    return OK;
}

PRIVATE struct device * secret_prepare(dev)
    int dev;
{
    secret_device.dv_base.lo = 0;
    secret_device.dv_base.hi = 0;
    secret_device.dv_size.lo = SECRET_SIZE;
    secret_device.dv_size.hi = 0;
    return &secret_device;
}

PRIVATE int secret_transfer(proc_nr, opcode, position, iov, nr_req)
    int proc_nr;
    int opcode;
    u64_t position;
    iovec_t *iov;
    unsigned nr_req;
{
    int bytes, ret;

    /*printf("secret_transfer()\n");*/

    bytes = SECRET_SIZE - position.lo < iov->iov_size ?
            SECRET_SIZE - position.lo : iov->iov_size;

    if (bytes <= 0)
    {
        return OK;
    }
    switch (opcode)
    {
        case DEV_GATHER_S:
            if (owner == -1 || owner == e.uid)
            {
               open_counter--;
               ret = sys_safecopyto(proc_nr, iov->iov_addr, 0,
                                (vir_bytes) (secret + position.lo),
                                 bytes, D);
               iov->iov_size -= bytes;
               if (open_counter == 0)
               {
                  memset(secret, 0, strlen(secret));
                  owner = -1;
               }
            }
            else
            {
               --open_counter; 
               ret = EACCES;
            }
            break;
        case DEV_SCATTER_S:
            --open_counter;
            if (iov->iov_size > SECRET_SIZE)
            {
            	printf("Input too big\n");
            	return ENOSPC;
            }
            ret = sys_safecopyfrom(proc_nr, iov->iov_addr, 0,
                                (vir_bytes) (secret + position.lo),
                                 bytes, D);

            iov->iov_size -= bytes;
            break;
        default:
            return EINVAL;
    }
    return ret;
}

PRIVATE void secret_geometry(entry)
    struct partition *entry;
{
    printf("secret_geometry()\n");
    entry->cylinders = 0;
    entry->heads     = 0;
    entry->sectors   = 0;
}

PRIVATE int sef_cb_lu_state_save(int state) {
/* Save the state. */
    ds_publish_u32("open_counter", open_counter, DSF_OVERWRITE);

    return OK;
}

PRIVATE int lu_state_restore() {
/* Restore the state. */
    u32_t value;

    ds_retrieve_u32("open_counter", &value);
    ds_delete_u32("open_counter");
    open_counter = (int) value;

    return OK;
}

PRIVATE void sef_local_startup()
{
    /*
     * Register init callbacks. Use the same function for all event types
     */
    sef_setcb_init_fresh(sef_cb_init);
    sef_setcb_init_lu(sef_cb_init);
    sef_setcb_init_restart(sef_cb_init);

    /*
     * Register live update callbacks.
     */
    /* - Agree to update immediately when LU is requested in a valid state. */
    sef_setcb_lu_prepare(sef_cb_lu_prepare_always_ready);
    /* - Support live update starting from any standard state. */
    sef_setcb_lu_state_isvalid(sef_cb_lu_state_isvalid_standard);
    /* - Register a custom routine to save the state. */
    sef_setcb_lu_state_save(sef_cb_lu_state_save);

    /* Let SEF perform startup. */
    sef_startup();
}

PRIVATE int sef_cb_init(int type, sef_init_info_t *info)
{
/* Initialize the hello driver. */
    int do_announce_driver = TRUE;

    open_counter = 0;
    switch(type) {
        case SEF_INIT_FRESH:
            printf("Refresed\n");
        break;

        case SEF_INIT_LU:
            /* Restore the state. */
            lu_state_restore();
            do_announce_driver = FALSE;

            printf("Hey, I'm a new version!\n"); 
        break;

        case SEF_INIT_RESTART:
            printf("Hey, I've just been restarted!\n");
        break;
    }

    /* Announce we are up when necessary. */
    if (do_announce_driver) {
        driver_announce();
    }

    /* Initialization completed successfully. */
    return OK;
}

PUBLIC int main(int argc, char **argv)
{
    /*
     * Perform initialization.
     */
    sef_local_startup();

    /*
     * Run the main loop.
     */
    driver_task(&secret_tab, DRIVER_STD);
    return OK;
}
