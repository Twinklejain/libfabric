/*
 * Copyright (c) 2014-2016, Cisco Systems, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <arpa/inet.h>
#include <asm/types.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_errno.h>
#include "fi.h"
#include "fi_enosys.h"
#include "prov.h"

#include "usnic_direct.h"
#include "libnl_utils.h"

#include "usdf.h"
#include "usdf_wait.h"
#include "fi_ext_usnic.h"
#include "usdf_progress.h"
#include "usdf_timer.h"
#include "usdf_dgram.h"
#include "usdf_msg.h"
#include "usdf_rdm.h"

struct usdf_usnic_info *__usdf_devinfo;

static int usdf_fabric_getname(uint32_t version, struct usd_device_attrs *dap,
			       char **name)
{
	int ret = FI_SUCCESS;
	char *bufp = NULL;
	struct in_addr in;
	char *addrnetw;

	if (FI_VERSION_GE(version, FI_VERSION(1, 4))) {
		in.s_addr = dap->uda_ipaddr_be & dap->uda_netmask_be;
		addrnetw = inet_ntoa(in);
		ret = asprintf(&bufp, "%s/%d", addrnetw, dap->uda_prefixlen);
		if (ret < 0) {
			USDF_DBG(
			    "asprintf failed while creating fabric name\n");
			ret = -ENOMEM;
		}
	} else {
		bufp = strdup(dap->uda_devname);
		if (!bufp) {
			USDF_DBG("strdup failed while creating fabric name\n");
			ret = -errno;
		}
	}

	*name = bufp;

	return ret;
}

static bool usdf_fabric_checkname(uint32_t version,
				  struct usd_device_attrs *dap, char *hint)
{
	int ret;
	bool valid = false;
	char *reference;

	USDF_DBG("checking devname: version=%d, devname='%s'\n", version, hint);

	if (version) {
		ret = usdf_fabric_getname(version, dap, &reference);
		if (ret < 0)
			return false;

		if (strcmp(reference, hint) == 0) {
			valid = true;
		} else {
			USDF_DBG("hint %s failed to match %s\n", hint,
				 reference);
		}

		free(reference);
		return valid;
	}

	/* The hint string itself is kind of a version check, in pre-1.4 the
	* name was just the device name. In 1.4 and beyond, then name is
	* actually CIDR
	* notation.
	*/
	if (strstr(hint, "/"))
		return usdf_fabric_checkname(FI_VERSION(1, 4), dap, hint);

	return usdf_fabric_checkname(FI_VERSION(1, 3), dap, hint);
}

static int usdf_validate_hints(uint32_t version, struct fi_info *hints,
			       struct usd_device_attrs *dap)
{
	struct fi_fabric_attr *fattrp;
	struct fi_domain_attr *dattrp;
	size_t size;

	switch (hints->addr_format) {
	case FI_FORMAT_UNSPEC:
	case FI_SOCKADDR_IN:
		size = sizeof(struct sockaddr_in);
		break;
	case FI_SOCKADDR:
		size = sizeof(struct sockaddr);
		break;
	default:
		return -FI_ENODATA;
	}
	if (hints->src_addr != NULL && hints->src_addrlen < size) {
		return -FI_ENODATA;
	}
	if (hints->dest_addr != NULL && hints->dest_addrlen < size) {
		return -FI_ENODATA;
	}

	if (hints->ep_attr != NULL) {
		switch (hints->ep_attr->protocol) {
		case FI_PROTO_UNSPEC:
		case FI_PROTO_UDP:
		case FI_PROTO_RUDP:
			break;
		default:
			return -FI_ENODATA;
		}
	}

	fattrp = hints->fabric_attr;
	if (fattrp != NULL) {
		if (fattrp->prov_version != 0 &&
		    fattrp->prov_version != USDF_PROV_VERSION) {
			return -FI_ENODATA;
		}

		if (fattrp->name != NULL &&
		    !usdf_fabric_checkname(version, dap, fattrp->name)) {
			return -FI_ENODATA;
		}
	}

