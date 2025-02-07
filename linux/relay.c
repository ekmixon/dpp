/*
 * (c) Copyright 2016, 2017, 2018, 2019 Hewlett Packard Enterprise Development LP
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, 
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>              /* DELETE ME WITH SCAN STUFF */
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/if_tun.h>
#include <linux/nl80211.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/pem.h>
#include <openssl/ec.h>
#include <openssl/hmac.h>
#ifdef HASAVAHI
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>
#include <avahi-common/timeval.h>
#endif  /* HASAVAHI */
#include "ieee802_11.h"
#include "service.h"
#include "radio.h"
#include "common.h"
#include "tlv.h"
#include "pkex.h"
#include "dpp.h"

#ifdef HASAVAHI
static AvahiSimplePoll *simple_poll = NULL;
#endif  /* HASAVAHI */

struct interface {
    TAILQ_ENTRY(interface) entry;
    char ifname[IFNAMSIZ];
    unsigned char bssid[ETH_ALEN];
    unsigned char bkhash[SHA256_DIGEST_LENGTH];
    unsigned char is_loopback;
    int nl80211_id;
    struct nl_cb *nl_cb;
    struct nl_sock *nl_sock;
    unsigned long ifindex;
    unsigned long wiphy;
    unsigned long freq;
    unsigned char offchan_tx_ok;
    unsigned long max_roc;
    int fd;     /* BPF socket */
};
TAILQ_HEAD(bar, interface) interfaces;

struct family_data {
    const char *group;
    int id;
};

#define WIRELESS_MTU    1300

struct cstate {
    TAILQ_ENTRY(cstate) entry;
    unsigned char peeraddr[ETH_ALEN];
    unsigned char myaddr[ETH_ALEN];
    unsigned char bkhash[SHA256_DIGEST_LENGTH+1];
    gas_action_resp_frame resp_hdr;
    char *buf;
    int left;
    int sofar;
    int fd;
};
TAILQ_HEAD(foo, cstate) cstates;

service_context srvctx;
static uint32_t port_bitmap[32] = { 0 };
unsigned int opclass = 81, channel = 6;
unsigned short portin, portout;
char controller[80];
unsigned char bkeyhash[SHA256_DIGEST_LENGTH+1];

static void
dump_buffer (unsigned char *buf, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        if (i && (i%4 == 0)) {
            printf(" ");
        }
        if (i && (i%32 == 0)) {
            printf("\n");
        }
        printf("%02x", buf[i]);
    }
    printf("\n");
}

static void
print_buffer (char *str, unsigned char *buf, int len)
{
    printf("%s:\n", str);
    dump_buffer(buf, len);
    printf("\n");
}

static int
ack_handler (struct nl_msg *msg, void *arg)
{
    int *ret = arg;
    
//    printf("ack...%d\n", *ret);
    *ret = 0;
    return NL_STOP;
}


static int
error_handler (struct sockaddr_nl *nla, struct nlmsgerr *err, void *arg)
{
    int *ret = arg;
    printf("error...%d: %s\n", err->error, nl_geterror(err->error));
    *ret = err->error;
    return NL_SKIP; 
}

static int
finish_handler (struct nl_msg *msg, void *arg)
{
    int *ret = arg;

    *ret = 0;
    return NL_SKIP;
}

static int
no_seq_check (struct nl_msg *msg, void *arg)
{
    return NL_OK;
}

static int cookie_handler(struct nl_msg *msg, void *arg)
{
    struct nlattr *tb[NL80211_ATTR_MAX + 1];
    struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
    unsigned long long *cookie = arg;

    nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
              genlmsg_attrlen(gnlh, 0), NULL);
    if (tb[NL80211_ATTR_COOKIE])
        *cookie = nla_get_u64(tb[NL80211_ATTR_COOKIE]);
    return NL_SKIP;
}

static void
nlmsg_clear(struct nl_msg *msg)
{
    /*
     * Clear nlmsg data, e.g., to make sure key material is not left in
     * heap memory for unnecessarily long time.
     */
    if (msg) {
        struct nlmsghdr *hdr = nlmsg_hdr(msg);
        void *data = nlmsg_data(hdr);
        /*
         * This would use nlmsg_datalen() or the older nlmsg_len() if
         * only libnl were to maintain a stable API.. Neither will work
         * with all released versions, so just calculate the length
         * here.
         */
        int len = hdr->nlmsg_len - NLMSG_HDRLEN;

        memset(data, 0, len);
    }
}

static int
get_phy_info (struct nl_msg *msg, void *arg)
{
    struct interface *inf = (struct interface *)arg;
    struct nlattr *tb[NL80211_ATTR_MAX + 1];
    struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));

    printf("got phy info!!!\n");
    nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
              genlmsg_attrlen(gnlh, 0), NULL);
    if (tb[NL80211_ATTR_MAC]) {
        memcpy(inf->bssid, nla_data(tb[NL80211_ATTR_MAC]), ETH_ALEN);
        printf("interface MAC address is " MACSTR "\n", MAC2STR(inf->bssid));
    }

    if (tb[NL80211_ATTR_WIPHY]) {
        inf->wiphy = nla_get_u32(tb[NL80211_ATTR_WIPHY]);
        printf("wiphy is %ld\n", inf->wiphy);
    } else {
        printf("wiphy is not present in response!\n");
    }

    return NL_SKIP;
}

static int
send_mgmt_msg (struct nl_msg *msg, struct interface *inf,
               int (*handler)(struct nl_msg *, void *), void *data)
{
    struct nl_cb *cb;
    int err = 0;
    
    if ((cb = nl_cb_clone(inf->nl_cb)) == NULL) {
        fprintf(stderr, "can't clone an nl_cb!\n");
        nlmsg_free(msg);
        return -1;
    }

    if (nl_send_auto_complete(inf->nl_sock, msg) < 0) {
        fprintf(stderr, "can't send an nl_msg!\n");
        nlmsg_free(msg);
        return -1;
    }

    nl_cb_err(cb, NL_CB_CUSTOM, error_handler, &err);
    nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, &err);
    nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, ack_handler, &err);
    nl_cb_set(cb, NL_CB_SEQ_CHECK, NL_CB_CUSTOM, no_seq_check, NULL);
    if (handler) {
        nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, handler, data);
    }

    err = 1;
    while (err > 0) {
        int res;
        if ((res = nl_recvmsgs(inf->nl_sock, cb)) < 0) {
            fprintf(stderr, "nl_recvmsgs failed: %d\n", res);
        }
    }
    if (err < 0) {
        fprintf(stderr, "send_mgmt_msg: error receiving nl_msgs: %d\n", err);
    }
    
    nl_cb_put(cb);
    printf("nl_mgmt_msg sent\n");
    if (!handler && data == (void *) -1) {
        nlmsg_clear(msg);
    }
    nlmsg_free(msg);

    return err;
}

static int
send_nl_msg (struct nl_msg *msg, struct interface *inf,
             int (*handler)(struct nl_msg *, void *), void *data)
{
    struct nl_cb *cb;
    int opt, err = 0;
    
    if ((cb = nl_cb_clone(inf->nl_cb)) == NULL) {
        fprintf(stderr, "can't clone an nl_cb!\n");
        nlmsg_free(msg);
        return -1;
    }

    opt = 1;
    setsockopt(nl_socket_get_fd(inf->nl_sock), SOL_NETLINK,
               NETLINK_EXT_ACK, &opt, sizeof(opt));
    opt = 1;
    setsockopt(nl_socket_get_fd(inf->nl_sock), SOL_NETLINK,
               NETLINK_CAP_ACK, &opt, sizeof(opt));

    if (nl_send_auto_complete(inf->nl_sock, msg) < 0) {
        fprintf(stderr, "can't send an nl_msg!\n");
        nlmsg_free(msg);
        return -1;
    }
    nl_cb_err(cb, NL_CB_CUSTOM, error_handler, &err);
    nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, &err);
    nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, ack_handler, &err);
    nl_cb_set(cb, NL_CB_SEQ_CHECK, NL_CB_CUSTOM, no_seq_check, NULL);
    if (handler) {
        nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, handler, data);
    }

    err = 1;
    while (err > 0) {
        int res;
        if ((res = nl_recvmsgs(inf->nl_sock, cb)) < 0) {
            fprintf(stderr, "nl_recvmsgs failed: %d\n", res);
        }
    }
    if (err < 0) {
        fprintf(stderr, "send_nl_msg: error receiving nl_msgs: %d\n", err);
    }
    
    nl_cb_put(cb);
    if (!handler && data == (void *) -1) {
        nlmsg_clear(msg);
    }
    nlmsg_free(msg);

    return err;
}

