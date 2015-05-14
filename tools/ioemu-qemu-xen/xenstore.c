/*
 * This file is subject to the terms and conditions of the GNU General
 * Public License.  See the file "COPYING" in the main directory of
 * this archive for more details.
 *
 * Copyright (C) 2006 Christian Limpach
 * Copyright (C) 2006 XenSource Ltd.
 *
 */

#include "qemu-common.h"
#include "qemu-char.h"

#include "block_int.h"
#include <unistd.h>
#ifndef CONFIG_STUBDOM
#include <sys/ipc.h>
#include <sys/shm.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>

#include "exec-all.h"
#include "sysemu.h"

#include "hw.h"
#include "pci.h"
#include "qemu-timer.h"

struct xs_handle *xsh = NULL;
static char *media_filename[MAX_DRIVES+1];
static QEMUTimer *insert_timer = NULL;

#define UWAIT_MAX (30*1000000) /* thirty seconds */
#define UWAIT     (100000)     /* 1/10th second  */

static int pasprintf(char **buf, const char *fmt, ...)
{
    va_list ap;
    int ret = 0;

    if (*buf)
        free(*buf);
    va_start(ap, fmt);
    if (vasprintf(buf, fmt, ap) == -1) {
        buf = NULL;
        ret = -1;
    }
    va_end(ap);
    return ret;
}

static void insert_media(void *opaque)
{
    int i;
    BlockDriverState *bs;

    for (i = 0; i < MAX_DRIVES + 1; i++) {
        bs = drives_table[i].bdrv;
        if (media_filename[i] && bs && bs->filename[0] == '\0') {
            bdrv_open2(bs, media_filename[i], 0, &bdrv_raw);
            pstrcpy(bs->filename, sizeof(bs->filename), media_filename[i]);
            free(media_filename[i]);
            media_filename[i] = NULL;
        }
    }
}

void xenstore_check_new_media_present(int timeout)
{

    if (insert_timer == NULL)
        insert_timer = qemu_new_timer(rt_clock, insert_media, NULL);
    qemu_mod_timer(insert_timer, qemu_get_clock(rt_clock) + timeout);
}

static void waitForDevice(char *fn)
{ 
    struct stat sbuf;
    int status;
    int uwait = UWAIT_MAX;

    do {
        status = stat(fn, &sbuf);
        if (!status) break;
        usleep(UWAIT);
        uwait -= UWAIT;
    } while (uwait > 0);

    return;
}

static int any_hdN;

static int parse_drive_name(const char *dev, DriveInfo *out) {
    /* alway sleaves out->bdrv unchanged */
    /* on success, returns 0 and fills in out->type, ->bus, ->unit */
    /* if drive name not understood, returns -1 and *out may be garbage */
    int ch, max, per_bus;

    /* Change xvdN to look like hdN */
    if (!any_hdN && !strncmp(dev, "xvd", 3) && strlen(dev) == 4) {
        ch = dev[3];
        fprintf(logfile, "Using %s for guest's hd%c\n", dev, ch);
        out->type = IF_IDE;
    } else if (!strncmp(dev, "hd", 2) && strlen(dev) == 3) {
        ch = dev[2];
        out->type = IF_IDE;
    } else if (!strncmp(dev, "sd", 2) && strlen(dev) == 3) {
        ch = dev[2];
        out->type = IF_SCSI;
    } else {
        fprintf(stderr, "qemu: ignoring not-understood drive `%s'\n", dev);
        return -1;
    }

    if (out->type == IF_SCSI) {
        max = MAX_SCSI_DEVS;
        per_bus = max;
    } else {
        max = 4;
        per_bus = 2;
    }

    ch = ch - 'a';
    if (ch >= max) {
        fprintf(stderr, "qemu: drive `%s' out of range\n", dev);
        return -1;
    }

    out->bus = ch / per_bus;
    out->unit = ch % per_bus;
    
    return 0;
}       

static int drive_name_to_index(const char *name) {
    DriveInfo tmp;
    int ret;

    ret = parse_drive_name(name, &tmp);  if (ret) return -1;
    ret = drive_get_index(tmp.type, tmp.bus, tmp.unit);
    return ret;
}