	dattrp = hints->domain_attr;
	if (dattrp) {
		if (!usdf_domain_checkname(version, dap, dattrp->name))
			return -FI_ENODATA;
	}

	return FI_SUCCESS;
}

static int
usdf_fill_addr_info(struct fi_info *fi, uint32_t addr_format,
		struct sockaddr_in *src, struct sockaddr_in *dest,
		struct usd_device_attrs *dap)
{
	struct sockaddr_in *sin;
	int ret;
#if ENABLE_DEBUG
	char requested[INET_ADDRSTRLEN], actual[INET_ADDRSTRLEN];
#endif

	if (addr_format != FI_FORMAT_UNSPEC) {
		fi->addr_format = addr_format;
	} else {
		fi->addr_format = FI_SOCKADDR_IN;
	}

	switch (fi->addr_format) {
	case FI_SOCKADDR:
	case FI_SOCKADDR_IN:
		if (src != NULL &&
		    src->sin_addr.s_addr != INADDR_ANY &&
		    src->sin_addr.s_addr != dap->uda_ipaddr_be) {
			USDF_DBG("src addr (%s) does not match device addr (%s)\n",
			inet_ntop(AF_INET, &src->sin_addr.s_addr,
				requested, sizeof(requested)),
			inet_ntop(AF_INET, &dap->uda_ipaddr_be,
				actual, sizeof(actual)));

			ret = -FI_ENODATA;
			goto fail;
		}
		sin = calloc(1, sizeof(*sin));
		fi->src_addr = sin;
		if (sin == NULL) {
			ret = -FI_ENOMEM;
			goto fail;
		}
		fi->src_addrlen = sizeof(*sin);
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = dap->uda_ipaddr_be;
		if (src != NULL) {
			sin->sin_port = src->sin_port;
		}

		/* copy in dest if specified */
		if (dest != NULL) {
			sin = calloc(1, sizeof(*sin));
			*sin = *dest;
			fi->dest_addr = sin;
			fi->dest_addrlen = sizeof(*sin);
		}
		break;
	default:
		ret = -FI_ENODATA;
		goto fail;
	}

	return 0;

fail:
	return ret;		// fi_freeinfo() in caller frees all
}

static int usdf_fill_info_dgram(
	uint32_t version,
	struct fi_info *hints,
	struct sockaddr_in *src,
	struct sockaddr_in *dest,
	struct usd_device_attrs *dap,
	struct fi_info **fi_first,
	struct fi_info **fi_last)
{
	struct fi_info *fi;
	struct fi_fabric_attr *fattrp;
	uint32_t addr_format;
	int ret;

	fi = fi_allocinfo();
	if (fi == NULL) {
		ret = -FI_ENOMEM;
		goto fail;
	}

	fi->caps = USDF_DGRAM_CAPS;

	if (hints != NULL) {
		fi->mode = hints->mode & USDF_DGRAM_SUPP_MODE;
		addr_format = hints->addr_format;

		/* check that we are capable of what's requested */
		if ((hints->caps & ~USDF_DGRAM_CAPS) != 0) {
			ret = -FI_ENODATA;
			goto fail;
		}

		/* app must support these modes */
		if ((hints->mode & USDF_DGRAM_REQ_MODE) != USDF_DGRAM_REQ_MODE) {
			ret = -FI_ENODATA;
			goto fail;
		}

		fi->handle = hints->handle;
	} else {
		fi->mode = USDF_DGRAM_SUPP_MODE;
		addr_format = FI_FORMAT_UNSPEC;
	}
	fi->ep_attr->type = FI_EP_DGRAM;

	ret = usdf_fill_addr_info(fi, addr_format, src, dest, dap);
	if (ret != 0) {
		goto fail;
	}

