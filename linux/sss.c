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
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <net/if.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/if_tun.h>
#include <linux/nl80211.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/ec.h>
#include <openssl/bio.h>
#include "ieee802_11.h"
#include "radio.h"
#include "service.h"
#include "pkex.h"
#include "tlv.h"
#include "dpp.h"

struct interface {
    TAILQ_ENTRY(interface) entry;
    char ifname[IFNAMSIZ];
    unsigned char bssid[ETH_ALEN];
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

struct pkex_instance {
    TAILQ_ENTRY(pkex_instance) entry;
    pkex_handle handle;
    unsigned char mymac[ETH_ALEN];
    unsigned char peermac[ETH_ALEN];
};
TAILQ_HEAD(blah, pkex_instance) pkex_instances;

struct dpp_instance {
    TAILQ_ENTRY(dpp_instance) entry;
    dpp_handle handle;
    unsigned int tid;
    unsigned char mymac[ETH_ALEN];
    unsigned char peermac[ETH_ALEN];
};
TAILQ_HEAD(foo, dpp_instance) dpp_instances;

struct family_data {
    const char *group;
    int id;
};

struct trigger_results {
    int done;
    int aborted;
};

#define WIRELESS_MTU    1400

service_context srvctx;
static int discovered = -1;
char our_ssid[33];
unsigned int opclass = 81, channel = 6;
char bootstrapfile[80];
int quit_at_fin = 0;

extern int nl_debug;

static void
exceptor (int fd, void *unused)
{
    srv_rem_input(srvctx, fd);
}

static void
dump_ssid (struct ieee80211_mgmt_frame *frame, int len)
{
    char el_id, el_len, ssid[33];
    unsigned char *ptr;
    int left;

    ptr = frame->beacon.variable;
    left = len - (IEEE802_11_HDR_LEN + sizeof(frame->beacon));
    while (left > 2) {
        el_id = *ptr++;
        left--;
        el_len = *ptr++;
        left--;
        if (el_len > left) {
            return;
        }
        if (el_id == IEEE802_11_IE_SSID) {
            if (el_len > 32) {
                return;
            }
            memset(ssid, 0, sizeof(ssid));
            memcpy(ssid, ptr, el_len);
            break;
        }
    }
}

struct dpp_instance *
find_instance_by_mac (unsigned char *me, unsigned char *peer)
{
    struct dpp_instance *found;
    unsigned char broadcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    
    TAILQ_FOREACH(found, &dpp_instances, entry) {
        if (memcmp(found->mymac, me, ETH_ALEN) == 0) {
            if (memcmp(found->peermac, peer, ETH_ALEN) == 0) {
                break;
            }
            if (memcmp(found->peermac, broadcast, ETH_ALEN) == 0) {
                memcpy(found->peermac, peer, ETH_ALEN);
                break;
            }
        }
    }
    if (found == NULL) {
        fprintf(stderr, "unable to find dpp peer, src="MACSTR", dst="MACSTR"\n",
                MAC2STR(me), MAC2STR(peer));
    }
    return found;
}

struct dpp_instance *
find_instance_by_handle (dpp_handle handle)
{
    struct dpp_instance *found;
    
    TAILQ_FOREACH(found, &dpp_instances, entry) {
        if (found->handle == handle) {
            break;
        }
    }
    if (found == NULL) {
        fprintf(stderr, "unable to find dpp peer, handle = %d\n", handle);
    }
    return found;
}

struct dpp_instance *
find_instance_by_tid (unsigned char tid)
{
    struct dpp_instance *found;
    
    TAILQ_FOREACH(found, &dpp_instances, entry) {
        if (found->tid == tid) {
            break;
        }
    }
    if (found == NULL) {
        fprintf(stderr, "unable to find dpp peer, tid = %d\n", tid);
    }
    return found;
}

static struct dpp_instance *
create_dpp_instance (unsigned char *mymac, unsigned char *peermac, unsigned char *bskey,
                     int is_initiator, int mauth)
{
    struct dpp_instance *instance;
    
    if ((instance = (struct dpp_instance *)malloc(sizeof(struct dpp_instance))) == NULL) {
        return NULL;
    }
    memcpy(instance->mymac, mymac, ETH_ALEN);
    memcpy(instance->peermac, peermac, ETH_ALEN);
    if ((instance->handle = dpp_create_peer(bskey, is_initiator, mauth, WIRELESS_MTU)) < 1) {
        free(instance);
        return NULL;
    }
    TAILQ_INSERT_HEAD(&dpp_instances, instance, entry);
    
    return instance;
}

struct dpp_instance *
create_discovery_instance (unsigned char *mymac, unsigned char *peermac)
{
    struct dpp_instance *instance;

    /*
     * reuse an instance if the peer who spoke DPP Auth and Config also
     * does DPP Discovery
     */
    if ((instance = find_instance_by_mac(mymac, peermac)) == NULL) {
        if ((instance = (struct dpp_instance *)malloc(sizeof(struct dpp_instance))) == NULL) {
            return NULL;
        }
        memcpy(instance->mymac, mymac, ETH_ALEN);
        memcpy(instance->peermac, peermac, ETH_ALEN);
        TAILQ_INSERT_HEAD(&dpp_instances, instance, entry);
    }
    instance->tid = get_dpp_discovery_tid();
    
    return instance;
}

struct pkex_instance *
find_pkex_instance_by_mac (unsigned char *me)
{
    struct pkex_instance *found;
    
    TAILQ_FOREACH(found, &pkex_instances, entry) {
        if (memcmp(found->mymac, me, ETH_ALEN) == 0) {
            break;
        }
    }
    if (found == NULL) {
        fprintf(stderr, "unable to find pkex peer with " MACSTR "\n",
                MAC2STR(me));
    }
    return found;
}

struct pkex_instance *
find_pkex_instance_by_handle (pkex_handle handle)
{
    struct pkex_instance *found;
    
    TAILQ_FOREACH(found, &pkex_instances, entry) {
        if (found->handle == handle) {
            break;
        }
    }
    if (found == NULL) {
        fprintf(stderr, "unable to find pkex peer, handle = %d\n", handle);
    }
    return found;
}

static struct pkex_instance *
create_pkex_instance (unsigned char *mymac, unsigned char *peermac, int version) 
{
    struct pkex_instance *instance;
    
    if ((instance = (struct pkex_instance *)malloc(sizeof(struct pkex_instance))) == NULL) {
        return NULL;
    }
    memcpy(instance->mymac, mymac, ETH_ALEN);
    memcpy(instance->peermac, peermac, ETH_ALEN);
    /*
     * no version won't create pkex state, it will just create this
     * state in sss that can be used to bootstrap a peer on these MAC
     * addresses
     */
    if (version) {
        if ((instance->handle = pkex_create_peer(version)) < 1) {
            free(instance);
            return NULL;
        }
    } else {
        instance->handle = 0;
    }
    TAILQ_INSERT_HEAD(&pkex_instances, instance, entry);
    return instance;
}

static void
process_incoming_mgmt_frame (struct interface *inf, struct ieee80211_mgmt_frame *frame, int framesize)
{
    dpp_action_frame *dpp;
    struct dpp_instance *instance;
    struct pkex_instance *pinst;
    char el_id, el_len, ssid[33];
    unsigned char *els, pmk[PMK_LEN], pmkid[PMKID_LEN], pkey[200];
    unsigned char mac[20], keyasn1[1024], keyhash[SHA256_DIGEST_LENGTH];
    unsigned int mdlen = SHA256_DIGEST_LENGTH;
    unsigned short frame_control;
    int type, stype, left, ret, oc, chan, asn1len;
    unsigned char broadcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    TLV *rhash;
    FILE *fp;
    EVP_MD_CTX *mdctx;

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

    frame_control = ieee_order(frame->frame_control);
    type = IEEE802_11_FC_GET_TYPE(frame_control);
    stype = IEEE802_11_FC_GET_STYPE(frame_control);

    printf("processing %d byte incoming management frame\n", framesize);
    if (type == IEEE802_11_FC_TYPE_MGMT) {
        switch (stype) {
            case IEEE802_11_FC_STYPE_ACTION:
                /*
                 * if it's not a public action frame then we don't care about it!
                 */
                if (frame->action.category != ACTION_PUBLIC) {
                    fprintf(stderr, "got a non public action frame from " MACSTR " to " MACSTR "\n",
                            MAC2STR(frame->sa), MAC2STR(frame->da));
                    return;
                }
                left = framesize - (IEEE802_11_HDR_LEN + sizeof(frame->action));
                /*
                 * it's very unfortunate that 3 protocols use public action frames which
                 * require looking into the contents to demultiplex while 1 protocol uses
                 * gas frames whose field is used for demultiplexing...
                 */
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
                            case DPP_SUB_AUTH_RESPONSE:
                            case DPP_SUB_AUTH_CONFIRM:
                                if ((instance = find_instance_by_mac(inf->bssid, frame->sa)) == NULL) {
                                    fprintf(stderr, "no dpp instance at " MACSTR " from " MACSTR "\n",
                                            MAC2STR(inf->bssid), MAC2STR(frame->sa));
                                    return;
                                }
                                if (process_dpp_auth_frame(frame->action.variable, left, instance->handle) < 0) {
                                    fprintf(stderr, "error processing DPP Auth frame from " MACSTR "\n",
                                            MAC2STR(frame->sa));
                                }
                                break;
                                /*
                                 * DPP Discovery
                                 */
                            case DPP_SUB_PEER_DISCOVER_REQ:
                                if ((instance = create_discovery_instance(inf->bssid, frame->sa)) == NULL) {
                                    break;
                                }
                                if (process_dpp_discovery_frame(frame->action.variable, left, instance->tid,
                                                                pmk, pmkid) < 0) {
                                    fprintf(stderr, "error processing DPP Discovery frame from " MACSTR "\n",
                                            MAC2STR(frame->sa));
                                }
                                break;
                            case DPP_SUB_PEER_DISCOVER_RESP:
                                if ((instance = find_instance_by_mac(inf->bssid, frame->sa)) == NULL) {
                                    fprintf(stderr, "no dpp instance at " MACSTR " from " MACSTR "\n",
                                            MAC2STR(inf->bssid), MAC2STR(frame->sa));
                                    return;
                                }
                                if (process_dpp_discovery_frame(frame->action.variable, left, instance->tid,
                                                                pmk, pmkid) < 0) {
                                    fprintf(stderr, "error processing DPP Discovery frame from " MACSTR "\n",
                                            MAC2STR(frame->sa));
                                }
                                break;
                                /*
                                 * PKEX
                                 */
                            case PKEX_SUB_EXCH_V1REQ:
                            case PKEX_SUB_EXCH_REQ:
                            case PKEX_SUB_EXCH_RESP:
                            case PKEX_SUB_COM_REV_REQ:
                            case PKEX_SUB_COM_REV_RESP:
                                if ((pinst = find_pkex_instance_by_mac(inf->bssid)) == NULL) {
                                    if (dpp->frame_type == PKEX_SUB_EXCH_V1REQ) {
                                        pinst = create_pkex_instance(inf->bssid, frame->sa, 1);
                                        pkex_update_macs(pinst->handle, inf->bssid, frame->sa);
                                    } else if (dpp->frame_type == PKEX_SUB_EXCH_REQ) {
                                        pinst = create_pkex_instance(inf->bssid, frame->sa, DPP_VERSION);
                                    } else {
                                        fprintf(stderr, "cannot find pkex instance from " MACSTR "\n",
                                                MAC2STR(frame->sa));
                                        break;
                                    }
                                }
                                if (memcmp(pinst->peermac, broadcast, ETH_ALEN) == 0) {
                                    /*
                                     * if we started out broadcasting, we now got a response so
                                     * update MACs
                                     */
                                    memcpy(pinst->peermac, frame->sa, ETH_ALEN);
                                    pkex_update_macs(pinst->handle, inf->bssid, frame->sa);
                                }
                                fprintf(stderr, "received PKEX frame from " MACSTR " to " MACSTR "\n",
                                        MAC2STR(inf->bssid), MAC2STR(frame->sa));
                                if (process_pkex_frame(frame->action.variable, left, pinst->handle) < 0) {
                                    fprintf(stderr, "error processing PKEX frame from " MACSTR "\n",
                                            MAC2STR(frame->sa));
                                }
                                break;
                            case DPP_CONFIG_RESULT:
                                if ((instance = find_instance_by_mac(inf->bssid, frame->sa)) == NULL) {
                                    fprintf(stderr, "no dpp instance at " MACSTR " from " MACSTR "\n",
                                            MAC2STR(inf->bssid), MAC2STR(frame->sa));
                                    return;
                                }
                                if (process_dpp_config_frame(BAD_DPP_SPEC_MESSAGE, frame->action.variable, left,
                                                             instance->handle) < 0) {
                                    fprintf(stderr, "error processing DPP Config frame from " MACSTR "\n",
                                            MAC2STR(frame->sa));
                                }
                                break;
                            case DPP_CHIRP:
                                /*
                                 * someone is out there, someone I know about?
                                 */
                                fprintf(stderr, "got a DPP chirp from " MACSTR "\n", MAC2STR(frame->sa));
                                if ((rhash = find_tlv(RESPONDER_BOOT_HASH, dpp->attributes, left)) == NULL) {
                                    fprintf(stderr, "malformed though\n");
                                    return;
                                }
                                if ((fp = fopen(bootstrapfile, "r")) == NULL) {
                                    return;
                                }
                                if ((mdctx = EVP_MD_CTX_new()) == NULL) {
                                    fclose(fp);
                                    return;
                                }
                                while (!feof(fp)) {
                                    memset(pkey, 0, sizeof(pkey));
                                    if (fscanf(fp, "%d %d %d %s %s", &ret, &oc, &chan, mac, pkey) < 0) {
                                        continue;
                                    }
                                    if ((asn1len = EVP_DecodeBlock(keyasn1, pkey, strlen((char *)pkey))) < 0) {
                                        continue;
                                    }
                                    fprintf(stderr, "checking %d...", ret);
                                    EVP_DigestInit(mdctx, EVP_sha256());
                                    EVP_DigestUpdate(mdctx, "chirp", strlen("chirp"));
                                    EVP_DigestUpdate(mdctx, keyasn1, asn1len - 1);
                                    EVP_DigestFinal(mdctx, keyhash, &mdlen);
                                    /*
                                     * if we have that bootstrap key, initiate to her!
                                     */
                                    if (memcmp(keyhash, TLV_value(rhash), SHA256_DIGEST_LENGTH) == 0) {
                                        fprintf(stderr, "YES!!!\n");
                                        create_dpp_instance(inf->bssid, frame->sa, pkey, 1, 0);
                                        break;
                                    } else {
                                        fprintf(stderr, "no\n");
                                    }
                                }
                                fclose(fp);
                                break;
                            default:
                                fprintf(stderr, "unknown DPP frame %d\n", dpp->frame_type);
                                break;
                        }
                        break;
                    case GAS_INITIAL_REQUEST:
                    case GAS_INITIAL_RESPONSE:
                    case GAS_COMEBACK_REQUEST:
                    case GAS_COMEBACK_RESPONSE:
                        /*
                         * DPP Configuration protocol
                         */
                        if ((instance = find_instance_by_mac(inf->bssid, frame->sa)) == NULL) {
                            fprintf(stderr, "no dpp instance at " MACSTR " from " MACSTR "\n",
                                    MAC2STR(inf->bssid), MAC2STR(frame->sa));
                            return;
                        }
                        if (process_dpp_config_frame(frame->action.field, frame->action.variable, left,
                                                     instance->handle) < 0) {
                            fprintf(stderr, "error processing DPP Config frame from " MACSTR "\n",
                                    MAC2STR(frame->sa));
                        }
                        break;
                    default:
                        fprintf(stderr, "weird non-DPP action frame from " MACSTR " to " MACSTR "\n",
                                MAC2STR(frame->sa), MAC2STR(frame->da));
                        break;
                }
                break;
            case IEEE802_11_FC_STYPE_BEACON:
                if ((instance = find_instance_by_mac(inf->bssid, frame->sa)) == NULL) {
                    return;
                }
                if (discovered != 0) {
                    /*
                     * not yet provisioned or already discovered
                     */
                    return;
                }
                dump_ssid(frame, framesize);
                els = frame->beacon.variable;
                left = framesize - (IEEE802_11_HDR_LEN + sizeof(frame->beacon));
                /*
                 * els is the next IE in the beacon,
                 * left is how much is left to read in the beacon
                 */
                while (left > 2) {
                    el_id = *els++; 
                    left--;
                    el_len = *els++; 
                    left--;
                    if (el_len > left) {
                        /*
                         * someone's trying to mess with us...
                         */
                        break;
                    }
                    if (el_id == IEEE802_11_IE_SSID) { 
                        if (el_len > 32) {
                            /*
                             * again with the messing...
                             */
                            break;
                        }
                        memset(ssid, 0, sizeof(ssid));
                        memcpy(ssid, els, el_len);
                        /*
                         * if it's not an interesting ssid then ignore the beacon
                         * otherwise if it's our's, discover the AP
                         */
                        if ((el_len == 0) || memcmp(ssid, our_ssid, strlen(ssid))) {
                            break;
                        }
                        if ((instance = create_discovery_instance(inf->bssid, frame->sa)) == NULL) {
                            break;
                        }
                        if (dpp_begin_discovery(instance->tid) > 0) {
                            discovered = 1;
                        }
                        break;
                    }
                    els += el_len;
                    left -= el_len;
                }
                break;
                
        }
    }
}