#define DIRECT_PCI_STR_LEN 160
char direct_pci_str[DIRECT_PCI_STR_LEN];
void xenstore_parse_domain_config(int hvm_domid)
{
    char **e = NULL;
    char *buf = NULL, *path;
    char *fpath = NULL, *bpath = NULL,
        *dev = NULL, *params = NULL, *type = NULL, *drv = NULL;
    int i, any_hdN = 0, ret;
    unsigned int len, num, hd_index, pci_devid = 0;
    BlockDriverState *bs;
    BlockDriver *format;

    for(i = 0; i < MAX_DRIVES + 1; i++)
        media_filename[i] = NULL;

    xsh = xs_daemon_open();
    if (xsh == NULL) {
        fprintf(logfile, "Could not contact xenstore for domain config\n");
        return;
    }

    path = xs_get_domain_path(xsh, hvm_domid);
    if (path == NULL) {
        fprintf(logfile, "xs_get_domain_path() error\n");
        goto out;
    }

    if (pasprintf(&buf, "%s/device/vbd", path) == -1)
        goto out;

    e = xs_directory(xsh, XBT_NULL, buf, &num);
    if (e == NULL)
        num = 0;

    for (i = 0; i < num; i++) {
        /* read the backend path */
        if (pasprintf(&buf, "%s/device/vbd/%s/backend", path, e[i]) == -1)
            continue;
        free(bpath);
        bpath = xs_read(xsh, XBT_NULL, buf, &len);
        if (bpath == NULL)
            continue;
        /* read the name of the device */
        if (pasprintf(&buf, "%s/dev", bpath) == -1)
            continue;
        free(dev);
        dev = xs_read(xsh, XBT_NULL, buf, &len);
        if (dev == NULL)
            continue;
        if (!strncmp(dev, "hd", 2)) {
            any_hdN = 1;
            break;
        }
    }
        
    for (i = 0; i < num; i++) {
	format = NULL; /* don't know what the format is yet */

        /* read the backend path */
        if (pasprintf(&buf, "%s/device/vbd/%s/backend", path, e[i]) == -1)
            continue;
        free(bpath);
        bpath = xs_read(xsh, XBT_NULL, buf, &len);
        if (bpath == NULL)
            continue;
        /* read the name of the device */
        if (pasprintf(&buf, "%s/dev", bpath) == -1)
            continue;
        free(dev);
        dev = xs_read(xsh, XBT_NULL, buf, &len);
        if (dev == NULL)
            continue;
	if (nb_drives >= MAX_DRIVES) {
	    fprintf(stderr, "qemu: too many drives, skipping `%s'\n", dev);
	    continue;
	}
	ret = parse_drive_name(dev, &drives_table[nb_drives]);
	if (ret)
	    continue;
        /* read the type of the device */
        if (pasprintf(&buf, "%s/device/vbd/%s/device-type", path, e[i]) == -1)
            continue;
        free(type);
        type = xs_read(xsh, XBT_NULL, buf, &len);
        if (pasprintf(&buf, "%s/params", bpath) == -1)
            continue;
        free(params);
        params = xs_read(xsh, XBT_NULL, buf, &len);
        if (params == NULL)
            continue;
        /* read the name of the device */
        if (pasprintf(&buf, "%s/type", bpath) == -1)
            continue;
        free(drv);
        drv = xs_read(xsh, XBT_NULL, buf, &len);
        if (drv == NULL)
            continue;
        /* Obtain blktap sub-type prefix */
        if (!strcmp(drv, "tap") && params[0]) {
            char *offset = strchr(params, ':'); 
            if (!offset)
                continue ;
	    free(drv);
	    drv = malloc(offset - params + 1);
	    memcpy(drv, params, offset - params);
	    drv[offset - params] = '\0';
	    if (!strcmp(drv, "aio"))
		/* qemu does aio anyway if it can */
		format = &bdrv_raw;
            memmove(params, offset+1, strlen(offset+1)+1 );
            fprintf(logfile, "Strip off blktap sub-type prefix to %s (drv '%s')\n", params, drv); 
        }
        /* Prefix with /dev/ if needed */
        if (!strcmp(drv, "phy") && params[0] != '/') {
            char *newparams = malloc(5 + strlen(params) + 1);
            sprintf(newparams, "/dev/%s", params);
            free(params);
            params = newparams;
	    format = &bdrv_raw;
        }

        /* 
         * check if device has a phantom vbd; the phantom is hooked
         * to the frontend device (for ease of cleanup), so lookup 
         * the frontend device, and see if there is a phantom_vbd
         * if there is, we will use resolution as the filename
         */
        if (pasprintf(&buf, "%s/device/vbd/%s/phantom_vbd", path, e[i]) == -1)
            continue;
        free(fpath);
        fpath = xs_read(xsh, XBT_NULL, buf, &len);
        if (fpath) {
            if (pasprintf(&buf, "%s/dev", fpath) == -1)
                continue;
            free(params);
            params = xs_read(xsh, XBT_NULL, buf , &len);
            if (params) {
                /* 
                 * wait for device, on timeout silently fail because we will 
                 * fail to open below
                 */
                waitForDevice(params);
            }
        }

        bs = bdrv_new(dev);
        /* check if it is a cdrom */
        if (type && !strcmp(type, "cdrom")) {
            bdrv_set_type_hint(bs, BDRV_TYPE_CDROM);
            if (pasprintf(&buf, "%s/params", bpath) != -1)
                xs_watch(xsh, buf, dev);
        }

        /* open device now if media present */
#ifdef CONFIG_STUBDOM
        if (pasprintf(&buf, "%s/device/vbd/%s", path, e[i]) == -1)
            continue;
	if (bdrv_open2(bs, buf, 0 /* snapshot */, &bdrv_vbd) == 0) {
	    pstrcpy(bs->filename, sizeof(bs->filename), params);
	    continue;
	}
#endif

        if (params[0]) {
	    if (!format) {
		if (!drv) {
		    fprintf(stderr, "qemu: type (image format) not specified for vbd '%s' or image '%s'\n", buf, params);
		    continue;
		}
		if (!strcmp(drv,"qcow")) {
		    /* autoguess qcow vs qcow2 */
		} else if (!strcmp(drv,"file")) {
		    format = &bdrv_raw;
		} else if (!strcmp(drv,"phy")) {
		    format = &bdrv_raw;
		} else {
		    format = bdrv_find_format(drv);
		    if (!format) {
			fprintf(stderr, "qemu: type (image format) '%s' unknown for vbd '%s' or image '%s'\n", drv, buf, params);
			continue;
		    }
		}
	    }
            pstrcpy(bs->filename, sizeof(bs->filename), params);
            if (bdrv_open2(bs, params, 0 /* snapshot */, format) < 0)
                fprintf(stderr, "qemu: could not open vbd '%s' or hard disk image '%s' (drv '%s' format '%s')\n", buf, params, drv ? drv : "?", format ? format->format_name : "0");
        }

	drives_table[nb_drives].bdrv = bs;
	nb_drives++;

    }

#ifdef CONFIG_STUBDOM
    if (pasprintf(&buf, "%s/device/vkbd", path) == -1)
        goto out;

    free(e);
    e = xs_directory(xsh, XBT_NULL, buf, &num);

    if (e) {
        for (i = 0; i < num; i++) {
            if (pasprintf(&buf, "%s/device/vkbd/%s", path, e[i]) == -1)
                continue;
            xenfb_connect_vkbd(buf);
        }
    }

    if (pasprintf(&buf, "%s/device/vfb", path) == -1)
        goto out;

    free(e);
    e = xs_directory(xsh, XBT_NULL, buf, &num);

    if (e) {
        for (i = 0; i < num; i++) {
            if (pasprintf(&buf, "%s/device/vfb/%s", path, e[i]) == -1)
                continue;
            xenfb_connect_vfb(buf);
        }
    }
#endif


    /* Set a watch for log-dirty requests from the migration tools */
    if (pasprintf(&buf, "/local/domain/0/device-model/%u/logdirty/next-active",
                  domid) != -1) {
        xs_watch(xsh, buf, "logdirty");
        fprintf(logfile, "Watching %s\n", buf);
    }

    /* Set a watch for suspend requests from the migration tools */
    if (pasprintf(&buf, 
                  "/local/domain/0/device-model/%u/command", domid) != -1) {
        xs_watch(xsh, buf, "dm-command");
        fprintf(logfile, "Watching %s\n", buf);
    }

    /* get the pci pass-through parameter */
    if (pasprintf(&buf, "/local/domain/0/backend/pci/%u/%u/num_devs",
                  hvm_domid, pci_devid) == -1)
        goto out;

    free(params);
    params = xs_read(xsh, XBT_NULL, buf, &len);
    if (params == NULL)
        goto out;
    num = atoi(params);

    for ( i = 0; i < num; i++ ) {
        if (pasprintf(&buf, "/local/domain/0/backend/pci/%u/%u/dev-%d",
                    hvm_domid, pci_devid, i) != -1) {
            free(dev);
            dev = xs_read(xsh, XBT_NULL, buf, &len);

            if ( strlen(dev) + strlen(direct_pci_str) > DIRECT_PCI_STR_LEN ) {
                fprintf(stderr, "qemu: too many pci pass-through devices\n");
                memset(direct_pci_str, 0, DIRECT_PCI_STR_LEN);
                goto out;
            }

            /* append to direct_pci_str */
            if ( dev ) {
                strcat(direct_pci_str, dev);
                strcat(direct_pci_str, "-");
            }
        }
    }

 out:
    free(type);
    free(params);
    free(dev);
    free(bpath);
    free(buf);
    free(path);
    free(e);
    free(drv);
    return;
}