	/* fabric attrs */
	fattrp = fi->fabric_attr;
	ret = usdf_fabric_getname(version, dap, &fattrp->name);
	if (ret < 0 || fattrp->name == NULL) {
		ret = -FI_ENOMEM;
		goto fail;
	}

	if (fi->mode & FI_MSG_PREFIX) {
		if (FI_VERSION_GE(version, FI_VERSION(1, 1)))
			fi->ep_attr->msg_prefix_size = USDF_HDR_BUF_ENTRY;
		else
			fi->mode &= ~FI_MSG_PREFIX;
	}

	ret = usdf_dgram_fill_ep_attr(version, hints, fi, dap);
	if (ret)
		goto fail;

	ret = usdf_dgram_fill_dom_attr(version, hints, fi, dap);
	if (ret)
		goto fail;

	ret = usdf_dgram_fill_tx_attr(hints, fi, dap);
	if (ret)
		goto fail;

	ret = usdf_dgram_fill_rx_attr(hints, fi, dap);
	if (ret)
		goto fail;

	/* add to tail of list */
	if (*fi_first == NULL) {
		*fi_first = fi;
	} else {
		(*fi_last)->next = fi;
	}
	*fi_last = fi;

	return 0;

fail:
	if (fi != NULL) {
		fi_freeinfo(fi);
	}
	return ret;
}

static int usdf_fill_info_msg(
	uint32_t version,
	struct fi_info *hints,
	struct sockaddr_in *src,
	struct sockaddr_in *dest,
	struct usd_device_attrs *dap,
	struct fi_info **fi_first,
	struct fi_info **fi_last)
{
	struct fi_info *fi;
	struct fi_fabric_attr *fattrp;
	uint32_t addr_format;
	int ret;

	fi = fi_allocinfo();
	if (fi == NULL) {
		ret = -FI_ENOMEM;
		goto fail;
	}

	fi->caps = USDF_MSG_CAPS;

	if (hints != NULL) {
		fi->mode = hints->mode & USDF_MSG_SUPP_MODE;
		addr_format = hints->addr_format;

		/* check that we are capable of what's requested */
		if ((hints->caps & ~USDF_MSG_CAPS) != 0) {
			ret = -FI_ENODATA;
			goto fail;
		}

		/* app must support these modes */
		if ((hints->mode & USDF_MSG_REQ_MODE) != USDF_MSG_REQ_MODE) {
			ret = -FI_ENODATA;
			goto fail;
		}

		fi->handle = hints->handle;
	} else {
		fi->mode = USDF_MSG_SUPP_MODE;
		addr_format = FI_FORMAT_UNSPEC;
	}
	fi->ep_attr->type = FI_EP_MSG;


	ret = usdf_fill_addr_info(fi, addr_format, src, dest, dap);
	if (ret != 0) {
		goto fail;
	}

	/* fabric attrs */
	fattrp = fi->fabric_attr;
	ret = usdf_fabric_getname(version, dap, &fattrp->name);
	if (ret < 0 || fattrp->name == NULL) {
		ret = -FI_ENOMEM;
		goto fail;
	}

	ret = usdf_msg_fill_ep_attr(hints, fi, dap);
	if (ret)
		goto fail;

	ret = usdf_msg_fill_dom_attr(version, hints, fi, dap);
	if (ret)
		goto fail;

	ret = usdf_msg_fill_tx_attr(hints, fi);
	if (ret)
		goto fail;

	ret = usdf_msg_fill_rx_attr(hints, fi);
	if (ret)
		goto fail;

	/* add to tail of list */
	if (*fi_first == NULL) {
		*fi_first = fi;
	} else {
		(*fi_last)->next = fi;
	}
	*fi_last = fi;

	return 0;

fail:
	if (fi != NULL) {
		fi_freeinfo(fi);
	}
	return ret;
}