struct nl_msg *
get_nl_msg (struct interface *inf, int flags, unsigned char cmd)
{
    struct nl_msg *msg;
    
    if ((msg = nlmsg_alloc()) == NULL) {
        fprintf(stderr, "can't allocate an nl_msg!\n");
        return NULL;
    }
    if (genlmsg_put(msg, 0, 0, inf->nl80211_id, 0, flags, cmd, 0) == NULL) {
        fprintf(stderr, "can't genlmsg_put an nl_msg!\n");
        return NULL;
    }
    nla_put_u32(msg, NL80211_ATTR_IFINDEX, inf->ifindex);
    return msg;
}

/*
 * cons up an action frame and send it out the interface
 */
static int
cons_action_frame (unsigned char field, 
                   unsigned char *mymac, unsigned char *peermac, 
                   char *data, int len)
{
    char buf[2048];
    struct interface *inf = NULL;
    struct ieee80211_mgmt_frame *frame;
    size_t framesize;
    struct nl_msg *msg;
    unsigned long long cookie;
    unsigned char broadcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    TAILQ_FOREACH(inf, &interfaces, entry) {
        if (memcmp(mymac, inf->bssid, ETH_ALEN) == 0) {
            break;
        }
    }
    if (inf == NULL) {
        fprintf(stderr, "can't find " MACSTR " to send mgmt frame!\n",
                MAC2STR(mymac));
        return -1;
    }
    framesize = IEEE802_11_HDR_LEN + sizeof(frame->action) + len;
    memset(buf, 0, sizeof(buf));
    frame = (struct ieee80211_mgmt_frame *)buf;

    /*
     * fill in the action frame header
     */
    frame->frame_control = ieee_order((IEEE802_11_FC_TYPE_MGMT << 2 | IEEE802_11_FC_STYPE_ACTION << 4));
    memcpy(frame->sa, mymac, ETH_ALEN);
    memcpy(frame->da, peermac, ETH_ALEN);
    memcpy(frame->bssid, broadcast, ETH_ALEN);
    frame->action.category = ACTION_PUBLIC;
    frame->action.field = field;
    memcpy(frame->action.variable, data, len);
    printf("sending %ld byte frame (%d)...\n\n", framesize, len);
    if (inf->is_loopback) {
        if (write(inf->fd, buf, framesize) < 0) {
            fprintf(stderr, "unable to write management frame!\n");
            return -1;
        }
    } else {
        if ((msg = get_nl_msg(inf, 0, NL80211_CMD_FRAME)) == NULL) {
            fprintf(stderr, "can't create an nl msg!\n");
            return -1;
        }
        nla_put_u32(msg, NL80211_ATTR_WIPHY_FREQ, inf->freq);
        nla_put_u32(msg, NL80211_ATTR_DURATION, 500);
        if (inf->offchan_tx_ok) {
            nla_put_flag(msg, NL80211_ATTR_OFFCHANNEL_TX_OK);
        }
        nla_put(msg, NL80211_ATTR_FRAME, framesize, buf);
        cookie = 0;
        if (send_nl_msg(msg, inf, cookie_handler, &cookie) < 0) {
            fprintf(stderr, "can't send nl msg!\n");
            return -1;
        }
    }
    return len;
}

static int
cons_next_fragment (struct cstate *cs, int first)
{
    char buffer[WIRELESS_MTU+sizeof(gas_action_comeback_resp_frame)];
    gas_action_resp_frame *resp;
    gas_action_comeback_resp_frame *cb_resp;
    
    if (cs->buf == NULL) {
        fprintf(stderr, "trying to send next fragment of NULL!\n");
        return 0;
    }
    /*
     * for first fragment, just send the header...
     */
    if (first) {
        memcpy(buffer, (char *)&cs->resp_hdr, sizeof(gas_action_resp_frame));
        resp = (gas_action_resp_frame *)&buffer[0];
        resp->comeback_delay = 1;
        resp->query_resplen = 0;
        cons_action_frame(GAS_INITIAL_RESPONSE, cs->myaddr, cs->peeraddr,
                          buffer, sizeof(gas_action_resp_frame));
        return cs->left;
    }
    /*
     * ...subsequent fragments get data (copy the header from the first message)
     */
    cb_resp = (gas_action_comeback_resp_frame *)&buffer[0];
    cb_resp->dialog_token = cs->resp_hdr.dialog_token;
    cb_resp->status_code = cs->resp_hdr.status_code;
    cb_resp->comeback_delay = 0;
    cb_resp->fragment_id = cs->left/WIRELESS_MTU;
    memcpy(cb_resp->ad_proto_elem, cs->resp_hdr.ad_proto_elem, 3);
    memcpy(cb_resp->ad_proto_id, cs->resp_hdr.ad_proto_id, 7);
    if (cs->left > WIRELESS_MTU) {
        printf("sending next fragment of %d to " MACSTR ", %d so far and %d left\n",
               WIRELESS_MTU, MAC2STR(cs->peeraddr), cs->sofar, cs->left);
        cb_resp->query_resplen = WIRELESS_MTU;
        cb_resp->fragment_id |= 0x80;  // more fragme
        memcpy(cb_resp->query_resp, cs->buf+cs->sofar, WIRELESS_MTU);
        cons_action_frame(GAS_COMEBACK_RESPONSE, cs->myaddr, cs->peeraddr,
                          buffer, WIRELESS_MTU + sizeof(gas_action_comeback_resp_frame));
        cs->sofar += WIRELESS_MTU;
        cs->left -= WIRELESS_MTU;
    } else {
        cb_resp->query_resplen = cs->left;
        printf("sending final fragment of %d to " MACSTR ", %d so far and %d left\n",
               cs->left, MAC2STR(cs->peeraddr), cs->sofar, cs->left);
        memcpy(cb_resp->query_resp, cs->buf+cs->sofar, cs->left);
        cons_action_frame(GAS_COMEBACK_RESPONSE, cs->myaddr, cs->peeraddr,
                          buffer, cs->left + sizeof(gas_action_comeback_resp_frame));
        cs->sofar = cs->left = 0;
        memset((char *)&cs->resp_hdr, 0, sizeof(gas_action_comeback_resp_frame));
        free(cs->buf); cs->buf = NULL;
    }
    return cs->left;
}