int xenstore_fd(void)
{
    if (xsh)
        return xs_fileno(xsh);
    return -1;
}

unsigned long *logdirty_bitmap = NULL;
unsigned long logdirty_bitmap_size;
extern int vga_ram_size, bios_size;

void xenstore_process_logdirty_event(void)
{
    char *act;
    static char *active_path = NULL;
    static char *next_active_path = NULL;
    static char *seg = NULL;
    unsigned int len;
    int i;

    if (!seg) {
        char *path = NULL, *key_ascii, key_terminated[17] = {0,};
        key_t key;
        int shmid;

        /* Find and map the shared memory segment for log-dirty bitmaps */
        if (pasprintf(&path, 
                      "/local/domain/0/device-model/%u/logdirty/key", 
                      domid) == -1) {
            fprintf(logfile, "Log-dirty: out of memory\n");
            exit(1);
        }
        
        key_ascii = xs_read(xsh, XBT_NULL, path, &len);
        free(path);

        if (!key_ascii) 
            /* No key yet: wait for the next watch */
            return;

#ifdef CONFIG_STUBDOM
        /* We pass the writes to hypervisor */
        seg = (void*)1;
#else
        strncpy(key_terminated, key_ascii, 16);
        free(key_ascii);
        key = (key_t) strtoull(key_terminated, NULL, 16);

        /* Figure out how bit the log-dirty bitmaps are */
        logdirty_bitmap_size = xc_memory_op(xc_handle, 
                                            XENMEM_maximum_gpfn, &domid) + 1;
        logdirty_bitmap_size = ((logdirty_bitmap_size + HOST_LONG_BITS - 1)
                                / HOST_LONG_BITS); /* longs */
        logdirty_bitmap_size *= sizeof (unsigned long); /* bytes */

        /* Map the shared-memory segment */
        fprintf(logfile, "%s: key=%16.16llx size=%lu\n", __FUNCTION__,
                (unsigned long long)key, logdirty_bitmap_size);
        shmid = shmget(key, 2 * logdirty_bitmap_size, S_IRUSR|S_IWUSR);
        if (shmid == -1) {
            fprintf(logfile, "Log-dirty: shmget failed: segment %16.16llx "
                    "(%s)\n", (unsigned long long)key, strerror(errno));
            exit(1);
        }

        seg = shmat(shmid, NULL, 0);
        if (seg == (void *)-1) {
            fprintf(logfile, "Log-dirty: shmat failed: segment %16.16llx "
                    "(%s)\n", (unsigned long long)key, strerror(errno));
            exit(1);
        }

        fprintf(logfile, "Log-dirty: mapped segment at %p\n", seg);

        /* Double-check that the bitmaps are the size we expect */
        if (logdirty_bitmap_size != *(uint32_t *)seg) {
            fprintf(logfile, "Log-dirty: got %u, calc %lu\n", 
                    *(uint32_t *)seg, logdirty_bitmap_size);
            /* Stale key: wait for next watch */
            shmdt(seg);
            seg = NULL;
            return;
        }
#endif

        /* Remember the paths for the next-active and active entries */
        if (pasprintf(&active_path, 
                      "/local/domain/0/device-model/%u/logdirty/active",
                      domid) == -1) {
            fprintf(logfile, "Log-dirty: out of memory\n");
            exit(1);
        }
        if (pasprintf(&next_active_path, 
                      "/local/domain/0/device-model/%u/logdirty/next-active",
                      domid) == -1) {
            fprintf(logfile, "Log-dirty: out of memory\n");
            exit(1);
        }
    }

    fprintf(logfile, "Triggered log-dirty buffer switch\n");
    
    /* Read the required active buffer from the store */
    act = xs_read(xsh, XBT_NULL, next_active_path, &len);
    if (!act) {
        fprintf(logfile, "Log-dirty: can't read next-active\n");
        exit(1);
    }

    /* Switch buffers */
    i = act[0] - '0';
    if (i != 0 && i != 1) {
        fprintf(logfile, "Log-dirty: bad next-active entry: %s\n", act);
        exit(1);
    }
    logdirty_bitmap = (unsigned long *)(seg + i * logdirty_bitmap_size);

    /* Ack that we've switched */
    xs_write(xsh, XBT_NULL, active_path, act, len);
    free(act);
}