/*
 * stolen from wpa_supplicant, thanks Jouni!
 */
static const char * nl80211_command_to_string(enum nl80211_commands cmd)
{
#define C2S(x) case x: return #x;
	switch (cmd) {
	C2S(NL80211_CMD_UNSPEC)
	C2S(NL80211_CMD_GET_WIPHY)
	C2S(NL80211_CMD_SET_WIPHY)
	C2S(NL80211_CMD_NEW_WIPHY)
	C2S(NL80211_CMD_DEL_WIPHY)
	C2S(NL80211_CMD_GET_INTERFACE)
	C2S(NL80211_CMD_SET_INTERFACE)
	C2S(NL80211_CMD_NEW_INTERFACE)
	C2S(NL80211_CMD_DEL_INTERFACE)
	C2S(NL80211_CMD_GET_KEY)
	C2S(NL80211_CMD_SET_KEY)
	C2S(NL80211_CMD_NEW_KEY)
	C2S(NL80211_CMD_DEL_KEY)
	C2S(NL80211_CMD_GET_BEACON)
	C2S(NL80211_CMD_SET_BEACON)
	C2S(NL80211_CMD_START_AP)
	C2S(NL80211_CMD_STOP_AP)
	C2S(NL80211_CMD_GET_STATION)
	C2S(NL80211_CMD_SET_STATION)
	C2S(NL80211_CMD_NEW_STATION)
	C2S(NL80211_CMD_DEL_STATION)
	C2S(NL80211_CMD_GET_MPATH)
	C2S(NL80211_CMD_SET_MPATH)
	C2S(NL80211_CMD_NEW_MPATH)
	C2S(NL80211_CMD_DEL_MPATH)
	C2S(NL80211_CMD_SET_BSS)
	C2S(NL80211_CMD_SET_REG)
	C2S(NL80211_CMD_REQ_SET_REG)
	C2S(NL80211_CMD_GET_MESH_CONFIG)
	C2S(NL80211_CMD_SET_MESH_CONFIG)
	C2S(NL80211_CMD_SET_MGMT_EXTRA_IE)
	C2S(NL80211_CMD_GET_REG)
	C2S(NL80211_CMD_GET_SCAN)
	C2S(NL80211_CMD_TRIGGER_SCAN)
	C2S(NL80211_CMD_NEW_SCAN_RESULTS)
	C2S(NL80211_CMD_SCAN_ABORTED)
	C2S(NL80211_CMD_REG_CHANGE)
	C2S(NL80211_CMD_AUTHENTICATE)
	C2S(NL80211_CMD_ASSOCIATE)
	C2S(NL80211_CMD_DEAUTHENTICATE)
	C2S(NL80211_CMD_DISASSOCIATE)
	C2S(NL80211_CMD_MICHAEL_MIC_FAILURE)
	C2S(NL80211_CMD_REG_BEACON_HINT)
	C2S(NL80211_CMD_JOIN_IBSS)
	C2S(NL80211_CMD_LEAVE_IBSS)
	C2S(NL80211_CMD_TESTMODE)
	C2S(NL80211_CMD_CONNECT)
	C2S(NL80211_CMD_ROAM)
	C2S(NL80211_CMD_DISCONNECT)
	C2S(NL80211_CMD_SET_WIPHY_NETNS)
	C2S(NL80211_CMD_GET_SURVEY)
	C2S(NL80211_CMD_NEW_SURVEY_RESULTS)
	C2S(NL80211_CMD_SET_PMKSA)
	C2S(NL80211_CMD_DEL_PMKSA)
	C2S(NL80211_CMD_FLUSH_PMKSA)
	C2S(NL80211_CMD_REMAIN_ON_CHANNEL)
	C2S(NL80211_CMD_CANCEL_REMAIN_ON_CHANNEL)
	C2S(NL80211_CMD_SET_TX_BITRATE_MASK)
	C2S(NL80211_CMD_REGISTER_FRAME)
	C2S(NL80211_CMD_FRAME)
	C2S(NL80211_CMD_FRAME_TX_STATUS)
	C2S(NL80211_CMD_SET_POWER_SAVE)
	C2S(NL80211_CMD_GET_POWER_SAVE)
	C2S(NL80211_CMD_SET_CQM)
	C2S(NL80211_CMD_NOTIFY_CQM)
	C2S(NL80211_CMD_SET_CHANNEL)
	C2S(NL80211_CMD_SET_WDS_PEER)
	C2S(NL80211_CMD_FRAME_WAIT_CANCEL)
	C2S(NL80211_CMD_JOIN_MESH)
	C2S(NL80211_CMD_LEAVE_MESH)
	C2S(NL80211_CMD_UNPROT_DEAUTHENTICATE)
	C2S(NL80211_CMD_UNPROT_DISASSOCIATE)
	C2S(NL80211_CMD_NEW_PEER_CANDIDATE)
	C2S(NL80211_CMD_GET_WOWLAN)
	C2S(NL80211_CMD_SET_WOWLAN)
	C2S(NL80211_CMD_START_SCHED_SCAN)
	C2S(NL80211_CMD_STOP_SCHED_SCAN)
	C2S(NL80211_CMD_SCHED_SCAN_RESULTS)
	C2S(NL80211_CMD_SCHED_SCAN_STOPPED)
	C2S(NL80211_CMD_SET_REKEY_OFFLOAD)
	C2S(NL80211_CMD_PMKSA_CANDIDATE)
	C2S(NL80211_CMD_TDLS_OPER)
	C2S(NL80211_CMD_TDLS_MGMT)
	C2S(NL80211_CMD_UNEXPECTED_FRAME)
	C2S(NL80211_CMD_PROBE_CLIENT)
	C2S(NL80211_CMD_REGISTER_BEACONS)
	C2S(NL80211_CMD_UNEXPECTED_4ADDR_FRAME)
	C2S(NL80211_CMD_SET_NOACK_MAP)
	C2S(NL80211_CMD_CH_SWITCH_NOTIFY)
	C2S(NL80211_CMD_START_P2P_DEVICE)
	C2S(NL80211_CMD_STOP_P2P_DEVICE)
	C2S(NL80211_CMD_CONN_FAILED)
	C2S(NL80211_CMD_SET_MCAST_RATE)
	C2S(NL80211_CMD_SET_MAC_ACL)
	C2S(NL80211_CMD_RADAR_DETECT)
	C2S(NL80211_CMD_GET_PROTOCOL_FEATURES)
	C2S(NL80211_CMD_UPDATE_FT_IES)
	C2S(NL80211_CMD_FT_EVENT)
	C2S(NL80211_CMD_CRIT_PROTOCOL_START)
	C2S(NL80211_CMD_CRIT_PROTOCOL_STOP)
	C2S(NL80211_CMD_GET_COALESCE)
	C2S(NL80211_CMD_SET_COALESCE)
	C2S(NL80211_CMD_CHANNEL_SWITCH)
	C2S(NL80211_CMD_VENDOR)
	C2S(NL80211_CMD_SET_QOS_MAP)
	C2S(NL80211_CMD_ADD_TX_TS)
	C2S(NL80211_CMD_DEL_TX_TS)
	C2S(NL80211_CMD_WIPHY_REG_CHANGE)
	C2S(NL80211_CMD_PORT_AUTHORIZED)
	C2S(NL80211_CMD_EXTERNAL_AUTH)
	C2S(NL80211_CMD_STA_OPMODE_CHANGED)
	C2S(NL80211_CMD_CONTROL_PORT_FRAME)
	C2S(NL80211_CMD_UPDATE_OWE_INFO)
	default:
		return "NL80211_CMD_UNKNOWN";
	}
#undef C2S
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
#if 0
    if ((gnlh->cmd != NL80211_CMD_FRAME) &&
        (gnlh->cmd != NL80211_CMD_FRAME_TX_STATUS) &&
        (gnlh->cmd != NL80211_CMD_FRAME_WAIT_CANCEL)) {
        fprintf(stderr, "mgmt_frame_in: got %s (%d)\n",
                nl80211_command_to_string(gnlh->cmd), gnlh->cmd);
        return NL_SKIP;
    }
    if (tb[NL80211_ATTR_FRAME]) {
        frame = (struct ieee80211_mgmt_frame *)nla_data(tb[NL80211_ATTR_FRAME]);
        framesize = nla_len(tb[NL80211_ATTR_FRAME]);
        process_incoming_mgmt_frame(inf, frame, framesize);
    } else {
        fprintf(stderr, "mgmt_frame_in() got something that's not a frame with %s\n",
                nl80211_command_to_string(gnlh->cmd));
    }
#endif
    switch (gnlh->cmd) {
        case NL80211_CMD_FRAME:
        case NL80211_CMD_FRAME_TX_STATUS:
            frame = (struct ieee80211_mgmt_frame *)nla_data(tb[NL80211_ATTR_FRAME]);
            framesize = nla_len(tb[NL80211_ATTR_FRAME]);
            process_incoming_mgmt_frame(inf, frame, framesize);
            break;
        case NL80211_CMD_FRAME_WAIT_CANCEL:
//            fprintf(stderr, "mgmt_frame_in() got a wait cancel!\n");
            break;
        default:
            fprintf(stderr, "mgmt_frame_in() got a %s\n",
                    nl80211_command_to_string(gnlh->cmd));
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
    unsigned char buf[WIRELESS_MTU];
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
ack_handler (struct nl_msg *msg, void *arg)
{
    int *ret = arg;
    
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

static int
cookie_handler(struct nl_msg *msg, void *arg)
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

    nl_socket_disable_seq_check(inf->nl_sock);

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
cons_action_frame (unsigned char field, unsigned char *mymac, unsigned char *peermac,
                   char *data, int len)
{
    char buf[WIRELESS_MTU];
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
        if (field == PUB_ACTION_VENDOR) {
            nla_put_u32(msg, NL80211_ATTR_DURATION, 500);
        } else {
            nla_put_u32(msg, NL80211_ATTR_DURATION, inf->max_roc);
        }
        if (inf->offchan_tx_ok) {
            nla_put_flag(msg, NL80211_ATTR_OFFCHANNEL_TX_OK);
        }
        nla_put(msg, NL80211_ATTR_FRAME, framesize, buf);
        printf("sending %ld byte frame on %ld\n", framesize, inf->freq);
        cookie = 0;
        if (send_nl_msg(msg, inf, cookie_handler, &cookie) < 0) {
            fprintf(stderr, "can't send nl msg!\n");
            return -1;
        }
    }
    return len;
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

    if ((msg = get_nl_msg(inf, 0, NL80211_CMD_SET_CHANNEL)) == NULL) {
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

int
change_dpp_freq (dpp_handle handle, unsigned long freq) 
{
    struct dpp_instance *instance;

    if ((instance = find_instance_by_handle(handle)) == NULL) {
        return -1;
    }
    return change_freq(instance->mymac, freq);
}

int
change_dpp_channel (dpp_handle handle, unsigned char class, unsigned char channel)
{
    struct dpp_instance *instance;
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

    if ((instance = find_instance_by_handle(handle)) == NULL) {
        return -1;
    }
    freak = chan2freq(channel);
    printf("trying to change to channel %d (%ld)\n", channel, freak);
    return change_freq(instance->mymac, freak);
}

int
transmit_config_frame (dpp_handle handle, unsigned char field, char *data, int len)
{
    struct dpp_instance *instance;

    if ((instance = find_instance_by_handle(handle)) == NULL) {
        return -1;
    }
//    nl_debug = 4;
    return cons_action_frame(field, instance->mymac, instance->peermac, data, len);
}

int
transmit_auth_frame (dpp_handle handle, char *data, int len)
{
    struct dpp_instance *instance;

    if ((instance = find_instance_by_handle(handle)) == NULL) {
        return -1;
    }
    return cons_action_frame(PUB_ACTION_VENDOR, instance->mymac, instance->peermac, data, len);
}

int
transmit_discovery_frame (unsigned char tid, char *data, int len)
{
    struct dpp_instance *instance;

    if ((instance = find_instance_by_tid(tid)) == NULL) {
        return -1;
    }
    return cons_action_frame(PUB_ACTION_VENDOR, instance->mymac, instance->peermac, data, len);
}

int
transmit_pkex_frame (pkex_handle handle, char *data, int len)
{
    struct pkex_instance *instance;

    if ((instance = find_pkex_instance_by_handle(handle)) == NULL) {
        return -1;
    }
    return cons_action_frame(PUB_ACTION_VENDOR, instance->mymac, instance->peermac, data, len);
}

static int
register_action_frame (struct interface *inf, int flags, unsigned char *match, int match_len)
{
    struct nl_msg *msg;
    unsigned short type = (IEEE802_11_FC_TYPE_MGMT << 2 | IEEE802_11_FC_STYPE_ACTION << 4);

    if ((msg = get_nl_msg(inf, flags, NL80211_CMD_REGISTER_ACTION)) == NULL) {
        fprintf(stderr, "can't allocate an nl_msg!\n");
        return -1;
    }
    nla_put_u16(msg, NL80211_ATTR_FRAME_TYPE, type);
    nla_put(msg, NL80211_ATTR_FRAME_MATCH, match_len, match);
    if (send_nl_msg(msg, inf, NULL, NULL)) {
        fprintf(stderr, "unable to register for action frame!\n");
        return -1;
    }
    
    return 1;
}

/*
 * send_beacon()
 *      beacons are normally sent out automagically by the radio but if we're
 *      simulating this protocol over the loopback we need to send them here.
 */
static void
send_beacon (timerid tid, void *data)
{
    struct interface *inf = (struct interface *)data;
    struct ieee80211_mgmt_frame *frame;
    unsigned char buf[2048], *el;
    unsigned char broadcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    int len, blen;

    if (inf == NULL) {
        return;
    }
    memset(buf, 0, sizeof(buf));
    /*
     * make a pseudo-beacon
     */
    frame = (struct ieee80211_mgmt_frame *)buf;
    frame->frame_control = ieee_order((IEEE802_11_FC_TYPE_MGMT << 2 | IEEE802_11_FC_STYPE_BEACON << 4));
    memcpy(frame->da, broadcast, ETH_ALEN);
    memcpy(frame->sa, inf->bssid, ETH_ALEN);
    memcpy(frame->bssid, inf->bssid, ETH_ALEN);

    /*
     * not a truely valid beacon but so what, this is a simulator and
     * all we really care about is the ssid
     */
    el = frame->beacon.variable;
    *el = IEEE802_11_IE_SSID;
    el++;
    *el = strlen(our_ssid);
    el++;
    memcpy(el, our_ssid, strlen(our_ssid));
    el += strlen(our_ssid);

    len = el - buf;
    blen = write(inf->fd, buf, len);
    if (blen < 0) {
        perror("write");
    }
    srv_add_timeout(srvctx, SRV_MSEC(100), send_beacon, inf);
    return;
}
static struct nl_sock *
create_nl_socket (struct nl_cb *cb)
{
    struct nl_sock *sock;
    
    if ((sock = nl_socket_alloc_cb(cb)) == NULL) {
        fprintf(stderr, "unable to alloc an nl socket\n");
        return NULL;
    }
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
scan_callback_trigger (struct nl_msg *msg, void *arg) 
{
    struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
    struct trigger_results *results = arg;

    switch (gnlh->cmd) {
        case NL80211_CMD_SCAN_ABORTED:
            results->done = 1;
            results->aborted = 1;
            break;
        case NL80211_CMD_NEW_SCAN_RESULTS:
            results->done = 1;
            results->aborted = 0;
            break;
        default:
//            printf("weird multicast message\n");
            break;
    }

    return NL_SKIP;
}

int
find_dpp_conie (unsigned char *ie, int ielen) {
    uint8_t len;
    uint8_t *data;
    char blah[33];
    const uint8_t dpp_config_conn[6] = {
        0xdd, 0x04, 0x50, 0x6f, 0x9a, 0x1e
    };

    memset(blah, 0, 33);
    while (ielen >= 2 && ielen >= ie[1]) {
        if (ie[0] == 0 && ie[1] >= 0 && ie[1] <= 32) {
            len = ie[1];
            data = ie + 2;
            memcpy(blah, data, len);
        }
        if (memcmp(ie, dpp_config_conn, sizeof(dpp_config_conn)) == 0) {
            printf("FOUND THE DPP CONFIGURATOR CONNECTIVITY IE on %s, ", blah);
            return 1;
        }
        ielen -= ie[1] + 2;
        ie += ie[1] + 2;
    }
    printf("didn't find the DPP Configurator connectivity IE on %s\n", blah);
    return 0;
}

static unsigned char
freq2chan (unsigned long freq)
{
    if (freq == 2484) {                 /* chan 14 */
        return 14;
    }
    if (freq < 2473) {                  /* chan 0-13 */
        return ((freq - 2407)/5);
    }
    if (freq < 2733) {                  /* chan 15-26 */
        return (((freq - 2512)/20) + 15);
    }
    return ((freq - 5000)/5);
}

static int
callback_dump(struct nl_msg *msg, void *arg)
{
    struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
    struct nlattr *tb[NL80211_ATTR_MAX + 1];
    struct nlattr *bss[NL80211_BSS_MAX + 1];
    static struct nla_policy bss_policy[NL80211_BSS_MAX + 1] = {
        [NL80211_BSS_TSF] = { .type = NLA_U64 },
        [NL80211_BSS_FREQUENCY] = { .type = NLA_U32 },
        [NL80211_BSS_BSSID] = { },
        [NL80211_BSS_BEACON_INTERVAL] = { .type = NLA_U16 },
        [NL80211_BSS_CAPABILITY] = { .type = NLA_U16 },
        [NL80211_BSS_INFORMATION_ELEMENTS] = { },
        [NL80211_BSS_SIGNAL_MBM] = { .type = NLA_U32 },
        [NL80211_BSS_SIGNAL_UNSPEC] = { .type = NLA_U8 },
        [NL80211_BSS_STATUS] = { .type = NLA_U32 },
        [NL80211_BSS_SEEN_MS_AGO] = { .type = NLA_U32 },
        [NL80211_BSS_BEACON_IES] = { },
    };
    struct interface *inf = (struct interface *)arg;
    uint32_t freq;

    nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), NULL);
    if (!tb[NL80211_ATTR_BSS]) {
        printf("bss info missing!\n");
        return NL_SKIP;
    }
    if (nla_parse_nested(bss, NL80211_BSS_MAX, tb[NL80211_ATTR_BSS], bss_policy)) {
        printf("failed to parse nested attributes!\n");
        return NL_SKIP;
    }
    if (!bss[NL80211_BSS_BSSID]) return NL_SKIP;
    if (!bss[NL80211_BSS_INFORMATION_ELEMENTS]) return NL_SKIP;
    /*
     * if an AP is beaconing out the DPP Configurator Connectivity IE
     * then add its frequency to the chirp list
     */
    if (find_dpp_conie(nla_data(bss[NL80211_BSS_INFORMATION_ELEMENTS]),
                       nla_len(bss[NL80211_BSS_INFORMATION_ELEMENTS])) > 0) {
        freq = nla_get_u32(bss[NL80211_BSS_FREQUENCY]);
        printf("on frequency %d, channel %d\n", freq, freq2chan(freq));
        dpp_add_chirp_freq(inf->bssid, freq);
    }
    
    return NL_SKIP;
}

int
trigger_scan (struct interface *inf, char *lookfor)
{
    struct trigger_results results = { .done = 0, .aborted = 0 };
    struct nl_msg *msg;
    struct nl_cb *cb;
    int err = 0;
    
    if ((msg = get_nl_msg(inf, 0, NL80211_CMD_TRIGGER_SCAN)) == NULL) {
        fprintf(stderr, "can't create nlmsg to trigger a scan!\n");
        return -1;
    }
    if (lookfor != NULL) {
        struct nlattr *ssids;
        
        printf("scanning for %s\n", lookfor);
        ssids = nla_nest_start(msg, NL80211_ATTR_SCAN_SSIDS);
        nla_put(msg, 1, strlen(lookfor), lookfor);
        nla_nest_end(msg, ssids);
    } else {
        printf("scanning for all SSIDs\n");
    }

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
    nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, scan_callback_trigger, &results);

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
    while (!results.done) {
        nl_recvmsgs(inf->nl_sock, cb);
    }
    if (results.aborted) {
        fprintf(stderr, "kernel aborted our scan :-(\n");
    } else {
        printf("scan finished.\n");
    }
    
    nl_cb_put(cb);
    nlmsg_free(msg);

    return 0;
}

int
find_dpp_ap (unsigned char *ie, int ielen, char *ssid) {
    uint8_t len;
    uint8_t *data;

    while (ielen >= 2 && ielen >= ie[1]) {
        if (ie[0] == 0 && ie[1] >= 0 && ie[1] <= 32) {
            len = ie[1];
            data = ie + 2;
            if ((strlen(ssid) == len) && (memcmp(ssid, data, len) == 0)) {
                printf("found %s!\n", ssid);
                return 1;
            }
        }
        ielen -= ie[1] + 2;
        ie += ie[1] + 2;
    }
    return 0;
}

int
find_dpp_access_point (struct nl_msg *msg, void *arg)
{
    struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
    struct nlattr *tb[NL80211_ATTR_MAX + 1];
    struct nlattr *bss[NL80211_BSS_MAX + 1];
    static struct nla_policy bss_policy[NL80211_BSS_MAX + 1] = {
        [NL80211_BSS_TSF] = { .type = NLA_U64 },
        [NL80211_BSS_FREQUENCY] = { .type = NLA_U32 },
        [NL80211_BSS_BSSID] = { },
        [NL80211_BSS_BEACON_INTERVAL] = { .type = NLA_U16 },
        [NL80211_BSS_CAPABILITY] = { .type = NLA_U16 },
        [NL80211_BSS_INFORMATION_ELEMENTS] = { },
        [NL80211_BSS_SIGNAL_MBM] = { .type = NLA_U32 },
        [NL80211_BSS_SIGNAL_UNSPEC] = { .type = NLA_U8 },
        [NL80211_BSS_STATUS] = { .type = NLA_U32 },
        [NL80211_BSS_SEEN_MS_AGO] = { .type = NLA_U32 },
        [NL80211_BSS_BEACON_IES] = { },
    };
    struct interface *inf = (struct interface *)arg;
    struct dpp_instance *instance;

    nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), NULL);
    if (!tb[NL80211_ATTR_BSS]) {
        printf("bss info missing!\n");
        return NL_SKIP;
    }
    if (nla_parse_nested(bss, NL80211_BSS_MAX, tb[NL80211_ATTR_BSS], bss_policy)) {
        printf("failed to parse nested attributes!\n");
        return NL_SKIP;
    }
    if (!bss[NL80211_BSS_BSSID]) return NL_SKIP;
    if (!bss[NL80211_BSS_INFORMATION_ELEMENTS]) return NL_SKIP;

    /*
     * find a DPP AP beaconing out this SSID
     */
    if (find_dpp_ap(nla_data(bss[NL80211_BSS_INFORMATION_ELEMENTS]),
                    nla_len(bss[NL80211_BSS_INFORMATION_ELEMENTS]), our_ssid) > 0) {
        /*
         * create a new instance since the DPP AP might not be the peer
         * to whom we spoke DPP Auth and provisioning
         */
        if ((instance = create_discovery_instance(inf->bssid, nla_data(bss[NL80211_BSS_BSSID]))) != NULL) {
            if (dpp_begin_discovery(instance->tid) > 0) {
                discovered = 1;
            }
        }
    }
    
    return NL_SKIP;
}