void
message_from_controller (int fd, void *data)
{
    struct cstate *cs = (struct cstate *)data;
    char buf[8192];
    uint32_t netlen;
    int len, rlen;

    if (read(cs->fd, (char *)&netlen, sizeof(uint32_t)) < 0) {
        fprintf(stderr, "unable to read message from controller!\n");
        srv_rem_input(srvctx, cs->fd);
        close(cs->fd);
        return;
    }
    netlen = ntohl(netlen);
    if (netlen > sizeof(buf)) {
        fprintf(stderr, "Not gonna read in %d bytes\n",
                netlen);
        srv_rem_input(srvctx, cs->fd);
        close(cs->fd);
        return;
    }
    len = 0;
    while(netlen) {
        if ((rlen = read(cs->fd, (buf + len), netlen)) < 1) {
            fprintf(stderr, "unable to read message from controller!\n");
            srv_rem_input(srvctx, cs->fd);
            close(cs->fd);
            return;
        }
        len += rlen;
        netlen -= rlen;
    }
    if (len == 0) {
        srv_rem_input(srvctx, cs->fd);
        close(cs->fd);
        return;
    }
        
    printf("read %d byte message from controller\n", len);
    if (cs->left) {
        fprintf(stderr, "we're still defraging the last message, chill!\n");
        return;
    }
    if (buf[0] == GAS_INITIAL_RESPONSE) {
        /*
         * keep a copy of the header for each fragment
         */
        memcpy(&cs->resp_hdr, &buf[1], sizeof(gas_action_resp_frame));
    }
    len--;      // minus 1 because the field is not part of the message to frag
    if (len > WIRELESS_MTU) {
        /*
         * fragmented initial response when the singular controller response is
         * too big, fragmented comeback response when the controller is
         * sending back certificates after first telling the enrollee to comeback
         */
        printf("need to fragment message that is %d\n", len);
        switch (buf[0]) {
            case GAS_INITIAL_RESPONSE:
                len -= sizeof(gas_action_resp_frame);
                cs->sofar = 0;
                cs->left = len;
                if ((cs->buf = malloc(len)) == NULL) {
                    fprintf(stderr, "unable to allocate space to fragment %d byte message!\n", len);
                    return;
                }
                memcpy(cs->buf, (buf + sizeof(gas_action_resp_frame) + 1), len);
                print_buffer("First 32 octets that I'm gonna fragment", (unsigned char *)cs->buf, 32); 
                cons_next_fragment(cs, 1);
                break;
            case GAS_COMEBACK_RESPONSE:
                len -= sizeof(gas_action_comeback_resp_frame);
                cs->sofar = 0;
                cs->left = len;
                if ((cs->buf = malloc(len)) == NULL) {
                    fprintf(stderr, "unable to allocate space to fragment %d byte message!\n", len);
                    return;
                }
                memcpy(cs->buf, (buf + sizeof(gas_action_comeback_resp_frame) + 1), len);
                print_buffer("First 32 octets that I'm gonna fragment", (unsigned char *)cs->buf, 32); 
                cons_next_fragment(cs, 0);
                break;
            default:
                fprintf(stderr, "bad frame from controller that needs fragmenting: %d\n",
                        buf[0]);
                return;
        }
    } else {
        printf("sending message from " MACSTR " to " MACSTR "\n",
               MAC2STR(cs->myaddr), MAC2STR(cs->peeraddr));
        if (cons_action_frame(buf[0], cs->myaddr, cs->peeraddr,
                              &buf[1], len) < 1) {
            fprintf(stderr, "unable to send message from controller to peer!\n");
            srv_rem_input(srvctx, cs->fd);
            close(cs->fd);
            return;
        }
    }
    return;
}