/* Accept state change commands from the control tools */
static void xenstore_process_dm_command_event(void)
{
    char *path = NULL, *command = NULL, *par = NULL;
    unsigned int len;

    if (pasprintf(&path, 
                  "/local/domain/0/device-model/%u/command", domid) == -1) {
        fprintf(logfile, "out of memory reading dm command\n");
        goto out;
    }
    command = xs_read(xsh, XBT_NULL, path, &len);
    if (!command)
        goto out;
    
    if (!strncmp(command, "save", len)) {
        fprintf(logfile, "dm-command: pause and save state\n");
        xen_pause_requested = 1;
    } else if (!strncmp(command, "continue", len)) {
        fprintf(logfile, "dm-command: continue after state save\n");
        xen_pause_requested = 0;
#ifdef CONFIG_PASSTHROUGH
    } else if (!strncmp(command, "pci-rem", len)) {
        fprintf(logfile, "dm-command: hot remove pass-through pci dev \n");

        if (pasprintf(&path, 
                      "/local/domain/0/device-model/%u/parameter", domid) == -1) {
            fprintf(logfile, "out of memory reading dm command parameter\n");
            goto out;
        }
        par = xs_read(xsh, XBT_NULL, path, &len);
        if (!par)
            goto out;

        do_pci_del(par);
        free(par);
    } else if (!strncmp(command, "pci-ins", len)) {
        fprintf(logfile, "dm-command: hot insert pass-through pci dev \n");

        if (pasprintf(&path, 
                      "/local/domain/0/device-model/%u/parameter", domid) == -1) {
            fprintf(logfile, "out of memory reading dm command parameter\n");
            goto out;
        }
        par = xs_read(xsh, XBT_NULL, path, &len);
        if (!par)
            goto out;

        do_pci_add(par);
        free(par);
#endif
    } else {
        fprintf(logfile, "dm-command: unknown command\"%*s\"\n", len, command);
    }

 out:
    free(path);
    free(command);
}