static void
scan_for_ssid (timerid id, void *data)
{
    struct interface *inf = (struct interface *)data;
    struct nl_msg *msg;

    if (inf == NULL) {
        fprintf(stderr, "bad data on callback-- no interface! Can't scan\n");
        return;
    }
    if (trigger_scan(inf, our_ssid) == 0) {
        msg = get_nl_msg(inf, NLM_F_DUMP, NL80211_CMD_GET_SCAN);
        if (send_nl_msg(msg, inf, find_dpp_access_point, inf)) {
            fprintf(stderr, "unable to register for action frame!\n");
        }
    }
    return;
}    

int
provision_connector (char *role, unsigned char *ssid, int ssidlen,
                     unsigned char *connector, int connlen, dpp_handle handle)
{
    struct interface *inf = NULL;
    struct dpp_instance *instance;
    
    if ((instance = find_instance_by_handle(handle)) == NULL) {
        fprintf(stderr, "no DPP instance to provision a connector for!\n");
        return -1;
    }
    
    printf("connector:\n%.*s\nwith ", connlen, connector);
    if (ssidlen == 1 && ssid[0] == '*') {
        printf("any SSID\n");
    } else {
        printf("SSID %.*s\n", ssidlen, ssid);
    }

    memset(our_ssid, 0, 33);
    memcpy(our_ssid, ssid, ssidlen);
    if (strncmp(role, "ap", 2) == 0) {
        TAILQ_FOREACH(inf, &interfaces, entry) {
            if (memcmp(instance->mymac, inf->bssid, ETH_ALEN) == 0) {
                break;
            }
        }
        if (inf == NULL) {
            fprintf(stderr, "can't find " MACSTR " to send mgmt frame!\n",
                    MAC2STR(instance->mymac));
            return -1;
        }
        discovered = 1;

        if (inf->is_loopback) {
            srv_add_timeout(srvctx, SRV_MSEC(1), send_beacon, inf);
        }
    } else if (strncmp(role, "sta", 3) == 0) {
        TAILQ_FOREACH(inf, &interfaces, entry) {
            if (memcmp(instance->mymac, inf->bssid, ETH_ALEN) == 0) {
                break;
            }
        }
        if (inf == NULL) {
            fprintf(stderr, "can't find " MACSTR " to send mgmt frame!\n",
                    MAC2STR(instance->mymac));
            return -1;
        }
        if (!inf->is_loopback) {
            /*
             * let DPP finish before we go off and start scanning for SSIDs!
             */
            srv_add_timeout(srvctx, SRV_SEC(1), scan_for_ssid, inf);
        }
        discovered = 0;
    } else {
        fprintf(stderr, "don't know what kind of device we are to do discovery: %s!\n", role);
    }
    return 1;
}