static void
process_incoming_mgmt_frame (struct interface *inf, struct ieee80211_mgmt_frame *frame, int framesize)
{
    dpp_action_frame *dpp;
    struct cstate *cs;
    TLV *tlv;
    unsigned short frame_control;
    int type, stype, left;
    unsigned char broadcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    struct sockaddr_in clnt;
    char buf[3000], *ptr;
    uint32_t netlen;

    /*
     * if we sent it, ignore it
     */
    if (memcmp(frame->sa, inf->bssid, ETH_ALEN) == 0) {
        return;
    }
    /*
     * if it's not for us and not broadcast, ignore it
     */
    if (memcmp(frame->da, inf->bssid, ETH_ALEN) &&
        memcmp(frame->da, broadcast, ETH_ALEN)) {
        return;
    }
    memset(buf, 0, sizeof(buf));

    frame_control = ieee_order(frame->frame_control);
    type = IEEE802_11_FC_GET_TYPE(frame_control);
    stype = IEEE802_11_FC_GET_STYPE(frame_control);

    printf("got an 802.11 frame from " MACSTR " on " MACSTR "\n", MAC2STR(frame->sa), MAC2STR(frame->da));
    /*
     * if it's not a public action frame then we don't care about it!
     */
    if ((type == IEEE802_11_FC_TYPE_MGMT) ||
        (stype != IEEE802_11_FC_STYPE_ACTION) ||
        (frame->action.category != ACTION_PUBLIC)) {

        left = framesize - (IEEE802_11_HDR_LEN + sizeof(frame->action));
        /*
         * copy the message into the buffer to send to the controller...
         */
        ptr = buf;
        netlen = htonl(left + 1);
        memcpy(ptr, (char *)&netlen, sizeof(uint32_t));
        ptr += sizeof(uint32_t);

        *ptr = frame->action.field;
        ptr++;
        
        memcpy(ptr, frame->action.variable, left);

        switch (frame->action.field) {
            case PUB_ACTION_VENDOR:
                /* 
                 * PKEX, DPP Auth, and DPP Discovery
                 */
                dpp = (dpp_action_frame *)frame->action.variable;
                switch (dpp->frame_type) {
                    /*
                     * DPP Auth
                     */
                    case DPP_SUB_AUTH_REQUEST:
                        printf("a DPP Auth request...\n");
                        tlv = (TLV *)dpp->attributes;  // Br -- check whether for controller
                        if ((TLV_length(tlv) != SHA256_DIGEST_LENGTH) ||
                            (TLV_type(tlv) != RESPONDER_BOOT_HASH) ||
                            memcmp(inf->bkhash, TLV_value(tlv), SHA256_DIGEST_LENGTH)) {
                            break;
                        }
                        TAILQ_FOREACH(cs, &cstates, entry) {
                            printf("checking whether " MACSTR " equals " MACSTR "\n",
                                   MAC2STR(cs->peeraddr), MAC2STR(frame->sa));
                            if (memcmp(cs->peeraddr, frame->sa, ETH_ALEN) == 0) {
                                break;
                            }
                        }
                        if (cs != NULL) {
                            printf("sending %ld byte message from " MACSTR " back to controller...\n\n",
                                   left+sizeof(uint32_t)+1, MAC2STR(cs->peeraddr));
                            if (write(cs->fd, buf, left+sizeof(uint32_t)+1) < 1) {
                                fprintf(stderr, "relay: unable to send message to controller!\n");
                            }
                            break;
                        }
                        /* fall through intentional, we didn't have a connection */
                    case DPP_CHIRP:
                    case PKEX_SUB_EXCH_REQ:
                        printf("a chirp or a PKEX request\n");
                        /*
                         * a gratuitous request, create new client state...
                         */
                        if ((cs = (struct cstate *)malloc(sizeof(struct cstate))) == NULL) {
                            return;
                        }
                        memset(cs, 0, sizeof(struct cstate));
                        
                        memset((char *)&clnt, 0, sizeof(struct sockaddr_in));

                        /*
                         * connect to the controller
                         */
                        clnt.sin_family = AF_INET;
                        clnt.sin_addr.s_addr = inet_addr(controller);
                        clnt.sin_port = htons(portout);
                        if ((cs->fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
                            fprintf(stderr, "unable to create socket to connect to ontroller!\n");
                            free(cs);
                            return;
                        }
                        if (connect(cs->fd, (struct sockaddr *)&clnt, sizeof(struct sockaddr_in)) < 0) {
                            fprintf(stderr, "unable to connect to controller to %s:%d!\n", controller, portout);
                            close(cs->fd);
                            free(cs);
                            return;
                        }
                        memcpy(cs->peeraddr, frame->sa, ETH_ALEN);
                        if (memcmp(frame->da, broadcast, ETH_ALEN) == 0) {
                            memcpy(cs->myaddr, inf->bssid, ETH_ALEN);
                        } else {
                            memcpy(cs->myaddr, frame->da, ETH_ALEN);
                        }
                        if (dpp->frame_type == DPP_SUB_AUTH_REQUEST) {
                            tlv = (TLV *)dpp->attributes;
                            tlv = TLV_next(tlv);    // point to Br after Status
                            memcpy(cs->bkhash, tlv->value, SHA256_DIGEST_LENGTH);
                        }
                        /*
                         * add the new client state to the list and set the socket callback
                         */
                        TAILQ_INSERT_TAIL(&cstates, cs, entry);
                        srv_add_input(srvctx, cs->fd, cs, message_from_controller);
                        /*
                         * send the message to the controller
                         */
                        printf("sending %ld byte message from " MACSTR " back to controller...\n\n",
                               left+sizeof(uint32_t)+1, MAC2STR(cs->peeraddr));
                        if (write(cs->fd, buf, left+sizeof(uint32_t)+1) < 1) {
                            fprintf(stderr, "relay: unable to send message to controller!\n");
                        }
                        break;
                    case DPP_SUB_AUTH_RESPONSE:
                        printf("a DPP auth response\n");
                        tlv = (TLV *)frame->action.variable;
                        tlv = TLV_next(tlv);    // point to Br after Status
                        /*
                         * find the outstanding client state structure
                         */
                        TAILQ_FOREACH(cs, &cstates, entry) {
                            printf("checking whether " MACSTR " equals " MACSTR "\n",
                                   MAC2STR(cs->peeraddr), MAC2STR(frame->sa));
                            if (memcmp(cs->peeraddr, frame->sa, ETH_ALEN) == 0) {
                                break;
                            }
                            if ((memcmp(cs->peeraddr, broadcast, ETH_ALEN) == 0) &&
                                (memcmp(cs->bkhash, tlv->value, SHA256_DIGEST_LENGTH) == 0)) {
                                memcpy(cs->peeraddr, frame->sa, ETH_ALEN);
                                break;
                            }
                        }
                        if (cs == NULL) {
                            printf("didn't find state for peer at " MACSTR "\n",
                                   MAC2STR(frame->sa));
                            return;
                        }
                        memcpy(cs->myaddr, frame->da, ETH_ALEN);
                        printf("sending %ld byte message from " MACSTR " back to controller...\n\n",
                               left+sizeof(uint32_t)+1, MAC2STR(cs->peeraddr));
                        if (write(cs->fd, buf, left+sizeof(uint32_t)+1) < 1) {
                            fprintf(stderr, "unable to send message to controller!\n");
                            return;
                        }
                        break;
                    case DPP_SUB_AUTH_CONFIRM:
                    case DPP_SUB_PEER_DISCOVER_REQ:
                    case DPP_SUB_PEER_DISCOVER_RESP:
                    case PKEX_SUB_COM_REV_REQ:
                    case PKEX_SUB_COM_REV_RESP:
                    case DPP_CONFIG_RESULT:
                        printf("one of the package of other frames...\n");
                        /*
                         * find the client state and send this off!
                         */
                        TAILQ_FOREACH(cs, &cstates, entry) {
                            if (memcmp(cs->peeraddr, frame->sa, ETH_ALEN) == 0) {
                                break;
                            }
                        }
                        if (cs == NULL) {
                            return;
                        }
                        printf("sending %ld byte message from " MACSTR " back to controller...\n\n",
                               left+sizeof(uint32_t)+1, MAC2STR(cs->peeraddr));
                        if (write(cs->fd, buf, left+sizeof(uint32_t)+1) < 1) {
                            fprintf(stderr, "unable to send message to controller!\n");
                            return;
                        }
                        break;
                    case PKEX_SUB_EXCH_RESP:
                        printf("a PKEX response\n");
                        tlv = (TLV *)frame->action.variable;
                        tlv = TLV_next(tlv);    // point to Identifier after group
                        /*
                         * find the outstanding client state structure
                         */
                        TAILQ_FOREACH(cs, &cstates, entry) {
                            if ((memcmp(cs->peeraddr, broadcast, ETH_ALEN) == 0) &&
                                (memcmp(cs->bkhash, tlv->value,
                                        TLV_length(tlv) < SHA256_DIGEST_LENGTH ?
                                        TLV_length(tlv) : SHA256_DIGEST_LENGTH) == 0)) {
                                memcpy(cs->peeraddr, frame->sa, ETH_ALEN);
                                break;
                            }
                            if (memcmp(cs->peeraddr, frame->sa, ETH_ALEN) == 0) {
                                break;
                            }
                        }
                        if (cs == NULL) {
                            return;
                        }
                        memcpy(cs->myaddr, frame->da, ETH_ALEN);
                        printf("sending %ld byte message from " MACSTR " back to controller...\n\n",
                               left+sizeof(uint32_t)+1, MAC2STR(cs->peeraddr));
                        if (write(cs->fd, buf, left+sizeof(uint32_t)+1) < 1) {
                            fprintf(stderr, "unable to send message to controller!\n");
                            return;
                        }
                        break;
                    default:
                        fprintf(stderr, "unknown DPP frame %d\n", dpp->frame_type);
                        break;
                }
                break;
            /*
             * DPP Configuration protocol
             */
            case GAS_INITIAL_REQUEST:
            case GAS_INITIAL_RESPONSE:
            case GAS_COMEBACK_REQUEST:
            case GAS_COMEBACK_RESPONSE:
                printf("GAS frame...\n");
                /*
                 * find the client state and send this off!
                 */
                TAILQ_FOREACH(cs, &cstates, entry) {
                    if (memcmp(cs->peeraddr, frame->sa, ETH_ALEN) == 0) {
                        break;
                    }
                }
                if (cs == NULL) {
                    return;
                }
                if (cs->left) {
                    if (frame->action.field != GAS_COMEBACK_REQUEST) {
                        fprintf(stderr, "in the middle of fragmenting got a %s\n",
                                frame->action.field == GAS_INITIAL_REQUEST ? "GAS Initial Request" : \
                                frame->action.field == GAS_INITIAL_RESPONSE ? "GAS Initial Response" : \
                                "GAS Comeback Response");
                        return;
                    }
                    printf("sending next fragment, %d left\n", cs->left);
                    cons_next_fragment(cs, 0);
                } else {
                    printf("sending %ld byte message from " MACSTR " back to controller...\n\n",
                           left+sizeof(uint32_t)+1, MAC2STR(cs->peeraddr));
                    if (write(cs->fd, buf, left+sizeof(uint32_t)+1) < 1) {
                        fprintf(stderr, "unable to send message to controller!\n");
                        return;
                    }
                }
                break;
            default:
                break;
        }
    }
}

static int
mgmt_frame_in (struct nl_msg *msg, void *data)
{
    struct interface *inf = (struct interface *)data;
    struct nlattr *tb[NL80211_ATTR_MAX + 1];
    struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
    struct ieee80211_mgmt_frame *frame;
    int framesize;
    
    nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
              genlmsg_attrlen(gnlh, 0), NULL);
    if (tb[NL80211_ATTR_FRAME]) {
        frame = (struct ieee80211_mgmt_frame *)nla_data(tb[NL80211_ATTR_FRAME]);
        framesize = nla_len(tb[NL80211_ATTR_FRAME]);
        process_incoming_mgmt_frame(inf, frame, framesize);
    }
    
    return NL_SKIP;
}

static void
nl_sock_in (int fd, void *data)
{
    struct interface *inf = (struct interface *)data;
    int res;
    
    if ((res = nl_recvmsgs(inf->nl_sock, inf->nl_cb)) < 0) {
        fprintf(stderr, "nl_recvmsgs failed: %d (%s)\n", res, nl_geterror(res));
        srv_rem_input(srvctx, nl_socket_get_fd(inf->nl_sock));
    }
}

static void
bpf_frame_in (int fd, void *data)
{
    struct interface *inf = (struct interface *)data;
    unsigned char buf[2048];
    struct ieee80211_mgmt_frame *frame;
    struct sockaddr_ll from;
    socklen_t fromlen;
    int framesize;

    fromlen = sizeof(from);
    if ((framesize = recvfrom(fd, buf, sizeof(buf), MSG_TRUNC,
                              (struct sockaddr *)&from, &fromlen)) < 0) {
        fprintf(stderr, "can't read off bpf socket!\n");
        perror("read");
        return;
    }
    /*
     * we don't want to see outgoing packets otherwise we'll see
     * everything twice
     */
    if (from.sll_pkttype == PACKET_OUTGOING) {
        return;
    }
    
    frame = (struct ieee80211_mgmt_frame *)buf;
    process_incoming_mgmt_frame(inf, frame, framesize);

    return;
}