void xenstore_record_dm(char *subpath, char *state)
{
    char *path = NULL;

    if (pasprintf(&path, 
                  "/local/domain/0/device-model/%u/%s", domid, subpath) == -1) {
        fprintf(logfile, "out of memory recording dm \n");
        goto out;
    }
    if (!xs_write(xsh, XBT_NULL, path, state, strlen(state)))
        fprintf(logfile, "error recording dm \n");

 out:
    free(path);
}

void xenstore_record_dm_state(char *state)
{
    xenstore_record_dm("state", state);
}

void xenstore_process_event(void *opaque)
{
    char **vec, *offset, *bpath = NULL, *buf = NULL, *drv = NULL, *image = NULL;
    unsigned int len, num, hd_index;

    vec = xs_read_watch(xsh, &num);
    if (!vec)
        return;

    if (!strcmp(vec[XS_WATCH_TOKEN], "logdirty")) {
        xenstore_process_logdirty_event();
        goto out;
    }

    if (!strcmp(vec[XS_WATCH_TOKEN], "dm-command")) {
        xenstore_process_dm_command_event();
        goto out;
    }

    if (strncmp(vec[XS_WATCH_TOKEN], "hd", 2) ||
        strlen(vec[XS_WATCH_TOKEN]) != 3)
        goto out;

    hd_index = drive_name_to_index(vec[XS_WATCH_TOKEN]);
    if (hd_index == -1) {
	fprintf(stderr,"medium change watch on `%s' -"
		" unknown device, ignored\n", vec[XS_WATCH_TOKEN]);
	goto out;
    }

    image = xs_read(xsh, XBT_NULL, vec[XS_WATCH_PATH], &len);

    fprintf(stderr,"medium change watch on `%s' (index: %d): %s\n",
	    vec[XS_WATCH_TOKEN], hd_index, image ? image : "<none>");

    if (image == NULL)
        goto out;  /* gone */

    /* Strip off blktap sub-type prefix */
    bpath = strdup(vec[XS_WATCH_PATH]); 
    if (bpath == NULL)
        goto out;
    if ((offset = strrchr(bpath, '/')) != NULL) 
        *offset = '\0';
    if (pasprintf(&buf, "%s/type", bpath) == -1) 
        goto out;
    drv = xs_read(xsh, XBT_NULL, buf, &len);
    if (drv && !strcmp(drv, "tap") && ((offset = strchr(image, ':')) != NULL))
        memmove(image, offset+1, strlen(offset+1)+1);

    if (!strcmp(image, drives_table[hd_index].bdrv->filename))
        goto out;  /* identical */

    drives_table[hd_index].bdrv->filename[0] = '\0';
    bdrv_close(drives_table[hd_index].bdrv);
    if (media_filename[hd_index]) {
        free(media_filename[hd_index]);
        media_filename[hd_index] = NULL;
    }

    if (image[0]) {
        media_filename[hd_index] = strdup(image);
        xenstore_check_new_media_present(5000);
    }

 out:
    free(drv);
    free(buf);
    free(bpath);
    free(image);
    free(vec);
}