int
save_bootstrap_key (pkex_handle handle, void *param)
{
    EC_KEY *peerbskey = (EC_KEY *)param;
    BIO *bio = NULL;
    FILE *fp = NULL;
    char newone[1024], existing[1024], b64bskey[1024];
    unsigned char *ptr, mac[2*ETH_ALEN];
    int ret = -1, len, octets, opc, chan;
    struct pkex_instance *instance;
    
    if ((instance = find_pkex_instance_by_handle(handle)) == NULL) {
        goto fin;
    }
    /*
     * get the base64 encoded EC_KEY as onerow[1]
     */
    if ((bio = BIO_new(BIO_s_mem())) == NULL) {
        fprintf(stderr, "unable to create bio!\n");
        goto fin;
    }
    (void)i2d_EC_PUBKEY_bio(bio, peerbskey);
    (void)BIO_flush(bio);
    len = BIO_get_mem_data(bio, &ptr);
    octets = EVP_EncodeBlock((unsigned char *)newone, ptr, len);
    BIO_free(bio);

    memset(b64bskey, 0, 1024);
    strncpy(b64bskey, newone, octets);

    if ((fp = fopen(bootstrapfile, "r+")) == NULL) {
        fprintf(stderr, "unable to open %s as bootstrapping file\n", bootstrapfile);
        goto fin;
    }
    ret = 0;
    while (!feof(fp)) {
        if (fscanf(fp, "%d %d %d %s %s", &ret, &opc, &chan, mac, existing) < 0) {
            continue;
        }
// TODO: stop appending everything!
        if (strcmp(existing, newone) == 0) {
            fprintf(stderr, "bootstrapping key is trusted already\n");
        }
    }
    ret++;
    /*
     * bootstrapping file is index opclass channel macaddr key
     */
    printf("it'll be %d, and it's %d long and it's %s\n", ret, octets, b64bskey);
    fprintf(fp, "%d %d %d %02x%02x%02x%02x%02x%02x %s\n", ret, opclass, channel,
            instance->peermac[0], instance->peermac[1], instance->peermac[2], 
            instance->peermac[3], instance->peermac[4], instance->peermac[5], 
            b64bskey);
  fin:
    if (fp != NULL) {
        fclose(fp);
    }
    return ret;
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
    printf("max ROC is %ld\n", inf->max_roc);
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
        free(inf);
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

/*
 * takes one of our mac addresses and an index into the bootstrapping key file.
 * Initiates DPP to peer whose DPP URI is at "keyidx".
 */
int
bootstrap_peer (pkex_handle handle, int keyidx, int is_initiator, int mauth)
{
    struct pkex_instance *instance;
    FILE *fp;
    int n, opclass, channel, ret = -1;
    unsigned char keyb64[1024], peermac[ETH_ALEN];
    char mac[20], *ptr;

    if ((instance = find_pkex_instance_by_handle(handle)) == NULL) {
        fprintf(stderr, "cannot find PKEX instance with handle %x\n", handle);
        return ret;
    }
    /*
     * if this instance created pkex state, clean it up now
     */
    if (instance->handle) {
        pkex_destroy_peer(instance->handle);
    }

    /*
     * TODO: delete PKEX instance and clean up state inside pkex.c
     */
    printf("looking for bootstrap key index %d\n", keyidx);
    if ((fp = fopen(bootstrapfile, "r")) == NULL) {
        goto fin;
    }
    while (!feof(fp)) {
        memset(keyb64, 0, sizeof(keyb64));
        if (fscanf(fp, "%d %d %d %s %s", &n, &opclass, &channel, mac, keyb64) < 1) {
            fclose(fp);
            goto fin;
        }
        if (n == keyidx) {
            break;
        }
    }
    if (feof(fp)) {
        fprintf(stderr, "unable to find bootstrap key with index %d\n", keyidx);
        fclose(fp);
        goto fin;
    }
    fclose(fp);
    printf("peer is on operating class %d and channel %d, checking...\n", opclass, channel);
    printf("peer's bootstrapping key is %s\n", keyb64);

    ptr = &mac[10];
    sscanf(ptr, "%hhx", &peermac[5]); mac[10] = '\0';
    ptr = &mac[8];
    sscanf(ptr, "%hhx", &peermac[4]); mac[8] = '\0';
    ptr = &mac[6];
    sscanf(ptr, "%hhx", &peermac[3]); mac[6] = '\0';
    ptr = &mac[4];
    sscanf(ptr, "%hhx", &peermac[2]); mac[4] = '\0';
    ptr = &mac[2];
    sscanf(ptr, "%hhx", &peermac[1]); mac[2] = '\0';
    ptr = &mac[0];
    sscanf(ptr, "%hhx", &peermac[0]); 

    if (change_freq(instance->mymac, chan2freq(channel)) < 0) {
        fprintf(stderr, "peer's channel and operating class is not supported!\n");
        goto fin;
    }

    if (create_dpp_instance(instance->mymac, peermac, keyb64, is_initiator, mauth) == NULL) {
        fprintf(stderr, "unable to create peer!\n");
    } else {
        printf("new peer is at " MACSTR "\n", MAC2STR(instance->peermac));
    }
fin:
    /*
     * clean up this state regardless of whether we successfully bootstrapped
     */
    TAILQ_REMOVE(&pkex_instances, instance, entry);
    free(instance);
    return ret;
}

/*
 * called by enrollees when they finish DPP
 */
void
term (unsigned short reason, dpp_handle handle)
{
    if (quit_at_fin) {
        printf("DPP is terminating...\n");
        dpp_free_peer(handle);
        exit(reason);
    } else {
        printf("DPP has ended...\n");
    }
}

int
main (int argc, char **argv)
{
    int c, debug = 0, is_initiator = 0, config_or_enroll = 0, mutual = 1, do_pkex = 0, do_dpp = 1, keyidx = 0;
    int chchandpp = 0, chirp = 0, ver, newgroup = 0;
    struct interface *inf;
    char interface[IFNAMSIZ], password[80], keyfile[80], signkeyfile[80], enrollee_role[10], mudurl[80];
    char *ptr, *endptr, identifier[80], pkexinfo[80], caip[40];
    unsigned char targetmac[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    struct nl_msg *msg;
    
    if ((srvctx = srv_create_context()) == NULL) {
        fprintf(stderr, "%s: cannot create service context!\n", argv[0]);
        exit(1);
    }
    TAILQ_INIT(&interfaces);
    TAILQ_INIT(&dpp_instances);
    ver = DPP_VERSION;
    
    memset(bootstrapfile, 0, 80);
    memset(signkeyfile, 0, 80);
    memset(mudurl, 0, 80);
    memset(identifier, 0, 80);
    memset(pkexinfo, 0, 80);
    memset(caip, 0, 40);
    for (;;) {
        c = getopt(argc, argv, "hirm:k:I:B:x:b:yase:c:d:p:n:z:qf:g:u:tw:v:");
        /*
         * left: l, o
         */
        if (c < 0) {
            break;
        }
        switch (c) {
            case 'I':           /* interface */
                if (strlen(optarg) > IFNAMSIZ) {
                    fprintf(stderr, "%s: interface name %s is 'too large'\n", argv[0], optarg);
                    exit(1);
                }
                strcpy(interface, optarg);
                printf("adding interface %s...\n", interface);
                add_interface(interface);
                break;
            case 'B':           /* bootstrap key file */
                strcpy(bootstrapfile, optarg);
                break;
            case 'k':           /* keyfile */
                strcpy(keyfile, optarg);
                break;
            case 'p':           /* password */
                strcpy(password, optarg);
                do_pkex = 1;
                break;
            case 'n':           /* pkex identifier */
                strcpy(identifier, optarg);
                break;
            case 'w':
                strcpy(caip, optarg);
                break;
            case 'd':           /* debug */
                debug = atoi(optarg);
                break;
            case 'i':           /* initiator */
                is_initiator = 1;
                break;
            case 'r':           /* responder */
                is_initiator = 0;
                break;
            case 'b':
                newgroup = atoi(optarg);
                break;
            case 'c':           /* configurator */
                strcpy(signkeyfile, optarg);
                config_or_enroll |= 0x02;
                do_dpp = 1;
                break;
            case 'e':           /* enrollee */
                strcpy(enrollee_role, optarg);
                config_or_enroll |= 0x01;
                do_dpp = 1;
                break;
            case 'a':           /* not mutual authentication */
                mutual = 0;
                do_dpp = 1;
                break;
            case 'm':
                ptr = optarg;
                targetmac[0] = (unsigned char)strtol(ptr, &endptr, 16); ptr = endptr+1; targetmac[0] &= 0xff;
                targetmac[1] = (unsigned char)strtol(ptr, &endptr, 16); ptr = endptr+1; targetmac[1] &= 0xff;
                targetmac[2] = (unsigned char)strtol(ptr, &endptr, 16); ptr = endptr+1; targetmac[2] &= 0xff;
                targetmac[3] = (unsigned char)strtol(ptr, &endptr, 16); ptr = endptr+1; targetmac[3] &= 0xff;
                targetmac[4] = (unsigned char)strtol(ptr, &endptr, 16); ptr = endptr+1; targetmac[4] &= 0xff;
                targetmac[5] = (unsigned char)strtol(ptr, &endptr, 16); ptr = endptr+1; targetmac[5] &= 0xff;
                break;
            case 'y':
                do_pkex = 1;
                do_dpp = 0;
                break;
            case 'x':
                keyidx = atoi(optarg);
                break;
            case 'z':
                strcpy(pkexinfo, optarg);
                break;
            case 'f':           /* channel */
                channel = atoi(optarg);
                break;
            case 'g':           /* operating class */
                opclass = atoi(optarg);
                break;
            case 's':
                chchandpp = 1;
                break;
            case 't':
                chirp = 1;
                break;
            case 'u':
                strcpy(mudurl, optarg);
                break;
            case 'v':
                ver = atoi(optarg);
                if (ver < 1 || ver > DPP_VERSION) {
                    fprintf(stderr, "%s: unknown version, %d\n", argv[0], ver);
                    exit(1);
                }
                break;
            case 'q':
                quit_at_fin = 1;
                break;
            default:
            case 'h':
                fprintf(stderr, 
                        "USAGE: %s [-hIBapkceirdfgst]\n"
                        "\t-h  show usage, and exit\n"
                        "\t-c <signkey> run DPP as the configurator, sign connectors with <signkey>\n"
                        "\t-e <role> run DPP as the enrollee in the role of <role> (sta or ap)\n"
                        "\t-i  run DPP as the initiator\n"
                        "\t-r  run DPP as the responder\n"
                        "\t-a  do not perform mutual authentication in DPP\n"
                        "\t-I <interface> to add to DPP (max %d characters)\n"
                        "\t-B <filename> of peer bootstrappign keys\n"
                        "\t-p <password> to use for PKEX\n"
                        "\t-f <channel> to use with DPP\n"
                        "\t-g <opclass> operating class to use with DPP\n"
                        "\t-z <info> to pass along with public key in PKEX\n"
                        "\t-n <identifier> for the code used in PKEX\n"
                        "\t-b <curve> Configurator asks for a new protocol key\n"
                        "\t-k <filename> my bootstrapping key\n"
                        "\t-y  bootstrapping (PKEX) only, don't run DPP\n"
                        "\t-x  <index> DPP only with key <index> in -B <filename>, don't do PKEX\n"
                        "\t-m <MAC address> to initiate to, otherwise uses broadcast\n"
                        "\t-s  change opclass/channel to what was set with -f and -g during DPP\n"
                        "\t-u <url> to find a MUD file (enrollee only)\n"
                        "\t-t  send DPP chirps (responder only)\n"
                        "\t-q  terminate the process upon completion (enrollee only)\n"
                        "\t-w <ipaddr> IP address of CA (for enterprise-only Configurators)\n"
                        "\t-j  enable mdns (client queries, configurator publishes)\n"
                        "\t-d <debug> set debugging mask\n",
                        argv[0], IFNAMSIZ);
                exit(1);
                
        }
    }
    /*
     * if we're doing DPP w/mutual authentication and/or doing PKEX then
     * a bootstrapping key has to be included
     */
    if ((mutual || do_pkex) && (bootstrapfile[0] == 0)) {
        fprintf(stderr, "%s: specify a peer bootstrapping key file with -B <filename>\n", argv[0]);
        exit(1);
    }
    if (is_initiator && !do_pkex && (keyidx == 0)) {
        fprintf(stderr, "%s: either do PKEX or specify an index into bootstrapping file with -x\n",
                argv[0]);
        exit(1);
    }
    if (TAILQ_EMPTY(&interfaces)) {
        fprintf(stderr, "%s: no interfaces defined!\n", argv[0]);
        add_interface("lo");
    }
    if (is_initiator) {
        printf("initiating to " MACSTR "\n", MAC2STR(targetmac));
    }

    /*
     * initialize data structures...
     */
    if (do_pkex) {
        if (pkex_initialize(is_initiator, password, 
                            identifier[0] == 0 ? NULL : identifier,
                            pkexinfo[0] == 0 ? NULL : pkexinfo, keyfile,
                            debug) < 0) {
            fprintf(stderr, "%s: cannot configure PKEX/DPP, check config file!\n", argv[0]);
            exit(1);
        }
    }
    if (do_dpp) {
        if (dpp_initialize(config_or_enroll, keyfile,
                           signkeyfile[0] == 0 ? NULL : signkeyfile, newgroup, enrollee_role,
                           mudurl[0] == 0 ? NULL : mudurl, chirp, caip[0] == 0 ? "127.0.0.1" : caip,
                           chchandpp ? opclass : 0, chchandpp ? channel : 0, debug) < 0) {
            fprintf(stderr, "%s: cannot configure DPP, check config file!\n", argv[0]);
            exit(1);
        }
    }
    
    /*
     * set up default channels for our interfaces or generate a
     * chirp list if we're chirping
     */
    printf("interfaces and MAC addresses:\n");
    TAILQ_FOREACH(inf, &interfaces, entry) {
        printf("\t%s: " MACSTR "\n", inf->ifname, MAC2STR(inf->bssid));
        if (chirp) {
            if (!inf->is_loopback) {
                /*
                 * first add channel 6 since we support 2.4GHz
                 */
                dpp_add_chirp_freq(inf->bssid, 2437);
                /*
                 * then add all the APs that are beaconing out a DPP ConfigConn IE
                 */
                printf("chirping, so scan for APs\n");
                if (trigger_scan(inf, NULL) == 0) {
                    msg = get_nl_msg(inf, NLM_F_DUMP, NL80211_CMD_GET_SCAN);
                    if (send_nl_msg(msg, inf, callback_dump, inf)) {
                        printf("can't get scan info from kernel!\n");
                    }
                } else {
                    printf("can't scan to find chirping channel :-(\n");
                }
            }
            /*
             * then add the configured channel if it's not on the list already
             */
            dpp_add_chirp_freq(inf->bssid, chan2freq(channel));
        }
        inf->freq = chan2freq(channel);
        printf("configured channel %ld\n", inf->freq);
        
        /*
         * if we're not changing the channel in DPP after the 1st message then
         * just set it now.
         */
        if (chchandpp == 0 ) {
            change_freq(inf->bssid, inf->freq);
        }
    }

    TAILQ_FOREACH(inf, &interfaces, entry) {
        /*
         * For each interface we're active on...
         */
        if (is_initiator) {
            struct pkex_instance *instance;
            /* 
             * if we're the initiator, then either start off with an
             * already bootstrapped peer or do pkex.
             */
            if (!do_pkex) {           /* 0 means don't create pkex state */
                instance = create_pkex_instance(inf->bssid, targetmac, 0);
                bootstrap_peer(instance->handle, keyidx, is_initiator, mutual);
            } else {
                /*
                 * always update the macs in case there's no v2+ answer we can
                 * switch back to v1
                 */
                instance = create_pkex_instance(inf->bssid, targetmac, ver);
                pkex_update_macs(instance->handle, inf->bssid, targetmac);
                pkex_initiate(instance->handle);
            }
        } else {
            /*
             * if we're the responder, then PKEX is already ready, just wait,
             * otherwise create a DPP peer and wait.
             */
            if (!do_pkex) {
                create_dpp_instance(inf->bssid, targetmac, NULL, is_initiator, mutual);
            }
        }
    }
    srv_add_exceptor(srvctx, exceptor);
    srv_main_loop(srvctx);
    printf("main loop exited!\n");
    exit(1);
}