static int
register_action_frame (struct interface *inf, int flags, unsigned char *match, int match_len)
{
    struct nl_msg *msg;
    unsigned short type = (IEEE802_11_FC_TYPE_MGMT << 2 | IEEE802_11_FC_STYPE_ACTION << 4);

    if ((msg = get_nl_msg(inf, flags, NL80211_CMD_REGISTER_FRAME)) == NULL) {
        fprintf(stderr, "can't allocate an nl_msg!\n");
        return -1;
    }
    nla_put_u16(msg, NL80211_ATTR_FRAME_TYPE, type);
    nla_put(msg, NL80211_ATTR_FRAME_MATCH, match_len, match);
    if (send_mgmt_msg(msg, inf, mgmt_frame_in, inf)) {
        fprintf(stderr, "unable to register for action frame!\n");
        return -1;
    }
    
    return 1;
}

/*
 * fin()
 *      sae has finished for the specified MAC address. If the reason
 *      is because it was successful, there will be a key (PMK) to plumb
 */
void
fin (unsigned short reason, unsigned char *mac, unsigned char *key, int keylen)
{
    printf("status of " MACSTR " is %d, ", MAC2STR(mac), reason);
    if ((reason == 0) && (key != NULL) && (keylen > 0)) {
        printf("plumb the %d byte key into the kernel now!\n", keylen);
    } else {
        printf("(an error)\n");
    }
}

static struct nl_sock *
create_nl_socket (struct nl_cb *cb)
{
    struct nl_sock *sock;
    uint32_t pid = getpid() & 0x3fffff;
    int i;
    
    if ((sock = nl_socket_alloc_cb(cb)) == NULL) {
        fprintf(stderr, "unable to alloc an nl socket\n");
        return NULL;
    }

    for (i = 0; i < 1024; i++) {
        if (port_bitmap[i/32] & (1 << (i % 32))) {
            continue;
        }
        port_bitmap[i/32] |= 1 << (i % 32);
        pid += i << 22;
        break;
    }
    nl_socket_set_local_port(sock, pid);

    if (genl_connect(sock)) {
        fprintf(stderr, "unable to connect an nl socket!\n");
        return NULL;
    }
    return sock;
}

static int family_handler(struct nl_msg *msg, void *arg)
{
    struct family_data *res = (struct family_data *)arg;
    struct nlattr *tb[CTRL_ATTR_MAX + 1];
    struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
    struct nlattr *mcgrp;
    int i;

    printf("inside family handler....\n");
    nla_parse(tb, CTRL_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
              genlmsg_attrlen(gnlh, 0), NULL);
    if (!tb[CTRL_ATTR_MCAST_GROUPS])
        return NL_SKIP;

    nla_for_each_nested(mcgrp, tb[CTRL_ATTR_MCAST_GROUPS], i) {
        struct nlattr *tb2[CTRL_ATTR_MCAST_GRP_MAX + 1];
        nla_parse(tb2, CTRL_ATTR_MCAST_GRP_MAX, nla_data(mcgrp),
                  nla_len(mcgrp), NULL);
        if (!tb2[CTRL_ATTR_MCAST_GRP_NAME] ||
            !tb2[CTRL_ATTR_MCAST_GRP_ID] ||
            strncmp(nla_data(tb2[CTRL_ATTR_MCAST_GRP_NAME]),
                    res->group,
                    nla_len(tb2[CTRL_ATTR_MCAST_GRP_NAME])) != 0)
            continue;
        res->id = nla_get_u32(tb2[CTRL_ATTR_MCAST_GRP_ID]);
        break;
    };

    return NL_SKIP;
}

static int
nl_get_multicast_id (struct interface *inf, const char *family, const char *group)
{
    struct nl_msg *msg;
    struct family_data res = { group, -ENOENT };
    
    if ((msg = nlmsg_alloc()) == NULL) {
        return -1;
    }
    if (!genlmsg_put(msg, 0, 0, genl_ctrl_resolve(inf->nl_sock, "nlctrl"), 0, 0,
                     CTRL_CMD_GETFAMILY, 0) ||
        nla_put_string(msg, CTRL_ATTR_FAMILY_NAME, family)) {
        nlmsg_free(msg);
        return -1;
    }
    if (send_nl_msg(msg, inf, family_handler, &res) < 0) {
        return -1;
    }
    return res.id;
}
    
static int
capabilities_handler (struct nl_msg *msg, void *arg)
{
    struct interface *inf = arg;
    struct nlattr *tb[NL80211_ATTR_MAX + 1];
    struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));

    nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
              genlmsg_attrlen(gnlh, 0), NULL);
    if (tb[NL80211_ATTR_OFFCHANNEL_TX_OK]) {
        inf->offchan_tx_ok = 1;
    } else {
        inf->offchan_tx_ok = 0;
    }
    if (tb[NL80211_ATTR_MAX_REMAIN_ON_CHANNEL_DURATION]) {
        inf->max_roc = nla_get_u32(tb[NL80211_ATTR_MAX_REMAIN_ON_CHANNEL_DURATION]);
    } else {
        inf->max_roc = 5000;
    }
    return NL_SKIP;
}

static int
get_driver_capabilities (struct interface *inf)
{
    struct nl_msg *msg;

    if ((msg = get_nl_msg(inf, 0, NL80211_CMD_GET_WIPHY)) == NULL) {
        fprintf(stderr, "unable to create nl_msg to get driver capabilities\n");
        return -1;
    }
    nla_put_flag(msg, NL80211_ATTR_SPLIT_WIPHY_DUMP);
    if (send_nl_msg(msg, inf, capabilities_handler, inf) < 0) {
        fprintf(stderr, "unable to send nl_msg to get driver capabilities\n");
        return -1;
    }
    return 1;
}