void xenstore_write_vncport(int display)
{
    char *buf = NULL, *path;
    char *portstr = NULL;

    if (xsh == NULL)
        return;

    path = xs_get_domain_path(xsh, domid);
    if (path == NULL) {
        fprintf(logfile, "xs_get_domain_path() error\n");
        goto out;
    }

    if (pasprintf(&buf, "%s/console/vnc-port", path) == -1)
        goto out;

    if (pasprintf(&portstr, "%d", display) == -1)
        goto out;

    if (xs_write(xsh, XBT_NULL, buf, portstr, strlen(portstr)) == 0)
        fprintf(logfile, "xs_write() vncport failed\n");

 out:
    free(portstr);
    free(buf);
}

void xenstore_write_vslots(char *vslots)
{
    char *path = NULL;
    int pci_devid = 0;

    if (pasprintf(&path, 
                  "/local/domain/0/backend/pci/%u/%u/vslots", domid, pci_devid) == -1) {
        fprintf(logfile, "out of memory when updating vslots.\n");
        goto out;
    }
    if (!xs_write(xsh, XBT_NULL, path, vslots, strlen(vslots)))
        fprintf(logfile, "error updating vslots \n");

 out:
    free(path);
}

void xenstore_read_vncpasswd(int domid, char *pwbuf, size_t pwbuflen)
{
    char *buf = NULL, *path, *uuid = NULL, *passwd = NULL;
    unsigned int i, len;

    pwbuf[0] = '\0';

    if (xsh == NULL)
        return;

    path = xs_get_domain_path(xsh, domid);
    if (path == NULL) {
        fprintf(logfile, "xs_get_domain_path() error. domid %d.\n", domid);
        return;
    }

    pasprintf(&buf, "%s/vm", path);
    free(path);
    uuid = xs_read(xsh, XBT_NULL, buf, &len);
    if (uuid == NULL) {
        fprintf(logfile, "xs_read(): uuid get error. %s.\n", buf);
        free(buf);
        return;
    }

    pasprintf(&buf, "%s/vncpasswd", uuid);
    free(uuid);
    passwd = xs_read(xsh, XBT_NULL, buf, &len);
    if (passwd == NULL) {
        fprintf(logfile, "xs_read(): vncpasswd get error. %s.\n", buf);
        free(buf);
        return;
    }

    if (len >= pwbuflen)
    {
        fprintf(logfile, "xenstore_read_vncpasswd(): truncated password to avoid buffer overflow\n");
        len = pwbuflen - 1;
    }

    for (i=0; i<len; i++)
        pwbuf[i] = passwd[i];
    pwbuf[len] = '\0';
    passwd[0] = '\0';
    if (xs_write(xsh, XBT_NULL, buf, passwd, 1) == 0)
        fprintf(logfile, "xs_write() vncpasswd failed.\n");

    free(passwd);
    free(buf);
}