static int usdf_fill_info_rdm(
	uint32_t version,
	struct fi_info *hints,
	struct sockaddr_in *src,
	struct sockaddr_in *dest,
	struct usd_device_attrs *dap,
	struct fi_info **fi_first,
	struct fi_info **fi_last)
{
	struct fi_info *fi;
	struct fi_fabric_attr *fattrp;
	uint32_t addr_format;
	int ret;

	fi = fi_allocinfo();
	if (fi == NULL) {
		ret = -FI_ENOMEM;
		goto fail;
	}

	fi->caps = USDF_RDM_CAPS;

	if (hints != NULL) {
		fi->mode = hints->mode & USDF_RDM_SUPP_MODE;
		addr_format = hints->addr_format;
		/* check that we are capable of what's requested */
		if ((hints->caps & ~USDF_RDM_CAPS) != 0) {
			ret = -FI_ENODATA;
			goto fail;
		}

		/* app must support these modes */
		if ((hints->mode & USDF_RDM_REQ_MODE) != USDF_RDM_REQ_MODE) {
			ret = -FI_ENODATA;
			goto fail;
		}

		fi->handle = hints->handle;
	} else {
		fi->mode = USDF_RDM_SUPP_MODE;
		addr_format = FI_FORMAT_UNSPEC;
	}
	fi->ep_attr->type = FI_EP_RDM;

	ret = usdf_fill_addr_info(fi, addr_format, src, dest, dap);
	if (ret != 0) {
		goto fail;
	}

	/* fabric attrs */
	fattrp = fi->fabric_attr;
	ret = usdf_fabric_getname(version, dap, &fattrp->name);
	if (ret < 0 || fattrp->name == NULL) {
		ret = -FI_ENOMEM;
		goto fail;
	}

	ret = usdf_rdm_fill_ep_attr(hints, fi, dap);
	if (ret)
		goto fail;

	ret = usdf_rdm_fill_dom_attr(version, hints, fi, dap);
	if (ret)
		goto fail;

	ret = usdf_rdm_fill_tx_attr(hints, fi);
	if (ret)
		goto fail;

	ret = usdf_rdm_fill_rx_attr(hints, fi);
	if (ret)
		goto fail;

	/* add to tail of list */
	if (*fi_first == NULL) {
		*fi_first = fi;
	} else {
		(*fi_last)->next = fi;
	}
	*fi_last = fi;

	return 0;

fail:
	if (fi != NULL) {
		fi_freeinfo(fi);
	}
	return ret;
}

static int
usdf_get_devinfo(void)
{
	struct usdf_usnic_info *dp;
	struct usdf_dev_entry *dep;
	struct usd_open_params params;
	int ret;
	int d;

	assert(__usdf_devinfo == NULL);

	dp = calloc(1, sizeof(*dp));
	if (dp == NULL) {
		ret = -FI_ENOMEM;
		goto fail;
	}
	__usdf_devinfo = dp;

	dp->uu_num_devs = USD_MAX_DEVICES;
	ret = usd_get_device_list(dp->uu_devs, &dp->uu_num_devs);
	if (ret != 0) {
		dp->uu_num_devs = 0;
		goto fail;
	}

	for (d = 0; d < dp->uu_num_devs; ++d) {
		dep = &dp->uu_info[d];

		memset(&params, 0, sizeof(params));
		params.flags = UOPF_SKIP_PD_ALLOC;
		params.cmd_fd = -1;
		params.context = NULL;
		ret = usd_open_with_params(dp->uu_devs[d].ude_devname,
						&params, &dep->ue_dev);
		if (ret != 0) {
			continue;
		}

		ret = usd_get_device_attrs(dep->ue_dev, &dep->ue_dattr);
		if (ret != 0) {
			continue;
		}

		dep->ue_dev_ok = 1;	/* this device is OK */

		usd_close(dep->ue_dev);
		dep->ue_dev = NULL;
	}
	return 0;

fail:
	return ret;
}