static void
add_interface (char *ptr)
{
    struct interface *inf;
    struct ifreq ifr;
    struct sockaddr_ll sll;
    int ifidx, mid;
    struct nl_msg *msg;

    TAILQ_FOREACH(inf, &interfaces, entry) {
        if (memcmp(&inf->ifname, ptr, strlen(ptr)) == 0) {
            printf("%s is already on the list!\n", ptr);
            return;
        }
    }
    if ((inf = (struct interface *)malloc(sizeof(struct interface))) == NULL) {
        fprintf(stderr, "failed to malloc space for new interface %s!\n", ptr);
        return;
    }
    strcpy(inf->ifname, ptr);

    /*
     * see if this is a loopback interface
     */
    if ((inf->fd = socket(PF_PACKET, SOCK_RAW, 0)) < 0) {
        fprintf(stderr, "unable to get raw socket to determine interface flags!\n");
        free(inf);
        return;
    }

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, inf->ifname, IFNAMSIZ);
    if (ioctl(inf->fd, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, "unable to get if index on %s\n", inf->ifname);
        return;
    }
    ifidx = ifr.ifr_ifindex;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, inf->ifname, IFNAMSIZ);
    if (ioctl(inf->fd, SIOCGIFFLAGS, &ifr) < 0) {
        fprintf(stderr, "unable to get ifflags for %s!\n", ptr);
        /*
         * should this be fatal? Dunno, let's just assume it's _not_ loopback
         */
        ifr.ifr_flags = 0;
    }
    if ((ifr.ifr_flags & IFF_LOOPBACK) == 0) {
        fprintf(stderr, "%s is NOT the loopback!\n", inf->ifname);
        inf->is_loopback = 0;

        inf->ifindex = ifidx;
        /*
         * set up the NL socket goo
         */
        if ((inf->nl_cb = nl_cb_alloc(NL_CB_DEFAULT)) == NULL) {
            fprintf(stderr, "unable to alloc an nl cb on %s\n", inf->ifname);
            free(inf);
            return;
        }

        if ((inf->nl_sock = create_nl_socket(inf->nl_cb)) == NULL) {
            fprintf(stderr, "failed to create nl_sock on %s\n", inf->ifname);
            free(inf);
            return;
        }

        if ((inf->nl80211_id = genl_ctrl_resolve(inf->nl_sock, "nl80211")) < 0) {
            fprintf(stderr, "unable to get nl80211 id!\n");
            nl_socket_free(inf->nl_sock);
            free(inf);
            return;
        }

        if ((mid = nl_get_multicast_id(inf, "nl80211", "scan")) < 0) {
            fprintf(stderr, "unable to get multicast id for mlme!\n");
            nl_socket_free(inf->nl_sock);
            free(inf);
            return;
        }
        nl_socket_add_membership(inf->nl_sock, mid);

        if ((mid = nl_get_multicast_id(inf, "nl80211", "mlme")) < 0) {
            fprintf(stderr, "unable to get multicast id for mlme!\n");
            nl_socket_free(inf->nl_sock);
            free(inf);
            return;
        }
        nl_socket_add_membership(inf->nl_sock, mid);

        if ((mid = nl_get_multicast_id(inf, "nl80211", "regulatory")) < 0) {
            fprintf(stderr, "unable to get multicast id for mlme!\n");
            nl_socket_free(inf->nl_sock);
            free(inf);
            return;
        }
        nl_socket_add_membership(inf->nl_sock, mid);

        if ((msg = get_nl_msg(inf, 0, NL80211_CMD_GET_INTERFACE)) == NULL) {
            fprintf(stderr, "unable to get nl_msg to get interface!\n");
            nl_socket_free(inf->nl_sock);
            free(inf);
            return;
        }
        printf("\ngetting the interface!\n");
        if (send_nl_msg(msg, inf, get_phy_info, inf) < 0) {
            fprintf(stderr, "unable to send nl_msg to get interface!\n");
            nl_socket_free(inf->nl_sock);
            free(inf);
            return;
        }
        printf("%s is interface %ld from ioctl\n", ptr, inf->ifindex);
        ifidx = if_nametoindex(ptr);
        printf("%s is interface %d from if_nametoindex()\n", ptr, ifidx);

        close(inf->fd);
        inf->fd = nl_socket_get_fd(inf->nl_sock);

        nl_cb_set(inf->nl_cb, NL_CB_SEQ_CHECK, NL_CB_CUSTOM, no_seq_check, NULL);
        nl_cb_set(inf->nl_cb, NL_CB_VALID, NL_CB_CUSTOM, mgmt_frame_in, inf);

        if (get_driver_capabilities(inf) < 0) {
            printf("can't get driver capabilities!\n");
            inf->offchan_tx_ok = 1;
            inf->max_roc = 5000;
        } else {
            printf("got driver capabilities, off chan is %s, max_roc is %ld\n",
                   inf->offchan_tx_ok ? "ok" : "not ok", inf->max_roc);
        }

        /*
         * register the various action frames we want to see
         */
        printf("\nask for GAS request frames\n");
        if (register_action_frame(inf, 0, (unsigned char *)GAS_INITIAL_REQ_MATCH, GAS_INITIAL_REQ_MATCH_LEN) < 1) {
            free(inf);
            return;
        }
        printf("\nask for GAS response frames\n");
        if (register_action_frame(inf, 0, (unsigned char *)GAS_INITIAL_RESP_MATCH, GAS_INITIAL_RESP_MATCH_LEN) < 1) {
            free(inf);
            return;
        }
        printf("\nask for GAS comeback request frames\n");
        if (register_action_frame(inf, 0, (unsigned char *)GAS_COMEBACK_REQ_MATCH, GAS_COMEBACK_REQ_MATCH_LEN) < 1) {
            free(inf);
            return;
        }
        printf("\nask for GAS comeback response frames\n");
        if (register_action_frame(inf, 0, (unsigned char *)GAS_COMEBACK_RESP_MATCH, GAS_COMEBACK_RESP_MATCH_LEN) < 1) {
            free(inf);
            return;
        }
        printf("\nask for DPP action frames\n");
        if (register_action_frame(inf, 0, (unsigned char *)DPP_PUB_ACTION_MATCH, DPP_PUB_ACTION_MATCH_LEN) < 1) {
            free(inf);
            return;
        }

        nl_socket_set_nonblocking(inf->nl_sock);
        printf("socket %d is for nl_sock_in\n", nl_socket_get_fd(inf->nl_sock));
        srv_add_input(srvctx, nl_socket_get_fd(inf->nl_sock), inf, nl_sock_in);
    } else {
        inf->is_loopback = 1;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, inf->ifname, IFNAMSIZ);
        if (ioctl(inf->fd, SIOCGIFINDEX, &ifr) < 0) {
            fprintf(stderr, "unable to get if index on %s\n", inf->ifname);
            free(inf);
            return;
        }

        memset(&sll, 0, sizeof(sll));
        sll.sll_family = AF_PACKET;
        sll.sll_ifindex = ifr.ifr_ifindex;
        sll.sll_protocol = htons(ETH_P_ALL);
        if (bind(inf->fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
            fprintf(stderr, "unable to bind socket to %s\n", inf->ifname);
            free(inf);
            return;
        }

        /*
         * make up a bssid for the loopback interface
         */
        if (!RAND_bytes(&inf->bssid[0], ETH_ALEN)) {
            fprintf(stderr, "unable to make a fake BSSID on %s!\n", inf->ifname);
        }
        srv_add_input(srvctx, inf->fd, inf, bpf_frame_in);
    }
    
    TAILQ_INSERT_TAIL(&interfaces, inf, entry);
    return;
}

static int
change_freq (unsigned char *mac, unsigned long freak)
{
    struct nl_msg *msg;
    unsigned long long cookie;
    struct interface *inf;
    
    TAILQ_FOREACH(inf, &interfaces, entry) {
        if (memcmp(mac, inf->bssid, ETH_ALEN) == 0) {
            break;
        }
    }
    if (inf == NULL) {
        fprintf(stderr, "can't find " MACSTR " to change channel!\n",
                MAC2STR(mac));
        return -1;
    }
    if (inf->is_loopback) {
        return 1;
    }

    if ((msg = get_nl_msg(inf, 0, NL80211_CMD_SET_WIPHY)) == NULL) {
        fprintf(stderr, "can't allocate an nl_msg!\n");
        return -1;
    }
    nla_put_u32(msg, NL80211_ATTR_WIPHY_FREQ, freak);
    nla_put_u32(msg, NL80211_ATTR_WIPHY_CHANNEL_TYPE, NL80211_CHAN_NO_HT);
    nla_put_u32(msg, NL80211_ATTR_DURATION, inf->max_roc);
    cookie = 0;

    if (send_nl_msg(msg, inf, cookie_handler, &cookie)) {
        fprintf(stderr, "unable to change channel!\n");
//        return -1;
    }
    printf("changing frequency to %ld\n", freak);
    inf->freq = freak;
    return 1;
}

static unsigned long
chan2freq (unsigned int chan)
{
    if (chan == 14) {
        return 2484;
    }
    if (chan < 14) {
        return 2407 + chan * 5;
    }
    if (chan < 27) {
        return 2512 + ((chan - 15)*20);
    }
    return 5000 + (chan * 5);
}

int
change_channel (unsigned char *mymac, unsigned char class, unsigned char channel)
{
    int i, maxregs;
    unsigned long freak;

    /*
     * make sure it's a valid class/channel
     */
    maxregs = (sizeof(regulatory)/sizeof(struct _regulatory));
    for (i = 0; i < maxregs; i++) {
        if ((regulatory[i].class == class) && (regulatory[i].channel == channel)) {
            break;
        }
    }
    if (i == maxregs) {
        fprintf(stderr, "opclass %d and channel %d is not supported!\n",
                class, channel);
        return -1;
    }

    freak = chan2freq(channel);
    printf("changing channel to %d and operating class %d (frequency %ld)\n", channel, class, freak);
    return change_freq(mymac, freak);
}