/*
 * get all device instances of a certain type
 */
char **xenstore_domain_get_devices(struct xs_handle *handle,
                                   const char *devtype, unsigned int *num)
{
    char *path;
    char *buf = NULL;
    char **e  = NULL;

    path = xs_get_domain_path(handle, domid);
    if (path == NULL)
        goto out;

    if (pasprintf(&buf, "%s/device/%s", path,devtype) == -1)
        goto out;

    e = xs_directory(handle, XBT_NULL, buf, num);

 out:
    free(path);
    free(buf);
    return e;
}

/*
 * Check whether a domain has devices of the given type
 */
int xenstore_domain_has_devtype(struct xs_handle *handle, const char *devtype)
{
    int rc = 0;
    unsigned int num;
    char **e = xenstore_domain_get_devices(handle, devtype, &num);
    if (e)
        rc = 1;
    free(e);
    return rc;
}

/*
 * Function that creates a path to a variable of an instance of a
 * certain device
 */
static char *get_device_variable_path(const char *devtype, const char *inst,
                                      const char *var)
{
    char *buf = NULL;
    if (pasprintf(&buf, "/local/domain/0/backend/%s/%d/%s/%s",
                  devtype,
                  domid,
                  inst,
                  var) == -1) {
        free(buf);
        buf = NULL;
    }
    return buf;
}

char *xenstore_backend_read_variable(struct xs_handle *handle,
                                     const char *devtype, const char *inst,
                                     const char *var)
{
    char *value = NULL;
    char *buf = NULL;
    unsigned int len;

    buf = get_device_variable_path(devtype, inst, var);
    if (NULL == buf)
        goto out;

    value = xs_read(handle, XBT_NULL, buf, &len);

    free(buf);

 out:
    return value;
}

/*
  Read the hotplug status variable from the backend given the type
  of device and its instance.
*/
char *xenstore_read_hotplug_status(struct xs_handle *handle,
                                   const char *devtype, const char *inst)
{
    return xenstore_backend_read_variable(handle, devtype, inst,
                                          "hotplug-status");
}

/*
   Subscribe to the hotplug status of a device given the type of device and
   its instance.
   In case an error occurrs, a negative number is returned.
 */
int xenstore_subscribe_to_hotplug_status(struct xs_handle *handle,
                                         const char *devtype,
                                         const char *inst,
                                         const char *token)
{
    int rc = 0;
    char *path = get_device_variable_path(devtype, inst, "hotplug-status");

    if (path == NULL)
        return -1;

    if (0 == xs_watch(handle, path, token))
        rc = -2;

    free(path);

    return rc;
}

/*
 * Unsubscribe from a subscription to the status of a hotplug variable of
 * a device.
 */
int xenstore_unsubscribe_from_hotplug_status(struct xs_handle *handle,
                                             const char *devtype,
                                             const char *inst,
                                             const char *token)
{
    int rc = 0;
    char *path;
    path = get_device_variable_path(devtype, inst, "hotplug-status");
    if (path == NULL)
        return -1;

    if (0 == xs_unwatch(handle, path, token))
        rc = -2;

    free(path);

    return rc;
}

