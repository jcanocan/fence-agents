/*
  Copyright Red Hat, Inc. 2006

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation; either version 2, or (at your option) any
  later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; see the file COPYING.  If not, write to the
  Free Software Foundation, Inc.,  675 Mass Ave, Cambridge, 
  MA 02139, USA.
*/
/*
 * Author: Lon Hohberger <lhh at redhat.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <virterror.h>
#include <nss.h>
#include <libgen.h>
//#include <uuid/uuid.h>
#include <server_plugin.h>

/* Local includes */
#include "xvm.h"
#include "simple_auth.h"
#include "options.h"
#include "mcast.h"
#include "tcp.h"
#include "virt.h"
#include "libcman.h"
#include "debug.h"


#define NAME "libvirt"
#define VERSION "0.1"

static inline int
wait_domain(const char *vm_name, virConnectPtr vp, int timeout)
{
	int tries = 0;
	int response = 1;
	virDomainPtr vdp;
	virDomainInfo vdi;

	vdp = virDomainLookupByName(vp, vm_name);
	if (!vdp)
		return 0;

	/* Check domain liveliness.  If the domain is still here,
	   we return failure, and the client must then retry */
	/* XXX On the xen 3.0.4 API, we will be able to guarantee
	   synchronous virDomainDestroy, so this check will not
	   be necessary */
	do {
		sleep(1);
		vdp = virDomainLookupByName(vp, vm_name);
		if (!vdp) {
			dbg_printf(2, "Domain no longer exists\n");
			response = 0;
			break;
		}

		memset(&vdi, 0, sizeof(vdi));
		virDomainGetInfo(vdp, &vdi);
		virDomainFree(vdp);

		if (vdi.state == VIR_DOMAIN_SHUTOFF) {
			dbg_printf(2, "Domain has been shut off\n");
			response = 0;
			break;
		}
		
		dbg_printf(4, "Domain still exists (state %d) after %d seconds\n",
			vdi.state, tries);

		if (++tries >= timeout)
			break;
	} while (1);

	return response;
}


static int
libvirt_null(const char *vm_name, void *priv)
{
	dbg_printf(5, "%s %s\n", __FUNCTION__, vm_name);
	printf("NULL operation: returning failure\n");
	return 1;
}


static int
libvirt_off(const char *vm_name, void *priv)
{
	virConnectPtr vp = (virConnectPtr)priv;
	virDomainPtr vdp;
	virDomainInfo vdi;
	int ret = -1;

	dbg_printf(5, "%s %s\n", __FUNCTION__, vm_name);

	vdp = virDomainLookupByName(vp, vm_name);

	if (!vdp ||
	    ((virDomainGetInfo(vdp, &vdi) == 0) &&
	     (vdi.state == VIR_DOMAIN_SHUTOFF))) {
		dbg_printf(2, "[NOCLUSTER] Nothing to "
			   "do - domain does not exist\n");

		if (vdp)
			virDomainFree(vdp);
		return 0;
	}

	dbg_printf(2, "[OFF] Calling virDomainDestroy\n");
	ret = virDomainDestroy(vdp);
	if (ret < 0) {
		printf("virDomainDestroy() failed: %d\n", ret);
		return 1;
	}

	if (ret) {
		printf("Domain still exists; fencing failed\n");
		return 1;
	}

	return 0;
}


static int
libvirt_on(const char *vm_name, void *priv)
{
	dbg_printf(5, "%s %s\n", __FUNCTION__, vm_name);

	return -ENOSYS;
}


static int
libvirt_devstatus(void *priv)
{
	dbg_printf(5, "%s ---\n", __FUNCTION__);

	if (priv)
		return 0;
	return 1;
}


