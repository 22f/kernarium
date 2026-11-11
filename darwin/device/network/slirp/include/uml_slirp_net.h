/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_SLIRP_NET_H
#define UML_SLIRP_NET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The SLiRP user-mode network provider, as a launcher hands it to the
 * emulator's network operations. A context comes from uml_slirp_net_create
 * and is released by uml_slirp_net_destroy, after uml_slirp_net_stop. The
 * other calls return a negative value on failure. */
void *uml_slirp_net_create(void);
void uml_slirp_net_destroy(void *context);
int uml_slirp_net_add_forward(void *context, const char *specification);
int uml_slirp_net_open(void *context, unsigned char mac[6], unsigned int *mtu);
int uml_slirp_net_irq_fd(void *context);
int uml_slirp_net_read(void *context, void *buffer, size_t capacity);
int uml_slirp_net_write(void *context, const void *buffer, size_t length);
void uml_slirp_net_stop(void *context);

/* What the provider has counted since uml_slirp_net_create, numbers only:
 * never an address, a port or a name. The fields are the NetworkCounts of the
 * provider (network_counters.hpp), which says what each one counts. A reader
 * that wants one guest session's share subtracts the reading it took when the
 * session began. */
typedef struct uml_slirp_net_counters {
    uint64_t dns_answered;
    uint64_t dns_policy_denied;
    uint64_t dns_timed_out;
    uint64_t dns_failed;
    uint64_t dhcp_discovers;
    uint64_t dhcp_requests;
    uint64_t dhcp_acks;
    uint64_t tcp_attempts;
    uint64_t tcp_established;
    uint64_t tcp_unreachable;
    uint64_t tcp_reset;
} uml_slirp_net_counters;

/* Fills `counters`; returns 0, or -EINVAL for a null argument. Any thread; it
 * takes no lock and allocates nothing. */
int uml_slirp_net_read_counters(void *context, uml_slirp_net_counters *counters);

#ifdef __cplusplus
}
#endif

#endif