static int
usdf_get_distance(
    struct usd_device_attrs *dap,
    uint32_t daddr_be,
    int *metric_o)
{
    uint32_t nh_ip_addr;
    int ret;

    USDF_TRACE("\n");

    ret = usnic_nl_rt_lookup(dap->uda_ipaddr_be, daddr_be,
            dap->uda_ifindex, &nh_ip_addr);
    if (ret != 0) {
        *metric_o = -1;
        ret = 0;
    } else if (nh_ip_addr == 0) {
        *metric_o = 0;
    } else {
        *metric_o = 1;
    }

    return ret;
}

static int
usdf_getinfo(uint32_t version, const char *node, const char *service,
	       uint64_t flags, struct fi_info *hints, struct fi_info **info)
{
	struct usdf_usnic_info *dp;
	struct usdf_dev_entry *dep;
	struct usd_device_attrs *dap;
	struct fi_info *fi_first;
	struct fi_info *fi_last;
	struct addrinfo *ai;
	struct sockaddr_in *src;
	struct sockaddr_in *dest;
	enum fi_ep_type ep_type;
	int metric;
	int d;
	int ret;

	USDF_TRACE("\n");

	fi_first = NULL;
	fi_last = NULL;
	ai = NULL;
	src = NULL;
	dest = NULL;

	/*
	 * Get and cache usNIC device info
	 */
	if (__usdf_devinfo == NULL) {
		ret = usdf_get_devinfo();
		if (ret != 0) {
			USDF_WARN("failed to usdf_get_devinfo, ret=%d (%s)\n",
					ret, fi_strerror(-ret));
			if (ret == -FI_ENODEV)
				ret = -FI_ENODATA;
			goto fail;
		}
	}
	dp = __usdf_devinfo;

	if (node != NULL || service != NULL) {
		ret = getaddrinfo(node, service, NULL, &ai);
		if (ret != 0) {
			USDF_DBG("getaddrinfo failed: %d: <%s>\n", ret,
				 gai_strerror(ret));
			goto fail;
		}

		if (flags & FI_SOURCE) {
			src = (struct sockaddr_in *)ai->ai_addr;
		} else {
			dest = (struct sockaddr_in *)ai->ai_addr;
		}
	}
	if (hints != NULL) {
		if (dest == NULL && hints->dest_addr != NULL) {
			dest = hints->dest_addr;
		}
		if (src == NULL && hints->src_addr != NULL) {
			src = hints->src_addr;
		}
	}

	for (d = 0; d < dp->uu_num_devs; ++d) {
		dep = &dp->uu_info[d];
		dap = &dep->ue_dattr;

		/* skip this device if it has some problem */
		if (!dep->ue_dev_ok) {
			USDF_DBG("skipping %s/%s\n", dap->uda_devname,
				dap->uda_ifname);
			continue;
		}

		/* See if dest is reachable from this device */
		if (dest != NULL && dest->sin_addr.s_addr != INADDR_ANY) {
			ret = usdf_get_distance(dap,
					dest->sin_addr.s_addr, &metric);
			if (ret != 0) {
				goto fail;
			}
			if (metric == -1) {
				USDF_DBG("dest %s unreachable from %s/%s, skipping\n",
					inet_ntoa(dest->sin_addr),
					dap->uda_devname, dap->uda_ifname);
				continue;
			}
		}

		/* Does this device match requested attributes? */
		if (hints != NULL) {
			ret = usdf_validate_hints(version, hints, dap);
			if (ret != 0) {
				USDF_DBG("hints do not match for %s/%s, skipping\n",
					dap->uda_devname, dap->uda_ifname);
				continue;
			}

			ep_type = hints->ep_attr ? hints->ep_attr->type :
				  FI_EP_UNSPEC;
		} else {
			ep_type = FI_EP_UNSPEC;
		}

		if (ep_type == FI_EP_DGRAM || ep_type == FI_EP_UNSPEC) {
			ret = usdf_fill_info_dgram(version, hints, src, dest,
					dap, &fi_first, &fi_last);
			if (ret != 0 && ret != -FI_ENODATA) {
				goto fail;
			}
		}

		if (ep_type == FI_EP_MSG || ep_type == FI_EP_UNSPEC) {
			ret = usdf_fill_info_msg(version, hints, src, dest,
					dap, &fi_first, &fi_last);
			if (ret != 0 && ret != -FI_ENODATA) {
				goto fail;
			}
		}

		if (ep_type == FI_EP_RDM || ep_type == FI_EP_UNSPEC) {
			ret = usdf_fill_info_rdm(version, hints, src, dest,
					dap, &fi_first, &fi_last);
			if (ret != 0 && ret != -FI_ENODATA) {
				goto fail;
			}
		}
	}

	if (fi_first != NULL) {
		*info = fi_first;
		ret = 0;
	} else {
		ret = -FI_ENODATA;
	}

fail:
	if (ret != 0) {
		fi_freeinfo(fi_first);
	}
	if (ai != NULL) {
		freeaddrinfo(ai);
	}
	if (ret != 0) {
		USDF_INFO("returning %d (%s)\n", ret, fi_strerror(-ret));
	}
	return ret;
}