static int
libvirt_status(const char *vm_name, void *priv)
{
	virConnectPtr vp = (virConnectPtr)priv;
	virDomainPtr vdp;
	virDomainInfo vdi;
	int ret = 0;

	dbg_printf(5, "%s %s\n", __FUNCTION__, vm_name);

	vdp = virDomainLookupByName(vp, vm_name);

	if (!vdp || ((virDomainGetInfo(vdp, &vdi) == 0) &&
	     (vdi.state == VIR_DOMAIN_SHUTOFF))) {
		ret = 1;
	}

	if (vdp)
		virDomainFree(vdp);
	return ret;
}


static int
libvirt_reboot(const char *vm_name, void *priv)
{
	virConnectPtr vp = (virConnectPtr)priv;
	virDomainPtr vdp, nvdp;
	virDomainInfo vdi;
	char *domain_desc;
	int ret;

	//uuid_unparse(vm_uuid, uu_string);
	dbg_printf(5, "%s %s\n", __FUNCTION__, vm_name);

	vdp = virDomainLookupByName(vp, vm_name);

	if (!vdp || ((virDomainGetInfo(vdp, &vdi) == 0) &&
	     (vdi.state == VIR_DOMAIN_SHUTOFF))) {
		dbg_printf(2, "[libvirt:REBOOT] Nothing to "
			   "do - domain does not exist\n");
		if (vdp)
			virDomainFree(vdp);
		return 0;
	}

	printf("Rebooting domain %s...\n", vm_name);
	domain_desc = virDomainGetXMLDesc(vdp, 0);

	if (!domain_desc) {
		printf("Failed getting domain description from "
		       "libvirt\n");
	}

	dbg_printf(2, "[REBOOT] Calling virDomainDestroy(%p)\n", vdp);
	ret = virDomainDestroy(vdp);
	if (ret < 0) {
		printf("virDomainDestroy() failed: %d/%d\n", ret, errno);
		free(domain_desc);
		virDomainFree(vdp);
		return 1;
	}

	ret = wait_domain(vm_name, vp, 15);

	if (ret) {
		printf("Domain still exists; fencing failed\n");
		if (domain_desc)
			free(domain_desc);
		return 1;
	}
		
	if (!domain_desc)
		return 0;

	/* 'on' is not a failure */
	ret = 0;

	dbg_printf(3, "[[ XML Domain Info ]]\n");
	dbg_printf(3, "%s\n[[ XML END ]]\n", domain_desc);
	dbg_printf(2, "Calling virDomainCreateLinux()...\n");

	nvdp = virDomainCreateLinux(vp, domain_desc, 0);
	if (nvdp == NULL) {
		/* More recent versions of libvirt or perhaps the
		 * KVM back-end do not let you create a domain from
		 * XML if there is already a defined domain description
		 * with the same name that it knows about.  You must
		 * then call virDomainCreate() */
		dbg_printf(2, "Failed; Trying virDomainCreate()...\n");
		if (virDomainCreate(vdp) < 0) {
			dbg_printf(1, "Failed to recreate guest"
				   " %s!\n", vm_name);
		}
	}

	free(domain_desc);

	return ret;
}

static int
libvirt_init(srv_context_t *c)
{
	virConnectPtr vp;

	vp = virConnectOpen(NULL);
	if (!vp)
		return -1;
	*c = (void *)vp;
	return 0;
}


static int
libvirt_shutdown(srv_context_t c)
{
	return virConnectClose((virConnectPtr)c);
}


static fence_callbacks_t libvirt_callbacks = {
	.null = libvirt_null,
	.off = libvirt_off,
	.on = libvirt_on,
	.reboot = libvirt_reboot,
	.status = libvirt_status,
	.devstatus = libvirt_devstatus
};

#ifdef _MODULE
fence_callbacks_t *
plugin_callbacks(void)
{
	return &libvirt_callbacks;
}
#else

static plugin_t libvirt_plugin = {
	.name = NAME,
	.version = VERSION,
	.callbacks = &libvirt_callbacks,
	.init = libvirt_init,
	.cleanup = libvirt_shutdown,
};

static void __attribute__((constructor))
initialize_plugin(void)
{
	plugin_register(&libvirt_plugin);
}
#endif