void
new_controller (int fd, void *data)
{
    struct sockaddr_in *serv = (struct sockaddr_in *)data;
    int sd, len, rlen;
    unsigned int clen;
    struct cstate *cs;
    unsigned char broadcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    struct interface *inf;
    char buf[3000];
    TLV *tlv;
    uint32_t netlen;
    dpp_action_frame *frame;
    struct wired_control *ctrl;
  
    printf("new controller!!!\n");
    clen = sizeof(struct sockaddr_in);
    if ((sd = accept(fd, (struct sockaddr *)serv, &clen)) < 0) {
        fprintf(stderr, "failed to accept new connection from controller!\n");
        return;
    }
    
    if ((cs = (struct cstate *)malloc(sizeof(struct cstate))) == NULL) {
        close(sd);
        fprintf(stderr, "failed to create connection from controller!\n");
        return;
    }
    memset(cs, 0, sizeof(struct cstate));
    cs->fd = sd;

    if (read(cs->fd, (char *)&netlen, sizeof(uint32_t)) < 0) {
        close(sd);
        free(cs);
        return;
    }
    netlen = ntohl(netlen);

    len = 0;
    while (netlen) {
        if ((rlen = read(cs->fd, (buf + len), netlen)) < 1) {
            fprintf(stderr, "unable to read message from new controller\n");
            close(sd);
            free(cs);
            return;
        }
        len += rlen;
        netlen -= rlen;
    }

    /*
     * if the controller has more than just the bootstrapping key then we get
     * that stuff first in a wired controll message (field = -1)
     */
    if (buf[0] == -1) {
        if (len < sizeof(struct wired_control)) {
            return;
        }
        ctrl = (struct wired_control *)&buf[1];
        printf("received notification of enrollee at " MACSTR " on %d/%d\n",
               MAC2STR(ctrl->peermac), ctrl->opclass, ctrl->channel);

        memcpy(cs->peeraddr, ctrl->peermac, ETH_ALEN);
        /*
         * TODO: figure out how to identify the right interface 
         */
        memcpy(cs->myaddr, ((struct interface *)TAILQ_FIRST(&interfaces))->bssid, ETH_ALEN);
        change_channel(((struct interface *)TAILQ_FIRST(&interfaces))->bssid, ctrl->opclass, ctrl->channel);

        TAILQ_INSERT_TAIL(&cstates, cs, entry);
        /*
         * the request is forthcoming, so just sit back and wait for it...
         */
        srv_add_input(srvctx, cs->fd, cs, message_from_controller);
        return;
    } else {
        memcpy(cs->peeraddr, broadcast, ETH_ALEN);
    }
    
    /*
     * this has to either be a DPP auth request or a PKEX exchange request
     */
    if (buf[0] != PUB_ACTION_VENDOR) {
        fprintf(stderr, "first message from controller not a DPP/PKEX request!\n");
        close(sd);
        free(cs);
        return;
    }
    frame = (dpp_action_frame *)&buf[1];
    switch (frame->frame_type) {
        /*
         * grab the hash of the peer's bootstrapping key to match up this
         * conversation when we get the AUTH_RESPONSE back
         */
        case DPP_SUB_AUTH_REQUEST:
            tlv = (TLV *)frame->attributes;
            memcpy(cs->bkhash, tlv->value, SHA256_DIGEST_LENGTH);
            break;
        case PKEX_SUB_EXCH_REQ:
            tlv = (TLV *)frame->attributes;
            tlv = TLV_next(tlv);        // see if there's an identifier
            if (TLV_type(tlv) == TRANSACTION_IDENTIFIER) {
                memcpy(cs->bkhash, tlv->value,
                       TLV_length(tlv) < SHA256_DIGEST_LENGTH ? TLV_length(tlv) : SHA256_DIGEST_LENGTH);
            }
            break;
        default:
            fprintf(stderr, "first message from controller not a DPP/PKEX request!\n");
            close(sd);
            free(cs);
            return;
    }
    /*
     * since we don't know where the peer is, send this out to all our interfaces.
     * -1 because the first byte read was PUB_ACTION_VENDOR
     */
    TAILQ_FOREACH(inf, &interfaces, entry) {
        cons_action_frame(PUB_ACTION_VENDOR, inf->bssid, broadcast, &buf[1], len - 1);
    }
    TAILQ_INSERT_TAIL(&cstates, cs, entry);
    
    srv_add_input(srvctx, cs->fd, cs, message_from_controller);

    return;
}

static void
compute_bk_hash (char *bkf)
{
    FILE *fp;
    BIO *bio;
    int asn1len;
    EVP_MD_CTX *mdctx;
    unsigned int mdlen = SHA256_DIGEST_LENGTH;
    unsigned char *asn1;
    EC_KEY *bk;
                
    if ((fp = fopen(bkf, "r")) == NULL) {
        fprintf(stderr, "unable to open %s as bootstrap key file\n", bkf);
        return;
    }
    bio = BIO_new(BIO_s_file());
    BIO_set_fp(bio, fp, BIO_CLOSE);
    if ((bk = PEM_read_bio_ECPrivateKey(bio, NULL, NULL, NULL)) == NULL) {
        fprintf(stderr, "unable to read bootstrap key from  %s\n", bkf);
        return;
    }
    BIO_free(bio);
    EC_KEY_set_conv_form(bk, POINT_CONVERSION_COMPRESSED);
    EC_KEY_set_asn1_flag(bk, OPENSSL_EC_NAMED_CURVE);

    if ((bio = BIO_new(BIO_s_mem())) == NULL) {
        fprintf(stderr, "unable to create bio for bootstrap key from %s\n", bkf);
        EC_KEY_free(bk);
        return;
    }
    if ((mdctx = EVP_MD_CTX_new()) == NULL) {
        BIO_free(bio);
        EC_KEY_free(bk);
        return;
    }
    (void)i2d_EC_PUBKEY_bio(bio, bk);
    (void)BIO_flush(bio);
    asn1len = BIO_get_mem_data(bio, &asn1);
    EVP_DigestInit(mdctx, EVP_sha256());
    EVP_DigestUpdate(mdctx, asn1, asn1len);
    EVP_DigestFinal(mdctx, bkeyhash, &mdlen);

    BIO_free(bio);
    EVP_MD_CTX_free(mdctx);
    EC_KEY_free(bk);
    return;
}

#ifdef HASAVAHI
static void
resolve_cb (AvahiServiceResolver *res, AvahiIfIndex ifidx, AvahiProtocol proto, AvahiResolverEvent event,
            const char *name, const char *type, const char *domain, const char *hostname,
            const AvahiAddress *addr, uint16_t port, AvahiStringList *txt, AvahiLookupResultFlags flags,
            void *userdata)
{
    char *t, *s;

    if (res == NULL) {
        return;
    }
    switch (event) {
        case AVAHI_RESOLVER_FAILURE:
            fprintf(stderr, "failed to resolve service '%s' of type '%s' in domain '%s' (error %s)\n",
                    name, type, domain, avahi_strerror(avahi_client_errno(avahi_service_resolver_get_client(res))));
            strcpy(controller, "ERR");
            portout = 0;
            break;
        case AVAHI_RESOLVER_FOUND:
            /*
             * record our resolved service...
             */
            t = avahi_string_list_to_string(txt);
            printf("resolved service '%s' of type '%s' in domain '%s' to %s on port %u (%s)\nTXT record: %s\n",
                   name, type, domain, hostname, port, controller, t);
            if (portout == 0) {
                char foo[AVAHI_ADDRESS_STR_MAX];
                int i;
                
                portout = port;
                avahi_address_snprint(foo, sizeof(foo), addr);
                if ((s = strstr(t, "bskeyhash=")) != NULL) {
                    /*
                     * +10 to skip "bskeyhash=" and 44 = ceil(SHA256_DIGEST_LENGTH/3)*4
                     */
                    i = EVP_DecodeBlock(bkeyhash, (unsigned char *)s+10, 44);
                    printf("decoded %d bytes\n", i);
                }
                printf("resolved address to %s\n", foo);
                strcpy(controller, foo);
                printf("recording controller %s at port %d and bskeyhash is %s\n", controller, portout, s+10);
            }
            avahi_free(t);
            break;
        default:
            fprintf(stderr, "resolver error: service not resolved (%d)\n", event);
            break;
    }
    avahi_service_resolver_free(res);
}