static int
usdf_fabric_close(fid_t fid)
{
	struct usdf_fabric *fp;
	int ret;
	void *rv;

	USDF_TRACE("\n");

	fp = fab_fidtou(fid);
	if (atomic_get(&fp->fab_refcnt) > 0) {
		return -FI_EBUSY;
	}
	/* Tell progression thread to exit */
	fp->fab_exit = 1;

	free(fp->fab_attr.name);
	free(fp->fab_attr.prov_name);

	if (fp->fab_thread) {
		ret = usdf_fabric_wake_thread(fp);
		if (ret != 0) {
			return ret;
		}
		pthread_join(fp->fab_thread, &rv);
	}
	usdf_timer_deinit(fp);
	if (fp->fab_epollfd != -1) {
		close(fp->fab_epollfd);
	}
	if (fp->fab_eventfd != -1) {
		close(fp->fab_eventfd);
	}
	if (fp->fab_arp_sockfd != -1) {
		close(fp->fab_arp_sockfd);
	}

	free(fp);
	return 0;
}

static struct fi_ops usdf_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = usdf_fabric_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = usdf_fabric_ops_open,
};

static struct fi_ops_fabric usdf_ops_fabric = {
	.size = sizeof(struct fi_ops_fabric),
	.domain = usdf_domain_open,
	.passive_ep = usdf_pep_open,
	.eq_open = usdf_eq_open,
	.wait_open = usdf_wait_open,
	.trywait = usdf_trywait
};