char *xenstore_vm_read(int domid, char *key, unsigned int *len)
{
    char *buf = NULL, *path = NULL, *value = NULL;

    if (xsh == NULL)
        goto out;

    path = xs_get_domain_path(xsh, domid);
    if (path == NULL) {
        fprintf(logfile, "xs_get_domain_path(%d): error\n", domid);
        goto out;
    }

    pasprintf(&buf, "%s/vm", path);
    free(path);
    path = xs_read(xsh, XBT_NULL, buf, NULL);
    if (path == NULL) {
        fprintf(logfile, "xs_read(%s): read error\n", buf);
        goto out;
    }

    pasprintf(&buf, "%s/%s", path, key);
    value = xs_read(xsh, XBT_NULL, buf, len);
    if (value == NULL) {
        fprintf(logfile, "xs_read(%s): read error\n", buf);
        goto out;
    }

 out:
    free(path);
    free(buf);
    return value;
}

int xenstore_vm_write(int domid, char *key, char *value)
{
    char *buf = NULL, *path = NULL;
    int rc = -1;

    if (xsh == NULL)
        goto out;

    path = xs_get_domain_path(xsh, domid);
    if (path == NULL) {
        fprintf(logfile, "xs_get_domain_path: error\n");
        goto out;
    }

    pasprintf(&buf, "%s/vm", path);
    free(path);
    path = xs_read(xsh, XBT_NULL, buf, NULL);
    if (path == NULL) {
        fprintf(logfile, "xs_read(%s): read error\n", buf);
        goto out;
    }

    pasprintf(&buf, "%s/%s", path, key);
    rc = xs_write(xsh, XBT_NULL, buf, value, strlen(value));
    if (rc == 0) {
        fprintf(logfile, "xs_write(%s, %s): write error\n", buf, key);
        goto out;
    }

 out:
    free(path);
    free(buf);
    return rc;
}


/*
 * Create a store entry for a device (e.g., monitor, serial/parallel lines).
 * The entry is <domain-path><storeString>/tty and the value is the name
 * of the pty associated with the device.
 */
static int store_dev_info(const char *devName, int domid,
                          CharDriverState *cState, const char *storeString)
{
#ifdef CONFIG_STUBDOM
    fprintf(logfile, "can't store dev %s name for domid %d in %s from a stub domain\n", devName, domid, storeString);
    return ENOSYS;
#else
    int xc_handle;
    struct xs_handle *xs;
    char *path;
    char *newpath;
    char *pts;
    char namebuf[128];
    int ret;

    /*
     * Only continue if we're talking to a pty
     */
    if (!cState->chr_getname) return 0;
    ret = cState->chr_getname(cState, namebuf, sizeof(namebuf));
    if (ret < 0) {
        fprintf(logfile, "ptsname failed (for '%s'): %s\n",
                storeString, strerror(errno));
        return 0;
    }
    if (memcmp(namebuf, "pty ", 4)) return 0;
    pts = namebuf + 4;

    /* We now have everything we need to set the xenstore entry. */
    xs = xs_daemon_open();
    if (xs == NULL) {
        fprintf(logfile, "Could not contact XenStore\n");
        return -1;
    }

    xc_handle = xc_interface_open();
    if (xc_handle == -1) {
        fprintf(logfile, "xc_interface_open() error\n");
        return -1;
    }

    path = xs_get_domain_path(xs, domid);
    if (path == NULL) {
        fprintf(logfile, "xs_get_domain_path() error\n");
        return -1;
    }
    newpath = realloc(path, (strlen(path) + strlen(storeString) +
                             strlen("/tty") + 1));
    if (newpath == NULL) {
        free(path); /* realloc errors leave old block */
        fprintf(logfile, "realloc error\n");
        return -1;
    }
    path = newpath;

    strcat(path, storeString);
    strcat(path, "/tty");
    if (!xs_write(xs, XBT_NULL, path, pts, strlen(pts))) {
        fprintf(logfile, "xs_write for '%s' fail", storeString);
        return -1;
    }

    free(path);
    xs_daemon_close(xs);
    close(xc_handle);

    return 0;
#endif
}

void xenstore_store_serial_port_info(int i, CharDriverState *chr,
				     const char *devname) {
    char buf[16];

    snprintf(buf, sizeof(buf), "/serial/%d", i);
    store_dev_info(devname, domid, chr, buf);
    if (i == 0) /* serial 0 is also called the console */
        store_dev_info(devname, domid, chr, "/console");
}