static void
check_results (AvahiTimeout *to, void *unused)
{
    avahi_simple_poll_quit(simple_poll);
    if (portout == 0) {
        fprintf(stderr, "unable to find a controller in the DNS, re-run with -C\n");
        exit(1);
    }
    printf("controller is at port %d on %s\n", portout, controller);
    return;
}

static void
browse_cb (AvahiServiceBrowser *browser, AvahiIfIndex ifidx, AvahiProtocol proto, AvahiBrowserEvent event,
           const char *name, const char *type, const char *domain, AvahiLookupResultFlags flags, void *userdata)
{
    AvahiClient *client = (AvahiClient *)userdata;
    struct timeval tv;

    if (browser == NULL) {
        fprintf(stderr, "null browser inside callback!\n");
        return;
    }
    switch (event) {
        case AVAHI_BROWSER_FAILURE:
            fprintf(stderr, "avahi browse failure\n");
            avahi_simple_poll_quit(simple_poll);
            break;
        case AVAHI_BROWSER_NEW:
            printf("new DPP service '%s' of type '%s' in domain '%s' found\n", name, type, domain);
            if (!avahi_service_resolver_new(client, ifidx, proto, name, type, domain,
                                            AVAHI_PROTO_INET, 0, resolve_cb, NULL)) {
                fprintf(stderr, "failed to resolve service '%s' (error %s)\n", name,
                        avahi_strerror(avahi_client_errno(client)));
            }
            break;
        case AVAHI_BROWSER_REMOVE:
            break;
        case AVAHI_BROWSER_CACHE_EXHAUSTED:
            break;
        case AVAHI_BROWSER_ALL_FOR_NOW:
            avahi_simple_poll_get(simple_poll)->timeout_new(avahi_simple_poll_get(simple_poll),
                                                            avahi_elapse_time(&tv, 1000, 0),
                                                            check_results, NULL);
            break;
    }
    return;
}

static void
client_cb (AvahiClient *c, AvahiClientState state, void *unused)
{
    if (c == NULL) {
        return;
    }
    if (state == AVAHI_CLIENT_FAILURE) {
        avahi_simple_poll_quit(simple_poll);
    }
}
#endif  /* HASAVAHI */

int
main (int argc, char **argv)
{
    int c, got_controller = 0, got_bsfile = 0, infd, opt;
    struct interface *inf;
    char iface[IFNAMSIZ], bootstrapfile[80]; 
    struct sockaddr_in saddr;
#ifdef HASAVAHI
    AvahiClient *client = NULL;
    AvahiServiceBrowser *sb = NULL;
    char *serv = "_controller._sub._dpp._tcp";
    int err;
#endif  /* HASAVAHI */

    if ((srvctx = srv_create_context()) == NULL) {
        fprintf(stderr, "%s: cannot create service context!\n", argv[0]);
        exit(1);
    }
    TAILQ_INIT(&interfaces);
    TAILQ_INIT(&cstates);
    memset(controller, 0, sizeof(controller));
    memset(bkeyhash, 0, SHA256_DIGEST_LENGTH);
    portin = DPP_PORT;
    portout = DPP_PORT;         /* may be overridden by mDNS */
    for (;;) {
        c = getopt(argc, argv, "hI:d:f:g:C:k:i:o:");
        if (c < 0) {
            break;
        }
        switch (c) {
            case 'I':           /* interface */
                if (strlen(optarg) > IFNAMSIZ) {
                    fprintf(stderr, "%s: interface name %s is 'too large'\n", argv[0], optarg);
                    exit(1);
                }
                strcpy(iface, optarg);
                printf("adding interface %s...\n", iface);
                add_interface(iface);
                break;
            case 'd':           /* debug */
//                debug = atoi(optarg);
                break;
            case 'f':           /* channel */
                channel = atoi(optarg);
                break;
            case 'g':           /* operating class */
                opclass = atoi(optarg);
                break;
            case 'C':
                got_controller = 1;
                strcpy(controller, optarg);
                break;
            case 'k':
                got_bsfile = 1;
                strcpy(bootstrapfile, optarg);
                break;
            case 'i':
                portin = atoi(optarg);
                break;
            case 'o':
                portout = atoi(optarg);
                break;
            default:
            case 'h':
                fprintf(stderr, 
                        "USAGE: %s [-hCIBapkceirdfgs]\n"
                        "\t-h  show usage, and exit\n"
                        "\t-I <interface> to add to DPP\n"
                        "\t-C <controller> to whom DPP frames are sent\n"
                        "\t-k <file> which is the Controller's bootstrapping key\n"
                        "\t-f <channel> to use with DPP\n"
                        "\t-g <opclass> operating class to use with DPP\n"
                        "\t-i <num> port number for inbound (default 8741)\n"
                        "\t-o <num> port number for outbound (default 8908)\n"
                        "\t-d <debug> set debugging mask\n",
                        argv[0]);
                exit(1);
                
        }
    }
    if (!got_controller) {
#ifdef HASAVAHI
        if ((simple_poll = avahi_simple_poll_new()) == NULL) {
            fprintf(stderr, "%s: unable to create avahi simple poll!\n", argv[0]);
            exit(1);
        }
        if ((client = avahi_client_new(avahi_simple_poll_get(simple_poll), 0, client_cb, NULL, &err)) == NULL) {
            fprintf(stderr, "%s: unable to create avahi client!\n", argv[0]);
            exit(1);
        }
        if ((sb = avahi_service_browser_new(client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, serv, NULL, 0,
                                            browse_cb, client)) == NULL) {
            fprintf(stderr, "%s: unable to create avahi service browser!\n", argv[0]);
            exit(1);
        }
        /*
         * do a quick browse for a controller....
         */
        avahi_simple_poll_loop(simple_poll);
#else    
        fprintf(stderr, "%s: need to specify a controller with -C or recompile with avahi\n", argv[0]);
        exit(1);
#endif  /* HASAVAHI */
    }
    if (TAILQ_EMPTY(&interfaces)) {
        fprintf(stderr, "%s: no interfaces defined!\n", argv[0]);
        add_interface("lo");
    }
    if (got_bsfile) {
        compute_bk_hash(bootstrapfile);
#ifndef HASAVAHI
    } else {
        fprintf(stderr, "%s: don't know the bootstrapping key hash! Try specifying a key with -k\n", argv[0]);
        exit(1);
#endif  /* !HASAVAHI */
    }
    printf("interfaces and MAC addresses:\n");
    TAILQ_FOREACH(inf, &interfaces, entry) {
        printf("\t%s: " MACSTR "\n", inf->ifname, MAC2STR(inf->bssid));
        if (inf->is_loopback) {
            srv_add_input(srvctx, inf->fd, inf, bpf_frame_in);
        } else {
            srv_add_input(srvctx, nl_socket_get_fd(inf->nl_sock), inf, nl_sock_in);
        }
        /*
         * for now just make all interfaces have the same bootstrap key and are on the
         * same channel
         */
        change_channel(inf->bssid, opclass, channel);
        memcpy(inf->bkhash, bkeyhash, SHA256_DIGEST_LENGTH);
    }
    /*
     * create and bind listening socket for new DPP conversations started
     * by the controller
     */
    if ((infd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "%s: unable to create inbound TCP socket!\n", argv[0]);
        exit(1);
    }
    opt = 1;
    if (setsockopt(infd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int)) < 0) {
        fprintf(stderr, "%s: unable to set TCP socket to reuse addr!\n", argv[0]);
        exit(1);
    }
    
    memset((char *)&saddr, 0, sizeof(struct sockaddr_in));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port = htons(portin);
    if ((bind(infd, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in)) < 0) ||
        (listen(infd, 0) < 0)) {
        fprintf(stderr, "%s: unable to bind/listen TCP socket!\n", argv[0]);
        exit(1);
    }
    srv_add_input(srvctx, infd, &saddr, new_controller);

    srv_main_loop(srvctx);

    exit(1);
}