static int
usdf_fabric_open(struct fi_fabric_attr *fattrp, struct fid_fabric **fabric,
	       void *context)
{
	struct fid_fabric *ff;
	struct usdf_fabric *fp;
	struct usdf_usnic_info *dp;
	struct usdf_dev_entry *dep;
	struct epoll_event ev;
	struct sockaddr_in sin;
	int ret;
	int d;

	USDF_TRACE("\n");

	/* Make sure this fabric exists */
	dp = __usdf_devinfo;
	for (d = 0; d < dp->uu_num_devs; ++d) {
		dep = &dp->uu_info[d];
		if (dep->ue_dev_ok &&
		    usdf_fabric_checkname(0, &(dep->ue_dattr), fattrp->name)) {
			break;
		}
	}
	if (d >= dp->uu_num_devs) {
		USDF_INFO("device \"%s\" does not exit, returning -FI_ENODEV\n",
				fattrp->name);
		return -FI_ENODEV;
	}

	fp = calloc(1, sizeof(*fp));
	if (fp == NULL) {
		USDF_INFO("unable to allocate memory for fabric\n");
		return -FI_ENOMEM;
	}
	fp->fab_epollfd = -1;
	fp->fab_arp_sockfd = -1;
	LIST_INIT(&fp->fab_domain_list);

	fp->fab_attr.fabric = fab_utof(fp);
	fp->fab_attr.name = strdup(fattrp->name);
	fp->fab_attr.prov_name = strdup(USDF_PROV_NAME);
	fp->fab_attr.prov_version = USDF_PROV_VERSION;
	if (fp->fab_attr.name == NULL ||
			fp->fab_attr.prov_name == NULL) {
		ret = -FI_ENOMEM;
		goto fail;
	}

	fp->fab_fid.fid.fclass = FI_CLASS_FABRIC;
	fp->fab_fid.fid.context = context;
	fp->fab_fid.fid.ops = &usdf_fi_ops;
	fp->fab_fid.ops = &usdf_ops_fabric;

	fp->fab_dev_attrs = &dep->ue_dattr;

	fp->fab_epollfd = epoll_create(1024);
	if (fp->fab_epollfd == -1) {
		ret = -errno;
		USDF_INFO("unable to allocate epoll fd\n");
		goto fail;
	}

	fp->fab_eventfd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);
	if (fp->fab_eventfd == -1) {
		ret = -errno;
		USDF_INFO("unable to allocate event fd\n");
		goto fail;
	}
	fp->fab_poll_item.pi_rtn = usdf_fabric_progression_cb;
	fp->fab_poll_item.pi_context = fp;
	ev.events = EPOLLIN;
	ev.data.ptr = &fp->fab_poll_item;
	ret = epoll_ctl(fp->fab_epollfd, EPOLL_CTL_ADD, fp->fab_eventfd, &ev);
	if (ret == -1) {
		ret = -errno;
		USDF_INFO("unable to EPOLL_CTL_ADD\n");
		goto fail;
	}

	/* initialize timer subsystem */
	ret = usdf_timer_init(fp);
	if (ret != 0) {
		USDF_INFO("unable to initialize timer\n");
		goto fail;
	}

	/* create and bind socket for ARP resolution */
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = fp->fab_dev_attrs->uda_ipaddr_be;
	fp->fab_arp_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fp->fab_arp_sockfd == -1) {
		USDF_INFO("unable to create socket\n");
		goto fail;
	}
	ret = bind(fp->fab_arp_sockfd, (struct sockaddr *) &sin, sizeof(sin));
	if (ret == -1) {
		ret = -errno;
		goto fail;
	}

	atomic_initialize(&fp->fab_refcnt, 0);
	atomic_initialize(&fp->num_blocked_waiting, 0);

	ret = pthread_create(&fp->fab_thread, NULL,
			usdf_fabric_progression_thread, fp);
	if (ret != 0) {
		ret = -ret;
		USDF_INFO("unable to create progress thread\n");
		goto fail;
	}

	fattrp->fabric = fab_utof(fp);
	fattrp->prov_version = USDF_PROV_VERSION;
	*fabric = fab_utof(fp);
	USDF_INFO("successfully opened %s/%s\n", fattrp->name,
			fp->fab_dev_attrs->uda_ifname);
	return 0;

fail:
	free(fp->fab_attr.name);
	free(fp->fab_attr.prov_name);
	ff = fab_utof(fp);
	usdf_fabric_close(&ff->fid);
	USDF_DBG("returning %d (%s)\n", ret, fi_strerror(-ret));
	return ret;
}

static void usdf_fini(void)
{
	USDF_TRACE("\n");
}

struct fi_provider usdf_ops = {
	.name = USDF_PROV_NAME,
	.version = USDF_PROV_VERSION,
	.fi_version = FI_VERSION(1, 4),
	.getinfo = usdf_getinfo,
	.fabric = usdf_fabric_open,
	.cleanup =  usdf_fini
};

USNIC_INI
{
#if USNIC_BUILD_FAKE_VERBS_DRIVER
	usdf_setup_fake_ibv_provider();
#endif
	return (&usdf_ops);
}
